/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client tests: the server's monitor layout handed from the channel's thread to SDL's
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
#include <atomic>
#include <cstdio>
#include <thread>
#include <tuple>
#include <vector>

#include "../sdl_server_layout.hpp"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return false;                                                    \
		}                                                                    \
	} while (0)

using sdl_server_layout::Display;
using sdl_server_layout::Inbox;
using sdl_server_layout::Layout;
using sdl_server_layout::Rect;

/* two monitors as the shadow sends them: the primary at 0,0, the other left of it */
static Layout twoMonitors(uint32_t width)
{
	const MONITOR_DEF monitors[] = { { 0, 0, 1023, 767, MONITOR_PRIMARY },
		                             { -800, 0, -1, 599, 0 } };
	return sdl_server_layout::copyOf(width, 768, 2, monitors);
}

/* the copy owns its monitors: the PDU's array may be freed right after */
static bool checkCopy()
{
	MONITOR_DEF monitors[] = { { 0, 0, 1023, 767, MONITOR_PRIMARY }, { 1024, 0, 1823, 599, 0 } };
	const auto layout = sdl_server_layout::copyOf(1824, 768, 2, monitors);
	monitors[1].left = 42;
	CHECK(layout.width == 1824);
	CHECK(layout.height == 768);
	CHECK(layout.monitors.size() == 2);
	CHECK(layout.monitors[1].left == 1024);
	CHECK(layout.monitors[1].bottom == 599);
	CHECK(layout.monitors[0].flags == MONITOR_PRIMARY);

	/* no array (a server without monitors): none, whatever the count says */
	const auto none = sdl_server_layout::copyOf(1024, 768, 3, nullptr);
	CHECK(none.monitors.empty());
	return true;
}

/* a second Reset Graphics before SDL's thread took the first: the newest wins, once */
static bool checkNewestWins()
{
	Inbox inbox;
	CHECK(!inbox.take());
	CHECK(!inbox.post(twoMonitors(1)));
	CHECK(inbox.post(twoMonitors(2))); /* replaced the first */
	auto taken = inbox.take();
	CHECK(taken);
	CHECK(taken->width == 2);
	/* the second event of the two finds nothing */
	CHECK(!inbox.take());
	CHECK(inbox.posted() == 2);
	return true;
}

/* the session ends before SDL's thread took the layout: the end replaces it, and says "none" */
static bool checkEndReplaces()
{
	Inbox inbox;
	(void)inbox.post(twoMonitors(1));
	CHECK(inbox.post(Layout{}));
	auto taken = inbox.take();
	CHECK(taken);
	CHECK(taken->monitors.empty());
	CHECK(!inbox.take());

	inbox.clear();
	CHECK(!inbox.take());
	(void)inbox.post(twoMonitors(3));
	inbox.clear();
	CHECK(!inbox.take());
	return true;
}

/* one thread posts, one takes, both as fast as they can: every take is a whole layout, newer
 * than the one before, and the last one posted is the last one taken. Run under ASan / TSan this
 * also shows no leak and no race. */
static bool checkThreads()
{
	constexpr uint32_t count = 20000;
	Inbox inbox;
	std::atomic<bool> done = false;
	std::thread channel(
	    [&]()
	    {
		    for (uint32_t x = 1; x <= count; x++)
			    (void)inbox.post(twoMonitors(x));
		    done = true;
	    });

	uint32_t last = 0;
	bool ordered = true;
	bool whole = true;
	const auto takeOne = [&]()
	{
		auto taken = inbox.take();
		if (!taken)
			return;
		if (taken->width <= last)
			ordered = false;
		if ((taken->monitors.size() != 2) || (taken->monitors[1].left != -800))
			whole = false;
		last = taken->width;
	};
	while (!done)
		takeOne();
	channel.join();
	takeOne();

	CHECK(ordered);
	CHECK(whole);
	CHECK(last == count);
	CHECK(!inbox.take());
	CHECK(inbox.posted() == count);
	return true;
}

/* the client's two 1920x1080 screens side by side, as it sent them in the core data */
static const std::vector<Rect> sentTwo = { { 0, 0, 1920, 1080 }, { 1920, 0, 1920, 1080 } };
static const std::vector<Display> twoScreens = { { 7, { 0, 0, 1920, 1080 }, true },
	                                             { 8, { 1920, 0, 1920, 1080 }, false } };

