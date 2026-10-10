#pragma once

// Software emulation of WGL_NV_DX_interop for the game's movie player.
//
// diva.exe (v7.10) decodes movies with Media Foundation, converts each frame to
// RGB with a DXVA2 video processor into one of a pool of shared D3D9Ex render
// target textures, and then exposes those textures to its OpenGL renderer with
// WGL_NV_DX_interop. Wine does not implement that extension, so the game's
// interop loader (0x140427AB0) fails and the movie renderer is never created.
//
// When the real extension is unavailable (or emulation is forced), we fill the
// game's interop function table with the functions below. Instead of sharing
// the texture, "locking" an object reads the D3D9 render target back to system
// memory and uploads it into the GL texture the game registered.
//
// Include this header from dllmain.cpp only (framework.h defines globals).

#include <d3d9.h>
#include <GL/gl.h>

#pragma comment(lib, "opengl32.lib")

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif

namespace GLDXEmu
{
	// ---- Addresses inside diva.exe 7.10 (MoviePlayLib::GLDXInterop) ----

	// bool LoadGLDXInterop(void): resolves the functions below, sets the flag.
	constexpr uintptr_t ADDR_LOADER = 0x140427AB0;

	// Function pointer table filled by the loader.
	struct InteropTable
	{
		void* wglGetProcAddress;              // 0x1411A1A20
		void* wglGetCurrentContext;           // 0x1411A1A28
		void* glDeleteTextures;               // 0x1411A1A30
		void* glGenTextures;                  // 0x1411A1A38
		void* wglDXOpenDeviceNV;              // 0x1411A1A40
		void* wglDXCloseDeviceNV;             // 0x1411A1A48
		void* wglDXSetResourceShareHandleNV;  // 0x1411A1A50
		void* wglDXRegisterObjectNV;          // 0x1411A1A58
		void* wglDXUnregisterObjectNV;        // 0x1411A1A60
		void* wglDXLockObjectsNV;             // 0x1411A1A68
		void* wglDXUnlockObjectsNV;           // 0x1411A1A70
	};
	static_assert(sizeof(InteropTable) == 0x58, "unexpected InteropTable layout");

	InteropTable* const table = (InteropTable*)0x1411A1A20;
	volatile BYTE* const loadedFlag = (volatile BYTE*)0x1411A1A84;

	// The loader looks this name up; used to make sure we are patching the right exe.
	const char* const EXPECTED_NAME = (const char*)0x140A04490;

	// ---- GL entry points (all OpenGL 1.1, exported by opengl32.dll) ----

