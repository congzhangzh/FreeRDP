/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: finding a v4l2loopback device (against a fake)
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <linux/videodev2.h>

#include <winpr/wlog.h>
#include <winpr/wtypes.h>

#include "camera_sink.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

/* a fake /dev: no real device is opened */
typedef struct
{
	const char* path;
	const char* driver;
	const char* card;
	UINT32 caps;
	BOOL usageEvents; /* v4l2loopback 0.13+: client usage events */
} FakeDevice;

/* v4l2loopback.c of 0.13.0, lines 802-805 */
#define FAKE_EVENT_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)
#define FAKE_MAX_EVENTS 16

#define FAKE_FD_BASE 100
#define FAKE_MAX 8

static const FakeDevice* g_devices;
static size_t g_count;
static int g_open[FAKE_MAX];
static size_t g_opens;
static struct v4l2_format g_fmt;
static struct v4l2_streamparm g_parm;
static size_t g_writes;
static size_t g_written;
static BOOL g_shrink; /* S_FMT answers a smaller size, as a device already in use */
static size_t g_noDeviceLines;
static size_t g_cameraLines;
static size_t g_fallbackLines;
static char g_lastLine[512];
static UINT32 g_queue[FAKE_MAX_EVENTS]; /* pending client usage counts */
static size_t g_queued;
static UINT32 g_readersNow; /* what SEND_INITIAL reports */
static size_t g_subscribes;
static size_t g_dequeues;
static const char* g_moduleVersion;

static void fake_reset(const FakeDevice* devices, size_t count)
{
	g_devices = devices;
	g_count = count;
	memset(g_open, 0, sizeof(g_open));
	g_opens = 0;
	memset(&g_fmt, 0, sizeof(g_fmt));
	memset(&g_parm, 0, sizeof(g_parm));
	g_writes = 0;
	g_written = 0;
	g_shrink = FALSE;
	g_noDeviceLines = 0;
	g_cameraLines = 0;
	g_fallbackLines = 0;
	g_queued = 0;
	g_readersNow = 0;
	g_subscribes = 0;
	g_dequeues = 0;
	g_moduleVersion = nullptr;
}

/* a reader starts or stops: the module queues an event */
static void fake_usage(UINT32 count)
{
	g_readersNow = count;
	if (g_queued < FAKE_MAX_EVENTS)
		g_queue[g_queued++] = count;
}

static size_t fake_open_count(void)
{
	size_t n = 0;
	for (size_t i = 0; i < FAKE_MAX; i++)
		n += (size_t)g_open[i];
	return n;
}

static int fake_open(const char* path, int flags)
{
	if ((flags & O_ACCMODE) != O_RDWR)
		return -1;
	for (size_t i = 0; i < g_count; i++)
	{
		if (strcmp(g_devices[i].path, path) == 0)
		{
			g_open[i]++;
			g_opens++;
			return FAKE_FD_BASE + (int)i;
		}
	}
	errno = ENOENT;
	return -1;
}

static int fake_ioctl(int fd, unsigned long request, void* arg)
{
	const size_t i = (size_t)(fd - FAKE_FD_BASE);
	if ((fd < FAKE_FD_BASE) || (i >= g_count) || !g_open[i])
	{
		errno = EBADF;
		return -1;
	}
	switch (request)
	{
		case VIDIOC_QUERYCAP:
		{
			struct v4l2_capability* cap = arg;
			memset(cap, 0, sizeof(*cap));
			(void)strncpy((char*)cap->driver, g_devices[i].driver, sizeof(cap->driver));
			(void)strncpy((char*)cap->card, g_devices[i].card, sizeof(cap->card));
			cap->capabilities = g_devices[i].caps | V4L2_CAP_DEVICE_CAPS;
			cap->device_caps = g_devices[i].caps;
			return 0;
		}
		case VIDIOC_S_FMT:
			g_fmt = *(struct v4l2_format*)arg;
			if (g_shrink)
			{
				struct v4l2_format* fmt = arg;
				fmt->fmt.pix.width = 640;
				fmt->fmt.pix.height = 480;
			}
			return 0;
		case VIDIOC_S_PARM:
			g_parm = *(struct v4l2_streamparm*)arg;
			return 0;
		case VIDIOC_SUBSCRIBE_EVENT:
		{
			const struct v4l2_event_subscription* sub = arg;
			g_subscribes++;
			/* 0.12.7 subscribes V4L2_EVENT_CTRL only (v4l2loopback.c:1779-1788) */
			if (!g_devices[i].usageEvents || (sub->type != FAKE_EVENT_CLIENT_USAGE))
			{
				errno = EINVAL;
				return -1;
			}
			if (sub->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL)
				fake_usage(g_readersNow);
			return 0;
		}
		case VIDIOC_DQEVENT:
		{
			struct v4l2_event* ev = arg;
			g_dequeues++;
			if (g_queued == 0)
			{
				errno = ENOENT;
				return -1;
			}
			memset(ev, 0, sizeof(*ev));
			ev->type = FAKE_EVENT_CLIENT_USAGE;
			memcpy(ev->u.data, &g_queue[0], sizeof(UINT32));
			memmove(&g_queue[0], &g_queue[1], (g_queued - 1) * sizeof(UINT32));
			g_queued--;
			return 0;
		}
		default:
			errno = ENOTTY;
			return -1;
	}
}

