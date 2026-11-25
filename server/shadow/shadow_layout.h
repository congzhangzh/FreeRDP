/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The layout the shadow server shares: its monitors and the surface over them
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

#ifndef FREERDP_SERVER_SHADOW_LAYOUT_H
#define FREERDP_SERVER_SHADOW_LAYOUT_H

#include <winpr/wtypes.h>

#include <freerdp/api.h>
#include <freerdp/settings_types.h>

/* What a client's graphics depend on besides the size: the monitors (in the server's form, the
 * primary flagged), the part of them the surface covers, and whether every monitor is shared.
 * Two layouts with the same size may differ: a monitor moved, another primary, one more. */
typedef struct
{
	UINT32 count;
	MONITOR_DEF monitors[16];
	INT32 x;
	INT32 y;
	UINT32 width;
	UINT32 height;
	BOOL allMonitors;
} SHADOW_LAYOUT;

#ifdef __cplusplus
extern "C"
{
#endif

	/** Fills a layout; at most 16 monitors are kept */
	void shadow_layout_set(SHADOW_LAYOUT* layout, const MONITOR_DEF* monitors, UINT32 count,
	                       INT32 x, INT32 y, UINT32 width, UINT32 height, BOOL allMonitors);

	/** Whether two layouts are the same: count, every monitor's rectangle and flags, the
	 *  surface's place and size, and the sharing mode */
	WINPR_ATTR_NODISCARD BOOL shadow_layout_equal(const SHADOW_LAYOUT* a, const SHADOW_LAYOUT* b);

	/** Takes the new layout when it differs from the stored one, and then counts a new
	 *  version (never 0, which stands for none). Returns TRUE when it changed. */
	WINPR_ATTR_NODISCARD BOOL shadow_layout_update(SHADOW_LAYOUT* stored, UINT32* version,
	                                               const SHADOW_LAYOUT* now);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_LAYOUT_H */
