/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The order the shadow server tries a client's GFX capability sets in
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

#include "shadow_caps_order.h"

/* The server's own order of the versions it supports past 8.1 */
static const UINT32 shadow_caps_versions[] = {
#if defined(WITH_GFX_AV2)
	RDPGFX_CAPVERSION_FRDP_2,
#endif
#if defined(WITH_GFX_AV1)
	RDPGFX_CAPVERSION_FRDP_1,
#endif
#if defined(WITH_GFX_AZURE)
	RDPGFX_CAPVERSION_115,    RDPGFX_CAPVERSION_113, RDPGFX_CAPVERSION_112,
	RDPGFX_CAPVERSION_111,
#endif
	RDPGFX_CAPVERSION_107,    RDPGFX_CAPVERSION_106, RDPGFX_CAPVERSION_106_ERR,
	RDPGFX_CAPVERSION_105,    RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_103,
	RDPGFX_CAPVERSION_102,    RDPGFX_CAPVERSION_101, RDPGFX_CAPVERSION_10,
};

/* The rank of a version in the server's priority, 8.0 lowest: 0 for one it does not know. 10.6 and
 * 10.6_ERR are the same capability set and rank the same. */
static size_t shadow_caps_rank(UINT32 version)
{
	if (version == RDPGFX_CAPVERSION_8)
		return 1;
	if (version == RDPGFX_CAPVERSION_81)
		return 2;
	if (version == RDPGFX_CAPVERSION_106_ERR)
		version = RDPGFX_CAPVERSION_106;
	for (size_t x = 0; x < ARRAYSIZE(shadow_caps_versions); x++)
	{
		if (shadow_caps_versions[x] == version)
			return 2 + ARRAYSIZE(shadow_caps_versions) - x;
	}
	return 0;
}

BOOL shadow_caps_reordered(const RDPGFX_CAPSET* capsSets, size_t count)
{
	WINPR_ASSERT(capsSets || (count == 0));

	BOOL up = FALSE;
	BOOL down = FALSE;
	size_t last = 0;
	for (size_t x = 0; x < count; x++)
	{
		const size_t rank = shadow_caps_rank(capsSets[x].version);
		if (rank == 0)
			continue;
		if ((last != 0) && (rank > last))
			up = TRUE;
		if ((last != 0) && (rank < last))
			down = TRUE;
		last = rank;
	}
	return up && down;
}

static BOOL shadow_caps_supported(UINT32 version)
{
	for (size_t x = 0; x < ARRAYSIZE(shadow_caps_versions); x++)
	{
		if (shadow_caps_versions[x] == version)
			return TRUE;
	}
	return FALSE;
}

size_t shadow_caps_order(const RDPGFX_CAPSET* capsSets, size_t count, BOOL followClient,
                         UINT32* versions, size_t size)
{
	WINPR_ASSERT(capsSets || (count == 0));
	WINPR_ASSERT(versions || (size == 0));

	size_t n = 0;
	if (!followClient || !shadow_caps_reordered(capsSets, count))
	{
		for (size_t x = 0; (x < ARRAYSIZE(shadow_caps_versions)) && (n < size); x++)
			versions[n++] = shadow_caps_versions[x];
		return n;
	}

	for (size_t x = count; (x > 0) && (n < size); x--)
	{
		const UINT32 version = capsSets[x - 1].version;
		if (!shadow_caps_supported(version))
			continue;

		BOOL twice = FALSE;
		for (size_t y = 0; y < n; y++)
		{
			if (versions[y] == version)
				twice = TRUE;
		}
		if (!twice)
			versions[n++] = version;
	}
	return n;
}
