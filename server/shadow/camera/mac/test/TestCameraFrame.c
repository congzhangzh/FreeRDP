/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: the placeholder, the raw frame conversion, the H.264 framing
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

/* Plain C, no Apple framework: runs with ctest on the macOS runner and on any other
 * machine (the code the extension and the sink helper share). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_config.h"
#include "camera_frame.h"
#include "camera_h264.h"
#include "placeholder.h"

static int g_failures = 0;

#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if (!(cond))                                                      \
		{                                                                 \
			(void)fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                                 \
		}                                                                 \
	} while (0)

/* the pixel at icon coordinates (x, y) of a width x height picture (placeholder.c's frame) */
static uint8_t at_icon(const uint8_t* gray, uint32_t width, uint32_t height, double x, double y)
{
	const double side = 0.40 * (double)((width < height) ? width : height);
	const long col = (long)(width / 2.0 + x * side);
	const long row = (long)(height / 2.0 + y * side);
	return gray[(size_t)row * width + (size_t)col];
}

static void test_placeholder(void)
{
	const uint32_t w = CAM_MAC_WIDTH;
	const uint32_t h = CAM_MAC_HEIGHT;
	uint8_t* gray = calloc((size_t)w * h, 1);
	CHECK(gray);
	if (!gray)
		return;
	CHECK(camera_placeholder_gray(gray, w, w, h));

	/* the corners and the edges: background */
	CHECK(gray[0] == CAMERA_PLACEHOLDER_BACKGROUND);
	CHECK(gray[(size_t)w * h - 1] == CAMERA_PLACEHOLDER_BACKGROUND);
	CHECK(gray[(size_t)(h / 2) * w + 2] == CAMERA_PLACEHOLDER_BACKGROUND);
	/* the body, away from the slash */
	CHECK(at_icon(gray, w, h, -0.30, 0.15) == CAMERA_PLACEHOLDER_FOREGROUND);
	/* the lens's cone */
	CHECK(at_icon(gray, w, h, 0.35, 0.0) == CAMERA_PLACEHOLDER_FOREGROUND);
	/* the slash itself, inside the body and outside it */
	CHECK(at_icon(gray, w, h, -0.10, -0.10) == CAMERA_PLACEHOLDER_FOREGROUND);
	CHECK(at_icon(gray, w, h, -0.38, -0.38) == CAMERA_PLACEHOLDER_FOREGROUND);
	/* the gap beside the slash, inside the body: background (0.045 from the line) */
	CHECK(at_icon(gray, w, h, -0.10 + 0.0318, -0.10 - 0.0318) == CAMERA_PLACEHOLDER_BACKGROUND);
	/* above the body, off the slash: background */
	CHECK(at_icon(gray, w, h, 0.0, -0.30) == CAMERA_PLACEHOLDER_BACKGROUND);

	/* nothing but the two tones and the edges between them, and a sane amount of icon */
	size_t fg = 0;
	for (size_t i = 0; i < (size_t)w * h; i++)
	{
		CHECK((gray[i] >= CAMERA_PLACEHOLDER_BACKGROUND) &&
		      (gray[i] <= CAMERA_PLACEHOLDER_FOREGROUND));
		if (gray[i] == CAMERA_PLACEHOLDER_FOREGROUND)
			fg++;
	}
	const double share = (double)fg / ((double)w * h);
	CHECK((share > 0.02) && (share < 0.10));
	printf("placeholder %ux%u: %.1f %% foreground\n", w, h, share * 100.0);

	/* NV12: video range luma, neutral chroma */
	const size_t frameSize = (size_t)w * h * 3 / 2;
	uint8_t* nv12 = malloc(frameSize);
	CHECK(nv12);
	if (nv12)
	{
		CHECK(camera_placeholder_nv12(nv12, w, nv12 + (size_t)w * h, w, w, h));
		CHECK(nv12[0] == 16 + (CAMERA_PLACEHOLDER_BACKGROUND * 219 + 127) / 255);
		bool neutral = true;
		for (size_t i = (size_t)w * h; i < frameSize; i++)
			neutral = neutral && (nv12[i] == 128);
		CHECK(neutral);
		CHECK(!camera_placeholder_nv12(nv12, w, nv12 + (size_t)w * h, w, w - 1, h));
		free(nv12);
	}

	/* BGRA: the grey in B, G and R, opaque */
	uint8_t* bgra = malloc(4ull * w * h);
	CHECK(bgra);
	if (bgra)
	{
		CHECK(camera_placeholder_bgra(bgra, 4ull * w, w, h));
		bool same = true;
		for (size_t i = 0; i < (size_t)w * h; i++)
			same = same && (bgra[4 * i] == gray[i]) && (bgra[4 * i + 1] == gray[i]) &&
			       (bgra[4 * i + 2] == gray[i]) && (bgra[4 * i + 3] == 255);
		CHECK(same);
		free(bgra);
	}
	free(gray);
}

