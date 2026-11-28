/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, browser camera backend for Emscripten builds
 *
 * Copyright 2026 Cong Zhang <congzhangzh@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * /dvc:rdpecam,sys:emscripten redirects the browser's cameras, as H.264.
 *
 * The cameras are the ones the page lists in Module.freerdpCameras ([{ deviceId, label }], after
 * the user allowed the camera), else one camera, the browser's default, named "Browser camera".
 * A label that names a FreeRDP server's own camera is never offered. A browser that cannot encode
 * H.264 (WebCodecs VideoEncoder) offers no camera.
 *
 * The browser opens a camera only when the server starts a stream (the page asks for the
 * permission beforehand) and closes it when the server stops, or when the server stopped asking
 * for samples (RequestDrivenCapture). The main browser thread captures the frames and encodes
 * them with a VideoEncoder (camera_emscripten.js): Constrained Baseline, real-time, at the bit
 * rate chosen here, one key frame per stream, the first (MS-RDPECAM has no key frame request, RDP
 * runs over TCP, and every recovery is a new Start Streams, which starts a new encoder).
 * It writes the encoder's avcC record, then each chunk, into a one-chunk slot in the shared wasm
 * memory and wakes the slot's state as a futex. A capture thread here waits on that futex, turns
 * each chunk into an Annex B sample with the SPS and PPS before every key frame
 * (camera_web_h264.c) and hands it to the channel. Encoded chunks are never dropped (that would
 * break the pictures up to the next key frame): when the network is slower than the camera, the
 * page skips camera frames before they are encoded.
 *
 * Stopping hands the slot to the main thread, which frees it once the capture can no longer write
 * to it (camera_emscripten_release).
 */

#include <freerdp/config.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include <winpr/assert.h>
#include <winpr/interlocked.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>

#include <freerdp/addin.h>

#include "../camera.h"
#include "camera_web_h264.h"

#define TAG CHANNELS_TAG("rdpecam-emscripten.client")

#define CAM_WEB_MAX_DEVICES 8
#define CAM_WEB_DEFAULT_NAME "Browser camera"
#define CAM_WEB_WAIT_MS 500.0

/* the slot's state, a futex */
#define CAM_WEB_SLOT_FREE 0   /* the page may write a chunk */
#define CAM_WEB_SLOT_FULL 1   /* a chunk waits for the capture thread */
#define CAM_WEB_SLOT_FAILED 2 /* the browser could not open the camera; the page wrote why */

/* Shared with camera_emscripten.js, which reads and writes it as 32 bit words: keep the order */
typedef struct
{
	volatile int32_t state;     /* [0] CAM_WEB_SLOT_* */
	volatile int32_t active;    /* [1] 1 while the page may write */
	volatile uint32_t kind;     /* [2] CAM_WEB_CHUNK_* */
	volatile uint32_t size;     /* [3] bytes at data */
	uint32_t capacity;          /* [4] room at data */
	volatile uint32_t frames;   /* [5] chunks the encoder gave */
	volatile uint32_t skipped;  /* [6] camera frames not encoded: the encoder or network was busy */
	volatile uint32_t encodeUs; /* [7] microseconds from encode() to its chunk, in total */
	volatile uint32_t keys;     /* [8] key frames */
	volatile uint32_t firstUs;  /* [9] microseconds from cam_web_start to the first chunk */
	BYTE* data;                 /* [10] */
} cam_web_capture;

/* what the slot holds */
#define CAM_WEB_CHUNK_CONFIG 0 /* the encoder's avcC record (decoderConfig.description) */
#define CAM_WEB_CHUNK_KEY 1    /* a key frame */
#define CAM_WEB_CHUNK_DELTA 2  /* any other frame */

WINPR_STATIC_ASSERT(offsetof(cam_web_capture, firstUs) == 9 * 4);
WINPR_STATIC_ASSERT(offsetof(cam_web_capture, data) == 10 * 4);
WINPR_STATIC_ASSERT(sizeof(void*) == 4);

