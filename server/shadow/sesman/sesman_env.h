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

#ifndef FREERDP_SERVER_SHADOW_SESMAN_ENV_H
#define FREERDP_SERVER_SHADOW_SESMAN_ENV_H

#include <stddef.h>

/* the settings of the shadow servers (FREERDP_SHADOW_*) passed on, at most this many */
#define SESMAN_ENV_MAX 32
/* "--setenv=NAME=value" */
#define SESMAN_ENV_LEN 256

/* Called for each setting that is not passed on, with why. */
typedef void (*sesman_env_dropped_fn)(const char* entry, const char* why);

/* Turns every FREERDP_SHADOW_* entry of envp into a systemd-run "--setenv=" option in out, but
 * the ones sesman sets itself (FREERDP_SHADOW_MUTTER_VIRTUAL). A setting that does not fit is
 * reported to dropped. Returns how many were written. */
size_t sesman_shadow_env(char* const* envp, char out[][SESMAN_ENV_LEN], size_t max,
                         sesman_env_dropped_fn dropped);

#endif /* FREERDP_SERVER_SHADOW_SESMAN_ENV_H */
