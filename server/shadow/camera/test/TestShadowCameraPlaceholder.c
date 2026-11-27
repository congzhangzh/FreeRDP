/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the still drawn without a client camera
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

#include "placeholder.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			goto fail;                                                       \
		}                                                                    \
	} while (0)

#define W 320
#define H 240

/* the checksum of the 320x240 YUYV still, see below */
#define PLACEHOLDER_YUYV_320X240_FNV1A 0x045ac757u

/* full range grey to video range luma, as placeholder.c */
static BYTE video_luma(BYTE gray)
{
	return (BYTE)(16 + (gray * 219 + 127) / 255);
}

static UINT32 fnv1a(const BYTE* data, size_t size)
{
	UINT32 hash = 2166136261u;
	for (size_t i = 0; i < size; i++)
	{
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

int TestShadowCameraPlaceholder(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = -1;
	BYTE* yuyv = calloc(2ull * W, H);
	BYTE* nv12 = calloc(3ull * W / 2, H);
	if (!yuyv || !nv12)
		goto fail;

	CHECK(camera_placeholder_yuyv(yuyv, 2ull * W, W, H));
	CHECK(camera_placeholder_nv12(nv12, W, nv12 + 1ull * W * H, W, W, H));

	/* YUYV: the same luma as NV12, chroma neutral */
	size_t symbol = 0;
	for (size_t y = 0; y < H; y++)
	{
		for (size_t x = 0; x < W; x++)
		{
			const BYTE luma = yuyv[(y * W + x) * 2];
			CHECK(luma == nv12[y * W + x]);
			CHECK(yuyv[(y * W + x) * 2 + 1] == 128);
			if (luma == video_luma(CAMERA_PLACEHOLDER_FOREGROUND))
				symbol++;
		}
	}
	for (size_t i = 0; i < 1ull * W * H / 2; i++)
		CHECK(nv12[1ull * W * H + i] == 128);

	/* dark background in the corners and around, the symbol in the middle, no stray pixels */
	const BYTE background = video_luma(CAMERA_PLACEHOLDER_BACKGROUND);
	CHECK(yuyv[0] == background);
	CHECK(yuyv[(W - 1) * 2] == background);
	CHECK(yuyv[((H - 1) * W) * 2] == background);
	CHECK(yuyv[((H - 1) * W + W - 1) * 2] == background);
	for (size_t x = 0; x < W; x++)
	{
		CHECK(yuyv[(10 * W + x) * 2] == background);
		CHECK(yuyv[((H - 10) * W + x) * 2] == background);
	}
	/* the icon is 40 % of the shorter side: 96 px, so between 5 % and 40 % of its box */
	CHECK(symbol > (96 * 96) / 20);
	CHECK(symbol < (96 * 96) * 2 / 5);

	/* the slash: the centre pixel is foreground, the icon's corners (on the slash's line) too */
	CHECK(yuyv[((H / 2) * W + W / 2) * 2] == video_luma(CAMERA_PLACEHOLDER_FOREGROUND));

	/* odd widths are refused, YUYV needs pairs */
	CHECK(!camera_placeholder_yuyv(yuyv, 2ull * W, W - 1, H));
	CHECK(!camera_placeholder_yuyv(yuyv, W, W, H));

	{
		const UINT32 sum = fnv1a(yuyv, 2ull * W * H);
		(void)printf("placeholder YUYV %dx%d fnv1a 0x%08" PRIx32 ", %" PRIuz " symbol pixels\n", W,
		             H, sum, symbol);
#if defined(__x86_64__) || defined(_M_X64)
		/* pinned on x86-64 (SSE2 doubles, no FMA): placeholder.c draws with doubles, so another
		 * ISA may round an edge pixel differently */
		CHECK(sum == PLACEHOLDER_YUYV_320X240_FNV1A);
#endif
	}
	rc = 0;
fail:
	free(yuyv);
	free(nv12);
	return rc;
}
