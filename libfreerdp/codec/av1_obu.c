/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 OBU and frame header parser for hardware decoders
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
 *
 * The bit reader, the frame header parser and the shear check are adapted from dav1d 1.5.4
 * (src/getbits.c, src/obu.c, src/warpmv.c), under this licence:
 *
 * Copyright © 2018-2021, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/platform.h>

#include <dav1d/dav1d.h>

#include "av1_obu.h"

/* ---- bit reader (dav1d src/getbits.c) ---- */

typedef struct
{
	UINT64 state;
	int bits_left;
	int error;
	const BYTE* ptr;
	const BYTE* ptr_start;
	const BYTE* ptr_end;
} GetBits;

static void gb_init(GetBits* c, const BYTE* data, size_t sz)
{
	c->ptr = c->ptr_start = data;
	c->ptr_end = &c->ptr_start[sz];
	c->state = 0;
	c->bits_left = 0;
	c->error = 0;
}

static unsigned gb_bit(GetBits* c)
{
	if (!c->bits_left)
	{
		if (c->ptr >= c->ptr_end)
			c->error = 1;
		else
		{
			const unsigned state = *c->ptr++;
			c->bits_left = 7;
			c->state = (UINT64)state << 57;
			return state >> 7;
		}
	}

	const UINT64 state = c->state;
	c->bits_left--;
	c->state = state << 1;
	return (unsigned)(state >> 63);
}

static void gb_refill(GetBits* c, int n)
{
	unsigned state = 0;
	do
	{
		if (c->ptr >= c->ptr_end)
		{
			c->error = 1;
			if (state)
				break;
			return;
		}
		state = (state << 8) | *c->ptr++;
		c->bits_left += 8;
	} while (n > c->bits_left);
	c->state |= (UINT64)state << (64 - c->bits_left);
}

static unsigned gb_bits(GetBits* c, int n)
{
	WINPR_ASSERT(n > 0 && n <= 32);
	if ((unsigned)n > (unsigned)c->bits_left)
		gb_refill(c, n);
	const UINT64 state = c->state;
	c->bits_left -= n;
	c->state = state << n;
	return (unsigned)(state >> (64 - n));
}

static int gb_sbits(GetBits* c, int n)
{
	WINPR_ASSERT(n > 0 && n <= 32);
	if ((unsigned)n > (unsigned)c->bits_left)
		gb_refill(c, n);
	const UINT64 state = c->state;
	c->bits_left -= n;
	c->state = state << n;
	return (int)((INT64)state >> (64 - n));
}

static unsigned gb_uleb128(GetBits* c)
{
	UINT64 val = 0;
	unsigned i = 0;
	unsigned more = 0;

	do
	{
		const unsigned v = gb_bits(c, 8);
		more = v & 0x80;
		val |= ((UINT64)(v & 0x7F)) << i;
		i += 7;
	} while (more && i < 56);

	if (val > UINT32_MAX || more)
	{
		c->error = 1;
		return 0;
	}
	return (unsigned)val;
}

static int ulog2(unsigned v)
{
	int r = -1;
	while (v)
	{
		v >>= 1;
		r++;
	}
	return r;
}

static unsigned gb_uniform(GetBits* c, unsigned max)
{
	/* [0..max-1], max > 1 */
	WINPR_ASSERT(max > 1);
	const int l = ulog2(max) + 1;
	const unsigned m = (1U << l) - max;
	const unsigned v = gb_bits(c, l - 1);
	return v < m ? v : (v << 1) - m + gb_bit(c);
}

static unsigned inv_recenter(unsigned r, unsigned v)
{
	if (v > (r << 1))
		return v;
	if ((v & 1) == 0)
		return (v >> 1) + r;
	return r - ((v + 1) >> 1);
}

static unsigned gb_subexp_u(GetBits* c, unsigned ref, unsigned n)
{
	unsigned v = 0;

	for (int i = 0;; i++)
	{
		const int b = i ? 3 + i - 1 : 3;

		if (n < v + 3 * (1u << b))
		{
			v += gb_uniform(c, n - v + 1);
			break;
		}
		if (!gb_bit(c))
		{
			v += gb_bits(c, b);
			break;
		}
		v += 1u << b;
	}

	return ref * 2 <= n ? inv_recenter(ref, v) : n - inv_recenter(n - ref, v);
}

static int gb_subexp(GetBits* c, int ref, unsigned n)
{
	return (int)gb_subexp_u(c, (unsigned)(ref + (1 << n)), 2u << n) - (1 << n);
}

static void gb_bytealign(GetBits* c)
{
	c->bits_left = 0;
	c->state = 0;
}

/* ---- helpers ---- */

static inline int imin(int a, int b)
{
	return a < b ? a : b;
}

static inline int imax(int a, int b)
{
	return a > b ? a : b;
}

