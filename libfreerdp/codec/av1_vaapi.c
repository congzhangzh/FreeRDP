/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 decoding with VA-API (Linux)
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

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>

#include <va/va.h>
#include <va/va_drm.h>

#include "av1_obu.h"
#include "av1_vaapi.h"

/* 8 reference slots, the frame being decoded and one spare */
#define AV1_SURFACES 10
/* the DRM render nodes, /dev/dri/renderD128 and up */
#define AV1_RENDER_FIRST 128
#define AV1_RENDER_COUNT 64

/* libva is loaded at run time: a client without it (or without a driver for its GPU) still
 * starts and decodes in software. The prototypes come from the headers. */
#define VA_FUNCTIONS(X)         \
	X(vaInitialize)             \
	X(vaTerminate)              \
	X(vaErrorStr)               \
	X(vaSetErrorCallback)       \
	X(vaSetInfoCallback)        \
	X(vaQueryVendorString)      \
	X(vaMaxNumProfiles)         \
	X(vaQueryConfigProfiles)    \
	X(vaMaxNumEntrypoints)      \
	X(vaQueryConfigEntrypoints) \
	X(vaGetConfigAttributes)    \
	X(vaCreateConfig)           \
	X(vaDestroyConfig)          \
	X(vaCreateSurfaces)         \
	X(vaDestroySurfaces)        \
	X(vaCreateContext)          \
	X(vaDestroyContext)         \
	X(vaCreateBuffer)           \
	X(vaDestroyBuffer)          \
	X(vaBeginPicture)           \
	X(vaRenderPicture)          \
	X(vaEndPicture)             \
	X(vaSyncSurface)            \
	X(vaCreateImage)            \
	X(vaDestroyImage)           \
	X(vaGetImage)               \
	X(vaMapBuffer)              \
	X(vaUnmapBuffer)

#define VA_FIELD(name) __typeof__(name)* name;
typedef struct
{
	BOOL loaded;
	__typeof__(vaGetDisplayDRM)* vaGetDisplayDRM;
	VA_FUNCTIONS(VA_FIELD)
} AV1_VA_API;

