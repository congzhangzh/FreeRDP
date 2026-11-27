/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a Windows virtual camera ("FreeRDP Camera") as the camera
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_SINK_WIN_H
#define FREERDP_SERVER_SHADOW_CAMERA_SINK_WIN_H

#include "../camera_sink.h"

#ifdef __cplusplus
extern "C"
{
#endif

	/** Registers the virtual camera "FreeRDP Camera" (MFCreateVirtualCamera) fed with NV12
	 *  width x height at fps through the frame ring (camera_ring.h), its media source
	 *  freerdp-shadow-camera-source.dll taken from beside the program. Below Windows build
	 *  22000 it logs exactly "camera: needs Windows 11 or Windows Server 2025 (build 22000 or
	 *  later); this system is build <N>, so no virtual camera" and returns nullptr, as it
	 *  does (with a line saying why) whenever there can be no camera. Needs SYSTEM or an
	 *  administrator. Its readers are the media source's running streams. */
	WINPR_ATTR_NODISCARD ShadowCameraSink* shadow_camera_sink_win_new(UINT32 width, UINT32 height,
	                                                                  UINT32 fps);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_SINK_WIN_H */