static inline int iclip(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline int apply_sign(int v, int s)
{
	return s < 0 ? -v : v;
}

static inline INT64 apply_sign64(INT64 v, INT64 s)
{
	return s < 0 ? -v : v;
}

static int get_poc_diff(int order_hint_n_bits, int poc0, int poc1)
{
	if (!order_hint_n_bits)
		return 0;
	const int mask = 1 << (order_hint_n_bits - 1);
	const int diff = poc0 - poc1;
	return (diff & (mask - 1)) - (diff & mask);
}

static inline int tile_log2(int sz, int tgt)
{
	int k = 0;
	for (; (sz << k) < tgt; k++)
		;
	return k;
}

#define IS_INTER_OR_SWITCH(h) ((h)->frame_type & 1)
#define IS_KEY_OR_INTRA(h) (!IS_INTER_OR_SWITCH(h))

static const Dav1dLoopfilterModeRefDeltas default_mode_ref_deltas = {
	.mode_delta = { 0, 0 },
	.ref_delta = { 1, 0, 0, 0, -1, 0, -1, -1 },
};

static const Dav1dWarpedMotionParams default_wm_params = {
	.type = DAV1D_WM_TYPE_IDENTITY,
	.matrix = { 0, 0, 1 << 16, 0, 0, 1 << 16 },
};

/* ---- shear check (dav1d src/warpmv.c): whether global motion can warp ---- */

static const UINT16 div_lut[257] = {
	16384, 16320, 16257, 16194, 16132, 16070, 16009, 15948, 15888, 15828, 15768, 15709, 15650,
	15592, 15534, 15477, 15420, 15364, 15308, 15252, 15197, 15142, 15087, 15033, 14980, 14926,
	14873, 14821, 14769, 14717, 14665, 14614, 14564, 14513, 14463, 14413, 14364, 14315, 14266,
	14218, 14170, 14122, 14075, 14028, 13981, 13935, 13888, 13843, 13797, 13752, 13707, 13662,
	13618, 13574, 13530, 13487, 13443, 13400, 13358, 13315, 13273, 13231, 13190, 13148, 13107,
	13066, 13026, 12985, 12945, 12906, 12866, 12827, 12788, 12749, 12710, 12672, 12633, 12596,
	12558, 12520, 12483, 12446, 12409, 12373, 12336, 12300, 12264, 12228, 12193, 12157, 12122,
	12087, 12053, 12018, 11984, 11950, 11916, 11882, 11848, 11815, 11782, 11749, 11716, 11683,
	11651, 11619, 11586, 11555, 11523, 11491, 11460, 11429, 11398, 11367, 11336, 11305, 11275,
	11245, 11215, 11185, 11155, 11125, 11096, 11067, 11038, 11009, 10980, 10951, 10923, 10894,
	10866, 10838, 10810, 10782, 10755, 10727, 10700, 10673, 10645, 10618, 10592, 10565, 10538,
	10512, 10486, 10460, 10434, 10408, 10382, 10356, 10331, 10305, 10280, 10255, 10230, 10205,
	10180, 10156, 10131, 10107, 10082, 10058, 10034, 10010, 9986,  9963,  9939,  9916,  9892,
	9869,  9846,  9823,  9800,  9777,  9754,  9732,  9709,  9687,  9664,  9642,  9620,  9598,
	9576,  9554,  9533,  9511,  9489,  9468,  9447,  9425,  9404,  9383,  9362,  9341,  9321,
	9300,  9279,  9259,  9239,  9218,  9198,  9178,  9158,  9138,  9118,  9098,  9079,  9059,
	9039,  9020,  9001,  8981,  8962,  8943,  8924,  8905,  8886,  8867,  8849,  8830,  8812,
	8793,  8775,  8756,  8738,  8720,  8702,  8684,  8666,  8648,  8630,  8613,  8595,  8577,
	8560,  8542,  8525,  8508,  8490,  8473,  8456,  8439,  8422,  8405,  8389,  8372,  8355,
	8339,  8322,  8306,  8289,  8273,  8257,  8240,  8224,  8208,  8192,
};

static int iclip_wmp(int v)
{
	const int cv = iclip(v, INT16_MIN, INT16_MAX);
	return apply_sign((abs(cv) + 32) >> 6, cv) * (1 << 6);
}

static int resolve_divisor_32(unsigned d, int* shift)
{
	*shift = ulog2(d);
	const int e = (int)(d - (1u << *shift));
	const int f = *shift > 8 ? (e + (1 << (*shift - 9))) >> (*shift - 8) : e << (8 - *shift);
	WINPR_ASSERT(f <= 256);
	*shift += 14;
	return div_lut[f];
}

/** @return TRUE when the parameters can't warp (the spec's !warpValid) */
static BOOL shear_invalid(const Dav1dWarpedMotionParams* wm)
{
	const int32_t* const mat = wm->matrix;

	if (mat[2] <= 0)
		return TRUE;

	const int alpha = iclip_wmp(mat[2] - 0x10000);
	const int beta = iclip_wmp(mat[3]);

	int shift = 0;
	const int y = apply_sign(resolve_divisor_32((unsigned)abs(mat[2]), &shift), mat[2]);
	const INT64 v1 = ((INT64)mat[4] * 0x10000) * y;
	const INT64 rnd = (1ll << shift) >> 1;
	const int gamma = iclip_wmp((int)apply_sign64((llabs(v1) + rnd) >> shift, v1));
	const INT64 v2 = ((INT64)mat[3] * mat[4]) * y;
	const int delta =
	    iclip_wmp(mat[5] - (int)apply_sign64((llabs(v2) + rnd) >> shift, v2) - 0x10000);

	return (4 * abs(alpha) + 7 * abs(beta) >= 0x10000) ||
	       (4 * abs(gamma) + 4 * abs(delta) >= 0x10000);
}

/* ---- the parser ---- */

#define AV1_MAX_TILES (DAV1D_MAX_TILE_COLS * DAV1D_MAX_TILE_ROWS)

struct S_AV1_OBU_PARSER
{
	Dav1dSequenceHeader seq;
	BOOL haveSeq;
	BOOL newSequence; /* reported with the next frame event */
	unsigned operatingPointIdc;

	AV1_OBU_FRAME_HEADER ref[DAV1D_NUM_REF_FRAMES];
	BOOL refValid[DAV1D_NUM_REF_FRAMES];

	AV1_OBU_FRAME_HEADER cur;
	BOOL haveCur; /* a frame header waits for its tiles */
	UINT32 nTilesSeen;
	AV1_OBU_TILE tiles[AV1_MAX_TILES];

	/* the last event's reference refresh, applied when the parser goes on, so the consumer
	 * still sees the references the frame was coded with */
	AV1_OBU_FRAME_HEADER pendingHdr;
	BYTE pendingRefresh;

	const BYTE* data;
	size_t size;
	size_t pos;
	BOOL failed;
};

static const AV1_OBU_FRAME_HEADER* ref_hdr(const AV1_OBU_PARSER* p, int slot)
{
	if ((slot < 0) || (slot >= DAV1D_NUM_REF_FRAMES) || !p->refValid[slot])
		return nullptr;
	return &p->ref[slot];
}

static int read_frame_size(AV1_OBU_PARSER* p, GetBits* gb, int use_ref)
{
	const Dav1dSequenceHeader* const seqhdr = &p->seq;
	Dav1dFrameHeader* const hdr = &p->cur.d;

	if (use_ref)
	{
		for (int i = 0; i < 7; i++)
		{
			if (gb_bit(gb))
			{
				const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[i]);
				if (!ref)
					return -1;
				hdr->width[1] = ref->d.width[1];
				hdr->height = ref->d.height;
				hdr->render_width = ref->d.render_width;
				hdr->render_height = ref->d.render_height;
				hdr->super_res.enabled = seqhdr->super_res && gb_bit(gb);
				if (hdr->super_res.enabled)
				{
					const int d = hdr->super_res.width_scale_denominator =
					    (uint8_t)(9 + gb_bits(gb, 3));
					hdr->width[0] =
					    imax((hdr->width[1] * 8 + (d >> 1)) / d, imin(16, hdr->width[1]));
				}
				else
				{
					hdr->super_res.width_scale_denominator = 8;
					hdr->width[0] = hdr->width[1];
				}
				return 0;
			}
		}
	}

	if (hdr->frame_size_override)
	{
		hdr->width[1] = (int)gb_bits(gb, seqhdr->width_n_bits) + 1;
		hdr->height = (int)gb_bits(gb, seqhdr->height_n_bits) + 1;
	}
	else
	{
		hdr->width[1] = seqhdr->max_width;
		hdr->height = seqhdr->max_height;
	}
	hdr->super_res.enabled = seqhdr->super_res && gb_bit(gb);
	if (hdr->super_res.enabled)
	{
		const int d = hdr->super_res.width_scale_denominator = (uint8_t)(9 + gb_bits(gb, 3));
		hdr->width[0] = imax((hdr->width[1] * 8 + (d >> 1)) / d, imin(16, hdr->width[1]));
	}
	else
	{
		hdr->super_res.width_scale_denominator = 8;
		hdr->width[0] = hdr->width[1];
	}
	hdr->have_render_size = (uint8_t)gb_bit(gb);
	if (hdr->have_render_size)
	{
		hdr->render_width = (int)gb_bits(gb, 16) + 1;
		hdr->render_height = (int)gb_bits(gb, 16) + 1;
	}
	else
	{
		hdr->render_width = hdr->width[1];
		hdr->render_height = hdr->height;
	}
	return 0;
}

