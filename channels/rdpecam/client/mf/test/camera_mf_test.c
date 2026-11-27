/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Media Foundation camera HAL tests, shared part
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
#include <string.h>

#include <winpr/file.h>
#include <winpr/interlocked.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/wlog.h>

#include "camera_mf_test.h"

#define CAM_MF_TEST_TAG "rdpecam-mf.client"
#define CAM_MF_TEST_MAX_LINES 64

typedef struct
{
	DWORD level;
	char text[512];
} CamMfTestLine;

static CRITICAL_SECTION lock;
static CamMfTestLine lines[CAM_MF_TEST_MAX_LINES];
static size_t nLines = 0;
static volatile LONG samples = 0;

static BOOL cam_mf_test_log_message(const wLogMessage* msg)
{
	if (!msg || !msg->PrefixString || !strstr(msg->PrefixString, CAM_MF_TEST_TAG))
		return TRUE;
	(void)printf("log %" PRIu32 ": %s\n", (UINT32)msg->Level,
	             msg->TextString ? msg->TextString : "");
	if (msg->Level < WLOG_INFO)
		return TRUE;

	EnterCriticalSection(&lock);
	if (nLines < CAM_MF_TEST_MAX_LINES)
	{
		lines[nLines].level = msg->Level;
		(void)_snprintf(lines[nLines].text, sizeof(lines[nLines].text), "%s",
		                msg->TextString ? msg->TextString : "");
		nLines++;
	}
	LeaveCriticalSection(&lock);
	return TRUE;
}

static BOOL cam_mf_test_log_other(const wLogMessage* msg)
{
	WINPR_UNUSED(msg);
	return TRUE;
}

BOOL cam_mf_test_log_start(void)
{
	if (!InitializeCriticalSectionEx(&lock, 0, 0))
		return FALSE;

	wLog* root = WLog_GetRoot();
	if (!WLog_SetLogAppenderType(root, WLOG_APPENDER_CALLBACK))
		return FALSE;

	wLogAppender* appender = WLog_GetLogAppender(root);
	if (!appender)
		return FALSE;

	wLogCallbacks callbacks = { .data = cam_mf_test_log_other,
		                        .image = cam_mf_test_log_other,
		                        .message = cam_mf_test_log_message,
		                        .package = cam_mf_test_log_other };
	if (!WLog_ConfigureAppender(appender, "callbacks", (void*)&callbacks))
		return FALSE;

	wLogLayout* layout = WLog_GetLogLayout(root);
	if (!WLog_Layout_SetPrefixFormat(root, layout, "%mn"))
		return FALSE;
	if (!WLog_OpenAppender(root))
		return FALSE;
	return WLog_SetLogLevel(root, WLOG_DEBUG);
}

size_t cam_mf_test_log_count(void)
{
	EnterCriticalSection(&lock);
	const size_t n = nLines;
	LeaveCriticalSection(&lock);
	return n;
}

size_t cam_mf_test_log_find(DWORD level, const char* text)
{
	size_t found = 0;
	EnterCriticalSection(&lock);
	for (size_t x = 0; x < nLines; x++)
	{
		if ((lines[x].level == level) && strstr(lines[x].text, text))
			found++;
	}
	LeaveCriticalSection(&lock);
	return found;
}

void cam_mf_test_log_print(void)
{
	EnterCriticalSection(&lock);
	for (size_t x = 0; x < nLines; x++)
		(void)printf("line %" PRIuz " (level %" PRIu32 "): %s\n", x, (UINT32)lines[x].level,
		             lines[x].text);
	LeaveCriticalSection(&lock);
}

static ICamHal* registered = nullptr;

static UINT cam_mf_test_register(IWTSPlugin* plugin, ICamHal* hal)
{
	WINPR_UNUSED(plugin);
	registered = hal;
	return CHANNEL_RC_OK;
}

