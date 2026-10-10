#pragma once

// Logging and pipeline diagnostics (active only with debug = 1).
// Include from dllmain.cpp only, after the globals.

#include <stdio.h>
#include <stdarg.h>

namespace Diag
{
	SRWLOCK logLock = SRWLOCK_INIT;
	FILE* logFile = NULL;
	bool logOpened = false;

	volatile LONG bltCount = 0, bltFailCount = 0;
}

void LogPrint(const char* format, ...)
{
	AcquireSRWLockExclusive(&Diag::logLock);

	if (!Diag::logOpened)
	{
		Diag::logOpened = true;
		Diag::logFile = _wfopen(LOG_FILE_STRING.c_str(), L"w");
	}

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);

	if (Diag::logFile)
	{
		va_start(args, format);
		vfprintf(Diag::logFile, format, args);
		va_end(args);
		fflush(Diag::logFile);
	}

	ReleaseSRWLockExclusive(&Diag::logLock);
}

namespace Diag
{
	inline const char* FormatName(D3DFORMAT format, char* buffer)
	{
		if ((DWORD)format > 0xFF)
		{
			memcpy(buffer, &format, 4);
			buffer[4] = 0;
		}
		else
			sprintf(buffer, "%u", (unsigned int)format);
		return buffer;
	}

	inline void LogEnvironment()
	{
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		typedef const char* (__cdecl* PFN_wine_get_version)(void);
		PFN_wine_get_version wineVersion = ntdll ? (PFN_wine_get_version)GetProcAddress(ntdll, "wine_get_version") : NULL;
		PRINT("[DivaMovie] Loaded (FFmpeg player build 2). Wine: %s\n", wineVersion ? wineVersion() : "no");

		HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
		if (!d3d9)
			d3d9 = LoadLibraryW(L"d3d9.dll");
		WCHAR path[MAX_PATH] = L"?";
		if (d3d9)
			GetModuleFileNameW(d3d9, path, MAX_PATH);
		PRINT("[DivaMovie] d3d9.dll: %ls\n", path);
	}
}

// ---- DXVA2 video processor tracing ----

VTABLE_HOOK(HRESULT, IDirectXVideoProcessor, VideoProcessBlt, IDirect3DSurface9* pRenderTarget,
	const DXVA2_VideoProcessBltParams* pBltParams, const DXVA2_VideoSample* pSamples, UINT NumSamples, HANDLE* pHandleComplete)
{
	HRESULT result = originalVideoProcessBlt(This, pRenderTarget, pBltParams, pSamples, NumSamples, pHandleComplete);

	LONG count = InterlockedIncrement(&Diag::bltCount);
	if (FAILED(result))
		InterlockedIncrement(&Diag::bltFailCount);

	if (count <= 3 || (FAILED(result) && Diag::bltFailCount <= 10) || count % 600 == 0)
	{
		D3DSURFACE_DESC rt = {}, src = {};
		char rtName[16], srcName[16];
		if (pRenderTarget)
			pRenderTarget->GetDesc(&rt);
		if (NumSamples > 0 && pSamples && pSamples[0].SrcSurface)
			pSamples[0].SrcSurface->GetDesc(&src);
		PRINT("[DivaMovie] VideoProcessBlt #%ld: 0x%08x (src %ux%u %s pool %u -> rt %ux%u %s, %ld failed so far)\n",
			count, (unsigned int)result, src.Width, src.Height, Diag::FormatName(src.Format, srcName), (unsigned int)src.Pool,
			rt.Width, rt.Height, Diag::FormatName(rt.Format, rtName), Diag::bltFailCount);
	}
	return result;
}