static void short_signaling(AV1_OBU_PARSER* p, Dav1dFrameHeader* hdr, int* earliest)
{
	const Dav1dSequenceHeader* const seqhdr = &p->seq;
	/* +1 allows for unconditional stores, as unused values can be dumped into
	 * frame_offset[-1]. */
	int frame_offset_mem[8 + 1] = { 0 };
	int* const frame_offset = &frame_offset_mem[1];
	int earliest_ref = -1;
	for (int i = 0, earliest_offset = INT_MAX; i < 8; i++)
	{
		const int diff =
		    get_poc_diff(seqhdr->order_hint_n_bits, p->ref[i].d.frame_offset, hdr->frame_offset);
		frame_offset[i] = diff;
		if (diff < earliest_offset)
		{
			earliest_offset = diff;
			earliest_ref = i;
		}
	}
	frame_offset[hdr->refidx[0]] = INT_MIN; /* = reference frame is used */
	frame_offset[hdr->refidx[3]] = INT_MIN;

	int refidx = -1;
	for (int i = 0, latest_offset = 0; i < 8; i++)
	{
		const int hint = frame_offset[i];
		if (hint >= latest_offset)
		{
			latest_offset = hint;
			refidx = i;
		}
	}
	frame_offset[refidx] = INT_MIN;
	hdr->refidx[6] = (int8_t)refidx;

	for (int i = 4; i < 6; i++)
	{
		/* Unsigned compares to handle negative values. */
		unsigned earliest_offset = UINT8_MAX;
		refidx = -1;
		for (int j = 0; j < 8; j++)
		{
			const unsigned hint = (unsigned)frame_offset[j];
			if (hint < earliest_offset)
			{
				earliest_offset = hint;
				refidx = j;
			}
		}
		frame_offset[refidx] = INT_MIN;
		hdr->refidx[i] = (int8_t)refidx;
	}

	for (int i = 1; i < 7; i++)
	{
		refidx = hdr->refidx[i];
		if (refidx < 0)
		{
			unsigned latest_offset = ~(unsigned)UINT8_MAX;
			for (int j = 0; j < 8; j++)
			{
				const unsigned hint = (unsigned)frame_offset[j];
				if (hint >= latest_offset)
				{
					latest_offset = hint;
					refidx = j;
				}
			}
			frame_offset[refidx] = INT_MIN;
			hdr->refidx[i] = (int8_t)(refidx >= 0 ? refidx : earliest_ref);
		}
	}
	*earliest = earliest_ref;
}

