#include <stdio.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

#include "../shadow_gfx_stats.h"

#define MS (1000ull * 1000ull)

WINPR_ATTR_NODISCARD
static BOOL expect_line(const char* what, const SHADOW_GFX_STATS* stats, UINT32 limit,
                        BOOL suspended, UINT64 elapsedMs, const char* expected)
{
	char line[512] = { 0 };
	if (!shadow_gfx_stats_format(stats, limit, suspended, elapsedMs, line, sizeof(line)))
	{
		(void)fprintf(stderr, "%s: the line does not fit\n", what);
		return FALSE;
	}
	if (strcmp(line, expected) != 0)
	{
		(void)fprintf(stderr, "%s:\n  got      '%s'\n  expected '%s'\n", what, line, expected);
		return FALSE;
	}
	(void)printf("%s: %s\n", what, line);
	return TRUE;
}

WINPR_ATTR_NODISCARD
static BOOL expect_acks(const char* what, const SHADOW_GFX_STATS* stats, UINT64 acks, UINT64 delays,
                        UINT64 sumNs, UINT64 maxNs)
{
	if ((stats->acks == acks) && (stats->ackDelays == delays) && (stats->ackDelaySumNs == sumNs) &&
	    (stats->ackDelayMaxNs == maxNs))
		return TRUE;
	(void)fprintf(stderr,
	              "%s: acks %" PRIu64 " delays %" PRIu64 " sum %" PRIu64 " max %" PRIu64
	              ", expected %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
	              what, stats->acks, stats->ackDelays, stats->ackDelaySumNs, stats->ackDelayMaxNs,
	              acks, delays, sumNs, maxNs);
	return FALSE;
}

/* A session at 30 fps: 150 rounds, 148 frames, the client acknowledges all but the last. */
WINPR_ATTR_NODISCARD
static BOOL test_normal(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	stats.captured = 150;
	stats.held = 2;
	stats.merged = 2;
	stats.maxInFlight = 3;
	for (UINT32 id = 1; id <= 148; id++)
	{
		const UINT64 start = 1000 * MS + 33ull * MS * id;
		const UINT64 end = start + ((id == 50) ? 21 : 9) * MS;
		shadow_gfx_stats_frame_sent(&stats, id, start, end);
		if (id < 148)
			shadow_gfx_stats_frame_acked(&stats, id, end + ((id == 70) ? 40 : 12) * MS);
	}
	return expect_line("normal", &stats, 8, FALSE, 5000,
	                   "gfx 5.0 s: captured 150 (30.0/s), sent 148 (29.6/s), held 2, merged 2, "
	                   "acks 147, ack delay avg 12 ms max 40 ms, encode avg 9.1 ms max 21.0 ms, in "
	                   "flight max 3 of 8");
}

/* Nothing sent: no division by zero, the averages are left out. */
WINPR_ATTR_NODISCARD
static BOOL test_zero_sent(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	if (!expect_line("empty", &stats, 8, FALSE, 5000,
	                 "gfx 5.0 s: captured 0 (0.0/s), sent 0 (0.0/s), held 0, merged 0, acks 0, ack "
	                 "delay -, encode -, in flight max 0 of 8"))
		return FALSE;

	stats.captured = 10;
	return expect_line("zero sent", &stats, 8, FALSE, 5000,
	                   "gfx 5.0 s: captured 10 (2.0/s), sent 0 (0.0/s), held 0, merged 0, acks 0, "
	                   "ack delay -, encode -, in flight max 0 of 8");
}

/* No time passed: no rates; no limit on the frames in flight. */
WINPR_ATTR_NODISCARD
static BOOL test_elapsed_zero(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	stats.captured = 3;
	shadow_gfx_stats_frame_sent(&stats, 1, 10 * MS, 12 * MS);
	return expect_line("elapsed 0", &stats, 0, FALSE, 0,
	                   "gfx 0.0 s: captured 3 (-/s), sent 1 (-/s), held 0, merged 0, acks 0, ack "
	                   "delay -, encode avg 2.0 ms max 2.0 ms, in flight max 0 (no limit)");
}

/* More frames than the ring holds: an acknowledgement for a frame whose place a newer one took,
 * a repeated one and one for a frame never sent count without a delay; the frame ids wrap. */
WINPR_ATTR_NODISCARD
static BOOL test_ring_wrap(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	for (UINT32 id = 1; id <= 100; id++)
		shadow_gfx_stats_frame_sent(&stats, id, id * MS, id * MS);

	shadow_gfx_stats_frame_acked(&stats, 10, 200 * MS); /* its place holds frame 74 */
	if (!expect_acks("overwritten", &stats, 1, 0, 0, 0))
		return FALSE;
	shadow_gfx_stats_frame_acked(&stats, 90, 95 * MS);
	if (!expect_acks("in the ring", &stats, 2, 1, 5 * MS, 5 * MS))
		return FALSE;
	shadow_gfx_stats_frame_acked(&stats, 90, 99 * MS);
	if (!expect_acks("twice", &stats, 3, 1, 5 * MS, 5 * MS))
		return FALSE;
	shadow_gfx_stats_frame_acked(&stats, 101, 99 * MS);
	if (!expect_acks("never sent", &stats, 4, 1, 5 * MS, 5 * MS))
		return FALSE;
	shadow_gfx_stats_frame_acked(&stats, 0, 99 * MS); /* its place holds frame 64 */
	if (!expect_acks("frame 0 not sent yet", &stats, 5, 1, 5 * MS, 5 * MS))
		return FALSE;

	/* frame ids past UINT32_MAX start at 0 again */
	shadow_gfx_stats_frame_sent(&stats, UINT32_MAX, 300 * MS, 300 * MS);
	shadow_gfx_stats_frame_sent(&stats, 0, 301 * MS, 301 * MS);
	shadow_gfx_stats_frame_acked(&stats, UINT32_MAX, 307 * MS);
	shadow_gfx_stats_frame_acked(&stats, 0, 341 * MS);
	if (!expect_acks("frame ids wrap", &stats, 7, 3, 52 * MS, 40 * MS))
		return FALSE;
	return expect_line(
	    "ring wrap", &stats, 8, FALSE, 5000,
	    "gfx 5.0 s: captured 0 (0.0/s), sent 102 (20.4/s), held 0, merged 0, acks 7, "
	    "ack delay avg 17 ms max 40 ms, encode avg 0.0 ms max 0.0 ms, in flight max 0 "
	    "of 8");
}

