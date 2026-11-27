/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, Media Foundation backend
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

/* The cameras of Windows, through Media Foundation, selected with /dvc:rdpecam[,sys:mf]
 *
 * Every video capture device of MFEnumDeviceSources is a camera, named by its friendly name;
 * its symbolic link identifies it and gives its channel name. The media types announced are
 * the native ones of the first format of the channel's list the camera has (H.264, MJPG, YUY2,
 * NV12, I420, passed through unchanged). A camera with none of them is announced as NV12, which
 * the source reader makes from its native frames (MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_
 * PROCESSING decodes, converts and scales; a mode above 1920x1080 is offered scaled down).
 *
 * /dvc:rdpecam,sys:mf,file:<path or URL>[,name:<camera name>] plays a media file in a loop as the
 * only camera, decoded to NV12 by the same source reader, at the pace of its time stamps: the
 * capture code runs on a machine without a camera (CI).
 *
 * mfplat.dll, mf.dll and mfreadwrite.dll are loaded at run time from System32, and stay loaded,
 * and the GUIDs are spelled out (as in libfreerdp/codec/h264_mf.c and av1_mf.c): the client still
 * starts on a Windows without Media Foundation (N editions, Windows Server without its Media
 * Foundation feature). There, or with a file that does not open, the backend still registers and
 * offers no camera, with one warning: a failed entry would fail the dynamic channels with it.
 *
 * Threads: Media Foundation runs in the multithreaded apartment of threads of this backend only,
 * never in the caller's thread, whose COM apartment is not ours to choose. Enumeration and the
 * media type probe run in a short-lived thread, each stream in its capture thread, which reads
 * the samples synchronously and hands them to the sample callback. A capture error while the
 * stream runs closes the reader and opens it again, with a growing delay, until the stream is
 * stopped: the requests of the server get samples again once the camera is back. The caller
 * waits for these threads a bounded time; a thread that does not end in it is left to end alone
 * with its own copy of what it needs, and never calls back after that. */

/* before any header that pulls in the COM interfaces */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/cmdline.h>
#include <winpr/collections.h>
#include <winpr/interlocked.h>
#include <winpr/library.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "camera.h"

#define TAG CHANNELS_TAG("rdpecam-mf.client")

/* the channel name of a camera: this prefix and a hash of its symbolic link, so that a camera
 * keeps its channel across enumerations; the file is the first camera, as in the movie backend */
#define CAM_MF_DEVICE_ID_PREFIX "RDCamera_Device_"
#define CAM_MF_FILE_DEVICE_ID CAM_MF_DEVICE_ID_PREFIX "0"

#define CAM_MF_FRAMERATE_NUMERATOR_DEFAULT 30
#define CAM_MF_FRAMERATE_DENOMINATOR_DEFAULT 1

/* the largest frame announced: an NV12 or YUY2 sample of it still fits into the sample
 * response buffer of the channel (ECAM_SAMPLE_RESPONSE_BUFFER_SIZE), as in the AVFoundation
 * backend; a larger mode is offered as NV12 scaled to fit 1920x1080 and 1280x720 */
#define CAM_MF_MAX_WIDTH 1920
#define CAM_MF_MAX_HEIGHT 1080
#define CAM_MF_SMALL_WIDTH 1280
#define CAM_MF_SMALL_HEIGHT 720

/* how long the caller waits for an enumeration or a probe, and for a capture to open */
#define CAM_MF_CALL_TIMEOUT_MS 10000
/* how long a stop waits for the capture thread, then again after shutting its media source
 * down (which ends a ReadSample that waits for a camera that sends nothing), before it leaves
 * the thread to end alone */
#define CAM_MF_STOP_TIMEOUT_MS 3000
#define CAM_MF_SHUTDOWN_TIMEOUT_MS 2000

/* the delays between two attempts to open a capture again after an error */
#define CAM_MF_REOPEN_FIRST_DELAY_MS 500
#define CAM_MF_REOPEN_MAX_DELAY_MS 5000

/* longest sleep of the capture thread, so that a stop request is seen quickly */
#define CAM_MF_MAX_SLEEP_MS 50

/* when the file playback fell behind by more than this, restart its clock instead of catching
 * up */
#define CAM_MF_MAX_LATE_NS (1000ull * 1000ull * 1000ull)

#if defined(CAM_MF_TESTING)
/* test hooks, built into mf/test only: a failed Media Foundation load, and the n-th sample read
 * of the process failing once */
extern volatile LONG cam_mf_test_fail_load;
extern volatile LONG cam_mf_test_fail_read_after;
volatile LONG cam_mf_test_fail_load = 0;
volatile LONG cam_mf_test_fail_read_after = 0;
#endif

/* threads left to end alone: Media Foundation is then not shut down */
static volatile LONG cam_mf_abandoned_threads = 0;

/* the GUIDs, spelled out (like h264_mf.c and av1_mf.c) so that no Media Foundation import
 * library is needed */
#define CAM_MF_FOURCC_GUID(a, b, c, d)                                              \
	{ (UINT32)(a) | ((UINT32)(b) << 8) | ((UINT32)(c) << 16) | ((UINT32)(d) << 24), \
	  0x0000,                                                                       \
	  0x0010,                                                                       \
	  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }

static const GUID cam_mf_GUID_NULL = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
static const GUID cam_mf_IID_IMFMediaSource = {
	0x279a808d, 0xaec7, 0x40c8, { 0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66 }
};
static const GUID cam_mf_IID_IMF2DBuffer2 = {
	0x33ae5ea6, 0x4316, 0x436f, { 0x8d, 0xdd, 0xd7, 0x3d, 0x22, 0xf8, 0x29, 0xec }
};
static const GUID cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE = {
	0xc60ac5fe, 0x252a, 0x478f, { 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3 }
};
static const GUID cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID = {
	0x8ac3587a, 0x4ae7, 0x42d8, { 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f }
};
static const GUID cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK = {
	0x58f0aad8, 0x22bf, 0x4f8a, { 0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f }
};
static const GUID cam_mf_MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME = {
	0x60d0e559, 0x52f8, 0x4fa2, { 0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01 }
};
static const GUID cam_mf_MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = {
	0x0f81da2c, 0xb537, 0x4672, { 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3 }
};
static const GUID cam_mf_MF_LOW_LATENCY = {
	0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee }
};
static const GUID cam_mf_MF_MT_MAJOR_TYPE = {
	0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f }
};
static const GUID cam_mf_MF_MT_SUBTYPE = {
	0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 }
};
static const GUID cam_mf_MF_MT_FRAME_SIZE = {
	0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d }
};
static const GUID cam_mf_MF_MT_FRAME_RATE = {
	0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 }
};
static const GUID cam_mf_MFMediaType_Video = CAM_MF_FOURCC_GUID('v', 'i', 'd', 's');
static const GUID cam_mf_MFVideoFormat_H264 = CAM_MF_FOURCC_GUID('H', '2', '6', '4');
static const GUID cam_mf_MFVideoFormat_MJPG = CAM_MF_FOURCC_GUID('M', 'J', 'P', 'G');
static const GUID cam_mf_MFVideoFormat_YUY2 = CAM_MF_FOURCC_GUID('Y', 'U', 'Y', '2');
static const GUID cam_mf_MFVideoFormat_NV12 = CAM_MF_FOURCC_GUID('N', 'V', '1', '2');
static const GUID cam_mf_MFVideoFormat_I420 = CAM_MF_FOURCC_GUID('I', '4', '2', '0');
static const GUID cam_mf_MFVideoFormat_IYUV = CAM_MF_FOURCC_GUID('I', 'Y', 'U', 'V');

typedef HRESULT(WINAPI* pfnMFStartup)(ULONG Version, DWORD dwFlags);
typedef HRESULT(WINAPI* pfnMFShutdown)(void);
typedef HRESULT(WINAPI* pfnMFCreateAttributes)(IMFAttributes** ppMFAttributes, UINT32 cInitialSize);
typedef HRESULT(WINAPI* pfnMFCreateMediaType)(IMFMediaType** ppMFType);
typedef HRESULT(WINAPI* pfnMFEnumDeviceSources)(IMFAttributes* pAttributes,
                                                IMFActivate*** pppSourceActivate,
                                                UINT32* pcSourceActivate);
typedef HRESULT(WINAPI* pfnMFCreateDeviceSource)(IMFAttributes* pAttributes,
                                                 IMFMediaSource** ppSource);
typedef HRESULT(WINAPI* pfnMFCreateSourceReaderFromMediaSource)(IMFMediaSource* pMediaSource,
                                                                IMFAttributes* pAttributes,
                                                                IMFSourceReader** ppSourceReader);
typedef HRESULT(WINAPI* pfnMFCreateSourceReaderFromURL)(LPCWSTR pwszURL, IMFAttributes* pAttributes,
                                                        IMFSourceReader** ppSourceReader);

/** the functions of Media Foundation; their DLLs stay loaded for the life of the process */
typedef struct
{
	BOOL started;

	pfnMFStartup MFStartup;
	pfnMFShutdown MFShutdown;
	pfnMFCreateAttributes MFCreateAttributes;
	pfnMFCreateMediaType MFCreateMediaType;
	pfnMFEnumDeviceSources MFEnumDeviceSources;
	pfnMFCreateDeviceSource MFCreateDeviceSource;
	pfnMFCreateSourceReaderFromMediaSource MFCreateSourceReaderFromMediaSource;
	pfnMFCreateSourceReaderFromURL MFCreateSourceReaderFromURL;
} CamMfApi;

/** what opens a camera or the file; copied by each thread that uses it */
typedef struct
{
	CamMfApi api;
	BOOL file;
	/* the symbolic link of a camera, or the path or URL of the file */
	WCHAR* link;
	char* name;
} CamMfTarget;

/** a native media type of a camera, or the video of the file */
typedef struct
{
	GUID subtype;
	/* the format of the channel it is, 0 when none */
	CAM_MEDIA_FORMAT format;
	UINT32 width;
	UINT32 height;
	UINT32 frameRateNumerator;
	UINT32 frameRateDenominator;
} CamMfNativeType;

