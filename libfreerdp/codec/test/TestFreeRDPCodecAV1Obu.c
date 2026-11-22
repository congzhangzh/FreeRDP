/* The AV1 header parser for hardware decoders (av1_obu.c) against dav1d: every frame header
 * dav1d shows must be the one the parser gave for that frame, with the tiles covering the
 * frame. The stream comes from FreeRDP's own AV1 encoder (the server's screen settings). */

#include <stdlib.h>
#include <string.h>

#include <winpr/crypto.h>

#include <freerdp/freerdp.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/av1.h>
#include <freerdp/codecs.h>

#include <dav1d/dav1d.h>

#include "../av1_obu.h"

/* a screen: text-like glyph rows that scroll, a panel that moves */
static BOOL drawScreen(BYTE* src, UINT32 format, UINT32 stride, UINT32 width, UINT32 height,
                       UINT32 frame)
{
	const UINT32 bpp = FreeRDPGetBytesPerPixel(format);
	const UINT32 white = FreeRDPGetColor(format, 0xF8, 0xF8, 0xF8, 0xFF);
	const UINT32 ink = FreeRDPGetColor(format, 0x20, 0x20, 0x28, 0xFF);
	const UINT32 panel = FreeRDPGetColor(format, 0x30, 0x58, 0xA8, 0xFF);
	const UINT32 panelLeft = (frame * 24) % (width / 2);

	for (UINT32 y = 0; y < height; y++)
	{
		const UINT32 row = (y + frame * 3) / 16;
		const UINT32 inRow = (y + frame * 3) % 16;
		for (UINT32 x = 0; x < width; x++)
		{
			UINT32 c = white;
			if ((x >= panelLeft) && (x < panelLeft + width / 4) && (y > height / 2))
				c = panel;
			else if ((inRow >= 3) && (inRow < 12) && (x > 8))
			{
				/* a glyph cell of 8 px: a pseudo random pattern per row and column */
				const UINT32 cell = (x / 8) * 31 + row * 17;
				const UINT32 bits = (cell * 2654435761u) >> 16;
				if ((bits >> ((x % 8) + (inRow % 3))) & 1)
					c = ink;
			}
			if (!FreeRDPWriteColor(&src[1ull * y * stride + 1ull * x * bpp], format, c))
				return FALSE;
		}
	}
	return TRUE;
}

static BOOL sameHeader(const Dav1dFrameHeader* a, const Dav1dFrameHeader* b)
{
	Dav1dFrameHeader x = *a;
	Dav1dFrameHeader y = *b;
	/* dav1d derives the shear parameters while it decodes */
	for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
	{
		memset(&x.gmv[i].u, 0, sizeof(x.gmv[i].u));
		memset(&y.gmv[i].u, 0, sizeof(y.gmv[i].u));
	}
	return memcmp(&x, &y, sizeof(x)) == 0;
}

static BOOL tilesCover(const AV1_OBU_EVENT* ev, UINT32 size)
{
	const Dav1dFrameHeader* h = &ev->hdr->d;
	if (ev->nTiles != (UINT32)h->tiling.cols * h->tiling.rows)
		return FALSE;
	for (UINT32 i = 0; i < ev->nTiles; i++)
	{
		const AV1_OBU_TILE* t = &ev->tiles[i];
		if ((t->size == 0) || (t->offset > size) || (t->size > size - t->offset) ||
		    (t->row != i / h->tiling.cols) || (t->column != i % h->tiling.cols))
			return FALSE;
		if ((i > 0) && (ev->tiles[i - 1].offset + ev->tiles[i - 1].size > t->offset))
			return FALSE;
	}
	return TRUE;
}

