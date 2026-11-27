/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a v4l2loopback device as the camera (Linux)
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
 * The module (v4l2loopback, GPL-2) is installed by the user; this file uses only the kernel's
 * UAPI header videodev2.h (GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause).
 *
 * The device is opened once at server start and keeps a writer attached until the server
 * stops: with exclusive_caps=1 (Chrome, WebRTC) a v4l2loopback device only offers capture
 * while a writer is attached, so the still is written from the start.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <winpr/assert.h>
#include <winpr/string.h>

#include <freerdp/log.h>

#include "camera_sink.h"

#define TAG SERVER_TAG("shadow.camera")

/* the driver name v4l2loopback reports (v4l2loopback.c, vidioc_querycap) */
#define V4L2LOOPBACK_DRIVER "v4l2 loopback"

/* the still is written this often while no client camera streams, so that readers
 * (v4l2loopback blocks them until a new frame comes) keep running */
#define V4L2_PLACEHOLDER_INTERVAL_MS 200

typedef struct
{
	ShadowCameraSink sink;
	ShadowCameraV4l2Ops ops;
	int fd;
	size_t frameSize;
	char path[64];
	BOOL writeFailed;
} V4l2Sink;

static int v4l2_real_open(const char* path, int flags)
{
	return open(path, flags);
}

static int v4l2_real_ioctl(int fd, unsigned long request, void* arg)
{
	int rc = 0;
	do
	{
		rc = ioctl(fd, request, arg);
	} while ((rc < 0) && (errno == EINTR));
	return rc;
}

static SSIZE_T v4l2_real_write(int fd, const void* buffer, size_t size)
{
	return write(fd, buffer, size);
}

static int v4l2_real_close(int fd)
{
	return close(fd);
}

static const ShadowCameraV4l2Ops v4l2_real_ops = { v4l2_real_open, v4l2_real_ioctl, v4l2_real_write,
	                                               v4l2_real_close };

/* 1: a v4l2loopback device with video output, labelled ours; 0: one not labelled ours;
 * -1: none (not v4l2loopback, no output, or QUERYCAP failed) */
