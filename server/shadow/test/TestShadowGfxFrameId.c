#include <stdio.h>

#include <winpr/wtypes.h>

#include "../shadow_encoder.h"

/* The frame ids of the GFX flow control: a round whose encode sends nothing (AVC420 when no block
 * changed) gives its id back, so the frames in flight (frameId - lastAckframeId) stay what the
 * client has not acknowledged yet. Before, such a round's id stayed in flight until a later frame
 * was acknowledged; with maxInFlight (8) of them after the last one, every frame was held for
 * good: FreeRDP Web on the Mac shadow got 6 frames, acknowledged all 6, and then none came, with
 * "in flight max 8 of 8". */

/* one round as shadow_client_send_surface_gfx() runs it: the encode takes an id
 * (shadow_encoder_create_frame_id() adds one) and sends a frame or not */
static UINT32 encode_round(rdpShadowEncoder* encoder, BOOL frameOut, BOOL giveBack)
{
	const UINT32 before = encoder->frameId;
	const UINT32 id = ++encoder->frameId;
	if (giveBack)
		shadow_encoder_unused_frame_id(encoder, before, frameOut);
	return frameOut ? id : 0;
}

/* The web session's rounds: 6 frames, each acknowledged, with rounds that send nothing between
 * them and after them. Returns the frames in flight at the end. */
static UINT32 session(BOOL giveBack, UINT32 start)
{
	rdpShadowEncoder encoder = WINPR_C_ARRAY_INIT;
	encoder.frameId = start;
	encoder.lastAckframeId = start;
	encoder.queueDepth = 0;

	for (size_t x = 0; x < 6; x++)
	{
		const UINT32 id = encode_round(&encoder, TRUE, giveBack);
		encoder.lastAckframeId = id; /* the client acknowledges it */
		(void)encode_round(&encoder, FALSE, giveBack);
	}
	for (size_t x = 0; x < 8; x++)
		(void)encode_round(&encoder, FALSE, giveBack);
	return shadow_encoder_inflight_frames(&encoder);
}

int TestShadowGfxFrameId(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = 0;

	/* the old arithmetic: the ids between two acknowledged frames are covered by the later
	 * acknowledgement, the 1 + 8 after the last one stay in flight: more than maxInFlight */
	const UINT32 before = session(FALSE, 0);
	(void)fprintf(stderr, "without giving ids back: %" PRIu32 " in flight (9: the stall)\n",
	              before);
	if (before != 9)
		rc = -1;

	/* ids given back: nothing in flight, everything sent was acknowledged */
	const UINT32 after = session(TRUE, 0);
	(void)fprintf(stderr, "giving ids back: %" PRIu32 " in flight (want 0)\n", after);
	if (after != 0)
		rc = -1;

	/* the same across the wrap of the 32 bit id */
	const UINT32 wrapped = session(TRUE, UINT32_MAX - 3);
	(void)fprintf(stderr, "giving ids back across the wrap: %" PRIu32 " in flight (want 0)\n",
	              wrapped);
	if (wrapped != 0)
		rc = -1;

	/* a frame sent and not yet acknowledged stays in flight, whatever rounds follow it */
	{
		rdpShadowEncoder encoder = WINPR_C_ARRAY_INIT;
		const UINT32 sent = encode_round(&encoder, TRUE, TRUE);
		for (size_t x = 0; x < 20; x++)
			(void)encode_round(&encoder, FALSE, TRUE);
		const UINT32 inFlight = shadow_encoder_inflight_frames(&encoder);
		(void)fprintf(stderr,
		              "one frame out, not acknowledged: frame %" PRIu32 ", %" PRIu32
		              " in flight (want 1)\n",
		              sent, inFlight);
		if ((sent != 1) || (inFlight != 1))
			rc = -1;
	}
	return rc;
}
