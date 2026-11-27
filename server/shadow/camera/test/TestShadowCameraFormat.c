/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the media type a client is asked for, and the pacing
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

#include <winpr/wtypes.h>

#include "camera_policy.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

static CAM_MEDIA_TYPE_DESCRIPTION mt(CAM_MEDIA_FORMAT format, UINT32 w, UINT32 h, UINT32 fps)
{
	return (CAM_MEDIA_TYPE_DESCRIPTION){ .Format = format,
		                                 .Width = w,
		                                 .Height = h,
		                                 .FrameRateNumerator = fps,
		                                 .FrameRateDenominator = 1,
		                                 .PixelAspectRatioNumerator = 1,
		                                 .PixelAspectRatioDenominator = 1 };
}

static int choose(const CAM_MEDIA_TYPE_DESCRIPTION* types, size_t count,
                  const ShadowCameraDecoders* decoders)
{
	size_t index = 0;
	if (!shadow_camera_choose_media_type(types, count, decoders, 1280, 720, 30, &index))
		return -1;
	return (int)index;
}

static int test_format_order(void)
{
	const ShadowCameraDecoders all = { .h264 = TRUE, .mjpg = TRUE };
	const ShadowCameraDecoders mjpgOnly = { .h264 = FALSE, .mjpg = TRUE };
	const ShadowCameraDecoders none = { .h264 = FALSE, .mjpg = FALSE };

	/* H.264 > MJPG > YUY2 > NV12 > I420 > RGB32 > RGB24 */
	const CAM_MEDIA_FORMAT order[] = { CAM_MEDIA_FORMAT_H264, CAM_MEDIA_FORMAT_MJPG,
		                               CAM_MEDIA_FORMAT_YUY2, CAM_MEDIA_FORMAT_NV12,
		                               CAM_MEDIA_FORMAT_I420, CAM_MEDIA_FORMAT_RGB32,
		                               CAM_MEDIA_FORMAT_RGB24 };
	for (size_t i = 1; i < ARRAYSIZE(order); i++)
		CHECK(shadow_camera_format_rank(order[i - 1], &all) <
		      shadow_camera_format_rank(order[i], &all));
	CHECK(shadow_camera_format_rank(CAM_MEDIA_FORMAT_H264, &none) < 0);
	CHECK(shadow_camera_format_rank(CAM_MEDIA_FORMAT_MJPG, &none) < 0);
	CHECK(shadow_camera_format_rank(CAM_MEDIA_FORMAT_INVALID, &all) < 0);

	/* the client lists them in any order */
	const CAM_MEDIA_TYPE_DESCRIPTION list[] = {
		mt(CAM_MEDIA_FORMAT_I420, 640, 480, 30), mt(CAM_MEDIA_FORMAT_RGB24, 640, 480, 30),
		mt(CAM_MEDIA_FORMAT_MJPG, 640, 480, 30), mt(CAM_MEDIA_FORMAT_YUY2, 640, 480, 30),
		mt(CAM_MEDIA_FORMAT_H264, 640, 480, 30), mt(CAM_MEDIA_FORMAT_NV12, 640, 480, 30)
	};
	CHECK(choose(list, ARRAYSIZE(list), &all) == 4);      /* H.264 */
	CHECK(choose(list, ARRAYSIZE(list), &mjpgOnly) == 2); /* MJPG without an H.264 decoder */
	CHECK(choose(list, ARRAYSIZE(list), &none) == 3);     /* raw only: YUY2 */

	/* raw only if nothing compressed is offered, even a compressed one of a worse size */
	const CAM_MEDIA_TYPE_DESCRIPTION rawFirst[] = { mt(CAM_MEDIA_FORMAT_YUY2, 1280, 720, 30),
		                                            mt(CAM_MEDIA_FORMAT_MJPG, 320, 240, 15) };
	CHECK(choose(rawFirst, ARRAYSIZE(rawFirst), &all) == 1);

	const CAM_MEDIA_TYPE_DESCRIPTION rgb[] = { mt(CAM_MEDIA_FORMAT_RGB24, 640, 480, 30),
		                                       mt(CAM_MEDIA_FORMAT_RGB32, 640, 480, 30) };
	CHECK(choose(rgb, ARRAYSIZE(rgb), &all) == 1);

	/* nothing usable */
	const CAM_MEDIA_TYPE_DESCRIPTION h264Only[] = { mt(CAM_MEDIA_FORMAT_H264, 640, 480, 30) };
	CHECK(choose(h264Only, ARRAYSIZE(h264Only), &none) == -1);
	CHECK(choose(nullptr, 0, &all) == -1);
	return 0;
}

