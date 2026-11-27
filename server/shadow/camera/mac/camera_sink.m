/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: pushing a client's frames into the camera extension's sink
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

#include <os/lock.h>
#include <os/log.h>

#import <Foundation/Foundation.h>
#include <CoreMediaIO/CMIOHardware.h>
#include <IOSurface/IOSurfaceRef.h>

#include "camera_config.h"
#include "camera_frame.h"
#include "camera_readers.h"
#include "camera_sink.h"
#include "placeholder.h"

/* how often a closed sink tries to open again while frames come (the extension approved
 * later, or restarted) */
#define CAM_MAC_SINK_RETRY_NS (1000ull * 1000ull * 1000ull)

struct cam_mac_sink
{
	os_unfair_lock lock;
	CFStringRef uid;
	CMIODeviceID device;
	CMIOStreamID stream;
	CMSimpleQueueRef queue;
	BOOL open;
	uint64_t lastTryNs;
	uint64_t owner;
	uint64_t pushed;
	uint64_t dropped;
	/* on demand (cam_mac_sink_watch_readers): the watcher (a CamMacReadersWatcher, retained),
	 * what it saw last, under lock */
	void* watcher;
	int readers;
	BOOL readersActive;
};

static os_log_t sink_log(void)
{
	static os_log_t log;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		log = os_log_create("org.freerdp.shadow.camera", "sink");
	});
	return log;
}

const char* cam_mac_sink_status_string(camMacSinkStatus status)
{
	switch (status)
	{
		case CAM_MAC_SINK_OK:
			return "ok";
		case CAM_MAC_SINK_NO_DEVICE:
			return "no camera extension device (not installed, not approved, or not running)";
		case CAM_MAC_SINK_NO_STREAM:
			return "the camera extension's device has no sink stream";
		case CAM_MAC_SINK_FULL:
			return "the sink queue is full (frame dropped)";
		case CAM_MAC_SINK_BAD_FRAME:
			return "not an NV12 frame of the camera's size";
		case CAM_MAC_SINK_ERROR:
		default:
			return "CoreMediaIO error";
	}
}

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* --- CoreMediaIO: the device, its sink ---------------------------------------------------- */

static CMIOObjectPropertyAddress global_address(CMIOObjectPropertySelector selector)
{
	const CMIOObjectPropertyAddress address = { selector, kCMIOObjectPropertyScopeGlobal,
		                                        kCMIOObjectPropertyElementMain };
	return address;
}

static CMIODeviceID find_device(CFStringRef uid)
{
	CMIOObjectPropertyAddress address = global_address(kCMIOHardwarePropertyDevices);
	UInt32 size = 0;
	if ((CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, NULL, &size) !=
	     kCMIOHardwareNoError) ||
	    (size == 0))
		return kCMIOObjectUnknown;
	const UInt32 count = size / (UInt32)sizeof(CMIODeviceID);
	CMIODeviceID* devices = calloc(count, sizeof(CMIODeviceID));
	if (!devices)
		return kCMIOObjectUnknown;
	UInt32 used = 0;
	CMIODeviceID found = kCMIOObjectUnknown;
	if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, NULL, size, &used,
	                              devices) == kCMIOHardwareNoError)
	{
		for (UInt32 x = 0; (x < used / sizeof(CMIODeviceID)) && (found == kCMIOObjectUnknown);
		     x++)
		{
			CMIOObjectPropertyAddress uidAddress = global_address(kCMIODevicePropertyDeviceUID);
			CFStringRef deviceUID = NULL;
			UInt32 uidUsed = 0;
			if ((CMIOObjectGetPropertyData(devices[x], &uidAddress, 0, NULL, sizeof(deviceUID),
			                               &uidUsed, &deviceUID) == kCMIOHardwareNoError) &&
			    deviceUID)
			{
				if (CFEqual(deviceUID, uid))
					found = devices[x];
				CFRelease(deviceUID);
			}
		}
	}
	free(devices);
	return found;
}

/* the sink: the stream whose direction is output (0; an app's camera stream is input, 1),
 * else the second stream, as the extension adds them (source first, then sink; OBS relies on
 * that order alone) */
