/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera extension: the provider, its device and streams
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

#include <time.h>

#include <os/log.h>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Security/Security.h>

#import "CameraProvider.h"

#include "camera_config.h"
#include "placeholder.h"

/* how often the sink is asked for a frame while it is started: four times the frame rate, so
 * a frame waits at most ~8 ms in its queue */
#define CAM_MAC_CONSUME_HZ (4 * CAM_MAC_FPS)

static os_log_t ext_log(void)
{
	static os_log_t log;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		log = os_log_create("org.freerdp.shadow.camera", "extension");
	});
	return log;
}

/* host time in nanoseconds (mach_absolute_time's clock, what CMIO's hostTime means) */
static uint64_t host_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* --- who may feed the sink ------------------------------------------------------------------
 * Only the app this extension belongs to: its server (signed as <app id>) and its agent
 * (<app id>.agent). The extension's own bundle id is <app id>.camera. With a Developer ID the
 * client's code must also be signed by the extension's own team (its pid, checked with
 * Security's code signing API); without one (an ad hoc build, which macOS never activates
 * anyway) only its signing identifier is compared. Fails closed: anything else is refused and
 * logged, so a local app cannot put its own video into the camera. */

static NSString* app_id(void)
{
	NSString* own = [NSBundle mainBundle].bundleIdentifier;
	return [own hasSuffix:@".camera"] ? [own substringToIndex:own.length - 7] : nil;
}

static NSString* own_team_id(void)
{
	static NSString* team;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		SecCodeRef code = NULL;
		SecStaticCodeRef staticCode = NULL;
		CFDictionaryRef info = NULL;
		if ((SecCodeCopySelf(kSecCSDefaultFlags, &code) == errSecSuccess) &&
		    (SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &staticCode) == errSecSuccess) &&
		    (SecCodeCopySigningInformation(staticCode, kSecCSSigningInformation, &info) ==
		     errSecSuccess))
		{
			NSString* value =
			    ((__bridge NSDictionary*)info)[(__bridge NSString*)kSecCodeInfoTeamIdentifier];
			team = [value copy];
		}
		if (info)
			CFRelease(info);
		if (staticCode)
			CFRelease(staticCode);
		if (code)
			CFRelease(code);
	});
	return team;
}

static BOOL client_may_feed(CMIOExtensionClient* client)
{
	NSString* app = app_id();
	NSString* signingID = client.signingID;
	if (!app || !signingID ||
	    (![signingID isEqualToString:app] &&
	     ![signingID isEqualToString:[app stringByAppendingString:@".agent"]]))
	{
		os_log_error(ext_log(), "sink: refused client pid %d, signed as %{public}@",
		             (int)client.pid, signingID ?: @"(nothing)");
		return NO;
	}
	NSString* team = own_team_id();
	if (!team)
	{
		os_log(ext_log(), "sink: client %{public}@ accepted by its identifier (no team: ad hoc)",
		       signingID);
		return YES;
	}

	NSString* text = [NSString
	    stringWithFormat:@"anchor apple generic and certificate leaf[subject.OU] = \"%@\" and "
	                     @"identifier \"%@\"",
	                     team, signingID];
	SecRequirementRef requirement = NULL;
	SecCodeRef guest = NULL;
	OSStatus status =
	    SecRequirementCreateWithString((__bridge CFStringRef)text, kSecCSDefaultFlags, &requirement);
	if (status == errSecSuccess)
	{
		NSDictionary* attributes = @{ (__bridge NSString*)kSecGuestAttributePid : @(client.pid) };
		status = SecCodeCopyGuestWithAttributes(NULL, (__bridge CFDictionaryRef)attributes,
		                                        kSecCSDefaultFlags, &guest);
	}
	if (status == errSecSuccess)
		status = SecCodeCheckValidity(guest, kSecCSDefaultFlags, requirement);
	if (guest)
		CFRelease(guest);
	if (requirement)
		CFRelease(requirement);
	if (status != errSecSuccess)
	{
		os_log_error(ext_log(), "sink: refused client pid %d (%{public}@): %d against %{public}@",
		             (int)client.pid, signingID, (int)status, text);
		return NO;
	}
	return YES;
}

/* --- the device ------------------------------------------------------------------------------ */

@class CamMacStreamSource;