static Layout layoutOf(uint32_t width, uint32_t height, std::vector<MONITOR_DEF> monitors)
{
	return sdl_server_layout::copyOf(width, height, static_cast<uint32_t>(monitors.size()),
	                                 monitors.data());
}

static bool same(const Rect& r, int32_t x, int32_t y, int32_t w, int32_t h)
{
	return r == Rect{ x, y, w, h };
}

/* the server echoes the client's layout: as sent, in another order, the primary first, moved
 * so its top left is 0,0, or anywhere else. Each one is the client's layout: no follow. */
static bool checkEchoIsSame()
{
	const std::vector<std::vector<MONITOR_DEF>> echoes = {
		/* as sent */
		{ { 0, 0, 1919, 1079, MONITOR_PRIMARY }, { 1920, 0, 3839, 1079, 0 } },
		/* the other order */
		{ { 1920, 0, 3839, 1079, 0 }, { 0, 0, 1919, 1079, MONITOR_PRIMARY } },
		/* another primary, put first */
		{ { 1920, 0, 3839, 1079, MONITOR_PRIMARY }, { 0, 0, 1919, 1079, 0 } },
		/* the primary at 0,0, the other left of it (Windows' form) */
		{ { 0, 0, 1919, 1079, MONITOR_PRIMARY }, { -1920, 0, -1, 1079, 0 } },
		/* moved anywhere */
		{ { 100, -50, 2019, 1029, MONITOR_PRIMARY }, { 2020, -50, 3939, 1029, 0 } }
	};
	for (const auto& echo : echoes)
	{
		CHECK(sdl_server_layout::sameLayout(echo, sentTwo));
		const auto p =
		    sdl_server_layout::plan(true, layoutOf(3840, 1080, echo), sentTwo, twoScreens);
		CHECK(!p.follow);
		CHECK(!p.warn);
		/* the kit's screens tests grep the decision line for this */
		CHECK(p.reason == "same as sent");
	}

	/* the client sent a layout with a monitor left of its primary; the server moves the top
	 * left to 0,0 and puts the primary first */
	const std::vector<Rect> sentLeft = { { 0, 0, 2560, 1440 }, { -1706, 200, 1706, 1066 } };
	const std::vector<MONITOR_DEF> shifted = { { 1706, 0, 4265, 1439, MONITOR_PRIMARY },
		                                       { 0, 200, 1705, 1265, 0 } };
	CHECK(sdl_server_layout::sameLayout(shifted, sentLeft));
	return true;
}

/* not the client's layout: one pixel off, a monitor more or less, another arrangement */
static bool checkOtherIsNotSame()
{
	CHECK(!sdl_server_layout::sameLayout(
	    { { 0, 0, 1919, 1079, MONITOR_PRIMARY }, { 1920, 0, 3838, 1079, 0 } }, sentTwo));
	CHECK(!sdl_server_layout::sameLayout({ { 0, 0, 1919, 1079, MONITOR_PRIMARY } }, sentTwo));
	CHECK(!sdl_server_layout::sameLayout(
	    { { 0, 0, 1919, 1079, MONITOR_PRIMARY }, { 0, 1080, 1919, 2159, 0 } }, sentTwo));
	CHECK(!sdl_server_layout::sameLayout({}, sentTwo));
	return true;
}

/* the Windows shadow after the CI's monitor move: 1024x768 primary, 800x600 left of it, on the
 * client's two 1920x1080 screens. Primary to primary, letterboxed; the session's top left is the
 * left monitor's. */
static bool checkShadowTwoMonitors()
{
	const auto server =
	    layoutOf(1824, 768, { { 0, 0, 1023, 767, MONITOR_PRIMARY }, { -800, 0, -1, 599, 0 } });
	const auto p = sdl_server_layout::plan(true, server, sentTwo, twoScreens);
	CHECK(p.follow);
	CHECK(!p.warn);
	CHECK(p.windows.size() == 2);

	const auto& primary = p.windows[0];
	CHECK(primary.monitor == 0);
	CHECK(primary.display == 7);
	CHECK(same(primary.source, 800, 0, 1024, 768));
	/* 1080 / 768 = 1.40625 < 1920 / 1024: 1440x1080, centred */
	CHECK(same(primary.target, 240, 0, 1440, 1080));
	CHECK(primary.scale == 1.40625f);

	const auto& left = p.windows[1];
	CHECK(left.monitor == 1);
	CHECK(left.display == 8);
	CHECK(same(left.source, 0, 0, 800, 600));
	CHECK(same(left.target, 240, 0, 1440, 1080));
	CHECK(left.scale == 1.8f);
	return true;
}

