/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the frame ring between the shadow server and its media source
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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "camera_ring.h"

#include <sddl.h>

C_ASSERT((offsetof(ShadowCameraRingHeader, frames) % 8) == 0);
C_ASSERT((offsetof(ShadowCameraRingHeader, slotSequence) % 8) == 0);
C_ASSERT((offsetof(ShadowCameraRingReaders, readerTick) % 8) == 0);

/* the slots start at a cache line, each one a multiple of 64 bytes */
#define RING_ALIGN 64u
/* the largest frame the ring holds: 4096x4096 NV12 */
#define RING_MAX_FRAME (4096u * 4096u * 3u / 2u)

/* SYSTEM and Administrators everything; Local Service: the frames read, the readers' section
 * read and write, the event waited on */
#define RING_FRAMES_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;LS)"
#define RING_READERS_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)"
#define RING_EVENT_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;LS)"

struct s_shadow_camera_ring
{
	HANDLE section;
	HANDLE readersSection;
	HANDLE event;
	BYTE* base;
	ShadowCameraRingHeader* header;
	ShadowCameraRingReaders* readers; /* nullptr for a reader that may not write it */
	size_t size;
	BOOL writer;
	BOOL counted; /* reader: this one is in readers */
	DWORD readersError;
};

typedef struct
{
	WCHAR frames[160];
	WCHAR readers[160];
	WCHAR event[160];
	BOOL local;
} RingNames;

/* the names, with the test prefix when SHADOW_CAMERA_RING_NAME_ENV is set */
static BOOL ring_names(RingNames* names)
{
	char prefix[128] = { 0 };
	const DWORD length =
	    GetEnvironmentVariableA(SHADOW_CAMERA_RING_NAME_ENV, prefix, (DWORD)sizeof(prefix));
	memset(names, 0, sizeof(*names));
	if ((length == 0) || (length >= sizeof(prefix)))
	{
		return (wcscpy_s(names->frames, ARRAYSIZE(names->frames), SHADOW_CAMERA_RING_NAME) == 0) &&
		       (wcscpy_s(names->readers, ARRAYSIZE(names->readers),
		                 SHADOW_CAMERA_RING_READERS_NAME) == 0) &&
		       (wcscpy_s(names->event, ARRAYSIZE(names->event), SHADOW_CAMERA_RING_EVENT_NAME) ==
		        0);
	}
	names->local = (_strnicmp(prefix, "Local\\", 6) == 0);
	return (_snwprintf_s(names->frames, ARRAYSIZE(names->frames), _TRUNCATE, L"%hs-Ring", prefix) >=
	        0) &&
	       (_snwprintf_s(names->readers, ARRAYSIZE(names->readers), _TRUNCATE, L"%hs-Readers",
	                     prefix) >= 0) &&
	       (_snwprintf_s(names->event, ARRAYSIZE(names->event), _TRUNCATE, L"%hs-Frame", prefix) >=
	        0);
}

/* a test's Local\ ring takes the default security (sa->lpSecurityDescriptor stays NULL) */
static BOOL ring_security(BOOL local, const WCHAR* sddl, SECURITY_ATTRIBUTES* sa)
{
	sa->nLength = sizeof(*sa);
	sa->lpSecurityDescriptor = NULL;
	sa->bInheritHandle = FALSE;
	if (local)
		return TRUE;
	return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
	                                                            &sa->lpSecurityDescriptor, NULL);
}

static void ring_free(ShadowCameraRing* ring)
{
	if (!ring)
		return;
	if (ring->base)
		(void)UnmapViewOfFile(ring->base);
	if (ring->readers)
		(void)UnmapViewOfFile(ring->readers);
	if (ring->event)
		(void)CloseHandle(ring->event);
	if (ring->readersSection)
		(void)CloseHandle(ring->readersSection);
	if (ring->section)
		(void)CloseHandle(ring->section);
	free(ring);
}

