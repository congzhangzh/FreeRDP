/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Media Foundation camera HAL tests, shared part
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

#ifndef FREERDP_RDPECAM_MF_TEST_H
#define FREERDP_RDPECAM_MF_TEST_H

#include <winpr/wtypes.h>

#include "camera.h"

/* ctest's skip code (SKIP_RETURN_CODE in CMakeLists.txt) */
#define CAM_MF_TEST_SKIP 77

/* the test hooks of camera_mf.c (CAM_MF_TESTING) */
extern volatile LONG cam_mf_test_fail_load;
extern volatile LONG cam_mf_test_fail_read_after;

UINT VCAPITYPE mf_freerdp_rdpecam_client_subsystem_entry(PFREERDP_CAMERA_HAL_ENTRY_POINTS);

/** the log lines of the backend, at INFO and above, from now on */
BOOL cam_mf_test_log_start(void);
size_t cam_mf_test_log_count(void);
/** @return how many of them are at that level and contain text */
size_t cam_mf_test_log_find(DWORD level, const char* text);
void cam_mf_test_log_print(void);

/** the entry point with the arguments of /dvc:rdpecam,<args...> */
UINT cam_mf_test_entry(int argc, const char* args[], ICamHal** hal);

/** the media types the channel asks for without FFmpeg: each format to itself */
INT16 cam_mf_test_media_types(ICamHal* hal, const char* deviceId, CAM_MEDIA_TYPE_DESCRIPTION* types,
                              size_t* nTypes);

/** the samples the HAL handed over */
UINT cam_mf_test_sample(CameraDevice* dev, size_t streamIndex, const BYTE* sample, size_t size);
LONG cam_mf_test_samples(void);
BOOL cam_mf_test_wait_samples(LONG count, DWORD timeoutMs);

/** writes an uncompressed YUY2 AVI of frames frames, each a different picture */
BOOL cam_mf_test_write_avi(const char* path, UINT32 width, UINT32 height, UINT32 fps,
                           UINT32 frames);

#endif /* FREERDP_RDPECAM_MF_TEST_H */