/* the client's screens in another order, its primary second, one at a negative place: windows
 * by rect and flag, not by index */
static bool checkOrderByPlace()
{
	const std::vector<Display> screens = { { 21, { 1920, 0, 1280, 1024 }, false },
		                                   { 22, { -1280, 0, 1280, 1024 }, false },
		                                   { 23, { 0, 0, 1920, 1080 }, true } };
	/* the server's primary last in its list, the others right of it */
	const auto server = layoutOf(3000, 900,
	                             { { 2000, 0, 2999, 899, 0 },
	                               { 1000, 0, 1999, 899, 0 },
	                               { 0, 0, 999, 899, MONITOR_PRIMARY } });
	const auto p = sdl_server_layout::plan(true, server, sentTwo, screens);
	CHECK(p.follow);
	CHECK(p.windows.size() == 3);
	CHECK((p.windows[0].monitor == 2) && (p.windows[0].display == 23));
	CHECK((p.windows[1].monitor == 1) && (p.windows[1].display == 22));
	CHECK((p.windows[2].monitor == 0) && (p.windows[2].display == 21));
	CHECK(same(p.windows[2].source, 2000, 0, 1000, 900));
	/* 1024 / 900 (1.138) < 1280 / 1000: the height fits, 1138x1024, bars left and right */
	CHECK(p.windows[1].target.h == 1024);
	CHECK(p.windows[1].target.w == 1138);
	CHECK(p.windows[1].target.x == (1280 - 1138) / 2);
	return true;
}

/* the server's two monitors one above the other, each the size of a client screen: 1:1, no bars */
static bool checkOneToOne()
{
	const auto stacked =
	    layoutOf(1920, 2160, { { 0, 0, 1919, 1079, MONITOR_PRIMARY }, { 0, 1080, 1919, 2159, 0 } });
	const auto p = sdl_server_layout::plan(true, stacked, sentTwo, twoScreens);
	CHECK(p.follow);
	CHECK(p.windows.size() == 2);
	CHECK(same(p.windows[1].source, 0, 1080, 1920, 1080));
	CHECK(same(p.windows[1].target, 0, 0, 1920, 1080));
	CHECK(p.windows[1].scale == 1.0f);
	return true;
}

/* what keeps today's windows: no /multimon, no list, more server monitors than screens (a
 * warning naming both counts), a monitor outside the desktop; fewer server monitors leave a
 * screen without a window */
static bool checkKeepToday()
{
	const auto two =
	    layoutOf(1824, 768, { { 0, 0, 1023, 767, MONITOR_PRIMARY }, { -800, 0, -1, 599, 0 } });
	auto p = sdl_server_layout::plan(false, two, sentTwo, twoScreens);
	CHECK(!p.follow && !p.warn);
	p = sdl_server_layout::plan(true, std::nullopt, sentTwo, twoScreens);
	CHECK(!p.follow && !p.warn);
	p = sdl_server_layout::plan(true, Layout{ 1024, 768, {} }, sentTwo, twoScreens);
	CHECK(!p.follow && !p.warn);

	const auto three = layoutOf(3000, 900,
	                            { { 0, 0, 999, 899, MONITOR_PRIMARY },
	                              { 1000, 0, 1999, 899, 0 },
	                              { 2000, 0, 2999, 899, 0 } });
	p = sdl_server_layout::plan(true, three, sentTwo, twoScreens);
	CHECK(!p.follow);
	CHECK(p.warn);
	CHECK(p.reason == "the server has 3 monitors, the client 2 screens");

	/* a desktop smaller than its monitors: a broken list, not followed */
	p = sdl_server_layout::plan(
	    true,
	    layoutOf(1024, 768, { { 0, 0, 1023, 767, MONITOR_PRIMARY }, { 1024, 0, 1823, 599, 0 } }),
	    sentTwo, twoScreens);
	CHECK(!p.follow);
	CHECK(p.warn);
	/* an empty monitor (right left of left) */
	p = sdl_server_layout::plan(true, layoutOf(1024, 768, { { 10, 0, 5, 767, MONITOR_PRIMARY } }),
	                            sentTwo, twoScreens);
	CHECK(!p.follow);
	CHECK(p.warn);

	/* one server monitor on two screens: one window, on the primary */
	p = sdl_server_layout::plan(true, layoutOf(1600, 900, { { 0, 0, 1599, 899, MONITOR_PRIMARY } }),
	                            sentTwo, twoScreens);
	CHECK(p.follow);
	CHECK(p.windows.size() == 1);
	CHECK(p.windows[0].display == 7);
	CHECK(same(p.windows[0].target, 0, 0, 1920, 1080));
	CHECK(p.windows[0].scale == 1.2f);
	return true;
}

