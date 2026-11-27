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

#include <string.h>

#include "placeholder.h"

/* The icon lives in a square of side s (40 % of the shorter side), centred; inside it the
 * coordinates run from -0.5 to 0.5, y down. A video camera: a rounded body on the left and
 * the lens's cone on the right; a slash from the top left to the bottom right, with a gap of
 * the background's tone on both sides of it, as the system icons draw it. */
#define ICON_SCALE 0.40
#define SUPERSAMPLE 3

#define BODY_X0 (-0.40)
#define BODY_X1 0.12
#define BODY_Y 0.20 /* half height */
#define BODY_R 0.06 /* corner radius */
#define CONE_X0 0.16
#define CONE_X1 0.40
#define CONE_Y0 0.07 /* half height at CONE_X0 */
#define CONE_Y1 0.20 /* half height at CONE_X1 */
#define SLASH_END 0.42
#define SLASH_HALF 0.030 /* half the line's width */
#define SLASH_GAP 0.035  /* the background around it */

static bool in_body(double x, double y)
{
	if ((x < BODY_X0) || (x > BODY_X1) || (y < -BODY_Y) || (y > BODY_Y))
		return false;
	/* the corners: inside the circle of radius BODY_R at each inner corner */
	const double cx = (x < BODY_X0 + BODY_R) ? (BODY_X0 + BODY_R)
	                  : (x > BODY_X1 - BODY_R) ? (BODY_X1 - BODY_R)
	                                           : x;
	const double cy = (y < -BODY_Y + BODY_R) ? (-BODY_Y + BODY_R)
	                  : (y > BODY_Y - BODY_R) ? (BODY_Y - BODY_R)
	                                          : y;
	const double dx = x - cx;
	const double dy = y - cy;
	return (dx * dx + dy * dy) <= BODY_R * BODY_R;
}

static bool in_cone(double x, double y)
{
	if ((x < CONE_X0) || (x > CONE_X1))
		return false;
	const double half = CONE_Y0 + (CONE_Y1 - CONE_Y0) * (x - CONE_X0) / (CONE_X1 - CONE_X0);
	return (y >= -half) && (y <= half);
}

/* the squared distance to the slash's segment, from (-SLASH_END, -SLASH_END) to
 * (SLASH_END, SLASH_END): squares, so the library needs no libm */
static double slash_distance2(double x, double y)
{
	const double ax = -SLASH_END;
	const double ay = -SLASH_END;
	const double dx = 2.0 * SLASH_END;
	const double dy = 2.0 * SLASH_END;
	double t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
	if (t < 0.0)
		t = 0.0;
	else if (t > 1.0)
		t = 1.0;
	const double px = ax + t * dx - x;
	const double py = ay + t * dy - y;
	return px * px + py * py;
}

/* foreground at (x, y), in icon coordinates */
static bool icon_at(double x, double y)
{
	const double d2 = slash_distance2(x, y);
	if (d2 <= SLASH_HALF * SLASH_HALF)
		return true;
	if (d2 <= (SLASH_HALF + SLASH_GAP) * (SLASH_HALF + SLASH_GAP))
		return false;
	return in_body(x, y) || in_cone(x, y);
}

bool camera_placeholder_gray(uint8_t* dst, size_t stride, uint32_t width, uint32_t height)
{
	if (!dst || (width == 0) || (height == 0) || (stride < width))
		return false;

	const double side = ICON_SCALE * (double)((width < height) ? width : height);
	const double cx = (double)width / 2.0;
	const double cy = (double)height / 2.0;
	const int samples = SUPERSAMPLE * SUPERSAMPLE;
	const int range = CAMERA_PLACEHOLDER_FOREGROUND - CAMERA_PLACEHOLDER_BACKGROUND;

	/* the icon's box in pixels, one pixel more on each side for the edges */
	const double half = side / 2.0 + 1.0;
	const int64_t bx0 = (int64_t)(cx - half);
	const int64_t bx1 = (int64_t)(cx + half) + 1;
	const int64_t by0 = (int64_t)(cy - half);
	const int64_t by1 = (int64_t)(cy + half) + 1;

	for (uint32_t row = 0; row < height; row++)
	{
		uint8_t* line = &dst[(size_t)row * stride];
		memset(line, CAMERA_PLACEHOLDER_BACKGROUND, width);
		if (((int64_t)row < by0) || ((int64_t)row > by1))
			continue;
		for (uint32_t col = 0; col < width; col++)
		{
			if (((int64_t)col < bx0) || ((int64_t)col > bx1))
				continue;
			int hits = 0;
			for (int sy = 0; sy < SUPERSAMPLE; sy++)
			{
				const double py = ((double)row + (sy + 0.5) / SUPERSAMPLE - cy) / side;
				for (int sx = 0; sx < SUPERSAMPLE; sx++)
				{
					const double px = ((double)col + (sx + 0.5) / SUPERSAMPLE - cx) / side;
					if (icon_at(px, py))
						hits++;
				}
			}
			if (hits)
				line[col] =
				    (uint8_t)(CAMERA_PLACEHOLDER_BACKGROUND + (range * hits + samples / 2) / samples);
		}
	}
	return true;
}