UINT cam_mf_test_entry(int argc, const char* args[], ICamHal** hal)
{
	char* argv[8] = { "rdpecam" };
	int n = 1;
	for (int x = 0; (x < argc) && (n < 8); x++)
		argv[n++] = (char*)args[x];

	ADDIN_ARGV addin = { .argc = n, .argv = argv };
	FREERDP_CAMERA_HAL_ENTRY_POINTS entry = { .plugin = nullptr,
		                                      .pRegisterCameraHal = cam_mf_test_register,
		                                      .ecam = nullptr,
		                                      .args = &addin };
	registered = nullptr;
	const UINT rc = mf_freerdp_rdpecam_client_subsystem_entry(&entry);
	*hal = registered;
	return rc;
}

INT16 cam_mf_test_media_types(ICamHal* hal, const char* deviceId, CAM_MEDIA_TYPE_DESCRIPTION* types,
                              size_t* nTypes)
{
	const CAM_MEDIA_FORMAT order[] = { CAM_MEDIA_FORMAT_H264, CAM_MEDIA_FORMAT_MJPG,
		                               CAM_MEDIA_FORMAT_YUY2, CAM_MEDIA_FORMAT_NV12,
		                               CAM_MEDIA_FORMAT_I420, CAM_MEDIA_FORMAT_RGB24,
		                               CAM_MEDIA_FORMAT_RGB32 };
	CAM_MEDIA_FORMAT_INFO supported[ARRAYSIZE(order)] = WINPR_C_ARRAY_INIT;
	for (size_t x = 0; x < ARRAYSIZE(order); x++)
		supported[x] = (CAM_MEDIA_FORMAT_INFO){ order[x], order[x] };
	return hal->GetMediaTypeDescriptions(hal, deviceId, 0, supported, ARRAYSIZE(supported), types,
	                                     nTypes);
}

UINT cam_mf_test_sample(CameraDevice* dev, size_t streamIndex, const BYTE* sample, size_t size)
{
	WINPR_UNUSED(dev);
	WINPR_UNUSED(streamIndex);
	WINPR_UNUSED(sample);
	const LONG n = InterlockedIncrement(&samples);
	if ((n <= 2) || ((n % 10) == 0))
		(void)printf("sample %" PRId32 ": %" PRIuz " bytes\n", (INT32)n, size);
	return CHANNEL_RC_OK;
}

LONG cam_mf_test_samples(void)
{
	return InterlockedCompareExchange(&samples, 0, 0);
}

BOOL cam_mf_test_wait_samples(LONG count, DWORD timeoutMs)
{
	const UINT64 start = GetTickCount64();
	while (cam_mf_test_samples() < count)
	{
		if (GetTickCount64() - start > timeoutMs)
			return FALSE;
		Sleep(10);
	}
	return TRUE;
}

static void put32(FILE* f, UINT32 v)
{
	const BYTE b[4] = { (BYTE)(v & 0xff), (BYTE)((v >> 8) & 0xff), (BYTE)((v >> 16) & 0xff),
		                (BYTE)((v >> 24) & 0xff) };
	(void)fwrite(b, 1, sizeof(b), f);
}

static void put16(FILE* f, UINT16 v)
{
	const BYTE b[2] = { (BYTE)(v & 0xff), (BYTE)((v >> 8) & 0xff) };
	(void)fwrite(b, 1, sizeof(b), f);
}

static void fourcc(FILE* f, const char* cc)
{
	(void)fwrite(cc, 1, 4, f);
}

