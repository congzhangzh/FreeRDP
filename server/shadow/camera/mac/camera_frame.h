/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: a client's raw frame to the extension's NV12
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_FRAME_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_FRAME_H

/* Plain C (no Apple framework), so the unit tests run on any CI machine. The compressed
 * formats (H.264, MJPG) are VideoToolbox's (camera_decode.h); these are the raw ones. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/* the bytes of one tightly packed raw frame of a CAM_MAC_FORMAT_* (camera_config.h);
	 * false for a compressed format, an odd size where the format needs an even one, or an
	 * overflow */
	bool cam_frame_raw_size(uint32_t format, uint32_t width, uint32_t height, size_t* size);

	/* converts a tightly packed raw frame (NV12, I420, YUY2, RGB24 or RGB32: B, G, R[, X] in
	 * memory, top-down) of srcWidth x srcHeight to NV12, video range, of dstWidth x
	 * dstHeight (nearest neighbour when the sizes differ; both even). RGB goes through
	 * BT.601. false when size is not the frame's or the format is not raw */
	bool cam_frame_to_nv12(uint32_t format, uint32_t srcWidth, uint32_t srcHeight,
	                       const uint8_t* src, size_t size, uint8_t* dstY, size_t dstYStride,
	                       uint8_t* dstUV, size_t dstUVStride, uint32_t dstWidth,
	                       uint32_t dstHeight);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_FRAME_H */
