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

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sdl_hidpi.hpp"

namespace sdl::hidpi
{
	std::optional<Setting> parse(const std::string& value)
	{
		if (value == "points")
			return Setting{ Mode::Points };
		if (value == "pixels")
			return Setting{ Mode::Pixels };
		const std::string prefix = "scale:";
		if (value.rfind(prefix, 0) != 0)
			return std::nullopt;
		const auto number = value.substr(prefix.size());
		if (number.empty() || (number.size() > 3) ||
		    (number.find_first_not_of("0123456789") != std::string::npos))
			return std::nullopt;
		const auto percent = static_cast<uint32_t>(std::strtoul(number.c_str(), nullptr, 10));
		if ((percent < minPercent) || (percent > maxPercent))
			return std::nullopt;
		return Setting{ Mode::Scale, percent };
	}

	Setting Policy::forDisplay(uint32_t id) const
	{
		const auto it = displays.find(id);
		return (it != displays.end()) ? it->second : fallback;
	}

	std::optional<Policy> parsePolicy(const std::string& value)
	{
		Policy policy;
		bool fallbackSeen = false;
		size_t start = 0;
		while (start <= value.size())
		{
			const auto end = std::min(value.find(',', start), value.size());
			const auto part = value.substr(start, end - start);
			const auto colon = part.find(':');
			const bool byId = (colon != std::string::npos) && (colon > 0) &&
			                  (part.find_first_not_of("0123456789") == colon);
			if (!byId)
			{
				const auto setting = parse(part);
				if (!setting || fallbackSeen)
					return std::nullopt;
				policy.fallback = *setting;
				fallbackSeen = true;
			}
			else
			{
				const auto setting = parse(part.substr(colon + 1));
				if (!setting)
					return std::nullopt;
				errno = 0;
				const unsigned long id = std::strtoul(part.substr(0, colon).c_str(), nullptr, 10);
				if ((errno != 0) || (id > UINT32_MAX))
					return std::nullopt;
				policy.displays[static_cast<uint32_t>(id)] = *setting;
			}
			start = end + 1;
		}
		return policy;
	}

	const char* name(Mode mode)
	{
		switch (mode)
		{
			case Mode::Pixels:
				return "pixels";
			case Mode::Scale:
				return "scale";
			case Mode::Points:
			default:
				return "points";
		}
	}

	std::string describe(const Setting& setting)
	{
		if (setting.mode == Mode::Scale)
			return "scale:" + std::to_string(setting.percent);
		return name(setting.mode);
	}

	int32_t evenRound(double v)
	{
		return static_cast<int32_t>(2 * std::lround(v / 2.0));
	}

	MonitorSize monitorSize(int32_t pointsWidth, int32_t pointsHeight, float density,
	                        float contentScale, const Setting& setting)
	{
		if (!(density > 0.0f))
			density = 1.0f;
		if (!(contentScale > 0.0f))
			contentScale = 1.0f;
		MonitorSize size;
		if (setting.mode == Mode::Scale)
		{
			/* the points times N/100 at scale 100, drawn scaled to the window */
			const double factor = static_cast<double>(setting.percent) / 100.0;
			size.width = evenRound(static_cast<double>(pointsWidth) * factor);
			size.height = evenRound(static_cast<double>(pointsHeight) * factor);
			size.scale = 100;
			return size;
		}
		/* points: one session pixel per point; a display of one pixel per point is the same */
		const float pixels = (setting.mode == Mode::Pixels) ? density : 1.0f;
		size.width = static_cast<int32_t>(std::lround(static_cast<double>(pointsWidth) * pixels));
		size.height = static_cast<int32_t>(std::lround(static_cast<double>(pointsHeight) * pixels));
		size.scale = static_cast<uint32_t>(std::lround(pixels * contentScale * 100.0f));
		return size;
	}

	uint32_t percent(float scale)
	{
		if (!(scale > 0.0f))
			return 100;
		return static_cast<uint32_t>(std::lround(scale * 100.0f));
	}

	uint32_t deviceScaleFactor(uint32_t desktopScaleFactor)
	{
		if (desktopScaleFactor >= 180)
			return 180;
		if (desktopScaleFactor >= 140)
			return 140;
		return 100;
	}

	std::pair<float, float> toWindow(float x, float y, const Mapping& m)
	{
		return { (x + static_cast<float>(m.offsetX)) * m.scaleX,
			     (y + static_cast<float>(m.offsetY)) * m.scaleY };
	}

	std::pair<float, float> toSession(float x, float y, const Mapping& m)
	{
		const float sx = (m.scaleX > 0.0f) ? m.scaleX : 1.0f;
		const float sy = (m.scaleY > 0.0f) ? m.scaleY : 1.0f;
		return { (x / sx) - static_cast<float>(m.offsetX),
			     (y / sy) - static_cast<float>(m.offsetY) };
	}

	std::string overlayText(const Setting& setting, float density)
	{
		char d[32] = {};
		(void)std::snprintf(d, sizeof(d), "%gx", static_cast<double>(density));
		if (setting.mode == Mode::Scale)
			return describe(setting) + " (display " + d + "): the session at the points x " +
			       std::to_string(setting.percent) + " %, drawn to fit";
		if (!(density > 1.0f))
			return std::string(name(setting.mode)) + " (display " + d + ": every pixel)";
		if (setting.mode == Mode::Pixels)
			return std::string("pixels (display ") + d + "): every pixel, the server scales its UI";
		return std::string("points (display ") + d +
		       "): the session in points, the system scales it";
	}
} // namespace sdl::hidpi