/** one capture: shared by the stream and its thread, freed by the last of them */
typedef struct
{
	volatile LONG refs;
	/* asked to capture; cleared by a stop */
	volatile LONG running;

	/* startError and source */
	CRITICAL_SECTION lock;
	/* set by the thread once its source reader runs, or failed (startError) */
	HANDLE ready;
	CAM_ERROR_CODE startError;
	/* the running source, shut down by a stop that waited too long for the thread */
	IMFMediaSource* source;

	/* detached, and held around each sample callback */
	CRITICAL_SECTION callbackLock;
	/* the stream gave the thread up: no callback any more */
	BOOL detached;

	CamMfTarget target;
	CAM_MEDIA_TYPE_DESCRIPTION mediaType;
	/* NV12 made by the source reader, not a native type */
	BOOL convert;
	CameraDevice* dev;
	size_t streamIndex;
	ICamHalSampleCapturedCallback sampleCallback;

	/* a YUY2 sample, packed when Media Foundation pads its lines */
	BYTE* packed;
	size_t packedSize;
} CamMfCapture;

typedef struct
{
	/* serializes start, stop and free */
	CRITICAL_SECTION control;
	HANDLE thread;
	CamMfCapture* capture;
} CamMfStream;

typedef struct s_CamMfHal CamMfHal;

typedef struct
{
	CamMfHal* hal;
	char deviceId[32];
	CamMfTarget target;

	/* the native media types, read on the first media type request */
	BOOL probed;
	CamMfNativeType* types;
	size_t nTypes;

	CamMfStream stream;
} CamMfDevice;

struct s_CamMfHal
{
	ICamHal iHal;
	/* Media Foundation loaded: without it, no camera is offered */
	BOOL available;
	CamMfApi api;

	/* file:<path or URL> and name:<camera name> */
	char* file;
	char* name;

	wHashTable* devices; /* Index: deviceId, Value: CamMfDevice */
};

static void cam_mf_release_media_type(IMFMediaType** type)
{
	WINPR_ASSERT(type);
	if (*type)
		IMFMediaType_Release(*type);
	*type = nullptr;
}

static void cam_mf_target_free(CamMfTarget* target)
{
	if (!target)
		return;
	free(target->link);
	free(target->name);
	target->link = nullptr;
	target->name = nullptr;
}

WINPR_ATTR_NODISCARD
static BOOL cam_mf_target_set(CamMfTarget* target, const CamMfApi* api, BOOL file,
                              const WCHAR* link, const char* name)
{
	WINPR_ASSERT(target);
	WINPR_ASSERT(api);
	WINPR_ASSERT(link);
	WINPR_ASSERT(name);

	target->api = *api;
	target->file = file;
	target->link = _wcsdup(link);
	target->name = _strdup(name);
	if (!target->link || !target->name)
	{
		cam_mf_target_free(target);
		return FALSE;
	}
	return TRUE;
}

/** @return the format of the channel a Media Foundation subtype is, 0 when none */
static CAM_MEDIA_FORMAT cam_mf_format_of_subtype(const GUID* subtype)
{
	WINPR_ASSERT(subtype);

	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_H264))
		return CAM_MEDIA_FORMAT_H264;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_MJPG))
		return CAM_MEDIA_FORMAT_MJPG;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_YUY2))
		return CAM_MEDIA_FORMAT_YUY2;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_NV12))
		return CAM_MEDIA_FORMAT_NV12;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_I420) ||
	    IsEqualGUID(subtype, &cam_mf_MFVideoFormat_IYUV))
		return CAM_MEDIA_FORMAT_I420;
	/* RGB is left out: Media Foundation's RGB frames are bottom-up, the channel's top-down */
	return (CAM_MEDIA_FORMAT)0;
}

static const char* cam_mf_format_name(CAM_MEDIA_FORMAT format)
{
	switch (format)
	{
		case CAM_MEDIA_FORMAT_H264:
			return "H264";
		case CAM_MEDIA_FORMAT_MJPG:
			return "MJPG";
		case CAM_MEDIA_FORMAT_YUY2:
			return "YUY2";
		case CAM_MEDIA_FORMAT_NV12:
			return "NV12";
		case CAM_MEDIA_FORMAT_I420:
			return "I420";
		case CAM_MEDIA_FORMAT_RGB24:
			return "RGB24";
		case CAM_MEDIA_FORMAT_RGB32:
			return "RGB32";
		default:
			return "unknown";
	}
}

/** @return the size of a frame of a raw format, 0 for a compressed one */
static UINT64 cam_mf_raw_frame_size(CAM_MEDIA_FORMAT format, UINT32 width, UINT32 height)
{
	switch (format)
	{
		case CAM_MEDIA_FORMAT_NV12:
		case CAM_MEDIA_FORMAT_I420:
			return 3ull * width * height / 2ull;
		case CAM_MEDIA_FORMAT_YUY2:
			return 2ull * width * height;
		default:
			return 0;
	}
}

/** @return a > b for the frame rates a = an / ad and b = bn / bd */
static BOOL cam_mf_rate_greater(UINT32 an, UINT32 ad, UINT32 bn, UINT32 bd)
{
	return (1ull * an * bd) > (1ull * bn * ad);
}

/** the size of a frame scaled down, its aspect kept, to fit into maxWidth x maxHeight (even) */
static void cam_mf_fit(UINT32 width, UINT32 height, UINT32 maxWidth, UINT32 maxHeight,
                       UINT32* fitWidth, UINT32* fitHeight)
{
	WINPR_ASSERT(fitWidth);
	WINPR_ASSERT(fitHeight);

	UINT64 w = width;
	UINT64 h = height;
	if ((width > maxWidth) || (height > maxHeight))
	{
		w = maxWidth;
		h = (width > 0) ? (1ull * height * maxWidth / width) : 0;
		if (h > maxHeight)
		{
			h = maxHeight;
			w = (height > 0) ? (1ull * width * maxHeight / height) : 0;
		}
	}
	*fitWidth = (UINT32)w & ~1u;
	*fitHeight = (UINT32)h & ~1u;
}

static HMODULE cam_mf_load_system_library(const char* name)
{
	/* from System32 only, never from the current or the application's directory */
	return LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/** loads Media Foundation, @return nullptr on success, else why it is not available */
WINPR_ATTR_NODISCARD
static const char* cam_mf_api_load(CamMfApi* api, HRESULT* hr)
{
	WINPR_ASSERT(api);
	WINPR_ASSERT(hr);

	*hr = S_OK;
#if defined(CAM_MF_TESTING)
	if (cam_mf_test_fail_load)
		return "mfplat.dll not found (test)";
#endif

	/* never freed: a capture thread left to end alone may still use them */
	HMODULE mfplat = cam_mf_load_system_library("mfplat.dll");
	HMODULE mf = cam_mf_load_system_library("mf.dll");
	HMODULE mfreadwrite = cam_mf_load_system_library("mfreadwrite.dll");
	if (!mfplat)
		return "mfplat.dll not found";
	if (!mf)
		return "mf.dll not found";
	if (!mfreadwrite)
		return "mfreadwrite.dll not found";

	api->MFStartup = GetProcAddressAs(mfplat, "MFStartup", pfnMFStartup);
	api->MFShutdown = GetProcAddressAs(mfplat, "MFShutdown", pfnMFShutdown);
	api->MFCreateAttributes = GetProcAddressAs(mfplat, "MFCreateAttributes", pfnMFCreateAttributes);
	api->MFCreateMediaType = GetProcAddressAs(mfplat, "MFCreateMediaType", pfnMFCreateMediaType);
	api->MFEnumDeviceSources = GetProcAddressAs(mf, "MFEnumDeviceSources", pfnMFEnumDeviceSources);
	api->MFCreateDeviceSource =
	    GetProcAddressAs(mf, "MFCreateDeviceSource", pfnMFCreateDeviceSource);
	api->MFCreateSourceReaderFromMediaSource = GetProcAddressAs(
	    mfreadwrite, "MFCreateSourceReaderFromMediaSource", pfnMFCreateSourceReaderFromMediaSource);
	api->MFCreateSourceReaderFromURL = GetProcAddressAs(mfreadwrite, "MFCreateSourceReaderFromURL",
	                                                    pfnMFCreateSourceReaderFromURL);

	if (!api->MFStartup || !api->MFShutdown || !api->MFCreateAttributes ||
	    !api->MFCreateMediaType || !api->MFEnumDeviceSources || !api->MFCreateDeviceSource ||
	    !api->MFCreateSourceReaderFromMediaSource || !api->MFCreateSourceReaderFromURL)
		return "a function is missing";

	*hr = api->MFStartup(MF_VERSION, MFSTARTUP_FULL);
	if (FAILED(*hr))
		return "MFStartup failed";
	api->started = TRUE;
	return nullptr;
}

/** a call run in a thread of the multithreaded apartment; data is freed with the call */
typedef BOOL (*CamMfMtaFn)(void* data);

typedef struct
{
	volatile LONG refs;
	CamMfMtaFn fn;
	void* data;
	void (*freeData)(void* data);
	BOOL result;
} CamMfMtaCall;

static void cam_mf_call_release(CamMfMtaCall* call)
{
	if (!call || (InterlockedDecrement(&call->refs) != 0))
		return;
	if (call->freeData)
		call->freeData(call->data);
	free(call);
}

static DWORD WINAPI cam_mf_mta_thread(LPVOID param)
{
	CamMfMtaCall* call = (CamMfMtaCall*)param;
	WINPR_ASSERT(call);

	const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr))
	{
		call->result = call->fn(call->data);
		CoUninitialize();
	}
	else
		WLog_ERR(TAG, "CoInitializeEx failure: 0x%08" PRIX32, (UINT32)hr);

	cam_mf_call_release(call);
	return 0;
}

/**
 * runs fn(data) in a thread of the multithreaded apartment and waits for it a bounded time
 *
 * @return the call, its data and result readable until cam_mf_call_release, or nullptr when it
 * did not end in time (it then frees data when it ends) or did not start (data freed)
 */
WINPR_ATTR_NODISCARD
static CamMfMtaCall* cam_mf_call_in_mta(CamMfMtaFn fn, void* data, void (*freeData)(void*))
{
	CamMfMtaCall* call = (CamMfMtaCall*)calloc(1, sizeof(CamMfMtaCall));
	if (!call)
	{
		freeData(data);
		return nullptr;
	}
	call->refs = 2;
	call->fn = fn;
	call->data = data;
	call->freeData = freeData;

	HANDLE thread = CreateThread(nullptr, 0, cam_mf_mta_thread, call, 0, nullptr);
	if (!thread)
	{
		WLog_ERR(TAG, "CreateThread failure");
		call->refs = 1;
		cam_mf_call_release(call);
		return nullptr;
	}

	const DWORD status = WaitForSingleObject(thread, CAM_MF_CALL_TIMEOUT_MS);
	(void)CloseHandle(thread);
	if (status != WAIT_OBJECT_0)
	{
		WLog_ERR(TAG, "Media Foundation did not answer in %" PRIu32 " ms, leaving its thread",
		         (UINT32)CAM_MF_CALL_TIMEOUT_MS);
		(void)InterlockedIncrement(&cam_mf_abandoned_threads);
		cam_mf_call_release(call);
		return nullptr;
	}
	return call;
}