static CMIOStreamID find_sink(CMIODeviceID device)
{
	CMIOObjectPropertyAddress address = global_address(kCMIODevicePropertyStreams);
	UInt32 size = 0;
	if ((CMIOObjectGetPropertyDataSize(device, &address, 0, NULL, &size) !=
	     kCMIOHardwareNoError) ||
	    (size < sizeof(CMIOStreamID)))
		return kCMIOObjectUnknown;
	const UInt32 count = size / (UInt32)sizeof(CMIOStreamID);
	CMIOStreamID* streams = calloc(count, sizeof(CMIOStreamID));
	if (!streams)
		return kCMIOObjectUnknown;
	UInt32 used = 0;
	CMIOStreamID found = kCMIOObjectUnknown;
	if (CMIOObjectGetPropertyData(device, &address, 0, NULL, size, &used, streams) ==
	    kCMIOHardwareNoError)
	{
		const UInt32 n = used / (UInt32)sizeof(CMIOStreamID);
		for (UInt32 x = 0; (x < n) && (found == kCMIOObjectUnknown); x++)
		{
			CMIOObjectPropertyAddress dirAddress = global_address(kCMIOStreamPropertyDirection);
			UInt32 direction = 1;
			UInt32 dirUsed = 0;
			if ((CMIOObjectGetPropertyData(streams[x], &dirAddress, 0, NULL, sizeof(direction),
			                               &dirUsed, &direction) == kCMIOHardwareNoError) &&
			    (direction == 0))
				found = streams[x];
		}
		if ((found == kCMIOObjectUnknown) && (n >= 2))
			found = streams[1];
	}
	free(streams);
	return found;
}

static void queue_altered(CMIOStreamID streamID, void* token, void* refCon)
{
	(void)streamID;
	(void)token;
	(void)refCon;
}

static void sink_close_locked(camMacSink* sink)
{
	if (!sink->open)
		return;
	(void)CMIODeviceStopStream(sink->device, sink->stream);
	if (sink->queue)
		CFRelease(sink->queue);
	sink->queue = NULL;
	sink->open = FALSE;
	sink->device = kCMIOObjectUnknown;
	sink->stream = kCMIOObjectUnknown;
}

static camMacSinkStatus sink_open_locked(camMacSink* sink)
{
	if (sink->open)
		return CAM_MAC_SINK_OK;
	sink->lastTryNs = now_ns();

	const CMIODeviceID device = find_device(sink->uid);
	if (device == kCMIOObjectUnknown)
		return CAM_MAC_SINK_NO_DEVICE;
	const CMIOStreamID stream = find_sink(device);
	if (stream == kCMIOObjectUnknown)
		return CAM_MAC_SINK_NO_STREAM;

	CMSimpleQueueRef queue = NULL;
	OSStatus status = CMIOStreamCopyBufferQueue(stream, queue_altered, sink, &queue);
	if ((status != kCMIOHardwareNoError) || !queue)
	{
		os_log_error(sink_log(), "CMIOStreamCopyBufferQueue: %d", (int)status);
		return CAM_MAC_SINK_ERROR;
	}
	status = CMIODeviceStartStream(device, stream);
	if (status != kCMIOHardwareNoError)
	{
		os_log_error(sink_log(), "CMIODeviceStartStream: %d", (int)status);
		CFRelease(queue);
		return CAM_MAC_SINK_ERROR;
	}
	sink->device = device;
	sink->stream = stream;
	sink->queue = queue;
	sink->open = TRUE;
	os_log(sink_log(), "sink open: device %u, stream %u, queue of %d", (unsigned)device,
	       (unsigned)stream, (int)CMSimpleQueueGetCapacity(queue));
	return CAM_MAC_SINK_OK;
}

/* --- on demand: the readers ------------------------------------------------------------- */

static uint64_t now_ms(void)
{
	return now_ns() / 1000000ull;
}

/* the extension's count of its source stream's clients; false when the device has no such
 * property (not ours, or gone) */
