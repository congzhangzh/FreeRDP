/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: who reads the virtual camera, the events over the ring's count
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
 *   TestShadowCameraWin TestShadowCameraWinReaders
 *
 * The decision of sink_win.c's Readers() (camera_readers.h) on a real ring's count: a Local\ ring
 * of its own, one reader counted in it (any Local Service process could write that count), and
 *   events seen, stopped (0), ring 1  -> 0: the count is ignored once an event came
 *   no event yet,           ring 1  -> 1: the count is the fallback
 *   events seen, started (1), ring 0 -> 1
 *   no event yet,           ring 0  -> 0
 */

#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../camera_readers.h"
#include "../camera_ring.h"

static BOOL expect(const char* what, LONG got, LONG want)
{
	if (got == want)
		return TRUE;
	fprintf(stderr, "%s: %ld, not %ld\n", what, got, want);
	return FALSE;
}

int TestShadowCameraWinReaders(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	char prefix[64] = { 0 };
	(void)_snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "Local\\TestShadowCameraReaders-%lu",
	                  GetCurrentProcessId());
	(void)SetEnvironmentVariableA(SHADOW_CAMERA_RING_NAME_ENV, prefix);

	int rc = -1;
	DWORD error = 0;
	ShadowCameraRing* writer = shadow_camera_ring_create(64, 64, 30, &error);
	ShadowCameraRing* reader = writer ? shadow_camera_ring_open(&error) : NULL;
	if (!writer || !reader)
	{
		fprintf(stderr, "ring: error %lu\n", error);
		goto out;
	}
	if (!expect("ring count with no reader", shadow_camera_ring_readers(writer), 0))
		goto out;
	if (!expect("no event yet, ring 0",
	            shadow_camera_win_readers(FALSE, 0, shadow_camera_ring_readers(writer)), 0))
		goto out;

	/* a reader writes the count */
	shadow_camera_ring_reader(reader, TRUE);
	const LONG ring = shadow_camera_ring_readers(writer);
	if (!expect("ring count with one reader", ring, 1) ||
	    !expect("events seen and stopped, ring 1", shadow_camera_win_readers(TRUE, 0, ring), 0) ||
	    !expect("no event yet, ring 1", shadow_camera_win_readers(FALSE, 0, ring), 1))
		goto out;
	shadow_camera_ring_reader(reader, FALSE);
	if (!expect("events seen and started, ring 0",
	            shadow_camera_win_readers(TRUE, 1, shadow_camera_ring_readers(writer)), 1))
		goto out;
	printf(
	    "TestShadowCameraWinReaders: events 0 + ring 1 -> 0, no events + ring 1 -> 1, events 1 + "
	    "ring 0 -> 1\n");
	rc = 0;

out:
	shadow_camera_ring_close(reader);
	shadow_camera_ring_close(writer);
	return rc;
}
