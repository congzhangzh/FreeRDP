/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2011-2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include <windows.h>
#include <winuser.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/constants.h>
#include <freerdp/log.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>
#include <freerdp/server/server-common.h>

#include "win_shadow.h"
#include "win_sas.h"
#include "win_audio.h"

#if defined(CHANNEL_CLIPRDR_SERVER)
#include "win_shadow_cliprdr.h"
#endif

#define TAG SERVER_TAG("shadow.win")

/* https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event
 * does not mention this flag is only supported if building for _WIN32_WINNT >= 0x0600
 */
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

/* The desktop this thread opened to follow the input desktop, if any */
static WINPR_TLS HDESK win_shadow_thread_desktop = nullptr;

/* Moves the calling thread to the input desktop when that changed: SendInput
 * and DXGI desktop duplication only work there. The input desktop is the
 * user's (Default) or, as SYSTEM on the console session (the service mode),
 * the secure one (Winlogon: the logon and lock screens, UAC prompts). A user
 * process cannot open the secure desktop and stays where it is. Returns TRUE
 * when the thread moved. */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_follow_input_desktop(void)
{
	WCHAR inputName[64] = WINPR_C_ARRAY_INIT;
	WCHAR threadName[64] = WINPR_C_ARRAY_INIT;
	HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);

	if (!input)
		return FALSE;

	if (!GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), nullptr))
		WLog_WARN(TAG, "GetUserObjectInformationW(input desktop, UOI_NAME) failed: 0x%08lX",
		          GetLastError());
	if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, threadName,
	                               sizeof(threadName), nullptr))
		WLog_WARN(TAG, "GetUserObjectInformationW(thread desktop, UOI_NAME) failed: 0x%08lX",
		          GetLastError());

	if (_wcsicmp(inputName, threadName) == 0)
	{
		(void)CloseDesktop(input);
		return FALSE;
	}

	if (!SetThreadDesktop(input))
	{
		WLog_WARN(TAG, "SetThreadDesktop(%ls) failed: 0x%08lX", inputName, GetLastError());
		(void)CloseDesktop(input);
		return FALSE;
	}

	/* the desktop GetThreadDesktop() gave first is not ours to close */
	if (win_shadow_thread_desktop)
		(void)CloseDesktop(win_shadow_thread_desktop);
	win_shadow_thread_desktop = input;
	WLog_INFO(TAG, "thread %lu follows the input desktop from %ls to %ls", GetCurrentThreadId(),
	          threadName, inputName);
	return TRUE;
}

/* SendInput, without ending the session when Windows refuses the event: it
 * does while the input desktop switches (the secure desktop comes or goes)
 * and for a window above our integrity level, and FreeRDP drops the client
 * when an input handler fails. The failure log names only the input kind:
 * the key codes themselves must never reach the log. */
static void win_shadow_send_input(const char* what, INPUT* event)
{
	WINPR_ASSERT(what);
	WINPR_ASSERT(event);

	if (SendInput(1, event, sizeof(INPUT)) == 0)
		WLog_WARN(TAG, "%s: SendInput refused the event (0x%08lX), it is dropped", what,
		          GetLastError());
}

/* UIPI: a process below High integrity (a scheduled task defaults to Medium)
 * may not send input to windows of a higher integrity level (an elevated
 * foreground application), and SendInput does not report the refusal: it
 * returns success and does not set the last error. The screen capture is
 * unaffected, so such a shadow serves a perfectly fine picture while every
 * keystroke and mouse click into an elevated window is silently dropped.
 * Say so at startup: this turns a "keyboard and mouse dead while picture is
 * fine" report into a log line readable in seconds.
 * https://learn.microsoft.com/en-us/windows/win32/secauthz/how-uipi-works */
static void win_shadow_check_input_integrity(void)
{
	HANDLE token = nullptr;
	DWORD length = 0;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		WLog_WARN(TAG, "OpenProcessToken failed: cannot read the process integrity level (0x%08lX)",
		          GetLastError());
		return;
	}

	/* the integrity level RID is the last sub-authority of the mandatory
	 * label SID (S-1-16-<rid>) */
	if (!GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &length) &&
	    (GetLastError() == ERROR_INSUFFICIENT_BUFFER))
	{
		PTOKEN_MANDATORY_LABEL label = (PTOKEN_MANDATORY_LABEL)malloc(length);

		if (!label)
			WLog_WARN(TAG, "out of memory: cannot read the process integrity level");
		else
		{
			if (GetTokenInformation(token, TokenIntegrityLevel, label, length, &length) &&
			    label->Label.Sid)
			{
				const PSID sid = label->Label.Sid;
				const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);

				if (rid < SECURITY_MANDATORY_HIGH_RID)
					WLog_WARN(TAG,
					          "process is NOT elevated (integrity level 0x%04lX, Medium or lower): "
					          "injected input to elevated windows will be silently dropped by UIPI "
					          "- run the shadow elevated (e.g. scheduled task RunLevel Highest) or "
					          "expect \"keyboard and mouse dead while picture is fine\" reports",
					          rid);
				else
					WLog_DBG(TAG,
					         "process integrity level 0x%04lX: injected input reaches elevated "
					         "windows",
					         rid);
			}
			else
				WLog_WARN(
				    TAG,
				    "GetTokenInformation(TokenIntegrityLevel) failed: cannot read the process "
				    "integrity level (0x%08lX)",
				    GetLastError());

			free(label);
		}
	}

	(void)CloseHandle(token);
}

