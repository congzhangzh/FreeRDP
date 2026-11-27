/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: a client's H.264 (Annex B) for VideoToolbox (AVCC)
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_H264_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_H264_H

/* An MS-RDPECAM H.264 sample is an Annex B byte stream (start codes; a key frame carries its
 * SPS and PPS), as Media Foundation and the FreeRDP clients send it. VideoToolbox wants the
 * parameter sets in a CMVideoFormatDescription and the other NAL units each behind a 4-byte
 * big-endian length (AVCC). Plain C, so it is tested on any CI machine. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CAM_H264_NAL_IDR 5
#define CAM_H264_NAL_SPS 7
#define CAM_H264_NAL_PPS 8

	typedef struct
	{
		const uint8_t* sps; /* the last SPS in the sample (no start code), or NULL */
		size_t spsSize;
		const uint8_t* pps; /* the last PPS, or NULL */
		size_t ppsSize;
		bool idr;           /* an IDR slice is in it */
		size_t nals;        /* NAL units in it */
		size_t avccSize;    /* what cam_h264_to_avcc writes (no SPS, PPS) */
	} cam_h264_info;

	/* finds the NAL units of an Annex B sample; false when it has none */
	bool cam_h264_scan(const uint8_t* data, size_t size, cam_h264_info* info);

	/* writes the sample's NAL units but the SPS and PPS (and access unit delimiters), each
	 * behind its length, to dst; the bytes written, 0 when dst is too small or there is no
	 * NAL unit left */
	size_t cam_h264_to_avcc(const uint8_t* data, size_t size, uint8_t* dst, size_t dstSize);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_H264_H */
