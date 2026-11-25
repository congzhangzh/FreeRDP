/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/print.h>
#include <winpr/library.h>
#include <freerdp/log.h>
#include <freerdp/types.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>

#include "win_dxgi.h"

#define TAG SERVER_TAG("shadow.win")

#ifdef WITH_DXGI_1_2

static D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
	                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1 };

static UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

static HMODULE d3d11_module = nullptr;

typedef HRESULT(WINAPI* fnD3D11CreateDevice)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                             HMODULE Software, UINT Flags,
                                             CONST D3D_FEATURE_LEVEL* pFeatureLevels,
                                             UINT FeatureLevels, UINT SDKVersion,
                                             ID3D11Device** ppDevice,
                                             D3D_FEATURE_LEVEL* pFeatureLevel,
                                             ID3D11DeviceContext** ppImmediateContext);

static fnD3D11CreateDevice pfnD3D11CreateDevice = nullptr;

#undef DEFINE_GUID
#define INITGUID

#include <initguid.h>

/* d3d11.h GUIDs */

DEFINE_GUID(IID_ID3D11DeviceChild, 0x1841e5c8, 0x16b0, 0x489b, 0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5,
            0xde, 0xae);
DEFINE_GUID(IID_ID3D11DepthStencilState, 0x03823efb, 0x8d8f, 0x4e1c, 0x9a, 0xa2, 0xf6, 0x4b, 0xb2,
            0xcb, 0xfd, 0xf1);
DEFINE_GUID(IID_ID3D11BlendState, 0x75b68faa, 0x347d, 0x4159, 0x8f, 0x45, 0xa0, 0x64, 0x0f, 0x01,
            0xcd, 0x9a);
DEFINE_GUID(IID_ID3D11RasterizerState, 0x9bb4ab81, 0xab1a, 0x4d8f, 0xb5, 0x06, 0xfc, 0x04, 0x20,
            0x0b, 0x6e, 0xe7);
DEFINE_GUID(IID_ID3D11Resource, 0xdc8e63f3, 0xd12b, 0x4952, 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a,
            0x86, 0x2d);
DEFINE_GUID(IID_ID3D11Buffer, 0x48570b85, 0xd1ee, 0x4fcd, 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0,
            0x37);
DEFINE_GUID(IID_ID3D11Texture1D, 0xf8fb5c27, 0xc6b3, 0x4f75, 0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef,
            0x56, 0x4c);
DEFINE_GUID(IID_ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3,
            0x4f, 0x9c);
DEFINE_GUID(IID_ID3D11Texture3D, 0x037e866e, 0xf56d, 0x4357, 0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e,
            0x25, 0x0e);
DEFINE_GUID(IID_ID3D11View, 0x839d1216, 0xbb2e, 0x412b, 0xb7, 0xf4, 0xa9, 0xdb, 0xeb, 0xe0, 0x8e,
            0xd1);
DEFINE_GUID(IID_ID3D11ShaderResourceView, 0xb0e06fe0, 0x8192, 0x4e1a, 0xb1, 0xca, 0x36, 0xd7, 0x41,
            0x47, 0x10, 0xb2);
DEFINE_GUID(IID_ID3D11RenderTargetView, 0xdfdba067, 0x0b8d, 0x4865, 0x87, 0x5b, 0xd7, 0xb4, 0x51,
            0x6c, 0xc1, 0x64);
DEFINE_GUID(IID_ID3D11DepthStencilView, 0x9fdac92a, 0x1876, 0x48c3, 0xaf, 0xad, 0x25, 0xb9, 0x4f,
            0x84, 0xa9, 0xb6);
DEFINE_GUID(IID_ID3D11UnorderedAccessView, 0x28acf509, 0x7f5c, 0x48f6, 0x86, 0x11, 0xf3, 0x16, 0x01,
            0x0a, 0x63, 0x80);
DEFINE_GUID(IID_ID3D11VertexShader, 0x3b301d64, 0xd678, 0x4289, 0x88, 0x97, 0x22, 0xf8, 0x92, 0x8b,
            0x72, 0xf3);
DEFINE_GUID(IID_ID3D11HullShader, 0x8e5c6061, 0x628a, 0x4c8e, 0x82, 0x64, 0xbb, 0xe4, 0x5c, 0xb3,
            0xd5, 0xdd);
DEFINE_GUID(IID_ID3D11DomainShader, 0xf582c508, 0x0f36, 0x490c, 0x99, 0x77, 0x31, 0xee, 0xce, 0x26,
            0x8c, 0xfa);
DEFINE_GUID(IID_ID3D11GeometryShader, 0x38325b96, 0xeffb, 0x4022, 0xba, 0x02, 0x2e, 0x79, 0x5b,
            0x70, 0x27, 0x5c);
DEFINE_GUID(IID_ID3D11PixelShader, 0xea82e40d, 0x51dc, 0x4f33, 0x93, 0xd4, 0xdb, 0x7c, 0x91, 0x25,
            0xae, 0x8c);
DEFINE_GUID(IID_ID3D11ComputeShader, 0x4f5b196e, 0xc2bd, 0x495e, 0xbd, 0x01, 0x1f, 0xde, 0xd3, 0x8e,
            0x49, 0x69);
DEFINE_GUID(IID_ID3D11InputLayout, 0xe4819ddc, 0x4cf0, 0x4025, 0xbd, 0x26, 0x5d, 0xe8, 0x2a, 0x3e,
            0x07, 0xb7);
DEFINE_GUID(IID_ID3D11SamplerState, 0xda6fea51, 0x564c, 0x4487, 0x98, 0x10, 0xf0, 0xd0, 0xf9, 0xb4,
            0xe3, 0xa5);
DEFINE_GUID(IID_ID3D11Asynchronous, 0x4b35d0cd, 0x1e15, 0x4258, 0x9c, 0x98, 0x1b, 0x13, 0x33, 0xf6,
            0xdd, 0x3b);
DEFINE_GUID(IID_ID3D11Query, 0xd6c00747, 0x87b7, 0x425e, 0xb8, 0x4d, 0x44, 0xd1, 0x08, 0x56, 0x0a,
            0xfd);
DEFINE_GUID(IID_ID3D11Predicate, 0x9eb576dd, 0x9f77, 0x4d86, 0x81, 0xaa, 0x8b, 0xab, 0x5f, 0xe4,
            0x90, 0xe2);
DEFINE_GUID(IID_ID3D11Counter, 0x6e8c49fb, 0xa371, 0x4770, 0xb4, 0x40, 0x29, 0x08, 0x60, 0x22, 0xb7,
            0x41);
DEFINE_GUID(IID_ID3D11ClassInstance, 0xa6cd7faa, 0xb0b7, 0x4a2f, 0x94, 0x36, 0x86, 0x62, 0xa6, 0x57,
            0x97, 0xcb);
DEFINE_GUID(IID_ID3D11ClassLinkage, 0xddf57cba, 0x9543, 0x46e4, 0xa1, 0x2b, 0xf2, 0x07, 0xa0, 0xfe,
            0x7f, 0xed);
DEFINE_GUID(IID_ID3D11CommandList, 0xa24bc4d1, 0x769e, 0x43f7, 0x80, 0x13, 0x98, 0xff, 0x56, 0x6c,
            0x18, 0xe2);
DEFINE_GUID(IID_ID3D11DeviceContext, 0xc0bfa96c, 0xe089, 0x44fb, 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61,
            0x90, 0xda);
DEFINE_GUID(IID_ID3D11VideoDecoder, 0x3C9C5B51, 0x995D, 0x48d1, 0x9B, 0x8D, 0xFA, 0x5C, 0xAE, 0xDE,
            0xD6, 0x5C);
DEFINE_GUID(IID_ID3D11VideoProcessorEnumerator, 0x31627037, 0x53AB, 0x4200, 0x90, 0x61, 0x05, 0xFA,
            0xA9, 0xAB, 0x45, 0xF9);
DEFINE_GUID(IID_ID3D11VideoProcessor, 0x1D7B0652, 0x185F, 0x41c6, 0x85, 0xCE, 0x0C, 0x5B, 0xE3,
            0xD4, 0xAE, 0x6C);
DEFINE_GUID(IID_ID3D11AuthenticatedChannel, 0x3015A308, 0xDCBD, 0x47aa, 0xA7, 0x47, 0x19, 0x24,
            0x86, 0xD1, 0x4D, 0x4A);
DEFINE_GUID(IID_ID3D11CryptoSession, 0x9B32F9AD, 0xBDCC, 0x40a6, 0xA3, 0x9D, 0xD5, 0xC8, 0x65, 0x84,
            0x57, 0x20);