/** @return the frame size and rate of a media type, the rate defaulting to 30 fps */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_read_type(IMFMediaType* type, CamMfNativeType* native)
{
	WINPR_ASSERT(type);
	WINPR_ASSERT(native);

	const CamMfNativeType empty = WINPR_C_ARRAY_INIT;
	*native = empty;
	if (FAILED(IMFMediaType_GetGUID(type, &cam_mf_MF_MT_SUBTYPE, &native->subtype)))
		return FALSE;

	UINT64 size = 0;
	if (FAILED(IMFMediaType_GetUINT64(type, &cam_mf_MF_MT_FRAME_SIZE, &size)))
		return FALSE;
	native->width = (UINT32)(size >> 32);
	native->height = (UINT32)(size & 0xFFFFFFFF);

	UINT64 rate = 0;
	if (SUCCEEDED(IMFMediaType_GetUINT64(type, &cam_mf_MF_MT_FRAME_RATE, &rate)) &&
	    ((rate >> 32) != 0) && ((rate & 0xFFFFFFFF) != 0))
	{
		native->frameRateNumerator = (UINT32)(rate >> 32);
		native->frameRateDenominator = (UINT32)(rate & 0xFFFFFFFF);
	}
	else
	{
		native->frameRateNumerator = CAM_MF_FRAMERATE_NUMERATOR_DEFAULT;
		native->frameRateDenominator = CAM_MF_FRAMERATE_DENOMINATOR_DEFAULT;
	}

	native->format = cam_mf_format_of_subtype(&native->subtype);
	return (native->width > 0) && (native->height > 0);
}

/**
 * opens the source reader of a camera or of the file, its first video stream selected; logs
 * nothing, the caller says what failed
 *
 * @return S_OK, or the first failure and in what (what)
 */
WINPR_ATTR_NODISCARD
static HRESULT cam_mf_open_reader(const CamMfTarget* target, BOOL processing,
                                  IMFMediaSource** psource, IMFSourceReader** preader,
                                  const char** what)
{
	WINPR_ASSERT(target);
	WINPR_ASSERT(psource);
	WINPR_ASSERT(preader);
	WINPR_ASSERT(what);

	const CamMfApi* api = &target->api;
	IMFAttributes* sourceAttributes = nullptr;
	IMFAttributes* readerAttributes = nullptr;
	IMFMediaSource* source = nullptr;
	IMFSourceReader* reader = nullptr;

	*psource = nullptr;
	*preader = nullptr;
	*what = "MFCreateAttributes";

	HRESULT hr = api->MFCreateAttributes(&readerAttributes, 2);
	if (FAILED(hr))
		goto out;
	/* the source reader decodes, converts and scales to the output type that is set */
	if (processing)
	{
		hr = IMFAttributes_SetUINT32(
		    readerAttributes, &cam_mf_MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
		if (FAILED(hr))
			goto out;
	}
	if (!target->file)
	{
		hr = IMFAttributes_SetUINT32(readerAttributes, &cam_mf_MF_LOW_LATENCY, TRUE);
		if (FAILED(hr))
			goto out;
	}

	if (target->file)
	{
		*what = "MFCreateSourceReaderFromURL";
		hr = api->MFCreateSourceReaderFromURL(target->link, readerAttributes, &reader);
		if (FAILED(hr))
			goto out;

		/* the source of the file, for a stop to shut it down; not needed to play it */
		if (FAILED(IMFSourceReader_GetServiceForStream(
		        reader, MF_SOURCE_READER_MEDIASOURCE, &cam_mf_GUID_NULL, &cam_mf_IID_IMFMediaSource,
		        (void**)&source)))
			source = nullptr;
	}
	else
	{
		hr = api->MFCreateAttributes(&sourceAttributes, 2);
		if (FAILED(hr))
			goto out;
		hr = IMFAttributes_SetGUID(sourceAttributes, &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
		                           &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
		if (FAILED(hr))
			goto out;
		hr = IMFAttributes_SetString(
		    sourceAttributes, &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
		    target->link);
		if (FAILED(hr))
			goto out;

		*what = "MFCreateDeviceSource";
		hr = api->MFCreateDeviceSource(sourceAttributes, &source);
		if (FAILED(hr))
			goto out;

		/* released with the reader, which then shuts the camera down */
		*what = "MFCreateSourceReaderFromMediaSource";
		hr = api->MFCreateSourceReaderFromMediaSource(source, readerAttributes, &reader);
		if (FAILED(hr))
			goto out;
	}

	*what = "SetStreamSelection";
	hr = IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	if (SUCCEEDED(hr))
		hr = IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		                                        TRUE);
	if (FAILED(hr))
		goto out;

	*psource = source;
	*preader = reader;
	source = nullptr;
	reader = nullptr;

out:
	if (reader)
		IMFSourceReader_Release(reader);
	if (source)
	{
		if (!target->file)
			(void)IMFMediaSource_Shutdown(source);
		IMFMediaSource_Release(source);
	}
	if (sourceAttributes)
		IMFAttributes_Release(sourceAttributes);
	if (readerAttributes)
		IMFAttributes_Release(readerAttributes);
	return hr;
}

static void cam_mf_close_reader(const CamMfTarget* target, IMFMediaSource** source,
                                IMFSourceReader** reader)
{
	WINPR_ASSERT(target);
	WINPR_ASSERT(source);
	WINPR_ASSERT(reader);

	if (*reader)
		IMFSourceReader_Release(*reader);
	if (*source)
	{
		/* the reader shut the camera down already, a second Shutdown does nothing */
		if (!target->file)
			(void)IMFMediaSource_Shutdown(*source);
		IMFMediaSource_Release(*source);
	}
	*reader = nullptr;
	*source = nullptr;
}

/** a probe: its own copy of the target, and the types it read */
typedef struct
{
	CamMfTarget target;
	CamMfNativeType* types;
	size_t nTypes;
	HRESULT hr;
	const char* what;
} CamMfProbe;

static void cam_mf_probe_free(void* data)
{
	CamMfProbe* probe = (CamMfProbe*)data;
	if (!probe)
		return;
	cam_mf_target_free(&probe->target);
	free(probe->types);
	free(probe);
}

/** reads the native media types of a camera, or the size and rate of the file (MTA) */
static BOOL cam_mf_probe_in_mta(void* data)
{
	CamMfProbe* probe = (CamMfProbe*)data;
	WINPR_ASSERT(probe);

	IMFMediaSource* source = nullptr;
	IMFSourceReader* reader = nullptr;
	BOOL rc = FALSE;

	probe->hr = cam_mf_open_reader(&probe->target, FALSE, &source, &reader, &probe->what);
	if (FAILED(probe->hr))
		return FALSE;

	probe->what = "GetNativeMediaType";
	size_t capacity = 0;
	for (DWORD index = 0;; index++)
	{
		IMFMediaType* type = nullptr;
		const HRESULT hr = IMFSourceReader_GetNativeMediaType(
		    reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type);
		if (hr == MF_E_NO_MORE_TYPES)
			break;
		if (FAILED(hr))
		{
			probe->hr = hr;
			goto out;
		}

		CamMfNativeType native = WINPR_C_ARRAY_INIT;
		const BOOL valid = cam_mf_read_type(type, &native);
		cam_mf_release_media_type(&type);
		if (!valid)
			continue;

		if (probe->nTypes == capacity)
		{
			const size_t newCapacity = capacity ? 2 * capacity : 32;
			CamMfNativeType* types =
			    (CamMfNativeType*)realloc(probe->types, newCapacity * sizeof(CamMfNativeType));
			if (!types)
			{
				probe->hr = E_OUTOFMEMORY;
				goto out;
			}
			probe->types = types;
			capacity = newCapacity;
		}
		probe->types[probe->nTypes++] = native;

		/* the video of a file has one type: the decoded frames take its size and rate */
		if (probe->target.file)
			break;
	}

	if (probe->nTypes == 0)
	{
		probe->hr = MF_E_INVALIDMEDIATYPE;
		goto out;
	}
	rc = TRUE;

out:
	cam_mf_close_reader(&probe->target, &source, &reader);
	return rc;
}

/** reads the native media types once; @return why that failed in hr and what, if it did */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_device_probe_quiet(CamMfDevice* device, HRESULT* hr, const char** what)
{
	WINPR_ASSERT(device);
	WINPR_ASSERT(hr);
	WINPR_ASSERT(what);

	*hr = E_FAIL;
	*what = "the probe thread";
	if (device->probed)
		return TRUE;

	CamMfProbe* probe = (CamMfProbe*)calloc(1, sizeof(CamMfProbe));
	if (!probe)
		return FALSE;
	if (!cam_mf_target_set(&probe->target, &device->target.api, device->target.file,
	                       device->target.link, device->target.name))
	{
		free(probe);
		return FALSE;
	}

	CamMfMtaCall* call = cam_mf_call_in_mta(cam_mf_probe_in_mta, probe, cam_mf_probe_free);
	if (!call)
		return FALSE;

	BOOL rc = call->result;
	*hr = probe->hr;
	*what = probe->what ? probe->what : "the probe";
	if (rc)
	{
		free(device->types);
		device->types = probe->types;
		device->nTypes = probe->nTypes;
		probe->types = nullptr;
		device->probed = TRUE;

		for (size_t x = 0; x < device->nTypes; x++)
		{
			const CamMfNativeType* native = &device->types[x];
			WLog_DBG(TAG,
			         "%s native type %" PRIuz ": %s %" PRIu32 "x%" PRIu32 " %" PRIu32 "/%" PRIu32,
			         device->target.name, x, cam_mf_format_name(native->format), native->width,
			         native->height, native->frameRateNumerator, native->frameRateDenominator);
		}
	}
	cam_mf_call_release(call);
	return rc;
}

WINPR_ATTR_NODISCARD
static BOOL cam_mf_device_probe(CamMfDevice* device)
{
	HRESULT hr = S_OK;
	const char* what = nullptr;
	if (cam_mf_device_probe_quiet(device, &hr, &what))
		return TRUE;
	WLog_ERR(TAG, "Unable to read the media types of %s: %s failed, 0x%08" PRIX32,
	         device->target.name, what, (UINT32)hr);
	return FALSE;
}

/** @return TRUE when a frame of that size may be announced in that format */
static BOOL cam_mf_size_allowed(CAM_MEDIA_FORMAT format, UINT32 width, UINT32 height)
{
	if ((width < 2) || (height < 2) || (width > CAM_MF_MAX_WIDTH) || (height > CAM_MF_MAX_HEIGHT))
		return FALSE;

	/* the chroma of NV12 and I420 is shared by two lines and two columns */
	if ((format == CAM_MEDIA_FORMAT_NV12) || (format == CAM_MEDIA_FORMAT_I420))
		return ((width % 2) == 0) && ((height % 2) == 0);
	if (format == CAM_MEDIA_FORMAT_YUY2)
		return (width % 2) == 0;
	return TRUE;
}

/** adds a frame size to the announced media types, or raises the frame rate of its entry */
static void cam_mf_add_media_type(CAM_MEDIA_FORMAT format, UINT32 width, UINT32 height,
                                  const CamMfNativeType* native,
                                  CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes, size_t* count,
                                  size_t maxMediaTypes)
{
	WINPR_ASSERT(native);
	WINPR_ASSERT(mediaTypes);
	WINPR_ASSERT(count);

	if (!cam_mf_size_allowed(format, width, height))
		return;

	for (size_t x = 0; x < *count; x++)
	{
		CAM_MEDIA_TYPE_DESCRIPTION* cur = &mediaTypes[x];
		if ((cur->Width != width) || (cur->Height != height))
			continue;

		if (cam_mf_rate_greater(native->frameRateNumerator, native->frameRateDenominator,
		                        cur->FrameRateNumerator, cur->FrameRateDenominator))
		{
			cur->FrameRateNumerator = native->frameRateNumerator;
			cur->FrameRateDenominator = native->frameRateDenominator;
		}
		return;
	}

	if (*count >= maxMediaTypes)
		return;

	mediaTypes[(*count)++] = (CAM_MEDIA_TYPE_DESCRIPTION){
		.Format = format,
		.Width = width,
		.Height = height,
		.FrameRateNumerator = native->frameRateNumerator,
		.FrameRateDenominator = native->frameRateDenominator,
		.PixelAspectRatioNumerator = 1,
		.PixelAspectRatioDenominator = 1,
	};
}

/** @return TRUE when frames in that format come out of the source reader without conversion */
static BOOL cam_mf_has_native_format(const CamMfDevice* device, CAM_MEDIA_FORMAT format)
{
	WINPR_ASSERT(device);

	/* a file is always decoded */
	if (device->target.file)
		return FALSE;

	for (size_t x = 0; x < device->nTypes; x++)
	{
		const CamMfNativeType* native = &device->types[x];
		if ((native->format == format) &&
		    cam_mf_size_allowed(format, native->width, native->height))
			return TRUE;
	}
	return FALSE;
}

/** the sizes of NV12 the source reader makes from a native type: its own when it may be
 * announced, else scaled down to fit 1920x1080 and 1280x720 */
static size_t cam_mf_converted_sizes(const CamMfNativeType* native, BOOL file, UINT32 widths[2],
                                     UINT32 heights[2])
{
	WINPR_ASSERT(native);

	UINT32 width = native->width;
	UINT32 height = native->height;
	if (file)
	{
		/* the reader scales an odd size to the even one below */
		width &= ~1u;
		height &= ~1u;
	}

	if (cam_mf_size_allowed(CAM_MEDIA_FORMAT_NV12, width, height))
	{
		widths[0] = width;
		heights[0] = height;
		return 1;
	}

	cam_mf_fit(width, height, CAM_MF_MAX_WIDTH, CAM_MF_MAX_HEIGHT, &widths[0], &heights[0]);
	cam_mf_fit(width, height, CAM_MF_SMALL_WIDTH, CAM_MF_SMALL_HEIGHT, &widths[1], &heights[1]);
	return 2;
}

/** fills the media types of a format, @return their number */
static size_t cam_mf_media_types_of_format(const CamMfDevice* device, CAM_MEDIA_FORMAT format,
                                           CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes,
                                           size_t maxMediaTypes)
{
	WINPR_ASSERT(device);

	size_t count = 0;
	if (cam_mf_has_native_format(device, format))
	{
		for (size_t x = 0; x < device->nTypes; x++)
		{
			const CamMfNativeType* native = &device->types[x];
			if (native->format == format)
				cam_mf_add_media_type(format, native->width, native->height, native, mediaTypes,
				                      &count, maxMediaTypes);
		}
		return count;
	}

	/* NV12 the source reader makes, in each native size: the last format of the list, the one
	 * of a file and of a camera without a format of the channel */
	if (format != CAM_MEDIA_FORMAT_NV12)
		return 0;

	for (size_t x = 0; x < device->nTypes; x++)
	{
		UINT32 widths[2] = WINPR_C_ARRAY_INIT;
		UINT32 heights[2] = WINPR_C_ARRAY_INIT;
		const size_t n =
		    cam_mf_converted_sizes(&device->types[x], device->target.file, widths, heights);
		for (size_t y = 0; y < n; y++)
			cam_mf_add_media_type(format, widths[y], heights[y], &device->types[x], mediaTypes,
			                      &count, maxMediaTypes);
	}
	return count;
}

static CamMfDevice* cam_mf_get_device(CamMfHal* hal, const char* deviceId)
{
	WINPR_ASSERT(hal);
	WINPR_ASSERT(deviceId);

	CamMfDevice* device = (CamMfDevice*)HashTable_GetItemValue(hal->devices, deviceId);
	if (!device)
		WLog_ERR(TAG, "Unknown camera %s", deviceId);
	return device;
}

/**
 * Function description
 *
 * @return -1 if error, otherwise index of supportedFormats array and mediaTypes/nMediaTypes filled
 * in
 */
static INT16 cam_mf_get_media_type_descriptions(ICamHal* ihal, const char* deviceId,
                                                WINPR_ATTR_UNUSED size_t streamIndex,
                                                const CAM_MEDIA_FORMAT_INFO* supportedFormats,
                                                size_t nSupportedFormats,
                                                CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes,
                                                size_t* nMediaTypes)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_ASSERT(supportedFormats);
	WINPR_ASSERT(mediaTypes);
	WINPR_ASSERT(nMediaTypes);

	const size_t maxMediaTypes = *nMediaTypes;
	*nMediaTypes = 0;

	if (!hal->available)
		return -1;

	CamMfDevice* device = cam_mf_get_device(hal, deviceId);
	if (!device || !cam_mf_device_probe(device))
		return -1;

	/* the first format of the list (the channel's preference) the camera can give */
	for (size_t formatIndex = 0; (formatIndex < nSupportedFormats) && (formatIndex <= INT16_MAX);
	     formatIndex++)
	{
		const CAM_MEDIA_FORMAT format = supportedFormats[formatIndex].inputFormat;
		const size_t count =
		    cam_mf_media_types_of_format(device, format, mediaTypes, maxMediaTypes);
		if (count == 0)
			continue;

		for (size_t x = 0; x < count; x++)
			WLog_DBG(TAG, "%s: %s %" PRIu32 "x%" PRIu32 " %" PRIu32 "/%" PRIu32 " fps%s",
			         device->target.name, cam_mf_format_name(format), mediaTypes[x].Width,
			         mediaTypes[x].Height, mediaTypes[x].FrameRateNumerator,
			         mediaTypes[x].FrameRateDenominator,
			         cam_mf_has_native_format(device, format) ? "" : " (converted)");

		*nMediaTypes = count;
		return (INT16)formatIndex;
	}

	WLog_ERR(TAG, "%s has no media type in a supported format", device->target.name);
	return -1;
}

