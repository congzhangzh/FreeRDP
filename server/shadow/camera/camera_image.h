/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: images and their conversion to the camera's format
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_IMAGE_H
#define FREERDP_SERVER_SHADOW_CAMERA_IMAGE_H

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/** The pixel layouts the camera reads (a client's raw samples, a decoder's output) and
	 *  writes (the virtual camera: YUY2 for v4l2loopback, NV12 for Windows). The RGB ones are
	 *  the byte orders of MS-RDPECAM's RGB24 and RGB32 (MFVideoFormat_RGB24/RGB32). */
	typedef enum
	{
		SHADOW_CAMERA_PIXEL_NONE = 0,
		SHADOW_CAMERA_PIXEL_I420,   /**< Y plane, then U and V planes at half size */
		SHADOW_CAMERA_PIXEL_NV12,   /**< Y plane, then one plane of U,V pairs at half size */
		SHADOW_CAMERA_PIXEL_YUY2,   /**< Y0 U Y1 V for each pair of pixels (V4L2 YUYV) */
		SHADOW_CAMERA_PIXEL_BGR24,  /**< B, G, R */
		SHADOW_CAMERA_PIXEL_BGRX32, /**< B, G, R, unused */
		/** B, G, R, unused, from FreeRDP's H.264 decoder: its YUV to RGB is full range BT.709
		 *  (libfreerdp/primitives/prim_internal.h), so its own inverse gives back the YUV the
		 *  stream carried, whatever matrix the camera used */
		SHADOW_CAMERA_PIXEL_BGRX32_CODEC,
	} SHADOW_CAMERA_PIXEL;

	/** An image: planes and strides as the format needs (one plane for the packed formats,
	 *  two for NV12, three for I420). It does not own its memory. */
	typedef struct
	{
		SHADOW_CAMERA_PIXEL format;
		UINT32 width;
		UINT32 height;
		BYTE* planes[3];
		UINT32 strides[3];
		BOOL bottomUp; /**< RGB only: the first row in memory is the bottom one */
	} ShadowCameraImage;

	/** The name of a format, for the logs */
	WINPR_ATTR_NODISCARD const char* shadow_camera_pixel_name(SHADOW_CAMERA_PIXEL format);

	/** The bytes of a tightly packed image, 0 for a size the format can't have (an odd width
	 *  for the 4:2:x formats, an odd height for the 4:2:0 ones, nothing, or too big) */
	WINPR_ATTR_NODISCARD size_t shadow_camera_image_size(SHADOW_CAMERA_PIXEL format, UINT32 width,
	                                                     UINT32 height);

	/** Points image at the tightly packed data of the given size. FALSE when size is smaller
	 *  than the format needs: the data comes from the remote client and is never over-read. */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_image_wrap(ShadowCameraImage* image,
	                                                   SHADOW_CAMERA_PIXEL format, UINT32 width,
	                                                   UINT32 height, const BYTE* data,
	                                                   size_t size);

	/** Fills dst (YUY2, NV12 or I420) with black */
	void shadow_camera_image_fill_black(ShadowCameraImage* dst);

	/** Scales src into dst keeping its aspect ratio, centred, the rest black (letterbox or
	 *  pillarbox). dst is YUY2, NV12 or I420; src any format. Nearest neighbour. */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_image_letterbox(const ShadowCameraImage* src,
	                                                        ShadowCameraImage* dst);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_IMAGE_H */
