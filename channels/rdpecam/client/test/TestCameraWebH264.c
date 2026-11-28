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

/* camera_web_h264.c on the host: the avcC record, chunks to Annex B samples, bit rate and level */

#include <stdio.h>
#include <string.h>

#include "../emscripten/camera_web_h264.h"

static int failures = 0;

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                      \
		}                                                                    \
	} while (0)

/* Constrained Baseline 3.0: one SPS (9 bytes), one PPS (4 bytes), 4-byte lengths */
static const BYTE avcc[] = {
	0x01, 0x42, 0xE0, 0x1E, 0xFF, 0xE1, 0x00, 0x09, 0x67, 0x42, 0xE0, 0x1E,
	0x8C, 0x8D, 0x40, 0x50, 0x1E, 0x01, 0x00, 0x04, 0x68, 0xCE, 0x3C, 0x80
};
static const BYTE sps_pps_annexb[] = { 0,    0,    0, 1, 0x67, 0x42, 0xE0, 0x1E, 0x8C, 0x8D, 0x40,
	                                   0x50, 0x1E, 0, 0, 0,    1,    0x68, 0xCE, 0x3C, 0x80 };

static void test_avcc(void)
{
	BYTE params[64] = { 0 };
	BYTE lengthSize = 0;
	size_t n = 0;

	CHECK(cam_web_h264_parse_avcc(avcc, sizeof(avcc), &lengthSize, params, sizeof(params), &n));
	CHECK(lengthSize == 4);
	CHECK(n == sizeof(sps_pps_annexb));
	CHECK(memcmp(params, sps_pps_annexb, sizeof(sps_pps_annexb)) == 0);

	/* too small for the parameter sets */
	CHECK(!cam_web_h264_parse_avcc(avcc, sizeof(avcc), &lengthSize, params, 20, &n));
	/* cut anywhere */
	for (size_t len = 0; len < sizeof(avcc); len++)
		CHECK(!cam_web_h264_parse_avcc(avcc, len, &lengthSize, params, sizeof(params), &n));

	BYTE bad[sizeof(avcc)];
	/* another version */
	memcpy(bad, avcc, sizeof(bad));
	bad[0] = 2;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	/* a length size of 3 does not exist */
	memcpy(bad, avcc, sizeof(bad));
	bad[4] = 0xFE;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	/* no SPS */
	memcpy(bad, avcc, sizeof(bad));
	bad[5] = 0xE0;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	/* the SPS is not one (a PPS in its place) */
	memcpy(bad, avcc, sizeof(bad));
	bad[8] = 0x68;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	/* no PPS */
	memcpy(bad, avcc, sizeof(bad));
	bad[17] = 0;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	/* an SPS longer than the record */
	memcpy(bad, avcc, sizeof(bad));
	bad[7] = 0x40;
	CHECK(!cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));

	/* 2-byte lengths */
	memcpy(bad, avcc, sizeof(bad));
	bad[4] = 0xFD;
	CHECK(cam_web_h264_parse_avcc(bad, sizeof(bad), &lengthSize, params, sizeof(params), &n));
	CHECK(lengthSize == 2);
}

