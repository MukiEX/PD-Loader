#include "framework.h"
#include "PluginConfigApi.h"

#include <evr.h>

#include <d3d9.h>
#include <dvdmedia.h>
#include <dxva2api.h>

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <SimpleIni.h>

bool forceSoftwareDecoding = false, forceHybridDecoding = false, debug = false;
bool forceInteropEmulation = false, disableInteropEmulation = false;
bool useFFmpegPlayer = true;

#include "GLDXInteropEmu.h"
#include "DivaMovieDiag.h"
#include "FFPlayer.h"

IDirect3DDeviceManager9* deviceManager;

IMFTransform* mfTransform;
int width;
int height;
unsigned long samplesize;
unsigned long subtype;
long srcStride;

IMFSample* dstSample;
IDirect3DSurface9* dstSurface;
IMFSample* srcSample;
IMFMediaBuffer* srcBuffer;

HRESULT initialize(IMFTransform* transform)
{
	HRESULT result = S_OK;

	IMFMediaType* type = NULL;
	IMFMediaBuffer* buffer = NULL;
	HANDLE hDevice = NULL;
	IDirectXVideoAccelerationService* service = NULL;

	AM_MEDIA_TYPE* format = NULL;
	VIDEOINFOHEADER2* header = NULL;
	UINT32 stride = 0;

	// Release what we already had beforehand
	SAFE_RELEASE(mfTransform);
	SAFE_RELEASE(dstSample);
	SAFE_RELEASE(dstSurface);
	SAFE_RELEASE(srcSample);
	SAFE_RELEASE(srcBuffer);

	ASSERT(transform->GetOutputCurrentType(0, &type));
	ASSERT(type->GetRepresentation(AM_MEDIA_TYPE_REPRESENTATION, (void**)&format));

	header = (VIDEOINFOHEADER2*)format->pbFormat;

	width = header->bmiHeader.biWidth;
	height = abs(header->bmiHeader.biHeight);
	samplesize = format->lSampleSize;
	subtype = format->subtype.Data1;

	// Row stride of the decoder's system memory output
	if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && (LONG)stride != 0)
		srcStride = abs((LONG)stride);
	else if (subtype == MAKEFOURCC('Y', 'U', 'Y', '2') || subtype == MAKEFOURCC('U', 'Y', 'V', 'Y'))
		srcStride = width * 2;
	else
		srcStride = width;

	PRINT("[DivaMovie] Software decoding output: %dx%d, format %.4s, stride %ld, sample size %lu\n",
		width, height, (const char*)&subtype, srcStride, samplesize);

	// System memory sample
	ASSERT(MFCreateSample(&srcSample));
	ASSERT(MFCreateMemoryBuffer(format->lSampleSize, &buffer));
	ASSERT(srcSample->AddBuffer(buffer));
	ASSERT(buffer->QueryInterface(&srcBuffer));

	// Video memory sample
	ASSERT(deviceManager->OpenDeviceHandle(&hDevice));
	ASSERT(deviceManager->GetVideoService(hDevice, __uuidof(IDirectXVideoAccelerationService), (void**)& service));
	ASSERT(service->CreateSurface(width, height, 0, (D3DFORMAT)subtype, D3DPOOL_DEFAULT, 0, DXVA2_VideoSoftwareRenderTarget, &dstSurface, NULL));
	ASSERT(MFCreateVideoSampleFromSurface(dstSurface, &dstSample));
	ASSERT(deviceManager->CloseDeviceHandle(hDevice));

	mfTransform = transform;
	mfTransform->AddRef();

end:
	if (format)
		type->FreeRepresentation(AM_MEDIA_TYPE_REPRESENTATION, format);

	SAFE_RELEASE(service);
	SAFE_RELEASE(buffer);
	SAFE_RELEASE(type);

	return result;
}

