/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * AV1 encoding with a Media Foundation hardware encoder
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

/* before any header that pulls in the COM interfaces */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <stdlib.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/library.h>
#include <winpr/sysinfo.h>
#include <winpr/wlog.h>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include "av1_mf.h"

/* the GUIDs, spelled out (like h264_mf.c) so no Media Foundation import library is needed:
 * mfplat.dll is loaded at run time, Windows N editions have none */
static const GUID sMFT_CATEGORY_VIDEO_ENCODER = {
	0xf79eac7d, 0xe545, 0x4387, { 0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a }
};
static const GUID sMFMediaType_Video = {
	0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID sMFVideoFormat_AV1 = { /* 'AV01' */
	                                     0x31305641,
	                                     0x0000,
	                                     0x0010,
	                                     { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID sMFVideoFormat_NV12 = {
	0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
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
static const GUID sMF_MT_FRAME_RATE = {
	0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 }
};
static const GUID sMF_MT_PIXEL_ASPECT_RATIO = {
	0xc6376a1e, 0x8d0a, 0x4027, { 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6 }
};
static const GUID sMF_MT_INTERLACE_MODE = {
	0xe2724bb8, 0xe676, 0x4806, { 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd }
};
static const GUID sMF_MT_AVG_BITRATE = {
	0x20332624, 0xfb0d, 0x4d9e, { 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e }
};
static const GUID sMF_MT_DEFAULT_STRIDE = {
	0x644b4e48, 0x1e02, 0x4516, { 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6 }
};
static const GUID sMF_MT_MPEG_SEQUENCE_HEADER = {
	0x3c036de7, 0x3ad0, 0x4c9e, { 0x92, 0x16, 0xee, 0x6d, 0x6a, 0xc2, 0x1c, 0xb3 }
};
static const GUID sMF_TRANSFORM_ASYNC = {
	0xf81a699a, 0x649a, 0x497d, { 0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a }
};
static const GUID sMF_TRANSFORM_ASYNC_UNLOCK = {
	0xe5666d6b, 0x3422, 0x4eb6, { 0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07 }
};
static const GUID sMFT_FRIENDLY_NAME_Attribute = {
	0x314ffbae, 0x5b41, 0x4c95, { 0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3 }
};
static const GUID sMFSampleExtension_CleanPoint = {
	0x9cdf01d8, 0xa0f0, 0x43ba, { 0xb0, 0x77, 0xea, 0xa0, 0x6c, 0xbd, 0x72, 0x8a }
};
static const GUID sIID_IMFTransform = {
	0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d }
};
static const GUID sIID_IMFMediaEventGenerator = {
	0x2cd0bd52, 0xbcd5, 0x4b89, { 0xb6, 0x2c, 0xea, 0xdc, 0x0c, 0x03, 0x1e, 0x7d }
};
static const GUID sIID_ICodecAPI = {
	0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda }
};
/* CODECAPI_AVLowLatencyMode, also MF_LOW_LATENCY */
static const GUID sCODECAPI_AVLowLatencyMode = {
	0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee }
};
static const GUID sCODECAPI_AVEncCommonRateControlMode = {
	0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 }
};
static const GUID sCODECAPI_AVEncCommonMeanBitRate = {
	0xf7222374, 0x2144, 0x4815, { 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 }
};
static const GUID sCODECAPI_AVEncMPVGOPSize = {
	0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 }
};
static const GUID sCODECAPI_AVEncVideoForceKeyFrame = {
	0x398c1b98, 0x8353, 0x475a, { 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45 }
};

/* values from codecapi.h and mfobjects.h */
#define AV1_MF_RC_CBR 0           /* eAVEncCommonRateControlMode_CBR */
#define AV1_MF_RC_LOW_DELAY_VBR 4 /* eAVEncCommonRateControlMode_LowDelayVBR */
#define AV1_MF_INTERLACE_PROGRESSIVE 2
#define AV1_MF_GOP 100000      /* key frames when asked for, not by count */
#define AV1_MF_TIMEOUT_MS 1000 /* the most a frame may take */
#define AV1_MF_FRAME_RATE 30   /* nominal: the samples carry the real durations */

