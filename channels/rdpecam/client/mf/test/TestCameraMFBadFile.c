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

/* A file:<path> that does not open is not offered, with one warning; the entry succeeds. */

#include <stdio.h>

#include "camera_mf_test.h"

int TestCameraMFBadFile(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (!cam_mf_test_log_start())
		return -1;

	ICamHal* hal = nullptr;
	const char* args[] = { "sys:mf", "file:C:\\does\\not\\exist\\camera.mp4" };
	const UINT rc = cam_mf_test_entry(ARRAYSIZE(args), args, &hal);

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
	(void)printf("enumerate %" PRIu32 ", media types %d (%" PRIuz ")\n", count, index, nTypes);
	cam_mf_test_log_print();

	/* no Media Foundation here at all: that is the other test's case */
	if (cam_mf_test_log_find(WLOG_WARN, "Media Foundation not available") == 1)
	{
		(void)printf("skipped: Media Foundation is not available\n");
		result = CAM_MF_TEST_SKIP;
		goto out;
	}

	if ((count != 0) || (index >= 0) || (nTypes != 0))
		goto out;
	/* a "not found" camera of media type requests (cam_mf_get_device) is not the entry's */
	if ((cam_mf_test_log_find(WLOG_WARN, "can't be opened") != 1) ||
	    (cam_mf_test_log_count() != 1 + cam_mf_test_log_find(WLOG_ERROR, "Unknown camera")))
	{
		(void)printf("expected exactly one warning from the entry\n");
		goto out;
	}
	result = 0;

out:
	if (hal)
		hal->Free(hal);
	return result;
}
