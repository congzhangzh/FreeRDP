/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Advanced Statistics Virtual Channel Extension: the server's numbers for the client
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

#ifndef FREERDP_CHANNEL_ASTATS_H
#define FREERDP_CHANNEL_ASTATS_H

#include <winpr/wtypes.h>

#include <freerdp/api.h>
#include <freerdp/dvc.h>
#include <freerdp/types.h>

/** @defgroup channel_astats FreeRDP::Advanced::Stats
 *
 * The server tells the client what its GFX frames did, period by period (5 s), so a client
 * shows the server's side next to its own: the frames captured, sent and held, the encode
 * time, the acknowledgement delay. One way, server to client; a client opens it with /astats.
 *
 * Every PDU, little endian: UINT16 pduType, UINT16 version (ASTATS_VERSION).
 *  - ASTATS_PDU_HELLO, once when the channel opens: UINT64 connectionId, a random number the
 *    server also writes in its statistics socket (/stats-ipc), to join the two.
 *  - ASTATS_PDU_REPORT, at the end of each period: UINT16 count, then count times
 *    (UINT16 id, UINT64 value) with the ASTATS_ID_* ids; a value the server does not know is
 *    left out.
 * A reader skips PDU types and ids it does not know, so new ones are added without a new
 * version.
 * @{ */

#define ASTATS_CHANNEL_NAME "astats"
#define ASTATS_DVC_CHANNEL_NAME "FreeRDP::Advanced::Stats"

#define ASTATS_VERSION 1u

#define ASTATS_PDU_HELLO 1u
#define ASTATS_PDU_REPORT 2u

/* the value ids of a report: never renumbered, new ones appended */
typedef enum
{
	ASTATS_ID_PERIOD_MS = 1,
	ASTATS_ID_CAPTURED = 2,
	ASTATS_ID_SENT = 3,
	ASTATS_ID_HELD = 4,
	ASTATS_ID_MERGED = 5,
	ASTATS_ID_ACKS = 6,
	ASTATS_ID_ACK_DELAY_AVG_MS = 7,
	ASTATS_ID_ACK_DELAY_MAX_MS = 8,
	ASTATS_ID_ACK_DELAY_MIN_MS = 9,
	ASTATS_ID_ENCODE_AVG_US = 10,
	ASTATS_ID_ENCODE_MAX_US = 11,
	ASTATS_ID_IN_FLIGHT_MAX = 12,
	ASTATS_ID_IN_FLIGHT_LIMIT = 13,
	ASTATS_ID_FRAME_ID_FIRST = 14,
	ASTATS_ID_FRAME_ID_LAST = 15,
	ASTATS_ID_FRAME_ID_ACKED_LAST = 16,
	ASTATS_ID_CAPTURE_AVG_US = 17,
	ASTATS_ID_CAPTURE_MAX_US = 18,
	ASTATS_ID_ROUND_AVG_US = 19,
	ASTATS_ID_ROUND_MAX_US = 20,
	ASTATS_ID_PERIOD_NUMBER = 21,  /* the server's count of this connection's periods */
	ASTATS_ID_PROCESS_CPU_MS = 22, /* the server process's CPU time in the period, all threads */
	ASTATS_ID_HOST_LOAD_X100 = 23, /* the host's 1-minute load average x 100 (Linux only) */
	ASTATS_ID_LAST = ASTATS_ID_HOST_LOAD_X100
} ASTATS_ID;

/* the most values one report carries */
#define ASTATS_MAX_VALUES 64u

typedef struct
{
	UINT16 id;
	UINT64 value;
} ASTATS_VALUE;

/** @} */

#endif /* FREERDP_CHANNEL_ASTATS_H */
