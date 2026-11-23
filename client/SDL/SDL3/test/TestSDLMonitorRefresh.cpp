/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client tests: the /multimon layout after one of two displays changed its scale
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
#include <cstdio>
#include <vector>

#include "../sdl_hidpi.hpp"
#include "../sdl_monitor_refresh.hpp"

using sdl::hidpi::Mode;

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return false;                                                    \
		}                                                                    \
	} while (0)

constexpr SDL_DisplayID A = 1; /* the primary */
constexpr SDL_DisplayID B = 2;

/* A display's monitor as the client measures it: the size and desktop scale from the display's
 * bounds, density and own scale, the device scale that goes with it */
static rdpMonitor measure(SDL_DisplayID id, const SDL_Rect& bounds, float density,
                          float contentScale, Mode mode)
{
	const auto size = sdl::hidpi::monitorSize(bounds.w, bounds.h, density, contentScale, mode);
	rdpMonitor m{};
	m.orig_screen = id;
	m.x = bounds.x;
	m.y = bounds.y;
	m.width = size.width;
	m.height = size.height;
	m.is_primary = (id == A);
	m.attributes.desktopScaleFactor = size.scale;
	m.attributes.deviceScaleFactor = sdl::hidpi::deviceScaleFactor(size.scale);
	return m;
}

/* the layout the client sends: the displays at their pixel origins */
static std::vector<rdpMonitor> layout(sdl_monitor_refresh::Displays& displays,
                                      const sdl_monitor_refresh::Bounds& bounds)
{
	sdl_monitor_refresh::place(displays, bounds, A, { A, B });
	std::vector<rdpMonitor> list;
	for (const auto& entry : displays)
		list.push_back(entry.second);
	return list;
}

static bool same(const rdpMonitor* m, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t desktop,
                 uint32_t device)
{
	if (!m)
	{
		(void)fprintf(stderr, "monitor missing\n");
		return false;
	}
	if ((m->x == x) && (m->y == y) && (m->width == w) && (m->height == h) &&
	    (m->attributes.desktopScaleFactor == desktop) &&
	    (m->attributes.deviceScaleFactor == device))
		return true;
	(void)fprintf(
	    stderr,
	    "display %u: got %dx%d%+d%+d scale %u device %u, want %dx%d%+d%+d scale %u device "
	    "%u\n",
	    m->orig_screen, m->width, m->height, m->x, m->y, m->attributes.desktopScaleFactor,
	    m->attributes.deviceScaleFactor, w, h, x, y, desktop, device);
	return false;
}

static bool offset(const std::vector<rdpMonitor>& list, SDL_DisplayID id, int32_t x, int32_t y)
{
	const auto m = sdl_monitor_refresh::find(list, id);
	CHECK(m);
	const auto [ox, oy] = sdl_monitor_refresh::windowOffset(list, *m);
	if ((ox == x) && (oy == y))
		return true;
	(void)fprintf(stderr, "display %u: window offset %d,%d, want %d,%d\n", id, ox, oy, x, y);
	return false;
}

/* B right of A, B goes from 100 % to 150 % (Windows, points): only B's factors change */
static bool checkRightToWindows150()
{
	const sdl_monitor_refresh::Bounds bounds{ { A, { 0, 0, 1920, 1080 } },
		                                      { B, { 1920, 0, 1920, 1080 } } };
	sdl_monitor_refresh::Displays displays{
		{ A, measure(A, bounds.at(A), 1.0f, 1.0f, Mode::Points) },
		{ B, measure(B, bounds.at(B), 1.0f, 1.0f, Mode::Points) }
	};
	auto list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, B), 1920, 0, 1920, 1080, 100, 100));

	displays.insert_or_assign(B, measure(B, bounds.at(B), 1.0f, 1.5f, Mode::Points));
	list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, A), 0, 0, 1920, 1080, 100, 100));
	CHECK(same(sdl_monitor_refresh::find(list, B), 1920, 0, 1920, 1080, 150, 140));
	CHECK(offset(list, A, 0, 0));
	CHECK(offset(list, B, -1920, 0));
	return true;
}

/* B left of A becomes a 2x display (pixels): it doubles in pixels, so its origin moves left and
 * A's window offset into the session moves with it */
static bool checkLeftTo2x()
{
	const sdl_monitor_refresh::Bounds bounds{ { A, { 0, 0, 1920, 1080 } },
		                                      { B, { -1920, 0, 1920, 1080 } } };
	sdl_monitor_refresh::Displays displays{
		{ A, measure(A, bounds.at(A), 1.0f, 1.0f, Mode::Pixels) },
		{ B, measure(B, bounds.at(B), 1.0f, 1.0f, Mode::Pixels) }
	};
	auto list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, B), -1920, 0, 1920, 1080, 100, 100));
	CHECK(offset(list, A, -1920, 0));
	CHECK(offset(list, B, 0, 0));

	displays.insert_or_assign(B, measure(B, bounds.at(B), 2.0f, 1.0f, Mode::Pixels));
	list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, A), 0, 0, 1920, 1080, 100, 100));
	CHECK(same(sdl_monitor_refresh::find(list, B), -3840, 0, 3840, 2160, 200, 180));
	CHECK(offset(list, A, -3840, 0));
	CHECK(offset(list, B, 0, 0));
	return true;
}

/* and back: a display that returns to 100 % gets its first layout again */
static bool checkBack()
{
	const sdl_monitor_refresh::Bounds bounds{ { A, { 0, 0, 1920, 1080 } },
		                                      { B, { 1920, 0, 1920, 1080 } } };
	sdl_monitor_refresh::Displays displays{
		{ A, measure(A, bounds.at(A), 1.0f, 1.0f, Mode::Pixels) },
		{ B, measure(B, bounds.at(B), 2.0f, 1.0f, Mode::Pixels) }
	};
	auto list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, B), 1920, 0, 3840, 2160, 200, 180));

	displays.insert_or_assign(B, measure(B, bounds.at(B), 1.0f, 1.0f, Mode::Pixels));
	list = layout(displays, bounds);
	CHECK(same(sdl_monitor_refresh::find(list, B), 1920, 0, 1920, 1080, 100, 100));
	CHECK(!sdl_monitor_refresh::find(list, 42));
	return true;
}

int main()
{
	for (auto* check : { checkRightToWindows150, checkLeftTo2x, checkBack })
	{
		if (!check())
			return -1;
	}
	(void)printf("monitor refresh: ok\n");
	return 0;
}
