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

#include <freerdp/config.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/string.h>

#include <freerdp/log.h>
#include <freerdp/settings.h>

#include "shadow_config.h"

#define TAG SERVER_TAG("shadow")

#define SHADOW_CONFIG_PREFIX "FREERDP_SHADOW_"
#define SHADOW_CONFIG_ENV_PREFIX "FREERDP_SHADOW_ENV_"
#define SHADOW_CONFIG_ENV_MAX 64

#if defined(_WIN32)
#define shadow_config_environ _environ
#else
extern char** environ;
#define shadow_config_environ environ
#endif
#define SHADOW_CONFIG_LINE 1024
#define SHADOW_CONFIG_MAX 64

/* each setting the file gave: its value, where it came from (for the messages about it), and
 * whether it became the environment's (applied) or the environment had its own */
static struct
{
	char name[96];
	char value[SHADOW_CONFIG_LINE];
	char origin[160];
	BOOL applied;
} shadow_config_origins[SHADOW_CONFIG_MAX];
static size_t shadow_config_count = 0;

static BOOL shadow_config_setenv(const char* name, const char* value)
{
#if defined(_WIN32)
	return _putenv_s(name, value) == 0;
#else
	return setenv(name, value, 0) == 0;
#endif
}

static char* shadow_config_trim(char* text)
{
	while (isspace((unsigned char)*text))
		text++;
	size_t length = strlen(text);
	while ((length > 0) && isspace((unsigned char)text[length - 1]))
		text[--length] = '\0';
	return text;
}

static BOOL shadow_config_name_valid(const char* name)
{
	if (strncmp(name, SHADOW_CONFIG_PREFIX, strlen(SHADOW_CONFIG_PREFIX)) != 0)
		return FALSE;
	if (strlen(name) <= strlen(SHADOW_CONFIG_PREFIX))
		return FALSE;
	for (const char* p = name; *p; p++)
	{
		if (!isupper((unsigned char)*p) && !isdigit((unsigned char)*p) && (*p != '_'))
			return FALSE;
	}
	return TRUE;
}

BOOL shadow_config_load(const char* path, BOOL required)
{
	shadow_config_count = 0;
	if (!path)
		return !required;

	FILE* fp = winpr_fopen(path, "r");
	if (!fp)
	{
		if (required)
			WLog_ERR(TAG, "cannot read the configuration file %s", path);
		return !required;
	}
	WLog_INFO(TAG, "configuration file %s", path);

	char line[SHADOW_CONFIG_LINE] = { 0 };
	size_t number = 0;
	while (fgets(line, sizeof(line), fp))
	{
		number++;
		if (!strchr(line, '\n') && !feof(fp))
		{
			WLog_WARN(TAG, "%s:%" PRIuz ": a line longer than %d characters: passed over", path,
			          number, SHADOW_CONFIG_LINE - 1);
			int c = 0;
			while (((c = fgetc(fp)) != EOF) && (c != '\n'))
				;
			continue;
		}

		char* text = shadow_config_trim(line);
		if ((*text == '\0') || (*text == '#') || (*text == ';'))
			continue;

		char* equal = strchr(text, '=');
		if (!equal)
		{
			WLog_WARN(TAG, "%s:%" PRIuz ": not a KEY=VALUE line: passed over", path, number);
			continue;
		}
		*equal = '\0';
		char* name = shadow_config_trim(text);
		char* value = shadow_config_trim(equal + 1);
		const size_t length = strlen(value);
		if ((length >= 2) && ((value[0] == '"') || (value[0] == '\'')) &&
		    (value[length - 1] == value[0]))
		{
			value[length - 1] = '\0';
			value++;
		}

		if (!shadow_config_name_valid(name))
		{
			WLog_WARN(TAG,
			          "%s:%" PRIuz ": %s is not a " SHADOW_CONFIG_PREFIX "* setting: passed over",
			          path, number, name);
			continue;
		}

		BOOL applied = FALSE;
		if (getenv(name))
			WLog_DBG(TAG, "%s:%" PRIuz ": %s is set in the environment, which wins", path, number,
			         name);
		else if (!shadow_config_setenv(name, value))
		{
			WLog_WARN(TAG, "%s:%" PRIuz ": %s could not be set: passed over", path, number, name);
			continue;
		}
		else
			applied = TRUE;
		if (shadow_config_count < SHADOW_CONFIG_MAX)
		{
			(void)_snprintf(shadow_config_origins[shadow_config_count].name,
			                sizeof(shadow_config_origins[shadow_config_count].name), "%s", name);
			(void)_snprintf(shadow_config_origins[shadow_config_count].value,
			                sizeof(shadow_config_origins[shadow_config_count].value), "%s", value);
			(void)_snprintf(shadow_config_origins[shadow_config_count].origin,
			                sizeof(shadow_config_origins[shadow_config_count].origin),
			                " (from %s:%" PRIuz ")", path, number);
			shadow_config_origins[shadow_config_count].applied = applied;
			shadow_config_count++;
		}
	}
	(void)fclose(fp);
	return TRUE;
}

