/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the counters and the JSON of the stats socket
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
#include <string.h>

#include <winpr/wtypes.h>

#include "camera_stats.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
			return FALSE;                                                    \
		}                                                                    \
	} while (0)

WINPR_ATTR_NODISCARD
static BOOL same(const char* what, const char* got, const char* expected)
{
	if (strcmp(got, expected) == 0)
		return TRUE;
	(void)fprintf(stderr, "%s:\n  got      %s\n  expected %s\n", what, got, expected);
	return FALSE;
}

/* one period of a stream: counts, averages, kbit/s, the gap; the next period starts empty
 * but keeps the totals */
WINPR_ATTR_NODISCARD
static BOOL test_period(void)
{
	ShadowCameraStats s = WINPR_C_ARRAY_INIT;
	shadow_camera_stats_init(&s, 1000);
	shadow_camera_stats_stream_started(&s);

	shadow_camera_stats_sample(&s, 1000, 1100); /* first: no gap yet */
	shadow_camera_stats_decoded(&s, 2000400);
	shadow_camera_stats_shown(&s);
	shadow_camera_stats_outstanding(&s, 2);
	shadow_camera_stats_sample(&s, 3000, 1133); /* gap 33 */
	shadow_camera_stats_decoded(&s, 4000000);
	shadow_camera_stats_shown(&s);
	shadow_camera_stats_outstanding(&s, 3);
	shadow_camera_stats_sample(&s, 1001, 1213); /* gap 80 */
	shadow_camera_stats_decoded(&s, 1000);
	shadow_camera_stats_dropped(&s);
	shadow_camera_stats_outstanding(&s, 1);
	shadow_camera_stats_sample_error(&s);
	shadow_camera_stats_sample_error(&s);

	ShadowCameraStatsValues v = WINPR_C_ARRAY_INIT;
	shadow_camera_stats_take_period(&s, 6000, TRUE, &v);
	CHECK(v.periodMs == 5000);
	CHECK(v.samples == 3);
	CHECK(v.bytes == 5001);
	CHECK(v.kbitPerS == 8); /* 40008 bits / 5000 ms = 8.0016 */
	CHECK(v.frames == 2);
	CHECK(v.dropped == 1);
	CHECK(v.sampleErrors == 2);
	CHECK(v.decodes == 3);
	CHECK(v.decodeAvgUs == 2000); /* 6001400 / 3 = 2000466.7 ns */
	CHECK(v.decodeMaxUs == 4000);
	/* streaming, last sample at 1213: quiet for 4787 ms, longer than any gap between two */
	CHECK(v.gapMaxMs == 4787);
	CHECK(v.outstandingMax == 3);
	CHECK(v.framesTotal == 2);

	/* the next period: empty but the total; the open gap measured from the same last sample */
	shadow_camera_stats_sample(&s, 2500, 6010);
	shadow_camera_stats_shown(&s);
	shadow_camera_stats_take_period(&s, 8500, FALSE, &v);
	CHECK(v.periodMs == 2500);
	CHECK(v.samples == 1);
	CHECK(v.kbitPerS == 8); /* 20000 / 2500 */
	CHECK(v.frames == 1);
	CHECK(v.dropped == 0);
	CHECK(v.sampleErrors == 0);
	CHECK(v.decodes == 0);
	CHECK(v.decodeAvgUs == 0);
	CHECK(v.gapMaxMs == 4797); /* 6010 - 1213: across the period's start */
	CHECK(v.outstandingMax == 0);
	CHECK(v.framesTotal == 3);

	/* not streaming: no open gap; a new stream starts its gap afresh */
	shadow_camera_stats_take_period(&s, 9000, FALSE, &v);
	CHECK(v.gapMaxMs == 0);
	shadow_camera_stats_stream_started(&s);
	shadow_camera_stats_sample(&s, 10, 20000);
	shadow_camera_stats_take_period(&s, 20000, TRUE, &v);
	CHECK(v.gapMaxMs == 0);
	CHECK(v.framesTotal == 3);

	/* kbit/s rounds; a period of 0 ms gives 0, never a division by 0 */
	shadow_camera_stats_sample(&s, 1000, 20001); /* 8000 bits */
	shadow_camera_stats_take_period(&s, 23000, FALSE, &v);
	CHECK(v.kbitPerS == 3); /* 2.67 */
	shadow_camera_stats_sample(&s, 1000, 23000);
	shadow_camera_stats_take_period(&s, 23000, FALSE, &v);
	CHECK(v.periodMs == 0);
	CHECK(v.kbitPerS == 0);
	return TRUE;
}