static BOOL testStream(UINT32 width, UINT32 height, UINT32 profile)
{
	const UINT32 format = PIXEL_FORMAT_BGRX32;
	const UINT32 stride = width * FreeRDPGetBytesPerPixel(format);
	const UINT32 frames = 12;
	BOOL rc = FALSE;
	UINT32 compared = 0;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	BYTE* src = calloc(stride, height);
	FREERDP_AV1_CONTEXT* enc = freerdp_av1_context_new(TRUE);
	AV1_OBU_PARSER* parser = av1_obu_parser_new();
	Dav1dContext* dav1d = nullptr;
	Dav1dSettings s = WINPR_C_ARRAY_INIT;
	dav1d_default_settings(&s);
	s.n_threads = 1;
	s.max_frame_delay = 1;

	if (!src || !enc || !parser || (dav1d_open(&dav1d, &s) < 0))
		goto fail;
	if (!freerdp_av1_context_reset(enc, width, height) ||
	    !freerdp_av1_context_set_option(enc, FREERDP_AV1_CONTEXT_OPTION_PROFILE, profile))
		goto fail;

	const RECTANGLE_16 rect = { .left = 0, .top = 0, .right = width, .bottom = height };
	for (UINT32 frame = 0; frame < frames; frame++)
	{
		if (!drawScreen(src, format, stride, width, height, frame))
			goto fail;
		BYTE* dst = nullptr;
		UINT32 dstsize = 0;
		const INT32 erc = freerdp_av1_compress(enc, src, format, stride, width, height, &rect, &dst,
		                                       &dstsize, &meta);
		free_h264_metablock(&meta);
		if (erc < 0)
			goto fail;

		/* the header of the frame this temporal unit shows */
		Dav1dFrameHeader want = WINPR_C_ARRAY_INIT;
		BOOL haveWant = FALSE;
		AV1_OBU_EVENT ev = WINPR_C_ARRAY_INIT;
		int prc = 0;
		av1_obu_parser_begin(parser, dst, dstsize);
		while ((prc = av1_obu_parser_next(parser, &ev)) > 0)
		{
			if (ev.type == AV1_OBU_EVENT_FRAME)
			{
				if (!tilesCover(&ev, dstsize))
				{
					(void)fprintf(stderr, "[%s] frame %" PRIu32 ": bad tiles\n", __func__, frame);
					goto fail;
				}
				if (ev.hdr->d.show_frame)
				{
					want = ev.hdr->d;
					haveWant = TRUE;
				}
			}
			else if (ev.type == AV1_OBU_EVENT_SHOW_EXISTING)
			{
				want = av1_obu_parser_reference(parser, ev.frameIdx)->d;
				haveWant = TRUE;
			}
		}
		if (prc < 0)
		{
			(void)fprintf(stderr, "[%s] frame %" PRIu32 ": the parser refused it\n", __func__,
			              frame);
			goto fail;
		}

		Dav1dData data = WINPR_C_ARRAY_INIT;
		BYTE* d = dav1d_data_create(&data, dstsize);
		if (!d)
			goto fail;
		memcpy(d, dst, dstsize);
		int drc = 0;
		do
		{
			drc = dav1d_send_data(dav1d, &data);
			Dav1dPicture pic = WINPR_C_ARRAY_INIT;
			while (dav1d_get_picture(dav1d, &pic) == 0)
			{
				const BOOL same = haveWant && sameHeader(&want, pic.frame_hdr);
				dav1d_picture_unref(&pic);
				if (!same)
				{
					(void)fprintf(stderr,
					              "[%s] profile %" PRIu32 " frame %" PRIu32
					              ": the parser's header is not dav1d's\n",
					              __func__, profile, frame);
					goto fail;
				}
				haveWant = FALSE;
				compared++;
			}
		} while (drc == DAV1D_ERR(EAGAIN));
		if (drc < 0)
			goto fail;
	}
	(void)fprintf(stderr,
	              "[%s] %" PRIu32 "x%" PRIu32 " profile %" PRIu32 ": %" PRIu32 " of %" PRIu32
	              " frame headers as dav1d's\n",
	              __func__, width, height, profile, compared, frames);
	rc = (compared == frames);

fail:
	if (dav1d)
		dav1d_close(&dav1d);
	av1_obu_parser_free(parser);
	free_h264_metablock(&meta);
	freerdp_av1_context_free(enc);
	free(src);
	return rc;
}

/* garbage and a frame without a sequence header are refused, the parser stays usable */
static BOOL testInvalid(void)
{
	BOOL rc = FALSE;
	AV1_OBU_PARSER* parser = av1_obu_parser_new();
	AV1_OBU_EVENT ev = WINPR_C_ARRAY_INIT;
	BYTE junk[64] = WINPR_C_ARRAY_INIT;
	if (!parser)
		return FALSE;

	/* a frame OBU (type 6) with a size field, no sequence header before it */
	const BYTE frameObu[] = { 0x32, 0x04, 0x10, 0x00, 0x00, 0x00 };
	av1_obu_parser_begin(parser, frameObu, sizeof(frameObu));
	if (av1_obu_parser_next(parser, &ev) >= 0)
		goto fail;

	for (size_t i = 0; i < 64; i++)
	{
		if (winpr_RAND(junk, sizeof(junk)) < 0)
			goto fail;
		av1_obu_parser_begin(parser, junk, sizeof(junk));
		while (av1_obu_parser_next(parser, &ev) > 0)
			;
	}

	/* a temporal delimiter alone: no frame */
	const BYTE td[] = { 0x12, 0x00 };
	av1_obu_parser_reset(parser);
	av1_obu_parser_begin(parser, td, sizeof(td));
	rc = (av1_obu_parser_next(parser, &ev) == 0);

fail:
	av1_obu_parser_free(parser);
	return rc;
}

int TestFreeRDPCodecAV1Obu(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!testInvalid())
		return -1;
#if !defined(WITH_LIBAOM)
	(void)fprintf(stderr, "[%s] skipping the stream check, no AV1 encoder compiled in\n", __func__);
	return 0;
#else
	if (!testStream(640, 360, 0) || !testStream(640, 360, 1) || !testStream(1920, 1080, 0))
		return -1;
	return 0;
#endif
}
