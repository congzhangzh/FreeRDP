/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: what a high pixel density display asks of the server (/sdl-hidpi)
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

#include <cstdint>
#include <map>
#include <optional>
#include <string>

/* On a display with more than one pixel per point (a Retina Mac, a 2x Wayland output) a window can
 * draw every pixel or one pixel per point that the system scales up. "points" asks the server for
 * the session in points at the display's own scale: a quarter of the pixels at 2x, the system
 * scales the picture. "pixels" asks for every pixel at the scale times the density, as before. A
 * display with one pixel per point is the same in both. Each display goes by its own density, and
 * a display can have a mode of its own (by its id in /monitor-list). Nothing here touches SDL, so
 * it is tested without a display. */
namespace sdl::hidpi
{
	enum class Mode
	{
		Points,
		Pixels
	};

	/** The mode of each display: a default and modes of their own by display id. */
	struct Policy
	{
		Mode fallback = Mode::Points;
		std::map<uint32_t, Mode> displays;

		[[nodiscard]] Mode forDisplay(uint32_t id) const;
	};

	/** @return the mode of its name (points, pixels), nothing for an unknown one */
	[[nodiscard]] std::optional<Mode> parse(const std::string& value);

	/** @return the policy of an /sdl-hidpi value: "<mode>[,<display id>:<mode>...]", the
	 * overrides alone keep the default (points); nothing when a part is not understood */
	[[nodiscard]] std::optional<Policy> parsePolicy(const std::string& value);

	[[nodiscard]] const char* name(Mode mode);

	struct MonitorSize
	{
		int32_t width = 0;
		int32_t height = 0;
		uint32_t scale = 100; /**< the desktop scale factor, percent */
	};

	/** What a monitor of pointsWidth x pointsHeight points asks of the server.
	 * @param density the display's pixels per point (2 on a Retina Mac)
	 * @param contentScale the display's own scale, beside the density (1.5 for 150 % on Windows) */
	[[nodiscard]] MonitorSize monitorSize(int32_t pointsWidth, int32_t pointsHeight, float density,
	                                      float contentScale, Mode mode);

	/** @return the overlay's line for the mode on a display of that density */
	[[nodiscard]] std::string overlayText(Mode mode, float density);
} // namespace sdl::hidpi
