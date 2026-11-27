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

#include <string.h>

#include "camera_h264.h"

#define NAL_AUD 9

/* the start code (00 00 01) at or after *pos: the index after it, or size */
static size_t after_start_code(const uint8_t* data, size_t size, size_t pos)
{
	for (size_t i = pos; i + 2 < size; i++)
	{
		if ((data[i] == 0) && (data[i + 1] == 0) && (data[i + 2] == 1))
			return i + 3;
	}
	return size;
}

/* the next NAL unit from *pos (which is just after a start code): its bytes without the
 * zeros before the next start code (a 4-byte start code's first zero, trailing_zero_8bits) */
static bool next_nal(const uint8_t* data, size_t size, size_t* pos, const uint8_t** nal,
                     size_t* nalSize)
{
	while (*pos < size)
	{
		const size_t begin = *pos;
		const size_t next = after_start_code(data, size, begin);
		size_t end = (next < size) ? next - 3 : size;
		*pos = next;
		while ((end > begin) && (data[end - 1] == 0))
			end--;
		if (end > begin)
		{
			*nal = &data[begin];
			*nalSize = end - begin;
			return true;
		}
	}
	return false;
}

bool cam_h264_scan(const uint8_t* data, size_t size, cam_h264_info* info)
{
	if (!data || !info)
		return false;
	memset(info, 0, sizeof(*info));
	size_t pos = after_start_code(data, size, 0);
	const uint8_t* nal = NULL;
	size_t nalSize = 0;
	while (next_nal(data, size, &pos, &nal, &nalSize))
	{
		const uint8_t type = nal[0] & 0x1F;
		info->nals++;
		switch (type)
		{
			case CAM_H264_NAL_SPS:
				info->sps = nal;
				info->spsSize = nalSize;
				break;
			case CAM_H264_NAL_PPS:
				info->pps = nal;
				info->ppsSize = nalSize;
				break;
			case NAL_AUD:
				break;
			default:
				if (type == CAM_H264_NAL_IDR)
					info->idr = true;
				info->avccSize += 4 + nalSize;
				break;
		}
	}
	return info->nals > 0;
}

size_t cam_h264_to_avcc(const uint8_t* data, size_t size, uint8_t* dst, size_t dstSize)
{
	if (!data || !dst)
		return 0;
	size_t pos = after_start_code(data, size, 0);
	size_t written = 0;
	const uint8_t* nal = NULL;
	size_t nalSize = 0;
	while (next_nal(data, size, &pos, &nal, &nalSize))
	{
		const uint8_t type = nal[0] & 0x1F;
		if ((type == CAM_H264_NAL_SPS) || (type == CAM_H264_NAL_PPS) || (type == NAL_AUD))
			continue;
		if ((nalSize > UINT32_MAX) || (dstSize - written < 4 + nalSize))
			return 0;
		dst[written++] = (uint8_t)(nalSize >> 24);
		dst[written++] = (uint8_t)(nalSize >> 16);
		dst[written++] = (uint8_t)(nalSize >> 8);
		dst[written++] = (uint8_t)nalSize;
		memcpy(&dst[written], nal, nalSize);
		written += nalSize;
	}
	return written;
}
