/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The order the shadow server tries the GFX codecs in
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

#ifndef FREERDP_SERVER_SHADOW_CODEC_ORDER_H
#define FREERDP_SERVER_SHADOW_CODEC_ORDER_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>
#include <freerdp/server/shadow.h>

/* macOS encodes HEVC with VideoToolbox, in hardware: at the bit rate of AV1 it ran faster on a
 * third of the CPU (kit probe 46129375634). So there HEVC comes first of all: a client that offers
 * HEVC gets it, one that does not still gets AV2 or AV1. Every other host: AV2, AV1, then HEVC. */
#if defined(__APPLE__)
#define SHADOW_GFX_HEVC_FIRST TRUE
#else
#define SHADOW_GFX_HEVC_FIRST FALSE
#endif

#ifdef __cplusplus
extern "C"
{
#endif

	/** The order the GFX codecs are tried in when /gfx-codec-order ranks none of them:
	 *  AV2, AV1, HEVC, AVC444, AVC420, RFX, progressive, planar; HEVC first with hevcFirst.
	 *  @return the codecs written, 0 when size is too small for all of them */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL size_t
	shadow_gfx_codec_default_order(BOOL hevcFirst, SHADOW_GFX_CODEC* order, size_t size);

	/** The capability versions a client's caps are tried with, for a codec order: each codec's
	 *  versions at its first place (AV2 FRDP_2, AV1 FRDP_1, HEVC 11x, the others 10x), of the
	 *  codecs this build has. The first of them the client advertises is confirmed.
	 *  @return the versions written, at most size */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL size_t shadow_gfx_caps_versions(
	    const SHADOW_GFX_CODEC* order, size_t orderCount, UINT32* versions, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CODEC_ORDER_H */
