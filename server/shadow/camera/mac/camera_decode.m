/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: a client's H.264 or MJPG decoded by VideoToolbox
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

#include <stdlib.h>
#include <string.h>

#include <os/log.h>

#import <Foundation/Foundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include "camera_config.h"
#include "camera_decode.h"
#include "camera_h264.h"

struct cam_mac_decoder
{
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t dstWidth;
	uint32_t dstHeight;
	CMVideoFormatDescriptionRef description;
	VTDecompressionSessionRef session;
	/* H.264: the parameter sets of the session, to see when they change */
	uint8_t* sps;
	size_t spsSize;
	uint8_t* pps;
	size_t ppsSize;
	uint64_t decoded;
	uint64_t failed;
};

static os_log_t decode_log(void)
{
	static os_log_t log;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		log = os_log_create("org.freerdp.shadow.camera", "decode");
	});
	return log;
}

static void drop_session(camMacDecoder* decoder)
{
	if (decoder->session)
	{
		VTDecompressionSessionInvalidate(decoder->session);
		CFRelease(decoder->session);
		decoder->session = NULL;
	}
}

static bool make_session(camMacDecoder* decoder)
{
	drop_session(decoder);
	if (!decoder->description)
		return false;
	/* VideoToolbox converts (4:2:2 JPEG, full range) and scales to what it is asked for */
	NSDictionary* destination = @{
		(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(id)kCVPixelBufferWidthKey : @(decoder->dstWidth),
		(id)kCVPixelBufferHeightKey : @(decoder->dstHeight),
		(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
	};
	/* no callback: each frame's comes with VTDecompressionSessionDecodeFrameWithOutputHandler */
	const OSStatus status = VTDecompressionSessionCreate(
	    kCFAllocatorDefault, decoder->description, NULL, (__bridge CFDictionaryRef)destination,
	    NULL, &decoder->session);
	if (status != noErr)
	{
		os_log_error(decode_log(), "VTDecompressionSessionCreate (%s): %d",
		             (decoder->format == CAM_MAC_FORMAT_H264) ? "H.264" : "JPEG", (int)status);
		decoder->session = NULL;
		return false;
	}
	return true;
}

static bool same_bytes(const uint8_t* a, size_t aSize, const uint8_t* b, size_t bSize)
{
	return a && b && (aSize == bSize) && (memcmp(a, b, aSize) == 0);
}

static uint8_t* copy_bytes(const uint8_t* data, size_t size)
{
	uint8_t* copy = malloc(size);
	if (copy)
		memcpy(copy, data, size);
	return copy;
}

/* a new SPS or PPS: a new format description and session */
static bool h264_parameters(camMacDecoder* decoder, const cam_h264_info* info)
{
	if (!info->sps || !info->pps)
		return decoder->session != NULL;
	if (decoder->session && same_bytes(info->sps, info->spsSize, decoder->sps, decoder->spsSize) &&
	    same_bytes(info->pps, info->ppsSize, decoder->pps, decoder->ppsSize))
		return true;

	free(decoder->sps);
	free(decoder->pps);
	decoder->sps = copy_bytes(info->sps, info->spsSize);
	decoder->spsSize = info->spsSize;
	decoder->pps = copy_bytes(info->pps, info->ppsSize);
	decoder->ppsSize = info->ppsSize;
	if (!decoder->sps || !decoder->pps)
		return false;

	if (decoder->description)
		CFRelease(decoder->description);
	decoder->description = NULL;
	const uint8_t* sets[2] = { decoder->sps, decoder->pps };
	const size_t sizes[2] = { decoder->spsSize, decoder->ppsSize };
	const OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
	    kCFAllocatorDefault, 2, sets, sizes, 4, &decoder->description);
	if (status != noErr)
	{
		os_log_error(decode_log(), "CMVideoFormatDescriptionCreateFromH264ParameterSets: %d",
		             (int)status);
		decoder->description = NULL;
		return false;
	}
	const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(decoder->description);
	os_log(decode_log(), "H.264 %dx%d from the client, decoded to %ux%u NV12", size.width,
	       size.height, decoder->dstWidth, decoder->dstHeight);
	return make_session(decoder);
}

camMacDecoder* cam_mac_decoder_new(uint32_t format, uint32_t width, uint32_t height,
                                   uint32_t dstWidth, uint32_t dstHeight)
{
	if (((format != CAM_MAC_FORMAT_H264) && (format != CAM_MAC_FORMAT_MJPG)) || (dstWidth == 0) ||
	    (dstHeight == 0) || (dstWidth % 2) || (dstHeight % 2))
		return NULL;
	camMacDecoder* decoder = calloc(1, sizeof(camMacDecoder));
	if (!decoder)
		return NULL;
	decoder->format = format;
	decoder->width = width;
	decoder->height = height;
	decoder->dstWidth = dstWidth;
	decoder->dstHeight = dstHeight;
	if (format == CAM_MAC_FORMAT_MJPG)
	{
		/* a JPEG image is a frame of its own: the codec type and the size are all */
		if ((width == 0) || (height == 0) ||
		    (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_JPEG,
		                                    (int32_t)width, (int32_t)height, NULL,
		                                    &decoder->description) != noErr) ||
		    !make_session(decoder))
		{
			cam_mac_decoder_free(decoder);
			return NULL;
		}
	}
	return decoder;
}