@interface CamMacDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, readonly) CMIOExtensionDevice* device;
- (void)sourceStarted;
- (void)sourceStopped;
- (void)sinkStarted:(CMIOExtensionClient*)client;
- (void)sinkStopped;
@end

@interface CamMacStreamSource : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, readonly) CMIOExtensionStream* stream;
@property(nonatomic, readonly) BOOL sink;
@property(atomic, strong, nullable) CMIOExtensionClient* client;
- (instancetype)initWithDevice:(CamMacDeviceSource*)device
                        format:(CMIOExtensionStreamFormat*)format
                          sink:(BOOL)sink;
@end

@implementation CamMacStreamSource
{
	__weak CamMacDeviceSource* _device;
	CMIOExtensionStreamFormat* _format;
}

- (instancetype)initWithDevice:(CamMacDeviceSource*)device
                        format:(CMIOExtensionStreamFormat*)format
                          sink:(BOOL)sink
{
	self = [super init];
	if (!self)
		return nil;
	_device = device;
	_format = format;
	_sink = sink;
	_stream = [CMIOExtensionStream
	    streamWithLocalizedName:sink ? @CAM_MAC_SINK_STREAM_NAME : @CAM_MAC_SOURCE_STREAM_NAME
	                   streamID:[NSUUID UUID]
	                  direction:sink ? CMIOExtensionStreamDirectionSink
	                                 : CMIOExtensionStreamDirectionSource
	                  clockType:CMIOExtensionStreamClockTypeHostTime
	                     source:self];
	return self;
}

- (NSArray<CMIOExtensionStreamFormat*>*)formats
{
	return @[ _format ];
}

- (NSSet<CMIOExtensionProperty>*)availableProperties
{
	if (_sink)
		return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex,
		                             CMIOExtensionPropertyStreamFrameDuration,
		                             CMIOExtensionPropertyStreamSinkBufferQueueSize,
		                             CMIOExtensionPropertyStreamSinkBuffersRequiredForStartup, nil];
	return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex,
	                             CMIOExtensionPropertyStreamFrameDuration, nil];
}

- (nullable CMIOExtensionStreamProperties*)streamPropertiesForProperties:
                                               (NSSet<CMIOExtensionProperty>*)properties
                                                                   error:(NSError**)outError
{
	(void)outError;
	CMIOExtensionStreamProperties* result =
	    [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
	if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex])
		result.activeFormatIndex = @0;
	if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration])
	{
		/* Objective-C has the frame duration as a CMTime's dictionary */
		CFDictionaryRef duration =
		    CMTimeCopyAsDictionary(CMTimeMake(1, CAM_MAC_FPS), kCFAllocatorDefault);
		result.frameDuration = CFBridgingRelease(duration);
	}
	if (_sink && [properties containsObject:CMIOExtensionPropertyStreamSinkBufferQueueSize])
		result.sinkBufferQueueSize = @(CAM_MAC_SINK_QUEUE);
	if (_sink &&
	    [properties containsObject:CMIOExtensionPropertyStreamSinkBuffersRequiredForStartup])
		result.sinkBuffersRequiredForStartup = @1;
	return result;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties*)streamProperties
                      error:(NSError**)outError
{
	(void)outError;
	/* one format: index 0 is all there is; anything else is ignored, as a fixed camera does */
	if (streamProperties.activeFormatIndex && (streamProperties.activeFormatIndex.intValue != 0))
		os_log(ext_log(), "format %d asked for; there is only 0",
		       streamProperties.activeFormatIndex.intValue);
	return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient*)client
{
	if (!_sink)
		return YES; /* any app may use the camera: macOS asks the user (camera TCC) */
	if (!client_may_feed(client))
		return NO;
	self.client = client;
	return YES;
}

- (BOOL)startStreamAndReturnError:(NSError**)outError
{
	(void)outError;
	CamMacDeviceSource* device = _device;
	if (_sink)
	{
		CMIOExtensionClient* client = self.client;
		if (client)
			[device sinkStarted:client];
	}
	else
		[device sourceStarted];
	return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError**)outError
{
	(void)outError;
	CamMacDeviceSource* device = _device;
	if (_sink)
	{
		[device sinkStopped];
		self.client = nil;
	}
	else
		[device sourceStopped];
	return YES;
}

@end