typedef struct
{
	CRITICAL_SECTION lock;
	int index; /* in the page's camera list */
	CameraDevice* dev;
	size_t streamIndex;
	ICamHalSampleCapturedCallback callback;
	CAM_MEDIA_TYPE_DESCRIPTION mediaType;

	cam_web_capture* capture; /* owned here until handed to the page to free */
	HANDLE thread;
	volatile LONG streaming;

	/* the encoder's parameter sets, from its avcC record */
	BYTE lengthSize;
	BYTE params[1024];
	size_t paramsSize;
	BOOL waitKey; /* nothing goes out before a key frame */

	BYTE* sample; /* the Annex B sample, sampleCapacity bytes */
	size_t sampleCapacity;
	UINT32 bitrate;

	UINT64 started;
	UINT64 firstSample; /* ms from StartStream to the first sample, 0 before it */
	UINT64 samples;
	UINT64 bytes;
	UINT32 rejected;
} CamWebStream;

typedef struct
{
	ICamHal iHal;
	CamWebStream streams[CAM_WEB_MAX_DEVICES];
} CamWebHal;

/* in camera_emscripten.js; the device functions run on the main thread (proxied) */
extern int cam_web_device_count(void);
extern int cam_web_device_label(int index, char* buffer, int size);
extern int cam_web_can_h264(void);
extern void cam_web_start(int index, cam_web_capture* capture, int width, int height, int fpsNum,
                          int fpsDen, int bitrate, const char* codec);
extern void cam_web_stop(int index, cam_web_capture* capture);

void camera_emscripten_release(void* capture);

/* Called by the main thread once the page can no longer write to the slot. */
EMSCRIPTEN_KEEPALIVE void camera_emscripten_release(void* capture)
{
	free(capture);
}

/* "web<N>": the index into the page's camera list */
WINPR_ATTR_NODISCARD
static int cam_web_index(const char* deviceId)
{
	if (!deviceId || (strncmp(deviceId, "web", 3) != 0))
		return -1;
	char* end = nullptr;
	const unsigned long n = strtoul(deviceId + 3, &end, 10);
	if (!end || (*end != '\0') || (end == deviceId + 3) || (n >= CAM_WEB_MAX_DEVICES))
		return -1;
	return (int)n;
}

static UINT cam_web_enumerate(ICamHal* ihal, ICamHalEnumCallback callback, CameraPlugin* ecam,
                              GENERIC_CHANNEL_CALLBACK* hchannel)
{
	WINPR_UNUSED(ihal);

	if (!cam_web_can_h264())
	{
		WLog_WARN(TAG, "this browser cannot encode H.264 (WebCodecs VideoEncoder): no camera");
		return 0;
	}

	const int listed = cam_web_device_count();
	const int count = (listed > 0) ? listed : 1;
	UINT offered = 0;

	for (int i = 0; (i < count) && (i < CAM_WEB_MAX_DEVICES); i++)
	{
		char name[256] = WINPR_C_ARRAY_INIT;
		if ((listed <= 0) || (cam_web_device_label(i, name, sizeof(name)) <= 0))
			(void)_snprintf(name, sizeof(name), "%s", CAM_WEB_DEFAULT_NAME);

		if (ecam_is_freerdp_camera(name))
		{
			WLog_DBG(TAG, "browser camera %d is \"%s\", a FreeRDP server's own camera: not offered",
			         i, name);
			continue;
		}

		char deviceId[16] = WINPR_C_ARRAY_INIT;
		(void)_snprintf(deviceId, sizeof(deviceId), "web%d", i);
		WLog_INFO(TAG, "camera %s: \"%s\"", deviceId, name);
		IFCALL(callback, ecam, hchannel, deviceId, name);
		offered++;
	}
	return offered;
}

static BOOL cam_web_activate(ICamHal* ihal, const char* deviceId, CAM_ERROR_CODE* errorCode)
{
	WINPR_UNUSED(ihal);
	WINPR_ASSERT(errorCode);
	*errorCode =
	    (cam_web_index(deviceId) < 0) ? CAM_ERROR_CODE_InvalidMessage : CAM_ERROR_CODE_None;
	return *errorCode == CAM_ERROR_CODE_None;
}

