/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 encoding with VA-API (Linux)
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
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/wlog.h>

#include <freerdp/types.h>

#include "av1_vaapi_enc.h"
#include "av1_vaapi_lib.h"

#include <va/va_enc_av1.h>

/* The application writes the sequence header and the frame headers (packed headers) and the
 * driver the tile groups; a driver that writes headers of its own (Mesa) reads them from the
 * packed ones. The bit offsets in the picture parameters let a driver's rate control put its
 * quantizer, loop filter and CDEF into the frame header (intel-media). */

#define AV1_OBU_SEQUENCE_HEADER 1
#define AV1_OBU_TEMPORAL_DELIMITER 2
#define AV1_OBU_FRAME_HEADER 3
#define AV1_OBU_FRAME 6

#define AV1_FRAME_KEY 0
#define AV1_FRAME_INTER 1
#define AV1_PRIMARY_REF_NONE 7
#define AV1_REF_FRAME_LAST 1
#define AV1_REFS_PER_FRAME 7
#define AV1_TX_MODE_LARGEST 1
#define AV1_TX_MODE_SELECT 2

#define AV1_SB 64 /* the superblock size: no 128x128 */
#define AV1_MAX_TILE_COLS 64
#define AV1_MAX_TILE_ROWS 64
#define AV1_ORDER_HINT_BITS 8
#define AV1_HDR_MAX 512   /* the most a packed header may take, bytes */
#define AV1_GOP 100000    /* one key frame, at the start */
#define AV1_FRAME_RATE 30 /* until the frames say otherwise */
#define AV1_RECON 2       /* the frame being encoded and the one it refers to */

typedef struct
{
	BYTE buf[AV1_HDR_MAX];
	size_t bits;
	BOOL overflow;
} AV1_BITS;

struct S_AV1_VAAPI_ENC
{
	wLog* log;
	char name[256];
	INT32 id; /* the device's id in av1_vaapi_enc_list() */

	int fd;
	VADisplay dpy;
	VAEntrypoint entrypoint;
	VAConfigID config;
	VAContextID context;
	VASurfaceID surfaces[1 + AV1_RECON]; /* the input, then the reconstructed frames */
	BOOL haveSurfaces;
	VABufferID coded;
	VAImage image;
	BOOL haveImage;

	UINT32 width;
	UINT32 height;
	UINT32 surfaceWidth;
	UINT32 surfaceHeight;

	/* what the driver can */
	UINT32 rcMode; /* VA_RC_* */
	UINT32 packed; /* VA_ENC_PACKED_HEADER_SEQUENCE | _PICTURE, or what of it the driver takes */
	UINT32 obuSizeBytes;
	UINT32 tileSizeBytesMinus1;
	BOOL txSelect;
	BOOL intraEdgeFilter;

	/* the stream */
	AV1_VAAPI_ENC_RATE rate;
	UINT32 kbps;
	UINT32 qindex; /* CQP; a rate control's start otherwise */
	BYTE level;    /* seq_level_idx */
	UINT32 sbCols;
	UINT32 sbRows;
	UINT32 tileColsLog2;
	UINT32 tileRowsLog2;
	UINT32 tileCols;
	UINT32 tileRows;
	UINT32 tileWidthSb;
	UINT32 tileHeightSb;
	AV1_BITS seqHdr;

	UINT64 frames;    /* encoded since the key frame */
	int last;         /* the reconstructed frame the next one refers to, -1: none */
	double frameMs;   /* the mean frame duration, moving */
	UINT32 sentFps10; /* the frame rate the driver was told, x 10 (0: none yet) */
	BOOL warnedHeaders;

	BYTE* raw; /* the coded frame as the driver wrote it */
	size_t rawSize;
	size_t rawCapacity;
	BYTE* out;
	size_t outSize;
	size_t outCapacity;
};

static void put_bits(AV1_BITS* b, UINT32 value, unsigned n)
{
	for (unsigned i = n; i-- > 0;)
	{
		if ((b->bits >> 3) >= sizeof(b->buf))
		{
			b->overflow = TRUE;
			return;
		}
		if ((value >> i) & 1u)
			b->buf[b->bits >> 3] |= (BYTE)(0x80u >> (b->bits & 7u));
		b->bits++;
	}
}

/* An OBU with a size field of @p sizeBytes bytes: its header and a size to be filled in by
 * end_obu(). */
static void begin_obu(AV1_BITS* b, UINT32 type)
{
	memset(b, 0, sizeof(*b));
	put_bits(b, 0, 1);    /* obu_forbidden_bit */
	put_bits(b, type, 4); /* obu_type */
	put_bits(b, 0, 1);    /* obu_extension_flag */
	put_bits(b, 1, 1);    /* obu_has_size_field */
	put_bits(b, 0, 1);    /* obu_reserved_1bit */
}

static void end_obu(AV1_BITS* b, UINT32 sizeBytes)
{
	/* trailing_bits() */
	put_bits(b, 1, 1);
	while (b->bits & 7u)
		put_bits(b, 0, 1);
	if (b->overflow)
		return;
	/* the payload follows the header byte and the size: leb128 in exactly sizeBytes bytes */
	const size_t payload = (b->bits >> 3) - 1 - sizeBytes;
	if (payload >= (1ull << (7 * sizeBytes)))
	{
		b->overflow = TRUE;
		return;
	}
	for (UINT32 i = 0; i < sizeBytes; i++)
	{
		BYTE v = (BYTE)((payload >> (7 * i)) & 0x7f);
		if (i + 1 < sizeBytes)
			v |= 0x80;
		b->buf[1 + i] = v;
	}
}

static void put_size_field(AV1_BITS* b, UINT32 sizeBytes)
{
	put_bits(b, 0, 8 * sizeBytes); /* filled in by end_obu() */
}

static UINT32 bit_length(UINT32 v)
{
	UINT32 n = 0;
	while (v)
	{
		n++;
		v >>= 1;
	}
	return n;
}

static UINT32 tile_log2(UINT32 blkSize, UINT32 target)
{
	UINT32 k = 0;
	while ((blkSize << k) < target)
		k++;
	return k;
}

/* The lowest level of the size (the x.1 one, for frame rate), 31 when none fits. */
static BYTE pick_level(UINT32 w, UINT32 h)
{
	const UINT64 area = 1ull * w * h;
	if ((area <= 2359296) && (w <= 6144) && (h <= 3456))
		return 9; /* 4.1 */
	if ((area <= 8912896) && (w <= 8192) && (h <= 4352))
		return 13; /* 5.1 */
	if ((area <= 35651584) && (w <= 16384) && (h <= 8704))
		return 17; /* 6.1 */
	return 31;
}

