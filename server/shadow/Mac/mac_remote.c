/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server: the daemon's side of the agents (capture, cursor and input over XPC)
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

#include <freerdp/config.h>

#include <string.h>
#include <sys/stat.h>

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <IOSurface/IOSurfaceRef.h>
#include <CoreVideo/CoreVideo.h>

#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>
#include <freerdp/log.h>

#include "mac_ipc.h"
#include "mac_remote.h"

#define TAG SERVER_TAG("shadow.mac")

#define MAC_REMOTE_MAX_DISPLAYS 16

struct mac_remote
{
	char service[128];
	dispatch_queue_t queue;
	xpc_connection_t listener;

	CRITICAL_SECTION lock;
	/* the agent on the console: the one that said hello last */
	xpc_connection_t agent;
	HANDLE helloEvent;
	UINT32 displayCount;
	CGDirectDisplayID ids[MAC_REMOTE_MAX_DISPLAYS];
	CGRect bounds[MAC_REMOTE_MAX_DISPLAYS];
	CGDirectDisplayID mainDisplay;
	BOOL displaysKnown;
	HANDLE displaysEvent; /* the server's, not owned */

	/* the capture the server asked for, sent again to an agent that comes later */
	BOOL capturing;
	macCaptureConfig config;
	CVPixelBufferPoolRef pool;
	size_t poolWidth;
	size_t poolHeight;

	macCursorImage cursor;
	BOOL cursorValid;
	CGPoint location;
	BOOL locationValid;
};

uid_t mac_remote_console_uid(void)
{
	struct stat st = WINPR_C_ARRAY_INIT;
	if (stat("/dev/console", &st) != 0)
		return 0;
	return st.st_uid;
}

static void mac_remote_send_start(macRemote* remote, xpc_connection_t agent)
{
	const macCaptureConfig* config = &remote->config;
	xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
	xpc_dictionary_set_string(msg, MAC_IPC_TYPE, MAC_IPC_START);
	xpc_dictionary_set_uint64(msg, "display", config->display);
	xpc_dictionary_set_uint64(msg, "width", config->width);
	xpc_dictionary_set_uint64(msg, "height", config->height);
	xpc_dictionary_set_uint64(msg, "fps", config->fps);
	xpc_dictionary_set_bool(msg, "audio", config->audio ? true : false);
	xpc_dictionary_set_uint64(msg, "rate", config->audioRate);
	xpc_dictionary_set_uint64(msg, "channels", config->audioChannels);
	xpc_connection_send_message(agent, msg);
	xpc_release(msg);
}

static void mac_remote_send_type(xpc_connection_t agent, const char* type)
{
	xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
	xpc_dictionary_set_string(msg, MAC_IPC_TYPE, type);
	xpc_connection_send_message(agent, msg);
	xpc_release(msg);
}

