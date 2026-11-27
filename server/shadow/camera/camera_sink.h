/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: where the frames go
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_SINK_H
#define FREERDP_SERVER_SHADOW_CAMERA_SINK_H

#include <winpr/wtypes.h>

#include "camera_image.h"

#ifdef __cplusplus
extern "C"
{
#endif

	/** What a frame shows, for the sinks that record it */
	typedef enum
	{
		SHADOW_CAMERA_FRAME_PLACEHOLDER,
		SHADOW_CAMERA_FRAME_CLIENT
	} SHADOW_CAMERA_FRAME_KIND;

	typedef struct s_shadow_camera_sink ShadowCameraSink;

	/** A sink takes frames of one fixed format and size, from one thread at a time (the
	 *  camera serializes the calls). */
	struct s_shadow_camera_sink
	{
		const char* name;
		SHADOW_CAMERA_PIXEL format; /**< YUY2 or NV12 */
		UINT32 width;
		UINT32 height;
		UINT32 fps;
		/** how often the still is written again while no client camera streams, in ms;
		 *  0: once each time it starts showing */
		UINT32 placeholderIntervalMs;

		/** writes a tightly packed frame of format, width x height; detail describes its
		 *  source ("H264 640x480") or nullptr */
		WINPR_ATTR_NODISCARD BOOL (*Write)(ShadowCameraSink* sink, const ShadowCameraImage* frame,
		                                   SHADOW_CAMERA_FRAME_KIND kind, const char* detail);
		/** closes the sink and frees it */
		void (*Free)(ShadowCameraSink* sink);
	};

	/** A sink writing every frame to <dir>/frames.yuyv (or .nv12) and a line for each to
	 *  <dir>/index.txt; for tests (CI), never the default. dir is created. */
	WINPR_ATTR_NODISCARD ShadowCameraSink* shadow_camera_sink_file_new(const char* dir,
	                                                                   SHADOW_CAMERA_PIXEL format,
	                                                                   UINT32 width, UINT32 height,
	                                                                   UINT32 fps);

#if defined(__linux__)
	/** The system calls the v4l2loopback sink makes, replaceable for tests */
	typedef struct
	{
		int (*open)(const char* path, int flags);
		int (*ioctl)(int fd, unsigned long request, void* arg);
		SSIZE_T (*write)(int fd, const void* buffer, size_t size);
		int (*close)(int fd);
	} ShadowCameraV4l2Ops;

/** the label v4l2loopback devices of ours are given (card_label=), preferred when several */
#define SHADOW_CAMERA_V4L2_LABEL "FreeRDP Camera"
/** the devices looked at: /dev/video0 to /dev/video63 */
#define SHADOW_CAMERA_V4L2_MAX_DEVICES 64

	/** Finds a v4l2loopback device (VIDIOC_QUERYCAP driver "v4l2 loopback" with video
	 *  output), the one labelled SHADOW_CAMERA_V4L2_LABEL first, or opens device when given,
	 *  and sets it to YUYV width x height at fps. Without one it logs exactly one line,
	 *  "camera: no v4l2loopback device", and returns nullptr. ops nullptr: the real calls. */
	WINPR_ATTR_NODISCARD ShadowCameraSink*
	shadow_camera_sink_v4l2_new(const char* device, UINT32 width, UINT32 height, UINT32 fps,
	                            const ShadowCameraV4l2Ops* ops);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_SINK_H */