/* The uniform tile layout with the fewest tiles (tile_info() of the specification). */
static BOOL setup_tiles(AV1_VAAPI_ENC* enc, UINT32 maxTiles)
{
	const UINT32 miCols = 2 * ((enc->width + 7) >> 3);
	const UINT32 miRows = 2 * ((enc->height + 7) >> 3);
	enc->sbCols = (miCols + 15) >> 4;
	enc->sbRows = (miRows + 15) >> 4;
	const UINT32 maxTileWidthSb = 4096 / AV1_SB;
	const UINT32 maxTileAreaSb = (4096 * 2304) / (AV1_SB * AV1_SB);
	const UINT32 minLog2TileCols = tile_log2(maxTileWidthSb, enc->sbCols);
	const UINT32 minLog2Tiles =
	    MAX(minLog2TileCols, tile_log2(maxTileAreaSb, enc->sbRows * enc->sbCols));

	enc->tileColsLog2 = minLog2TileCols;
	enc->tileWidthSb = (enc->sbCols + (1u << enc->tileColsLog2) - 1) >> enc->tileColsLog2;
	enc->tileCols = (enc->sbCols + enc->tileWidthSb - 1) / enc->tileWidthSb;
	enc->tileRowsLog2 = (minLog2Tiles > enc->tileColsLog2) ? minLog2Tiles - enc->tileColsLog2 : 0;
	enc->tileHeightSb = (enc->sbRows + (1u << enc->tileRowsLog2) - 1) >> enc->tileRowsLog2;
	enc->tileRows = (enc->sbRows + enc->tileHeightSb - 1) / enc->tileHeightSb;
	if ((enc->tileCols > AV1_MAX_TILE_COLS) || (enc->tileRows > AV1_MAX_TILE_ROWS))
		return FALSE;
	return (maxTiles == 0) || (enc->tileCols * enc->tileRows <= maxTiles);
}

static void write_tile_info(const AV1_VAAPI_ENC* enc, AV1_BITS* b)
{
	const UINT32 maxLog2TileCols = tile_log2(1, MIN(enc->sbCols, AV1_MAX_TILE_COLS));
	const UINT32 maxLog2TileRows = tile_log2(1, MIN(enc->sbRows, AV1_MAX_TILE_ROWS));
	put_bits(b, 1, 1); /* uniform_tile_spacing_flag */
	if (enc->tileColsLog2 < maxLog2TileCols)
		put_bits(b, 0, 1); /* increment_tile_cols_log2: no more than the fewest */
	if (enc->tileRowsLog2 < maxLog2TileRows)
		put_bits(b, 0, 1); /* increment_tile_rows_log2 */
	if ((enc->tileColsLog2 > 0) || (enc->tileRowsLog2 > 0))
	{
		put_bits(b, 0, enc->tileColsLog2 + enc->tileRowsLog2); /* context_update_tile_id */
		put_bits(b, enc->tileSizeBytesMinus1, 2);              /* tile_size_bytes_minus_1 */
	}
}

static void write_sequence_header(AV1_VAAPI_ENC* enc)
{
	AV1_BITS* b = &enc->seqHdr;
	const UINT32 wBits = MAX(bit_length(enc->width - 1), 1);
	const UINT32 hBits = MAX(bit_length(enc->height - 1), 1);

	begin_obu(b, AV1_OBU_SEQUENCE_HEADER);
	put_size_field(b, enc->obuSizeBytes);
	put_bits(b, 0, 3);          /* seq_profile: 8 bit 4:2:0 */
	put_bits(b, 0, 1);          /* still_picture */
	put_bits(b, 0, 1);          /* reduced_still_picture_header */
	put_bits(b, 0, 1);          /* timing_info_present_flag */
	put_bits(b, 0, 1);          /* initial_display_delay_present_flag */
	put_bits(b, 0, 5);          /* operating_points_cnt_minus_1 */
	put_bits(b, 0, 12);         /* operating_point_idc[0] */
	put_bits(b, enc->level, 5); /* seq_level_idx[0] */
	if (enc->level > 7)
		put_bits(b, 0, 1);                        /* seq_tier[0] */
	put_bits(b, wBits - 1, 4);                    /* frame_width_bits_minus_1 */
	put_bits(b, hBits - 1, 4);                    /* frame_height_bits_minus_1 */
	put_bits(b, enc->width - 1, wBits);           /* max_frame_width_minus_1 */
	put_bits(b, enc->height - 1, hBits);          /* max_frame_height_minus_1 */
	put_bits(b, 0, 1);                            /* frame_id_numbers_present_flag */
	put_bits(b, 0, 1);                            /* use_128x128_superblock */
	put_bits(b, 0, 1);                            /* enable_filter_intra */
	put_bits(b, enc->intraEdgeFilter ? 1 : 0, 1); /* enable_intra_edge_filter */
	put_bits(b, 0, 1);                            /* enable_interintra_compound */
	put_bits(b, 0, 1);                            /* enable_masked_compound */
	put_bits(b, 0, 1);                            /* enable_warped_motion */
	put_bits(b, 0, 1);                            /* enable_dual_filter */
	put_bits(b, 1, 1);                            /* enable_order_hint */
	put_bits(b, 0, 1);                            /* enable_jnt_comp */
	put_bits(b, 0, 1);                            /* enable_ref_frame_mvs */
	put_bits(b, 0, 1);                            /* seq_choose_screen_content_tools */
	put_bits(b, 0, 1);                            /* seq_force_screen_content_tools */
	put_bits(b, AV1_ORDER_HINT_BITS - 1, 3);      /* order_hint_bits_minus_1 */
	put_bits(b, 0, 1);                            /* enable_superres */
	put_bits(b, 1, 1);                            /* enable_cdef */
	put_bits(b, 0, 1);                            /* enable_restoration */
	/* color_config() */
	put_bits(b, 0, 1); /* high_bitdepth */
	put_bits(b, 0, 1); /* mono_chrome */
	put_bits(b, 0, 1); /* color_description_present_flag */
	put_bits(b, 0, 1); /* color_range: studio, as libaom */
	put_bits(b, 0, 2); /* chroma_sample_position: unknown */
	put_bits(b, 0, 1); /* separate_uv_delta_q */
	put_bits(b, 0, 1); /* film_grain_params_present */
	end_obu(b, enc->obuSizeBytes);
}

/* An approximation of the AV1 8 bit AC quantizer (Ac_Qlookup[0]) by straight lines through
 * some of its points: enough to pick a loop filter level. */
