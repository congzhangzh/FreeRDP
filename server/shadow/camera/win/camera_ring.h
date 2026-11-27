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

/*
 * The Windows virtual camera is a media source DLL (freerdp-shadow-camera-source.dll) that
 * Frame Server loads in its own service process (svchost.exe, session 0, Local Service), not
 * in the shadow server. The two meet in a named shared memory section: the shadow server
 * (the writer, sink_win.c) puts each NV12 frame into one of SHADOW_CAMERA_RING_SLOTS slots
 * and signals an event; the media source (the reader) copies the newest complete frame for
 * each sample it delivers, the same one again when nothing new came (the still the shadow
 * server wrote, while no client camera streams).
 *
 * Each slot has a sequence number, odd while the writer fills it (a seqlock): a reader that
 * sees it change while copying copies again. The readers count is the media sources' own:
 * a stream that starts adds one, one that stops or shuts down takes it back, and readerTick
 * says when one last took a frame, so a reader that died without a word stops counting.
 *
 * Plain Win32, no WinPR: the media source DLL links nothing of FreeRDP.
 */

#ifndef FREERDP_SERVER_SHADOW_CAMERA_RING_H
#define FREERDP_SERVER_SHADOW_CAMERA_RING_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** the section and the event, in the Global\ namespace (the reader is in session 0) */
#define SHADOW_CAMERA_RING_NAME L"Global\\FreeRDP-Shadow-Camera-Ring-v1"
#define SHADOW_CAMERA_RING_EVENT_NAME L"Global\\FreeRDP-Shadow-Camera-Frame-v1"
/** tests: a different name prefix (e.g. "Local\\TestCamera-1234"), read by both sides */
#define SHADOW_CAMERA_RING_NAME_ENV "FREERDP_SHADOW_CAMERA_RING"

#define SHADOW_CAMERA_RING_MAGIC 0x46435246u /* "FRCF" */
#define SHADOW_CAMERA_RING_VERSION 1u
#define SHADOW_CAMERA_RING_SLOTS 3u
#define SHADOW_CAMERA_RING_FOURCC_NV12 0x3231564Eu /* 'N','V','1','2' */
/** a reader that took no frame for this long no longer counts */
#define SHADOW_CAMERA_RING_READER_TIMEOUT_MS 3000u

	typedef struct
	{
		UINT32 magic;
		UINT32 version;
		UINT32 headerSize;
		UINT32 slotCount;
		UINT32 width;
		UINT32 height;
		UINT32 fps;
		UINT32 fourcc;     /**< SHADOW_CAMERA_RING_FOURCC_NV12 */
		UINT32 frameSize;  /**< tightly packed NV12: width * height * 3 / 2 */
		UINT32 slotOffset; /**< from the start of the section */
		UINT32 slotStride;
		UINT32 writerPid;
		volatile LONG latest;       /**< the slot of the newest complete frame, -1 none yet */
		volatile LONG readers;      /**< media source streams running */
		volatile LONG64 frames;     /**< frames written */
		volatile LONG64 readerTick; /**< GetTickCount64() when a reader last took a frame */
		volatile LONG64 writerTick; /**< GetTickCount64() of the last frame written */
		volatile LONG64 slotSequence[SHADOW_CAMERA_RING_SLOTS]; /**< odd while written */
	} ShadowCameraRingHeader;

	typedef struct s_shadow_camera_ring ShadowCameraRing;

	/** The writer: creates the section and the event (Local System and Administrators: all;
	 *  Local Service: read and write the section, wait on the event). error: GetLastError()
	 *  of what failed. A ring that exists already (another shadow server) is an error:
	 *  ERROR_ALREADY_EXISTS. */
	ShadowCameraRing* shadow_camera_ring_create(UINT32 width, UINT32 height, UINT32 fps,
	                                            DWORD* error);

	/** The reader: opens the ring the writer made, nullptr when there is none */
	ShadowCameraRing* shadow_camera_ring_open(DWORD* error);

	void shadow_camera_ring_close(ShadowCameraRing* ring);

	const ShadowCameraRingHeader* shadow_camera_ring_header(const ShadowCameraRing* ring);

	/** The event the writer sets after each frame */
	HANDLE shadow_camera_ring_event(const ShadowCameraRing* ring);

	/** writer: one tightly packed NV12 frame of the ring's size */
	void shadow_camera_ring_write(ShadowCameraRing* ring, const BYTE* nv12);

	/** writer: the media source streams running, 0 when none took a frame lately */
	LONG shadow_camera_ring_readers(const ShadowCameraRing* ring);

	/** reader: copies the newest complete frame into dst (NV12, Y rows then UV rows, pitch
	 *  bytes apart); FALSE when there is none yet. frame: its number (1 for the first). */
	BOOL shadow_camera_ring_read(ShadowCameraRing* ring, BYTE* dst, LONG pitch, UINT64* frame);

	/** reader: a stream starts (TRUE) or stops (FALSE) */
	void shadow_camera_ring_reader(ShadowCameraRing* ring, BOOL started);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_RING_H */
