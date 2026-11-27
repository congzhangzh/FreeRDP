/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a client's camera (MS-RDPECAM) as a camera of the host
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
 * Threads: each client's enumerator and each device run a thread of their own (the server
 * contexts of channels/rdpecam/server); their callbacks come here. The camera's state (the
 * sink, the owner, the frame buffers) is guarded by g_camera.lock, a client's list of devices
 * by its own lock. Lock order: a client's lock, then g_camera.lock; a channel context is
 * never closed (its thread joined) while either is held.
 *
 * A device's flow: Activate Device, Stream List, Media Type List (the choice), Start Streams,
 * then Sample Requests, kept ceil(fps * RTT) + 1 outstanding (at most 4); each sample is
 * decoded, scaled into the camera's size and written to the sink.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/collections.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>

#include <freerdp/log.h>
#include <freerdp/server/rdpecam.h>
#include <freerdp/server/rdpecam-enumerator.h>

#include "../shadow_autodetect.h"

#include "shadow_camera.h"
#include "camera_decode.h"
#include "camera_image.h"
#include "camera_policy.h"
#include "camera_sink.h"
#include "placeholder.h"

#define TAG SERVER_TAG("shadow.camera")

#define CAMERA_DEFAULT_WIDTH 1280
#define CAMERA_DEFAULT_HEIGHT 720
#define CAMERA_DEFAULT_FPS 30
#define CAMERA_MAX_WIDTH 4096
#define CAMERA_MAX_HEIGHT 4096
#define CAMERA_MAX_FPS 120

/* a client camera that sent nothing for this long counts as gone: the still comes back */
#define CAMERA_STALL_MS 2000
/* this many sample errors in a row and the device is given up */
#define CAMERA_MAX_SAMPLE_ERRORS 30
/* the newest MS-RDPECAM version */
#define CAMERA_PROTOCOL_VERSION 2

typedef struct s_camera_client CameraClient;

typedef enum
{
	CAMERA_DEVICE_ACTIVATING,
	CAMERA_DEVICE_STREAM_LIST,
	CAMERA_DEVICE_MEDIA_TYPES,
	CAMERA_DEVICE_STARTING,
	CAMERA_DEVICE_STREAMING,
	CAMERA_DEVICE_FAILED
} CAMERA_DEVICE_STATE;

typedef struct
{
	CameraClient* cc;
	CameraDeviceServerContext* ctx;
	char* channel; /* the device's channel name */
	char* name;    /* the device's name, UTF-8 */

	/* the device's thread only, once its context is open */
	CAMERA_DEVICE_STATE state;
	BYTE streamIndex;
	CAM_MEDIA_TYPE_DESCRIPTION type;
	ShadowCameraDecoder* decoder;
	UINT32 outstanding;
	UINT32 errors;
	UINT64 frames;
	char detail[64];
} CameraDevice;

struct s_camera_client
{
	rdpShadowClient* client;
	CamDevEnumServerContext* enumerator;
	BYTE version;
	CRITICAL_SECTION lock;
	wArrayList* devices; /* CameraDevice*, guarded by lock */
};

typedef struct
{
	CRITICAL_SECTION lock;
	BOOL running;
	ShadowCameraSink* sink;
	ShadowCameraDecoders decoders;
	ShadowCameraOwner owner;
	BOOL ownerStreaming; /* the owner wrote a frame since it took the camera */
	UINT64 lastClientFrameMs;
	BOOL stillShown; /* the still is what the sink showed last */
	UINT64 lastStillMs;
	BYTE* frameBuffer; /* a client frame in the sink's format and size */
	ShadowCameraImage frame;
	BYTE* stillBuffer;
	ShadowCameraImage still;
	HANDLE thread;
	HANDLE stopEvent;
	HANDLE wakeEvent;
	wArrayList* clients; /* CameraClient* */
	UINT64 clientFrames;
	UINT64 stills;
} CameraServer;

static INIT_ONCE g_cameraOnce = INIT_ONCE_STATIC_INIT;
static CameraServer g_camera;

static BOOL CALLBACK camera_init_once(WINPR_ATTR_UNUSED PINIT_ONCE once,
                                      WINPR_ATTR_UNUSED PVOID param,
                                      WINPR_ATTR_UNUSED PVOID* context)
{
	return InitializeCriticalSectionAndSpinCount(&g_camera.lock, 4000);
}

static BOOL camera_global(void)
{
	return InitOnceExecuteOnce(&g_cameraOnce, camera_init_once, nullptr, nullptr);
}

static const char* camera_client_name(const CameraClient* cc)
{
	WINPR_ASSERT(cc);
	WINPR_ASSERT(cc->client);
	const freerdp_peer* peer = cc->client->context.peer;
	return (peer && peer->hostname) ? peer->hostname : "?";
}

