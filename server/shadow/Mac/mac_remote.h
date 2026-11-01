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

#ifndef FREERDP_SERVER_SHADOW_MAC_REMOTE_H
#define FREERDP_SERVER_SHADOW_MAC_REMOTE_H

#include <winpr/wtypes.h>
#include <freerdp/server/shadow.h>

#include <ApplicationServices/ApplicationServices.h>

#include "mac_capture.h"
#include "mac_cursor.h"

typedef struct mac_remote macRemote;

#ifdef __cplusplus
extern "C"
{
#endif

	/* listens on the launchd Mach service (the daemon's plist declares it in MachServices) */
	macRemote *mac_remote_new(const char *service);
	void mac_remote_free(macRemote *remote);

	/* the displays of the agent on the console, as mac_shadow_list_displays gives them; waits
	 * up to timeout ms for an agent to say hello */
	UINT32 mac_remote_list_displays(macRemote *remote, DWORD timeout, MONITOR_DEF *monitors,
	                                CGDirectDisplayID *ids, CGRect *bounds, UINT32 maxMonitors);

	/* the capture runs in the agent; frames, sound and the end of the stream come back through
	 * the config's callbacks, as from mac_capture. FALSE when no agent is there (yet) */
	BOOL mac_remote_capture_start(macRemote *remote, const macCaptureConfig *config);
	void mac_remote_capture_stop(macRemote *remote);

	/* the agent posts the event; it is released here */
	BOOL mac_remote_post(macRemote *remote, CGEventRef event);

	/* the cursor and pointer position the agent last sent */
	BOOL mac_remote_cursor_get(macRemote *remote, macCursorImage *image, UINT32 maxSize);
	BOOL mac_remote_location(macRemote *remote, CGPoint *where);

	/* the user on the console (root at the login window), from /dev/console */
	uid_t mac_remote_console_uid(void);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_MAC_REMOTE_H */
