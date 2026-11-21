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

#ifndef FREERDP_SERVER_SHADOW_GFX_STATS_H
#define FREERDP_SERVER_SHADOW_GFX_STATS_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>

/* send times kept per client for the acknowledgement delay: a power of 2, more than the frames a
 * client may have in flight; an acknowledgement for an older frame is counted without a delay */
#define SHADOW_GFX_STATS_RING 64u

/* the logger of the line: its own, so a log filter can keep it or leave it out by name */
#define SHADOW_GFX_STATS_TAG "com.freerdp.server.shadow.gfx"

/* The frames in flight (shadow_gfx_window()): at least what xrdp keeps, at most what a slow link
 * may queue (32 frames are half a second at 60 fps), and two more than one delay's worth */
#define SHADOW_GFX_WINDOW_MIN 8u
#define SHADOW_GFX_WINDOW_MAX 32u
#define SHADOW_GFX_WINDOW_SPARE 2u

/* the period of the counters when no /gfx-stats-period is given */
#define SHADOW_GFX_STATS_DEFAULT_SECONDS 5u

typedef struct
{
	UINT32 frameId;
	UINT64 sentNs; /* 0: no frame */
} SHADOW_GFX_SENT_FRAME;

/* What one client's GFX frames did since the last 5 s line (shadow_client.c), so a session that
 * runs at 30 fps tells whether capture, the encoder, the flow control or the client holds it.
 * The client thread alone reads and writes it. */
typedef struct
{
	UINT64 captured;      /* rounds with a new picture or damage for this client */
	UINT64 sent;          /* frames encoded and sent */
	UINT64 held;          /* rounds that waited for an acknowledgement */
	UINT64 merged;        /* frames sent that carried the damage of held rounds */
	UINT64 acks;          /* frame acknowledgements received */
	UINT64 ackDelays;     /* acknowledgements whose frame's send time was still in the ring */
	UINT64 ackDelaySumNs; /* send to acknowledgement */
	UINT64 ackDelayMaxNs;
	UINT64 encodeSumNs; /* encode and send of one frame */
	UINT64 encodeMaxNs;
	UINT32 maxInFlight; /* the most frames in flight seen */
	SHADOW_GFX_SENT_FRAME ring[SHADOW_GFX_STATS_RING];
} SHADOW_GFX_STATS;

#ifdef __cplusplus
extern "C"
{
#endif

	/** counts a frame sent: its encode time and its send time for the acknowledgement delay */
	FREERDP_LOCAL void shadow_gfx_stats_frame_sent(SHADOW_GFX_STATS* stats, UINT32 frameId,
	                                               UINT64 startNs, UINT64 endNs);

	/** counts an acknowledgement and, if the frame's send time is in the ring, its delay
	 *  @return that delay in ns, 0 when the send time was not in the ring */
	FREERDP_LOCAL UINT64 shadow_gfx_stats_frame_acked(SHADOW_GFX_STATS* stats, UINT32 frameId,
	                                                  UINT64 ackNs);

	/** The frames in flight the link carries without a queue: the frames acknowledged per second
	 *  (rate) times the round trip without a queue (baseAckNs), rounded up, and
	 *  SHADOW_GFX_WINDOW_SPARE more, from SHADOW_GFX_WINDOW_MIN to SHADOW_GFX_WINDOW_MAX;
	 *  SHADOW_GFX_WINDOW_MIN while either is not known (0). */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL UINT32 shadow_gfx_window(UINT64 baseAckNs, UINT32 rate);

	/** The next second's frames in flight from this second's: window frames were allowed, at most
	 *  peak were out, the acknowledgements took avgAckNs on average and rate came a second.
	 *  No queue (avgAckNs at most 5/4 of baseAckNs) and the window reached (the encoder slows
	 *  down at window - 1): SHADOW_GFX_WINDOW_SPARE more. A queue (above 3/2): what the link
	 *  carries (shadow_gfx_window()). Else, or nothing known: unchanged. Always within
	 *  SHADOW_GFX_WINDOW_MIN to SHADOW_GFX_WINDOW_MAX. */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL UINT32 shadow_gfx_window_next(UINT32 window, UINT32 peak,
	                                                                 UINT64 baseAckNs,
	                                                                 UINT64 avgAckNs, UINT32 rate);

	/** starts the next period: the counters are zeroed, the send times of frames in flight kept */
	FREERDP_LOCAL void shadow_gfx_stats_next_period(SHADOW_GFX_STATS* stats);

	/** forgets the send times, for when the frame ids start again */
	FREERDP_LOCAL void shadow_gfx_stats_forget_frames(SHADOW_GFX_STATS* stats);

	/** TRUE when SHADOW_GFX_STATS_TAG is at DEBUG or TRACE: the line is written without
	 * /gfx-stats-period
	 */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL shadow_gfx_stats_debug(void);

	/** writes the line (shadow_gfx_stats_format) at INFO on its own logger, SHADOW_GFX_STATS_TAG */
	FREERDP_LOCAL void shadow_gfx_stats_write(const SHADOW_GFX_STATS* stats, UINT32 inFlightLimit,
	                                          BOOL acksSuspended, UINT64 elapsedMs);

	/** writes the 5 s line.
	 *  @param inFlightLimit the encoder's limit of frames in flight, 0: none
	 *  @param acksSuspended the client suspended its acknowledgements
	 *  @param elapsedMs the period the counters cover
	 *  @return TRUE if the line fit into buffer */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL shadow_gfx_stats_format(const SHADOW_GFX_STATS* stats,
	                                                                UINT32 inFlightLimit,
	                                                                BOOL acksSuspended,
	                                                                UINT64 elapsedMs, char* buffer,
	                                                                size_t size);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_GFX_STATS_H */
