/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: what each client camera did, for the stats socket
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/string.h>

#include "camera_stats.h"

void shadow_camera_stats_init(ShadowCameraStats* stats, UINT64 nowMs)
{
	WINPR_ASSERT(stats);
	const ShadowCameraStats empty = { .periodStartMs = nowMs };
	*stats = empty;
}

void shadow_camera_stats_stream_started(ShadowCameraStats* stats)
{
	WINPR_ASSERT(stats);
	stats->lastSampleMs = 0;
}

void shadow_camera_stats_sample(ShadowCameraStats* stats, size_t bytes, UINT64 nowMs)
{
	WINPR_ASSERT(stats);
	stats->samples++;
	stats->bytes += bytes;
	if ((stats->lastSampleMs != 0) && (nowMs >= stats->lastSampleMs) &&
	    (nowMs - stats->lastSampleMs > stats->gapMaxMs))
		stats->gapMaxMs = nowMs - stats->lastSampleMs;
	stats->lastSampleMs = nowMs;
}

void shadow_camera_stats_decoded(ShadowCameraStats* stats, UINT64 ns)
{
	WINPR_ASSERT(stats);
	stats->decodes++;
	stats->decodeSumNs += ns;
	if (ns > stats->decodeMaxNs)
		stats->decodeMaxNs = ns;
}

void shadow_camera_stats_shown(ShadowCameraStats* stats)
{
	WINPR_ASSERT(stats);
	stats->frames++;
	stats->framesTotal++;
}

void shadow_camera_stats_dropped(ShadowCameraStats* stats)
{
	WINPR_ASSERT(stats);
	stats->dropped++;
}

void shadow_camera_stats_sample_error(ShadowCameraStats* stats)
{
	WINPR_ASSERT(stats);
	stats->sampleErrors++;
}

void shadow_camera_stats_outstanding(ShadowCameraStats* stats, UINT32 outstanding)
{
	WINPR_ASSERT(stats);
	if (outstanding > stats->outstandingMax)
		stats->outstandingMax = outstanding;
}

static UINT64 div_round(UINT64 value, UINT64 by)
{
	return (by == 0) ? 0 : (value + by / 2) / by;
}

void shadow_camera_stats_take_period(ShadowCameraStats* stats, UINT64 nowMs, BOOL streaming,
                                     ShadowCameraStatsValues* values)
{
	WINPR_ASSERT(stats);
	WINPR_ASSERT(values);

	const UINT64 periodMs = (nowMs > stats->periodStartMs) ? nowMs - stats->periodStartMs : 0;
	UINT64 gap = stats->gapMaxMs;
	/* a stream that went quiet: the time since its last sample is a gap too */
	if (streaming && (stats->lastSampleMs != 0) && (nowMs > stats->lastSampleMs) &&
	    (nowMs - stats->lastSampleMs > gap))
		gap = nowMs - stats->lastSampleMs;

	const ShadowCameraStatsValues v = {
		.periodMs = periodMs,
		.samples = stats->samples,
		.bytes = stats->bytes,
		/* bits per ms = kbit/s */
		.kbitPerS = div_round(stats->bytes * 8ull, periodMs),
		.frames = stats->frames,
		.dropped = stats->dropped,
		.sampleErrors = stats->sampleErrors,
		.decodes = stats->decodes,
		.decodeAvgUs = div_round(div_round(stats->decodeSumNs, stats->decodes), 1000),
		.decodeMaxUs = div_round(stats->decodeMaxNs, 1000),
		.gapMaxMs = gap,
		.outstandingMax = stats->outstandingMax,
		.framesTotal = stats->framesTotal,
	};
	*values = v;

	const ShadowCameraStats next = { .periodStartMs = nowMs,
		                             .lastSampleMs = stats->lastSampleMs,
		                             .framesTotal = stats->framesTotal };
	*stats = next;
}

size_t shadow_camera_stats_name_length(const char* name)
{
	if (!name)
		return 0;
	size_t length = strnlen(name, SHADOW_CAMERA_STATS_NAME_MAX + 1);
	if (length <= SHADOW_CAMERA_STATS_NAME_MAX)
		return length;
	/* cut before the sequence the limit falls into: back over its continuation bytes, then
	 * over its lead byte */
	length = SHADOW_CAMERA_STATS_NAME_MAX;
	while ((length > 0) && ((((const BYTE*)name)[length] & 0xC0) == 0x80))
		length--;
	return length;
}

void shadow_camera_refusals_add(ShadowCameraRefusals* refusals, const char* name,
                                const char* reason)
{
	WINPR_ASSERT(refusals);
	const size_t slot = refusals->next % SHADOW_CAMERA_STATS_REFUSALS;
	const size_t length = shadow_camera_stats_name_length(name);
	if (length > 0)
		memcpy(refusals->names[slot], name, length);
	refusals->names[slot][length] = '\0';
	refusals->reasons[slot] = reason;
	refusals->next = (slot + 1) % SHADOW_CAMERA_STATS_REFUSALS;
	if (refusals->count < SHADOW_CAMERA_STATS_REFUSALS)
		refusals->count++;
}

