/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression
 *
 * Copyright 2014 Mike McDonald <Mike.McDonald@software.dell.com>
 * Copyright 2015 Vic Lee <llyzs.vic@gmail.com>
 * Copyright 2014 Armin Novak <armin.novak@gmail.com>
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

#include <winpr/winpr.h>
#include <winpr/library.h>
#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <wels/codec_def.h>
#include <wels/codec_api.h>
#include <wels/codec_ver.h>

#include "h264.h"

typedef void (*pWelsGetCodecVersionEx)(OpenH264Version* pVersion);

typedef long (*pWelsCreateDecoder)(ISVCDecoder** ppDecoder);
typedef void (*pWelsDestroyDecoder)(ISVCDecoder* pDecoder);

typedef int (*pWelsCreateSVCEncoder)(ISVCEncoder** ppEncoder);
typedef void (*pWelsDestroySVCEncoder)(ISVCEncoder* pEncoder);

typedef struct
{
#if defined(WITH_OPENH264_LOADING)
	HMODULE lib;
	OpenH264Version version;
#endif
	pWelsGetCodecVersionEx WelsGetCodecVersionEx;
	WINPR_ATTR_NODISCARD pWelsCreateDecoder WelsCreateDecoder;
	pWelsDestroyDecoder WelsDestroyDecoder;
	WINPR_ATTR_NODISCARD pWelsCreateSVCEncoder WelsCreateSVCEncoder;
	pWelsDestroySVCEncoder WelsDestroySVCEncoder;
	ISVCDecoder* pDecoder;
	ISVCEncoder* pEncoder;
	SEncParamExt EncParamExt;
	UINT64 lastFrameNS; /* when the last frame came, 0: none yet */
	double frameMs;     /* the time between frames, smoothed; 0: not known yet */
} H264_CONTEXT_OPENH264;

#if defined(WITH_OPENH264_LOADING)
static const char* openh264_library_names[] = {
#if defined(_WIN32)
	"openh264.dll"
#elif defined(__APPLE__)
	"libopenh264.dylib"
#else
	"libopenh264.so.8",     "libopenh264.so.2.6.0", "libopenh264.so.7",
	"libopenh264.so.2.5.0", "libopenh264.so.2.4.1", "libopenh264.so.2.4.0",
	"libopenh264.so.2.3.1", "libopenh264.so.2.3.0", "libopenh264.so",

#endif
};
#endif

static void openh264_trace_callback(void* ctx, int level, const char* message)
{
	H264_CONTEXT* h264 = ctx;
	if (h264)
		WLog_Print(h264->log, WLOG_TRACE, "%d - %s", level, message);
}

static int openh264_decompress(H264_CONTEXT* WINPR_RESTRICT h264,
                               const BYTE* WINPR_RESTRICT pSrcData, UINT32 SrcSize)
{
	DECODING_STATE state = dsInvalidArgument;
	SBufferInfo sBufferInfo = WINPR_C_ARRAY_INIT;
	SSysMEMBuffer* pSystemBuffer = nullptr;
	H264_CONTEXT_OPENH264* sys = nullptr;
	UINT32* iStride = nullptr;
	BYTE** pYUVData = nullptr;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData || (SrcSize == 0));

	sys = (H264_CONTEXT_OPENH264*)h264->pSystemData;
	WINPR_ASSERT(sys);

	iStride = h264->iStride;
	WINPR_ASSERT(iStride);

	pYUVData = h264->pYUVData;
	WINPR_ASSERT(pYUVData);

	if (!sys->pDecoder)
		return -2001;

	/* DecodeFrame2 takes the size as int */
	if (SrcSize > INT32_MAX)
	{
		WLog_Print(h264->log, WLOG_WARN, "SrcSize %" PRIu32 " exceeds INT32_MAX", SrcSize);
		return -2007;
	}

	/*
	 * Decompress the image.  The RDP host only seems to send I420 format.
	 */
	pYUVData[0] = nullptr;
	pYUVData[1] = nullptr;
	pYUVData[2] = nullptr;

	WINPR_ASSERT(sys->pDecoder);
	state = (*sys->pDecoder)
	            ->DecodeFrame2(sys->pDecoder, pSrcData, WINPR_ASSERTING_INT_CAST(int, SrcSize),
	                           pYUVData, &sBufferInfo);

	if (sBufferInfo.iBufferStatus != 1)
	{
		if (state == dsNoParamSets)
		{
			/* this happens on the first frame due to missing parameter sets */
			state =
			    (*sys->pDecoder)->DecodeFrame2(sys->pDecoder, nullptr, 0, pYUVData, &sBufferInfo);
		}
		else if (state == dsErrorFree)
		{
			/* call DecodeFrame2 again to decode without delay */
			state =
			    (*sys->pDecoder)->DecodeFrame2(sys->pDecoder, nullptr, 0, pYUVData, &sBufferInfo);
		}
		else
		{
			WLog_Print(h264->log, WLOG_WARN, "DecodeFrame2 state: 0x%04X iBufferStatus: %d", state,
			           sBufferInfo.iBufferStatus);
			return -2002;
		}
	}

	if (state != dsErrorFree)
	{
		WLog_Print(h264->log, WLOG_WARN, "DecodeFrame2 state: 0x%02X", state);
		return -2003;
	}

