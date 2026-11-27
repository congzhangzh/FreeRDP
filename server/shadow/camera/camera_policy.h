/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the decisions (media type, pacing, owner), kept free of
 * channels and devices so they can be tested alone
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_POLICY_H
#define FREERDP_SERVER_SHADOW_CAMERA_POLICY_H

#include <winpr/wtypes.h>
#include <freerdp/channels/rdpecam.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** the most sample requests kept outstanding with a client */
#define SHADOW_CAMERA_MAX_OUTSTANDING 4u

	/** What this build can decode; the raw formats need nothing */
	typedef struct
	{
		BOOL h264;
		BOOL mjpg;
	} ShadowCameraDecoders;

	/** The name of a media format, for the logs */
	WINPR_ATTR_NODISCARD const char* shadow_camera_format_name(CAM_MEDIA_FORMAT format);

	/** The preference of a format, lower is better: H.264, then MJPG, then the raw ones (YUY2,
	 *  NV12, I420, RGB32, RGB24). -1 for a format this build can't take. */
	WINPR_ATTR_NODISCARD int shadow_camera_format_rank(CAM_MEDIA_FORMAT format,
	                                                   const ShadowCameraDecoders* decoders);

	/** Chooses one of a client's media types for a camera of width x height at fps frames per
	 *  second: the best format first (see shadow_camera_format_rank), then the size (the
	 *  largest that fits, else the smallest that doesn't), then the frame rate (the closest to
	 *  fps, a faster one before a slower one). Types with a zero or absurd size or frame rate
	 *  are skipped. FALSE when none can be taken, else *index is the choice. */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_choose_media_type(
	    const CAM_MEDIA_TYPE_DESCRIPTION* types, size_t count, const ShadowCameraDecoders* decoders,
	    UINT32 width, UINT32 height, UINT32 fps, size_t* index);

	/** How many sample requests to keep outstanding: ceil(fps * rtt) + 1, at most
	 *  SHADOW_CAMERA_MAX_OUTSTANDING; rttMs 0 (not measured) gives 1 */
	WINPR_ATTR_NODISCARD UINT32 shadow_camera_pacing_outstanding(UINT32 fpsNumerator,
	                                                             UINT32 fpsDenominator,
	                                                             UINT32 rttMs);

	/** One camera owner per host: the first camera that asks gets it, until it is released;
	 *  the others are refused. A release never hands the camera to anyone: the next camera to
	 *  ask after it gets it. */
	typedef struct
	{
		const void* owner;
	} ShadowCameraOwner;

	typedef enum
	{
		SHADOW_CAMERA_CLAIM_GRANTED,
		SHADOW_CAMERA_CLAIM_REFUSED
	} SHADOW_CAMERA_CLAIM;

	/** A camera (any unique pointer, a client's device) asks for the virtual camera */
	WINPR_ATTR_NODISCARD SHADOW_CAMERA_CLAIM shadow_camera_owner_claim(ShadowCameraOwner* owner,
	                                                                   const void* camera);

	/** The camera leaves (removed, failed, its client gone). TRUE if it was the owner */
	BOOL shadow_camera_owner_release(ShadowCameraOwner* owner, const void* camera);

	WINPR_ATTR_NODISCARD BOOL shadow_camera_owner_is(const ShadowCameraOwner* owner,
	                                                 const void* camera);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_POLICY_H */
