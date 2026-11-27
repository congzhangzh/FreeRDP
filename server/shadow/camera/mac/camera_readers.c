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

#include <string.h>

#include "camera_readers.h"

#define CAM_READERS_MAX 1000000

void cam_readers_init(cam_readers_state* state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

bool cam_readers_update(cam_readers_state* state, int count, uint64_t nowMs, uint32_t holdMs)
{
	if (!state)
		return false;
	if (count > 0)
	{
		state->idle = false;
		if (state->active)
			return false;
		state->active = true;
		return true;
	}
	if (!state->active)
		return false;
	if (!state->idle)
	{
		state->idle = true;
		state->idleSinceMs = nowMs;
	}
	if ((nowMs >= state->idleSinceMs) && (nowMs - state->idleSinceMs >= holdMs))
	{
		state->active = false;
		state->idle = false;
		return true;
	}
	return false;
}

bool cam_readers_parse(const char* text, int* count)
{
	if (!text || !count || !*text)
		return false;
	long value = 0;
	for (const char* p = text; *p; p++)
	{
		if ((*p < '0') || (*p > '9'))
			return false;
		value = value * 10 + (*p - '0');
		if (value > CAM_READERS_MAX)
			return false;
	}
	*count = (int)value;
	return true;
}
