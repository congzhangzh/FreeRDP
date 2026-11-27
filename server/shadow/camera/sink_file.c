/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a sink writing the frames to files, for tests
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

#include <winpr/assert.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/string.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>

#include "camera_sink.h"

#define TAG SERVER_TAG("shadow.camera")

/*
 * <dir>/frames.<yuyv|nv12>: the frames one after the other, each frame_size bytes.
 * <dir>/index.txt: a header line, then one line per frame:
 *   # format=YUY2 width=1280 height=720 fps=30 frame_size=1843200
 *   <number> <ms since the sink opened> <placeholder|client> <offset> [<detail>]
 * Each line is flushed with its frame, so a test can read them while the server runs.
 */
typedef struct
{
	ShadowCameraSink sink;
	FILE* frames;
	FILE* index;
	size_t frameSize;
	UINT64 count;
	UINT64 startMs;
} FileSink;

static BOOL file_sink_write(ShadowCameraSink* sink, const ShadowCameraImage* frame,
                            SHADOW_CAMERA_FRAME_KIND kind, const char* detail)
{
	FileSink* fs = (FileSink*)sink;
	WINPR_ASSERT(fs);
	WINPR_ASSERT(frame);

	if ((frame->format != sink->format) || (frame->width != sink->width) ||
	    (frame->height != sink->height))
		return FALSE;

	/* the planes of a tightly packed frame follow each other */
	const size_t offset = (size_t)(fs->count * fs->frameSize);
	if (fwrite(frame->planes[0], 1, fs->frameSize, fs->frames) != fs->frameSize)
	{
		WLog_ERR(TAG, "camera: writing frame %" PRIu64 " failed", fs->count);
		return FALSE;
	}
	(void)fflush(fs->frames);

	(void)fprintf(fs->index, "%" PRIu64 " %" PRIu64 " %s %" PRIuz "%s%s\n", fs->count,
	              GetTickCount64() - fs->startMs,
	              (kind == SHADOW_CAMERA_FRAME_CLIENT) ? "client" : "placeholder", offset,
	              detail ? " " : "", detail ? detail : "");
	(void)fflush(fs->index);
	fs->count++;
	return TRUE;
}

static void file_sink_free(ShadowCameraSink* sink)
{
	FileSink* fs = (FileSink*)sink;
	if (!fs)
		return;
	if (fs->frames)
		(void)fclose(fs->frames);
	if (fs->index)
		(void)fclose(fs->index);
	free(fs);
}

ShadowCameraSink* shadow_camera_sink_file_new(const char* dir, SHADOW_CAMERA_PIXEL format,
                                              UINT32 width, UINT32 height, UINT32 fps)
{
	if (!dir || (*dir == '\0') ||
	    ((format != SHADOW_CAMERA_PIXEL_YUY2) && (format != SHADOW_CAMERA_PIXEL_NV12)))
		return nullptr;

	const size_t frameSize = shadow_camera_image_size(format, width, height);
	if (frameSize == 0)
		return nullptr;

	if (!winpr_PathFileExists(dir) && !winpr_PathMakePath(dir, nullptr))
	{
		WLog_ERR(TAG, "camera: unable to create %s", dir);
		return nullptr;
	}

	FileSink* fs = calloc(1, sizeof(FileSink));
	if (!fs)
		return nullptr;

	fs->sink.name = "file";
	fs->sink.format = format;
	fs->sink.width = width;
	fs->sink.height = height;
	fs->sink.fps = fps;
	fs->sink.placeholderIntervalMs = 0;
	fs->sink.Write = file_sink_write;
	fs->sink.Free = file_sink_free;
	fs->frameSize = frameSize;
	fs->startMs = GetTickCount64();

	char* framesPath =
	    GetCombinedPath(dir, (format == SHADOW_CAMERA_PIXEL_NV12) ? "frames.nv12" : "frames.yuyv");
	char* indexPath = GetCombinedPath(dir, "index.txt");
	if (framesPath && indexPath)
	{
		fs->frames = winpr_fopen(framesPath, "wb");
		fs->index = winpr_fopen(indexPath, "w");
	}
	if (!fs->frames || !fs->index)
	{
		WLog_ERR(TAG, "camera: unable to open the files of the file sink in %s", dir);
		free(framesPath);
		free(indexPath);
		file_sink_free(&fs->sink);
		return nullptr;
	}

	(void)fprintf(fs->index,
	              "# format=%s width=%" PRIu32 " height=%" PRIu32 " fps=%" PRIu32
	              " frame_size=%" PRIuz "\n",
	              shadow_camera_pixel_name(format), width, height, fps, frameSize);
	(void)fflush(fs->index);
	WLog_INFO(TAG, "camera: file sink %s (%s %" PRIu32 "x%" PRIu32 ")", framesPath,
	          shadow_camera_pixel_name(format), width, height);
	free(framesPath);
	free(indexPath);
	return &fs->sink;
}
