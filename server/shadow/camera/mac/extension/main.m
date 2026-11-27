/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera extension: its process
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

/* The system starts it (registerassistantservice, on the Mach service its Info.plist names)
 * once the user approved the extension; it serves its one device for as long as the system
 * keeps it. */

#import <Foundation/Foundation.h>
#import <CoreMediaIO/CMIOExtension.h>

#import "CameraProvider.h"

int main(int argc, const char* argv[])
{
	(void)argc;
	(void)argv;
	@autoreleasepool
	{
		CamMacProviderSource* source = [[CamMacProviderSource alloc] initWithClientQueue:nil];
		if (!source)
			return 1;
		[CMIOExtensionProvider startServiceWithProvider:source.provider];
		CFRunLoopRun();
	}
	return 0;
}
