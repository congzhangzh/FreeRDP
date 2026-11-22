/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 encoding with a Media Foundation hardware encoder
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

#ifndef FREERDP_LIB_CODEC_AV1_MF_H
#define FREERDP_LIB_CODEC_AV1_MF_H

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

/** A GPU's AV1 encoder behind Media Foundation's vendor-neutral interface (an asynchronous
 * hardware encoder MFT, e.g. "NVIDIA AV1 Encoder MFT"): 8 bit 4:2:0 in, AV1 temporal units in
 * the low overhead bitstream format out, one per frame, a sequence header in every key frame. */
typedef struct S_AV1_MF AV1_MF;

/** The hardware AV1 encoder MFTs (the devices), in Media Foundation's order: their names in
 * @p names. @return how many there are, at most @p max */
size_t av1_mf_list(char names[][256], size_t max);

/** @param cbr TRUE: constant bit rate, FALSE: a low delay variable one
 *  @param device an id of av1_mf_list(), < 0: the first
 *  @return the encoder, or nullptr when there is no such hardware AV1 encoder or it refuses
 *          this size or rate (the reason is logged) */
AV1_MF* av1_mf_new(wLog* log, UINT32 width, UINT32 height, UINT32 kbps, BOOL cbr, INT32 device);

/** The encoder's device id (of av1_mf_list()). */
INT32 av1_mf_device(const AV1_MF* enc);
void av1_mf_free(AV1_MF* enc);

/** The encoder's name (UTF-8), for the log. */
const char* av1_mf_name(const AV1_MF* enc);

/** Encode one frame (I420), shown @p durationMs after the last one.
 *
 * @return 1 and @p ppData / @p pSize set (valid until the next call) for a temporal unit,
 *         or < 0 when the encoder failed: it is then of no further use. */
INT32 av1_mf_encode(AV1_MF* enc, const BYTE* const planes[3], const UINT32 strides[3],
                    UINT32 durationMs, BOOL keyFrame, const BYTE** ppData, UINT32* pSize);

#endif /* FREERDP_LIB_CODEC_AV1_MF_H */