/** @return how much the capture of NV12 from that subtype costs, the lowest is preferred */
static int cam_mf_conversion_cost(const GUID* subtype)
{
	WINPR_ASSERT(subtype);

	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_NV12))
		return 0;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_YUY2))
		return 1;
	if (IsEqualGUID(subtype, &cam_mf_MFVideoFormat_MJPG))
		return 3;
	return 2;
}

/** @return 0 when native gives the media type started, else 1 for its own size, 2 scaled */
static int cam_mf_native_match(const CamMfNativeType* native,
                               const CAM_MEDIA_TYPE_DESCRIPTION* mediaType, BOOL convert)
{
	if (!convert)
		return ((native->format == mediaType->Format) && (native->width == mediaType->Width) &&
		        (native->height == mediaType->Height))
		           ? 1
		           : 0;

	UINT32 widths[2] = WINPR_C_ARRAY_INIT;
	UINT32 heights[2] = WINPR_C_ARRAY_INIT;
	const size_t n = cam_mf_converted_sizes(native, FALSE, widths, heights);
	for (size_t y = 0; y < n; y++)
	{
		if ((widths[y] == mediaType->Width) && (heights[y] == mediaType->Height))
			return (n == 1) ? 1 : 2;
	}
	return 0;
}

/** selects the native media type of a camera that gives the media type started */
WINPR_ATTR_NODISCARD
static HRESULT cam_mf_set_native_type(CamMfCapture* capture, IMFSourceReader* reader)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(reader);

	const CAM_MEDIA_TYPE_DESCRIPTION* mediaType = &capture->mediaType;
	const BOOL convert = capture->convert;
	IMFMediaType* best = nullptr;
	CamMfNativeType bestNative = WINPR_C_ARRAY_INIT;
	int bestMatch = 0;
	BOOL bestExactRate = FALSE;

	for (DWORD index = 0;; index++)
	{
		IMFMediaType* type = nullptr;
		const HRESULT hr = IMFSourceReader_GetNativeMediaType(
		    reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type);
		if (hr == MF_E_NO_MORE_TYPES)
			break;
		if (FAILED(hr))
		{
			cam_mf_release_media_type(&best);
			return hr;
		}

		CamMfNativeType native = WINPR_C_ARRAY_INIT;
		const int match =
		    cam_mf_read_type(type, &native) ? cam_mf_native_match(&native, mediaType, convert) : 0;
		if (match == 0)
		{
			cam_mf_release_media_type(&type);
			continue;
		}

		/* its own size before a scaled one, the rate announced, else the highest; for NV12 to
		 * make, the cheapest subtype */
		const BOOL exactRate =
		    (1ull * native.frameRateNumerator * mediaType->FrameRateDenominator) ==
		    (1ull * mediaType->FrameRateNumerator * native.frameRateDenominator);
		const int cost = cam_mf_conversion_cost(&native.subtype);
		const int bestCost = best ? cam_mf_conversion_cost(&bestNative.subtype) : 0;
		BOOL better = FALSE;
		if (!best)
			better = TRUE;
		else if (match != bestMatch)
			better = match < bestMatch;
		else if (exactRate != bestExactRate)
			better = exactRate;
		else if (convert && (cost != bestCost))
			better = cost < bestCost;
		else
			better =
			    cam_mf_rate_greater(native.frameRateNumerator, native.frameRateDenominator,
			                        bestNative.frameRateNumerator, bestNative.frameRateDenominator);

		if (better)
		{
			cam_mf_release_media_type(&best);
			best = type;
			bestNative = native;
			bestMatch = match;
			bestExactRate = exactRate;
		}
		else
			cam_mf_release_media_type(&type);
	}

	if (!best)
	{
		WLog_ERR(TAG, "%s has no native type for %s %" PRIu32 "x%" PRIu32, capture->target.name,
		         cam_mf_format_name(mediaType->Format), mediaType->Width, mediaType->Height);
		return MF_E_INVALIDMEDIATYPE;
	}

	const HRESULT hr = IMFSourceReader_SetCurrentMediaType(
	    reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best);
	cam_mf_release_media_type(&best);
	if (FAILED(hr))
		WLog_ERR(TAG, "SetCurrentMediaType(%s native) failure: 0x%08" PRIX32, capture->target.name,
		         (UINT32)hr);
	else
		WLog_DBG(TAG, "%s native type: %" PRIu32 "x%" PRIu32 " %" PRIu32 "/%" PRIu32 " fps",
		         capture->target.name, bestNative.width, bestNative.height,
		         bestNative.frameRateNumerator, bestNative.frameRateDenominator);
	return hr;
}