/* ---- the sink and the still (g_camera.lock held) ---- */

static BOOL camera_image_alloc(ShadowCameraImage* image, BYTE** buffer, SHADOW_CAMERA_PIXEL format,
                               UINT32 width, UINT32 height)
{
	const size_t size = shadow_camera_image_size(format, width, height);
	if (size == 0)
		return FALSE;
	*buffer = calloc(1, size);
	if (!*buffer)
		return FALSE;
	return shadow_camera_image_wrap(image, format, width, height, *buffer, size);
}

static BOOL camera_draw_still(ShadowCameraImage* image)
{
	WINPR_ASSERT(image);

	switch (image->format)
	{
		case SHADOW_CAMERA_PIXEL_YUY2:
			return camera_placeholder_yuyv(image->planes[0], image->strides[0], image->width,
			                               image->height);
		case SHADOW_CAMERA_PIXEL_NV12:
			return camera_placeholder_nv12(image->planes[0], image->strides[0], image->planes[1],
			                               image->strides[1], image->width, image->height);
		default:
			return FALSE;
	}
}

static void camera_write_still_locked(void)
{
	if (!g_camera.sink)
		return;
	if (!g_camera.sink->Write(g_camera.sink, &g_camera.still, SHADOW_CAMERA_FRAME_PLACEHOLDER,
	                          nullptr))
		WLog_DBG(TAG, "camera: writing the still failed");
	g_camera.stills++;
	g_camera.stillShown = TRUE;
	g_camera.lastStillMs = GetTickCount64();
}

/* shows the still when no client camera streams: once for a sink that keeps the last frame,
 * again and again for one whose readers wait for frames */
static DWORD WINAPI camera_still_thread(WINPR_ATTR_UNUSED LPVOID arg)
{
	HANDLE events[] = { g_camera.stopEvent, g_camera.wakeEvent };

	for (;;)
	{
		DWORD timeout = INFINITE;

		EnterCriticalSection(&g_camera.lock);
		if (g_camera.sink)
		{
			const UINT64 now = GetTickCount64();
			const UINT32 interval = g_camera.sink->placeholderIntervalMs;
			const UINT64 idle = now - g_camera.lastClientFrameMs;
			const BOOL streaming = g_camera.ownerStreaming && (idle < CAMERA_STALL_MS);

			if (streaming)
				timeout = (DWORD)(CAMERA_STALL_MS - idle + 1);
			else
			{
				if (!g_camera.stillShown ||
				    ((interval > 0) && (now - g_camera.lastStillMs >= interval)))
					camera_write_still_locked();
				if (interval > 0)
					timeout = interval;
			}
		}
		LeaveCriticalSection(&g_camera.lock);

		const DWORD status = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, timeout);
		if (status == WAIT_OBJECT_0)
			break;
		if (status == WAIT_FAILED)
			break;
	}
	return 0;
}

static void camera_wake(void)
{
	if (g_camera.wakeEvent)
		(void)SetEvent(g_camera.wakeEvent);
}

/* ---- settings ---- */

static BOOL camera_parse_size(const char* text, UINT32* width, UINT32* height, UINT32* fps)
{
	unsigned w = 0;
	unsigned h = 0;
	unsigned f = *fps;
	char tail = '\0';
	const int n = sscanf(text, "%ux%u@%u%c", &w, &h, &f, &tail);
	if ((n != 2) && (n != 3))
		return FALSE;
	if ((w < 2) || (h < 2) || (w > CAMERA_MAX_WIDTH) || (h > CAMERA_MAX_HEIGHT) || (w % 2) ||
	    (h % 2) || (f < 1) || (f > CAMERA_MAX_FPS))
		return FALSE;
	*width = w;
	*height = h;
	*fps = f;
	return TRUE;
}

