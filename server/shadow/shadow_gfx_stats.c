/**
 * FreeRDP: A Remote Desktop Protocol Implementation
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
#include <time.h>

#include <winpr/assert.h>
#include <winpr/wlog.h>
#include <freerdp/types.h>

#include "shadow_gfx_stats.h"

static UINT64 add_saturated(UINT64 a, UINT64 b)
{
	return (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
}

/* value / divisor rounded to the nearest, without an overflow */
static UINT64 div_round(UINT64 value, UINT64 divisor)
{
	WINPR_ASSERT(divisor > 0);
	return value / divisor + (((value % divisor) >= (divisor - divisor / 2)) ? 1 : 0);
}

/* count per second in tenths, rounded; saturates instead of wrapping */
static UINT64 rate_tenths(UINT64 count, UINT64 elapsedMs)
{
	WINPR_ASSERT(elapsedMs > 0);
	if (count <= (UINT64_MAX / 10000))
		return div_round(count * 10000, elapsedMs);
	const UINT64 perMs = count / elapsedMs;
	return (perMs <= (UINT64_MAX / 10000)) ? perMs * 10000 : UINT64_MAX;
}

void shadow_gfx_stats_frame_sent(SHADOW_GFX_STATS* stats, UINT32 frameId, UINT64 startNs,
                                 UINT64 endNs)
{
	WINPR_ASSERT(stats);
	WINPR_STATIC_ASSERT((SHADOW_GFX_STATS_RING & (SHADOW_GFX_STATS_RING - 1)) == 0);

	const UINT64 encodeNs = (endNs >= startNs) ? endNs - startNs : 0;
	if (stats->sent == 0)
		stats->firstFrameId = frameId;
	stats->lastFrameId = frameId;
	stats->sent++;
	stats->encodeSumNs = add_saturated(stats->encodeSumNs, encodeNs);
	stats->encodeMaxNs = MAX(stats->encodeMaxNs, encodeNs);

	SHADOW_GFX_SENT_FRAME* slot = &stats->ring[frameId & (SHADOW_GFX_STATS_RING - 1)];
	slot->frameId = frameId;
	slot->sentNs = endNs;
}

void shadow_gfx_stats_capture(SHADOW_GFX_STATS* stats, UINT64 captureNs)
{
	WINPR_ASSERT(stats);
	stats->captures++;
	stats->captureSumNs = add_saturated(stats->captureSumNs, captureNs);
	stats->captureMaxNs = MAX(stats->captureMaxNs, captureNs);
}

void shadow_gfx_stats_round(SHADOW_GFX_STATS* stats, UINT64 startNs, UINT64 endNs)
{
	WINPR_ASSERT(stats);
	if ((startNs == 0) || (endNs < startNs))
		return;
	const UINT64 roundNs = endNs - startNs;
	stats->rounds++;
	stats->roundSumNs = add_saturated(stats->roundSumNs, roundNs);
	stats->roundMaxNs = MAX(stats->roundMaxNs, roundNs);
}

UINT64 shadow_gfx_stats_frame_acked(SHADOW_GFX_STATS* stats, UINT32 frameId, UINT64 ackNs)
{
	WINPR_ASSERT(stats);

	stats->acks++;

	/* a frame sent so long ago that a newer one took its place, a frame the client acknowledges
	 * twice, or a frame id that was never sent has no delay */
	SHADOW_GFX_SENT_FRAME* slot = &stats->ring[frameId & (SHADOW_GFX_STATS_RING - 1)];
	if ((slot->sentNs == 0) || (slot->frameId != frameId))
		return 0;
	const UINT64 sentNs = slot->sentNs;
	slot->sentNs = 0;
	if (ackNs < sentNs)
		return 0;

	const UINT64 delayNs = ackNs - sentNs;
	stats->ackDelays++;
	stats->ackDelaySumNs = add_saturated(stats->ackDelaySumNs, delayNs);
	stats->ackDelayMaxNs = MAX(stats->ackDelayMaxNs, delayNs);
	if ((stats->ackDelayMinNs == 0) || (delayNs < stats->ackDelayMinNs))
		stats->ackDelayMinNs = delayNs;
	return delayNs;
}

