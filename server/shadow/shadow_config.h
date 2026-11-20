/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The shadow server's configuration file
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

#ifndef FREERDP_SERVER_SHADOW_CONFIG_H
#define FREERDP_SERVER_SHADOW_CONFIG_H

#include <winpr/wtypes.h>

/* The file's settings: KEY=VALUE lines with the names of the FREERDP_SHADOW_* environment
 * variables (the format systemd's EnvironmentFile= reads), # or ; comments, blank lines, a value
 * in quotes or not. Each becomes an environment variable of the process unless the environment
 * already has it, so the environment wins over the file and everything that reads a
 * FREERDP_SHADOW_* variable gets the file's value too. A line that is no FREERDP_SHADOW_*
 * setting is logged with its file and line and passed over. required: a file that cannot be
 * read is an error; otherwise a missing file means no settings. */
WINPR_ATTR_NODISCARD
BOOL shadow_config_load(const char* path, BOOL required);

/* " (from <file>:<line>)" for a setting the file gave, "" otherwise: for the messages about a
 * value */
const char* shadow_config_origin(const char* name);

/* The value the file gave for name, also when the environment has its own and wins; nullptr
 * when the file has none. origin (may be nullptr) gets " (from <file>:<line>)". For a setting
 * made of keys (FREERDP_SHADOW_CODEC) the file's keys go first and the environment's over them. */
const char* shadow_config_file_value(const char* name, const char** origin);

/* <the FreeRDP config directory>/shadow/shadow.conf, to free; nullptr without one */
WINPR_ATTR_MALLOC(free, 1)
char* shadow_config_default_path(void);

#endif /* FREERDP_SERVER_SHADOW_CONFIG_H */
