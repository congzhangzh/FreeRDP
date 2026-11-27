/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: MJPG with Windows' own MJPEG decoder (Media Foundation)
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_DECODE_MF_H
#define FREERDP_SERVER_SHADOW_CAMERA_DECODE_MF_H

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct s_camera_mf_mjpeg CameraMfMjpeg;

	/** Whether Windows has an MJPEG decoder MFT (Windows Server 2022 and 2025 do; the Windows
	 *  host has no FFmpeg). Loads mfplat.dll at run time. */
	WINPR_ATTR_NODISCARD BOOL camera_mf_mjpeg_available(void);

	/** A decoder of MJPG width x height to NV12 */
	WINPR_ATTR_NODISCARD CameraMfMjpeg* camera_mf_mjpeg_new(UINT32 width, UINT32 height);
	void camera_mf_mjpeg_free(CameraMfMjpeg* decoder);

	/** Decodes one JPEG into nv12 (tightly packed, width * height * 3 / 2 bytes); FALSE when
	 *  it gave no picture */
	WINPR_ATTR_NODISCARD BOOL camera_mf_mjpeg_decode(CameraMfMjpeg* decoder, const BYTE* jpeg,
	                                                 size_t size, BYTE* nv12);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_DECODE_MF_H */
