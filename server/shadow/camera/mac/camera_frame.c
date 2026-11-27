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

#include <string.h>

#include "camera_config.h"
#include "camera_frame.h"

static bool mul_size(size_t a, size_t b, size_t* out)
{
	if (a && (b > SIZE_MAX / a))
		return false;
	*out = a * b;
	return true;
}

bool cam_frame_raw_size(uint32_t format, uint32_t width, uint32_t height, size_t* size)
{
	size_t pixels = 0;
	if (!size || (width == 0) || (height == 0) || !mul_size(width, height, &pixels))
		return false;
	switch (format)
	{
		case CAM_MAC_FORMAT_NV12:
		case CAM_MAC_FORMAT_I420:
			if ((width % 2) || (height % 2))
				return false;
			*size = pixels + pixels / 2;
			return true;
		case CAM_MAC_FORMAT_YUY2:
			if (width % 2)
				return false;
			return mul_size(pixels, 2, size);
		case CAM_MAC_FORMAT_RGB24:
			return mul_size(pixels, 3, size);
		case CAM_MAC_FORMAT_RGB32:
			return mul_size(pixels, 4, size);
		default:
			return false;
	}
}

/* BT.601, video range, integer (the usual 8-bit coefficients) */
static uint8_t rgb_y(int r, int g, int b)
{
	return (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}
static uint8_t rgb_u(int r, int g, int b)
{
	return (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}
static uint8_t rgb_v(int r, int g, int b)
{
	return (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

typedef struct
{
	uint32_t format;
	uint32_t width;
	uint32_t height;
	const uint8_t* data;
} cam_src;

static uint8_t src_luma(const cam_src* s, uint32_t x, uint32_t y)
{
	const size_t w = s->width;
	switch (s->format)
	{
		case CAM_MAC_FORMAT_NV12:
		case CAM_MAC_FORMAT_I420:
			return s->data[(size_t)y * w + x];
		case CAM_MAC_FORMAT_YUY2:
			return s->data[((size_t)y * w + x) * 2];
		case CAM_MAC_FORMAT_RGB24:
		{
			const uint8_t* p = &s->data[((size_t)y * w + x) * 3];
			return rgb_y(p[2], p[1], p[0]);
		}
		case CAM_MAC_FORMAT_RGB32:
		{
			const uint8_t* p = &s->data[((size_t)y * w + x) * 4];
			return rgb_y(p[2], p[1], p[0]);
		}
		default:
			return 16;
	}
}

/* the chroma that covers source pixel (x, y) */
static void src_chroma(const cam_src* s, uint32_t x, uint32_t y, uint8_t* u, uint8_t* v)
{
	const size_t w = s->width;
	const size_t h = s->height;
	switch (s->format)
	{
		case CAM_MAC_FORMAT_NV12:
		{
			const uint8_t* p = &s->data[w * h + (size_t)(y / 2) * w + (x & ~1u)];
			*u = p[0];
			*v = p[1];
			return;
		}
		case CAM_MAC_FORMAT_I420:
		{
			const size_t c = (size_t)(y / 2) * (w / 2) + x / 2;
			*u = s->data[w * h + c];
			*v = s->data[w * h + (w / 2) * (h / 2) + c];
			return;
		}
		case CAM_MAC_FORMAT_YUY2:
		{
			/* Y0 U Y1 V per two pixels */
			const uint8_t* p = &s->data[((size_t)y * w + (x & ~1u)) * 2];
			*u = p[1];
			*v = p[3];
			return;
		}
		case CAM_MAC_FORMAT_RGB24:
		case CAM_MAC_FORMAT_RGB32:
		{
			const size_t bpp = (s->format == CAM_MAC_FORMAT_RGB24) ? 3 : 4;
			const uint8_t* p = &s->data[((size_t)y * w + x) * bpp];
			*u = rgb_u(p[2], p[1], p[0]);
			*v = rgb_v(p[2], p[1], p[0]);
			return;
		}
		default:
			*u = 128;
			*v = 128;
			return;
	}
}

/* same size NV12 or I420: rows copied, no per pixel work */
static bool copy_planar(const cam_src* s, uint8_t* dstY, size_t dstYStride, uint8_t* dstUV,
                        size_t dstUVStride)
{
	const size_t w = s->width;
	const size_t h = s->height;
	for (size_t row = 0; row < h; row++)
		memcpy(&dstY[row * dstYStride], &s->data[row * w], w);
	const uint8_t* chroma = &s->data[w * h];
	for (size_t row = 0; row < h / 2; row++)
	{
		uint8_t* out = &dstUV[row * dstUVStride];
		if (s->format == CAM_MAC_FORMAT_NV12)
			memcpy(out, &chroma[row * w], w);
		else
		{
			const uint8_t* u = &chroma[row * (w / 2)];
			const uint8_t* v = &chroma[(w / 2) * (h / 2) + row * (w / 2)];
			for (size_t x = 0; x < w / 2; x++)
			{
				out[2 * x] = u[x];
				out[2 * x + 1] = v[x];
			}
		}
	}
	return true;
}

bool cam_frame_to_nv12(uint32_t format, uint32_t srcWidth, uint32_t srcHeight, const uint8_t* src,
                       size_t size, uint8_t* dstY, size_t dstYStride, uint8_t* dstUV,
                       size_t dstUVStride, uint32_t dstWidth, uint32_t dstHeight)
{
	size_t expected = 0;
	if (!src || !dstY || !dstUV || !cam_frame_raw_size(format, srcWidth, srcHeight, &expected))
		return false;
	if ((size != expected) || (dstWidth == 0) || (dstHeight == 0) || (dstWidth % 2) ||
	    (dstHeight % 2) || (dstYStride < dstWidth) || (dstUVStride < dstWidth))
		return false;

	const cam_src s = { format, srcWidth, srcHeight, src };
	if ((srcWidth == dstWidth) && (srcHeight == dstHeight) &&
	    ((format == CAM_MAC_FORMAT_NV12) || (format == CAM_MAC_FORMAT_I420)))
		return copy_planar(&s, dstY, dstYStride, dstUV, dstUVStride);

	for (uint32_t y = 0; y < dstHeight; y++)
	{
		const uint32_t sy = (uint32_t)(((uint64_t)y * srcHeight) / dstHeight);
		uint8_t* out = &dstY[(size_t)y * dstYStride];
		for (uint32_t x = 0; x < dstWidth; x++)
			out[x] = src_luma(&s, (uint32_t)(((uint64_t)x * srcWidth) / dstWidth), sy);
	}
	for (uint32_t cy = 0; cy < dstHeight / 2; cy++)
	{
		const uint32_t sy = (uint32_t)(((uint64_t)(2 * cy) * srcHeight) / dstHeight);
		uint8_t* out = &dstUV[(size_t)cy * dstUVStride];
		for (uint32_t cx = 0; cx < dstWidth / 2; cx++)
		{
			const uint32_t sx = (uint32_t)(((uint64_t)(2 * cx) * srcWidth) / dstWidth);
			src_chroma(&s, sx, sy, &out[2 * cx], &out[2 * cx + 1]);
		}
	}
	return true;
}

bool cam_jpeg_frame_size(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height)
{
	if (!data || !width || !height || (size < 4) || (data[0] != 0xFF) || (data[1] != 0xD8))
		return false;

	size_t pos = 2;
	while (pos + 4 <= size)
	{
		if (data[pos] != 0xFF)
			return false;
		const uint8_t marker = data[pos + 1];
		if (marker == 0xFF) /* fill byte */
		{
			pos++;
			continue;
		}
		/* standalone markers carry no length */
		if ((marker == 0x01) || ((marker >= 0xD0) && (marker <= 0xD7)))
		{
			pos += 2;
			continue;
		}
		if ((marker == 0xD9) || (marker == 0xDA)) /* EOI or SOS before any frame header */
			return false;
		const size_t length = ((size_t)data[pos + 2] << 8) | data[pos + 3];
		if ((length < 2) || (length > size - pos - 2))
			return false;
		/* SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC) */
		if ((marker >= 0xC0) && (marker <= 0xCF) && (marker != 0xC4) && (marker != 0xC8) &&
		    (marker != 0xCC))
		{
			if (length < 7)
				return false;
			*height = ((uint32_t)data[pos + 5] << 8) | data[pos + 6];
			*width = ((uint32_t)data[pos + 7] << 8) | data[pos + 8];
			return (*width > 0) && (*height > 0);
		}
		pos += 2 + length;
	}
	return false;
}