BOOL cam_mf_test_write_avi(const char* path, UINT32 width, UINT32 height, UINT32 fps, UINT32 frames)
{
	const UINT32 frameSize = width * height * 2;
	const UINT32 avihSize = 56;
	const UINT32 strhSize = 56;
	const UINT32 strfSize = 40;
	const UINT32 strlSize = 4 + (8 + strhSize) + (8 + strfSize);
	const UINT32 hdrlSize = 4 + (8 + avihSize) + (8 + strlSize);
	const UINT32 moviSize = 4 + frames * (8 + frameSize);
	const UINT32 idx1Size = frames * 16;
	const UINT32 riffSize = 4 + (8 + hdrlSize) + (8 + moviSize) + (8 + idx1Size);

	BYTE* frame = (BYTE*)malloc(frameSize);
	FILE* f = winpr_fopen(path, "wb");
	if (!frame || !f)
	{
		free(frame);
		if (f)
			(void)fclose(f);
		return FALSE;
	}

	fourcc(f, "RIFF");
	put32(f, riffSize);
	fourcc(f, "AVI ");

	fourcc(f, "LIST");
	put32(f, hdrlSize);
	fourcc(f, "hdrl");
	fourcc(f, "avih");
	put32(f, avihSize);
	put32(f, 1000000 / fps);   /* dwMicroSecPerFrame */
	put32(f, frameSize * fps); /* dwMaxBytesPerSec */
	put32(f, 0);               /* dwPaddingGranularity */
	put32(f, 0x10);            /* dwFlags: AVIF_HASINDEX */
	put32(f, frames);          /* dwTotalFrames */
	put32(f, 0);               /* dwInitialFrames */
	put32(f, 1);               /* dwStreams */
	put32(f, frameSize);       /* dwSuggestedBufferSize */
	put32(f, width);
	put32(f, height);
	for (int x = 0; x < 4; x++)
		put32(f, 0);

	fourcc(f, "LIST");
	put32(f, strlSize);
	fourcc(f, "strl");
	fourcc(f, "strh");
	put32(f, strhSize);
	fourcc(f, "vids");
	fourcc(f, "YUY2");
	put32(f, 0); /* dwFlags */
	put16(f, 0); /* wPriority */
	put16(f, 0); /* wLanguage */
	put32(f, 0); /* dwInitialFrames */
	put32(f, 1); /* dwScale */
	put32(f, fps);
	put32(f, 0); /* dwStart */
	put32(f, frames);
	put32(f, frameSize);
	put32(f, 0xFFFFFFFF); /* dwQuality */
	put32(f, 0);          /* dwSampleSize */
	put16(f, 0);
	put16(f, 0);
	put16(f, (UINT16)width);
	put16(f, (UINT16)height);
	fourcc(f, "strf");
	put32(f, strfSize);
	put32(f, 40);
	put32(f, width);
	put32(f, height);
	put16(f, 1);  /* biPlanes */
	put16(f, 16); /* biBitCount */
	fourcc(f, "YUY2");
	put32(f, frameSize);
	for (int x = 0; x < 4; x++)
		put32(f, 0);

	fourcc(f, "LIST");
	put32(f, moviSize);
	fourcc(f, "movi");
	for (UINT32 n = 0; n < frames; n++)
	{
		/* a bright bar moving down, on grey */
		for (UINT32 y = 0; y < height; y++)
		{
			const BYTE luma = (((y + n * 4) % height) < height / 4) ? 235 : 64;
			for (UINT32 x = 0; x < width; x += 2)
			{
				BYTE* p = &frame[2ull * (1ull * y * width + x)];
				p[0] = luma;
				p[1] = 128;
				p[2] = luma;
				p[3] = 128;
			}
		}
		fourcc(f, "00dc");
		put32(f, frameSize);
		(void)fwrite(frame, 1, frameSize, f);
	}

	fourcc(f, "idx1");
	put32(f, idx1Size);
	for (UINT32 n = 0; n < frames; n++)
	{
		fourcc(f, "00dc");
		put32(f, 0x10); /* AVIIF_KEYFRAME */
		put32(f, 4 + n * (8 + frameSize));
		put32(f, frameSize);
	}

	const BOOL rc = (ferror(f) == 0);
	(void)fclose(f);
	free(frame);
	return rc;
}