#if OPENH264_MAJOR >= 2
	state = (*sys->pDecoder)->FlushFrame(sys->pDecoder, pYUVData, &sBufferInfo);
	if (state != dsErrorFree)
	{
		WLog_Print(h264->log, WLOG_WARN, "FlushFrame state: 0x%02X", state);
		return -2003;
	}
#endif

	pSystemBuffer = &sBufferInfo.UsrData.sSystemBuffer;
	iStride[0] = WINPR_ASSERTING_INT_CAST(uint32_t, pSystemBuffer->iStride[0]);
	iStride[1] = WINPR_ASSERTING_INT_CAST(uint32_t, pSystemBuffer->iStride[1]);
	iStride[2] = WINPR_ASSERTING_INT_CAST(uint32_t, pSystemBuffer->iStride[1]);

	if (sBufferInfo.iBufferStatus != 1)
	{
		WLog_Print(h264->log, WLOG_WARN, "DecodeFrame2 iBufferStatus: %d",
		           sBufferInfo.iBufferStatus);
		return 0;
	}

	if (state != dsErrorFree)
	{
		WLog_Print(h264->log, WLOG_WARN, "DecodeFrame2 state: 0x%02X", state);
		return -2003;
	}

	if (pSystemBuffer->iFormat != videoFormatI420)
		return -2004;

	if (!pYUVData[0] || !pYUVData[1] || !pYUVData[2])
		return -2005;

	if ((pSystemBuffer->iWidth <= 0) || (pSystemBuffer->iHeight <= 0))
		return -2006;

	h264->YUVWidth = WINPR_ASSERTING_INT_CAST(UINT32, pSystemBuffer->iWidth);
	h264->YUVHeight = WINPR_ASSERTING_INT_CAST(UINT32, pSystemBuffer->iHeight);

	return 1;
}

/* OpenH264's rate control gives a frame target bit rate / frame rate (fMaxFrameRate) and dates the
 * frames 1 / frame rate apart. Frames come on damage, not at the configured rate, and AVC444
 * encodes two pictures per frame, so the encoder gets the real rate of its own calls: the time
 * between them, up to 100 ms (a pause counts as one slow frame), averaged over about ten calls (the
 * times, not their inverses, so the average is calls / elapsed time). It is passed on when it moved
 * by a tenth or more; OpenH264 rescales its budget then (RcUpdateBitrateFps). OpenH264 takes at
 * most 60 frames per second: above that it gets 60 and the bit rate scaled by 60 / rate, which
 * gives each call the same share. The configured frame rate is the first guess. */
#define OPENH264_MAX_FRAME_MS 100.0
#define OPENH264_MAX_FRAME_RATE 60.0

static void openh264_measure_frame_rate(H264_CONTEXT_OPENH264* sys)
{
	WINPR_ASSERT(sys);

	const UINT64 now = winpr_GetTickCount64NS();
	if (sys->lastFrameNS != 0)
	{
		double ms = (double)(now - sys->lastFrameNS) / 1000000.0;
		if (ms > OPENH264_MAX_FRAME_MS)
			ms = OPENH264_MAX_FRAME_MS;
		sys->frameMs = (sys->frameMs > 0.0) ? (0.9 * sys->frameMs + 0.1 * ms) : ms;
	}
	sys->lastFrameNS = now;
}