VTABLE_HOOK(HRESULT, IDirectXVideoProcessorService, CreateVideoProcessor, REFGUID VideoProcDeviceGuid,
	const DXVA2_VideoDesc* pVideoDesc, D3DFORMAT RenderTargetFormat, UINT MaxNumSubStreams, IDirectXVideoProcessor** ppVidProcess)
{
	HRESULT result = originalCreateVideoProcessor(This, VideoProcDeviceGuid, pVideoDesc, RenderTargetFormat, MaxNumSubStreams, ppVidProcess);

	char inName[16], rtName[16];
	PRINT("[DivaMovie] CreateVideoProcessor {%08lx-...}: %ux%u %s -> %s: 0x%08x\n",
		VideoProcDeviceGuid.Data1, pVideoDesc ? pVideoDesc->SampleWidth : 0, pVideoDesc ? pVideoDesc->SampleHeight : 0,
		Diag::FormatName(pVideoDesc ? pVideoDesc->Format : D3DFMT_UNKNOWN, inName), Diag::FormatName(RenderTargetFormat, rtName),
		(unsigned int)result);

	if (SUCCEEDED(result) && ppVidProcess && *ppVidProcess)
		INSTALL_VTABLE_HOOK(*ppVidProcess, VideoProcessBlt, 8);

	return result;
}

VTABLE_HOOK(HRESULT, IDirect3DDeviceManager9, GetVideoService, HANDLE hDevice, REFIID riid, void** ppService)
{
	HRESULT result = originalGetVideoService(This, hDevice, riid, ppService);

	if (IsEqualGUID(riid, __uuidof(IDirectXVideoProcessorService)))
	{
		PRINT("[DivaMovie] GetVideoService(IDirectXVideoProcessorService): 0x%08x\n", (unsigned int)result);
		if (SUCCEEDED(result) && ppService && *ppService)
			INSTALL_VTABLE_HOOK(*ppService, CreateVideoProcessor, 11);
	}
	return result;
}

namespace Diag
{
	inline void HookDeviceManager(IDirect3DDeviceManager9* manager)
	{
		if (debug && manager)
			INSTALL_VTABLE_HOOK(manager, GetVideoService, 9);
	}
}

// ---- Media Foundation source / decoder lookup tracing ----

#include <mfidl.h>

namespace Diag
{
	inline void FormatGuid(const GUID& g, char* buffer)
	{
		// FOURCC-based subtypes ({XXXXXXXX-0000-0010-8000-00AA00389B71}) print as text
		static const BYTE fourccTail[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
		if (g.Data2 == 0 && g.Data3 == 0x10 && memcmp(g.Data4, fourccTail, 8) == 0 && g.Data1 > 0xFF)
		{
			memcpy(buffer, &g.Data1, 4);
			buffer[4] = 0;
			return;
		}
		sprintf(buffer, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3,
			g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	}

	inline void LogSourceStreams(IUnknown* object)
	{
		IMFMediaSource* source = NULL;
		IMFPresentationDescriptor* pd = NULL;
		DWORD count = 0;

		if (FAILED(object->QueryInterface(__uuidof(IMFMediaSource), (void**)&source)))
		{
			PRINT("[DivaMovie]   object is not an IMFMediaSource\n");
			return;
		}
		HRESULT hr = source->CreatePresentationDescriptor(&pd);
		if (FAILED(hr))
		{
			PRINT("[DivaMovie]   CreatePresentationDescriptor: 0x%08x\n", (unsigned int)hr);
			source->Release();
			return;
		}
		pd->GetStreamDescriptorCount(&count);
		PRINT("[DivaMovie]   %lu stream(s)\n", count);
		for (DWORD i = 0; i < count; i++)
		{
			BOOL selected = FALSE;
			IMFStreamDescriptor* sd = NULL;
			IMFMediaTypeHandler* handler = NULL;
			IMFMediaType* type = NULL;
			GUID major = {}, subtype = {};
			char majorName[48] = "?", subName[48] = "?";

			if (SUCCEEDED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)) &&
				SUCCEEDED(sd->GetMediaTypeHandler(&handler)) &&
				SUCCEEDED(handler->GetCurrentMediaType(&type)))
			{
				type->GetGUID(MF_MT_MAJOR_TYPE, &major);
				type->GetGUID(MF_MT_SUBTYPE, &subtype);
				FormatGuid(major, majorName);
				FormatGuid(subtype, subName);
			}
			PRINT("[DivaMovie]   stream %lu: major %s subtype %s%s\n", i,
				IsEqualGUID(major, MFMediaType_Video) ? "video" : IsEqualGUID(major, MFMediaType_Audio) ? "audio" : majorName,
				subName, selected ? " (selected)" : "");

			SAFE_RELEASE(type);
			SAFE_RELEASE(handler);
			SAFE_RELEASE(sd);
		}
		pd->Release();
		source->Release();
	}
}

