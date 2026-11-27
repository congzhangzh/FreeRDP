/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: a client's H.264 or MJPG decoded by VideoToolbox
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_DECODE_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_DECODE_H

/* The client's preferred formats, H.264 then MJPG (raw last: the most bytes on the wire),
 * decoded with the Mac's own VideoToolbox (no FFmpeg), straight to the camera's NV12 at the
 * camera's size: VideoToolbox converts and scales to the destination attributes it is given.
 * Runs in the root daemon as well (no window server needed). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <CoreVideo/CoreVideo.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct cam_mac_decoder camMacDecoder;

	/* format: CAM_MAC_FORMAT_H264 or CAM_MAC_FORMAT_MJPG; width x height: the client's media
	 * type (MJPG needs it up front; H.264 takes it from its SPS); the output is NV12 ('420v')
	 * of dstWidth x dstHeight */
	camMacDecoder* cam_mac_decoder_new(uint32_t format, uint32_t width, uint32_t height,
	                                   uint32_t dstWidth, uint32_t dstHeight);
	void cam_mac_decoder_free(camMacDecoder* decoder);

	/* one sample (H.264: an Annex B access unit; MJPG: one JPEG image), decoded at once: a new
	 * pixel buffer (+1), or nullptr (H.264 before its first SPS and PPS, or a decode error:
	 * see the log). A new SPS or PPS starts a new session by itself. */
	CVPixelBufferRef cam_mac_decoder_decode(camMacDecoder* decoder, const uint8_t* data,
	                                        size_t size);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_DECODE_H */
