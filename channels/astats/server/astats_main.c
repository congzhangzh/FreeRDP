/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Advanced Statistics Virtual Channel Extension: the server side
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

#include <freerdp/config.h>

#include <stdlib.h>

#include <winpr/assert.h>
#include <winpr/stream.h>
#include <winpr/wtsapi.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/log.h>
#include <freerdp/server/astats.h>
#include <freerdp/channels/wtsvc.h>

#include "../common/astats_common.h"

#define TAG CHANNELS_TAG("astats.server")

typedef enum
{
	ASTATS_SERVER_IDLE,    /* not asked for yet: the dynamic channels are not up */
	ASTATS_SERVER_OPENING, /* asked for, the client has not answered */
	ASTATS_SERVER_OPEN,    /* open, the HELLO sent */
	ASTATS_SERVER_DONE     /* refused, closed or broken: never again */
} eAStatsServerState;

struct s_astats_server_context
{
	HANDLE vcm;
	HANDLE channel;
	UINT64 connectionId;
	eAStatsServerState state;
	wStream* s;
};

static BOOL astats_server_write(astats_server_context* context)
{
	ULONG written = 0;
	const size_t length = Stream_GetPosition(context->s);
	if ((length > UINT32_MAX) ||
	    !WTSVirtualChannelWrite(context->channel, Stream_BufferAs(context->s, char), (ULONG)length,
	                            &written) ||
	    (written != length))
	{
		WLog_DBG(TAG, "the client's channel is gone: no more reports");
		context->state = ASTATS_SERVER_DONE;
		return FALSE;
	}
	return TRUE;
}

/* asks for the channel once the dynamic channels are up */
static void astats_server_open(astats_server_context* context)
{
	if (WTSVirtualChannelManagerGetDrdynvcState(context->vcm) != DRDYNVC_STATE_READY)
		return;

	DWORD* sessionId = nullptr;
	DWORD bytes = 0;
	if (!WTSQuerySessionInformationA(context->vcm, WTS_CURRENT_SESSION, WTSSessionId,
	                                 (LPSTR*)&sessionId, &bytes) ||
	    !sessionId)
	{
		context->state = ASTATS_SERVER_DONE;
		return;
	}
	const DWORD id = *sessionId;
	WTSFreeMemory(sessionId);

	context->channel = WTSVirtualChannelOpenEx(id, ASTATS_DVC_CHANNEL_NAME, WTS_CHANNEL_OPTION_DYNAMIC);
	context->state = context->channel ? ASTATS_SERVER_OPENING : ASTATS_SERVER_DONE;
}

/* the client's answer: open (send the HELLO), not yet, or refused */
static void astats_server_opening(astats_server_context* context)
{
	BOOL* ready = nullptr;
	DWORD bytes = 0;
	if (!WTSVirtualChannelQuery(context->channel, WTSVirtualChannelReady, (PVOID*)&ready, &bytes))
	{
		/* refused: a client without /astats */
		WLog_DBG(TAG, "the client has no %s", ASTATS_DVC_CHANNEL_NAME);
		context->state = ASTATS_SERVER_DONE;
		return;
	}
	const BOOL open = ready && (bytes == sizeof(BOOL)) && *ready;
	WTSFreeMemory(ready);
	if (!open)
		return;

	Stream_ResetPosition(context->s);
	if (!astats_write_hello(context->s, context->connectionId))
	{
		context->state = ASTATS_SERVER_DONE;
		return;
	}
	context->state = ASTATS_SERVER_OPEN;
	if (astats_server_write(context))
		WLog_DBG(TAG, "open, connection %016" PRIx64, context->connectionId);
}

void astats_server_context_check(astats_server_context* context)
{
	if (!context)
		return;
	switch (context->state)
	{
		case ASTATS_SERVER_IDLE:
			astats_server_open(context);
			if (context->state == ASTATS_SERVER_OPENING)
				astats_server_opening(context);
			break;
		case ASTATS_SERVER_OPENING:
			astats_server_opening(context);
			break;
		default:
			break;
	}
}

BOOL astats_server_context_report(astats_server_context* context, const ASTATS_VALUE* values,
                                  size_t count)
{
	if (!context)
		return FALSE;
	astats_server_context_check(context);
	if (context->state != ASTATS_SERVER_OPEN)
		return FALSE;
	Stream_ResetPosition(context->s);
	if (!astats_write_report(context->s, values, count))
		return FALSE;
	return astats_server_write(context);
}

BOOL astats_server_context_is_open(const astats_server_context* context)
{
	return context && (context->state == ASTATS_SERVER_OPEN);
}

astats_server_context* astats_server_context_new(HANDLE vcm, UINT64 connectionId)
{
	astats_server_context* context = calloc(1, sizeof(*context));
	if (!context)
		return nullptr;
	context->vcm = vcm;
	context->connectionId = connectionId;
	context->s = Stream_New(nullptr, 6 + 10 * ASTATS_MAX_VALUES);
	if (!context->s)
	{
		free(context);
		return nullptr;
	}
	return context;
}

void astats_server_context_free(astats_server_context* context)
{
	if (!context)
		return;
	if (context->channel)
		(void)WTSVirtualChannelClose(context->channel);
	Stream_Free(context->s, TRUE);
	free(context);
}