static int v4l2_probe(const ShadowCameraV4l2Ops* ops, int fd)
{
	struct v4l2_capability cap = WINPR_C_ARRAY_INIT;
	if (ops->ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		return -1;

	char driver[sizeof(cap.driver) + 1] = WINPR_C_ARRAY_INIT;
	char card[sizeof(cap.card) + 1] = WINPR_C_ARRAY_INIT;
	memcpy(driver, cap.driver, sizeof(cap.driver));
	memcpy(card, cap.card, sizeof(cap.card));
	if (strcmp(driver, V4L2LOOPBACK_DRIVER) != 0)
		return -1;

	const UINT32 caps =
	    (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
	if ((caps & V4L2_CAP_VIDEO_OUTPUT) == 0)
		return -1;
	return (strcmp(card, SHADOW_CAMERA_V4L2_LABEL) == 0) ? 1 : 0;
}

/* opens path read-write and probes it: the fd of a v4l2loopback device or -1 */
static int v4l2_open_probe(const ShadowCameraV4l2Ops* ops, const char* path, int* score)
{
	const int fd = ops->open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	*score = v4l2_probe(ops, fd);
	if (*score < 0)
	{
		(void)ops->close(fd);
		return -1;
	}
	return fd;
}

static BOOL v4l2_set_format(V4l2Sink* vs)
{
	struct v4l2_format fmt = WINPR_C_ARRAY_INIT;
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = vs->sink.width;
	fmt.fmt.pix.height = vs->sink.height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = vs->sink.width * 2;
	fmt.fmt.pix.sizeimage = (UINT32)vs->frameSize;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	if (vs->ops.ioctl(vs->fd, VIDIOC_S_FMT, &fmt) < 0)
	{
		WLog_ERR(TAG, "camera: %s: VIDIOC_S_FMT YUYV %" PRIu32 "x%" PRIu32 " failed: %s", vs->path,
		         vs->sink.width, vs->sink.height, strerror(errno));
		return FALSE;
	}
	if ((fmt.fmt.pix.width != vs->sink.width) || (fmt.fmt.pix.height != vs->sink.height) ||
	    (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV))
	{
		WLog_ERR(TAG,
		         "camera: %s took %" PRIu32 "x%" PRIu32 " instead of YUYV %" PRIu32 "x%" PRIu32
		         " (is another writer attached?)",
		         vs->path, fmt.fmt.pix.width, fmt.fmt.pix.height, vs->sink.width, vs->sink.height);
		return FALSE;
	}

	/* the frame rate readers are told; v4l2loopback takes it on the output side */
	struct v4l2_streamparm parm = WINPR_C_ARRAY_INIT;
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = vs->sink.fps;
	if (vs->ops.ioctl(vs->fd, VIDIOC_S_PARM, &parm) < 0)
		WLog_WARN(TAG, "camera: %s: VIDIOC_S_PARM %" PRIu32 " fps failed: %s", vs->path,
		          vs->sink.fps, strerror(errno));
	return TRUE;
}

static BOOL v4l2_sink_write(ShadowCameraSink* sink, const ShadowCameraImage* frame,
                            WINPR_ATTR_UNUSED SHADOW_CAMERA_FRAME_KIND kind,
                            WINPR_ATTR_UNUSED const char* detail)
{
	V4l2Sink* vs = (V4l2Sink*)sink;
	WINPR_ASSERT(vs);
	WINPR_ASSERT(frame);

	if ((frame->format != SHADOW_CAMERA_PIXEL_YUY2) || (frame->width != sink->width) ||
	    (frame->height != sink->height))
		return FALSE;

	const SSIZE_T written = vs->ops.write(vs->fd, frame->planes[0], vs->frameSize);
	if ((written < 0) || ((size_t)written != vs->frameSize))
	{
		/* once: a reader that went away or a full queue must not flood the log */
		if (!vs->writeFailed)
			WLog_WARN(TAG, "camera: writing to %s failed: %s", vs->path,
			          (written < 0) ? strerror(errno) : "short write");
		vs->writeFailed = TRUE;
		return FALSE;
	}
	vs->writeFailed = FALSE;
	return TRUE;
}

static void v4l2_sink_free(ShadowCameraSink* sink)
{
	V4l2Sink* vs = (V4l2Sink*)sink;
	if (!vs)
		return;
	if (vs->fd >= 0)
		(void)vs->ops.close(vs->fd);
	free(vs);
}

ShadowCameraSink* shadow_camera_sink_v4l2_new(const char* device, UINT32 width, UINT32 height,
                                              UINT32 fps, const ShadowCameraV4l2Ops* ops)
{
	const size_t frameSize = shadow_camera_image_size(SHADOW_CAMERA_PIXEL_YUY2, width, height);
	if ((frameSize == 0) || (fps == 0))
		return nullptr;

	V4l2Sink* vs = calloc(1, sizeof(V4l2Sink));
	if (!vs)
		return nullptr;
	vs->ops = ops ? *ops : v4l2_real_ops;
	vs->fd = -1;
	vs->frameSize = frameSize;
	vs->sink.name = "v4l2loopback";
	vs->sink.format = SHADOW_CAMERA_PIXEL_YUY2;
	vs->sink.width = width;
	vs->sink.height = height;
	vs->sink.fps = fps;
	vs->sink.placeholderIntervalMs = V4L2_PLACEHOLDER_INTERVAL_MS;
	vs->sink.Write = v4l2_sink_write;
	vs->sink.Free = v4l2_sink_free;

	int score = -1;
	if (device)
	{
		vs->fd = v4l2_open_probe(&vs->ops, device, &score);
		(void)snprintf(vs->path, sizeof(vs->path), "%s", device);
	}
	else
	{
		/* the one labelled ours, else the first */
		for (int i = 0; (i < SHADOW_CAMERA_V4L2_MAX_DEVICES) && (score < 1); i++)
		{
			char path[sizeof(vs->path)] = WINPR_C_ARRAY_INIT;
			(void)snprintf(path, sizeof(path), "/dev/video%d", i);
			int s = -1;
			const int fd = v4l2_open_probe(&vs->ops, path, &s);
			if (fd < 0)
				continue;
			if (s > score)
			{
				if (vs->fd >= 0)
					(void)vs->ops.close(vs->fd);
				vs->fd = fd;
				score = s;
				(void)snprintf(vs->path, sizeof(vs->path), "%s", path);
			}
			else
				(void)vs->ops.close(fd);
		}
	}

	if (vs->fd < 0)
	{
		WLog_INFO(TAG, "camera: no v4l2loopback device");
		v4l2_sink_free(&vs->sink);
		return nullptr;
	}

	if (!v4l2_set_format(vs))
	{
		v4l2_sink_free(&vs->sink);
		return nullptr;
	}

	WLog_INFO(TAG, "camera: v4l2loopback %s, YUYV %" PRIu32 "x%" PRIu32 " at %" PRIu32 " fps",
	          vs->path, width, height, fps);
	return &vs->sink;
}