static UINT32 ac_quant(UINT32 qindex)
{
	static const UINT32 points[][2] = {
		{ 0, 4 }, { 104, 111 }, { 150, 260 }, { 200, 658 }, { 255, 1828 }
	};
	for (size_t i = 1; i < ARRAYSIZE(points); i++)
	{
		if (qindex <= points[i][0])
		{
			const UINT32 q0 = points[i - 1][0];
			const UINT32 v0 = points[i - 1][1];
			return v0 + (points[i][1] - v0) * (qindex - q0) / (points[i][0] - q0);
		}
	}
	return 1828;
}

/* With constant quantizers the header's loop filter is what the frame gets: the level libaom
 * picks from the quantizer (its LPF_PICK_FROM_Q model, 8 bit), 4 lower on key frames. */
static UINT32 cqp_filter_level(UINT32 qindex, BOOL key)
{
	INT64 level = ((INT64)ac_quant(qindex) * 20723 + 1015158) >> 18;
	if (key)
		level -= 4;
	if (level < 0)
		level = 0;
	if (level > 63)
		level = 63;
	return (UINT32)level;
}

typedef struct
{
	AV1_BITS bits;
	UINT32 qindexOffset;
	UINT32 segmentationOffset;
	UINT32 loopFilterOffset;
	UINT32 cdefOffset;
	UINT32 cdefBits;
} AV1_FRAME_HDR;

/* The frame header OBU for frame @p pic (its parameters already set). */
static BOOL write_frame_header(const AV1_VAAPI_ENC* enc, const VAEncPictureParameterBufferAV1* pic,
                               AV1_FRAME_HDR* fh)
{
	AV1_BITS* b = &fh->bits;
	const BOOL key = (pic->picture_flags.bits.frame_type == AV1_FRAME_KEY);

	begin_obu(b, AV1_OBU_FRAME_HEADER);
	put_size_field(b, enc->obuSizeBytes);
	put_bits(b, 0, 1);                                  /* show_existing_frame */
	put_bits(b, pic->picture_flags.bits.frame_type, 2); /* frame_type */
	put_bits(b, 1, 1);                                  /* show_frame */
	if (!key)
		put_bits(b, 0, 1); /* error_resilient_mode (1 for a shown key frame, not coded) */
	put_bits(b, 0, 1);     /* disable_cdf_update */
	put_bits(b, 0, 1);     /* frame_size_override_flag */
	put_bits(b, pic->order_hint, AV1_ORDER_HINT_BITS);
	if (!key)
	{
		put_bits(b, pic->primary_ref_frame, 3);
		put_bits(b, pic->refresh_frame_flags, 8);
	}
	if (key)
	{
		/* frame_size(), superres_params(): nothing; render_size() */
		put_bits(b, 0, 1); /* render_and_frame_size_different */
	}
	else
	{
		put_bits(b, 0, 1); /* frame_refs_short_signaling */
		for (size_t i = 0; i < AV1_REFS_PER_FRAME; i++)
			put_bits(b, pic->ref_frame_idx[i], 3);
		put_bits(b, 0, 1); /* render_and_frame_size_different */
		put_bits(b, pic->picture_flags.bits.allow_high_precision_mv, 1);
		put_bits(b, 0, 1);                         /* is_filter_switchable */
		put_bits(b, pic->interpolation_filter, 2); /* interpolation_filter */
		put_bits(b, 0, 1);                         /* is_motion_mode_switchable */
	}
	put_bits(b, 0, 1); /* disable_frame_end_update_cdf */
	write_tile_info(enc, b);

	/* quantization_params() */
	fh->qindexOffset = (UINT32)b->bits;
	put_bits(b, pic->base_qindex, 8);
	put_bits(b, 0, 1); /* DeltaQYDc: delta_coded */
	put_bits(b, 0, 1); /* DeltaQUDc */
	put_bits(b, 0, 1); /* DeltaQUAc */
	put_bits(b, 0, 1); /* using_qmatrix */
	/* segmentation_params() */
	fh->segmentationOffset = (UINT32)b->bits;
	put_bits(b, 0, 1); /* segmentation_enabled */
	/* delta_q_params(): base_q_idx is never 0 (lossless) */
	put_bits(b, 0, 1); /* delta_q_present */
	/* loop_filter_params() */
	fh->loopFilterOffset = (UINT32)b->bits;
	put_bits(b, pic->filter_level[0], 6);
	put_bits(b, pic->filter_level[1], 6);
	if (pic->filter_level[0] || pic->filter_level[1])
	{
		put_bits(b, pic->filter_level_u, 6);
		put_bits(b, pic->filter_level_v, 6);
	}
	put_bits(b, pic->loop_filter_flags.bits.sharpness_level, 3);
	put_bits(b, 0, 1); /* loop_filter_delta_enabled */
	/* cdef_params() */
	fh->cdefOffset = (UINT32)b->bits;
	put_bits(b, pic->cdef_damping_minus_3, 2);
	put_bits(b, pic->cdef_bits, 2);
	for (size_t i = 0; i < (1u << pic->cdef_bits); i++)
	{
		put_bits(b, pic->cdef_y_strengths[i] >> 2, 4);
		put_bits(b, pic->cdef_y_strengths[i] & 3, 2);
		put_bits(b, pic->cdef_uv_strengths[i] >> 2, 4);
		put_bits(b, pic->cdef_uv_strengths[i] & 3, 2);
	}
	fh->cdefBits = (UINT32)b->bits - fh->cdefOffset;
	/* lr_params(): no restoration; read_tx_mode() */
	put_bits(b, (pic->mode_control_flags.bits.tx_mode == AV1_TX_MODE_SELECT) ? 1 : 0, 1);
	if (!key)
		put_bits(b, 0, 1); /* reference_select */
	put_bits(b, 0, 1);     /* reduced_tx_set */
	if (!key)
	{
		for (size_t i = 0; i < AV1_REFS_PER_FRAME; i++)
			put_bits(b, 0, 1); /* is_global */
	}
	end_obu(b, enc->obuSizeBytes);
	return !b->overflow;
}

static void free_stream(AV1_VAAPI_ENC* enc)
{
	if (!enc->dpy)
		return;
	if (enc->haveImage)
		(void)av1_va.vaDestroyImage(enc->dpy, enc->image.image_id);
	enc->haveImage = FALSE;
	if (enc->coded != VA_INVALID_ID)
		(void)av1_va.vaDestroyBuffer(enc->dpy, enc->coded);
	enc->coded = VA_INVALID_ID;
	if (enc->context != VA_INVALID_ID)
		(void)av1_va.vaDestroyContext(enc->dpy, enc->context);
	enc->context = VA_INVALID_ID;
	if (enc->haveSurfaces)
		(void)av1_va.vaDestroySurfaces(enc->dpy, enc->surfaces, ARRAYSIZE(enc->surfaces));
	enc->haveSurfaces = FALSE;
}

