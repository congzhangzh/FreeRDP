/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The order the shadow server tries the GFX codecs in
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
#include <freerdp/channels/rdpgfx.h>

#include "shadow_codec_order.h"

size_t shadow_gfx_codec_default_order(BOOL hevcFirst, SHADOW_GFX_CODEC* order, size_t size)
{
	static const SHADOW_GFX_CODEC rest[] = { SHADOW_GFX_CODEC_AVC444, SHADOW_GFX_CODEC_AVC420,
		                                     SHADOW_GFX_CODEC_RFX, SHADOW_GFX_CODEC_PROGRESSIVE,
		                                     SHADOW_GFX_CODEC_PLANAR };
	const size_t total = 3 + ARRAYSIZE(rest);
	if (!order || (size < total))
		return 0;

	size_t n = 0;
	if (hevcFirst)
		order[n++] = SHADOW_GFX_CODEC_HEVC;
	order[n++] = SHADOW_GFX_CODEC_AV2;
	order[n++] = SHADOW_GFX_CODEC_AV1;
	if (!hevcFirst)
		order[n++] = SHADOW_GFX_CODEC_HEVC;
	for (size_t x = 0; x < ARRAYSIZE(rest); x++)
		order[n++] = rest[x];
	return n;
}

size_t shadow_gfx_caps_versions(const SHADOW_GFX_CODEC* order, size_t orderCount, UINT32* versions,
                                size_t size)
{
	WINPR_ASSERT(order || (orderCount == 0));
	WINPR_ASSERT(versions || (size == 0));

#if defined(WITH_GFX_AV2)
	static const UINT32 av2[] = { RDPGFX_CAPVERSION_FRDP_2 };
#endif
#if defined(WITH_GFX_AV1)
	static const UINT32 av1[] = { RDPGFX_CAPVERSION_FRDP_1 };
#endif
#if defined(WITH_GFX_AZURE)
	static const UINT32 hevc[] = { RDPGFX_CAPVERSION_115, RDPGFX_CAPVERSION_113,
		                           RDPGFX_CAPVERSION_112, RDPGFX_CAPVERSION_111 };
#endif
	static const UINT32 v10[] = { RDPGFX_CAPVERSION_107,     RDPGFX_CAPVERSION_106,
		                          RDPGFX_CAPVERSION_106_ERR, RDPGFX_CAPVERSION_105,
		                          RDPGFX_CAPVERSION_104,     RDPGFX_CAPVERSION_103,
		                          RDPGFX_CAPVERSION_102,     RDPGFX_CAPVERSION_101,
		                          RDPGFX_CAPVERSION_10 };

	enum
	{
		GROUP_AV2,
		GROUP_AV1,
		GROUP_HEVC,
		GROUP_V10,
		GROUP_COUNT
	};
	BOOL added[GROUP_COUNT] = WINPR_C_ARRAY_INIT;
	size_t count = 0;

	for (size_t x = 0; x < orderCount; x++)
	{
		const UINT32* group = nullptr;
		size_t groupSize = 0;
		size_t id = GROUP_COUNT;
		switch (order[x])
		{
#if defined(WITH_GFX_AV2)
			case SHADOW_GFX_CODEC_AV2:
				group = av2;
				groupSize = ARRAYSIZE(av2);
				id = GROUP_AV2;
				break;
#endif
#if defined(WITH_GFX_AV1)
			case SHADOW_GFX_CODEC_AV1:
				group = av1;
				groupSize = ARRAYSIZE(av1);
				id = GROUP_AV1;
				break;
#endif
#if defined(WITH_GFX_AZURE)
			case SHADOW_GFX_CODEC_HEVC:
				group = hevc;
				groupSize = ARRAYSIZE(hevc);
				id = GROUP_HEVC;
				break;
#endif
			case SHADOW_GFX_CODEC_AVC444:
			case SHADOW_GFX_CODEC_AVC420:
			case SHADOW_GFX_CODEC_RFX:
			case SHADOW_GFX_CODEC_PROGRESSIVE:
			case SHADOW_GFX_CODEC_PLANAR:
				group = v10;
				groupSize = ARRAYSIZE(v10);
				id = GROUP_V10;
				break;
			default:
				break;
		}
		if (!group || added[id])
			continue;
		added[id] = TRUE;
		for (size_t y = 0; (y < groupSize) && (count < size); y++)
			versions[count++] = group[y];
	}
	return count;
}
