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

#ifndef FREERDP_LIB_CODEC_AV1_VAAPI_H
#define FREERDP_LIB_CODEC_AV1_VAAPI_H

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

/** One decode path for every GPU vendor on Linux (AMD and Intel through Mesa or intel-media,
 * NVIDIA through nvidia-vaapi-driver): the driver's VA-API AV1 decoder (profile 0: 8 bit
 * 4:2:0) on a DRM render node, the headers parsed by av1_obu.c. libva is loaded at run time:
 * without it (or a driver) there is no device and the software decoder decodes. */
typedef struct S_AV1_VAAPI AV1_VAAPI;

/** A decoded picture: 8 bit planar 4:2:0, valid until the next call on the decoder. */
typedef struct
{
	const BYTE* planes[3];
	UINT32 strides[3];
	UINT32 width;
	UINT32 height;
} AV1_VAAPI_PICTURE;

/** The stream (its sequence header) is one this GPU can't decode: use software. */
#define AV1_VAAPI_UNSUPPORTED (-100)
/** The input is no valid temporal unit (or comes before any sequence header): refused, the
 * decoder keeps the stream. */
#define AV1_VAAPI_INVALID (-101)

/** Any device: the first render node with an AV1 decoder. */
#define AV1_VAAPI_ANY_DEVICE (-1)

/** The render nodes with an AV1 decoder (the devices), in /dev/dri order: their names in
 * @p names. @return how many there are, at most @p max */
size_t av1_vaapi_list(char names[][256], size_t max);

/** @return a decoder on device @p device (an id of av1_vaapi_list(), or AV1_VAAPI_ANY_DEVICE),
 *          or nullptr when there is no such device. */
AV1_VAAPI* av1_vaapi_new(wLog* log, INT32 device);

/** The decoder's device id (of av1_vaapi_list()). */
INT32 av1_vaapi_device(const AV1_VAAPI* dec);
void av1_vaapi_free(AV1_VAAPI* dec);

/** The device's name (UTF-8), for the log. */
const char* av1_vaapi_name(const AV1_VAAPI* dec);

/** Forget the stream: the next temporal unit must start a new one (sequence header). */
void av1_vaapi_reset(AV1_VAAPI* dec);

/** Decode one temporal unit (low overhead bitstream format).
 *
 * @return TRUE and @p pic filled when a frame was shown, FALSE when none was,
 *         AV1_VAAPI_UNSUPPORTED when the GPU can't decode this stream (no frame of it was
 *         decoded, so a software decoder can start with the same temporal unit),
 *         AV1_VAAPI_INVALID for bad input, or another negative value when the GPU failed
 *         in the middle of the stream.
 */
INT32 av1_vaapi_decode(AV1_VAAPI* dec, const BYTE* pSrcData, UINT32 SrcSize,
                       AV1_VAAPI_PICTURE* pic);

#endif /* FREERDP_LIB_CODEC_AV1_VAAPI_H */
