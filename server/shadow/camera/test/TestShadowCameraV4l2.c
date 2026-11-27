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
} FakeDevice;

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

static const ShadowCameraV4l2Ops g_ops = { fake_open, fake_ioctl, fake_write, fake_close };

/* counts the camera's log lines */
static BOOL log_message(const wLogMessage* msg)
{
	if (!msg || !msg->TextString)
		return TRUE;
	if (strstr(msg->TextString, "camera:"))
		g_cameraLines++;
	if (strcmp(msg->TextString, "camera: no v4l2loopback device") == 0)
		g_noDeviceLines++;
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
	return 0;
}
