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

#ifndef FREERDP_SERVER_SHADOW_MAC_SHADOW_H
#define FREERDP_SERVER_SHADOW_MAC_SHADOW_H

#include <freerdp/server/shadow.h>

typedef struct mac_shadow_subsystem macShadowSubsystem;

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>
#include <winpr/collections.h>

#include <freerdp/codec/region.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include "mac_capture.h"
#include "mac_cursor.h"
#include "mac_input.h"
#include "mac_remote.h"

#define MAC_SHADOW_MAX_DISPLAYS 16

struct mac_shadow_subsystem
{
	rdpShadowSubsystem common;

	/* the display shared, in global display coordinates (points) */
	CGDirectDisplayID display;
	CGRect bounds;
	/* the size of the surface and of the captured frames, in pixels */
	UINT32 width;
	UINT32 height;

	/* the stream, how it was made (to make it again) and the event its end
	 * sets: it ends when the display turns off, and starts again once the
	 * display is back */
	macCapture *capture;
	macCaptureConfig config;
	HANDLE lostEvent;
	/* set when the displays changed (another display mode, or as a daemon the session on the
	 * console changed to one with other displays): the surface follows them */
	HANDLE displaysEvent;
	BOOL reconfigureRegistered;
	HANDLE thread;
	HANDLE stopEvent;

	/* the newest frame and what changed since it was last sent, under frameLock */
	CRITICAL_SECTION frameLock;
	CVPixelBufferRef frame;
	REGION16 pending;
	HANDLE frameEvent;
	BOOL sizeWarned;

	/* where input goes: Quartz, or the HID system as root at the login window */
	macInput input;
	/* the server runs as a root daemon (FREERDP_SHADOW_MAC_AGENT_SERVICE): capture, cursor
	 * and input are an agent's, in the session on the console; nullptr otherwise */
	macRemote *remote;

	/* input state: mouse buttons down, modifiers down, the last click */
	UINT32 buttonsDown;
	CGEventFlags modifiers;
	UINT64 lastClickTime;
	CGMouseButton lastClickButton;
	CGPoint lastClickPoint;
	int64_t clickCount;
	INT32 wheelRemainder[2];

	/* sound: samples gathered into messages of 20 ms */
	wStream *audioStream;

	/* sound statistics, logged every few seconds at debug level */
	UINT64 audioFrames;
	UINT64 audioMessages;
	float audioPeak;
	UINT64 audioLogged;

	/* the cursor: the clients draw it themselves, from its shape and position */
	UINT32 cursorHash;
	BOOL cursorKnown;
	UINT64 cursorNext;

	/* under clientLock: the clients connected (the display stays awake while
	 * there are any), when one last moved the pointer, and whether a client
	 * asked for the whole screen, cursor included */
	CRITICAL_SECTION clientLock;
	UINT32 clients;
	IOPMAssertionID awake;
	UINT64 pointerInput;
	BOOL cursorResend;
};

#endif /* FREERDP_SERVER_SHADOW_MAC_SHADOW_H */