static AV1_VA_API va = WINPR_C_ARRAY_INIT;
static INIT_ONCE vaOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK va_load(PINIT_ONCE once, PVOID param, PVOID* context)
{
	WINPR_UNUSED(once);
	WINPR_UNUSED(param);
	WINPR_UNUSED(context);

	/* never closed: a driver may keep threads of its own */
	void* lib = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
	void* drm = lib ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : nullptr;
	if (!lib || !drm)
		return TRUE;
	BOOL ok = TRUE;
	va.vaGetDisplayDRM = (__typeof__(va.vaGetDisplayDRM))dlsym(drm, "vaGetDisplayDRM");
	ok = ok && va.vaGetDisplayDRM;
#define VA_LOAD(name)                                 \
	va.name = (__typeof__(va.name))dlsym(lib, #name); \
	ok = ok && va.name;
	VA_FUNCTIONS(VA_LOAD)
	va.loaded = ok;
	return TRUE;
}

static BOOL va_loaded(void)
{
	if (!InitOnceExecuteOnce(&vaOnce, va_load, nullptr, nullptr))
		return FALSE;
	return va.loaded;
}

struct S_AV1_VAAPI
{
	wLog* log;
	char name[256];
	INT32 id; /* the device's id in av1_vaapi_list() */

	int fd;
	VADisplay dpy;
	VAConfigID config;

	AV1_OBU_PARSER* parser;

	/* the decoder for the current sequence */
	VAContextID context;
	VASurfaceID surfaces[AV1_SURFACES];
	BOOL haveSurfaces;
	UINT32 maxWidth;
	UINT32 maxHeight;
	UINT32 surfaceWidth;
	UINT32 surfaceHeight;
	int slotSurface[DAV1D_NUM_REF_FRAMES]; /* -1: empty */

	/* copy back */
	VAImage image;
	BOOL haveImage;
	BOOL mapped;
	BYTE* u;
	BYTE* v;
	size_t chromaSize;
};

/* libva's own messages go to the log (not to stderr) */
static void va_message(void* context, const char* message)
{
	wLog* log = context;
	if (!log || !message)
		return;
	const size_t len = strlen(message);
	const int n = (len > 0) && (message[len - 1] == '\n') ? (int)len - 1 : (int)len;
	WLog_Print(log, WLOG_DEBUG, "libva: %.*s", n, message);
}

static void unmap_image(AV1_VAAPI* dec)
{
	if (dec->mapped)
		(void)va.vaUnmapBuffer(dec->dpy, dec->image.buf);
	dec->mapped = FALSE;
}

static void free_image(AV1_VAAPI* dec)
{
	unmap_image(dec);
	if (dec->haveImage)
		(void)va.vaDestroyImage(dec->dpy, dec->image.image_id);
	dec->haveImage = FALSE;
}

static void free_sequence(AV1_VAAPI* dec)
{
	if (!dec->dpy)
		return;
	free_image(dec);
	if (dec->context != VA_INVALID_ID)
		(void)va.vaDestroyContext(dec->dpy, dec->context);
	dec->context = VA_INVALID_ID;
	if (dec->haveSurfaces)
		(void)va.vaDestroySurfaces(dec->dpy, dec->surfaces, AV1_SURFACES);
	dec->haveSurfaces = FALSE;
	dec->maxWidth = dec->maxHeight = 0;
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		dec->slotSurface[i] = -1;
}

static void close_device(AV1_VAAPI* dec)
{
	free_sequence(dec);
	if (dec->dpy)
	{
		if (dec->config != VA_INVALID_ID)
			(void)va.vaDestroyConfig(dec->dpy, dec->config);
		(void)va.vaTerminate(dec->dpy);
	}
	dec->config = VA_INVALID_ID;
	dec->dpy = nullptr;
	if (dec->fd >= 0)
		close(dec->fd);
	dec->fd = -1;
	dec->name[0] = '\0';
}

static BOOL device_decodes_av1(VADisplay dpy)
{
	const int maxProfiles = va.vaMaxNumProfiles(dpy);
	if (maxProfiles <= 0)
		return FALSE;
	VAProfile* profiles = calloc((size_t)maxProfiles, sizeof(VAProfile));
	if (!profiles)
		return FALSE;
	int n = 0;
	BOOL have = FALSE;
	if (va.vaQueryConfigProfiles(dpy, profiles, &n) == VA_STATUS_SUCCESS)
	{
		for (int i = 0; (i < n) && !have; i++)
			have = (profiles[i] == VAProfileAV1Profile0);
	}
	free(profiles);
	if (!have)
		return FALSE;

	const int maxEntrypoints = va.vaMaxNumEntrypoints(dpy);
	if (maxEntrypoints <= 0)
		return FALSE;
	VAEntrypoint* entrypoints = calloc((size_t)maxEntrypoints, sizeof(VAEntrypoint));
	if (!entrypoints)
		return FALSE;
	have = FALSE;
	if (va.vaQueryConfigEntrypoints(dpy, VAProfileAV1Profile0, entrypoints, &n) ==
	    VA_STATUS_SUCCESS)
	{
		for (int i = 0; (i < n) && !have; i++)
			have = (entrypoints[i] == VAEntrypointVLD);
	}
	free(entrypoints);
	if (!have)
		return FALSE;

	VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat };
	if (va.vaGetConfigAttributes(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &attrib, 1) !=
	    VA_STATUS_SUCCESS)
		return FALSE;
	return (attrib.value != VA_ATTRIB_NOT_SUPPORTED) && (attrib.value & VA_RT_FORMAT_YUV420);
}