/* Ctrl+Alt+Del: asks freerdp-shadow-service for the secure attention
 * sequence (win_sas.h) */
static void win_shadow_send_sas(void)
{
	HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, WIN_SHADOW_SAS_EVENT);

	if (!event)
	{
		WLog_WARN(TAG, "Ctrl+Alt+Del: no freerdp-shadow-service to send it (0x%08lX)",
		          GetLastError());
		return;
	}

	if (SetEvent(event))
		WLog_INFO(TAG,
		          "Ctrl+Alt+Del: asked freerdp-shadow-service for the secure attention sequence");
	else
		WLog_WARN(TAG, "Ctrl+Alt+Del: SetEvent failed (0x%08lX)", GetLastError());
	(void)CloseHandle(event);
}

static BOOL win_shadow_input_synchronize_event(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client, UINT32 flags)
{
	WLog_WARN(TAG, "TODO: Implement!");
	return TRUE;
}

static BOOL win_shadow_input_keyboard_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                            UINT16 flags, UINT8 code)
{
	winShadowSubsystem* win = (winShadowSubsystem*)subsystem;
	const BOOL down = (flags & KBD_FLAGS_RELEASE) == 0;

	WINPR_ASSERT(win);
	(void)win_shadow_follow_input_desktop();

	/* scan codes: Ctrl 0x1D, Alt 0x38 (the right ones extended), Delete
	 * 0x53 (the keypad's not extended); Windows takes either Delete */
	if (code == 0x1D)
		win->ctrlDown = down;
	else if (code == 0x38)
		win->altDown = down;
	else if ((code == 0x53) && win->ctrlDown && win->altDown)
	{
		if (down)
			win_shadow_send_sas();
		return TRUE; /* never typed into the desktop */
	}
	INPUT event = WINPR_C_ARRAY_INIT;
	event.type = INPUT_KEYBOARD;
	KEYBDINPUT* ki =
#if defined(NONAMELESSUNION)
	    &event.u.ki;
#else
	    &event.ki;
#endif

	ki->wVk = 0;
	ki->wScan = code;
	ki->dwFlags = KEYEVENTF_SCANCODE;
	ki->dwExtraInfo = 0;
	ki->time = 0;

	if (flags & KBD_FLAGS_RELEASE)
		ki->dwFlags |= KEYEVENTF_KEYUP;

	if (flags & KBD_FLAGS_EXTENDED)
		ki->dwFlags |= KEYEVENTF_EXTENDEDKEY;

	win_shadow_send_input("keyboard", &event);
	return TRUE;
}

static BOOL win_shadow_input_unicode_keyboard_event(rdpShadowSubsystem* subsystem,
                                                    rdpShadowClient* client, UINT16 flags,
                                                    UINT16 code)
{
	(void)win_shadow_follow_input_desktop();
	INPUT event = WINPR_C_ARRAY_INIT;
	event.type = INPUT_KEYBOARD;
	KEYBDINPUT* ki =
#if defined(NONAMELESSUNION)
	    &event.u.ki;
#else
	    &event.ki;
#endif
	ki->wVk = 0;
	ki->wScan = code;
	ki->dwFlags = KEYEVENTF_UNICODE;
	ki->dwExtraInfo = 0;
	ki->time = 0;

	if (flags & KBD_FLAGS_RELEASE)
		ki->dwFlags |= KEYEVENTF_KEYUP;

	win_shadow_send_input("unicode keyboard", &event);
	return TRUE;
}

