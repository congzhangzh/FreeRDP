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
 */

#ifndef FREERDP_LIB_CODEC_AV1_OBU_H
#define FREERDP_LIB_CODEC_AV1_OBU_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>

#include <dav1d/headers.h>

/** A hardware decoder (DXVA, VA-API, ...) gets the parsed headers and finds the tiles itself in
 * the bitstream. The headers are dav1d's public structures, filled the way dav1d fills them. */

/** A frame header with what a hardware decoder needs on top of dav1d's. */
typedef struct
{
	Dav1dFrameHeader d;
	/** per segment: bit n set when feature n was coded (alt_q, alt_lf_y_v, alt_lf_y_h,
	 * alt_lf_u, alt_lf_v, ref_frame, skip, globalmv) */
	BYTE segFeatureMask[DAV1D_MAX_SEGMENTS];
	/** per reference (LAST..ALTREF): the global motion parameters can't be used for warping */
	BYTE gmInvalid[DAV1D_REFS_PER_FRAME];
} AV1_OBU_FRAME_HEADER;

typedef struct
{
	UINT32 offset; /**< tile data offset in the temporal unit */
	UINT32 size;
	UINT16 row;
	UINT16 column;
} AV1_OBU_TILE;

typedef enum
{
	AV1_OBU_EVENT_END = 0,      /**< the temporal unit is done */
	AV1_OBU_EVENT_FRAME,        /**< a frame and all its tiles: decode it */
	AV1_OBU_EVENT_SHOW_EXISTING /**< show reference slot frameIdx again */
} AV1_OBU_EVENT_TYPE;

typedef struct
{
	AV1_OBU_EVENT_TYPE type;
	/** a sequence header that starts a new sequence came before this frame: all references
	 * are gone */
	BOOL newSequence;
	const AV1_OBU_FRAME_HEADER* hdr; /**< FRAME */
	const AV1_OBU_TILE* tiles;       /**< FRAME: tiles in tile order */
	UINT32 nTiles;
	BYTE frameIdx; /**< SHOW_EXISTING */
	/** the reference slots this event refreshes, with the decoded (or shown key) frame */
	BYTE refreshFrameFlags;
} AV1_OBU_EVENT;

typedef struct S_AV1_OBU_PARSER AV1_OBU_PARSER;

FREERDP_LOCAL AV1_OBU_PARSER* av1_obu_parser_new(void);
FREERDP_LOCAL void av1_obu_parser_free(AV1_OBU_PARSER* p);

/** Forget the stream: the next frame needs a sequence header. */
FREERDP_LOCAL void av1_obu_parser_reset(AV1_OBU_PARSER* p);

/** Start a temporal unit (low overhead bitstream format). The data must stay valid until
 * av1_obu_parser_next() returned AV1_OBU_EVENT_END. */
FREERDP_LOCAL void av1_obu_parser_begin(AV1_OBU_PARSER* p, const BYTE* data, size_t size);

/** The next frame of the temporal unit.
 *
 * The reference headers advance with each event, as if the frame was decoded: the slots of
 * an event's refreshFrameFlags take its frame when av1_obu_parser_next() or
 * av1_obu_parser_begin() is called again, so until then av1_obu_parser_reference() still
 * gives the references the frame was coded with. The event's pointers stay valid as long.
 * @return 1 and @p ev filled, 0 at the end of the temporal unit, or < 0 for invalid data or
 *         a frame before any sequence header (the rest of the temporal unit is skipped). */
FREERDP_LOCAL int av1_obu_parser_next(AV1_OBU_PARSER* p, AV1_OBU_EVENT* ev);

/** The sequence header, or nullptr before the first one. */
FREERDP_LOCAL const Dav1dSequenceHeader* av1_obu_parser_sequence(const AV1_OBU_PARSER* p);

/** The frame header in reference slot @p slot, or nullptr when the slot is empty. */
FREERDP_LOCAL const AV1_OBU_FRAME_HEADER* av1_obu_parser_reference(const AV1_OBU_PARSER* p,
                                                                   size_t slot);

#endif /* FREERDP_LIB_CODEC_AV1_OBU_H */