/* the calls per second: the real rate when known, else the configured frame rate; at least 1 */
static double openh264_call_rate(const H264_CONTEXT* h264, const H264_CONTEXT_OPENH264* sys)
{
	WINPR_ASSERT(h264);
	WINPR_ASSERT(sys);

	const double rate = (sys->frameMs > 0.0) ? (1000.0 / sys->frameMs) : (double)h264->FrameRate;
	return (rate < 1.0) ? 1.0 : rate;
}

/* the frame rate for OpenH264: the call rate, at most 60 */
static float openh264_frame_rate(const H264_CONTEXT* h264, const H264_CONTEXT_OPENH264* sys)
{
	const double rate = openh264_call_rate(h264, sys);
	return (float)((rate > OPENH264_MAX_FRAME_RATE) ? OPENH264_MAX_FRAME_RATE : rate);
}

/* the bit rate for OpenH264: the configured one, scaled by 60 / rate above 60 calls per second */
static int openh264_bit_rate(const H264_CONTEXT* h264, const H264_CONTEXT_OPENH264* sys)
{
	const double rate = openh264_call_rate(h264, sys);
	double bitRate = (double)h264->BitRate;
	if (rate > OPENH264_MAX_FRAME_RATE)
		bitRate = bitRate * OPENH264_MAX_FRAME_RATE / rate;
	return (bitRate < 1.0) ? 1 : (int)bitRate;
}