/* the display on render node @p node when it decodes AV1 */
static BOOL open_device(AV1_VAAPI* dec, wLog* log, int node)
{
	char path[64] = WINPR_C_ARRAY_INIT;
	(void)snprintf(path, sizeof(path), "/dev/dri/renderD%d", node);
	const int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	VADisplay dpy = va.vaGetDisplayDRM(fd);
	if (!dpy)
	{
		close(fd);
		return FALSE;
	}
	(void)va.vaSetErrorCallback(dpy, va_message, log);
	(void)va.vaSetInfoCallback(dpy, va_message, log);
	int major = 0;
	int minor = 0;
	if (va.vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS)
	{
		/* the display is terminated, which also releases it */
		(void)va.vaTerminate(dpy);
		close(fd);
		return FALSE;
	}
	if (!device_decodes_av1(dpy))
	{
		(void)va.vaTerminate(dpy);
		close(fd);
		return FALSE;
	}
	dec->fd = fd;
	dec->dpy = dpy;
	const char* vendor = va.vaQueryVendorString(dpy);
	(void)snprintf(dec->name, sizeof(dec->name), "%s (%s)", vendor ? vendor : "VA-API", path);
	return TRUE;
}

size_t av1_vaapi_list(char names[][256], size_t max)
{
	if (!va_loaded())
		return 0;
	size_t count = 0;
	for (int i = 0; (i < AV1_RENDER_COUNT) && (count < max); i++)
	{
		AV1_VAAPI probe = { .fd = -1, .config = VA_INVALID_ID, .context = VA_INVALID_ID };
		if (open_device(&probe, nullptr, AV1_RENDER_FIRST + i))
		{
			(void)snprintf(names[count], 256, "%s", probe.name);
			count++;
			close_device(&probe);
		}
	}
	return count;
}

INT32 av1_vaapi_device(const AV1_VAAPI* dec)
{
	return dec ? dec->id : -1;
}

void av1_vaapi_free(AV1_VAAPI* dec)
{
	if (!dec)
		return;
	close_device(dec);
	av1_obu_parser_free(dec->parser);
	free(dec->u);
	free(dec->v);
	free(dec);
}

AV1_VAAPI* av1_vaapi_new(wLog* log, INT32 device)
{
	if (!va_loaded())
	{
		WLog_Print(log, WLOG_DEBUG, "VA-API: no libva.so.2 / libva-drm.so.2");
		return nullptr;
	}
	AV1_VAAPI* dec = calloc(1, sizeof(AV1_VAAPI));
	if (!dec)
		return nullptr;
	dec->log = log;
	dec->fd = -1;
	dec->config = VA_INVALID_ID;
	dec->context = VA_INVALID_ID;
	for (size_t i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		dec->slotSurface[i] = -1;
	dec->parser = av1_obu_parser_new();
	if (!dec->parser)
		goto fail;

	/* the devices are the render nodes with an AV1 decoder, in order (/codec-list) */
	INT32 id = 0;
	for (int i = 0; (i < AV1_RENDER_COUNT) && !dec->dpy; i++)
	{
		if (open_device(dec, log, AV1_RENDER_FIRST + i))
		{
			if ((device >= 0) && (id != device))
				close_device(dec);
			else
				dec->id = id;
			id++;
		}
	}
	if (!dec->dpy)
		goto fail;

	VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
	const VAStatus st = va.vaCreateConfig(dec->dpy, VAProfileAV1Profile0, VAEntrypointVLD, &attrib,
	                                      1, &dec->config);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(log, WLOG_WARN, "VA-API: vaCreateConfig on %s: %s", dec->name,
		           va.vaErrorStr(st));
		dec->config = VA_INVALID_ID;
		goto fail;
	}
	return dec;

fail:
	av1_vaapi_free(dec);
	return nullptr;
}

const char* av1_vaapi_name(const AV1_VAAPI* dec)
{
	return dec ? dec->name : "";
}

void av1_vaapi_reset(AV1_VAAPI* dec)
{
	if (!dec)
		return;
	free_sequence(dec);
	av1_obu_parser_reset(dec->parser);
}