static BOOL cam_web_deactivate(ICamHal* ihal, const char* deviceId, CAM_ERROR_CODE* errorCode)
{
	WINPR_UNUSED(ihal);
	WINPR_UNUSED(deviceId);
	WINPR_ASSERT(errorCode);
	*errorCode = CAM_ERROR_CODE_None;
	return TRUE;
}

/* The browser scales every frame to the size the server picked, so any size can be offered; the
 * first one is what most applications take. */
static const struct
{
	UINT32 width;
	UINT32 height;
} cam_web_sizes[] = { { 640, 480 }, { 1280, 720 }, { 640, 360 }, { 320, 240 } };

static INT16 cam_web_get_media_type_descriptions(ICamHal* ihal, const char* deviceId,
                                                 size_t streamIndex,
                                                 const CAM_MEDIA_FORMAT_INFO* supportedFormats,
                                                 size_t nSupportedFormats,
                                                 CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes,
                                                 size_t* nMediaTypes)
{
	WINPR_UNUSED(ihal);
	WINPR_ASSERT(nMediaTypes);

	const size_t maxMediaTypes = *nMediaTypes;
	*nMediaTypes = 0;

	if ((cam_web_index(deviceId) < 0) || (streamIndex != 0))
		return -1;

	/* H.264 only: raw video is ~110 Mbit/s at 640x480 and MJPG is still too much for the networks
	 * a browser is on */
	const CAM_MEDIA_FORMAT format = CAM_MEDIA_FORMAT_H264;
	size_t formatIndex = 0;
	for (; formatIndex < nSupportedFormats; formatIndex++)
	{
		if ((supportedFormats[formatIndex].inputFormat == format) &&
		    (supportedFormats[formatIndex].outputFormat == format))
			break;
	}
	if ((formatIndex >= nSupportedFormats) || (formatIndex > INT16_MAX))
	{
		WLog_ERR(TAG, "the channel takes no H.264 from the browser");
		return -1;
	}

	size_t n = 0;
	for (; (n < ARRAYSIZE(cam_web_sizes)) && (n < maxMediaTypes); n++)
	{
		mediaTypes[n] = (CAM_MEDIA_TYPE_DESCRIPTION){ .Format = format,
			                                          .Width = cam_web_sizes[n].width,
			                                          .Height = cam_web_sizes[n].height,
			                                          .FrameRateNumerator = 30,
			                                          .FrameRateDenominator = 1,
			                                          .PixelAspectRatioNumerator = 1,
			                                          .PixelAspectRatioDenominator = 1 };
	}
	*nMediaTypes = n;
	WLog_INFO(TAG, "camera %s: H264, %" PRIuz " sizes", deviceId, n);
	return (INT16)formatIndex;
}

static void cam_web_log_stats(const char* deviceId, const CamWebStream* stream,
                              const cam_web_capture* capture)
{
	const UINT64 ms = GetTickCount64() - stream->started;
	const UINT64 kbits = ms ? (stream->bytes * 8ull / ms) : 0; /* bits per ms = kbit/s */
	const uint32_t frames = capture ? capture->frames : 0;
	const uint32_t encodeUs = capture ? capture->encodeUs : 0;
	const uint32_t skipped = capture ? capture->skipped : 0;
	const uint32_t keys = capture ? capture->keys : 0;
	const double encodeMs = frames ? (encodeUs / 1000.0 / frames) : 0.0;

	WLog_INFO(TAG,
	          "camera %s stopped: H264 %" PRIu32 "x%" PRIu32 " asked %" PRIu32
	          " kbit/s, sent %" PRIu64 " kbit/s, %" PRIu64 " samples in %" PRIu64
	          " ms, first after %" PRIu64 " ms; %" PRIu32 " chunks (%" PRIu32 " key), %" PRIu32
	          " camera frames skipped, %" PRIu32 " rejected, encode %.2f ms per frame",
	          deviceId, stream->mediaType.Width, stream->mediaType.Height, stream->bitrate / 1000,
	          kbits, stream->samples, ms, stream->firstSample, frames, keys, skipped,
	          stream->rejected, encodeMs);
}