VTABLE_HOOK(HRESULT, IMFSourceResolver, CreateObjectFromURL, LPCWSTR pwszURL, DWORD dwFlags, IPropertyStore* pProps,
	MF_OBJECT_TYPE* pObjectType, IUnknown** ppObject)
{
	HRESULT result = originalCreateObjectFromURL(This, pwszURL, dwFlags, pProps, pObjectType, ppObject);
	PRINT("[DivaMovie] CreateObjectFromURL(%ls, flags 0x%lx): 0x%08x\n", pwszURL ? pwszURL : L"(null)", dwFlags, (unsigned int)result);
	if (SUCCEEDED(result) && ppObject && *ppObject)
		Diag::LogSourceStreams(*ppObject);
	return result;
}

HOOK(HRESULT, TraceSourceResolver, PROC_ADDRESS("mfplat.dll", "MFCreateSourceResolver"), IMFSourceResolver** ppISourceResolver)
{
	HRESULT result = originalTraceSourceResolver(ppISourceResolver);
	PRINT("[DivaMovie] MFCreateSourceResolver: 0x%08x\n", (unsigned int)result);
	if (SUCCEEDED(result) && ppISourceResolver && *ppISourceResolver)
		INSTALL_VTABLE_HOOK(*ppISourceResolver, CreateObjectFromURL, 3);
	return result;
}

HOOK(HRESULT, TraceMFStartup, PROC_ADDRESS("mfplat.dll", "MFStartup"), ULONG Version, DWORD dwFlags)
{
	HRESULT result = originalTraceMFStartup(Version, dwFlags);
	PRINT("[DivaMovie] MFStartup(0x%lx): 0x%08x\n", Version, (unsigned int)result);
	return result;
}

HOOK(HRESULT, TraceMFTEnum, PROC_ADDRESS("mfplat.dll", "MFTEnum"), GUID guidCategory, UINT32 Flags,
	MFT_REGISTER_TYPE_INFO* pInputType, MFT_REGISTER_TYPE_INFO* pOutputType, IMFAttributes* pAttributes,
	CLSID** ppclsidMFT, UINT32* pcMFTs)
{
	HRESULT result = originalTraceMFTEnum(guidCategory, Flags, pInputType, pOutputType, pAttributes, ppclsidMFT, pcMFTs);
	char inName[48] = "-", outName[48] = "-", first[48] = "-";
	if (pInputType)
		Diag::FormatGuid(pInputType->guidSubtype, inName);
	if (pOutputType)
		Diag::FormatGuid(pOutputType->guidSubtype, outName);
	if (SUCCEEDED(result) && pcMFTs && *pcMFTs && ppclsidMFT && *ppclsidMFT)
		Diag::FormatGuid((*ppclsidMFT)[0], first);
	PRINT("[DivaMovie] MFTEnum(in %s, out %s): 0x%08x, %u found, first %s\n", inName, outName, (unsigned int)result,
		pcMFTs ? *pcMFTs : 0, first);
	return result;
}

HOOK(HRESULT, TraceD3DCreate9Ex, PROC_ADDRESS("d3d9.dll", "Direct3DCreate9Ex"), UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	HRESULT result = originalTraceD3DCreate9Ex(SDKVersion, ppD3D);
	PRINT("[DivaMovie] Direct3DCreate9Ex: 0x%08x\n", (unsigned int)result);
	return result;
}


// ---- The game's own MoviePlayLib enter/leave trace (a no-op stub in release builds) ----

namespace Diag
{
	const wchar_t* const TRACE_ENTER = (const wchar_t*)0x140A03918; // L">%S(%p)\n"
	const wchar_t* const TRACE_LEAVE = (const wchar_t*)0x140A03900; // L"<%S(%p)\n"

	struct TraceCounter { const char* name; LONG count; };
	TraceCounter traceCounters[128];
	SRWLOCK traceLock = SRWLOCK_INIT;

	// Returns how many times this function name has been traced (including this call)
	inline LONG CountTrace(const char* name)
	{
		LONG n = 0;
		AcquireSRWLockExclusive(&traceLock);
		for (auto& c : traceCounters)
		{
			if (c.name == name || !c.name)
			{
				c.name = name;
				n = ++c.count;
				break;
			}
		}
		ReleaseSRWLockExclusive(&traceLock);
		return n;
	}

