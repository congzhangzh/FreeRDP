/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: VideoToolbox decodes a client's MJPG and H.264 to the camera's NV12
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

/* The pictures are made here: the placeholder at 640x360 as a JPEG (ImageIO), and as H.264
 * (VideoToolbox's own encoder, its output turned into the Annex B a client sends). Each is
 * decoded by camera_decode.m to NV12 '420v' at 1280x720, and its tones are checked. This is
 * the proof that VideoToolbox decodes JPEG (kCMVideoCodecType_JPEG) straight to NV12 on the
 * runner. H.264 is skipped (not failed) when the runner has no encoder to make the stream. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <VideoToolbox/VideoToolbox.h>

#include "camera_config.h"
#include "camera_decode.h"
#include "camera_sink.h"
#include "placeholder.h"

static int g_failures = 0;

#define CHECK(cond)                                                                    \
	do                                                                                 \
	{                                                                                  \
		if (!(cond))                                                                   \
		{                                                                              \
			(void)fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond);     \
			g_failures++;                                                              \
		}                                                                              \
	} while (0)

#define SRC_W 640
#define SRC_H 360

static int video_luma(int gray)
{
	return 16 + (gray * 219 + 127) / 255;
}

/* the decoded camera frame shows the placeholder: dark corner, light body (a lossy codec:
 * within a margin) */
static void check_frame(const char* what, CVPixelBufferRef pixels)
{
	CHECK(pixels);
	if (!pixels)
		return;
	const OSType format = CVPixelBufferGetPixelFormatType(pixels);
	printf("%s: decoded %zux%zu '%c%c%c%c'\n", what, CVPixelBufferGetWidth(pixels),
	       CVPixelBufferGetHeight(pixels), (char)(format >> 24), (char)(format >> 16),
	       (char)(format >> 8), (char)format);
	CHECK(format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
	CHECK(CVPixelBufferGetWidth(pixels) == CAM_MAC_WIDTH);
	CHECK(CVPixelBufferGetHeight(pixels) == CAM_MAC_HEIGHT);
	if ((format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ||
	    (CVPixelBufferGetWidth(pixels) != CAM_MAC_WIDTH))
		return;
	CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
	const uint8_t* y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
	const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
	const int corner = y[20 * ys + 20];
	const size_t bx = (size_t)(640 - 0.30 * 288);
	const size_t by = (size_t)(360 + 0.15 * 288);
	const int body = y[by * ys + bx];
	const uint8_t* uv = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
	const size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
	const int u = uv[(by / 2) * uvs + (bx & ~1u)];
	const int v = uv[(by / 2) * uvs + (bx & ~1u) + 1];
	CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
	printf("%s: corner luma %d (want ~%d), body luma %d (want ~%d), chroma %d/%d (want ~128)\n",
	       what, corner, video_luma(CAMERA_PLACEHOLDER_BACKGROUND), body,
	       video_luma(CAMERA_PLACEHOLDER_FOREGROUND), u, v);
	CHECK(abs(corner - video_luma(CAMERA_PLACEHOLDER_BACKGROUND)) <= 8);
	CHECK(abs(body - video_luma(CAMERA_PLACEHOLDER_FOREGROUND)) <= 12);
	CHECK((abs(u - 128) <= 6) && (abs(v - 128) <= 6));
}

/* --- MJPG ----------------------------------------------------------------------------------- */

static NSData* placeholder_jpeg(void)
{
	uint8_t* gray = malloc((size_t)SRC_W * SRC_H);
	if (!gray || !camera_placeholder_gray(gray, SRC_W, SRC_W, SRC_H))
	{
		free(gray);
		return nil;
	}
	CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
	CFDataRef bytes = CFDataCreate(kCFAllocatorDefault, gray, (CFIndex)SRC_W * SRC_H);
	free(gray);
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
	CGImageRef image = CGImageCreate(SRC_W, SRC_H, 8, 8, SRC_W, space, kCGImageAlphaNone, provider,
	                                 NULL, false, kCGRenderingIntentDefault);
	NSMutableData* jpeg = [NSMutableData data];
	CGImageDestinationRef dest = CGImageDestinationCreateWithData(
	    (__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, NULL);
	BOOL ok = NO;
	if (dest && image)
	{
		NSDictionary* options =
		    @{ (__bridge NSString*)kCGImageDestinationLossyCompressionQuality : @0.9 };
		CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)options);
		ok = CGImageDestinationFinalize(dest);
	}
	if (dest)
		CFRelease(dest);
	if (image)
		CGImageRelease(image);
	CGDataProviderRelease(provider);
	CFRelease(bytes);
	CGColorSpaceRelease(space);
	return ok ? jpeg : nil;
}

static void test_mjpg(void)
{
	NSData* jpeg = placeholder_jpeg();
	CHECK(jpeg && (jpeg.length > 100));
	if (!jpeg)
		return;
	printf("MJPG: a %lu-byte JPEG of %dx%d\n", (unsigned long)jpeg.length, SRC_W, SRC_H);
	printf("MJPG: VideoToolbox hardware JPEG decode %s\n",
	       VTIsHardwareDecodeSupported(kCMVideoCodecType_JPEG) ? "supported" : "not supported");
	camMacDecoder* decoder =
	    cam_mac_decoder_new(CAM_MAC_FORMAT_MJPG, SRC_W, SRC_H, CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CHECK(decoder);
	if (!decoder)
		return;
	/* twice: the session is kept between frames */
	for (int n = 0; n < 2; n++)
	{
		CVPixelBufferRef frame = cam_mac_decoder_decode(decoder, jpeg.bytes, jpeg.length);
		check_frame("MJPG", frame);
		if (frame)
			CVPixelBufferRelease(frame);
	}
	/* garbage: no frame, no crash */
	const uint8_t junk[64] = { 0xFF, 0xD8, 0xFF, 0x00 };
	CVPixelBufferRef none = cam_mac_decoder_decode(decoder, junk, sizeof(junk));
	CHECK(none == NULL);
	if (none)
		CVPixelBufferRelease(none);
	cam_mac_decoder_free(decoder);
}

/* --- H.264 ---------------------------------------------------------------------------------- */

static void append_start_code(NSMutableData* out, const uint8_t* nal, size_t size)
{
	static const uint8_t start[4] = { 0, 0, 0, 1 };
	[out appendBytes:start length:sizeof(start)];
	[out appendBytes:nal length:size];
}

/* VideoToolbox's sample (AVCC, parameter sets in its format description) as a client's Annex B
 * access unit: SPS, PPS, then the slices, each behind a start code */
static NSData* annexb_of(CMSampleBufferRef sample)
{
	CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
	size_t count = 0;
	int lengthSize = 0;
	if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &count,
	                                                       &lengthSize) != noErr)
		return nil;
	NSMutableData* out = [NSMutableData data];
	for (size_t i = 0; i < count; i++)
	{
		const uint8_t* set = NULL;
		size_t size = 0;
		if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &size, NULL,
		                                                       NULL) != noErr)
			return nil;
		append_start_code(out, set, size);
	}
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
	const size_t total = CMBlockBufferGetDataLength(block);
	uint8_t* data = malloc(total);
	if (!data || (CMBlockBufferCopyDataBytes(block, 0, total, data) != kCMBlockBufferNoErr))
	{
		free(data);
		return nil;
	}
	for (size_t pos = 0; pos + (size_t)lengthSize <= total;)
	{
		size_t nal = 0;
		for (int b = 0; b < lengthSize; b++)
			nal = (nal << 8) | data[pos + (size_t)b];
		pos += (size_t)lengthSize;
		if (pos + nal > total)
			break;
		append_start_code(out, &data[pos], nal);
		pos += nal;
	}
	free(data);
	return out;
}

