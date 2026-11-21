/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Advanced Statistics Virtual Channel Extension: the client side
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

#include <stdio.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/stream.h>

#include <freerdp/freerdp.h>
#include <freerdp/metrics.h>
#include <freerdp/channels/log.h>
#include <freerdp/client/channels.h>

#include "../common/astats_common.h"

#define TAG CHANNELS_TAG("astats.client")

WINPR_STATIC_ASSERT(ASTATS_ID_LAST < FREERDP_METRICS_SERVER_STATS);

typedef struct
{
	GENERIC_DYNVC_PLUGIN base;
	rdpContext* rdpcontext;
} ASTATS_PLUGIN;

/* The server's numbers go where the client's statistics are (rdpMetrics), so the overlay, the
 * stats socket and the web client read them like their own (client_stats.c) */
static UINT astats_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	WINPR_ASSERT(callback);
	ASTATS_PLUGIN* astats = (ASTATS_PLUGIN*)callback->plugin;
	WINPR_ASSERT(astats);

	ASTATS_PDU* pdu = calloc(1, sizeof(*pdu));
	if (!pdu)
		return CHANNEL_RC_NO_MEMORY;
	if (!astats_read(data, pdu))
	{
		free(pdu);
		return ERROR_INVALID_DATA;
	}

	rdpMetrics* metrics = astats->rdpcontext ? astats->rdpcontext->metrics : nullptr;
	if (metrics)
	{
		switch (pdu->pduType)
		{
			case ASTATS_PDU_HELLO:
				(void)_snprintf(metrics->ServerConnectionId, sizeof(metrics->ServerConnectionId),
				                "%016" PRIx64, pdu->connectionId);
				WLog_DBG(TAG, "connection %s", metrics->ServerConnectionId);
				break;
			case ASTATS_PDU_REPORT:
			{
				UINT64 values[FREERDP_METRICS_SERVER_STATS] = WINPR_C_ARRAY_INIT;
				for (size_t x = 0; x < pdu->count; x++)
				{
					if (pdu->values[x].id < ARRAYSIZE(values))
						values[pdu->values[x].id] = pdu->values[x].value;
				}
				memcpy(metrics->ServerStats, values, sizeof(values));
				/* last: a reader that sees the count go up sees the values of that report */
				metrics->ServerStatsReports++;
				WLog_DBG(TAG, "report %" PRIu64 ": period %" PRIu64 ", sent %" PRIu64
				              ", frames %" PRIu64 "-%" PRIu64,
				         metrics->ServerStatsReports, values[ASTATS_ID_PERIOD_NUMBER],
				         values[ASTATS_ID_SENT], values[ASTATS_ID_FRAME_ID_FIRST],
				         values[ASTATS_ID_FRAME_ID_LAST]);
				break;
			}
			default:
				break;
		}
	}
	free(pdu);
	return CHANNEL_RC_OK;
}

static UINT astats_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	if (callback)
	{
		ASTATS_PLUGIN* astats = (ASTATS_PLUGIN*)callback->plugin;
		/* nothing writes on it (the client sends nothing): no lock needed, only no pointer
		 * left to the freed callback */
		if (astats && astats->base.listener_callback &&
		    (astats->base.listener_callback->channel_callback == callback))
			astats->base.listener_callback->channel_callback = nullptr;
		free(callback);
	}
	return CHANNEL_RC_OK;
}

static UINT init_plugin_cb(GENERIC_DYNVC_PLUGIN* base, rdpContext* rcontext,
                           WINPR_ATTR_UNUSED rdpSettings* settings)
{
	ASTATS_PLUGIN* astats = (ASTATS_PLUGIN*)base;
	WINPR_ASSERT(astats);
	astats->rdpcontext = rcontext;
	return CHANNEL_RC_OK;
}

static void terminate_plugin_cb(WINPR_ATTR_UNUSED GENERIC_DYNVC_PLUGIN* base)
{
}

static const IWTSVirtualChannelCallback astats_functions = { astats_on_data_received,
	                                                         nullptr, /* Open */
	                                                         astats_on_close, nullptr };

FREERDP_ENTRY_POINT(UINT VCAPITYPE astats_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))
{
	return freerdp_generic_DVCPluginEntry(pEntryPoints, TAG, ASTATS_DVC_CHANNEL_NAME,
	                                      sizeof(ASTATS_PLUGIN), sizeof(GENERIC_CHANNEL_CALLBACK),
	                                      &astats_functions, init_plugin_cb, terminate_plugin_cb);
}