static void close_device(AV1_VAAPI_ENC* enc)
{
	free_stream(enc);
	if (enc->dpy && (enc->config != VA_INVALID_ID))
		(void)av1_va.vaDestroyConfig(enc->dpy, enc->config);
	av1_va_close(enc->fd, enc->dpy);
	enc->config = VA_INVALID_ID;
	enc->dpy = nullptr;
	enc->fd = -1;
	enc->name[0] = '\0';
}

static BOOL device_encodes_av1(VADisplay dpy)
{
	return av1_va_supports(dpy, VAEntrypointEncSlice) ||
	       av1_va_supports(dpy, VAEntrypointEncSliceLP);
}

static BOOL open_device(AV1_VAAPI_ENC* enc, wLog* log, int node)
{
	return av1_va_open(log, node, device_encodes_av1, &enc->fd, &enc->dpy, enc->name,
	                   sizeof(enc->name));
}

size_t av1_vaapi_enc_list(char names[][256], size_t max)
{
	if (!av1_va_load())
		return 0;
	size_t count = 0;
	for (int i = 0; (i < AV1_VA_RENDER_COUNT) && (count < max); i++)
	{
		AV1_VAAPI_ENC probe = {
			.fd = -1, .config = VA_INVALID_ID, .context = VA_INVALID_ID, .coded = VA_INVALID_ID
		};
		if (open_device(&probe, nullptr, AV1_VA_RENDER_FIRST + i))
		{
			(void)snprintf(names[count], 256, "%s", probe.name);
			count++;
			close_device(&probe);
		}
	}
	return count;
}

INT32 av1_vaapi_enc_device(const AV1_VAAPI_ENC* enc)
{
	return enc ? enc->id : -1;
}

const char* av1_vaapi_enc_name(const AV1_VAAPI_ENC* enc)
{
	return enc ? enc->name : "";
}

void av1_vaapi_enc_free(AV1_VAAPI_ENC* enc)
{
	if (!enc)
		return;
	close_device(enc);
	free(enc->raw);
	free(enc->out);
	free(enc);
}

static UINT32 get_attribute(AV1_VAAPI_ENC* enc, VAConfigAttribType type)
{
	VAConfigAttrib attrib = { .type = type };
	if (av1_va.vaGetConfigAttributes(enc->dpy, VAProfileAV1Profile0, enc->entrypoint, &attrib, 1) !=
	    VA_STATUS_SUCCESS)
		return VA_ATTRIB_NOT_SUPPORTED;
	return attrib.value;
}

#define VA_CHECK(enc, call, what)                                                        \
	do                                                                                   \
	{                                                                                    \
		const VAStatus st_ = (call);                                                     \
		if (st_ != VA_STATUS_SUCCESS)                                                    \
		{                                                                                \
			WLog_Print((enc)->log, WLOG_WARN, "VA-API: %s on %s: %s", what, (enc)->name, \
			           av1_va.vaErrorStr(st_));                                          \
			goto fail;                                                                   \
		}                                                                                \
	} while (0)

