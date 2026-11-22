/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 decoding with Direct3D 11 video acceleration (D3D11VA, DXVA AV1)
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

/* before any header that pulls in the COM interfaces */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/wlog.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxva.h>

#include "av1_obu.h"
#include "av1_d3d11va.h"

/* 8 reference slots, the frame being decoded and one spare */
#define AV1_SURFACES 10
/* surfaces are 128 aligned, like the superblocks (other decoders do the same) */
#define AV1_SURFACE_ALIGN 128
#define AV1_BITSTREAM_ALIGN 128

/* DXVA_ModeAV1_VLD_Profile0 (dxva.h), here so no INITGUID is needed */
static const GUID av1_profile0 = {
	0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a }
};

struct S_AV1_D3D11VA
{
	wLog* log;
	char adapterName[256];

	ID3D11Device* device;
	ID3D11DeviceContext* ctx;
	ID3D11VideoDevice* vdev;
	ID3D11VideoContext* vctx;

	AV1_OBU_PARSER* parser;

	/* the decoder for the current sequence */
	ID3D11VideoDecoder* decoder;
	ID3D11Texture2D* surfaces;
	ID3D11VideoDecoderOutputView* views[AV1_SURFACES];
	UINT32 maxWidth;
	UINT32 maxHeight;
	int slotSurface[DAV1D_NUM_REF_FRAMES]; /* -1: empty */

	/* copy back */
	ID3D11Texture2D* staging;
	UINT32 stagingWidth;
	UINT32 stagingHeight;
	BOOL mapped;
	BYTE* u;
	BYTE* v;
	size_t chromaSize;
};

static void release(IUnknown** p)
{
	if (*p)
		IUnknown_Release(*p);
	*p = nullptr;
}
#define RELEASE(x) release((IUnknown**)&(x))

static void unmap_staging(AV1_D3D11VA* dec)
{
	if (dec->mapped)
		ID3D11DeviceContext_Unmap(dec->ctx, (ID3D11Resource*)dec->staging, 0);
	dec->mapped = FALSE;
}

static void free_sequence(AV1_D3D11VA* dec)
{
	unmap_staging(dec);
	for (size_t i = 0; i < AV1_SURFACES; i++)
		RELEASE(dec->views[i]);
	RELEASE(dec->surfaces);
	RELEASE(dec->decoder);
	dec->maxWidth = dec->maxHeight = 0;
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		dec->slotSurface[i] = -1;
}

static BOOL adapter_decodes_av1(ID3D11VideoDevice* vdev)
{
	BOOL have = FALSE;
	const UINT n = ID3D11VideoDevice_GetVideoDecoderProfileCount(vdev);
	for (UINT i = 0; (i < n) && !have; i++)
	{
		GUID g = WINPR_C_ARRAY_INIT;
		if (SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderProfile(vdev, i, &g)) &&
		    IsEqualGUID(&g, &av1_profile0))
			have = TRUE;
	}
	if (!have)
		return FALSE;
	BOOL nv12 = FALSE;
	if (FAILED(ID3D11VideoDevice_CheckVideoDecoderFormat(vdev, &av1_profile0, DXGI_FORMAT_NV12,
	                                                     &nv12)))
		return FALSE;
	return nv12;
}

/* the device on adapter @a when it decodes AV1 */
static BOOL open_adapter(AV1_D3D11VA* dec, IDXGIAdapter1* a)
{
	DXGI_ADAPTER_DESC1 desc = WINPR_C_ARRAY_INIT;
	if (FAILED(IDXGIAdapter1_GetDesc1(a, &desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
		return FALSE;

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	HRESULT hr = D3D11CreateDevice((IDXGIAdapter*)a, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
	                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
	                               &device, nullptr, &ctx);
	if (FAILED(hr))
		return FALSE;

	ID3D11VideoDevice* vdev = nullptr;
	ID3D11VideoContext* vctx = nullptr;
	if (FAILED(ID3D11Device_QueryInterface(device, &IID_ID3D11VideoDevice, (void**)&vdev)) ||
	    !adapter_decodes_av1(vdev) ||
	    FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11VideoContext, (void**)&vctx)))
	{
		RELEASE(vdev);
		RELEASE(ctx);
		RELEASE(device);
		return FALSE;
	}

	dec->device = device;
	dec->ctx = ctx;
	dec->vdev = vdev;
	dec->vctx = vctx;
	if (WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, dec->adapterName,
	                        sizeof(dec->adapterName), nullptr, nullptr) <= 0)
		(void)_snprintf(dec->adapterName, sizeof(dec->adapterName), "adapter %04x:%04x",
		                desc.VendorId, desc.DeviceId);
	return TRUE;
}

