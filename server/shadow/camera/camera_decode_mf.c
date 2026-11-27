/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: MJPG with Windows' own MJPEG decoder (Media Foundation)
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
 * The Windows host is built without FFmpeg, so a client's MJPG camera is decoded by the MJPEG
 * decoder MFT of Windows (MFTEnumEx found it on Windows Server 2022 and 2025 and decoded a
 * JPEG to NV12: the kit's probe-camera-win). mfplat.dll is loaded at run time, as the H.264
 * decoder of libfreerdp/codec/h264_mf.c does, so the shadow server starts without it.
 */

#define COBJMACROS

#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>

#include <freerdp/log.h>

#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include "camera_decode_mf.h"

#define TAG SERVER_TAG("shadow.camera")

/* the values of the Windows SDK's mfapi.h and mftransform.h */
static const GUID sMFT_CATEGORY_VIDEO_DECODER = {
	0xd6c02d4b, 0x6833, 0x45b4, { 0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91 }
};
static const GUID sIID_IMFTransform = {
	0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d }
};
static const GUID sMF_MT_MAJOR_TYPE = {
	0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f }
};
static const GUID sMF_MT_SUBTYPE = {
	0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 }
};
static const GUID sMF_MT_FRAME_SIZE = {
	0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d }
};
static const GUID sMF_MT_DEFAULT_STRIDE = {
	0x644b4e48, 0x1e02, 0x4516, { 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6 }
};
static const GUID sMFMediaType_Video = {
	0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID sMFVideoFormat_MJPG = {
	0x47504a4d, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID sMFVideoFormat_NV12 = {
	0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

typedef HRESULT(STDAPICALLTYPE* pfnMFStartup)(ULONG version, DWORD flags);
typedef HRESULT(STDAPICALLTYPE* pfnMFShutdown)(void);
typedef HRESULT(STDAPICALLTYPE* pfnMFTEnumEx)(GUID category, UINT32 flags,
                                              const MFT_REGISTER_TYPE_INFO* input,
                                              const MFT_REGISTER_TYPE_INFO* output,
                                              IMFActivate*** activates, UINT32* count);
typedef HRESULT(STDAPICALLTYPE* pfnMFCreateMediaType)(IMFMediaType** type);
typedef HRESULT(STDAPICALLTYPE* pfnMFCreateSample)(IMFSample** sample);
typedef HRESULT(STDAPICALLTYPE* pfnMFCreateMemoryBuffer)(DWORD size, IMFMediaBuffer** buffer);

typedef struct
{
	HMODULE module;
	pfnMFStartup MFStartup;
	pfnMFShutdown MFShutdown;
	pfnMFTEnumEx MFTEnumEx;
	pfnMFCreateMediaType MFCreateMediaType;
	pfnMFCreateSample MFCreateSample;
	pfnMFCreateMemoryBuffer MFCreateMemoryBuffer;
} MfPlat;

struct s_camera_mf_mjpeg
{
	MfPlat mf;
	BOOL started;
	HRESULT co;
	IMFTransform* transform;
	UINT32 width;
	UINT32 height;
	UINT32 outWidth; /* the output type's size (may be padded) */
	UINT32 outHeight;
	LONG outStride;
	BOOL providesSamples;
	DWORD outSize;
	IMFSample* output; /* ours, when the decoder does not provide its own */
};

static BOOL mf_load(MfPlat* mf)
{
	mf->module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!mf->module)
		return FALSE;
	mf->MFStartup = (pfnMFStartup)(void*)GetProcAddress(mf->module, "MFStartup");
	mf->MFShutdown = (pfnMFShutdown)(void*)GetProcAddress(mf->module, "MFShutdown");
	mf->MFTEnumEx = (pfnMFTEnumEx)(void*)GetProcAddress(mf->module, "MFTEnumEx");
	mf->MFCreateMediaType =
	    (pfnMFCreateMediaType)(void*)GetProcAddress(mf->module, "MFCreateMediaType");
	mf->MFCreateSample = (pfnMFCreateSample)(void*)GetProcAddress(mf->module, "MFCreateSample");
	mf->MFCreateMemoryBuffer =
	    (pfnMFCreateMemoryBuffer)(void*)GetProcAddress(mf->module, "MFCreateMemoryBuffer");
	return mf->MFStartup && mf->MFShutdown && mf->MFTEnumEx && mf->MFCreateMediaType &&
	       mf->MFCreateSample && mf->MFCreateMemoryBuffer;
}

static void mf_unload(MfPlat* mf)
{
	if (mf->module)
		(void)FreeLibrary(mf->module);
	memset(mf, 0, sizeof(*mf));
}

/* the first MJPEG decoder MFT, activated; nullptr when Windows has none */
static IMFTransform* mf_find_decoder(const MfPlat* mf)
{
	const MFT_REGISTER_TYPE_INFO input = { sMFMediaType_Video, sMFVideoFormat_MJPG };
	IMFActivate** activates = nullptr;
	UINT32 count = 0;
	const HRESULT hr =
	    mf->MFTEnumEx(sMFT_CATEGORY_VIDEO_DECODER,
	                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
	                  &input, nullptr, &activates, &count);
	if (FAILED(hr))
	{
		WLog_DBG(TAG, "camera: MFTEnumEx(MJPEG decoder) failed: 0x%08" PRIX32, (UINT32)hr);
		return nullptr;
	}
	IMFTransform* transform = nullptr;
	for (UINT32 i = 0; i < count; i++)
	{
		if (!transform)
			(void)IMFActivate_ActivateObject(activates[i], &sIID_IMFTransform, (void**)&transform);
		IMFActivate_Release(activates[i]);
	}
	CoTaskMemFree((void*)activates);
	return transform;
}

/* sets the output to NV12 and notes its size and stride */
static BOOL mf_set_output(CameraMfMjpeg* d)
{
	for (DWORD i = 0;; i++)
	{
		IMFMediaType* type = nullptr;
		if (FAILED(IMFTransform_GetOutputAvailableType(d->transform, 0, i, &type)))
			return FALSE;
		GUID subtype = { 0 };
		const BOOL nv12 = SUCCEEDED(IMFMediaType_GetGUID(type, &sMF_MT_SUBTYPE, &subtype)) &&
		                  IsEqualGUID(&subtype, &sMFVideoFormat_NV12);
		BOOL ok = FALSE;
		if (nv12 && SUCCEEDED(IMFTransform_SetOutputType(d->transform, 0, type, 0)))
		{
			UINT64 size = 0;
			UINT32 stride = 0;
			if (SUCCEEDED(IMFMediaType_GetUINT64(type, &sMF_MT_FRAME_SIZE, &size)))
			{
				d->outWidth = (UINT32)(size >> 32);
				d->outHeight = (UINT32)(size & 0xFFFFFFFFu);
			}
			else
			{
				d->outWidth = d->width;
				d->outHeight = d->height;
			}
			d->outStride = SUCCEEDED(IMFMediaType_GetUINT32(type, &sMF_MT_DEFAULT_STRIDE, &stride))
			                   ? (LONG)stride
			                   : (LONG)d->outWidth;
			ok = (d->outWidth >= d->width) && (d->outHeight >= d->height) &&
			     (d->outStride >= (LONG)d->width);
		}
		IMFMediaType_Release(type);
		if (ok)
			break;
		if (nv12)
			return FALSE;
	}

	MFT_OUTPUT_STREAM_INFO info = { 0 };
	if (FAILED(IMFTransform_GetOutputStreamInfo(d->transform, 0, &info)))
		return FALSE;
	d->providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
	                                      MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))
	                         ? TRUE
	                         : FALSE;
	const DWORD needed = (DWORD)d->outStride * d->outHeight * 3u / 2u;
	d->outSize = (info.cbSize > needed) ? info.cbSize : needed;
	if (d->output)
	{
		IMFSample_Release(d->output);
		d->output = nullptr;
	}
	if (!d->providesSamples)
	{
		IMFMediaBuffer* buffer = nullptr;
		if (FAILED(d->mf.MFCreateSample(&d->output)) ||
		    FAILED(d->mf.MFCreateMemoryBuffer(d->outSize, &buffer)))
		{
			if (buffer)
				IMFMediaBuffer_Release(buffer);
			return FALSE;
		}
		const HRESULT hr = IMFSample_AddBuffer(d->output, buffer);
		IMFMediaBuffer_Release(buffer);
		if (FAILED(hr))
			return FALSE;
	}
	return TRUE;
}