/* What the driver can, the rate control it takes, the config. */
static BOOL configure(AV1_VAAPI_ENC* enc)
{
	enc->entrypoint = av1_va_supports(enc->dpy, VAEntrypointEncSlice) ? VAEntrypointEncSlice
	                                                                  : VAEntrypointEncSliceLP;

	const UINT32 maxW = get_attribute(enc, VAConfigAttribMaxPictureWidth);
	const UINT32 maxH = get_attribute(enc, VAConfigAttribMaxPictureHeight);
	if (((maxW != VA_ATTRIB_NOT_SUPPORTED) && (enc->width > maxW)) ||
	    ((maxH != VA_ATTRIB_NOT_SUPPORTED) && (enc->height > maxH)))
	{
		WLog_Print(enc->log, WLOG_INFO, "VA-API: %s encodes at most %" PRIu32 "x%" PRIu32,
		           enc->name, maxW, maxH);
		return FALSE;
	}

	static const UINT32 modes[] = { VA_RC_CBR, VA_RC_VBR, VA_RC_CQP };
	static const char* const names[] = { "CBR", "VBR", "CQP" };
	enc->rcMode = modes[enc->rate];
	const UINT32 rc = get_attribute(enc, VAConfigAttribRateControl);
	if ((rc == VA_ATTRIB_NOT_SUPPORTED) || !(rc & enc->rcMode))
	{
		WLog_Print(enc->log, WLOG_INFO, "VA-API: %s has no %s rate control (0x%08" PRIx32 ")",
		           enc->name, names[enc->rate], rc);
		return FALSE;
	}

	const UINT32 packed = get_attribute(enc, VAConfigAttribEncPackedHeaders);
	enc->packed = (packed == VA_ATTRIB_NOT_SUPPORTED)
	                  ? 0
	                  : packed & (VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE);

	VAConfigAttribValEncAV1 av1 = { .value = get_attribute(enc, VAConfigAttribEncAV1) };
	if (av1.value == VA_ATTRIB_NOT_SUPPORTED)
		av1.value = 0;
	enc->intraEdgeFilter = av1.bits.support_intra_edge_filter != 0;

	/* without it: sizes in 4 bytes, both transform modes */
	VAConfigAttribValEncAV1Ext2 ext2 = { .value = get_attribute(enc, VAConfigAttribEncAV1Ext2) };
	if (ext2.value == VA_ATTRIB_NOT_SUPPORTED)
	{
		ext2.value = 0;
		ext2.bits.obu_size_bytes_minus1 = 3;
		ext2.bits.tile_size_bytes_minus1 = 3;
		ext2.bits.tx_mode_support = 0x06;
	}
	enc->obuSizeBytes = ext2.bits.obu_size_bytes_minus1 + 1u;
	enc->tileSizeBytesMinus1 = ext2.bits.tile_size_bytes_minus1;
	if (!(ext2.bits.tx_mode_support & 0x06))
	{
		WLog_Print(enc->log, WLOG_INFO, "VA-API: %s has no transform mode for AV1 (0x%" PRIx32 ")",
		           enc->name, (UINT32)ext2.bits.tx_mode_support);
		return FALSE;
	}
	enc->txSelect = (ext2.bits.tx_mode_support & 0x04) != 0;

	const UINT32 maxTiles = ext2.bits.max_tile_num_minus1 ? ext2.bits.max_tile_num_minus1 + 1u : 0;
	if (!setup_tiles(enc, maxTiles))
	{
		WLog_Print(enc->log, WLOG_INFO, "VA-API: %s takes too few tiles for %" PRIu32 "x%" PRIu32,
		           enc->name, enc->width, enc->height);
		return FALSE;
	}
	enc->level = pick_level(enc->width, enc->height);

	VAConfigAttrib attribs[3] = { { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
		                          { .type = VAConfigAttribRateControl, .value = enc->rcMode },
		                          { .type = VAConfigAttribEncPackedHeaders,
		                            .value = enc->packed } };
	const int nAttribs = enc->packed ? 3 : 2;
	VA_CHECK(enc,
	         av1_va.vaCreateConfig(enc->dpy, VAProfileAV1Profile0, enc->entrypoint, attribs,
	                               nAttribs, &enc->config),
	         "vaCreateConfig");
	return TRUE;

fail:
	enc->config = VA_INVALID_ID;
	return FALSE;
}

/* The surfaces (superblock aligned), the context, the coded buffer and the upload image. */
static BOOL setup_stream(AV1_VAAPI_ENC* enc)
{
	enc->surfaceWidth = (enc->width + AV1_SB - 1) & ~(UINT32)(AV1_SB - 1);
	enc->surfaceHeight = (enc->height + AV1_SB - 1) & ~(UINT32)(AV1_SB - 1);

	VASurfaceAttrib fmt = { .type = VASurfaceAttribPixelFormat,
		                    .flags = VA_SURFACE_ATTRIB_SETTABLE,
		                    .value = { .type = VAGenericValueTypeInteger,
		                               .value = { .i = VA_FOURCC_NV12 } } };
	VA_CHECK(enc,
	         av1_va.vaCreateSurfaces(enc->dpy, VA_RT_FORMAT_YUV420, enc->surfaceWidth,
	                                 enc->surfaceHeight, enc->surfaces, ARRAYSIZE(enc->surfaces),
	                                 &fmt, 1),
	         "vaCreateSurfaces");
	enc->haveSurfaces = TRUE;
	VA_CHECK(enc,
	         av1_va.vaCreateContext(enc->dpy, enc->config, (int)enc->surfaceWidth,
	                                (int)enc->surfaceHeight, VA_PROGRESSIVE, enc->surfaces,
	                                ARRAYSIZE(enc->surfaces), &enc->context),
	         "vaCreateContext");

	/* a frame is smaller than its pictures: the raw size and room for the headers */
	const size_t codedSize = 3ull * enc->surfaceWidth * enc->surfaceHeight / 2 + 65536;
	if (codedSize > UINT32_MAX)
		goto fail;
	VA_CHECK(enc,
	         av1_va.vaCreateBuffer(enc->dpy, enc->context, VAEncCodedBufferType,
	                               (unsigned int)codedSize, 1, nullptr, &enc->coded),
	         "vaCreateBuffer(coded)");

	VAImageFormat nv12 = { .fourcc = VA_FOURCC_NV12,
		                   .byte_order = VA_LSB_FIRST,
		                   .bits_per_pixel = 12 };
	VA_CHECK(enc,
	         av1_va.vaCreateImage(enc->dpy, &nv12, (int)enc->surfaceWidth, (int)enc->surfaceHeight,
	                              &enc->image),
	         "vaCreateImage");
	enc->haveImage = TRUE;
	/* upload() writes whole rows of both planes */
	{
		const VAImage* img = &enc->image;
		const UINT64 yEnd = img->offsets[0] + 1ull * img->pitches[0] * enc->surfaceHeight;
		const UINT64 uvEnd = img->offsets[1] + 1ull * img->pitches[1] * (enc->surfaceHeight / 2);
		if ((img->num_planes < 2) || (img->pitches[0] < enc->surfaceWidth) ||
		    (img->pitches[1] < enc->surfaceWidth) || (yEnd > img->data_size) ||
		    (uvEnd > img->data_size))
		{
			WLog_Print(enc->log, WLOG_WARN, "VA-API: %s: an NV12 image of an unexpected layout",
			           enc->name);
			goto fail;
		}
	}
	return TRUE;

fail:
	return FALSE;
}

AV1_VAAPI_ENC* av1_vaapi_enc_new(wLog* log, UINT32 width, UINT32 height, AV1_VAAPI_ENC_RATE rate,
                                 UINT32 kbps, UINT32 qindex, INT32 device)
{
	if ((width == 0) || (height == 0) || (width > 65536) || (height > 65536) ||
	    (rate > AV1_VAAPI_ENC_CQP) || ((rate != AV1_VAAPI_ENC_CQP) && (kbps == 0)) ||
	    ((rate == AV1_VAAPI_ENC_CQP) && ((qindex == 0) || (qindex > 255))))
		return nullptr;
	if (!av1_va_load())
	{
		WLog_Print(log, WLOG_DEBUG, "VA-API: no libva.so.2 / libva-drm.so.2");
		return nullptr;
	}
	AV1_VAAPI_ENC* enc = calloc(1, sizeof(AV1_VAAPI_ENC));
	if (!enc)
		return nullptr;
	enc->log = log;
	enc->fd = -1;
	enc->config = VA_INVALID_ID;
	enc->context = VA_INVALID_ID;
	enc->coded = VA_INVALID_ID;
	enc->width = width;
	enc->height = height;
	enc->rate = rate;
	enc->kbps = kbps;
	/* a rate control's start: a middle quantizer */
	enc->qindex = (rate == AV1_VAAPI_ENC_CQP) ? qindex : 128;
	enc->last = -1;
	enc->frameMs = 1000.0 / AV1_FRAME_RATE;

	/* the devices are the render nodes with an AV1 encoder, in order (/codec-list) */
	INT32 id = 0;
	for (int i = 0; (i < AV1_VA_RENDER_COUNT) && !enc->dpy; i++)
	{
		if (open_device(enc, log, AV1_VA_RENDER_FIRST + i))
		{
			if ((device >= 0) && (id != device))
				close_device(enc);
			else
				enc->id = id;
			id++;
		}
	}
	if (!enc->dpy)
		goto fail;
	if (!configure(enc) || !setup_stream(enc))
		goto fail;
	write_sequence_header(enc);
	if (enc->seqHdr.overflow)
		goto fail;
	return enc;

fail:
	av1_vaapi_enc_free(enc);
	return nullptr;
}

/* I420 into the NV12 input surface, the edges repeated out to the surface size */
static BOOL upload(AV1_VAAPI_ENC* enc, const BYTE* const planes[3], const UINT32 strides[3])
{
	void* data = nullptr;
	VA_CHECK(enc, av1_va.vaMapBuffer(enc->dpy, enc->image.buf, &data), "vaMapBuffer(image)");
	{
		BYTE* base = data;
		const UINT32 w = enc->width;
		const UINT32 h = enc->height;
		const UINT32 sw = enc->surfaceWidth;
		const UINT32 sh = enc->surfaceHeight;
		BYTE* y = &base[enc->image.offsets[0]];
		const size_t yPitch = enc->image.pitches[0];
		for (UINT32 r = 0; r < sh; r++)
		{
			BYTE* dst = &y[r * yPitch];
			if (r < h)
			{
				const BYTE* src = &planes[0][1ull * r * strides[0]];
				memcpy(dst, src, w);
				memset(&dst[w], src[w - 1], sw - w);
			}
			else
				memcpy(dst, &y[(h - 1) * yPitch], sw);
		}

		const UINT32 cw = (w + 1) / 2;
		const UINT32 ch = (h + 1) / 2;
		BYTE* uv = &base[enc->image.offsets[1]];
		const size_t uvPitch = enc->image.pitches[1];
		for (UINT32 r = 0; r < sh / 2; r++)
		{
			BYTE* dst = &uv[r * uvPitch];
			if (r < ch)
			{
				const BYTE* u = &planes[1][1ull * r * strides[1]];
				const BYTE* v = &planes[2][1ull * r * strides[2]];
				for (UINT32 c = 0; c < sw / 2; c++)
				{
					const UINT32 s = (c < cw) ? c : cw - 1;
					dst[2 * c] = u[s];
					dst[2 * c + 1] = v[s];
				}
			}
			else
				memcpy(dst, &uv[(ch - 1) * uvPitch], sw);
		}
	}
	VA_CHECK(enc, av1_va.vaUnmapBuffer(enc->dpy, enc->image.buf), "vaUnmapBuffer(image)");
	VA_CHECK(enc,
	         av1_va.vaPutImage(enc->dpy, enc->surfaces[0], enc->image.image_id, 0, 0,
	                           enc->surfaceWidth, enc->surfaceHeight, 0, 0, enc->surfaceWidth,
	                           enc->surfaceHeight),
	         "vaPutImage");
	return TRUE;

fail:
	return FALSE;
}

typedef struct
{
	VABufferID ids[16];
	unsigned count;
} AV1_BUFFERS;

static BOOL add_buffer(AV1_VAAPI_ENC* enc, AV1_BUFFERS* bufs, VABufferType type, size_t size,
                       const void* data)
{
	if ((bufs->count >= ARRAYSIZE(bufs->ids)) || (size > UINT32_MAX))
		return FALSE;
	VA_CHECK(enc,
	         av1_va.vaCreateBuffer(enc->dpy, enc->context, type, (unsigned int)size, 1,
	                               WINPR_CAST_CONST_PTR_AWAY(data, void*), &bufs->ids[bufs->count]),
	         "vaCreateBuffer");
	bufs->count++;
	return TRUE;

fail:
	return FALSE;
}

static BOOL add_packed(AV1_VAAPI_ENC* enc, AV1_BUFFERS* bufs, UINT32 type, const AV1_BITS* b)
{
	const VAEncPackedHeaderParameterBuffer param = { .type = type,
		                                             .bit_length = (UINT32)b->bits,
		                                             .has_emulation_bytes = 0 };
	return add_buffer(enc, bufs, VAEncPackedHeaderParameterBufferType, sizeof(param), &param) &&
	       add_buffer(enc, bufs, VAEncPackedHeaderDataBufferType, b->bits / 8, b->buf);
}

static BOOL add_misc(AV1_VAAPI_ENC* enc, AV1_BUFFERS* bufs, VAEncMiscParameterType type,
                     const void* data, size_t size)
{
	BYTE buf[sizeof(VAEncMiscParameterBuffer) + 256] = WINPR_C_ARRAY_INIT;
	if (size > 256)
		return FALSE;
	VAEncMiscParameterBuffer* misc = (VAEncMiscParameterBuffer*)buf;
	misc->type = type;
	memcpy(misc->data, data, size);
	return add_buffer(enc, bufs, VAEncMiscParameterBufferType,
	                  sizeof(VAEncMiscParameterBuffer) + size, buf);
}

/* The rate control: the bit rate at the start; the frame rate when the frames' real one moves
 * off it by more than 5 % (frames come on damage: an encoder gives a frame its bits from the
 * frame rate). */
static BOOL add_rate(AV1_VAAPI_ENC* enc, AV1_BUFFERS* bufs, BOOL first, UINT32 durationMs)
{
	if (enc->rate == AV1_VAAPI_ENC_CQP)
		return TRUE;
	if (durationMs > 0)
		enc->frameMs += (durationMs - enc->frameMs) / 8.0; /* about the last 8 frames */
	double fps = 1000.0 / enc->frameMs;
	if (fps < 1.0)
		fps = 1.0;
	if (fps > 240.0)
		fps = 240.0;
	const UINT32 fps10 = (UINT32)(fps * 10.0 + 0.5);
	const BOOL rateMoved = (enc->sentFps10 == 0) || (fps10 * 100 < enc->sentFps10 * 95) ||
	                       (fps10 * 100 > enc->sentFps10 * 105);
	const UINT32 bps = enc->kbps * 1000u;

	if (first)
	{
		/* VBR: kbps is the target, a third more the peak */
		VAEncMiscParameterRateControl rc = WINPR_C_ARRAY_INIT;
		rc.bits_per_second = (enc->rate == AV1_VAAPI_ENC_VBR) ? bps / 3u * 4u : bps;
		rc.target_percentage = (enc->rate == AV1_VAAPI_ENC_VBR) ? 75 : 100;
		rc.window_size = 1000;
		VAEncMiscParameterHRD hrd = { .initial_buffer_fullness = rc.bits_per_second / 2,
			                          .buffer_size = rc.bits_per_second };
		if (!add_misc(enc, bufs, VAEncMiscParameterTypeRateControl, &rc, sizeof(rc)) ||
		    !add_misc(enc, bufs, VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd)))
			return FALSE;
	}
	if (first || rateMoved)
	{
		/* the numerator in the low 16 bits, the denominator in the high ones */
		VAEncMiscParameterFrameRate fr = { .framerate = fps10 | (10u << 16) };
		if (!add_misc(enc, bufs, VAEncMiscParameterTypeFrameRate, &fr, sizeof(fr)))
			return FALSE;
		if (enc->sentFps10 != 0)
			WLog_Print(enc->log, WLOG_DEBUG, "VA-API: %s at %.1f fps (was %.1f)", enc->name,
			           fps10 / 10.0, enc->sentFps10 / 10.0);
		enc->sentFps10 = fps10;
	}
	return TRUE;
}

