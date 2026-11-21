/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server: the GFX counters on a local socket (/stats-ipc)
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

#ifndef FREERDP_SERVER_SHADOW_STATS_IPC_H
#define FREERDP_SERVER_SHADOW_STATS_IPC_H

#include <winpr/wtypes.h>
#include <freerdp/api.h>

/* The schema of a line: adding a field keeps the version; renaming or removing one is a new
 * version. The client's socket (/sdl-stats-ipc) is "freerdp-sdl-stats". */
#define SHADOW_STATS_IPC_SCHEMA "freerdp-shadow-stats"
#define SHADOW_STATS_IPC_VERSION 1u

/* readers at once; one more is closed at once */
#define SHADOW_STATS_IPC_MAX_READERS 8u

/* clients a line carries; more are left out of it */
#define SHADOW_STATS_IPC_MAX_CLIENTS 64u

/** A read-only stream of lines on a local stream socket (AF_UNIX, on Windows too) for a program
 * of the same user that measures the server, the same rules as the SDL client's /sdl-stats-ipc.
 *
 * A reader that connects gets the newest line at once, then each new line, until it closes;
 * what it sends is ignored. A reader slower than the lines gets the newest one when it is ready,
 * never a queue. Only the user that runs the server may connect: the socket file is 0600 in a
 * directory only that user (or root) may change, and on Linux and macOS each reader's user is
 * checked too. One thread of its own does the socket work.
 *
 * Each line carries the newest period of every client (shadow_stats_ipc_client_period()), so a
 * reader that misses a line loses no period as long as it reads once a period. */
typedef struct s_shadow_stats_ipc SHADOW_STATS_IPC;

#ifdef __cplusplus
extern "C"
{
#endif

	/** The path used without one, per user and per process: freerdp-shadow-<pid>.stats.sock in
	 *  XDG_RUNTIME_DIR or TMPDIR, or in %LOCALAPPDATA%\FreeRDP on Windows.
	 *  @return FALSE with the reason in error */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL shadow_stats_ipc_default_path(char* path, size_t size,
	                                                                      char* error,
	                                                                      size_t errorSize);

	FREERDP_LOCAL void shadow_stats_ipc_free(SHADOW_STATS_IPC* ipc);

	/** Opens the socket at path. A file there is replaced only when it is a socket of this user
	 *  that nobody listens on.
	 *  @return the socket, or nullptr with the reason in error */
	WINPR_ATTR_MALLOC(shadow_stats_ipc_free, 1)
	WINPR_ATTR_NODISCARD FREERDP_LOCAL SHADOW_STATS_IPC* shadow_stats_ipc_open(const char* path,
	                                                                           char* error,
	                                                                           size_t errorSize);

	/** Any thread: sends line (without its line end) to every reader. */
	FREERDP_LOCAL void shadow_stats_ipc_publish(SHADOW_STATS_IPC* ipc, const char* line);

	/** Any thread: a client's newest period (a JSON object, shadow_gfx_stats_json()) replaces its
	 *  last one, and a line with every client goes out. */
	FREERDP_LOCAL void shadow_stats_ipc_client_period(SHADOW_STATS_IPC* ipc, UINT64 connectionId,
	                                                  const char* object);

	/** Any thread: the client is gone; the next lines leave it out. */
	FREERDP_LOCAL void shadow_stats_ipc_client_gone(SHADOW_STATS_IPC* ipc, UINT64 connectionId);

	WINPR_ATTR_NODISCARD FREERDP_LOCAL const char* shadow_stats_ipc_path(const SHADOW_STATS_IPC* ipc);

	/** The peer of a connected TCP socket as "ip:port", an IPv6 one as "[ip]:port" (no zone),
	 *  which the client sees as its own local end (behind a relay, a tunnel or NAT they differ).
	 *  @return FALSE when it has none */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL BOOL shadow_stats_ipc_peer_name(int sockfd, char* buffer,
	                                                                   size_t size);

	/** @return the readers connected now */
	WINPR_ATTR_NODISCARD FREERDP_LOCAL size_t shadow_stats_ipc_readers(SHADOW_STATS_IPC* ipc);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_STATS_IPC_H */