static int openh264_compress(H264_CONTEXT* WINPR_RESTRICT h264,
                             const BYTE** WINPR_RESTRICT pYUVData,
                             const UINT32* WINPR_RESTRICT iStride, BYTE** WINPR_RESTRICT ppDstData,
                             UINT32* WINPR_RESTRICT pDstSize)
{
	int status = 0;
	SFrameBSInfo info = WINPR_C_ARRAY_INIT;
	SSourcePicture pic = WINPR_C_ARRAY_INIT;

	H264_CONTEXT_OPENH264* sys = nullptr;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pYUVData);
	WINPR_ASSERT(iStride);
	WINPR_ASSERT(ppDstData);
	WINPR_ASSERT(pDstSize);

	sys = &((H264_CONTEXT_OPENH264*)h264->pSystemData)[0];
	WINPR_ASSERT(sys);

	if (!sys->pEncoder)
		return -1;

	openh264_measure_frame_rate(sys);

	if (!pYUVData[0] || !pYUVData[1] || !pYUVData[2])
		return -1;

	if ((h264->width > INT_MAX) || (h264->height > INT_MAX))
		return -1;

	if ((h264->FrameRate > INT_MAX) || (h264->NumberOfThreads > INT_MAX) ||
	    (h264->BitRate > INT_MAX) || (h264->QP > INT_MAX))
		return -1;

	WINPR_ASSERT(sys->pEncoder);
	if ((sys->EncParamExt.iPicWidth != (int)h264->width) ||
	    (sys->EncParamExt.iPicHeight != (int)h264->height))
	{
		WINPR_ASSERT((*sys->pEncoder)->GetDefaultParams);
		status = (*sys->pEncoder)->GetDefaultParams(sys->pEncoder, &sys->EncParamExt);

		if (status != cmResultSuccess)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "Failed to get OpenH264 default parameters (status=%d)", status);
			return status > 0 ? -status : status;
		}

		EUsageType usageType = SCREEN_CONTENT_REAL_TIME;

		switch (h264->UsageType)
		{
			case H264_CAMERA_VIDEO_NON_REAL_TIME:
				usageType = CAMERA_VIDEO_NON_REAL_TIME;
				break;
			case H264_CAMERA_VIDEO_REAL_TIME:
				usageType = CAMERA_VIDEO_REAL_TIME;
				break;
			case H264_SCREEN_CONTENT_NON_REAL_TIME:
				usageType = SCREEN_CONTENT_NON_REAL_TIME;
				break;
			case H264_SCREEN_CONTENT_REAL_TIME:
			default:
				break;
		}

		sys->EncParamExt.iUsageType = usageType;
		sys->EncParamExt.iPicWidth = WINPR_ASSERTING_INT_CAST(int, h264->width);
		sys->EncParamExt.iPicHeight = WINPR_ASSERTING_INT_CAST(int, h264->height);
		sys->EncParamExt.fMaxFrameRate = openh264_frame_rate(h264, sys);
		sys->EncParamExt.iMaxBitrate = UNSPECIFIED_BIT_RATE;
		sys->EncParamExt.bEnableDenoise = 0;
		sys->EncParamExt.bEnableLongTermReference = 0;
		sys->EncParamExt.iSpatialLayerNum = 1;
		sys->EncParamExt.iMultipleThreadIdc =
		    WINPR_ASSERTING_INT_CAST(unsigned short, h264->NumberOfThreads);
		sys->EncParamExt.sSpatialLayers[0].fFrameRate = sys->EncParamExt.fMaxFrameRate;
		sys->EncParamExt.sSpatialLayers[0].iVideoWidth = sys->EncParamExt.iPicWidth;
		sys->EncParamExt.sSpatialLayers[0].iVideoHeight = sys->EncParamExt.iPicHeight;
		sys->EncParamExt.sSpatialLayers[0].iMaxSpatialBitrate = sys->EncParamExt.iMaxBitrate;

		switch (h264->RateControlMode)
		{
			case H264_RATECONTROL_VBR:
				sys->EncParamExt.iRCMode = RC_BITRATE_MODE;
				sys->EncParamExt.iTargetBitrate = openh264_bit_rate(h264, sys);
				sys->EncParamExt.sSpatialLayers[0].iSpatialBitrate =
				    sys->EncParamExt.iTargetBitrate;
				sys->EncParamExt.bEnableFrameSkip = 1;
				break;

			case H264_RATECONTROL_CQP:
				sys->EncParamExt.iRCMode = RC_OFF_MODE;
				sys->EncParamExt.sSpatialLayers[0].iDLayerQp = (int)h264->QP;
				sys->EncParamExt.bEnableFrameSkip = 0;
				break;
			default:
				break;
		}

		if (sys->EncParamExt.iMultipleThreadIdc > 1)
		{
#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
			sys->EncParamExt.sSpatialLayers[0].sSliceCfg.uiSliceMode = SM_AUTO_SLICE;
#else
			sys->EncParamExt.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
#endif
		}

		WINPR_ASSERT((*sys->pEncoder)->InitializeExt);
		status = (*sys->pEncoder)->InitializeExt(sys->pEncoder, &sys->EncParamExt);

		if (status != cmResultSuccess)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to initialize OpenH264 encoder (status=%d)",
			           status);
			return status > 0 ? -status : status;
		}

		WINPR_ASSERT((*sys->pEncoder)->GetOption);
		status =
		    (*sys->pEncoder)
		        ->GetOption(sys->pEncoder, ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &sys->EncParamExt);

		if (status != cmResultSuccess)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "Failed to get initial OpenH264 encoder parameters (status=%d)", status);
			return status > 0 ? -status : status;
		}
	}
	else
	{
		switch (h264->RateControlMode)
		{
			case H264_RATECONTROL_VBR:
			{
				/* the configured bit rate at once, a scaled one when it moved by a tenth */
				const int bitRate = openh264_bit_rate(h264, sys);
				const int current = sys->EncParamExt.iTargetBitrate;
				if ((bitRate != current) &&
				    ((bitRate == (int)h264->BitRate) || (bitRate - current >= current / 10) ||
				     (current - bitRate >= current / 10)))
				{
					SBitrateInfo bitrate = WINPR_C_ARRAY_INIT;

					sys->EncParamExt.iTargetBitrate = bitRate;
					bitrate.iLayer = SPATIAL_LAYER_ALL;
					bitrate.iBitrate = bitRate;

					WINPR_ASSERT((*sys->pEncoder)->SetOption);
					status = (*sys->pEncoder)
					             ->SetOption(sys->pEncoder, ENCODER_OPTION_BITRATE, &bitrate);

					if (status != cmResultSuccess)
					{
						WLog_Print(h264->log, WLOG_ERROR,
						           "Failed to set encoder bitrate (status=%d)", status);
						return status > 0 ? -status : status;
					}
				}

				const float frameRate = openh264_frame_rate(h264, sys);
				const float change = frameRate - sys->EncParamExt.fMaxFrameRate;
				if ((change >= sys->EncParamExt.fMaxFrameRate / 10.0f) ||
				    (-change >= sys->EncParamExt.fMaxFrameRate / 10.0f))
				{
					sys->EncParamExt.fMaxFrameRate = frameRate;

					WINPR_ASSERT((*sys->pEncoder)->SetOption);
					status = (*sys->pEncoder)
					             ->SetOption(sys->pEncoder, ENCODER_OPTION_FRAME_RATE,
					                         &sys->EncParamExt.fMaxFrameRate);

					if (status != cmResultSuccess)
					{
						WLog_Print(h264->log, WLOG_ERROR,
						           "Failed to set encoder framerate (status=%d)", status);
						return status > 0 ? -status : status;
					}
				}

				break;
			}

			case H264_RATECONTROL_CQP:
				if (sys->EncParamExt.sSpatialLayers[0].iDLayerQp != (int)h264->QP)
				{
					sys->EncParamExt.sSpatialLayers[0].iDLayerQp = (int)h264->QP;

					WINPR_ASSERT((*sys->pEncoder)->SetOption);
					status = (*sys->pEncoder)
					             ->SetOption(sys->pEncoder, ENCODER_OPTION_SVC_ENCODE_PARAM_EXT,
					                         &sys->EncParamExt);

					if (status != cmResultSuccess)
					{
						WLog_Print(h264->log, WLOG_ERROR,
						           "Failed to set encoder parameters (status=%d)", status);
						return status > 0 ? -status : status;
					}
				}

				break;
			default:
				break;
		}
	}

	pic.iPicWidth = (int)h264->width;
	pic.iPicHeight = (int)h264->height;
	pic.iColorFormat = videoFormatI420;
	pic.iStride[0] = (int)iStride[0];
	pic.iStride[1] = (int)iStride[1];
	pic.iStride[2] = (int)iStride[2];
	pic.pData[0] = WINPR_CAST_CONST_PTR_AWAY(pYUVData[0], BYTE*);
	pic.pData[1] = WINPR_CAST_CONST_PTR_AWAY(pYUVData[1], BYTE*);
	pic.pData[2] = WINPR_CAST_CONST_PTR_AWAY(pYUVData[2], BYTE*);

	WINPR_ASSERT((*sys->pEncoder)->EncodeFrame);
	status = (*sys->pEncoder)->EncodeFrame(sys->pEncoder, &pic, &info);

	if (status != cmResultSuccess)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to encode frame (status=%d)", status);
		return status > 0 ? -status : status;
	}

	*ppDstData = info.sLayerInfo[0].pBsBuf;
	*pDstSize = 0;

	for (int i = 0; i < info.iLayerNum; i++)
	{
		for (int j = 0; j < info.sLayerInfo[i].iNalCount; j++)
		{
			const int val = info.sLayerInfo[i].pNalLengthInByte[j];
			*pDstSize += WINPR_ASSERTING_INT_CAST(uint32_t, val);
		}
	}

	return 1;
}

