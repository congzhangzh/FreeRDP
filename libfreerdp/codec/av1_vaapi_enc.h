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

#ifndef FREERDP_LIB_CODEC_AV1_VAAPI_ENC_H
#define FREERDP_LIB_CODEC_AV1_VAAPI_ENC_H

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

/** A GPU's AV1 encoder through VA-API (Intel through intel-media or Mesa, AMD through Mesa, a
 * GPU under WSL through Mesa's d3d12): 8 bit 4:2:0 in, AV1 temporal units in the low overhead
 * bitstream format out, one per frame. One key frame (with the sequence header) starts the
 * stream; every other frame refers to the one before it. libva is loaded at run time. */
typedef struct S_AV1_VAAPI_ENC AV1_VAAPI_ENC;

/** The render nodes with an AV1 encoder (the devices), in /dev/dri order: their names in
 * @p names. @return how many there are, at most @p max */
size_t av1_vaapi_enc_list(char names[][256], size_t max);

typedef enum
{
	AV1_VAAPI_ENC_CBR, /**< constant bit rate, kbps */
	AV1_VAAPI_ENC_VBR, /**< variable bit rate around kbps */
	AV1_VAAPI_ENC_CQP  /**< constant quantizer: qindex, 1 to 255 */
} AV1_VAAPI_ENC_RATE;

/** @param device an id of av1_vaapi_enc_list(), < 0: the first
 *  @return the encoder, or nullptr when there is no such device or it refuses this size or rate
 *          (the reason is logged) */
AV1_VAAPI_ENC* av1_vaapi_enc_new(wLog* log, UINT32 width, UINT32 height, AV1_VAAPI_ENC_RATE rate,
                                 UINT32 kbps, UINT32 qindex, INT32 device);

/** The encoder's device id (of av1_vaapi_enc_list()). */
INT32 av1_vaapi_enc_device(const AV1_VAAPI_ENC* enc);
void av1_vaapi_enc_free(AV1_VAAPI_ENC* enc);

/** The device's name (UTF-8), for the log. */
const char* av1_vaapi_enc_name(const AV1_VAAPI_ENC* enc);

/** Encode one frame (I420), shown @p durationMs after the last one.
 *
 * @return 1 and @p ppData / @p pSize set (valid until the next call) for a temporal unit,
 *         or < 0 when the encoder failed: it is then of no further use. */
INT32 av1_vaapi_enc_encode(AV1_VAAPI_ENC* enc, const BYTE* const planes[3], const UINT32 strides[3],
                           UINT32 durationMs, const BYTE** ppData, UINT32* pSize);

#endif /* FREERDP_LIB_CODEC_AV1_VAAPI_ENC_H */