UINT32 shadow_gfx_window(UINT64 baseAckNs, UINT32 rate)
{
	if ((baseAckNs == 0) || (rate == 0))
		return SHADOW_GFX_WINDOW_MIN;
	/* the frames acknowledged during one round trip, rounded up; a delay of seconds or a rate
	 * of thousands caps first, so the product stays far below UINT64_MAX */
	const UINT64 perDelay =
	    (MIN(baseAckNs, 10000000000ull) * MIN(rate, 1000u) + 999999999ull) / 1000000000ull;
	const UINT64 window = perDelay + SHADOW_GFX_WINDOW_SPARE;
	return (UINT32)MIN(MAX(window, SHADOW_GFX_WINDOW_MIN), SHADOW_GFX_WINDOW_MAX);
}

UINT32 shadow_gfx_window_next(UINT32 window, UINT32 peak, UINT64 baseAckNs, UINT64 avgAckNs,
                              UINT32 rate)
{
	UINT32 next = MIN(MAX(window, SHADOW_GFX_WINDOW_MIN), SHADOW_GFX_WINDOW_MAX);
	if ((baseAckNs == 0) || (avgAckNs == 0))
		return next;
	const UINT64 base = MIN(baseAckNs, 10000000000ull);
	const UINT64 avg = MIN(avgAckNs, 10000000000ull);
	if ((avg * 4 <= base * 5) && (peak + 1 >= next))
		next = MIN(next + SHADOW_GFX_WINDOW_SPARE, SHADOW_GFX_WINDOW_MAX);
	else if (avg * 2 > base * 3)
		next = shadow_gfx_window(base, rate);
	return next;
}

BOOL shadow_gfx_stats_local_time(UINT64 unixMs, char* buffer, size_t size)
{
	if (!buffer || (size == 0))
		return FALSE;
	const time_t t = (time_t)(unixMs / 1000ull);
	struct tm local = WINPR_C_ARRAY_INIT;
#if defined(_WIN32)
	if (localtime_s(&local, &t) != 0)
		return FALSE;
#else
	if (!localtime_r(&t, &local))
		return FALSE;
#endif
	char text[40] = WINPR_C_ARRAY_INIT;
	const size_t n = strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S%z", &local);
	/* %z is "-0500": ISO 8601 writes "-05:00" */
	if ((n < 5) || ((text[n - 5] != '+') && (text[n - 5] != '-')))
		return FALSE;
	const int rc = snprintf(buffer, size, "%.*s:%s", (int)(n - 2), text, &text[n - 2]);
	return (rc > 0) && ((size_t)rc < size);
}

void shadow_gfx_stats_values(const SHADOW_GFX_STATS* stats, UINT32 inFlightLimit, UINT64 elapsedMs,
                             UINT32 lastAckedFrameId, SHADOW_GFX_VALUES* values)
{
	WINPR_ASSERT(stats);
	WINPR_ASSERT(values);

	const SHADOW_GFX_VALUES v = {
		.periodMs = elapsedMs,
		.captured = stats->captured,
		.sent = stats->sent,
		.held = stats->held,
		.merged = stats->merged,
		.acks = stats->acks,
		.ackDelayAvgMs =
		    stats->ackDelays ? div_round(stats->ackDelaySumNs / stats->ackDelays, 1000000) : 0,
		.ackDelayMaxMs = div_round(stats->ackDelayMaxNs, 1000000),
		.ackDelayMinMs = div_round(stats->ackDelayMinNs, 1000000),
		.encodeAvgUs = stats->sent ? div_round(stats->encodeSumNs / stats->sent, 1000) : 0,
		.encodeMaxUs = div_round(stats->encodeMaxNs, 1000),
		.inFlightMax = stats->maxInFlight,
		.inFlightLimit = inFlightLimit,
		.frameIdFirst = stats->sent ? stats->firstFrameId : 0,
		.frameIdLast = stats->sent ? stats->lastFrameId : 0,
		.frameIdAckedLast = lastAckedFrameId,
		.captures = stats->captures,
		.captureAvgUs =
		    stats->captures ? div_round(stats->captureSumNs / stats->captures, 1000) : 0,
		.captureMaxUs = div_round(stats->captureMaxNs, 1000),
		.rounds = stats->rounds,
		.roundAvgUs = stats->rounds ? div_round(stats->roundSumNs / stats->rounds, 1000) : 0,
		.roundMaxUs = div_round(stats->roundMaxNs, 1000),
	};
	*values = v;
}

