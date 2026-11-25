/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_SERVER_SHADOW_SCREEN_H
#define FREERDP_SERVER_SHADOW_SCREEN_H

#include <freerdp/server/shadow.h>

#include <winpr/crt.h>
#include <winpr/synch.h>

#include "shadow_layout.h"

struct rdp_shadow_screen
{
	rdpShadowServer* server;

	UINT32 width;
	UINT32 height;

	CRITICAL_SECTION lock;
	REGION16 invalidRegion;

	rdpShadowSurface* primary;
	rdpShadowSurface* lobby;

	/* the layout the surface was last made for, and its version: it counts every change, also
	 * one that keeps the size (a monitor moved, another primary), for the GFX clients'
	 * monitors (shadow_layout.h) */
	SHADOW_LAYOUT layout;
	UINT32 layoutVersion;
};

#ifdef __cplusplus
extern "C"
{
#endif

	void shadow_screen_free(rdpShadowScreen* screen);

	/** whether the server shares every monitor: the subsystem can (allMonitors), /monitors
	 *  chose it or it is the subsystem's default, and there is more than one */
	WINPR_ATTR_NODISCARD BOOL shadow_screen_all_monitors(const rdpShadowServer* server);

	WINPR_ATTR_MALLOC(shadow_screen_free, 1)
	WINPR_ATTR_NODISCARD
	rdpShadowScreen* shadow_screen_new(rdpShadowServer* server);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_SCREEN_H */
