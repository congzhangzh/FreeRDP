/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the media source behind the Windows virtual camera
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
 * freerdp-shadow-camera-source.dll: the custom media source of the "FreeRDP Camera" virtual
 * camera (MFCreateVirtualCamera, Windows build 22000 and later). Frame Server loads it in
 * its own service process (svchost.exe, session 0, Local Service); the frames come from the
 * shadow server through the ring of camera_ring.h.
 *
 * C++: the Media Foundation interfaces are COM classes. It follows the rules of "Frame
 * Server Custom Media Source" (Microsoft Learn) and the design of Microsoft's Windows-Camera
 * VirtualCamera sample (no code taken), as proven by the kit's probe-camera-winvcam on
 * Windows Server 2025 and Windows 11 on Arm:
 *
 * - the CLSID makes an IMFActivate; each ActivateObject creates a new source (Frame Server
 *   Monitor activates, inspects and shuts one down while the camera is registered);
 *   DetachObject and ShutdownObject return E_NOTIMPL, as the rules say
 * - the source: IMFMediaSourceEx, IMFGetService (none), IKsControl (no controls:
 *   ERROR_SET_NOT_FOUND) and IMFSampleAllocatorControl (Frame Server gives the stream its
 *   allocator); its attributes: the activate's, and the Legacy camera profile
 * - one stream, IMFMediaStream2, NV12 at the ring's size and rate; MF_DEVICESTREAM_* on
 *   its attributes and its descriptor (category PINNAME_VIDEO_CAPTURE, id 0, shared,
 *   colour)
 * - a live source: no pause, MFGetSystemTime timestamps, the request token on each sample.
 *   Requests are served at the ring's frame rate by the stream's own thread, with the
 *   newest frame of the ring (the same again while nothing new came), black while the ring
 *   has none or is not there
 * - a stream that runs counts as a reader in the ring (the shadow server starts the
 *   client's camera only while there is one) and stops counting when it stops
 *
 * With a "logs" folder beside the DLL (the shadow server makes it when asked to,
 * FREERDP_SHADOW_CAMERA_SOURCE_LOG=1), every process that loads it writes
 * logs\source-<pid>.log: who loaded it and what it was asked.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksproxy.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "../camera_mf_ids.h"
#include "../camera_ring.h"

namespace
{
	/* the size the media type has when there is no ring (the shadow server's default) */
	constexpr UINT32 kDefaultWidth = 1280;
	constexpr UINT32 kDefaultHeight = 720;
	constexpr UINT32 kDefaultFps = 30;
	constexpr ULONG kMaxPending = 16;

	HMODULE g_module = nullptr;
	volatile LONG g_objects = 0;
	volatile LONG g_locks = 0;

	/* ------------------------------------------------------------------ the log */

	CRITICAL_SECTION g_logLock;
	HANDLE g_logFile = INVALID_HANDLE_VALUE;

	void LogOpen()
	{
		InitializeCriticalSection(&g_logLock);
		WCHAR path[MAX_PATH + 64] = {};
		const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
		WCHAR* slash = ((length > 0) && (length < MAX_PATH)) ? wcsrchr(path, L'\\') : nullptr;
		if (!slash)
			return;
		(void)_snwprintf_s(slash + 1, 60, _TRUNCATE, L"logs\\source-%lu.log",
		                   GetCurrentProcessId());
		WCHAR dir[MAX_PATH + 64] = {};
		(void)wcscpy_s(dir, ARRAYSIZE(dir), path);
		WCHAR* dirEnd = wcsrchr(dir, L'\\');
		if (dirEnd)
			*dirEnd = L'\0';
		const DWORD attributes = GetFileAttributesW(dir);
		if ((attributes == INVALID_FILE_ATTRIBUTES) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
			return;
		g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	void Log(const char* format, ...)
	{
		if (g_logFile == INVALID_HANDLE_VALUE)
			return;
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		char line[1024] = {};
		int used =
		    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u pid %lu tid %lu ",
		                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
		                GetCurrentProcessId(), GetCurrentThreadId());
		if (used < 0)
			used = 0;
		va_list args;
		va_start(args, format);
		(void)_vsnprintf_s(line + used, sizeof(line) - (size_t)used - 2, _TRUNCATE, format, args);
		va_end(args);
		const size_t end = strlen(line);
		line[end] = '\n';
		line[end + 1] = '\0';
		EnterCriticalSection(&g_logLock);
		DWORD written = 0;
		(void)WriteFile(g_logFile, line, (DWORD)strlen(line), &written, nullptr);
		(void)FlushFileBuffers(g_logFile);
		LeaveCriticalSection(&g_logLock);
	}

	HRESULT Failed(const char* what, HRESULT hr)
	{
		if (FAILED(hr))
			Log("%s failed hr=0x%08lX", what, (unsigned long)hr);
		return hr;
	}

	void LogGuid(const char* what, REFGUID g)
	{
		Log("%s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", what, (unsigned long)g.Data1,
		    g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4],
		    g.Data4[5], g.Data4[6], g.Data4[7]);
	}

#define CAMERA_RETURN_IF_FAILED(what, expr)         \
	do                                              \
	{                                               \
		const HRESULT hr_ = Failed((what), (expr)); \
		if (FAILED(hr_))                            \
			return hr_;                             \
	} while (0)

	template <class T> void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	class AutoLock
	{
	  public:
		explicit AutoLock(CRITICAL_SECTION* cs) : cs_(cs)
		{
			EnterCriticalSection(cs_);
		}
		~AutoLock()
		{
			LeaveCriticalSection(cs_);
		}
		AutoLock(const AutoLock&) = delete;
		AutoLock& operator=(const AutoLock&) = delete;

	  private:
		CRITICAL_SECTION* cs_;
	};

	/* ------------------------------------------------------------------ interfaces the
	 * toolchain may not declare, under our names (camera_mf_ids.h has their IIDs) */

	struct CameraMediaStream2 : public IMFMediaStream
	{
		virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
		virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* value) = 0;
	};

	struct CameraSampleAllocatorControl : public IUnknown
	{
		virtual HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD streamId,
		                                                      IUnknown* allocator) = 0;
		virtual HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId,
		                                                    int* usage) = 0;
	};

	struct CameraSensorProfile : public IUnknown
	{
		virtual HRESULT STDMETHODCALLTYPE GetProfileId(void* id) = 0;
		virtual HRESULT STDMETHODCALLTYPE AddProfileFilter(UINT32 streamId, LPCWSTR filter) = 0;
		virtual HRESULT STDMETHODCALLTYPE IsMediaTypeSupported(UINT32 streamId, IMFMediaType* type,
		                                                       BOOL* supported) = 0;
		virtual HRESULT STDMETHODCALLTYPE AddBlockedControl(LPCWSTR control) = 0;
	};

	struct CameraSensorProfileCollection : public IUnknown
	{
		virtual DWORD STDMETHODCALLTYPE GetProfileCount() = 0;
		virtual HRESULT STDMETHODCALLTYPE GetProfile(DWORD index,
		                                             CameraSensorProfile** profile) = 0;
		virtual HRESULT STDMETHODCALLTYPE AddProfile(CameraSensorProfile* profile) = 0;
		virtual HRESULT STDMETHODCALLTYPE FindProfile(void* id, CameraSensorProfile** profile) = 0;
		virtual void STDMETHODCALLTYPE RemoveProfileByIndex(DWORD index) = 0;
		virtual void STDMETHODCALLTYPE RemoveProfile(void* id) = 0;
	};

	/* ------------------------------------------------------------------ helpers */

	struct Format
	{
		UINT32 width;
		UINT32 height;
		UINT32 fps;
	};

	HRESULT CreateVideoType(const Format& format, IMFMediaType** out)
	{
		IMFMediaType* type = nullptr;
		CAMERA_RETURN_IF_FAILED("MFCreateMediaType", MFCreateMediaType(&type));
		HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(hr))
			hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		if (SUCCEEDED(hr))
			hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, format.width, format.height);
		if (SUCCEEDED(hr))
			hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, format.fps, 1);
		if (SUCCEEDED(hr))
			hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		if (SUCCEEDED(hr))
			hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (SUCCEEDED(hr))
			hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		if (SUCCEEDED(hr))
			hr = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
		if (SUCCEEDED(hr))
			hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, format.width);
		if (SUCCEEDED(hr))
			hr = type->SetUINT32(MF_MT_SAMPLE_SIZE, format.width * format.height * 3 / 2);
		if (FAILED(hr))
		{
			type->Release();
			return Failed("CreateVideoType", hr);
		}
		*out = type;
		return S_OK;
	}

	HRESULT SetCaptureStreamAttributes(IMFAttributes* attrs)
	{
		CAMERA_RETURN_IF_FAILED(
		    "STREAM_CATEGORY",
		    attrs->SetGUID(CameraMF_DEVICESTREAM_STREAM_CATEGORY, CameraMF_PINNAME_VIDEO_CAPTURE));
		CAMERA_RETURN_IF_FAILED("STREAM_ID", attrs->SetUINT32(CameraMF_DEVICESTREAM_STREAM_ID, 0));
		CAMERA_RETURN_IF_FAILED("FRAMESERVER_SHARED",
		                        attrs->SetUINT32(CameraMF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
		CAMERA_RETURN_IF_FAILED("FRAMESOURCE_TYPES",
		                        attrs->SetUINT32(CameraMF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
		                                         CameraMF_FrameSourceTypes_Color));
		return S_OK;
	}

	/* the Legacy camera profile (any resolution, up to 30 fps, any subtype) for stream 0;
	 * looked up at run time in mfsensorgroup.dll, a failure is logged, not fatal */
	void AddSensorProfiles(IMFAttributes* attrs)
	{
		using CreateCollectionFn = HRESULT(WINAPI*)(CameraSensorProfileCollection**);
		using CreateProfileFn = HRESULT(WINAPI*)(REFGUID, UINT32, LPCWSTR, CameraSensorProfile**);
		HMODULE module =
		    LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		auto createCollection =
		    module ? reinterpret_cast<CreateCollectionFn>(reinterpret_cast<void*>(
		                 GetProcAddress(module, "MFCreateSensorProfileCollection")))
		           : nullptr;
		auto createProfile = module ? reinterpret_cast<CreateProfileFn>(reinterpret_cast<void*>(
		                                  GetProcAddress(module, "MFCreateSensorProfile")))
		                            : nullptr;
		if (!createCollection || !createProfile)
		{
			Log("sensor profiles: mfsensorgroup.dll has no profile functions");
			return;
		}
		CameraSensorProfileCollection* collection = nullptr;
		CameraSensorProfile* profile = nullptr;
		HRESULT hr = Failed("MFCreateSensorProfileCollection", createCollection(&collection));
		if (SUCCEEDED(hr))
			hr = Failed("MFCreateSensorProfile",
			            createProfile(CameraMF_KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
		if (SUCCEEDED(hr))
			hr = Failed("AddProfileFilter",
			            profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))"));
		if (SUCCEEDED(hr))
			hr = Failed("AddProfile", collection->AddProfile(profile));
		if (SUCCEEDED(hr))
			(void)Failed(
			    "SENSORPROFILE_COLLECTION",
			    attrs->SetUnknown(CameraMF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection));
		SafeRelease(profile);
		SafeRelease(collection);
	}

	/* black: Y 16, U and V 128 */
	void FillBlack(BYTE* dst, LONG pitch, UINT32 width, UINT32 height)
	{
		for (UINT32 y = 0; y < height; y++)
			memset(dst + (size_t)y * (size_t)pitch, 16, width);
		for (UINT32 y = 0; y < height / 2; y++)
			memset(dst + (size_t)(height + y) * (size_t)pitch, 128, width);
	}

	class Source;

	/* ------------------------------------------------------------------ the stream */

	class Stream final : public CameraMediaStream2
	{
	  public:
		static HRESULT Create(Source* source, IMFStreamDescriptor* descriptor,
		                      ShadowCameraRing* ring, const Format& format, Stream** out);

		/* IUnknown */
		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if ((riid == __uuidof(IUnknown)) || (riid == __uuidof(IMFMediaEventGenerator)) ||
			    (riid == __uuidof(IMFMediaStream)) || (riid == CameraMF_IID_IMFMediaStream2))
			{
				*ppv = static_cast<CameraMediaStream2*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		STDMETHODIMP_(ULONG) AddRef() override
		{
			return (ULONG)InterlockedIncrement(&refs_);
		}
		STDMETHODIMP_(ULONG) Release() override
		{
			const LONG refs = InterlockedDecrement(&refs_);
			if (refs == 0)
				delete this;
			return (ULONG)refs;
		}

		/* IMFMediaEventGenerator */
		STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override
		{
			IMFMediaEventQueue* queue = nullptr;
			{
				AutoLock lock(&cs_);
				if (!queue_)
					return MF_E_SHUTDOWN;
				queue = queue_;
				queue->AddRef();
			}
			const HRESULT hr = queue->GetEvent(flags, event);
			queue->Release();
			return hr;
		}
		STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override
		{
			AutoLock lock(&cs_);
			return queue_ ? queue_->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
		}
		STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override
		{
			AutoLock lock(&cs_);
			return queue_ ? queue_->EndGetEvent(result, event) : MF_E_SHUTDOWN;
		}
		STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
		                        const PROPVARIANT* value) override
		{
			AutoLock lock(&cs_);
			return queue_ ? queue_->QueueEventParamVar(type, extended, status, value)
			              : MF_E_SHUTDOWN;
		}

		/* IMFMediaStream */
		STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
		STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override
		{
			if (!descriptor)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			*descriptor = descriptor_;
			descriptor_->AddRef();
			return S_OK;
		}
		/* queued; the stream's thread answers at the frame rate */
		STDMETHODIMP RequestSample(IUnknown* token) override
		{
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			if (state_ != MF_STREAM_STATE_RUNNING)
				return MF_E_MEDIA_SOURCE_WRONGSTATE;
			if (pending_ >= kMaxPending)
				return Failed("RequestSample: too many pending", MF_E_NOTACCEPTING);
			if (token)
				token->AddRef();
			tokens_[(head_ + pending_) % kMaxPending] = token;
			pending_++;
			(void)SetEvent(wake_);
			return S_OK;
		}

		/* IMFMediaStream2: Frame Server sets the state of each stream itself */
		STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override
		{
			AutoLock lock(&cs_);
			Log("stream %p SetStreamState(%d) from %d", (void*)this, (int)value, (int)state_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			switch (value)
			{
				case MF_STREAM_STATE_RUNNING:
					return RunLocked();
				case MF_STREAM_STATE_PAUSED:
				case MF_STREAM_STATE_STOPPED:
					StopLocked(value);
					return S_OK;
				default:
					return MF_E_INVALID_STATE_TRANSITION;
			}
		}
		STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override
		{
			if (!value)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			*value = state_;
			return S_OK;
		}

		/* the source's calls, with its lock held */
		HRESULT Start()
		{
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			CAMERA_RETURN_IF_FAILED("stream run", RunLocked());
			return Failed("MEStreamStarted",
			              queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
		}
		HRESULT Stop()
		{
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			StopLocked(MF_STREAM_STATE_STOPPED);
			return Failed("MEStreamStopped",
			              queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
		}
		void Shutdown();
		HRESULT GetAttributes(IMFAttributes** out)
		{
			AutoLock lock(&cs_);
			if (!attributes_)
				return MF_E_SHUTDOWN;
			*out = attributes_;
			attributes_->AddRef();
			return S_OK;
		}
		HRESULT SetAllocator(IMFVideoSampleAllocator* allocator)
		{
			AutoLock lock(&cs_);
			if (!queue_)
				return MF_E_SHUTDOWN;
			if (state_ == MF_STREAM_STATE_RUNNING)
				return Failed("SetDefaultAllocator while running", MF_E_INVALIDREQUEST);
			SafeRelease(allocator_);
			allocator_ = allocator;
			if (allocator_)
				allocator_->AddRef();
			allocatorReady_ = false;
			return S_OK;
		}

	  private:
		Stream(Source* source, IMFStreamDescriptor* descriptor, ShadowCameraRing* ring,
		       const Format& format);
		~Stream();

		static DWORD WINAPI ThreadMain(LPVOID arg)
		{
			static_cast<Stream*>(arg)->Serve();
			return 0;
		}

		/* cs_ held */
		HRESULT RunLocked()
		{
			if (state_ == MF_STREAM_STATE_RUNNING)
				return S_OK;
			if (allocator_ && !allocatorReady_)
			{
				CAMERA_RETURN_IF_FAILED("InitializeSampleAllocator",
				                        allocator_->InitializeSampleAllocator(10, type_));
				allocatorReady_ = true;
			}
			state_ = MF_STREAM_STATE_RUNNING;
			nextDue_ = 0;
			shadow_camera_ring_reader(ring_, TRUE);
			Log("stream %p running, %ux%u at %u fps, ring %s, allocator %s", (void*)this,
			    format_.width, format_.height, format_.fps, ring_ ? "open" : "absent (black)",
			    allocator_ ? "provided" : "own");
			return S_OK;
		}
		void StopLocked(MF_STREAM_STATE state)
		{
			if (state_ == MF_STREAM_STATE_RUNNING)
				Log("stream %p stops after %llu samples", (void*)this,
				    (unsigned long long)delivered_);
			state_ = state;
			shadow_camera_ring_reader(ring_, FALSE);
			while (pending_ > 0)
			{
				SafeRelease(tokens_[head_]);
				head_ = (head_ + 1) % kMaxPending;
				pending_--;
			}
		}

		void Serve();
		HRESULT DeliverLocked();
		HRESULT FillSample(IMFSample* sample);

		volatile LONG refs_ = 1;
		CRITICAL_SECTION cs_;
		IMFMediaSourceEx* source_ = nullptr; /* strong, until Shutdown */
		IMFStreamDescriptor* descriptor_ = nullptr;
		IMFMediaEventQueue* queue_ = nullptr;
		IMFAttributes* attributes_ = nullptr;
		IMFMediaType* type_ = nullptr;
		IMFVideoSampleAllocator* allocator_ = nullptr; /* Frame Server's, when it gives one */
		bool allocatorReady_ = false;
		ShadowCameraRing* ring_ = nullptr; /* the source's */
		Format format_ = {};
		MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
		IUnknown* tokens_[kMaxPending] = {};
		ULONG head_ = 0;
		ULONG pending_ = 0;
		LONGLONG nextDue_ = 0; /* MFGetSystemTime of the next sample */
		UINT64 delivered_ = 0;
		UINT64 lastFrame_ = 0;
		HANDLE wake_ = nullptr;
		HANDLE stop_ = nullptr;
		HANDLE thread_ = nullptr;
	};

	/* ------------------------------------------------------------------ the source */

	class Source final : public IMFMediaSourceEx,
	                     public IMFGetService,
	                     public IKsControl,
	                     public CameraSampleAllocatorControl
	{
	  public:
		static HRESULT Create(IMFAttributes* activateAttributes, Source** out)
		{
			Source* source = new (std::nothrow) Source();
			if (!source)
				return E_OUTOFMEMORY;
			const HRESULT hr = source->Initialize(activateAttributes);
			if (FAILED(hr))
			{
				(void)source->Shutdown();
				source->Release();
				return Failed("Source::Initialize", hr);
			}
			*out = source;
			return S_OK;
		}

		/* IUnknown */
		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if ((riid == __uuidof(IUnknown)) || (riid == __uuidof(IMFMediaEventGenerator)) ||
			    (riid == __uuidof(IMFMediaSource)) || (riid == __uuidof(IMFMediaSourceEx)))
				*ppv = static_cast<IMFMediaSourceEx*>(this);
			else if (riid == __uuidof(IMFGetService))
				*ppv = static_cast<IMFGetService*>(this);
			else if (riid == CameraMF_IID_IKsControl)
				*ppv = static_cast<IKsControl*>(this);
			else if (riid == CameraMF_IID_IMFSampleAllocatorControl)
				*ppv = static_cast<CameraSampleAllocatorControl*>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		STDMETHODIMP_(ULONG) AddRef() override
		{
			return (ULONG)InterlockedIncrement(&refs_);
		}
		STDMETHODIMP_(ULONG) Release() override
		{
			const LONG refs = InterlockedDecrement(&refs_);
			if (refs == 0)
				delete this;
			return (ULONG)refs;
		}

		/* IMFMediaEventGenerator */
		STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override
		{
			IMFMediaEventQueue* queue = nullptr;
			{
				AutoLock lock(&cs_);
				if (shutdown_)
					return MF_E_SHUTDOWN;
				queue = queue_;
				queue->AddRef();
			}
			const HRESULT hr = queue->GetEvent(flags, event);
			queue->Release();
			return hr;
		}
		STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override
		{
			AutoLock lock(&cs_);
			return shutdown_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(callback, state);
		}
		STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override
		{
			AutoLock lock(&cs_);
			return shutdown_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(result, event);
		}
		STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
		                        const PROPVARIANT* value) override
		{
			AutoLock lock(&cs_);
			return shutdown_ ? MF_E_SHUTDOWN
			                 : queue_->QueueEventParamVar(type, extended, status, value);
		}

		/* IMFMediaSource */
		STDMETHODIMP GetCharacteristics(DWORD* characteristics) override
		{
			if (!characteristics)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			*characteristics = MFMEDIASOURCE_IS_LIVE;
			return S_OK;
		}
		STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override
		{
			if (!out)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			return presentation_->Clone(out);
		}
		STDMETHODIMP Start(IMFPresentationDescriptor* presentation, const GUID* timeFormat,
		                   const PROPVARIANT*) override
		{
			if (!presentation)
				return E_INVALIDARG;
			if (timeFormat && (*timeFormat != GUID_NULL))
				return MF_E_UNSUPPORTED_TIME_FORMAT;
			AutoLock lock(&cs_);
			if (shutdown_)
				return Failed("Start after Shutdown", MF_E_SHUTDOWN);

			DWORD count = 0;
			CAMERA_RETURN_IF_FAILED("GetStreamDescriptorCount",
			                        presentation->GetStreamDescriptorCount(&count));
			if (count != 1)
				return E_INVALIDARG;
			BOOL selected = FALSE;
			IMFStreamDescriptor* descriptor = nullptr;
			CAMERA_RETURN_IF_FAILED(
			    "GetStreamDescriptorByIndex",
			    presentation->GetStreamDescriptorByIndex(0, &selected, &descriptor));
			DWORD id = 0;
			const HRESULT idHr = descriptor->GetStreamIdentifier(&id);
			descriptor->Release();
			CAMERA_RETURN_IF_FAILED("GetStreamIdentifier", idHr);
			if (id != 0)
				return E_INVALIDARG;
			Log("source %p Start, stream selected %d", (void*)this, selected);

			if (selected)
			{
				CAMERA_RETURN_IF_FAILED("MENewStream",
				                        queue_->QueueEventParamUnk(
				                            announced_ ? MEUpdatedStream : MENewStream, GUID_NULL,
				                            S_OK, static_cast<IMFMediaStream*>(stream_)));
				announced_ = true;
				CAMERA_RETURN_IF_FAILED("stream Start", stream_->Start());
			}

			PROPVARIANT startTime;
			PropVariantInit(&startTime);
			startTime.vt = VT_I8;
			startTime.hVal.QuadPart = MFGetSystemTime();
			return Failed("MESourceStarted",
			              queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime));
		}
		STDMETHODIMP Stop() override
		{
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			CAMERA_RETURN_IF_FAILED("stream Stop", stream_->Stop());
			return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
		}
		STDMETHODIMP Pause() override
		{
			AutoLock lock(&cs_);
			return shutdown_ ? MF_E_SHUTDOWN : MF_E_INVALID_STATE_TRANSITION;
		}
		STDMETHODIMP Shutdown() override
		{
			Stream* stream = nullptr;
			{
				AutoLock lock(&cs_);
				if (shutdown_)
					return MF_E_SHUTDOWN;
				Log("source %p Shutdown", (void*)this);
				shutdown_ = true;
				if (queue_)
					queue_->Shutdown();
				stream = stream_;
				stream_ = nullptr;
			}
			if (stream)
			{
				stream->Shutdown(); /* gives back its reference to us */
				stream->Release();
			}
			return S_OK;
		}

		/* IMFMediaSourceEx */
		STDMETHODIMP GetSourceAttributes(IMFAttributes** out) override
		{
			if (!out)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			*out = attributes_;
			attributes_->AddRef();
			return S_OK;
		}
		STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** out) override
		{
			if (!out)
				return E_POINTER;
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			if (streamId != 0)
				return MF_E_INVALIDSTREAMNUMBER;
			return stream_->GetAttributes(out);
		}
		STDMETHODIMP SetD3DManager(IUnknown*) override
		{
			AutoLock lock(&cs_);
			return shutdown_ ? MF_E_SHUTDOWN : S_OK; /* system memory samples only */
		}

		/* IMFGetService: none */
		STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* ppv) override
		{
			if (ppv)
				*ppv = nullptr;
			return MF_E_UNSUPPORTED_SERVICE;
		}

		/* IKsControl: no camera controls */
		STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override
		{
			return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
		}
		STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override
		{
			return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
		}
		STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override
		{
			return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
		}

		/* IMFSampleAllocatorControl */
		STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override
		{
			AutoLock lock(&cs_);
			Log("source %p SetDefaultAllocator(%lu, %p)", (void*)this, streamId, (void*)allocator);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			if (streamId != 0)
				return MF_E_INVALIDSTREAMNUMBER;
			if (!allocator)
				return E_POINTER;
			IMFVideoSampleAllocator* video = nullptr;
			CAMERA_RETURN_IF_FAILED("IMFVideoSampleAllocator",
			                        allocator->QueryInterface(IID_PPV_ARGS(&video)));
			const HRESULT hr = stream_->SetAllocator(video);
			video->Release();
			return hr;
		}
		STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, int* usage) override
		{
			AutoLock lock(&cs_);
			if (shutdown_)
				return MF_E_SHUTDOWN;
			if (!inputStreamId || !usage)
				return E_POINTER;
			if (streamId != 0)
				return MF_E_INVALIDSTREAMNUMBER;
			*inputStreamId = streamId;
			*usage = CameraMF_AllocatorUsage_UsesProvidedAllocator;
			return S_OK;
		}

	  private:
		Source()
		{
			InitializeCriticalSection(&cs_);
			(void)InterlockedIncrement(&g_objects);
		}
		~Source()
		{
			if (!shutdown_)
				(void)Shutdown();
			SafeRelease(queue_);
			SafeRelease(presentation_);
			SafeRelease(attributes_);
			shadow_camera_ring_close(ring_);
			DeleteCriticalSection(&cs_);
			(void)InterlockedDecrement(&g_objects);
		}

		HRESULT Initialize(IMFAttributes* activateAttributes)
		{
			/* the ring says the size; without it the default, and black frames */
			DWORD error = 0;
			ring_ = shadow_camera_ring_open(&error);
			Format format = { kDefaultWidth, kDefaultHeight, kDefaultFps };
			if (ring_)
			{
				const ShadowCameraRingHeader* h = shadow_camera_ring_header(ring_);
				format = { h->width, h->height, h->fps };
			}
			Log("source %p: ring %s (error %lu), NV12 %ux%u at %u fps", (void*)this,
			    ring_ ? "open" : "not open", ring_ ? 0ul : error, format.width, format.height,
			    format.fps);

			CAMERA_RETURN_IF_FAILED("MFCreateEventQueue", MFCreateEventQueue(&queue_));
			CAMERA_RETURN_IF_FAILED("MFCreateAttributes", MFCreateAttributes(&attributes_, 8));
			if (activateAttributes)
				CAMERA_RETURN_IF_FAILED("CopyAllItems",
				                        activateAttributes->CopyAllItems(attributes_));
			AddSensorProfiles(attributes_);

			IMFMediaType* type = nullptr;
			CAMERA_RETURN_IF_FAILED("CreateVideoType", CreateVideoType(format, &type));
			IMFStreamDescriptor* descriptor = nullptr;
			IMFMediaTypeHandler* handler = nullptr;
			HRESULT hr = Failed("MFCreateStreamDescriptor",
			                    MFCreateStreamDescriptor(0, 1, &type, &descriptor));
			if (SUCCEEDED(hr))
				hr = descriptor->GetMediaTypeHandler(&handler);
			if (SUCCEEDED(hr))
				hr = handler->SetCurrentMediaType(type);
			if (SUCCEEDED(hr))
				hr = SetCaptureStreamAttributes(descriptor);
			if (SUCCEEDED(hr))
				hr = Failed("MFCreatePresentationDescriptor",
				            MFCreatePresentationDescriptor(1, &descriptor, &presentation_));
			if (SUCCEEDED(hr))
				hr = presentation_->SelectStream(0);
			if (SUCCEEDED(hr))
				hr = Stream::Create(this, descriptor, ring_, format, &stream_);
			SafeRelease(handler);
			SafeRelease(descriptor);
			SafeRelease(type);
			return hr;
		}

		volatile LONG refs_ = 1;
		CRITICAL_SECTION cs_;
		bool shutdown_ = false;
		bool announced_ = false;
		IMFMediaEventQueue* queue_ = nullptr;
		IMFAttributes* attributes_ = nullptr;
		IMFPresentationDescriptor* presentation_ = nullptr;
		Stream* stream_ = nullptr;
		ShadowCameraRing* ring_ = nullptr; /* the streams use it; closed with the source */
	};

	/* ------------------------------------------------------------------ the stream's body */

	HRESULT Stream::Create(Source* source, IMFStreamDescriptor* descriptor, ShadowCameraRing* ring,
	                       const Format& format, Stream** out)
	{
		Stream* stream = new (std::nothrow) Stream(source, descriptor, ring, format);
		if (!stream)
			return E_OUTOFMEMORY;
		HRESULT hr = MFCreateEventQueue(&stream->queue_);
		if (SUCCEEDED(hr))
			hr = MFCreateAttributes(&stream->attributes_, 4);
		if (SUCCEEDED(hr))
			hr = SetCaptureStreamAttributes(stream->attributes_);
		if (SUCCEEDED(hr))
			hr = CreateVideoType(format, &stream->type_);
		if (SUCCEEDED(hr))
		{
			stream->wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			stream->stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!stream->wake_ || !stream->stop_)
				hr = HRESULT_FROM_WIN32(GetLastError());
		}
		if (SUCCEEDED(hr))
		{
			stream->AddRef(); /* the thread's, given back when it ends */
			stream->thread_ = CreateThread(nullptr, 0, ThreadMain, stream, 0, nullptr);
			if (!stream->thread_)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				stream->Release();
			}
		}
		if (FAILED(hr))
		{
			stream->Shutdown();
			stream->Release();
			return Failed("Stream::Create", hr);
		}
		*out = stream;
		return S_OK;
	}

	Stream::Stream(Source* source, IMFStreamDescriptor* descriptor, ShadowCameraRing* ring,
	               const Format& format)
	    : source_(source), descriptor_(descriptor), ring_(ring), format_(format)
	{
		InitializeCriticalSection(&cs_);
		source_->AddRef();
		descriptor_->AddRef();
		(void)InterlockedIncrement(&g_objects);
	}

	Stream::~Stream()
	{
		SafeRelease(type_);
		SafeRelease(descriptor_);
		if (wake_)
			(void)CloseHandle(wake_);
		if (stop_)
			(void)CloseHandle(stop_);
		if (thread_)
			(void)CloseHandle(thread_);
		DeleteCriticalSection(&cs_);
		(void)InterlockedDecrement(&g_objects);
	}

	void Stream::Shutdown()
	{
		IMFMediaSourceEx* source = nullptr;
		{
			AutoLock lock(&cs_);
			StopLocked(MF_STREAM_STATE_STOPPED);
			if (queue_)
				queue_->Shutdown();
			SafeRelease(queue_);
			SafeRelease(attributes_);
			if (allocator_)
				(void)allocator_->UninitializeSampleAllocator();
			SafeRelease(allocator_);
			ring_ = nullptr;
			source = source_;
			source_ = nullptr; /* breaks the source <-> stream cycle */
			if (stop_)
				(void)SetEvent(stop_);
		}
		if (source)
			source->Release();
	}

	STDMETHODIMP Stream::GetMediaSource(IMFMediaSource** source)
	{
		if (!source)
			return E_POINTER;
		AutoLock lock(&cs_);
		if (!source_)
			return MF_E_SHUTDOWN;
		*source = source_;
		source_->AddRef();
		return S_OK;
	}

	/* the stream's thread: one sample per pending request, one frame interval apart */
	void Stream::Serve()
	{
		const LONGLONG interval = 10000000LL / (LONGLONG)(format_.fps ? format_.fps : kDefaultFps);
		HANDLE handles[2] = { stop_, wake_ };
		for (;;)
		{
			DWORD timeout = INFINITE;
			{
				AutoLock lock(&cs_);
				if (!queue_)
					break;
				while ((pending_ > 0) && (state_ == MF_STREAM_STATE_RUNNING))
				{
					const LONGLONG now = MFGetSystemTime();
					if (nextDue_ && (now < nextDue_))
					{
						timeout = (DWORD)((nextDue_ - now) / 10000) + 1;
						break;
					}
					const HRESULT hr = DeliverLocked();
					if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
					{
						timeout = 5; /* Frame Server still holds every sample */
						break;
					}
					nextDue_ = (nextDue_ && (now - nextDue_ < interval)) ? nextDue_ + interval
					                                                     : now + interval;
				}
			}
			if (WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0)
				break;
		}
		Release(); /* the thread's reference */
	}

	/* cs_ held, pending_ > 0 */
	HRESULT Stream::DeliverLocked()
	{
		IMFSample* sample = nullptr;
		HRESULT hr = S_OK;
		if (allocator_)
			hr = allocator_->AllocateSample(&sample);
		else
		{
			IMFMediaBuffer* buffer = nullptr;
			hr = MFCreate2DMediaBuffer(format_.width, format_.height, MFVideoFormat_NV12.Data1,
			                           FALSE, &buffer);
			if (SUCCEEDED(hr))
				hr = MFCreateSample(&sample);
			if (SUCCEEDED(hr))
				hr = sample->AddBuffer(buffer);
			SafeRelease(buffer);
		}
		if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
		{
			SafeRelease(sample);
			return hr;
		}
		if (SUCCEEDED(hr))
			hr = FillSample(sample);
		if (SUCCEEDED(hr))
			hr = sample->SetSampleTime(MFGetSystemTime());
		if (SUCCEEDED(hr))
			hr = sample->SetSampleDuration(10000000LL / (LONGLONG)format_.fps);

		IUnknown* token = tokens_[head_];
		tokens_[head_] = nullptr;
		head_ = (head_ + 1) % kMaxPending;
		pending_--;
		if (SUCCEEDED(hr) && token)
			hr = sample->SetUnknown(CameraMF_SampleExtension_Token, token);
		SafeRelease(token);
		if (SUCCEEDED(hr))
			hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
		SafeRelease(sample);
		if (SUCCEEDED(hr))
			delivered_++;
		return Failed("deliver a sample", hr);
	}

	/* the newest frame of the ring, or black, at the buffer's pitch */
	HRESULT Stream::FillSample(IMFSample* sample)
	{
		IMFMediaBuffer* buffer = nullptr;
		IMF2DBuffer* buffer2d = nullptr;
		HRESULT hr = sample->GetBufferByIndex(0, &buffer);
		BYTE* data = nullptr;
		LONG pitch = 0;
		DWORD capacity = 0;
		const DWORD frameSize = format_.width * format_.height * 3 / 2;
		if (SUCCEEDED(hr) && SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))))
			hr = buffer2d->Lock2D(&data, &pitch);
		else if (SUCCEEDED(hr))
		{
			hr = buffer->Lock(&data, &capacity, nullptr);
			pitch = (LONG)format_.width;
			if (SUCCEEDED(hr) && (capacity < frameSize))
			{
				(void)buffer->Unlock();
				hr = MF_E_BUFFERTOOSMALL;
			}
		}
		if (SUCCEEDED(hr))
		{
			UINT64 frame = 0;
			if ((pitch >= (LONG)format_.width) &&
			    shadow_camera_ring_read(ring_, data, pitch, &frame))
			{
				if (lastFrame_ == 0)
					Log("stream %p: first ring frame %llu", (void*)this, (unsigned long long)frame);
				lastFrame_ = frame;
			}
			else
				FillBlack(data, pitch, format_.width, format_.height);
			if (buffer2d)
				(void)buffer2d->Unlock2D();
			else
				(void)buffer->Unlock();
			DWORD contiguous = frameSize;
			if (buffer2d)
				(void)buffer2d->GetContiguousLength(&contiguous);
			hr = buffer->SetCurrentLength(contiguous);
		}
		SafeRelease(buffer2d);
		SafeRelease(buffer);
		return hr;
	}

	/* ------------------------------------------------------------------ the activate */

	class Activate final : public IMFActivate
	{
	  public:
		Activate()
		{
			InitializeCriticalSection(&cs_);
			(void)InterlockedIncrement(&g_objects);
		}
		HRESULT Initialize()
		{
			return MFCreateAttributes(&attrs_, 4);
		}

		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if ((riid == __uuidof(IUnknown)) || (riid == __uuidof(IMFAttributes)) ||
			    (riid == __uuidof(IMFActivate)))
			{
				*ppv = static_cast<IMFActivate*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		STDMETHODIMP_(ULONG) AddRef() override
		{
			return (ULONG)InterlockedIncrement(&refs_);
		}
		STDMETHODIMP_(ULONG) Release() override
		{
			const LONG refs = InterlockedDecrement(&refs_);
			if (refs == 0)
				delete this;
			return (ULONG)refs;
		}

		/* a new source for every activation */
		STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			*ppv = nullptr;
			LogGuid("ActivateObject", riid);
			AutoLock lock(&cs_);
			Source* source = nullptr;
			CAMERA_RETURN_IF_FAILED("Source::Create", Source::Create(attrs_, &source));
			const HRESULT hr = source->QueryInterface(riid, ppv);
			if (FAILED(hr))
				(void)source->Shutdown();
			source->Release();
			return Failed("ActivateObject", hr);
		}
		/* "must return E_NOTIMPL" (Frame Server Custom Media Source, IMFActivate) */
		STDMETHODIMP ShutdownObject() override
		{
			return E_NOTIMPL;
		}
		STDMETHODIMP DetachObject() override
		{
			return E_NOTIMPL;
		}

		/* IMFAttributes */
		STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override
		{
			return attrs_->GetItem(key, value);
		}
		STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override
		{
			return attrs_->GetItemType(key, type);
		}
		STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override
		{
			return attrs_->CompareItem(key, value, result);
		}
		STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type,
		                     BOOL* result) override
		{
			return attrs_->Compare(theirs, type, result);
		}
		STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override
		{
			return attrs_->GetUINT32(key, value);
		}
		STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override
		{
			return attrs_->GetUINT64(key, value);
		}
		STDMETHODIMP GetDouble(REFGUID key, double* value) override
		{
			return attrs_->GetDouble(key, value);
		}
		STDMETHODIMP GetGUID(REFGUID key, GUID* value) override
		{
			return attrs_->GetGUID(key, value);
		}
		STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override
		{
			return attrs_->GetStringLength(key, length);
		}
		STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override
		{
			return attrs_->GetString(key, value, size, length);
		}
		STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override
		{
			return attrs_->GetAllocatedString(key, value, length);
		}
		STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override
		{
			return attrs_->GetBlobSize(key, size);
		}
		STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* written) override
		{
			return attrs_->GetBlob(key, buffer, size, written);
		}
		STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override
		{
			return attrs_->GetAllocatedBlob(key, buffer, size);
		}
		STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override
		{
			return attrs_->GetUnknown(key, riid, ppv);
		}
		STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override
		{
			return attrs_->SetItem(key, value);
		}
		STDMETHODIMP DeleteItem(REFGUID key) override
		{
			return attrs_->DeleteItem(key);
		}
		STDMETHODIMP DeleteAllItems() override
		{
			return attrs_->DeleteAllItems();
		}
		STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override
		{
			return attrs_->SetUINT32(key, value);
		}
		STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override
		{
			return attrs_->SetUINT64(key, value);
		}
		STDMETHODIMP SetDouble(REFGUID key, double value) override
		{
			return attrs_->SetDouble(key, value);
		}
		STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override
		{
			return attrs_->SetGUID(key, value);
		}
		STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override
		{
			return attrs_->SetString(key, value);
		}
		STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override
		{
			return attrs_->SetBlob(key, buffer, size);
		}
		STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override
		{
			return attrs_->SetUnknown(key, value);
		}
		STDMETHODIMP LockStore() override
		{
			return attrs_->LockStore();
		}
		STDMETHODIMP UnlockStore() override
		{
			return attrs_->UnlockStore();
		}
		STDMETHODIMP GetCount(UINT32* count) override
		{
			return attrs_->GetCount(count);
		}
		STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override
		{
			return attrs_->GetItemByIndex(index, key, value);
		}
		STDMETHODIMP CopyAllItems(IMFAttributes* dest) override
		{
			return attrs_->CopyAllItems(dest);
		}

	  private:
		~Activate()
		{
			SafeRelease(attrs_);
			DeleteCriticalSection(&cs_);
			(void)InterlockedDecrement(&g_objects);
		}

		volatile LONG refs_ = 1;
		CRITICAL_SECTION cs_;
		IMFAttributes* attrs_ = nullptr;
	};

	/* ------------------------------------------------------------------ the class factory */

	class ClassFactory final : public IClassFactory
	{
	  public:
		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if ((riid == __uuidof(IUnknown)) || (riid == __uuidof(IClassFactory)))
			{
				*ppv = static_cast<IClassFactory*>(this);
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		/* one static instance */
		STDMETHODIMP_(ULONG) AddRef() override
		{
			return 2;
		}
		STDMETHODIMP_(ULONG) Release() override
		{
			return 1;
		}
		STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			*ppv = nullptr;
			if (outer)
				return CLASS_E_NOAGGREGATION;
			Activate* activate = new (std::nothrow) Activate();
			if (!activate)
				return E_OUTOFMEMORY;
			HRESULT hr = activate->Initialize();
			if (SUCCEEDED(hr))
				hr = activate->QueryInterface(riid, ppv);
			activate->Release();
			return Failed("CreateInstance", hr);
		}
		STDMETHODIMP LockServer(BOOL lock) override
		{
			if (lock)
				(void)InterlockedIncrement(&g_locks);
			else
				(void)InterlockedDecrement(&g_locks);
			return S_OK;
		}
	};

	ClassFactory g_factory;

	const WCHAR kClassKey[] = L"SOFTWARE\\Classes\\CLSID\\" CAMERA_MF_CLSID_SOURCE_TEXT;

	LONG SetString(HKEY key, const WCHAR* name, const WCHAR* value)
	{
		const DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(WCHAR));
		return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
	}
} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = instance;
		(void)DisableThreadLibraryCalls(instance);
		LogOpen();
		WCHAR image[MAX_PATH] = {};
		(void)GetModuleFileNameW(nullptr, image, MAX_PATH);
		DWORD session = 0;
		(void)ProcessIdToSessionId(GetCurrentProcessId(), &session);
		char imageText[MAX_PATH * 2] = {};
		(void)WideCharToMultiByte(CP_UTF8, 0, image, -1, imageText, (int)sizeof(imageText), nullptr,
		                          nullptr);
		Log("loaded by %s, session %lu", imageText, session);
	}
	else if ((reason == DLL_PROCESS_DETACH) && !reserved && (g_logFile != INVALID_HANDLE_VALUE))
	{
		(void)CloseHandle(g_logFile);
		g_logFile = INVALID_HANDLE_VALUE;
	}
	return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = nullptr;
	if (clsid != CameraMF_CLSID_Source)
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_factory.QueryInterface(riid, ppv);
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void)
{
	return ((g_objects == 0) && (g_locks == 0)) ? S_OK : S_FALSE;
}