BOOL shadow_gfx_stats_json(const SHADOW_GFX_VALUES* values, const SHADOW_GFX_PERIOD_INFO* info,
                           char* buffer, size_t size)
{
	WINPR_ASSERT(values);
	WINPR_ASSERT(info);

	if (!buffer || (size == 0))
		return FALSE;
	/* an address has no character JSON would escape; anything else is left out */
	const char* peer = info->peer;
	if (peer && ((*peer == '\0') || (strspn(peer, "0123456789abcdefABCDEF.:[]") != strlen(peer))))
		peer = nullptr;
	/* the first and the last frame sent, both inclusive; none when the period sent none */
	char frames[96] = WINPR_C_ARRAY_INIT;
	if (values->sent > 0)
		(void)snprintf(frames, sizeof(frames),
		               ",\"GFX_FRAME_ID_FIRST\":%" PRIu64 ",\"GFX_FRAME_ID_LAST\":%" PRIu64,
		               values->frameIdFirst, values->frameIdLast);
	/* the capture and the round times, when the subsystem tells them */
	char times[160] = WINPR_C_ARRAY_INIT;
	int at = 0;
	if (values->captures > 0)
		at = snprintf(times, sizeof(times),
		              ",\"GFX_CAPTURE_AVG_US\":%" PRIu64 ",\"GFX_CAPTURE_MAX_US\":%" PRIu64,
		              values->captureAvgUs, values->captureMaxUs);
	if ((values->rounds > 0) && (at >= 0) && ((size_t)at < sizeof(times)))
		(void)snprintf(&times[at], sizeof(times) - (size_t)at,
		               ",\"GFX_ROUND_AVG_US\":%" PRIu64 ",\"GFX_ROUND_MAX_US\":%" PRIu64,
		               values->roundAvgUs, values->roundMaxUs);
	const char* startedAt = info->startedAt;
	if (startedAt &&
	    ((*startedAt == '\0') || (strspn(startedAt, "0123456789-+:T") != strlen(startedAt))))
		startedAt = nullptr;
	const int rc = snprintf(
	    buffer, size,
	    "{\"connection_id\":\"%016" PRIx64 "\",\"peer\":%s%s%s,\"connected_ms\":%" PRIu64
	    ",\"started_at\":%s%s%s"
	    ",\"periods\":%" PRIu64 ",\"time_ms\":%" PRIu64 ",\"ns\":%" PRIu64 ",\"period_ms\":%" PRIu64
	    ",\"acks_suspended\":%s,\"fps\":%" PRIu32 ",\"max_fps\":%" PRIu32
	    ",\"counters\":{\"GFX_PERIOD_MS\":%" PRIu64 ",\"GFX_CAPTURED\":%" PRIu64
	    ",\"GFX_SENT\":%" PRIu64 ",\"GFX_HELD\":%" PRIu64 ",\"GFX_MERGED\":%" PRIu64
	    ",\"GFX_ACKS\":%" PRIu64 ",\"GFX_ACK_DELAY_AVG_MS\":%" PRIu64
	    ",\"GFX_ACK_DELAY_MAX_MS\":%" PRIu64 ",\"GFX_ACK_DELAY_MIN_MS\":%" PRIu64
	    ",\"GFX_ENCODE_AVG_US\":%" PRIu64 ",\"GFX_ENCODE_MAX_US\":%" PRIu64
	    ",\"GFX_IN_FLIGHT_MAX\":%" PRIu64 ",\"GFX_IN_FLIGHT_LIMIT\":%" PRIu64
	    "%s%s,\"GFX_FRAME_ID_ACKED_LAST\":%" PRIu64 "}}",
	    info->connectionId, peer ? "\"" : "", peer ? peer : "null", peer ? "\"" : "",
	    info->connectedMs, startedAt ? "\"" : "", startedAt ? startedAt : "null",
	    startedAt ? "\"" : "", info->periods, info->timeMs, info->ns, values->periodMs,
	    info->acksSuspended ? "true" : "false", info->fps, info->maxFps, values->periodMs,
	    values->captured, values->sent, values->held, values->merged, values->acks,
	    values->ackDelayAvgMs, values->ackDelayMaxMs, values->ackDelayMinMs, values->encodeAvgUs,
	    values->encodeMaxUs, values->inFlightMax, values->inFlightLimit, frames, times,
	    values->frameIdAckedLast);
	return (rc > 0) && ((size_t)rc < size);
}

