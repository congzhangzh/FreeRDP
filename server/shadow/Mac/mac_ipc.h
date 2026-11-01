/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server: the messages between the server (a root daemon) and its agents
 *
 * Copyright 2026 Cong Zhang
 *
 * Copyright 2026 Cong Zhang
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

#ifndef FREERDP_SERVER_SHADOW_MAC_IPC_H
#define FREERDP_SERVER_SHADOW_MAC_IPC_H

/* The server runs as a launchd daemon: it keeps the clients, TLS, the login and the encoder,
 * but it has no connection to the window server, so it can neither capture nor post input.
 * An agent does that in the session on the console (launchd starts one at the login window
 * and one in each user's session) and talks to the server over XPC, on the server's Mach
 * service. The server uses the agent of the session on the console: the one that said hello
 * last. Frames cross as IOSurface objects, without a copy.
 *
 * Every message is an XPC dictionary with a "type":
 *
 * agent -> server
 *   hello     uid, session ("LoginWindow" or "Aqua"), main (display id), displays: an array
 *             of { id, x, y, w, h } in global display coordinates (points)
 *   frame     surface (IOSurface XPC object), rects (CGRect array, the dirty rectangles in the
 *             frame's pixels); sent with a reply: the agent keeps the frame until the server
 *             answered, and has at most MAC_IPC_FRAMES_IN_FLIGHT out
 *   audio     samples (float, interleaved), frames, channels, rate
 *   stopped   the capture stream ended (the display turned off)
 *   cursor    w, h, xhot, yhot, pixels (32-bit BGRA, premultiplied)
 *   position  x, y: the pointer, in global display coordinates
 *
 * server -> agent
 *   start     display, width, height (of the frames, in pixels), fps, audio, rate, channels
 *   stop
 *   event     data: a CGEvent (CGEventCreateData), for the agent to post
 */

#define MAC_IPC_SERVICE_DEFAULT "org.freerdp.shadow"
#define MAC_IPC_FRAMES_IN_FLIGHT 2

#define MAC_IPC_TYPE "type"
#define MAC_IPC_HELLO "hello"
#define MAC_IPC_FRAME "frame"
#define MAC_IPC_AUDIO "audio"
#define MAC_IPC_STOPPED "stopped"
#define MAC_IPC_CURSOR "cursor"
#define MAC_IPC_POSITION "position"
#define MAC_IPC_START "start"
#define MAC_IPC_STOP "stop"
#define MAC_IPC_EVENT "event"

#define MAC_IPC_SESSION_LOGINWINDOW "LoginWindow"
#define MAC_IPC_SESSION_AQUA "Aqua"

#endif /* FREERDP_SERVER_SHADOW_MAC_IPC_H */
