/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server: posting input events (Quartz, or the HID system at the login window)
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

#ifndef FREERDP_SERVER_SHADOW_MAC_INPUT_H
#define FREERDP_SERVER_SHADOW_MAC_INPUT_H

#include <winpr/wtypes.h>

#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>

/* kCGEventSourceUserData of a key event that carries a Unicode character and no key: the HID
 * system can not type it (it would type the key 0, A), so mac_input_post drops it there. The
 * field survives CGEventCreateData, so the mark crosses to the agent too. */
#define MAC_INPUT_UNICODE_EVENT 0x46524450554e49LL /* "FRDPUNI" */

/* Where input goes: Quartz (CGEventPost) in a user's session; the HID system itself
 * (IOHIDPostEvent) at the login window, where Secure Event Input drops Quartz events.
 * Posting to the HID system needs a process in the console session (a launchd agent there). */
typedef struct
{
	io_connect_t hid; /* MACH_PORT_NULL: Quartz */
	UINT64 refused;   /* events the HID system refused, for the log */
	BOOL unicodeWarned;
} macInput;

#ifdef __cplusplus
extern "C"
{
#endif

	BOOL mac_input_open_hid(macInput* input);
	BOOL mac_input_uses_hid(const macInput* input);
	void mac_input_close(macInput* input);

	/* posts the event and releases it; a refused event is logged, not an error: a failing
	 * input handler would make FreeRDP drop the client */
	BOOL mac_input_post(macInput* input, CGEventRef event);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_MAC_INPUT_H */
