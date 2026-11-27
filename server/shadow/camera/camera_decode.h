/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a client's samples to images
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_DECODE_H
#define FREERDP_SERVER_SHADOW_CAMERA_DECODE_H

#include <freerdp/channels/rdpecam.h>

#include "camera_image.h"
#include "camera_policy.h"

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct s_shadow_camera_decoder ShadowCameraDecoder;

	/** What this build decodes: H.264 with FreeRDP's H.264 decoder (FFmpeg's h264 on the
	 *  Linux host), MJPG with FFmpeg's mjpeg through freerdp_video_sample_convert() */
	void shadow_camera_decoders_probe(ShadowCameraDecoders* decoders);

	/** A decoder for samples of type (any format of MS-RDPECAM) */
	WINPR_ATTR_NODISCARD ShadowCameraDecoder*
	shadow_camera_decoder_new(const CAM_MEDIA_TYPE_DESCRIPTION* type);
	void shadow_camera_decoder_free(ShadowCameraDecoder* decoder);

	/** Turns one sample into an image, valid until the next call. A raw sample shorter than
	 *  a frame is refused. *picture is FALSE when the sample gave no picture yet (an H.264
	 *  decoder that waits for more), TRUE with the image otherwise. */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_decoder_decode(ShadowCameraDecoder* decoder,
	                                                       const BYTE* sample, size_t size,
	                                                       ShadowCameraImage* image, BOOL* picture);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_DECODE_H */
