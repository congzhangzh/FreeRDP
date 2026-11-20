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
	stats->sent++;
	stats->encodeSumNs = add_saturated(stats->encodeSumNs, encodeNs);
	stats->encodeMaxNs = MAX(stats->encodeMaxNs, encodeNs);

	SHADOW_GFX_SENT_FRAME* slot = &stats->ring[frameId & (SHADOW_GFX_STATS_RING - 1)];
	slot->frameId = frameId;
	slot->sentNs = endNs;
}

void shadow_gfx_stats_frame_acked(SHADOW_GFX_STATS* stats, UINT32 frameId, UINT64 ackNs)
{
	WINPR_ASSERT(stats);

	stats->acks++;

	/* a frame sent so long ago that a newer one took its place, a frame the client acknowledges
	 * twice, or a frame id that was never sent has no delay */
	SHADOW_GFX_SENT_FRAME* slot = &stats->ring[frameId & (SHADOW_GFX_STATS_RING - 1)];
	if ((slot->sentNs == 0) || (slot->frameId != frameId))
		return;
	const UINT64 sentNs = slot->sentNs;
	slot->sentNs = 0;
	if (ackNs < sentNs)
		return;

	const UINT64 delayNs = ackNs - sentNs;
	stats->ackDelays++;
	stats->ackDelaySumNs = add_saturated(stats->ackDelaySumNs, delayNs);
	stats->ackDelayMaxNs = MAX(stats->ackDelayMaxNs, delayNs);
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

	char limit[32] = " (no limit)";
	if (inFlightLimit > 0)
		(void)snprintf(limit, sizeof(limit), " of %" PRIu32, inFlightLimit);

	const UINT64 seconds = div_round(elapsedMs, 100);
	const int rc =
	    snprintf(buffer, size,
	             "gfx %" PRIu64 ".%" PRIu64 " s: captured %" PRIu64 " (%s/s), sent %" PRIu64
	             " (%s/s), held %" PRIu64 ", merged %" PRIu64 ", acks %" PRIu64
	             "%s, ack delay %s, encode %s, in flight max %" PRIu32 "%s",
	             seconds / 10, seconds % 10, stats->captured, captured, stats->sent, sent,
	             stats->held, stats->merged, stats->acks, acksSuspended ? " (suspended)" : "",
	             delay, encode, stats->maxInFlight, limit);
	return (rc > 0) && ((size_t)rc < size);
}