size_t shadow_gfx_stats_report(const SHADOW_GFX_VALUES* values, UINT64 period, ASTATS_VALUE* report,
                               size_t size)
{
	WINPR_ASSERT(values);
	if (!report)
		return 0;
	size_t count = 0;
#define ASTATS_ADD(id_, value_)             \
	do                                      \
	{                                       \
		if (count < size)                   \
		{                                   \
			report[count].id = (id_);       \
			report[count].value = (value_); \
			count++;                        \
		}                                   \
	} while (0)
	ASTATS_ADD(ASTATS_ID_PERIOD_MS, values->periodMs);
	ASTATS_ADD(ASTATS_ID_CAPTURED, values->captured);
	ASTATS_ADD(ASTATS_ID_SENT, values->sent);
	ASTATS_ADD(ASTATS_ID_HELD, values->held);
	ASTATS_ADD(ASTATS_ID_MERGED, values->merged);
	ASTATS_ADD(ASTATS_ID_ACKS, values->acks);
	ASTATS_ADD(ASTATS_ID_ACK_DELAY_AVG_MS, values->ackDelayAvgMs);
	ASTATS_ADD(ASTATS_ID_ACK_DELAY_MAX_MS, values->ackDelayMaxMs);
	ASTATS_ADD(ASTATS_ID_ACK_DELAY_MIN_MS, values->ackDelayMinMs);
	ASTATS_ADD(ASTATS_ID_ENCODE_AVG_US, values->encodeAvgUs);
	ASTATS_ADD(ASTATS_ID_ENCODE_MAX_US, values->encodeMaxUs);
	ASTATS_ADD(ASTATS_ID_IN_FLIGHT_MAX, values->inFlightMax);
	ASTATS_ADD(ASTATS_ID_IN_FLIGHT_LIMIT, values->inFlightLimit);
	if (values->sent > 0)
	{
		ASTATS_ADD(ASTATS_ID_FRAME_ID_FIRST, values->frameIdFirst);
		ASTATS_ADD(ASTATS_ID_FRAME_ID_LAST, values->frameIdLast);
	}
	ASTATS_ADD(ASTATS_ID_FRAME_ID_ACKED_LAST, values->frameIdAckedLast);
	if (values->captures > 0)
	{
		ASTATS_ADD(ASTATS_ID_CAPTURE_AVG_US, values->captureAvgUs);
		ASTATS_ADD(ASTATS_ID_CAPTURE_MAX_US, values->captureMaxUs);
	}
	if (values->rounds > 0)
	{
		ASTATS_ADD(ASTATS_ID_ROUND_AVG_US, values->roundAvgUs);
		ASTATS_ADD(ASTATS_ID_ROUND_MAX_US, values->roundMaxUs);
	}
	ASTATS_ADD(ASTATS_ID_PERIOD_NUMBER, period);
#undef ASTATS_ADD
	return count;
}

void shadow_gfx_stats_next_period(SHADOW_GFX_STATS* stats)
{
	WINPR_ASSERT(stats);

	SHADOW_GFX_SENT_FRAME ring[SHADOW_GFX_STATS_RING];
	memcpy(ring, stats->ring, sizeof(ring));
	memset(stats, 0, sizeof(*stats));
	memcpy(stats->ring, ring, sizeof(ring));
}

void shadow_gfx_stats_forget_frames(SHADOW_GFX_STATS* stats)
{
	WINPR_ASSERT(stats);
	memset(stats->ring, 0, sizeof(stats->ring));
}

BOOL shadow_gfx_stats_debug(void)
{
	wLog* log = WLog_Get(SHADOW_GFX_STATS_TAG);
	return log && WLog_IsLevelActive(log, WLOG_DEBUG);
}