DEFINE_GUID(IID_ID3D11VideoDecoderOutputView, 0xC2931AEA, 0x2A85, 0x4f20, 0x86, 0x0F, 0xFB, 0xA1,
            0xFD, 0x25, 0x6E, 0x18);
DEFINE_GUID(IID_ID3D11VideoProcessorInputView, 0x11EC5A5F, 0x51DC, 0x4945, 0xAB, 0x34, 0x6E, 0x8C,
            0x21, 0x30, 0x0E, 0xA5);
DEFINE_GUID(IID_ID3D11VideoProcessorOutputView, 0xA048285E, 0x25A9, 0x4527, 0xBD, 0x93, 0xD6, 0x8B,
            0x68, 0xC4, 0x42, 0x54);
DEFINE_GUID(IID_ID3D11VideoContext, 0x61F21C45, 0x3C0E, 0x4a74, 0x9C, 0xEA, 0x67, 0x10, 0x0D, 0x9A,
            0xD5, 0xE4);
DEFINE_GUID(IID_ID3D11VideoDevice, 0x10EC4D5B, 0x975A, 0x4689, 0xB9, 0xE4, 0xD0, 0xAA, 0xC3, 0x0F,
            0xE3, 0x33);
DEFINE_GUID(IID_ID3D11Device, 0xdb6f6ddb, 0xac77, 0x4e88, 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1,
            0x40);

/* dxgi.h GUIDs */

DEFINE_GUID(IID_IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a,
            0x2e);
DEFINE_GUID(IID_IDXGIDeviceSubObject, 0x3d3e0379, 0xf9de, 0x4d58, 0xbb, 0x6c, 0x18, 0xd6, 0x29,
            0x92, 0xf1, 0xa6);
DEFINE_GUID(IID_IDXGIResource, 0x035f3ab4, 0x482e, 0x4e50, 0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96,
            0x0b);
DEFINE_GUID(IID_IDXGIKeyedMutex, 0x9d8e1289, 0xd7b3, 0x465f, 0x81, 0x26, 0x25, 0x0e, 0x34, 0x9a,
            0xf8, 0x5d);
DEFINE_GUID(IID_IDXGISurface, 0xcafcb56c, 0x6ac3, 0x4889, 0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60,
            0xec);
DEFINE_GUID(IID_IDXGISurface1, 0x4AE63092, 0x6327, 0x4c1b, 0x80, 0xAE, 0xBF, 0xE1, 0x2E, 0xA3, 0x2B,
            0x86);
DEFINE_GUID(IID_IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d,
            0xc0);
DEFINE_GUID(IID_IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13,
            0xaa);
DEFINE_GUID(IID_IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8,
            0x88, 0x6a);
DEFINE_GUID(IID_IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3,
            0x69);
DEFINE_GUID(IID_IDXGIDevice, 0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8,
            0x4c);
DEFINE_GUID(IID_IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3,
            0x87);
DEFINE_GUID(IID_IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a,
            0x05);
DEFINE_GUID(IID_IDXGIDevice1, 0x77db970f, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39,
            0x2c);

/* dxgi1_2.h GUIDs */

DEFINE_GUID(IID_IDXGIDisplayControl, 0xea9dbf1a, 0xc88e, 0x4486, 0x85, 0x4a, 0x98, 0xaa, 0x01, 0x38,
            0xf3, 0x0c);
DEFINE_GUID(IID_IDXGIOutputDuplication, 0x191cfac3, 0xa341, 0x470d, 0xb2, 0x6e, 0xa8, 0x64, 0xf4,
            0x28, 0x31, 0x9c);
DEFINE_GUID(IID_IDXGISurface2, 0xaba496dd, 0xb617, 0x4cb8, 0xa8, 0x66, 0xbc, 0x44, 0xd7, 0xeb, 0x1f,
            0xa2);
DEFINE_GUID(IID_IDXGIResource1, 0x30961379, 0x4609, 0x4a41, 0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e,
            0xe0, 0xc1);
DEFINE_GUID(IID_IDXGIDevice2, 0x05008617, 0xfbfd, 0x4051, 0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6,
            0xa9);
DEFINE_GUID(IID_IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6,
            0xf4, 0xaa);
DEFINE_GUID(IID_IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6,
            0xd0);
DEFINE_GUID(IID_IDXGIAdapter2, 0x0AA1AE0A, 0xFA0E, 0x4B84, 0x86, 0x44, 0xE0, 0x5F, 0xF8, 0xE5, 0xAC,
            0xB5);
DEFINE_GUID(IID_IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66,
            0xcc);

WINPR_ATTR_NODISCARD
const char* GetDxgiErrorString(HRESULT hr)
{
	switch (hr)
	{
		case DXGI_STATUS_OCCLUDED:
			return "DXGI_STATUS_OCCLUDED";
		case DXGI_STATUS_CLIPPED:
			return "DXGI_STATUS_CLIPPED";
		case DXGI_STATUS_NO_REDIRECTION:
			return "DXGI_STATUS_NO_REDIRECTION";
		case DXGI_STATUS_NO_DESKTOP_ACCESS:
			return "DXGI_STATUS_NO_DESKTOP_ACCESS";
		case DXGI_STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE:
			return "DXGI_STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE";
		case DXGI_STATUS_MODE_CHANGED:
			return "DXGI_STATUS_MODE_CHANGED";
		case DXGI_STATUS_MODE_CHANGE_IN_PROGRESS:
			return "DXGI_STATUS_MODE_CHANGE_IN_PROGRESS";
		case DXGI_ERROR_INVALID_CALL:
			return "DXGI_ERROR_INVALID_CALL";
		case DXGI_ERROR_NOT_FOUND:
			return "DXGI_ERROR_NOT_FOUND";
		case DXGI_ERROR_MORE_DATA:
			return "DXGI_ERROR_MORE_DATA";
		case DXGI_ERROR_UNSUPPORTED:
			return "DXGI_ERROR_UNSUPPORTED";
		case DXGI_ERROR_DEVICE_REMOVED:
			return "DXGI_ERROR_DEVICE_REMOVED";
		case DXGI_ERROR_DEVICE_HUNG:
			return "DXGI_ERROR_DEVICE_HUNG";
		case DXGI_ERROR_DEVICE_RESET:
			return "DXGI_ERROR_DEVICE_RESET";
		case DXGI_ERROR_WAS_STILL_DRAWING:
			return "DXGI_ERROR_WAS_STILL_DRAWING";
		case DXGI_ERROR_FRAME_STATISTICS_DISJOINT:
			return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
		case DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE:
			return "DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
			return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
		case DXGI_ERROR_NONEXCLUSIVE:
			return "DXGI_ERROR_NONEXCLUSIVE";
		case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
			return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
		case DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED:
			return "DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED";
		case DXGI_ERROR_REMOTE_OUTOFMEMORY:
			return "DXGI_ERROR_REMOTE_OUTOFMEMORY";
		case DXGI_ERROR_ACCESS_LOST:
			return "DXGI_ERROR_ACCESS_LOST";
		case DXGI_ERROR_WAIT_TIMEOUT:
			return "DXGI_ERROR_WAIT_TIMEOUT";
		case DXGI_ERROR_SESSION_DISCONNECTED:
			return "DXGI_ERROR_SESSION_DISCONNECTED";
		case DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE:
			return "DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE";
		case DXGI_ERROR_CANNOT_PROTECT_CONTENT:
			return "DXGI_ERROR_CANNOT_PROTECT_CONTENT";
		case DXGI_ERROR_ACCESS_DENIED:
			return "DXGI_ERROR_ACCESS_DENIED";
		case DXGI_ERROR_NAME_ALREADY_EXISTS:
			return "DXGI_ERROR_NAME_ALREADY_EXISTS";
		case DXGI_ERROR_SDK_COMPONENT_MISSING:
			return "DXGI_ERROR_SDK_COMPONENT_MISSING";
		case DXGI_STATUS_UNOCCLUDED:
			return "DXGI_STATUS_UNOCCLUDED";
		case DXGI_STATUS_DDA_WAS_STILL_DRAWING:
			return "DXGI_STATUS_DDA_WAS_STILL_DRAWING";
		case DXGI_ERROR_MODE_CHANGE_IN_PROGRESS:
			return "DXGI_ERROR_MODE_CHANGE_IN_PROGRESS";
		case DXGI_DDI_ERR_WASSTILLDRAWING:
			return "DXGI_DDI_ERR_WASSTILLDRAWING";
		case DXGI_DDI_ERR_UNSUPPORTED:
			return "DXGI_DDI_ERR_UNSUPPORTED";
		case DXGI_DDI_ERR_NONEXCLUSIVE:
			return "DXGI_DDI_ERR_NONEXCLUSIVE";
		case E_ACCESSDENIED:
			return "E_ACCESSDENIED";
	}

	return "DXGI_ERROR_UNKNOWN";
}