static ShadowCameraSink* camera_sink_from_settings(void)
{
	UINT32 width = CAMERA_DEFAULT_WIDTH;
	UINT32 height = CAMERA_DEFAULT_HEIGHT;
	UINT32 fps = CAMERA_DEFAULT_FPS;

	const char* size = getenv("FREERDP_SHADOW_CAMERA_SIZE");
	if (size && (*size != '\0') && !camera_parse_size(size, &width, &height, &fps))
		WLog_WARN(TAG,
		          "camera: FREERDP_SHADOW_CAMERA_SIZE=%s is not <width>x<height>[@<fps>] (even, "
		          "at most %ux%u at %u fps), using %" PRIu32 "x%" PRIu32 "@%" PRIu32,
		          size, CAMERA_MAX_WIDTH, CAMERA_MAX_HEIGHT, CAMERA_MAX_FPS, width, height, fps);

	const char* sink = getenv("FREERDP_SHADOW_CAMERA_SINK");
	if (!sink || (*sink == '\0'))
	{
#if defined(__linux__)
		sink = "v4l2";
#else
		sink = "none";
#endif
	}

	if ((strcmp(sink, "none") == 0) || (strcmp(sink, "off") == 0))
	{
		WLog_INFO(TAG, "camera: off (FREERDP_SHADOW_CAMERA_SINK=%s)", sink);
		return nullptr;
	}

	if (strncmp(sink, "file:", 5) == 0)
	{
		SHADOW_CAMERA_PIXEL format = SHADOW_CAMERA_PIXEL_YUY2;
		const char* name = getenv("FREERDP_SHADOW_CAMERA_FORMAT");
		if (name && (_stricmp(name, "nv12") == 0))
			format = SHADOW_CAMERA_PIXEL_NV12;
		else if (name && (*name != '\0') && (_stricmp(name, "yuyv") != 0) &&
		         (_stricmp(name, "yuy2") != 0))
			WLog_WARN(TAG,
			          "camera: FREERDP_SHADOW_CAMERA_FORMAT=%s is not yuyv or nv12, using yuyv",
			          name);
		return shadow_camera_sink_file_new(sink + 5, format, width, height, fps);
	}

#if defined(__linux__)
	if (strcmp(sink, "v4l2") == 0)
		return shadow_camera_sink_v4l2_new(nullptr, width, height, fps, nullptr);
	if (strncmp(sink, "v4l2:", 5) == 0)
		return shadow_camera_sink_v4l2_new(sink + 5, width, height, fps, nullptr);
#endif

	WLog_WARN(TAG, "camera: FREERDP_SHADOW_CAMERA_SINK=%s is not known, no camera", sink);
	return nullptr;
}

/* ---- ownership ---- */

static void camera_release(CameraDevice* dev, const char* why)
{
	WINPR_ASSERT(dev);

	EnterCriticalSection(&g_camera.lock);
	if (shadow_camera_owner_release(&g_camera.owner, dev))
	{
		WLog_INFO(TAG, "camera: \"%s\" of client %s released the virtual camera (%s)", dev->name,
		          camera_client_name(dev->cc), why);
		g_camera.ownerStreaming = FALSE;
		g_camera.stillShown = FALSE;
		camera_wake();
	}
	LeaveCriticalSection(&g_camera.lock);
}

static void camera_fail(CameraDevice* dev, const char* why)
{
	WINPR_ASSERT(dev);
	dev->state = CAMERA_DEVICE_FAILED;
	camera_release(dev, why);
}

/* ---- a device ---- */

static void camera_device_free(CameraDevice* dev)
{
	if (!dev)
		return;
	/* joins the device's thread: no callback runs after this */
	if (dev->ctx)
		camera_device_server_context_free(dev->ctx);
	camera_release(dev, "closed");
	shadow_camera_decoder_free(dev->decoder);
	free(dev->channel);
	free(dev->name);
	free(dev);
}

static CAM_SHARED_MSG_HEADER camera_header(const CameraDevice* dev, CAM_MSG_ID id)
{
	return (CAM_SHARED_MSG_HEADER){ .Version = dev->cc->version, .MessageId = id };
}

static void camera_refill(CameraDevice* dev)
{
	if (dev->state != CAMERA_DEVICE_STREAMING)
		return;

	const UINT32 rtt = shadow_client_autodetect_rtt_ms(dev->cc->client);
	const UINT32 target = shadow_camera_pacing_outstanding(dev->type.FrameRateNumerator,
	                                                       dev->type.FrameRateDenominator, rtt);
	while (dev->outstanding < target)
	{
		const CAM_SAMPLE_REQUEST request = { .Header = camera_header(dev, CAM_MSG_ID_SampleRequest),
			                                 .StreamIndex = dev->streamIndex };
		if (dev->ctx->SampleRequest(dev->ctx, &request) != CHANNEL_RC_OK)
		{
			camera_fail(dev, "sample request failed");
			return;
		}
		dev->outstanding++;
	}
}

static BOOL camera_device_channel_id_assigned(CameraDeviceServerContext* context,
                                              WINPR_ATTR_UNUSED UINT32 channelId)
{
	WINPR_ASSERT(context);
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);

	/* the client creates the channel before it reads what follows the Create Request */
	dev->state = CAMERA_DEVICE_ACTIVATING;
	const CAM_ACTIVATE_DEVICE_REQUEST request = { .Header = camera_header(
		                                              dev, CAM_MSG_ID_ActivateDeviceRequest) };
	if (context->ActivateDeviceRequest(context, &request) != CHANNEL_RC_OK)
	{
		camera_fail(dev, "activate request failed");
		return FALSE;
	}
	return TRUE;
}