static BOOL win_shadow_input_mouse_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                         UINT16 flags, UINT16 x, UINT16 y)
{
	(void)win_shadow_follow_input_desktop();
	INPUT event = WINPR_C_ARRAY_INIT;
	float width;
	float height;
	MOUSEINPUT* mi =
#if defined(NONAMELESSUNION)
	    &event.u.mi;
#else
	    &event.mi;
#endif

	event.type = INPUT_MOUSE;

	if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))
	{
		if (flags & PTR_FLAGS_WHEEL)
			mi->dwFlags = MOUSEEVENTF_WHEEL;
		else
			mi->dwFlags = MOUSEEVENTF_HWHEEL;
		mi->mouseData = flags & WheelRotationMask;

		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			mi->mouseData *= -1;

		win_shadow_send_input("mouse", &event);
	}
	else
	{
		/* Normalize against the captured framebuffer, not GetSystemMetrics:
		 * this process is not marked DPI aware, so the metrics are
		 * virtualized (scaled down) on a desktop with display scaling while
		 * the client coordinates are physical pixels of the primary screen
		 * the subsystem geometry describes. */
		width = (float)((winShadowSubsystem*)subsystem)->width;
		height = (float)((winShadowSubsystem*)subsystem)->height;
		mi->dx = (LONG)((float)x * (65535.0f / width));
		mi->dy = (LONG)((float)y * (65535.0f / height));
		mi->dwFlags = MOUSEEVENTF_ABSOLUTE;

		if (flags & PTR_FLAGS_MOVE)
		{
			mi->dwFlags |= MOUSEEVENTF_MOVE;
			win_shadow_send_input("mouse", &event);
		}

		mi->dwFlags = MOUSEEVENTF_ABSOLUTE;

		if (flags & PTR_FLAGS_BUTTON1)
		{
			if (flags & PTR_FLAGS_DOWN)
				mi->dwFlags |= MOUSEEVENTF_LEFTDOWN;
			else
				mi->dwFlags |= MOUSEEVENTF_LEFTUP;

			win_shadow_send_input("mouse", &event);
		}
		else if (flags & PTR_FLAGS_BUTTON2)
		{
			if (flags & PTR_FLAGS_DOWN)
				mi->dwFlags |= MOUSEEVENTF_RIGHTDOWN;
			else
				mi->dwFlags |= MOUSEEVENTF_RIGHTUP;

			win_shadow_send_input("mouse", &event);
		}
		else if (flags & PTR_FLAGS_BUTTON3)
		{
			if (flags & PTR_FLAGS_DOWN)
				mi->dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
			else
				mi->dwFlags |= MOUSEEVENTF_MIDDLEUP;

			win_shadow_send_input("mouse", &event);
		}
	}

	return TRUE;
}

static BOOL win_shadow_input_extended_mouse_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client, UINT16 flags, UINT16 x,
                                                  UINT16 y)
{
	(void)win_shadow_follow_input_desktop();
	INPUT event = WINPR_C_ARRAY_INIT;
	float width;
	float height;
	MOUSEINPUT* mi =
#if defined(NONAMELESSUNION)
	    &event.u.mi;
#else
	    &event.mi;
#endif

	if ((flags & PTR_XFLAGS_BUTTON1) || (flags & PTR_XFLAGS_BUTTON2))
	{
		event.type = INPUT_MOUSE;

		if (flags & PTR_FLAGS_MOVE)
		{
			/* same as above: the physical framebuffer, not virtualized
			 * GetSystemMetrics values, is the coordinate domain */
			width = (float)((winShadowSubsystem*)subsystem)->width;
			height = (float)((winShadowSubsystem*)subsystem)->height;
			mi->dx = (LONG)((float)x * (65535.0f / width));
			mi->dy = (LONG)((float)y * (65535.0f / height));
			mi->dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
			win_shadow_send_input("extended mouse", &event);
		}

		mi->dx = mi->dy = mi->dwFlags = 0;

		if (flags & PTR_XFLAGS_DOWN)
			mi->dwFlags |= MOUSEEVENTF_XDOWN;
		else
			mi->dwFlags |= MOUSEEVENTF_XUP;

		if (flags & PTR_XFLAGS_BUTTON1)
			mi->mouseData = XBUTTON1;
		else if (flags & PTR_XFLAGS_BUTTON2)
			mi->mouseData = XBUTTON2;

		win_shadow_send_input("extended mouse", &event);
	}

	return TRUE;
}

