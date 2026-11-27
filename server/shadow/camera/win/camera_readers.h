/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: who reads the Windows virtual camera
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

/*
 * Who reads the Windows virtual camera, decided from two signals:
 *
 * - the virtual camera's own events, which Frame Server sends to the shadow server through
 *   IMFVirtualCamera::Start's callback (SOURCE_START: an app streams; SOURCE_STOP,
 *   SOURCE_UNINITIALIZE, PIPELINE_SHUTDOWN or an error: none does);
 * - the reader count the media source keeps in the ring's readers' section.
 *
 * Any Local Service process may write that section, so its count could keep a remote client's
 * camera on: once the callback delivered an event, the events alone decide and the count is
 * ignored. The count is a fallback only while no event came since the camera was registered.
 */

#ifndef FREERDP_SERVER_SHADOW_CAMERA_READERS_H
#define FREERDP_SERVER_SHADOW_CAMERA_READERS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/** eventsSeen: the callback delivered an event since registration; eventState: 1 from
 *  SOURCE_START until a stop, else 0; ringCount: the ring's reader count */
static inline LONG shadow_camera_win_readers(BOOL eventsSeen, LONG eventState, LONG ringCount)
{
	if (eventsSeen)
		return (eventState > 0) ? 1 : 0;
	return (ringCount > 0) ? ringCount : 0;
}

#endif /* FREERDP_SERVER_SHADOW_CAMERA_READERS_H */
