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

/* A capture error while streaming is never silent: the capture thread opens the reader again and
 * the samples come again (MS-RDPECAM 2.2.3.13 wants an answer to each sample request). The
 * camera is a YUY2 AVI written here, played by the source reader as NV12; the 10th sample read
 * of the process fails (CAM_MF_TESTING hook). */

#include <stdio.h>

#include <winpr/file.h>
#include <winpr/path.h>

#include "camera_mf_test.h"

int TestCameraMFCaptureError(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!cam_mf_test_log_start())
		return -1;

	char dir[MAX_PATH] = WINPR_C_ARRAY_INIT;
	char path[MAX_PATH + 64] = WINPR_C_ARRAY_INIT;
	char fileArg[MAX_PATH + 80] = WINPR_C_ARRAY_INIT;
	if (GetTempPathA(sizeof(dir), dir) == 0)
		return -1;
	(void)_snprintf(path, sizeof(path), "%sTestCameraMFCaptureError-%" PRIu32 ".avi", dir,
	                (UINT32)GetCurrentProcessId());
	(void)_snprintf(fileArg, sizeof(fileArg), "file:%s", path);
	if (!cam_mf_test_write_avi(path, 160, 120, 15, 30))
	{
		(void)printf("unable to write %s\n", path);
		return -1;
	}

	ICamHal* hal = nullptr;
	const char* args[] = { "sys:mf", fileArg, "name:Test Camera" };
	const UINT rc = cam_mf_test_entry(ARRAYSIZE(args), args, &hal);

	int result = -1;
	BOOL started = FALSE;
	if ((rc != CHANNEL_RC_OK) || !hal)
		goto out;

	if (cam_mf_test_log_find(WLOG_WARN, "") > 0)
	{
		cam_mf_test_log_print();
		(void)printf("skipped: Media Foundation does not play the generated AVI here\n");
		result = CAM_MF_TEST_SKIP;
		goto out;
	}

	CAM_MEDIA_TYPE_DESCRIPTION types[8] = WINPR_C_ARRAY_INIT;
	size_t nTypes = ARRAYSIZE(types);
	const INT16 index = cam_mf_test_media_types(hal, "RDCamera_Device_0", types, &nTypes);
	if ((index < 0) || (nTypes != 1) || (types[0].Format != CAM_MEDIA_FORMAT_NV12) ||
	    (types[0].Width != 160) || (types[0].Height != 120))
	{
		(void)printf("media types %d (%" PRIuz "): expected NV12 160x120\n", index, nTypes);
		goto out;
	}

	CameraDevice dev = WINPR_C_ARRAY_INIT;
	(void)strncpy(dev.deviceId, "RDCamera_Device_0", sizeof(dev.deviceId) - 1);
	cam_mf_test_fail_read_after = 10;
	const CAM_ERROR_CODE error = hal->StartStream(hal, &dev, 0, &types[0], cam_mf_test_sample);
	if (error != CAM_ERROR_CODE_None)
	{
		(void)printf("start: %u\n", error);
		goto out;
	}
	started = TRUE;

	/* 9 samples, the failure, then the reader again: 30 samples within 15 s */
	const BOOL recovered = cam_mf_test_wait_samples(30, 15000);
	(void)printf("samples: %" PRId32 "%s\n", (INT32)cam_mf_test_samples(),
	             recovered ? "" : " (not recovered)");
	(void)hal->StopStream(hal, "RDCamera_Device_0", 0);
	started = FALSE;
	cam_mf_test_log_print();

	if (!recovered || (cam_mf_test_fail_read_after != 0))
		goto out;
	if ((cam_mf_test_log_find(WLOG_WARN, "ReadSample") != 1) ||
	    (cam_mf_test_log_find(WLOG_INFO, "opened again") < 1))
	{
		(void)printf("expected the failure and the reopening in the log\n");
		goto out;
	}
	result = 0;

out:
	if (hal)
	{
		if (started)
			(void)hal->StopStream(hal, "RDCamera_Device_0", 0);
		hal->Free(hal);
	}
	(void)winpr_DeleteFile(path);
	return result;
}
