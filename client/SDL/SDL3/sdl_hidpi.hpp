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
#include <utility>

/* On a display with more than one pixel per point (a Retina Mac, a 2x Wayland output) a window can
 * draw every pixel or one pixel per point that the system scales up. "points" asks the server for
 * the session in points at the display's own scale: a quarter of the pixels at 2x, the system
 * scales the picture. "pixels" asks for every pixel at the scale times the density, as before. A
 * display with one pixel per point is the same in both. "scale:N" asks for the points times N/100
 * at scale 100 and the client draws that session scaled to the window's pixels. Each display goes
 * by its own density, and a display can have a mode of its own (by its id in /monitor-list).
 * Nothing here touches SDL, so it is tested without a display. */
namespace sdl::hidpi
{
	enum class Mode
	{
		Points,
		Pixels,
		Scale
	};

	/** A display's mode; percent only for Scale */
	struct Setting
	{
		Mode mode = Mode::Points;
		uint32_t percent = 100;

		Setting() = default;
		Setting(Mode m, uint32_t p = 100) : mode(m), percent(p) // NOLINT: a mode is a setting
		{
		}
		[[nodiscard]] bool operator==(const Setting& other) const
		{
			return (mode == other.mode) && ((mode != Mode::Scale) || (percent == other.percent));
		}
	};

	/** scale:N, N from 50 to 400 */
	constexpr uint32_t minPercent = 50;
	constexpr uint32_t maxPercent = 400;

	/** The setting of each display: a default and settings of their own by display id. */
	struct Policy
	{
		Setting fallback;
		std::map<uint32_t, Setting> displays;

		[[nodiscard]] Setting forDisplay(uint32_t id) const;
	};

	/** @return the setting of its name (points, pixels, scale:N), nothing for an unknown one */
	[[nodiscard]] std::optional<Setting> parse(const std::string& value);

	/** @return the policy of an /sdl-hidpi value: "<setting>[,<display id>:<setting>...]", the
	 * overrides alone keep the default (points); nothing when a part is not understood */
	[[nodiscard]] std::optional<Policy> parsePolicy(const std::string& value);

	[[nodiscard]] const char* name(Mode mode);

	/** @return "points", "pixels" or "scale:150" */
	[[nodiscard]] std::string describe(const Setting& setting);

	/** @return v rounded to the nearest even number (a session size) */
	[[nodiscard]] int32_t evenRound(double v);

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
	                                      float contentScale, const Setting& setting);

	/** Where a part of the session is drawn in a window: (session + offset) x scale, the offset
	 * being minus the window's monitor position in the session (/multimon), the scale the window's
	 * pixels per session pixel (1 except with scale:N). */
	struct Mapping
	{
		float scaleX = 1.0f;
		float scaleY = 1.0f;
		int32_t offsetX = 0;
		int32_t offsetY = 0;
	};

	/** @return the window pixel of a session pixel */
	[[nodiscard]] std::pair<float, float> toWindow(float x, float y, const Mapping& m);

	/** @return the session pixel of a window pixel (the inverse of toWindow) */
	[[nodiscard]] std::pair<float, float> toSession(float x, float y, const Mapping& m);

	/** @return the overlay's line for the setting on a display of that density */
	[[nodiscard]] std::string overlayText(const Setting& setting, float density);
} // namespace sdl::hidpi
