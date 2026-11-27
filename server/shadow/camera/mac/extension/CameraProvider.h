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

#import <Foundation/Foundation.h>
#import <CoreMediaIO/CMIOExtension.h>

NS_ASSUME_NONNULL_BEGIN

/* One device, "FreeRDP Camera", with two streams of NV12 1280x720 at 30 fps (camera_config.h):
 *  - the source, the camera the apps on the Mac see;
 *  - the sink, which the shadow server (or its agent, in the daemon mode) feeds with the
 *    client's frames through CoreMediaIO (camera_sink.h).
 * A frame from the sink goes on to the source as it is. While the sink sends nothing (no
 * client camera, its owner left, or the sink is not started), the source shows the
 * placeholder (../placeholder.h): the camera stays present until the server stops. */
@interface CamMacProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, readonly) CMIOExtensionProvider* provider;
- (instancetype)initWithClientQueue:(nullable dispatch_queue_t)clientQueue;
@end

NS_ASSUME_NONNULL_END
