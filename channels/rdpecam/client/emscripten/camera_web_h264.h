/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, browser camera: WebCodecs H.264 to MS-RDPECAM samples
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

#ifndef FREERDP_CHANNEL_RDPECAM_CLIENT_WEB_H264_H
#define FREERDP_CHANNEL_RDPECAM_CLIENT_WEB_H264_H

#include <winpr/wtypes.h>

#include <freerdp/api.h>

/** @brief room for one encoded frame of width x height (a larger one is dropped); 0 for a size the
 * browser camera does not take */
WINPR_ATTR_NODISCARD FREERDP_LOCAL size_t cam_web_h264_capacity(UINT32 width, UINT32 height);

/** @brief the bit rate asked of the browser's encoder: 1 Mbit/s for 640x480 at 30 frames per
 * second, in proportion to the pixels per second, within 150 kbit/s and 4 Mbit/s */
WINPR_ATTR_NODISCARD FREERDP_LOCAL UINT32 cam_web_h264_bitrate(UINT32 width, UINT32 height,
                                                               UINT32 fpsNum, UINT32 fpsDen);

/** @brief the codec string for VideoEncoder: Constrained Baseline (no B-frames), the lowest level
 * that holds width x height at the frame rate */
WINPR_ATTR_NODISCARD FREERDP_LOCAL const char* cam_web_h264_codec(UINT32 width, UINT32 height,
                                                                  UINT32 fpsNum, UINT32 fpsDen);

/** @brief reads an AVCDecoderConfigurationRecord (avcC, the encoder's decoderConfig.description)
 *
 * @param avcc the record, len bytes
 * @param lengthSize receives the size of the NAL unit lengths in a chunk (1, 2 or 4)
 * @param params receives the SPS and PPS units, each after a 4-byte start code
 * @param paramsSize receives the bytes written to params
 * @return TRUE for a whole record with at least one SPS and one PPS that fit into paramsCapacity
 */
WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL cam_web_h264_parse_avcc(const BYTE* avcc, size_t len,
                                                                BYTE* lengthSize, BYTE* params,
                                                                size_t paramsCapacity,
                                                                size_t* paramsSize);

/** @brief turns a chunk of the encoder (NAL units, each after its length) into an Annex B sample
 * (each after a 4-byte start code), with the SPS and PPS before a key frame's units unless the
 * chunk carries its own
 *
 * @return TRUE and outSize; FALSE for a chunk whose lengths do not add up, an empty unit, a key
 * frame without parameter sets, or a sample larger than outCapacity
 */
WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL cam_web_h264_to_annexb(BYTE lengthSize, const BYTE* params,
                                                               size_t paramsSize, BOOL key,
                                                               const BYTE* chunk, size_t chunkSize,
                                                               BYTE* out, size_t outCapacity,
                                                               size_t* outSize);

#endif /* FREERDP_CHANNEL_RDPECAM_CLIENT_WEB_H264_H */