@implementation CamMacDeviceSource
{
	dispatch_queue_t _queue; /* all the state below lives on it */
	CamMacStreamSource* _source;
	CamMacStreamSource* _sinkSource;
	CVPixelBufferRef _placeholder;
	CMVideoFormatDescriptionRef _description;
	dispatch_source_t _sourceTimer;
	dispatch_source_t _sinkTimer;
	NSInteger _sourceClients;
	CMIOExtensionClient* _sinkClient;
	BOOL _consuming;
	uint64_t _lastSinkNs;
	BOOL _showingPlaceholder;
	uint64_t _sinkFrames;
}

static CVPixelBufferRef make_placeholder(void)
{
	NSDictionary* attributes = @{ (id)kCVPixelBufferIOSurfacePropertiesKey : @{} };
	CVPixelBufferRef pixels = NULL;
	if (CVPixelBufferCreate(kCFAllocatorDefault, CAM_MAC_WIDTH, CAM_MAC_HEIGHT,
	                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
	                        (__bridge CFDictionaryRef)attributes, &pixels) != kCVReturnSuccess)
		return NULL;
	CVPixelBufferLockBaseAddress(pixels, 0);
	const bool ok = camera_placeholder_nv12(
	    CVPixelBufferGetBaseAddressOfPlane(pixels, 0), CVPixelBufferGetBytesPerRowOfPlane(pixels, 0),
	    CVPixelBufferGetBaseAddressOfPlane(pixels, 1), CVPixelBufferGetBytesPerRowOfPlane(pixels, 1),
	    CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
	CVPixelBufferUnlockBaseAddress(pixels, 0);
	if (!ok)
	{
		CVPixelBufferRelease(pixels);
		return NULL;
	}
	return pixels;
}

- (instancetype)init
{
	self = [super init];
	if (!self)
		return nil;
	_queue = dispatch_queue_create("org.freerdp.shadow.camera.device", DISPATCH_QUEUE_SERIAL);
	_placeholder = make_placeholder();
	if (!_placeholder ||
	    (CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
	                                    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
	                                    CAM_MAC_WIDTH, CAM_MAC_HEIGHT, NULL,
	                                    &_description) != noErr))
	{
		os_log_fault(ext_log(), "no placeholder or format description");
		return nil;
	}
	const CMTime frame = CMTimeMake(1, CAM_MAC_FPS);
	CMIOExtensionStreamFormat* format =
	    [CMIOExtensionStreamFormat streamFormatWithFormatDescription:_description
	                                                maxFrameDuration:frame
	                                                minFrameDuration:frame
	                                             validFrameDurations:nil];

	NSUUID* deviceID = [[NSUUID alloc] initWithUUIDString:@CAM_MAC_DEVICE_UUID];
	_device = [CMIOExtensionDevice deviceWithLocalizedName:@CAM_MAC_DEVICE_NAME
	                                              deviceID:deviceID
	                                        legacyDeviceID:nil
	                                                source:self];
	_source = [[CamMacStreamSource alloc] initWithDevice:self format:format sink:NO];
	_sinkSource = [[CamMacStreamSource alloc] initWithDevice:self format:format sink:YES];
	NSError* error = nil;
	/* the source first, then the sink: the order camera_sink.m falls back on */
	if (![_device addStream:_source.stream error:&error] ||
	    ![_device addStream:_sinkSource.stream error:&error])
	{
		os_log_fault(ext_log(), "addStream: %{public}@", error);
		return nil;
	}
	return self;
}

- (void)dealloc
{
	if (_placeholder)
		CVPixelBufferRelease(_placeholder);
	if (_description)
		CFRelease(_description);
}

- (NSSet<CMIOExtensionProperty>*)availableProperties
{
	return [NSSet setWithObjects:CMIOExtensionPropertyDeviceTransportType,
	                             CMIOExtensionPropertyDeviceModel, @CAM_MAC_READERS_PROPERTY, nil];
}

/* on demand: how many clients read the source stream now (camera_config.h, CAM_MAC_READERS_*),
 * as the decimal string the shadow side parses */
- (CMIOExtensionPropertyState*)readersState
{
	__block NSInteger readers = 0;
	dispatch_sync(_queue, ^{
		readers = self->_sourceClients;
	});
	return [CMIOExtensionPropertyState propertyStateWithValue:[@(readers) stringValue]];
}

