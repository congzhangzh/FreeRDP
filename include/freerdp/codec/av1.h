/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 Bitmap Compression
 *
 * Copyright 2026 Armin Novak <anovak@thincast.com>
 * Copyright 2026 Thincast Technologies GmbH
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

#pragma once

#include <winpr/wlog.h>

#include <freerdp/api.h>
#include <freerdp/types.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/decode_result.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct S_FREERDP_AV1_CONTEXT FREERDP_AV1_CONTEXT;

	typedef enum
	{
		FREERDP_AV1_VBR, /**< Variable Bit Rate (VBR) mode */
		FREERDP_AV1_CBR, /**< Constant Bit Rate (CBR) mode */
		FREERDP_AV1_CQ,  /**< Constrained Quality (CQ)  mode */
		FREERDP_AV1_Q    /**< Constant Quality (Q) mode */
	} FREERDP_AV1_RATECONTROL;

	typedef enum
	{
		FREERDP_AV1_CONTEXT_OPTION_PROFILE,
		FREERDP_AV1_CONTEXT_OPTION_RATECONTROL,
		FREERDP_AV1_CONTEXT_OPTION_BITRATE,
		FREERDP_AV1_CONTEXT_OPTION_USAGETYPE,
		FREERDP_AV1_CONTEXT_OPTION_CQ_LEVEL, /**< the quality of FREERDP_AV1_Q and FREERDP_AV1_CQ,
		                                          1 to 63 (lower is better); 0 leaves libaom's */
		FREERDP_AV1_CONTEXT_OPTION_THREADS,  /**< encoder threads, 0 for up to 4 */
		FREERDP_AV1_CONTEXT_OPTION_SPEED,    /**< libaom's cpu-used, 0 (slowest) to 12 */
		FREERDP_AV1_CONTEXT_OPTION_TILE_COLUMNS, /**< log2 of the tile columns, 0 (one) to 6;
		                                             the threads write the tiles' bitstreams
		                                             side by side */
		FREERDP_AV1_CONTEXT_OPTION_BACKEND       /**< FREERDP_AV1_BACKEND_*, encoder and decoder;
		                                              from the next freerdp_av1_context_reset() */
	} FREERDP_AV1_CONTEXT_OPTION;

	/** Which AV1 implementation goes first (FREERDP_AV1_CONTEXT_OPTION_BACKEND). */
	typedef enum
	{
		FREERDP_AV1_BACKEND_HARDWARE = 0, /**< the GPU's when it can, software as fallback */
		FREERDP_AV1_BACKEND_SOFTWARE = 1  /**< software; the GPU's only when no software codec
		                                       is built in */
	} FREERDP_AV1_BACKEND;

/** /codec-first, the same on the client (decoding) and the shadow server (encoding):
 *  "hardware" (the default): the GPU's codec when one takes the stream, software as fallback;
 *  "hardware:<id>[,<id>...]": the devices of /codec-list in this order, then software;
 *  "software": the software codec (the GPU's only when none is built in). */
#define FREERDP_CODEC_FIRST_MAX_DEVICES 8

	/** @brief whether @p value is a /codec-first value
	 *  @since version 3.34.0 */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_codec_first_valid(const char* value);

	/** @brief applies a /codec-first value (nullptr: "hardware"), from the next
	 *  freerdp_av1_context_reset()
	 *  @since version 3.34.0 */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_set_codec_first(FREERDP_AV1_CONTEXT* av1,
	                                                     const char* value);

	/** @brief frees a list of freerdp_av1_hardware_list() @since version 3.34.0 */
	FREERDP_API void freerdp_av1_hardware_list_free(char** list, size_t count);

	/** @brief this machine's hardware AV1 decoders or encoders (/codec-list), one line each:
	 *  "<id> <interface> <device> (AV1 4:2:0 decode|encode)"; the ids are the ones of
	 *  /codec-first:hardware:<id>[,<id>...]
	 *  @return the lines (none: an empty list) or \b nullptr on failure, free with
	 *          freerdp_av1_hardware_list_free()
	 *  @since version 3.34.0 */
	WINPR_ATTR_MALLOC(freerdp_av1_hardware_list_free, 1)
	FREERDP_API char** freerdp_av1_hardware_list(BOOL encoder, size_t* count);

	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_set_option(FREERDP_AV1_CONTEXT* av1,
	                                                FREERDP_AV1_CONTEXT_OPTION option,
	                                                UINT32 value);

	WINPR_ATTR_NODISCARD
	FREERDP_API UINT32 freerdp_av1_context_get_option(FREERDP_AV1_CONTEXT* av1,
	                                                  FREERDP_AV1_CONTEXT_OPTION option);

	WINPR_ATTR_NODISCARD
	FREERDP_API INT32 freerdp_av1_compress(FREERDP_AV1_CONTEXT* freerdp_av1, const BYTE* pSrcData,
	                                       DWORD SrcFormat, UINT32 nSrcStep, UINT32 nSrcWidth,
	                                       UINT32 nSrcHeight, const RECTANGLE_16* regionRect,
	                                       BYTE** ppDstData, UINT32* pDstSize,
	                                       RDPGFX_H264_METABLOCK* meta);

	WINPR_ATTR_NODISCARD
	FREERDP_API INT32 freerdp_av1_decompress(FREERDP_AV1_CONTEXT* freerdp_av1, const BYTE* pSrcData,
	                                         UINT32 SrcSize, BYTE* pDstData, DWORD DstFormat,
	                                         UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight,
	                                         const RECTANGLE_16* regionRects, UINT32 numRegionRect);

	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_reset(FREERDP_AV1_CONTEXT* av1, UINT32 width,
	                                           UINT32 height);

	/** @brief Get the result of the last freerdp_av1_decompress()
	 *
	 *  @param av1 The decoder context
	 *  @param result Where to store the result
	 *
	 *  @return \b TRUE if the result was stored, \b FALSE for invalid arguments
	 *  @since version 3.34.0
	 */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL freerdp_av1_context_get_last_decode(const FREERDP_AV1_CONTEXT* av1,
	                                                     FREERDP_DECODE_RESULT* result);

	FREERDP_API void freerdp_av1_context_free(FREERDP_AV1_CONTEXT* av1);

	/** @brief create a new encoder/decoder instance for AV1 streams
	 *
	 *  @param Compressor \b TRUE if an encoder should be created, \b FALSE for a decoder
	 *
	 *  @return An allocated instance or \b nullptr if failed.
	 *  @since version 3.25.0
	 */
	WINPR_ATTR_MALLOC(freerdp_av1_context_free, 1)
	FREERDP_API FREERDP_AV1_CONTEXT* freerdp_av1_context_new(BOOL Compressor);

#ifdef __cplusplus
}
#endif