static void test_sizes(void)
{
	size_t size = 0;
	CHECK(cam_frame_raw_size(CAM_MAC_FORMAT_NV12, 1280, 720, &size) && (size == 1382400));
	CHECK(cam_frame_raw_size(CAM_MAC_FORMAT_I420, 640, 480, &size) && (size == 460800));
	CHECK(cam_frame_raw_size(CAM_MAC_FORMAT_YUY2, 640, 480, &size) && (size == 614400));
	CHECK(cam_frame_raw_size(CAM_MAC_FORMAT_RGB24, 2, 2, &size) && (size == 12));
	CHECK(cam_frame_raw_size(CAM_MAC_FORMAT_RGB32, 2, 2, &size) && (size == 16));
	CHECK(!cam_frame_raw_size(CAM_MAC_FORMAT_NV12, 641, 480, &size));
	CHECK(!cam_frame_raw_size(CAM_MAC_FORMAT_MJPG, 640, 480, &size));
	CHECK(!cam_frame_raw_size(CAM_MAC_FORMAT_H264, 640, 480, &size));
	CHECK(!cam_frame_raw_size(CAM_MAC_FORMAT_NV12, 0, 480, &size));
}

static void test_convert(void)
{
	uint8_t y[16 * 8];
	uint8_t uv[16 * 4];

	/* NV12 4x2 as it is: copied */
	const uint8_t nv12[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 100, 200, 101, 201 };
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_NV12, 4, 2, nv12, sizeof(nv12), y, 4, uv, 4, 4, 2));
	CHECK(memcmp(y, nv12, 8) == 0);
	CHECK(memcmp(uv, &nv12[8], 4) == 0);
	/* the wrong size: refused */
	CHECK(!cam_frame_to_nv12(CAM_MAC_FORMAT_NV12, 4, 2, nv12, 11, y, 4, uv, 4, 4, 2));
	/* a compressed format: not this function's */
	CHECK(!cam_frame_to_nv12(CAM_MAC_FORMAT_MJPG, 4, 2, nv12, sizeof(nv12), y, 4, uv, 4, 4, 2));

	/* NV12 4x2 to 8x4: each pixel twice in both directions, chroma pairs too */
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_NV12, 4, 2, nv12, sizeof(nv12), y, 8, uv, 8, 8, 4));
	const uint8_t y2[32] = { 1, 1, 2, 2, 3, 3, 4, 4, 1, 1, 2, 2, 3, 3, 4, 4,
		                     5, 5, 6, 6, 7, 7, 8, 8, 5, 5, 6, 6, 7, 7, 8, 8 };
	CHECK(memcmp(y, y2, sizeof(y2)) == 0);
	const uint8_t uv2[16] = { 100, 200, 100, 200, 101, 201, 101, 201,
		                      100, 200, 100, 200, 101, 201, 101, 201 };
	CHECK(memcmp(uv, uv2, sizeof(uv2)) == 0);

	/* I420 4x2: U and V interleaved */
	const uint8_t i420[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 100, 101, 200, 201 };
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_I420, 4, 2, i420, sizeof(i420), y, 4, uv, 4, 4, 2));
	CHECK(memcmp(y, i420, 8) == 0);
	const uint8_t uvI[4] = { 100, 200, 101, 201 };
	CHECK(memcmp(uv, uvI, 4) == 0);

	/* YUY2 2x2: Y0 U Y1 V per pair; the chroma of the top row */
	const uint8_t yuy2[8] = { 10, 90, 20, 190, 30, 91, 40, 191 };
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_YUY2, 2, 2, yuy2, sizeof(yuy2), y, 2, uv, 2, 2, 2));
	const uint8_t yY[4] = { 10, 20, 30, 40 };
	CHECK(memcmp(y, yY, 4) == 0);
	CHECK((uv[0] == 90) && (uv[1] == 190));

	/* RGB32 (B, G, R, X): white, black, red, blue */
	const uint8_t rgb32[16] = { 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0 };
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_RGB32, 2, 2, rgb32, sizeof(rgb32), y, 2, uv, 2, 2, 2));
	CHECK(y[0] == 235); /* white */
	CHECK(y[1] == 16);  /* black */
	CHECK(y[2] == 82);  /* red, BT.601 */
	CHECK(y[3] == 41);  /* blue */
	CHECK((uv[0] == 128) && (uv[1] == 128)); /* the top left pixel's: white */

	/* RGB24: one red pixel row */
	const uint8_t rgb24[12] = { 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255 };
	CHECK(cam_frame_to_nv12(CAM_MAC_FORMAT_RGB24, 2, 2, rgb24, sizeof(rgb24), y, 2, uv, 2, 2, 2));
	CHECK((y[0] == 82) && (uv[0] == 90) && (uv[1] == 240));
}

