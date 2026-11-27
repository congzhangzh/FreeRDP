/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: a Windows virtual camera ("FreeRDP Camera") as the camera
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
 * MFCreateVirtualCamera (mfsensorgroup.dll, Windows build 22000 and later: Windows 11 and
 * Windows Server 2025; Server 2022, build 20348, has none) registers a camera whose frames
 * come from a media source DLL that Frame Server loads in its own service (svchost.exe,
 * session 0, Local Service). This sink:
 *
 * - checks the build (RtlGetVersion: GetVersionEx lies to programs without a manifest) and
 *   looks MFCreateVirtualCamera up at run time, so the shadow server still starts without it;
 * - creates the frame ring (camera_ring.h) that the media source reads;
 * - installs freerdp-shadow-camera-source.dll, from beside the shadow server's program, in
 *   %ProgramData%\FreeRDP\Camera (Local Service must read it: not a user's profile) and
 *   registers its CLSID under HKLM;
 * - registers the camera as "FreeRDP Camera" (Windows adds "Windows Virtual Camera"), for all
 *   users and for the life of this process (the shadow server runs as SYSTEM in the console
 *   session: a current-user camera would be SYSTEM's own), removing a stale one first, and
 *   for all users until removed when that fails; removes it when the server stops;
 * - reports readers: the virtual camera's events (SOURCE_START to SOURCE_STOP) once it sent
 *   one, the media source's count in the ring only before (camera_readers.h), so the client's
 *   camera runs only while an app reads.
 *
 * Media Foundation and COM run on a thread of the sink's own (MTA), the frames do not:
 * Write copies into the ring.
 */

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <winpr/assert.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/thread.h>

#include <freerdp/log.h>

#include <objbase.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>

#include "camera_mf_ids.h"
#include "camera_readers.h"
#include "camera_ring.h"
#include "sink_win.h"

#define TAG SERVER_TAG("shadow.camera")

#define WIN_CAMERA_NAME L"FreeRDP Camera"
#define WIN_CAMERA_MIN_BUILD 22000u
/* %ProgramData%\FreeRDP\Camera: SYSTEM and Administrators all; Local Service, users and
 * app containers read; logs\ (only with FREERDP_SHADOW_CAMERA_SOURCE_LOG=1): Local Service
 * may write */
#define WIN_CAMERA_DIR_SDDL                                                                    \
	L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1200a9;;;BU)(A;OICI;" \
	L"0x1200a9;;;AC)"
#define WIN_CAMERA_LOG_SDDL L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)"
#define WIN_CAMERA_LOG_ENV "FREERDP_SHADOW_CAMERA_SOURCE_LOG"

typedef HRESULT(STDAPICALLTYPE* MFStartupFn)(ULONG version, DWORD flags);
typedef HRESULT(STDAPICALLTYPE* MFShutdownFn)(void);

typedef struct s_win_sink WinSink;

/* IMFVirtualCamera::Start's callback: logs the pipeline's events */
typedef struct
{
	IMFAsyncCallback iface;
	volatile LONG refs;
	WinSink* sink;
} WinCameraCallback;

struct s_win_sink
{
	ShadowCameraSink sink;
	ShadowCameraRing* ring;
	size_t frameSize;
	LONG lastReaders;
	/* SOURCE_START to SOURCE_STOP (the virtual camera's own events): 1 while an app streams */
	volatile LONG sourceStreams;
	/* the callback delivered an event since registration: from then on only the events count
	 * (camera_readers.h) */
	volatile LONG eventsSeen;
	/* which signal Readers() went by last: 0 none yet, 1 the ring's count, 2 the events */
	LONG signal;

	HANDLE thread;
	HANDLE ready; /* the thread set up the camera, or gave up: started */
	HANDLE stop;
	BOOL started;

	CameraMFCreateVirtualCameraFn createCamera;
	WCHAR dllPath[MAX_PATH];
	WinCameraCallback callback;
};

/* ---- the Windows build ---- */

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW* info);

static DWORD win_camera_build(void)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFn getVersion =
	    ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
	OSVERSIONINFOW info = { 0 };
	info.dwOSVersionInfoSize = sizeof(info);
	if (!getVersion || (getVersion(&info) != 0))
		return 0;
	return info.dwBuildNumber;
}

/* ---- the media source: installed and registered ---- */