void av1_d3d11va_free(AV1_D3D11VA* dec)
{
	if (!dec)
		return;
	free_sequence(dec);
	RELEASE(dec->staging);
	RELEASE(dec->vctx);
	RELEASE(dec->vdev);
	RELEASE(dec->ctx);
	RELEASE(dec->device);
	av1_obu_parser_free(dec->parser);
	free(dec->u);
	free(dec->v);
	free(dec);
}

AV1_D3D11VA* av1_d3d11va_new(wLog* log, INT32 adapter)
{
	AV1_D3D11VA* dec = calloc(1, sizeof(AV1_D3D11VA));
	if (!dec)
		return nullptr;
	dec->log = log;
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		dec->slotSurface[i] = -1;
	dec->parser = av1_obu_parser_new();
	if (!dec->parser)
		goto fail;

	if (adapter == AV1_D3D11VA_ANY_ADAPTER)
	{
		char env[16] = WINPR_C_ARRAY_INIT;
		const DWORD n = GetEnvironmentVariableA("FREERDP_AV1_D3D11VA_ADAPTER", env, sizeof(env));
		if ((n > 0) && (n < sizeof(env)))
			adapter = atoi(env);
	}

	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory)))
		goto fail;
	for (UINT i = 0; !dec->device; i++)
	{
		if ((adapter >= 0) && (i != (UINT)adapter))
		{
			if (i > (UINT)adapter)
				break;
			continue;
		}
		IDXGIAdapter1* a = nullptr;
		if (IDXGIFactory1_EnumAdapters1(factory, i, &a) == DXGI_ERROR_NOT_FOUND)
			break;
		(void)open_adapter(dec, a);
		IDXGIAdapter1_Release(a);
	}
	IDXGIFactory1_Release(factory);
	if (!dec->device)
		goto fail;
	return dec;

fail:
	av1_d3d11va_free(dec);
	return nullptr;
}

const char* av1_d3d11va_adapter_name(const AV1_D3D11VA* dec)
{
	return dec ? dec->adapterName : "";
}

void av1_d3d11va_reset(AV1_D3D11VA* dec)
{
	if (!dec)
		return;
	free_sequence(dec);
	av1_obu_parser_reset(dec->parser);
}

/* @return 0, AV1_D3D11VA_UNSUPPORTED or another negative value */
static INT32 setup_sequence(AV1_D3D11VA* dec, const Dav1dSequenceHeader* seq)
{
	free_sequence(dec);

	/* profile 0, 8 bit, 4:2:0 */
	if ((seq->profile != 0) || (seq->hbd != 0) || (seq->layout != DAV1D_PIXEL_LAYOUT_I420))
	{
		WLog_Print(dec->log, WLOG_DEBUG,
		           "D3D11VA: profile %u, %u bit, layout %d is not 8 bit 4:2:0: software decode",
		           seq->profile, 8u + 2u * seq->hbd, seq->layout);
		return AV1_D3D11VA_UNSUPPORTED;
	}

	const UINT32 w = (UINT32)seq->max_width;
	const UINT32 h = (UINT32)seq->max_height;
	D3D11_VIDEO_DECODER_DESC desc = {
		.Guid = av1_profile0, .SampleWidth = w, .SampleHeight = h, .OutputFormat = DXGI_FORMAT_NV12
	};
	UINT count = 0;
	if (FAILED(ID3D11VideoDevice_GetVideoDecoderConfigCount(dec->vdev, &desc, &count)) ||
	    (count == 0))
	{
		WLog_Print(dec->log, WLOG_DEBUG, "D3D11VA: no AV1 decoder for %" PRIu32 "x%" PRIu32, w, h);
		return AV1_D3D11VA_UNSUPPORTED;
	}
	D3D11_VIDEO_DECODER_CONFIG config = WINPR_C_ARRAY_INIT;
	BOOL found = FALSE;
	for (UINT i = 0; (i < count) && !found; i++)
	{
		D3D11_VIDEO_DECODER_CONFIG c = WINPR_C_ARRAY_INIT;
		if (FAILED(ID3D11VideoDevice_GetVideoDecoderConfig(dec->vdev, &desc, i, &c)))
			continue;
		/* AV1 tiles go to the driver as they are (no slice short format) */
		if (c.ConfigBitstreamRaw == 1)
		{
			config = c;
			found = TRUE;
		}
	}
	if (!found)
		return AV1_D3D11VA_UNSUPPORTED;

	HRESULT hr = ID3D11VideoDevice_CreateVideoDecoder(dec->vdev, &desc, &config, &dec->decoder);
	if (FAILED(hr))
	{
		WLog_Print(dec->log, WLOG_WARN,
		           "D3D11VA: CreateVideoDecoder %" PRIu32 "x%" PRIu32 ": 0x%08lx", w, h,
		           (unsigned long)hr);
		return AV1_D3D11VA_UNSUPPORTED;
	}

	D3D11_TEXTURE2D_DESC td = { .Width = (w + AV1_SURFACE_ALIGN - 1) & ~(AV1_SURFACE_ALIGN - 1u),
		                        .Height = (h + AV1_SURFACE_ALIGN - 1) & ~(AV1_SURFACE_ALIGN - 1u),
		                        .MipLevels = 1,
		                        .ArraySize = AV1_SURFACES,
		                        .Format = DXGI_FORMAT_NV12,
		                        .SampleDesc = { .Count = 1 },
		                        .Usage = D3D11_USAGE_DEFAULT,
		                        .BindFlags = D3D11_BIND_DECODER };
	hr = ID3D11Device_CreateTexture2D(dec->device, &td, nullptr, &dec->surfaces);
	if (FAILED(hr))
	{
		WLog_Print(dec->log, WLOG_WARN, "D3D11VA: surfaces: 0x%08lx", (unsigned long)hr);
		return AV1_D3D11VA_UNSUPPORTED;
	}
	for (UINT i = 0; i < AV1_SURFACES; i++)
	{
		D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = { .DecodeProfile = av1_profile0,
			                                        .ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D,
			                                        .Texture2D = { .ArraySlice = i } };
		hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(
		    dec->vdev, (ID3D11Resource*)dec->surfaces, &vd, &dec->views[i]);
		if (FAILED(hr))
		{
			WLog_Print(dec->log, WLOG_WARN, "D3D11VA: output view: 0x%08lx", (unsigned long)hr);
			return AV1_D3D11VA_UNSUPPORTED;
		}
	}
	dec->maxWidth = w;
	dec->maxHeight = h;
	WLog_Print(dec->log, WLOG_DEBUG, "D3D11VA: AV1 decoder %" PRIu32 "x%" PRIu32 " on %s", w, h,
	           dec->adapterName);
	return 0;
}

