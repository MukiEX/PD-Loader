#pragma once

// Replacement for diva.exe's MoviePlayLib::Player, backed by FFmpeg.
//
// The game creates movie players through CreatePlayer (0x1404255C0) and only talks
// to them through a 20-slot COM-style interface. We return our own object with the
// same interface, so Media Foundation, DXVA2, Direct3D 9 and WGL_NV_DX_interop are
// never involved: frames are decoded on the CPU and uploaded into a game texture.
//
// Include from dllmain.cpp only, after GLDXInteropEmu.h.

#include "FFMovie.h"
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace FFPlayer
{
	// ---- diva.exe 7.10 ----

	constexpr uintptr_t ADDR_CREATE_PLAYER = 0x1404255C0;   // HRESULT CreatePlayer(IPlayer** out)

	// GameTexture* CreateTexture(id, internalFormat, width, height, levels, data, mipmaps)
	typedef void* (__fastcall* PFN_CreateGameTexture)(UINT32 id, UINT32 internalFormat, int width, int height,
		int levels, void** data, bool mipmaps);
	const PFN_CreateGameTexture CreateGameTexture = (PFN_CreateGameTexture)0x14069B8E0;
	typedef void (__fastcall* PFN_ReleaseGameTexture)(void* texture);
	const PFN_ReleaseGameTexture ReleaseGameTexture = (PFN_ReleaseGameTexture)0x14069DA70;
	volatile WORD* const textureIdCounter = (volatile WORD*)0x1411A1A78; // shared with the original movie player
	constexpr UINT32 TEXTURE_ID_BASE = 0x24000000;
	constexpr UINT32 GL_RGBA8_FORMAT = 0x8058;
	constexpr size_t GAME_TEXTURE_NAME_OFFSET = 0x10; // GLuint inside the game's texture struct

	inline bool IsSupportedExecutable()
	{
		static const BYTE createPlayer[] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x3D };   // push rsi; sub rsp,20h; cmp [rip+..],0
		static const BYTE createTexture[] = { 0x48, 0x83, 0xEC, 0x48, 0x0F, 0xB6, 0x84, 0x24 };  // sub rsp,48h; movzx eax,byte [rsp+..]
		static const BYTE releaseTexture[] = { 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56 };       // mov [rsp+20h],rsi; push r14
		return memcmp((void*)ADDR_CREATE_PLAYER, createPlayer, sizeof(createPlayer)) == 0 &&
			memcmp((void*)CreateGameTexture, createTexture, sizeof(createTexture)) == 0 &&
			memcmp((void*)ReleaseGameTexture, releaseTexture, sizeof(releaseTexture)) == 0;
	}

	// Player states as reported by MoviePlayLib (see the game's switch at 0x14041F656)
	enum State : LONG
	{
		STATE_IDLE = 0,
		STATE_OPENING = 1,
		STATE_OPENED = 2,
		STATE_ERROR = 3,
		STATE_PAUSED = 4,
		STATE_PLAYING = 5,
		STATE_STOPPED = 6,
	};

	struct VideoInfo
	{
		int width, height;
		int textureWidth, textureHeight;
		float framesPerSecond;
		float pixelAspect;
	};

	FFLib ff;
	bool ffTried = false;
	SRWLOCK ffLoadLock = SRWLOCK_INIT;
	bool movieAudio = false;

	inline bool EnsureFFmpeg()
	{
		AcquireSRWLockExclusive(&ffLoadLock);
		if (!ffTried)
		{
			ffTried = true;
			std::wstring directory = DirPath() + L"\\plugins\\";
			if (ff.Load(directory))
			{
				PRINT("[DivaMovie] FFmpeg loaded (avcodec %u.%u.%u)\n", ff.avcodec_version() >> 16,
					(ff.avcodec_version() >> 8) & 0xFF, ff.avcodec_version() & 0xFF);
			}
			else
			{
				PRINT("[DivaMovie] FFmpeg not available (%s); using the game's movie player\n", ff.error.c_str());
			}
		}
		ReleaseSRWLockExclusive(&ffLoadLock);
		return ff.loaded;
	}

	// ---- waveOut audio (optional) ----

	class WaveOutSink : public AudioSink
	{
	public:
		bool Open()
		{
			WAVEFORMATEX format = {};
			format.wFormatTag = WAVE_FORMAT_PCM;
			format.nChannels = CHANNELS;
			format.nSamplesPerSec = SAMPLE_RATE;
			format.wBitsPerSample = 16;
			format.nBlockAlign = CHANNELS * 2;
			format.nAvgBytesPerSec = SAMPLE_RATE * format.nBlockAlign;

			event = CreateEventW(NULL, FALSE, FALSE, NULL);
			if (!event || waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
			{
				device = NULL;
				return false;
			}
			waveOutPause(device);
			for (int i = 0; i < BUFFERS; i++)
			{
				headers[i].lpData = (LPSTR)blocks[i];
				headers[i].dwBufferLength = sizeof(blocks[i]);
				waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR));
				headers[i].dwFlags |= WHDR_DONE; // free to fill
			}
			running = true;
			thread = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { ((WaveOutSink*)p)->Feed(); return 0; }, this, 0, NULL);
			return thread != NULL;
		}

		~WaveOutSink() override
		{
			running = false;
			if (event)
				SetEvent(event);
			if (thread)
			{
				WaitForSingleObject(thread, INFINITE);
				CloseHandle(thread);
			}
			if (device)
			{
				waveOutReset(device);
				for (int i = 0; i < BUFFERS; i++)
					waveOutUnprepareHeader(device, &headers[i], sizeof(WAVEHDR));
				waveOutClose(device);
			}
			if (event)
				CloseHandle(event);
		}

		void Write(const int16_t* samples, int frames) override
		{
			size_t values = (size_t)frames * CHANNELS;
			while (running && !aborted)
			{
				AcquireSRWLockExclusive(&lock);
				size_t space = RING - used;
				// Only apply backpressure while audio is actually playing; otherwise
				// (paused, or never started because the video follows the song) drop
				// what does not fit so video decoding never stalls.
				if (!playing && space < values)
				{
					ReleaseSRWLockExclusive(&lock);
					return;
				}
				if (space >= values || space == RING)
				{
					size_t n = values < space ? values : space;
					for (size_t i = 0; i < n; i++)
						ring[(start + used + i) % RING] = samples[i];
					used += n;
					ReleaseSRWLockExclusive(&lock);
					return;
				}
				ReleaseSRWLockExclusive(&lock);
				Sleep(5);
			}
		}

		void Play() { playing = true; if (device) waveOutRestart(device); }
		void Pause() { playing = false; if (device) waveOutPause(device); }
		void Abort() { aborted = true; }
		void SetVolume(float v) { volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

	private:
		static const int BUFFERS = 4;
		static const int BLOCK_FRAMES = SAMPLE_RATE / 25; // 40 ms
		static const size_t RING = (size_t)SAMPLE_RATE * CHANNELS * 2; // 2 s

		HWAVEOUT device = NULL;
		HANDLE event = NULL, thread = NULL;
		volatile bool running = false;
		volatile bool playing = false;
		volatile bool aborted = false;
		WAVEHDR headers[BUFFERS] = {};
		int16_t blocks[BUFFERS][BLOCK_FRAMES * CHANNELS];
		SRWLOCK lock = SRWLOCK_INIT;
		int16_t ring[RING];
		size_t start = 0, used = 0;
		volatile float volume = 1.0f;

		void Feed()
		{
			while (running)
			{
				for (int i = 0; i < BUFFERS && running; i++)
				{
					if (!(headers[i].dwFlags & WHDR_DONE))
						continue;
					int16_t* block = blocks[i];
					size_t values = BLOCK_FRAMES * CHANNELS;
					AcquireSRWLockExclusive(&lock);
					size_t n = used < values ? used : values;
					float v = volume;
					for (size_t k = 0; k < n; k++)
						block[k] = (int16_t)(ring[(start + k) % RING] * v);
					start = (start + n) % RING;
					used -= n;
					ReleaseSRWLockExclusive(&lock);
					memset(block + n, 0, (values - n) * sizeof(int16_t)); // underrun: silence
					headers[i].dwFlags &= ~WHDR_DONE;
					waveOutWrite(device, &headers[i], sizeof(WAVEHDR));
				}
				WaitForSingleObject(event, 100);
			}
		}
	};

	// ---- Texture object handed to the game (slot 3 returns the game texture) ----

	struct TextureObject;
	struct TextureVtbl
	{
		HRESULT (*QueryInterface)(TextureObject*, REFIID, void**);
		ULONG (*AddRef)(TextureObject*);
		ULONG (*Release)(TextureObject*);
		void* (*GetGameTexture)(TextureObject*);
		void* (*Destroy)(TextureObject*, int);
	};

	struct TextureObject
	{
		const TextureVtbl* vtbl;
		volatile LONG references;
		void* gameTexture;
		int width, height;

		static TextureObject* Create(int width, int height)
		{
			UINT32 id = TEXTURE_ID_BASE | *textureIdCounter;
			void* texture = CreateGameTexture(id, GL_RGBA8_FORMAT, width, height, 0, NULL, false);
			if (!texture)
				return nullptr;
			(*textureIdCounter)++;
			TextureObject* object = new TextureObject{ &vtable, 1, texture, width, height };
			PRINT("[DivaMovie] FFmpeg player: created %dx%d texture (id 0x%08x, GL %u)\n", width, height, id, object->Name());
			return object;
		}

		GLuint Name() const { return *(GLuint*)((BYTE*)gameTexture + GAME_TEXTURE_NAME_OFFSET); }

		static HRESULT QI(TextureObject* self, REFIID riid, void** out)
		{
			if (!out)
				return E_POINTER;
			if (IsEqualGUID(riid, __uuidof(IUnknown)))
			{
				*out = self;
				AddRefImpl(self);
				return S_OK;
			}
			*out = nullptr;
			return E_NOINTERFACE;
		}
		static ULONG AddRefImpl(TextureObject* self) { return InterlockedIncrement(&self->references); }
		static ULONG ReleaseImpl(TextureObject* self)
		{
			LONG n = InterlockedDecrement(&self->references);
			if (n == 0)
			{
				ReleaseGameTexture(self->gameTexture);
				delete self;
			}
			return n;
		}
		static void* GetGameTextureImpl(TextureObject* self) { return self->gameTexture; }
		static void* DestroyImpl(TextureObject* self, int) { return self; }

		static const TextureVtbl vtable;
	};
	const TextureVtbl TextureObject::vtable = { QI, AddRefImpl, ReleaseImpl, GetGameTextureImpl, DestroyImpl };

	// ---- The player ----

	struct Player;
	typedef HRESULT (*PFN_Stub)(Player*, void*, void*, void*);
	struct PlayerVtbl
	{
		HRESULT (*QueryInterface)(Player*, REFIID, void**);   // 0
		ULONG (*AddRef)(Player*);                              // 1
		ULONG (*Release)(Player*);                             // 2
		HRESULT (*Close)(Player*);                             // 3
		HRESULT (*Open)(Player*, const char*);                 // 4
		HRESULT (*Shutdown)(Player*);                          // 5
		HRESULT (*SetClock)(Player*, IUnknown*);               // 6
		LONG (*GetState)(Player*);                             // 7
		HRESULT (*Play)(Player*);                              // 8
		HRESULT (*Pause)(Player*);                             // 9
		double (*GetDuration)(Player*);                        // 10
		double (*GetPosition)(Player*);                        // 11
		PFN_Stub Slot12;                                       // 12 (unused by the game)
		PFN_Stub Slot13;                                       // 13 (unused by the game: seek)
		HRESULT (*SetVolume)(Player*, const float*);           // 14
		HRESULT (*GetVideoInfo)(Player*, VideoInfo*);          // 15
		PFN_Stub Slot16;                                       // 16 (unused by the game)
		PFN_Stub Slot17;                                       // 17 (unused by the game)
		HRESULT (*GetTexture)(Player*, TextureObject**);       // 18
		void* (*Destroy)(Player*, int);                        // 19
	};

	struct Player
	{
		const PlayerVtbl* vtbl;
		volatile LONG references = 1;
		volatile LONG state = STATE_IDLE;

		SRWLOCK lock = SRWLOCK_INIT;
		MovieDecoder* decoder = nullptr;
		WaveOutSink* audio = nullptr;
		IUnknown* clock = nullptr;
		TextureObject* texture = nullptr;
		HANDLE openThread = NULL;
		std::string path;
		volatile bool closing = false;

		// Internal clock (used when the game gives no external clock)
		double clockBase = 0.0;      // seconds accumulated while playing
		double playStart = 0.0;      // FFNow() when playback (re)started
		bool clockRunning = false;
		float volume = 1.0f;
		bool loggedFirstUpload = false;

		static const PlayerVtbl vtable;

		Player() : vtbl(&vtable) {}

		double Now()
		{
			if (clock)
			{
				// The game's clock interface: slot 6 returns 100 ns ticks (see ExternalClock at 0x1404245B0)
				typedef INT64 (*PFN_GetTime)(IUnknown*);
				INT64 ticks = ((PFN_GetTime)(*(void***)clock)[6])(clock);
				return ticks * 1e-7;
			}
			return clockBase + (clockRunning ? FFNow() - playStart : 0.0);
		}

		double Duration()
		{
			if (!decoder)
				return 0.0;
			return decoder->duration > 0.0 ? decoder->duration : decoder->EndTime();
		}

		void OpenWorker()
		{
			std::string utf8 = ToUtf8(path);
			WaveOutSink* sink = nullptr;
			if (movieAudio)
			{
				sink = new WaveOutSink();
				if (!sink->Open())
				{
					PRINT("[DivaMovie] FFmpeg player: could not open audio output\n");
					delete sink;
					sink = nullptr;
				}
			}

			MovieDecoder* d = new MovieDecoder(ff, sink);
			bool ok = d->Open(utf8.c_str());
			if (ok)
			{
				PRINT("[DivaMovie] FFmpeg player: opened %s (%dx%d, %.3f fps, %.3f s%s)\n", path.c_str(), d->width, d->height,
					d->fps, d->duration, d->hasAudio ? ", with audio" : "");
			}
			else
			{
				PRINT("[DivaMovie] FFmpeg player: failed to open %s: %s\n", path.c_str(), d->error.c_str());
			}

			AcquireSRWLockExclusive(&lock);
			if (ok && !closing)
			{
				decoder = d;
				audio = d->hasAudio ? sink : nullptr;
				if (!audio)
					delete sink;
				decoder->Start();
				InterlockedExchange(&state, STATE_OPENED);
			}
			else
			{
				delete d;
				delete sink;
				InterlockedExchange(&state, STATE_ERROR);
			}
			ReleaseSRWLockExclusive(&lock);
		}

		static std::string ToUtf8(const std::string& ansi)
		{
			int wide = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, NULL, 0);
			if (wide <= 0)
				return ansi;
			std::wstring w(wide, L'\0');
			MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, &w[0], wide);
			int bytes = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
			if (bytes <= 0)
				return ansi;
			std::string out(bytes, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], bytes, NULL, NULL);
			out.resize(bytes - 1);
			return out;
		}

		void Teardown()
		{
			closing = true;
			if (openThread)
			{
				WaitForSingleObject(openThread, INFINITE);
				CloseHandle(openThread);
				openThread = NULL;
			}
			AcquireSRWLockExclusive(&lock);
			MovieDecoder* d = decoder;
			WaveOutSink* a = audio;
			decoder = nullptr;
			audio = nullptr;
			ReleaseSRWLockExclusive(&lock);
			if (a)
				a->Abort(); // unblock the decoding thread if it waits on audio
			delete d;       // stops the decoding thread
			delete a;
		}

		// ---- Interface ----

		static HRESULT QI(Player* self, REFIID riid, void** out)
		{
			if (!out)
				return E_POINTER;
			if (IsEqualGUID(riid, __uuidof(IUnknown)))
			{
				*out = self;
				AddRefImpl(self);
				return S_OK;
			}
			*out = nullptr;
			return E_NOINTERFACE;
		}

		static ULONG AddRefImpl(Player* self) { return InterlockedIncrement(&self->references); }

		static ULONG ReleaseImpl(Player* self)
		{
			LONG n = InterlockedDecrement(&self->references);
			if (n == 0)
			{
				self->Teardown();
				if (self->texture)
					self->texture->vtbl->Release(self->texture);
				if (self->clock)
					self->clock->Release();
				PRINT("[DivaMovie] FFmpeg player: released %s\n", self->path.c_str());
				delete self;
			}
			return n;
		}

		static HRESULT CloseImpl(Player* self)
		{
			self->Teardown();
			InterlockedExchange(&self->state, STATE_IDLE);
			return S_OK;
		}

		static HRESULT OpenImpl(Player* self, const char* path)
		{
			if (!path)
				return E_POINTER;
			if (self->state != STATE_IDLE || self->openThread)
				return MF_E_INVALIDREQUEST;
			self->path = path;
			self->closing = false;
			InterlockedExchange(&self->state, STATE_OPENING);
			self->openThread = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { ((Player*)p)->OpenWorker(); return 0; }, self, 0, NULL);
			if (!self->openThread)
			{
				InterlockedExchange(&self->state, STATE_ERROR);
				return HRESULT_FROM_WIN32(GetLastError());
			}
			return S_OK;
		}

		static HRESULT SetClockImpl(Player* self, IUnknown* clock)
		{
			if (clock)
				clock->AddRef();
			if (self->clock)
				self->clock->Release();
			self->clock = clock;
			return S_OK;
		}

		static LONG GetStateImpl(Player* self)
		{
			LONG s = self->state;
			if (s == STATE_PLAYING)
			{
				AcquireSRWLockShared(&self->lock);
				bool finished = self->decoder && self->decoder->Finished(self->Now());
				ReleaseSRWLockShared(&self->lock);
				if (finished)
				{
					InterlockedCompareExchange(&self->state, STATE_STOPPED, STATE_PLAYING);
					PRINT("[DivaMovie] FFmpeg player: finished %s\n", self->path.c_str());
					s = STATE_STOPPED;
				}
			}
			return s;
		}

		static HRESULT PlayImpl(Player* self)
		{
			LONG s = self->state;
			if (s != STATE_OPENED && s != STATE_PAUSED && s != STATE_STOPPED)
				return s == STATE_PLAYING ? S_OK : MF_E_INVALIDREQUEST;
			if (!self->clockRunning)
			{
				self->playStart = FFNow();
				self->clockRunning = true;
			}
			// Movie audio only when the video is not slaved to the song
			if (self->audio && !self->clock)
				self->audio->Play();
			InterlockedExchange(&self->state, STATE_PLAYING);
			return S_OK;
		}

		static HRESULT PauseImpl(Player* self)
		{
			if (self->state != STATE_PLAYING)
				return S_OK;
			if (self->clockRunning)
			{
				self->clockBase += FFNow() - self->playStart;
				self->clockRunning = false;
			}
			if (self->audio)
				self->audio->Pause();
			InterlockedExchange(&self->state, STATE_PAUSED);
			return S_OK;
		}

		static double GetDurationImpl(Player* self)
		{
			AcquireSRWLockShared(&self->lock);
			double d = self->Duration();
			ReleaseSRWLockShared(&self->lock);
			return d;
		}

		static double GetPositionImpl(Player* self)
		{
			double t = self->Now();
			if (self->state == STATE_STOPPED)
			{
				// The game only treats the movie as over once position >= duration
				double d = GetDurationImpl(self);
				if (t < d)
					t = d;
			}
			return t;
		}

		static HRESULT StubOk(Player*, void*, void*, void*) { return S_OK; }
		static HRESULT StubNotImpl(Player*, void*, void*, void*) { return E_NOTIMPL; }

		static HRESULT SetVolumeImpl(Player* self, const float* channels)
		{
			if (!channels)
				return E_POINTER;
			self->volume = channels[0];
			if (self->audio)
				self->audio->SetVolume(channels[0]);
			return S_OK;
		}

		static HRESULT GetVideoInfoImpl(Player* self, VideoInfo* info)
		{
			if (!info)
				return E_POINTER;
			memset(info, 0, sizeof(*info));
			AcquireSRWLockShared(&self->lock);
			if (self->decoder)
			{
				info->width = info->textureWidth = self->decoder->width;
				info->height = info->textureHeight = self->decoder->height;
				info->framesPerSecond = (float)self->decoder->fps;
				info->pixelAspect = (float)self->decoder->pixelAspect;
			}
			ReleaseSRWLockShared(&self->lock);
			return S_OK;
		}

		// Called by the game on its render thread with the GL context current.
		static HRESULT GetTextureImpl(Player* self, TextureObject** out)
		{
			if (!out)
				return E_POINTER;
			*out = nullptr;

			AcquireSRWLockShared(&self->lock);
			MovieDecoder* d = self->decoder;
			if (!d)
			{
				ReleaseSRWLockShared(&self->lock);
				return S_OK;
			}

			bool changed = false;
			const MovieDecoder::Frame* frame = d->FrameFor(self->Now(), &changed);
			if (frame && !self->texture)
			{
				self->texture = TextureObject::Create(d->width, d->height);
				changed = true;
			}
			if (frame && self->texture && changed)
				Upload(self->texture, frame, d->width, d->height);
			if (frame && self->texture && !self->loggedFirstUpload)
			{
				self->loggedFirstUpload = true;
				const BYTE* c = frame->pixels + ((d->height / 2) * d->width + d->width / 2) * 4;
				PRINT("[DivaMovie] FFmpeg player: first frame uploaded (pts %.3f, center B=%u G=%u R=%u)\n",
					frame->pts, c[0], c[1], c[2]);
			}
			ReleaseSRWLockShared(&self->lock);

			if (self->texture)
			{
				self->texture->vtbl->AddRef(self->texture);
				*out = self->texture;
			}
			return S_OK;
		}

		static void Upload(TextureObject* texture, const MovieDecoder::Frame* frame, int width, int height)
		{
			if (!GLDXEmu::pglTexSubImage2D && !GLDXEmu::LoadGL())
				return;
			GLDXEmu::GLUploadState saved;
			saved.Save();
			GLDXEmu::pglBindTexture(GL_TEXTURE_2D, texture->Name());
			GLDXEmu::pglPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			GLDXEmu::pglPixelStorei(GL_UNPACK_ROW_LENGTH, width);
			int w = width < texture->width ? width : texture->width;
			int h = height < texture->height ? height : texture->height;
			GLDXEmu::pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, frame->pixels);
			saved.Restore();
		}

		static void* DestroyImpl(Player* self, int) { return self; }
	};

	const PlayerVtbl Player::vtable = {
		Player::QI, Player::AddRefImpl, Player::ReleaseImpl, Player::CloseImpl, Player::OpenImpl,
		Player::CloseImpl, Player::SetClockImpl, Player::GetStateImpl, Player::PlayImpl, Player::PauseImpl,
		Player::GetDurationImpl, Player::GetPositionImpl, Player::StubOk, Player::StubOk, Player::SetVolumeImpl,
		Player::GetVideoInfoImpl, Player::StubNotImpl, Player::StubNotImpl, Player::GetTextureImpl, Player::DestroyImpl,
	};
	static_assert(sizeof(PlayerVtbl) == 20 * sizeof(void*), "IPlayer has 20 slots");
	static_assert(sizeof(TextureVtbl) == 5 * sizeof(void*), "texture interface has 5 slots");
	static_assert(sizeof(VideoInfo) == 0x18, "VideoInfo is 0x18 bytes");
}
