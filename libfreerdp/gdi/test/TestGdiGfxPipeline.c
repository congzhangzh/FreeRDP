/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The graphics pipeline is published only when it is ready
 *
 * Copyright 2026 Cong Zhang <congzhangzh@gmail.com>
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

#include <stdio.h>

#include <winpr/crt.h>
#include <winpr/synch.h>

#include <freerdp/freerdp.h>
#include <freerdp/codecs.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>

/* gdi->gfx is how another thread finds the graphics pipeline (the channel's thread sets it up),
 * so it is set only once everything is ready, the lock included: never after a failed setup. */
static BOOL test_init(rdpContext* context, UINT32 width, UINT32 height, BOOL ready)
{
	BOOL rc = FALSE;
	rdpGdi gdi = WINPR_C_ARRAY_INIT;
	gdi.context = context;
	RdpgfxClientContext* gfx = calloc(1, sizeof(RdpgfxClientContext));
	if (!gfx)
		return FALSE;
	if (!freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopWidth, width) ||
	    !freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopHeight, height))
		goto out;

	const BOOL init = gdi_graphics_pipeline_init(&gdi, gfx);
	if (init != ready)
	{
		(void)fprintf(stderr, "%" PRIu32 "x%" PRIu32 ": init %d, expected %d\n", width, height,
		              init, ready);
		goto out;
	}
	if (!init)
	{
		if (gdi.gfx)
		{
			(void)fprintf(stderr, "a failed setup published the graphics pipeline\n");
			goto out;
		}
		freerdp_client_codecs_free(gfx->codecs);
		rc = TRUE;
		goto out;
	}

	if (gdi.gfx != gfx)
	{
		(void)fprintf(stderr, "the ready graphics pipeline is not published\n");
		goto out;
	}
	EnterCriticalSection(&gfx->mux); /* ready, as whoever finds it there uses it */
	LeaveCriticalSection(&gfx->mux);
	gdi_graphics_pipeline_uninit(&gdi, gfx);
	rc = (gdi.gfx == nullptr);
out:
	free(gfx);
	return rc;
}

int TestGdiGfxPipeline(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
	int rc = -1;
	freerdp* instance = freerdp_new();
	if (!instance)
		return -1;
	instance->ContextSize = sizeof(rdpContext);
	if (!freerdp_context_new(instance))
		goto out;

	/* a size the planar codec refuses (its planes would exceed 4 GiB): the setup fails */
	if (!test_init(instance->context, 65535, 65535, FALSE))
		goto out;
	if (!test_init(instance->context, 64, 64, TRUE))
		goto out;
	rc = 0;
out:
	freerdp_context_free(instance);
	freerdp_free(instance);
	return rc;
}