/** asks the source reader for NV12 frames of the size started, which it then makes */
WINPR_ATTR_NODISCARD
static HRESULT cam_mf_set_nv12_output(CamMfCapture* capture, IMFSourceReader* reader)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(reader);

	const CAM_MEDIA_TYPE_DESCRIPTION* mediaType = &capture->mediaType;
	IMFMediaType* type = nullptr;
	HRESULT hr = capture->target.api.MFCreateMediaType(&type);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(type, &cam_mf_MF_MT_MAJOR_TYPE, &cam_mf_MFMediaType_Video);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(type, &cam_mf_MF_MT_SUBTYPE, &cam_mf_MFVideoFormat_NV12);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT64(type, &cam_mf_MF_MT_FRAME_SIZE,
		                            ((UINT64)mediaType->Width << 32) | mediaType->Height);
	if (SUCCEEDED(hr))
		hr = IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		                                         nullptr, type);
	cam_mf_release_media_type(&type);
	if (FAILED(hr))
	{
		WLog_ERR(TAG, "Unable to get NV12 %" PRIu32 "x%" PRIu32 " from %s: 0x%08" PRIX32,
		         mediaType->Width, mediaType->Height, capture->target.name, (UINT32)hr);
		return hr;
	}

	/* the frames must be the size announced */
	hr = IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
	                                         &type);
	if (FAILED(hr))
		return hr;

	CamMfNativeType current = WINPR_C_ARRAY_INIT;
	const BOOL valid = cam_mf_read_type(type, &current);
	cam_mf_release_media_type(&type);
	if (!valid || (current.format != CAM_MEDIA_FORMAT_NV12) ||
	    (current.width != mediaType->Width) || (current.height != mediaType->Height))
	{
		WLog_ERR(TAG, "%s gives %s %" PRIu32 "x%" PRIu32 " instead of NV12 %" PRIu32 "x%" PRIu32,
		         capture->target.name, cam_mf_format_name(current.format), current.width,
		         current.height, mediaType->Width, mediaType->Height);
		return MF_E_INVALIDMEDIATYPE;
	}
	return S_OK;
}

WINPR_ATTR_NODISCARD
static BOOL cam_mf_capture_running(CamMfCapture* capture)
{
	WINPR_ASSERT(capture);
	return InterlockedCompareExchange(&capture->running, 0, 0) != 0;
}

/** hands a sample to the channel, unless the stream gave this capture up */
WINPR_ATTR_NODISCARD
static UINT cam_mf_callback(CamMfCapture* capture, const BYTE* data, size_t size)
{
	WINPR_ASSERT(capture);

	UINT error = CHANNEL_RC_OK;
	EnterCriticalSection(&capture->callbackLock);
	if (!capture->detached)
		error = capture->sampleCallback(capture->dev, capture->streamIndex, data, size);
	LeaveCriticalSection(&capture->callbackLock);
	return error;
}

/**
 * hands a raw sample in a 2D buffer to the channel without a copy when its lines are not
 * padded, and YUY2 (one plane) packed when they are
 *
 * @return TRUE when the sample was handled, FALSE to read it through a contiguous copy
 */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_deliver_2d(CamMfCapture* capture, IMF2DBuffer2* buffer2d, UINT* error)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(buffer2d);
	WINPR_ASSERT(error);

	const CAM_MEDIA_TYPE_DESCRIPTION* mt = &capture->mediaType;
	const UINT64 frameSize = cam_mf_raw_frame_size(mt->Format, mt->Width, mt->Height);
	const UINT64 line = (mt->Format == CAM_MEDIA_FORMAT_YUY2) ? 2ull * mt->Width : mt->Width;

	BYTE* scanline0 = nullptr;
	LONG pitch = 0;
	BYTE* bufferStart = nullptr;
	DWORD bufferLength = 0;
	if (FAILED(IMF2DBuffer2_Lock2DSize(buffer2d, MF2DBuffer_LockFlags_Read, &scanline0, &pitch,
	                                   &bufferStart, &bufferLength)))
		return FALSE;

	BOOL handled = FALSE;
	const BYTE* bufferEnd = bufferStart + bufferLength;
	/* the first line inside the buffer, lines top-down (a negative pitch is bottom-up) */
	if (!scanline0 || (scanline0 < bufferStart) || (scanline0 > bufferEnd) || (pitch <= 0))
		goto out;

	const UINT64 available = (UINT64)(bufferEnd - scanline0);
	if ((UINT64)pitch == line)
	{
		if ((available < frameSize) || (frameSize > SIZE_MAX))
			goto out;
		*error = cam_mf_callback(capture, scanline0, (size_t)frameSize);
		handled = TRUE;
		goto out;
	}

	/* padded lines: YUY2 is one plane, packed here; the planes of NV12 and I420 are read
	 * through a contiguous copy, which knows where the chroma plane starts */
	if ((mt->Format != CAM_MEDIA_FORMAT_YUY2) || ((UINT64)pitch < line) || (mt->Height == 0) ||
	    (available < (UINT64)pitch * (mt->Height - 1) + line) || (frameSize > SIZE_MAX))
		goto out;

	if (capture->packedSize < frameSize)
	{
		BYTE* packed = (BYTE*)realloc(capture->packed, (size_t)frameSize);
		if (!packed)
			goto out;
		capture->packed = packed;
		capture->packedSize = (size_t)frameSize;
	}

	for (UINT32 y = 0; y < mt->Height; y++)
		memcpy(capture->packed + (size_t)(line * y), scanline0 + (size_t)((UINT64)pitch * y),
		       (size_t)line);
	*error = cam_mf_callback(capture, capture->packed, (size_t)frameSize);
	handled = TRUE;

out:
	(void)IMF2DBuffer2_Unlock2D(buffer2d);
	return handled;
}

/** hands a sample to the channel, @return the result of the sample callback */
WINPR_ATTR_NODISCARD
static UINT cam_mf_deliver(CamMfCapture* capture, IMFSample* sample)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(sample);

	const CAM_MEDIA_TYPE_DESCRIPTION* mt = &capture->mediaType;
	const UINT64 frameSize = cam_mf_raw_frame_size(mt->Format, mt->Width, mt->Height);
	IMFMediaBuffer* buffer = nullptr;
	UINT error = CHANNEL_RC_OK;

	/* a raw frame in one 2D buffer is read in place when its lines are not padded */
	DWORD nBuffers = 0;
	if ((frameSize > 0) && SUCCEEDED(IMFSample_GetBufferCount(sample, &nBuffers)) &&
	    (nBuffers == 1) && SUCCEEDED(IMFSample_GetBufferByIndex(sample, 0, &buffer)))
	{
		IMF2DBuffer2* buffer2d = nullptr;
		BOOL handled = FALSE;
		if (SUCCEEDED(
		        IMFMediaBuffer_QueryInterface(buffer, &cam_mf_IID_IMF2DBuffer2, (void**)&buffer2d)))
		{
			handled = cam_mf_deliver_2d(capture, buffer2d, &error);
			IMF2DBuffer2_Release(buffer2d);
		}
		IMFMediaBuffer_Release(buffer);
		buffer = nullptr;
		if (handled)
			return error;
	}

	/* otherwise the bytes of the sample, of the minimum pitch: Lock of a 2D buffer gives a
	 * contiguous copy of its planes */
	HRESULT hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
	if (FAILED(hr))
	{
		WLog_DBG(TAG, "ConvertToContiguousBuffer failure: 0x%08" PRIX32, (UINT32)hr);
		return CHANNEL_RC_OK;
	}

	BYTE* data = nullptr;
	DWORD length = 0;
	hr = IMFMediaBuffer_Lock(buffer, &data, nullptr, &length);
	if (FAILED(hr))
	{
		WLog_DBG(TAG, "Lock failure: 0x%08" PRIX32, (UINT32)hr);
		IMFMediaBuffer_Release(buffer);
		return CHANNEL_RC_OK;
	}

	if (frameSize > 0)
	{
		if (((UINT64)length < frameSize) || (frameSize > SIZE_MAX))
			WLog_DBG(TAG, "Frame dropped: %" PRIu32 " bytes for a %" PRIu64 " byte frame",
			         (UINT32)length, frameSize);
		else
			error = cam_mf_callback(capture, data, (size_t)frameSize);
	}
	else if (length > 0)
		error = cam_mf_callback(capture, data, length);

	(void)IMFMediaBuffer_Unlock(buffer);
	IMFMediaBuffer_Release(buffer);
	return error;
}

