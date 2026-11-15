/*
 * FreeRDP: A Remote Desktop Protocol Implementation
 * freerdp-shadow-sesman: the settings it passes on to the shadow servers
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

#include <stdio.h>
#include <string.h>

#include "sesman_env.h"

#define SHADOW_PREFIX "FREERDP_SHADOW_"

/* set by sesman for each user: an admin's value would only fight with it */
static const char* const own[] = { "FREERDP_SHADOW_MUTTER_VIRTUAL=" };

size_t sesman_shadow_env(char* const* envp, char out[][SESMAN_ENV_LEN], size_t max,
                         sesman_env_dropped_fn dropped)
{
	size_t count = 0;

	for (char* const* e = envp; e && *e; e++)
	{
		if (strncmp(*e, SHADOW_PREFIX, strlen(SHADOW_PREFIX)) != 0)
			continue;

		int mine = 0;
		for (size_t x = 0; x < sizeof(own) / sizeof(own[0]); x++)
		{
			if (strncmp(*e, own[x], strlen(own[x])) == 0)
				mine = 1;
		}
		if (mine)
		{
			if (dropped)
				dropped(*e, "sesman sets it itself");
			continue;
		}

		if (count >= max)
		{
			if (dropped)
				dropped(*e, "too many settings");
			continue;
		}

		const int n = snprintf(out[count], SESMAN_ENV_LEN, "--setenv=%s", *e);
		if ((n <= 0) || ((size_t)n >= SESMAN_ENV_LEN))
		{
			if (dropped)
				dropped(*e, "too long");
			continue;
		}
		count++;
	}
	return count;
}