static int parse_frame_hdr(AV1_OBU_PARSER* p, GetBits* gb)
{
	const Dav1dSequenceHeader* const seqhdr = &p->seq;
	Dav1dFrameHeader* const hdr = &p->cur.d;

	if (!seqhdr->reduced_still_picture_header)
		hdr->show_existing_frame = (uint8_t)gb_bit(gb);
	if (hdr->show_existing_frame)
	{
		hdr->existing_frame_idx = (uint8_t)gb_bits(gb, 3);
		if (seqhdr->decoder_model_info_present && !seqhdr->equal_picture_interval)
			hdr->frame_presentation_delay = gb_bits(gb, seqhdr->frame_presentation_delay_length);
		if (seqhdr->frame_id_numbers_present)
		{
			hdr->frame_id = gb_bits(gb, seqhdr->frame_id_n_bits);
			const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->existing_frame_idx);
			if (!ref || ref->d.frame_id != hdr->frame_id)
				return -1;
		}
		return 0;
	}

	if (seqhdr->reduced_still_picture_header)
	{
		hdr->frame_type = DAV1D_FRAME_TYPE_KEY;
		hdr->show_frame = 1;
	}
	else
	{
		hdr->frame_type = (enum Dav1dFrameType)gb_bits(gb, 2);
		hdr->show_frame = (uint8_t)gb_bit(gb);
	}
	if (hdr->show_frame)
	{
		if (seqhdr->decoder_model_info_present && !seqhdr->equal_picture_interval)
			hdr->frame_presentation_delay = gb_bits(gb, seqhdr->frame_presentation_delay_length);
		hdr->showable_frame = hdr->frame_type != DAV1D_FRAME_TYPE_KEY;
	}
	else
		hdr->showable_frame = (uint8_t)gb_bit(gb);
	hdr->error_resilient_mode = (hdr->frame_type == DAV1D_FRAME_TYPE_KEY && hdr->show_frame) ||
	                            hdr->frame_type == DAV1D_FRAME_TYPE_SWITCH ||
	                            seqhdr->reduced_still_picture_header || gb_bit(gb);
	hdr->disable_cdf_update = (uint8_t)gb_bit(gb);
	hdr->allow_screen_content_tools = (uint8_t)(seqhdr->screen_content_tools == DAV1D_ADAPTIVE
	                                                ? gb_bit(gb)
	                                                : (unsigned)seqhdr->screen_content_tools);
	if (hdr->allow_screen_content_tools)
		hdr->force_integer_mv = (uint8_t)(seqhdr->force_integer_mv == DAV1D_ADAPTIVE
		                                      ? gb_bit(gb)
		                                      : (unsigned)seqhdr->force_integer_mv);

	if (IS_KEY_OR_INTRA(hdr))
		hdr->force_integer_mv = 1;

	if (seqhdr->frame_id_numbers_present)
		hdr->frame_id = gb_bits(gb, seqhdr->frame_id_n_bits);

	if (!seqhdr->reduced_still_picture_header)
		hdr->frame_size_override =
		    (uint8_t)(hdr->frame_type == DAV1D_FRAME_TYPE_SWITCH ? 1 : gb_bit(gb));
	if (seqhdr->order_hint)
		hdr->frame_offset = (uint8_t)gb_bits(gb, seqhdr->order_hint_n_bits);
	hdr->primary_ref_frame =
	    (uint8_t)(!hdr->error_resilient_mode && IS_INTER_OR_SWITCH(hdr) ? gb_bits(gb, 3)
	                                                                    : DAV1D_PRIMARY_REF_NONE);

	if (seqhdr->decoder_model_info_present)
	{
		hdr->buffer_removal_time_present = (uint8_t)gb_bit(gb);
		if (hdr->buffer_removal_time_present)
		{
			for (int i = 0; i < seqhdr->num_operating_points; i++)
			{
				const struct Dav1dSequenceHeaderOperatingPoint* const seqop =
				    &seqhdr->operating_points[i];
				struct Dav1dFrameHeaderOperatingPoint* const op = &hdr->operating_points[i];
				if (seqop->decoder_model_param_present)
				{
					const int in_temporal_layer = (seqop->idc >> hdr->temporal_id) & 1;
					const int in_spatial_layer = (seqop->idc >> (hdr->spatial_id + 8)) & 1;
					if (!seqop->idc || (in_temporal_layer && in_spatial_layer))
						op->buffer_removal_time = gb_bits(gb, seqhdr->buffer_removal_delay_length);
				}
			}
		}
	}

	if (IS_KEY_OR_INTRA(hdr))
	{
		hdr->refresh_frame_flags =
		    (uint8_t)((hdr->frame_type == DAV1D_FRAME_TYPE_KEY && hdr->show_frame)
		                  ? 0xff
		                  : gb_bits(gb, 8));
		if (hdr->refresh_frame_flags != 0xff && hdr->error_resilient_mode && seqhdr->order_hint)
			for (int i = 0; i < 8; i++)
				gb_bits(gb, seqhdr->order_hint_n_bits);
		if (read_frame_size(p, gb, 0) < 0)
			return -1;
		if (hdr->allow_screen_content_tools && !hdr->super_res.enabled)
			hdr->allow_intrabc = (uint8_t)gb_bit(gb);
	}
	else
	{
		hdr->refresh_frame_flags =
		    (uint8_t)(hdr->frame_type == DAV1D_FRAME_TYPE_SWITCH ? 0xff : gb_bits(gb, 8));
		if (hdr->error_resilient_mode && seqhdr->order_hint)
			for (int i = 0; i < 8; i++)
				gb_bits(gb, seqhdr->order_hint_n_bits);
		if (seqhdr->order_hint)
		{
			hdr->frame_ref_short_signaling = (uint8_t)gb_bit(gb);
			if (hdr->frame_ref_short_signaling)
			{
				hdr->refidx[0] = (int8_t)gb_bits(gb, 3);
				hdr->refidx[1] = hdr->refidx[2] = -1;
				hdr->refidx[3] = (int8_t)gb_bits(gb, 3);
				for (int i = 0; i < 8; i++)
				{
					if (!p->refValid[i])
						return -1;
				}
				int earliest = -1;
				short_signaling(p, hdr, &earliest);
			}
		}
		for (int i = 0; i < 7; i++)
		{
			if (!hdr->frame_ref_short_signaling)
				hdr->refidx[i] = (int8_t)gb_bits(gb, 3);
			if (seqhdr->frame_id_numbers_present)
			{
				const unsigned delta_ref_frame_id = gb_bits(gb, seqhdr->delta_frame_id_n_bits) + 1;
				const unsigned ref_frame_id =
				    (hdr->frame_id + (1u << seqhdr->frame_id_n_bits) - delta_ref_frame_id) &
				    ((1u << seqhdr->frame_id_n_bits) - 1);
				const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[i]);
				if (!ref || ref->d.frame_id != ref_frame_id)
					return -1;
			}
		}
		const int use_ref = !hdr->error_resilient_mode && hdr->frame_size_override;
		if (read_frame_size(p, gb, use_ref) < 0)
			return -1;
		if (!hdr->force_integer_mv)
			hdr->hp = (uint8_t)gb_bit(gb);
		hdr->subpel_filter_mode =
		    gb_bit(gb) ? DAV1D_FILTER_SWITCHABLE : (enum Dav1dFilterMode)gb_bits(gb, 2);
		hdr->switchable_motion_mode = (uint8_t)gb_bit(gb);
		if (!hdr->error_resilient_mode && seqhdr->ref_frame_mvs && seqhdr->order_hint &&
		    IS_INTER_OR_SWITCH(hdr))
			hdr->use_ref_frame_mvs = (uint8_t)gb_bit(gb);
	}

	if (!seqhdr->reduced_still_picture_header && !hdr->disable_cdf_update)
		hdr->refresh_context = (uint8_t)!gb_bit(gb);

	/* tile info */
	hdr->tiling.uniform = (uint8_t)gb_bit(gb);
	const int sbsz_min1 = (64 << seqhdr->sb128) - 1;
	const int sbsz_log2 = 6 + seqhdr->sb128;
	const int sbw = (hdr->width[0] + sbsz_min1) >> sbsz_log2;
	const int sbh = (hdr->height + sbsz_min1) >> sbsz_log2;
	const int max_tile_width_sb = 4096 >> sbsz_log2;
	const int max_tile_area_sb = 4096 * 2304 >> (2 * sbsz_log2);
	hdr->tiling.min_log2_cols = (uint8_t)tile_log2(max_tile_width_sb, sbw);
	hdr->tiling.max_log2_cols = (uint8_t)tile_log2(1, imin(sbw, DAV1D_MAX_TILE_COLS));
	hdr->tiling.max_log2_rows = (uint8_t)tile_log2(1, imin(sbh, DAV1D_MAX_TILE_ROWS));
	const int min_log2_tiles =
	    imax(tile_log2(max_tile_area_sb, sbw * sbh), hdr->tiling.min_log2_cols);
	if (hdr->tiling.uniform)
	{
		for (hdr->tiling.log2_cols = hdr->tiling.min_log2_cols;
		     hdr->tiling.log2_cols < hdr->tiling.max_log2_cols && gb_bit(gb);
		     hdr->tiling.log2_cols++)
			;
		const int tile_w = 1 + ((sbw - 1) >> hdr->tiling.log2_cols);
		hdr->tiling.cols = 0;
		for (int sbx = 0; sbx < sbw; sbx += tile_w, hdr->tiling.cols++)
			hdr->tiling.col_start_sb[hdr->tiling.cols] = (uint16_t)sbx;
		hdr->tiling.min_log2_rows = (uint8_t)imax(min_log2_tiles - hdr->tiling.log2_cols, 0);

		for (hdr->tiling.log2_rows = hdr->tiling.min_log2_rows;
		     hdr->tiling.log2_rows < hdr->tiling.max_log2_rows && gb_bit(gb);
		     hdr->tiling.log2_rows++)
			;
		const int tile_h = 1 + ((sbh - 1) >> hdr->tiling.log2_rows);
		hdr->tiling.rows = 0;
		for (int sby = 0; sby < sbh; sby += tile_h, hdr->tiling.rows++)
			hdr->tiling.row_start_sb[hdr->tiling.rows] = (uint16_t)sby;
	}
	else
	{
		hdr->tiling.cols = 0;
		int widest_tile = 0;
		int max_area = sbw * sbh;
		for (int sbx = 0; sbx < sbw && hdr->tiling.cols < DAV1D_MAX_TILE_COLS; hdr->tiling.cols++)
		{
			const int tile_width_sb = imin(sbw - sbx, max_tile_width_sb);
			const int tile_w =
			    (tile_width_sb > 1) ? 1 + (int)gb_uniform(gb, (unsigned)tile_width_sb) : 1;
			hdr->tiling.col_start_sb[hdr->tiling.cols] = (uint16_t)sbx;
			sbx += tile_w;
			widest_tile = imax(widest_tile, tile_w);
		}
		hdr->tiling.log2_cols = (uint8_t)tile_log2(1, hdr->tiling.cols);
		if (min_log2_tiles)
			max_area >>= min_log2_tiles + 1;
		const int max_tile_height_sb = imax(max_area / widest_tile, 1);

		hdr->tiling.rows = 0;
		for (int sby = 0; sby < sbh && hdr->tiling.rows < DAV1D_MAX_TILE_ROWS; hdr->tiling.rows++)
		{
			const int tile_height_sb = imin(sbh - sby, max_tile_height_sb);
			const int tile_h =
			    (tile_height_sb > 1) ? 1 + (int)gb_uniform(gb, (unsigned)tile_height_sb) : 1;
			hdr->tiling.row_start_sb[hdr->tiling.rows] = (uint16_t)sby;
			sby += tile_h;
		}
		hdr->tiling.log2_rows = (uint8_t)tile_log2(1, hdr->tiling.rows);
	}
	hdr->tiling.col_start_sb[hdr->tiling.cols] = (uint16_t)sbw;
	hdr->tiling.row_start_sb[hdr->tiling.rows] = (uint16_t)sbh;
	if (hdr->tiling.log2_cols || hdr->tiling.log2_rows)
	{
		hdr->tiling.update = (uint16_t)gb_bits(gb, hdr->tiling.log2_cols + hdr->tiling.log2_rows);
		if (hdr->tiling.update >= hdr->tiling.cols * hdr->tiling.rows)
			return -1;
		hdr->tiling.n_bytes = (uint8_t)(gb_bits(gb, 2) + 1);
	}

	/* quantization */
	hdr->quant.yac = (uint8_t)gb_bits(gb, 8);
	if (gb_bit(gb))
		hdr->quant.ydc_delta = (int8_t)gb_sbits(gb, 7);
	if (!seqhdr->monochrome)
	{
		const unsigned diff_uv_delta = seqhdr->separate_uv_delta_q ? gb_bit(gb) : 0;
		if (gb_bit(gb))
			hdr->quant.udc_delta = (int8_t)gb_sbits(gb, 7);
		if (gb_bit(gb))
			hdr->quant.uac_delta = (int8_t)gb_sbits(gb, 7);
		if (diff_uv_delta)
		{
			if (gb_bit(gb))
				hdr->quant.vdc_delta = (int8_t)gb_sbits(gb, 7);
			if (gb_bit(gb))
				hdr->quant.vac_delta = (int8_t)gb_sbits(gb, 7);
		}
		else
		{
			hdr->quant.vdc_delta = hdr->quant.udc_delta;
			hdr->quant.vac_delta = hdr->quant.uac_delta;
		}
	}
	hdr->quant.qm = (uint8_t)gb_bit(gb);
	if (hdr->quant.qm)
	{
		hdr->quant.qm_y = (uint8_t)gb_bits(gb, 4);
		hdr->quant.qm_u = (uint8_t)gb_bits(gb, 4);
		hdr->quant.qm_v = (uint8_t)(seqhdr->separate_uv_delta_q ? gb_bits(gb, 4) : hdr->quant.qm_u);
	}

	/* segmentation */
	hdr->segmentation.enabled = (uint8_t)gb_bit(gb);
	if (hdr->segmentation.enabled)
	{
		if (hdr->primary_ref_frame == DAV1D_PRIMARY_REF_NONE)
		{
			hdr->segmentation.update_map = 1;
			hdr->segmentation.update_data = 1;
		}
		else
		{
			hdr->segmentation.update_map = (uint8_t)gb_bit(gb);
			if (hdr->segmentation.update_map)
				hdr->segmentation.temporal = (uint8_t)gb_bit(gb);
			hdr->segmentation.update_data = (uint8_t)gb_bit(gb);
		}

		if (hdr->segmentation.update_data)
		{
			hdr->segmentation.seg_data.last_active_segid = -1;
			for (int i = 0; i < DAV1D_MAX_SEGMENTS; i++)
			{
				Dav1dSegmentationData* const seg = &hdr->segmentation.seg_data.d[i];
				BYTE mask = 0;
				if (gb_bit(gb))
				{
					seg->delta_q = (int16_t)gb_sbits(gb, 9);
					mask |= 0x01;
				}
				if (gb_bit(gb))
				{
					seg->delta_lf_y_v = (int8_t)gb_sbits(gb, 7);
					mask |= 0x02;
				}
				if (gb_bit(gb))
				{
					seg->delta_lf_y_h = (int8_t)gb_sbits(gb, 7);
					mask |= 0x04;
				}
				if (gb_bit(gb))
				{
					seg->delta_lf_u = (int8_t)gb_sbits(gb, 7);
					mask |= 0x08;
				}
				if (gb_bit(gb))
				{
					seg->delta_lf_v = (int8_t)gb_sbits(gb, 7);
					mask |= 0x10;
				}
				if (gb_bit(gb))
				{
					seg->ref = (int8_t)gb_bits(gb, 3);
					mask |= 0x20;
					hdr->segmentation.seg_data.preskip = 1;
				}
				else
					seg->ref = -1;
				if ((seg->skip = (uint8_t)gb_bit(gb)))
				{
					mask |= 0x40;
					hdr->segmentation.seg_data.preskip = 1;
				}
				if ((seg->globalmv = (uint8_t)gb_bit(gb)))
				{
					mask |= 0x80;
					hdr->segmentation.seg_data.preskip = 1;
				}
				if (mask)
					hdr->segmentation.seg_data.last_active_segid = (int8_t)i;
				p->cur.segFeatureMask[i] = mask;
			}
		}
		else
		{
			/* the segmentation data of the primary reference frame */
			const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[hdr->primary_ref_frame]);
			if (!ref)
				return -1;
			hdr->segmentation.seg_data = ref->d.segmentation.seg_data;
			memcpy(p->cur.segFeatureMask, ref->segFeatureMask, sizeof(p->cur.segFeatureMask));
		}
	}
	else
	{
		for (int i = 0; i < DAV1D_MAX_SEGMENTS; i++)
			hdr->segmentation.seg_data.d[i].ref = -1;
	}

	/* delta q, delta lf */
	if (hdr->quant.yac)
	{
		hdr->delta.q.present = (uint8_t)gb_bit(gb);
		if (hdr->delta.q.present)
		{
			hdr->delta.q.res_log2 = (uint8_t)gb_bits(gb, 2);
			if (!hdr->allow_intrabc)
			{
				hdr->delta.lf.present = (uint8_t)gb_bit(gb);
				if (hdr->delta.lf.present)
				{
					hdr->delta.lf.res_log2 = (uint8_t)gb_bits(gb, 2);
					hdr->delta.lf.multi = (uint8_t)gb_bit(gb);
				}
			}
		}
	}

	/* lossless */
	const int delta_lossless = !hdr->quant.ydc_delta && !hdr->quant.udc_delta &&
	                           !hdr->quant.uac_delta && !hdr->quant.vdc_delta &&
	                           !hdr->quant.vac_delta;
	hdr->all_lossless = 1;
	for (int i = 0; i < DAV1D_MAX_SEGMENTS; i++)
	{
		hdr->segmentation.qidx[i] =
		    (uint8_t)(hdr->segmentation.enabled
		                  ? iclip(hdr->quant.yac + hdr->segmentation.seg_data.d[i].delta_q, 0, 255)
		                  : hdr->quant.yac);
		hdr->segmentation.lossless[i] = !hdr->segmentation.qidx[i] && delta_lossless;
		hdr->all_lossless &= hdr->segmentation.lossless[i];
	}

	/* loop filter */
	if (hdr->all_lossless || hdr->allow_intrabc)
	{
		hdr->loopfilter.mode_ref_delta_enabled = 1;
		hdr->loopfilter.mode_ref_delta_update = 1;
		hdr->loopfilter.mode_ref_deltas = default_mode_ref_deltas;
	}
	else
	{
		hdr->loopfilter.level_y[0] = (uint8_t)gb_bits(gb, 6);
		hdr->loopfilter.level_y[1] = (uint8_t)gb_bits(gb, 6);
		if (!seqhdr->monochrome && (hdr->loopfilter.level_y[0] || hdr->loopfilter.level_y[1]))
		{
			hdr->loopfilter.level_u = (uint8_t)gb_bits(gb, 6);
			hdr->loopfilter.level_v = (uint8_t)gb_bits(gb, 6);
		}
		hdr->loopfilter.sharpness = (uint8_t)gb_bits(gb, 3);

		if (hdr->primary_ref_frame == DAV1D_PRIMARY_REF_NONE)
			hdr->loopfilter.mode_ref_deltas = default_mode_ref_deltas;
		else
		{
			const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[hdr->primary_ref_frame]);
			if (!ref)
				return -1;
			hdr->loopfilter.mode_ref_deltas = ref->d.loopfilter.mode_ref_deltas;
		}
		hdr->loopfilter.mode_ref_delta_enabled = (uint8_t)gb_bit(gb);
		if (hdr->loopfilter.mode_ref_delta_enabled)
		{
			hdr->loopfilter.mode_ref_delta_update = (uint8_t)gb_bit(gb);
			if (hdr->loopfilter.mode_ref_delta_update)
			{
				for (int i = 0; i < 8; i++)
					if (gb_bit(gb))
						hdr->loopfilter.mode_ref_deltas.ref_delta[i] = (int8_t)gb_sbits(gb, 7);
				for (int i = 0; i < 2; i++)
					if (gb_bit(gb))
						hdr->loopfilter.mode_ref_deltas.mode_delta[i] = (int8_t)gb_sbits(gb, 7);
			}
		}
	}

	/* cdef */
	if (!hdr->all_lossless && seqhdr->cdef && !hdr->allow_intrabc)
	{
		hdr->cdef.damping = (uint8_t)(gb_bits(gb, 2) + 3);
		hdr->cdef.n_bits = (uint8_t)gb_bits(gb, 2);
		for (int i = 0; i < (1 << hdr->cdef.n_bits); i++)
		{
			hdr->cdef.y_strength[i] = (uint8_t)gb_bits(gb, 6);
			if (!seqhdr->monochrome)
				hdr->cdef.uv_strength[i] = (uint8_t)gb_bits(gb, 6);
		}
	}

	/* loop restoration */
	if ((!hdr->all_lossless || hdr->super_res.enabled) && seqhdr->restoration &&
	    !hdr->allow_intrabc)
	{
		hdr->restoration.type[0] = (enum Dav1dRestorationType)gb_bits(gb, 2);
		if (!seqhdr->monochrome)
		{
			hdr->restoration.type[1] = (enum Dav1dRestorationType)gb_bits(gb, 2);
			hdr->restoration.type[2] = (enum Dav1dRestorationType)gb_bits(gb, 2);
		}

		if (hdr->restoration.type[0] || hdr->restoration.type[1] || hdr->restoration.type[2])
		{
			/* log2 of the restoration unit size */
			hdr->restoration.unit_size[0] = (uint8_t)(6 + seqhdr->sb128);
			if (gb_bit(gb))
			{
				hdr->restoration.unit_size[0]++;
				if (!seqhdr->sb128)
					hdr->restoration.unit_size[0] += (uint8_t)gb_bit(gb);
			}
			hdr->restoration.unit_size[1] = hdr->restoration.unit_size[0];
			if ((hdr->restoration.type[1] || hdr->restoration.type[2]) && seqhdr->ss_hor == 1 &&
			    seqhdr->ss_ver == 1)
				hdr->restoration.unit_size[1] -= (uint8_t)gb_bit(gb);
		}
		else
			hdr->restoration.unit_size[0] = 8;
	}

	if (!hdr->all_lossless)
		hdr->txfm_mode = gb_bit(gb) ? DAV1D_TX_SWITCHABLE : DAV1D_TX_LARGEST;
	if (IS_INTER_OR_SWITCH(hdr))
		hdr->switchable_comp_refs = (uint8_t)gb_bit(gb);
	if (hdr->switchable_comp_refs && IS_INTER_OR_SWITCH(hdr) && seqhdr->order_hint)
	{
		const int poc = hdr->frame_offset;
		int off_before = -1;
		int off_after = -1;
		int off_before_idx = 0;
		int off_after_idx = 0;
		for (int i = 0; i < 7; i++)
		{
			const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[i]);
			if (!ref)
				return -1;
			const int refpoc = ref->d.frame_offset;

			const int diff = get_poc_diff(seqhdr->order_hint_n_bits, refpoc, poc);
			if (diff > 0)
			{
				if (off_after < 0 || get_poc_diff(seqhdr->order_hint_n_bits, off_after, refpoc) > 0)
				{
					off_after = refpoc;
					off_after_idx = i;
				}
			}
			else if (diff < 0 && (off_before < 0 ||
			                      get_poc_diff(seqhdr->order_hint_n_bits, refpoc, off_before) > 0))
			{
				off_before = refpoc;
				off_before_idx = i;
			}
		}

		if ((off_before | off_after) >= 0)
		{
			hdr->skip_mode_refs[0] = (int8_t)imin(off_before_idx, off_after_idx);
			hdr->skip_mode_refs[1] = (int8_t)imax(off_before_idx, off_after_idx);
			hdr->skip_mode_allowed = 1;
		}
		else if (off_before >= 0)
		{
			int off_before2 = -1;
			int off_before2_idx = 0;
			for (int i = 0; i < 7; i++)
			{
				const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, hdr->refidx[i]);
				if (!ref)
					return -1;
				const int refpoc = ref->d.frame_offset;
				if (get_poc_diff(seqhdr->order_hint_n_bits, refpoc, off_before) < 0)
				{
					if (off_before2 < 0 ||
					    get_poc_diff(seqhdr->order_hint_n_bits, refpoc, off_before2) > 0)
					{
						off_before2 = refpoc;
						off_before2_idx = i;
					}
				}
			}

			if (off_before2 >= 0)
			{
				hdr->skip_mode_refs[0] = (int8_t)imin(off_before_idx, off_before2_idx);
				hdr->skip_mode_refs[1] = (int8_t)imax(off_before_idx, off_before2_idx);
				hdr->skip_mode_allowed = 1;
			}
		}
	}
	if (hdr->skip_mode_allowed)
		hdr->skip_mode_enabled = (uint8_t)gb_bit(gb);
	if (!hdr->error_resilient_mode && IS_INTER_OR_SWITCH(hdr) && seqhdr->warped_motion)
		hdr->warp_motion = (uint8_t)gb_bit(gb);
	hdr->reduced_txtp_set = (uint8_t)gb_bit(gb);

	for (int i = 0; i < 7; i++)
		hdr->gmv[i] = default_wm_params;

	if (IS_INTER_OR_SWITCH(hdr))
	{
		for (int i = 0; i < 7; i++)
		{
			hdr->gmv[i].type = !gb_bit(gb)  ? DAV1D_WM_TYPE_IDENTITY
			                   : gb_bit(gb) ? DAV1D_WM_TYPE_ROT_ZOOM
			                   : gb_bit(gb) ? DAV1D_WM_TYPE_TRANSLATION
			                                : DAV1D_WM_TYPE_AFFINE;

			if (hdr->gmv[i].type != DAV1D_WM_TYPE_IDENTITY)
			{
				const Dav1dWarpedMotionParams* ref_gmv = &default_wm_params;
				if (hdr->primary_ref_frame != DAV1D_PRIMARY_REF_NONE)
				{
					const AV1_OBU_FRAME_HEADER* ref =
					    ref_hdr(p, hdr->refidx[hdr->primary_ref_frame]);
					if (!ref)
						return -1;
					ref_gmv = &ref->d.gmv[i];
				}
				int32_t* const mat = hdr->gmv[i].matrix;
				const int32_t* const ref_mat = ref_gmv->matrix;
				int bits = 0;
				int shift = 0;

				if (hdr->gmv[i].type >= DAV1D_WM_TYPE_ROT_ZOOM)
				{
					mat[2] = (1 << 16) + 2 * gb_subexp(gb, (ref_mat[2] - (1 << 16)) >> 1, 12);
					mat[3] = 2 * gb_subexp(gb, ref_mat[3] >> 1, 12);

					bits = 12;
					shift = 10;
				}
				else
				{
					bits = 9 - !hdr->hp;
					shift = 13 + !hdr->hp;
				}

				if (hdr->gmv[i].type == DAV1D_WM_TYPE_AFFINE)
				{
					mat[4] = 2 * gb_subexp(gb, ref_mat[4] >> 1, 12);
					mat[5] = (1 << 16) + 2 * gb_subexp(gb, (ref_mat[5] - (1 << 16)) >> 1, 12);
				}
				else
				{
					mat[4] = -mat[3];
					mat[5] = mat[2];
				}

				mat[0] = gb_subexp(gb, ref_mat[0] >> shift, (unsigned)bits) * (1 << shift);
				mat[1] = gb_subexp(gb, ref_mat[1] >> shift, (unsigned)bits) * (1 << shift);
			}
			p->cur.gmInvalid[i] = shear_invalid(&hdr->gmv[i]) ? 1 : 0;
		}
	}

	if (seqhdr->film_grain_present && (hdr->show_frame || hdr->showable_frame))
	{
		hdr->film_grain.present = (uint8_t)gb_bit(gb);
		if (hdr->film_grain.present)
		{
			const unsigned seed = gb_bits(gb, 16);
			hdr->film_grain.update =
			    (uint8_t)(hdr->frame_type != DAV1D_FRAME_TYPE_INTER || gb_bit(gb));
			if (!hdr->film_grain.update)
			{
				const int refidx = (int)gb_bits(gb, 3);
				int i = 0;
				for (; i < 7; i++)
					if (hdr->refidx[i] == refidx)
						break;
				const AV1_OBU_FRAME_HEADER* ref = ref_hdr(p, refidx);
				if (i == 7 || !ref)
					return -1;
				hdr->film_grain.data = ref->d.film_grain.data;
				hdr->film_grain.data.seed = seed;
			}
			else
			{
				Dav1dFilmGrainData* const fgd = &hdr->film_grain.data;
				fgd->seed = seed;

				fgd->num_y_points = (int)gb_bits(gb, 4);
				if (fgd->num_y_points > 14)
					return -1;
				for (int i = 0; i < fgd->num_y_points; i++)
				{
					fgd->y_points[i][0] = (uint8_t)gb_bits(gb, 8);
					if (i && fgd->y_points[i - 1][0] >= fgd->y_points[i][0])
						return -1;
					fgd->y_points[i][1] = (uint8_t)gb_bits(gb, 8);
				}

				if (!seqhdr->monochrome)
					fgd->chroma_scaling_from_luma = (int)gb_bit(gb);
				if (seqhdr->monochrome || fgd->chroma_scaling_from_luma ||
				    (seqhdr->ss_ver == 1 && seqhdr->ss_hor == 1 && !fgd->num_y_points))
				{
					fgd->num_uv_points[0] = fgd->num_uv_points[1] = 0;
				}
				else
				{
					for (int pl = 0; pl < 2; pl++)
					{
						fgd->num_uv_points[pl] = (int)gb_bits(gb, 4);
						if (fgd->num_uv_points[pl] > 10)
							return -1;
						for (int i = 0; i < fgd->num_uv_points[pl]; i++)
						{
							fgd->uv_points[pl][i][0] = (uint8_t)gb_bits(gb, 8);
							if (i && fgd->uv_points[pl][i - 1][0] >= fgd->uv_points[pl][i][0])
								return -1;
							fgd->uv_points[pl][i][1] = (uint8_t)gb_bits(gb, 8);
						}
					}
				}

				if (seqhdr->ss_hor == 1 && seqhdr->ss_ver == 1 &&
				    !!fgd->num_uv_points[0] != !!fgd->num_uv_points[1])
					return -1;

				fgd->scaling_shift = (int)gb_bits(gb, 2) + 8;
				fgd->ar_coeff_lag = (int)gb_bits(gb, 2);
				const int num_y_pos = 2 * fgd->ar_coeff_lag * (fgd->ar_coeff_lag + 1);
				if (fgd->num_y_points)
					for (int i = 0; i < num_y_pos; i++)
						fgd->ar_coeffs_y[i] = (int8_t)((int)gb_bits(gb, 8) - 128);
				for (int pl = 0; pl < 2; pl++)
				{
					if (fgd->num_uv_points[pl] || fgd->chroma_scaling_from_luma)
					{
						const int num_uv_pos = num_y_pos + !!fgd->num_y_points;
						for (int i = 0; i < num_uv_pos; i++)
							fgd->ar_coeffs_uv[pl][i] = (int8_t)((int)gb_bits(gb, 8) - 128);
						if (!fgd->num_y_points)
							fgd->ar_coeffs_uv[pl][num_uv_pos] = 0;
					}
				}
				fgd->ar_coeff_shift = gb_bits(gb, 2) + 6;
				fgd->grain_scale_shift = (int)gb_bits(gb, 2);
				for (int pl = 0; pl < 2; pl++)
				{
					if (fgd->num_uv_points[pl])
					{
						fgd->uv_mult[pl] = (int)gb_bits(gb, 8) - 128;
						fgd->uv_luma_mult[pl] = (int)gb_bits(gb, 8) - 128;
						fgd->uv_offset[pl] = (int)gb_bits(gb, 9) - 256;
					}
				}
				fgd->overlap_flag = (int)gb_bit(gb);
				fgd->clip_to_restricted_range = (int)gb_bit(gb);
			}
		}
	}

	return gb->error ? -1 : 0;
}

