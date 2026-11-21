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

#ifndef FREERDP_CHANNEL_ASTATS_SERVER_ASTATS_H
#define FREERDP_CHANNEL_ASTATS_SERVER_ASTATS_H

#include <freerdp/api.h>
#include <freerdp/types.h>
#include <freerdp/channels/astats.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/** One client's FreeRDP::Advanced::Stats. No thread of its own: the server calls
	 *  astats_server_context_check() from the thread that serves the client's channels, and
	 *  astats_server_context_report() at the end of each period. The channel is asked for once,
	 *  when the dynamic channels are up; a client without it (no /astats) refuses it and is
	 *  never asked again. @since version 3.34.0 */
	typedef struct s_astats_server_context astats_server_context;

	FREERDP_API void astats_server_context_free(astats_server_context* context);

	WINPR_ATTR_MALLOC(astats_server_context_free, 1)
	WINPR_ATTR_NODISCARD
	FREERDP_API astats_server_context* astats_server_context_new(HANDLE vcm, UINT64 connectionId);

	/** Opens the channel when it can, and sends the HELLO once the client has it open. Cheap
	 *  once the channel is open or refused. */
	FREERDP_API void astats_server_context_check(astats_server_context* context);

	/** Sends a REPORT when the channel is open; otherwise nothing.
	 *  @return TRUE when it was sent */
	FREERDP_API BOOL astats_server_context_report(astats_server_context* context,
	                                              const ASTATS_VALUE* values, size_t count);

	/** @return TRUE while the channel is open */
	WINPR_ATTR_NODISCARD
	FREERDP_API BOOL astats_server_context_is_open(const astats_server_context* context);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_CHANNEL_ASTATS_SERVER_ASTATS_H */