static DWORD WINAPI cam_web_capture_thread(LPVOID arg)
{
	CamWebStream* stream = arg;
	WINPR_ASSERT(stream);

	cam_web_capture* capture = stream->capture;
	WINPR_ASSERT(capture);
	BOOL drained = FALSE;

	while (InterlockedCompareExchange(&stream->streaming, 0, 0))
	{
		const int32_t state = __atomic_load_n(&capture->state, __ATOMIC_SEQ_CST);
		if (state == CAM_WEB_SLOT_FREE)
		{
			(void)emscripten_futex_wait(&capture->state, CAM_WEB_SLOT_FREE, CAM_WEB_WAIT_MS);
			continue;
		}
		if (state == CAM_WEB_SLOT_FAILED)
		{
			WLog_ERR(TAG, "camera %s: the browser could not open the camera",
			         stream->dev->deviceId);
			break;
		}

		const uint32_t kind = capture->kind;
		const size_t size = capture->size;
		BOOL send = FALSE;
		size_t sampleSize = 0;
		if (size > capture->capacity)
			stream->rejected++;
		else if (kind == CAM_WEB_CHUNK_CONFIG)
		{
			if (!cam_web_h264_parse_avcc(capture->data, size, &stream->lengthSize, stream->params,
			                             sizeof(stream->params), &stream->paramsSize))
			{
				WLog_ERR(TAG, "camera %s: the encoder's avcC record (%" PRIuz " bytes) is unusable",
				         stream->dev->deviceId, size);
				stream->paramsSize = 0;
			}
		}
		else if ((kind == CAM_WEB_CHUNK_KEY) || (kind == CAM_WEB_CHUNK_DELTA))
		{
			const BOOL key = (kind == CAM_WEB_CHUNK_KEY);
			if (stream->paramsSize == 0)
				stream->rejected++;
			else if (stream->waitKey && !key)
				stream->rejected++;
			else if (!cam_web_h264_to_annexb(stream->lengthSize, stream->params, stream->paramsSize,
			                                 key, capture->data, size, stream->sample,
			                                 stream->sampleCapacity, &sampleSize))
			{
				/* the next frames depend on this one: wait for a key frame */
				stream->rejected++;
				stream->waitKey = TRUE;
			}
			else
			{
				send = TRUE;
				if (key)
					stream->waitKey = FALSE;
			}
		}
		else
			stream->rejected++;

		if (!send)
		{
			if (stream->rejected == 1)
				WLog_WARN(TAG,
				          "camera %s: a chunk (kind %" PRIu32 ", %" PRIuz
				          " bytes) cannot be sent (logged once)",
				          stream->dev->deviceId, kind, size);
			__atomic_store_n(&capture->state, CAM_WEB_SLOT_FREE, __ATOMIC_SEQ_CST);
			continue;
		}

		const UINT rc =
		    stream->callback(stream->dev, stream->streamIndex, stream->sample, sampleSize);
		/* the channel copied the sample: the page may write the next frame */
		__atomic_store_n(&capture->state, CAM_WEB_SLOT_FREE, __ATOMIC_SEQ_CST);
		if (rc == ECAM_SAMPLE_CAPTURE_DRAINED)
		{
			drained = TRUE;
			break;
		}
		if (rc != CHANNEL_RC_OK)
			WLog_DBG(TAG, "sample callback failed: %" PRIu32, rc);
		else
		{
			if (stream->samples++ == 0)
			{
				stream->firstSample = GetTickCount64() - stream->started;
				WLog_INFO(TAG,
				          "camera %s: first H264 sample (%" PRIuz " bytes) after %" PRIu64 " ms",
				          stream->dev->deviceId, sampleSize, stream->firstSample);
			}
			stream->bytes += sampleSize;
		}
	}

	if (drained)
	{
		/* the server stopped asking: close the browser's camera now; StartStream opens it again */
		EnterCriticalSection(&stream->lock);
		(void)InterlockedExchange(&stream->streaming, 0);
		cam_web_log_stats(stream->dev->deviceId, stream, stream->capture);
		if (stream->capture)
		{
			stream->capture->active = 0;
			cam_web_stop(stream->index, stream->capture);
		}
		stream->capture = nullptr;
		LeaveCriticalSection(&stream->lock);
	}
	return 0;
}

