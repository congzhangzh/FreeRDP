/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the media source read in process from the frame ring
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

/*
 *   TestShadowCameraWin TestShadowCameraWinSource [<freerdp-shadow-camera-source.dll>]
 *
 * The writer's side of the ring (a Local\ ring of its own, FREERDP_SHADOW_CAMERA_RING), the
 * media source DLL loaded directly (no registration, no Frame Server) and read by a source
 * reader: the media type is the ring's, the frames are the ring's newest bit for bit, a new
 * frame shows up, and the source counts as a reader while it streams and not after. Runs on
 * Windows and under Wine; the virtual camera itself needs Frame Server (the kit's CI).
 */

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "../camera_mf_ids.h"
#include "../camera_ring.h"

#define TEST_WIDTH 320u
#define TEST_HEIGHT 240u
#define TEST_FPS 30u
#define TEST_FRAME (TEST_WIDTH * TEST_HEIGHT * 3u / 2u)

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID clsid, REFIID riid, void** ppv);

static void draw(BYTE* frame, UINT32 n)
{
	for (UINT32 y = 0; y < TEST_HEIGHT; y++)
		for (UINT32 x = 0; x < TEST_WIDTH; x++)
			frame[y * TEST_WIDTH + x] = (BYTE)((x + 2u * y + 7u * n) & 0xFF);
	BYTE* uv = frame + TEST_WIDTH * TEST_HEIGHT;
	for (UINT32 i = 0; i < TEST_WIDTH * TEST_HEIGHT / 2u; i++)
		uv[i] = (BYTE)((i % 2) ? (40u + n) : (200u - n));
}

/* one sample, copied tightly packed into frame; FALSE on an error or no sample */
static BOOL read_frame(IMFSourceReader* reader, BYTE* frame)
{
	for (int attempt = 0; attempt < 20; attempt++)
	{
		DWORD index = 0;
		DWORD flags = 0;
		LONGLONG time = 0;
		IMFSample* sample = NULL;
		HRESULT hr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		                                        0, &index, &flags, &time, &sample);
		if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
		{
			fprintf(stderr, "ReadSample hr=0x%08lx flags=0x%lx\n", (unsigned long)hr,
			        (unsigned long)flags);
			if (sample)
				IMFSample_Release(sample);
			return FALSE;
		}
		if (!sample)
			continue;
		IMFMediaBuffer* buffer = NULL;
		BOOL ok = FALSE;
		if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
		{
			BYTE* data = NULL;
			DWORD length = 0;
			if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length)))
			{
				ok = (length >= TEST_FRAME);
				if (ok)
					memcpy(frame, data, TEST_FRAME);
				else
					fprintf(stderr, "sample of %lu bytes\n", (unsigned long)length);
				(void)IMFMediaBuffer_Unlock(buffer);
			}
			IMFMediaBuffer_Release(buffer);
		}
		IMFSample_Release(sample);
		return ok;
	}
	return FALSE;
}

static IMFMediaSource* load_source(const WCHAR* path, HMODULE* module)
{
	*module = LoadLibraryW(path);
	if (!*module)
	{
		fprintf(stderr, "LoadLibrary(%ls) failed: %lu\n", path, GetLastError());
		return NULL;
	}
	GetClassObjectFn getClassObject =
	    (GetClassObjectFn)(void*)GetProcAddress(*module, "DllGetClassObject");
	IClassFactory* factory = NULL;
	IMFActivate* activate = NULL;
	IMFMediaSource* source = NULL;
	HRESULT hr = getClassObject
	                 ? getClassObject(&CameraMF_CLSID_Source, &IID_IClassFactory, (void**)&factory)
	                 : E_NOINTERFACE;
	if (SUCCEEDED(hr))
		hr = IClassFactory_CreateInstance(factory, NULL, &IID_IMFActivate, (void**)&activate);
	if (SUCCEEDED(hr))
		hr = IMFActivate_ActivateObject(activate, &IID_IMFMediaSource, (void**)&source);
	if (FAILED(hr))
		fprintf(stderr, "no media source: hr=0x%08lx\n", (unsigned long)hr);
	if (activate)
		IMFActivate_Release(activate);
	if (factory)
		IClassFactory_Release(factory);
	return source;
}

