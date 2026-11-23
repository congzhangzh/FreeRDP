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

#ifndef FREERDP_LIB_CODEC_AV1_VAAPI_LIB_H
#define FREERDP_LIB_CODEC_AV1_VAAPI_LIB_H

#include <winpr/wtypes.h>
#include <winpr/wlog.h>

#include <va/va.h>
#include <va/va_drm.h>

/* the DRM render nodes, /dev/dri/renderD128 and up */
#define AV1_VA_RENDER_FIRST 128
#define AV1_VA_RENDER_COUNT 64

/* libva is loaded at run time: a program without it (or without a driver for its GPU) still
 * runs, with no device. These are the functions the decoder and the encoder call. */
#define AV1_VA_FUNCTIONS(X)     \
	X(vaInitialize)             \
	X(vaTerminate)              \
	X(vaErrorStr)               \
	X(vaSetErrorCallback)       \
	X(vaSetInfoCallback)        \
	X(vaQueryVendorString)      \
	X(vaMaxNumProfiles)         \
	X(vaQueryConfigProfiles)    \
	X(vaMaxNumEntrypoints)      \
	X(vaQueryConfigEntrypoints) \
	X(vaGetConfigAttributes)    \
	X(vaCreateConfig)           \
	X(vaDestroyConfig)          \
	X(vaCreateSurfaces)         \
	X(vaDestroySurfaces)        \
	X(vaCreateContext)          \
	X(vaDestroyContext)         \
	X(vaCreateBuffer)           \
	X(vaDestroyBuffer)          \
	X(vaBeginPicture)           \
	X(vaRenderPicture)          \
	X(vaEndPicture)             \
	X(vaSyncSurface)            \
	X(vaCreateImage)            \
	X(vaDestroyImage)           \
	X(vaGetImage)               \
	X(vaPutImage)               \
	X(vaMapBuffer)              \
	X(vaUnmapBuffer)

#define AV1_VA_FIELD(name) __typeof__(name)* name;
typedef struct
{
	BOOL loaded;
	__typeof__(vaGetDisplayDRM)* vaGetDisplayDRM;
	AV1_VA_FUNCTIONS(AV1_VA_FIELD)
} AV1_VA_API;
#undef AV1_VA_FIELD

/** libva's functions, valid once av1_va_load() returned TRUE. */
extern AV1_VA_API av1_va;

/** Load libva.so.2 and libva-drm.so.2 (once). @return FALSE when they are not there. */
BOOL av1_va_load(void);

/** Whether the display does AV1 profile 0 (8 bit 4:2:0) at @p entrypoint. */
BOOL av1_va_supports(VADisplay dpy, VAEntrypoint entrypoint);

typedef BOOL (*AV1_VA_USABLE)(VADisplay dpy);

/** Open render node @p node (AV1_VA_RENDER_FIRST + n) and initialize its display, libva's
 * messages going to @p log (DEBUG).
 *
 * @return TRUE and @p fd, @p dpy and @p name ("<vendor> (/dev/dri/renderDNNN)") set when the
 *         node is there and @p usable says so; FALSE with nothing left open otherwise. */
BOOL av1_va_open(wLog* log, int node, AV1_VA_USABLE usable, int* fd, VADisplay* dpy, char* name,
                 size_t nameSize);

/** Terminate @p dpy and close @p fd (either may be unset: nullptr, < 0). */
void av1_va_close(int fd, VADisplay dpy);

#endif /* FREERDP_LIB_CODEC_AV1_VAAPI_LIB_H */
