/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Display Control Channel
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
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

#include <atomic>
#include <vector>

#include <freerdp/types.h>
#include <freerdp/event.h>
#include <freerdp/client/disp.h>

#include "sdl_types.hpp"

#include <SDL3/SDL.h>

class sdlDispContext
{

  public:
	explicit sdlDispContext(SdlContext* sdl);
	sdlDispContext(const sdlDispContext& other) = delete;
	sdlDispContext(sdlDispContext&& other) = delete;
	virtual ~sdlDispContext();

	sdlDispContext& operator=(const sdlDispContext& other) = delete;
	sdlDispContext& operator=(sdlDispContext&& other) = delete;

	[[nodiscard]] bool init(DispClientContext* disp);
	[[nodiscard]] bool uninit(DispClientContext* disp);

	[[nodiscard]] bool handleEvent(const SDL_DisplayEvent& ev);
	[[nodiscard]] bool handleEvent(const SDL_WindowEvent& ev);

	/** What an SDL_EVENT_USER_DISPLAY_LAYOUT asks for */
	enum LayoutRequest
	{
		LAYOUT_ARM = 0,  /**< send now if due and check again every second (addTimer) */
		LAYOUT_TICK = 1, /**< the timer: send if the layout changed */
		LAYOUT_CAPS = 2  /**< a new display channel: forget the layout sent, then arm */
	};

	/** On the event thread: the layout is read from the monitor array, which that thread
	 * replaces when a window or display changes, so it is only read and sent there. */
	[[nodiscard]] bool handleLayoutEvent(int request);

  private:
	[[nodiscard]] UINT DisplayControlCaps(UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA,
	                                      UINT32 maxMonitorAreaFactorB);
	[[nodiscard]] bool setWindowResizeable();

	[[nodiscard]] bool sendResize();
	[[nodiscard]] bool settings_changed(const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>& layout);
	[[nodiscard]] bool sendLayout(const rdpMonitor* monitors, size_t nmonitors);

	[[nodiscard]] bool addTimer();
	/** Any thread: ask the event thread for handleLayoutEvent(request) */
	[[nodiscard]] static bool requestLayout(LayoutRequest request);

	[[nodiscard]] bool updateMonitor(SDL_WindowID id);
	[[nodiscard]] bool updateMonitors(SDL_EventType type, SDL_DisplayID displayID);

	[[nodiscard]] static UINT DisplayControlCaps(DispClientContext* disp, UINT32 maxNumMonitors,
	                                             UINT32 maxMonitorAreaFactorA,
	                                             UINT32 maxMonitorAreaFactorB);
	static void OnActivated(void* context, const ActivatedEventArgs* e);
	static void OnGraphicsReset(void* context, const GraphicsResetEventArgs* e);
	[[nodiscard]] static Uint32 SDLCALL OnTimer(void* param, SDL_TimerID timerID, Uint32 interval);

	SdlContext* _sdl = nullptr;
	DispClientContext* _disp = nullptr;
	UINT64 _lastSentDate = 0;
	std::atomic<bool> _activated = false; /* set by the channel's thread */
	bool _waitingResize = false;
	SDL_TimerID _timer = 0;
	std::atomic<unsigned> _timer_retries = 0; /* counted by SDL's timer thread */
	SDL_ThreadID _eventThread = 0;
	std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> _last_sent_layout;
};
