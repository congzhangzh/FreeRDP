/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the client's camera on demand, and who may own it
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

#include <winpr/synch.h>
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

static int test_debounce(void)
{
	ShadowCameraDemand d = WINPR_C_ARRAY_INIT;
	UINT32 recheck = 0;

	/* nobody reads: off, nothing to wait for */
	CHECK(!shadow_camera_demand_update(&d, 0, 1000, &recheck));
	CHECK(recheck == INFINITE);

	/* a reader: on at once */
	CHECK(shadow_camera_demand_update(&d, 1, 1100, &recheck));
	CHECK(recheck == INFINITE);

	/* the reader goes: still on for the debounce, asked to look again in time */
	CHECK(shadow_camera_demand_update(&d, 0, 2000, &recheck));
	CHECK(recheck == SHADOW_CAMERA_DEMAND_DEBOUNCE_MS);
	CHECK(shadow_camera_demand_update(&d, 0, 2300, &recheck));
	CHECK(recheck == 200);

	/* a brief reopen within the debounce: the camera never stopped */
	CHECK(shadow_camera_demand_update(&d, 1, 2400, &recheck));
	CHECK(recheck == INFINITE);

	/* gone again: the debounce starts over */
	CHECK(shadow_camera_demand_update(&d, 0, 3000, &recheck));
	CHECK(shadow_camera_demand_update(&d, 0, 3499, &recheck));
	CHECK(recheck == 1);
	CHECK(!shadow_camera_demand_update(&d, 0, 3500, &recheck));
	CHECK(recheck == INFINITE);
	CHECK(!shadow_camera_demand_update(&d, 0, 9000, &recheck));

	/* two readers, then one: on all along */
	CHECK(shadow_camera_demand_update(&d, 2, 9100, &recheck));
	CHECK(shadow_camera_demand_update(&d, 1, 9200, &recheck));
	CHECK(recheck == INFINITE);

	/* a sink that can't tell: on, whatever came before */
	ShadowCameraDemand unknown = WINPR_C_ARRAY_INIT;
	for (UINT64 t = 0; t < 10000; t += 700)
	{
		CHECK(shadow_camera_demand_update(&unknown, -1, t, &recheck));
		CHECK(recheck == INFINITE);
	}

	/* a clock that went back is no stop */
	ShadowCameraDemand back = WINPR_C_ARRAY_INIT;
	CHECK(shadow_camera_demand_update(&back, 1, 5000, &recheck));
	CHECK(shadow_camera_demand_update(&back, 0, 5000, &recheck));
	CHECK(shadow_camera_demand_update(&back, 0, 4000, &recheck));
	return 0;
}

static int test_stream_actions(void)
{
	CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_IDLE, TRUE) ==
	      SHADOW_CAMERA_STREAM_START);
	CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_IDLE, FALSE) ==
	      SHADOW_CAMERA_STREAM_NONE);
	CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_STREAMING, FALSE) ==
	      SHADOW_CAMERA_STREAM_STOP);
	CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_STREAMING, TRUE) ==
	      SHADOW_CAMERA_STREAM_NONE);
	/* a request in flight is answered first, whatever the demand */
	for (int wanted = 0; wanted < 2; wanted++)
	{
		CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_STARTING, wanted) ==
		      SHADOW_CAMERA_STREAM_NONE);
		CHECK(shadow_camera_stream_action(SHADOW_CAMERA_STREAM_STOPPING, wanted) ==
		      SHADOW_CAMERA_STREAM_NONE);
	}
	return 0;
}

/* the owner is kept while its stream stops and starts with the readers */
static int test_owner_kept(void)
{
	int a = 0;
	int b = 0;
	ShadowCameraOwner owner = WINPR_C_ARRAY_INIT;
	ShadowCameraDemand d = WINPR_C_ARRAY_INIT;
	SHADOW_CAMERA_STREAM stream = SHADOW_CAMERA_STREAM_IDLE;
	UINT32 recheck = 0;
	size_t starts = 0;
	size_t stops = 0;

	CHECK(shadow_camera_admit(&owner, &a, TRUE, TRUE) == SHADOW_CAMERA_ADMIT_GRANTED);

	/* readers over time: 0, 1, 0 (debounced away), 1, 0 for good */
	const struct
	{
		UINT64 t;
		INT32 readers;
	} steps[] = { { 0, 0 },    { 1000, 1 }, { 2000, 0 }, { 2200, 1 },
		          { 3000, 1 }, { 4000, 0 }, { 4600, 0 }, { 6000, 0 } };
	for (size_t i = 0; i < ARRAYSIZE(steps); i++)
	{
		const BOOL wanted = shadow_camera_demand_update(&d, steps[i].readers, steps[i].t, &recheck);
		switch (shadow_camera_stream_action(stream, wanted))
		{
			case SHADOW_CAMERA_STREAM_START:
				starts++;
				stream = SHADOW_CAMERA_STREAM_STREAMING; /* Start Streams answered */
				break;
			case SHADOW_CAMERA_STREAM_STOP:
				stops++;
				stream = SHADOW_CAMERA_STREAM_IDLE; /* Stop Streams answered */
				break;
			default:
				break;
		}
		/* never released by a stop: no one else can take it */
		CHECK(shadow_camera_owner_is(&owner, &a));
		CHECK(shadow_camera_admit(&owner, &b, TRUE, TRUE) == SHADOW_CAMERA_ADMIT_OWNED);
	}
	CHECK(starts == 1);
	CHECK(stops == 1);
	CHECK(stream == SHADOW_CAMERA_STREAM_IDLE);
	return 0;
}

/* D2: a view-only client's camera is refused, and takes nothing */
static int test_view_only(void)
{
	int a = 0;
	int b = 0;
	ShadowCameraOwner owner = WINPR_C_ARRAY_INIT;

	CHECK(shadow_camera_admit(&owner, &a, FALSE, TRUE) == SHADOW_CAMERA_ADMIT_VIEW_ONLY);
	CHECK(owner.owner == nullptr);
	CHECK(shadow_camera_admit(&owner, &a, TRUE, FALSE) == SHADOW_CAMERA_ADMIT_NO_CAMERA);
	CHECK(owner.owner == nullptr);
	CHECK(shadow_camera_admit(&owner, &b, TRUE, TRUE) == SHADOW_CAMERA_ADMIT_GRANTED);
	/* view-only is said first, even when another owns it */
	CHECK(shadow_camera_admit(&owner, &a, FALSE, TRUE) == SHADOW_CAMERA_ADMIT_VIEW_ONLY);
	CHECK(shadow_camera_admit(&owner, &a, TRUE, TRUE) == SHADOW_CAMERA_ADMIT_OWNED);
	CHECK(shadow_camera_owner_is(&owner, &b));
	return 0;
}

int TestShadowCameraDemand(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	if (test_debounce() != 0)
		return -1;
	if (test_stream_actions() != 0)
		return -1;
	if (test_owner_kept() != 0)
		return -1;
	if (test_view_only() != 0)
		return -1;
	return 0;
}