static BOOL win_camera_secure_dir(const WCHAR* path, const WCHAR* sddl)
{
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
		return FALSE;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
	BOOL rc = CreateDirectoryW(path, &sa);
	if (!rc && (GetLastError() == ERROR_ALREADY_EXISTS))
		rc = SetFileSecurityW(path, DACL_SECURITY_INFORMATION, sd);
	LocalFree(sd);
	return rc;
}

static BOOL win_camera_install(WinSink* ws)
{
	WCHAR program[MAX_PATH] = { 0 };
	const DWORD length = GetModuleFileNameW(nullptr, program, ARRAYSIZE(program));
	WCHAR* slash =
	    ((length > 0) && (length < ARRAYSIZE(program))) ? wcsrchr(program, L'\\') : nullptr;
	if (!slash)
		return FALSE;
	slash[1] = L'\0';
	WCHAR source[MAX_PATH] = { 0 };
	if (_snwprintf_s(source, ARRAYSIZE(source), _TRUNCATE, L"%ls%ls", program,
	                 CAMERA_MF_SOURCE_DLL) < 0)
		return FALSE;
	if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES)
	{
		WLog_WARN(TAG, "camera: %ls is not beside the shadow server (%ls), so no virtual camera",
		          CAMERA_MF_SOURCE_DLL, program);
		return FALSE;
	}

	WCHAR data[MAX_PATH] = { 0 };
	const DWORD dataLength = GetEnvironmentVariableW(L"ProgramData", data, ARRAYSIZE(data));
	if ((dataLength == 0) || (dataLength >= ARRAYSIZE(data)))
		wcscpy_s(data, ARRAYSIZE(data), L"C:\\ProgramData");
	WCHAR dir[MAX_PATH] = { 0 };
	(void)_snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE, L"%ls\\FreeRDP", data);
	(void)CreateDirectoryW(dir, nullptr);
	(void)_snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE, L"%ls\\FreeRDP\\Camera", data);
	if (!win_camera_secure_dir(dir, WIN_CAMERA_DIR_SDDL))
	{
		WLog_WARN(TAG, "camera: unable to prepare %ls (error %" PRIu32 "), so no virtual camera",
		          dir, (UINT32)GetLastError());
		return FALSE;
	}
	const char* log = getenv(WIN_CAMERA_LOG_ENV);
	if (log && (strcmp(log, "1") == 0))
	{
		WCHAR logs[MAX_PATH] = { 0 };
		(void)_snwprintf_s(logs, ARRAYSIZE(logs), _TRUNCATE, L"%ls\\logs", dir);
		if (win_camera_secure_dir(logs, WIN_CAMERA_LOG_SDDL))
			WLog_INFO(TAG, "camera: the media source logs to %ls", logs);
	}

	(void)_snwprintf_s(ws->dllPath, ARRAYSIZE(ws->dllPath), _TRUNCATE, L"%ls\\%ls", dir,
	                   CAMERA_MF_SOURCE_DLL);
	if (!CopyFileW(source, ws->dllPath, FALSE))
	{
		/* Frame Server may still hold the one a former server installed */
		const DWORD error = GetLastError();
		if (GetFileAttributesW(ws->dllPath) == INVALID_FILE_ATTRIBUTES)
		{
			WLog_WARN(TAG,
			          "camera: copying %ls to %ls failed (error %" PRIu32 "), so no virtual camera",
			          source, ws->dllPath, (UINT32)error);
			return FALSE;
		}
		WLog_WARN(TAG, "camera: %ls is in use (error %" PRIu32 "), keeping the one installed",
		          ws->dllPath, (UINT32)error);
	}

	HKEY classKey = nullptr;
	LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
	                          L"SOFTWARE\\Classes\\CLSID\\" CAMERA_MF_CLSID_SOURCE_TEXT, 0, nullptr,
	                          0, KEY_WRITE, nullptr, &classKey, nullptr);
	HKEY serverKey = nullptr;
	if (rc == ERROR_SUCCESS)
	{
		const WCHAR name[] = L"FreeRDP shadow camera source";
		rc = RegSetValueExW(classKey, nullptr, 0, REG_SZ, (const BYTE*)name, sizeof(name));
	}
	if (rc == ERROR_SUCCESS)
		rc = RegCreateKeyExW(classKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr,
		                     &serverKey, nullptr);
	if (rc == ERROR_SUCCESS)
		rc = RegSetValueExW(serverKey, nullptr, 0, REG_SZ, (const BYTE*)ws->dllPath,
		                    (DWORD)((wcslen(ws->dllPath) + 1) * sizeof(WCHAR)));
	if (rc == ERROR_SUCCESS)
	{
		const WCHAR model[] = L"Both";
		rc = RegSetValueExW(serverKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)model,
		                    sizeof(model));
	}
	if (serverKey)
		(void)RegCloseKey(serverKey);
	if (classKey)
		(void)RegCloseKey(classKey);
	if (rc != ERROR_SUCCESS)
	{
		WLog_WARN(TAG,
		          "camera: registering the media source needs SYSTEM or an administrator (error "
		          "%" PRId32 "), so no virtual camera",
		          (INT32)rc);
		return FALSE;
	}
	return TRUE;
}