static UINT camera_device_success(CameraDeviceServerContext* context,
                                  WINPR_ATTR_UNUSED const CAM_SUCCESS_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);

	switch (dev->state)
	{
		case CAMERA_DEVICE_ACTIVATING:
		{
			dev->state = CAMERA_DEVICE_STREAM_LIST;
			const CAM_STREAM_LIST_REQUEST request = { .Header = camera_header(
				                                          dev, CAM_MSG_ID_StreamListRequest) };
			if (context->StreamListRequest(context, &request) != CHANNEL_RC_OK)
				camera_fail(dev, "stream list request failed");
		}
		break;
		case CAMERA_DEVICE_STARTING:
			dev->state = CAMERA_DEVICE_STREAMING;
			WLog_INFO(TAG, "camera: \"%s\" of client %s streams %s", dev->name,
			          camera_client_name(dev->cc), dev->detail);
			camera_refill(dev);
			break;
		default:
			break;
	}
	return CHANNEL_RC_OK;
}

static UINT camera_device_error(CameraDeviceServerContext* context,
                                const CAM_ERROR_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);
	WINPR_ASSERT(response);

	WLog_WARN(TAG, "camera: \"%s\" of client %s answered error %" PRIu32 " in state %d", dev->name,
	          camera_client_name(dev->cc), (UINT32)response->ErrorCode, dev->state);
	camera_fail(dev, "client error");
	return CHANNEL_RC_OK;
}

static UINT camera_device_stream_list(CameraDeviceServerContext* context,
                                      const CAM_STREAM_LIST_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);
	WINPR_ASSERT(response);

	if (dev->state != CAMERA_DEVICE_STREAM_LIST)
		return CHANNEL_RC_OK;
	if (response->N_Descriptions == 0)
	{
		camera_fail(dev, "no stream");
		return CHANNEL_RC_OK;
	}

	/* a colour capture stream, the first one otherwise */
	BYTE index = 0;
	for (BYTE i = 0; i < response->N_Descriptions; i++)
	{
		const CAM_STREAM_DESCRIPTION* d = &response->StreamDescriptions[i];
		if ((d->FrameSourceTypes & CAM_STREAM_FRAME_SOURCE_TYPE_Color) &&
		    (d->StreamCategory == CAM_STREAM_CATEGORY_Capture))
		{
			index = i;
			break;
		}
	}

	dev->streamIndex = index;
	dev->state = CAMERA_DEVICE_MEDIA_TYPES;
	const CAM_MEDIA_TYPE_LIST_REQUEST request = {
		.Header = camera_header(dev, CAM_MSG_ID_MediaTypeListRequest), .StreamIndex = index
	};
	if (context->MediaTypeListRequest(context, &request) != CHANNEL_RC_OK)
		camera_fail(dev, "media type list request failed");
	return CHANNEL_RC_OK;
}

static UINT camera_device_media_types(CameraDeviceServerContext* context,
                                      const CAM_MEDIA_TYPE_LIST_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);
	WINPR_ASSERT(response);

	if (dev->state != CAMERA_DEVICE_MEDIA_TYPES)
		return CHANNEL_RC_OK;

	UINT32 width = 0;
	UINT32 height = 0;
	UINT32 fps = 0;
	ShadowCameraDecoders decoders = WINPR_C_ARRAY_INIT;
	EnterCriticalSection(&g_camera.lock);
	if (g_camera.sink)
	{
		width = g_camera.sink->width;
		height = g_camera.sink->height;
		fps = g_camera.sink->fps;
	}
	decoders = g_camera.decoders;
	LeaveCriticalSection(&g_camera.lock);

	size_t index = 0;
	if ((width == 0) ||
	    !shadow_camera_choose_media_type(response->MediaTypeDescriptions, response->N_Descriptions,
	                                     &decoders, width, height, fps, &index))
	{
		WLog_WARN(TAG,
		          "camera: \"%s\" of client %s: none of its %" PRIuz
		          " media types can be used (H.264 %s, MJPG %s)",
		          dev->name, camera_client_name(dev->cc), response->N_Descriptions,
		          decoders.h264 ? "yes" : "no", decoders.mjpg ? "yes" : "no");
		camera_fail(dev, "no media type");
		return CHANNEL_RC_OK;
	}

	dev->type = response->MediaTypeDescriptions[index];
	(void)_snprintf(dev->detail, sizeof(dev->detail), "%s %" PRIu32 "x%" PRIu32,
	                shadow_camera_format_name(dev->type.Format), dev->type.Width, dev->type.Height);
	WLog_INFO(TAG,
	          "camera: \"%s\" of client %s: chose %s at %" PRIu32 "/%" PRIu32 " fps, of %" PRIuz
	          " media types, for a camera of %" PRIu32 "x%" PRIu32,
	          dev->name, camera_client_name(dev->cc), dev->detail, dev->type.FrameRateNumerator,
	          dev->type.FrameRateDenominator, response->N_Descriptions, width, height);

	dev->decoder = shadow_camera_decoder_new(&dev->type);
	if (!dev->decoder)
	{
		camera_fail(dev, "no decoder");
		return CHANNEL_RC_OK;
	}

	CAM_START_STREAMS_REQUEST request = { .Header =
		                                      camera_header(dev, CAM_MSG_ID_StartStreamsRequest),
		                                  .N_Infos = 1 };
	request.StartStreamsInfo[0].StreamIndex = dev->streamIndex;
	request.StartStreamsInfo[0].MediaTypeDescription = dev->type;
	dev->state = CAMERA_DEVICE_STARTING;
	if (context->StartStreamsRequest(context, &request) != CHANNEL_RC_OK)
		camera_fail(dev, "start streams request failed");
	return CHANNEL_RC_OK;
}

