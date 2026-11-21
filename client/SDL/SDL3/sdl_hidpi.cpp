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
	std::optional<Mode> parse(const std::string& value)
	{
		if (value == "points")
			return Mode::Points;
		if (value == "pixels")
			return Mode::Pixels;
		return std::nullopt;
	}

	Mode Policy::forDisplay(uint32_t id) const
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
			if (colon == std::string::npos)
			{
				const auto mode = parse(part);
				if (!mode || fallbackSeen)
					return std::nullopt;
				policy.fallback = *mode;
				fallbackSeen = true;
			}
			else
			{
				const auto idText = part.substr(0, colon);
				const auto mode = parse(part.substr(colon + 1));
				if (!mode || idText.empty() ||
				    (idText.find_first_not_of("0123456789") != std::string::npos))
					return std::nullopt;
				errno = 0;
				const unsigned long id = std::strtoul(idText.c_str(), nullptr, 10);
				if ((errno != 0) || (id > UINT32_MAX))
					return std::nullopt;
				policy.displays[static_cast<uint32_t>(id)] = *mode;
			}
			start = end + 1;
		}
		return policy;
	}

	const char* name(Mode mode)
	{
		return (mode == Mode::Pixels) ? "pixels" : "points";
	}

	MonitorSize monitorSize(int32_t pointsWidth, int32_t pointsHeight, float density,
	                        float contentScale, Mode mode)
	{
		if (!(density > 0.0f))
			density = 1.0f;
		if (!(contentScale > 0.0f))
			contentScale = 1.0f;
		/* points: one session pixel per point; a display of one pixel per point is the same */
		const float pixels = (mode == Mode::Pixels) ? density : 1.0f;
		MonitorSize size;
		size.width = static_cast<int32_t>(std::lround(static_cast<double>(pointsWidth) * pixels));
		size.height = static_cast<int32_t>(std::lround(static_cast<double>(pointsHeight) * pixels));
		size.scale = static_cast<uint32_t>(std::lround(pixels * contentScale * 100.0f));
		return size;
	}

	std::string overlayText(Mode mode, float density)
	{
		char d[32] = {};
		(void)std::snprintf(d, sizeof(d), "%gx", static_cast<double>(density));
		if (!(density > 1.0f))
			return std::string(name(mode)) + " (display " + d + ": every pixel)";
		if (mode == Mode::Pixels)
			return std::string("pixels (display ") + d + "): every pixel, the server scales its UI";
		return std::string("points (display ") + d +
		       "): the session in points, the system scales it";
	}
} // namespace sdl::hidpi