	typedef void (WINAPI* PFN_glBindTexture)(GLenum, GLuint);
	typedef void (WINAPI* PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
	typedef void (WINAPI* PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
	typedef void (WINAPI* PFN_glTexParameteri)(GLenum, GLenum, GLint);
	typedef void (WINAPI* PFN_glGetIntegerv)(GLenum, GLint*);
	typedef void (WINAPI* PFN_glPixelStorei)(GLenum, GLint);
	typedef GLenum (WINAPI* PFN_glGetError)(void);
	typedef void (WINAPI* PFN_glBindBuffer)(GLenum, GLuint);

	PFN_glBindTexture pglBindTexture;
	PFN_glTexImage2D pglTexImage2D;
	PFN_glTexSubImage2D pglTexSubImage2D;
	PFN_glTexParameteri pglTexParameteri;
	PFN_glGetIntegerv pglGetIntegerv;
	PFN_glPixelStorei pglPixelStorei;
	PFN_glGetError pglGetError;
	PFN_glBindBuffer pglBindBuffer; // optional (GL 1.5), resolved lazily with a context current
	bool bindBufferResolved = false;

	bool emulationActive = false;

	// ---- Emulated objects ----

	struct EmuDevice
	{
		IDirect3DDevice9* device;
	};

	struct EmuObject
	{
		IDirect3DDevice9* device;
		IDirect3DTexture9* texture;
		IDirect3DSurface9* level0;   // render target that the video processor writes to
		IDirect3DSurface9* sysmem;   // readback copy
		GLuint name;
		UINT width, height;
		D3DFORMAT format;
		unsigned int uploads;
	};

	inline bool IsRunningUnderWine()
	{
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
	}

	// D3D calls can, on some implementations, change the thread's current GL context.
	struct GLContextGuard
	{
		HGLRC context;
		HDC dc;
		GLContextGuard() : context(wglGetCurrentContext()), dc(wglGetCurrentDC()) {}
		void Restore()
		{
			if (context && wglGetCurrentContext() != context)
				wglMakeCurrent(dc, context);
		}
	};

	// Saves and restores the GL state touched by an upload.
	struct GLUploadState
	{
		GLint texture, rowLength, skipRows, skipPixels, alignment, unpackBuffer;

		void Save()
		{
			pglGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
			pglGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
			pglGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
			pglGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
			pglGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
			unpackBuffer = 0;
			if (pglBindBuffer)
			{
				pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
				if (unpackBuffer)
					pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			}
			pglPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
			pglPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		}

		void Restore()
		{
			pglBindTexture(GL_TEXTURE_2D, (GLuint)texture);
			pglPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
			pglPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
			pglPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
			pglPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
			if (pglBindBuffer && unpackBuffer)
				pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
		}
	};

	// Pixel unpack buffers (GL 2.1) would redirect glTexSubImage2D's source pointer,
	// so on contexts that have them we unbind any active one around our uploads.
	inline void ResolveOptionalGL()
	{
		if (bindBufferResolved || !wglGetCurrentContext())
			return;
		bindBufferResolved = true;

		const char* version = (const char*)glGetString(GL_VERSION);
		int major = 0, minor = 0;
		if (!version || sscanf(version, "%d.%d", &major, &minor) != 2)
			return;
		if (major < 2 || (major == 2 && minor < 1))
			return;

		pglBindBuffer = (PFN_glBindBuffer)wglGetProcAddress("glBindBuffer");
		PRINT("[DivaMovie] Interop emulation: OpenGL %d.%d%s\n", major, minor, pglBindBuffer ? "" : " (glBindBuffer missing)");
	}

	inline void ReleaseObject(EmuObject* obj)
	{
		SAFE_RELEASE(obj->sysmem);
		SAFE_RELEASE(obj->level0);
		SAFE_RELEASE(obj->texture);
		SAFE_RELEASE(obj->device);
		delete obj;
	}

	// Copies the current contents of the D3D9 render target into the GL texture.
	inline bool Upload(EmuObject* obj)
	{
		HRESULT result = S_OK;
		D3DLOCKED_RECT locked = {};
		bool isLocked = false;
		GLContextGuard guard;

		ASSERT(obj->device->GetRenderTargetData(obj->level0, obj->sysmem));
		ASSERT(obj->sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY));
		isLocked = true;
		guard.Restore();

		{
			GLUploadState state;
			state.Save();
			pglBindTexture(GL_TEXTURE_2D, obj->name);
			pglPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			pglPixelStorei(GL_UNPACK_ROW_LENGTH, locked.Pitch / 4);
			pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, obj->width, obj->height, GL_BGRA, GL_UNSIGNED_BYTE, locked.pBits);
			state.Restore();
		}

		if (debug && obj->uploads < 3)
		{
			const BYTE* center = (const BYTE*)locked.pBits + (obj->height / 2) * locked.Pitch + (obj->width / 2) * 4;
			PRINT("[DivaMovie] Interop emulation: uploaded %ux%u frame to texture %u (center pixel B=%u G=%u R=%u, glGetError=0x%x)\n",
				obj->width, obj->height, obj->name, center[0], center[1], center[2], pglGetError());
		}
		obj->uploads++;

	end:
		if (isLocked)
		{
			obj->sysmem->UnlockRect();
			guard.Restore();
		}
		return SUCCEEDED(result);
	}

	// ---- Emulated WGL_NV_DX_interop entry points ----

