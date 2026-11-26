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

using sdl_server_layout::Inbox;
using sdl_server_layout::Layout;

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

int main()
{
	for (auto* check : { checkCopy, checkNewestWins, checkEndReplaces, checkThreads })
	{
		if (!check())
			return -1;
	}
	(void)printf("server layout: ok\n");
	return 0;
}
