/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: raw samples and their scaling into the camera
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>

#include "camera_decode.h"
#include "camera_image.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

static int test_sizes(void)
{
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_YUY2, 640, 480) == 640ull * 480 * 2);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, 640, 480) == 640ull * 480 * 3 / 2);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_I420, 640, 480) == 640ull * 480 * 3 / 2);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_BGR24, 3, 1) == 9);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_BGRX32, 3, 1) == 12);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_YUY2, 641, 480) == 0);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, 640, 481) == 0);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_YUY2, 0, 480) == 0);
	CHECK(shadow_camera_image_size(SHADOW_CAMERA_PIXEL_YUY2, 100000, 2) == 0);

	/* a sample shorter than its frame is refused, never over-read */
	BYTE small[16] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage image = WINPR_C_ARRAY_INIT;
	CHECK(!shadow_camera_image_wrap(&image, SHADOW_CAMERA_PIXEL_YUY2, 4, 4, small,
	                                sizeof(small) - 1));
	CHECK(shadow_camera_image_wrap(&image, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, small, sizeof(small)));
	CHECK(!shadow_camera_image_wrap(&image, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, nullptr, 16));
	return 0;
}

static int test_pillarbox_yuy2(void)
{
	/* a 4x2 YUY2 source into an 8x2 YUY2 camera: 2 black columns on each side */
	const BYTE src[] = { 50, 90, 60, 170, 70, 91, 80, 171, 100, 92, 110, 172, 120, 93, 130, 173 };
	BYTE out[8 * 2 * 2] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage s = WINPR_C_ARRAY_INIT;
	ShadowCameraImage d = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&s, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, src, sizeof(src)));
	CHECK(shadow_camera_image_wrap(&d, SHADOW_CAMERA_PIXEL_YUY2, 8, 2, out, sizeof(out)));
	CHECK(shadow_camera_image_letterbox(&s, &d));

	const BYTE row0[] = { 16, 128, 16, 128, 50, 90, 60, 170, 70, 91, 80, 171, 16, 128, 16, 128 };
	const BYTE row1[] = {
		16, 128, 16, 128, 100, 92, 110, 172, 120, 93, 130, 173, 16, 128, 16, 128
	};
	CHECK(memcmp(out, row0, sizeof(row0)) == 0);
	CHECK(memcmp(out + 16, row1, sizeof(row1)) == 0);
	return 0;
}

static int test_letterbox_rgb_to_nv12(void)
{
	/* a 4x2 RGB32 source (red, green, blue, white per column) into a 4x6 NV12 camera:
	 * two black rows above and below (the offset stays even for the chroma) */
	BYTE src[4 * 2 * 4] = WINPR_C_ARRAY_INIT;
	const BYTE colours[4][3] = { { 0, 0, 255 }, { 0, 255, 0 }, { 255, 0, 0 }, { 255, 255, 255 } };
	for (size_t y = 0; y < 2; y++)
		for (size_t x = 0; x < 4; x++)
			memcpy(&src[(y * 4 + x) * 4], colours[x], 3);
	BYTE out[4 * 6 * 3 / 2] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage s = WINPR_C_ARRAY_INIT;
	ShadowCameraImage d = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&s, SHADOW_CAMERA_PIXEL_BGRX32, 4, 2, src, sizeof(src)));
	CHECK(shadow_camera_image_wrap(&d, SHADOW_CAMERA_PIXEL_NV12, 4, 6, out, sizeof(out)));
	CHECK(shadow_camera_image_letterbox(&s, &d));

	/* BT.601 limited range: red Y 82 U 90 V 240, green Y 144, blue Y 41 U 240 V 110, white
	 * Y 235 */
	const BYTE black[] = { 16, 16, 16, 16 };
	const BYTE luma[] = { 82, 144, 41, 235 };
	CHECK(memcmp(out + 0, black, 4) == 0);
	CHECK(memcmp(out + 4, black, 4) == 0);
	CHECK(memcmp(out + 8, luma, 4) == 0);
	CHECK(memcmp(out + 12, luma, 4) == 0);
	CHECK(memcmp(out + 16, black, 4) == 0);
	CHECK(memcmp(out + 20, black, 4) == 0);
	/* the UV rows: black, then red's and blue's chroma (each pair's first pixel), black */
	const BYTE uvBlack[] = { 128, 128, 128, 128 };
	CHECK(memcmp(out + 24, uvBlack, 4) == 0);
	CHECK((out[28] == 90) && (out[29] == 240));
	CHECK((out[30] == 240) && (out[31] == 110));
	CHECK(memcmp(out + 32, uvBlack, 4) == 0);
	return 0;
}