/* @return 0, AV1_VAAPI_UNSUPPORTED or another negative value */
static INT32 setup_sequence(AV1_VAAPI* dec, const Dav1dSequenceHeader* seq)
{
	free_sequence(dec);

	/* profile 0, 8 bit, 4:2:0; film grain would need a second output surface: software */
	if ((seq->profile != 0) || (seq->hbd != 0) || (seq->layout != DAV1D_PIXEL_LAYOUT_I420) ||
	    seq->film_grain_present)
	{
		WLog_Print(dec->log, WLOG_DEBUG,
		           "VA-API: profile %u, %u bit, layout %d, film grain %u is not 8 bit 4:2:0: "
		           "software decode",
		           seq->profile, 8u + 2u * seq->hbd, seq->layout, seq->film_grain_present);
		return AV1_VAAPI_UNSUPPORTED;
	}

	const UINT32 w = (UINT32)seq->max_width;
	const UINT32 h = (UINT32)seq->max_height;
	/* even, so the copy back (whole chroma samples) stays inside the surface */
	const UINT32 sw = (w + 1) & ~1u;
	const UINT32 sh = (h + 1) & ~1u;
	VASurfaceAttrib attrib = { .type = VASurfaceAttribPixelFormat,
		                       .flags = VA_SURFACE_ATTRIB_SETTABLE,
		                       .value = { .type = VAGenericValueTypeInteger,
		                                  .value = { .i = VA_FOURCC_NV12 } } };
	VAStatus st = va.vaCreateSurfaces(dec->dpy, VA_RT_FORMAT_YUV420, sw, sh, dec->surfaces,
	                                  AV1_SURFACES, &attrib, 1);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_WARN, "VA-API: vaCreateSurfaces %" PRIu32 "x%" PRIu32 ": %s", sw,
		           sh, va.vaErrorStr(st));
		return AV1_VAAPI_UNSUPPORTED;
	}
	dec->haveSurfaces = TRUE;

	st = va.vaCreateContext(dec->dpy, dec->config, (int)w, (int)h, VA_PROGRESSIVE, dec->surfaces,
	                        AV1_SURFACES, &dec->context);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_WARN, "VA-API: vaCreateContext %" PRIu32 "x%" PRIu32 ": %s", w, h,
		           va.vaErrorStr(st));
		dec->context = VA_INVALID_ID;
		return AV1_VAAPI_UNSUPPORTED;
	}
	dec->maxWidth = w;
	dec->maxHeight = h;
	dec->surfaceWidth = sw;
	dec->surfaceHeight = sh;
	WLog_Print(dec->log, WLOG_DEBUG, "VA-API: AV1 decoder %" PRIu32 "x%" PRIu32 " on %s", w, h,
	           dec->name);
	return 0;
}

static int free_surface(const AV1_VAAPI* dec, int busy)
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

static VASurfaceID slot_surface(const AV1_VAAPI* dec, int slot)
{
	if ((slot < 0) || (slot >= DAV1D_NUM_REF_FRAMES) || (dec->slotSurface[slot] < 0))
		return VA_INVALID_SURFACE;
	return dec->surfaces[dec->slotSurface[slot]];
}

