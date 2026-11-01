/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server: the agent in a session (capture, cursor and input for the server)
 *
 * Copyright 2026 Cong Zhang
 *
 * Copyright 2026 Cong Zhang
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

/* launchd starts the agent in the session on the console (a LaunchAgent for the LoginWindow
 * and Aqua sessions); it talks to the server, a root daemon, over XPC (see mac_ipc.h).
 * Screen Recording and Accessibility are granted to this program, not to the server. */

#include <freerdp/config.h>

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <IOSurface/IOSurfaceRef.h>
#include <ApplicationServices/ApplicationServices.h>

#include <winpr/wlog.h>
#include <freerdp/log.h>

#include "mac_capture.h"
#include "mac_cursor.h"
#include "mac_input.h"
#include "mac_ipc.h"

#define TAG SERVER_TAG("shadow.mac.agent")

#define MAC_AGENT_CURSOR_MS 30
#define MAC_AGENT_CURSOR_MAX 96
#define MAC_AGENT_MAX_DISPLAYS 16

static dispatch_queue_t g_queue;
static xpc_connection_t g_server;
static macInput g_input;
static macCapture *g_capture;
static macCaptureConfig g_config;
static dispatch_source_t g_cursorTimer;
static atomic_int g_inFlight;
static UINT32 g_cursorHash;
static BOOL g_cursorKnown;
static CGPoint g_location;
static BOOL g_locationKnown;

/* the displays the last hello announced (see agent_displays_tick) */
static UINT32 g_displaysHash;
static UINT32 agent_displays_hash(void);

static void agent_send(xpc_object_t msg)
{
	xpc_connection_send_message(g_server, msg);
	xpc_release(msg);
}

static xpc_object_t agent_message(const char *type)
{
	xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
	xpc_dictionary_set_string(msg, MAC_IPC_TYPE, type);
	return msg;
}

static void agent_send_hello(void)
{
	CGDirectDisplayID active[MAC_AGENT_MAX_DISPLAYS] = { 0 };
	uint32_t count = 0;
	if (CGGetActiveDisplayList(MAC_AGENT_MAX_DISPLAYS, active, &count) != kCGErrorSuccess)
		count = 0;

	xpc_object_t displays = xpc_array_create(nullptr, 0);
	for (uint32_t x = 0; x < count; x++)
	{
		const CGRect rect = CGDisplayBounds(active[x]);
		xpc_object_t d = xpc_dictionary_create(nullptr, nullptr, 0);
		xpc_dictionary_set_uint64(d, "id", active[x]);
		xpc_dictionary_set_double(d, "x", rect.origin.x);
		xpc_dictionary_set_double(d, "y", rect.origin.y);
		xpc_dictionary_set_double(d, "w", rect.size.width);
		xpc_dictionary_set_double(d, "h", rect.size.height);
		xpc_array_append_value(displays, d);
		xpc_release(d);
	}

	xpc_object_t msg = agent_message(MAC_IPC_HELLO);
	xpc_dictionary_set_uint64(msg, "uid", getuid());
	xpc_dictionary_set_string(msg, "session",
	                          (getuid() == 0) ? MAC_IPC_SESSION_LOGINWINDOW : MAC_IPC_SESSION_AQUA);
	xpc_dictionary_set_uint64(msg, "main", CGMainDisplayID());
	xpc_dictionary_set_value(msg, "displays", displays);
	xpc_release(displays);
	agent_send(msg);
	g_displaysHash = agent_displays_hash();
	WLog_INFO(TAG, "hello sent: uid %u, %u display(s)", (unsigned)getuid(), count);
}

/* a display mode change (or a display added or gone): tell the server, which resizes and asks
 * for the capture again. Checked once a second: a display reconfiguration callback is only
 * delivered to a process with an application's window server connection, not to this agent. */
#define MAC_AGENT_DISPLAYS_MS 1000
static dispatch_source_t g_displaysTimer;
static UINT32 agent_displays_hash(void)
{
	CGDirectDisplayID active[MAC_AGENT_MAX_DISPLAYS] = { 0 };
	uint32_t count = 0;
	if (CGGetActiveDisplayList(MAC_AGENT_MAX_DISPLAYS, active, &count) != kCGErrorSuccess)
		count = 0;
	UINT32 hash = 2166136261u ^ count;
	for (uint32_t x = 0; x < count; x++)
	{
		const CGRect rect = CGDisplayBounds(active[x]);
		const double values[] = { active[x], rect.origin.x, rect.origin.y, rect.size.width,
			                      rect.size.height };
		const BYTE *bytes = (const BYTE *)values;
		for (size_t y = 0; y < sizeof(values); y++)
		{
			hash ^= bytes[y];
			hash *= 16777619u;
		}
	}
	return hash;
}

static void agent_displays_tick(void)
{
	const UINT32 hash = agent_displays_hash();
	if (hash == g_displaysHash)
		return;
	g_displaysHash = hash;
	WLog_INFO(TAG, "the displays changed");
	agent_send_hello();
}