static void camera_submit(CameraDevice* dev, const ShadowCameraImage* image)
{
	EnterCriticalSection(&g_camera.lock);
	if (g_camera.sink && shadow_camera_owner_is(&g_camera.owner, dev))
	{
		if (shadow_camera_image_letterbox(image, &g_camera.frame))
		{
			if (!g_camera.sink->Write(g_camera.sink, &g_camera.frame, SHADOW_CAMERA_FRAME_CLIENT,
			                          dev->detail))
				WLog_DBG(TAG, "camera: writing a frame failed");
			g_camera.clientFrames++;
			g_camera.ownerStreaming = TRUE;
			g_camera.stillShown = FALSE;
			g_camera.lastClientFrameMs = GetTickCount64();
		}
	}
	LeaveCriticalSection(&g_camera.lock);
}

static UINT camera_device_sample(CameraDeviceServerContext* context,
                                 const CAM_SAMPLE_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);
	WINPR_ASSERT(response);

	if (dev->state != CAMERA_DEVICE_STREAMING)
		return CHANNEL_RC_OK;
	if (dev->outstanding > 0)
		dev->outstanding--;
	dev->errors = 0;

	ShadowCameraImage image = WINPR_C_ARRAY_INIT;
	BOOL picture = FALSE;
	if (shadow_camera_decoder_decode(dev->decoder, response->Sample, response->SampleSize, &image,
	                                 &picture))
	{
		if (picture)
		{
			if (dev->frames++ == 0)
				WLog_INFO(TAG, "camera: first frame from \"%s\" of client %s: %s, %" PRIuz " bytes",
				          dev->name, camera_client_name(dev->cc), dev->detail,
				          response->SampleSize);
			camera_submit(dev, &image);
		}
	}
	else
		WLog_DBG(TAG, "camera: a %s sample of %" PRIuz " bytes was dropped", dev->detail,
		         response->SampleSize);

	camera_refill(dev);
	return CHANNEL_RC_OK;
}

static UINT camera_device_sample_error(CameraDeviceServerContext* context,
                                       const CAM_SAMPLE_ERROR_RESPONSE* response)
{
	CameraDevice* dev = context->userdata;
	WINPR_ASSERT(dev);
	WINPR_ASSERT(response);

	if (dev->state != CAMERA_DEVICE_STREAMING)
		return CHANNEL_RC_OK;
	if (dev->outstanding > 0)
		dev->outstanding--;
	if (++dev->errors >= CAMERA_MAX_SAMPLE_ERRORS)
	{
		WLog_WARN(TAG,
		          "camera: \"%s\" of client %s: %" PRIu32 " sample errors in a row (last %" PRIu32
		          "), given up",
		          dev->name, camera_client_name(dev->cc), dev->errors, (UINT32)response->ErrorCode);
		camera_fail(dev, "sample errors");
		return CHANNEL_RC_OK;
	}
	camera_refill(dev);
	return CHANNEL_RC_OK;
}

/* ---- a client's enumerator ---- */

static UINT camera_enum_select_version(CamDevEnumServerContext* context,
                                       const CAM_SELECT_VERSION_REQUEST* request)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(request);
	CameraClient* cc = context->userdata;
	WINPR_ASSERT(cc);

	BYTE version = request->Header.Version;
	if (version > CAMERA_PROTOCOL_VERSION)
		version = CAMERA_PROTOCOL_VERSION;
	if (version < 1)
		version = 1;
	cc->version = version;

	const CAM_SELECT_VERSION_RESPONSE response = {
		.Header = { .Version = version, .MessageId = CAM_MSG_ID_SelectVersionResponse }
	};
	return context->SelectVersionResponse(context, &response);
}