void shadow_gfx_stats_write(const SHADOW_GFX_STATS* stats, UINT32 inFlightLimit, BOOL acksSuspended,
                            UINT64 elapsedMs)
{
	WINPR_ASSERT(stats);

	/* the logger by name, not through WLog_INFO: WLog_INFO keeps the first logger a source file
	 * asks for and uses it for every tag after it */
	wLog* log = WLog_Get(SHADOW_GFX_STATS_TAG);
	if (!log || !WLog_IsLevelActive(log, WLOG_INFO))
		return;
	char line[512] = WINPR_C_ARRAY_INIT;
	if (shadow_gfx_stats_format(stats, inFlightLimit, acksSuspended, elapsedMs, line, sizeof(line)))
		WLog_Print(log, WLOG_INFO, "%s", line);
}

BOOL shadow_gfx_stats_format(const SHADOW_GFX_STATS* stats, UINT32 inFlightLimit,
                             BOOL acksSuspended, UINT64 elapsedMs, char* buffer, size_t size)
{
	WINPR_ASSERT(stats);

	if (!buffer || (size == 0))
		return FALSE;

	char captured[32] = "-";
	char sent[32] = "-";
	if (elapsedMs > 0)
	{
		const UINT64 c = rate_tenths(stats->captured, elapsedMs);
		const UINT64 s = rate_tenths(stats->sent, elapsedMs);
		(void)snprintf(captured, sizeof(captured), "%" PRIu64 ".%" PRIu64, c / 10, c % 10);
		(void)snprintf(sent, sizeof(sent), "%" PRIu64 ".%" PRIu64, s / 10, s % 10);
	}

	char delay[64] = "-";
	if (stats->ackDelays > 0)
		(void)snprintf(delay, sizeof(delay), "avg %" PRIu64 " ms max %" PRIu64 " ms",
		               div_round(stats->ackDelaySumNs / stats->ackDelays, 1000000),
		               div_round(stats->ackDelayMaxNs, 1000000));

	char encode[64] = "-";
	if (stats->sent > 0)
	{
		const UINT64 avg = div_round(stats->encodeSumNs / stats->sent, 100000);
		const UINT64 max = div_round(stats->encodeMaxNs, 100000);
		(void)snprintf(encode, sizeof(encode),
		               "avg %" PRIu64 ".%" PRIu64 " ms max %" PRIu64 ".%" PRIu64 " ms", avg / 10,
		               avg % 10, max / 10, max % 10);
	}

	char times[96] = WINPR_C_ARRAY_INIT;
	if (stats->captures > 0)
	{
		const UINT64 avg = div_round(stats->captureSumNs / stats->captures, 100000);
		const UINT64 max = div_round(stats->captureMaxNs, 100000);
		(void)snprintf(times, sizeof(times),
		               ", capture avg %" PRIu64 ".%" PRIu64 " ms max %" PRIu64 ".%" PRIu64 " ms",
		               avg / 10, avg % 10, max / 10, max % 10);
	}
	if (stats->rounds > 0)
	{
		const UINT64 avg = div_round(stats->roundSumNs / stats->rounds, 100000);
		const UINT64 max = div_round(stats->roundMaxNs, 100000);
		const size_t used = strlen(times);
		(void)snprintf(&times[used], sizeof(times) - used,
		               ", round avg %" PRIu64 ".%" PRIu64 " ms max %" PRIu64 ".%" PRIu64 " ms",
		               avg / 10, avg % 10, max / 10, max % 10);
	}

	char limit[32] = " (no limit)";
	if (inFlightLimit > 0)
		(void)snprintf(limit, sizeof(limit), " of %" PRIu32, inFlightLimit);

	const UINT64 seconds = div_round(elapsedMs, 100);
	const int rc =
	    snprintf(buffer, size,
	             "gfx %" PRIu64 ".%" PRIu64 " s: captured %" PRIu64 " (%s/s), sent %" PRIu64
	             " (%s/s), held %" PRIu64 ", merged %" PRIu64 ", acks %" PRIu64
	             "%s, ack delay %s, encode %s, in flight max %" PRIu32 "%s%s",
	             seconds / 10, seconds % 10, stats->captured, captured, stats->sent, sent,
	             stats->held, stats->merged, stats->acks, acksSuspended ? " (suspended)" : "",
	             delay, encode, stats->maxInFlight, limit, times);
	return (rc > 0) && ((size_t)rc < size);
}
