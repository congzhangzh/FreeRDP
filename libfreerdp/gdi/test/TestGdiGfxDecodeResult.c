/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Every decoded HEVC surface command reaches the channel's decoder statistics
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

#include <winpr/crt.h>
#include <winpr/synch.h>

#include <freerdp/freerdp.h>
#include <freerdp/codecs.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>

#if defined(WITH_GFX_AZURE)
#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/hevc.h>

#define TEST_SIZE 256 /* hardware HEVC encoders may refuse very small pictures */
#define TEST_SURFACE 1

/* the channel keeps the surfaces; here a single one */
static void* g_surface = nullptr;

static UINT test_set_surface(WINPR_ATTR_UNUSED RdpgfxClientContext* context, UINT16 surfaceId,
                             void* data)
{
	if (surfaceId != TEST_SURFACE)
		return ERROR_INVALID_PARAMETER;
	g_surface = data;
	return CHANNEL_RC_OK;
}

static void* test_get_surface(WINPR_ATTR_UNUSED RdpgfxClientContext* context, UINT16 surfaceId)
{
	return (surfaceId == TEST_SURFACE) ? g_surface : nullptr;
}

/* what DecodeResult was told: the FreeRDP Web page shows these as the decoder's pictures */
static size_t g_results = 0;
static UINT16 g_codecId = 0;
static FREERDP_DECODE_STATUS g_status = FREERDP_DECODE_NONE;

static void test_decode_result(WINPR_ATTR_UNUSED RdpgfxClientContext* context, UINT16 codecId,
                               const FREERDP_DECODE_RESULT* result,
                               WINPR_ATTR_UNUSED UINT64 decodeNS)
{
	g_results++;
	g_codecId = codecId;
	g_status = result->status;
}

/* A flat grey picture as HEVC 4:2:0, in a buffer of the encoder *penc (free it after use).
 * Returns 1 on success, 0 when this build has no encoder. */
static int encode_picture(FREERDP_HEVC_CONTEXT** penc, BYTE** coded, UINT32* codedSize,
                          RDPGFX_H264_METABLOCK* meta)
{
	const UINT32 step = TEST_SIZE * 4;
	const RECTANGLE_16 rect = { 0, 0, TEST_SIZE, TEST_SIZE };
	int rc = -1;
	BYTE* src = calloc(TEST_SIZE, step);
	FREERDP_HEVC_CONTEXT* enc = *penc = freerdp_hevc_context_new(TRUE);
	if (!src)
		goto out;
	if (!enc || !freerdp_hevc_context_set_option(enc, FREERDP_HEVC_CONTEXT_OPTION_PROFILE, 0))
	{
		rc = 0;
		goto out;
	}
	memset(src, 0x80, 1ull * TEST_SIZE * step);
	if (!freerdp_hevc_context_reset(enc, TEST_SIZE, TEST_SIZE) ||
	    (freerdp_hevc_compress(enc, src, PIXEL_FORMAT_BGRX32, step, TEST_SIZE, TEST_SIZE, &rect,
	                           coded, codedSize, meta) <= 0))
		goto out;
	rc = 1;
out:
	free(src);
	return rc;
}

/* An HEVC command is decoded and the decoder's verdict reaches DecodeResult, as it does for
 * AVC420: without it the page counts the commands but never a picture or a failure. */
