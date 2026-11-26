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

#include <cstdint>
#include <mutex>
#include <optional>
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
} // namespace sdl_server_layout
