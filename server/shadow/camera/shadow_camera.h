/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a client's camera (MS-RDPECAM) as a camera of the host
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_H
#define FREERDP_SERVER_SHADOW_CAMERA_H

#include <freerdp/server/shadow.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/*
	 * One virtual camera per host, present from server start to server stop. Without a client
	 * camera it shows a still (a camera with a slash). The first client camera announced
	 * (MS-RDPECAM Device Added) owns it; the others are refused with one log line. When the
	 * owner goes, the still comes back and the next camera announced takes it: a client
	 * already connected is never switched to on its own.
	 *
	 * Settings (environment or shadow.conf):
	 *   FREERDP_SHADOW_CAMERA_SINK   v4l2 (Linux default) | v4l2:<device> | file:<dir> | none
	 *   FREERDP_SHADOW_CAMERA_SIZE   <width>x<height>[@<fps>], default 1280x720@30
	 *   FREERDP_SHADOW_CAMERA_FORMAT yuyv (default) | nv12, for the file sink
	 */

	/** Opens the sink and shows the still; FALSE only on an internal error (no sink is no
	 *  error: the server runs without a camera) */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_server_start(rdpShadowServer* server);

	/** Closes the sink; the clients are gone */
	void shadow_camera_server_stop(rdpShadowServer* server);

	/** A client's camera state, from its channels' post connect */
	WINPR_ATTR_NODISCARD BOOL shadow_camera_client_init(rdpShadowClient* client);

	/** Opens the client's camera device enumerator; once, when its dynamic channels are ready */
	void shadow_camera_client_open(rdpShadowClient* client);

	/** The client goes: its cameras are closed, the virtual camera freed if it owned it */
	void shadow_camera_client_uninit(rdpShadowClient* client);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_H */