static int test_hevc(rdpContext* context)
{
	int rc = -1;
	FREERDP_HEVC_CONTEXT* enc = nullptr;
	BYTE* coded = nullptr; /* the encoder's */
	UINT32 codedSize = 0;
	RDPGFX_H264_METABLOCK meta = WINPR_C_ARRAY_INIT;
	rdpGdi gdi = WINPR_C_ARRAY_INIT;
	gdi.context = context;
	gdi.log = WLog_Get("com.freerdp.test.gdi");
	RdpgfxClientContext* gfx = calloc(1, sizeof(RdpgfxClientContext));
	if (!gfx)
		return -1;

	const int encoded = encode_picture(&enc, &coded, &codedSize, &meta);
	if (encoded <= 0)
	{
		(void)fprintf(stderr, "%s\n",
		              encoded ? "the HEVC encode failed"
		                      : "no HEVC encoder in this build, skipped");
		rc = encoded;
		goto out;
	}

	if (!freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopWidth, TEST_SIZE) ||
	    !freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopHeight, TEST_SIZE))
		goto out;
	gfx->SetSurfaceData = test_set_surface;
	gfx->GetSurfaceData = test_get_surface;
	if (!gdi_graphics_pipeline_init(&gdi, gfx))
		goto out;
	gfx->UpdateSurfaces = nullptr; /* no output here */
	gfx->DecodeResult = test_decode_result;

	const RDPGFX_CREATE_SURFACE_PDU create = { .surfaceId = TEST_SURFACE,
		                                       .width = TEST_SIZE,
		                                       .height = TEST_SIZE,
		                                       .pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888 };
	if (gfx->CreateSurface(gfx, &create) != CHANNEL_RC_OK)
		goto uninit;

	RDPGFX_HEVC_BITMAP_STREAM bs = { .meta = meta, .length = codedSize, .data = coded };
	const RDPGFX_SURFACE_COMMAND cmd = { .surfaceId = TEST_SURFACE,
		                                 .codecId = RDPGFX_CODECID_HEVC420,
		                                 .format = PIXEL_FORMAT_BGRX32,
		                                 .left = 0,
		                                 .top = 0,
		                                 .right = TEST_SIZE,
		                                 .bottom = TEST_SIZE,
		                                 .width = TEST_SIZE,
		                                 .height = TEST_SIZE,
		                                 .length = codedSize,
		                                 .data = coded,
		                                 .extra = &bs };
	const UINT status = gfx->SurfaceCommand(gfx, &cmd);
	if (status != CHANNEL_RC_OK)
	{
		/* no decoder: the surface keeps no HEVC context and nothing was decoded */
		const gdiGfxSurface* surface = g_surface;
		if (surface && !surface->hevc)
		{
			(void)fprintf(stderr, "no HEVC decoder in this build, skipped\n");
			rc = 0;
		}
		else
			(void)fprintf(stderr, "the HEVC command failed: 0x%08" PRIx32 "\n", status);
		goto delete;
	}

	(void)fprintf(stderr,
	              "HEVC 4:2:0 command: %" PRIuz " decode result(s), codec 0x%04" PRIx16
	              ", status %d (picture is %d)\n",
	              g_results, g_codecId, (int)g_status, (int)FREERDP_DECODE_PICTURE);
	if ((g_results == 1) && (g_codecId == RDPGFX_CODECID_HEVC420) &&
	    (g_status == FREERDP_DECODE_PICTURE))
		rc = 1;
	else
		(void)fprintf(stderr, "WRONG: want 1 result for codec 0x%04x with a picture\n",
		              RDPGFX_CODECID_HEVC420);

delete:
{
	const RDPGFX_DELETE_SURFACE_PDU del = { .surfaceId = TEST_SURFACE };
	if (gfx->DeleteSurface(gfx, &del) != CHANNEL_RC_OK)
		rc = -1;
}
uninit:
	gdi_graphics_pipeline_uninit(&gdi, gfx);
out:
	free_h264_metablock(&meta);
	freerdp_hevc_context_free(enc);
	free(gfx);
	return rc;
}
#endif

int TestGdiGfxDecodeResult(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
#if defined(WITH_GFX_AZURE)
	int rc = -1;
	freerdp* instance = freerdp_new();
	if (!instance)
		return -1;
	instance->ContextSize = sizeof(rdpContext);
	if (freerdp_context_new(instance) && (test_hevc(instance->context) >= 0))
		rc = 0;
	freerdp_context_free(instance);
	freerdp_free(instance);
	return rc;
#else
	(void)fprintf(stderr, "built without WITH_GFX_AZURE: no HEVC, skipped\n");
	return 0;
#endif
}