/* ---- the camera's events ---- */

static WinCameraCallback* win_callback(IMFAsyncCallback* iface)
{
	return (WinCameraCallback*)(void*)iface;
}

static HRESULT STDMETHODCALLTYPE win_callback_query(IMFAsyncCallback* This, REFIID riid, void** ppv)
{
	if (!ppv)
		return E_POINTER;
	if (IsEqualIID(riid, &CameraMF_IID_IUnknown) ||
	    IsEqualIID(riid, &CameraMF_IID_IMFAsyncCallback))
	{
		*ppv = This;
		IMFAsyncCallback_AddRef(This);
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE win_callback_addref(IMFAsyncCallback* This)
{
	return (ULONG)InterlockedIncrement(&win_callback(This)->refs);
}

/* the callback lives in the sink, which outlives the camera */
static ULONG STDMETHODCALLTYPE win_callback_release(IMFAsyncCallback* This)
{
	return (ULONG)InterlockedDecrement(&win_callback(This)->refs);
}

static HRESULT STDMETHODCALLTYPE win_callback_parameters(WINPR_ATTR_UNUSED IMFAsyncCallback* This,
                                                         WINPR_ATTR_UNUSED DWORD* flags,
                                                         WINPR_ATTR_UNUSED DWORD* queue)
{
	return E_NOTIMPL;
}

static const char* win_camera_event_name(const GUID* extended)
{
	if (IsEqualGUID(extended, &CameraMF_VCAMEVENT_SOURCE_INITIALIZE))
		return "SOURCE_INITIALIZE";
	if (IsEqualGUID(extended, &CameraMF_VCAMEVENT_SOURCE_START))
		return "SOURCE_START";
	if (IsEqualGUID(extended, &CameraMF_VCAMEVENT_SOURCE_STOP))
		return "SOURCE_STOP";
	if (IsEqualGUID(extended, &CameraMF_VCAMEVENT_SOURCE_UNINITIALIZE))
		return "SOURCE_UNINITIALIZE";
	if (IsEqualGUID(extended, &CameraMF_VCAMEVENT_PIPELINE_SHUTDOWN))
		return "PIPELINE_SHUTDOWN";
	return "other";
}

static HRESULT STDMETHODCALLTYPE win_callback_invoke(IMFAsyncCallback* This, IMFAsyncResult* result)
{
	WinSink* ws = win_callback(This)->sink;
	if (!result)
		return S_OK;
	IUnknown* unknown = nullptr;
	IMFMediaEvent* event = nullptr;
	if (SUCCEEDED(IMFAsyncResult_GetObject(result, &unknown)) && unknown)
	{
		(void)IUnknown_QueryInterface(unknown, &CameraMF_IID_IMFMediaEvent, (void**)&event);
		IUnknown_Release(unknown);
		unknown = nullptr;
	}
	if (!event && SUCCEEDED(IMFAsyncResult_GetState(result, &unknown)) && unknown)
	{
		(void)IUnknown_QueryInterface(unknown, &CameraMF_IID_IMFMediaEvent, (void**)&event);
		IUnknown_Release(unknown);
	}
	if (!event)
		return S_OK;
	MediaEventType type = 0;
	GUID extended = { 0 };
	HRESULT status = S_OK;
	(void)IMFMediaEvent_GetType(event, &type);
	(void)IMFMediaEvent_GetExtendedType(event, &extended);
	(void)IMFMediaEvent_GetStatus(event, &status);
	IMFMediaEvent_Release(event);
	if (type == MEError)
		WLog_WARN(TAG, "camera: the virtual camera's pipeline reports an error 0x%08" PRIX32,
		          (UINT32)status);
	else
		WLog_INFO(TAG, "camera: virtual camera event %s (status 0x%08" PRIX32 ")",
		          win_camera_event_name(&extended), (UINT32)status);

	/* the readers: Frame Server says when an app starts and stops streaming */
	if (ws && ((type == MEExtendedType) || (type == MEError)))
		(void)InterlockedExchange(&ws->eventsSeen, 1);
	if (ws && (type == MEExtendedType))
	{
		if (IsEqualGUID(&extended, &CameraMF_VCAMEVENT_SOURCE_START))
			(void)InterlockedExchange(&ws->sourceStreams, 1);
		else if (IsEqualGUID(&extended, &CameraMF_VCAMEVENT_SOURCE_STOP) ||
		         IsEqualGUID(&extended, &CameraMF_VCAMEVENT_SOURCE_UNINITIALIZE) ||
		         IsEqualGUID(&extended, &CameraMF_VCAMEVENT_PIPELINE_SHUTDOWN))
			(void)InterlockedExchange(&ws->sourceStreams, 0);
	}
	else if (ws && (type == MEError))
		(void)InterlockedExchange(&ws->sourceStreams, 0);
	return S_OK;
}

static const IMFAsyncCallbackVtbl win_callback_vtbl = { win_callback_query, win_callback_addref,
	                                                    win_callback_release,
	                                                    win_callback_parameters,
	                                                    win_callback_invoke };

/* ---- the camera, on the sink's thread ---- */

static CameraMFVirtualCamera* win_camera_create(WinSink* ws, int access, int lifetime,
                                                const char* mode)
{
	CameraMFVirtualCamera* camera = nullptr;
	HRESULT hr =
	    ws->createCamera(CameraMF_VirtualCameraType_SoftwareCameraSource, lifetime, access,
	                     WIN_CAMERA_NAME, CAMERA_MF_CLSID_SOURCE_TEXT, nullptr, 0, &camera);
	if (FAILED(hr))
	{
		WLog_WARN(TAG, "camera: MFCreateVirtualCamera (%s) failed: 0x%08" PRIX32, mode, (UINT32)hr);
		return nullptr;
	}
	hr = camera->lpVtbl->Start(camera, &ws->callback.iface);
	if (FAILED(hr))
	{
		WLog_WARN(TAG, "camera: starting the virtual camera (%s) failed: 0x%08" PRIX32, mode,
		          (UINT32)hr);
		(void)camera->lpVtbl->Shutdown(camera);
		camera->lpVtbl->Release(camera);
		return nullptr;
	}
	LPWSTR link = nullptr;
	UINT32 length = 0;
	if (SUCCEEDED(camera->lpVtbl->GetAllocatedString(
	        camera, &CameraMF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
	        &length)))
	{
		WLog_INFO(TAG, "camera: virtual camera \"%ls\" (%s) registered: %ls", WIN_CAMERA_NAME, mode,
		          link);
		CoTaskMemFree(link);
	}
	return camera;
}

/* a camera left by a server that did not stop (a system-lifetime one stays registered) */
static void win_camera_remove_stale(WinSink* ws, int access, int lifetime)
{
	CameraMFVirtualCamera* camera = nullptr;
	if (FAILED(ws->createCamera(CameraMF_VirtualCameraType_SoftwareCameraSource, lifetime, access,
	                            WIN_CAMERA_NAME, CAMERA_MF_CLSID_SOURCE_TEXT, nullptr, 0, &camera)))
		return;
	const HRESULT hr = camera->lpVtbl->Remove(camera);
	if (SUCCEEDED(hr))
		WLog_INFO(TAG, "camera: removed a stale virtual camera");
	(void)camera->lpVtbl->Shutdown(camera);
	camera->lpVtbl->Release(camera);
}

static DWORD WINAPI win_camera_thread(LPVOID arg)
{
	WinSink* ws = arg;
	CameraMFVirtualCamera* camera = nullptr;
	HMODULE mfplat = nullptr;
	MFShutdownFn mfShutdown = nullptr;
	BOOL mfStarted = FALSE;

	const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	mfplat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	MFStartupFn mfStartup =
	    mfplat ? (MFStartupFn)(void*)GetProcAddress(mfplat, "MFStartup") : nullptr;
	mfShutdown = mfplat ? (MFShutdownFn)(void*)GetProcAddress(mfplat, "MFShutdown") : nullptr;
	if (FAILED(co) || !mfStartup || !mfShutdown)
		WLog_WARN(TAG,
		          "camera: COM (0x%08" PRIX32
		          ") or Media Foundation unavailable, so no virtual camera",
		          (UINT32)co);
	else
	{
		const HRESULT hr = mfStartup(MF_VERSION, MFSTARTUP_FULL);
		mfStarted = SUCCEEDED(hr);
		if (!mfStarted)
			WLog_WARN(TAG, "camera: MFStartup failed: 0x%08" PRIX32 ", so no virtual camera",
			          (UINT32)hr);
	}

	if (mfStarted)
	{
		win_camera_remove_stale(ws, CameraMF_VirtualCameraAccess_AllUsers,
		                        CameraMF_VirtualCameraLifetime_System);
		/* proven by the kit's probe-camera-winvcam (as an administrator); then a camera
		 * for all users that stays until removed */
		camera =
		    win_camera_create(ws, CameraMF_VirtualCameraAccess_AllUsers,
		                      CameraMF_VirtualCameraLifetime_Session, "all users, this session");
		if (!camera)
			camera = win_camera_create(ws, CameraMF_VirtualCameraAccess_AllUsers,
			                           CameraMF_VirtualCameraLifetime_System,
			                           "all users, until removed");
	}

	ws->started = (camera != nullptr);
	(void)SetEvent(ws->ready);

	if (camera)
	{
		(void)WaitForSingleObject(ws->stop, INFINITE);
		const HRESULT hr = camera->lpVtbl->Remove(camera);
		if (FAILED(hr))
			WLog_WARN(TAG, "camera: removing the virtual camera failed: 0x%08" PRIX32, (UINT32)hr);
		(void)camera->lpVtbl->Shutdown(camera);
		camera->lpVtbl->Release(camera);
		WLog_INFO(TAG, "camera: virtual camera removed");
	}
	if (mfStarted)
		(void)mfShutdown();
	if (mfplat)
		(void)FreeLibrary(mfplat);
	if (SUCCEEDED(co))
		CoUninitialize();
	return 0;
}

/* ---- the sink ---- */

static BOOL win_sink_write(ShadowCameraSink* sink, const ShadowCameraImage* frame,
                           WINPR_ATTR_UNUSED SHADOW_CAMERA_FRAME_KIND kind,
                           WINPR_ATTR_UNUSED const char* detail)
{
	WinSink* ws = (WinSink*)sink;
	WINPR_ASSERT(ws);
	WINPR_ASSERT(frame);
	if ((frame->format != SHADOW_CAMERA_PIXEL_NV12) || (frame->width != sink->width) ||
	    (frame->height != sink->height))
		return FALSE;
	/* tightly packed, Y then UV: the core's frames are (camera_sink.h) */
	if ((frame->planes[1] != frame->planes[0] + (size_t)sink->width * sink->height) ||
	    (frame->strides[0] != sink->width))
		return FALSE;
	shadow_camera_ring_write(ws->ring, frame->planes[0]);
	return TRUE;
}

/* the readers: once the virtual camera sent an event, its events alone (SOURCE_START to
 * SOURCE_STOP); before any, the media source's count in the ring (camera_readers.h); never
 * blocks */
static INT32 win_sink_readers(ShadowCameraSink* sink)
{
	WinSink* ws = (WinSink*)sink;
	WINPR_ASSERT(ws);
	const BOOL seen = InterlockedCompareExchange(&ws->eventsSeen, 0, 0) != 0;
	const LONG events = InterlockedCompareExchange(&ws->sourceStreams, 0, 0);
	const LONG ring = seen ? 0 : shadow_camera_ring_readers(ws->ring);
	const LONG readers = shadow_camera_win_readers(seen, events, ring);

	const LONG signal = seen ? 2 : 1;
	if (InterlockedExchange(&ws->signal, signal) != signal)
		WLog_INFO(TAG, "camera: %s",
		          seen ? "the virtual camera's events say who reads it, the media source's count "
		                 "is ignored"
		               : "no event from the virtual camera yet, its readers come from the "
		                 "media source's count until one comes");

	const LONG last = InterlockedExchange(&ws->lastReaders, readers);
	if ((readers > 0) != (last > 0))
		WLog_INFO(TAG, "camera: %s (by its %s)",
		          (readers > 0) ? "an app reads the virtual camera"
		                        : "no app reads the virtual camera",
		          seen ? "events" : "media source's count");
	return (INT32)readers;
}

static void win_sink_free(ShadowCameraSink* sink)
{
	WinSink* ws = (WinSink*)sink;
	if (!ws)
		return;
	if (ws->thread)
	{
		(void)SetEvent(ws->stop);
		(void)WaitForSingleObject(ws->thread, INFINITE);
		(void)CloseHandle(ws->thread);
	}
	if (ws->ready)
		(void)CloseHandle(ws->ready);
	if (ws->stop)
		(void)CloseHandle(ws->stop);
	shadow_camera_ring_close(ws->ring);
	free(ws);
}

ShadowCameraSink* shadow_camera_sink_win_new(UINT32 width, UINT32 height, UINT32 fps)
{
	const DWORD build = win_camera_build();
	if (build < WIN_CAMERA_MIN_BUILD)
	{
		WLog_INFO(TAG,
		          "camera: needs Windows 11 or Windows Server 2025 (build 22000 or later); this "
		          "system is build %" PRIu32 ", so no virtual camera",
		          (UINT32)build);
		return nullptr;
	}

	HMODULE sensorGroup =
	    LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	CameraMFCreateVirtualCameraFn createCamera =
	    sensorGroup ? (CameraMFCreateVirtualCameraFn)(void*)GetProcAddress(sensorGroup,
	                                                                       "MFCreateVirtualCamera")
	                : nullptr;
	if (!createCamera)
	{
		WLog_INFO(TAG,
		          "camera: mfsensorgroup.dll has no MFCreateVirtualCamera on build %" PRIu32
		          ", so no virtual camera",
		          (UINT32)build);
		if (sensorGroup)
			(void)FreeLibrary(sensorGroup);
		return nullptr;
	}

	const size_t frameSize = shadow_camera_image_size(SHADOW_CAMERA_PIXEL_NV12, width, height);
	if ((frameSize == 0) || (fps == 0))
		return nullptr;

	WinSink* ws = calloc(1, sizeof(WinSink));
	if (!ws)
		return nullptr;
	ws->frameSize = frameSize;
	ws->createCamera = createCamera; /* mfsensorgroup.dll stays loaded */
	ws->callback.iface.lpVtbl = (IMFAsyncCallbackVtbl*)&win_callback_vtbl;
	ws->callback.refs = 1;
	ws->callback.sink = ws;
	ws->sink.name = "Windows virtual camera";
	ws->sink.format = SHADOW_CAMERA_PIXEL_NV12;
	ws->sink.width = width;
	ws->sink.height = height;
	ws->sink.fps = fps;
	ws->sink.placeholderIntervalMs = 0; /* the media source repeats the newest frame */
	ws->sink.Write = win_sink_write;
	ws->sink.Readers = win_sink_readers;
	ws->sink.Free = win_sink_free;

	DWORD error = 0;
	ws->ring = shadow_camera_ring_create(width, height, fps, &error);
	if (!ws->ring)
	{
		if (error == ERROR_ALREADY_EXISTS)
			WLog_WARN(TAG, "camera: another shadow server has the virtual camera, so none here");
		else
			WLog_WARN(TAG,
			          "camera: the frame ring needs SYSTEM or an administrator (error %" PRIu32
			          "), so no virtual camera",
			          (UINT32)error);
		win_sink_free(&ws->sink);
		return nullptr;
	}

	if (!win_camera_install(ws))
	{
		win_sink_free(&ws->sink);
		return nullptr;
	}

	ws->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ws->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (ws->ready && ws->stop)
		ws->thread = CreateThread(nullptr, 0, win_camera_thread, ws, 0, nullptr);
	if (!ws->thread)
	{
		win_sink_free(&ws->sink);
		return nullptr;
	}
	(void)WaitForSingleObject(ws->ready, INFINITE);
	if (!ws->started)
	{
		win_sink_free(&ws->sink);
		return nullptr;
	}

	WLog_INFO(TAG,
	          "camera: virtual camera \"FreeRDP Camera\", NV12 %" PRIu32 "x%" PRIu32 " at %" PRIu32
	          " fps, media source %ls",
	          width, height, fps, ws->dllPath);
	return &ws->sink;
}
