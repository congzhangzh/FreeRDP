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

#include <string.h>
#include <stdlib.h>

#include <winpr/assert.h>
#include <winpr/cast.h>

#include "camera_image.h"

/* video in the limited range of BT.601: black is Y 16, U and V 128 */
#define CAMERA_BLACK_Y 16
#define CAMERA_NEUTRAL_UV 128

/* the longest side an image may have here; a client's media type is checked against it */
#define CAMERA_MAX_DIMENSION 8192u

const char* shadow_camera_pixel_name(SHADOW_CAMERA_PIXEL format)
{
	switch (format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
			return "I420";
		case SHADOW_CAMERA_PIXEL_NV12:
			return "NV12";
		case SHADOW_CAMERA_PIXEL_YUY2:
			return "YUY2";
		case SHADOW_CAMERA_PIXEL_BGR24:
			return "RGB24";
		case SHADOW_CAMERA_PIXEL_BGRX32:
			return "RGB32";
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
			return "RGB32 (H.264 decoder)";
		case SHADOW_CAMERA_PIXEL_NONE:
		default:
			return "none";
	}
}

size_t shadow_camera_image_size(SHADOW_CAMERA_PIXEL format, UINT32 width, UINT32 height)
{
	if ((width == 0) || (height == 0) || (width > CAMERA_MAX_DIMENSION) ||
	    (height > CAMERA_MAX_DIMENSION))
		return 0;

	const size_t pixels = 1ull * width * height;
	switch (format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
		case SHADOW_CAMERA_PIXEL_NV12:
			if ((width % 2) || (height % 2))
				return 0;
			return pixels + pixels / 2;
		case SHADOW_CAMERA_PIXEL_YUY2:
			if (width % 2)
				return 0;
			return pixels * 2;
		case SHADOW_CAMERA_PIXEL_BGR24:
			return pixels * 3;
		case SHADOW_CAMERA_PIXEL_BGRX32:
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
			return pixels * 4;
		case SHADOW_CAMERA_PIXEL_NONE:
		default:
			return 0;
	}
}

BOOL shadow_camera_image_wrap(ShadowCameraImage* image, SHADOW_CAMERA_PIXEL format, UINT32 width,
                              UINT32 height, const BYTE* data, size_t size)
{
	WINPR_ASSERT(image);

	const size_t needed = shadow_camera_image_size(format, width, height);
	if ((needed == 0) || !data || (size < needed))
		return FALSE;

	BYTE* base = WINPR_CAST_CONST_PTR_AWAY(data, BYTE*);
	const size_t luma = 1ull * width * height;
	*image = (ShadowCameraImage){ .format = format, .width = width, .height = height };
	switch (format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
			image->planes[0] = base;
			image->planes[1] = base + luma;
			image->planes[2] = base + luma + luma / 4;
			image->strides[0] = width;
			image->strides[1] = width / 2;
			image->strides[2] = width / 2;
			break;
		case SHADOW_CAMERA_PIXEL_NV12:
			image->planes[0] = base;
			image->planes[1] = base + luma;
			image->strides[0] = width;
			image->strides[1] = width;
			break;
		case SHADOW_CAMERA_PIXEL_YUY2:
			image->planes[0] = base;
			image->strides[0] = width * 2;
			break;
		case SHADOW_CAMERA_PIXEL_BGR24:
			image->planes[0] = base;
			image->strides[0] = width * 3;
			break;
		case SHADOW_CAMERA_PIXEL_BGRX32:
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
			image->planes[0] = base;
			image->strides[0] = width * 4;
			break;
		case SHADOW_CAMERA_PIXEL_NONE:
		default:
			return FALSE;
	}
	return TRUE;
}

static BOOL camera_image_is_yuv_output(const ShadowCameraImage* dst)
{
	WINPR_ASSERT(dst);

	if (!dst->planes[0] || (dst->width == 0) || (dst->height == 0) || (dst->width % 2))
		return FALSE;
	switch (dst->format)
	{
		case SHADOW_CAMERA_PIXEL_YUY2:
			return TRUE;
		case SHADOW_CAMERA_PIXEL_NV12:
			return dst->planes[1] && ((dst->height % 2) == 0);
		case SHADOW_CAMERA_PIXEL_I420:
			return dst->planes[1] && dst->planes[2] && ((dst->height % 2) == 0);
		default:
			return FALSE;
	}
}