// Copies a decoded frame from the contiguous system memory buffer into the locked
// D3D9 surface plane by plane, so that a surface pitch wider than the frame
// (common on AMD and Intel) does not skew the image or misplace the chroma planes.
HRESULT copyFrame(BYTE* dst, LONG dstPitch, const BYTE* src, DWORD srcLength)
{
	const LONG s = srcStride;
	const int h = height;
	const int ch = height / 2;

	switch (subtype)
	{
	case MAKEFOURCC('N', 'V', '1', '2'):
		if (srcLength < (DWORD)(s * h + s * ch))
			break;
		MFCopyImage(dst, dstPitch, src, s, width, h);
		return MFCopyImage(dst + dstPitch * h, dstPitch, src + s * h, s, width, ch);

	case MAKEFOURCC('Y', 'V', '1', '2'):
	case MAKEFOURCC('I', '4', '2', '0'):
	case MAKEFOURCC('I', 'Y', 'U', 'V'):
	{
		const LONG cs = s / 2;
		const LONG cp = dstPitch / 2;
		const int cw = width / 2;
		if (srcLength < (DWORD)(s * h + 2 * cs * ch))
			break;
		MFCopyImage(dst, dstPitch, src, s, width, h);
		MFCopyImage(dst + dstPitch * h, cp, src + s * h, cs, cw, ch);
		return MFCopyImage(dst + dstPitch * h + cp * ch, cp, src + s * h + cs * ch, cs, cw, ch);
	}

	case MAKEFOURCC('Y', 'U', 'Y', '2'):
	case MAKEFOURCC('U', 'Y', 'V', 'Y'):
		if (srcLength < (DWORD)(s * h))
			break;
		return MFCopyImage(dst, dstPitch, src, s, width * 2, h);
	}

	// Unknown layout or unexpected size: copy the buffer as-is (previous behaviour)
	const DWORD bytes = srcLength < samplesize ? srcLength : samplesize;
	return MFCopyImage(dst, dstPitch, src, bytes, bytes, 1);
}

VTABLE_HOOK(HRESULT, IMFTransform, ProcessOutput, DWORD dwFlags, DWORD cOutputBufferCount,
	MFT_OUTPUT_DATA_BUFFER* pOutputSamples, DWORD* pdwStatus)
{
	if (pOutputSamples->pSample != NULL || deviceManager == NULL || cOutputBufferCount != 1)
		return originalProcessOutput(This, dwFlags, cOutputBufferCount, pOutputSamples, pdwStatus);

	HRESULT result = S_OK;
	D3DLOCKED_RECT rect;
	BYTE* buf = NULL;
	DWORD length = 0;
	bool srcLocked = false, dstLocked = false;
	LONGLONG duration, time;
	DWORD flags;

	if (mfTransform != This)
		ASSERT(initialize(This));

	pOutputSamples->pSample = srcSample;
	pOutputSamples->pSample->AddRef();

	result = originalProcessOutput(This, dwFlags, cOutputBufferCount, pOutputSamples, pdwStatus);
	if (FAILED(result))
	{
		// e.g. MF_E_TRANSFORM_NEED_MORE_INPUT: hand the caller back the NULL sample it gave us
		if (pOutputSamples->pSample == srcSample)
		{
			pOutputSamples->pSample->Release();
			pOutputSamples->pSample = NULL;
		}
		// The output type is renegotiated after a stream change; set up again on the next frame
		if (result == MF_E_TRANSFORM_STREAM_CHANGE)
			SAFE_RELEASE(mfTransform);
		goto end;
	}

	ASSERT(srcBuffer->Lock(&buf, NULL, &length));
	srcLocked = true;
	ASSERT(dstSurface->LockRect(&rect, NULL, 0));
	dstLocked = true;
	ASSERT(copyFrame((BYTE*)rect.pBits, rect.Pitch, buf, length));
	dstSurface->UnlockRect();
	dstLocked = false;
	srcBuffer->Unlock();
	srcLocked = false;

	ASSERT(srcSample->GetSampleDuration(&duration));
	ASSERT(dstSample->SetSampleDuration(duration));

	ASSERT(srcSample->GetSampleTime(&time));
	ASSERT(dstSample->SetSampleTime(time));

	ASSERT(srcSample->GetSampleFlags(&flags));
	ASSERT(dstSample->SetSampleFlags(flags));

	pOutputSamples->pSample->Release();
	pOutputSamples->pSample = dstSample;
	pOutputSamples->pSample->AddRef();
end:
	if (dstLocked)
		dstSurface->UnlockRect();
	if (srcLocked)
		srcBuffer->Unlock();
	return result;
}