static bool read_readers(CMIODeviceID device, int* count)
{
	CMIOObjectPropertyAddress address = global_address(CAM_MAC_READERS_SELECTOR);
	CFStringRef value = NULL;
	UInt32 used = 0;
	if ((CMIOObjectGetPropertyData(device, &address, 0, NULL, sizeof(value), &used, &value) !=
	     kCMIOHardwareNoError) ||
	    !value)
		return false;
	char text[16] = { 0 };
	const bool ok = (CFGetTypeID(value) == CFStringGetTypeID()) &&
	                CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8) &&
	                cam_readers_parse(text, count);
	CFRelease(value);
	return ok;
}

/* The watcher: a CoreMediaIO property listener on the extension's device, AND a poll every
 * CAM_MAC_READERS_POLL_MS. The poll is a permanent part of the design, not a bridge until the
 * listener is proven: it finds the device when the extension is approved or restarted after the
 * server started (a listener needs the device), it reads the count again after a notification
 * that never came (no documentation promises a custom property's notifications reach another
 * process), and it is what ends the hold (camera_readers.h) when no notification comes at all.
 * One property read a second costs nothing next to a camera stream. */
@interface CamMacReadersWatcher : NSObject
- (instancetype)initWithSink:(camMacSink*)sink fn:(camMacReadersFn)fn context:(void*)context;
- (void)stop;
@end

@implementation CamMacReadersWatcher
{
	camMacSink* _sink; /* outlives the watcher: cam_mac_sink_free stops it first */
	camMacReadersFn _fn;
	void* _context;
	dispatch_queue_t _queue;
	dispatch_source_t _timer;
	CMIODeviceID _device;
	CMIOObjectPropertyListenerBlock _listener;
	cam_readers_state _state;
}

- (instancetype)initWithSink:(camMacSink*)sink fn:(camMacReadersFn)fn context:(void*)context
{
	self = [super init];
	if (!self)
		return nil;
	_sink = sink;
	_fn = fn;
	_context = context;
	_device = kCMIOObjectUnknown;
	cam_readers_init(&_state);
	_queue = dispatch_queue_create("org.freerdp.shadow.camera.readers", DISPATCH_QUEUE_SERIAL);
	_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
	if (!_queue || !_timer)
		return nil;
	const uint64_t interval = CAM_MAC_READERS_POLL_MS * NSEC_PER_MSEC;
	dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, interval, interval / 10);
	__weak CamMacReadersWatcher* weakSelf = self;
	dispatch_source_set_event_handler(_timer, ^{
		[weakSelf tick];
	});
	dispatch_resume(_timer);
	return self;
}

- (void)listen
{
	CMIOObjectPropertyAddress address = global_address(CAM_MAC_READERS_SELECTOR);
	__weak CamMacReadersWatcher* weakSelf = self;
	CMIOObjectPropertyListenerBlock listener =
	    ^(UInt32 count, const CMIOObjectPropertyAddress addresses[]) {
		    (void)count;
		    (void)addresses;
		    [weakSelf tick];
	    };
	const OSStatus status = CMIOObjectAddPropertyListenerBlock(_device, &address, _queue, listener);
	if (status == kCMIOHardwareNoError)
		_listener = listener;
	else
		os_log_error(sink_log(), "readers: no listener on device %u (%d): polling only",
		             (unsigned)_device, (int)status);
}

- (void)unlisten
{
	if (_listener && (_device != kCMIOObjectUnknown))
	{
		CMIOObjectPropertyAddress address = global_address(CAM_MAC_READERS_SELECTOR);
		(void)CMIOObjectRemovePropertyListenerBlock(_device, &address, _queue, _listener);
	}
	_listener = nil;
}

