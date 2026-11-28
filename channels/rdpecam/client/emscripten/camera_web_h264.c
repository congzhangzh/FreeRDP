/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, browser camera: WebCodecs H.264 to MS-RDPECAM samples
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

/*
 * The browser's VideoEncoder is configured with avc: { format: "avc" } (every browser with H.264
 * encoding has it; "annexb" is not everywhere): each chunk is one frame as NAL units after their
 * lengths, and the SPS and PPS come once, in the avcC record of the first output's metadata. An
 * MS-RDPECAM H.264 sample is an Annex B access unit, and a server may join at any key frame, so
 * every key frame goes out with the SPS and PPS in front (in-band). No browser dependency: the
 * unit test runs on any host.
 */

#include <freerdp/config.h>

#include <string.h>

#include <winpr/spec.h>

#include "camera_web_h264.h"

#define CAM_WEB_MAX_SIDE 4096u
#define NAL_TYPE(b) ((b) & 0x1F)
#define NAL_SPS 7
#define NAL_PPS 8

static const BYTE start_code[4] = { 0, 0, 0, 1 };

size_t cam_web_h264_capacity(UINT32 width, UINT32 height)
{
	if ((width == 0) || (height == 0) || (width > CAM_WEB_MAX_SIDE) || (height > CAM_WEB_MAX_SIDE))
		return 0;
	/* an encoded frame is far below the raw I420 frame; the parameter sets come on top */
	return 3ull * width * height / 2 + 4096ull;
}

UINT32 cam_web_h264_bitrate(UINT32 width, UINT32 height, UINT32 fpsNum, UINT32 fpsDen)
{
	if ((fpsNum == 0) || (fpsDen == 0))
		return 0;
	const double pixelsPerSecond = 1.0 * width * height * fpsNum / fpsDen;
	const double rate = 1000000.0 * pixelsPerSecond / (640.0 * 480.0 * 30.0);
	if (rate < 150000.0)
		return 150000;
	if (rate > 4000000.0)
		return 4000000;
	return (UINT32)(rate + 0.5);
}

const char* cam_web_h264_codec(UINT32 width, UINT32 height, UINT32 fpsNum, UINT32 fpsDen)
{
	/* Constrained Baseline (42E0) and the level's largest frame and macroblocks per second
	 * (H.264 Table A-1) */
	static const struct
	{
		const char* codec;
		UINT32 maxFs;
		UINT32 maxMbps;
	} levels[] = { { "avc1.42E01E", 1620, 40500 },     /* 3.0 */
		           { "avc1.42E01F", 3600, 108000 },    /* 3.1 */
		           { "avc1.42E020", 5120, 216000 },    /* 3.2 */
		           { "avc1.42E028", 8192, 245760 },    /* 4.0 */
		           { "avc1.42E032", 22080, 589824 },   /* 5.0 */
		           { "avc1.42E033", 36864, 983040 } }; /* 5.1 */

	if ((fpsNum == 0) || (fpsDen == 0))
		return nullptr;
	const UINT64 mbs = (UINT64)((width + 15) / 16) * ((height + 15) / 16);
	const UINT64 mbps = (mbs * fpsNum + fpsDen - 1) / fpsDen;
	for (size_t i = 0; i < ARRAYSIZE(levels); i++)
	{
		if ((mbs <= levels[i].maxFs) && (mbps <= levels[i].maxMbps))
			return levels[i].codec;
	}
	return nullptr;
}

/* appends one parameter set (start code + unit) to params */
WINPR_ATTR_NODISCARD
static BOOL put_unit(const BYTE* unit, size_t len, BYTE* out, size_t capacity, size_t* pos)
{
	if ((len == 0) || (*pos + sizeof(start_code) + len > capacity))
		return FALSE;
	memcpy(out + *pos, start_code, sizeof(start_code));
	memcpy(out + *pos + sizeof(start_code), unit, len);
	*pos += sizeof(start_code) + len;
	return TRUE;
}

