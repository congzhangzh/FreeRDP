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

#include <freerdp/config.h>

#include <freerdp/channels/rdpgfx.h>

#include "shadow_gfx_caps.h"

UINT32 shadow_gfx_hevc_confirm_flags(UINT32 offered, UINT32 confirm, BOOL hevc, BOOL hevc444)
{
	/* a flag the client did not offer is never confirmed: it would get HEVC it cannot decode */
	confirm &= ~(UINT32)(RDPGFX_CAPS_FLAG_HEVC_ENABLE | RDPGFX_CAPS_FLAG_HEVC_I444);
	if (!hevc || ((offered & RDPGFX_CAPS_FLAG_HEVC_ENABLE) == 0))
		return confirm;
	confirm |= RDPGFX_CAPS_FLAG_HEVC_ENABLE;
	if (hevc444 && ((offered & RDPGFX_CAPS_FLAG_HEVC_I444) != 0))
		confirm |= RDPGFX_CAPS_FLAG_HEVC_I444;
	return confirm;
}

BOOL shadow_gfx_hevc_used(UINT32 version, UINT32 confirm, BOOL hevc, BOOL hevc444)
{
	if (version < RDPGFX_CAPVERSION_112)
		return FALSE;

	/* HEVC is only active when AVC444 is not */
	if ((confirm & RDPGFX_CAPS_FLAG_AVC_DISABLED) == 0)
		return FALSE;

	/* HEVC must be requested */
	if ((confirm & RDPGFX_CAPS_FLAG_HEVC_ENABLE) == 0)
		return FALSE;

	if (version >= RDPGFX_CAPVERSION_115)
		return hevc || hevc444;
	return hevc;
}