static void test_chunks(void)
{
	BYTE out[128] = { 0 };
	size_t n = 0;

	/* a key frame: an IDR slice (5 bytes) */
	const BYTE idr[] = { 0, 0, 0, 5, 0x65, 0x88, 0x84, 0x00, 0x33 };
	const BYTE idrAnnexb[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33 };
	CHECK(cam_web_h264_to_annexb(4, sps_pps_annexb, sizeof(sps_pps_annexb), TRUE, idr, sizeof(idr),
	                             out, sizeof(out), &n));
	CHECK(n == sizeof(sps_pps_annexb) + sizeof(idrAnnexb));
	CHECK(memcmp(out, sps_pps_annexb, sizeof(sps_pps_annexb)) == 0);
	CHECK(memcmp(out + sizeof(sps_pps_annexb), idrAnnexb, sizeof(idrAnnexb)) == 0);

	/* a key frame needs the parameter sets */
	CHECK(!cam_web_h264_to_annexb(4, nullptr, 0, TRUE, idr, sizeof(idr), out, sizeof(out), &n));

	/* a key frame with its own SPS and PPS: nothing in front */
	const BYTE inband[] = {
		0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x68, 0xCE, 0, 0, 0, 2, 0x65, 0x88
	};
	const BYTE inbandAnnexb[] = { 0, 0,    0,    1, 0x67, 0x42, 0, 0,    0,
		                          1, 0x68, 0xCE, 0, 0,    0,    1, 0x65, 0x88 };
	CHECK(cam_web_h264_to_annexb(4, sps_pps_annexb, sizeof(sps_pps_annexb), TRUE, inband,
	                             sizeof(inband), out, sizeof(out), &n));
	CHECK((n == sizeof(inbandAnnexb)) && (memcmp(out, inbandAnnexb, n) == 0));

	/* a delta frame: two slices, 2-byte lengths, no parameter sets */
	const BYTE delta[] = { 0, 3, 0x41, 0x9A, 0x02, 0, 2, 0x41, 0x9B };
	const BYTE deltaAnnexb[] = { 0, 0, 0, 1, 0x41, 0x9A, 0x02, 0, 0, 0, 1, 0x41, 0x9B };
	CHECK(cam_web_h264_to_annexb(2, sps_pps_annexb, sizeof(sps_pps_annexb), FALSE, delta,
	                             sizeof(delta), out, sizeof(out), &n));
	CHECK((n == sizeof(deltaAnnexb)) && (memcmp(out, deltaAnnexb, n) == 0));
	/* 1-byte lengths */
	const BYTE delta1[] = { 3, 0x41, 0x9A, 0x02 };
	CHECK(
	    cam_web_h264_to_annexb(1, nullptr, 0, FALSE, delta1, sizeof(delta1), out, sizeof(out), &n));
	CHECK(n == 7);

	/* lengths that do not add up: short, long, an empty unit, a cut length */
	const BYTE shortLen[] = { 0, 0, 0, 3, 0x41, 0x9A, 0x02, 0x00 };
	CHECK(!cam_web_h264_to_annexb(4, nullptr, 0, FALSE, shortLen, sizeof(shortLen), out,
	                              sizeof(out), &n));
	const BYTE longLen[] = { 0, 0, 0, 9, 0x41, 0x9A, 0x02 };
	CHECK(!cam_web_h264_to_annexb(4, nullptr, 0, FALSE, longLen, sizeof(longLen), out, sizeof(out),
	                              &n));
	const BYTE empty[] = { 0, 0, 0, 0, 0, 0, 0, 1, 0x41 };
	CHECK(
	    !cam_web_h264_to_annexb(4, nullptr, 0, FALSE, empty, sizeof(empty), out, sizeof(out), &n));
	const BYTE cut[] = { 0, 0, 0, 1, 0x41, 0, 0 };
	CHECK(!cam_web_h264_to_annexb(4, nullptr, 0, FALSE, cut, sizeof(cut), out, sizeof(out), &n));
	/* lengths of a size that does not exist; an empty chunk */
	CHECK(
	    !cam_web_h264_to_annexb(3, nullptr, 0, FALSE, delta, sizeof(delta), out, sizeof(out), &n));
	CHECK(!cam_web_h264_to_annexb(4, nullptr, 0, FALSE, idr, 0, out, sizeof(out), &n));
	/* a sample larger than the room for it */
	CHECK(!cam_web_h264_to_annexb(4, sps_pps_annexb, sizeof(sps_pps_annexb), TRUE, idr, sizeof(idr),
	                              out, sizeof(sps_pps_annexb) + 8, &n));
	CHECK(cam_web_h264_to_annexb(4, sps_pps_annexb, sizeof(sps_pps_annexb), TRUE, idr, sizeof(idr),
	                             out, sizeof(sps_pps_annexb) + 9, &n));
}

static void test_rate_and_level(void)
{
	CHECK(cam_web_h264_bitrate(640, 480, 30, 1) == 1000000);
	CHECK(cam_web_h264_bitrate(1280, 720, 30, 1) == 3000000);
	CHECK(cam_web_h264_bitrate(320, 240, 30, 1) == 250000);
	CHECK(cam_web_h264_bitrate(160, 120, 15, 1) == 150000);
	CHECK(cam_web_h264_bitrate(1920, 1080, 60, 1) == 4000000);
	CHECK(cam_web_h264_bitrate(640, 480, 30, 0) == 0);

	CHECK(strcmp(cam_web_h264_codec(640, 480, 30, 1), "avc1.42E01E") == 0);
	CHECK(strcmp(cam_web_h264_codec(640, 360, 30, 1), "avc1.42E01E") == 0);
	CHECK(strcmp(cam_web_h264_codec(320, 240, 30, 1), "avc1.42E01E") == 0);
	CHECK(strcmp(cam_web_h264_codec(1280, 720, 30, 1), "avc1.42E01F") == 0);
	CHECK(strcmp(cam_web_h264_codec(1280, 720, 60, 1), "avc1.42E020") == 0);
	CHECK(strcmp(cam_web_h264_codec(1920, 1080, 30, 1), "avc1.42E028") == 0);
	CHECK(cam_web_h264_codec(4096, 4096, 60, 1) == nullptr);
	CHECK(cam_web_h264_codec(640, 480, 30, 0) == nullptr);

	CHECK(cam_web_h264_capacity(0, 480) == 0);
	CHECK(cam_web_h264_capacity(4097, 480) == 0);
	CHECK(cam_web_h264_capacity(640, 480) == 640u * 480u * 3u / 2u + 4096u);
}

int TestCameraWebH264(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	test_avcc();
	test_chunks();
	test_rate_and_level();

	(void)printf("TestCameraWebH264: %d failure(s)\n", failures);
	return failures ? -1 : 0;
}