/* a new named section of size bytes; ERROR_ALREADY_EXISTS fails */
static HANDLE ring_new_section(const WCHAR* name, BOOL local, const WCHAR* sddl, size_t size,
                               DWORD* error)
{
	SECURITY_ATTRIBUTES sa = { 0 };
	if (!ring_security(local, sddl, &sa))
	{
		*error = GetLastError();
		return NULL;
	}
	HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : NULL,
	                                    PAGE_READWRITE, (DWORD)((UINT64)size >> 32),
	                                    (DWORD)(size & 0xFFFFFFFFu), name);
	*error = GetLastError();
	LocalFree(sa.lpSecurityDescriptor);
	if (section && (*error == ERROR_ALREADY_EXISTS))
	{
		(void)CloseHandle(section);
		return NULL;
	}
	if (section)
		*error = ERROR_SUCCESS;
	return section;
}

ShadowCameraRing* shadow_camera_ring_create(UINT32 width, UINT32 height, UINT32 fps, DWORD* error)
{
	DWORD dummy = 0;
	if (!error)
		error = &dummy;
	*error = ERROR_INVALID_PARAMETER;
	if ((width == 0) || (height == 0) || (width % 2) || (height % 2) || (fps == 0) ||
	    (width > 4096) || (height > 4096))
		return NULL;

	const UINT32 frameSize = width * height * 3u / 2u;
	if (frameSize > RING_MAX_FRAME)
		return NULL;
	const UINT32 headerSize = (UINT32)sizeof(ShadowCameraRingHeader);
	const UINT32 slotOffset = (headerSize + RING_ALIGN - 1u) & ~(RING_ALIGN - 1u);
	const UINT32 slotStride = (frameSize + RING_ALIGN - 1u) & ~(RING_ALIGN - 1u);
	const size_t size = (size_t)slotOffset + (size_t)slotStride * SHADOW_CAMERA_RING_SLOTS;

	RingNames names;
	if (!ring_names(&names))
		return NULL;

	ShadowCameraRing* ring = calloc(1, sizeof(ShadowCameraRing));
	if (!ring)
	{
		*error = ERROR_OUTOFMEMORY;
		return NULL;
	}
	ring->writer = TRUE;
	ring->size = size;

	ring->section = ring_new_section(names.frames, names.local, RING_FRAMES_SDDL, size, error);
	if (!ring->section)
		goto fail;
	ring->readersSection = ring_new_section(names.readers, names.local, RING_READERS_SDDL,
	                                        sizeof(ShadowCameraRingReaders), error);
	if (!ring->readersSection)
		goto fail;

	SECURITY_ATTRIBUTES eventSa = { 0 };
	if (!ring_security(names.local, RING_EVENT_SDDL, &eventSa))
	{
		*error = GetLastError();
		goto fail;
	}
	ring->event =
	    CreateEventW(eventSa.lpSecurityDescriptor ? &eventSa : NULL, FALSE, FALSE, names.event);
	*error = GetLastError();
	LocalFree(eventSa.lpSecurityDescriptor);
	if (!ring->event || (*error == ERROR_ALREADY_EXISTS))
		goto fail;

	ring->base = MapViewOfFile(ring->section, FILE_MAP_ALL_ACCESS, 0, 0, size);
	ring->readers = MapViewOfFile(ring->readersSection, FILE_MAP_ALL_ACCESS, 0, 0,
	                              sizeof(ShadowCameraRingReaders));
	if (!ring->base || !ring->readers)
	{
		*error = GetLastError();
		goto fail;
	}
	ring->header = (ShadowCameraRingHeader*)ring->base;

	ShadowCameraRingHeader* h = ring->header;
	h->headerSize = headerSize;
	h->slotCount = SHADOW_CAMERA_RING_SLOTS;
	h->width = width;
	h->height = height;
	h->fps = fps;
	h->fourcc = SHADOW_CAMERA_RING_FOURCC_NV12;
	h->frameSize = frameSize;
	h->slotOffset = slotOffset;
	h->slotStride = slotStride;
	h->writerPid = GetCurrentProcessId();
	h->latest = -1;
	h->version = SHADOW_CAMERA_RING_VERSION;
	ring->readers->version = SHADOW_CAMERA_RING_VERSION;
	MemoryBarrier();
	ring->readers->magic = SHADOW_CAMERA_RING_MAGIC;
	h->magic = SHADOW_CAMERA_RING_MAGIC; /* last: a reader sees all of it or nothing */

	*error = ERROR_SUCCESS;
	return ring;

fail:
	ring_free(ring);
	return NULL;
}

