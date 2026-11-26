/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client: the server's monitor layout, from a GFX Reset Graphics
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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <freerdp/settings_types.h>

/* A server that sends Reset Graphics ([MS-RDPEGFX] 2.2.2.14) names its monitors in it. The PDU
 * arrives on the channel's (drdynvc) thread, the windows live on SDL's thread: the channel thread
 * copies the list into an Inbox and wakes SDL's thread with an event that carries no pointer, and
 * SDL's thread takes the newest copy. Nothing here calls SDL, so it is tested without a display. */
namespace sdl_server_layout
{
	/** The desktop and its monitors as the server sent them (inclusive right and bottom) */
	struct Layout
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<MONITOR_DEF> monitors;
	};

	/** @return a copy of a Reset Graphics' desktop and monitors; none without a monitor array */
	[[nodiscard]] inline Layout copyOf(uint32_t width, uint32_t height, uint32_t count,
	                                   const MONITOR_DEF* monitors)
	{
		Layout layout;
		layout.width = width;
		layout.height = height;
		if (monitors)
			layout.monitors.assign(monitors, monitors + count);
		return layout;
	}

	/** One layout between two threads: the newest wins. A layout replaced before it was taken
	 * is freed by the replacement (value semantics); the event that woke the taker carries no
	 * pointer, so a late or extra event finds the inbox empty and does nothing. */
	class Inbox
	{
	  public:
		/** channel thread: keep @a layout in place of any not yet taken
		 * @return true if it replaced one (the older one is dropped) */
		bool post(Layout layout)
		{
			std::scoped_lock lock(_mux);
			const bool replaced = _pending.has_value();
			_pending = std::move(layout);
			_posted++;
			return replaced;
		}

		/** SDL's thread: the newest layout, once */
		[[nodiscard]] std::optional<Layout> take()
		{
			std::scoped_lock lock(_mux);
			auto layout = std::move(_pending);
			_pending.reset();
			return layout;
		}

		/** the session ended (the channel closed): drop what was not taken */
		void clear()
		{
			std::scoped_lock lock(_mux);
			_pending.reset();
		}

		/** @return how many layouts were posted since the start (for logs and tests) */
		[[nodiscard]] uint64_t posted() const
		{
			std::scoped_lock lock(_mux);
			return _posted;
		}

	  private:
		mutable std::mutex _mux;
		std::optional<Layout> _pending;
		uint64_t _posted = 0;
	};

	/** A rectangle in pixels, width and height exclusive */
	struct Rect
	{
		int32_t x = 0;
		int32_t y = 0;
		int32_t w = 0;
		int32_t h = 0;

		bool operator==(const Rect& o) const
		{
			return (x == o.x) && (y == o.y) && (w == o.w) && (h == o.h);
		}
		bool operator<(const Rect& o) const
		{
			return std::tie(x, y, w, h) < std::tie(o.x, o.y, o.w, o.h);
		}
	};

	/** @return a server monitor as a rectangle (its right and bottom are inclusive) */
	[[nodiscard]] inline Rect rectOf(const MONITOR_DEF& m)
	{
		return { m.left, m.top, m.right - m.left + 1, m.bottom - m.top + 1 };
	}

	/** @return the rectangles moved so that their common top left is 0,0, in one order: the
	 * same set of monitors gives the same list whatever origin and order it came in */
	[[nodiscard]] inline std::vector<Rect> normalized(std::vector<Rect> rects)
	{
		if (rects.empty())
			return rects;
		int32_t minX = rects.front().x;
		int32_t minY = rects.front().y;
		for (const auto& r : rects)
		{
			minX = std::min(minX, r.x);
			minY = std::min(minY, r.y);
		}
		for (auto& r : rects)
		{
			r.x -= minX;
			r.y -= minY;
		}
		std::sort(rects.begin(), rects.end());
		return rects;
	}

	/** @return true if the server's monitors are the layout the client sent: the same
	 * rectangles, in any order, with any origin (a server may put its primary first, or move
	 * the top left to 0,0). The primary flag is not compared: it places no window. */
	[[nodiscard]] inline bool sameLayout(const std::vector<MONITOR_DEF>& server,
	                                     const std::vector<Rect>& sent)
	{
		std::vector<Rect> rects;
		rects.reserve(server.size());
		for (const auto& m : server)
			rects.push_back(rectOf(m));
		return normalized(std::move(rects)) == normalized(sent);
	}

	/** A client screen a window can go to */
	struct Display
	{
		uint32_t id = 0; /* SDL_DisplayID */
		Rect bounds;     /* in the client's desktop, pixels */
		bool primary = false;
	};

	/** One window of a plan that follows the server */
	struct Window
	{
		size_t monitor = 0;   /* its index in the server's list */
		uint32_t display = 0; /* SDL_DisplayID */
		Rect source;          /* the part of the session it shows, session pixels */
		Rect target;          /* where that part is drawn, window pixels (letterboxed) */
		float scale = 1.0f;   /* window pixels per session pixel, the same both ways */
	};

	struct Plan
	{
		/** false: the windows stay as the client's own screens make them (as before) */
		bool follow = false;
		/** why, for the log */
		std::string reason;
		/** the server sent a list the client can't follow: log the reason as a warning */
		bool warn = false;
		std::vector<Window> windows;
	};

	/** @return the order windows take: the primary first, then left to right, top to bottom */
	template <typename T, typename Primary, typename Place>
	[[nodiscard]] std::vector<size_t> placeOrder(const std::vector<T>& items, Primary primary,
	                                             Place place)
	{
		std::vector<size_t> order(items.size());
		for (size_t x = 0; x < order.size(); x++)
			order[x] = x;
		std::stable_sort(order.begin(), order.end(),
		                 [&](size_t a, size_t b)
		                 {
			                 const bool pa = primary(items[a]);
			                 const bool pb = primary(items[b]);
			                 if (pa != pb)
				                 return pa;
			                 const Rect ra = place(items[a]);
			                 const Rect rb = place(items[b]);
			                 return std::tie(ra.x, ra.y) < std::tie(rb.x, rb.y);
		                 });
		return order;
	}

	/** @return @a source scaled to fit @a display's size, the aspect kept, centred (window
	 * pixels, the window covering the display) */
	[[nodiscard]] inline std::pair<Rect, float> letterbox(const Rect& source, const Rect& display)
	{
		const float sx = static_cast<float>(display.w) / static_cast<float>(source.w);
		const float sy = static_cast<float>(display.h) / static_cast<float>(source.h);
		const float scale = std::min(sx, sy);
		auto w = static_cast<int32_t>(static_cast<float>(source.w) * scale + 0.5f);
		auto h = static_cast<int32_t>(static_cast<float>(source.h) * scale + 0.5f);
		w = std::min(w, display.w);
		h = std::min(h, display.h);
		return { Rect{ (display.w - w) / 2, (display.h - h) / 2, w, h }, scale };
	}

	/** @return the windows for a server layout. The plan follows the server only with
	 * /multimon, a server list, and a list that is not the layout the client sent; each server
	 * monitor then gets a client screen, primary to primary, the others left to right, and is
	 * letterboxed there. More server monitors than client screens keep today's windows. */
	[[nodiscard]] inline Plan plan(bool multimon, const std::optional<Layout>& server,
	                               const std::vector<Rect>& sent,
	                               const std::vector<Display>& displays)
	{
		Plan p;
		if (!multimon)
		{
			p.reason = "no /multimon";
			return p;
		}
		if (!server || server->monitors.empty())
		{
			p.reason = "no server list";
			return p;
		}
		const auto& monitors = server->monitors;
		if (sameLayout(monitors, sent))
		{
			p.reason = "same as sent";
			return p;
		}
		if (monitors.size() > displays.size())
		{
			p.reason = "the server has " + std::to_string(monitors.size()) +
			           " monitors, the client " + std::to_string(displays.size()) + " screens";
			p.warn = true;
			return p;
		}

		/* session pixels: the desktop's top left is the monitors' common top left */
		int32_t minX = monitors.front().left;
		int32_t minY = monitors.front().top;
		for (const auto& m : monitors)
		{
			minX = std::min(minX, m.left);
			minY = std::min(minY, m.top);
		}
		for (size_t x = 0; x < monitors.size(); x++)
		{
			auto r = rectOf(monitors[x]);
			r.x -= minX;
			r.y -= minY;
			const bool inside = (r.w > 0) && (r.h > 0) &&
			                    (static_cast<int64_t>(r.x) + r.w <= server->width) &&
			                    (static_cast<int64_t>(r.y) + r.h <= server->height);
			if (!inside)
			{
				p.reason = "server monitor " + std::to_string(x) + " is not inside its " +
				           std::to_string(server->width) + "x" + std::to_string(server->height) +
				           " desktop";
				p.warn = true;
				return p;
			}
		}
		for (const auto& d : displays)
		{
			if ((d.bounds.w <= 0) || (d.bounds.h <= 0))
			{
				p.reason = "a client screen has no size";
				p.warn = true;
				return p;
			}
		}

		const auto serverOrder = placeOrder(
		    monitors, [](const MONITOR_DEF& m) { return (m.flags & MONITOR_PRIMARY) != 0; },
		    [](const MONITOR_DEF& m) { return rectOf(m); });
		const auto displayOrder = placeOrder(
		    displays, [](const Display& d) { return d.primary; },
		    [](const Display& d) { return d.bounds; });
		for (size_t x = 0; x < serverOrder.size(); x++)
		{
			const size_t index = serverOrder[x];
			const auto& display = displays[displayOrder[x]];
			Window w;
			w.monitor = index;
			w.display = display.id;
			w.source = rectOf(monitors[index]);
			w.source.x -= minX;
			w.source.y -= minY;
			std::tie(w.target, w.scale) = letterbox(w.source, display.bounds);
			p.windows.push_back(w);
		}
		p.follow = true;
		p.reason = "the server's " + std::to_string(monitors.size()) + " monitors";
		return p;
	}

	/** A damaged part of the session, as one window following the server draws it */
	struct Draw
	{
		Rect source; /* the damage inside the window's source, session pixels */
		Rect target; /* where it goes, window pixels: may pass the letterbox by the rounding,
		              * so the caller clips to the window's target */
	};

	/** @return @a damage clipped to @a source and placed in the window, none if it is not in
	 * the source (the damage of another monitor never reaches this window or its bars) */
	[[nodiscard]] inline std::optional<Draw> toWindow(const Rect& damage, const Rect& source,
	                                                  const Rect& target, float scale)
	{
		const int32_t x0 = std::max(damage.x, source.x);
		const int32_t y0 = std::max(damage.y, source.y);
		const int32_t x1 = std::min(damage.x + damage.w, source.x + source.w);
		const int32_t y1 = std::min(damage.y + damage.h, source.y + source.h);
		if ((x1 <= x0) || (y1 <= y0))
			return std::nullopt;
		Draw d;
		d.source = { x0, y0, x1 - x0, y1 - y0 };
		/* edges by rounding each one, so neighbouring damage meets without a gap */
		const auto edge = [&](int32_t at, int32_t from, int32_t base)
		{ return base + static_cast<int32_t>(static_cast<float>(at - from) * scale + 0.5f); };
		const int32_t l = edge(x0, source.x, target.x);
		const int32_t t = edge(y0, source.y, target.y);
		const int32_t r = edge(x1, source.x, target.x);
		const int32_t b = edge(y1, source.y, target.y);
		d.target = { l, t, std::max(r - l, 1), std::max(b - t, 1) };
		return d;
	}

	/** @return the session pixel of a window pixel (the inverse of toWindow), kept inside the
	 * source: a point on a bar goes to the source's nearest edge */
	[[nodiscard]] inline std::pair<float, float> toSession(float x, float y, const Rect& source,
	                                                       const Rect& target, float scale)
	{
		const float s = (scale > 0.0f) ? scale : 1.0f;
		float sx = static_cast<float>(source.x) + (x - static_cast<float>(target.x)) / s;
		float sy = static_cast<float>(source.y) + (y - static_cast<float>(target.y)) / s;
		const float maxX = static_cast<float>(source.x + source.w - 1);
		const float maxY = static_cast<float>(source.y + source.h - 1);
		sx = std::clamp(sx, static_cast<float>(source.x), maxX);
		sy = std::clamp(sy, static_cast<float>(source.y), maxY);
		return { sx, sy };
	}
} // namespace sdl_server_layout
