/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_SERVER_SHADOW_ENCODER_H
#define FREERDP_SERVER_SHADOW_ENCODER_H

#include <winpr/crt.h>
#include <winpr/stream.h>

#include <freerdp/freerdp.h>
#include <freerdp/codecs.h>

#include <freerdp/server/shadow.h>

#include "shadow_gfx_stats.h"
#include <freerdp/server/astats.h>

/* the most frames per second a subsystem that paces its capture by
 * shadow_encoder_preferred_fps() is asked for (X11, Windows) */
#define SHADOW_ENCODER_MAX_FPS 60

/* one GFX surface of a client, for one monitor when the server shares all of them */
typedef struct
{
	UINT16 surfaceId;
	RECTANGLE_16 rect;         /* its part of the server's surface */
	rdpShadowEncoder* encoder; /* its codecs, sized to rect */
} SHADOW_GFX_OUTPUT;

#define SHADOW_GFX_OUTPUTS_MAX 16

struct rdp_shadow_encoder
{
	rdpShadowClient* client;
	rdpShadowServer* server;

	UINT32 width;
	UINT32 height;
	UINT32 codecs;

	BYTE** grid;
	UINT32 gridWidth;
	UINT32 gridHeight;
	BYTE* gridBuffer;
	UINT32 maxTileWidth;
	UINT32 maxTileHeight;

	wStream* bs;

	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	BITMAP_PLANAR_CONTEXT* planar;
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
	H264_CONTEXT* h264;
	PROGRESSIVE_CONTEXT* progressive;
#if defined(WITH_GFX_AV1)
	FREERDP_AV1_CONTEXT* av1;
#endif
#if defined(WITH_GFX_AV2)
	FREERDP_AV2_CONTEXT* av2;
#endif
#if defined(WITH_GFX_AZURE)
	FREERDP_HEVC_CONTEXT* hevc;
#endif
	UINT32 fps;
	UINT32 maxFps;
	BOOL frameAck;
	UINT32 frameId;
	UINT32 lastAckframeId;
	UINT32 queueDepth;

	/* GFX flow control (shadow_client.c): no new frame while maxInFlight frames are not
	 * acknowledged (0: no limit). gfxHeld: a round's damage waits in the client's region.
	 * maxInFlight follows the link (shadow_encoder_ack_delay()) unless
	 * FREERDP_SHADOW_GFX_FRAMES_IN_FLIGHT fixes it (fixedInFlight). */
	UINT32 maxInFlight;
	BOOL fixedInFlight;
	UINT64 ackDelayMinNs;     /* the lowest acknowledgement delay of this span, 0: none */
	UINT64 ackDelayPrevMinNs; /* of the span before */
	UINT64 ackDelaySpanStart; /* ms */
	UINT32 acksCounted;       /* acknowledgements in this second */
	UINT64 acksSecondStart;   /* ms */
	UINT64 acksDelaySumNs;    /* their delays, and how many had one */
	UINT32 acksDelays;
	UINT32 inFlightPeak; /* the most frames out in this second (shadow_client_gfx_must_wait) */
	UINT64 lastAckTick;
	BOOL gfxHeld;
	BOOL everAcked; /* the client acknowledged a frame at least once */
	BOOL warnedNoAck;
	UINT64 statsLogged;
	SHADOW_GFX_STATS gfxStats; /* the 5 s line */
	/* who this connection is to a reader of the stats socket and the client (random, not 0),
	 * since when (Unix epoch ms), and the periods it has had */
	UINT64 connectionId;
	UINT64 connectedMs;
	char startedAt[40]; /* the same, ISO 8601 on this machine's clock; "" when it has none */
	char peer[64];      /* the client's address, "ip:port"; "" when not known */
	UINT64 statsPeriods;
	UINT64 roundStartNs;       /* this round's capture start (rdpShadowSubsystem::captureStartNs) */
	UINT64 statsCpuNs;         /* the process's CPU time at the last period's end, 0: none yet */
	astats_server_context* astats; /* FreeRDP::Advanced::Stats, nullptr without */
	BOOL gfxFrameOut;          /* the frame being encoded went out */

	/* GFX RFX dirty tile skipping (shadow_rfx_dirty.c): snapshot of the last
	 * encoded content (BGRX copy sharing the surface geometry) plus per tile
	 * encoded flags. Only used by the GFX RFX path; a missing or mismatching
	 * snapshot encodes the frame in full. */
	BYTE* gfxSnapshot;
	BOOL* gfxTileEncoded;
	UINT32 gfxSnapshotWidth;
	UINT32 gfxSnapshotHeight;
	UINT32 gfxSnapshotStep;
	BOOL gfxSnapshotValid;

	/* An output's encoder holds codecs only, sized outputWidth x outputHeight (0: the
	 * screen's). The client's encoder keeps the frames (ids, flow control, counters) and, with
	 * more than one GFX surface (one per monitor), the outputs and the frame being sent over
	 * them: one StartFrame, a surface command per changed output, one EndFrame. */
	UINT32 outputWidth;
	UINT32 outputHeight;
	UINT32 outputCount; /* > 1: the surfaces of outputs[] */
	SHADOW_GFX_OUTPUT outputs[SHADOW_GFX_OUTPUTS_MAX];
	BOOL roundOpen;               /* a frame over the outputs is being sent */
	BOOL roundStarted;            /* its StartFrame went out */
	UINT32 roundFrameId;          /* its id */
	UINT16 roundSurfaceId;        /* the output being encoded */
	rdpShadowEncoder* roundCodec; /* and its encoder */
};

#ifdef __cplusplus
extern "C"
{
#endif

	WINPR_ATTR_NODISCARD int shadow_encoder_reset(rdpShadowEncoder* encoder);
	WINPR_ATTR_NODISCARD int shadow_encoder_prepare(rdpShadowEncoder* encoder, UINT32 codecs);
	WINPR_ATTR_NODISCARD UINT32 shadow_encoder_create_frame_id(rdpShadowEncoder* encoder);
	/** an acknowledgement's delay (send to acknowledgement, ns): sizes the frames in flight */
	void shadow_encoder_ack_delay(rdpShadowEncoder* encoder, UINT64 delayNs);

	void shadow_encoder_free(rdpShadowEncoder* encoder);

	WINPR_ATTR_MALLOC(shadow_encoder_free, 1)
	rdpShadowEncoder* shadow_encoder_new(rdpShadowClient* client);

	/** the codecs of one GFX surface of width x height (one monitor's), no frames of its own */
	WINPR_ATTR_MALLOC(shadow_encoder_free, 1)
	rdpShadowEncoder* shadow_encoder_new_output(rdpShadowClient* client, UINT32 width,
	                                            UINT32 height);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_ENCODER_H */