ShadowCameraRing* shadow_camera_ring_open(DWORD* error)
{
	DWORD dummy = 0;
	if (!error)
		error = &dummy;

	RingNames names;
	if (!ring_names(&names))
	{
		*error = ERROR_INVALID_NAME;
		return NULL;
	}

	ShadowCameraRing* ring = calloc(1, sizeof(ShadowCameraRing));
	if (!ring)
	{
		*error = ERROR_OUTOFMEMORY;
		return NULL;
	}

	/* the frames: read-only */
	ring->section = OpenFileMappingW(FILE_MAP_READ, FALSE, names.frames);
	if (!ring->section)
		goto fail;
	/* the event: the media source paces itself, so a reader that may not wait on it goes on */
	ring->event = OpenEventW(SYNCHRONIZE, FALSE, names.event);

	/* the header first, then the whole ring at the size it says */
	ShadowCameraRingHeader* probe =
	    MapViewOfFile(ring->section, FILE_MAP_READ, 0, 0, sizeof(ShadowCameraRingHeader));
	if (!probe)
		goto fail;
	const BOOL valid = (probe->magic == SHADOW_CAMERA_RING_MAGIC) &&
	                   (probe->version == SHADOW_CAMERA_RING_VERSION) &&
	                   (probe->headerSize == sizeof(ShadowCameraRingHeader)) &&
	                   (probe->slotCount == SHADOW_CAMERA_RING_SLOTS) &&
	                   (probe->fourcc == SHADOW_CAMERA_RING_FOURCC_NV12) &&
	                   (probe->frameSize == probe->width * probe->height * 3u / 2u) &&
	                   (probe->frameSize <= RING_MAX_FRAME) &&
	                   (probe->slotStride >= probe->frameSize) &&
	                   (probe->slotOffset >= probe->headerSize);
	const size_t size =
	    valid ? (size_t)probe->slotOffset + (size_t)probe->slotStride * SHADOW_CAMERA_RING_SLOTS
	          : 0;
	(void)UnmapViewOfFile(probe);
	if (!valid)
	{
		SetLastError(ERROR_REVISION_MISMATCH);
		goto fail;
	}
	ring->base = MapViewOfFile(ring->section, FILE_MAP_READ, 0, 0, size);
	if (!ring->base)
		goto fail;
	ring->header = (ShadowCameraRingHeader*)ring->base;
	ring->size = size;

	/* the readers' section: read-write when this reader may, else it just does not count */
	ring->readersSection = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, names.readers);
	if (ring->readersSection)
		ring->readers = MapViewOfFile(ring->readersSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
		                              sizeof(ShadowCameraRingReaders));
	if (!ring->readers)
		ring->readersError = GetLastError();
	else if ((ring->readers->magic != SHADOW_CAMERA_RING_MAGIC) ||
	         (ring->readers->version != SHADOW_CAMERA_RING_VERSION))
	{
		(void)UnmapViewOfFile(ring->readers);
		ring->readers = NULL;
		ring->readersError = ERROR_REVISION_MISMATCH;
	}
	*error = ERROR_SUCCESS;
	return ring;

fail:
	*error = GetLastError();
	ring_free(ring);
	return NULL;
}

void shadow_camera_ring_close(ShadowCameraRing* ring)
{
	if (!ring)
		return;
	if (!ring->writer)
		shadow_camera_ring_reader(ring, FALSE);
	ring_free(ring);
}

const ShadowCameraRingHeader* shadow_camera_ring_header(const ShadowCameraRing* ring)
{
	return ring ? ring->header : NULL;
}

HANDLE shadow_camera_ring_event(const ShadowCameraRing* ring)
{
	return ring ? ring->event : NULL;
}