/* on _queue, after the count changed: the watchers hear it at once (they poll too) */
- (void)notifyReaders
{
	CMIOExtensionPropertyState* state =
	    [CMIOExtensionPropertyState propertyStateWithValue:[@(_sourceClients) stringValue]];
	[_device notifyPropertiesChanged:@{ @CAM_MAC_READERS_PROPERTY : state }];
	os_log(ext_log(), "readers: %ld", (long)_sourceClients);
}

- (nullable CMIOExtensionDeviceProperties*)devicePropertiesForProperties:
                                               (NSSet<CMIOExtensionProperty>*)properties
                                                                   error:(NSError**)outError
{
	(void)outError;
	CMIOExtensionDeviceProperties* result =
	    [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
	if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType])
		result.transportType = @('virt'); /* kIOAudioDeviceTransportTypeVirtual */
	if ([properties containsObject:CMIOExtensionPropertyDeviceModel])
		result.model = @CAM_MAC_DEVICE_MODEL;
	if ([properties containsObject:@CAM_MAC_READERS_PROPERTY])
		[result setPropertyState:[self readersState] forProperty:@CAM_MAC_READERS_PROPERTY];
	return result;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties*)deviceProperties
                      error:(NSError**)outError
{
	(void)deviceProperties;
	(void)outError;
	return YES;
}

/* the source's clock: the placeholder while the sink is idle */
- (void)sourceTick
{
	if (_sourceClients <= 0)
		return;
	const uint64_t now = host_ns();
	if (_lastSinkNs && (now - _lastSinkNs < CAM_MAC_SINK_IDLE_MS * 1000000ull))
		return; /* the sink feeds the source */

	CMIOExtensionStreamDiscontinuityFlags flags = CMIOExtensionStreamDiscontinuityFlagNone;
	if (!_showingPlaceholder)
	{
		flags = CMIOExtensionStreamDiscontinuityFlagTime;
		_showingPlaceholder = YES;
		os_log(ext_log(), "no frames from the sink: the placeholder");
	}
	const CMSampleTimingInfo timing = { .duration = CMTimeMake(1, CAM_MAC_FPS),
		                                .presentationTimeStamp =
		                                    CMClockGetTime(CMClockGetHostTimeClock()),
		                                .decodeTimeStamp = kCMTimeInvalid };
	CMSampleBufferRef sample = NULL;
	if (CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, _placeholder, _description,
	                                             &timing, &sample) != noErr)
		return;
	[_source.stream sendSampleBuffer:sample discontinuity:flags hostTimeInNanoseconds:now];
	CFRelease(sample);
}

/* a frame from the sink: on to the source as it is */
- (void)sinkFrame:(CMSampleBufferRef)sample
         sequence:(uint64_t)sequence
    discontinuity:(CMIOExtensionStreamDiscontinuityFlags)discontinuity
{
	const uint64_t now = host_ns();
	CMIOExtensionStreamDiscontinuityFlags flags = discontinuity;
	if (_showingPlaceholder || (_lastSinkNs == 0))
	{
		flags |= CMIOExtensionStreamDiscontinuityFlagTime;
		_showingPlaceholder = NO;
		os_log(ext_log(), "frames from the sink");
	}
	_lastSinkNs = now;
	_sinkFrames++;
	if (_sourceClients > 0)
		[_source.stream sendSampleBuffer:sample discontinuity:flags hostTimeInNanoseconds:now];
	[_sinkSource.stream
	    notifyScheduledOutputChanged:[CMIOExtensionScheduledOutput
	                                     scheduledOutputWithSequenceNumber:sequence
	                                                 hostTimeInNanoseconds:now]];
}

- (void)consume
{
	CMIOExtensionClient* client = _sinkClient;
	if (!client || _consuming)
		return;
	_consuming = YES;
	__weak CamMacDeviceSource* weakSelf = self;
	[_sinkSource.stream
	    consumeSampleBufferFromClient:client
	                completionHandler:^(CMSampleBufferRef _Nullable sample, uint64_t sequence,
	                                    CMIOExtensionStreamDiscontinuityFlags discontinuity,
	                                    BOOL hasMore, NSError* _Nullable error) {
		                (void)error;
		                if (sample)
			                CFRetain(sample);
		                CamMacDeviceSource* strongSelf = weakSelf;
		                if (!strongSelf)
		                {
			                if (sample)
				                CFRelease(sample);
			                return;
		                }
		                dispatch_async(strongSelf->_queue, ^{
			                strongSelf->_consuming = NO;
			                if (sample)
			                {
				                [strongSelf sinkFrame:sample
				                             sequence:sequence
				                        discontinuity:discontinuity];
				                CFRelease(sample);
			                }
			                if (hasMore)
				                [strongSelf consume];
		                });
	                }];
}