WINPR_ATTR_NODISCARD
static CamWebStream* cam_web_stream(CamWebHal* hal, const char* deviceId)
{
	const int i = cam_web_index(deviceId);
	if (i < 0)
		return nullptr;
	return &hal->streams[i];
}

static void cam_web_join(CamWebStream* stream)
{
	if (stream->thread)
	{
		(void)WaitForSingleObject(stream->thread, INFINITE);
		(void)CloseHandle(stream->thread);
		stream->thread = nullptr;
	}
}

static CAM_ERROR_CODE cam_web_stream_stop(CamWebStream* stream)
{
	EnterCriticalSection(&stream->lock);
	(void)InterlockedExchange(&stream->streaming, 0);
	cam_web_capture* capture = stream->capture;
	if (capture)
	{
		capture->active = 0;
		emscripten_futex_wake(&capture->state, INT32_MAX);
	}
	LeaveCriticalSection(&stream->lock);

	cam_web_join(stream);

	EnterCriticalSection(&stream->lock);
	if (stream->capture)
	{
		cam_web_log_stats(stream->dev->deviceId, stream, stream->capture);
		cam_web_stop(stream->index, stream->capture); /* the page frees it */
		stream->capture = nullptr;
	}
	LeaveCriticalSection(&stream->lock);
	return CAM_ERROR_CODE_None;
}