/* ---- the OBU loop ---- */

static void refresh(AV1_OBU_PARSER* p, const AV1_OBU_FRAME_HEADER* hdr, BYTE flags)
{
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
	{
		if (flags & (1u << i))
		{
			p->ref[i] = *hdr;
			p->refValid[i] = TRUE;
		}
	}
}

static void apply_pending(AV1_OBU_PARSER* p)
{
	if (p->pendingRefresh)
		refresh(p, &p->pendingHdr, p->pendingRefresh);
	p->pendingRefresh = 0;
}

AV1_OBU_PARSER* av1_obu_parser_new(void)
{
	return calloc(1, sizeof(AV1_OBU_PARSER));
}

void av1_obu_parser_free(AV1_OBU_PARSER* p)
{
	free(p);
}

void av1_obu_parser_reset(AV1_OBU_PARSER* p)
{
	if (!p)
		return;
	p->haveSeq = FALSE;
	p->newSequence = FALSE;
	p->haveCur = FALSE;
	p->pendingRefresh = 0;
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		p->refValid[i] = FALSE;
	p->data = nullptr;
	p->size = p->pos = 0;
}

void av1_obu_parser_begin(AV1_OBU_PARSER* p, const BYTE* data, size_t size)
{
	WINPR_ASSERT(p);
	apply_pending(p);
	p->data = data;
	p->size = data ? size : 0;
	p->pos = 0;
	p->failed = FALSE;
	/* a frame doesn't span temporal units */
	p->haveCur = FALSE;
}