/* a channel name a client may give its device: printable, not one of the known channels */
static BOOL camera_channel_name_valid(const char* name)
{
	static const char* reserved[] = { RDPECAM_CONTROL_DVC_CHANNEL_NAME,
		                              "Microsoft::Windows::RDS::Graphics",
		                              "Microsoft::Windows::RDS::DisplayControl",
		                              "Microsoft::Windows::RDS::Geometry::v08.01",
		                              "Microsoft::Windows::RDS::Video::Control::v08.01",
		                              "Microsoft::Windows::RDS::Video::Data::v08.01",
		                              "AUDIO_INPUT",
		                              "AUDIO_PLAYBACK_DVC",
		                              "AUDIO_PLAYBACK_LOSSY_DVC",
		                              "ECHO",
		                              "FileRedirectorChannel",
		                              "rdpinpt",
		                              "URBDRC" };

	const size_t length = name ? strnlen(name, 256) : 0;
	if ((length == 0) || (length >= 256))
		return FALSE;
	for (size_t i = 0; i < length; i++)
	{
		if ((name[i] < 0x21) || (name[i] > 0x7e))
			return FALSE;
	}
	for (size_t i = 0; i < ARRAYSIZE(reserved); i++)
	{
		if (_stricmp(name, reserved[i]) == 0)
			return FALSE;
	}
	return TRUE;
}

static CameraDevice* camera_client_find_locked(CameraClient* cc, const char* channel)
{
	const size_t count = ArrayList_Count(cc->devices);
	for (size_t i = 0; i < count; i++)
	{
		CameraDevice* dev = ArrayList_GetItem(cc->devices, i);
		if (dev && (strcmp(dev->channel, channel) == 0))
			return dev;
	}
	return nullptr;
}

static BOOL camera_device_open(CameraDevice* dev)
{
	CameraClient* cc = dev->cc;
	CameraDeviceServerContext* ctx = camera_device_server_context_new(cc->client->vcm);
	if (!ctx)
		return FALSE;
	dev->ctx = ctx;
	ctx->virtualChannelName = _strdup(dev->channel);
	if (!ctx->virtualChannelName)
		return FALSE;
	ctx->protocolVersion = cc->version;
	ctx->userdata = dev;
	ctx->ChannelIdAssigned = camera_device_channel_id_assigned;
	ctx->SuccessResponse = camera_device_success;
	ctx->ErrorResponse = camera_device_error;
	ctx->StreamListResponse = camera_device_stream_list;
	ctx->MediaTypeListResponse = camera_device_media_types;
	ctx->SampleResponse = camera_device_sample;
	ctx->SampleErrorResponse = camera_device_sample_error;
	if (ctx->Initialize(ctx, FALSE) != CHANNEL_RC_OK)
		return FALSE;
	return ctx->Open(ctx) == CHANNEL_RC_OK;
}

static UINT camera_enum_device_added(CamDevEnumServerContext* context,
                                     const CAM_DEVICE_ADDED_NOTIFICATION* notification)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(notification);
	CameraClient* cc = context->userdata;
	WINPR_ASSERT(cc);

	if (!camera_channel_name_valid(notification->VirtualChannelName))
	{
		WLog_WARN(TAG, "camera: client %s announced a camera with an invalid channel name",
		          camera_client_name(cc));
		return CHANNEL_RC_OK;
	}

	CameraDevice* dev = calloc(1, sizeof(CameraDevice));
	if (!dev)
		return CHANNEL_RC_NO_MEMORY;
	dev->cc = cc;
	dev->channel = _strdup(notification->VirtualChannelName);
	dev->name = ConvertWCharToUtf8Alloc(notification->DeviceName, nullptr);
	if (!dev->channel || !dev->name)
	{
		camera_device_free(dev);
		return CHANNEL_RC_NO_MEMORY;
	}

	EnterCriticalSection(&cc->lock);
	const BOOL known = camera_client_find_locked(cc, dev->channel) != nullptr;
	LeaveCriticalSection(&cc->lock);
	if (known)
	{
		WLog_DBG(TAG, "camera: client %s announced \"%s\" twice", camera_client_name(cc),
		         dev->name);
		camera_device_free(dev);
		return CHANNEL_RC_OK;
	}

	/* the one line a refused camera gets */
	if (!cc->client->mayInteract)
	{
		WLog_INFO(TAG, "camera: \"%s\" of client %s refused: the client may not interact",
		          dev->name, camera_client_name(cc));
		camera_device_free(dev);
		return CHANNEL_RC_OK;
	}
	EnterCriticalSection(&g_camera.lock);
	const BOOL hasSink = g_camera.running && g_camera.sink;
	const SHADOW_CAMERA_CLAIM claim =
	    hasSink ? shadow_camera_owner_claim(&g_camera.owner, dev) : SHADOW_CAMERA_CLAIM_REFUSED;
	LeaveCriticalSection(&g_camera.lock);
	if (!hasSink)
	{
		WLog_INFO(TAG, "camera: \"%s\" of client %s refused: the server has no virtual camera",
		          dev->name, camera_client_name(cc));
		camera_device_free(dev);
		return CHANNEL_RC_OK;
	}
	if (claim != SHADOW_CAMERA_CLAIM_GRANTED)
	{
		WLog_INFO(TAG, "camera: \"%s\" of client %s refused: another client owns the camera",
		          dev->name, camera_client_name(cc));
		camera_device_free(dev);
		return CHANNEL_RC_OK;
	}

	WLog_INFO(TAG, "camera: \"%s\" of client %s owns the virtual camera (channel %s)", dev->name,
	          camera_client_name(cc), dev->channel);

	EnterCriticalSection(&cc->lock);
	const BOOL added = ArrayList_Append(cc->devices, dev);
	LeaveCriticalSection(&cc->lock);
	if (!added)
	{
		camera_device_free(dev);
		return CHANNEL_RC_NO_MEMORY;
	}
	if (!camera_device_open(dev))
	{
		WLog_ERR(TAG, "camera: opening the channel of \"%s\" failed", dev->name);
		camera_release(dev, "open failed");
	}
	return CHANNEL_RC_OK;
}

