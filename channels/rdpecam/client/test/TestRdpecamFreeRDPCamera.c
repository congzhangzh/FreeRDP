/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM client: a FreeRDP server's own camera is not offered back
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

#include <stdio.h>

#include "../camera.h"

int TestRdpecamFreeRDPCamera(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	const struct
	{
		const char* name;
		BOOL own;
	} cases[] = {
		{ "FreeRDP Camera", TRUE },                          /* v4l2loopback card_label, macOS */
		{ "FreeRDP Camera (Windows Virtual Camera)", TRUE }, /* Windows lists it so */
		{ "FreeRDP Camera 2", FALSE },
		{ "FreeRDP Cameras", FALSE },
		{ "My FreeRDP Camera", FALSE },
		{ "freerdp camera", FALSE },
		{ "OBS Virtual Camera", FALSE }, /* a virtual camera shared on purpose */
		{ "Dummy video device (0x0000)", FALSE },
		{ "Integrated Camera", FALSE },
		{ "", FALSE },
		{ nullptr, FALSE },
	};
	int rc = 0;
	for (size_t i = 0; i < ARRAYSIZE(cases); i++)
	{
		if (ecam_is_freerdp_camera(cases[i].name) != cases[i].own)
		{
			(void)fprintf(stderr, "\"%s\": expected %s\n", cases[i].name ? cases[i].name : "(null)",
			              cases[i].own ? "skipped" : "offered");
			rc = -1;
		}
	}
	return rc;
}
