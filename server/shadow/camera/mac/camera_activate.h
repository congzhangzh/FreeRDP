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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_ACTIVATE_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_ACTIVATE_H

/* A camera extension is installed by the app that carries it
 * (Contents/Library/SystemExtensions/<app id>.camera.systemextension), from /Applications,
 * with an OSSystemExtensionRequest; the user approves it once in System Settings. The request
 * must come from the app's main executable: it holds the restricted entitlement
 * com.apple.developer.system-extension.install, which needs the app's embedded provisioning
 * profile. So freerdp-shadow-cli (the app's CFBundleExecutable) runs this when its first
 * argument is --camera-extension:
 *
 *   FreeRDP-Shadow.app/Contents/MacOS/freerdp-shadow-cli --camera-extension activate
 *   ... --camera-extension deactivate
 *   ... --camera-extension status       the extension's state, and whether its device is seen
 *   ... --camera-extension pattern [s]  pushes a moving test picture into the camera for s
 *                                       seconds (30 by default), to see it in an app
 */

#ifdef __cplusplus
extern "C"
{
#endif

	/* argv[0] is the --camera-extension word (or the tool's name), argv[1] the command; the
	 * process's exit code: 0 done, 1 failed, 2 waits for the user's approval, 64 usage */
	int cam_mac_camera_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_ACTIVATE_H */
