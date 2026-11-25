/**
 * FreeRDP: A Remote Desktop Protocol Implementation
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

#ifndef FREERDP_SERVER_SHADOW_GFX_CAPS_H
#define FREERDP_SERVER_SHADOW_GFX_CAPS_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>

/** @brief The HEVC flags of a GFX caps confirm
 *
 *  RDPGFX_CAPS_FLAG_HEVC_ENABLE only when the client offered it in this capability set and the
 *  server takes HEVC (on, with an encoder); RDPGFX_CAPS_FLAG_HEVC_I444 the same way, with HEVC.
 *
 *  @param offered The flags of the client's capability set
 *  @param confirm The flags the confirm has so far
 *  @param hevc The server takes HEVC 4:2:0
 *  @param hevc444 The server takes HEVC 4:4:4
 *  @return confirm with its HEVC flags set or cleared
 */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL UINT32 shadow_gfx_hevc_confirm_flags(UINT32 offered, UINT32 confirm, BOOL hevc,
                                                   BOOL hevc444);

/** @brief Whether the server sends HEVC to a client
 *
 *  HEVC needs capability version 11.2 or later and a confirm with RDPGFX_CAPS_FLAG_HEVC_ENABLE
 *  and RDPGFX_CAPS_FLAG_AVC_DISABLED (HEVC is only used when AVC444 is not); 4:4:4 alone needs
 *  version 11.5.
 *
 *  @param version The confirmed capability version
 *  @param confirm The confirmed flags
 *  @param hevc The client's HEVC 4:2:0 is on
 *  @param hevc444 The client's HEVC 4:4:4 is on
 *  @return TRUE when the frames go out as HEVC
 */
WINPR_ATTR_NODISCARD
FREERDP_LOCAL BOOL shadow_gfx_hevc_used(UINT32 version, UINT32 confirm, BOOL hevc, BOOL hevc444);

#endif /* FREERDP_SERVER_SHADOW_GFX_CAPS_H */