void shadow_camera_image_fill_black(ShadowCameraImage* dst)
{
	WINPR_ASSERT(dst);

	if (!camera_image_is_yuv_output(dst))
		return;

	for (UINT32 y = 0; y < dst->height; y++)
	{
		BYTE* row = dst->planes[0] + 1ull * y * dst->strides[0];
		if (dst->format == SHADOW_CAMERA_PIXEL_YUY2)
		{
			for (UINT32 x = 0; x < dst->width; x += 2)
			{
				row[2 * x + 0] = CAMERA_BLACK_Y;
				row[2 * x + 1] = CAMERA_NEUTRAL_UV;
				row[2 * x + 2] = CAMERA_BLACK_Y;
				row[2 * x + 3] = CAMERA_NEUTRAL_UV;
			}
		}
		else
			memset(row, CAMERA_BLACK_Y, dst->width);
	}

	if (dst->format == SHADOW_CAMERA_PIXEL_NV12)
	{
		for (UINT32 y = 0; y < dst->height / 2; y++)
			memset(dst->planes[1] + 1ull * y * dst->strides[1], CAMERA_NEUTRAL_UV, dst->width);
	}
	else if (dst->format == SHADOW_CAMERA_PIXEL_I420)
	{
		for (size_t p = 1; p < 3; p++)
		{
			for (UINT32 y = 0; y < dst->height / 2; y++)
				memset(dst->planes[p] + 1ull * y * dst->strides[p], CAMERA_NEUTRAL_UV,
				       dst->width / 2);
		}
	}
}

/* BT.601, limited range, integer: the offsets keep every sum positive before the shift */
static BYTE camera_rgb_y(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

static BYTE camera_rgb_u(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, (-38 * r - 74 * g + 112 * b + 32896) >> 8);
}

static BYTE camera_rgb_v(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, (112 * r - 94 * g - 18 * b + 32896) >> 8);
}

/* the inverse of FreeRDP's codec YUV to RGB (full range BT.709, prim_internal.h RGB2Y/U/V) */
static BYTE camera_codec_y(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, (54 * r + 183 * g + 18 * b) >> 8);
}

static BYTE camera_codec_u(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, (-29 * r - 99 * g + 128 * b + 32768) >> 8);
}

static BYTE camera_codec_v(INT32 r, INT32 g, INT32 b)
{
	return WINPR_ASSERTING_INT_CAST(BYTE, (128 * r - 116 * g - 12 * b + 32768) >> 8);
}

static const BYTE* camera_rgb_pixel(const ShadowCameraImage* src, UINT32 x, UINT32 y)
{
	const UINT32 row = src->bottomUp ? (src->height - 1 - y) : y;
	const size_t bpp = (src->format == SHADOW_CAMERA_PIXEL_BGR24) ? 3 : 4;
	return src->planes[0] + 1ull * row * src->strides[0] + bpp * x;
}

static BYTE camera_sample_y(const ShadowCameraImage* src, UINT32 x, UINT32 y)
{
	switch (src->format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
		case SHADOW_CAMERA_PIXEL_NV12:
			return src->planes[0][1ull * y * src->strides[0] + x];
		case SHADOW_CAMERA_PIXEL_YUY2:
			return src->planes[0][1ull * y * src->strides[0] + 2ull * x];
		case SHADOW_CAMERA_PIXEL_BGR24:
		case SHADOW_CAMERA_PIXEL_BGRX32:
		{
			const BYTE* p = camera_rgb_pixel(src, x, y);
			return camera_rgb_y(p[2], p[1], p[0]);
		}
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
		{
			const BYTE* p = camera_rgb_pixel(src, x, y);
			return camera_codec_y(p[2], p[1], p[0]);
		}
		default:
			return CAMERA_BLACK_Y;
	}
}

static void camera_sample_uv(const ShadowCameraImage* src, UINT32 x, UINT32 y, BYTE* u, BYTE* v)
{
	switch (src->format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
			*u = src->planes[1][1ull * (y / 2) * src->strides[1] + x / 2];
			*v = src->planes[2][1ull * (y / 2) * src->strides[2] + x / 2];
			break;
		case SHADOW_CAMERA_PIXEL_NV12:
		{
			const BYTE* p = &src->planes[1][1ull * (y / 2) * src->strides[1] + (x & ~1u)];
			*u = p[0];
			*v = p[1];
		}
		break;
		case SHADOW_CAMERA_PIXEL_YUY2:
		{
			const BYTE* p = &src->planes[0][1ull * y * src->strides[0] + 2ull * (x & ~1u)];
			*u = p[1];
			*v = p[3];
		}
		break;
		case SHADOW_CAMERA_PIXEL_BGR24:
		case SHADOW_CAMERA_PIXEL_BGRX32:
		{
			const BYTE* p = camera_rgb_pixel(src, x, y);
			*u = camera_rgb_u(p[2], p[1], p[0]);
			*v = camera_rgb_v(p[2], p[1], p[0]);
		}
		break;
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
		{
			const BYTE* p = camera_rgb_pixel(src, x, y);
			*u = camera_codec_u(p[2], p[1], p[0]);
			*v = camera_codec_v(p[2], p[1], p[0]);
		}
		break;
		default:
			*u = CAMERA_NEUTRAL_UV;
			*v = CAMERA_NEUTRAL_UV;
			break;
	}
}