/* reads count units of the given type, each after a 16 bit length */
WINPR_ATTR_NODISCARD
static BOOL read_sets(const BYTE* avcc, size_t len, size_t* at, size_t count, BYTE type, BYTE* out,
                      size_t capacity, size_t* pos)
{
	for (size_t i = 0; i < count; i++)
	{
		if (*at + 2 > len)
			return FALSE;
		const size_t n = ((size_t)avcc[*at] << 8) | avcc[*at + 1];
		*at += 2;
		if ((n == 0) || (*at + n > len) || (NAL_TYPE(avcc[*at]) != type))
			return FALSE;
		if (!put_unit(avcc + *at, n, out, capacity, pos))
			return FALSE;
		*at += n;
	}
	return TRUE;
}

BOOL cam_web_h264_parse_avcc(const BYTE* avcc, size_t len, BYTE* lengthSize, BYTE* params,
                             size_t paramsCapacity, size_t* paramsSize)
{
	if (!avcc || !lengthSize || !params || !paramsSize)
		return FALSE;
	*paramsSize = 0;

	/* version 1, profile, compatibility, level, 6 bits 1 + length size - 1, 3 bits 1 + SPS count */
	if ((len < 7) || (avcc[0] != 1))
		return FALSE;
	const BYTE size = (BYTE)((avcc[4] & 0x03) + 1);
	if (size == 3)
		return FALSE;

	size_t at = 5;
	size_t pos = 0;
	const size_t nSps = avcc[at++] & 0x1F;
	if ((nSps == 0) || !read_sets(avcc, len, &at, nSps, NAL_SPS, params, paramsCapacity, &pos))
		return FALSE;
	if (at >= len)
		return FALSE;
	const size_t nPps = avcc[at++];
	if ((nPps == 0) || !read_sets(avcc, len, &at, nPps, NAL_PPS, params, paramsCapacity, &pos))
		return FALSE;
	/* a High profile record has more fields after this; Constrained Baseline has none */

	*lengthSize = size;
	*paramsSize = pos;
	return TRUE;
}

WINPR_ATTR_NODISCARD
static size_t read_length(const BYTE* p, BYTE lengthSize)
{
	size_t n = 0;
	for (BYTE i = 0; i < lengthSize; i++)
		n = (n << 8) | p[i];
	return n;
}

BOOL cam_web_h264_to_annexb(BYTE lengthSize, const BYTE* params, size_t paramsSize, BOOL key,
                            const BYTE* chunk, size_t chunkSize, BYTE* out, size_t outCapacity,
                            size_t* outSize)
{
	if (!chunk || !out || !outSize || (chunkSize == 0))
		return FALSE;
	if ((lengthSize != 1) && (lengthSize != 2) && (lengthSize != 4))
		return FALSE;
	*outSize = 0;

	/* the units must fill the chunk exactly; does the chunk carry its own SPS? */
	BOOL inBandSps = FALSE;
	for (size_t at = 0; at < chunkSize;)
	{
		if (at + lengthSize > chunkSize)
			return FALSE;
		const size_t n = read_length(chunk + at, lengthSize);
		at += lengthSize;
		if ((n == 0) || (n > chunkSize - at))
			return FALSE;
		if (NAL_TYPE(chunk[at]) == NAL_SPS)
			inBandSps = TRUE;
		at += n;
	}

	size_t pos = 0;
	if (key && !inBandSps)
	{
		if (!params || (paramsSize == 0) || (paramsSize > outCapacity))
			return FALSE;
		memcpy(out, params, paramsSize);
		pos = paramsSize;
	}

	for (size_t at = 0; at < chunkSize;)
	{
		const size_t n = read_length(chunk + at, lengthSize);
		at += lengthSize;
		if (!put_unit(chunk + at, n, out, outCapacity, &pos))
			return FALSE;
		at += n;
	}
	*outSize = pos;
	return TRUE;
}