/* A client that suspended its acknowledgements, a clock that went back, a new period that keeps
 * the frames in flight. */
WINPR_ATTR_NODISCARD
static BOOL test_suspended_and_periods(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	shadow_gfx_stats_frame_sent(&stats, 7, 50 * MS, 60 * MS);
	shadow_gfx_stats_frame_acked(&stats, 7, 59 * MS);
	if (!expect_acks("clock back", &stats, 1, 0, 0, 0))
		return FALSE;
	if (!expect_line("suspended", &stats, 8, TRUE, 5000,
	                 "gfx 5.0 s: captured 0 (0.0/s), sent 1 (0.2/s), held 0, merged 0, acks 1 "
	                 "(suspended), ack delay -, encode avg 10.0 ms max 10.0 ms, in flight max 0 of "
	                 "8"))
		return FALSE;

	shadow_gfx_stats_frame_sent(&stats, 8, 1000 * MS, 1000 * MS);
	shadow_gfx_stats_next_period(&stats);
	if ((stats.sent != 0) || (stats.acks != 0) || (stats.encodeMaxNs != 0))
	{
		(void)fprintf(stderr, "next period: the counters are not zero\n");
		return FALSE;
	}
	shadow_gfx_stats_frame_acked(&stats, 8, 1020 * MS);
	if (!expect_acks("next period", &stats, 1, 1, 20 * MS, 20 * MS))
		return FALSE;

	shadow_gfx_stats_frame_sent(&stats, 9, 1100 * MS, 1100 * MS);
	shadow_gfx_stats_forget_frames(&stats);
	shadow_gfx_stats_frame_acked(&stats, 9, 1120 * MS);
	return expect_acks("forgotten", &stats, 2, 1, 20 * MS, 20 * MS);
}

/* Huge values saturate, a short buffer is refused. */
WINPR_ATTR_NODISCARD
static BOOL test_limits(void)
{
	SHADOW_GFX_STATS stats = { 0 };
	stats.captured = UINT64_MAX;
	shadow_gfx_stats_frame_sent(&stats, 1, 0, UINT64_MAX);
	shadow_gfx_stats_frame_sent(&stats, 2, 0, UINT64_MAX);
	if (stats.encodeSumNs != UINT64_MAX)
	{
		(void)fprintf(stderr, "limits: the encode sum wrapped\n");
		return FALSE;
	}
	char line[512] = { 0 };
	if (!shadow_gfx_stats_format(&stats, 8, FALSE, 1, line, sizeof(line)))
	{
		(void)fprintf(stderr, "limits: no line\n");
		return FALSE;
	}
	(void)printf("limits: %s\n", line);

	SHADOW_GFX_STATS empty = { 0 };
	const char* expected = "gfx 5.0 s: captured 0 (0.0/s), sent 0 (0.0/s), held 0, merged 0, acks "
	                       "0, ack delay -, encode -, in flight max 0 of 8";
	const size_t length = strlen(expected);
	if (shadow_gfx_stats_format(&empty, 8, FALSE, 5000, line, length) ||
	    !shadow_gfx_stats_format(&empty, 8, FALSE, 5000, line, length + 1) ||
	    shadow_gfx_stats_format(&empty, 8, FALSE, 5000, nullptr, sizeof(line)) ||
	    shadow_gfx_stats_format(&empty, 8, FALSE, 5000, line, 0))
	{
		(void)fprintf(stderr, "limits: buffer size not checked\n");
		return FALSE;
	}
	return TRUE;
}

/* without /gfx-stats-period the line is written only when its logger is at DEBUG or TRACE */
static BOOL test_debug_switch(void)
{
	wLog* log = WLog_Get(SHADOW_GFX_STATS_TAG);
	if (!log)
		return FALSE;
	const DWORD level = WLog_GetLogLevel(log);
	BOOL ok = WLog_SetLogLevel(log, WLOG_INFO) && !shadow_gfx_stats_debug();
	ok = WLog_SetLogLevel(log, WLOG_DEBUG) && shadow_gfx_stats_debug() && ok;
	ok = WLog_SetLogLevel(log, WLOG_TRACE) && shadow_gfx_stats_debug() && ok;
	ok = WLog_SetLogLevel(log, level) && ok;
	if (!ok)
		(void)fprintf(stderr, "debug switch: INFO must not write, DEBUG and TRACE must\n");
	return ok;
}

int TestShadowGfxStats(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!test_normal() || !test_zero_sent() || !test_elapsed_zero() || !test_ring_wrap() ||
	    !test_suspended_and_periods() || !test_limits() || !test_debug_switch())
		return -1;
	return 0;
}