static void win_shadow_d3d11_module_init()
{
	if (d3d11_module)
		return;

	d3d11_module = LoadLibraryA("d3d11.dll");

	if (!d3d11_module)
		return;

	pfnD3D11CreateDevice = GetProcAddressAs(d3d11_module, "D3D11CreateDevice", fnD3D11CreateDevice);
}

static HMODULE dxgi_module = nullptr;

typedef HRESULT(WINAPI* fnCreateDXGIFactory1)(REFIID riid, void** ppFactory);

static fnCreateDXGIFactory1 pfnCreateDXGIFactory1 = nullptr;

static void win_shadow_dxgi_module_init(void)
{
	if (dxgi_module)
		return;

	dxgi_module = LoadLibraryA("dxgi.dll");

	if (!dxgi_module)
		return;

	pfnCreateDXGIFactory1 =
	    GetProcAddressAs(dxgi_module, "CreateDXGIFactory1", fnCreateDXGIFactory1);
}

/* Called for every output attached to the desktop, on every adapter; FALSE ends the walk */
typedef BOOL (*win_shadow_dxgi_output_fn)(void* arg, IDXGIAdapter1* adapter,
                                          const DXGI_OUTPUT_DESC* desc);

/* Walks the outputs of every adapter, from a factory of its own: a factory keeps the adapters
 * it saw when it was made. An output of a virtual or an indirect display adapter (WARP) is on
 * an adapter of its own, not on the one of the primary output. */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_dxgi_for_each_output(win_shadow_dxgi_output_fn fn, void* arg)
{
	IDXGIFactory1* factory = nullptr;
	BOOL more = TRUE;

	WINPR_ASSERT(fn);

	win_shadow_dxgi_module_init();

	if (!pfnCreateDXGIFactory1)
	{
		WLog_ERR(TAG, "dxgi.dll did not provide CreateDXGIFactory1");
		return FALSE;
	}

	HRESULT hr = pfnCreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory);

	if (FAILED(hr))
	{
		WLog_ERR(TAG, "CreateDXGIFactory1 failure: %s (0x%08lX)", GetDxgiErrorString(hr), hr);
		return FALSE;
	}

	for (UINT a = 0; more; a++)
	{
		IDXGIAdapter1* adapter = nullptr;

		/* DXGI_ERROR_NOT_FOUND: no (more) adapter at this index */
		if (FAILED(factory->lpVtbl->EnumAdapters1(factory, a, &adapter)))
			break;

		for (UINT o = 0; more; o++)
		{
			IDXGIOutput* output = nullptr;
			DXGI_OUTPUT_DESC desc = WINPR_C_ARRAY_INIT;

			/* DXGI_ERROR_NOT_FOUND: no (more) output at this index */
			if (FAILED(adapter->lpVtbl->EnumOutputs(adapter, o, &output)))
				break;

			hr = output->lpVtbl->GetDesc(output, &desc);
			output->lpVtbl->Release(output);

			if (FAILED(hr))
			{
				WLog_WARN(TAG, "adapter %u output %u: IDXGIOutput::GetDesc failure: %s (0x%08lX)",
				          a, o, GetDxgiErrorString(hr), hr);
				continue;
			}

			if (desc.AttachedToDesktop)
				more = fn(arg, adapter, &desc);
		}

		adapter->lpVtbl->Release(adapter);
	}

	factory->lpVtbl->Release(factory);
	return TRUE;
}

typedef struct
{
	const winShadowSubsystem* subsystem;
	const rdpShadowSurface* surface;
	winDxgiState found[WIN_SHADOW_DXGI_MAX_OUTPUTS];
	UINT32 count;
} winDxgiMap;

/* Takes an output when its picture shows in the server's surface */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_dxgi_map_output(void* arg, WINPR_ATTR_UNUSED IDXGIAdapter1* adapter,
                                       const DXGI_OUTPUT_DESC* desc)
{
	winDxgiMap* map = (winDxgiMap*)arg;
	WINPR_ASSERT(map);
	WINPR_ASSERT(desc);

	const RECT* d = &desc->DesktopCoordinates;
	const INT64 width = (INT64)d->right - d->left;
	const INT64 height = (INT64)d->bottom - d->top;

	/* the monitors and the surface are in the coordinates of the monitors' bounding box,
	 * whose top left is at desktopLeft,desktopTop on the virtual desktop */
	const INT64 x = (INT64)d->left - map->subsystem->desktopLeft - map->surface->x;
	const INT64 y = (INT64)d->top - map->subsystem->desktopTop - map->surface->y;

	if ((width <= 0) || (height <= 0) || (x >= map->surface->width) ||
	    (y >= map->surface->height) || (x + width <= 0) || (y + height <= 0))
		return TRUE;

	/* the same place twice: a mirror of an output taken already */
	for (UINT32 index = 0; index < map->count; index++)
	{
		if (EqualRect(&map->found[index].desktop, d))
			return TRUE;
	}

	if (map->count >= ARRAYSIZE(map->found))
	{
		WLog_WARN(TAG, "more than %" PRIu32 " outputs show: the others are not captured",
		          (UINT32)ARRAYSIZE(map->found));
		return FALSE;
	}

	winDxgiState* state = &map->found[map->count++];
	state->desktop = *d;
	state->surfaceX = (INT32)x;
	state->surfaceY = (INT32)y;
	return TRUE;
}

typedef struct
{
	const RECT* desktop;
	IDXGIAdapter1* adapter;
} winDxgiAdapterOf;

/* Finds the adapter of the output at a place on the virtual desktop */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_dxgi_find_adapter(void* arg, IDXGIAdapter1* adapter,
                                         const DXGI_OUTPUT_DESC* desc)
{
	winDxgiAdapterOf* find = (winDxgiAdapterOf*)arg;
	WINPR_ASSERT(find);
	WINPR_ASSERT(desc);

	if (!EqualRect(&desc->DesktopCoordinates, find->desktop))
		return TRUE;

	adapter->lpVtbl->AddRef(adapter);
	find->adapter = adapter;
	return FALSE;
}

WINPR_ATTR_NODISCARD
static int win_shadow_dxgi_release_frame_data(winShadowSubsystem* subsystem);

/* How often a paused capture tries to duplicate the output again */
#define WIN_SHADOW_DXGI_DENIED_RETRY_MS 250

/* How often the capture tries again after a failure which does not recover by
 * itself (no D3D11 device, no output of the adapter duplicatable at all):
 * unlike a refused desktop, that state does not go away on its own */
#define WIN_SHADOW_DXGI_INIT_RETRY_MS 60000

/* How often the capture creates a fresh D3D11 device again after the device
 * was removed or reset (driver crash, GPU change, TDR), and after a failure
 * which classified itself in no other way: either state may clear soon, so
 * retry at the same low rate as a refused desktop */
#define WIN_SHADOW_DXGI_RESET_RETRY_MS 500

/* The input desktop is one this process may not capture: the secure desktop (logon, lock
 * screen, UAC) for a process in the user's session. DuplicateOutput says E_ACCESSDENIED,
 * AcquireNextFrame and Map DXGI_ERROR_ACCESS_DENIED. */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_dxgi_is_denied(HRESULT hr)
{
	return (hr == DXGI_ERROR_ACCESS_DENIED) || (hr == E_ACCESSDENIED);
}

/* Pause the capture: said once, then quiet until it resumes; the capture loop tries again
 * after WIN_SHADOW_DXGI_DENIED_RETRY_MS (and at once when the input desktop changes) */
static void win_shadow_dxgi_pause(winShadowSubsystem* subsystem, const char* what, HRESULT hr)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(what);

	if (!subsystem->dxgi->dxgiDenied)
		WLog_WARN(TAG,
		          "%s: %s (0x%08lX): this process may not capture the current desktop (the "
		          "secure desktop?), capture paused until it can",
		          what, GetDxgiErrorString(hr), hr);
	subsystem->dxgi->dxgiDenied = TRUE;
	subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_DENIED_RETRY_MS;
}

/* A failure which classified itself in no other way must not spin the capture
 * loop: retry at the reset rate. An earlier classification (a refused desktop,
 * a failure which does not recover by itself) is kept. */