/* the VA-API picture parameters of frame @p f, decoded into surface @p cur */
static void fill_pic_params(const AV1_VAAPI* dec, const Dav1dSequenceHeader* s,
                            const AV1_OBU_FRAME_HEADER* f, int cur,
                            VADecPictureParameterBufferAV1* pp)
{
	const Dav1dFrameHeader* h = &f->d;
	memset(pp, 0, sizeof(*pp));

	pp->profile = s->profile;
	pp->order_hint_bits_minus_1 =
	    (s->order_hint && s->order_hint_n_bits) ? s->order_hint_n_bits - 1 : 0;
	pp->bit_depth_idx = s->hbd;
	pp->matrix_coefficients = (uint8_t)s->mtrx;

	pp->seq_info_fields.fields.still_picture = s->still_picture;
	pp->seq_info_fields.fields.use_128x128_superblock = s->sb128;
	pp->seq_info_fields.fields.enable_filter_intra = s->filter_intra;
	pp->seq_info_fields.fields.enable_intra_edge_filter = s->intra_edge_filter;
	pp->seq_info_fields.fields.enable_interintra_compound = s->inter_intra;
	pp->seq_info_fields.fields.enable_masked_compound = s->masked_compound;
	pp->seq_info_fields.fields.enable_dual_filter = s->dual_filter;
	pp->seq_info_fields.fields.enable_order_hint = s->order_hint;
	pp->seq_info_fields.fields.enable_jnt_comp = s->jnt_comp;
	pp->seq_info_fields.fields.enable_cdef = s->cdef;
	pp->seq_info_fields.fields.mono_chrome = s->monochrome;
	pp->seq_info_fields.fields.color_range = s->color_range;
	pp->seq_info_fields.fields.subsampling_x = s->ss_hor;
	pp->seq_info_fields.fields.subsampling_y = s->ss_ver;
	pp->seq_info_fields.fields.film_grain_params_present = s->film_grain_present;

	pp->current_frame = dec->surfaces[cur];
	pp->current_display_picture = dec->surfaces[cur];
	/* frame_width_minus_1 of the spec: the upscaled width */
	pp->frame_width_minus1 = (uint16_t)(h->width[1] - 1);
	pp->frame_height_minus1 = (uint16_t)(h->height - 1);

	/* a shown key frame refreshes every slot: it refers to none */
	const BOOL shownKey = (h->frame_type == DAV1D_FRAME_TYPE_KEY) && h->show_frame;
	for (int i = 0; i < DAV1D_NUM_REF_FRAMES; i++)
		pp->ref_frame_map[i] = shownKey ? VA_INVALID_SURFACE : slot_surface(dec, i);
	for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
		pp->ref_frame_idx[i] = (uint8_t)h->refidx[i];
	pp->primary_ref_frame = h->primary_ref_frame;
	pp->order_hint = h->frame_offset;

	pp->seg_info.segment_info_fields.bits.enabled = h->segmentation.enabled;
	pp->seg_info.segment_info_fields.bits.update_map = h->segmentation.update_map;
	pp->seg_info.segment_info_fields.bits.temporal_update = h->segmentation.temporal;
	pp->seg_info.segment_info_fields.bits.update_data = h->segmentation.update_data;
	if (h->segmentation.enabled)
	{
		for (size_t i = 0; i < DAV1D_MAX_SEGMENTS; i++)
		{
			const Dav1dSegmentationData* d = &h->segmentation.seg_data.d[i];
			pp->seg_info.feature_mask[i] = f->segFeatureMask[i];
			pp->seg_info.feature_data[i][0] = d->delta_q;
			pp->seg_info.feature_data[i][1] = d->delta_lf_y_v;
			pp->seg_info.feature_data[i][2] = d->delta_lf_y_h;
			pp->seg_info.feature_data[i][3] = d->delta_lf_u;
			pp->seg_info.feature_data[i][4] = d->delta_lf_v;
			pp->seg_info.feature_data[i][5] = (int16_t)(d->ref >= 0 ? d->ref : 0);
		}
	}
	/* film grain: refused in setup_sequence() */

	pp->tile_cols = (uint8_t)h->tiling.cols;
	pp->tile_rows = (uint8_t)h->tiling.rows;
	for (size_t i = 0; i < h->tiling.cols; i++)
		pp->width_in_sbs_minus_1[i] =
		    (uint16_t)(h->tiling.col_start_sb[i + 1] - h->tiling.col_start_sb[i] - 1);
	for (size_t i = 0; i < h->tiling.rows; i++)
		pp->height_in_sbs_minus_1[i] =
		    (uint16_t)(h->tiling.row_start_sb[i + 1] - h->tiling.row_start_sb[i] - 1);
	pp->tile_count_minus_1 = (uint16_t)(h->tiling.cols * h->tiling.rows - 1);
	pp->context_update_tile_id = h->tiling.update;

	pp->pic_info_fields.bits.frame_type = (uint32_t)h->frame_type;
	pp->pic_info_fields.bits.show_frame = h->show_frame;
	pp->pic_info_fields.bits.showable_frame = h->showable_frame;
	pp->pic_info_fields.bits.error_resilient_mode = h->error_resilient_mode;
	pp->pic_info_fields.bits.disable_cdf_update = h->disable_cdf_update;
	pp->pic_info_fields.bits.allow_screen_content_tools = h->allow_screen_content_tools;
	pp->pic_info_fields.bits.force_integer_mv = h->force_integer_mv;
	pp->pic_info_fields.bits.allow_intrabc = h->allow_intrabc;
	pp->pic_info_fields.bits.use_superres = h->super_res.enabled;
	pp->pic_info_fields.bits.allow_high_precision_mv = h->hp;
	pp->pic_info_fields.bits.is_motion_mode_switchable = h->switchable_motion_mode;
	pp->pic_info_fields.bits.use_ref_frame_mvs = h->use_ref_frame_mvs;
	pp->pic_info_fields.bits.disable_frame_end_update_cdf = !h->refresh_context;
	pp->pic_info_fields.bits.uniform_tile_spacing_flag = h->tiling.uniform;
	pp->pic_info_fields.bits.allow_warped_motion = h->warp_motion;

	pp->superres_scale_denominator =
	    h->super_res.enabled ? (uint8_t)h->super_res.width_scale_denominator : 8;
	pp->interp_filter = (uint8_t)h->subpel_filter_mode;

	pp->filter_level[0] = h->loopfilter.level_y[0];
	pp->filter_level[1] = h->loopfilter.level_y[1];
	pp->filter_level_u = h->loopfilter.level_u;
	pp->filter_level_v = h->loopfilter.level_v;
	pp->loop_filter_info_fields.bits.sharpness_level = h->loopfilter.sharpness;
	pp->loop_filter_info_fields.bits.mode_ref_delta_enabled = h->loopfilter.mode_ref_delta_enabled;
	pp->loop_filter_info_fields.bits.mode_ref_delta_update = h->loopfilter.mode_ref_delta_update;
	for (size_t i = 0; i < 8; i++)
		pp->ref_deltas[i] = h->loopfilter.mode_ref_deltas.ref_delta[i];
	for (size_t i = 0; i < 2; i++)
		pp->mode_deltas[i] = h->loopfilter.mode_ref_deltas.mode_delta[i];

	pp->base_qindex = h->quant.yac;
	pp->y_dc_delta_q = h->quant.ydc_delta;
	pp->u_dc_delta_q = h->quant.udc_delta;
	pp->u_ac_delta_q = h->quant.uac_delta;
	pp->v_dc_delta_q = h->quant.vdc_delta;
	pp->v_ac_delta_q = h->quant.vac_delta;
	pp->qmatrix_fields.bits.using_qmatrix = h->quant.qm;
	if (h->quant.qm)
	{
		pp->qmatrix_fields.bits.qm_y = h->quant.qm_y;
		pp->qmatrix_fields.bits.qm_u = h->quant.qm_u;
		pp->qmatrix_fields.bits.qm_v = h->quant.qm_v;
	}

	pp->mode_control_fields.bits.delta_q_present_flag = h->delta.q.present;
	pp->mode_control_fields.bits.log2_delta_q_res = h->delta.q.res_log2;
	pp->mode_control_fields.bits.delta_lf_present_flag = h->delta.lf.present;
	pp->mode_control_fields.bits.log2_delta_lf_res = h->delta.lf.res_log2;
	pp->mode_control_fields.bits.delta_lf_multi = h->delta.lf.multi;
	pp->mode_control_fields.bits.tx_mode = (uint32_t)h->txfm_mode;
	pp->mode_control_fields.bits.reference_select = h->switchable_comp_refs;
	pp->mode_control_fields.bits.reduced_tx_set_used = h->reduced_txtp_set;
	pp->mode_control_fields.bits.skip_mode_present = h->skip_mode_enabled;

	pp->cdef_damping_minus_3 = h->cdef.damping ? (uint8_t)(h->cdef.damping - 3) : 0;
	pp->cdef_bits = h->cdef.n_bits;
	/* dav1d keeps the 6 coded bits (primary << 2 | secondary), as VA-API does */
	for (size_t i = 0; i < 8; i++)
	{
		pp->cdef_y_strengths[i] = h->cdef.y_strength[i];
		pp->cdef_uv_strengths[i] = h->cdef.uv_strength[i];
	}

	/* dav1d keeps the coded lr_type; VA-API wants FrameRestorationType (Remap_Lr_Type) */
	static const uint8_t remap_lr_type[4] = { 0, 3, 1, 2 };
	pp->loop_restoration_fields.bits.yframe_restoration_type =
	    remap_lr_type[h->restoration.type[0] & 3];
	pp->loop_restoration_fields.bits.cbframe_restoration_type =
	    remap_lr_type[h->restoration.type[1] & 3];
	pp->loop_restoration_fields.bits.crframe_restoration_type =
	    remap_lr_type[h->restoration.type[2] & 3];
	if (h->restoration.type[0] || h->restoration.type[1] || h->restoration.type[2])
	{
		/* dav1d keeps log2 of the unit sizes: 6 + lr_unit_shift, and that - lr_uv_shift */
		pp->loop_restoration_fields.bits.lr_unit_shift =
		    (uint16_t)(h->restoration.unit_size[0] - 6);
		pp->loop_restoration_fields.bits.lr_uv_shift =
		    (uint16_t)(h->restoration.unit_size[0] - h->restoration.unit_size[1]);
	}

	for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
	{
		pp->wm[i].wmtype = (VAAV1TransformationType)h->gmv[i].type;
		for (size_t j = 0; j < 6; j++)
			pp->wm[i].wmmat[j] = h->gmv[i].matrix[j];
		pp->wm[i].invalid = f->gmInvalid[i];
	}
}