BOOL camera_mf_mjpeg_available(void)
{
	MfPlat mf = { 0 };
	BOOL found = FALSE;
	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (mf_load(&mf) && SUCCEEDED(mf.MFStartup(MF_VERSION, MFSTARTUP_LITE)))
	{
		IMFTransform* transform = mf_find_decoder(&mf);
		found = (transform != nullptr);
		if (transform)
			IMFTransform_Release(transform);
		(void)mf.MFShutdown();
	}
	mf_unload(&mf);
	if (SUCCEEDED(co))
		CoUninitialize();
	return found;
}

CameraMfMjpeg* camera_mf_mjpeg_new(UINT32 width, UINT32 height)
{
	if ((width == 0) || (height == 0) || (width % 2) || (height % 2))
		return nullptr;
	CameraMfMjpeg* d = calloc(1, sizeof(CameraMfMjpeg));
	if (!d)
		return nullptr;
	d->width = width;
	d->height = height;
	d->co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (!mf_load(&d->mf) || FAILED(d->mf.MFStartup(MF_VERSION, MFSTARTUP_LITE)))
		goto fail;
	d->started = TRUE;
	d->transform = mf_find_decoder(&d->mf);
	if (!d->transform)
		goto fail;

	IMFMediaType* input = nullptr;
	HRESULT hr = d->mf.MFCreateMediaType(&input);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(input, &sMF_MT_MAJOR_TYPE, &sMFMediaType_Video);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(input, &sMF_MT_SUBTYPE, &sMFVideoFormat_MJPG);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT64(input, &sMF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
	if (SUCCEEDED(hr))
		hr = IMFTransform_SetInputType(d->transform, 0, input, 0);
	if (input)
		IMFMediaType_Release(input);
	if (FAILED(hr) || !mf_set_output(d))
	{
		WLog_ERR(TAG,
		         "camera: the MJPEG decoder MFT takes no MJPG %" PRIu32 "x%" PRIu32
		         " to NV12 (0x%08" PRIX32 ")",
		         width, height, (UINT32)hr);
		goto fail;
	}
	(void)IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	(void)IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	return d;

fail:
	camera_mf_mjpeg_free(d);
	return nullptr;
}

void camera_mf_mjpeg_free(CameraMfMjpeg* d)
{
	if (!d)
		return;
	if (d->output)
		IMFSample_Release(d->output);
	if (d->transform)
		IMFTransform_Release(d->transform);
	if (d->started)
		(void)d->mf.MFShutdown();
	mf_unload(&d->mf);
	if (SUCCEEDED(d->co))
		CoUninitialize();
	free(d);
}

/* copies the decoder's NV12 (at its stride, UV after outHeight rows) into a tight frame */
static BOOL mf_copy_out(const CameraMfMjpeg* d, IMFSample* sample, BYTE* nv12)
{
	IMFMediaBuffer* buffer = nullptr;
	if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
		return FALSE;
	BYTE* data = nullptr;
	DWORD length = 0;
	BOOL ok = FALSE;
	if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, nullptr, &length)))
	{
		const size_t stride = (size_t)d->outStride;
		const size_t uvOffset = stride * d->outHeight;
		if (length >= uvOffset + stride * (d->height / 2u))
		{
			for (UINT32 y = 0; y < d->height; y++)
				memcpy(nv12 + (size_t)y * d->width, data + (size_t)y * stride, d->width);
			BYTE* uv = nv12 + (size_t)d->width * d->height;
			for (UINT32 y = 0; y < d->height / 2u; y++)
				memcpy(uv + (size_t)y * d->width, data + uvOffset + (size_t)y * stride, d->width);
			ok = TRUE;
		}
		(void)IMFMediaBuffer_Unlock(buffer);
	}
	IMFMediaBuffer_Release(buffer);
	return ok;
}

