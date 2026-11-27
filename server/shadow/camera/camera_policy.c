/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the decisions (media type, pacing, owner)
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

#include <winpr/assert.h>
#include <winpr/synch.h>

#include "camera_policy.h"

/* a media type larger or faster than this is taken for garbage */
#define CAMERA_POLICY_MAX_DIMENSION 8192u
#define CAMERA_POLICY_MAX_FPS 1000u

const char* shadow_camera_format_name(CAM_MEDIA_FORMAT format)
{
	switch (format)
	{
		case CAM_MEDIA_FORMAT_H264:
			return "H264";
		case CAM_MEDIA_FORMAT_MJPG:
			return "MJPG";
		case CAM_MEDIA_FORMAT_YUY2:
			return "YUY2";
		case CAM_MEDIA_FORMAT_NV12:
			return "NV12";
		case CAM_MEDIA_FORMAT_I420:
			return "I420";
		case CAM_MEDIA_FORMAT_RGB24:
			return "RGB24";
		case CAM_MEDIA_FORMAT_RGB32:
			return "RGB32";
		case CAM_MEDIA_FORMAT_INVALID:
		default:
			return "invalid";
	}
}

int shadow_camera_format_rank(CAM_MEDIA_FORMAT format, const ShadowCameraDecoders* decoders)
{
	WINPR_ASSERT(decoders);

	switch (format)
	{
		case CAM_MEDIA_FORMAT_H264:
			return decoders->h264 ? 0 : -1;
		case CAM_MEDIA_FORMAT_MJPG:
			return decoders->mjpg ? 1 : -1;
		case CAM_MEDIA_FORMAT_YUY2:
			return 2;
		case CAM_MEDIA_FORMAT_NV12:
			return 3;
		case CAM_MEDIA_FORMAT_I420:
			return 4;
		case CAM_MEDIA_FORMAT_RGB32:
			return 5;
		case CAM_MEDIA_FORMAT_RGB24:
			return 6;
		case CAM_MEDIA_FORMAT_INVALID:
		default:
			return -1;
	}
}

/* frames per second times 1000 */
static UINT64 camera_policy_millifps(const CAM_MEDIA_TYPE_DESCRIPTION* type)
{
	return 1000ull * type->FrameRateNumerator / type->FrameRateDenominator;
}

static BOOL camera_policy_valid(const CAM_MEDIA_TYPE_DESCRIPTION* type)
{
	if ((type->Width == 0) || (type->Height == 0) || (type->Width > CAMERA_POLICY_MAX_DIMENSION) ||
	    (type->Height > CAMERA_POLICY_MAX_DIMENSION))
		return FALSE;
	if ((type->FrameRateNumerator == 0) || (type->FrameRateDenominator == 0))
		return FALSE;
	const UINT64 mfps = camera_policy_millifps(type);
	return (mfps > 0) && (mfps <= 1000ull * CAMERA_POLICY_MAX_FPS);
}

/* < 0 when a is better than b, > 0 when b is better, 0 when they are as good */
static int camera_policy_compare(const CAM_MEDIA_TYPE_DESCRIPTION* a,
                                 const CAM_MEDIA_TYPE_DESCRIPTION* b,
                                 const ShadowCameraDecoders* decoders, UINT32 width, UINT32 height,
                                 UINT32 fps)
{
	const int rankA = shadow_camera_format_rank(a->Format, decoders);
	const int rankB = shadow_camera_format_rank(b->Format, decoders);
	if (rankA != rankB)
		return rankA - rankB;

	const BOOL fitsA = (a->Width <= width) && (a->Height <= height);
	const BOOL fitsB = (b->Width <= width) && (b->Height <= height);
	if (fitsA != fitsB)
		return fitsA ? -1 : 1;
	const UINT64 areaA = 1ull * a->Width * a->Height;
	const UINT64 areaB = 1ull * b->Width * b->Height;
	if (areaA != areaB)
	{
		/* fitting: the larger, too large: the smaller */
		if (fitsA)
			return (areaA > areaB) ? -1 : 1;
		return (areaA < areaB) ? -1 : 1;
	}

	const UINT64 target = 1000ull * fps;
	const UINT64 fpsA = camera_policy_millifps(a);
	const UINT64 fpsB = camera_policy_millifps(b);
	const UINT64 distA = (fpsA > target) ? fpsA - target : target - fpsA;
	const UINT64 distB = (fpsB > target) ? fpsB - target : target - fpsB;
	if (distA != distB)
		return (distA < distB) ? -1 : 1;
	if (fpsA != fpsB)
		return (fpsA > fpsB) ? -1 : 1;
	return 0;
}

