/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the file sink (CI)
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

#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>
#include <winpr/wtypes.h>

#include "camera_sink.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			goto fail;                                                       \
		}                                                                    \
	} while (0)

static char* read_all(const char* path, size_t* size)
{
	FILE* fp = winpr_fopen(path, "rb");
	if (!fp)
		return nullptr;
	char* data = calloc(1, 4096);
	*size = data ? fread(data, 1, 4095, fp) : 0;
	(void)fclose(fp);
	return data;
}

int TestShadowCameraFileSink(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	int rc = -1;
	char name[64] = WINPR_C_ARRAY_INIT;
	(void)_snprintf(name, sizeof(name), "TestShadowCameraFileSink-%" PRIu32, GetCurrentProcessId());
	char* temp = GetKnownPath(KNOWN_PATH_TEMP);
	char* dir = temp ? GetCombinedPath(temp, name) : nullptr;
	char* frames = dir ? GetCombinedPath(dir, "frames.yuyv") : nullptr;
	char* index = dir ? GetCombinedPath(dir, "index.txt") : nullptr;
	ShadowCameraSink* sink = nullptr;
	char* text = nullptr;
	char* data = nullptr;
	CHECK(frames && index);

	CHECK(!shadow_camera_sink_file_new(dir, SHADOW_CAMERA_PIXEL_BGRX32, 4, 2, 30));
	CHECK(!shadow_camera_sink_file_new(dir, SHADOW_CAMERA_PIXEL_YUY2, 3, 2, 30));
	sink = shadow_camera_sink_file_new(dir, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, 30);
	CHECK(sink);
	CHECK(sink->placeholderIntervalMs == 0);

	BYTE a[16];
	BYTE b[16];
	memset(a, 0x11, sizeof(a));
	memset(b, 0x22, sizeof(b));
	ShadowCameraImage fa = WINPR_C_ARRAY_INIT;
	ShadowCameraImage fb = WINPR_C_ARRAY_INIT;
	ShadowCameraImage wrong = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_image_wrap(&fa, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, a, sizeof(a)));
	CHECK(shadow_camera_image_wrap(&fb, SHADOW_CAMERA_PIXEL_YUY2, 4, 2, b, sizeof(b)));
	CHECK(shadow_camera_image_wrap(&wrong, SHADOW_CAMERA_PIXEL_YUY2, 2, 2, b, sizeof(b)));
	CHECK(sink->Write(sink, &fa, SHADOW_CAMERA_FRAME_PLACEHOLDER, nullptr));
	CHECK(sink->Write(sink, &fb, SHADOW_CAMERA_FRAME_CLIENT, "H264 640x480"));
	CHECK(!sink->Write(sink, &wrong, SHADOW_CAMERA_FRAME_CLIENT, nullptr));
	sink->Free(sink);
	sink = nullptr;

	size_t size = 0;
	data = read_all(frames, &size);
	CHECK(data && (size == 32));
	CHECK((memcmp(data, a, 16) == 0) && (memcmp(data + 16, b, 16) == 0));

	text = read_all(index, &size);
	CHECK(text);
	(void)printf("%s", text);
	CHECK(strstr(text, "# format=YUY2 width=4 height=2 fps=30 frame_size=16\n") == text);
	{
		const char* line1 = strchr(text, '\n') + 1;
		const char* line2 = strchr(line1, '\n') + 1;
		unsigned long long n = 0;
		unsigned long long ms = 0;
		char kind[16] = WINPR_C_ARRAY_INIT;
		unsigned long long offset = 0;
		CHECK(sscanf(line1, "%llu %llu %15s %llu", &n, &ms, kind, &offset) == 4);
		CHECK((n == 0) && (strcmp(kind, "placeholder") == 0) && (offset == 0));
		CHECK(sscanf(line2, "%llu %llu %15s %llu", &n, &ms, kind, &offset) == 4);
		CHECK((n == 1) && (strcmp(kind, "client") == 0) && (offset == 16));
		CHECK(strstr(line2, " H264 640x480\n"));
		CHECK(*(strchr(line2, '\n') + 1) == '\0');
	}
	rc = 0;
fail:
	if (sink)
		sink->Free(sink);
	free(text);
	free(data);
	if (frames)
		(void)winpr_DeleteFile(frames);
	if (index)
		(void)winpr_DeleteFile(index);
	if (dir)
		(void)winpr_RemoveDirectory(dir);
	free(frames);
	free(index);
	free(dir);
	free(temp);
	return rc;
}