const Dav1dSequenceHeader* av1_obu_parser_sequence(const AV1_OBU_PARSER* p)
{
	return (p && p->haveSeq) ? &p->seq : nullptr;
}

const AV1_OBU_FRAME_HEADER* av1_obu_parser_reference(const AV1_OBU_PARSER* p, size_t slot)
{
	if (!p || (slot >= DAV1D_NUM_REF_FRAMES) || !p->refValid[slot])
		return nullptr;
	return &p->ref[slot];
}

static int on_sequence_header(AV1_OBU_PARSER* p, const BYTE* obu, size_t obuSize)
{
	Dav1dSequenceHeader seq = WINPR_C_ARRAY_INIT;
	if (dav1d_parse_sequence_header(&seq, obu, obuSize) < 0)
		return -1;

	if (!p->haveSeq ||
	    memcmp(&seq, &p->seq, offsetof(Dav1dSequenceHeader, operating_parameter_info)) != 0)
	{
		/* a new video sequence: nothing of the old one stays */
		for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
			p->refValid[i] = FALSE;
		p->haveCur = FALSE;
		p->newSequence = TRUE;
	}
	p->seq = seq;
	p->haveSeq = TRUE;
	p->operatingPointIdc = seq.operating_points[0].idc;
	return 0;
}

