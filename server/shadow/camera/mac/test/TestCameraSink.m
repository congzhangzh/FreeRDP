/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: the sink helper's buffers, and the sink without an extension
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

/* What runs without the extension activated (a CI runner can't approve one): the pixel
 * buffers and sample buffers the sink pushes, built from a client's raw sample, and the sink
 * finding no device without hanging or crashing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#import <Foundation/Foundation.h>

#include "camera_config.h"
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

static BOOL is_camera_nv12(CVPixelBufferRef pixels, size_t width, size_t height)
{
	return pixels &&
	       (CVPixelBufferGetPixelFormatType(pixels) ==
	        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) &&
	       (CVPixelBufferGetWidth(pixels) == width) && (CVPixelBufferGetHeight(pixels) == height) &&
	       (CVPixelBufferGetPlaneCount(pixels) == 2) && (CVPixelBufferGetIOSurface(pixels) != NULL);
}

static void test_pixel_buffers(void)
{
	CVPixelBufferRef pixels = cam_mac_pixel_buffer_new(CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CHECK(is_camera_nv12(pixels, CAM_MAC_WIDTH, CAM_MAC_HEIGHT));
	CVPixelBufferRelease(pixels);
	CHECK(cam_mac_pixel_buffer_new(641, 360) == NULL);

	/* NV12 320x180 with a pattern: scaled to 1280x720, each source pixel 4x4 */
	const uint32_t w = 320;
	const uint32_t h = 180;
	const size_t size = (size_t)w * h * 3 / 2;
	uint8_t* nv12 = malloc(size);
	CHECK(nv12);
	if (!nv12)
		return;
	for (size_t i = 0; i < (size_t)w * h; i++)
		nv12[i] = (uint8_t)(16 + (i * 7) % 200);
	for (size_t i = (size_t)w * h; i < size; i++)
		nv12[i] = (uint8_t)(64 + (i * 3) % 128);
	pixels = cam_mac_pixel_buffer_from_raw(CAM_MAC_FORMAT_NV12, w, h, nv12, size);
	CHECK(is_camera_nv12(pixels, CAM_MAC_WIDTH, CAM_MAC_HEIGHT));
	if (pixels)
	{
		CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
		const uint8_t* y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
		const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
		const uint8_t* uv = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
		const size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
		BOOL same = YES;
		for (uint32_t sy = 0; sy < h; sy += 17)
			for (uint32_t sx = 0; sx < w; sx += 13)
			{
				same = same && (y[(size_t)(4 * sy + 3) * ys + 4 * sx + 1] == nv12[sy * w + sx]);
				/* chroma: source pair (sx & ~1) of row sy / 2 */
				const uint8_t* c = &nv12[(size_t)w * h + (sy / 2) * w + (sx & ~1u)];
				const uint8_t* d = &uv[(size_t)(2 * sy) * uvs + 4 * (sx & ~1u)];
				same = same && (d[0] == c[0]) && (d[1] == c[1]);
			}
		CHECK(same);
		CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferRelease(pixels);
	}
	/* the wrong size, a compressed format: nothing */
	CHECK(cam_mac_pixel_buffer_from_raw(CAM_MAC_FORMAT_NV12, w, h, nv12, size - 1) == NULL);
	CHECK(cam_mac_pixel_buffer_from_raw(CAM_MAC_FORMAT_MJPG, w, h, nv12, size) == NULL);
	free(nv12);

	/* the placeholder: its background in the corner, the icon's tone in the body */
	pixels = cam_mac_pixel_buffer_placeholder(CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CHECK(is_camera_nv12(pixels, CAM_MAC_WIDTH, CAM_MAC_HEIGHT));
	if (pixels)
	{
		CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
		const uint8_t* y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
		const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
		CHECK(y[0] == 16 + (CAMERA_PLACEHOLDER_BACKGROUND * 219 + 127) / 255);
		/* the body at icon (-0.30, 0.15): side 288 px around (640, 360) */
		const size_t bx = (size_t)(640 - 0.30 * 288);
		const size_t by = (size_t)(360 + 0.15 * 288);
		CHECK(y[by * ys + bx] == 16 + (CAMERA_PLACEHOLDER_FOREGROUND * 219 + 127) / 255);
		CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
	}

	/* the sample buffer around it */
	const CMTime pts = CMTimeMake(123456789, 1000000000);
	CMSampleBufferRef sample = cam_mac_sample_buffer_new(pixels, pts);
	CHECK(sample);
	if (sample)
	{
		CHECK(CMSampleBufferIsValid(sample) && CMSampleBufferDataIsReady(sample));
		CHECK(CMSampleBufferGetImageBuffer(sample) == pixels);
		CHECK(CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample), pts) == 0);
		CHECK(CMTimeCompare(CMSampleBufferGetDuration(sample), CMTimeMake(1, CAM_MAC_FPS)) == 0);
		CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
		CHECK(format && (CMFormatDescriptionGetMediaSubType(format) == CAM_MAC_PIXEL_FORMAT));
		CFRelease(sample);
	}
	CVPixelBufferRelease(pixels);
}