static UINT camera_enum_device_removed(CamDevEnumServerContext* context,
                                       const CAM_DEVICE_REMOVED_NOTIFICATION* notification)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(notification);
	CameraClient* cc = context->userdata;
	WINPR_ASSERT(cc);

	if (!notification->VirtualChannelName)
		return CHANNEL_RC_OK;

	EnterCriticalSection(&cc->lock);
	CameraDevice* dev = camera_client_find_locked(cc, notification->VirtualChannelName);
	if (dev)
		ArrayList_Remove(cc->devices, dev);
	LeaveCriticalSection(&cc->lock);

	if (dev)
	{
		WLog_INFO(TAG, "camera: client %s removed \"%s\"", camera_client_name(cc), dev->name);
		camera_device_free(dev);
	}
	return CHANNEL_RC_OK;
}

/* ---- the hooks ---- */

BOOL shadow_camera_server_start(WINPR_ATTR_UNUSED rdpShadowServer* server)
{
	if (!camera_global())
		return FALSE;

	ShadowCameraSink* sink = camera_sink_from_settings();
	ShadowCameraDecoders decoders = WINPR_C_ARRAY_INIT;
	shadow_camera_decoders_probe(&decoders);

	EnterCriticalSection(&g_camera.lock);
	g_camera.owner.owner = nullptr;
	g_camera.ownerStreaming = FALSE;
	g_camera.clientFrames = 0;
	g_camera.stills = 0;
	g_camera.stillShown = FALSE;
	g_camera.decoders = decoders;
	if (!g_camera.clients)
		g_camera.clients = ArrayList_New(FALSE);
	if (sink)
	{
		if (!camera_image_alloc(&g_camera.frame, &g_camera.frameBuffer, sink->format, sink->width,
		                        sink->height) ||
		    !camera_image_alloc(&g_camera.still, &g_camera.stillBuffer, sink->format, sink->width,
		                        sink->height) ||
		    !camera_draw_still(&g_camera.still))
		{
			WLog_ERR(TAG, "camera: unable to prepare the frames, no camera");
			sink->Free(sink);
			sink = nullptr;
		}
	}
	g_camera.sink = sink;
	g_camera.running = TRUE;
	/* at once: v4l2loopback with exclusive_caps=1 offers capture only once a writer wrote */
	camera_write_still_locked();
	LeaveCriticalSection(&g_camera.lock);

	if (!g_camera.clients)
		return FALSE;
	if (!sink)
		return TRUE;

	WLog_INFO(TAG,
	          "camera: %s camera %s %" PRIu32 "x%" PRIu32 " at %" PRIu32
	          " fps, decoders: H.264 %s, MJPG %s",
	          sink->name, shadow_camera_pixel_name(sink->format), sink->width, sink->height,
	          sink->fps, decoders.h264 ? "yes" : "no", decoders.mjpg ? "yes" : "no");

	g_camera.stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	g_camera.wakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!g_camera.stopEvent || !g_camera.wakeEvent)
		return FALSE;
	g_camera.thread = CreateThread(nullptr, 0, camera_still_thread, nullptr, 0, nullptr);
	return g_camera.thread != nullptr;
}