static void mac_remote_on_hello(macRemote* remote, xpc_connection_t peer, xpc_object_t msg)
{
	const char* session = xpc_dictionary_get_string(msg, "session");
	xpc_object_t displays = xpc_dictionary_get_value(msg, "displays");
	if (!displays || (xpc_get_type(displays) != XPC_TYPE_ARRAY))
		return;

	const size_t count = MIN(xpc_array_get_count(displays), MAC_REMOTE_MAX_DISPLAYS);
	CGDirectDisplayID ids[MAC_REMOTE_MAX_DISPLAYS] = WINPR_C_ARRAY_INIT;
	CGRect bounds[MAC_REMOTE_MAX_DISPLAYS] = WINPR_C_ARRAY_INIT;
	for (size_t x = 0; x < count; x++)
	{
		xpc_object_t d = xpc_array_get_value(displays, x);
		ids[x] = (CGDirectDisplayID)xpc_dictionary_get_uint64(d, "id");
		bounds[x] =
		    CGRectMake(xpc_dictionary_get_double(d, "x"), xpc_dictionary_get_double(d, "y"),
		               xpc_dictionary_get_double(d, "w"), xpc_dictionary_get_double(d, "h"));
	}

	EnterCriticalSection(&remote->lock);
	/* the same displays as before (the usual handover: login window and session on the same
	 * display mode) or not (another mode, another display: the server resizes first) */
	BOOL displaysChanged = remote->displaysKnown && (remote->displayCount != count);
	for (size_t x = 0; remote->displaysKnown && !displaysChanged && (x < count); x++)
		displaysChanged =
		    (remote->ids[x] != ids[x]) || !CGRectEqualToRect(remote->bounds[x], bounds[x]);
	remote->displayCount = (UINT32)count;
	memcpy(remote->ids, ids, sizeof(ids));
	memcpy(remote->bounds, bounds, sizeof(bounds));
	remote->displaysKnown = TRUE;
	remote->mainDisplay = (CGDirectDisplayID)xpc_dictionary_get_uint64(msg, "main");
	HANDLE displaysEvent = remote->displaysEvent;

	const BOOL changed = remote->agent != peer;
	if (changed)
	{
		if (remote->agent)
			xpc_release(remote->agent);
		remote->agent = (xpc_connection_t)xpc_retain(peer);
		remote->cursorValid = FALSE;
		remote->locationValid = FALSE;
	}
	const BOOL capturing = remote->capturing;
	LeaveCriticalSection(&remote->lock);

	WLog_INFO(TAG, "agent: uid %" PRIu64 ", %s session, %" PRIuz " display(s)%s%s",
	          (UINT64)xpc_dictionary_get_uint64(msg, "uid"), session ? session : "?", count,
	          changed ? ", now the agent on the console" : "",
	          displaysChanged ? ", the displays changed" : "");
	(void)SetEvent(remote->helloEvent);

	if (displaysChanged && displaysEvent)
		(void)SetEvent(displaysEvent); /* the server resizes, then asks for the capture */
	else if (capturing)
		/* an agent that comes while the server captures (the user logged in, the login
		 * window's agent ended): it captures from now on */
		mac_remote_send_start(remote, peer);
}

static BOOL mac_remote_is_agent(macRemote* remote, xpc_connection_t peer)
{
	EnterCriticalSection(&remote->lock);
	const BOOL rc = remote->agent == peer;
	LeaveCriticalSection(&remote->lock);
	return rc;
}

/* copies the frame out of the agent's IOSurface: the agent gets its buffer back at once, and
 * the server keeps a picture of its own as long as it wants */
static CVPixelBufferRef mac_remote_copy_frame(macRemote* remote, IOSurfaceRef surface)
{
	const size_t width = IOSurfaceGetWidth(surface);
	const size_t height = IOSurfaceGetHeight(surface);
	if ((width != remote->config.width) || (height != remote->config.height))
	{
		WLog_DBG(TAG, "agent frame %" PRIuz "x%" PRIuz " instead of %" PRIuz "x%" PRIuz ": dropped",
		         width, height, remote->config.width, remote->config.height);
		return nullptr;
	}

	if (!remote->pool || (remote->poolWidth != width) || (remote->poolHeight != height))
	{
		if (remote->pool)
			CVPixelBufferPoolRelease(remote->pool);
		remote->pool = nullptr;
		const int32_t format = kCVPixelFormatType_32BGRA;
		CFNumberRef f = CFNumberCreate(nullptr, kCFNumberSInt32Type, &format);
		CFNumberRef w = CFNumberCreate(nullptr, kCFNumberSInt64Type, &width);
		CFNumberRef h = CFNumberCreate(nullptr, kCFNumberSInt64Type, &height);
		const void* keys[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey,
			                   kCVPixelBufferHeightKey };
		const void* values[] = { f, w, h };
		CFDictionaryRef attrs =
		    CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
		                       &kCFTypeDictionaryValueCallBacks);
		(void)CVPixelBufferPoolCreate(nullptr, nullptr, attrs, &remote->pool);
		CFRelease(attrs);
		CFRelease(h);
		CFRelease(w);
		CFRelease(f);
		remote->poolWidth = width;
		remote->poolHeight = height;
	}

	CVPixelBufferRef frame = nullptr;
	if (!remote->pool ||
	    (CVPixelBufferPoolCreatePixelBuffer(nullptr, remote->pool, &frame) != kCVReturnSuccess))
		return nullptr;

	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess)
	{
		CVPixelBufferRelease(frame);
		return nullptr;
	}
	(void)CVPixelBufferLockBaseAddress(frame, 0);
	const BYTE* src = IOSurfaceGetBaseAddress(surface);
	const size_t srcStride = IOSurfaceGetBytesPerRow(surface);
	BYTE* dst = CVPixelBufferGetBaseAddress(frame);
	const size_t dstStride = CVPixelBufferGetBytesPerRow(frame);
	const size_t line = MIN(MIN(srcStride, dstStride), width * 4);
	for (size_t y = 0; y < height; y++)
		memcpy(&dst[y * dstStride], &src[y * srcStride], line);
	(void)CVPixelBufferUnlockBaseAddress(frame, 0);
	(void)IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
	return frame;
}