static int win_shadow_surface_copy(winShadowSubsystem* subsystem)
{
	rdpShadowServer* server;
	rdpShadowSurface* surface;
	RECTANGLE_16 surfaceRect;
	server = subsystem->base.server;
	surface = server->surface;

	if (ArrayList_Count(server->clients) < 1)
		return 1;

	/* the damage is in the surface's own coordinates: its top left is 0,0 here, wherever the
	 * surface is on the desktop (surface->x, surface->y) */
	surfaceRect.left = 0;
	surfaceRect.top = 0;
	surfaceRect.right = (UINT16)surface->width;
	surfaceRect.bottom = (UINT16)surface->height;
	region16_intersect_rect(&(surface->invalidRegion), &(surface->invalidRegion), &surfaceRect);

	if (region16_is_empty(&(surface->invalidRegion)))
		return 1;

#if defined(WITH_DXGI_1_2)
	{
		/* every output at its place in the surface; the clients encode the damage copied, the
		 * damage of an output without a frame now waits for its next one */
		REGION16 pending = WINPR_C_ARRAY_INIT;
		region16_init(&pending);
		const int status = win_shadow_dxgi_copy_outputs(subsystem, &pending);

		/* see below: no lock held across the wait */
		if (status > 0)
			shadow_subsystem_frame_update(&subsystem->base);

		(void)region16_copy(&(surface->invalidRegion), &pending);
		region16_uninit(&pending);
		return (status > 0) ? 1 : status;
	}
#elif defined(WITH_WDS_API)
	{
		int x, y;
		int width;
		int height;
		RECTANGLE_16 invalidRect;
		const RECTANGLE_16* extents;
		rdpGdi* gdi;
		shwContext* shw;
		rdpContext* context;

		extents = region16_extents(&(surface->invalidRegion));
		CopyMemory(&invalidRect, extents, sizeof(RECTANGLE_16));
		shadow_capture_align_clip_rect(&invalidRect, &surfaceRect);
		x = invalidRect.left;
		y = invalidRect.top;
		width = invalidRect.right - invalidRect.left;
		height = invalidRect.bottom - invalidRect.top;

		WLog_INFO(TAG, "SurfaceCopy x: %d y: %d width: %d height: %d right: %d bottom: %d", x, y,
		          width, height, x + width, y + height);

		WINPR_ASSERT(subsystem);
		shw = subsystem->shw;
		WINPR_ASSERT(shw);

		context = &shw->common.context;
		WINPR_ASSERT(context);

		gdi = context->gdi;
		WINPR_ASSERT(gdi);

		/* The GDI primary buffer spans the whole desktop, the surface is its part at
		 * surface->x, surface->y */
		const int srcX = surface->x + x;
		const int srcY = surface->y + y;

		/* The inner framebuffer is owned by the WDS attendee client. It can be
		 * absent before its first frame, or smaller than the surface while a
		 * desktop resize is in flight: copying past its bounds crashes the
		 * server inside freerdp_image_copy_no_overlap's memcpy. Clamp the copy
		 * to what is actually there and skip the frame when nothing overlaps. */
		if (!gdi || !gdi->primary_buffer || (gdi->width <= 0) || (gdi->height <= 0))
		{
			region16_clear(&(surface->invalidRegion));
			return 0;
		}
		if (srcX + width > gdi->width)
			width = gdi->width - srcX;
		if (srcY + height > gdi->height)
			height = gdi->height - srcY;
		if ((width <= 0) || (height <= 0))
		{
			region16_clear(&(surface->invalidRegion));
			return 1;
		}

		if (!freerdp_image_copy_no_overlap(surface->data, surface->format, surface->scanline, x, y,
		                                   width, height, gdi->primary_buffer, gdi->dstFormat,
		                                   gdi->width * 4, srcX, srcY, nullptr, FREERDP_FLIP_NONE))
			return ERROR_INTERNAL_ERROR;

		region16_clear(&(surface->invalidRegion));
	}
#else
	/* no capture backend compiled in */
	region16_clear(&(surface->invalidRegion));
	return 1;
#endif

	/* shadow_subsystem_frame_update() publishes the frame to all subscriber
	 * clients and waits until every one of them consumed it. That wait must
	 * not happen while the server clients list is locked: the listener
	 * thread needs this lock in shadow_client_accepted() (ArrayList_Append)
	 * and shadow_server_check_peer_restrictions() (ArrayList Count), so
	 * holding it across an unresponsive subscriber would stop accepting new
	 * connections entirely. The frame publication iterates the update
	 * event's own subscriber list, not the server clients list, so the lock
	 * is not needed here at all. */
	shadow_subsystem_frame_update(&subsystem->base);
	return 1;
}

#if defined(WITH_WDS_API)

static DWORD WINAPI win_shadow_subsystem_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	DWORD status;
	DWORD nCount;
	HANDLE events[32];
	HANDLE StopEvent;
	StopEvent = subsystem->base.server->StopEvent;
	nCount = 0;
	events[nCount++] = StopEvent;
	events[nCount++] = subsystem->RdpUpdateEnterEvent;

	while (1)
	{
		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		if (WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		if (WaitForSingleObject(subsystem->RdpUpdateEnterEvent, 0) == WAIT_OBJECT_0)
		{
			/* the shared desktop can be resized between two updates: keep the
			 * shadow screen in step before the copy, as the DXGI variant does */
			(void)win_shadow_check_resize(subsystem);
			win_shadow_surface_copy(subsystem);
			(void)ResetEvent(subsystem->RdpUpdateEnterEvent);
			(void)SetEvent(subsystem->RdpUpdateLeaveEvent);
		}
	}

	ExitThread(0);
	return 0;
}

#elif defined(WITH_DXGI_1_2)

