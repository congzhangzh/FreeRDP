/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: installing the camera extension, its state, a test pattern
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

#include "camera_activate.h"
#include "camera_config.h"
#include "camera_sink.h"

/* how long activate waits for the user's approval before it leaves (the approval still
 * completes later: sysextd keeps the request) */
#define CAM_MAC_ACTIVATE_WAIT_S 120

enum
{
	CAM_EXIT_OK = 0,
	CAM_EXIT_FAILED = 1,
	CAM_EXIT_APPROVAL = 2,
	CAM_EXIT_USAGE = 64
};

@interface CamMacRequestDelegate : NSObject <OSSystemExtensionRequestDelegate>
@property(atomic) BOOL done;
@property(atomic) int exitCode;
@end

@implementation CamMacRequestDelegate

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest*)request
                  actionForReplacingExtension:(OSSystemExtensionProperties*)existing
                                withExtension:(OSSystemExtensionProperties*)extension
{
	(void)request;
	printf("replacing the installed camera extension %s (%s) with %s (%s)\n",
	       existing.bundleShortVersion.UTF8String, existing.bundleVersion.UTF8String,
	       extension.bundleShortVersion.UTF8String, extension.bundleVersion.UTF8String);
	return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest*)request
{
	(void)request;
	printf("waiting for your approval: System Settings > General > Login Items & Extensions > "
	       "Camera Extensions (on macOS 14 and older: Privacy & Security), allow FreeRDP "
	       "Shadow\n");
	fflush(stdout);
	self.exitCode = CAM_EXIT_APPROVAL;
}

- (void)request:(OSSystemExtensionRequest*)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result
{
	(void)request;
	if (result == OSSystemExtensionRequestWillCompleteAfterReboot)
		printf("done after the next restart\n");
	else
		printf("done\n");
	self.exitCode = CAM_EXIT_OK;
	self.done = YES;
}

- (void)request:(OSSystemExtensionRequest*)request didFailWithError:(NSError*)error
{
	(void)request;
	fprintf(stderr, "failed: %s (%s %ld)\n", error.localizedDescription.UTF8String,
	        error.domain.UTF8String, (long)error.code);
	self.exitCode = CAM_EXIT_FAILED;
	self.done = YES;
}

- (void)request:(OSSystemExtensionRequest*)request
    foundProperties:(NSArray<OSSystemExtensionProperties*>*)properties
{
	(void)request;
	if (properties.count == 0)
		printf("extension: not installed\n");
	for (OSSystemExtensionProperties* p in properties)
		printf("extension: %s %s (%s)%s%s%s\n", p.bundleIdentifier.UTF8String,
		       p.bundleShortVersion.UTF8String, p.bundleVersion.UTF8String,
		       p.isEnabled ? " enabled" : " not enabled",
		       p.isAwaitingUserApproval ? ", awaiting your approval" : "",
		       p.isUninstalling ? ", uninstalling" : "");
	self.exitCode = CAM_EXIT_OK;
	self.done = YES;
}

@end

/* the extension the app carries: <app id>.camera */
static NSString* extension_id(void)
{
	NSString* app = [NSBundle mainBundle].bundleIdentifier;
	if (!app)
	{
		fprintf(stderr, "not run from an app: start it as "
		                "/Applications/FreeRDP-Shadow.app/Contents/MacOS/freerdp-shadow-cli "
		                "--camera-extension <command>\n");
		return nil;
	}
	if (![[NSBundle mainBundle].bundlePath hasPrefix:@"/Applications/"])
		fprintf(stderr, "warning: %s is not in /Applications: macOS installs a system "
		                "extension only from an app there\n",
		        [NSBundle mainBundle].bundlePath.UTF8String);
	return [app stringByAppendingString:@".camera"];
}