DWORD shadow_camera_ring_readers_error(const ShadowCameraRing* ring)
{
	return ring ? ring->readersError : ERROR_INVALID_HANDLE;
}

static LONG64 ring_load64(volatile LONG64* value)
{
	return InterlockedCompareExchange64(value, 0, 0);
}

void shadow_camera_ring_write(ShadowCameraRing* ring, const BYTE* nv12)
{
	if (!ring || !ring->writer || !nv12)
		return;
	ShadowCameraRingHeader* h = ring->header;
	const LONG latest = h->latest;
	const UINT32 slot = (latest < 0) ? 0u : ((UINT32)latest + 1u) % SHADOW_CAMERA_RING_SLOTS;
	const LONG64 frame = ring_load64(&h->frames) + 1;

	(void)InterlockedExchange64(&h->slotSequence[slot], frame * 2 - 1); /* odd: writing */
	memcpy(ring->base + h->slotOffset + (size_t)h->slotStride * slot, nv12, h->frameSize);
	(void)InterlockedExchange64(&h->slotSequence[slot], frame * 2); /* even: complete */
	(void)InterlockedExchange(&h->latest, (LONG)slot);
	(void)InterlockedExchange64(&h->frames, frame);
	(void)InterlockedExchange64(&h->writerTick, (LONG64)GetTickCount64());
	(void)SetEvent(ring->event);
}

LONG shadow_camera_ring_readers(const ShadowCameraRing* ring)
{
	if (!ring || !ring->readers)
		return 0;
	ShadowCameraRingReaders* r = ring->readers;
	const LONG readers = InterlockedCompareExchange(&r->readers, 0, 0);
	if (readers <= 0)
		return 0;
	const UINT64 tick = (UINT64)ring_load64(&r->readerTick);
	if ((tick == 0) || (GetTickCount64() - tick > SHADOW_CAMERA_RING_READER_TIMEOUT_MS))
		return 0;
	return readers;
}

BOOL shadow_camera_ring_read(ShadowCameraRing* ring, BYTE* dst, LONG pitch, UINT64* frame)
{
	if (!ring || !dst)
		return FALSE;
	const ShadowCameraRingHeader* h = ring->header;
	if ((pitch < 0) || ((UINT32)pitch < h->width))
		return FALSE;

	for (int attempt = 0; attempt < 8; attempt++)
	{
		/* the frames are mapped read-only here: plain aligned reads (atomic on the 64-bit
		 * targets), never an interlocked operation, which writes even when it changes nothing */
		const LONG latest = h->latest;
		MemoryBarrier();
		if ((latest < 0) || ((UINT32)latest >= SHADOW_CAMERA_RING_SLOTS))
			return FALSE;
		const LONG64 before = h->slotSequence[latest];
		MemoryBarrier();
		if (before & 1)
			continue; /* being written: the writer moves on to the next slot */

		const BYTE* src = ring->base + h->slotOffset + (size_t)h->slotStride * (UINT32)latest;
		const UINT32 rows = h->height + h->height / 2u; /* Y, then interleaved UV */
		if ((UINT32)pitch == h->width)
			memcpy(dst, src, h->frameSize);
		else
		{
			for (UINT32 y = 0; y < rows; y++)
				memcpy(dst + (size_t)y * (size_t)pitch, src + (size_t)y * h->width, h->width);
		}
		MemoryBarrier();
		if (h->slotSequence[latest] != before)
			continue; /* overwritten while copying */

		if (ring->readers)
			(void)InterlockedExchange64(&ring->readers->readerTick, (LONG64)GetTickCount64());
		if (frame)
			*frame = (UINT64)(before / 2);
		return TRUE;
	}
	return FALSE;
}

void shadow_camera_ring_reader(ShadowCameraRing* ring, BOOL started)
{
	if (!ring || ring->writer || !ring->readers || (ring->counted == started))
		return;
	ring->counted = started;
	if (started)
	{
		(void)InterlockedExchange64(&ring->readers->readerTick, (LONG64)GetTickCount64());
		(void)InterlockedIncrement(&ring->readers->readers);
	}
	else
		(void)InterlockedDecrement(&ring->readers->readers);
}