static DWORD WINAPI win_shadow_subsystem_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	DWORD status;
	DWORD nCount;
	UINT64 cTime;
	DWORD dwTimeout;
	DWORD dwInterval;
	UINT64 frameTime;
	HANDLE events[32];
	HANDLE StopEvent;
	WINPR_ASSERT(subsystem->base.server);
	StopEvent = subsystem->base.server->StopEvent;
	wMessagePipe* MsgPipe = subsystem->base.MsgPipe;
	WINPR_ASSERT(MsgPipe);
	nCount = 0;
	events[nCount++] = StopEvent;
	events[nCount++] = MessageQueue_Event(MsgPipe->In);
	/* /framerate caps the first rate too; the encoders' suggestion takes over from there */
	subsystem->base.captureFrameRate =
	    (subsystem->base.server->maxFrameRate > 0) ? subsystem->base.server->maxFrameRate : 120;
	dwInterval = 1000 / subsystem->base.captureFrameRate;
	frameTime = GetTickCount64() + dwInterval;
	BOOL refresh = FALSE;

	while (1)
	{
		dwTimeout = INFINITE;
		cTime = GetTickCount64();
		dwTimeout = (DWORD)((cTime > frameTime) ? 0 : frameTime - cTime);
		status = WaitForMultipleObjects(nCount, events, FALSE, dwTimeout);

		if (WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		/* a client asks for the whole screen (its graphics channel is
		 * ready, or it asked to refresh), as the X11 subsystem answers */
		wMessage message = WINPR_C_ARRAY_INIT;
		BOOL quit = FALSE;
		while (!quit && MessageQueue_Peek(MsgPipe->In, &message, TRUE))
		{
			quit = (message.id == WMQ_QUIT);
			if (message.id == SHADOW_MSG_IN_REFRESH_REQUEST_ID)
				refresh = TRUE;
			if (message.Free)
				message.Free(&message);
		}
		if (quit)
			break;

		if ((status == WAIT_TIMEOUT) || (GetTickCount64() > frameTime))
		{
			const int resize_status = win_shadow_check_resize(subsystem);
			BOOL copy = FALSE;

			/* apply a pending desktop display change before the
			 * restart/acquire below; on fatal resize error (<0) skip this
			 * round, the next tick retries */
			if (resize_status >= 0)
			{
				rdpShadowSurface* surface = subsystem->base.server->surface;
				WINPR_ASSERT(surface);

				/* a desktop switch (logon, lock screen, UAC), a refresh request,
				 * or no duplication yet: duplicate the outputs again on the input
				 * desktop, and send all of them. DXGI reports changes only: on a
				 * desktop that does not change, a new client would otherwise wait
				 * for its first picture until something moves; a new duplication
				 * starts with the whole desktop image. A missing duplication is
				 * retried from its dxgiRetryTime on, not on every pass: every
				 * WIN_SHADOW_DXGI_DENIED_RETRY_MS while a desktop this process
				 * may not capture is active, every WIN_SHADOW_DXGI_INIT_RETRY_MS
				 * after a failure which does not recover by itself (no D3D11
				 * device, an output which refuses the duplication) */
				const BOOL followed = win_shadow_follow_input_desktop();
				/* a desktop change leaves the duplications behind on the old
				 * geometry: win_shadow_check_resize has just resized the shadow
				 * screen, so the next frame would be copied from objects of the
				 * previous layout. Set the outputs up again for the new one and
				 * send all of it, as for a desktop switch. */
				const BOOL resized = (resize_status > 0);
				const BOOL moved = (subsystem->outputsSurfaceX != surface->x) ||
				                   (subsystem->outputsSurfaceY != surface->y) ||
				                   (subsystem->outputsSurfaceWidth != surface->width) ||
				                   (subsystem->outputsSurfaceHeight != surface->height);
				BOOL restart = followed || refresh || resized;
				refresh = FALSE;

				if ((resized || moved || !subsystem->outputsMapped) &&
				    (GetTickCount64() >= subsystem->outputsRetryTime))
				{
					if (win_shadow_dxgi_map_outputs(subsystem) < 0)
					{
						subsystem->outputsMapped = FALSE;
						subsystem->outputsRetryTime =
						    GetTickCount64() + WIN_SHADOW_OUTPUTS_RETRY_MS;
					}
					restart = TRUE;
				}

				for (UINT32 index = 0; index < subsystem->outputCount; index++)
				{
					win_shadow_dxgi_select(subsystem, index);

					const BOOL retry = !subsystem->dxgi->dxgiOutputDuplication &&
					                   (GetTickCount64() >= subsystem->dxgi->dxgiRetryTime);
					if (restart || retry)
					{
						if (win_shadow_dxgi_restart_duplication(subsystem) > 0)
							win_shadow_dxgi_invalidate_output(subsystem);
					}

					/* without a duplication (capture paused, or a failed restart)
					 * this output has no frame */
					const int frame = subsystem->dxgi->dxgiOutputDuplication
					                      ? win_shadow_dxgi_get_next_frame(subsystem)
					                      : 0;

					/* a frame without dirty rectangles of its own (the first of a
					 * new duplication) still holds the whole image: what is marked
					 * invalid already is copied. A frame whose rectangles cannot
					 * be read is copied whole: its damage is not lost. */
					if ((frame > 0) && (win_shadow_dxgi_get_invalid_region(subsystem) < 0))
						win_shadow_dxgi_invalidate_output(subsystem);
				}

				if (win_shadow_dxgi_frame_acquired(subsystem))
				{
					EnterCriticalSection(&(surface->lock));
					copy = !region16_is_empty(&(surface->invalidRegion));
					LeaveCriticalSection(&(surface->lock));
				}
			}

			if (copy)
				win_shadow_surface_copy(subsystem);

			/* Follow the encoder capture rate suggestion, like the X11 and Mac
			 * subsystems do: shadow_encoder_create_frame_id() lowers the
			 * preferred fps while frames are still in flight (not yet
			 * acknowledged by the client). This subsystem hardcoded the
			 * capture rate and never read the suggestion, so a slow client
			 * could not throttle the capture and in-flight frames (latency)
			 * accumulated without bound. Use the lowest suggestion of all
			 * connected clients. */
			UINT32 captureFrameRate = UINT32_MAX;
			ArrayList_Lock(subsystem->base.server->clients);
			const UINT32 clientCount = ArrayList_Count(subsystem->base.server->clients);
			for (UINT32 index = 0; index < clientCount; index++)
			{
				const rdpShadowClient* client =
				    (rdpShadowClient*)ArrayList_GetItem(subsystem->base.server->clients, index);

				if (client && client->encoder)
				{
					const UINT32 suggestion = shadow_encoder_preferred_fps(client->encoder);
					if (suggestion < captureFrameRate)
						captureFrameRate = suggestion;
				}
			}
			ArrayList_Unlock(subsystem->base.server->clients);

			if ((captureFrameRate == UINT32_MAX) || (captureFrameRate < 1))
				captureFrameRate = subsystem->base.captureFrameRate;
			subsystem->base.captureFrameRate = captureFrameRate;

			dwInterval = 1000 / subsystem->base.captureFrameRate;
			frameTime += dwInterval;
		}
	}

	ExitThread(0);
	return 0;
}