static void openh264_uninit(H264_CONTEXT* h264)
{
	H264_CONTEXT_OPENH264* sysContexts = nullptr;

	WINPR_ASSERT(h264);

	sysContexts = (H264_CONTEXT_OPENH264*)h264->pSystemData;

	if (sysContexts)
	{
		for (UINT32 x = 0; x < h264->numSystemData; x++)
		{
			H264_CONTEXT_OPENH264* sys = &sysContexts[x];

			if (sys->pDecoder)
			{
				(*sys->pDecoder)->Uninitialize(sys->pDecoder);
				sysContexts->WelsDestroyDecoder(sys->pDecoder);
				sys->pDecoder = nullptr;
			}

			if (sys->pEncoder)
			{
				(*sys->pEncoder)->Uninitialize(sys->pEncoder);
				sysContexts->WelsDestroySVCEncoder(sys->pEncoder);
				sys->pEncoder = nullptr;
			}
		}

#if defined(WITH_OPENH264_LOADING)
		if (sysContexts->lib)
			FreeLibrary(sysContexts->lib);
#endif
		free(h264->pSystemData);
		h264->pSystemData = nullptr;
	}
}

#if defined(WITH_OPENH264_LOADING)
static BOOL openh264_load_functionpointers(H264_CONTEXT* h264, const char* name)
{
	H264_CONTEXT_OPENH264* sysContexts;

	WINPR_ASSERT(name);

	if (!h264)
		return FALSE;

	sysContexts = h264->pSystemData;

	if (!sysContexts)
		return FALSE;

	sysContexts->lib = LoadLibraryA(name);

	if (!sysContexts->lib)
		return FALSE;

	sysContexts->WelsGetCodecVersionEx =
	    GetProcAddressAs(sysContexts->lib, "WelsGetCodecVersionEx", pWelsGetCodecVersionEx);
	sysContexts->WelsCreateDecoder =
	    GetProcAddressAs(sysContexts->lib, "WelsCreateDecoder", pWelsCreateDecoder);
	sysContexts->WelsDestroyDecoder =
	    GetProcAddressAs(sysContexts->lib, "WelsDestroyDecoder", pWelsDestroyDecoder);
	sysContexts->WelsCreateSVCEncoder =
	    GetProcAddressAs(sysContexts->lib, "WelsCreateSVCEncoder", pWelsCreateSVCEncoder);
	sysContexts->WelsDestroySVCEncoder =
	    GetProcAddressAs(sysContexts->lib, "WelsDestroySVCEncoder", pWelsDestroySVCEncoder);

	if (!sysContexts->WelsCreateDecoder || !sysContexts->WelsDestroyDecoder ||
	    !sysContexts->WelsCreateSVCEncoder || !sysContexts->WelsDestroySVCEncoder ||
	    !sysContexts->WelsGetCodecVersionEx)
	{
		FreeLibrary(sysContexts->lib);
		sysContexts->lib = nullptr;
		return FALSE;
	}

	sysContexts->WelsGetCodecVersionEx(&sysContexts->version);
	WLog_Print(h264->log, WLOG_DEBUG, "loaded %s %u.%u.%u", name, sysContexts->version.uMajor,
	           sysContexts->version.uMinor, sysContexts->version.uRevision);

	if ((sysContexts->version.uMajor < 1) ||
	    ((sysContexts->version.uMajor == 1) && (sysContexts->version.uMinor < 6)))
	{
		WLog_Print(
		    h264->log, WLOG_ERROR,
		    "OpenH264 %s %u.%u.%u is too old, need at least version 1.6.0 for dynamic loading",
		    name, sysContexts->version.uMajor, sysContexts->version.uMinor,
		    sysContexts->version.uRevision);
		FreeLibrary(sysContexts->lib);
		sysContexts->lib = nullptr;
		return FALSE;
	}

	return TRUE;
}
#endif