static int free_surface(const AV1_D3D11VA* dec, int busy)
{
	for (int s = 0; s < AV1_SURFACES; s++)
	{
		BOOL used = (s == busy);
		for (size_t i = 0; (i < DAV1D_NUM_REF_FRAMES) && !used; i++)
			used = (dec->slotSurface[i] == s);
		if (!used)
			return s;
	}
	return -1;
}

static UCHAR texture_index(const AV1_D3D11VA* dec, int slot)
{
	if ((slot < 0) || (slot >= DAV1D_NUM_REF_FRAMES) || (dec->slotSurface[slot] < 0))
		return 0xFF;
	return (UCHAR)dec->slotSurface[slot];
}

/* the DXVA picture parameters of frame @p f, decoded into surface @p cur */
static void fill_pic_params(const AV1_D3D11VA* dec, const Dav1dSequenceHeader* s,
                            const AV1_OBU_FRAME_HEADER* f, int cur, DXVA_PicParams_AV1* pp)
{
	const Dav1dFrameHeader* h = &f->d;
	memset(pp, 0, sizeof(*pp));

	pp->width = (UINT)h->width[1];
	pp->height = (UINT)h->height;
	pp->max_width = (UINT)s->max_width;
	pp->max_height = (UINT)s->max_height;
	pp->CurrPicTextureIndex = (UCHAR)cur;
	pp->superres_denom = h->super_res.enabled ? h->super_res.width_scale_denominator : 8;
	pp->bitdepth = (UCHAR)(8 + 2 * s->hbd);
	pp->seq_profile = s->profile;

	pp->tiles.cols = h->tiling.cols;
	pp->tiles.rows = h->tiling.rows;
	pp->tiles.context_update_id = h->tiling.update;
	for (size_t i = 0; i < h->tiling.cols; i++)
		pp->tiles.widths[i] = (USHORT)(h->tiling.col_start_sb[i + 1] - h->tiling.col_start_sb[i]);
	for (size_t i = 0; i < h->tiling.rows; i++)
		pp->tiles.heights[i] = (USHORT)(h->tiling.row_start_sb[i + 1] - h->tiling.row_start_sb[i]);

	pp->coding.use_128x128_superblock = s->sb128;
	pp->coding.intra_edge_filter = s->intra_edge_filter;
	pp->coding.interintra_compound = s->inter_intra;
	pp->coding.masked_compound = s->masked_compound;
	pp->coding.warped_motion = h->warp_motion;
	pp->coding.dual_filter = s->dual_filter;
	pp->coding.jnt_comp = s->jnt_comp;
	pp->coding.screen_content_tools = h->allow_screen_content_tools;
	pp->coding.integer_mv = h->force_integer_mv;
	pp->coding.cdef = s->cdef;
	pp->coding.restoration = s->restoration;
	pp->coding.film_grain = s->film_grain_present;
	pp->coding.intrabc = h->allow_intrabc;
	pp->coding.high_precision_mv = h->hp;
	pp->coding.switchable_motion_mode = h->switchable_motion_mode;
	pp->coding.filter_intra = s->filter_intra;
	pp->coding.disable_frame_end_update_cdf = !h->refresh_context;
	pp->coding.disable_cdf_update = h->disable_cdf_update;
	pp->coding.reference_mode = h->switchable_comp_refs;
	pp->coding.skip_mode = h->skip_mode_enabled;
	pp->coding.reduced_tx_set = h->reduced_txtp_set;
	pp->coding.superres = h->super_res.enabled;
	pp->coding.tx_mode = (UINT)h->txfm_mode;
	pp->coding.use_ref_frame_mvs = h->use_ref_frame_mvs;
	pp->coding.enable_ref_frame_mvs = s->ref_frame_mvs;
	pp->coding.reference_frame_update = 1;

	pp->format.frame_type = (UCHAR)h->frame_type;
	pp->format.show_frame = h->show_frame;
	pp->format.showable_frame = h->showable_frame;
	pp->format.subsampling_x = s->ss_hor;
	pp->format.subsampling_y = s->ss_ver;
	pp->format.mono_chrome = s->monochrome;

	pp->primary_ref_frame = h->primary_ref_frame;
	pp->order_hint = h->frame_offset;
	pp->order_hint_bits = s->order_hint ? s->order_hint_n_bits : 0;

	for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
	{
		DXVA_PicEntry_AV1* e = &pp->frame_refs[i];
		const int slot = h->refidx[i];
		const AV1_OBU_FRAME_HEADER* ref = av1_obu_parser_reference(dec->parser, (size_t)slot);
		if (ref && (dec->slotSurface[slot] >= 0))
		{
			e->width = (UINT)ref->d.width[1];
			e->height = (UINT)ref->d.height;
			e->Index = (UCHAR)slot;
		}
		else
			e->Index = 0xFF;
		e->wminvalid = f->gmInvalid[i];
		e->wmtype = (UCHAR)h->gmv[i].type;
		for (size_t j = 0; j < 6; j++)
			e->wmmat[j] = h->gmv[i].matrix[j];
	}
	for (int i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		pp->RefFrameMapTextureIndex[i] = texture_index(dec, i);

	pp->loop_filter.filter_level[0] = h->loopfilter.level_y[0];
	pp->loop_filter.filter_level[1] = h->loopfilter.level_y[1];
	pp->loop_filter.filter_level_u = h->loopfilter.level_u;
	pp->loop_filter.filter_level_v = h->loopfilter.level_v;
	pp->loop_filter.sharpness_level = h->loopfilter.sharpness;
	pp->loop_filter.mode_ref_delta_enabled = h->loopfilter.mode_ref_delta_enabled;
	pp->loop_filter.mode_ref_delta_update = h->loopfilter.mode_ref_delta_update;
	pp->loop_filter.delta_lf_multi = h->delta.lf.multi;
	pp->loop_filter.delta_lf_present = h->delta.lf.present;
	for (size_t i = 0; i < 8; i++)
		pp->loop_filter.ref_deltas[i] = h->loopfilter.mode_ref_deltas.ref_delta[i];
	for (size_t i = 0; i < 2; i++)
		pp->loop_filter.mode_deltas[i] = h->loopfilter.mode_ref_deltas.mode_delta[i];
	pp->loop_filter.delta_lf_res = h->delta.lf.res_log2;
	/* dav1d keeps the coded lr_type; DXVA wants FrameRestorationType (Remap_Lr_Type) */
	static const UCHAR remap_lr_type[4] = { 0, 3, 1, 2 };
	const BOOL lr = h->restoration.type[0] || h->restoration.type[1] || h->restoration.type[2];
	for (size_t i = 0; i < 3; i++)
	{
		pp->loop_filter.frame_restoration_type[i] = remap_lr_type[h->restoration.type[i] & 3];
		pp->loop_filter.log2_restoration_unit_size[i] =
		    lr ? h->restoration.unit_size[i ? 1 : 0] : 8;
	}

	pp->quantization.delta_q_present = h->delta.q.present;
	pp->quantization.delta_q_res = h->delta.q.res_log2;
	pp->quantization.base_qindex = h->quant.yac;
	pp->quantization.y_dc_delta_q = h->quant.ydc_delta;
	pp->quantization.u_dc_delta_q = h->quant.udc_delta;
	pp->quantization.v_dc_delta_q = h->quant.vdc_delta;
	pp->quantization.u_ac_delta_q = h->quant.uac_delta;
	pp->quantization.v_ac_delta_q = h->quant.vac_delta;
	pp->quantization.qm_y = h->quant.qm ? h->quant.qm_y : 0xFF;
	pp->quantization.qm_u = h->quant.qm ? h->quant.qm_u : 0xFF;
	pp->quantization.qm_v = h->quant.qm ? h->quant.qm_v : 0xFF;

	pp->cdef.damping = h->cdef.damping ? (UCHAR)(h->cdef.damping - 3) : 0;
	pp->cdef.bits = h->cdef.n_bits;
	for (size_t i = 0; i < 8; i++)
	{
		/* dav1d keeps the 6 coded bits: primary << 2 | secondary */
		pp->cdef.y_strengths[i].primary = h->cdef.y_strength[i] >> 2;
		pp->cdef.y_strengths[i].secondary = h->cdef.y_strength[i] & 3;
		pp->cdef.uv_strengths[i].primary = h->cdef.uv_strength[i] >> 2;
		pp->cdef.uv_strengths[i].secondary = h->cdef.uv_strength[i] & 3;
	}

	pp->interp_filter = (UCHAR)h->subpel_filter_mode;

	pp->segmentation.enabled = h->segmentation.enabled;
	pp->segmentation.update_map = h->segmentation.update_map;
	pp->segmentation.update_data = h->segmentation.update_data;
	pp->segmentation.temporal_update = h->segmentation.temporal;
	if (h->segmentation.enabled)
	{
		for (size_t i = 0; i < DAV1D_MAX_SEGMENTS; i++)
		{
			const Dav1dSegmentationData* d = &h->segmentation.seg_data.d[i];
			pp->segmentation.feature_mask[i].mask = f->segFeatureMask[i];
			pp->segmentation.feature_data[i][0] = d->delta_q;
			pp->segmentation.feature_data[i][1] = d->delta_lf_y_v;
			pp->segmentation.feature_data[i][2] = d->delta_lf_y_h;
			pp->segmentation.feature_data[i][3] = d->delta_lf_u;
			pp->segmentation.feature_data[i][4] = d->delta_lf_v;
			pp->segmentation.feature_data[i][5] = (SHORT)(d->ref >= 0 ? d->ref : 0);
		}
	}

	if (h->film_grain.present)
	{
		const Dav1dFilmGrainData* g = &h->film_grain.data;
		pp->film_grain.apply_grain = 1;
		pp->film_grain.scaling_shift_minus8 = (USHORT)(g->scaling_shift - 8);
		pp->film_grain.chroma_scaling_from_luma = (USHORT)g->chroma_scaling_from_luma;
		pp->film_grain.ar_coeff_lag = (USHORT)g->ar_coeff_lag;
		pp->film_grain.ar_coeff_shift_minus6 = (USHORT)(g->ar_coeff_shift - 6);
		pp->film_grain.grain_scale_shift = (USHORT)g->grain_scale_shift;
		pp->film_grain.overlap_flag = (USHORT)g->overlap_flag;
		pp->film_grain.clip_to_restricted_range = (USHORT)g->clip_to_restricted_range;
		pp->film_grain.matrix_coeff_is_identity = (s->mtrx == DAV1D_MC_IDENTITY);
		pp->film_grain.grain_seed = (USHORT)g->seed;
		pp->film_grain.num_y_points = (UCHAR)g->num_y_points;
		for (int i = 0; i < g->num_y_points; i++)
		{
			pp->film_grain.scaling_points_y[i][0] = g->y_points[i][0];
			pp->film_grain.scaling_points_y[i][1] = g->y_points[i][1];
		}
		pp->film_grain.num_cb_points = (UCHAR)g->num_uv_points[0];
		for (int i = 0; i < g->num_uv_points[0]; i++)
		{
			pp->film_grain.scaling_points_cb[i][0] = g->uv_points[0][i][0];
			pp->film_grain.scaling_points_cb[i][1] = g->uv_points[0][i][1];
		}
		pp->film_grain.num_cr_points = (UCHAR)g->num_uv_points[1];
		for (int i = 0; i < g->num_uv_points[1]; i++)
		{
			pp->film_grain.scaling_points_cr[i][0] = g->uv_points[1][i][0];
			pp->film_grain.scaling_points_cr[i][1] = g->uv_points[1][i][1];
		}
		/* DXVA keeps the coded values: the coefficients + 128, the offsets + 256 */
		for (size_t i = 0; i < 24; i++)
			pp->film_grain.ar_coeffs_y[i] = (UCHAR)(g->ar_coeffs_y[i] + 128);
		for (size_t i = 0; i < 25; i++)
		{
			pp->film_grain.ar_coeffs_cb[i] = (UCHAR)(g->ar_coeffs_uv[0][i] + 128);
			pp->film_grain.ar_coeffs_cr[i] = (UCHAR)(g->ar_coeffs_uv[1][i] + 128);
		}
		pp->film_grain.cb_mult = (UCHAR)(g->uv_mult[0] + 128);
		pp->film_grain.cb_luma_mult = (UCHAR)(g->uv_luma_mult[0] + 128);
		pp->film_grain.cr_mult = (UCHAR)(g->uv_mult[1] + 128);
		pp->film_grain.cr_luma_mult = (UCHAR)(g->uv_luma_mult[1] + 128);
		pp->film_grain.cb_offset = (SHORT)(g->uv_offset[0] + 256);
		pp->film_grain.cr_offset = (SHORT)(g->uv_offset[1] + 256);
	}

	/* 0: no status report. A number asks the driver for one per frame, and D3D11 has no call
	 * to read them: NVIDIA's driver (nvwgf2umx.dll 32.0.15.9613, RTX 5070) fails fast once 24
	 * are queued, on the 25th frame of every decoder. */
	pp->StatusReportFeedbackNumber = 0;
}

static HRESULT begin_frame(AV1_D3D11VA* dec, int cur)
{
	HRESULT hr = E_FAIL;
	/* the driver may still use the surface: E_PENDING, try again (100 ms at most) */
	for (int tries = 0; tries < 50; tries++)
	{
		hr = ID3D11VideoContext_DecoderBeginFrame(dec->vctx, dec->decoder, dec->views[cur], 0,
		                                          nullptr);
		if (hr != E_PENDING)
			break;
		Sleep(2);
	}
	return hr;
}

static BOOL put_buffer(AV1_D3D11VA* dec, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data,
                       UINT size)
{
	UINT avail = 0;
	void* p = nullptr;
	if (FAILED(ID3D11VideoContext_GetDecoderBuffer(dec->vctx, dec->decoder, type, &avail, &p)))
		return FALSE;
	const BOOL fits = (size <= avail);
	if (fits)
		memcpy(p, data, size);
	ID3D11VideoContext_ReleaseDecoderBuffer(dec->vctx, dec->decoder, type);
	return fits;
}

static BOOL decode_frame(AV1_D3D11VA* dec, const Dav1dSequenceHeader* seq, const BYTE* tu,
                         const AV1_OBU_EVENT* ev, int cur)
{
	DXVA_PicParams_AV1 pp = WINPR_C_ARRAY_INIT;
	fill_pic_params(dec, seq, ev->hdr, cur, &pp);

	DXVA_Tile_AV1* tiles = calloc(ev->nTiles, sizeof(DXVA_Tile_AV1));
	if (!tiles)
		return FALSE;
	size_t total = 0;
	for (UINT32 i = 0; i < ev->nTiles; i++)
	{
		tiles[i].DataOffset = (UINT)total;
		tiles[i].DataSize = ev->tiles[i].size;
		tiles[i].row = ev->tiles[i].row;
		tiles[i].column = ev->tiles[i].column;
		tiles[i].anchor_frame = 0xFF;
		total += ev->tiles[i].size;
	}
	const size_t padded = (total + AV1_BITSTREAM_ALIGN - 1) & ~(size_t)(AV1_BITSTREAM_ALIGN - 1);

	BOOL rc = FALSE;
	WLog_Print(dec->log, WLOG_TRACE,
	           "D3D11VA: frame type %d %dx%d into surface %d, %" PRIu32 " tiles, %" PRIuz " bytes",
	           ev->hdr->d.frame_type, ev->hdr->d.width[1], ev->hdr->d.height, cur, ev->nTiles,
	           total);
	HRESULT hr = begin_frame(dec, cur);
	if (FAILED(hr))
	{
		WLog_Print(dec->log, WLOG_ERROR, "D3D11VA: DecoderBeginFrame: 0x%08lx", (unsigned long)hr);
		free(tiles);
		return FALSE;
	}

	if (!put_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &pp, sizeof(pp)))
		goto end;
	if (!put_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, tiles,
	                (UINT)(ev->nTiles * sizeof(DXVA_Tile_AV1))))
		goto end;
	{
		UINT avail = 0;
		BYTE* p = nullptr;
		if (FAILED(ID3D11VideoContext_GetDecoderBuffer(
		        dec->vctx, dec->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &avail, (void**)&p)))
			goto end;
		const BOOL fits = (padded <= avail);
		if (fits)
		{
			for (UINT32 i = 0; i < ev->nTiles; i++)
				memcpy(&p[tiles[i].DataOffset], &tu[ev->tiles[i].offset], ev->tiles[i].size);
			memset(&p[total], 0, padded - total);
		}
		ID3D11VideoContext_ReleaseDecoderBuffer(dec->vctx, dec->decoder,
		                                        D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
		if (!fits)
		{
			WLog_Print(dec->log, WLOG_ERROR,
			           "D3D11VA: frame of %" PRIuz " bytes, the bitstream buffer has %u", total,
			           avail);
			goto end;
		}
	}

	{
		D3D11_VIDEO_DECODER_BUFFER_DESC bufs[3] = {
			{ .BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, .DataSize = sizeof(pp) },
			{ .BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
			  .DataSize = (UINT)(ev->nTiles * sizeof(DXVA_Tile_AV1)) },
			{ .BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, .DataSize = (UINT)padded },
		};
		WLog_Print(dec->log, WLOG_TRACE, "D3D11VA: submit");
		hr = ID3D11VideoContext_SubmitDecoderBuffers(dec->vctx, dec->decoder, 3, bufs);
		if (FAILED(hr))
		{
			WLog_Print(dec->log, WLOG_ERROR, "D3D11VA: SubmitDecoderBuffers: 0x%08lx",
			           (unsigned long)hr);
			goto end;
		}
	}
	rc = TRUE;

end:
	WLog_Print(dec->log, WLOG_TRACE, "D3D11VA: end frame (submitted %d)", rc);
	hr = ID3D11VideoContext_DecoderEndFrame(dec->vctx, dec->decoder);
	if (FAILED(hr))
	{
		WLog_Print(dec->log, WLOG_ERROR, "D3D11VA: DecoderEndFrame: 0x%08lx", (unsigned long)hr);
		rc = FALSE;
	}
	free(tiles);
	return rc;
}

