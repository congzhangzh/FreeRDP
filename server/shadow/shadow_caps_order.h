/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The order the shadow server tries a client's GFX capability sets in
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

#ifndef FREERDP_SERVER_SHADOW_CAPS_ORDER_H
#define FREERDP_SERVER_SHADOW_CAPS_ORDER_H

#include <winpr/wtypes.h>

#include <freerdp/channels/rdpgfx.h>

/* The capability versions to try a client's Caps Advertise with, the first that works is
 * confirmed. followClient: the client's own sets from its last to its first, those the server
 * supports. Clients list them from the least to the most preferred (8.0 ... 10.7, 11.x, FRDP_1,
 * FRDP_2), so for a client that keeps that order this is the server's own order; one with an order
 * of its own puts the set it prefers last. Without followClient: the server's own order (FRDP_2,
 * FRDP_1, 11.x, 10.7 ... 10.0). 8.1 and 8.0 are tried after these, as before. Returns how many
 * versions were written. */
WINPR_ATTR_NODISCARD
size_t shadow_caps_order(const RDPGFX_CAPSET* capsSets, size_t count, BOOL followClient,
                         UINT32* versions, size_t size);

#endif /* FREERDP_SERVER_SHADOW_CAPS_ORDER_H */