/* on _queue: look, publish, report a change */
- (void)tick
{
	if (_device == kCMIOObjectUnknown)
	{
		os_unfair_lock_lock(&_sink->lock);
		CFStringRef uid = (CFStringRef)CFRetain(_sink->uid);
		os_unfair_lock_unlock(&_sink->lock);
		_device = find_device(uid);
		CFRelease(uid);
		if (_device != kCMIOObjectUnknown)
			[self listen];
	}
	int count = -1;
	if ((_device != kCMIOObjectUnknown) && !read_readers(_device, &count))
	{
		[self unlisten];
		_device = kCMIOObjectUnknown;
		count = -1;
	}
	const bool changed = cam_readers_update(&_state, count, now_ms(), CAM_MAC_READERS_HOLD_MS);
	os_unfair_lock_lock(&_sink->lock);
	_sink->readers = count;
	_sink->readersActive = _state.active;
	os_unfair_lock_unlock(&_sink->lock);
	if (changed)
	{
		os_log(sink_log(), "readers: %{public}s (%d app(s))",
		       _state.active ? "an app reads the camera" : "no app reads the camera", count);
		if (_fn)
			_fn(_context, _state.active);
	}
}

- (void)stop
{
	dispatch_source_t timer = _timer;
	_timer = nil;
	if (timer)
		dispatch_source_cancel(timer);
	/* after any tick in flight: no callback once this returns */
	dispatch_sync(_queue, ^{
		[self unlisten];
		self->_fn = NULL;
	});
}

@end

camMacSink* cam_mac_sink_new(const char* deviceUID)
{
	camMacSink* sink = calloc(1, sizeof(camMacSink));
	if (!sink)
		return NULL;
	sink->lock = OS_UNFAIR_LOCK_INIT;
	sink->uid = CFStringCreateWithCString(kCFAllocatorDefault,
	                                      deviceUID ? deviceUID : CAM_MAC_DEVICE_UUID,
	                                      kCFStringEncodingUTF8);
	if (!sink->uid)
	{
		free(sink);
		return NULL;
	}
	sink->device = kCMIOObjectUnknown;
	sink->stream = kCMIOObjectUnknown;
	sink->readers = -1;
	return sink;
}

void cam_mac_sink_free(camMacSink* sink)
{
	if (!sink)
		return;
	if (sink->watcher)
	{
		CamMacReadersWatcher* watcher = (__bridge_transfer CamMacReadersWatcher*)sink->watcher;
		sink->watcher = NULL;
		[watcher stop];
	}
	cam_mac_sink_close(sink);
	os_log(sink_log(), "sink: %llu frame(s) pushed, %llu dropped",
	       (unsigned long long)sink->pushed, (unsigned long long)sink->dropped);
	CFRelease(sink->uid);
	free(sink);
}

camMacSinkStatus cam_mac_sink_open(camMacSink* sink)
{
	if (!sink)
		return CAM_MAC_SINK_ERROR;
	os_unfair_lock_lock(&sink->lock);
	const camMacSinkStatus status = sink_open_locked(sink);
	os_unfair_lock_unlock(&sink->lock);
	return status;
}

void cam_mac_sink_close(camMacSink* sink)
{
	if (!sink)
		return;
	os_unfair_lock_lock(&sink->lock);
	sink_close_locked(sink);
	os_unfair_lock_unlock(&sink->lock);
}

bool cam_mac_sink_is_open(camMacSink* sink)
{
	if (!sink)
		return false;
	os_unfair_lock_lock(&sink->lock);
	const bool open = sink->open;
	os_unfair_lock_unlock(&sink->lock);
	return open;
}

void cam_mac_sink_set_owner(camMacSink* sink, uint64_t owner)
{
	if (!sink)
		return;
	os_unfair_lock_lock(&sink->lock);
	if (owner != sink->owner)
	{
		os_log(sink_log(), "sink owner %llu -> %llu", (unsigned long long)sink->owner,
		       (unsigned long long)owner);
		sink->owner = owner;
		/* the queue holds at most CAM_MAC_SINK_QUEUE frames (66 ms) of the old owner; they are
		 * not taken back (the queue has one consumer, CoreMediaIO's). Time goes on from the
		 * host clock, and the extension marks the cut itself when the sink was idle (the
		 * placeholder in between) */
	}
	os_unfair_lock_unlock(&sink->lock);
}

/* --- frames ------------------------------------------------------------------------------- */