void cam_mac_decoder_free(camMacDecoder* decoder)
{
	if (!decoder)
		return;
	drop_session(decoder);
	if (decoder->description)
		CFRelease(decoder->description);
	free(decoder->sps);
	free(decoder->pps);
	free(decoder);
}

static CMSampleBufferRef sample_of(CMVideoFormatDescriptionRef description, const uint8_t* data,
                                   size_t size)
{
	CMBlockBufferRef block = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, size, kCFAllocatorDefault,
	                                       NULL, 0, size, kCMBlockBufferAssureMemoryNowFlag,
	                                       &block) != kCMBlockBufferNoErr)
		return NULL;
	if (CMBlockBufferReplaceDataBytes(data, block, 0, size) != kCMBlockBufferNoErr)
	{
		CFRelease(block);
		return NULL;
	}
	CMSampleBufferRef sample = NULL;
	const size_t sizes[1] = { size };
	const OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, description, 1,
	                                                  0, NULL, 1, sizes, &sample);
	CFRelease(block);
	return (status == noErr) ? sample : NULL;
}

static CVPixelBufferRef decode_sample(camMacDecoder* decoder, CMSampleBufferRef sample)
{
	__block CVPixelBufferRef out = NULL;
	__block OSStatus result = noErr;
	VTDecodeInfoFlags info = 0;
	/* flags 0: synchronous, the handler runs before this returns */
	OSStatus status = VTDecompressionSessionDecodeFrameWithOutputHandler(
	    decoder->session, sample, 0, &info,
	    ^(OSStatus frameStatus, VTDecodeInfoFlags frameInfo, CVImageBufferRef image,
	      CMTime pts, CMTime duration) {
		    (void)frameInfo;
		    (void)pts;
		    (void)duration;
		    result = frameStatus;
		    if ((frameStatus == noErr) && image && !out)
			    out = CVPixelBufferRetain(image);
	    });
	(void)VTDecompressionSessionWaitForAsynchronousFrames(decoder->session);
	if ((status != noErr) || (result != noErr))
	{
		if (decoder->failed++ % 100 == 0)
			os_log_error(decode_log(), "decode: %d / %d (%llu failed so far)", (int)status,
			             (int)result, (unsigned long long)decoder->failed);
		/* the session is gone (the Mac slept, the media server restarted): a new one */
		if ((status == kVTInvalidSessionErr) || (result == kVTInvalidSessionErr))
			(void)make_session(decoder);
	}
	return out;
}

CVPixelBufferRef cam_mac_decoder_decode(camMacDecoder* decoder, const uint8_t* data, size_t size)
{
	if (!decoder || !data || (size == 0))
		return NULL;

	CMSampleBufferRef sample = NULL;
	if (decoder->format == CAM_MAC_FORMAT_MJPG)
		sample = sample_of(decoder->description, data, size);
	else
	{
		cam_h264_info info = { 0 };
		if (!cam_h264_scan(data, size, &info) || !h264_parameters(decoder, &info) ||
		    (info.avccSize == 0))
			return NULL; /* nothing to decode yet: waiting for a key frame's SPS and PPS */
		uint8_t* avcc = malloc(info.avccSize);
		if (!avcc)
			return NULL;
		const size_t avccSize = cam_h264_to_avcc(data, size, avcc, info.avccSize);
		if (avccSize)
			sample = sample_of(decoder->description, avcc, avccSize);
		free(avcc);
	}
	if (!sample || !decoder->session)
	{
		if (sample)
			CFRelease(sample);
		return NULL;
	}
	CVPixelBufferRef out = decode_sample(decoder, sample);
	CFRelease(sample);
	if (out)
		decoder->decoded++;
	return out;
}