static BOOL openh264_init(H264_CONTEXT* h264)
{
	if (h264->hevc || h264->hevc444)
		return FALSE;

#if defined(WITH_OPENH264_LOADING)
	BOOL success = FALSE;
#endif
	long status = 0;
	H264_CONTEXT_OPENH264* sysContexts = nullptr;
	static int traceLevel = WELS_LOG_DEBUG;
#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
	static EVideoFormatType videoFormat = videoFormatI420;
#endif
	static WelsTraceCallback traceCallback = openh264_trace_callback;

	WINPR_ASSERT(h264);

	h264->numSystemData = 1;
	sysContexts =
	    (H264_CONTEXT_OPENH264*)calloc(h264->numSystemData, sizeof(H264_CONTEXT_OPENH264));

	if (!sysContexts)
		goto EXCEPTION;

	h264->pSystemData = (void*)sysContexts;
#if defined(WITH_OPENH264_LOADING)

	for (size_t i = 0; i < ARRAYSIZE(openh264_library_names); i++)
	{
		const char* current = openh264_library_names[i];
		success = openh264_load_functionpointers(h264, current);

		if (success)
			break;
	}

	if (!success)
		goto EXCEPTION;

#else
	sysContexts->WelsGetCodecVersionEx = WelsGetCodecVersionEx;
	sysContexts->WelsCreateDecoder = WelsCreateDecoder;
	sysContexts->WelsDestroyDecoder = WelsDestroyDecoder;
	sysContexts->WelsCreateSVCEncoder = WelsCreateSVCEncoder;
	sysContexts->WelsDestroySVCEncoder = WelsDestroySVCEncoder;
#endif

	for (UINT32 x = 0; x < h264->numSystemData; x++)
	{
		SDecodingParam sDecParam = WINPR_C_ARRAY_INIT;
		H264_CONTEXT_OPENH264* sys = &sysContexts[x];

		if (h264->Compressor)
		{
#if defined(WITH_OPENH264_LOADING)
			if (sysContexts->version.uMajor != OPENH264_MAJOR ||
			    sysContexts->version.uMinor != OPENH264_MINOR)
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "OpenH264 encoder ABI mismatch: runtime %d.%d.%d vs compiled %d.%d.%d",
				           sysContexts->version.uMajor, sysContexts->version.uMinor,
				           sysContexts->version.uRevision, OPENH264_MAJOR, OPENH264_MINOR,
				           OPENH264_REVISION);
				goto EXCEPTION;
			}
#endif
			const int rc = sysContexts->WelsCreateSVCEncoder(&sys->pEncoder);
			if (rc != 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Failed to create OpenH264 encoder: %d", rc);
				goto EXCEPTION;
			}
			if (!sys->pEncoder)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Failed to create OpenH264 encoder");
				goto EXCEPTION;
			}
		}
		else
		{
			const long rc = sysContexts->WelsCreateDecoder(&sys->pDecoder);
			if (rc != 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Failed to create OpenH264 decoder: %ld", rc);
				goto EXCEPTION;
			}
			if (!sys->pDecoder)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Failed to create OpenH264 decoder");
				goto EXCEPTION;
			}