static double seconds_now(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static void test_sink_without_extension(void)
{
	/* a device no one has: as the sink is before the extension is approved */
	camMacSink* sink = cam_mac_sink_new("00000000-0000-0000-0000-000000000000");
	CHECK(sink);
	if (!sink)
		return;
	const double start = seconds_now();
	CHECK(cam_mac_sink_open(sink) == CAM_MAC_SINK_NO_DEVICE);
	CHECK(!cam_mac_sink_is_open(sink));
	CHECK(seconds_now() - start < 5.0);

	CVPixelBufferRef small = cam_mac_pixel_buffer_new(640, 360);
	CHECK(cam_mac_sink_push(sink, small) == CAM_MAC_SINK_BAD_FRAME);
	CVPixelBufferRelease(small);
	CVPixelBufferRef frame = cam_mac_pixel_buffer_placeholder(CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	/* owners come and go: no device, no crash, nothing queued */
	for (uint64_t owner = 1; owner <= 3; owner++)
	{
		cam_mac_sink_set_owner(sink, owner);
		CHECK(cam_mac_sink_push(sink, frame) == CAM_MAC_SINK_NO_DEVICE);
	}
	cam_mac_sink_set_owner(sink, 0);
	CVPixelBufferRelease(frame);
	cam_mac_sink_close(sink);
	cam_mac_sink_free(sink);

	/* the real camera's UID: on a runner it is not there either (printed, not checked: a Mac
	 * with the extension active would find it) */
	sink = cam_mac_sink_new(NULL);
	printf("the FreeRDP Camera device here: %s\n",
	       cam_mac_sink_status_string(cam_mac_sink_open(sink)));
	cam_mac_sink_free(sink);
}

static void on_readers(void* context, bool active)
{
	(void)active;
	(*(int*)context)++;
}

/* on demand, before the extension exists: the watcher runs, finds no device, says "nobody
 * reads" (count unknown), calls nobody back, and stops cleanly with the sink */
static void test_readers_without_extension(void)
{
	camMacSink* sink = cam_mac_sink_new("00000000-0000-0000-0000-000000000000");
	CHECK(sink);
	if (!sink)
		return;
	CHECK(!cam_mac_sink_readers_active(sink));
	CHECK(cam_mac_sink_readers(sink) == -1);
	int calls = 0;
	CHECK(cam_mac_sink_watch_readers(sink, on_readers, &calls));
	CHECK(cam_mac_sink_watch_readers(sink, on_readers, &calls)); /* once per sink: still true */
	usleep(2500 * 1000); /* two polls and more (CAM_MAC_READERS_POLL_MS) */
	CHECK(!cam_mac_sink_readers_active(sink));
	CHECK(cam_mac_sink_readers(sink) == -1);
	CHECK(calls == 0);
	cam_mac_sink_free(sink); /* stops the watcher: no callback after this */
	CHECK(calls == 0);
	CHECK(!cam_mac_sink_readers_active(NULL));
	CHECK(cam_mac_sink_readers(NULL) == -1);
}

int main(void)
{
	@autoreleasepool
	{
		test_pixel_buffers();
		test_sink_without_extension();
		test_readers_without_extension();
	}
	if (g_failures)
		(void)fprintf(stderr, "TestCameraSink: %d check(s) failed\n", g_failures);
	else
		printf("TestCameraSink: all checks passed\n");
	return g_failures ? 1 : 0;
}
