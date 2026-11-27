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

#include <stdlib.h>

#include <winpr/assert.h>
#include <winpr/stream.h>

#include <freerdp/log.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/video.h>

#include "camera_decode.h"
#if defined(_WIN32)
#include "camera_decode_mf.h"
#endif

#define TAG SERVER_TAG("shadow.camera")

struct s_shadow_camera_decoder
{
	CAM_MEDIA_TYPE_DESCRIPTION type;
	SHADOW_CAMERA_PIXEL raw; /* the layout of a raw format, NONE for H.264 and MJPG */

	H264_CONTEXT* h264;
	BYTE* rgb; /* the H.264 decoder's output, BGRX32 */

	FREERDP_VIDEO_CONTEXT* video;
	wStream* nv12; /* the MJPG decoder's output */
#if defined(_WIN32)
	CameraMfMjpeg* mfMjpeg; /* Windows' MJPEG decoder, when there is no FFmpeg */
#endif
};

static SHADOW_CAMERA_PIXEL camera_decode_raw_layout(CAM_MEDIA_FORMAT format)
{
	switch (format)
	{
		case CAM_MEDIA_FORMAT_YUY2:
			return SHADOW_CAMERA_PIXEL_YUY2;
		case CAM_MEDIA_FORMAT_NV12:
			return SHADOW_CAMERA_PIXEL_NV12;
		case CAM_MEDIA_FORMAT_I420:
			return SHADOW_CAMERA_PIXEL_I420;
		case CAM_MEDIA_FORMAT_RGB24:
			return SHADOW_CAMERA_PIXEL_BGR24;
		case CAM_MEDIA_FORMAT_RGB32:
			return SHADOW_CAMERA_PIXEL_BGRX32;
		default:
			return SHADOW_CAMERA_PIXEL_NONE;
	}
}

void shadow_camera_decoders_probe(ShadowCameraDecoders* decoders)
{
	WINPR_ASSERT(decoders);

	H264_CONTEXT* h264 = h264_context_new(FALSE);
	decoders->h264 = (h264 != nullptr);
	h264_context_free(h264);

	decoders->mjpg =
	    freerdp_video_conversion_supported(FREERDP_VIDEO_FORMAT_MJPEG, FREERDP_VIDEO_FORMAT_NV12);
#if defined(_WIN32)
	if (!decoders->mjpg)
		decoders->mjpg = camera_mf_mjpeg_available();
#endif
}

ShadowCameraDecoder* shadow_camera_decoder_new(const CAM_MEDIA_TYPE_DESCRIPTION* type)
{
	WINPR_ASSERT(type);

	const size_t rgbSize =
	    shadow_camera_image_size(SHADOW_CAMERA_PIXEL_BGRX32, type->Width, type->Height);
	if (rgbSize == 0)
		return nullptr;

	ShadowCameraDecoder* decoder = calloc(1, sizeof(ShadowCameraDecoder));
	if (!decoder)
		return nullptr;
	decoder->type = *type;
	decoder->raw = camera_decode_raw_layout(type->Format);

	switch (type->Format)
	{
		case CAM_MEDIA_FORMAT_H264:
			decoder->h264 = h264_context_new(FALSE);
			decoder->rgb = calloc(1, rgbSize);
			if (!decoder->h264 || !decoder->rgb ||
			    !h264_context_reset(decoder->h264, type->Width, type->Height))
				goto fail;
			break;
		case CAM_MEDIA_FORMAT_MJPG:
		{
			const size_t nv12Size =
			    shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, type->Width, type->Height);
			if (nv12Size == 0)
				goto fail;
			decoder->nv12 = Stream_New(nullptr, nv12Size);
			if (!decoder->nv12)
				goto fail;
#if defined(_WIN32)
			if (!freerdp_video_conversion_supported(FREERDP_VIDEO_FORMAT_MJPEG,
			                                        FREERDP_VIDEO_FORMAT_NV12))
			{
				decoder->mfMjpeg = camera_mf_mjpeg_new(type->Width, type->Height);
				if (!decoder->mfMjpeg)
					goto fail;
				break;
			}
#endif
			decoder->video = freerdp_video_context_new(type->Width, type->Height);
			if (!decoder->video)
				goto fail;
		}
		break;
		default:
			if (decoder->raw == SHADOW_CAMERA_PIXEL_NONE)
				goto fail;
			if (shadow_camera_image_size(decoder->raw, type->Width, type->Height) == 0)
				goto fail;
			break;
	}
	return decoder;

fail:
	WLog_ERR(TAG, "camera: no decoder for %s %" PRIu32 "x%" PRIu32,
	         shadow_camera_format_name(type->Format), type->Width, type->Height);
	shadow_camera_decoder_free(decoder);
	return nullptr;
}