	inline bool IsTraceStubPresent()
	{
		// mov [rsp+10h],rdx / mov [rsp+18h],r8 / mov [rsp+20h],r9 / ret
		static const BYTE stub[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0xC3 };
		return memcmp((const void*)0x1404241F0, stub, sizeof(stub)) == 0
			&& memcmp(TRACE_ENTER, L">%S(%p)", 7 * sizeof(wchar_t)) == 0
			&& memcmp(TRACE_LEAVE, L"<%S(%p)", 7 * sizeof(wchar_t)) == 0;
	}
}

HOOK(void, MoviePlayTrace, 0x1404241F0, const wchar_t* format, const char* name, void* object)
{
	if ((format != Diag::TRACE_ENTER && format != Diag::TRACE_LEAVE) || !name)
		return;
	LONG n = Diag::CountTrace(name);
	if (n <= 8 || n == 100 || n == 1000 || n % 10000 == 0)
		PRINT("[MoviePlayLib] %c %s(%p) [thread %lu, #%ld]\n", format == Diag::TRACE_ENTER ? '>' : '<', name, object,
			GetCurrentThreadId(), n);
}


// ---- Crash reporting: logs faults (module+offset, registers, likely return addresses) ----

namespace Diag
{
	volatile LONG crashReports = 0;

	inline void DescribeAddress(DWORD64 address, char* buffer, size_t size)
	{
		HMODULE module = NULL;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)address, &module) && module)
		{
			char path[MAX_PATH];
			if (GetModuleFileNameA(module, path, MAX_PATH))
			{
				const char* base = strrchr(path, '\\');
				base = base ? base + 1 : path;
				snprintf(name, sizeof(name), "%s", base);
			}
			snprintf(buffer, size, "%s+0x%llx", name, (unsigned long long)(address - (DWORD64)module));
		}
		else
			snprintf(buffer, size, "0x%llx", (unsigned long long)address);
	}

	inline bool IsCodeAddress(DWORD64 value)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (value < 0x10000 || !VirtualQuery((LPCVOID)value, &mbi, sizeof(mbi)))
			return false;
		return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
			(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
	}

	LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
	{
		DWORD code = info->ExceptionRecord->ExceptionCode;
		if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
			code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
			return EXCEPTION_CONTINUE_SEARCH;
		if (InterlockedIncrement(&crashReports) > 8)
			return EXCEPTION_CONTINUE_SEARCH;

		CONTEXT* c = info->ContextRecord;
		char where[MAX_PATH + 32];
		DescribeAddress(c->Rip, where, sizeof(where));
		PRINT("[DivaMovie] *** Exception 0x%08lx at %s (thread %lu)\n", code, where, GetCurrentThreadId());
		if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
			PRINT("[DivaMovie]     %s address 0x%llx\n",
				info->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute",
				(unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
		PRINT("[DivaMovie]     rax=%llx rbx=%llx rcx=%llx rdx=%llx\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
		PRINT("[DivaMovie]     rsi=%llx rdi=%llx r8=%llx r9=%llx rsp=%llx\n", c->Rsi, c->Rdi, c->R8, c->R9, c->Rsp);

		// Likely return addresses on the stack (heuristic scan)
		DWORD64* stack = (DWORD64*)c->Rsp;
		int found = 0;
		for (int i = 0; i < 512 && found < 16; i++)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(&stack[i], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
				break;
			if (IsCodeAddress(stack[i]))
			{
				DescribeAddress(stack[i], where, sizeof(where));
				PRINT("[DivaMovie]     stack[%d]: %s\n", i, where);
				found++;
			}
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

namespace Diag
{
	inline void InstallTracing()
	{
		if (!debug)
			return;
		AddVectoredExceptionHandler(1, CrashHandler);
		INSTALL_HOOK(TraceMFStartup);
		INSTALL_HOOK(TraceSourceResolver);
		INSTALL_HOOK(TraceMFTEnum);
		INSTALL_HOOK(TraceD3DCreate9Ex);
		if (IsTraceStubPresent())
			INSTALL_HOOK(MoviePlayTrace)
		else
			PRINT("[DivaMovie] MoviePlayLib trace stub not recognised\n");
	}
}