/* the tile group at gb (after its OBU header), up to obuEnd */
static int on_tile_group(AV1_OBU_PARSER* p, GetBits* gb, const BYTE* obuEnd)
{
	const Dav1dFrameHeader* const hdr = &p->cur.d;
	const UINT32 nTiles = (UINT32)hdr->tiling.cols * hdr->tiling.rows;
	const int have_tile_pos = nTiles > 1 ? (int)gb_bit(gb) : 0;
	UINT32 start = 0;
	UINT32 end = nTiles - 1;

	if (have_tile_pos)
	{
		const int n_bits = hdr->tiling.log2_cols + hdr->tiling.log2_rows;
		start = gb_bits(gb, n_bits);
		end = gb_bits(gb, n_bits);
	}
	gb_bytealign(gb);
	if (gb->error || (gb->ptr > obuEnd))
		return -1;
	/* tile groups in order, see 6.10.1 */
	if ((start > end) || (start != p->nTilesSeen) || (end >= nTiles))
		return -1;

	const BYTE* t = gb->ptr;
	for (UINT32 n = start; n <= end; n++)
	{
		size_t size = 0;
		if (n == end)
			size = (size_t)(obuEnd - t);
		else
		{
			const size_t nb = hdr->tiling.n_bytes;
			if ((size_t)(obuEnd - t) < nb)
				return -1;
			for (size_t b = 0; b < nb; b++)
				size |= (size_t)t[b] << (8 * b);
			size++;
			t += nb;
		}
		if ((size == 0) || (size > (size_t)(obuEnd - t)) || (size > UINT32_MAX))
			return -1;
		AV1_OBU_TILE* tile = &p->tiles[n];
		tile->offset = (UINT32)(t - p->data);
		tile->size = (UINT32)size;
		tile->row = (UINT16)(n / hdr->tiling.cols);
		tile->column = (UINT16)(n % hdr->tiling.cols);
		t += size;
	}
	p->nTilesSeen = end + 1;
	return 0;
}

