/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2015 Jiang Zihao <zihao.jiang@yahoo.com>
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

#include <errno.h>
#include <stdlib.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>
#include <freerdp/codec/dsp.h>
#include <freerdp/server/server-common.h>

#include "shadow.h"

#include "shadow_rdpsnd.h"

#define TAG SERVER_TAG("shadow")

/* The milliseconds of sound gathered into one wave: rdpsnd's own default is 50 */
#define SOUND_PACKET_MS_NAME "FREERDP_SHADOW_SOUND_PACKET_MS"
#define SOUND_PACKET_MS_MIN 10
#define SOUND_PACKET_MS_MAX 200

/* How often the delay of the client's sound is logged, in seconds, 0 for never */
#define SOUND_DELAY_LOG_NAME "FREERDP_SHADOW_SOUND_DELAY_LOG"
#define SOUND_DELAY_LOG_DEFAULT 30
#define SOUND_DELAY_LOG_MAX 3600
/* A second answer later than this is a first one: a client may answer only once */
#define SOUND_SECOND_ANSWER_MS 5000

/* A client answers each wave twice ([MS-RDPEA] 2.2.3.8): when it arrives, with the wave's
 * timestamp, and when it is queued for playing, with the timestamp plus the time from its
 * arrival to its playing. The waves carry the capture time (GetTickCount64, low 16 bits), so
 * the first answer tells how long a wave took from the capture to the client and back, the
 * second how long the client keeps it before it plays. */
typedef struct
{
	CRITICAL_SECTION lock;
	UINT16 arrived[256];
	UINT64 answeredAt[256];
	UINT64 logMs;
	UINT64 logged;
	UINT32 arrivals;
	UINT64 arrivalSum;
	UINT32 arrivalMax;
	UINT32 plays;
	UINT64 playSum;
	UINT32 playMax;
} ShadowRdpsndDelay;

static void rdpsnd_activated(RdpsndServerContext* context)
{
	WINPR_ASSERT(context);
	for (size_t i = 0; i < context->num_client_formats; i++)
	{
		for (size_t j = 0; j < context->num_server_formats; j++)
		{
			if (audio_format_compatible(&context->server_formats[j], &context->client_formats[i]))
			{
				const UINT rc = context->SelectFormat(context, WINPR_ASSERTING_INT_CAST(UINT16, i));
				if (rc != CHANNEL_RC_OK)
					WLog_WARN(TAG, "SelectFormat failed with %" PRIu32, rc);
				return;
			}
		}
	}

	WLog_ERR(TAG, "Could not agree on a audio format with the server\n");
}

/* A setting from the environment, or its default when unset or out of range */
WINPR_ATTR_NODISCARD
static UINT32 shadow_rdpsnd_env_u32(const char* name, UINT32 value, UINT32 min, UINT32 max)
{
	const char* text = getenv(name);
	if (!text || (*text == '\0'))
		return value;

	char* end = nullptr;
	errno = 0;
	const unsigned long long parsed = strtoull(text, &end, 10);
	if ((errno != 0) || !end || (*end != '\0') || (parsed < min) || (parsed > max))
	{
		WLog_WARN(TAG, "%s=%s is not a number from %" PRIu32 " to %" PRIu32 ": %" PRIu32 " instead",
		          name, text, min, max, value);
		return value;
	}
	return (UINT32)parsed;
}

/* the milliseconds from a to b on the 16-bit clock of the waves; UINT32_MAX when b is before a
 * or the answer is too late to tell */
WINPR_ATTR_NODISCARD
static UINT32 shadow_rdpsnd_ms_between(UINT16 a, UINT16 b)
{
	/* the client takes its sum modulo UINT16_MAX, not 65536: one ms off at the wrap */
	const UINT16 diff = (UINT16)(b - a);
	if (diff > 30000)
		return UINT32_MAX;
	return diff;
}

static void shadow_rdpsnd_delay_log(ShadowRdpsndDelay* delay, UINT64 now)
{
	WINPR_ASSERT(delay);

	if (delay->logMs == 0)
		return;
	if (delay->logged == 0)
	{
		delay->logged = now;
		return;
	}
	if (now - delay->logged < delay->logMs)
		return;

	if ((delay->arrivals > 0) || (delay->plays > 0))
	{
		const UINT64 arrivalAvg = delay->arrivals ? delay->arrivalSum / delay->arrivals : 0;
		const UINT64 playAvg = delay->plays ? delay->playSum / delay->plays : 0;
		WLog_INFO(TAG,
		          "sound delay: capture to the client and back %" PRIu64 " ms (max %" PRIu32
		          "), the client plays a wave %" PRIu64 " ms after it arrives (max %" PRIu32
		          "), %" PRIu32 " waves in %" PRIu64 " s",
		          arrivalAvg, delay->arrivalMax, playAvg, delay->playMax, delay->arrivals,
		          (now - delay->logged) / 1000);
	}
	delay->logged = now;
	delay->arrivals = 0;
	delay->arrivalSum = 0;
	delay->arrivalMax = 0;
	delay->plays = 0;
	delay->playSum = 0;
	delay->playMax = 0;
}