static BOOL decode_frame(AV1_VAAPI* dec, const Dav1dSequenceHeader* seq, const BYTE* tu,
                         const AV1_OBU_EVENT* ev, int cur)
{
	VADecPictureParameterBufferAV1 pp = WINPR_C_ARRAY_INIT;
	fill_pic_params(dec, seq, ev->hdr, cur, &pp);

	VASliceParameterBufferAV1* tiles = calloc(ev->nTiles, sizeof(VASliceParameterBufferAV1));
	if (!tiles)
		return FALSE;
	size_t total = 0;
	for (UINT32 i = 0; i < ev->nTiles; i++)
	{
		tiles[i].slice_data_offset = (uint32_t)total;
		tiles[i].slice_data_size = ev->tiles[i].size;
		tiles[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
		tiles[i].tile_row = ev->tiles[i].row;
		tiles[i].tile_column = ev->tiles[i].column;
		total += ev->tiles[i].size;
	}
	BYTE* data = malloc(total ? total : 1);
	if (!data)
	{
		free(tiles);
		return FALSE;
	}
	for (UINT32 i = 0; i < ev->nTiles; i++)
		memcpy(&data[tiles[i].slice_data_offset], &tu[ev->tiles[i].offset], ev->tiles[i].size);

	BOOL rc = FALSE;
	VABufferID bufs[3] = { VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID };
	WLog_Print(dec->log, WLOG_TRACE,
	           "VA-API: frame type %d %dx%d into surface %d, %" PRIu32 " tiles, %" PRIuz " bytes",
	           ev->hdr->d.frame_type, ev->hdr->d.width[1], ev->hdr->d.height, cur, ev->nTiles,
	           total);
	VAStatus st = va.vaCreateBuffer(dec->dpy, dec->context, VAPictureParameterBufferType,
	                                sizeof(pp), 1, &pp, &bufs[0]);
	if (st == VA_STATUS_SUCCESS)
		st = va.vaCreateBuffer(dec->dpy, dec->context, VASliceParameterBufferType,
		                       sizeof(VASliceParameterBufferAV1), ev->nTiles, tiles, &bufs[1]);
	if (st == VA_STATUS_SUCCESS)
		st = va.vaCreateBuffer(dec->dpy, dec->context, VASliceDataBufferType, (unsigned)total, 1,
		                       data, &bufs[2]);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaCreateBuffer: %s", va.vaErrorStr(st));
		goto end;
	}

	st = va.vaBeginPicture(dec->dpy, dec->context, dec->surfaces[cur]);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaBeginPicture: %s", va.vaErrorStr(st));
		goto end;
	}
	st = va.vaRenderPicture(dec->dpy, dec->context, bufs, 3);
	if (st != VA_STATUS_SUCCESS)
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaRenderPicture: %s", va.vaErrorStr(st));
	/* always ended once begun */
	const VAStatus est = va.vaEndPicture(dec->dpy, dec->context);
	if (est != VA_STATUS_SUCCESS)
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaEndPicture: %s", va.vaErrorStr(est));
	rc = (st == VA_STATUS_SUCCESS) && (est == VA_STATUS_SUCCESS);