static void win_shadow_dxgi_schedule_retry(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	if (GetTickCount64() >= subsystem->dxgi->dxgiRetryTime)
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_RESET_RETRY_MS;
}

static void win_shadow_dxgi_drop_duplication(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	(void)win_shadow_dxgi_release_frame_data(subsystem);

	if (subsystem->dxgi->dxgiDesktopImage)
	{
		subsystem->dxgi->dxgiDesktopImage->lpVtbl->Release(subsystem->dxgi->dxgiDesktopImage);
		subsystem->dxgi->dxgiDesktopImage = nullptr;
	}

	if (subsystem->dxgi->dxgiOutputDuplication)
	{
		subsystem->dxgi->dxgiOutputDuplication->lpVtbl->Release(
		    subsystem->dxgi->dxgiOutputDuplication);
		subsystem->dxgi->dxgiOutputDuplication = nullptr;
	}

	if (subsystem->dxgi->dxgiStage)
	{
		subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
		subsystem->dxgi->dxgiStage = nullptr;
	}
}

/* (Re)create the staging texture, the CPU readable copy of the desktop image
 * the region copy reads from. It must hold the whole desktop image: the copy
 * below maps it and hands its pitch to freerdp_image_copy_no_overlap(), which
 * asserts when a row of the requested region does not fit in that pitch. */
WINPR_ATTR_NODISCARD
static int win_shadow_dxgi_create_stage(winShadowSubsystem* subsystem, UINT32 width, UINT32 height)
{
	D3D11_TEXTURE2D_DESC textureDesc = WINPR_C_ARRAY_INIT;
	HRESULT hr;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->dxgi->dxgiDevice);

	if (subsystem->dxgi->dxgiSurface)
	{
		if (subsystem->dxgi->dxgiSurfaceMapped)
			subsystem->dxgi->dxgiSurface->lpVtbl->Unmap(subsystem->dxgi->dxgiSurface);
		subsystem->dxgi->dxgiSurface->lpVtbl->Release(subsystem->dxgi->dxgiSurface);
		subsystem->dxgi->dxgiSurface = nullptr;
	}
	subsystem->dxgi->dxgiSurfaceMapped = FALSE;

	if (subsystem->dxgi->dxgiStage)
	{
		subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
		subsystem->dxgi->dxgiStage = nullptr;
	}

	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Usage = D3D11_USAGE_STAGING;
	textureDesc.BindFlags = 0;
	textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	textureDesc.MiscFlags = 0;

	hr = subsystem->dxgi->dxgiDevice->lpVtbl->CreateTexture2D(
	    subsystem->dxgi->dxgiDevice, &textureDesc, nullptr, &(subsystem->dxgi->dxgiStage));

	if (FAILED(hr))
	{
		WLog_ERR(TAG,
		         "ID3D11Device::CreateTexture2D(%" PRIu32 "x%" PRIu32 ") failure: %s (0x%08lX)",
		         width, height, GetDxgiErrorString(hr), hr);
		return -1;
	}

	return 1;
}

WINPR_ATTR_NODISCARD
static int win_shadow_dxgi_init_duplication(winShadowSubsystem* subsystem)
{
	HRESULT hr;
	BOOL found = FALSE;
	UINT outputIndex = 0;
	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* dxgiAdapter = nullptr;

	WINPR_ASSERT(subsystem);
	winDxgiState* state = subsystem->dxgi;
	WINPR_ASSERT(state);
	const RECT* desktop = &state->desktop;
	const LONG width = desktop->right - desktop->left;
	const LONG height = desktop->bottom - desktop->top;

	hr = subsystem->dxgi->dxgiDevice->lpVtbl->QueryInterface(subsystem->dxgi->dxgiDevice,
	                                                         &IID_IDXGIDevice, (void**)&dxgiDevice);

	if (FAILED(hr))
	{
		WLog_ERR(TAG, "ID3D11Device::QueryInterface(IDXGIDevice) failure: %s (0x%08lX)",
		         GetDxgiErrorString(hr), hr);
		return -1;
	}

	hr = dxgiDevice->lpVtbl->GetParent(dxgiDevice, &IID_IDXGIAdapter, (void**)&dxgiAdapter);

	if (dxgiDevice)
	{
		dxgiDevice->lpVtbl->Release(dxgiDevice);
		dxgiDevice = nullptr;
	}

	if (FAILED(hr))
	{
		WLog_ERR(TAG, "IDXGIDevice::GetParent(IDXGIAdapter) failure: %s (0x%08lX)",
		         GetDxgiErrorString(hr), hr);
		return -1;
	}

	/* the output of this state, on the adapter its device was made on: a duplication works
	 * only on a device of the output's own adapter */
	for (UINT index = 0; !found; index++)
	{
		IDXGIOutput* output = nullptr;
		IDXGIOutput1* output1 = nullptr;
		DXGI_OUTPUT_DESC desc = WINPR_C_ARRAY_INIT;

		hr = dxgiAdapter->lpVtbl->EnumOutputs(dxgiAdapter, index, &output);

		/* DXGI_ERROR_NOT_FOUND: no (more) output at this index */
		if (FAILED(hr))
			break;

		hr = output->lpVtbl->GetDesc(output, &desc);

		if (FAILED(hr) || !desc.AttachedToDesktop || !EqualRect(&desc.DesktopCoordinates, desktop))
		{
			output->lpVtbl->Release(output);
			continue;
		}

		found = TRUE;
		outputIndex = index;

		hr = output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1);
		output->lpVtbl->Release(output);

		if (FAILED(hr))
		{
			WLog_WARN(TAG, "IDXGIOutput %u::QueryInterface(IDXGIOutput1) failure: %s (0x%08lX)",
			          index, GetDxgiErrorString(hr), hr);
			break;
		}

		hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown*)subsystem->dxgi->dxgiDevice,
		                                      &(subsystem->dxgi->dxgiOutputDuplication));
		output1->lpVtbl->Release(output1);
	}

	if (dxgiAdapter)
	{
		dxgiAdapter->lpVtbl->Release(dxgiAdapter);
		dxgiAdapter = nullptr;
	}

	if (!found)
	{
		/* the displays changed under the capture: the capture loop follows the new layout
		 * (win_shadow_check_resize), and tries this output again soon */
		WLog_WARN(TAG, "the output %ldx%ld at %ld,%ld is no longer on its adapter", width, height,
		          desktop->left, desktop->top);
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_RESET_RETRY_MS;
		return -1;
	}

	if (!subsystem->dxgi->dxgiOutputDuplication)
	{
		if (win_shadow_dxgi_is_denied(hr))
		{
			/* the output denies the duplication while this process may not see the
			 * desktop: the capture loop tries again from dxgiRetryTime on */
			win_shadow_dxgi_pause(subsystem, "IDXGIOutput1::DuplicateOutput", hr);
			return -1;
		}

		/* this does not recover by itself (an output of a virtual or an indirect display
		 * adapter which refuses the duplication for good): the capture loop tries again at a
		 * low rate */
		WLog_ERR(TAG,
		         "IDXGIOutput %u (%ldx%ld at %ld,%ld) refused the duplication: %s (0x%08lX), next "
		         "attempt in %u ms",
		         outputIndex, width, height, desktop->left, desktop->top, GetDxgiErrorString(hr),
		         hr, WIN_SHADOW_DXGI_INIT_RETRY_MS);
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_INIT_RETRY_MS;
		return -1;
	}

	WLog_INFO(
	    TAG,
	    "duplicating output %u (%ldx%ld at %ld,%ld, at %" PRId32 ",%" PRId32 " in the surface)",
	    outputIndex, width, height, desktop->left, desktop->top, state->surfaceX, state->surfaceY);

	/* the staging texture holds the output's whole image; fetch_frame_data recreates it when
	 * a frame is larger */
	if (win_shadow_dxgi_create_stage(subsystem, (UINT32)width, (UINT32)height) < 0)
		return -1;

	if (subsystem->dxgi->dxgiDenied)
	{
		WLog_INFO(TAG, "capture resumed");
		subsystem->dxgi->dxgiDenied = FALSE;
	}
	return 1;
}