/* full range grey to video range luma (16..235) */
static uint8_t video_luma(uint8_t gray)
{
	return (uint8_t)(16 + (gray * 219 + 127) / 255);
}

static bool luma_plane(uint8_t* y, size_t yStride, uint32_t width, uint32_t height)
{
	if (!camera_placeholder_gray(y, yStride, width, height))
		return false;
	for (uint32_t row = 0; row < height; row++)
	{
		uint8_t* line = &y[(size_t)row * yStride];
		for (uint32_t col = 0; col < width; col++)
			line[col] = video_luma(line[col]);
	}
	return true;
}

bool camera_placeholder_nv12(uint8_t* y, size_t yStride, uint8_t* uv, size_t uvStride,
                             uint32_t width, uint32_t height)
{
	if (!uv || (width % 2) || (height % 2) || (uvStride < width))
		return false;
	if (!luma_plane(y, yStride, width, height))
		return false;
	for (uint32_t row = 0; row < height / 2; row++)
		memset(&uv[(size_t)row * uvStride], 128, width);
	return true;
}

bool camera_placeholder_i420(uint8_t* y, size_t yStride, uint8_t* u, size_t uStride, uint8_t* v,
                             size_t vStride, uint32_t width, uint32_t height)
{
	if (!u || !v || (width % 2) || (height % 2) || (uStride < width / 2) || (vStride < width / 2))
		return false;
	if (!luma_plane(y, yStride, width, height))
		return false;
	for (uint32_t row = 0; row < height / 2; row++)
	{
		memset(&u[(size_t)row * uStride], 128, width / 2);
		memset(&v[(size_t)row * vStride], 128, width / 2);
	}
	return true;
}

bool camera_placeholder_bgra(uint8_t* dst, size_t stride, uint32_t width, uint32_t height)
{
	if (!dst || (stride < 4ull * width))
		return false;
	/* the grey into the first quarter of each row, then spread to B, G, R, A from the right:
	 * pixel x goes to 4x >= x, so nothing is overwritten before it is read */
	if (!camera_placeholder_gray(dst, stride, width, height))
		return false;
	for (uint32_t row = 0; row < height; row++)
	{
		uint8_t* line = &dst[(size_t)row * stride];
		for (uint32_t col = width; col-- > 0;)
		{
			const uint8_t g = line[col];
			line[4ull * col + 0] = g;
			line[4ull * col + 1] = g;
			line[4ull * col + 2] = g;
			line[4ull * col + 3] = 255;
		}
	}
	return true;
}

bool camera_placeholder_yuyv(uint8_t* dst, size_t stride, uint32_t width, uint32_t height)
{
	if (!dst || (width % 2) || (stride < 2ull * width))
		return false;
	/* the luma into the first half of each row, then spread to Y, U/V from the right: pixel x
	 * goes to 2x >= x, so nothing is overwritten before it is read */
	if (!luma_plane(dst, stride, width, height))
		return false;
	for (uint32_t row = 0; row < height; row++)
	{
		uint8_t* line = &dst[(size_t)row * stride];
		for (uint32_t col = width; col-- > 0;)
		{
			line[2ull * col + 0] = line[col];
			line[2ull * col + 1] = 128;
		}
	}
	return true;
}