#endif

typedef struct
{
	MONITOR_DEF* monitors;
	UINT32 max;
	UINT32 count;
} winMonitorEnum;

static BOOL CALLBACK win_shadow_monitor_proc(HMONITOR hMonitor, WINPR_ATTR_UNUSED HDC hdc,
                                             WINPR_ATTR_UNUSED LPRECT rect, LPARAM param)
{
	winMonitorEnum* list = (winMonitorEnum*)param;
	MONITORINFO info = WINPR_C_ARRAY_INIT;

	WINPR_ASSERT(list);

	if (list->count >= list->max)
		return FALSE;

	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(hMonitor, &info) || (info.rcMonitor.right <= info.rcMonitor.left) ||
	    (info.rcMonitor.bottom <= info.rcMonitor.top))
		return TRUE;

	/* right and bottom are inclusive, as the shadow server reads them */
	MONITOR_DEF* monitor = &list->monitors[list->count++];
	monitor->left = info.rcMonitor.left;
	monitor->top = info.rcMonitor.top;
	monitor->right = info.rcMonitor.right - 1;
	monitor->bottom = info.rcMonitor.bottom - 1;
	monitor->flags = (info.dwFlags & MONITORINFOF_PRIMARY) ? MONITOR_PRIMARY : 0;
	return TRUE;
}

/* The monitors of the desktop in the server's form (shadow_monitors_normalize: the primary
 * first, the top left of their bounding box at 0,0), and where that top left is on the
 * virtual desktop */
static UINT32 win_shadow_enum_monitors_at(MONITOR_DEF* monitors, UINT32 maxMonitors, INT32* pLeft,
                                          INT32* pTop)
{
	winMonitorEnum list = { monitors, maxMonitors, 0 };
	INT32 left = 0;
	INT32 top = 0;

	if (!monitors || (maxMonitors < 1))
		return 0;

	/* FALSE also when the callback stopped at maxMonitors: keep what it found */
	(void)EnumDisplayMonitors(nullptr, nullptr, win_shadow_monitor_proc, (LPARAM)&list);

	for (UINT32 index = 0; index < list.count; index++)
	{
		if ((index == 0) || (monitors[index].left < left))
			left = monitors[index].left;
		if ((index == 0) || (monitors[index].top < top))
			top = monitors[index].top;
	}

	/* Fallback to the primary screen metrics if enumeration did not
	 * yield a usable rectangle, otherwise shadow_screen_new computes a
	 * 1x1 surface and clients are resized to 1x1. The primary's top left
	 * is the virtual desktop's 0,0. */
	if ((list.count == 0) || !shadow_monitors_normalize(monitors, list.count))
	{
		WLog_WARN(TAG, "no usable monitor list (%" PRIu32 " found): sharing the primary screen",
		          list.count);
		MONITOR_DEF* monitor = &monitors[0];
		monitor->left = 0;
		monitor->top = 0;
		monitor->right = GetSystemMetrics(SM_CXSCREEN) - 1;
		monitor->bottom = GetSystemMetrics(SM_CYSCREEN) - 1;
		monitor->flags = MONITOR_PRIMARY;
		list.count = 1;
		left = 0;
		top = 0;
	}

	if (pLeft)
		*pLeft = left;
	if (pTop)
		*pTop = top;
	return list.count;
}

static UINT32 win_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
	return win_shadow_enum_monitors_at(monitors, maxMonitors, nullptr, nullptr);
}

/* The subsystem's size is that of the part of the desktop the server shares: its surface, or
 * the selected monitor while there is none yet (the screen is made after the subsystem) */
static void win_shadow_update_size(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	const rdpShadowSurface* surface =
	    subsystem->base.server ? subsystem->base.server->surface : nullptr;
	if (surface)
	{
		subsystem->width = (int)surface->width;
		subsystem->height = (int)surface->height;
	}
	else if (subsystem->base.selectedMonitor < subsystem->base.numMonitors)
	{
		const MONITOR_DEF* monitor = &subsystem->base.monitors[subsystem->base.selectedMonitor];
		subsystem->width = monitor->right - monitor->left + 1;
		subsystem->height = monitor->bottom - monitor->top + 1;
	}

	MONITOR_DEF* virtualScreen = &(subsystem->base.virtualScreen);
	virtualScreen->left = 0;
	virtualScreen->top = 0;
	virtualScreen->right = 0;
	virtualScreen->bottom = 0;
	for (UINT32 index = 0; index < subsystem->base.numMonitors; index++)
	{
		const MONITOR_DEF* monitor = &subsystem->base.monitors[index];
		virtualScreen->right = MAX(virtualScreen->right, monitor->right);
		virtualScreen->bottom = MAX(virtualScreen->bottom, monitor->bottom);
	}
	virtualScreen->flags = 1;
}