	BOOL WINAPI SetResourceShareHandle(void* dxObject, HANDLE shareHandle)
	{
		// Nothing to share: we work with the D3D9 object pointer directly.
		return TRUE;
	}

	HANDLE WINAPI OpenDevice(void* dxDevice)
	{
		if (!dxDevice)
		{
			SetLastError(ERROR_INVALID_PARAMETER);
			return NULL;
		}

		IDirect3DDevice9* device = NULL;
		if (FAILED(((IUnknown*)dxDevice)->QueryInterface(__uuidof(IDirect3DDevice9), (void**)&device)))
		{
			SetLastError(ERROR_INVALID_PARAMETER);
			return NULL;
		}

		ResolveOptionalGL();
		PRINT("[DivaMovie] Interop emulation: device opened\n");
		return new EmuDevice{ device };
	}

	BOOL WINAPI CloseDevice(HANDLE hDevice)
	{
		EmuDevice* dev = (EmuDevice*)hDevice;
		if (!dev)
			return FALSE;
		SAFE_RELEASE(dev->device);
		delete dev;
		return TRUE;
	}

	HANDLE WINAPI RegisterObject(HANDLE hDevice, void* dxObject, GLuint name, GLenum type, GLenum access)
	{
		HRESULT result = S_OK;
		EmuDevice* dev = (EmuDevice*)hDevice;
		EmuObject* obj = NULL;
		D3DSURFACE_DESC desc = {};
		GLContextGuard guard;

		if (!dev || !dxObject || type != GL_TEXTURE_2D)
		{
			PRINT("[DivaMovie] Interop emulation: unsupported RegisterObject (type 0x%x)\n", type);
			SetLastError(ERROR_INVALID_PARAMETER);
			return NULL;
		}

		obj = new EmuObject{};
		obj->name = name;

		ASSERT(((IUnknown*)dxObject)->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&obj->texture));
		ASSERT(obj->texture->GetLevelDesc(0, &desc));
		ASSERT(obj->texture->GetSurfaceLevel(0, &obj->level0));

		if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
		{
			PRINT("[DivaMovie] Interop emulation: unsupported texture format %u\n", (unsigned int)desc.Format);
			result = E_INVALIDARG;
			goto end;
		}