end:
	/* the buffers are the caller's (libva 2): freed after the picture */
	for (size_t i = 0; i < ARRAYSIZE(bufs); i++)
	{
		if (bufs[i] != VA_INVALID_ID)
			(void)va.vaDestroyBuffer(dec->dpy, bufs[i]);
	}
	free(data);
	free(tiles);
	return rc;
}

/* surface @p s to system memory: Y in the mapped image, U and V separated */
static BOOL copy_back(AV1_VAAPI* dec, int s, UINT32 w, UINT32 h, AV1_VAAPI_PICTURE* pic)
{
	const UINT32 w2 = (w + 1) & ~1u;
	const UINT32 h2 = (h + 1) & ~1u;
	if (!dec->haveImage || (dec->image.width != w2) || (dec->image.height != h2))
	{
		free_image(dec);
		VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12,
			                  .byte_order = VA_LSB_FIRST,
			                  .bits_per_pixel = 12 };
		const VAStatus st = va.vaCreateImage(dec->dpy, &fmt, (int)w2, (int)h2, &dec->image);
		if (st != VA_STATUS_SUCCESS)
		{
			WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaCreateImage: %s", va.vaErrorStr(st));
			return FALSE;
		}
		dec->haveImage = TRUE;
	}

	WLog_Print(dec->log, WLOG_TRACE, "VA-API: copy back surface %d %" PRIu32 "x%" PRIu32, s, w, h);
	VAStatus st = va.vaSyncSurface(dec->dpy, dec->surfaces[s]);
	if (st == VA_STATUS_SUCCESS)
		st = va.vaGetImage(dec->dpy, dec->surfaces[s], 0, 0, w2, h2, dec->image.image_id);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaGetImage: %s", va.vaErrorStr(st));
		return FALSE;
	}
	void* p = nullptr;
	st = va.vaMapBuffer(dec->dpy, dec->image.buf, &p);
	if (st != VA_STATUS_SUCCESS)
	{
		WLog_Print(dec->log, WLOG_ERROR, "VA-API: vaMapBuffer: %s", va.vaErrorStr(st));
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
	const BYTE* base = p;
	const BYTE* y = &base[dec->image.offsets[0]];
	const BYTE* uv = &base[dec->image.offsets[1]];
	for (UINT32 r = 0; r < ch; r++)
	{
		const BYTE* src = &uv[(size_t)dec->image.pitches[1] * r];
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
	pic->strides[0] = dec->image.pitches[0];
	pic->strides[1] = pic->strides[2] = cw;
	pic->width = w;
	pic->height = h;
	return TRUE;
}

INT32 av1_vaapi_decode(AV1_VAAPI* dec, const BYTE* pSrcData, UINT32 SrcSize, AV1_VAAPI_PICTURE* pic)
{
	WINPR_ASSERT(dec);
	WINPR_ASSERT(pic);

	unmap_image(dec);
	if (!pSrcData || (SrcSize == 0))
		return AV1_VAAPI_INVALID;

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
			WLog_Print(dec->log, WLOG_WARN, "VA-API: invalid AV1 temporal unit (%" PRIu32 " bytes)",
			           SrcSize);
			return submitted ? -1 : AV1_VAAPI_INVALID;
		}
		if (rc == 0)
			break;

		const Dav1dSequenceHeader* seq = av1_obu_parser_sequence(dec->parser);
		WINPR_ASSERT(seq);
		if (ev.newSequence || (dec->context == VA_INVALID_ID))
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
				return submitted ? -3 : AV1_VAAPI_INVALID;
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
			return submitted ? -4 : AV1_VAAPI_INVALID;
		/* an inter frame needs its references (none after a reset or a lost key frame) */
		if (h->frame_type & 1)
		{
			for (size_t i = 0; i < DAV1D_REFS_PER_FRAME; i++)
			{
				if (slot_surface(dec, h->refidx[i]) == VA_INVALID_SURFACE)
					return submitted ? -5 : AV1_VAAPI_INVALID;
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