int av1_obu_parser_next(AV1_OBU_PARSER* p, AV1_OBU_EVENT* ev)
{
	WINPR_ASSERT(p);
	WINPR_ASSERT(ev);

	const AV1_OBU_EVENT empty = WINPR_C_ARRAY_INIT;
	*ev = empty;

	apply_pending(p);
	if (p->failed)
		return -1;
	if (p->size > UINT32_MAX)
		goto fail;

	while (p->pos < p->size)
	{
		GetBits gb = WINPR_C_ARRAY_INIT;
		const BYTE* const obuStart = &p->data[p->pos];
		gb_init(&gb, obuStart, p->size - p->pos);

		/* obu header */
		gb_bit(&gb); /* forbidden */
		const unsigned type = gb_bits(&gb, 4);
		const unsigned has_extension = gb_bit(&gb);
		const unsigned has_length_field = gb_bit(&gb);
		gb_bit(&gb); /* reserved */

		unsigned temporal_id = 0;
		unsigned spatial_id = 0;
		if (has_extension)
		{
			temporal_id = gb_bits(&gb, 3);
			spatial_id = gb_bits(&gb, 2);
			gb_bits(&gb, 3);
		}

		const BYTE* obuEnd = gb.ptr_end;
		if (has_length_field)
		{
			const size_t len = gb_uleb128(&gb);
			if (gb.error || (len > (size_t)(gb.ptr_end - gb.ptr)))
				goto fail;
			obuEnd = gb.ptr + len;
		}
		if (gb.error)
			goto fail;
		gb.ptr_end = obuEnd;
		p->pos = (size_t)(obuEnd - p->data);

		/* skip OBUs that are not in operating point 0 */
		if ((type != DAV1D_OBU_SEQ_HDR) && (type != DAV1D_OBU_TD) && has_extension &&
		    (p->operatingPointIdc != 0))
		{
			const unsigned in_temporal_layer = (p->operatingPointIdc >> temporal_id) & 1;
			const unsigned in_spatial_layer = (p->operatingPointIdc >> (spatial_id + 8)) & 1;
			if (!in_temporal_layer || !in_spatial_layer)
				continue;
		}

		switch (type)
		{
			case DAV1D_OBU_SEQ_HDR:
				if (on_sequence_header(p, obuStart, (size_t)(obuEnd - obuStart)) < 0)
					goto fail;
				break;

			case DAV1D_OBU_TD:
				p->haveCur = FALSE;
				break;

			case DAV1D_OBU_REDUNDANT_FRAME_HDR:
				if (p->haveCur)
					break;
				WINPR_FALLTHROUGH
			case DAV1D_OBU_FRAME:
			case DAV1D_OBU_FRAME_HDR:
			{
				if (!p->haveSeq)
					goto fail;
				memset(&p->cur, 0, sizeof(p->cur));
				p->cur.d.temporal_id = (uint8_t)temporal_id;
				p->cur.d.spatial_id = (uint8_t)spatial_id;
				p->haveCur = FALSE;
				p->nTilesSeen = 0;
				if (parse_frame_hdr(p, &gb) < 0)
					goto fail;

				if (p->cur.d.show_existing_frame)
				{
					if (type == DAV1D_OBU_FRAME)
						goto fail;
					const BYTE idx = p->cur.d.existing_frame_idx;
					if (!p->refValid[idx])
						goto fail;
					ev->type = AV1_OBU_EVENT_SHOW_EXISTING;
					ev->frameIdx = idx;
					ev->newSequence = p->newSequence;
					p->newSequence = FALSE;
					/* showing a key frame again resets the references to it (7.21) */
					if (p->ref[idx].d.frame_type == DAV1D_FRAME_TYPE_KEY)
					{
						ev->refreshFrameFlags = 0xff;
						p->pendingHdr = p->ref[idx];
						p->pendingRefresh = 0xff;
					}
					return 1;
				}
				p->haveCur = TRUE;
				if (type != DAV1D_OBU_FRAME)
					break;
				/* the tile group of the frame OBU follows */
				gb_bytealign(&gb);
			}
				WINPR_FALLTHROUGH
			case DAV1D_OBU_TILE_GRP:
			{
				if (!p->haveCur)
					goto fail;
				if (on_tile_group(p, &gb, obuEnd) < 0)
					goto fail;
				const UINT32 nTiles = (UINT32)p->cur.d.tiling.cols * p->cur.d.tiling.rows;
				if (p->nTilesSeen < nTiles)
					break;

				/* the frame is complete */
				p->haveCur = FALSE;
				ev->type = AV1_OBU_EVENT_FRAME;
				ev->newSequence = p->newSequence;
				p->newSequence = FALSE;
				ev->tiles = p->tiles;
				ev->nTiles = nTiles;
				ev->refreshFrameFlags = p->cur.d.refresh_frame_flags;
				p->pendingHdr = p->cur;
				p->pendingRefresh = p->cur.d.refresh_frame_flags;
				ev->hdr = &p->pendingHdr;
				return 1;
			}

			default:
				/* metadata, padding, reserved */
				break;
		}
	}
	return 0;

fail:
	p->failed = TRUE;
	p->haveCur = FALSE;
	return -1;
}