typedef HRESULT(__stdcall* pfnMFStartup)(ULONG Version, DWORD dwFlags);
typedef HRESULT(__stdcall* pfnMFShutdown)(void);
typedef HRESULT(__stdcall* pfnMFCreateSample)(IMFSample** ppIMFSample);
typedef HRESULT(__stdcall* pfnMFCreateMemoryBuffer)(DWORD cbMaxLength, IMFMediaBuffer** ppBuffer);
typedef HRESULT(__stdcall* pfnMFCreateMediaType)(IMFMediaType** ppMFType);
typedef HRESULT(__stdcall* pfnMFTEnumEx)(GUID guidCategory, UINT32 Flags,
                                         const MFT_REGISTER_TYPE_INFO* pInputType,
                                         const MFT_REGISTER_TYPE_INFO* pOutputType,
                                         IMFActivate*** pppMFTActivate, UINT32* pnumMFTActivate);

struct S_AV1_MF
{
	wLog* log;
	char name[256];
	INT32 id; /* the device's id in av1_mf_list() */
	UINT32 width;
	UINT32 height;

	HMODULE mfplat;
	BOOL started;
	pfnMFStartup MFStartup;
	pfnMFShutdown MFShutdown;
	pfnMFCreateSample MFCreateSample;
	pfnMFCreateMemoryBuffer MFCreateMemoryBuffer;
	pfnMFCreateMediaType MFCreateMediaType;
	pfnMFTEnumEx MFTEnumEx;

	IMFActivate* activate;
	IMFTransform* mft;
	IMFMediaEventGenerator* events;
	ICodecAPI* codecApi;
	BOOL providesSamples;
	DWORD outputSize;
	BOOL streaming;
	LONGLONG time; /* the next sample's time, 100 ns */

	/* The encoder gives a frame its bits from the nominal frame rate, not from the sample's
	 * duration: the mean bit rate it is told is scaled by the real frame rate, so a frame gets
	 * the target bit rate x its duration (frames come on damage, at any rate). */
	UINT32 kbps;        /* the target */
	UINT32 meanBitRate; /* what the encoder was told, bit/s */
	double frameMs;     /* the mean frame duration, moving */
	BOOL rateFollows;   /* FALSE once the encoder refused a new mean bit rate */

	UINT32 needInput;  /* METransformNeedInput not yet answered */
	UINT32 haveOutput; /* METransformHaveOutput not yet collected */

	BYTE* seqHdr; /* the sequence header OBU, from the output type */
	UINT32 seqHdrSize;
	BYTE* out;
	size_t outSize;
	size_t outCapacity;
};

#define RELEASE(x)                   \
	do                               \
	{                                \
		if (x)                       \
			(x)->lpVtbl->Release(x); \
		(x) = nullptr;               \
	} while (0)