static int test_codec_inverse(void)
{
	/* FreeRDP's H.264 decoder turned limited range BT.601 red (Y 82, U 90, V 240) and white
	 * (Y 235) into these BGRX pixels with its full range BT.709 matrix (prim_internal.h
	 * YUV2R/G/B): its inverse gives the YUV back, within rounding */
	const BYTE src[] = { 11, 36, 255, 0, 235, 235, 235, 0, 11, 36, 255, 0, 235, 235, 235, 0 };
	BYTE out[2 * 2 * 2] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage s = WINPR_C_ARRAY_INIT;
	ShadowCameraImage d = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&s, SHADOW_CAMERA_PIXEL_BGRX32_CODEC, 2, 2, src, sizeof(src)));
	CHECK(shadow_camera_image_wrap(&d, SHADOW_CAMERA_PIXEL_YUY2, 2, 2, out, sizeof(out)));
	CHECK(shadow_camera_image_letterbox(&s, &d));
	(void)printf("codec inverse: red Y %u U %u V %u, white Y %u\n", out[0], out[1], out[3], out[2]);
	CHECK((out[0] >= 79) && (out[0] <= 85));   /* red Y 82 */
	CHECK((out[1] >= 87) && (out[1] <= 93));   /* red U 90 */
	CHECK((out[3] >= 236) && (out[3] <= 243)); /* red V 240 */
	CHECK((out[2] >= 232) && (out[2] <= 236)); /* white Y 235 */
	return 0;
}

static int test_downscale_bottom_up(void)
{
	/* a bottom-up 2x2 RGB24: the first row in memory is the bottom one */
	const BYTE src[] = { 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 255 };
	BYTE out[2 * 2 * 2] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage s = WINPR_C_ARRAY_INIT;
	ShadowCameraImage d = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&s, SHADOW_CAMERA_PIXEL_BGR24, 2, 2, src, sizeof(src)));
	s.bottomUp = TRUE;
	CHECK(shadow_camera_image_wrap(&d, SHADOW_CAMERA_PIXEL_YUY2, 2, 2, out, sizeof(out)));
	CHECK(shadow_camera_image_letterbox(&s, &d));
	CHECK((out[0] == 235) && (out[2] == 235)); /* white on top */
	CHECK((out[4] == 16) && (out[6] == 16));   /* black below */

	/* 640x480 I420 with a horizontal ramp into 320x240 NV12: every other column */
	const size_t size = shadow_camera_image_size(SHADOW_CAMERA_PIXEL_I420, 640, 480);
	BYTE* big = calloc(1, size);
	BYTE* small = calloc(1, shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, 320, 240));
	if (!big || !small)
	{
		free(big);
		free(small);
		return -1;
	}
	for (size_t y = 0; y < 480; y++)
		for (size_t x = 0; x < 640; x++)
			big[y * 640 + x] = (BYTE)(x / 4);
	memset(big + 640 * 480, 128, 640 * 480 / 2);
	ShadowCameraImage bs = WINPR_C_ARRAY_INIT;
	ShadowCameraImage sd = WINPR_C_ARRAY_INIT;
	int rc = -1;
	if (shadow_camera_image_wrap(&bs, SHADOW_CAMERA_PIXEL_I420, 640, 480, big, size) &&
	    shadow_camera_image_wrap(&sd, SHADOW_CAMERA_PIXEL_NV12, 320, 240, small,
	                             shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, 320, 240)) &&
	    shadow_camera_image_letterbox(&bs, &sd))
	{
		rc = 0;
		for (size_t x = 0; x < 320; x++)
		{
			if (small[100 * 320 + x] != (BYTE)((2 * x + 1) / 4))
				rc = -1;
		}
	}
	free(big);
	free(small);
	CHECK(rc == 0);
	return 0;
}

static int test_raw_decoder(void)
{
	/* a client's raw sample: passed through when complete, refused when short */
	const CAM_MEDIA_TYPE_DESCRIPTION type = { .Format = CAM_MEDIA_FORMAT_YUY2,
		                                      .Width = 64,
		                                      .Height = 48,
		                                      .FrameRateNumerator = 30,
		                                      .FrameRateDenominator = 1,
		                                      .PixelAspectRatioNumerator = 1,
		                                      .PixelAspectRatioDenominator = 1 };
	ShadowCameraDecoder* decoder = shadow_camera_decoder_new(&type);
	CHECK(decoder);
	const size_t size = 64ull * 48 * 2;
	BYTE* sample = calloc(1, size);
	ShadowCameraImage image = WINPR_C_ARRAY_INIT;
	BOOL picture = TRUE;
	const BOOL shortOk =
	    sample && shadow_camera_decoder_decode(decoder, sample, size - 2, &image, &picture);
	const BOOL fullOk =
	    sample && shadow_camera_decoder_decode(decoder, sample, size, &image, &picture);
	free(sample);
	shadow_camera_decoder_free(decoder);
	CHECK(!shortOk);
	CHECK(fullOk && picture);
	CHECK((image.format == SHADOW_CAMERA_PIXEL_YUY2) && (image.width == 64) &&
	      (image.height == 48));

	/* odd sizes and unknown formats get no decoder */
	CAM_MEDIA_TYPE_DESCRIPTION odd = type;
	odd.Width = 63;
	CHECK(!shadow_camera_decoder_new(&odd));
	CAM_MEDIA_TYPE_DESCRIPTION invalid = type;
	invalid.Format = CAM_MEDIA_FORMAT_INVALID;
	CHECK(!shadow_camera_decoder_new(&invalid));
	return 0;
}