static void mac_remote_on_frame(macRemote* remote, xpc_connection_t peer, xpc_object_t msg)
{
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	xpc_object_t value = xpc_dictionary_get_value(msg, "surface");
	IOSurfaceRef surface = value ? IOSurfaceLookupFromXPCObject(value) : nullptr;
	size_t length = 0;
	const void* rects = xpc_dictionary_get_data(msg, "rects", &length);

	if (surface && mac_remote_is_agent(remote, peer) && remote->capturing && remote->config.onFrame)
	{
		CVPixelBufferRef frame = mac_remote_copy_frame(remote, surface);
		if (frame)
		{
			remote->config.onFrame(remote->config.context, frame, (const CGRect*)rects,
			                       rects ? length / sizeof(CGRect) : 0);
			CVPixelBufferRelease(frame);
		}
	}
	if (surface)
		CFRelease(surface);
	if (reply)
	{
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	}
}

static void mac_remote_on_cursor(macRemote* remote, xpc_object_t msg)
{
	size_t length = 0;
	const void* pixels = xpc_dictionary_get_data(msg, "pixels", &length);
	const UINT32 width = (UINT32)xpc_dictionary_get_uint64(msg, "w");
	const UINT32 height = (UINT32)xpc_dictionary_get_uint64(msg, "h");
	if (!pixels || (length != 4ull * width * height) || (width == 0) || (height == 0))
		return;

	BYTE* copy = malloc(length);
	if (!copy)
		return;
	memcpy(copy, pixels, length);

	EnterCriticalSection(&remote->lock);
	free(remote->cursor.pixels);
	remote->cursor.pixels = copy;
	remote->cursor.width = width;
	remote->cursor.height = height;
	remote->cursor.xHot = (UINT32)xpc_dictionary_get_uint64(msg, "xhot");
	remote->cursor.yHot = (UINT32)xpc_dictionary_get_uint64(msg, "yhot");
	remote->cursorValid = TRUE;
	LeaveCriticalSection(&remote->lock);
	WLog_DBG(TAG, "cursor %" PRIu32 "x%" PRIu32 " from the agent", width, height);
}

static void mac_remote_on_message(macRemote* remote, xpc_connection_t peer, xpc_object_t msg)
{
	const char* type = xpc_dictionary_get_string(msg, MAC_IPC_TYPE);
	if (!type)
		return;

	if (strcmp(type, MAC_IPC_FRAME) == 0)
		mac_remote_on_frame(remote, peer, msg);
	else if (strcmp(type, MAC_IPC_HELLO) == 0)
		mac_remote_on_hello(remote, peer, msg);
	else if (!mac_remote_is_agent(remote, peer))
		return; /* only the agent on the console has a say */
	else if (strcmp(type, MAC_IPC_AUDIO) == 0)
	{
		size_t length = 0;
		const float* samples = xpc_dictionary_get_data(msg, "samples", &length);
		const UINT32 channels = (UINT32)xpc_dictionary_get_uint64(msg, "channels");
		if (samples && remote->capturing && remote->config.onAudio && (channels > 0))
			remote->config.onAudio(remote->config.context, samples,
			                       length / sizeof(float) / channels, channels,
			                       (UINT32)xpc_dictionary_get_uint64(msg, "rate"));
	}
	else if (strcmp(type, MAC_IPC_STOPPED) == 0)
	{
		if (remote->capturing && remote->config.onStopped)
			remote->config.onStopped(remote->config.context);
	}
	else if (strcmp(type, MAC_IPC_CURSOR) == 0)
		mac_remote_on_cursor(remote, msg);
	else if (strcmp(type, MAC_IPC_POSITION) == 0)
	{
		EnterCriticalSection(&remote->lock);
		remote->location =
		    CGPointMake(xpc_dictionary_get_double(msg, "x"), xpc_dictionary_get_double(msg, "y"));
		remote->locationValid = TRUE;
		LeaveCriticalSection(&remote->lock);
	}
}

