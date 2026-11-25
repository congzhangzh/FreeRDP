/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_SERVER_SHADOW_WIN_DXGI_H
#define FREERDP_SERVER_SHADOW_WIN_DXGI_H

/* WITH_DXGI_1_2 comes from -DWITH_SHADOW_WIN_DXGI=ON (Win/CMakeLists.txt) */

#ifdef WITH_DXGI_1_2

#ifndef CINTERFACE
#define CINTERFACE
#endif

#include <d3d11.h>
#include <dxgi1_2.h>

#endif

#include <freerdp/codec/region.h>

#include "win_shadow.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef WITH_DXGI_1_2

	WINPR_ATTR_NODISCARD int win_shadow_dxgi_init(winShadowSubsystem* subsystem);
	WINPR_ATTR_NODISCARD int win_shadow_dxgi_uninit(winShadowSubsystem* subsystem);
	WINPR_ATTR_NODISCARD int win_shadow_dxgi_restart_duplication(winShadowSubsystem* subsystem);

	/* Sets up the outputs to duplicate: every output whose picture shows in the server's
	 * surface, with its place there; drops the earlier ones. Returns their count, <0 on
	 * failure. */
	WINPR_ATTR_NODISCARD int win_shadow_dxgi_map_outputs(winShadowSubsystem* subsystem);

	/* Makes outputs[index] the one the other functions work on */
	void win_shadow_dxgi_select(winShadowSubsystem* subsystem, UINT32 index);

	/* Marks the current output's whole picture for the next copy */
	void win_shadow_dxgi_invalidate_output(winShadowSubsystem* subsystem);

	/* Whether an output holds a frame acquired in this pass */
	WINPR_ATTR_NODISCARD BOOL win_shadow_dxgi_frame_acquired(winShadowSubsystem* subsystem);

	/* Copies the damaged part of every output holding a frame into the server's surface, each
	 * at its place there. The surface's invalid region is then the damage copied (what the
	 * clients encode), pending the damage of the outputs without a frame now, for their next
	 * one. Returns 1 when something was copied, 0 when nothing was, <0 on failure. */
	WINPR_ATTR_NODISCARD int win_shadow_dxgi_copy_outputs(winShadowSubsystem* subsystem,
	                                                      REGION16* pending);

	WINPR_ATTR_NODISCARD int win_shadow_dxgi_fetch_frame_data(winShadowSubsystem* subsystem,
	                                                          BYTE** ppDstData, int* pnDstStep,
	                                                          int x, int y, int width, int height);

	WINPR_ATTR_NODISCARD int win_shadow_dxgi_get_next_frame(winShadowSubsystem* subsystem);
	WINPR_ATTR_NODISCARD int win_shadow_dxgi_get_invalid_region(winShadowSubsystem* subsystem);

#endif

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_WIN_DXGI_H */
