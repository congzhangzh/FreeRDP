/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: whether an app on the Mac reads the virtual camera
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_READERS_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_READERS_H

/* The readers count (camera_config.h, CAM_MAC_READERS_*) turned into "in use", plain C so it is
 * tested anywhere: in use at once when an app starts reading; not in use only once nobody has
 * read for the hold time (an app that stops and starts its stream again, to change its format
 * say, does not stop the client's camera). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct
	{
		bool active; /* in use, as reported */
		bool idle;   /* no reader since idleSinceMs */
		uint64_t idleSinceMs;
	} cam_readers_state;

	void cam_readers_init(cam_readers_state* state);

	/* count: the readers now (< 0: unknown, no device: as 0); nowMs: a monotonic clock. True
	 * when state->active changed. Called on each notification and each poll, so a hold ends at
	 * the poll after it. */
	bool cam_readers_update(cam_readers_state* state, int count, uint64_t nowMs, uint32_t holdMs);

	/* the property's value: a decimal count, nothing else; false otherwise */
	bool cam_readers_parse(const char* text, int* count);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_READERS_H */
