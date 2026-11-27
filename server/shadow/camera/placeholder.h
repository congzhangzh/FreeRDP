/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow Server: the picture of a virtual camera that no client feeds
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_PLACEHOLDER_H
#define FREERDP_SERVER_SHADOW_CAMERA_PLACEHOLDER_H

/* The still a shadow server's virtual camera shows while no client camera feeds it: a video
 * camera with a slash through it, light grey on a dark background, no text, centred, the
 * same on Linux, Windows and macOS. It is drawn here, procedurally, at any size (no image
 * file to ship), 3x3 supersampled for smooth edges. Plain C with no FreeRDP dependency, so
 * a sandboxed camera extension can build it too. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* the two tones, full range (0..255) */
#define CAMERA_PLACEHOLDER_BACKGROUND 31
#define CAMERA_PLACEHOLDER_FOREGROUND 204

	/* one byte per pixel, full range: the picture's shape and tones, the base of the others */
	bool camera_placeholder_gray(uint8_t* dst, size_t stride, uint32_t width, uint32_t height);

	/* NV12, video range (BT.601/709 alike: grey has no chroma): width and height even */
	bool camera_placeholder_nv12(uint8_t* y, size_t yStride, uint8_t* uv, size_t uvStride,
	                             uint32_t width, uint32_t height);

	/* I420, video range: width and height even */
	bool camera_placeholder_i420(uint8_t* y, size_t yStride, uint8_t* u, size_t uStride,
	                             uint8_t* v, size_t vStride, uint32_t width, uint32_t height);

	/* 32-bit B, G, R, A (A = 255) */
	bool camera_placeholder_bgra(uint8_t* dst, size_t stride, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_PLACEHOLDER_H */