/* surface @p s to system memory: Y in the staging map, U and V separated */
static BOOL copy_back(AV1_D3D11VA* dec, int s, UINT32 w, UINT32 h, AV1_D3D11VA_PICTURE* pic)
{
	const UINT32 w2 = (w + 1) & ~1u;
	const UINT32 h2 = (h + 1) & ~1u;
	if (!dec->staging || (dec->stagingWidth != w2) || (dec->stagingHeight != h2))
	{
		RELEASE(dec->staging);
		D3D11_TEXTURE2D_DESC td = { .Width = w2,
			                        .Height = h2,
			                        .MipLevels = 1,
			                        .ArraySize = 1,
			                        .Format = DXGI_FORMAT_NV12,
			                        .SampleDesc = { .Count = 1 },
			                        .Usage = D3D11_USAGE_STAGING,
			                        .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
		const HRESULT hr = ID3D11Device_CreateTexture2D(dec->device, &td, nullptr, &dec->staging);
		if (FAILED(hr))
		{
			WLog_Print(dec->log, WLOG_ERROR, "D3D11VA: staging texture: 0x%08lx",
			           (unsigned long)hr);
			return FALSE;
		}
		dec->stagingWidth = w2;
		dec->stagingHeight = h2;
	}

	WLog_Print(dec->log, WLOG_TRACE, "D3D11VA: copy back surface %d %" PRIu32 "x%" PRIu32, s, w, h);
	const D3D11_BOX box = { 0, 0, 0, w2, h2, 1 };
	ID3D11DeviceContext_CopySubresourceRegion(dec->ctx, (ID3D11Resource*)dec->staging, 0, 0, 0, 0,
	                                          (ID3D11Resource*)dec->surfaces, (UINT)s, &box);
	D3D11_MAPPED_SUBRESOURCE m = WINPR_C_ARRAY_INIT;
	const HRESULT hr =
	    ID3D11DeviceContext_Map(dec->ctx, (ID3D11Resource*)dec->staging, 0, D3D11_MAP_READ, 0, &m);
	if (FAILED(hr))
	{
		WLog_Print(dec->log, WLOG_ERROR, "D3D11VA: Map: 0x%08lx", (unsigned long)hr);
		return FALSE;
	}
	dec->mapped = TRUE;

	const UINT32 cw = (w + 1) / 2;
	const UINT32 ch = (h + 1) / 2;
	const size_t need = (size_t)cw * ch;
	if (need > dec->chromaSize)
	{
		BYTE* u = realloc(dec->u, need);
		if (u)
			dec->u = u;
		BYTE* v = realloc(dec->v, need);
		if (v)
			dec->v = v;
		if (!u || !v)
			return FALSE;
		dec->chromaSize = need;
	}
	const BYTE* y = m.pData;
	const BYTE* uv = &y[(size_t)m.RowPitch * h2];
	for (UINT32 r = 0; r < ch; r++)
	{
		const BYTE* src = &uv[(size_t)m.RowPitch * r];
		BYTE* du = &dec->u[(size_t)cw * r];
		BYTE* dv = &dec->v[(size_t)cw * r];
		for (UINT32 c = 0; c < cw; c++)
		{
			du[c] = src[2 * c];
			dv[c] = src[2 * c + 1];
		}
	}

	pic->planes[0] = y;
	pic->planes[1] = dec->u;
	pic->planes[2] = dec->v;
	pic->strides[0] = m.RowPitch;
	pic->strides[1] = pic->strides[2] = cw;
	pic->width = w;
	pic->height = h;
	return TRUE;
}

INT32 av1_d3d11va_decode(AV1_D3D11VA* dec, const BYTE* pSrcData, UINT32 SrcSize,
                         AV1_D3D11VA_PICTURE* pic)
{
	WINPR_ASSERT(dec);
	WINPR_ASSERT(pic);

	unmap_staging(dec);
	if (!pSrcData || (SrcSize == 0))
		return AV1_D3D11VA_INVALID;

	BOOL submitted = FALSE;
	int show = -1;
	UINT32 showWidth = 0;
	UINT32 showHeight = 0;

	av1_obu_parser_begin(dec->parser, pSrcData, SrcSize);
	for (;;)
	{
		AV1_OBU_EVENT ev = WINPR_C_ARRAY_INIT;
		const int rc = av1_obu_parser_next(dec->parser, &ev);
		if (rc < 0)
		{
			WLog_Print(dec->log, WLOG_WARN,
			           "D3D11VA: invalid AV1 temporal unit (%" PRIu32 " bytes)", SrcSize);
			return submitted ? -1 : AV1_D3D11VA_INVALID;
		}
		if (rc == 0)
			break;

		const Dav1dSequenceHeader* seq = av1_obu_parser_sequence(dec->parser);
		WINPR_ASSERT(seq);
		if (ev.newSequence || !dec->decoder)
		{
			const INT32 src = setup_sequence(dec, seq);
			if (src < 0)
				return submitted ? -2 : src;
		}

		if (ev.type == AV1_OBU_EVENT_SHOW_EXISTING)
		{
			const int s = dec->slotSurface[ev.frameIdx];
			const AV1_OBU_FRAME_HEADER* ref = av1_obu_parser_reference(dec->parser, ev.frameIdx);
			if ((s < 0) || !ref)
				return submitted ? -3 : AV1_D3D11VA_INVALID;
			show = s;
			showWidth = (UINT32)ref->d.width[1];
			showHeight = (UINT32)ref->d.height;
			for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
			{
				if (ev.refreshFrameFlags & (1u << i))
					dec->slotSurface[i] = s;
			}
			continue;
		}

		const Dav1dFrameHeader* h = &ev.hdr->d;
		if ((h->width[1] <= 0) || (h->height <= 0) || ((UINT32)h->width[1] > dec->maxWidth) ||
		    ((UINT32)h->height > dec->maxHeight))
			return submitted ? -4 : AV1_D3D11VA_INVALID;
		/* an inter frame needs its references (none after a reset or a lost key frame) */
		if (h->frame_type & 1)
		{
			for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
			{
				if (texture_index(dec, h->refidx[i]) == 0xFF)
					return submitted ? -5 : AV1_D3D11VA_INVALID;
			}
		}

		const int cur = free_surface(dec, show);
		if (cur < 0)
			return -6;
		if (!decode_frame(dec, seq, pSrcData, &ev, cur))
			return -7;
		submitted = TRUE;
		for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		{
			if (ev.refreshFrameFlags & (1u << i))
				dec->slotSurface[i] = cur;
		}
		if (h->show_frame)
		{
			show = cur;
			showWidth = (UINT32)h->width[1];
			showHeight = (UINT32)h->height;
		}
	}

	if (show < 0)
		return FALSE;
	if (!copy_back(dec, show, showWidth, showHeight, pic))
		return -8;
	return TRUE;
}