/* Creates the device of the current output on that output's adapter, and its duplication */
int win_shadow_dxgi_init(winShadowSubsystem* subsystem)
{
	HRESULT hr;
	winDxgiAdapterOf find = WINPR_C_ARRAY_INIT;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->dxgi);

	win_shadow_d3d11_module_init();

	if (!pfnD3D11CreateDevice)
	{
		/* this does not recover by itself: the capture loop tries again at a
		 * low rate */
		WLog_ERR(TAG, "d3d11.dll did not provide D3D11CreateDevice, next attempt in %u ms",
		         WIN_SHADOW_DXGI_INIT_RETRY_MS);
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_INIT_RETRY_MS;
		return -1;
	}

	find.desktop = &subsystem->dxgi->desktop;

	if (!win_shadow_dxgi_for_each_output(win_shadow_dxgi_find_adapter, &find) || !find.adapter)
	{
		/* the displays changed under the capture: the capture loop follows the new layout
		 * (win_shadow_check_resize), and tries this output again soon */
		WLog_WARN(TAG, "no adapter has the output at %ld,%ld", find.desktop->left,
		          find.desktop->top);
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_RESET_RETRY_MS;
		return -1;
	}

	/* an explicit adapter takes D3D_DRIVER_TYPE_UNKNOWN; it may be a software one (WARP: the
	 * Microsoft Basic Render Driver, an indirect display) */
	hr = pfnD3D11CreateDevice((IDXGIAdapter*)find.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
	                          FeatureLevels, NumFeatureLevels, D3D11_SDK_VERSION,
	                          &(subsystem->dxgi->dxgiDevice), &(subsystem->dxgi->featureLevel),
	                          &(subsystem->dxgi->dxgiDeviceContext));
	find.adapter->lpVtbl->Release(find.adapter);

	if (FAILED(hr))
	{
		/* this does not recover by itself: the capture loop tries again at a
		 * low rate */
		WLog_ERR(TAG, "D3D11CreateDevice failure: 0x%08lX, next attempt in %u ms", hr,
		         WIN_SHADOW_DXGI_INIT_RETRY_MS);
		subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_INIT_RETRY_MS;
		return -1;
	}

	return win_shadow_dxgi_init_duplication(subsystem);
}

/* Releases the current output's device and every object made on it */
static void win_shadow_dxgi_uninit_state(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->dxgi);

	if (subsystem->dxgi->dxgiStage)
	{
		subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
		subsystem->dxgi->dxgiStage = nullptr;
	}

	if (subsystem->dxgi->dxgiDesktopImage)
	{
		subsystem->dxgi->dxgiDesktopImage->lpVtbl->Release(subsystem->dxgi->dxgiDesktopImage);
		subsystem->dxgi->dxgiDesktopImage = nullptr;
	}

	if (subsystem->dxgi->dxgiOutputDuplication)
	{
		subsystem->dxgi->dxgiOutputDuplication->lpVtbl->Release(
		    subsystem->dxgi->dxgiOutputDuplication);
		subsystem->dxgi->dxgiOutputDuplication = nullptr;
	}

	if (subsystem->dxgi->dxgiDeviceContext)
	{
		subsystem->dxgi->dxgiDeviceContext->lpVtbl->Release(subsystem->dxgi->dxgiDeviceContext);
		subsystem->dxgi->dxgiDeviceContext = nullptr;
	}

	if (subsystem->dxgi->dxgiDevice)
	{
		subsystem->dxgi->dxgiDevice->lpVtbl->Release(subsystem->dxgi->dxgiDevice);
		subsystem->dxgi->dxgiDevice = nullptr;
	}
}

/* Releases every output: their frames, duplications and devices */
static void win_shadow_dxgi_drop_outputs(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	for (UINT32 index = 0; index < subsystem->outputCount; index++)
	{
		win_shadow_dxgi_select(subsystem, index);
		(void)win_shadow_dxgi_release_frame_data(subsystem);

		if (subsystem->dxgi->dxgiResource)
		{
			subsystem->dxgi->dxgiResource->lpVtbl->Release(subsystem->dxgi->dxgiResource);
			subsystem->dxgi->dxgiResource = nullptr;
		}

		win_shadow_dxgi_uninit_state(subsystem);
		free(subsystem->dxgi->MetadataBuffer);
	}

	ZeroMemory(subsystem->outputs, sizeof(subsystem->outputs));
	subsystem->outputCount = 0;
	subsystem->outputsMapped = FALSE;
	subsystem->dxgi = &subsystem->outputs[0];
}

int win_shadow_dxgi_uninit(winShadowSubsystem* subsystem)
{
	win_shadow_dxgi_drop_outputs(subsystem);
	return 1;
}

void win_shadow_dxgi_select(winShadowSubsystem* subsystem, UINT32 index)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(index < ARRAYSIZE(subsystem->outputs));
	subsystem->dxgi = &subsystem->outputs[index];
}

int win_shadow_dxgi_map_outputs(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->base.server);

	const rdpShadowSurface* surface = subsystem->base.server->surface;
	if (!surface)
		return -1;

	winDxgiMap* map = (winDxgiMap*)calloc(1, sizeof(winDxgiMap));
	if (!map)
		return -1;
	map->subsystem = subsystem;
	map->surface = surface;

	if (!win_shadow_dxgi_for_each_output(win_shadow_dxgi_map_output, map))
	{
		free(map);
		return -1;
	}

	win_shadow_dxgi_drop_outputs(subsystem);
	CopyMemory(subsystem->outputs, map->found, sizeof(subsystem->outputs));
	subsystem->outputCount = map->count;
	free(map);

	subsystem->outputsSurfaceX = surface->x;
	subsystem->outputsSurfaceY = surface->y;
	subsystem->outputsSurfaceWidth = surface->width;
	subsystem->outputsSurfaceHeight = surface->height;
	subsystem->outputsMapped = TRUE;

	for (UINT32 index = 0; index < subsystem->outputCount; index++)
	{
		const winDxgiState* state = &subsystem->outputs[index];
		WLog_INFO(TAG,
		          "output %" PRIu32 ": %ldx%ld at %ld,%ld on the desktop, at %" PRId32 ",%" PRId32
		          " in the %" PRIu32 "x%" PRIu32 " surface",
		          index, state->desktop.right - state->desktop.left,
		          state->desktop.bottom - state->desktop.top, state->desktop.left,
		          state->desktop.top, state->surfaceX, state->surfaceY, surface->width,
		          surface->height);
	}

	if (subsystem->outputCount == 0)
		WLog_WARN(TAG, "no output shows the shared part of the desktop");
	return (int)subsystem->outputCount;
}

/* The D3D11 device itself is gone (removed, reset, hung): a new output
 * duplication on it can only fail, and unlike a refused desktop the device
 * does not become usable again. Drop every object held on it and the device
 * itself; the capture loop creates a fresh device once dxgiRetryTime has
 * passed. */
static void win_shadow_dxgi_reset_device(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	win_shadow_dxgi_drop_duplication(subsystem);

	if (subsystem->dxgi->dxgiResource)
	{
		subsystem->dxgi->dxgiResource->lpVtbl->Release(subsystem->dxgi->dxgiResource);
		subsystem->dxgi->dxgiResource = nullptr;
	}

	win_shadow_dxgi_uninit_state(subsystem);

	subsystem->dxgi->dxgiRetryTime = GetTickCount64() + WIN_SHADOW_DXGI_RESET_RETRY_MS;
}