/* 32x16 baseline JPEG, 4:2:0, left half red, right half blue (ImageMagick, quality 95) */
static const BYTE g_redBlueJpeg[] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
	0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x20, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
	0x01, 0xff, 0xc4, 0x00, 0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4,
	0x00, 0x16, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x0a, 0x08, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x0c,
	0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x8b, 0xc0, 0x4a, 0x6d, 0xfc, 0x48,
	0x60, 0x15, 0x40, 0xd7, 0xbf, 0xff, 0xd9,
};

static int test_mjpg_decoder(void)
{
	ShadowCameraDecoders decoders = WINPR_C_ARRAY_INIT;
	shadow_camera_decoders_probe(&decoders);
	if (!decoders.mjpg)
	{
		(void)printf("MJPG: no decoder in this build, skipped\n");
		return 0;
	}

	const CAM_MEDIA_TYPE_DESCRIPTION type = {
		.Format = CAM_MEDIA_FORMAT_MJPG,
		.Width = 32,
		.Height = 16,
		.FrameRateNumerator = 30,
		.FrameRateDenominator = 1,
		.PixelAspectRatioNumerator = 1,
		.PixelAspectRatioDenominator = 1,
		.Flags = CAM_MEDIA_TYPE_DESCRIPTION_FLAG_DecodingRequired
	};
	ShadowCameraDecoder* decoder = shadow_camera_decoder_new(&type);
	CHECK(decoder);
	ShadowCameraImage image = WINPR_C_ARRAY_INIT;
	BOOL picture = FALSE;
	const BOOL ok = shadow_camera_decoder_decode(decoder, g_redBlueJpeg, sizeof(g_redBlueJpeg),
	                                             &image, &picture);
	BYTE yuyv[32 * 16 * 2] = WINPR_C_ARRAY_INIT;
	ShadowCameraImage out = WINPR_C_ARRAY_INIT;
	const BOOL scaled =
	    ok && picture &&
	    shadow_camera_image_wrap(&out, SHADOW_CAMERA_PIXEL_YUY2, 32, 16, yuyv, sizeof(yuyv)) &&
	    shadow_camera_image_letterbox(&image, &out);
	CHECK(ok && picture && scaled);
	CHECK((image.format == SHADOW_CAMERA_PIXEL_NV12) && (image.width == 32) &&
	      (image.height == 16));

	/* a truncated JPEG gives no picture */
	BOOL truncatedPicture = FALSE;
	ShadowCameraImage truncated = WINPR_C_ARRAY_INIT;
	const BOOL truncatedOk =
	    shadow_camera_decoder_decode(decoder, g_redBlueJpeg, 40, &truncated, &truncatedPicture);
	shadow_camera_decoder_free(decoder);
	CHECK(!truncatedOk || !truncatedPicture);

	/* red: Y 82 U 90 V 240; blue: Y 41 U 240 V 110; JPEG is lossy, allow some */
	const BYTE* row = &yuyv[8 * 32 * 2];
	(void)printf("MJPG: red Y %u U %u V %u, blue Y %u U %u V %u\n", row[4 * 2], row[4 * 2 + 1],
	             row[4 * 2 + 3], row[28 * 2], row[28 * 2 + 1], row[28 * 2 + 3]);
	CHECK((row[4 * 2] > 70) && (row[4 * 2] < 95));
	CHECK((row[4 * 2 + 1] < 110) && (row[4 * 2 + 3] > 220));
	CHECK((row[28 * 2] > 30) && (row[28 * 2] < 55));
	CHECK((row[28 * 2 + 1] > 220) && (row[28 * 2 + 3] < 130));
	return 0;
}

int TestShadowCameraImage(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (test_sizes() != 0)
		return -1;
	if (test_pillarbox_yuy2() != 0)
		return -1;
	if (test_letterbox_rgb_to_nv12() != 0)
		return -1;
	if (test_downscale_bottom_up() != 0)
		return -1;
	if (test_codec_inverse() != 0)
		return -1;
	if (test_raw_decoder() != 0)
		return -1;
	if (test_mjpg_decoder() != 0)
		return -1;
	return 0;
}