static int test_size_and_rate(void)
{
	const ShadowCameraDecoders all = { .h264 = TRUE, .mjpg = TRUE };

	/* the largest that fits 1280x720 */
	const CAM_MEDIA_TYPE_DESCRIPTION sizes[] = { mt(CAM_MEDIA_FORMAT_H264, 640, 480, 30),
		                                         mt(CAM_MEDIA_FORMAT_H264, 1920, 1080, 30),
		                                         mt(CAM_MEDIA_FORMAT_H264, 1280, 720, 30),
		                                         mt(CAM_MEDIA_FORMAT_H264, 320, 240, 30) };
	CHECK(choose(sizes, ARRAYSIZE(sizes), &all) == 2);

	/* none fits: the smallest */
	const CAM_MEDIA_TYPE_DESCRIPTION large[] = { mt(CAM_MEDIA_FORMAT_H264, 3840, 2160, 30),
		                                         mt(CAM_MEDIA_FORMAT_H264, 1920, 1080, 30) };
	CHECK(choose(large, ARRAYSIZE(large), &all) == 1);

	/* the closest rate, the faster one on a tie */
	const CAM_MEDIA_TYPE_DESCRIPTION rates[] = { mt(CAM_MEDIA_FORMAT_H264, 640, 480, 15),
		                                         mt(CAM_MEDIA_FORMAT_H264, 640, 480, 60),
		                                         mt(CAM_MEDIA_FORMAT_H264, 640, 480, 30) };
	CHECK(choose(rates, ARRAYSIZE(rates), &all) == 2);
	const CAM_MEDIA_TYPE_DESCRIPTION tie[] = { mt(CAM_MEDIA_FORMAT_H264, 640, 480, 25),
		                                       mt(CAM_MEDIA_FORMAT_H264, 640, 480, 35) };
	CHECK(choose(tie, ARRAYSIZE(tie), &all) == 1);

	/* garbage is skipped, also when it would rank first */
	CAM_MEDIA_TYPE_DESCRIPTION bad[] = { mt(CAM_MEDIA_FORMAT_H264, 0, 480, 30),
		                                 mt(CAM_MEDIA_FORMAT_H264, 640, 480, 30),
		                                 mt(CAM_MEDIA_FORMAT_H264, 100000, 480, 30),
		                                 mt(CAM_MEDIA_FORMAT_YUY2, 640, 480, 30) };
	bad[1].FrameRateDenominator = 0;
	CHECK(choose(bad, ARRAYSIZE(bad), &all) == 3);
	return 0;
}

static int test_pacing(void)
{
	/* ceil(fps * rtt) + 1, at most 4 */
	CHECK(shadow_camera_pacing_outstanding(30, 1, 0) == 1);
	CHECK(shadow_camera_pacing_outstanding(30, 1, 1) == 2);
	CHECK(shadow_camera_pacing_outstanding(30, 1, 33) == 2);
	CHECK(shadow_camera_pacing_outstanding(30, 1, 34) == 3);
	CHECK(shadow_camera_pacing_outstanding(30, 1, 50) == 3);
	CHECK(shadow_camera_pacing_outstanding(15, 1, 100) == 3);
	CHECK(shadow_camera_pacing_outstanding(60, 1, 40) == 4);
	CHECK(shadow_camera_pacing_outstanding(30, 1, 1000) == SHADOW_CAMERA_MAX_OUTSTANDING);
	CHECK(shadow_camera_pacing_outstanding(30000, 1001, 100) == 4);
	CHECK(shadow_camera_pacing_outstanding(30, 0, 50) == 3);
	CHECK(shadow_camera_pacing_outstanding(0, 1, 50) == 1);
	return 0;
}

int TestShadowCameraFormat(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (test_format_order() != 0)
		return -1;
	if (test_size_and_rate() != 0)
		return -1;
	if (test_pacing() != 0)
		return -1;
	return 0;
}
