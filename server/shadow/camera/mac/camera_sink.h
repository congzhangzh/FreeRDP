/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: pushing a client's frames into the camera extension's sink
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_SINK_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_SINK_H

/* The process in the user's GUI session pushes the frames: the server itself in a user's
 * session (the no-root LaunchAgent), or freerdp-shadow-agent in the daemon mode (the root
 * daemon decodes and sends it each frame as an IOSurface, see mac_ipc.h). It finds the
 * extension's device by its UID (camera_config.h), takes the sink stream's buffer queue
 * (CMIOStreamCopyBufferQueue), starts the stream and enqueues CMSampleBuffers of NV12
 * CVPixelBuffers; the extension hands them to its source stream, the camera the apps see.
 *
 * One camera owner at a time (the shadow side's camera core decides who): the owner is
 * cam_mac_sink_set_owner()'s; the stream and its pool stay up across a change, the frames are
 * stamped with the host clock (never the client's), so time only goes forward across owners,
 * and the extension shows its placeholder in the gap when there is one
 * (CAM_MAC_SINK_IDLE_MS). Thread safe. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct cam_mac_sink camMacSink;

	typedef enum
	{
		CAM_MAC_SINK_OK = 0,
		CAM_MAC_SINK_NO_DEVICE,  /* the extension is not active (not installed, not approved) */
		CAM_MAC_SINK_NO_STREAM,  /* its device has no sink stream */
		CAM_MAC_SINK_FULL,       /* the queue is full: the frame was dropped */
		CAM_MAC_SINK_BAD_FRAME,  /* not NV12 of the extension's size, or not convertible */
		CAM_MAC_SINK_ERROR       /* CoreMediaIO refused: see the log */
	} camMacSinkStatus;

	const char* cam_mac_sink_status_string(camMacSinkStatus status);

	/* deviceUID: nullptr for the extension's (CAM_MAC_DEVICE_UUID) */
	camMacSink* cam_mac_sink_new(const char* deviceUID);
	void cam_mac_sink_free(camMacSink* sink);

	/* finds the device and its sink and starts the stream; again after a failure (the
	 * extension approved later, or restarted). Cheap when already open. */
	camMacSinkStatus cam_mac_sink_open(camMacSink* sink);
	/* stops the stream, drops what is queued: the extension shows its placeholder again */
	void cam_mac_sink_close(camMacSink* sink);
	bool cam_mac_sink_is_open(camMacSink* sink);

	/* the client whose camera feeds the sink now (0: none) */
	void cam_mac_sink_set_owner(camMacSink* sink, uint64_t owner);

	/* an NV12 ('420v') pixel buffer of CAM_MAC_WIDTH x CAM_MAC_HEIGHT, stamped with the host
	 * clock now; opens the sink when it is not open. The buffer is retained as long as the
	 * extension needs it. */
	camMacSinkStatus cam_mac_sink_push(camMacSink* sink, CVPixelBufferRef nv12);

	/* a raw MS-RDPECAM sample (CAM_MAC_FORMAT_NV12, I420, YUY2, RGB24, RGB32) converted and
	 * pushed (compressed ones go through camera_decode.h first) */
	camMacSinkStatus cam_mac_sink_push_raw(camMacSink* sink, uint32_t format, uint32_t width,
	                                       uint32_t height, const uint8_t* data, size_t size);

	/* --- on demand: is an app on the Mac reading the camera? -------------------------------
	 * The extension keeps the count of its source stream's clients in a device property
	 * (camera_config.h, CAM_MAC_READERS_*). The watcher listens to it (a CoreMediaIO property
	 * listener) and polls it every CAM_MAC_READERS_POLL_MS (the extension approved later,
	 * restarted, a lost notification), and holds "in use" CAM_MAC_READERS_HOLD_MS after the
	 * last reader (camera_readers.h). Independent of open/close: it watches before the first
	 * frame, which is the point. */

	/* fn(context, active) runs on the watcher's own queue at each change; it must not free the
	 * sink. Once per sink; true when watching. */
	typedef void (*camMacReadersFn)(void *context, bool active);
	bool cam_mac_sink_watch_readers(camMacSink *sink, camMacReadersFn fn, void *context);

	/* an app reads the camera now (with the hold); false when nobody does, or no watcher yet */
	bool cam_mac_sink_readers_active(camMacSink *sink);

	/* the readers the extension counted at the last look; -1: unknown (no device, no watcher) */
	int cam_mac_sink_readers(camMacSink *sink);

	/* --- the parts, for the daemon (which converts but cannot push) and the tests --- */

	/* a new IOSurface-backed NV12 ('420v') pixel buffer, width x height (even); +1 */
	CVPixelBufferRef cam_mac_pixel_buffer_new(uint32_t width, uint32_t height);

	/* a raw sample converted to a new CAM_MAC_WIDTH x CAM_MAC_HEIGHT NV12 pixel buffer (+1),
	 * scaled when its size differs; nullptr when it is not a raw format or not that size */
	CVPixelBufferRef cam_mac_pixel_buffer_from_raw(uint32_t format, uint32_t width,
	                                               uint32_t height, const uint8_t* data,
	                                               size_t size);

	/* the placeholder (../placeholder.h) as a new NV12 pixel buffer (+1) */
	CVPixelBufferRef cam_mac_pixel_buffer_placeholder(uint32_t width, uint32_t height);

	/* a ready sample buffer (+1) around the pixel buffer, at pts, of one frame's duration
	 * (1 / CAM_MAC_FPS) */
	CMSampleBufferRef cam_mac_sample_buffer_new(CVPixelBufferRef pixels, CMTime pts);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_SINK_H */
