/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server: posting input events
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

#include <math.h>

#include <winpr/wlog.h>
#include <freerdp/log.h>

#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hidsystem/ev_keymap.h>

#include "mac_input.h"

#define TAG SERVER_TAG("shadow.mac")

/* At the login window Secure Event Input drops every event posted through
 * Quartz, whatever the tap or the event source; an event posted to the HID
 * system itself still arrives, as from a real keyboard or mouse. That takes
 * IOHIDPostEvent, deprecated since macOS 11 but the only way there, and root.
 * The event is built with Quartz as everywhere else and handed over field by
 * field (the NX_* flag masks have the values of the Quartz ones). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static io_connect_t mac_input_hid_open(void)
{
	io_connect_t connect = MACH_PORT_NULL;
	io_service_t service =
	    IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
	if (!service)
		return MACH_PORT_NULL;

	const kern_return_t rc =
	    IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect);
	IOObjectRelease(service);
	return (rc == KERN_SUCCESS) ? connect : MACH_PORT_NULL;
}

static BOOL mac_input_hid_post(macInput* input, CGEventRef event)
{
	const CGEventType type = CGEventGetType(event);
	const CGPoint where = CGEventGetLocation(event);
	const IOGPoint location = { (SInt16)lround(where.x), (SInt16)lround(where.y) };
	NXEventData data = { 0 };
	UInt32 nxType = 0;
	IOOptionBits options = 0;

	switch (type)
	{
		case kCGEventKeyDown:
		case kCGEventKeyUp:
		case kCGEventFlagsChanged:
		{
			UniChar text[4] = { 0 };
			UniCharCount length = 0;
			CGEventKeyboardGetUnicodeString(event, ARRAYSIZE(text), &length, text);
			const UInt16 character = ((length > 0) && (text[0] < 0x80)) ? text[0] : 0;

			data.key.keyCode = (UInt16)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
			data.key.repeat =
			    (SInt16)CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat);
			data.key.charSet = NX_ASCIISET;
			data.key.charCode = character;
			data.key.origCharSet = NX_ASCIISET;
			data.key.origCharCode = character;
			if (type == kCGEventFlagsChanged)
			{
				nxType = NX_FLAGSCHANGED;
				options = kIOHIDSetGlobalEventFlags;
			}
			else
				nxType = (type == kCGEventKeyDown) ? NX_KEYDOWN : NX_KEYUP;
			break;
		}

		case kCGEventMouseMoved:
		case kCGEventLeftMouseDragged:
		case kCGEventRightMouseDragged:
		case kCGEventOtherMouseDragged:
			nxType = (type == kCGEventMouseMoved)          ? NX_MOUSEMOVED
			         : (type == kCGEventLeftMouseDragged)  ? NX_LMOUSEDRAGGED
			         : (type == kCGEventRightMouseDragged) ? NX_RMOUSEDRAGGED
			                                               : NX_OMOUSEDRAGGED;
			options = kIOHIDSetCursorPosition;
			break;

		case kCGEventLeftMouseDown:
		case kCGEventLeftMouseUp:
		case kCGEventRightMouseDown:
		case kCGEventRightMouseUp:
		case kCGEventOtherMouseDown:
		case kCGEventOtherMouseUp:
		{
			const BOOL down = (type == kCGEventLeftMouseDown) || (type == kCGEventRightMouseDown) ||
			                  (type == kCGEventOtherMouseDown);
			nxType = (type == kCGEventLeftMouseDown)    ? NX_LMOUSEDOWN
			         : (type == kCGEventLeftMouseUp)    ? NX_LMOUSEUP
			         : (type == kCGEventRightMouseDown) ? NX_RMOUSEDOWN
			         : (type == kCGEventRightMouseUp)   ? NX_RMOUSEUP
			         : (type == kCGEventOtherMouseDown) ? NX_OMOUSEDOWN
			                                            : NX_OMOUSEUP;
			data.mouse.click = (SInt32)CGEventGetIntegerValueField(event, kCGMouseEventClickState);
			data.mouse.buttonNumber =
			    (UInt8)CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
			data.mouse.pressure = down ? 255 : 0;
			options = kIOHIDSetCursorPosition;
			break;
		}

		case kCGEventScrollWheel:
			nxType = NX_SCROLLWHEELMOVED;
			data.scrollWheel.deltaAxis1 =
			    (SInt16)CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
			data.scrollWheel.deltaAxis2 =
			    (SInt16)CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);
			/* 16.16 fixed point */
			data.scrollWheel.fixedDeltaAxis1 = (SInt32)lround(
			    CGEventGetDoubleValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1) * 65536.0);
			data.scrollWheel.fixedDeltaAxis2 = (SInt32)lround(
			    CGEventGetDoubleValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2) * 65536.0);
			data.scrollWheel.pointDeltaAxis1 =
			    (SInt32)CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1);
			data.scrollWheel.pointDeltaAxis2 =
			    (SInt32)CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis2);
			break;

		default:
			WLog_DBG(TAG, "no HID event for Quartz event type %" PRIu32, (UINT32)type);
			return TRUE;
	}

	const kern_return_t rc =
	    IOHIDPostEvent(input->hid, nxType, location, &data, kNXEventDataVersion,
	                   (IOOptionBits)CGEventGetFlags(event), options);
	/* a refused event is dropped, never the session: a failing input handler
	 * would make FreeRDP drop the client */
	if (rc != KERN_SUCCESS)
	{
		input->refused++;
		if ((input->refused <= 5) || ((input->refused % 100) == 0))
			WLog_WARN(TAG,
			          "the HID system refused an event (type %" PRIu32 "): 0x%08" PRIx32
			          ", %" PRIu64 " so far",
			          nxType, (UINT32)rc, input->refused);
	}
	else
		WLog_DBG(TAG, "HID event type %" PRIu32 " posted", nxType);
	return TRUE;
}
#pragma clang diagnostic pop

BOOL mac_input_open_hid(macInput* input)
{
	WINPR_ASSERT(input);
	input->hid = mac_input_hid_open();
	return input->hid != MACH_PORT_NULL;
}

BOOL mac_input_uses_hid(const macInput* input)
{
	WINPR_ASSERT(input);
	return input->hid != MACH_PORT_NULL;
}

void mac_input_close(macInput* input)
{
	WINPR_ASSERT(input);
	if (input->hid != MACH_PORT_NULL)
		(void)IOServiceClose(input->hid);
	input->hid = MACH_PORT_NULL;
}

BOOL mac_input_post(macInput* input, CGEventRef event)
{
	WINPR_ASSERT(input);
	if (!event)
		return FALSE;

	BOOL rc = TRUE;
	if (input->hid != MACH_PORT_NULL)
	{
		/* the HID system has no Unicode character set: a key event with no key would type
		 * the key 0 (A) */
		if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == MAC_INPUT_UNICODE_EVENT)
		{
			if (!input->unicodeWarned)
				WLog_WARN(TAG, "Unicode keyboard input can not be posted to the HID system: "
				               "dropped (scancodes work)");
			input->unicodeWarned = TRUE;
		}
		else
			rc = mac_input_hid_post(input, event);
	}
	else
		CGEventPost(kCGHIDEventTap, event);
	CFRelease(event);
	return rc;
}