#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
			sDecParam.eOutputColorFormat = videoFormatI420;
#endif
			sDecParam.eEcActiveIdc = ERROR_CON_FRAME_COPY;
			sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
			status = (*sys->pDecoder)->Initialize(sys->pDecoder, &sDecParam);

			if (status != 0)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "Failed to initialize OpenH264 decoder (status=%ld)", status);
				goto EXCEPTION;
			}

#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
			status =
			    (*sys->pDecoder)->SetOption(sys->pDecoder, DECODER_OPTION_DATAFORMAT, &videoFormat);
#endif

			if (status != 0)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "Failed to set data format option on OpenH264 decoder (status=%ld)",
				           status);
				goto EXCEPTION;
			}

			if (WLog_GetLogLevel(h264->log) == WLOG_TRACE)
			{
				status = (*sys->pDecoder)
				             ->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_LEVEL, &traceLevel);

				if (status != 0)
				{
					WLog_Print(h264->log, WLOG_ERROR,
					           "Failed to set trace level option on OpenH264 decoder (status=%ld)",
					           status);
					goto EXCEPTION;
				}

				status = (*sys->pDecoder)
				             ->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_CALLBACK_CONTEXT,
				                         (void*)&h264);

				if (status != 0)
				{
					WLog_Print(h264->log, WLOG_ERROR,
					           "Failed to set trace callback context option on OpenH264 decoder "
					           "(status=%ld)",
					           status);
					goto EXCEPTION;
				}

				status = (*sys->pDecoder)
				             ->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_CALLBACK,
				                         (void*)&traceCallback);

				if (status != 0)
				{
					WLog_Print(
					    h264->log, WLOG_ERROR,
					    "Failed to set trace callback option on OpenH264 decoder (status=%ld)",
					    status);
					goto EXCEPTION;
				}
			}
		}
	}

	h264->hwAccel = FALSE; /* not supported */
	h264->decoderHardware = FREERDP_DECODE_HARDWARE_NO;
	return TRUE;
EXCEPTION:
	openh264_uninit(h264);
	return FALSE;
}

const H264_CONTEXT_SUBSYSTEM g_Subsystem_OpenH264 = { "OpenH264", openh264_init, openh264_uninit,
	                                                  openh264_decompress, openh264_compress };