static NSDictionary* nv12_attributes(uint32_t width, uint32_t height)
{
	return @{
		(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(id)kCVPixelBufferWidthKey : @(width),
		(id)kCVPixelBufferHeightKey : @(height),
		(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
	};
}

/* the camera's size comes from a pool: a frame every 33 ms needs no new IOSurface each time */
static CVPixelBufferPoolRef camera_pool(void)
{
	static CVPixelBufferPoolRef pool;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSDictionary* poolAttributes =
		    @{ (id)kCVPixelBufferPoolMinimumBufferCountKey : @(CAM_MAC_SINK_QUEUE + 2) };
		if (CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttributes,
		                            (__bridge CFDictionaryRef)nv12_attributes(CAM_MAC_WIDTH,
		                                                                      CAM_MAC_HEIGHT),
		                            &pool) != kCVReturnSuccess)
			pool = NULL;
	});
	return pool;
}

CVPixelBufferRef cam_mac_pixel_buffer_new(uint32_t width, uint32_t height)
{
	if ((width == 0) || (height == 0) || (width % 2) || (height % 2))
		return NULL;
	CVPixelBufferRef pixels = NULL;
	CVPixelBufferPoolRef pool =
	    ((width == CAM_MAC_WIDTH) && (height == CAM_MAC_HEIGHT)) ? camera_pool() : NULL;
	if (pool)
	{
		if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixels) ==
		    kCVReturnSuccess)
			return pixels;
		pixels = NULL;
	}
	if (CVPixelBufferCreate(kCFAllocatorDefault, width, height,
	                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
	                        (__bridge CFDictionaryRef)nv12_attributes(width, height),
	                        &pixels) != kCVReturnSuccess)
		return NULL;
	return pixels;
}

CVPixelBufferRef cam_mac_pixel_buffer_from_raw(uint32_t format, uint32_t width, uint32_t height,
                                               const uint8_t* data, size_t size)
{
	size_t expected = 0;
	if (!data || !cam_frame_raw_size(format, width, height, &expected) || (expected != size))
		return NULL;
	CVPixelBufferRef pixels = cam_mac_pixel_buffer_new(CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	if (!pixels)
		return NULL;
	CVPixelBufferLockBaseAddress(pixels, 0);
	const bool ok = cam_frame_to_nv12(
	    format, width, height, data, size, CVPixelBufferGetBaseAddressOfPlane(pixels, 0),
	    CVPixelBufferGetBytesPerRowOfPlane(pixels, 0), CVPixelBufferGetBaseAddressOfPlane(pixels, 1),
	    CVPixelBufferGetBytesPerRowOfPlane(pixels, 1), CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CVPixelBufferUnlockBaseAddress(pixels, 0);
	if (!ok)
	{
		CVPixelBufferRelease(pixels);
		return NULL;
	}
	return pixels;
}

CVPixelBufferRef cam_mac_pixel_buffer_placeholder(uint32_t width, uint32_t height)
{
	CVPixelBufferRef pixels = cam_mac_pixel_buffer_new(width, height);
	if (!pixels)
		return NULL;
	CVPixelBufferLockBaseAddress(pixels, 0);
	const bool ok = camera_placeholder_nv12(
	    CVPixelBufferGetBaseAddressOfPlane(pixels, 0), CVPixelBufferGetBytesPerRowOfPlane(pixels, 0),
	    CVPixelBufferGetBaseAddressOfPlane(pixels, 1), CVPixelBufferGetBytesPerRowOfPlane(pixels, 1),
	    width, height);
	CVPixelBufferUnlockBaseAddress(pixels, 0);
	if (!ok)
	{
		CVPixelBufferRelease(pixels);
		return NULL;
	}
	return pixels;
}

CMSampleBufferRef cam_mac_sample_buffer_new(CVPixelBufferRef pixels, CMTime pts)
{
	if (!pixels)
		return NULL;
	CMVideoFormatDescriptionRef format = NULL;
	if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixels, &format) != noErr)
		return NULL;
	const CMSampleTimingInfo timing = { .duration = CMTimeMake(1, CAM_MAC_FPS),
		                                .presentationTimeStamp = pts,
		                                .decodeTimeStamp = kCMTimeInvalid };
	CMSampleBufferRef sample = NULL;
	const OSStatus status = CMSampleBufferCreateReadyWithImageBuffer(
	    kCFAllocatorDefault, pixels, format, &timing, &sample);
	CFRelease(format);
	return (status == noErr) ? sample : NULL;
}