/* damage as the CI shadow's primary window draws it: source 800,0 1024x768, letterboxed at
 * 240,0 1440x1080 (scale 1.40625) */
static bool checkDrawInWindow()
{
	const Rect source{ 800, 0, 1024, 768 };
	const Rect target{ 240, 0, 1440, 1080 };
	const float scale = 1.40625f;

	auto all = sdl_server_layout::toWindow(source, source, target, scale);
	CHECK(all);
	CHECK(same(all->source, 800, 0, 1024, 768));
	CHECK(same(all->target, 240, 0, 1440, 1080));

	/* the other monitor's damage never reaches this window */
	CHECK(!sdl_server_layout::toWindow({ 0, 0, 800, 600 }, source, target, scale));
	CHECK(!sdl_server_layout::toWindow({ 0, 0, 800, 768 }, source, target, scale));

	/* damage across both monitors: only this one's part */
	auto part = sdl_server_layout::toWindow({ 0, 0, 900, 10 }, source, target, scale);
	CHECK(part);
	CHECK(same(part->source, 800, 0, 100, 10));
	CHECK(same(part->target, 240, 0, 141, 14));

	/* neighbouring damage meets without a gap or an overlap */
	auto left = sdl_server_layout::toWindow({ 800, 100, 200, 50 }, source, target, scale);
	auto right = sdl_server_layout::toWindow({ 1000, 100, 824, 50 }, source, target, scale);
	CHECK(left && right);
	CHECK(left->target.x + left->target.w == right->target.x);
	CHECK(right->target.x + right->target.w == target.x + target.w);
	return true;
}

/* window pixels back to the session, the bars to the nearest edge of the monitor */
static bool checkInputFromWindow()
{
	const Rect source{ 800, 0, 1024, 768 };
	const Rect target{ 240, 0, 1440, 1080 };
	const float scale = 1.40625f;

	auto [x, y] = sdl_server_layout::toSession(960.0f, 540.0f, source, target, scale);
	CHECK((x == 1312.0f) && (y == 384.0f));
	std::tie(x, y) = sdl_server_layout::toSession(0.0f, 0.0f, source, target, scale);
	CHECK((x == 800.0f) && (y == 0.0f));
	std::tie(x, y) = sdl_server_layout::toSession(1919.0f, 1079.0f, source, target, scale);
	CHECK((x == 1823.0f) && (y == 767.0f));

	/* the inverse of the draw: every session pixel comes back within one window pixel */
	for (int32_t sx = source.x; sx < source.x + source.w; sx += 37)
	{
		for (int32_t sy = source.y; sy < source.y + source.h; sy += 29)
		{
			const auto d = sdl_server_layout::toWindow({ sx, sy, 1, 1 }, source, target, scale);
			CHECK(d);
			const auto [bx, by] = sdl_server_layout::toSession(
			    static_cast<float>(d->target.x) + 0.5f, static_cast<float>(d->target.y) + 0.5f,
			    source, target, scale);
			CHECK((static_cast<int32_t>(bx) == sx) && (static_cast<int32_t>(by) == sy));
		}
	}
	return true;
}

int main()
{
	for (auto* check :
	     { checkCopy, checkNewestWins, checkEndReplaces, checkThreads, checkEchoIsSame,
	       checkOtherIsNotSame, checkShadowTwoMonitors, checkOrderByPlace, checkOneToOne,
	       checkKeepToday, checkDrawInWindow, checkInputFromWindow })
	{
		if (!check())
			return -1;
	}
	(void)printf("server layout: ok\n");
	return 0;
}