int win_shadow_dxgi_fetch_frame_data(winShadowSubsystem* subsystem, BYTE** ppDstData,
                                     int* pnDstStep, int x, int y, int width, int height)
{
	int status;
	HRESULT hr;
	D3D11_BOX Box;
	D3D11_TEXTURE2D_DESC srcDesc = WINPR_C_ARRAY_INIT;
	DXGI_MAPPED_RECT mappedRect;

	if ((width * height) < 1)
		return 0;

	{
		/* The shadow surface may be a pixel larger than the desktop image
		 * (exclusive vs inclusive rect edges); clamp the requested region to
		 * the source texture, otherwise the out-of-bounds copy trips
		 * DXGI_ERROR_DEVICE_REMOVED and capture silently dies. */
		subsystem->dxgi->dxgiDesktopImage->lpVtbl->GetDesc(subsystem->dxgi->dxgiDesktopImage,
		                                                   &srcDesc);
		if (x + width > (int)srcDesc.Width)
			width = (int)srcDesc.Width - x;
		if (y + height > (int)srcDesc.Height)
			height = (int)srcDesc.Height - y;

		/* a desktop that shrank below the region leaves nothing to copy, and
		 * an empty box is not a valid copy source */
		if ((width <= 0) || (height <= 0))
			return 0;

		/* The staging texture is the destination of the copy below and was
		 * created for the desktop size the duplication was set up with. A
		 * desktop resize since then (display mode change, a session connected
		 * to the console) leaves it smaller than the acquired frame while the
		 * duplication keeps delivering frames of the new size.
		 * CopySubresourceRegion clips the copy to the smaller destination
		 * without failing, so the mapped pitch stays that of the old width:
		 * shorter than one row of the requested region, which trips the
		 * stride assertion of freerdp_image_copy_no_overlap() in the caller
		 * and kills the capture thread. Follow the frame instead of dropping
		 * it: the texture holds the whole desktop image, so one recreation
		 * covers every region of this geometry. */
		{
			D3D11_TEXTURE2D_DESC dstDesc = WINPR_C_ARRAY_INIT;
			BOOL fits = FALSE;

			if (subsystem->dxgi->dxgiStage)
			{
				subsystem->dxgi->dxgiStage->lpVtbl->GetDesc(subsystem->dxgi->dxgiStage, &dstDesc);
				fits = ((UINT32)width <= dstDesc.Width) && ((UINT32)height <= dstDesc.Height);
			}

			if (!fits)
			{
				WLog_INFO(TAG,
				          "desktop is %" PRIu32 "x%" PRIu32 ", the staging texture was %" PRIu32
				          "x%" PRIu32 ": recreating it",
				          srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);

				if (win_shadow_dxgi_create_stage(subsystem, srcDesc.Width, srcDesc.Height) < 0)
					return 0;
			}
		}
	}

	Box.top = y;
	Box.left = x;
	Box.right = x + width;
	Box.bottom = y + height;
	Box.front = 0;
	Box.back = 1;

	/* the destination now holds the whole frame, so this copy cannot clip it
	 * (CopySubresourceRegion reports no failure in the C vtable) */
	subsystem->dxgi->dxgiDeviceContext->lpVtbl->CopySubresourceRegion(
	    subsystem->dxgi->dxgiDeviceContext, (ID3D11Resource*)subsystem->dxgi->dxgiStage, 0, 0, 0, 0,
	    (ID3D11Resource*)subsystem->dxgi->dxgiDesktopImage, 0, &Box);

	if (subsystem->dxgi->dxgiSurface)
	{
		subsystem->dxgi->dxgiSurface->lpVtbl->Release(subsystem->dxgi->dxgiSurface);
		subsystem->dxgi->dxgiSurface = nullptr;
	}

	hr = subsystem->dxgi->dxgiStage->lpVtbl->QueryInterface(
	    subsystem->dxgi->dxgiStage, &IID_IDXGISurface, (void**)&(subsystem->dxgi->dxgiSurface));

	if (FAILED(hr))
	{
		WLog_ERR(TAG, "ID3D11Texture2D::QueryInterface(IDXGISurface) failure: %s 0x%08lX",
		         GetDxgiErrorString(hr), hr);
		return -1;
	}

	hr = subsystem->dxgi->dxgiSurface->lpVtbl->Map(subsystem->dxgi->dxgiSurface, &mappedRect,
	                                               DXGI_MAP_READ);

	if (FAILED(hr))
	{
		if (win_shadow_dxgi_is_denied(hr))
		{
			win_shadow_dxgi_pause(subsystem, "IDXGISurface::Map", hr);
			win_shadow_dxgi_drop_duplication(subsystem);
			return 0;
		}

		WLog_ERR(TAG, "IDXGISurface::Map failure: %s 0x%08lX", GetDxgiErrorString(hr), hr);

		if ((hr == DXGI_ERROR_DEVICE_REMOVED) || (hr == DXGI_ERROR_DEVICE_RESET))
		{
			/* the device itself is gone: a new duplication on it can only
			 * fail; drop it, the capture loop creates a fresh device once
			 * dxgiRetryTime has passed */
			win_shadow_dxgi_reset_device(subsystem);
			return 0;
		}
		else if (hr == DXGI_ERROR_ACCESS_DENIED)
		{
			/* The console desktop switched to the secure desktop (lock
			 * screen, UAC prompt, ...). Re-create the duplication with a
			 * bounded retry loop instead of killing the capture thread. */
			if (subsystem->dxgi->dxgiSurface)
			{
				subsystem->dxgi->dxgiSurface->lpVtbl->Release(subsystem->dxgi->dxgiSurface);
				subsystem->dxgi->dxgiSurface = nullptr;
				subsystem->dxgi->dxgiSurfaceMapped = FALSE;
			}

			if (subsystem->dxgi->dxgiStage)
			{
				subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
				subsystem->dxgi->dxgiStage = nullptr;
			}

			if (subsystem->dxgi->dxgiOutputDuplication)
			{
				subsystem->dxgi->dxgiOutputDuplication->lpVtbl->Release(
				    subsystem->dxgi->dxgiOutputDuplication);
				subsystem->dxgi->dxgiOutputDuplication = nullptr;
			}

			for (int attempt = 0; attempt < 240; ++attempt)
			{
				if (WaitForSingleObject(subsystem->base.server->StopEvent, 0) == WAIT_OBJECT_0)
					return -1;

				Sleep(500);
				status = win_shadow_dxgi_init_duplication(subsystem);
				if (status >= 0)
					return 0;
			}

			return -1;
		}

		return -1;
	}

	subsystem->dxgi->dxgiSurfaceMapped = TRUE;

	*ppDstData = mappedRect.pBits;
	*pnDstStep = mappedRect.Pitch;

	return 1;
}

int win_shadow_dxgi_release_frame_data(winShadowSubsystem* subsystem)
{
	if (subsystem->dxgi->dxgiSurface)
	{
		if (subsystem->dxgi->dxgiSurfaceMapped)
			subsystem->dxgi->dxgiSurface->lpVtbl->Unmap(subsystem->dxgi->dxgiSurface);

		subsystem->dxgi->dxgiSurface->lpVtbl->Release(subsystem->dxgi->dxgiSurface);
		subsystem->dxgi->dxgiSurface = nullptr;
	}
	subsystem->dxgi->dxgiSurfaceMapped = FALSE;

	if (subsystem->dxgi->dxgiOutputDuplication)
	{
		if (subsystem->dxgi->dxgiFrameAcquired)
			subsystem->dxgi->dxgiOutputDuplication->lpVtbl->ReleaseFrame(
			    subsystem->dxgi->dxgiOutputDuplication);
	}
	subsystem->dxgi->dxgiFrameAcquired = FALSE;

	subsystem->dxgi->pendingFrames = 0;

	return 1;
}

/* A new duplication of the output, on the calling thread's desktop: after a
 * switch between the user's desktop and the secure one (logon, lock screen,
 * UAC), or when an earlier attempt failed. A device that was removed or reset
 * in between (driver crash, GPU change, TDR) is unusable for a new duplication
 * and is replaced by a fresh one; a healthy device is kept. */
int win_shadow_dxgi_restart_duplication(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	if (!subsystem->dxgi->dxgiDevice)
	{
		const int status = win_shadow_dxgi_init(subsystem);

		if (status < 0)
		{
			/* the initialization may have created the device and failed on
			 * the frame objects afterwards */
			if (subsystem->dxgi->dxgiDevice)
			{
				if (FAILED(subsystem->dxgi->dxgiDevice->lpVtbl->GetDeviceRemovedReason(
				        subsystem->dxgi->dxgiDevice)))
				{
					win_shadow_dxgi_reset_device(subsystem);
				}
				else
				{
					/* a live device stays for the classified retry, the
					 * partial objects must not */
					win_shadow_dxgi_drop_duplication(subsystem);
					win_shadow_dxgi_schedule_retry(subsystem);
				}
			}
			else
			{
				/* without a device a stray context may still be around */
				win_shadow_dxgi_uninit_state(subsystem);
				win_shadow_dxgi_schedule_retry(subsystem);
			}
		}

		return status;
	}

	/* the device kept from the last desktop may have died in between: a new
	 * duplication on it can only fail with DXGI_ERROR_DEVICE_REMOVED while
	 * the capture loop keeps retrying the dead device forever */
	const HRESULT reason =
	    subsystem->dxgi->dxgiDevice->lpVtbl->GetDeviceRemovedReason(subsystem->dxgi->dxgiDevice);

	if (FAILED(reason))
	{
		WLog_WARN(TAG, "D3D11 device is no longer usable: %s (0x%08lX)", GetDxgiErrorString(reason),
		          reason);
		win_shadow_dxgi_reset_device(subsystem);
		return -1;
	}

	win_shadow_dxgi_drop_duplication(subsystem);
	const int status = win_shadow_dxgi_init_duplication(subsystem);

	if (status < 0)
	{
		/* the device may have died while the outputs refused the
		 * duplication: check once more before keeping it for the retry,
		 * the device reset overrides any failure classification */
		if (subsystem->dxgi->dxgiDevice &&
		    FAILED(subsystem->dxgi->dxgiDevice->lpVtbl->GetDeviceRemovedReason(
		        subsystem->dxgi->dxgiDevice)))
		{
			win_shadow_dxgi_reset_device(subsystem);
			return -1;
		}

		/* a partial initialization may leave frame objects behind */
		win_shadow_dxgi_drop_duplication(subsystem);
		win_shadow_dxgi_schedule_retry(subsystem);
	}

	return status;
}