static SSIZE_T fake_write(int fd, WINPR_ATTR_UNUSED const void* buffer, size_t size)
{
	if ((fd < FAKE_FD_BASE) || ((size_t)(fd - FAKE_FD_BASE) >= g_count))
		return -1;
	g_writes++;
	g_written += size;
	return (SSIZE_T)size;
}

static int fake_close(int fd)
{
	const size_t i = (size_t)(fd - FAKE_FD_BASE);
	if ((fd < FAKE_FD_BASE) || (i >= g_count) || !g_open[i])
		return -1;
	g_open[i]--;
	return 0;
}

static BOOL fake_module_version(char* buffer, size_t size)
{
	if (!g_moduleVersion)
		return FALSE;
	(void)snprintf(buffer, size, "%s", g_moduleVersion);
	return TRUE;
}

static const ShadowCameraV4l2Ops g_ops = { fake_open, fake_ioctl, fake_write, fake_close,
	                                       fake_module_version };

/* counts the camera's log lines */
static BOOL log_message(const wLogMessage* msg)
{
	if (!msg || !msg->TextString)
		return TRUE;
	if (strstr(msg->TextString, "camera:"))
		g_cameraLines++;
	if (strcmp(msg->TextString, "camera: no v4l2loopback device") == 0)
		g_noDeviceLines++;
	if (strstr(msg->TextString, "can't tell when an app reads it"))
	{
		g_fallbackLines++;
		(void)snprintf(g_lastLine, sizeof(g_lastLine), "%s", msg->TextString);
	}
	return TRUE;
}

static BOOL log_capture(void)
{
	wLog* root = WLog_GetRoot();
	if (!WLog_SetLogLevel(root, WLOG_INFO) ||
	    !WLog_SetLogAppenderType(root, WLOG_APPENDER_CALLBACK))
		return FALSE;
	wLogAppender* appender = WLog_GetLogAppender(root);
	wLogCallbacks callbacks = WINPR_C_ARRAY_INIT;
	callbacks.message = log_message;
	return appender && WLog_ConfigureAppender(appender, "callbacks", &callbacks);
}

#define CAPS_OUTPUT (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE)
#define CAPS_CAPTURE (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)

