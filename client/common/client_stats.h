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

#include <freerdp/client/rdpgfx.h>

#if !defined(_WIN32)
/* The web client's reader, see client_stats.c: built everywhere but Windows, so tests run it;
 * only the web client calls it, through CLIENT_STATS_WEB_* */
FREERDP_LOCAL void client_stats_web_attach(const rdpContext* context);
FREERDP_LOCAL void client_stats_web_detach(const rdpContext* context);
FREERDP_LOCAL void client_stats_web_barrier(void);
FREERDP_LOCAL void client_stats_web_gfx_connected(RdpgfxClientContext* gfx);
FREERDP_LOCAL uint64_t client_stats_web_value(size_t index);
FREERDP_LOCAL const char* client_stats_web_string(size_t index, char* buffer, size_t size);
#endif

#if defined(__EMSCRIPTEN__)
#define CLIENT_STATS_WEB_ATTACH(context) client_stats_web_attach(context)
#define CLIENT_STATS_WEB_DETACH(context) client_stats_web_detach(context)
#define CLIENT_STATS_WEB_BARRIER() client_stats_web_barrier()
#define CLIENT_STATS_WEB_GFX_CONNECTED(gfx) client_stats_web_gfx_connected(gfx)
#else
#define CLIENT_STATS_WEB_ATTACH(context) WINPR_UNUSED(context)
#define CLIENT_STATS_WEB_DETACH(context) WINPR_UNUSED(context)
#define CLIENT_STATS_WEB_BARRIER() (void)0
#define CLIENT_STATS_WEB_GFX_CONNECTED(gfx) WINPR_UNUSED(gfx)
#endif

#endif /* FREERDP_CLIENT_COMMON_CLIENT_STATS_H */