/* Win counterpart of x11_shadow_check_resize: track desktop display
 * changes while the server is running (a display mode change, a monitor
 * added, removed or moved, another primary). Returns 1 when the geometry
 * changed, 0 when it did not and <0 on fatal error. */
int win_shadow_check_resize(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	MONITOR_DEF monitors[ARRAYSIZE(subsystem->base.monitors)] = WINPR_C_ARRAY_INIT;
	INT32 left = 0;
	INT32 top = 0;
	const UINT32 count = win_shadow_enum_monitors_at(monitors, ARRAYSIZE(monitors), &left, &top);

	if (count < 1)
		return 0;

	BOOL same = (count == subsystem->base.numMonitors) && (left == subsystem->desktopLeft) &&
	            (top == subsystem->desktopTop);
	for (UINT32 index = 0; same && (index < count); index++)
	{
		const MONITOR_DEF* a = &monitors[index];
		const MONITOR_DEF* b = &subsystem->base.monitors[index];
		same = (a->left == b->left) && (a->top == b->top) && (a->right == b->right) &&
		       (a->bottom == b->bottom) && (a->flags == b->flags);
	}
	if (same)
		return 0;

	CopyMemory(subsystem->base.monitors, monitors, sizeof(MONITOR_DEF) * count);
	subsystem->base.numMonitors = count;
	subsystem->desktopLeft = left;
	subsystem->desktopTop = top;

	if (subsystem->base.selectedMonitor >= count)
	{
		WLog_WARN(TAG, "monitor %" PRIu32 " is gone: sharing the primary one",
		          subsystem->base.selectedMonitor);
		subsystem->base.selectedMonitor = 0;
	}

	/* The screen does not exist yet while the subsystem is initialized;
	 * in that case updating the geometry below is all that is needed. */
	if (subsystem->base.server && subsystem->base.server->screen &&
	    !shadow_screen_resize(subsystem->base.server->screen))
		return -1;

	win_shadow_update_size(subsystem);

	WLog_INFO(TAG, "desktop changed: %" PRIu32 " monitor(s), sharing %dx%d", count,
	          subsystem->width, subsystem->height);
	for (UINT32 index = 0; index < count; index++)
	{
		const MONITOR_DEF* monitor = &monitors[index];
		WLog_INFO(
		    TAG, "  monitor %" PRIu32 ": %" PRId32 "x%" PRId32 " at %" PRId32 ",%" PRId32 "%s",
		    index, monitor->right - monitor->left + 1, monitor->bottom - monitor->top + 1,
		    monitor->left, monitor->top, (monitor->flags & MONITOR_PRIMARY) ? " (primary)" : "");
	}
	return 1;
}

static BOOL win_shadow_client_connect(rdpShadowSubsystem* subsystem, rdpShadowClient* client)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(client);

#if defined(CHANNEL_CLIPRDR_SERVER)
	/* cliprdr is a static channel: the join decision is final by the time
	 * ClientConnect runs (shadow_client_channels_post_connect precedes it). */
	if (WTSVirtualChannelManagerIsChannelJoined(client->vcm, CLIPRDR_SVC_CHANNEL_NAME))
	{
		if (win_shadow_cliprdr_init(client) < 0)
			return FALSE;
	}
#endif
	return TRUE;
}

static void win_shadow_client_disconnect(rdpShadowSubsystem* subsystem, rdpShadowClient* client)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(client);

#if defined(CHANNEL_CLIPRDR_SERVER)
	win_shadow_cliprdr_uninit(client);
#endif
}

static int win_shadow_subsystem_init(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	int status = 0;

	/* This binary carries no dpiAware manifest, so on a desktop with
	 * display scaling the process is virtualized: geometry APIs
	 * (GetSystemMetrics, display modes, the WDS capture coordinate space)
	 * report scaled-down values while the client and injected input speak
	 * physical pixels. Claim per-monitor awareness so all of them agree,
	 * falling back to system-wide awareness on older Windows. */
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
	if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
	    !SetProcessDPIAware())
		WLog_WARN(TAG, "DPI awareness setup failed: 0x%08lX", GetLastError());
#else
	if (!SetProcessDPIAware())
		WLog_WARN(TAG, "DPI awareness setup failed: 0x%08lX", GetLastError());