int TestShadowCameraWinSource(int argc, char* argv[])
{
	int rc = -1;
	WCHAR path[MAX_PATH] = { 0 };
	if (argc > 1)
		(void)MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
	else
	{
		/* beside the test program */
		const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
		WCHAR* slash = ((length > 0) && (length < MAX_PATH)) ? wcsrchr(path, L'\\') : NULL;
		if (!slash || (_snwprintf_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), _TRUNCATE,
		                            L"%ls", CAMERA_MF_SOURCE_DLL) < 0))
			return -1;
	}

	char prefix[64] = { 0 };
	(void)_snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "Local\\TestShadowCamera-%lu",
	                  GetCurrentProcessId());
	(void)SetEnvironmentVariableA(SHADOW_CAMERA_RING_NAME_ENV, prefix);

	BYTE* expected = calloc(1, TEST_FRAME);
	BYTE* got = calloc(1, TEST_FRAME);
	ShadowCameraRing* ring = NULL;
	HMODULE module = NULL;
	IMFMediaSource* source = NULL;
	IMFSourceReader* reader = NULL;
	IMFAttributes* options = NULL;
	IMFMediaType* type = NULL;
	BOOL mfStarted = FALSE;
	const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (!expected || !got)
		goto out;

	DWORD error = 0;
	ring = shadow_camera_ring_create(TEST_WIDTH, TEST_HEIGHT, TEST_FPS, &error);
	if (!ring)
	{
		fprintf(stderr, "shadow_camera_ring_create failed: %lu\n", error);
		goto out;
	}
	draw(expected, 1);
	shadow_camera_ring_write(ring, expected);

	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
		goto out;
	mfStarted = TRUE;

	source = load_source(path, &module);
	if (!source)
		goto out;
	if (FAILED(MFCreateAttributes(&options, 1)) ||
	    FAILED(IMFAttributes_SetUINT32(options, &MF_READWRITE_DISABLE_CONVERTERS, TRUE)) ||
	    FAILED(MFCreateSourceReaderFromMediaSource(source, options, &reader)))
	{
		fprintf(stderr, "no source reader\n");
		goto out;
	}

	/* the ring's type */
	GUID subtype = { 0 };
	UINT64 size = 0;
	if (FAILED(IMFSourceReader_GetCurrentMediaType(
	        reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
	    FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)) ||
	    FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size)) ||
	    !IsEqualGUID(&subtype, &MFVideoFormat_NV12) || ((UINT32)(size >> 32) != TEST_WIDTH) ||
	    ((UINT32)size != TEST_HEIGHT))
	{
		fprintf(stderr, "media type: not NV12 %ux%u (%ux%u)\n", TEST_WIDTH, TEST_HEIGHT,
		        (unsigned)(size >> 32), (unsigned)size);
		goto out;
	}

	/* the newest frame, bit for bit, again and again */
	for (int i = 0; i < 3; i++)
	{
		if (!read_frame(reader, got) || (memcmp(got, expected, TEST_FRAME) != 0))
		{
			fprintf(stderr, "sample %d is not the ring's frame 1\n", i);
			goto out;
		}
	}
	if (shadow_camera_ring_readers(ring) != 1)
	{
		fprintf(stderr, "readers while streaming: %ld, not 1\n", shadow_camera_ring_readers(ring));
		goto out;
	}

	/* a new frame shows up */
	draw(expected, 2);
	shadow_camera_ring_write(ring, expected);
	BOOL seen = FALSE;
	for (int i = 0; (i < 10) && !seen; i++)
	{
		if (!read_frame(reader, got))
			goto out;
		seen = (memcmp(got, expected, TEST_FRAME) == 0);
	}
	if (!seen)
	{
		fprintf(stderr, "the ring's frame 2 never came\n");
		goto out;
	}

	/* the reader goes: the source shuts down and stops counting */
	IMFSourceReader_Release(reader);
	reader = NULL;
	(void)IMFMediaSource_Shutdown(source);
	if (shadow_camera_ring_readers(ring) != 0)
	{
		fprintf(stderr, "readers after shutdown: %ld, not 0\n", shadow_camera_ring_readers(ring));
		goto out;
	}
	printf("TestShadowCameraWinSource: NV12 %ux%u from the ring, frames exact, readers 1 then 0\n",
	       TEST_WIDTH, TEST_HEIGHT);
	rc = 0;

out:
	if (type)
		IMFMediaType_Release(type);
	if (reader)
		IMFSourceReader_Release(reader);
	if (options)
		IMFAttributes_Release(options);
	if (source)
	{
		(void)IMFMediaSource_Shutdown(source);
		IMFMediaSource_Release(source);
	}
	if (mfStarted)
		(void)MFShutdown();
	/* the DLL stays loaded: its stream thread may still be ending */
	shadow_camera_ring_close(ring);
	if (SUCCEEDED(co))
		CoUninitialize();
	free(expected);
	free(got);
	return rc;
}