/* one key frame of the placeholder from VideoToolbox's encoder; nil when there is none */
static NSData* placeholder_h264(void)
{
	VTCompressionSessionRef session = NULL;
	if (VTCompressionSessionCreate(kCFAllocatorDefault, SRC_W, SRC_H, kCMVideoCodecType_H264, NULL,
	                               NULL, NULL, NULL, NULL, &session) != noErr)
		return nil;
	(void)VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	(void)VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
	                           kVTProfileLevel_H264_Baseline_AutoLevel);
	CVPixelBufferRef pixels = cam_mac_pixel_buffer_placeholder(SRC_W, SRC_H);
	__block NSData* annexb = nil;
	NSDictionary* frameOptions = @{ (__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES };
	const OSStatus status = VTCompressionSessionEncodeFrameWithOutputHandler(
	    session, pixels, CMTimeMake(0, CAM_MAC_FPS), CMTimeMake(1, CAM_MAC_FPS),
	    (__bridge CFDictionaryRef)frameOptions, NULL,
	    ^(OSStatus frameStatus, VTEncodeInfoFlags info, CMSampleBufferRef sample) {
		    (void)info;
		    if ((frameStatus == noErr) && sample && !annexb)
			    annexb = annexb_of(sample);
	    });
	(void)VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
	VTCompressionSessionInvalidate(session);
	CFRelease(session);
	CVPixelBufferRelease(pixels);
	return (status == noErr) ? annexb : nil;
}

static void test_h264(void)
{
	NSData* annexb = placeholder_h264();
	if (!annexb)
	{
		printf("SKIP H.264: no VideoToolbox H.264 encoder here to make the test stream\n");
		return;
	}
	printf("H.264: a %lu-byte Annex B key frame of %dx%d\n", (unsigned long)annexb.length, SRC_W,
	       SRC_H);
	camMacDecoder* decoder =
	    cam_mac_decoder_new(CAM_MAC_FORMAT_H264, SRC_W, SRC_H, CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CHECK(decoder);
	if (!decoder)
		return;
	CVPixelBufferRef frame = cam_mac_decoder_decode(decoder, annexb.bytes, annexb.length);
	check_frame("H.264", frame);
	if (frame)
		CVPixelBufferRelease(frame);
	cam_mac_decoder_free(decoder);

	/* a delta frame before any SPS/PPS: nothing yet, no crash */
	decoder = cam_mac_decoder_new(CAM_MAC_FORMAT_H264, SRC_W, SRC_H, CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	const uint8_t slice[] = { 0, 0, 0, 1, 0x41, 0x9A, 0x00, 0x10 };
	frame = cam_mac_decoder_decode(decoder, slice, sizeof(slice));
	CHECK(frame == NULL);
	if (frame)
		CVPixelBufferRelease(frame);
	cam_mac_decoder_free(decoder);
}

int main(void)
{
	@autoreleasepool
	{
		test_mjpg();
		test_h264();
	}
	if (g_failures)
		(void)fprintf(stderr, "TestCameraDecode: %d check(s) failed\n", g_failures);
	else
		printf("TestCameraDecode: all checks passed\n");
	return g_failures ? 1 : 0;
}