/** sleeps until the clock reaches due (ns), giving up early when the capture is stopped */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_wait_until(CamMfCapture* capture, UINT64 due)
{
	WINPR_ASSERT(capture);

	for (;;)
	{
		if (!cam_mf_capture_running(capture))
			return FALSE;

		const UINT64 now = winpr_GetTickCount64NS();
		if (now >= due)
			return TRUE;

		UINT64 ms = (due - now) / 1000000ull;
		if (ms > CAM_MF_MAX_SLEEP_MS)
			ms = CAM_MF_MAX_SLEEP_MS;
		Sleep((DWORD)ms);
	}
}

/** plays the file from its start again */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_rewind(CamMfCapture* capture, IMFSourceReader* reader)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(reader);

	PROPVARIANT position;
	PropVariantInit(&position);
	position.vt = VT_I8;
	position.hVal.QuadPart = 0;

	const HRESULT hr = IMFSourceReader_SetCurrentPosition(reader, &cam_mf_GUID_NULL, &position);
	if (FAILED(hr))
	{
		WLog_WARN(TAG, "Unable to rewind %s: 0x%08" PRIX32, capture->target.name, (UINT32)hr);
		return FALSE;
	}
	WLog_DBG(TAG, "End of %s, playing it again", capture->target.name);
	return TRUE;
}

typedef enum
{
	CAM_MF_CAPTURE_STOPPED, /* asked to stop */
	CAM_MF_CAPTURE_DRAINED, /* the channel released the device for lack of requests */
	CAM_MF_CAPTURE_FAILED   /* an error while running: the reader is opened again */
} CamMfCaptureEnd;

WINPR_ATTR_NODISCARD
static CamMfCaptureEnd cam_mf_capture_loop(CamMfCapture* capture, IMFSourceReader* reader)
{
	WINPR_ASSERT(capture);
	WINPR_ASSERT(reader);

	BOOL clock = FALSE;
	UINT64 clockStart = 0;
	LONGLONG timestampStart = 0;
	size_t framesSinceRewind = 0;

	while (cam_mf_capture_running(capture))
	{
		DWORD streamFlags = 0;
		LONGLONG timestamp = 0;
		IMFSample* sample = nullptr;
		HRESULT hr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		                                        0, nullptr, &streamFlags, &timestamp, &sample);
#if defined(CAM_MF_TESTING)
		if ((InterlockedCompareExchange(&cam_mf_test_fail_read_after, 0, 0) > 0) &&
		    (InterlockedDecrement(&cam_mf_test_fail_read_after) == 0))
			hr = E_FAIL;
#endif
		if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ERROR))
		{
			if (sample)
				IMFSample_Release(sample);
			/* MF_E_SHUTDOWN: a stop that waited too long shut the source down */
			if (!cam_mf_capture_running(capture))
				return CAM_MF_CAPTURE_STOPPED;
			WLog_WARN(TAG, "ReadSample(%s) failure: 0x%08" PRIX32 ", flags 0x%08" PRIX32,
			          capture->target.name, (UINT32)hr, (UINT32)streamFlags);
			return CAM_MF_CAPTURE_FAILED;
		}

		if (streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
			WLog_WARN(TAG, "The media type of %s changed", capture->target.name);

		if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
		{
			if (sample)
				IMFSample_Release(sample);
			if (!capture->target.file)
			{
				WLog_WARN(TAG, "Camera %s ended its stream", capture->target.name);
				return CAM_MF_CAPTURE_FAILED;
			}
			if (framesSinceRewind == 0)
			{
				WLog_WARN(TAG, "%s did not produce any frame", capture->target.name);
				return CAM_MF_CAPTURE_FAILED;
			}
			if (!cam_mf_rewind(capture, reader))
				return CAM_MF_CAPTURE_FAILED;
			framesSinceRewind = 0;
			clock = FALSE;
			continue;
		}

		/* MF_SOURCE_READERF_STREAMTICK: a gap in the stream, no sample */
		if (!sample)
			continue;
		framesSinceRewind++;

		/* a file is played at the pace of its time stamps, in units of 100 ns */
		if (capture->target.file)
		{
			const UINT64 now = winpr_GetTickCount64NS();
			if (!clock || (timestamp < timestampStart))
			{
				clock = TRUE;
				clockStart = now;
				timestampStart = timestamp;
			}

			UINT64 due = clockStart + (UINT64)(timestamp - timestampStart) * 100ull;
			if (now > due + CAM_MF_MAX_LATE_NS)
			{
				WLog_DBG(TAG, "Playback late by %" PRIu64 " ms, resetting the clock",
				         (UINT64)((now - due) / 1000000ull));
				clockStart = due = now;
				timestampStart = timestamp;
			}

			if (!cam_mf_wait_until(capture, due))
			{
				IMFSample_Release(sample);
				return CAM_MF_CAPTURE_STOPPED;
			}
		}

		const UINT error = cam_mf_deliver(capture, sample);
		IMFSample_Release(sample);

		if (error == ECAM_SAMPLE_CAPTURE_DRAINED)
		{
			WLog_DBG(TAG, "No more sample requests, releasing %s", capture->target.name);
			return CAM_MF_CAPTURE_DRAINED;
		}
		else if (error != CHANNEL_RC_OK)
			WLog_ERR(TAG, "Failure in sampleCallback: %" PRIu32, error);
	}
	return CAM_MF_CAPTURE_STOPPED;
}

/** opens the reader and sets the media types of the capture */
WINPR_ATTR_NODISCARD
static CAM_ERROR_CODE cam_mf_capture_open(CamMfCapture* capture, IMFMediaSource** source,
                                          IMFSourceReader** reader)
{
	WINPR_ASSERT(capture);

	const char* what = nullptr;
	const HRESULT hr =
	    cam_mf_open_reader(&capture->target, capture->convert, source, reader, &what);
	if (FAILED(hr))
	{
		WLog_WARN(TAG, "Unable to open %s: %s failed, 0x%08" PRIX32, capture->target.name, what,
		          (UINT32)hr);
		return CAM_ERROR_CODE_ItemNotFound;
	}

	if (!capture->target.file && FAILED(cam_mf_set_native_type(capture, *reader)))
		return CAM_ERROR_CODE_InvalidMediaType;

	if (capture->convert && FAILED(cam_mf_set_nv12_output(capture, *reader)))
		return CAM_ERROR_CODE_InvalidMediaType;

	return CAM_ERROR_CODE_None;
}

/** the source a stop may shut down, or none */
static void cam_mf_capture_set_source(CamMfCapture* capture, IMFMediaSource* source)
{
	WINPR_ASSERT(capture);

	if (source)
		IMFMediaSource_AddRef(source);
	EnterCriticalSection(&capture->lock);
	IMFMediaSource* old = capture->source;
	capture->source = source;
	LeaveCriticalSection(&capture->lock);
	if (old)
		IMFMediaSource_Release(old);
}

static void cam_mf_capture_release(CamMfCapture* capture)
{
	if (!capture || (InterlockedDecrement(&capture->refs) != 0))
		return;

	cam_mf_capture_set_source(capture, nullptr);
	if (capture->ready)
		(void)CloseHandle(capture->ready);
	DeleteCriticalSection(&capture->lock);
	DeleteCriticalSection(&capture->callbackLock);
	cam_mf_target_free(&capture->target);
	free(capture->packed);
	free(capture);
}

/** sleeps ms while the capture runs, @return FALSE when it was stopped */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_sleep_running(CamMfCapture* capture, DWORD ms)
{
	return cam_mf_wait_until(capture, winpr_GetTickCount64NS() + 1000000ull * ms);
}

static DWORD WINAPI cam_mf_capture_thread(LPVOID param)
{
	CamMfCapture* capture = (CamMfCapture*)param;
	WINPR_ASSERT(capture);

	IMFMediaSource* source = nullptr;
	IMFSourceReader* reader = nullptr;

	CAM_ERROR_CODE error = CAM_ERROR_CODE_UnexpectedError;
	const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hrCom))
		WLog_ERR(TAG, "CoInitializeEx failure: 0x%08" PRIX32, (UINT32)hrCom);
	else
		error = cam_mf_capture_open(capture, &source, &reader);

	EnterCriticalSection(&capture->lock);
	capture->startError = error;
	LeaveCriticalSection(&capture->lock);
	if (error == CAM_ERROR_CODE_None)
		cam_mf_capture_set_source(capture, source);
	(void)SetEvent(capture->ready);

	while (error == CAM_ERROR_CODE_None)
	{
		const CamMfCaptureEnd end = cam_mf_capture_loop(capture, reader);
		cam_mf_capture_set_source(capture, nullptr);
		cam_mf_close_reader(&capture->target, &source, &reader);
		if ((end != CAM_MF_CAPTURE_FAILED) || !cam_mf_capture_running(capture))
			break;

		/* the requests of the server wait for samples: open the camera again until it comes
		 * back or the stream is stopped */
		DWORD delay = CAM_MF_REOPEN_FIRST_DELAY_MS;
		BOOL reopened = FALSE;
		for (unsigned attempt = 1; !reopened && cam_mf_sleep_running(capture, delay); attempt++)
		{
			if (cam_mf_capture_open(capture, &source, &reader) == CAM_ERROR_CODE_None)
			{
				WLog_INFO(TAG, "%s opened again (attempt %u)", capture->target.name, attempt);
				cam_mf_capture_set_source(capture, source);
				reopened = TRUE;
			}
			else
			{
				cam_mf_close_reader(&capture->target, &source, &reader);
				delay = (delay * 2 > CAM_MF_REOPEN_MAX_DELAY_MS) ? CAM_MF_REOPEN_MAX_DELAY_MS
				                                                 : delay * 2;
			}
		}
		if (!reopened)
			break;
	}

	cam_mf_close_reader(&capture->target, &source, &reader);
	if (SUCCEEDED(hrCom))
		CoUninitialize();
	cam_mf_capture_release(capture);
	return CHANNEL_RC_OK;
}

