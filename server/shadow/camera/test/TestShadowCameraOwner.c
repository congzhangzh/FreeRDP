/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: one camera owner per host
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

#include <winpr/wtypes.h>

#include "camera_policy.h"

#define CHECK(cond)                                                          \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
		{                                                                    \
			(void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return -1;                                                       \
		}                                                                    \
	} while (0)

int TestShadowCameraOwner(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	/* the cameras of three clients: A connects first, then B, both with a camera; C later */
	int a = 0;
	int b = 0;
	int c = 0;
	ShadowCameraOwner owner = WINPR_C_ARRAY_INIT;

	/* the first camera wins, the second is refused */
	CHECK(shadow_camera_owner_claim(&owner, &a) == SHADOW_CAMERA_CLAIM_GRANTED);
	CHECK(shadow_camera_owner_claim(&owner, &b) == SHADOW_CAMERA_CLAIM_REFUSED);
	CHECK(shadow_camera_owner_is(&owner, &a));
	CHECK(!shadow_camera_owner_is(&owner, &b));
	CHECK(shadow_camera_owner_claim(&owner, &a) == SHADOW_CAMERA_CLAIM_GRANTED);

	/* a camera that does not own it cannot release it */
	CHECK(!shadow_camera_owner_release(&owner, &b));
	CHECK(shadow_camera_owner_is(&owner, &a));

	/* the owner leaves: the camera is free, B (connected all along) is not switched to */
	CHECK(shadow_camera_owner_release(&owner, &a));
	CHECK(!shadow_camera_owner_release(&owner, &a));
	CHECK(owner.owner == nullptr);
	CHECK(!shadow_camera_owner_is(&owner, &b));

	/* the next camera that starts takes it: C, which connects now */
	CHECK(shadow_camera_owner_claim(&owner, &c) == SHADOW_CAMERA_CLAIM_GRANTED);
	CHECK(shadow_camera_owner_claim(&owner, &b) == SHADOW_CAMERA_CLAIM_REFUSED);

	/* C leaves, then B starts its camera again (Device Added): B takes it */
	CHECK(shadow_camera_owner_release(&owner, &c));
	CHECK(shadow_camera_owner_claim(&owner, &b) == SHADOW_CAMERA_CLAIM_GRANTED);
	CHECK(shadow_camera_owner_claim(&owner, &a) == SHADOW_CAMERA_CLAIM_REFUSED);
	CHECK(!shadow_camera_owner_release(&owner, nullptr));
	return 0;
}