const char* shadow_config_origin(const char* name)
{
	if (!name)
		return "";
	for (size_t x = 0; x < shadow_config_count; x++)
	{
		if (shadow_config_origins[x].applied && (strcmp(shadow_config_origins[x].name, name) == 0))
			return shadow_config_origins[x].origin;
	}
	return "";
}

const char* shadow_config_file_value(const char* name, const char** origin)
{
	for (size_t x = 0; name && (x < shadow_config_count); x++)
	{
		/* the first line for a name, as for the environment: a later one does not replace it */
		if (strcmp(shadow_config_origins[x].name, name) == 0)
		{
			if (origin)
				*origin = shadow_config_origins[x].origin;
			return shadow_config_origins[x].value;
		}
	}
	return nullptr;
}

char* shadow_config_default_path(void)
{
	char* config = freerdp_settings_get_config_path();
	if (!config)
		return nullptr;
	char* dir = GetCombinedPath(config, "shadow");
	free(config);
	if (!dir)
		return nullptr;
	char* path = GetCombinedPath(dir, "shadow.conf");
	free(dir);
	return path;
}

BOOL shadow_config_env_split(const char* entry, char* name, size_t size, const char** value)
{
	if (!entry || !name || (size == 0) || !value)
		return FALSE;
	if (strncmp(entry, SHADOW_CONFIG_ENV_PREFIX, strlen(SHADOW_CONFIG_ENV_PREFIX)) != 0)
		return FALSE;

	const char* start = entry + strlen(SHADOW_CONFIG_ENV_PREFIX);
	const char* equal = strchr(start, '=');
	if (!equal || (equal == start) || ((size_t)(equal - start) >= size))
		return FALSE;
	if (isdigit((unsigned char)*start))
		return FALSE;
	for (const char* p = start; p < equal; p++)
	{
		if (!isalnum((unsigned char)*p) && (*p != '_'))
			return FALSE;
	}

	memcpy(name, start, (size_t)(equal - start));
	name[equal - start] = '\0';
	*value = equal + 1;
	return TRUE;
}

size_t shadow_config_pass_env(void)
{
	/* setenv() may move the environment while it is walked: take the entries first */
	char* entries[SHADOW_CONFIG_ENV_MAX] = WINPR_C_ARRAY_INIT;
	size_t count = 0;
	for (char** e = shadow_config_environ; e && *e; e++)
	{
		if (strncmp(*e, SHADOW_CONFIG_ENV_PREFIX, strlen(SHADOW_CONFIG_ENV_PREFIX)) != 0)
			continue;
		if (count >= ARRAYSIZE(entries))
		{
			WLog_WARN(
			    TAG, "more than %d " SHADOW_CONFIG_ENV_PREFIX "* settings: the rest is passed over",
			    SHADOW_CONFIG_ENV_MAX);
			break;
		}
		entries[count] = _strdup(*e);
		if (entries[count])
			count++;
	}

	size_t applied = 0;
	for (size_t x = 0; x < count; x++)
	{
		char name[96] = WINPR_C_ARRAY_INIT;
		const char* value = nullptr;
		const char* equal = strchr(entries[x], '=');
		const int length = equal ? (int)(equal - entries[x]) : (int)strlen(entries[x]);

		if (!shadow_config_env_split(entries[x], name, sizeof(name), &value))
			WLog_WARN(TAG, "%.*s is not " SHADOW_CONFIG_ENV_PREFIX "<NAME>: passed over", length,
			          entries[x]);
		else if (getenv(name))
			WLog_INFO(TAG, "%.*s: %s is set already, which wins", length, entries[x], name);
		else if (!shadow_config_setenv(name, value))
			WLog_WARN(TAG, "%.*s: %s could not be set", length, entries[x], name);
		else
		{
			WLog_INFO(TAG, "%s=%s (from %.*s)", name, value, length, entries[x]);
			applied++;
		}
		free(entries[x]);
	}
	return applied;
}
