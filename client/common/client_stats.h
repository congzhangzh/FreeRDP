/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Runtime statistics of a client session
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

#ifndef FREERDP_CLIENT_COMMON_CLIENT_STATS_H
#define FREERDP_CLIENT_COMMON_CLIENT_STATS_H

#include <freerdp/api.h>
#include <freerdp/freerdp.h>

#if defined(__EMSCRIPTEN__)
/* The session a page reads, see client_stats.c */
FREERDP_LOCAL void client_stats_web_attach(const rdpContext* context);
FREERDP_LOCAL void client_stats_web_detach(const rdpContext* context);
FREERDP_LOCAL void client_stats_web_barrier(void);
#else
#define client_stats_web_attach(context) WINPR_UNUSED(context)
#define client_stats_web_detach(context) WINPR_UNUSED(context)
#define client_stats_web_barrier() (void)0
#endif

#endif /* FREERDP_CLIENT_COMMON_CLIENT_STATS_H */