void shadow_camera_decoder_free(ShadowCameraDecoder* decoder)
{
	if (!decoder)
		return;
	h264_context_free(decoder->h264);
	free(decoder->rgb);
	freerdp_video_context_free(decoder->video);
	Stream_Free(decoder->nv12, TRUE);
#if defined(_WIN32)
	camera_mf_mjpeg_free(decoder->mfMjpeg);
#endif
	free(decoder);
}

static BOOL camera_decode_h264(ShadowCameraDecoder* decoder, const BYTE* sample, size_t size,
                               ShadowCameraImage* image, BOOL* picture)
{
	const UINT32 width = decoder->type.Width;
	const UINT32 height = decoder->type.Height;
	if ((size == 0) || (size > UINT32_MAX) || (width > UINT16_MAX) || (height > UINT16_MAX))
		return FALSE;

	const RECTANGLE_16 rect = { 0, 0, (UINT16)width, (UINT16)height };
	const INT32 rc = avc420_decompress(decoder->h264, sample, (UINT32)size, decoder->rgb,
	                                   PIXEL_FORMAT_BGRX32, width * 4, width, height, &rect, 1);
	if (rc < 0)
	{
		WLog_DBG(TAG, "camera: H.264 sample of %" PRIuz " bytes not decoded (%" PRId32 ")", size,
		         rc);
		return FALSE;
	}

	FREERDP_DECODE_RESULT result = WINPR_C_ARRAY_INIT;
	if (h264_context_get_last_decode(decoder->h264, &result) &&
	    (result.status == FREERDP_DECODE_NO_PICTURE))
	{
		*picture = FALSE;
		return TRUE;
	}

	*picture = TRUE;
	return shadow_camera_image_wrap(
	    image, SHADOW_CAMERA_PIXEL_BGRX32_CODEC, width, height, decoder->rgb,
	    shadow_camera_image_size(SHADOW_CAMERA_PIXEL_BGRX32, width, height));
}

static BOOL camera_decode_mjpg(ShadowCameraDecoder* decoder, const BYTE* sample, size_t size,
                               ShadowCameraImage* image, BOOL* picture)
{
	if (size == 0)
		return FALSE;

	Stream_ResetPosition(decoder->nv12);
#if defined(_WIN32)
	if (decoder->mfMjpeg)
	{
		if (!camera_mf_mjpeg_decode(decoder->mfMjpeg, sample, size, Stream_Buffer(decoder->nv12)))
			return FALSE;
		*picture = TRUE;
		return shadow_camera_image_wrap(image, SHADOW_CAMERA_PIXEL_NV12, decoder->type.Width,
		                                decoder->type.Height, Stream_Buffer(decoder->nv12),
		                                Stream_Capacity(decoder->nv12));
	}
#endif
	if (!freerdp_video_sample_convert(decoder->video, FREERDP_VIDEO_FORMAT_MJPEG, sample, size,
	                                  FREERDP_VIDEO_FORMAT_NV12, decoder->nv12))
		return FALSE;

	*picture = TRUE;
	return shadow_camera_image_wrap(image, SHADOW_CAMERA_PIXEL_NV12, decoder->type.Width,
	                                decoder->type.Height, Stream_Buffer(decoder->nv12),
	                                Stream_GetPosition(decoder->nv12));
}

BOOL shadow_camera_decoder_decode(ShadowCameraDecoder* decoder, const BYTE* sample, size_t size,
                                  ShadowCameraImage* image, BOOL* picture)
{
	WINPR_ASSERT(decoder);
	WINPR_ASSERT(image);
	WINPR_ASSERT(picture);

	*picture = FALSE;
	if (!sample)
		return FALSE;

	switch (decoder->type.Format)
	{
		case CAM_MEDIA_FORMAT_H264:
			return camera_decode_h264(decoder, sample, size, image, picture);
		case CAM_MEDIA_FORMAT_MJPG:
			return camera_decode_mjpg(decoder, sample, size, image, picture);
		default:
			break;
	}

	/* raw: the sample is the frame, never read past what it holds */
	if (!shadow_camera_image_wrap(image, decoder->raw, decoder->type.Width, decoder->type.Height,
	                              sample, size))
	{
		WLog_DBG(TAG,
		         "camera: %s sample of %" PRIuz " bytes is shorter than a %" PRIu32 "x%" PRIu32
		         " frame",
		         shadow_camera_format_name(decoder->type.Format), size, decoder->type.Width,
		         decoder->type.Height);
		return FALSE;
	}
	image->bottomUp =
	    (decoder->type.Flags & CAM_MEDIA_TYPE_DESCRIPTION_FLAG_BottomUpImage) ? TRUE : FALSE;
	*picture = TRUE;
	return TRUE;
}
