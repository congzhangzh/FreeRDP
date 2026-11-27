/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Shadow server virtual camera: the Media Foundation names the toolchains may lack
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
 * mingw-w64 (llvm-mingw 20261020) has no mfvirtualcamera.h and none of the Frame Server
 * custom media source names (IMFMediaStream2, IMFSampleAllocatorControl, the
 * MF_DEVICESTREAM_* attributes, camera profiles). They are declared here under names of our
 * own, so that they never clash with a toolchain that has them, with the values of the
 * Windows SDK 10.0.26100 (mfidl.h, mfapi.h, mfvirtualcamera.h, shared/ksmedia.h). The
 * methods are in the SDK's order: the interfaces are called through these tables.
 */

#ifndef FREERDP_SERVER_SHADOW_CAMERA_MF_IDS_H
#define FREERDP_SERVER_SHADOW_CAMERA_MF_IDS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfidl.h>
#include <mfobjects.h>

#if defined(__GNUC__) || defined(__clang__)
#define CAMERA_MF_UNUSED __attribute__((unused))
#else
#define CAMERA_MF_UNUSED
#endif

#define CAMERA_MF_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	static const GUID name CAMERA_MF_UNUSED = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

#ifdef __cplusplus
extern "C"
{
#endif

	/* --- the media source's COM class, registered by the shadow server */
	/* {1C540942-3127-4447-B8D8-ECF6FAA0B2CB} */
	CAMERA_MF_GUID(CameraMF_CLSID_Source, 0x1c540942, 0x3127, 0x4447, 0xb8, 0xd8, 0xec, 0xf6, 0xfa,
	               0xa0, 0xb2, 0xcb);
#define CAMERA_MF_CLSID_SOURCE_TEXT L"{1C540942-3127-4447-B8D8-ECF6FAA0B2CB}"
#define CAMERA_MF_SOURCE_DLL L"freerdp-shadow-camera-source.dll"

	/* --- interfaces (mfidl.h, ksproxy.h) */
	CAMERA_MF_GUID(CameraMF_IID_IMFMediaStream2, 0xC5BC37D6, 0x75C7, 0x46A1, 0xA1, 0x32, 0x81, 0xB5,
	               0xF7, 0x23, 0xC2, 0x0F);
	CAMERA_MF_GUID(CameraMF_IID_IMFSampleAllocatorControl, 0xDA62B958, 0x3A38, 0x4A97, 0xBD, 0x27,
	               0x14, 0x9C, 0x64, 0x0C, 0x07, 0x71);
	CAMERA_MF_GUID(CameraMF_IID_IMFSensorProfile, 0x22F765D1, 0x8DAB, 0x4107, 0x84, 0x6D, 0x56,
	               0xBA, 0xF7, 0x22, 0x15, 0xE7);
	CAMERA_MF_GUID(CameraMF_IID_IMFSensorProfileCollection, 0xC95EA55B, 0x0187, 0x48BE, 0x93, 0x53,
	               0x8D, 0x25, 0x07, 0x66, 0x23, 0x51);
	CAMERA_MF_GUID(CameraMF_IID_IMFVirtualCamera, 0x1C08A864, 0xEF6C, 0x4C75, 0xAF, 0x59, 0x5F,
	               0x2D, 0x68, 0xDA, 0x95, 0x63);
	CAMERA_MF_GUID(CameraMF_IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00,
	               0x00, 0x00, 0x46);
	CAMERA_MF_GUID(CameraMF_IID_IMFAsyncCallback, 0xa27003cf, 0x2354, 0x4f2a, 0x8d, 0x6a, 0xab,
	               0x7c, 0xff, 0x15, 0x43, 0x7e);
	CAMERA_MF_GUID(CameraMF_IID_IMFMediaEvent, 0xDF598932, 0xF10C, 0x4E39, 0xBB, 0xA2, 0xC3, 0x08,
	               0xF1, 0x01, 0xDA, 0xA3);
	CAMERA_MF_GUID(CameraMF_IID_IKsControl, 0x28F54685, 0x06FD, 0x11D2, 0xB2, 0x7A, 0x00, 0xA0,
	               0xC9, 0x22, 0x31, 0x96);

	/* --- attributes (mfidl.h, mfapi.h, ksmedia.h) */
	CAMERA_MF_GUID(CameraMF_DEVICESTREAM_STREAM_CATEGORY, 0x2939e7b8, 0xa62e, 0x4579, 0xb6, 0x74,
	               0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba);
	CAMERA_MF_GUID(CameraMF_DEVICESTREAM_STREAM_ID, 0x11bd5120, 0xd124, 0x446b, 0x88, 0xe6, 0x17,
	               0x06, 0x02, 0x57, 0xff, 0xf9);
	CAMERA_MF_GUID(CameraMF_DEVICESTREAM_FRAMESERVER_SHARED, 0x1CB378E9, 0xB279, 0x41D4, 0xAF, 0x97,
	               0x34, 0xA2, 0x43, 0xE6, 0x83, 0x20);
	CAMERA_MF_GUID(CameraMF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, 0x17145fd1, 0x1b2b, 0x423c,
	               0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88);
	CAMERA_MF_GUID(CameraMF_DEVICEMFT_SENSORPROFILE_COLLECTION, 0x36EBDC44, 0xB12C, 0x441B, 0x89,
	               0xF4, 0x08, 0xB2, 0xF4, 0x1A, 0x9C, 0xFC);
	CAMERA_MF_GUID(CameraMF_PINNAME_VIDEO_CAPTURE, 0xFB6C4281, 0x0353, 0x11d1, 0x90, 0x5F, 0x00,
	               0x00, 0xC0, 0xCC, 0x16, 0xBA);
	CAMERA_MF_GUID(CameraMF_KSCAMERAPROFILE_Legacy, 0xB4894D81, 0x62B7, 0x4EEC, 0x87, 0x40, 0x80,
	               0x65, 0x8C, 0x4A, 0x9D, 0x3E);
	CAMERA_MF_GUID(CameraMF_SampleExtension_Token, 0x8294da66, 0xf328, 0x4805, 0xb5, 0x51, 0x00,
	               0xde, 0xb4, 0xc5, 0x7a, 0x61);
	CAMERA_MF_GUID(CameraMF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, 0x58f0aad8,
	               0x22bf, 0x4f8a, 0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f);

	/* --- the virtual camera's events (mfvirtualcamera.h), MEExtendedType */
	CAMERA_MF_GUID(CameraMF_VCAMEVENT_SOURCE_INITIALIZE, 0xE52C4DFF, 0xE46D, 0x4D0B, 0xBC, 0x75,
	               0xDD, 0xD4, 0xC8, 0x72, 0x3F, 0x96);
	CAMERA_MF_GUID(CameraMF_VCAMEVENT_SOURCE_START, 0xB1EEB989, 0xB456, 0x4F4A, 0xAE, 0x40, 0x07,
	               0x9C, 0x28, 0xE2, 0x4A, 0xF8);
	CAMERA_MF_GUID(CameraMF_VCAMEVENT_SOURCE_STOP, 0xB7FE7A61, 0xFE91, 0x415E, 0x86, 0x08, 0xD3,
	               0x7D, 0xED, 0xB1, 0xA5, 0x8B);
	CAMERA_MF_GUID(CameraMF_VCAMEVENT_SOURCE_UNINITIALIZE, 0xA0EBABA7, 0xA422, 0x4E33, 0x84, 0x01,
	               0xB3, 0x7D, 0x28, 0x00, 0xAA, 0x67);
	CAMERA_MF_GUID(CameraMF_VCAMEVENT_PIPELINE_SHUTDOWN, 0x45A81B31, 0x43F8, 0x4E5D, 0x8C, 0xE2,
	               0x22, 0xDC, 0xE0, 0x26, 0x99, 0x6D);

	/* --- enumerations (mfapi.h, mfidl.h, mfvirtualcamera.h) */
	enum
	{
		CameraMF_FrameSourceTypes_Color = 0x0001,
		CameraMF_AllocatorUsage_UsesProvidedAllocator = 0,
		CameraMF_VirtualCameraType_SoftwareCameraSource = 0,
		CameraMF_VirtualCameraLifetime_Session = 0,
		CameraMF_VirtualCameraLifetime_System = 1,
		CameraMF_VirtualCameraAccess_CurrentUser = 0,
		CameraMF_VirtualCameraAccess_AllUsers = 1
	};

	/* --- IMFVirtualCamera (mfvirtualcamera.h): IMFAttributes, then its own methods */
	typedef struct CameraMFVirtualCamera CameraMFVirtualCamera;
	typedef struct
	{
		HRESULT(STDMETHODCALLTYPE* QueryInterface)(CameraMFVirtualCamera* This, REFIID riid,
		                                           void** ppv);
		ULONG(STDMETHODCALLTYPE* AddRef)(CameraMFVirtualCamera* This);
		ULONG(STDMETHODCALLTYPE* Release)(CameraMFVirtualCamera* This);
		/* IMFAttributes: 30 methods, of which only GetAllocatedString is used here */
		void* GetItem;
		void* GetItemType;
		void* CompareItem;
		void* Compare;
		void* GetUINT32;
		void* GetUINT64;
		void* GetDouble;
		void* GetGUID;
		void* GetStringLength;
		void* GetString;
		HRESULT(STDMETHODCALLTYPE* GetAllocatedString)(CameraMFVirtualCamera* This, REFGUID key,
		                                               LPWSTR* value, UINT32* length);
		void* GetBlobSize;
		void* GetBlob;
		void* GetAllocatedBlob;
		void* GetUnknown;
		void* SetItem;
		void* DeleteItem;
		void* DeleteAllItems;
		void* SetUINT32;
		void* SetUINT64;
		void* SetDouble;
		void* SetGUID;
		void* SetString;
		void* SetBlob;
		void* SetUnknown;
		void* LockStore;
		void* UnlockStore;
		void* GetCount;
		void* GetItemByIndex;
		void* CopyAllItems;
		/* IMFVirtualCamera */
		void* AddDeviceSourceInfo;
		void* AddProperty;
		void* AddRegistryEntry;
		HRESULT(STDMETHODCALLTYPE* Start)(CameraMFVirtualCamera* This, IMFAsyncCallback* callback);
		HRESULT(STDMETHODCALLTYPE* Stop)(CameraMFVirtualCamera* This);
		HRESULT(STDMETHODCALLTYPE* Remove)(CameraMFVirtualCamera* This);
		void* GetMediaSource;
		void* SendCameraProperty;
		void* CreateSyncEvent;
		void* CreateSyncSemaphore;
		HRESULT(STDMETHODCALLTYPE* Shutdown)(CameraMFVirtualCamera* This);
	} CameraMFVirtualCameraVtbl;

	struct CameraMFVirtualCamera
	{
		const CameraMFVirtualCameraVtbl* lpVtbl;
	};

	/** MFCreateVirtualCamera (mfsensorgroup.dll, Windows build 22000 and later) */
	typedef HRESULT(STDAPICALLTYPE* CameraMFCreateVirtualCameraFn)(
	    int type, int lifetime, int access, LPCWSTR friendlyName, LPCWSTR sourceId,
	    const GUID* categories, ULONG categoryCount, CameraMFVirtualCamera** camera);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_CAMERA_MF_IDS_H */