/* the shadow server registers the class itself (sink_win.c); these are for regsvr32 */
extern "C" HRESULT STDAPICALLTYPE DllRegisterServer(void)
{
	WCHAR path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
	if ((length == 0) || (length >= MAX_PATH))
		return HRESULT_FROM_WIN32(GetLastError());
	HKEY classKey = nullptr;
	LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0, nullptr, 0, KEY_WRITE, nullptr,
	                          &classKey, nullptr);
	if (rc != ERROR_SUCCESS)
		return HRESULT_FROM_WIN32(rc);
	rc = SetString(classKey, nullptr, L"FreeRDP shadow camera source");
	HKEY serverKey = nullptr;
	if (rc == ERROR_SUCCESS)
		rc = RegCreateKeyExW(classKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr,
		                     &serverKey, nullptr);
	if (rc == ERROR_SUCCESS)
		rc = SetString(serverKey, nullptr, path);
	if (rc == ERROR_SUCCESS)
		rc = SetString(serverKey, L"ThreadingModel", L"Both");
	if (serverKey)
		(void)RegCloseKey(serverKey);
	(void)RegCloseKey(classKey);
	return HRESULT_FROM_WIN32(rc);
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer(void)
{
	const LONG rc = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClassKey);
	return ((rc == ERROR_SUCCESS) || (rc == ERROR_FILE_NOT_FOUND)) ? S_OK : HRESULT_FROM_WIN32(rc);
}