static int test_prefers_ours(void)
{
	const FakeDevice devices[] = {
		{ "/dev/video0", "uvcvideo", "Integrated Camera", CAPS_CAPTURE },
		{ "/dev/video1", "v4l2 loopback", "OBS Virtual Camera", CAPS_OUTPUT | CAPS_CAPTURE },
		{ "/dev/video3", "v4l2 loopback", SHADOW_CAMERA_V4L2_LABEL, CAPS_OUTPUT | CAPS_CAPTURE },
		{ "/dev/video5", "v4l2 loopback", "no output", CAPS_CAPTURE },
	};
	fake_reset(devices, ARRAYSIZE(devices));

	ShadowCameraSink* sink = shadow_camera_sink_v4l2_new(nullptr, 1280, 720, 30, &g_ops);
	CHECK(sink);
	CHECK(strcmp(sink->name, "v4l2loopback") == 0);
	CHECK(sink->format == SHADOW_CAMERA_PIXEL_YUY2);
	CHECK(sink->placeholderIntervalMs > 0);
	/* only ours stays open; it was found after the first loopback, which was closed */
	CHECK((g_open[0] == 0) && (g_open[1] == 0) && (g_open[2] == 1) && (g_open[3] == 0));
	CHECK(g_opens == 3); /* video0, video1, video3: the scan stops at ours */
	CHECK(g_fmt.type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
	CHECK(g_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV);
	CHECK((g_fmt.fmt.pix.width == 1280) && (g_fmt.fmt.pix.height == 720));
	CHECK((g_fmt.fmt.pix.bytesperline == 2560) && (g_fmt.fmt.pix.sizeimage == 1280 * 720 * 2));
	CHECK(g_parm.type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
	CHECK((g_parm.parm.output.timeperframe.numerator == 1) &&
	      (g_parm.parm.output.timeperframe.denominator == 30));

	static BYTE frame[1280 * 720 * 2];
	ShadowCameraImage image = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&image, SHADOW_CAMERA_PIXEL_YUY2, 1280, 720, frame,
	                               sizeof(frame)));
	CHECK(sink->Write(sink, &image, SHADOW_CAMERA_FRAME_PLACEHOLDER, nullptr));
	CHECK((g_writes == 1) && (g_written == sizeof(frame)));
	ShadowCameraImage small = WINPR_C_ARRAY_INIT;
	CHECK(
	    shadow_camera_image_wrap(&small, SHADOW_CAMERA_PIXEL_YUY2, 640, 360, frame, sizeof(frame)));
	CHECK(!sink->Write(sink, &small, SHADOW_CAMERA_FRAME_CLIENT, nullptr));
	CHECK(g_writes == 1);

	sink->Free(sink);
	CHECK(fake_open_count() == 0);
	return 0;
}

static int test_first_loopback(void)
{
	const FakeDevice devices[] = {
		{ "/dev/video0", "uvcvideo", "Integrated Camera", CAPS_CAPTURE },
		{ "/dev/video2", "v4l2 loopback", "Dummy video device", CAPS_OUTPUT | CAPS_CAPTURE },
		{ "/dev/video4", "v4l2 loopback", "Second", CAPS_OUTPUT | CAPS_CAPTURE },
	};
	fake_reset(devices, ARRAYSIZE(devices));
	ShadowCameraSink* sink = shadow_camera_sink_v4l2_new(nullptr, 640, 480, 15, &g_ops);
	CHECK(sink);
	CHECK((g_open[0] == 0) && (g_open[1] == 1) && (g_open[2] == 0));
	sink->Free(sink);
	CHECK(fake_open_count() == 0);

	/* a device named on purpose: only that one is opened */
	fake_reset(devices, ARRAYSIZE(devices));
	sink = shadow_camera_sink_v4l2_new("/dev/video4", 640, 480, 15, &g_ops);
	CHECK(sink);
	CHECK((g_opens == 1) && (g_open[2] == 1));
	sink->Free(sink);

	/* a device that is no loopback is refused even when named */
	fake_reset(devices, ARRAYSIZE(devices));
	CHECK(!shadow_camera_sink_v4l2_new("/dev/video0", 640, 480, 15, &g_ops));
	CHECK(fake_open_count() == 0);
	CHECK(g_noDeviceLines == 1);

	/* S_FMT that does not take the size (a writer of another size is attached) */
	fake_reset(devices, ARRAYSIZE(devices));
	g_shrink = TRUE;
	CHECK(!shadow_camera_sink_v4l2_new(nullptr, 1280, 720, 30, &g_ops));
	CHECK(fake_open_count() == 0);
	return 0;
}

static int test_none(void)
{
	const FakeDevice devices[] = {
		{ "/dev/video0", "uvcvideo", "Integrated Camera", CAPS_CAPTURE },
		{ "/dev/video1", "uvcvideo", "Integrated Camera", V4L2_CAP_META_CAPTURE },
		{ "/dev/video9", "v4l2 loopback", "capture only", CAPS_CAPTURE },
	};
	fake_reset(devices, ARRAYSIZE(devices));
	CHECK(!shadow_camera_sink_v4l2_new(nullptr, 1280, 720, 30, &g_ops));
	CHECK(fake_open_count() == 0);
	/* exactly one line, and nothing else said */
	CHECK(g_noDeviceLines == 1);
	CHECK(g_cameraLines == 1);

	fake_reset(nullptr, 0);
	CHECK(!shadow_camera_sink_v4l2_new(nullptr, 1280, 720, 30, &g_ops));
	CHECK((g_noDeviceLines == 1) && (g_cameraLines == 1) && (g_opens == 0));

	/* sizes it can't take: no device is even looked for */
	fake_reset(devices, ARRAYSIZE(devices));
	CHECK(!shadow_camera_sink_v4l2_new(nullptr, 1281, 720, 30, &g_ops));
	CHECK(!shadow_camera_sink_v4l2_new(nullptr, 1280, 720, 0, &g_ops));
	CHECK(g_opens == 0);
	return 0;
}