VTABLE_HOOK(HRESULT, IMFTransform, ProcessMessage, MFT_MESSAGE_TYPE eMessage, ULONG_PTR ulParam)
{
	if (forceSoftwareDecoding)
	{
		if (eMessage == MFT_MESSAGE_SET_D3D_MANAGER) {
			PRINT("[DivaMovie] Force Software Decoding enabled\n");
			INSTALL_VTABLE_HOOK(This, ProcessOutput, 25);
			return S_OK;
		}
		return originalProcessMessage(This, eMessage, ulParam);
	}
	else
	{
		HRESULT result = originalProcessMessage(This, eMessage, ulParam);
		if (eMessage == MFT_MESSAGE_SET_D3D_MANAGER)
			PRINT("[DivaMovie] Decoder SET_D3D_MANAGER(%p): 0x%08x\n", (void*)ulParam, (unsigned int)result);
		if (eMessage == MFT_MESSAGE_SET_D3D_MANAGER && result == MF_E_UNSUPPORTED_D3D_TYPE)
		{
			PRINT("[DivaMovie] This system does not support DXVA hardware decoding\n");
			result = originalProcessMessage(This, eMessage, NULL);
			if (SUCCEEDED(result))
				INSTALL_VTABLE_HOOK(This, ProcessOutput, 25);
		}
		else if (!forceHybridDecoding && eMessage == MFT_MESSAGE_SET_D3D_MANAGER && result == 0)
		{
			// Under Wine the decoder accepts the D3D manager, but DivaMovie is still needed
			// for the WGL_NV_DX_interop emulation, so don't disable it there.
			if (GLDXEmu::IsRunningUnderWine())
			{
				PRINT("[DivaMovie] Decoder accepted the D3D manager (Wine); keeping DivaMovie enabled\n");
				return result;
			}

			static bool warn = false;
			if (!warn)
			{
				MessageBoxW(0, L"Your system supports DXVA hardware decoding and Force Hybrid Decoding is disabled.\nDivaMovie will be disabled.\n\nIt is HIGHLY recommended to restart the game now.", L"DivaMovie", MB_ICONINFORMATION);

				CSimpleIniW ini_reader;
				ini_reader.SetUnicode(false);
				ini_reader.LoadFile(MASTER_CONFIG_FILE);
				ini_reader.SetValue(L"plugins", L"DivaMovie.dva", L"0");
				ini_reader.SaveFile(MASTER_CONFIG_FILE);

				warn = true;
			}
		}
		return result;
	}
	
}

HOOK(void*, IMFTransformInitializer, 0x140420B90, void* a1, void* a2, IMFTransform** transform)
{
	void* result = originalIMFTransformInitializer(a1, a2, transform);
	PRINT("[DivaMovie] Video decoder created: %p\n", transform ? (void*)*transform : NULL);
	INSTALL_VTABLE_HOOK(*transform, ProcessMessage, 23);
	return result;
}

HOOK(HRESULT, DXVA2CreateDirect3DDeviceManager, PROC_ADDRESS("dxva2.dll", "DXVA2CreateDirect3DDeviceManager9"),
	UINT* pResetToken, IDirect3DDeviceManager9** ppDeviceManager)
{
	HRESULT result = originalDXVA2CreateDirect3DDeviceManager(pResetToken, ppDeviceManager);

	if (FAILED(result))
		return result;

	if (deviceManager)
		deviceManager->Release();

	deviceManager = *ppDeviceManager;
	deviceManager->AddRef();

	PRINT("[DivaMovie] D3D device manager created\n");
	Diag::HookDeviceManager(deviceManager);

	return result;
}

// The movie player's WGL_NV_DX_interop loader. Falls back to emulation when the
// extension is unavailable (Wine) or when emulation is forced.
HOOK(bool, GLDXInteropLoad, GLDXEmu::ADDR_LOADER)
{
	PRINT("[DivaMovie] Movie interop loader called (GL context %p)\n", (void*)wglGetCurrentContext());
	if (*GLDXEmu::loadedFlag)
		return true;

	if (!forceInteropEmulation)
	{
		if (originalGLDXInteropLoad())
		{
			PRINT("[DivaMovie] Using native WGL_NV_DX_interop\n");
			return true;
		}
		if (disableInteropEmulation)
		{
			PRINT("[DivaMovie] WGL_NV_DX_interop is not available and interop emulation is disabled\n");
			return false;
		}
		PRINT("[DivaMovie] WGL_NV_DX_interop is not available, using interop emulation\n");
	}
	else
	{
		PRINT("[DivaMovie] Force Interop Emulation enabled\n");
	}

	if (GLDXEmu::Install())
		return true;

	PRINT("[DivaMovie] Failed to install interop emulation\n");
	return false;
}