/** ends the capture of the stream, @c stream->control held */
static void cam_mf_stream_stop_locked(CamMfDevice* device)
{
	WINPR_ASSERT(device);

	CamMfStream* stream = &device->stream;
	CamMfCapture* capture = stream->capture;
	HANDLE thread = stream->thread;
	stream->capture = nullptr;
	stream->thread = nullptr;
	if (!capture)
		return;

	(void)InterlockedExchange(&capture->running, 0);
	if (thread && (WaitForSingleObject(thread, CAM_MF_STOP_TIMEOUT_MS) == WAIT_TIMEOUT))
	{
		/* a camera that sends nothing keeps ReadSample waiting: shutting its source down ends
		 * it with MF_E_SHUTDOWN */
		EnterCriticalSection(&capture->lock);
		IMFMediaSource* source = capture->source;
		if (source)
			IMFMediaSource_AddRef(source);
		LeaveCriticalSection(&capture->lock);

		WLog_WARN(TAG, "%s does not stop, shutting it down", device->target.name);
		if (source)
		{
			(void)IMFMediaSource_Shutdown(source);
			IMFMediaSource_Release(source);
		}

		if (WaitForSingleObject(thread, CAM_MF_SHUTDOWN_TIMEOUT_MS) == WAIT_TIMEOUT)
		{
			/* it ends alone, with its own copy of the target: no callback from now on */
			EnterCriticalSection(&capture->callbackLock);
			capture->detached = TRUE;
			LeaveCriticalSection(&capture->callbackLock);
			(void)InterlockedIncrement(&cam_mf_abandoned_threads);
			WLog_ERR(TAG, "%s still does not stop, leaving its thread", device->target.name);
		}
	}

	if (thread)
		(void)CloseHandle(thread);
	cam_mf_capture_release(capture);
}

static CAM_ERROR_CODE cam_mf_stream_stop(CamMfDevice* device)
{
	WINPR_ASSERT(device);

	EnterCriticalSection(&device->stream.control);
	cam_mf_stream_stop_locked(device);
	LeaveCriticalSection(&device->stream.control);
	return CAM_ERROR_CODE_None;
}

WINPR_ATTR_NODISCARD
static CamMfCapture* cam_mf_capture_new(CamMfDevice* device, CameraDevice* dev, size_t streamIndex,
                                        const CAM_MEDIA_TYPE_DESCRIPTION* mediaType,
                                        ICamHalSampleCapturedCallback callback)
{
	CamMfCapture* capture = (CamMfCapture*)calloc(1, sizeof(CamMfCapture));
	if (!capture)
		return nullptr;

	if (!InitializeCriticalSectionEx(&capture->lock, 0, 0))
	{
		free(capture);
		return nullptr;
	}
	if (!InitializeCriticalSectionEx(&capture->callbackLock, 0, 0))
	{
		DeleteCriticalSection(&capture->lock);
		free(capture);
		return nullptr;
	}

	capture->refs = 1;
	capture->running = 1;
	capture->startError = CAM_ERROR_CODE_UnexpectedError;
	capture->mediaType = *mediaType;
	capture->convert = !cam_mf_has_native_format(device, mediaType->Format);
	capture->dev = dev;
	capture->streamIndex = streamIndex;
	capture->sampleCallback = callback;
	capture->ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!capture->ready ||
	    !cam_mf_target_set(&capture->target, &device->target.api, device->target.file,
	                       device->target.link, device->target.name))
	{
		cam_mf_capture_release(capture);
		return nullptr;
	}
	return capture;
}

static CAM_ERROR_CODE cam_mf_stream_start(ICamHal* ihal, CameraDevice* dev, size_t streamIndex,
                                          const CAM_MEDIA_TYPE_DESCRIPTION* mediaType,
                                          ICamHalSampleCapturedCallback callback)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_ASSERT(dev);
	WINPR_ASSERT(mediaType);

	if (!hal->available)
		return CAM_ERROR_CODE_NotInitialized;

	CamMfDevice* device = cam_mf_get_device(hal, dev->deviceId);
	if (!device)
		return CAM_ERROR_CODE_ItemNotFound;
	if (!cam_mf_device_probe(device))
		return CAM_ERROR_CODE_ItemNotFound;

	CamMfStream* stream = &device->stream;
	EnterCriticalSection(&stream->control);

	/* The channel starts a stream that is not running only. A capture released for lack of
	 * sample requests may still be ending its thread: the channel saw the end already
	 * (ECAM_SAMPLE_CAPTURE_DRAINED), so it is not refused, it is waited for. */
	cam_mf_stream_stop_locked(device);

	CAM_ERROR_CODE error = CAM_ERROR_CODE_OutOfMemory;
	CamMfCapture* capture = cam_mf_capture_new(device, dev, streamIndex, mediaType, callback);
	if (!capture)
		goto out;

	/* a reference for the thread */
	(void)InterlockedIncrement(&capture->refs);
	HANDLE thread = CreateThread(nullptr, 0, cam_mf_capture_thread, capture, 0, nullptr);
	if (!thread)
	{
		WLog_ERR(TAG, "CreateThread failure");
		(void)InterlockedDecrement(&capture->refs);
		cam_mf_capture_release(capture);
		goto out;
	}
	stream->capture = capture;
	stream->thread = thread;

	/* the thread opens the camera: its result is the result of the start */
	HANDLE handles[] = { capture->ready, thread };
	const DWORD status =
	    WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, CAM_MF_CALL_TIMEOUT_MS);

	EnterCriticalSection(&capture->lock);
	error = capture->startError;
	LeaveCriticalSection(&capture->lock);
	if (status == WAIT_TIMEOUT)
	{
		WLog_ERR(TAG, "%s did not open in %" PRIu32 " ms", device->target.name,
		         (UINT32)CAM_MF_CALL_TIMEOUT_MS);
		error = CAM_ERROR_CODE_UnexpectedError;
	}

	if (error != CAM_ERROR_CODE_None)
	{
		cam_mf_stream_stop_locked(device);
		goto out;
	}

	WLog_INFO(TAG, "Camera %s: %s %" PRIu32 "x%" PRIu32 ", fps: %" PRIu32 "/%" PRIu32 "%s",
	          device->target.name, cam_mf_format_name(mediaType->Format), mediaType->Width,
	          mediaType->Height, mediaType->FrameRateNumerator, mediaType->FrameRateDenominator,
	          capture->convert ? " (converted)" : "");

out:
	LeaveCriticalSection(&stream->control);
	return error;
}

static CAM_ERROR_CODE cam_mf_stream_stop_by_device_id(ICamHal* ihal, const char* deviceId,
                                                      WINPR_ATTR_UNUSED size_t streamIndex)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);

	CamMfDevice* device = (CamMfDevice*)HashTable_GetItemValue(hal->devices, deviceId);
	if (!device)
		return CAM_ERROR_CODE_NotInitialized;

	return cam_mf_stream_stop(device);
}

static BOOL cam_mf_activate(ICamHal* ihal, const char* deviceId, CAM_ERROR_CODE* errorCode)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_ASSERT(errorCode);

	if (!hal->available || !HashTable_GetItemValue(hal->devices, deviceId))
	{
		*errorCode = CAM_ERROR_CODE_ItemNotFound;
		return FALSE;
	}

	*errorCode = CAM_ERROR_CODE_None;
	return TRUE;
}

static BOOL cam_mf_deactivate(ICamHal* ihal, const char* deviceId, CAM_ERROR_CODE* errorCode)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);
	WINPR_ASSERT(errorCode);

	CamMfDevice* device = (CamMfDevice*)HashTable_GetItemValue(hal->devices, deviceId);
	if (!device)
	{
		*errorCode = CAM_ERROR_CODE_ItemNotFound;
		return FALSE;
	}

	*errorCode = cam_mf_stream_stop(device);
	return TRUE;
}

static void cam_mf_device_free(void* obj)
{
	CamMfDevice* device = (CamMfDevice*)obj;
	if (!device)
		return;

	(void)cam_mf_stream_stop(device);
	DeleteCriticalSection(&device->stream.control);

	free(device->types);
	cam_mf_target_free(&device->target);
	free(device);
}

WINPR_ATTR_MALLOC(cam_mf_device_free, 1)
static CamMfDevice* cam_mf_device_new(CamMfHal* hal, const char* deviceId, const char* name,
                                      const WCHAR* link, BOOL file)
{
	WINPR_ASSERT(hal);
	WINPR_ASSERT(deviceId);
	WINPR_ASSERT(name);
	WINPR_ASSERT(link);

	CamMfDevice* device = (CamMfDevice*)calloc(1, sizeof(CamMfDevice));
	if (!device)
		return nullptr;

	if (!InitializeCriticalSectionEx(&device->stream.control, 0, 0))
	{
		free(device);
		return nullptr;
	}

	device->hal = hal;
	(void)strncpy(device->deviceId, deviceId, sizeof(device->deviceId) - 1);
	if (!cam_mf_target_set(&device->target, &hal->api, file, link, name))
	{
		cam_mf_device_free(device);
		return nullptr;
	}
	return device;
}

/** a camera of the last enumeration */
typedef struct
{
	char deviceId[32];
	char* name;
	WCHAR* link;
} CamMfFound;

typedef struct
{
	CamMfApi api;
	CamMfFound* found;
	size_t nFound;
} CamMfEnumeration;

static void cam_mf_enumeration_free(void* data)
{
	CamMfEnumeration* enumeration = (CamMfEnumeration*)data;
	if (!enumeration)
		return;
	for (size_t x = 0; x < enumeration->nFound; x++)
	{
		free(enumeration->found[x].name);
		free(enumeration->found[x].link);
	}
	free(enumeration->found);
	free(enumeration);
}

/** @return the channel name of a camera, from its symbolic link (FNV-1a) */
static void cam_mf_device_id(const WCHAR* link, char* deviceId, size_t size)
{
	WINPR_ASSERT(link);

	UINT32 hash = 2166136261u;
	for (const WCHAR* cur = link; *cur; cur++)
	{
		hash ^= (UINT32)*cur;
		hash *= 16777619u;
	}
	(void)_snprintf(deviceId, size, CAM_MF_DEVICE_ID_PREFIX "%08" PRIX32, hash);
}