static CAM_ERROR_CODE cam_web_start_stream(ICamHal* ihal, CameraDevice* dev, size_t streamIndex,
                                           const CAM_MEDIA_TYPE_DESCRIPTION* mediaType,
                                           ICamHalSampleCapturedCallback callback)
{
	CamWebHal* hal = (CamWebHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_ASSERT(dev);
	WINPR_ASSERT(mediaType);

	CamWebStream* stream = cam_web_stream(hal, dev->deviceId);
	if (!stream || (streamIndex != 0))
		return CAM_ERROR_CODE_InvalidStreamNumber;
	if (mediaType->Format != CAM_MEDIA_FORMAT_H264)
		return CAM_ERROR_CODE_InvalidMediaType;

	const size_t capacity = cam_web_h264_capacity(mediaType->Width, mediaType->Height);
	const char* codec =
	    cam_web_h264_codec(mediaType->Width, mediaType->Height, mediaType->FrameRateNumerator,
	                       mediaType->FrameRateDenominator);
	if ((capacity == 0) || !codec)
		return CAM_ERROR_CODE_InvalidMediaType;
	const UINT32 bitrate =
	    cam_web_h264_bitrate(mediaType->Width, mediaType->Height, mediaType->FrameRateNumerator,
	                         mediaType->FrameRateDenominator);

	if (InterlockedCompareExchange(&stream->streaming, 0, 0))
	{
		WLog_ERR(TAG, "camera %s is already streaming", dev->deviceId);
		return CAM_ERROR_CODE_UnexpectedError;
	}
	/* the thread of a stream that drained */
	cam_web_join(stream);

	cam_web_capture* capture = calloc(1, sizeof(cam_web_capture) + capacity);
	/* a sample: the chunk with start codes for lengths, the parameter sets in front */
	const size_t sampleCapacity = capacity + sizeof(stream->params) + 4096;
	BYTE* sample = malloc(sampleCapacity);
	if (!capture || !sample)
	{
		free(capture);
		free(sample);
		return CAM_ERROR_CODE_OutOfMemory;
	}
	capture->capacity = (uint32_t)capacity;
	capture->data = (BYTE*)(capture + 1);
	capture->active = 1;

	EnterCriticalSection(&stream->lock);
	free(stream->sample);
	stream->sample = sample;
	stream->sampleCapacity = sampleCapacity;
	stream->paramsSize = 0;
	stream->waitKey = TRUE;
	stream->bitrate = bitrate;
	stream->firstSample = 0;
	stream->dev = dev;
	stream->streamIndex = streamIndex;
	stream->callback = callback;
	stream->mediaType = *mediaType;
	stream->capture = capture;
	stream->started = GetTickCount64();
	stream->samples = 0;
	stream->bytes = 0;
	stream->rejected = 0;
	(void)InterlockedExchange(&stream->streaming, 1);

	stream->thread = CreateThread(nullptr, 0, cam_web_capture_thread, stream, 0, nullptr);
	if (!stream->thread)
	{
		(void)InterlockedExchange(&stream->streaming, 0);
		stream->capture = nullptr;
		LeaveCriticalSection(&stream->lock);
		free(capture);
		return CAM_ERROR_CODE_OutOfMemory;
	}

	WLog_INFO(TAG,
	          "camera %s: start H264 %" PRIu32 "x%" PRIu32 " @ %" PRIu32 "/%" PRIu32
	          ", %s, %" PRIu32 " kbit/s",
	          dev->deviceId, mediaType->Width, mediaType->Height, mediaType->FrameRateNumerator,
	          mediaType->FrameRateDenominator, codec, bitrate / 1000);
	cam_web_start(stream->index, capture, (int)mediaType->Width, (int)mediaType->Height,
	              (int)mediaType->FrameRateNumerator, (int)mediaType->FrameRateDenominator,
	              (int)bitrate, codec);
	LeaveCriticalSection(&stream->lock);
	return CAM_ERROR_CODE_None;
}

static CAM_ERROR_CODE cam_web_stop_stream(ICamHal* ihal, const char* deviceId, size_t streamIndex)
{
	CamWebHal* hal = (CamWebHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_UNUSED(streamIndex);

	CamWebStream* stream = cam_web_stream(hal, deviceId);
	if (!stream)
		return CAM_ERROR_CODE_InvalidStreamNumber;
	return cam_web_stream_stop(stream);
}

static CAM_ERROR_CODE cam_web_free(ICamHal* ihal)
{
	CamWebHal* hal = (CamWebHal*)ihal;
	if (!hal)
		return CAM_ERROR_CODE_NotInitialized;

	for (int i = 0; i < CAM_WEB_MAX_DEVICES; i++)
	{
		(void)cam_web_stream_stop(&hal->streams[i]);
		free(hal->streams[i].sample);
		DeleteCriticalSection(&hal->streams[i].lock);
	}
	free(hal);
	return CAM_ERROR_CODE_None;
}

FREERDP_ENTRY_POINT(UINT VCAPITYPE emscripten_freerdp_rdpecam_client_subsystem_entry(
    PFREERDP_CAMERA_HAL_ENTRY_POINTS pEntryPoints))
{
	WINPR_ASSERT(pEntryPoints);

	CamWebHal* hal = calloc(1, sizeof(CamWebHal));
	if (!hal)
		return CHANNEL_RC_NO_MEMORY;

	for (size_t i = 0; i < CAM_WEB_MAX_DEVICES; i++)
	{
		hal->streams[i].index = (int)i;
		if (!InitializeCriticalSectionEx(&hal->streams[i].lock, 0, 0))
		{
			for (size_t j = 0; j < i; j++)
				DeleteCriticalSection(&hal->streams[j].lock);
			free(hal);
			return CHANNEL_RC_NO_MEMORY;
		}
	}

	hal->iHal.Enumerate = cam_web_enumerate;
	hal->iHal.RequestDrivenCapture = TRUE;
	hal->iHal.GetMediaTypeDescriptions = cam_web_get_media_type_descriptions;
	hal->iHal.Activate = cam_web_activate;
	hal->iHal.Deactivate = cam_web_deactivate;
	hal->iHal.StartStream = cam_web_start_stream;
	hal->iHal.StopStream = cam_web_stop_stream;
	hal->iHal.Free = cam_web_free;

	const UINT ret = pEntryPoints->pRegisterCameraHal(pEntryPoints->plugin, &hal->iHal);
	if (ret != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "RegisterCameraHal failed with error %" PRIu32 "", ret);
		(void)cam_web_free(&hal->iHal);
	}
	return ret;
}