		ASSERT(obj->texture->GetDevice(&obj->device));
		ASSERT(obj->device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &obj->sysmem, NULL));

		obj->width = desc.Width;
		obj->height = desc.Height;
		obj->format = desc.Format;

		guard.Restore();

		// The game hands us a freshly generated texture name; give it storage.
		// RGB8 so the alpha channel always reads as 1, like an X8R8G8B8 surface.
		{
			GLUploadState state;
			state.Save();
			pglBindTexture(GL_TEXTURE_2D, name);
			pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, desc.Width, desc.Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
			pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			state.Restore();
		}

		PRINT("[DivaMovie] Interop emulation: registered %ux%u texture as GL texture %u\n", desc.Width, desc.Height, name);

	end:
		if (FAILED(result))
		{
			PRINT("[DivaMovie] Interop emulation: RegisterObject failed (0x%08x)\n", (unsigned int)result);
			ReleaseObject(obj);
			guard.Restore();
			SetLastError(ERROR_INVALID_DATA);
			return NULL;
		}
		return obj;
	}

	BOOL WINAPI UnregisterObject(HANDLE hDevice, HANDLE hObject)
	{
		EmuObject* obj = (EmuObject*)hObject;
		if (!obj)
			return FALSE;
		// The game deletes the GL texture itself.
		ReleaseObject(obj);
		return TRUE;
	}

	BOOL WINAPI LockObjects(HANDLE hDevice, GLint count, HANDLE* hObjects)
	{
		if (!hObjects)
			return FALSE;
		static LONG lockCount = 0;
		LONG n = InterlockedIncrement(&lockCount);
		if (n <= 3 || n % 600 == 0)
			PRINT("[DivaMovie] Interop emulation: lock #%ld (%d object(s))\n", n, count);
		for (GLint i = 0; i < count; i++)
		{
			EmuObject* obj = (EmuObject*)hObjects[i];
			// A failed readback leaves the previous frame in the texture; keep the movie running.
			if (obj && !Upload(obj))
				PRINT("[DivaMovie] Interop emulation: frame upload failed for GL texture %u\n", obj->name);
		}
		return TRUE;
	}

	BOOL WINAPI UnlockObjects(HANDLE hDevice, GLint count, HANDLE* hObjects)
	{
		return TRUE;
	}

	// ---- Installation ----

	inline bool IsSupportedExecutable()
	{
		// The loader address is code; the name string sits in .rdata. Both must match v7.10.
		const BYTE* loader = (const BYTE*)ADDR_LOADER;
		static const BYTE prologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }; // push rbx; sub rsp, 20h
		return memcmp(loader, prologue, sizeof(prologue)) == 0
			&& memcmp(EXPECTED_NAME, "wglDXOpenDeviceNV", sizeof("wglDXOpenDeviceNV")) == 0;
	}

	// Resolves the OpenGL 1.1 entry points used for uploads (shared with the FFmpeg player).
	inline bool LoadGL()
	{
		HMODULE gl = GetModuleHandleW(L"opengl32.dll");
		if (!gl)
			gl = LoadLibraryW(L"opengl32.dll");
		if (!gl)
			return false;

		pglBindTexture = (PFN_glBindTexture)GetProcAddress(gl, "glBindTexture");
		pglTexImage2D = (PFN_glTexImage2D)GetProcAddress(gl, "glTexImage2D");
		pglTexSubImage2D = (PFN_glTexSubImage2D)GetProcAddress(gl, "glTexSubImage2D");
		pglTexParameteri = (PFN_glTexParameteri)GetProcAddress(gl, "glTexParameteri");
		pglGetIntegerv = (PFN_glGetIntegerv)GetProcAddress(gl, "glGetIntegerv");
		pglPixelStorei = (PFN_glPixelStorei)GetProcAddress(gl, "glPixelStorei");
		pglGetError = (PFN_glGetError)GetProcAddress(gl, "glGetError");
		ResolveOptionalGL();
		return pglBindTexture && pglTexImage2D && pglTexSubImage2D && pglTexParameteri && pglGetIntegerv &&
			pglPixelStorei && pglGetError;
	}

	inline bool Install()
	{
		if (!LoadGL())
			return false;
		HMODULE gl = GetModuleHandleW(L"opengl32.dll");

		void* getProcAddress = (void*)GetProcAddress(gl, "wglGetProcAddress");
		void* getCurrentContext = (void*)GetProcAddress(gl, "wglGetCurrentContext");
		void* deleteTextures = (void*)GetProcAddress(gl, "glDeleteTextures");
		void* genTextures = (void*)GetProcAddress(gl, "glGenTextures");

		if (!getProcAddress || !getCurrentContext || !deleteTextures || !genTextures)
			return false;

		// The game resolves glDeleteTextures/glGenTextures through wglGetProcAddress, which is
		// not required to return OpenGL 1.1 functions (Wine returns NULL), so take them from
		// opengl32.dll's exports instead.
		table->wglGetProcAddress = getProcAddress;
		table->wglGetCurrentContext = getCurrentContext;
		table->glDeleteTextures = deleteTextures;
		table->glGenTextures = genTextures;
		table->wglDXOpenDeviceNV = (void*)OpenDevice;
		table->wglDXCloseDeviceNV = (void*)CloseDevice;
		table->wglDXSetResourceShareHandleNV = (void*)SetResourceShareHandle;
		table->wglDXRegisterObjectNV = (void*)RegisterObject;
		table->wglDXUnregisterObjectNV = (void*)UnregisterObject;
		table->wglDXLockObjectsNV = (void*)LockObjects;
		table->wglDXUnlockObjectsNV = (void*)UnlockObjects;
		*loadedFlag = 1;

		emulationActive = true;
		return true;
	}
}
