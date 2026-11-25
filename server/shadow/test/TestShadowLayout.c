/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The shared layout: a change of the monitors is noticed when the size stays
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

#include "../shadow_layout.h"

/* a monitor of width x height at x,y, as left/top/right/bottom (inclusive) */
static MONITOR_DEF mon(INT32 x, INT32 y, INT32 width, INT32 height, UINT32 flags)
{
	const MONITOR_DEF m = {
		.left = x, .top = y, .right = x + width - 1, .bottom = y + height - 1, .flags = flags
	};
	return m;
}

/* the bounding box of every monitor, all of them shared */
static void all(SHADOW_LAYOUT* layout, const MONITOR_DEF* monitors, UINT32 count)
{
	INT32 right = 0;
	INT32 bottom = 0;
	for (UINT32 i = 0; i < count; i++)
	{
		if (monitors[i].right + 1 > right)
			right = monitors[i].right + 1;
		if (monitors[i].bottom + 1 > bottom)
			bottom = monitors[i].bottom + 1;
	}
	shadow_layout_set(layout, monitors, count, 0, 0, (UINT32)right, (UINT32)bottom, TRUE);
}

static BOOL expect(const char* what, BOOL got, BOOL want)
{
	if (got == want)
		return TRUE;
	(void)fprintf(stderr, "%s: got %s, want %s\n", what, got ? "TRUE" : "FALSE",
	              want ? "TRUE" : "FALSE");
	return FALSE;
}

/* every case keeps the bounding box at 3200x1080: only the layout differs */
static BOOL test_same_size_changes(void)
{
	BOOL rc = TRUE;
	SHADOW_LAYOUT base = { 0 };
	SHADOW_LAYOUT other = { 0 };

	/* 1920x1080 primary on the left, a 1280x1024 monitor right of it, at the top */
	const MONITOR_DEF two[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
		                        mon(1920, 0, 1280, 1024, 0) };
	all(&base, two, 2);

	/* the same list again: no change */
	all(&other, two, 2);
	rc &= expect("same list", shadow_layout_equal(&base, &other), TRUE);

	/* the right one moved down by 56: the box is still 3200x1080 */
	const MONITOR_DEF moved[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
		                          mon(1920, 56, 1280, 1024, 0) };
	all(&other, moved, 2);
	rc &=
	    expect("box size kept", (other.width == base.width) && (other.height == base.height), TRUE);
	rc &= expect("a monitor moved", shadow_layout_equal(&base, &other), FALSE);

	/* the other one is the primary now: normalized, it comes first */
	const MONITOR_DEF primary[] = { mon(1920, 0, 1280, 1024, MONITOR_PRIMARY),
		                            mon(0, 0, 1920, 1080, 0) };
	all(&other, primary, 2);
	rc &= expect("another primary", shadow_layout_equal(&base, &other), FALSE);

	/* the same rectangles, only the flag moved */
	const MONITOR_DEF flag[] = { mon(0, 0, 1920, 1080, 0),
		                         mon(1920, 0, 1280, 1024, MONITOR_PRIMARY) };
	all(&other, flag, 2);
	rc &= expect("primary flag moved", shadow_layout_equal(&base, &other), FALSE);

	/* a third monitor inside the same box (below the small one) */
	const MONITOR_DEF three[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
		                          mon(1920, 0, 1280, 1024, 0), mon(1920, 1024, 640, 56, 0) };
	all(&other, three, 3);
	rc &= expect("box size kept with three", other.width == 3200 && other.height == 1080, TRUE);
	rc &= expect("one more monitor", shadow_layout_equal(&base, &other), FALSE);

	/* one monitor shared of the same list (no longer all of them) */
	shadow_layout_set(&other, two, 2, 0, 0, 3200, 1080, FALSE);
	rc &= expect("not all shared any more", shadow_layout_equal(&base, &other), FALSE);

	/* the surface elsewhere over the same monitors */
	shadow_layout_set(&other, two, 2, 1920, 0, 3200, 1080, TRUE);
	rc &= expect("the surface moved", shadow_layout_equal(&base, &other), FALSE);

	/* a size change is a change too */
	shadow_layout_set(&other, two, 2, 0, 0, 3200, 1024, TRUE);
	rc &= expect("another size", shadow_layout_equal(&base, &other), FALSE);
	return rc;
}

static BOOL test_versions(void)
{
	BOOL rc = TRUE;
	SHADOW_LAYOUT stored = { 0 };
	SHADOW_LAYOUT now = { 0 };
	UINT32 version = 0;

	const MONITOR_DEF two[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
		                        mon(1920, 0, 1280, 1024, 0) };
	const MONITOR_DEF moved[] = { mon(0, 0, 1920, 1080, MONITOR_PRIMARY),
		                          mon(1920, 56, 1280, 1024, 0) };

	/* the first layout counts even when it equals the empty one stored */
	all(&now, two, 0);
	rc &= expect("first take", shadow_layout_update(&stored, &version, &now), TRUE);
	rc &= expect("first version 1", version == 1, TRUE);

	all(&now, two, 2);
	rc &= expect("a new list", shadow_layout_update(&stored, &version, &now), TRUE);
	rc &= expect("version 2", version == 2, TRUE);

	rc &= expect("the same list", shadow_layout_update(&stored, &version, &now), FALSE);
	rc &= expect("still version 2", version == 2, TRUE);

	all(&now, moved, 2);
	rc &= expect("moved, same size", shadow_layout_update(&stored, &version, &now), TRUE);
	rc &= expect("version 3", version == 3, TRUE);
	rc &= expect("stored the new one", shadow_layout_equal(&stored, &now), TRUE);

	/* the count wraps past 0, which stands for none */
	version = UINT32_MAX;
	all(&now, two, 2);
	rc &= expect("change at the top", shadow_layout_update(&stored, &version, &now), TRUE);
	rc &= expect("wraps to 1, not 0", version == 1, TRUE);

	/* more than 16 monitors keep 16 */
	MONITOR_DEF many[20] = { 0 };
	for (INT32 i = 0; i < 20; i++)
		many[i] = mon(i * 100, 0, 100, 100, (i == 0) ? MONITOR_PRIMARY : 0);
	all(&now, many, 20);
	rc &= expect("16 kept", now.count == 16, TRUE);
	return rc;
}

int TestShadowLayout(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	if (!test_same_size_changes())
		return -1;
	if (!test_versions())
		return -1;
	return 0;
}
