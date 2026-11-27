/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: the extension counts the readers of its source stream (on demand)
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

/* The extension's own objects in this process (no activation, no CoreMediaIO service): its
 * provider, its device and the two streams. The source stream is started and stopped as apps
 * do, and the device's readers property (CAM_MAC_READERS_PROPERTY, what the shadow side
 * watches) must follow: 0, 1, 2, 1, 0, never below 0. CoreMediaIO's objects may refuse to work
 * outside an extension's service; the checks run in a child process, so that is a SKIP (77),
 * not a crash of the test run. */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <CoreMediaIO/CMIOExtension.h>

#import "CameraProvider.h"

#include "camera_config.h"

static int g_failures = 0;

#define CHECK(cond)                                                               \
	do                                                                            \
	{                                                                             \
		if (!(cond))                                                              \
		{                                                                         \
			(void)fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                                         \
		}                                                                         \
	} while (0)

static NSString *readers(id<CMIOExtensionDeviceSource> device)
{
	NSString *key = @CAM_MAC_READERS_PROPERTY;
	NSError *error = nil;
	CMIOExtensionDeviceProperties *properties =
	    [device devicePropertiesForProperties:[NSSet setWithObject:key] error:&error];
	id value = properties.propertiesDictionary[key].value;
	return [value isKindOfClass:[NSString class]] ? value : @"(not a string)";
}

static int run(void)
{
	@autoreleasepool
	{
		CamMacProviderSource *provider = [[CamMacProviderSource alloc] initWithClientQueue:nil];
		CHECK(provider);
		if (!provider)
			return 1;
		CMIOExtensionDevice *device = provider.provider.devices.firstObject;
		CHECK(device);
		id<CMIOExtensionDeviceSource> source = device.source;
		CHECK([source.availableProperties containsObject:@CAM_MAC_READERS_PROPERTY]);

		CMIOExtensionStream *camera = nil;
		CMIOExtensionStream *sink = nil;
		for (CMIOExtensionStream *stream in device.streams)
		{
			if (stream.direction == CMIOExtensionStreamDirectionSource)
				camera = stream;
			else if (stream.direction == CMIOExtensionStreamDirectionSink)
				sink = stream;
		}
		CHECK(camera && sink);
		CHECK((device.streams.count == 2) && (device.streams.firstObject == camera));
		if (!camera)
			return 1;

		NSError *error = nil;
		printf("readers before: %s\n", readers(source).UTF8String);
		CHECK([readers(source) isEqualToString:@"0"]);
		CHECK([camera.source startStreamAndReturnError:&error]); /* an app opens the camera */
		CHECK([readers(source) isEqualToString:@"1"]);
		CHECK([camera.source startStreamAndReturnError:&error]); /* a second one */
		CHECK([readers(source) isEqualToString:@"2"]);
		CHECK([camera.source stopStreamAndReturnError:&error]);
		CHECK([readers(source) isEqualToString:@"1"]);
		CHECK([camera.source stopStreamAndReturnError:&error]);
		CHECK([readers(source) isEqualToString:@"0"]);
		CHECK([camera.source stopStreamAndReturnError:&error]); /* one stop too many */
		CHECK([readers(source) isEqualToString:@"0"]);
		printf("readers after: %s\n", readers(source).UTF8String);
	}
	return g_failures ? 1 : 0;
}

int main(void)
{
	const pid_t pid = fork();
	if (pid < 0)
		return 1;
	if (pid == 0)
		_exit(run());
	int status = 0;
	if (waitpid(pid, &status, 0) != pid)
		return 1;
	if (WIFSIGNALED(status))
	{
		printf("SKIP: the extension's objects need CoreMediaIO's extension service here "
		       "(the child ended on signal %d)\n",
		       WTERMSIG(status));
		return 77;
	}
	const int rc = WEXITSTATUS(status);
	printf("TestCameraExtension: %s\n", rc ? "FAILED" : "all checks passed");
	return rc;
}