static void fill_picture(const AV1_VAAPI_ENC* enc, BOOL key, int recon,
                         VAEncPictureParameterBufferAV1* pic)
{
	memset(pic, 0, sizeof(*pic));
	pic->frame_width_minus_1 = (uint16_t)(enc->width - 1);
	pic->frame_height_minus_1 = (uint16_t)(enc->height - 1);
	pic->reconstructed_frame = enc->surfaces[1 + recon];
	pic->coded_buf = enc->coded;
	for (size_t i = 0; i < ARRAYSIZE(pic->reference_frames); i++)
		pic->reference_frames[i] = VA_INVALID_SURFACE;
	/* low delay: a key frame fills every slot, then each frame refers to slot 0 (the frame
	 * before it) and replaces it */
	if (key)
	{
		pic->primary_ref_frame = AV1_PRIMARY_REF_NONE;
		pic->refresh_frame_flags = 0xff;
		pic->picture_flags.bits.frame_type = AV1_FRAME_KEY;
		pic->picture_flags.bits.error_resilient_mode = 1;
	}
	else
	{
		pic->reference_frames[0] = enc->surfaces[1 + enc->last];
		for (size_t i = 0; i < AV1_REFS_PER_FRAME; i++)
			pic->ref_frame_idx[i] = 0;
		pic->primary_ref_frame = 0;
		pic->refresh_frame_flags = 0x01;
		pic->ref_frame_ctrl_l0.fields.search_idx0 = AV1_REF_FRAME_LAST;
		pic->picture_flags.bits.frame_type = AV1_FRAME_INTER;
	}
	pic->order_hint = (uint8_t)(enc->frames & 0xff);

	pic->base_qindex = (uint8_t)enc->qindex;
	if (enc->rate == AV1_VAAPI_ENC_CQP)
	{
		/* the driver takes the header's filters as they are: libaom's level for the
		 * quantizer, no CDEF (it softens text) */
		const UINT32 level = cqp_filter_level(enc->qindex, key);
		pic->filter_level[0] = pic->filter_level[1] = (uint8_t)level;
		pic->filter_level_u = pic->filter_level_v = (uint8_t)level;
	}
	else
	{
		pic->min_base_qindex = 1;
		pic->max_base_qindex = 255;
	}
	pic->mode_control_flags.bits.tx_mode = enc->txSelect ? AV1_TX_MODE_SELECT : AV1_TX_MODE_LARGEST;
	pic->tile_cols = (uint8_t)enc->tileCols;
	pic->tile_rows = (uint8_t)enc->tileRows;
	for (UINT32 i = 0; i < enc->tileCols; i++)
	{
		const UINT32 start = i * enc->tileWidthSb;
		pic->width_in_sbs_minus_1[i] = (uint16_t)(MIN(enc->tileWidthSb, enc->sbCols - start) - 1);
	}
	for (UINT32 i = 0; i < enc->tileRows; i++)
	{
		const UINT32 start = i * enc->tileHeightSb;
		pic->height_in_sbs_minus_1[i] = (uint16_t)(MIN(enc->tileHeightSb, enc->sbRows - start) - 1);
	}
	pic->tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
}