static bool is_camera_frame(CVPixelBufferRef pixels)
{
	return pixels &&
	       (CVPixelBufferGetPixelFormatType(pixels) ==
	        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) &&
	       (CVPixelBufferGetWidth(pixels) == CAM_MAC_WIDTH) &&
	       (CVPixelBufferGetHeight(pixels) == CAM_MAC_HEIGHT);
}

camMacSinkStatus cam_mac_sink_push(camMacSink* sink, CVPixelBufferRef nv12)
{
	if (!sink)
		return CAM_MAC_SINK_ERROR;
	if (!is_camera_frame(nv12))
		return CAM_MAC_SINK_BAD_FRAME;

	os_unfair_lock_lock(&sink->lock);
	camMacSinkStatus status = CAM_MAC_SINK_OK;
	if (!sink->open)
	{
		/* not more than once a second: finding the device asks the CoreMediaIO server */
		status = (now_ns() - sink->lastTryNs >= CAM_MAC_SINK_RETRY_NS) ? sink_open_locked(sink)
		                                                               : CAM_MAC_SINK_NO_DEVICE;
	}
	if (status == CAM_MAC_SINK_OK)
	{
		if (CMSimpleQueueGetCount(sink->queue) >= CMSimpleQueueGetCapacity(sink->queue))
		{
			sink->dropped++;
			status = CAM_MAC_SINK_FULL;
		}
		else
		{
			/* the host clock, never the client's: an owner change keeps time going forward */
			CMSampleBufferRef sample =
			    cam_mac_sample_buffer_new(nv12, CMClockGetTime(CMClockGetHostTimeClock()));
			if (!sample)
				status = CAM_MAC_SINK_ERROR;
			/* the queue takes the reference: CoreMediaIO releases it once consumed */
			else if (CMSimpleQueueEnqueue(sink->queue, sample) != noErr)
			{
				CFRelease(sample);
				sink->dropped++;
				status = CAM_MAC_SINK_FULL;
			}
			else
				sink->pushed++;
		}
	}
	os_unfair_lock_unlock(&sink->lock);
	return status;
}

camMacSinkStatus cam_mac_sink_push_raw(camMacSink* sink, uint32_t format, uint32_t width,
                                       uint32_t height, const uint8_t* data, size_t size)
{
	CVPixelBufferRef pixels = cam_mac_pixel_buffer_from_raw(format, width, height, data, size);
	if (!pixels)
		return CAM_MAC_SINK_BAD_FRAME;
	const camMacSinkStatus status = cam_mac_sink_push(sink, pixels);
	CVPixelBufferRelease(pixels);
	return status;
}

bool cam_mac_sink_watch_readers(camMacSink* sink, camMacReadersFn fn, void* context)
{
	if (!sink)
		return false;
	os_unfair_lock_lock(&sink->lock);
	const bool watching = sink->watcher != NULL;
	os_unfair_lock_unlock(&sink->lock);
	if (watching)
		return true;
	CamMacReadersWatcher* watcher = [[CamMacReadersWatcher alloc] initWithSink:sink
	                                                                        fn:fn
	                                                                   context:context];
	if (!watcher)
		return false;
	os_unfair_lock_lock(&sink->lock);
	sink->watcher = (__bridge_retained void*)watcher;
	os_unfair_lock_unlock(&sink->lock);
	return true;
}

bool cam_mac_sink_readers_active(camMacSink* sink)
{
	if (!sink)
		return false;
	os_unfair_lock_lock(&sink->lock);
	const bool active = sink->readersActive;
	os_unfair_lock_unlock(&sink->lock);
	return active;
}

int cam_mac_sink_readers(camMacSink* sink)
{
	if (!sink)
		return -1;
	os_unfair_lock_lock(&sink->lock);
	const int readers = sink->readers;
	os_unfair_lock_unlock(&sink->lock);
	return readers;
}
