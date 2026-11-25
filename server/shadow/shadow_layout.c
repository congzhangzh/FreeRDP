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

#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/string.h>

#include <freerdp/types.h>

#include "shadow_layout.h"

void shadow_layout_set(SHADOW_LAYOUT* layout, const MONITOR_DEF* monitors, UINT32 count, INT32 x,
                       INT32 y, UINT32 width, UINT32 height, BOOL allMonitors)
{
	WINPR_ASSERT(layout);
	WINPR_ASSERT(monitors || (count == 0));

	ZeroMemory(layout, sizeof(*layout));
	layout->count = MIN(count, (UINT32)ARRAYSIZE(layout->monitors));
	for (UINT32 index = 0; index < layout->count; index++)
		layout->monitors[index] = monitors[index];
	layout->x = x;
	layout->y = y;
	layout->width = width;
	layout->height = height;
	layout->allMonitors = allMonitors;
}

BOOL shadow_layout_equal(const SHADOW_LAYOUT* a, const SHADOW_LAYOUT* b)
{
	WINPR_ASSERT(a);
	WINPR_ASSERT(b);

	if ((a->count != b->count) || (a->x != b->x) || (a->y != b->y) || (a->width != b->width) ||
	    (a->height != b->height) || (!a->allMonitors != !b->allMonitors))
		return FALSE;

	for (UINT32 index = 0; index < a->count; index++)
	{
		const MONITOR_DEF* ma = &a->monitors[index];
		const MONITOR_DEF* mb = &b->monitors[index];
		if ((ma->left != mb->left) || (ma->top != mb->top) || (ma->right != mb->right) ||
		    (ma->bottom != mb->bottom) || (ma->flags != mb->flags))
			return FALSE;
	}
	return TRUE;
}

BOOL shadow_layout_update(SHADOW_LAYOUT* stored, UINT32* version, const SHADOW_LAYOUT* now)
{
	WINPR_ASSERT(stored);
	WINPR_ASSERT(version);
	WINPR_ASSERT(now);

	if ((*version != 0) && shadow_layout_equal(stored, now))
		return FALSE;

	*stored = *now;
	(*version)++;
	if (*version == 0)
		*version = 1;
	return TRUE;
}