static void mac_remote_on_peer_gone(macRemote* remote, xpc_connection_t peer)
{
	EnterCriticalSection(&remote->lock);
	const BOOL wasAgent = remote->agent == peer;
	if (wasAgent)
	{
		xpc_release(remote->agent);
		remote->agent = nullptr;
		(void)ResetEvent(remote->helloEvent);
	}
	const BOOL capturing = remote->capturing;
	LeaveCriticalSection(&remote->lock);

	if (wasAgent)
	{
		WLog_INFO(TAG, "the agent on the console went away");
		/* as when the display turns off: the server tries again until an agent is back */
		if (capturing && remote->config.onStopped)
			remote->config.onStopped(remote->config.context);
	}
}

static BOOL mac_remote_peer_allowed(xpc_connection_t peer)
{
	/* an agent runs as root (at the login window) or as the user on the console; anyone else
	 * could see and type */
	const uid_t uid = xpc_connection_get_euid(peer);
	if ((uid == 0) || (uid == mac_remote_console_uid()))
		return TRUE;
	WLog_WARN(TAG, "agent connection from uid %" PRIu32 " refused: not root, not on the console",
	          (UINT32)uid);
	return FALSE;
}

macRemote* mac_remote_new(const char* service)
{
	macRemote* remote = calloc(1, sizeof(macRemote));
	if (!remote)
		return nullptr;

	(void)strncpy(remote->service, service ? service : MAC_IPC_SERVICE_DEFAULT,
	              sizeof(remote->service) - 1);
	if (!InitializeCriticalSectionAndSpinCount(&remote->lock, 4000))
	{
		free(remote);
		return nullptr;
	}
	remote->helloEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	remote->queue = dispatch_queue_create("org.freerdp.shadow.agents", DISPATCH_QUEUE_SERIAL);
	remote->listener = xpc_connection_create_mach_service(remote->service, remote->queue,
	                                                      XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (!remote->helloEvent || !remote->queue || !remote->listener)
	{
		mac_remote_free(remote);
		return nullptr;
	}

	xpc_connection_set_event_handler(remote->listener, ^(xpc_object_t peer) {
	  if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
	  {
		  if (peer == XPC_ERROR_CONNECTION_INVALID)
			  WLog_ERR(TAG,
				       "Mach service %s: listener invalid (is it in the plist's "
				       "MachServices?)",
				       remote->service);
		  return;
	  }
	  if (!mac_remote_peer_allowed((xpc_connection_t)peer))
	  {
		  xpc_connection_cancel((xpc_connection_t)peer);
		  return;
	  }
	  xpc_connection_t connection = (xpc_connection_t)peer;
	  xpc_connection_set_target_queue(connection, remote->queue);
	  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
		if (xpc_get_type(event) == XPC_TYPE_DICTIONARY)
			mac_remote_on_message(remote, connection, event);
		else if (xpc_get_type(event) == XPC_TYPE_ERROR)
			mac_remote_on_peer_gone(remote, connection);
	  });
	  xpc_connection_resume(connection);
	});
	xpc_connection_resume(remote->listener);
	WLog_INFO(TAG, "waiting for agents on the Mach service %s", remote->service);
	return remote;
}

void mac_remote_free(macRemote* remote)
{
	if (!remote)
		return;
	if (remote->listener)
	{
		xpc_connection_cancel(remote->listener);
		xpc_release(remote->listener);
	}
	if (remote->queue)
		dispatch_sync(remote->queue, ^{
		                  /* let the handlers that run finish */
		              });
	if (remote->agent)
		xpc_release(remote->agent);
	if (remote->pool)
		CVPixelBufferPoolRelease(remote->pool);
	free(remote->cursor.pixels);
	if (remote->queue)
		dispatch_release(remote->queue);
	if (remote->helloEvent)
		(void)CloseHandle(remote->helloEvent);
	DeleteCriticalSection(&remote->lock);
	free(remote);
}

