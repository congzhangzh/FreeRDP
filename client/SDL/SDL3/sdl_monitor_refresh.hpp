/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the /multimon layout after a display changed its scale, mode or place
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
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <freerdp/settings_types.h>

#include "sdl_display_offsets.hpp"

/* With /multimon each display has a window that shows its part of the session. When a display
 * changes its scale (or mode, or place) its monitor is measured again; a new pixel size moves the
 * pixel origins of the displays beside it, so all are placed again, and each window's offset into
 * the session follows. Nothing here calls SDL, so it is tested without a display. */
namespace sdl_monitor_refresh
{
	using Displays = std::map<SDL_DisplayID, rdpMonitor>;
	using Bounds = std::map<SDL_DisplayID, SDL_Rect>;

	/** Places the displays at their pixel origins, from their logical bounds (points) and their
	 * pixel sizes; only the selected displays need to fit together (all when none is). */
	inline void place(Displays& displays, const Bounds& bounds, SDL_DisplayID primary,
	                  const std::vector<SDL_DisplayID>& selected)
	{
		sdl_display_offsets::Offsets offsets;
		for (const auto& entry : displays)
		{
			SDL_Rect logical{};
			const auto found = bounds.find(entry.first);
			if (found != bounds.end())
				logical = found->second;

			SDL_Rect pixel{};
			pixel.w = entry.second.width;
			pixel.h = entry.second.height;
			offsets.emplace(entry.first, std::pair{ logical, pixel });
		}
		if (offsets.empty())
			return;
		sdl_display_offsets::update(offsets, primary, selected);
		for (auto& entry : displays)
		{
			const auto& pixel = offsets.at(entry.first).second;
			entry.second.x = pixel.x;
			entry.second.y = pixel.y;
		}
	}

	/** @return the offset of a window showing monitor m of the layout sent: minus its position
	 * from the layout's top left corner (which is never right or below the primary at 0,0) */
	inline std::pair<int32_t, int32_t> windowOffset(const std::vector<rdpMonitor>& layout,
	                                                const rdpMonitor& m)
	{
		int32_t originX = 0;
		int32_t originY = 0;
		for (const auto& cur : layout)
		{
			originX = std::min<int32_t>(cur.x, originX);
			originY = std::min<int32_t>(cur.y, originY);
		}
		return { originX - m.x, originY - m.y };
	}

	/** @return the monitor of the layout showing display id, nullptr if none does */
	inline const rdpMonitor* find(const std::vector<rdpMonitor>& layout, SDL_DisplayID id)
	{
		const auto it = std::find_if(layout.begin(), layout.end(),
		                             [id](const rdpMonitor& m) { return m.orig_screen == id; });
		return (it != layout.end()) ? &*it : nullptr;
	}
} // namespace sdl_monitor_refresh