/* a name is cut at 64 bytes, never inside a UTF-8 sequence */
WINPR_ATTR_NODISCARD
static BOOL test_name_length(void)
{
	char name[100] = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_stats_name_length(nullptr) == 0);
	CHECK(shadow_camera_stats_name_length("") == 0);
	CHECK(shadow_camera_stats_name_length("cam") == 3);

	memset(name, 'a', 64);
	CHECK(shadow_camera_stats_name_length(name) == 64);
	name[64] = 'b';
	CHECK(shadow_camera_stats_name_length(name) == 64);

	/* "é" (C3 A9) at 63-64: the limit falls after its lead byte */
	memset(name, 'a', sizeof(name) - 1);
	name[63] = (char)0xC3;
	name[64] = (char)0xA9;
	CHECK(shadow_camera_stats_name_length(name) == 63);
	/* "€" (E2 82 AC) at 62-64: ends exactly at 64, kept */
	memset(name, 'a', sizeof(name) - 1);
	name[61] = (char)0xE2;
	name[62] = (char)0x82;
	name[63] = (char)0xAC;
	CHECK(shadow_camera_stats_name_length(name) == 64);
	/* "😀" (F0 9F 98 80) at 62-65: cut before it */
	memset(name, 'a', sizeof(name) - 1);
	name[62] = (char)0xF0;
	name[63] = (char)0x9F;
	name[64] = (char)0x98;
	name[65] = (char)0x80;
	CHECK(shadow_camera_stats_name_length(name) == 62);
	return TRUE;
}

/* the newest 4 refusals, oldest first, names cut */
WINPR_ATTR_NODISCARD
static BOOL test_refusals(void)
{
	ShadowCameraRefusals r = WINPR_C_ARRAY_INIT;
	shadow_camera_refusals_add(&r, "one", "a");
	CHECK(r.count == 1);
	shadow_camera_refusals_add(&r, "two", "b");
	shadow_camera_refusals_add(&r, "three", "c");
	shadow_camera_refusals_add(&r, "four", "d");
	CHECK(r.count == 4);
	shadow_camera_refusals_add(&r, "five", "e");
	CHECK(r.count == 4);

	const char* names[] = { "two", "three", "four", "five" };
	const char* reasons[] = { "b", "c", "d", "e" };
	for (size_t x = 0; x < r.count; x++)
	{
		const size_t slot =
		    (r.next + SHADOW_CAMERA_STATS_REFUSALS - r.count + x) % SHADOW_CAMERA_STATS_REFUSALS;
		CHECK(strcmp(r.names[slot], names[x]) == 0);
		CHECK(strcmp(r.reasons[slot], reasons[x]) == 0);
	}

	char longName[80] = WINPR_C_ARRAY_INIT;
	memset(longName, 'x', sizeof(longName) - 1);
	shadow_camera_refusals_add(&r, longName, "f");
	const size_t last = (r.next + SHADOW_CAMERA_STATS_REFUSALS - 1) % SHADOW_CAMERA_STATS_REFUSALS;
	CHECK(strlen(r.names[last]) == 64);
	shadow_camera_refusals_add(&r, nullptr, "g");
	CHECK(r.names[(r.next + SHADOW_CAMERA_STATS_REFUSALS - 1) % SHADOW_CAMERA_STATS_REFUSALS][0] ==
	      '\0');
	return TRUE;
}

/* quotes, backslashes and control characters are escaped; UTF-8 passes */
WINPR_ATTR_NODISCARD
static BOOL test_json_string(void)
{
	char buffer[128] = WINPR_C_ARRAY_INIT;
	size_t at = 0;
	const char* text = "a\"b\\c\n\x01\x1f\xc3\xa9";
	CHECK(shadow_camera_stats_json_string(buffer, sizeof(buffer), &at, text, strlen(text)));
	if (!same("escaped", buffer, "\"a\\\"b\\\\c\\u000a\\u0001\\u001f\xc3\xa9\""))
		return FALSE;
	CHECK(at == strlen(buffer));

	/* only the first length bytes */
	at = 0;
	CHECK(shadow_camera_stats_json_string(buffer, sizeof(buffer), &at, "abcdef", 3));
	if (!same("length", buffer, "\"abc\""))
		return FALSE;

	at = 0;
	CHECK(shadow_camera_stats_json_string(buffer, sizeof(buffer), &at, nullptr, 0));
	if (!same("null", buffer, "null"))
		return FALSE;

	/* too small: nothing written, the position kept */
	at = 2;
	buffer[0] = 'x';
	buffer[1] = 'y';
	CHECK(!shadow_camera_stats_json_string(buffer, 8, &at, "abcdefgh", 8));
	CHECK(at == 2);
	CHECK(buffer[2] == '\0');
	return TRUE;
}

