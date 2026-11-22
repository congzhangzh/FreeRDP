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

#ifndef FREERDP_LIB_CODEC_AV1_D3D11VA_H
#define FREERDP_LIB_CODEC_AV1_D3D11VA_H

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

/** One decode path for every GPU vendor (NVIDIA, AMD, Intel): the driver's DXVA AV1 decoder
 * (profile 0: 8 bit 4:2:0) through ID3D11VideoDecoder, the headers parsed by av1_obu.c. */
typedef struct S_AV1_D3D11VA AV1_D3D11VA;

/** A decoded picture: 8 bit planar 4:2:0, valid until the next call on the decoder. */
typedef struct
{
	const BYTE* planes[3];
	UINT32 strides[3];
	UINT32 width;
	UINT32 height;
} AV1_D3D11VA_PICTURE;

/** The stream (its sequence header) is one this GPU can't decode: use software. */
#define AV1_D3D11VA_UNSUPPORTED (-100)
/** The input is no valid temporal unit (or comes before any sequence header): refused, the
 * decoder keeps the stream. */
#define AV1_D3D11VA_INVALID (-101)

/** Any adapter: the first hardware adapter with an AV1 decoder, or the one the environment
 * variable FREERDP_AV1_D3D11VA_ADAPTER (a DXGI adapter index) names. */
#define AV1_D3D11VA_ANY_ADAPTER (-1)

/** @return a decoder on the DXGI adapter @p adapter (an index, or AV1_D3D11VA_ANY_ADAPTER),
 *          or nullptr when it has no AV1 decoder. */
AV1_D3D11VA* av1_d3d11va_new(wLog* log, INT32 adapter);
void av1_d3d11va_free(AV1_D3D11VA* dec);

/** The adapter's name (UTF-8), for the log. */
const char* av1_d3d11va_adapter_name(const AV1_D3D11VA* dec);

/** Forget the stream: the next temporal unit must start a new one (sequence header). */
void av1_d3d11va_reset(AV1_D3D11VA* dec);

/** Decode one temporal unit (low overhead bitstream format).
 *
 * @return TRUE and @p pic filled when a frame was shown, FALSE when none was,
 *         AV1_D3D11VA_UNSUPPORTED when the GPU can't decode this stream (no frame of it was
 *         decoded, so a software decoder can start with the same temporal unit),
 *         AV1_D3D11VA_INVALID for bad input, or another negative value when the GPU failed
 *         in the middle of the stream.
 */
INT32 av1_d3d11va_decode(AV1_D3D11VA* dec, const BYTE* pSrcData, UINT32 SrcSize,
                         AV1_D3D11VA_PICTURE* pic);

#endif /* FREERDP_LIB_CODEC_AV1_D3D11VA_H */