#endif

	/* what the clients see of this host (General Capability Set) */
	WINPR_ASSERT(arg->server);
	if (!freerdp_settings_set_uint32(arg->server->settings, FreeRDP_OsMajorType,
	                                 OSMAJORTYPE_WINDOWS) ||
	    !freerdp_settings_set_uint32(arg->server->settings, FreeRDP_OsMinorType,
	                                 OSMINORTYPE_WINDOWS_NT))
		return -1;

	win_shadow_check_input_integrity();
	subsystem->base.numMonitors =
	    win_shadow_enum_monitors_at(subsystem->base.monitors, ARRAYSIZE(subsystem->base.monitors),
	                                &subsystem->desktopLeft, &subsystem->desktopTop);
	if (subsystem->base.selectedMonitor >= subsystem->base.numMonitors)
		subsystem->base.selectedMonitor = 0;
	win_shadow_update_size(subsystem);

	if ((subsystem->width <= 0) || (subsystem->height <= 0))
	{
		subsystem->width = GetSystemMetrics(SM_CXSCREEN);
		subsystem->height = GetSystemMetrics(SM_CYSCREEN);
	}
#if defined(WITH_WDS_API)
	/* The Windows Desktop Sharing API is the low performance capture path:
	 * its throughput degrades with multiple clients and high frame rates.
	 * Which capture backend a binary carries is decided at build time (the
	 * alternative, DXGI desktop duplication, is WITH_SHADOW_WIN_DXGI=ON),
	 * so a binary holding only the slow path would degrade silently: say
	 * what was built in. */
	WLog_WARN(TAG,
	          "capture uses the Windows Desktop Sharing API: the low performance capture path, "
	          "throughput drops with multiple clients or high frame rates - build with "
	          "WITH_SHADOW_WIN_DXGI=ON for DXGI desktop duplication");
	status = win_shadow_wds_init(subsystem);
#elif defined(WITH_DXGI_1_2)
	/* a duplication per output, each at its place in the surface: every monitor unless
	 * /monitors:<index>. The outputs follow the surface, which the server makes after this:
	 * the capture thread duplicates them on its first pass. */
	subsystem->base.allMonitors = SHADOW_ALL_MONITORS_DEFAULT;
	status = 1;
#endif
	/* the capture retries on its own: a failed start is not fatal here */
	(void)status;
	WLog_INFO(TAG, "%" PRIu32 " monitor(s), width: %d height: %d", subsystem->base.numMonitors,
	          subsystem->width, subsystem->height);

	if (!win_shadow_audio_init(subsystem))
		WLog_WARN(TAG, "audio: no formats to offer: no sound");
	return 1;
}

static int win_shadow_subsystem_uninit(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return -1;

	win_shadow_audio_uninit(subsystem);
#if defined(WITH_WDS_API)
	win_shadow_wds_uninit(subsystem);
#elif defined(WITH_DXGI_1_2)
	win_shadow_dxgi_uninit(subsystem);
#endif
	return 1;
}

static int win_shadow_subsystem_start(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	HANDLE thread;

	if (!subsystem)
		return -1;

	/* the server made its screen after the subsystem's initialization: from now on the size
	 * is the surface's (all the monitors it shares) */
	win_shadow_update_size(subsystem);

	if (!(thread =
	          CreateThread(nullptr, 0, win_shadow_subsystem_thread, (void*)subsystem, 0, nullptr)))
	{
		WLog_ERR(TAG, "Failed to create thread");
		return -1;
	}

	/* the picture works without the sound */
	if (!win_shadow_audio_start(subsystem))
		WLog_WARN(TAG, "audio: no capture thread: no sound");
	return 1;
}

static int win_shadow_subsystem_stop(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return -1;

	win_shadow_audio_stop(subsystem);
	return 1;
}

static void win_shadow_subsystem_free(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return;

	win_shadow_subsystem_uninit(arg);
	free(subsystem);
}

static rdpShadowSubsystem* win_shadow_subsystem_new(void)
{
	winShadowSubsystem* subsystem;
	subsystem = (winShadowSubsystem*)calloc(1, sizeof(winShadowSubsystem));

	if (!subsystem)
		return nullptr;

	subsystem->base.SynchronizeEvent = win_shadow_input_synchronize_event;
	subsystem->base.KeyboardEvent = win_shadow_input_keyboard_event;
	subsystem->base.UnicodeKeyboardEvent = win_shadow_input_unicode_keyboard_event;
	subsystem->base.MouseEvent = win_shadow_input_mouse_event;
	subsystem->base.ExtendedMouseEvent = win_shadow_input_extended_mouse_event;
	subsystem->base.ClientConnect = win_shadow_client_connect;
	subsystem->base.ClientDisconnect = win_shadow_client_disconnect;
#if defined(WITH_DXGI_1_2)
	subsystem->dxgi = &subsystem->outputs[0];
#endif
	return &subsystem->base;
}

FREERDP_API const char* ShadowSubsystemName(void)
{
	return "Win";
}

FREERDP_API int ShadowSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints)
{
	char name[] = "windows shadow subsystem";
	char* arg[] = { name };

	freerdp_server_warn_unmaintained(ARRAYSIZE(arg), arg);
	pEntryPoints->New = win_shadow_subsystem_new;
	pEntryPoints->Free = win_shadow_subsystem_free;
	pEntryPoints->Init = win_shadow_subsystem_init;
	pEntryPoints->Uninit = win_shadow_subsystem_uninit;
	pEntryPoints->Start = win_shadow_subsystem_start;
	pEntryPoints->Stop = win_shadow_subsystem_stop;
	pEntryPoints->EnumMonitors = win_shadow_enum_monitors;
	return 1;
}
