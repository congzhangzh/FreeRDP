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

/* TRUE when a client's sets are in an order of its own: by the server's priority (8.0 lowest,
 * FRDP_2 highest) they go up at one place and down at another. A list in ascending order (FreeRDP's
 * client) or descending order is not; versions the server does not know are passed over, and 10.6
 * and 10.6_ERR count as one. */
WINPR_ATTR_NODISCARD
BOOL shadow_caps_reordered(const RDPGFX_CAPSET* capsSets, size_t count);

/* The capability versions to try a client's Caps Advertise with, the first that works is
 * confirmed. followClient and a client with an order of its own (shadow_caps_reordered): the
 * client's sets from its last to its first, those the server supports, so the set it prefers goes
 * last. Otherwise the server's own order (FRDP_2, FRDP_1, 11.x, 10.7 ... 10.0), whatever the
 * client's. 8.1 and 8.0 are tried after these, as before. Returns how many versions were
 * written. */
WINPR_ATTR_NODISCARD
size_t shadow_caps_order(const RDPGFX_CAPSET* capsSets, size_t count, BOOL followClient,
                         UINT32* versions, size_t size);

#endif /* FREERDP_SERVER_SHADOW_CAPS_ORDER_H */