void shadow_camera_server_stop(WINPR_ATTR_UNUSED rdpShadowServer* server)
{
	if (!camera_global())
		return;

	if (g_camera.thread)
	{
		(void)SetEvent(g_camera.stopEvent);
		(void)WaitForSingleObject(g_camera.thread, INFINITE);
		(void)CloseHandle(g_camera.thread);
		g_camera.thread = nullptr;
	}
	if (g_camera.stopEvent)
		(void)CloseHandle(g_camera.stopEvent);
	if (g_camera.wakeEvent)
		(void)CloseHandle(g_camera.wakeEvent);
	g_camera.stopEvent = nullptr;
	g_camera.wakeEvent = nullptr;

	EnterCriticalSection(&g_camera.lock);
	g_camera.running = FALSE;
	if (g_camera.sink)
	{
		WLog_INFO(TAG, "camera: stopped, %" PRIu64 " client frames and %" PRIu64 " stills shown",
		          g_camera.clientFrames, g_camera.stills);
		g_camera.sink->Free(g_camera.sink);
	}
	g_camera.sink = nullptr;
	free(g_camera.frameBuffer);
	free(g_camera.stillBuffer);
	g_camera.frameBuffer = nullptr;
	g_camera.stillBuffer = nullptr;
	g_camera.frame = (ShadowCameraImage){ 0 };
	g_camera.still = (ShadowCameraImage){ 0 };
	if (g_camera.clients && (ArrayList_Count(g_camera.clients) == 0))
	{
		ArrayList_Free(g_camera.clients);
		g_camera.clients = nullptr;
	}
	LeaveCriticalSection(&g_camera.lock);
}

static CameraClient* camera_client_get(rdpShadowClient* client)
{
	CameraClient* found = nullptr;
	EnterCriticalSection(&g_camera.lock);
	const size_t count = g_camera.clients ? ArrayList_Count(g_camera.clients) : 0;
	for (size_t i = 0; i < count; i++)
	{
		CameraClient* cc = ArrayList_GetItem(g_camera.clients, i);
		if (cc && (cc->client == client))
		{
			found = cc;
			break;
		}
	}
	LeaveCriticalSection(&g_camera.lock);
	return found;
}

BOOL shadow_camera_client_init(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	if (!camera_global())
		return FALSE;

	CameraClient* cc = calloc(1, sizeof(CameraClient));
	if (!cc)
		return FALSE;
	cc->client = client;
	cc->version = 1;
	if (!InitializeCriticalSectionAndSpinCount(&cc->lock, 4000))
	{
		free(cc);
		return FALSE;
	}
	cc->devices = ArrayList_New(FALSE);

	EnterCriticalSection(&g_camera.lock);
	/* a server that was not started has no camera, its clients are still kept track of */
	if (!g_camera.clients)
		g_camera.clients = ArrayList_New(FALSE);
	const BOOL added = cc->devices && g_camera.clients && ArrayList_Append(g_camera.clients, cc);
	LeaveCriticalSection(&g_camera.lock);
	if (!added)
	{
		ArrayList_Free(cc->devices);
		DeleteCriticalSection(&cc->lock);
		free(cc);
		return FALSE;
	}
	return TRUE;
}

void shadow_camera_client_open(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	if (!camera_global())
		return;

	CameraClient* cc = camera_client_get(client);
	if (!cc || cc->enumerator)
		return;

	EnterCriticalSection(&g_camera.lock);
	const BOOL hasSink = g_camera.running && g_camera.sink;
	LeaveCriticalSection(&g_camera.lock);
	if (!hasSink)
		return;

	CamDevEnumServerContext* enumerator = cam_dev_enum_server_context_new(client->vcm);
	if (!enumerator)
		return;
	enumerator->userdata = cc;
	enumerator->SelectVersionRequest = camera_enum_select_version;
	enumerator->DeviceAddedNotification = camera_enum_device_added;
	enumerator->DeviceRemovedNotification = camera_enum_device_removed;
	if ((enumerator->Initialize(enumerator, FALSE) != CHANNEL_RC_OK) ||
	    (enumerator->Open(enumerator) != CHANNEL_RC_OK))
	{
		WLog_WARN(TAG, "camera: opening the camera enumerator of client %s failed",
		          camera_client_name(cc));
		cam_dev_enum_server_context_free(enumerator);
		return;
	}
	cc->enumerator = enumerator;
	WLog_DBG(TAG, "camera: enumerator of client %s opened", camera_client_name(cc));
}

void shadow_camera_client_uninit(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	if (!camera_global())
		return;

	CameraClient* cc = camera_client_get(client);
	if (!cc)
		return;

	EnterCriticalSection(&g_camera.lock);
	ArrayList_Remove(g_camera.clients, cc);
	LeaveCriticalSection(&g_camera.lock);

	/* the enumerator first: no device is added after its thread is joined */
	if (cc->enumerator)
		cam_dev_enum_server_context_free(cc->enumerator);
	cc->enumerator = nullptr;

	for (;;)
	{
		EnterCriticalSection(&cc->lock);
		CameraDevice* dev = nullptr;
		if (ArrayList_Count(cc->devices) > 0)
		{
			dev = ArrayList_GetItem(cc->devices, 0);
			ArrayList_RemoveAt(cc->devices, 0);
		}
		LeaveCriticalSection(&cc->lock);
		if (!dev)
			break;
		camera_device_free(dev);
	}

	ArrayList_Free(cc->devices);
	DeleteCriticalSection(&cc->lock);
	free(cc);
}