int win_shadow_dxgi_get_next_frame(winShadowSubsystem* subsystem)
{
	UINT i = 0;
	int status;
	HRESULT hr = 0;
	UINT timeout = 1;
	UINT DataBufferSize = 0;
	BYTE* DataBuffer = nullptr;

	if (subsystem->dxgi->dxgiFrameAcquired)
	{
		win_shadow_dxgi_release_frame_data(subsystem);
	}

	if (subsystem->dxgi->dxgiDesktopImage)
	{
		subsystem->dxgi->dxgiDesktopImage->lpVtbl->Release(subsystem->dxgi->dxgiDesktopImage);
		subsystem->dxgi->dxgiDesktopImage = nullptr;
	}

	hr = subsystem->dxgi->dxgiOutputDuplication->lpVtbl->AcquireNextFrame(
	    subsystem->dxgi->dxgiOutputDuplication, timeout, &(subsystem->dxgi->dxgiFrameInfo),
	    &(subsystem->dxgi->dxgiResource));

	if (SUCCEEDED(hr))
	{
		subsystem->dxgi->dxgiFrameAcquired = TRUE;
		subsystem->dxgi->pendingFrames = subsystem->dxgi->dxgiFrameInfo.AccumulatedFrames;
	}

	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return 0;

	if (FAILED(hr))
	{
		if (win_shadow_dxgi_is_denied(hr))
		{
			/* the duplication is no good any more: drop it, the capture loop makes a new one
			 * (on the new input desktop when this process may open it) */
			win_shadow_dxgi_pause(subsystem, "IDXGIOutputDuplication::AcquireNextFrame", hr);
			win_shadow_dxgi_drop_duplication(subsystem);
			return 0;
		}

		WLog_ERR(TAG, "IDXGIOutputDuplication::AcquireNextFrame failure: %s (0x%08lX)",
		         GetDxgiErrorString(hr), hr);

		if ((hr == DXGI_ERROR_DEVICE_REMOVED) || (hr == DXGI_ERROR_DEVICE_RESET))
		{
			/* the device is gone for good: do not ask it again (the removal
			 * may race ahead and still report S_OK), drop it; the capture
			 * loop creates a fresh device once dxgiRetryTime has passed */
			win_shadow_dxgi_reset_device(subsystem);
			return 0;
		}
		else if (hr == DXGI_ERROR_ACCESS_LOST)
		{
			win_shadow_dxgi_release_frame_data(subsystem);

			if (subsystem->dxgi->dxgiDesktopImage)
			{
				subsystem->dxgi->dxgiDesktopImage->lpVtbl->Release(
				    subsystem->dxgi->dxgiDesktopImage);
				subsystem->dxgi->dxgiDesktopImage = nullptr;
			}

			if (subsystem->dxgi->dxgiStage)
			{
				subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
				subsystem->dxgi->dxgiStage = nullptr;
			}

			if (subsystem->dxgi->dxgiOutputDuplication)
			{
				subsystem->dxgi->dxgiOutputDuplication->lpVtbl->Release(
				    subsystem->dxgi->dxgiOutputDuplication);
				subsystem->dxgi->dxgiOutputDuplication = nullptr;
			}

			status = win_shadow_dxgi_restart_duplication(subsystem);

			if (status < 0)
			{
				/* a device reset already scheduled the retry: defer this
				 * frame instead of failing the capture pass */
				return subsystem->dxgi->dxgiDevice ? -1 : 0;
			}

			return 0;
		}
		else if (hr == DXGI_ERROR_ACCESS_DENIED)
		{
			/* The console desktop switched to the secure desktop (lock
			 * screen, UAC prompt, ...); the duplication stays denied while
			 * that desktop is active. Release it and re-create with a
			 * bounded retry loop instead of killing the capture thread. */
			win_shadow_dxgi_release_frame_data(subsystem);

			if (subsystem->dxgi->dxgiDesktopImage)
			{
				subsystem->dxgi->dxgiDesktopImage->lpVtbl->Release(
				    subsystem->dxgi->dxgiDesktopImage);
				subsystem->dxgi->dxgiDesktopImage = nullptr;
			}

			if (subsystem->dxgi->dxgiStage)
			{
				subsystem->dxgi->dxgiStage->lpVtbl->Release(subsystem->dxgi->dxgiStage);
				subsystem->dxgi->dxgiStage = nullptr;
			}

			if (subsystem->dxgi->dxgiOutputDuplication)
			{
				subsystem->dxgi->dxgiOutputDuplication->lpVtbl->Release(
				    subsystem->dxgi->dxgiOutputDuplication);
				subsystem->dxgi->dxgiOutputDuplication = nullptr;
			}

			for (int attempt = 0; attempt < 240; ++attempt)
			{
				if (WaitForSingleObject(subsystem->base.server->StopEvent, 0) == WAIT_OBJECT_0)
					return -1;

				Sleep(500);
				status = win_shadow_dxgi_init_duplication(subsystem);
				if (status >= 0)
					return 0;
			}

			return -1;
		}
		else if (hr == DXGI_ERROR_INVALID_CALL)
		{
			win_shadow_dxgi_uninit_state(subsystem);

			status = win_shadow_dxgi_init(subsystem);

			if (status < 0)
				return -1;

			return 0;
		}

		return -1;
	}

	hr = subsystem->dxgi->dxgiResource->lpVtbl->QueryInterface(
	    subsystem->dxgi->dxgiResource, &IID_ID3D11Texture2D,
	    (void**)&(subsystem->dxgi->dxgiDesktopImage));

	if (subsystem->dxgi->dxgiResource)
	{
		subsystem->dxgi->dxgiResource->lpVtbl->Release(subsystem->dxgi->dxgiResource);
		subsystem->dxgi->dxgiResource = nullptr;
	}

	if (FAILED(hr))
	{
		WLog_ERR(TAG, "IDXGIResource::QueryInterface(ID3D11Texture2D) failure: %s (0x%08lX)",
		         GetDxgiErrorString(hr), hr);
		return -1;
	}

	return 1;
}

/* An output's rectangle (output coordinates, right and bottom exclusive) in the server's
 * surface; FALSE when nothing of it is in the surface */
WINPR_ATTR_NODISCARD
static BOOL win_shadow_dxgi_surface_rect(const winDxgiState* state, const RECT* rect,
                                         RECTANGLE_16* surfaceRect)
{
	WINPR_ASSERT(state);
	WINPR_ASSERT(rect);
	WINPR_ASSERT(surfaceRect);

	const INT64 left = MAX(0, (INT64)rect->left + state->surfaceX);
	const INT64 top = MAX(0, (INT64)rect->top + state->surfaceY);
	const INT64 right = MIN(UINT16_MAX, (INT64)rect->right + state->surfaceX);
	const INT64 bottom = MIN(UINT16_MAX, (INT64)rect->bottom + state->surfaceY);

	if ((right <= left) || (bottom <= top))
		return FALSE;

	surfaceRect->left = (UINT16)left;
	surfaceRect->top = (UINT16)top;
	surfaceRect->right = (UINT16)right;
	surfaceRect->bottom = (UINT16)bottom;
	return TRUE;
}

