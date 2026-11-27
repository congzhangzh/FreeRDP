/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Media Foundation camera HAL tests
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

/* Without Media Foundation (Windows N, Windows Server without its feature) the backend registers,
 * offers no camera and says so in one warning: the entry must not fail the dynamic channels. */

#include <stdio.h>

#include "camera_mf_test.h"

int TestCameraMFNoMediaFoundation(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!cam_mf_test_log_start())
		return -1;

	cam_mf_test_fail_load = 1;
	ICamHal* hal = nullptr;
	const char* args[] = { "sys:mf", "file:C:\\camera.mp4" };
	const UINT rc = cam_mf_test_entry(ARRAYSIZE(args), args, &hal);
	cam_mf_test_fail_load = 0;

	int result = -1;
	if ((rc != CHANNEL_RC_OK) || !hal)
	{
		(void)printf("entry: %" PRIu32 ", hal %p: expected CHANNEL_RC_OK and a HAL\n", rc,
		             (void*)hal);
		goto out;
	}

	const UINT count = hal->Enumerate(hal, nullptr, nullptr, nullptr);
	CAM_MEDIA_TYPE_DESCRIPTION types[8] = WINPR_C_ARRAY_INIT;
	size_t nTypes = ARRAYSIZE(types);
	const INT16 index = cam_mf_test_media_types(hal, "RDCamera_Device_0", types, &nTypes);
	CAM_ERROR_CODE activateError = CAM_ERROR_CODE_None;
	const BOOL activated = hal->Activate(hal, "RDCamera_Device_0", &activateError);
	CameraDevice dev = WINPR_C_ARRAY_INIT;
	(void)strncpy(dev.deviceId, "RDCamera_Device_0", sizeof(dev.deviceId) - 1);
	const CAM_ERROR_CODE startError = hal->StartStream(hal, &dev, 0, &types[0], cam_mf_test_sample);

	(void)printf("enumerate %" PRIu32 ", media types %d (%" PRIuz "), activate %d, start %u\n",
	             count, index, nTypes, activated, startError);
	cam_mf_test_log_print();

	if ((count != 0) || (index >= 0) || (nTypes != 0) || activated ||
	    (startError == CAM_ERROR_CODE_None))
		goto out;
	if ((cam_mf_test_log_count() != 1) ||
	    (cam_mf_test_log_find(WLOG_WARN, "Media Foundation not available") != 1))
	{
		(void)printf("expected exactly one log line, a warning\n");
		goto out;
	}
	result = 0;

out:
	if (hal)
		hal->Free(hal);
	return result;
}