static BOOL out_reserve(AV1_VAAPI_ENC* enc, size_t size)
{
	if (size <= enc->outCapacity)
		return TRUE;
	BYTE* out = realloc(enc->out, size);
	if (!out)
		return FALSE;
	enc->out = out;
	enc->outCapacity = size;
	return TRUE;
}

static BOOL out_append(AV1_VAAPI_ENC* enc, const void* data, size_t size)
{
	if (!out_reserve(enc, enc->outSize + size))
		return FALSE;
	memcpy(&enc->out[enc->outSize], data, size);
	enc->outSize += size;
	return TRUE;
}

static BOOL raw_append(AV1_VAAPI_ENC* enc, const void* data, size_t size)
{
	if (enc->rawSize + size > enc->rawCapacity)
	{
		BYTE* raw = realloc(enc->raw, enc->rawSize + size);
		if (!raw)
			return FALSE;
		enc->raw = raw;
		enc->rawCapacity = enc->rawSize + size;
	}
	memcpy(&enc->raw[enc->rawSize], data, size);
	enc->rawSize += size;
	return TRUE;
}

/* The OBU types in @p data (bit n set: type n seen), FALSE when it is no OBU sequence. */
static BOOL obu_types(const BYTE* data, size_t size, UINT32* types, BYTE* first)
{
	*types = 0;
	*first = 0;
	size_t pos = 0;
	while (pos < size)
	{
		const BYTE h = data[pos];
		const BYTE type = (h >> 3) & 0x0f;
		if (pos == 0)
			*first = type;
		*types |= 1u << type;
		pos += (h & 0x04) ? 2 : 1;
		if (!(h & 0x02))
			return pos <= size; /* no size: the rest is its payload */
		UINT64 len = 0;
		for (unsigned i = 0;; i++)
		{
			if ((pos >= size) || (i >= 8))
				return FALSE;
			const BYTE b = data[pos++];
			len |= (UINT64)(b & 0x7f) << (7 * i);
			if (!(b & 0x80))
				break;
		}
		if (len > size - pos)
			return FALSE;
		pos += (size_t)len;
	}
	return TRUE;
}

/* The coded frame as a temporal unit: a temporal delimiter first, the sequence header in the
 * key frame and the frame header before the tile groups, put in when the driver didn't. */
static BOOL collect(AV1_VAAPI_ENC* enc, BOOL key, const AV1_FRAME_HDR* fh)
{
	enc->outSize = 0;
	enc->rawSize = 0;
	void* data = nullptr;
	VA_CHECK(enc, av1_va.vaMapBuffer(enc->dpy, enc->coded, &data), "vaMapBuffer(coded)");
	{
		BOOL ok = TRUE;
		for (const VACodedBufferSegment* seg = data; seg && ok; seg = seg->next)
		{
			if (seg->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK)
			{
				WLog_Print(enc->log, WLOG_WARN, "VA-API: %s: the frame overflowed its buffer",
				           enc->name);
				ok = FALSE;
			}
			else if (seg->size > 0)
			{
				if (!raw_append(enc, seg->buf, seg->size))
					ok = FALSE;
			}
		}
		(void)av1_va.vaUnmapBuffer(enc->dpy, enc->coded);
		if (!ok)
			return FALSE;
	}

	UINT32 types = 0;
	BYTE first = 0;
	if ((enc->rawSize == 0) || !obu_types(enc->raw, enc->rawSize, &types, &first))
	{
		WLog_Print(enc->log, WLOG_WARN, "VA-API: %s: %" PRIuz " bytes of no OBUs", enc->name,
		           enc->rawSize);
		return FALSE;
	}
	const BOOL haveFrameHdr = (types & ((1u << AV1_OBU_FRAME_HEADER) | (1u << AV1_OBU_FRAME))) != 0;
	const BOOL needSeqHdr = key && !(types & (1u << AV1_OBU_SEQUENCE_HEADER));
	/* with a rate control of its own the frame header must be the driver's: this one has a
	 * quantizer and filters the frame wasn't coded with */
	if (!haveFrameHdr && (enc->rate != AV1_VAAPI_ENC_CQP))
	{
		WLog_Print(enc->log, WLOG_WARN,
		           "VA-API: %s writes no frame header with its own rate control: unusable",
		           enc->name);
		return FALSE;
	}
	if ((needSeqHdr || !haveFrameHdr) && !enc->warnedHeaders)
	{
		WLog_Print(enc->log, WLOG_DEBUG,
		           "VA-API: %s writes no%s%s: this encoder puts them in front", enc->name,
		           needSeqHdr ? " sequence header" : "", !haveFrameHdr ? " frame header" : "");
		enc->warnedHeaders = TRUE;
	}
	static const BYTE td[2] = { AV1_OBU_TEMPORAL_DELIMITER << 3 | 0x02, 0 };
	if ((first != AV1_OBU_TEMPORAL_DELIMITER) && !out_append(enc, td, sizeof(td)))
		return FALSE;
	if (needSeqHdr && !out_append(enc, enc->seqHdr.buf, enc->seqHdr.bits / 8))
		return FALSE;
	if (!haveFrameHdr && !out_append(enc, fh->bits.buf, fh->bits.bits / 8))
		return FALSE;
	return out_append(enc, enc->raw, enc->rawSize);

fail:
	return FALSE;
}