int win_shadow_dxgi_get_invalid_region(winShadowSubsystem* subsystem)
{
	HRESULT hr;
	POINT* pSrcPt;
	RECT* pDstRect;
	RECT* pDirtyRect;
	UINT numMoveRects;
	UINT numDirtyRects;
	UINT UsedBufferSize;
	RECTANGLE_16 invalidRect;
	UINT MetadataBufferSize;
	UINT MoveRectsBufferSize;
	UINT DirtyRectsBufferSize;
	RECT* pDirtyRectsBuffer;
	DXGI_OUTDUPL_MOVE_RECT* pMoveRect;
	DXGI_OUTDUPL_MOVE_RECT* pMoveRectBuffer;
	rdpShadowSurface* surface = nullptr;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->base.server);
	surface = subsystem->base.server->surface;
	WINPR_ASSERT(surface);

	if (subsystem->dxgi->dxgiFrameInfo.AccumulatedFrames == 0)
		return 0;

	if (subsystem->dxgi->dxgiFrameInfo.TotalMetadataBufferSize == 0)
		return 0;

	MetadataBufferSize = subsystem->dxgi->dxgiFrameInfo.TotalMetadataBufferSize;

	if (MetadataBufferSize > subsystem->dxgi->MetadataBufferSize)
	{
		subsystem->dxgi->MetadataBuffer =
		    (BYTE*)realloc(subsystem->dxgi->MetadataBuffer, MetadataBufferSize);

		if (!subsystem->dxgi->MetadataBuffer)
			return -1;

		subsystem->dxgi->MetadataBufferSize = MetadataBufferSize;
	}

	/* GetFrameMoveRects */

	UsedBufferSize = 0;

	MoveRectsBufferSize = MetadataBufferSize - UsedBufferSize;
	pMoveRectBuffer = (DXGI_OUTDUPL_MOVE_RECT*)&(subsystem->dxgi->MetadataBuffer[UsedBufferSize]);

	hr = subsystem->dxgi->dxgiOutputDuplication->lpVtbl->GetFrameMoveRects(
	    subsystem->dxgi->dxgiOutputDuplication, MoveRectsBufferSize, pMoveRectBuffer,
	    &MoveRectsBufferSize);

	if (FAILED(hr))
	{
		WLog_ERR(TAG,
		         "IDXGIOutputDuplication::GetFrameMoveRects failure: %s (0x%08lX) Size: %u Total "
		         "%u Used: %u",
		         GetDxgiErrorString(hr), hr, MoveRectsBufferSize, MetadataBufferSize,
		         UsedBufferSize);
		return -1;
	}

	/* GetFrameDirtyRects */

	UsedBufferSize += MoveRectsBufferSize;

	DirtyRectsBufferSize = MetadataBufferSize - UsedBufferSize;
	pDirtyRectsBuffer = (RECT*)&(subsystem->dxgi->MetadataBuffer[UsedBufferSize]);

	hr = subsystem->dxgi->dxgiOutputDuplication->lpVtbl->GetFrameDirtyRects(
	    subsystem->dxgi->dxgiOutputDuplication, DirtyRectsBufferSize, pDirtyRectsBuffer,
	    &DirtyRectsBufferSize);

	if (FAILED(hr))
	{
		WLog_ERR(TAG,
		         "IDXGIOutputDuplication::GetFrameDirtyRects failure: %s (0x%08lX) Size: %u Total "
		         "%u Used: %u",
		         GetDxgiErrorString(hr), hr, DirtyRectsBufferSize, MetadataBufferSize,
		         UsedBufferSize);
		return -1;
	}

	numMoveRects = MoveRectsBufferSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);

	for (UINT i = 0; i < numMoveRects; i++)
	{
		pMoveRect = &pMoveRectBuffer[i];
		pSrcPt = &(pMoveRect->SourcePoint);
		pDstRect = &(pMoveRect->DestinationRect);

		if (!win_shadow_dxgi_surface_rect(subsystem->dxgi, pDstRect, &invalidRect))
			continue;

		region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion), &invalidRect);
	}

	numDirtyRects = DirtyRectsBufferSize / sizeof(RECT);

	for (UINT i = 0; i < numDirtyRects; i++)
	{
		pDirtyRect = &pDirtyRectsBuffer[i];

		if (!win_shadow_dxgi_surface_rect(subsystem->dxgi, pDirtyRect, &invalidRect))
			continue;

		region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion), &invalidRect);
	}

	return 1;
}

void win_shadow_dxgi_invalidate_output(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->base.server);

	rdpShadowSurface* surface = subsystem->base.server->surface;
	const winDxgiState* state = subsystem->dxgi;
	WINPR_ASSERT(surface);
	WINPR_ASSERT(state);

	const RECT local = { 0, 0, state->desktop.right - state->desktop.left,
		                 state->desktop.bottom - state->desktop.top };
	RECTANGLE_16 rect = WINPR_C_ARRAY_INIT;

	if (!win_shadow_dxgi_surface_rect(state, &local, &rect))
		return;

	EnterCriticalSection(&(surface->lock));
	(void)region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion), &rect);
	LeaveCriticalSection(&(surface->lock));
}

BOOL win_shadow_dxgi_frame_acquired(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);

	for (UINT32 index = 0; index < subsystem->outputCount; index++)
	{
		const winDxgiState* state = &subsystem->outputs[index];
		if (state->dxgiFrameAcquired && state->dxgiDesktopImage)
			return TRUE;
	}
	return FALSE;
}

int win_shadow_dxgi_copy_outputs(winShadowSubsystem* subsystem, REGION16* pending)
{
	BOOL copied = FALSE;
	BOOL failed = FALSE;
	REGION16 done = WINPR_C_ARRAY_INIT;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(pending);
	WINPR_ASSERT(subsystem->base.server);

	rdpShadowSurface* surface = subsystem->base.server->surface;
	WINPR_ASSERT(surface);

	const RECTANGLE_16 surfaceRect = { 0, 0, (UINT16)surface->width, (UINT16)surface->height };

	/* the damage of the outputs copied now is what the clients encode; what an output without
	 * a frame now leaves of it stays for its next frame */
	region16_init(&done);
	region16_clear(pending);

	for (UINT32 index = 0; index < subsystem->outputCount; index++)
	{
		REGION16 part = WINPR_C_ARRAY_INIT;
		RECTANGLE_16 outputRect = WINPR_C_ARRAY_INIT;

		win_shadow_dxgi_select(subsystem, index);
		const winDxgiState* state = subsystem->dxgi;
		const RECT local = { 0, 0, state->desktop.right - state->desktop.left,
			                 state->desktop.bottom - state->desktop.top };

		if (!win_shadow_dxgi_surface_rect(state, &local, &outputRect) ||
		    !rectangles_intersection(&outputRect, &surfaceRect, &outputRect))
			continue;

		region16_init(&part);

		if (!region16_intersect_rect(&part, &(surface->invalidRegion), &outputRect))
		{
			/* keep all of the output for its next frame rather than lose its damage */
			region16_uninit(&part);
			(void)region16_union_rect(pending, pending, &outputRect);
			failed = TRUE;
			continue;
		}

		if (region16_is_empty(&part))
		{
			region16_uninit(&part);
			continue;
		}

		int status = 0;
		if (state->dxgiFrameAcquired && state->dxgiDesktopImage)
		{
			RECTANGLE_16 invalidRect = *region16_extents(&part);
			(void)shadow_capture_align_clip_rect(&invalidRect, &outputRect);
			const int x = invalidRect.left;
			const int y = invalidRect.top;
			const int width = invalidRect.right - invalidRect.left;
			const int height = invalidRect.bottom - invalidRect.top;
			int nDstStep = 0;
			BYTE* pDstData = nullptr;

			WLog_INFO(TAG, "SurfaceCopy x: %d y: %d width: %d height: %d right: %d bottom: %d", x,
			          y, width, height, x + width, y + height);

			/* the output's own coordinates; fetch_frame_data copies the region to the staging
			 * texture origin */
			status = win_shadow_dxgi_fetch_frame_data(subsystem, &pDstData, &nDstStep,
			                                          x - state->surfaceX, y - state->surfaceY,
			                                          width, height);

			if ((status > 0) &&
			    !freerdp_image_copy_no_overlap(surface->data, surface->format, surface->scanline,
			                                   (UINT32)x, (UINT32)y, (UINT32)width, (UINT32)height,
			                                   pDstData, PIXEL_FORMAT_BGRX32, (UINT32)nDstStep, 0,
			                                   0, nullptr, FREERDP_FLIP_NONE))
				status = -1;
		}

		if (status > 0)
			copied = TRUE;
		else if (status < 0)
			failed = TRUE;

		{
			REGION16* to = (status > 0) ? &done : pending;
			UINT32 count = 0;
			const RECTANGLE_16* rects = region16_rects(&part, &count);
			for (UINT32 r = 0; r < count; r++)
				(void)region16_union_rect(to, to, &rects[r]);
		}

		region16_uninit(&part);
	}

	(void)region16_copy(&(surface->invalidRegion), &done);
	region16_uninit(&done);

	/* a copied output's damage is gone from the region: its frame must go out even when
	 * another output failed */
	if (copied)
		return 1;
	return failed ? -1 : 0;
}

#endif