UINT32 mac_remote_list_displays(macRemote* remote, DWORD timeout, MONITOR_DEF* monitors,
                                CGDirectDisplayID* ids, CGRect* bounds, UINT32 maxMonitors)
{
	WINPR_ASSERT(remote);
	if (WaitForSingleObject(remote->helloEvent, timeout) != WAIT_OBJECT_0)
	{
		WLog_ERR(TAG, "no agent said hello within %" PRIu32 " ms", timeout);
		return 0;
	}

	EnterCriticalSection(&remote->lock);
	const UINT32 count = MIN(remote->displayCount, maxMonitors);
	CGFloat minX = 0;
	CGFloat minY = 0;
	for (UINT32 x = 0; x < count; x++)
	{
		minX = MIN(minX, remote->bounds[x].origin.x);
		minY = MIN(minY, remote->bounds[x].origin.y);
	}
	for (UINT32 x = 0; x < count; x++)
	{
		const CGRect rect = remote->bounds[x];
		MONITOR_DEF* monitor = &monitors[x];
		monitor->left = (INT32)(rect.origin.x - minX);
		monitor->top = (INT32)(rect.origin.y - minY);
		monitor->right = monitor->left + (INT32)rect.size.width - 1;
		monitor->bottom = monitor->top + (INT32)rect.size.height - 1;
		monitor->flags = (remote->ids[x] == remote->mainDisplay) ? 1 : 0;
		if (ids)
			ids[x] = remote->ids[x];
		if (bounds)
			bounds[x] = rect;
	}
	LeaveCriticalSection(&remote->lock);
	return count;
}

void mac_remote_set_displays_event(macRemote* remote, HANDLE event)
{
	WINPR_ASSERT(remote);
	EnterCriticalSection(&remote->lock);
	remote->displaysEvent = event;
	LeaveCriticalSection(&remote->lock);
}

BOOL mac_remote_capture_start(macRemote* remote, const macCaptureConfig* config)
{
	WINPR_ASSERT(remote);
	WINPR_ASSERT(config);

	EnterCriticalSection(&remote->lock);
	remote->config = *config;
	remote->capturing = TRUE;
	xpc_connection_t agent = remote->agent ? (xpc_connection_t)xpc_retain(remote->agent) : nullptr;
	LeaveCriticalSection(&remote->lock);

	if (!agent)
		return FALSE;
	mac_remote_send_start(remote, agent);
	xpc_release(agent);
	return TRUE;
}

void mac_remote_capture_stop(macRemote* remote)
{
	WINPR_ASSERT(remote);
	EnterCriticalSection(&remote->lock);
	remote->capturing = FALSE;
	xpc_connection_t agent = remote->agent ? (xpc_connection_t)xpc_retain(remote->agent) : nullptr;
	LeaveCriticalSection(&remote->lock);
	if (agent)
	{
		mac_remote_send_type(agent, MAC_IPC_STOP);
		xpc_release(agent);
	}
}

BOOL mac_remote_post(macRemote* remote, CGEventRef event)
{
	WINPR_ASSERT(remote);
	if (!event)
		return FALSE;

	CFDataRef data = CGEventCreateData(nullptr, event);
	CFRelease(event);
	if (!data)
		return TRUE; /* dropped, not an error: see mac_input_post */

	EnterCriticalSection(&remote->lock);
	xpc_connection_t agent = remote->agent ? (xpc_connection_t)xpc_retain(remote->agent) : nullptr;
	LeaveCriticalSection(&remote->lock);
	if (agent)
	{
		xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
		xpc_dictionary_set_string(msg, MAC_IPC_TYPE, MAC_IPC_EVENT);
		xpc_dictionary_set_data(msg, "data", CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
		xpc_connection_send_message(agent, msg);
		xpc_release(msg);
		xpc_release(agent);
	}
	CFRelease(data);
	return TRUE;
}

BOOL mac_remote_cursor_get(macRemote* remote, macCursorImage* image, UINT32 maxSize)
{
	WINPR_ASSERT(remote);
	WINPR_ASSERT(image);

	BOOL rc = FALSE;
	EnterCriticalSection(&remote->lock);
	if (remote->cursorValid && (remote->cursor.width <= maxSize) &&
	    (remote->cursor.height <= maxSize))
	{
		const size_t length = 4ull * remote->cursor.width * remote->cursor.height;
		BYTE* copy = malloc(length);
		if (copy)
		{
			memcpy(copy, remote->cursor.pixels, length);
			*image = remote->cursor;
			image->pixels = copy;
			rc = TRUE;
		}
	}
	LeaveCriticalSection(&remote->lock);
	return rc;
}

BOOL mac_remote_location(macRemote* remote, CGPoint* where)
{
	WINPR_ASSERT(remote);
	WINPR_ASSERT(where);
	EnterCriticalSection(&remote->lock);
	const BOOL rc = remote->locationValid;
	if (rc)
		*where = remote->location;
	LeaveCriticalSection(&remote->lock);
	return rc;
}