/** lists the video capture devices (MTA) */
static BOOL cam_mf_enumerate_in_mta(void* data)
{
	CamMfEnumeration* enumeration = (CamMfEnumeration*)data;
	WINPR_ASSERT(enumeration);

	const CamMfApi* api = &enumeration->api;
	IMFAttributes* attributes = nullptr;
	IMFActivate** activates = nullptr;
	UINT32 count = 0;
	BOOL rc = FALSE;

	HRESULT hr = api->MFCreateAttributes(&attributes, 1);
	if (SUCCEEDED(hr))
		hr = IMFAttributes_SetGUID(attributes, &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
		                           &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if (SUCCEEDED(hr))
		hr = api->MFEnumDeviceSources(attributes, &activates, &count);
	if (FAILED(hr))
	{
		WLog_ERR(TAG, "MFEnumDeviceSources failure: 0x%08" PRIX32, (UINT32)hr);
		goto out;
	}

	if (count > 0)
	{
		enumeration->found = (CamMfFound*)calloc(count, sizeof(CamMfFound));
		if (!enumeration->found)
			goto out;
	}

	for (UINT32 x = 0; x < count; x++)
	{
		WCHAR* link = nullptr;
		WCHAR* name = nullptr;
		UINT32 length = 0;

		if (FAILED(IMFActivate_GetAllocatedString(
		        activates[x], &cam_mf_MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
		        &link, &length)))
			continue;
		if (FAILED(IMFActivate_GetAllocatedString(
		        activates[x], &cam_mf_MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)))
			name = nullptr;

		CamMfFound* found = &enumeration->found[enumeration->nFound];
		found->link = _wcsdup(link);
		found->name = name ? ConvertWCharToUtf8Alloc(name, nullptr) : _strdup("Camera");
		CoTaskMemFree(link);
		CoTaskMemFree(name);
		if (!found->link || !found->name)
		{
			free(found->link);
			free(found->name);
			found->link = nullptr;
			found->name = nullptr;
			continue;
		}

		cam_mf_device_id(found->link, found->deviceId, sizeof(found->deviceId));
		enumeration->nFound++;
	}
	rc = TRUE;

out:
	for (UINT32 x = 0; x < count; x++)
		IMFActivate_Release(activates[x]);
	CoTaskMemFree((void*)activates);
	if (attributes)
		IMFAttributes_Release(attributes);
	return rc;
}

/**
 * Function description
 *
 * @return number of video capture devices
 */
static UINT cam_mf_enumerate(ICamHal* ihal, ICamHalEnumCallback callback, CameraPlugin* ecam,
                             GENERIC_CHANNEL_CALLBACK* hchannel)
{
	CamMfHal* hal = (CamMfHal*)ihal;
	WINPR_ASSERT(hal);

	if (!hal->available)
		return 0;

	if (hal->file)
	{
		/* the file only, when it opened */
		CamMfDevice* device =
		    (CamMfDevice*)HashTable_GetItemValue(hal->devices, CAM_MF_FILE_DEVICE_ID);
		if (!device)
			return 0;
		IFCALL(callback, ecam, hchannel, device->deviceId, device->target.name);
		return 1;
	}

	CamMfEnumeration* enumeration = (CamMfEnumeration*)calloc(1, sizeof(CamMfEnumeration));
	if (!enumeration)
		return 0;
	enumeration->api = hal->api;

	CamMfMtaCall* call =
	    cam_mf_call_in_mta(cam_mf_enumerate_in_mta, enumeration, cam_mf_enumeration_free);
	if (!call)
		return 0;

	UINT count = 0;
	for (size_t x = 0; call->result && (x < enumeration->nFound); x++)
	{
		CamMfFound* found = &enumeration->found[x];
		CamMfDevice* device = (CamMfDevice*)HashTable_GetItemValue(hal->devices, found->deviceId);
		if (!device)
		{
			device = cam_mf_device_new(hal, found->deviceId, found->name, found->link, FALSE);
			if (!device)
				continue;
			if (!HashTable_Insert(hal->devices, device->deviceId, device))
			{
				cam_mf_device_free(device);
				continue;
			}
		}

		WLog_DBG(TAG, "Camera %s: %s", device->deviceId, device->target.name);
		count++;
		IFCALL(callback, ecam, hchannel, device->deviceId, device->target.name);
	}

	cam_mf_call_release(call);
	return count;
}

static CAM_ERROR_CODE cam_mf_free(ICamHal* ihal)
{
	CamMfHal* hal = (CamMfHal*)ihal;

	if (!hal)
		return CAM_ERROR_CODE_NotInitialized;

	/* the devices stop their streams, before Media Foundation shuts down; not when a thread
	 * was left to end alone, it may still use it. The DLLs stay loaded. */
	HashTable_Free(hal->devices);
	if (hal->api.started && (InterlockedCompareExchange(&cam_mf_abandoned_threads, 0, 0) == 0))
		(void)hal->api.MFShutdown();

	free(hal->file);
	free(hal->name);
	free(hal);
	return CAM_ERROR_CODE_None;
}

static const char* cam_mf_basename(const char* path)
{
	const char* name = path;
	for (const char* cur = path; *cur; cur++)
	{
		if ((*cur == '/') || (*cur == '\\'))
			name = cur + 1;
	}
	return name;
}

WINPR_ATTR_NODISCARD
static BOOL cam_mf_parse_args(CamMfHal* hal, const ADDIN_ARGV* args)
{
	WINPR_ASSERT(hal);

	COMMAND_LINE_ARGUMENT_A mf_args[] = {
		{ "file", COMMAND_LINE_VALUE_REQUIRED, "<path or URL>", nullptr, nullptr, -1, nullptr,
		  "media file to play as the camera" },
		{ "name", COMMAND_LINE_VALUE_REQUIRED, "<name>", nullptr, nullptr, -1, nullptr,
		  "name of the camera of the file" },
		{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr }
	};

	if (!args || (args->argc <= 1))
		return TRUE;

	const DWORD flags =
	    COMMAND_LINE_SIGIL_NONE | COMMAND_LINE_SEPARATOR_COLON | COMMAND_LINE_IGN_UNKNOWN_KEYWORD;
	const int status =
	    CommandLineParseArgumentsA(args->argc, args->argv, mf_args, flags, hal, nullptr, nullptr);
	if (status != 0)
		return FALSE;

	const COMMAND_LINE_ARGUMENT_A* arg = mf_args;
	do
	{
		if (!(arg->Flags & COMMAND_LINE_VALUE_PRESENT))
			continue;

		CommandLineSwitchStart(arg) CommandLineSwitchCase(arg, "file")
		{
			free(hal->file);
			hal->file = _strdup(arg->Value);
			if (!hal->file)
				return FALSE;
		}
		CommandLineSwitchCase(arg, "name")
		{
			free(hal->name);
			hal->name = _strdup(arg->Value);
			if (!hal->name)
				return FALSE;
		}
		CommandLineSwitchDefault(arg)
		{
		}
		CommandLineSwitchEnd(arg)
	} while ((arg = CommandLineFindNextArgumentA(arg)) != nullptr);

	return TRUE;
}

/**
 * the file as the only camera, probed now; a file that does not open is not offered, with one
 * warning
 *
 * @return FALSE when out of memory only
 */
WINPR_ATTR_NODISCARD
static BOOL cam_mf_add_file(CamMfHal* hal)
{
	WINPR_ASSERT(hal);
	WINPR_ASSERT(hal->file);

	char* name = nullptr;
	if (hal->name)
		name = _strdup(hal->name);
	else
	{
		size_t len = 0;
		winpr_asprintf(&name, &len, "File: %s", cam_mf_basename(hal->file));
	}
	WCHAR* link = ConvertUtf8ToWCharAlloc(hal->file, nullptr);

	BOOL rc = FALSE;
	CamMfDevice* device = nullptr;
	if (!name || !link)
		goto out;

	device = cam_mf_device_new(hal, CAM_MF_FILE_DEVICE_ID, name, link, TRUE);
	if (!device)
		goto out;

	HRESULT hr = S_OK;
	const char* what = nullptr;
	if (!cam_mf_device_probe_quiet(device, &hr, &what))
	{
		WLog_WARN(TAG, "camera file %s can't be opened (%s failed, 0x%08" PRIX32 "); not offered",
		          hal->file, what, (UINT32)hr);
		rc = TRUE;
		goto out;
	}

	const CamMfNativeType* video = &device->types[0];
	WLog_INFO(TAG, "%s: %" PRIu32 "x%" PRIu32 " at %" PRIu32 "/%" PRIu32 " fps, played as NV12",
	          hal->file, video->width, video->height, video->frameRateNumerator,
	          video->frameRateDenominator);

	if (!HashTable_Insert(hal->devices, device->deviceId, device))
		goto out;
	device = nullptr;
	rc = TRUE;

out:
	cam_mf_device_free(device);
	free(link);
	free(name);
	return rc;
}

/**
 * Function description
 *
 * Without Media Foundation, or with a file that does not open, the backend still registers and
 * offers no camera: an error here would fail the dynamic channel plugin.
 *
 * @return 0 on success, otherwise a Win32 error code (out of memory only)
 */
FREERDP_ENTRY_POINT(UINT VCAPITYPE mf_freerdp_rdpecam_client_subsystem_entry(
    PFREERDP_CAMERA_HAL_ENTRY_POINTS pEntryPoints))
{
	UINT ret = CHANNEL_RC_NO_MEMORY;
	WINPR_ASSERT(pEntryPoints);

	CamMfHal* hal = (CamMfHal*)calloc(1, sizeof(CamMfHal));
	if (!hal)
		return CHANNEL_RC_NO_MEMORY;

	hal->iHal.Enumerate = cam_mf_enumerate;
	hal->iHal.RequestDrivenCapture = TRUE;
	hal->iHal.GetMediaTypeDescriptions = cam_mf_get_media_type_descriptions;
	hal->iHal.Activate = cam_mf_activate;
	hal->iHal.Deactivate = cam_mf_deactivate;
	hal->iHal.StartStream = cam_mf_stream_start;
	hal->iHal.StopStream = cam_mf_stream_stop_by_device_id;
	hal->iHal.Free = cam_mf_free;

	hal->devices = HashTable_New(TRUE);
	if (!hal->devices || !HashTable_SetupForStringData(hal->devices, FALSE))
		goto error;

	wObject* obj = HashTable_ValueObject(hal->devices);
	WINPR_ASSERT(obj);
	obj->fnObjectFree = cam_mf_device_free;

	if (!cam_mf_parse_args(hal, pEntryPoints->args))
		WLog_WARN(TAG, "camera: invalid arguments, ignored");

	HRESULT hr = S_OK;
	const char* why = cam_mf_api_load(&hal->api, &hr);
	if (why)
		WLog_WARN(TAG, "camera: Media Foundation not available (%s, 0x%08" PRIX32 "); no cameras",
		          why, (UINT32)hr);
	else
	{
		hal->available = TRUE;
		if (hal->file && !cam_mf_add_file(hal))
			goto error;
	}

	ret = pEntryPoints->pRegisterCameraHal(pEntryPoints->plugin, &hal->iHal);
	if (ret != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "RegisterCameraHal failed with error %" PRIu32 "", ret);
		goto error;
	}

	return ret;

error:
	cam_mf_free(&hal->iHal);
	return ret;
}