// The game's movie player factory. Replaced by the FFmpeg player when the FFmpeg
// libraries are available next to the plugin; otherwise the original player is used.
HOOK(HRESULT, CreatePlayer, FFPlayer::ADDR_CREATE_PLAYER, void** player)
{
	if (useFFmpegPlayer && player && FFPlayer::EnsureFFmpeg())
	{
		*player = new FFPlayer::Player();
		PRINT("[DivaMovie] FFmpeg player created\n");
		return S_OK;
	}
	return originalCreatePlayer(player);
}

void loadConfig() {
	forceSoftwareDecoding = GetPrivateProfileIntW(L"general", L"force_software_decoding", 0, CONFIG_FILE) > 0 ? true : false;
	forceHybridDecoding = GetPrivateProfileIntW(L"general", L"force_hybrid_decoding", 0, CONFIG_FILE) > 0 ? true : false;
	forceInteropEmulation = GetPrivateProfileIntW(L"general", L"force_interop_emulation", 0, CONFIG_FILE) > 0 ? true : false;
	disableInteropEmulation = GetPrivateProfileIntW(L"general", L"disable_interop_emulation", 0, CONFIG_FILE) > 0 ? true : false;
	useFFmpegPlayer = GetPrivateProfileIntW(L"general", L"ffmpeg_player", 1, CONFIG_FILE) > 0 ? true : false;
	FFPlayer::movieAudio = GetPrivateProfileIntW(L"general", L"movie_audio", 0, CONFIG_FILE) > 0 ? true : false;
	debug = GetPrivateProfileIntW(L"general", L"debug", 0, CONFIG_FILE) > 0 ? true : false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		loadConfig();
		Diag::LogEnvironment();
		Diag::InstallTracing();
		INSTALL_HOOK(DXVA2CreateDirect3DDeviceManager);
		INSTALL_HOOK(IMFTransformInitializer);

		// Check before hooking: Detours overwrites the bytes being checked.
		if (useFFmpegPlayer)
		{
			if (FFPlayer::IsSupportedExecutable())
				INSTALL_HOOK(CreatePlayer)
			else
				PRINT("[DivaMovie] Unrecognised movie player code; FFmpeg player unavailable\n");
		}
		if (GLDXEmu::IsSupportedExecutable())
			INSTALL_HOOK(GLDXInteropLoad)
		else
			PRINT("[DivaMovie] Unrecognised movie interop code; interop emulation unavailable\n");
	}

	return TRUE;
}

PluginConfig::PluginConfigOption config[] = {
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"ffmpeg_player", L"general", CONFIG_FILE, L"FFmpeg Movie Player", L"Play movies with FFmpeg instead of Media Foundation (needs the FFmpeg DLLs in the plugins folder).\nWorks under Wine/Proton and supports WMV and MP4 movies.", true } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"movie_audio", L"general", CONFIG_FILE, L"Movie Audio", L"Play audio tracks of movies (FFmpeg player only).\nNever plays during songs, where the video follows the song.", false } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"force_software_decoding", L"general", CONFIG_FILE, L"Force Software Decoding", L"Use software decoding even on systems that support DXVA hardware decoding.", false } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"force_hybrid_decoding", L"general", CONFIG_FILE, L"Force Hybrid Decoding", L"Do not disable DivaMovie on systems that support DXVA hardware decoding.\nCan cause issues on recent drivers.", false } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"force_interop_emulation", L"general", CONFIG_FILE, L"Force Interop Emulation", L"Copy movie frames from Direct3D to OpenGL through system memory even if the driver supports WGL_NV_DX_interop.\nEmulation is used automatically when the extension is missing (e.g. Wine).", false } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"disable_interop_emulation", L"general", CONFIG_FILE, L"Disable Interop Emulation", L"Never emulate WGL_NV_DX_interop, even when the extension is missing.", false } },
	{ PluginConfig::CONFIG_BOOLEAN, new PluginConfig::PluginConfigBooleanData{ L"debug", L"general", CONFIG_FILE, L"Debug (slow!)", L"Enable PRINT (possibly at the cost of performance).", false } },
};

extern "C" __declspec(dllexport) LPCWSTR GetPluginName(void)
{
	return L"DivaMovie";
}

extern "C" __declspec(dllexport) LPCWSTR GetPluginDescription(void)
{
	return L"DivaMovie Plugin by Skyth\n\nDivaMovie enables movies on systems that do not support DXVA hardware decoding,\nand on systems without WGL_NV_DX_interop (e.g. Wine/Proton).";
}

extern "C" __declspec(dllexport) PluginConfig::PluginConfigArray GetPluginOptions(void)
{
	return PluginConfig::PluginConfigArray{ _countof(config), config };
}
