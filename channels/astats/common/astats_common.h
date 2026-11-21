/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Advanced Statistics Virtual Channel Extension: the PDUs
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

#ifndef FREERDP_INT_ASTATS_COMMON_H
#define FREERDP_INT_ASTATS_COMMON_H

#include <winpr/assert.h>
#include <winpr/stream.h>

#include <freerdp/channels/astats.h>
#include <freerdp/channels/log.h>

/** a HELLO PDU: the connection's id */
WINPR_ATTR_NODISCARD
static inline BOOL astats_write_hello(wStream* s, UINT64 connectionId)
{
	WINPR_ASSERT(s);
	if (!Stream_EnsureRemainingCapacity(s, 12))
		return FALSE;
	Stream_Write_UINT16(s, ASTATS_PDU_HELLO);
	Stream_Write_UINT16(s, ASTATS_VERSION);
	Stream_Write_UINT64(s, connectionId);
	return TRUE;
}

/** a REPORT PDU with count values (at most ASTATS_MAX_VALUES) */
WINPR_ATTR_NODISCARD
static inline BOOL astats_write_report(wStream* s, const ASTATS_VALUE* values, size_t count)
{
	WINPR_ASSERT(s);
	if ((count > ASTATS_MAX_VALUES) || (!values && (count > 0)))
		return FALSE;
	if (!Stream_EnsureRemainingCapacity(s, 6 + 10 * count))
		return FALSE;
	Stream_Write_UINT16(s, ASTATS_PDU_REPORT);
	Stream_Write_UINT16(s, ASTATS_VERSION);
	Stream_Write_UINT16(s, (UINT16)count);
	for (size_t x = 0; x < count; x++)
	{
		Stream_Write_UINT16(s, values[x].id);
		Stream_Write_UINT64(s, values[x].value);
	}
	return TRUE;
}

typedef struct
{
	UINT16 pduType; /* ASTATS_PDU_*, or a type this reader does not know */
	UINT64 connectionId;
	ASTATS_VALUE values[ASTATS_MAX_VALUES];
	size_t count;
} ASTATS_PDU;

/** Reads one PDU. A type it does not know is read as that type with nothing in it; the ids
 *  are read as they come, known or not (the reader keeps the ones it knows).
 *  @return FALSE when the PDU is cut short or has more values than ASTATS_MAX_VALUES */
WINPR_ATTR_NODISCARD
static inline BOOL astats_read(wStream* s, ASTATS_PDU* pdu)
{
	WINPR_ASSERT(s);
	WINPR_ASSERT(pdu);
	memset(pdu, 0, sizeof(*pdu));
	if (!Stream_CheckAndLogRequiredLength(CHANNELS_TAG("astats"), s, 4))
		return FALSE;
	Stream_Read_UINT16(s, pdu->pduType);
	UINT16 version = 0;
	Stream_Read_UINT16(s, version);
	WINPR_UNUSED(version); /* a newer one only appends */

	switch (pdu->pduType)
	{
		case ASTATS_PDU_HELLO:
			if (!Stream_CheckAndLogRequiredLength(CHANNELS_TAG("astats"), s, 8))
				return FALSE;
			Stream_Read_UINT64(s, pdu->connectionId);
			return TRUE;
		case ASTATS_PDU_REPORT:
		{
			if (!Stream_CheckAndLogRequiredLength(CHANNELS_TAG("astats"), s, 2))
				return FALSE;
			UINT16 count = 0;
			Stream_Read_UINT16(s, count);
			if (count > ASTATS_MAX_VALUES)
				return FALSE;
			if (!Stream_CheckAndLogRequiredLengthOfSize(CHANNELS_TAG("astats"), s, count, 10ull))
				return FALSE;
			for (size_t x = 0; x < count; x++)
			{
				Stream_Read_UINT16(s, pdu->values[x].id);
				Stream_Read_UINT64(s, pdu->values[x].value);
			}
			pdu->count = count;
			return TRUE;
		}
		default:
			return TRUE;
	}
}

#endif /* FREERDP_INT_ASTATS_COMMON_H */