BOOL shadow_camera_choose_media_type(const CAM_MEDIA_TYPE_DESCRIPTION* types, size_t count,
                                     const ShadowCameraDecoders* decoders, UINT32 width,
                                     UINT32 height, UINT32 fps, size_t* index)
{
	WINPR_ASSERT(decoders);
	WINPR_ASSERT(index);

	if (!types)
		return FALSE;

	BOOL found = FALSE;
	size_t best = 0;
	for (size_t i = 0; i < count; i++)
	{
		const CAM_MEDIA_TYPE_DESCRIPTION* type = &types[i];
		if (!camera_policy_valid(type) || (shadow_camera_format_rank(type->Format, decoders) < 0))
			continue;
		if (!found || (camera_policy_compare(type, &types[best], decoders, width, height, fps) < 0))
		{
			best = i;
			found = TRUE;
		}
	}

	if (found)
		*index = best;
	return found;
}

UINT32 shadow_camera_pacing_outstanding(UINT32 fpsNumerator, UINT32 fpsDenominator, UINT32 rttMs)
{
	if (fpsDenominator == 0)
		fpsDenominator = 1;

	/* ceil(fps * rtt): the frames the client captures while a request and its sample travel */
	const UINT64 num = 1ull * fpsNumerator * rttMs;
	const UINT64 den = 1000ull * fpsDenominator;
	const UINT64 inFlight = (num + den - 1) / den;
	const UINT64 outstanding = inFlight + 1;
	if (outstanding > SHADOW_CAMERA_MAX_OUTSTANDING)
		return SHADOW_CAMERA_MAX_OUTSTANDING;
	return (UINT32)outstanding;
}

SHADOW_CAMERA_CLAIM shadow_camera_owner_claim(ShadowCameraOwner* owner, const void* camera)
{
	WINPR_ASSERT(owner);
	WINPR_ASSERT(camera);

	if (!owner->owner)
		owner->owner = camera;
	return (owner->owner == camera) ? SHADOW_CAMERA_CLAIM_GRANTED : SHADOW_CAMERA_CLAIM_REFUSED;
}

BOOL shadow_camera_owner_release(ShadowCameraOwner* owner, const void* camera)
{
	WINPR_ASSERT(owner);

	if (!camera || (owner->owner != camera))
		return FALSE;
	owner->owner = nullptr;
	return TRUE;
}

BOOL shadow_camera_owner_is(const ShadowCameraOwner* owner, const void* camera)
{
	WINPR_ASSERT(owner);
	return camera && (owner->owner == camera);
}

SHADOW_CAMERA_ADMIT shadow_camera_admit(ShadowCameraOwner* owner, const void* camera,
                                        BOOL mayInteract, BOOL hasCamera)
{
	WINPR_ASSERT(owner);
	WINPR_ASSERT(camera);

	if (!mayInteract)
		return SHADOW_CAMERA_ADMIT_VIEW_ONLY;
	if (!hasCamera)
		return SHADOW_CAMERA_ADMIT_NO_CAMERA;
	if (shadow_camera_owner_claim(owner, camera) != SHADOW_CAMERA_CLAIM_GRANTED)
		return SHADOW_CAMERA_ADMIT_OWNED;
	return SHADOW_CAMERA_ADMIT_GRANTED;
}

BOOL shadow_camera_demand_update(ShadowCameraDemand* demand, INT32 readers, UINT64 nowMs,
                                 UINT32* recheckMs)
{
	WINPR_ASSERT(demand);
	WINPR_ASSERT(recheckMs);

	*recheckMs = INFINITE;
	if (readers != 0)
	{
		/* a reader, or a sink that can't tell: run */
		demand->on = TRUE;
		demand->leaving = FALSE;
		return TRUE;
	}

	if (!demand->on)
		return FALSE;

	if (!demand->leaving)
	{
		demand->leaving = TRUE;
		demand->zeroSince = nowMs;
	}
	const UINT64 elapsed = (nowMs > demand->zeroSince) ? nowMs - demand->zeroSince : 0;
	if (elapsed >= SHADOW_CAMERA_DEMAND_DEBOUNCE_MS)
	{
		demand->on = FALSE;
		demand->leaving = FALSE;
		return FALSE;
	}
	*recheckMs = (UINT32)(SHADOW_CAMERA_DEMAND_DEBOUNCE_MS - elapsed);
	return TRUE;
}

SHADOW_CAMERA_STREAM_ACTION shadow_camera_stream_action(SHADOW_CAMERA_STREAM state, BOOL wanted)
{
	switch (state)
	{
		case SHADOW_CAMERA_STREAM_IDLE:
			return wanted ? SHADOW_CAMERA_STREAM_START : SHADOW_CAMERA_STREAM_NONE;
		case SHADOW_CAMERA_STREAM_STREAMING:
			return wanted ? SHADOW_CAMERA_STREAM_NONE : SHADOW_CAMERA_STREAM_STOP;
		case SHADOW_CAMERA_STREAM_STARTING:
		case SHADOW_CAMERA_STREAM_STOPPING:
		default:
			return SHADOW_CAMERA_STREAM_NONE;
	}
}