static void destroy_buffers(AV1_VAAPI_ENC* enc, AV1_BUFFERS* bufs)
{
	for (unsigned i = 0; i < bufs->count; i++)
		(void)av1_va.vaDestroyBuffer(enc->dpy, bufs->ids[i]);
	bufs->count = 0;
}

INT32 av1_vaapi_enc_encode(AV1_VAAPI_ENC* enc, const BYTE* const planes[3], const UINT32 strides[3],
                           UINT32 durationMs, const BYTE** ppData, UINT32* pSize)
{
	WINPR_ASSERT(enc);
	WINPR_ASSERT(planes);
	WINPR_ASSERT(strides);
	WINPR_ASSERT(ppData);
	WINPR_ASSERT(pSize);
	*ppData = nullptr;
	*pSize = 0;

	const BOOL key = (enc->last < 0);
	const int recon = key ? 0 : (enc->last ^ 1);
	if (!upload(enc, planes, strides))
		return -1;

	VAEncPictureParameterBufferAV1 pic = WINPR_C_ARRAY_INIT;
	fill_picture(enc, key, recon, &pic);
	AV1_FRAME_HDR fh = WINPR_C_ARRAY_INIT;
	if (!write_frame_header(enc, &pic, &fh))
		return -2;
	if (enc->rate != AV1_VAAPI_ENC_CQP)
	{
		/* where the driver's rate control writes its choices into the header */
		pic.bit_offset_qindex = fh.qindexOffset;
		pic.bit_offset_segmentation = fh.segmentationOffset;
		pic.bit_offset_loopfilter_params = fh.loopFilterOffset;
		pic.bit_offset_cdef_params = fh.cdefOffset;
		pic.size_in_bits_cdef_params = fh.cdefBits;
		pic.size_in_bits_frame_hdr_obu = (UINT32)fh.bits.bits;
		pic.byte_offset_frame_hdr_obu_size =
		    1u + ((key && (enc->packed & VA_ENC_PACKED_HEADER_SEQUENCE))
		              ? (UINT32)(enc->seqHdr.bits / 8)
		              : 0u);
	}

	AV1_BUFFERS bufs = WINPR_C_ARRAY_INIT;
	INT32 rc = -3;
	if (key)
	{
		VAEncSequenceParameterBufferAV1 seq = WINPR_C_ARRAY_INIT;
		seq.seq_profile = 0;
		seq.seq_level_idx = enc->level;
		seq.seq_tier = 0;
		seq.intra_period = AV1_GOP;
		seq.ip_period = 1;
		if (enc->rate != AV1_VAAPI_ENC_CQP)
			seq.bits_per_second = enc->kbps * 1000u;
		seq.seq_fields.bits.enable_order_hint = 1;
		seq.seq_fields.bits.enable_cdef = 1;
		seq.seq_fields.bits.enable_intra_edge_filter = enc->intraEdgeFilter ? 1 : 0;
		seq.seq_fields.bits.subsampling_x = 1;
		seq.seq_fields.bits.subsampling_y = 1;
		seq.order_hint_bits_minus_1 = AV1_ORDER_HINT_BITS - 1;
		if (!add_buffer(enc, &bufs, VAEncSequenceParameterBufferType, sizeof(seq), &seq))
			goto out;
		if ((enc->packed & VA_ENC_PACKED_HEADER_SEQUENCE) &&
		    !add_packed(enc, &bufs, VAEncPackedHeaderSequence, &enc->seqHdr))
			goto out;
	}
	if (!add_rate(enc, &bufs, key, durationMs))
		goto out;
	if (!add_buffer(enc, &bufs, VAEncPictureParameterBufferType, sizeof(pic), &pic))
		goto out;
	if ((enc->packed & VA_ENC_PACKED_HEADER_PICTURE) &&
	    !add_packed(enc, &bufs, VAEncPackedHeaderPicture, &fh.bits))
		goto out;
	{
		VAEncTileGroupBufferAV1 tg = WINPR_C_ARRAY_INIT;
		tg.tg_start = 0;
		tg.tg_end = (uint8_t)(enc->tileCols * enc->tileRows - 1);
		if (!add_buffer(enc, &bufs, VAEncSliceParameterBufferType, sizeof(tg), &tg))
			goto out;
	}

	rc = -4;
	VA_CHECK(enc, av1_va.vaBeginPicture(enc->dpy, enc->context, enc->surfaces[0]),
	         "vaBeginPicture");
	{
		const VAStatus st =
		    av1_va.vaRenderPicture(enc->dpy, enc->context, bufs.ids, (int)bufs.count);
		const VAStatus end = av1_va.vaEndPicture(enc->dpy, enc->context);
		if ((st != VA_STATUS_SUCCESS) || (end != VA_STATUS_SUCCESS))
		{
			WLog_Print(enc->log, WLOG_WARN, "VA-API: vaRenderPicture / vaEndPicture on %s: %s",
			           enc->name, av1_va.vaErrorStr((st != VA_STATUS_SUCCESS) ? st : end));
			goto out;
		}
	}
	rc = -5;
	VA_CHECK(enc, av1_va.vaSyncSurface(enc->dpy, enc->surfaces[0]), "vaSyncSurface");
	rc = -6;
	if (!collect(enc, key, &fh) || (enc->outSize > UINT32_MAX))
		goto out;

	enc->last = recon;
	enc->frames++;
	*ppData = enc->out;
	*pSize = (UINT32)enc->outSize;
	rc = 1;
	goto out;

fail:
out:
	destroy_buffers(enc, &bufs);
	return rc;
}