static dispatch_source_t start_timer(dispatch_queue_t queue, uint64_t hz, dispatch_block_t tick)
{
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	const uint64_t interval = NSEC_PER_SEC / hz;
	dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, interval, interval / 10);
	dispatch_source_set_event_handler(timer, tick);
	dispatch_resume(timer);
	return timer;
}

- (void)sourceStarted
{
	dispatch_async(_queue, ^{
		if (self->_sourceClients++ == 0)
		{
			__weak CamMacDeviceSource* weakSelf = self;
			self->_showingPlaceholder = NO;
			self->_sourceTimer = start_timer(self->_queue, CAM_MAC_FPS, ^{
				[weakSelf sourceTick];
			});
			os_log(ext_log(), "the camera is in use");
		}
		[self notifyReaders];
	});
}

- (void)sourceStopped
{
	dispatch_async(_queue, ^{
		if ((self->_sourceClients > 0) && (--self->_sourceClients == 0) && self->_sourceTimer)
		{
			dispatch_source_cancel(self->_sourceTimer);
			self->_sourceTimer = nil;
			os_log(ext_log(), "the camera is no longer in use");
		}
		[self notifyReaders];
	});
}

- (void)sinkStarted:(CMIOExtensionClient*)client
{
	dispatch_async(_queue, ^{
		self->_sinkClient = client;
		self->_consuming = NO;
		if (!self->_sinkTimer)
		{
			__weak CamMacDeviceSource* weakSelf = self;
			self->_sinkTimer = start_timer(self->_queue, CAM_MAC_CONSUME_HZ, ^{
				[weakSelf consume];
			});
		}
		os_log(ext_log(), "sink started by pid %d (%{public}@)", (int)client.pid,
		       client.signingID ?: @"?");
	});
}

- (void)sinkStopped
{
	dispatch_async(_queue, ^{
		if (self->_sinkTimer)
		{
			dispatch_source_cancel(self->_sinkTimer);
			self->_sinkTimer = nil;
		}
		self->_sinkClient = nil;
		self->_lastSinkNs = 0; /* the placeholder at the next tick */
		os_log(ext_log(), "sink stopped after %llu frame(s)",
		       (unsigned long long)self->_sinkFrames);
	});
}

@end

/* --- the provider ---------------------------------------------------------------------------- */

@implementation CamMacProviderSource
{
	CamMacDeviceSource* _deviceSource;
}

- (instancetype)initWithClientQueue:(nullable dispatch_queue_t)clientQueue
{
	self = [super init];
	if (!self)
		return nil;
	_provider = [CMIOExtensionProvider providerWithSource:self clientQueue:clientQueue];
	_deviceSource = [CamMacDeviceSource new];
	NSError* error = nil;
	if (!_deviceSource || ![_provider addDevice:_deviceSource.device error:&error])
	{
		os_log_fault(ext_log(), "addDevice: %{public}@", error);
		return nil;
	}
	return self;
}

- (BOOL)connectClient:(CMIOExtensionClient*)client error:(NSError**)outError
{
	(void)client;
	(void)outError;
	return YES;
}

- (void)disconnectClient:(CMIOExtensionClient*)client
{
	(void)client;
}

- (NSSet<CMIOExtensionProperty>*)availableProperties
{
	return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer,
	                             CMIOExtensionPropertyProviderName, nil];
}

- (nullable CMIOExtensionProviderProperties*)providerPropertiesForProperties:
                                                 (NSSet<CMIOExtensionProperty>*)properties
                                                                       error:(NSError**)outError
{
	(void)outError;
	CMIOExtensionProviderProperties* result =
	    [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
	if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer])
		result.manufacturer = @CAM_MAC_MANUFACTURER;
	if ([properties containsObject:CMIOExtensionPropertyProviderName])
		result.name = @CAM_MAC_DEVICE_NAME;
	return result;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties*)providerProperties
                        error:(NSError**)outError
{
	(void)providerProperties;
	(void)outError;
	return YES;
}

@end