const char* shadow_camera_stats_state_name(SHADOW_CAMERA_STATS_STATE state)
{
	switch (state)
	{
		case SHADOW_CAMERA_STATS_NEGOTIATING:
			return "negotiating";
		case SHADOW_CAMERA_STATS_IDLE:
			return "idle";
		case SHADOW_CAMERA_STATS_STARTING:
			return "starting";
		case SHADOW_CAMERA_STATS_STREAMING:
			return "streaming";
		case SHADOW_CAMERA_STATS_STOPPING:
			return "stopping";
		case SHADOW_CAMERA_STATS_FAILED:
			return "failed";
		case SHADOW_CAMERA_STATS_REFUSED:
			return "refused";
		default:
			return "unknown";
	}
}

WINPR_ATTR_FORMAT_ARG(4, 5)
static BOOL append(char* buffer, size_t size, size_t* at, WINPR_FORMAT_ARG const char* format, ...)
{
	if (*at >= size)
		return FALSE;
	va_list args = WINPR_C_ARRAY_INIT;
	va_start(args, format);
	const int rc = vsnprintf(&buffer[*at], size - *at, format, args);
	va_end(args);
	if ((rc < 0) || ((size_t)rc >= size - *at))
	{
		buffer[*at] = '\0';
		return FALSE;
	}
	*at += (size_t)rc;
	return TRUE;
}

BOOL shadow_camera_stats_json_string(char* buffer, size_t size, size_t* at, const char* text,
                                     size_t length)
{
	WINPR_ASSERT(buffer);
	WINPR_ASSERT(at);

	const size_t start = *at;
	if (!text)
		return append(buffer, size, at, "null");

	BOOL ok = append(buffer, size, at, "\"");
	for (size_t x = 0; ok && (x < length) && (text[x] != '\0'); x++)
	{
		const unsigned char c = (unsigned char)text[x];
		if ((c == '"') || (c == '\\'))
			ok = append(buffer, size, at, "\\%c", c);
		else if (c < 0x20)
			ok = append(buffer, size, at, "\\u%04x", (unsigned)c);
		else
			ok = append(buffer, size, at, "%c", c);
	}
	ok = ok && append(buffer, size, at, "\"");
	if (!ok)
	{
		*at = start;
		if (start < size)
			buffer[start] = '\0';
	}
	return ok;
}

BOOL shadow_camera_stats_json_device(char* buffer, size_t size, size_t* at,
                                     const ShadowCameraDeviceInfo* info,
                                     const ShadowCameraStatsValues* values)
{
	WINPR_ASSERT(buffer);
	WINPR_ASSERT(at);
	WINPR_ASSERT(info);

	const size_t start = *at;
	BOOL ok = append(buffer, size, at, "{\"name\":") &&
	          shadow_camera_stats_json_string(buffer, size, at, info->name ? info->name : "",
	                                          shadow_camera_stats_name_length(info->name)) &&
	          append(buffer, size, at, ",\"state\":\"%s\",\"owner\":%s",
	                 shadow_camera_stats_state_name(info->state), info->owner ? "true" : "false");
	if (ok && info->reason)
		ok = append(buffer, size, at, ",\"reason\":") &&
		     shadow_camera_stats_json_string(buffer, size, at, info->reason, strlen(info->reason));
	if (ok && info->format)
		ok =
		    append(buffer, size, at, ",\"format\":") &&
		    shadow_camera_stats_json_string(buffer, size, at, info->format, strlen(info->format)) &&
		    append(buffer, size, at,
		           ",\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"fps_num\":%" PRIu32
		           ",\"fps_den\":%" PRIu32,
		           info->width, info->height, info->fpsNumerator, info->fpsDenominator);
	if (ok && values)
	{
		ok = append(buffer, size, at,
		            ",\"counters\":{\"CAMERA_PERIOD_MS\":%" PRIu64 ",\"CAMERA_SAMPLES\":%" PRIu64
		            ",\"CAMERA_BYTES\":%" PRIu64 ",\"CAMERA_KBIT_S\":%" PRIu64
		            ",\"CAMERA_FRAMES\":%" PRIu64 ",\"CAMERA_DROPPED\":%" PRIu64
		            ",\"CAMERA_SAMPLE_ERRORS\":%" PRIu64,
		            values->periodMs, values->samples, values->bytes, values->kbitPerS,
		            values->frames, values->dropped, values->sampleErrors);
		/* the decode times only when something was decoded */
		if (ok && (values->decodes > 0))
			ok = append(buffer, size, at,
			            ",\"CAMERA_DECODE_AVG_US\":%" PRIu64 ",\"CAMERA_DECODE_MAX_US\":%" PRIu64,
			            values->decodeAvgUs, values->decodeMaxUs);
		ok = ok &&
		     append(buffer, size, at,
		            ",\"CAMERA_SAMPLE_GAP_MAX_MS\":%" PRIu64 ",\"CAMERA_OUTSTANDING_MAX\":%" PRIu32
		            ",\"CAMERA_OUTSTANDING_NOW\":%" PRIu32
		            ",\"CAMERA_OUTSTANDING_TARGET_NOW\":%" PRIu32
		            ",\"CAMERA_FRAMES_TOTAL\":%" PRIu64 ",\"CAMERA_STARTS_TOTAL\":%" PRIu32 "}",
		            values->gapMaxMs, values->outstandingMax, info->outstandingNow,
		            info->outstandingTargetNow, values->framesTotal, info->startsTotal);
	}
	ok = ok && append(buffer, size, at, "}");
	if (!ok)
	{
		*at = start;
		if (start < size)
			buffer[start] = '\0';
	}
	return ok;
}