static int test_usage_events(void)
{
	/* 0.13+: subscribed with SEND_INITIAL, the readers follow the events */
	const FakeDevice devices[] = {
		{ "/dev/video0", "v4l2 loopback", SHADOW_CAMERA_V4L2_LABEL, CAPS_OUTPUT | CAPS_CAPTURE,
		  TRUE },
	};
	fake_reset(devices, ARRAYSIZE(devices));
	ShadowCameraSink* sink = shadow_camera_sink_v4l2_new(nullptr, 640, 480, 30, &g_ops);
	CHECK(sink && sink->Readers);
	CHECK(g_subscribes == 1);
	CHECK(g_fallbackLines == 0);
	CHECK(sink->Readers(sink) == 0); /* the initial event */
	CHECK(sink->Readers(sink) == 0); /* nothing new: the last value */
	fake_usage(1);
	CHECK(sink->Readers(sink) == 1);
	CHECK(sink->Readers(sink) == 1);
	/* several at once: the last one counts */
	fake_usage(0);
	fake_usage(1);
	fake_usage(0);
	CHECK(sink->Readers(sink) == 0);
	CHECK(g_queued == 0);
	fake_usage(2);
	CHECK(sink->Readers(sink) == 2);
	sink->Free(sink);
	CHECK(fake_open_count() == 0);

	/* a reader already there when the server starts */
	fake_reset(devices, ARRAYSIZE(devices));
	g_readersNow = 1;
	sink = shadow_camera_sink_v4l2_new(nullptr, 640, 480, 30, &g_ops);
	CHECK(sink);
	CHECK(sink->Readers(sink) == 1);
	sink->Free(sink);
	return 0;
}

static int test_fallback(void)
{
	/* 0.12.x: SUBSCRIBE fails, readers unknown, exactly one line, with the module's version */
	const FakeDevice devices[] = {
		{ "/dev/video0", "v4l2 loopback", SHADOW_CAMERA_V4L2_LABEL, CAPS_OUTPUT | CAPS_CAPTURE,
		  FALSE },
	};
	fake_reset(devices, ARRAYSIZE(devices));
	g_moduleVersion = "0.12.7";
	ShadowCameraSink* sink = shadow_camera_sink_v4l2_new(nullptr, 640, 480, 30, &g_ops);
	CHECK(sink);
	CHECK(g_subscribes == 1);
	for (size_t i = 0; i < 10; i++)
		CHECK(sink->Readers(sink) == -1);
	CHECK(g_dequeues == 0);
	CHECK(g_fallbackLines == 1);
	(void)printf("%s\n", g_lastLine);
	CHECK(strcmp(g_lastLine, "camera: v4l2loopback 0.12.7 can't tell when an app reads it; the "
	                         "camera stays on while connected; 0.13+ fixes this") == 0);
	sink->Free(sink);

	/* 0.12.7 declares no MODULE_VERSION: no /sys/module/v4l2loopback/version */
	fake_reset(devices, ARRAYSIZE(devices));
	sink = shadow_camera_sink_v4l2_new(nullptr, 640, 480, 30, &g_ops);
	CHECK(sink);
	CHECK(g_fallbackLines == 1);
	CHECK(strcmp(g_lastLine, "camera: v4l2loopback before 0.13 can't tell when an app reads it; "
	                         "the camera stays on while connected; 0.13+ fixes this") == 0);
	sink->Free(sink);
	return 0;
}

int TestShadowCameraV4l2(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!log_capture())
		return -1;
	if (test_prefers_ours() != 0)
		return -1;
	if (test_first_loopback() != 0)
		return -1;
	if (test_none() != 0)
		return -1;
	if (test_usage_events() != 0)
		return -1;
	if (test_fallback() != 0)
		return -1;
	return 0;
}