BOOL camera_mf_mjpeg_decode(CameraMfMjpeg* d, const BYTE* jpeg, size_t size, BYTE* nv12)
{
	WINPR_ASSERT(d);
	if (!jpeg || !nv12 || (size == 0) || (size > UINT32_MAX))
		return FALSE;

	IMFSample* input = nullptr;
	IMFMediaBuffer* buffer = nullptr;
	HRESULT hr = d->mf.MFCreateSample(&input);
	if (SUCCEEDED(hr))
		hr = d->mf.MFCreateMemoryBuffer((DWORD)size, &buffer);
	if (SUCCEEDED(hr))
	{
		BYTE* data = nullptr;
		hr = IMFMediaBuffer_Lock(buffer, &data, nullptr, nullptr);
		if (SUCCEEDED(hr))
		{
			memcpy(data, jpeg, size);
			(void)IMFMediaBuffer_Unlock(buffer);
			hr = IMFMediaBuffer_SetCurrentLength(buffer, (DWORD)size);
		}
	}
	if (SUCCEEDED(hr))
		hr = IMFSample_AddBuffer(input, buffer);
	if (buffer)
		IMFMediaBuffer_Release(buffer);
	if (SUCCEEDED(hr))
		hr = IMFTransform_ProcessInput(d->transform, 0, input, 0);
	if (input)
		IMFSample_Release(input);
	if (FAILED(hr))
	{
		WLog_DBG(TAG, "camera: MJPEG ProcessInput failed: 0x%08" PRIX32, (UINT32)hr);
		return FALSE;
	}

	for (int round = 0; round < 3; round++)
	{
		MFT_OUTPUT_DATA_BUFFER out = { 0 };
		DWORD status = 0;
		out.pSample = d->providesSamples ? nullptr : d->output;
		if (!d->providesSamples)
		{
			IMFMediaBuffer* outBuffer = nullptr;
			if (SUCCEEDED(IMFSample_GetBufferByIndex(d->output, 0, &outBuffer)))
			{
				(void)IMFMediaBuffer_SetCurrentLength(outBuffer, 0);
				IMFMediaBuffer_Release(outBuffer);
			}
		}
		hr = IMFTransform_ProcessOutput(d->transform, 0, 1, &out, &status);
		if (out.pEvents)
			IMFCollection_Release(out.pEvents);
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			if (d->providesSamples && out.pSample)
				IMFSample_Release(out.pSample);
			if (!mf_set_output(d))
				return FALSE;
			continue;
		}
		if (FAILED(hr))
		{
			if (d->providesSamples && out.pSample)
				IMFSample_Release(out.pSample);
			if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
				WLog_DBG(TAG, "camera: MJPEG ProcessOutput failed: 0x%08" PRIX32, (UINT32)hr);
			return FALSE;
		}
		const BOOL ok = out.pSample && mf_copy_out(d, out.pSample, nv12);
		if (d->providesSamples && out.pSample)
			IMFSample_Release(out.pSample);
		return ok;
	}
	return FALSE;
}
