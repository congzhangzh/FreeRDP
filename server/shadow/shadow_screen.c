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

#include <freerdp/config.h>

#include <winpr/assert.h>

#include <freerdp/log.h>

#include "shadow_surface.h"

#include "shadow_screen.h"
#include "shadow_lobby.h"

#define TAG SERVER_TAG("shadow")

BOOL shadow_screen_all_monitors(const rdpShadowServer* server)
{
	WINPR_ASSERT(server);
	const rdpShadowSubsystem* subsystem = server->subsystem;
	WINPR_ASSERT(subsystem);

	if ((subsystem->numMonitors < 2) || server->shareSubRect)
		return FALSE;
	switch (server->monitorsChoice)
	{
		case SHADOW_MONITORS_CHOICE_ONE:
			return FALSE;
		case SHADOW_MONITORS_CHOICE_ALL:
			return subsystem->allMonitors != SHADOW_ALL_MONITORS_NONE;
		case SHADOW_MONITORS_CHOICE_DEFAULT:
		default:
			return subsystem->allMonitors == SHADOW_ALL_MONITORS_DEFAULT;
	}
}

/* The part of the desktop the screen shares, in the subsystem's coordinates: the selected
 * monitor, or the bounding box of every monitor */
WINPR_ATTR_NODISCARD
static BOOL shadow_screen_shared_rect(const rdpShadowServer* server, UINT16* x, UINT16* y,
                                      UINT16* width, UINT16* height)
{
	const rdpShadowSubsystem* subsystem = server->subsystem;
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->selectedMonitor < ARRAYSIZE(subsystem->monitors));

	const MONITOR_DEF* selected = &subsystem->monitors[subsystem->selectedMonitor];
	INT64 left = selected->left;
	INT64 top = selected->top;
	INT64 right = selected->right;
	INT64 bottom = selected->bottom;
	if (shadow_screen_all_monitors(server))
	{
		const UINT32 count = MIN(subsystem->numMonitors, ARRAYSIZE(subsystem->monitors));
		for (UINT32 index = 0; index < count; index++)
		{
			const MONITOR_DEF* monitor = &subsystem->monitors[index];
			left = MIN(left, monitor->left);
			top = MIN(top, monitor->top);
			right = MAX(right, monitor->right);
			bottom = MAX(bottom, monitor->bottom);
		}
	}

	const INT64 w = right - left + 1;
	const INT64 h = bottom - top + 1;
	if ((left < 0) || (left > UINT16_MAX) || (top < 0) || (top > UINT16_MAX) || (w < 0) ||
	    (w > UINT16_MAX) || (h < 0) || (h > UINT16_MAX))
	{
		WLog_ERR(TAG,
		         "the shared part of the desktop %" PRId64 ",%" PRId64 " %" PRId64 "x%" PRId64
		         " does not fit a surface",
		         left, top, w, h);
		return FALSE;
	}
	*x = (UINT16)left;
	*y = (UINT16)top;
	*width = (UINT16)w;
	*height = (UINT16)h;
	return TRUE;
}

rdpShadowScreen* shadow_screen_new(rdpShadowServer* server)
{
	WINPR_ASSERT(server);
	WINPR_ASSERT(server->subsystem);

	rdpShadowScreen* screen = (rdpShadowScreen*)calloc(1, sizeof(rdpShadowScreen));

	if (!screen)
		goto fail;

	screen->server = server;

	{
		const rdpShadowSubsystem* subsystem = server->subsystem;
		if (!InitializeCriticalSectionAndSpinCount(&(screen->lock), 4000))
			goto fail;

		region16_init(&(screen->invalidRegion));

		{
			UINT16 x = 0;
			UINT16 y = 0;
			UINT16 width = 0;
			UINT16 height = 0;
			if (!shadow_screen_shared_rect(server, &x, &y, &width, &height))
				goto fail;
			if (shadow_screen_all_monitors(server))
				WLog_INFO(TAG,
				          "sharing all %" PRIu32 " monitors: %" PRIu16 "x%" PRIu16 " at %" PRIu16
				          ",%" PRIu16,
				          subsystem->numMonitors, width, height, x, y);

			screen->width = width;
			screen->height = height;

			screen->primary = shadow_surface_new(server, x, y, width, height);

			if (!screen->primary)
				goto fail;

			server->surface = screen->primary;

			screen->lobby = shadow_surface_new(server, x, y, width, height);
		}
	}

	if (!screen->lobby)
		goto fail;

	server->lobby = screen->lobby;

	if (!shadow_client_init_lobby(server))
		goto fail;

	return screen;

fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	shadow_screen_free(screen);
	WINPR_PRAGMA_DIAG_POP

	return nullptr;
}

void shadow_screen_free(rdpShadowScreen* screen)
{
	if (!screen)
		return;

	DeleteCriticalSection(&(screen->lock));

	region16_uninit(&(screen->invalidRegion));

	if (screen->primary)
	{
		shadow_surface_free(screen->primary);
		screen->primary = nullptr;
	}

	if (screen->lobby)
	{
		shadow_surface_free(screen->lobby);
		screen->lobby = nullptr;
	}

	free(screen);
}

BOOL shadow_screen_resize(rdpShadowScreen* screen)
{
	if (!screen)
		return FALSE;

	WINPR_ASSERT(screen->server);
	WINPR_ASSERT(screen->server->subsystem);

	UINT16 x = 0;
	UINT16 y = 0;
	UINT16 width = 0;
	UINT16 height = 0;
	if (!shadow_screen_shared_rect(screen->server, &x, &y, &width, &height))
		return FALSE;

	if (shadow_surface_resize(screen->primary, x, y, width, height) &&
	    shadow_surface_resize(screen->lobby, x, y, width, height))
	{
		if (((UINT32)width != screen->width) || ((UINT32)height != screen->height))
		{
			/* screen size is changed. Store new size and reinit lobby */
			screen->width = (UINT32)width;
			screen->height = (UINT32)height;
			return shadow_client_init_lobby(screen->server);
		}
		return TRUE;
	}

	return FALSE;
}