static void agent_on_frame(WINPR_ATTR_UNUSED void *context, CVPixelBufferRef frame,
                           const CGRect *dirty, size_t count)
{
	IOSurfaceRef surface = CVPixelBufferGetIOSurface(frame);
	if (!surface)
		return;
	/* the server copies each frame; while it is behind, skip frames (the next one has what
	 * changed: the dirty rectangles are merged on the server's side) */
	if (atomic_load(&g_inFlight) >= MAC_IPC_FRAMES_IN_FLIGHT)
		return;

	xpc_object_t xsurface = IOSurfaceCreateXPCObject(surface);
	if (!xsurface)
		return;
	xpc_object_t msg = agent_message(MAC_IPC_FRAME);
	xpc_dictionary_set_value(msg, "surface", xsurface);
	xpc_release(xsurface);
	if (dirty && (count > 0))
		xpc_dictionary_set_data(msg, "rects", dirty, count * sizeof(CGRect));

	CVPixelBufferRetain(frame);
	atomic_fetch_add(&g_inFlight, 1);
	xpc_connection_send_message_with_reply(g_server, msg, g_queue, ^(xpc_object_t reply) {
		(void)reply;
		CVPixelBufferRelease(frame);
		atomic_fetch_sub(&g_inFlight, 1);
	});
	xpc_release(msg);
}

static void agent_on_audio(WINPR_ATTR_UNUSED void *context, const float *samples, size_t frames,
                           UINT32 channels, UINT32 rate)
{
	xpc_object_t msg = agent_message(MAC_IPC_AUDIO);
	xpc_dictionary_set_data(msg, "samples", samples, frames * channels * sizeof(float));
	xpc_dictionary_set_uint64(msg, "channels", channels);
	xpc_dictionary_set_uint64(msg, "rate", rate);
	agent_send(msg);
}

static void agent_on_stopped(WINPR_ATTR_UNUSED void *context)
{
	agent_send(agent_message(MAC_IPC_STOPPED));
}

static UINT32 agent_cursor_hash(const macCursorImage *image)
{
	UINT32 hash = 2166136261u;
	const UINT32 header[] = { image->width, image->height, image->xHot, image->yHot };
	const BYTE *parts[] = { (const BYTE *)header, image->pixels };
	const size_t sizes[] = { sizeof(header), 4ull * image->width * image->height };
	for (size_t p = 0; p < 2; p++)
		for (size_t x = 0; x < sizes[p]; x++)
		{
			hash ^= parts[p][x];
			hash *= 16777619u;
		}
	return hash;
}

static void agent_cursor_tick(void)
{
	macCursorImage image = { 0 };
	if (mac_cursor_get(&image, MAC_AGENT_CURSOR_MAX))
	{
		const UINT32 hash = agent_cursor_hash(&image);
		if (!g_cursorKnown || (hash != g_cursorHash))
		{
			xpc_object_t msg = agent_message(MAC_IPC_CURSOR);
			xpc_dictionary_set_uint64(msg, "w", image.width);
			xpc_dictionary_set_uint64(msg, "h", image.height);
			xpc_dictionary_set_uint64(msg, "xhot", image.xHot);
			xpc_dictionary_set_uint64(msg, "yhot", image.yHot);
			xpc_dictionary_set_data(msg, "pixels", image.pixels, 4ull * image.width * image.height);
			agent_send(msg);
			WLog_DBG(TAG, "cursor %" PRIu32 "x%" PRIu32 " (hot spot %" PRIu32 ",%" PRIu32 ") sent",
			         image.width, image.height, image.xHot, image.yHot);
			g_cursorHash = hash;
			g_cursorKnown = TRUE;
		}
		mac_cursor_image_free(&image);
	}

	CGEventRef event = CGEventCreate(nullptr);
	if (event)
	{
		const CGPoint where = CGEventGetLocation(event);
		CFRelease(event);
		if (!g_locationKnown || (where.x != g_location.x) || (where.y != g_location.y))
		{
			xpc_object_t msg = agent_message(MAC_IPC_POSITION);
			xpc_dictionary_set_double(msg, "x", where.x);
			xpc_dictionary_set_double(msg, "y", where.y);
			agent_send(msg);
			g_location = where;
			g_locationKnown = TRUE;
		}
	}
}

static void agent_stop(void)
{
	if (g_cursorTimer)
	{
		dispatch_source_cancel(g_cursorTimer);
		g_cursorTimer = nullptr;
	}
	if (g_capture)
	{
		mac_capture_stop(g_capture);
		mac_capture_free(g_capture);
		g_capture = nullptr;
	}
}