static BOOL camera_image_is_source(const ShadowCameraImage* src)
{
	WINPR_ASSERT(src);

	if ((src->width == 0) || (src->height == 0) || !src->planes[0])
		return FALSE;
	switch (src->format)
	{
		case SHADOW_CAMERA_PIXEL_I420:
			return src->planes[1] && src->planes[2] && ((src->width % 2) == 0) &&
			       ((src->height % 2) == 0);
		case SHADOW_CAMERA_PIXEL_NV12:
			return src->planes[1] && ((src->width % 2) == 0) && ((src->height % 2) == 0);
		case SHADOW_CAMERA_PIXEL_YUY2:
			return (src->width % 2) == 0;
		case SHADOW_CAMERA_PIXEL_BGR24:
		case SHADOW_CAMERA_PIXEL_BGRX32:
		case SHADOW_CAMERA_PIXEL_BGRX32_CODEC:
			return TRUE;
		default:
			return FALSE;
	}
}

BOOL shadow_camera_image_letterbox(const ShadowCameraImage* src, ShadowCameraImage* dst)
{
	WINPR_ASSERT(src);
	WINPR_ASSERT(dst);

	if (!camera_image_is_source(src) || !camera_image_is_yuv_output(dst))
		return FALSE;

	/* the largest even rectangle of src's aspect ratio that fits, centred on even offsets */
	UINT32 outW = 0;
	UINT32 outH = 0;
	const UINT64 fullHeightWidth = 1ull * src->width * dst->height / src->height;
	if (fullHeightWidth <= dst->width)
	{
		outW = (UINT32)fullHeightWidth;
		outH = dst->height;
	}
	else
	{
		outW = dst->width;
		outH = (UINT32)(1ull * src->height * dst->width / src->width);
	}
	outW &= ~1u;
	outH &= ~1u;
	const UINT32 x0 = ((dst->width - outW) / 2) & ~1u;
	const UINT32 y0 = ((dst->height - outH) / 2) & ~1u;

	shadow_camera_image_fill_black(dst);
	if ((outW == 0) || (outH == 0))
		return TRUE;

	/* the source column of each output column, at the centre of the output pixel */
	UINT32* columns = calloc(outW, sizeof(UINT32));
	if (!columns)
		return FALSE;
	for (UINT32 i = 0; i < outW; i++)
		columns[i] = (UINT32)((2ull * i + 1) * src->width / (2ull * outW));

	for (UINT32 j = 0; j < outH; j++)
	{
		const UINT32 sy = (UINT32)((2ull * j + 1) * src->height / (2ull * outH));
		const UINT32 dy = y0 + j;

		if (dst->format == SHADOW_CAMERA_PIXEL_YUY2)
		{
			BYTE* row = dst->planes[0] + 1ull * dy * dst->strides[0] + 2ull * x0;
			for (UINT32 i = 0; i < outW; i += 2)
			{
				BYTE u = 0;
				BYTE v = 0;
				camera_sample_uv(src, columns[i], sy, &u, &v);
				row[2 * i + 0] = camera_sample_y(src, columns[i], sy);
				row[2 * i + 1] = u;
				row[2 * i + 2] = camera_sample_y(src, columns[i + 1], sy);
				row[2 * i + 3] = v;
			}
			continue;
		}

		BYTE* row = dst->planes[0] + 1ull * dy * dst->strides[0] + x0;
		for (UINT32 i = 0; i < outW; i++)
			row[i] = camera_sample_y(src, columns[i], sy);

		if (j % 2)
			continue;
		for (UINT32 i = 0; i < outW; i += 2)
		{
			BYTE u = 0;
			BYTE v = 0;
			camera_sample_uv(src, columns[i], sy, &u, &v);
			if (dst->format == SHADOW_CAMERA_PIXEL_NV12)
			{
				BYTE* p = dst->planes[1] + 1ull * (dy / 2) * dst->strides[1] + x0 + i;
				p[0] = u;
				p[1] = v;
			}
			else
			{
				dst->planes[1][1ull * (dy / 2) * dst->strides[1] + (x0 + i) / 2] = u;
				dst->planes[2][1ull * (dy / 2) * dst->strides[2] + (x0 + i) / 2] = v;
			}
		}
	}

	free(columns);
	return TRUE;
}