static void test_h264(void)
{
	/* SPS, PPS (3-byte start code), an access unit delimiter, an IDR slice with a trailing
	 * zero before a 3-byte start code, a non-IDR slice */
	const uint8_t annexb[] = { 0,    0,    0,    1,    0x67, 0xAA, 0xBB, 0,    0,    1,
		                       0x68, 0xCC, 0,    0,    0,    1,    0x09, 0xF0, 0,    0,
		                       1,    0x65, 0x11, 0x22, 0x33, 0,    0,    0,    1,    0x41,
		                       0x44 };
	cam_h264_info info = { 0 };
	CHECK(cam_h264_scan(annexb, sizeof(annexb), &info));
	CHECK(info.nals == 5);
	CHECK(info.idr);
	CHECK(info.sps && (info.spsSize == 3) && (info.sps[0] == 0x67) && (info.sps[2] == 0xBB));
	CHECK(info.pps && (info.ppsSize == 2) && (info.pps[1] == 0xCC));
	CHECK(info.avccSize == 14);

	uint8_t avcc[32] = { 0 };
	const size_t n = cam_h264_to_avcc(annexb, sizeof(annexb), avcc, sizeof(avcc));
	const uint8_t want[14] = { 0, 0, 0, 4, 0x65, 0x11, 0x22, 0x33, 0, 0, 0, 2, 0x41, 0x44 };
	CHECK(n == sizeof(want));
	CHECK(memcmp(avcc, want, sizeof(want)) == 0);
	/* too small a buffer: nothing */
	CHECK(cam_h264_to_avcc(annexb, sizeof(annexb), avcc, 10) == 0);
	/* no start code at all: no NAL unit */
	const uint8_t junk[4] = { 1, 2, 3, 4 };
	CHECK(!cam_h264_scan(junk, sizeof(junk), &info));
}

int main(void)
{
	test_placeholder();
	test_sizes();
	test_convert();
	test_h264();
	if (g_failures)
		(void)fprintf(stderr, "TestCameraFrame: %d check(s) failed\n", g_failures);
	else
		printf("TestCameraFrame: all checks passed\n");
	return g_failures ? 1 : 0;
}