static int run_request(OSSystemExtensionRequest* request, CamMacRequestDelegate* delegate,
                       int waitSeconds)
{
	request.delegate = delegate;
	[[OSSystemExtensionManager sharedManager] submitRequest:request];
	NSDate* until = [NSDate dateWithTimeIntervalSinceNow:waitSeconds];
	while (!delegate.done && ([until timeIntervalSinceNow] > 0))
		[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
		                      beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
	if (!delegate.done && (delegate.exitCode != CAM_EXIT_APPROVAL))
	{
		fprintf(stderr, "no answer from sysextd in %d s\n", waitSeconds);
		return CAM_EXIT_FAILED;
	}
	return delegate.exitCode;
}

static int camera_activate(bool activate)
{
	NSString* identifier = extension_id();
	if (!identifier)
		return CAM_EXIT_FAILED;
	printf("%s %s\n", activate ? "activating" : "deactivating", identifier.UTF8String);
	CamMacRequestDelegate* delegate = [CamMacRequestDelegate new];
	OSSystemExtensionRequest* request =
	    activate ? [OSSystemExtensionRequest
	                   activationRequestForExtension:identifier
	                                           queue:dispatch_get_main_queue()]
	             : [OSSystemExtensionRequest
	                   deactivationRequestForExtension:identifier
	                                             queue:dispatch_get_main_queue()];
	return run_request(request, delegate, CAM_MAC_ACTIVATE_WAIT_S);
}

static int camera_status(void)
{
	int rc = CAM_EXIT_OK;
	NSString* identifier = extension_id();
	if (identifier)
	{
		CamMacRequestDelegate* delegate = [CamMacRequestDelegate new];
		OSSystemExtensionRequest* request =
		    [OSSystemExtensionRequest propertiesRequestForExtension:identifier
		                                                      queue:dispatch_get_main_queue()];
		rc = run_request(request, delegate, 20);
	}
	/* the device as the apps see it, and its sink */
	camMacSink* sink = cam_mac_sink_new(NULL);
	if (!sink)
		return CAM_EXIT_FAILED;
	const camMacSinkStatus status = cam_mac_sink_open(sink);
	printf("device %s: %s\n", CAM_MAC_DEVICE_UUID, cam_mac_sink_status_string(status));
	cam_mac_sink_free(sink);
	return (status == CAM_MAC_SINK_OK) ? rc : CAM_EXIT_FAILED;
}

/* the placeholder with a white bar running across it, 30 times a second */
static int camera_pattern(int seconds)
{
	camMacSink* sink = cam_mac_sink_new(NULL);
	if (!sink)
		return CAM_EXIT_FAILED;
	camMacSinkStatus status = cam_mac_sink_open(sink);
	if (status != CAM_MAC_SINK_OK)
	{
		fprintf(stderr, "%s\n", cam_mac_sink_status_string(status));
		cam_mac_sink_free(sink);
		return CAM_EXIT_FAILED;
	}
	cam_mac_sink_set_owner(sink, 1);
	const int frames = seconds * CAM_MAC_FPS;
	int pushed = 0;
	for (int n = 0; n < frames; n++)
	{
		CVPixelBufferRef pixels = cam_mac_pixel_buffer_placeholder(CAM_MAC_WIDTH, CAM_MAC_HEIGHT);
		if (!pixels)
			break;
		CVPixelBufferLockBaseAddress(pixels, 0);
		uint8_t* y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
		const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
		const size_t x0 = (size_t)(n * 16) % (CAM_MAC_WIDTH - 40);
		for (size_t row = 0; row < CAM_MAC_HEIGHT; row++)
			memset(&y[row * stride + x0], 235, 40);
		CVPixelBufferUnlockBaseAddress(pixels, 0);
		status = cam_mac_sink_push(sink, pixels);
		CVPixelBufferRelease(pixels);
		if (status == CAM_MAC_SINK_OK)
			pushed++;
		if (n % CAM_MAC_FPS == 0)
		{
			printf("%d s: %d frame(s) pushed, last: %s\n", n / CAM_MAC_FPS, pushed,
			       cam_mac_sink_status_string(status));
			fflush(stdout);
		}
		usleep(1000000 / CAM_MAC_FPS);
	}
	cam_mac_sink_set_owner(sink, 0);
	cam_mac_sink_free(sink);
	printf("%d of %d frame(s) pushed\n", pushed, frames);
	return (pushed > 0) ? CAM_EXIT_OK : CAM_EXIT_FAILED;
}

int cam_mac_camera_main(int argc, char** argv)
{
	@autoreleasepool
	{
		const char* command = (argc > 1) ? argv[1] : "";
		if (strcmp(command, "activate") == 0)
			return camera_activate(true);
		if (strcmp(command, "deactivate") == 0)
			return camera_activate(false);
		if (strcmp(command, "status") == 0)
			return camera_status();
		if (strcmp(command, "pattern") == 0)
		{
			const int seconds = (argc > 2) ? atoi(argv[2]) : 30;
			return camera_pattern((seconds > 0) ? seconds : 30);
		}
		fprintf(stderr, "usage: %s activate | deactivate | status | pattern [seconds]\n",
		        (argc > 0) ? argv[0] : "--camera-extension");
		return CAM_EXIT_USAGE;
	}
}