static UINT shadow_rdpsnd_confirm_block(RdpsndServerContext* context, BYTE confirmBlockNum,
                                        UINT16 wtimestamp)
{
	WINPR_ASSERT(context);
	ShadowRdpsndDelay* delay = context->data;
	if (!delay)
		return CHANNEL_RC_OK;

	const UINT64 now = GetTickCount64();
	EnterCriticalSection(&delay->lock);
	const UINT64 first = delay->answeredAt[confirmBlockNum];
	if ((first == 0) || (now - first > SOUND_SECOND_ANSWER_MS))
	{
		/* the first answer: the wave arrived */
		delay->answeredAt[confirmBlockNum] = now;
		delay->arrived[confirmBlockNum] = wtimestamp;
		const UINT32 ms = shadow_rdpsnd_ms_between(wtimestamp, (UINT16)(now & 0xFFFF));
		if (ms != UINT32_MAX)
		{
			delay->arrivals++;
			delay->arrivalSum += ms;
			delay->arrivalMax = MAX(delay->arrivalMax, ms);
		}
	}
	else
	{
		/* the second answer: the wave was queued for playing */
		delay->answeredAt[confirmBlockNum] = 0;
		const UINT32 ms = shadow_rdpsnd_ms_between(delay->arrived[confirmBlockNum], wtimestamp);
		if (ms != UINT32_MAX)
		{
			delay->plays++;
			delay->playSum += ms;
			delay->playMax = MAX(delay->playMax, ms);
		}
	}
	shadow_rdpsnd_delay_log(delay, now);
	LeaveCriticalSection(&delay->lock);
	return CHANNEL_RC_OK;
}

int shadow_client_rdpsnd_init(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	RdpsndServerContext* rdpsnd = client->rdpsnd = rdpsnd_server_context_new(client->vcm);

	if (!rdpsnd)
	{
		return 0;
	}

	ShadowRdpsndDelay* delay = calloc(1, sizeof(ShadowRdpsndDelay));
	if (!delay)
		return -1;
	if (!InitializeCriticalSectionAndSpinCount(&delay->lock, 4000))
	{
		free(delay);
		return -1;
	}
	delay->logMs = 1000ULL * shadow_rdpsnd_env_u32(SOUND_DELAY_LOG_NAME, SOUND_DELAY_LOG_DEFAULT, 0,
	                                               SOUND_DELAY_LOG_MAX);
	rdpsnd->data = delay;
	rdpsnd->ConfirmBlock = shadow_rdpsnd_confirm_block;
	rdpsnd->latency =
	    shadow_rdpsnd_env_u32(SOUND_PACKET_MS_NAME, 50, SOUND_PACKET_MS_MIN, SOUND_PACKET_MS_MAX);

	if (client->subsystem->rdpsndFormats)
	{
		/* rdpsnd_server_context_free() frees the list: each client gets its own
		 * copy (shadow_client_rdpsnd_uninit() cleans up after a failure here) */
		const size_t count = client->subsystem->nRdpsndFormats;
		rdpsnd->server_formats = audio_formats_new(count);
		if (!rdpsnd->server_formats)
			return -1;

		for (size_t x = 0; x < count; x++)
		{
			if (!audio_format_copy(&client->subsystem->rdpsndFormats[x],
			                       &rdpsnd->server_formats[x]))
				return -1;
		}
		rdpsnd->num_server_formats = count;
	}
	else
	{
		rdpsnd->num_server_formats = server_rdpsnd_get_formats(&rdpsnd->server_formats);
	}

	if (rdpsnd->num_server_formats > 0)
		rdpsnd->src_format = &rdpsnd->server_formats[0];

	rdpsnd->Activated = rdpsnd_activated;

	const UINT error = rdpsnd->Initialize(rdpsnd, TRUE);
	if (error != CHANNEL_RC_OK)
		return -1;
	return 1;
}

void shadow_client_rdpsnd_uninit(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	if (client->rdpsnd)
	{
		ShadowRdpsndDelay* delay = client->rdpsnd->data;
		client->rdpsnd->Stop(client->rdpsnd);
		rdpsnd_server_context_free(client->rdpsnd);
		client->rdpsnd = nullptr;
		if (delay)
		{
			DeleteCriticalSection(&delay->lock);
			free(delay);
		}
	}
}