void av1_mf_free(AV1_MF* enc)
{
	if (!enc)
		return;
	if (enc->mft && enc->streaming)
	{
		(void)IMFTransform_ProcessMessage(enc->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		(void)IMFTransform_ProcessMessage(enc->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
	}
	RELEASE(enc->codecApi);
	RELEASE(enc->events);
	RELEASE(enc->mft);
	if (enc->activate)
	{
		(void)IMFActivate_ShutdownObject(enc->activate);
		RELEASE(enc->activate);
	}
	if (enc->started && enc->MFShutdown)
		(void)enc->MFShutdown();
	if (enc->mfplat)
		FreeLibrary(enc->mfplat);
	if (enc->seqHdr)
		CoTaskMemFree(enc->seqHdr);
	free(enc->out);
	free(enc);
}

const char* av1_mf_name(const AV1_MF* enc)
{
	return enc ? enc->name : "";
}

static BOOL load(AV1_MF* enc)
{
	enc->mfplat = LoadLibraryA("mfplat.dll");
	if (!enc->mfplat)
	{
		WLog_Print(enc->log, WLOG_DEBUG, "Media Foundation: no mfplat.dll");
		return FALSE;
	}
	enc->MFStartup = GetProcAddressAs(enc->mfplat, "MFStartup", pfnMFStartup);
	enc->MFShutdown = GetProcAddressAs(enc->mfplat, "MFShutdown", pfnMFShutdown);
	enc->MFCreateSample = GetProcAddressAs(enc->mfplat, "MFCreateSample", pfnMFCreateSample);
	enc->MFCreateMemoryBuffer =
	    GetProcAddressAs(enc->mfplat, "MFCreateMemoryBuffer", pfnMFCreateMemoryBuffer);
	enc->MFCreateMediaType =
	    GetProcAddressAs(enc->mfplat, "MFCreateMediaType", pfnMFCreateMediaType);
	enc->MFTEnumEx = GetProcAddressAs(enc->mfplat, "MFTEnumEx", pfnMFTEnumEx);
	if (!enc->MFStartup || !enc->MFShutdown || !enc->MFCreateSample || !enc->MFCreateMemoryBuffer ||
	    !enc->MFCreateMediaType || !enc->MFTEnumEx)
		return FALSE;
	if (FAILED(enc->MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
		return FALSE;
	enc->started = TRUE;
	return TRUE;
}

/* the first hardware encoder MFT with AV1 output */
/* the friendly name of an encoder MFT, UTF-8 */
static void encoder_name(IMFActivate* act, char* buf, size_t size)
{
	WCHAR* name = nullptr;
	UINT32 len = 0;
	buf[0] = '\0';
	if (SUCCEEDED(IMFActivate_GetAllocatedString(act, &sMFT_FRIENDLY_NAME_Attribute, &name, &len)))
	{
		if (WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, (int)size, nullptr, nullptr) <= 0)
			buf[0] = '\0';
		CoTaskMemFree(name);
	}
	if (!buf[0])
		(void)_snprintf(buf, size, "hardware AV1 encoder MFT");
}

/* the device @p device (an id of av1_mf_list(), < 0: the first) */
static BOOL find_encoder(AV1_MF* enc, INT32 device)
{
	MFT_REGISTER_TYPE_INFO outInfo = { sMFMediaType_Video, sMFVideoFormat_AV1 };
	IMFActivate** acts = nullptr;
	UINT32 count = 0;
	const HRESULT hr = enc->MFTEnumEx(sMFT_CATEGORY_VIDEO_ENCODER,
	                                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
	                                  &outInfo, &acts, &count);
	if (FAILED(hr) || (count == 0))
	{
		WLog_Print(enc->log, WLOG_DEBUG,
		           "Media Foundation: no hardware AV1 encoder (0x%08" PRIX32 ")", (UINT32)hr);
		CoTaskMemFree(acts);
		return FALSE;
	}
	const UINT32 pick = (device < 0) ? 0 : (UINT32)device;
	for (UINT32 i = 0; i < count; i++)
	{
		if (i == pick)
			enc->activate = acts[i];
		else
			IMFActivate_Release(acts[i]);
	}
	CoTaskMemFree(acts);
	if (!enc->activate)
	{
		WLog_Print(enc->log, WLOG_DEBUG, "Media Foundation: no hardware AV1 encoder %" PRId32,
		           device);
		return FALSE;
	}
	enc->id = (INT32)pick;

	encoder_name(enc->activate, enc->name, sizeof(enc->name));
	return TRUE;
}

static HRESULT set_ui4(AV1_MF* enc, const GUID* key, UINT32 value, const char* what)
{
	if (!enc->codecApi)
		return E_NOINTERFACE;
	VARIANT var;
	VariantInit(&var);
	V_VT(&var) = VT_UI4;
	V_UI4(&var) = value;
	const HRESULT hr = ICodecAPI_SetValue(enc->codecApi, key, &var);
	if (FAILED(hr))
		WLog_Print(enc->log, WLOG_DEBUG, "Media Foundation: %s %" PRIu32 ": 0x%08" PRIX32, what,
		           value, (UINT32)hr);
	return hr;
}

static BOOL configure(AV1_MF* enc, UINT32 kbps, BOOL cbr)
{
	HRESULT hr = IMFActivate_ActivateObject(enc->activate, &sIID_IMFTransform, (void**)&enc->mft);
	if (FAILED(hr))
	{
		WLog_Print(enc->log, WLOG_WARN, "%s: ActivateObject 0x%08" PRIX32, enc->name, (UINT32)hr);
		return FALSE;
	}

	IMFAttributes* attr = nullptr;
	UINT32 async = 0;
	if (SUCCEEDED(IMFTransform_GetAttributes(enc->mft, &attr)) && attr)
	{
		(void)IMFAttributes_GetUINT32(attr, &sMF_TRANSFORM_ASYNC, &async);
		(void)IMFAttributes_SetUINT32(attr, &sMF_TRANSFORM_ASYNC_UNLOCK, TRUE);
		(void)IMFAttributes_SetUINT32(attr, &sCODECAPI_AVLowLatencyMode, TRUE);
		RELEASE(attr);
	}
	/* hardware encoders are asynchronous: they say when they need input and have output */
	if (!async || FAILED(IMFTransform_QueryInterface(enc->mft, &sIID_IMFMediaEventGenerator,
	                                                 (void**)&enc->events)))
	{
		WLog_Print(enc->log, WLOG_WARN, "%s is no asynchronous encoder", enc->name);
		return FALSE;
	}

	/* the output type first, then the input */
	IMFMediaType* type = nullptr;
	if (FAILED(enc->MFCreateMediaType(&type)))
		return FALSE;
	const UINT64 size = ((UINT64)enc->width << 32) | enc->height;
	const UINT64 rate = ((UINT64)AV1_MF_FRAME_RATE << 32) | 1;
	(void)IMFMediaType_SetGUID(type, &sMF_MT_MAJOR_TYPE, &sMFMediaType_Video);
	(void)IMFMediaType_SetGUID(type, &sMF_MT_SUBTYPE, &sMFVideoFormat_AV1);
	(void)IMFMediaType_SetUINT32(type, &sMF_MT_AVG_BITRATE, kbps * 1000u);
	(void)IMFMediaType_SetUINT64(type, &sMF_MT_FRAME_SIZE, size);
	(void)IMFMediaType_SetUINT64(type, &sMF_MT_FRAME_RATE, rate);
	(void)IMFMediaType_SetUINT64(type, &sMF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
	(void)IMFMediaType_SetUINT32(type, &sMF_MT_INTERLACE_MODE, AV1_MF_INTERLACE_PROGRESSIVE);
	hr = IMFTransform_SetOutputType(enc->mft, 0, type, 0);
	RELEASE(type);
	if (FAILED(hr))
	{
		WLog_Print(enc->log, WLOG_WARN,
		           "%s refuses AV1 %" PRIu32 "x%" PRIu32 " at %" PRIu32 " kbit/s: 0x%08" PRIX32,
		           enc->name, enc->width, enc->height, kbps, (UINT32)hr);
		return FALSE;
	}

	BOOL nv12 = FALSE;
	for (DWORD i = 0; !nv12; i++)
	{
		if (FAILED(IMFTransform_GetInputAvailableType(enc->mft, 0, i, &type)))
			break;
		GUID sub = WINPR_C_ARRAY_INIT;
		nv12 = SUCCEEDED(IMFMediaType_GetGUID(type, &sMF_MT_SUBTYPE, &sub)) &&
		       IsEqualGUID(&sub, &sMFVideoFormat_NV12);
		if (!nv12)
			RELEASE(type);
	}
	if (!nv12)
	{
		WLog_Print(enc->log, WLOG_WARN, "%s takes no NV12", enc->name);
		return FALSE;
	}
	(void)IMFMediaType_SetUINT64(type, &sMF_MT_FRAME_SIZE, size);
	(void)IMFMediaType_SetUINT64(type, &sMF_MT_FRAME_RATE, rate);
	(void)IMFMediaType_SetUINT32(type, &sMF_MT_DEFAULT_STRIDE, enc->width);
	(void)IMFMediaType_SetUINT32(type, &sMF_MT_INTERLACE_MODE, AV1_MF_INTERLACE_PROGRESSIVE);
	hr = IMFTransform_SetInputType(enc->mft, 0, type, 0);
	RELEASE(type);
	if (FAILED(hr))
	{
		WLog_Print(enc->log, WLOG_WARN, "%s: SetInputType NV12: 0x%08" PRIX32, enc->name,
		           (UINT32)hr);
		return FALSE;
	}

	if (SUCCEEDED(IMFTransform_QueryInterface(enc->mft, &sIID_ICodecAPI, (void**)&enc->codecApi)))
	{
		(void)set_ui4(enc, &sCODECAPI_AVLowLatencyMode, 1, "AVLowLatencyMode");
		(void)set_ui4(enc, &sCODECAPI_AVEncCommonRateControlMode,
		              cbr ? AV1_MF_RC_CBR : AV1_MF_RC_LOW_DELAY_VBR, "AVEncCommonRateControlMode");
		(void)set_ui4(enc, &sCODECAPI_AVEncCommonMeanBitRate, kbps * 1000u,
		              "AVEncCommonMeanBitRate");
		(void)set_ui4(enc, &sCODECAPI_AVEncMPVGOPSize, AV1_MF_GOP, "AVEncMPVGOPSize");
	}

	MFT_OUTPUT_STREAM_INFO info = WINPR_C_ARRAY_INIT;
	if (FAILED(IMFTransform_GetOutputStreamInfo(enc->mft, 0, &info)))
		return FALSE;
	enc->providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
	                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
	enc->outputSize = info.cbSize ? info.cbSize : enc->width * enc->height * 3 / 2;

	if (FAILED(IMFTransform_ProcessMessage(enc->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
	    FAILED(IMFTransform_ProcessMessage(enc->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
		return FALSE;
	enc->streaming = TRUE;
	return TRUE;
}

size_t av1_mf_list(char names[][256], size_t max)
{
	AV1_MF probe = WINPR_C_ARRAY_INIT;
	size_t n = 0;
	if (load(&probe))
	{
		MFT_REGISTER_TYPE_INFO outInfo = { sMFMediaType_Video, sMFVideoFormat_AV1 };
		IMFActivate** acts = nullptr;
		UINT32 count = 0;
		if (SUCCEEDED(probe.MFTEnumEx(sMFT_CATEGORY_VIDEO_ENCODER,
		                              MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
		                              &outInfo, &acts, &count)))
		{
			for (UINT32 i = 0; i < count; i++)
			{
				if (n < max)
					encoder_name(acts[i], names[n++], 256);
				IMFActivate_Release(acts[i]);
			}
			CoTaskMemFree(acts);
		}
	}
	if (probe.started)
		(void)probe.MFShutdown();
	if (probe.mfplat)
		FreeLibrary(probe.mfplat);
	return n;
}

INT32 av1_mf_device(const AV1_MF* enc)
{
	return enc ? enc->id : -1;
}

AV1_MF* av1_mf_new(wLog* log, UINT32 width, UINT32 height, UINT32 kbps, BOOL cbr, INT32 device)
{
	if ((width == 0) || (height == 0) || (width % 2) || (height % 2) || (kbps == 0))
		return nullptr;
	AV1_MF* enc = calloc(1, sizeof(AV1_MF));
	if (!enc)
		return nullptr;
	enc->log = log;
	enc->width = width;
	enc->height = height;
	enc->kbps = kbps;
	enc->meanBitRate = kbps * 1000u;
	enc->frameMs = 1000.0 / AV1_MF_FRAME_RATE;
	enc->rateFollows = TRUE;
	if (!load(enc) || !find_encoder(enc, device) || !configure(enc, kbps, cbr))
	{
		av1_mf_free(enc);
		return nullptr;
	}
	return enc;
}

/* wait for the next event of the encoder: counts NeedInput and HaveOutput */
static BOOL next_event(AV1_MF* enc, UINT64 deadline)
{
	for (;;)
	{
		IMFMediaEvent* ev = nullptr;
		const HRESULT hr = IMFMediaEventGenerator_GetEvent(enc->events, MF_EVENT_FLAG_NO_WAIT, &ev);
		if (hr == MF_E_NO_EVENTS_AVAILABLE)
		{
			if (GetTickCount64() > deadline)
			{
				WLog_Print(enc->log, WLOG_WARN, "%s: no answer in %d ms", enc->name,
				           AV1_MF_TIMEOUT_MS);
				return FALSE;
			}
			Sleep(1);
			continue;
		}
		if (FAILED(hr))
		{
			WLog_Print(enc->log, WLOG_WARN, "%s: GetEvent 0x%08" PRIX32, enc->name, (UINT32)hr);
			return FALSE;
		}
		MediaEventType type = MEUnknown;
		HRESULT status = S_OK;
		(void)IMFMediaEvent_GetType(ev, &type);
		(void)IMFMediaEvent_GetStatus(ev, &status);
		RELEASE(ev);
		if (FAILED(status))
		{
			WLog_Print(enc->log, WLOG_WARN, "%s: event %" PRIu32 " failed: 0x%08" PRIX32, enc->name,
			           (UINT32)type, (UINT32)status);
			return FALSE;
		}
		if (type == METransformNeedInput)
			enc->needInput++;
		else if (type == METransformHaveOutput)
			enc->haveOutput++;
		return TRUE;
	}
}

static BOOL feed(AV1_MF* enc, const BYTE* const planes[3], const UINT32 strides[3],
                 UINT32 durationMs)
{
	const UINT32 w = enc->width;
	const UINT32 h = enc->height;
	const DWORD size = w * h * 3 / 2;
	IMFMediaBuffer* buf = nullptr;
	IMFSample* sample = nullptr;
	BYTE* p = nullptr;
	BOOL rc = FALSE;

	if (FAILED(enc->MFCreateMemoryBuffer(size, &buf)) ||
	    FAILED(IMFMediaBuffer_Lock(buf, &p, nullptr, nullptr)))
		goto fail;
	for (UINT32 y = 0; y < h; y++)
		memcpy(&p[1ull * y * w], &planes[0][1ull * y * strides[0]], w);
	BYTE* uv = &p[1ull * w * h];
	for (UINT32 y = 0; y < h / 2; y++)
	{
		const BYTE* u = &planes[1][1ull * y * strides[1]];
		const BYTE* v = &planes[2][1ull * y * strides[2]];
		BYTE* d = &uv[1ull * y * w];
		for (UINT32 x = 0; x < w / 2; x++)
		{
			d[2 * x] = u[x];
			d[2 * x + 1] = v[x];
		}
	}
	(void)IMFMediaBuffer_Unlock(buf);
	(void)IMFMediaBuffer_SetCurrentLength(buf, size);
	if (FAILED(enc->MFCreateSample(&sample)) || FAILED(IMFSample_AddBuffer(sample, buf)))
		goto fail;
	const LONGLONG duration = (LONGLONG)durationMs * 10000;
	(void)IMFSample_SetSampleTime(sample, enc->time);
	(void)IMFSample_SetSampleDuration(sample, duration);
	enc->time += duration;

	const HRESULT hr = IMFTransform_ProcessInput(enc->mft, 0, sample, 0);
	if (FAILED(hr))
	{
		WLog_Print(enc->log, WLOG_WARN, "%s: ProcessInput 0x%08" PRIX32, enc->name, (UINT32)hr);
		goto fail;
	}
	enc->needInput--;
	rc = TRUE;
fail:
	RELEASE(sample);
	RELEASE(buf);
	return rc;
}

static BOOL out_reserve(AV1_MF* enc, size_t size)
{
	if (size <= enc->outCapacity)
		return TRUE;
	BYTE* p = realloc(enc->out, size);
	if (!p)
		return FALSE;
	enc->out = p;
	enc->outCapacity = size;
	return TRUE;
}

/* the first OBU of @p type in the temporal unit, its payload in @p payload */
static BOOL find_obu(const BYTE* data, size_t size, BYTE type, const BYTE** payload)
{
	size_t pos = 0;
	while (pos < size)
	{
		const BYTE hdr = data[pos];
		const BYTE t = (hdr >> 3) & 0x0f;
		size_t p = pos + 1 + ((hdr & 0x04) ? 1 : 0);
		if (!(hdr & 0x02) || (p > size))
			return FALSE; /* no size field: can't walk on */
		size_t len = 0;
		for (unsigned i = 0; i < 8; i++)
		{
			if (p >= size)
				return FALSE;
			const BYTE b = data[p++];
			len |= (size_t)(b & 0x7f) << (7 * i);
			if (!(b & 0x80))
				break;
		}
		if (len > size - p)
			return FALSE;
		if (t == type)
		{
			*payload = &data[p];
			return TRUE;
		}
		pos = p + len;
	}
	return FALSE;
}

/* a key frame: the frame header (show_existing_frame 0, frame_type KEY) */
static BOOL is_key_frame(const BYTE* data, size_t size)
{
	const BYTE* payload = nullptr;
	if (!find_obu(data, size, 6 /* frame */, &payload) &&
	    !find_obu(data, size, 3 /* frame header */, &payload))
		return FALSE;
	return ((payload[0] & 0x80) == 0) && (((payload[0] >> 5) & 3) == 0);
}

static BOOL collect(AV1_MF* enc)
{
	MFT_OUTPUT_DATA_BUFFER ob = WINPR_C_ARRAY_INIT;
	IMFMediaBuffer* buf = nullptr;
	BOOL rc = FALSE;

	if (!enc->providesSamples)
	{
		IMFMediaBuffer* b = nullptr;
		if (FAILED(enc->MFCreateMemoryBuffer(enc->outputSize, &b)))
			return FALSE;
		if (FAILED(enc->MFCreateSample(&ob.pSample)) || FAILED(IMFSample_AddBuffer(ob.pSample, b)))
		{
			RELEASE(b);
			RELEASE(ob.pSample);
			return FALSE;
		}
		RELEASE(b);
	}
	DWORD status = 0;
	const HRESULT hr = IMFTransform_ProcessOutput(enc->mft, 0, 1, &ob, &status);
	enc->haveOutput--;
	if (FAILED(hr))
	{
		WLog_Print(enc->log, WLOG_WARN, "%s: ProcessOutput 0x%08" PRIX32, enc->name, (UINT32)hr);
		goto fail;
	}

	if (!enc->seqHdr)
	{
		IMFMediaType* cur = nullptr;
		if (SUCCEEDED(IMFTransform_GetOutputCurrentType(enc->mft, 0, &cur)))
		{
			(void)IMFMediaType_GetAllocatedBlob(cur, &sMF_MT_MPEG_SEQUENCE_HEADER, &enc->seqHdr,
			                                    &enc->seqHdrSize);
			RELEASE(cur);
		}
	}

	BYTE* data = nullptr;
	DWORD len = 0;
	if (FAILED(IMFSample_ConvertToContiguousBuffer(ob.pSample, &buf)) ||
	    FAILED(IMFMediaBuffer_Lock(buf, &data, nullptr, &len)))
		goto fail;

	/* The encoder keeps the sequence header in its output type; the stream needs it in band
	 * in every key frame, after the temporal delimiter. */
	UINT32 clean = 0;
	(void)IMFSample_GetUINT32(ob.pSample, &sMFSampleExtension_CleanPoint, &clean);
	const BYTE* sh = nullptr;
	const BOOL key = clean || is_key_frame(data, len);
	const BOOL insert = key && enc->seqHdr && !find_obu(data, len, 1, &sh) && (len >= 2) &&
	                    (((data[0] >> 3) & 0x0f) == 2) && (data[1] == 0);
	if (!out_reserve(enc, (size_t)len + (insert ? enc->seqHdrSize : 0)))
	{
		(void)IMFMediaBuffer_Unlock(buf);
		goto fail;
	}
	if (insert)
	{
		memcpy(enc->out, data, 2);
		memcpy(&enc->out[2], enc->seqHdr, enc->seqHdrSize);
		memcpy(&enc->out[2 + enc->seqHdrSize], &data[2], len - 2);
		enc->outSize = (size_t)len + enc->seqHdrSize;
	}
	else
	{
		memcpy(enc->out, data, len);
		enc->outSize = len;
	}
	(void)IMFMediaBuffer_Unlock(buf);
	rc = TRUE;

fail:
	RELEASE(buf);
	RELEASE(ob.pSample);
	if (ob.pEvents)
		RELEASE(ob.pEvents);
	return rc;
}

/* the mean bit rate for the real frame rate: target x nominal rate x mean frame duration */
static void follow_frame_rate(AV1_MF* enc, UINT32 durationMs)
{
	if (!enc->rateFollows || (durationMs == 0))
		return;
	enc->frameMs += (durationMs - enc->frameMs) / 8.0; /* about the last 8 frames */
	const double want = enc->kbps * 1000.0 * AV1_MF_FRAME_RATE * enc->frameMs / 1000.0;
	if ((want < 1000.0) || (want > (double)UINT32_MAX))
		return;
	const double now = enc->meanBitRate;
	if ((want > now * 0.95) && (want < now * 1.05))
		return;
	const HRESULT hr =
	    set_ui4(enc, &sCODECAPI_AVEncCommonMeanBitRate, (UINT32)want, "AVEncCommonMeanBitRate");
	if (SUCCEEDED(hr))
	{
		enc->meanBitRate = (UINT32)want;
		return;
	}
	enc->rateFollows = FALSE;
	WLog_Print(enc->log, WLOG_WARN,
	           "%s takes no new mean bit rate (0x%08" PRIX32 "): at %.0f fps it gets the bit "
	           "rate of %d fps",
	           enc->name, (UINT32)hr, 1000.0 / enc->frameMs, AV1_MF_FRAME_RATE);
}

INT32 av1_mf_encode(AV1_MF* enc, const BYTE* const planes[3], const UINT32 strides[3],
                    UINT32 durationMs, BOOL keyFrame, const BYTE** ppData, UINT32* pSize)
{
	WINPR_ASSERT(enc);
	WINPR_ASSERT(ppData);
	WINPR_ASSERT(pSize);
	*ppData = nullptr;
	*pSize = 0;

	const UINT64 deadline = GetTickCount64() + AV1_MF_TIMEOUT_MS;
	while (enc->needInput == 0)
	{
		if (!next_event(enc, deadline))
			return -1;
	}
	if (keyFrame)
		(void)set_ui4(enc, &sCODECAPI_AVEncVideoForceKeyFrame, 1, "AVEncVideoForceKeyFrame");
	follow_frame_rate(enc, durationMs);
	if (!feed(enc, planes, strides, durationMs))
		return -2;

	/* low latency: each frame comes out before the next goes in */
	while (enc->haveOutput == 0)
	{
		if (!next_event(enc, deadline))
			return -3;
	}
	if (!collect(enc) || (enc->outSize == 0) || (enc->outSize > UINT32_MAX))
		return -4;
	*ppData = enc->out;
	*pSize = (UINT32)enc->outSize;
	return 1;
}