WINPR_ATTR_NODISCARD
static BOOL test_json_device(void)
{
	ShadowCameraDeviceInfo info = { .name = "Cam \"1\"",
		                            .state = SHADOW_CAMERA_STATS_STREAMING,
		                            .owner = TRUE,
		                            .format = "H264",
		                            .width = 640,
		                            .height = 480,
		                            .fpsNumerator = 30,
		                            .fpsDenominator = 1,
		                            .startsTotal = 2,
		                            .outstandingNow = 1,
		                            .outstandingTargetNow = 2 };
	const ShadowCameraStatsValues v = { .periodMs = 5000,
		                                .samples = 150,
		                                .bytes = 400000,
		                                .kbitPerS = 640,
		                                .frames = 149,
		                                .dropped = 1,
		                                .sampleErrors = 0,
		                                .decodes = 150,
		                                .decodeAvgUs = 2100,
		                                .decodeMaxUs = 5400,
		                                .gapMaxMs = 41,
		                                .outstandingMax = 2,
		                                .framesTotal = 1234 };
	char buffer[1024] = WINPR_C_ARRAY_INIT;
	size_t at = 0;
	CHECK(shadow_camera_stats_json_device(buffer, sizeof(buffer), &at, &info, &v));
	if (!same("streaming", buffer,
	          "{\"name\":\"Cam \\\"1\\\"\",\"state\":\"streaming\",\"owner\":true,\"format\":"
	          "\"H264\",\"width\":640,\"height\":480,\"fps_num\":30,\"fps_den\":1,\"counters\":{"
	          "\"CAMERA_PERIOD_MS\":5000,\"CAMERA_SAMPLES\":150,\"CAMERA_BYTES\":400000,"
	          "\"CAMERA_KBIT_S\":640,\"CAMERA_FRAMES\":149,\"CAMERA_DROPPED\":1,"
	          "\"CAMERA_SAMPLE_ERRORS\":0,\"CAMERA_DECODE_AVG_US\":2100,"
	          "\"CAMERA_DECODE_MAX_US\":5400,\"CAMERA_SAMPLE_GAP_MAX_MS\":41,"
	          "\"CAMERA_OUTSTANDING_MAX\":2,\"CAMERA_OUTSTANDING_NOW\":1,"
	          "\"CAMERA_OUTSTANDING_TARGET_NOW\":2,\"CAMERA_FRAMES_TOTAL\":1234,"
	          "\"CAMERA_STARTS_TOTAL\":2}}"))
		return FALSE;
	CHECK(at == strlen(buffer));

	/* nothing decoded: no decode times */
	ShadowCameraStatsValues idle = v;
	idle.decodes = 0;
	at = 0;
	CHECK(shadow_camera_stats_json_device(buffer, sizeof(buffer), &at, &info, &idle));
	CHECK(strstr(buffer, "CAMERA_DECODE") == nullptr);
	CHECK(strstr(buffer, "\"CAMERA_SAMPLE_ERRORS\":0,\"CAMERA_SAMPLE_GAP_MAX_MS\":41") != nullptr);

	/* refused: a reason, no media type, no counters; appended after what is there */
	const ShadowCameraDeviceInfo refused = { .name = "Other",
		                                     .state = SHADOW_CAMERA_STATS_REFUSED,
		                                     .reason = "another client owns the camera" };
	(void)strcpy(buffer, "[");
	at = 1;
	CHECK(shadow_camera_stats_json_device(buffer, sizeof(buffer), &at, &refused, nullptr));
	if (!same("refused", buffer,
	          "[{\"name\":\"Other\",\"state\":\"refused\",\"owner\":false,\"reason\":\"another "
	          "client owns the camera\"}"))
		return FALSE;

	/* a long name is cut at 64 bytes */
	char longName[90] = WINPR_C_ARRAY_INIT;
	memset(longName, 'n', sizeof(longName) - 1);
	info.name = longName;
	at = 0;
	CHECK(shadow_camera_stats_json_device(buffer, sizeof(buffer), &at, &info, nullptr));
	char expected[200] = WINPR_C_ARRAY_INIT;
	(void)snprintf(expected, sizeof(expected), "{\"name\":\"%.64s\",", longName);
	CHECK(strncmp(buffer, expected, strlen(expected)) == 0);

	/* too small: FALSE, nothing left behind */
	info.name = "Cam";
	at = 0;
	CHECK(!shadow_camera_stats_json_device(buffer, 60, &at, &info, &v));
	CHECK(at == 0);
	CHECK(buffer[0] == '\0');

	/* every state has its name */
	for (int state = SHADOW_CAMERA_STATS_NEGOTIATING; state <= SHADOW_CAMERA_STATS_REFUSED; state++)
		CHECK(strcmp(shadow_camera_stats_state_name((SHADOW_CAMERA_STATS_STATE)state), "unknown") !=
		      0);
	return TRUE;
}

int TestShadowCameraStats(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!test_period() || !test_name_length() || !test_refusals() || !test_json_string() ||
	    !test_json_device())
		return -1;
	return 0;
}
