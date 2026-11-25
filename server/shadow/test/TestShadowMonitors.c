/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * shadow_monitors_normalize: the monitors a subsystem found, in the server's form
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

#include <stdio.h>

#include <freerdp/settings_types.h>
#include <freerdp/server/shadow.h>

/* a monitor of width x height at x,y, as left/top/right/bottom (inclusive) */
static MONITOR_DEF mon(INT32 x, INT32 y, INT32 width, INT32 height, UINT32 flags)
{
	const MONITOR_DEF m = {
		.left = x, .top = y, .right = x + width - 1, .bottom = y + height - 1, .flags = flags
	};
	return m;
}

static BOOL same(const char* what, const MONITOR_DEF* got, const MONITOR_DEF* want, UINT32 count)
{
	for (UINT32 i = 0; i < count; i++)
	{
		if ((got[i].left != want[i].left) || (got[i].top != want[i].top) ||
		    (got[i].right != want[i].right) || (got[i].bottom != want[i].bottom) ||
		    (got[i].flags != want[i].flags))
		{
			(void)fprintf(stderr,
			              "%s: monitor %" PRIu32 " is %" PRId32 ",%" PRId32 "-%" PRId32 ",%" PRId32
			              " flags %" PRIu32 ", expected %" PRId32 ",%" PRId32 "-%" PRId32
			              ",%" PRId32 " flags %" PRIu32 "\n",
			              what, i, got[i].left, got[i].top, got[i].right, got[i].bottom,
			              got[i].flags, want[i].left, want[i].top, want[i].right, want[i].bottom,
			              want[i].flags);
			return FALSE;
		}
	}
	return TRUE;
}

int TestShadowMonitors(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	/* a monitor left of the primary: Windows gives it a negative left */
	{
		MONITOR_DEF m[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY), mon(-1600, 0, 1600, 900, 0) };
		const MONITOR_DEF want[] = { mon(1600, 0, 1920, 1080, MONITOR_PRIMARY),
			                         mon(0, 0, 1600, 900, 0) };
		if (!shadow_monitors_normalize(m, ARRAYSIZE(m)) || !same("left", m, want, ARRAYSIZE(m)))
			return -1;
	}
	/* a monitor above the primary */
	{
		MONITOR_DEF m[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY), mon(160, -900, 1600, 900, 0) };
		const MONITOR_DEF want[] = { mon(0, 900, 1920, 1080, MONITOR_PRIMARY),
			                         mon(160, 0, 1600, 900, 0) };
		if (!shadow_monitors_normalize(m, ARRAYSIZE(m)) || !same("above", m, want, ARRAYSIZE(m)))
			return -1;
	}
	/* an L shape: positions kept, the gap belongs to no monitor */
	{
		MONITOR_DEF m[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY), mon(1920, 500, 1280, 1024, 0) };
		const MONITOR_DEF want[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
			                         mon(1920, 500, 1280, 1024, 0) };
		if (!shadow_monitors_normalize(m, ARRAYSIZE(m)) || !same("L", m, want, ARRAYSIZE(m)))
			return -1;
	}
	/* the primary not first: it moves to index 0, the others keep their order */
	{
		MONITOR_DEF m[] = { mon(-1280, 0, 1280, 1024, 0), mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
			                mon(1920, 0, 2560, 1440, 0) };
		const MONITOR_DEF want[] = { mon(1280, 0, 1920, 1080, MONITOR_PRIMARY),
			                         mon(0, 0, 1280, 1024, 0), mon(3200, 0, 2560, 1440, 0) };
		if (!shadow_monitors_normalize(m, ARRAYSIZE(m)) ||
		    !same("primary not first", m, want, ARRAYSIZE(m)))
			return -1;
	}
	/* no primary flag: the first is the primary, a second flag is dropped */
	{
		MONITOR_DEF m[] = { mon(100, 50, 800, 600, 0), mon(900, 50, 800, 600, 0) };
		const MONITOR_DEF want[] = { mon(0, 0, 800, 600, MONITOR_PRIMARY), mon(800, 0, 800, 600, 0) };
		if (!shadow_monitors_normalize(m, ARRAYSIZE(m)) || !same("no flag", m, want, ARRAYSIZE(m)))
			return -1;
	}
	/* refused: none, an empty one, a box too wide for a surface */
	{
		MONITOR_DEF empty[] = { mon(0, 0, 800, 600, MONITOR_PRIMARY), { 10, 10, 5, 20, 0 } };
		MONITOR_DEF wide[] = { mon(0, 0, 40000, 600, MONITOR_PRIMARY), mon(40000, 0, 40000, 600, 0) };
		if (shadow_monitors_normalize(empty, 0) || shadow_monitors_normalize(nullptr, 1) ||
		    shadow_monitors_normalize(empty, ARRAYSIZE(empty)) ||
		    shadow_monitors_normalize(wide, ARRAYSIZE(wide)))
		{
			(void)fprintf(stderr, "a monitor list that cannot be a surface was accepted\n");
			return -1;
		}
	}
	return 0;
}