static void agent_start(xpc_object_t msg)
{
	agent_stop();
	g_config = (macCaptureConfig){
		.display = (CGDirectDisplayID)xpc_dictionary_get_uint64(msg, "display"),
		.width = (size_t)xpc_dictionary_get_uint64(msg, "width"),
		.height = (size_t)xpc_dictionary_get_uint64(msg, "height"),
		.fps = (UINT32)xpc_dictionary_get_uint64(msg, "fps"),
		.showCursor = FALSE, /* the clients draw it */
		.audio = xpc_dictionary_get_bool(msg, "audio") ? TRUE : FALSE,
		.audioRate = (UINT32)xpc_dictionary_get_uint64(msg, "rate"),
		.audioChannels = (UINT32)xpc_dictionary_get_uint64(msg, "channels"),
		.onFrame = agent_on_frame,
		.onAudio = agent_on_audio,
		.onStopped = agent_on_stopped,
		.context = nullptr,
	};
	g_cursorKnown = FALSE;
	g_locationKnown = FALSE;

	g_capture = mac_capture_new(&g_config);
	if (!g_capture || !mac_capture_start(g_capture))
	{
		WLog_ERR(TAG, "capture of display %u (%zux%zu) failed", g_config.display, g_config.width,
		         g_config.height);
		if (g_capture)
			mac_capture_free(g_capture);
		g_capture = nullptr;
		agent_on_stopped(nullptr); /* the server tries again */
		return;
	}
	WLog_INFO(TAG, "capturing display %u at %zux%zu", g_config.display, g_config.width,
	          g_config.height);

	g_cursorTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
	dispatch_source_set_timer(g_cursorTimer, DISPATCH_TIME_NOW, MAC_AGENT_CURSOR_MS * NSEC_PER_MSEC,
	                          5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(g_cursorTimer, ^{
		agent_cursor_tick();
	});
	dispatch_resume(g_cursorTimer);
}

static void agent_on_event(xpc_object_t msg)
{
	size_t length = 0;
	const void *bytes = xpc_dictionary_get_data(msg, "data", &length);
	if (!bytes)
		return;
	CFDataRef data = CFDataCreate(nullptr, bytes, (CFIndex)length);
	if (!data)
		return;
	CGEventRef event = CGEventCreateFromData(nullptr, data);
	CFRelease(data);
	if (event)
		(void)mac_input_post(&g_input, event);
}

static void agent_on_message(xpc_object_t msg)
{
	const char *type = xpc_dictionary_get_string(msg, MAC_IPC_TYPE);
	if (!type)
		return;
	if (strcmp(type, MAC_IPC_EVENT) == 0)
		agent_on_event(msg);
	else if (strcmp(type, MAC_IPC_START) == 0)
		agent_start(msg);
	else if (strcmp(type, MAC_IPC_STOP) == 0)
		agent_stop();
}

int main(int argc, char *argv[])
{
	const char *service = (argc > 1) ? argv[1] : MAC_IPC_SERVICE_DEFAULT;

	@autoreleasepool
	{
		/* the permissions are this program's: macOS asks once, in a user's session */
		if (!mac_capture_check_access())
			WLog_WARN(TAG, "no permission to record the screen yet (System Settings > Privacy "
			               "& Security > Screen & System Audio Recording)");
		if ((getuid() == 0) && mac_input_open_hid(&g_input))
			WLog_INFO(TAG, "input: posted to the HID system (running as root, as at the login "
			               "window)");

		g_queue = dispatch_queue_create("org.freerdp.shadow.agent", DISPATCH_QUEUE_SERIAL);
		g_server = xpc_connection_create_mach_service(service, g_queue, 0);
		if (!g_queue || !g_server)
			return 1;

		xpc_connection_set_event_handler(g_server, ^(xpc_object_t event) {
			if (xpc_get_type(event) == XPC_TYPE_DICTIONARY)
				agent_on_message(event);
			else if (event == XPC_ERROR_CONNECTION_INTERRUPTED)
			{
				/* the server restarted: say hello again, it asks for the capture again */
				WLog_INFO(TAG, "the server restarted");
				agent_stop();
				agent_send_hello();
			}
			else if (event == XPC_ERROR_CONNECTION_INVALID)
			{
				/* no such service (the server is not installed): launchd starts the agent
				 * again later */
				WLog_ERR(TAG, "no server on the Mach service %s", service);
				exit(1);
			}
		});
		xpc_connection_resume(g_server);
		dispatch_async(g_queue, ^{
			agent_send_hello();
		});
		g_displaysTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
		dispatch_source_set_timer(
		    g_displaysTimer,
		    dispatch_time(DISPATCH_TIME_NOW, MAC_AGENT_DISPLAYS_MS * NSEC_PER_MSEC),
		    MAC_AGENT_DISPLAYS_MS * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(g_displaysTimer, ^{
			agent_displays_tick();
		});
		dispatch_resume(g_displaysTimer);

		/* the main run loop: AppKit (the cursor) and ScreenCaptureKit may want one */
		CFRunLoopRun();
	}
	return 0;
}
