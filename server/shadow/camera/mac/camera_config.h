/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS Shadow Server camera: what the camera extension and the shadow side agree on
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

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MAC_CONFIG_H
#define FREERDP_SERVER_SHADOW_CAMERA_MAC_CONFIG_H

/* The camera extension (a CoreMediaIO Camera Extension in FreeRDP-Shadow.app) has one
 * device with two streams: the source the apps on the Mac see, and a sink the shadow side
 * pushes the client's frames into (CMIOStreamCopyBufferQueue + CMSimpleQueueEnqueue, the
 * pattern OBS's virtual camera uses). Both ends find each other by the device's UID below;
 * the bundle ids, the app group and the Mach service come from the build
 * (CAMERA_MAC_BUNDLE_ID, CMakeLists.txt) and live in the Info.plists only. */

/* the device's UID (CMIOExtensionDevice deviceID, kCMIODevicePropertyDeviceUID): fixed, so a
 * new build is the same camera to the apps that remember it */
#define CAM_MAC_DEVICE_UUID "AB9AC9A2-6D08-4DD3-BC75-25A3E66BE765"
#define CAM_MAC_DEVICE_NAME "FreeRDP Camera"
#define CAM_MAC_DEVICE_MODEL "FreeRDP remote camera"
#define CAM_MAC_MANUFACTURER "FreeRDP"
#define CAM_MAC_SOURCE_STREAM_NAME "FreeRDP Camera"
#define CAM_MAC_SINK_STREAM_NAME "FreeRDP Camera Sink"

/* the one format both streams have: NV12, video range ('420v'), 1280x720 at 30 frames per
 * second. Every client frame is converted (raw) or decoded (H.264, MJPG) to it. */
#define CAM_MAC_WIDTH 1280
#define CAM_MAC_HEIGHT 720
#define CAM_MAC_FPS 30
#define CAM_MAC_PIXEL_FORMAT 0x34323076u /* '420v', kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange */

/* the sink's queue (CMIOExtensionStreamProperties sinkBufferQueueSize): a frame being shown
 * and one waiting; more only adds latency */
#define CAM_MAC_SINK_QUEUE 2

/* the extension shows the placeholder (../placeholder.h) when the sink sent nothing for this
 * long: the client's camera stopped, its owner left, or no client has one */
#define CAM_MAC_SINK_IDLE_MS 500

/* the client's sample formats, MS-RDPECAM 2.2.3.1 (CAM_MEDIA_FORMAT in
 * freerdp/channels/rdpecam.h; the values are the protocol's, so they are the same) */
#define CAM_MAC_FORMAT_H264 0x01u
#define CAM_MAC_FORMAT_MJPG 0x02u
#define CAM_MAC_FORMAT_YUY2 0x03u
#define CAM_MAC_FORMAT_NV12 0x04u
#define CAM_MAC_FORMAT_I420 0x05u
#define CAM_MAC_FORMAT_RGB24 0x06u
#define CAM_MAC_FORMAT_RGB32 0x07u

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MAC_CONFIG_H */
