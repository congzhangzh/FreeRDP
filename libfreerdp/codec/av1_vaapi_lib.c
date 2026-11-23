/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * libva for the AV1 decoder and encoder (Linux), loaded at run time
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <winpr/synch.h>
#include <winpr/wlog.h>

#include "av1_vaapi_lib.h"

AV1_VA_API av1_va = WINPR_C_ARRAY_INIT;
static INIT_ONCE vaOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK va_load(PINIT_ONCE once, PVOID param, PVOID* context)
{
	WINPR_UNUSED(once);
	WINPR_UNUSED(param);
	WINPR_UNUSED(context);

	/* never closed: a driver may keep threads of its own */
	void* lib = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
	void* drm = lib ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : nullptr;
	if (!lib || !drm)
		return TRUE;
	BOOL ok = TRUE;
	av1_va.vaGetDisplayDRM = (__typeof__(av1_va.vaGetDisplayDRM))dlsym(drm, "vaGetDisplayDRM");
	ok = ok && av1_va.vaGetDisplayDRM;
#define VA_LOAD(name)                                         \
	av1_va.name = (__typeof__(av1_va.name))dlsym(lib, #name); \
	ok = ok && av1_va.name;
	AV1_VA_FUNCTIONS(VA_LOAD)
#undef VA_LOAD
	av1_va.loaded = ok;
	return TRUE;
}

BOOL av1_va_load(void)
{
	if (!InitOnceExecuteOnce(&vaOnce, va_load, nullptr, nullptr))
		return FALSE;
	return av1_va.loaded;
}

/* libva's own messages go to the log (not to stderr) */
static void va_message(void* context, const char* message)
{
	wLog* log = context;
	if (!log || !message)
		return;
	const size_t len = strlen(message);
	const int n = (len > 0) && (message[len - 1] == '\n') ? (int)len - 1 : (int)len;
	WLog_Print(log, WLOG_DEBUG, "libva: %.*s", n, message);
}

BOOL av1_va_supports(VADisplay dpy, VAEntrypoint entrypoint)
{
	const int maxProfiles = av1_va.vaMaxNumProfiles(dpy);
	if (maxProfiles <= 0)
		return FALSE;
	VAProfile* profiles = calloc((size_t)maxProfiles, sizeof(VAProfile));
	if (!profiles)
		return FALSE;
	int n = 0;
	BOOL have = FALSE;
	if (av1_va.vaQueryConfigProfiles(dpy, profiles, &n) == VA_STATUS_SUCCESS)
	{
		for (int i = 0; (i < n) && !have; i++)
			have = (profiles[i] == VAProfileAV1Profile0);
	}
	free(profiles);
	if (!have)
		return FALSE;

	const int maxEntrypoints = av1_va.vaMaxNumEntrypoints(dpy);
	if (maxEntrypoints <= 0)
		return FALSE;
	VAEntrypoint* entrypoints = calloc((size_t)maxEntrypoints, sizeof(VAEntrypoint));
	if (!entrypoints)
		return FALSE;
	have = FALSE;
	if (av1_va.vaQueryConfigEntrypoints(dpy, VAProfileAV1Profile0, entrypoints, &n) ==
	    VA_STATUS_SUCCESS)
	{
		for (int i = 0; (i < n) && !have; i++)
			have = (entrypoints[i] == entrypoint);
	}
	free(entrypoints);
	if (!have)
		return FALSE;

	VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat };
	if (av1_va.vaGetConfigAttributes(dpy, VAProfileAV1Profile0, entrypoint, &attrib, 1) !=
	    VA_STATUS_SUCCESS)
		return FALSE;
	return (attrib.value != VA_ATTRIB_NOT_SUPPORTED) && (attrib.value & VA_RT_FORMAT_YUV420);
}

void av1_va_close(int fd, VADisplay dpy)
{
	if (dpy)
		(void)av1_va.vaTerminate(dpy);
	if (fd >= 0)
		close(fd);
}

BOOL av1_va_open(wLog* log, int node, AV1_VA_USABLE usable, int* fd, VADisplay* dpy, char* name,
                 size_t nameSize)
{
	char path[64] = WINPR_C_ARRAY_INIT;
	(void)snprintf(path, sizeof(path), "/dev/dri/renderD%d", node);
	const int f = open(path, O_RDWR | O_CLOEXEC);
	if (f < 0)
		return FALSE;
	VADisplay d = av1_va.vaGetDisplayDRM(f);
	if (!d)
	{
		close(f);
		return FALSE;
	}
	(void)av1_va.vaSetErrorCallback(d, va_message, log);
	(void)av1_va.vaSetInfoCallback(d, va_message, log);
	int major = 0;
	int minor = 0;
	/* a display that fails to initialize is terminated too, which releases it */
	if ((av1_va.vaInitialize(d, &major, &minor) != VA_STATUS_SUCCESS) || !usable(d))
	{
		av1_va_close(f, d);
		return FALSE;
	}
	*fd = f;
	*dpy = d;
	const char* vendor = av1_va.vaQueryVendorString(d);
	(void)snprintf(name, nameSize, "%s (%s)", vendor ? vendor : "VA-API", path);
	return TRUE;
}
