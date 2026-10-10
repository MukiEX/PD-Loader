#pragma once

// Runtime loading of the FFmpeg shared libraries (no import libraries needed).
// The struct layouts used elsewhere come from the headers this is compiled against,
// so the library major versions must match; Load() checks that.

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <string>

#ifdef _WIN32
typedef HMODULE FFLibHandle;
#else
#include <dlfcn.h>
typedef void* FFLibHandle;
#endif

#define FF_FUNCTIONS(X) \
	X(avutil, avutil_version) \
	X(avutil, av_frame_alloc) \
	X(avutil, av_frame_free) \
	X(avutil, av_frame_unref) \
	X(avutil, av_strerror) \
	X(avutil, av_log_set_level) \
	X(avutil, av_opt_set) \
	X(avutil, av_channel_layout_default) \
	X(avutil, av_channel_layout_uninit) \
	X(avutil, av_samples_get_buffer_size) \
	X(swresample, swresample_version) \
	X(swresample, swr_alloc_set_opts2) \
	X(swresample, swr_init) \
	X(swresample, swr_convert) \
	X(swresample, swr_free) \
	X(swresample, swr_get_out_samples) \
	X(swscale, swscale_version) \
	X(swscale, sws_getCachedContext) \
	X(swscale, sws_scale) \
	X(swscale, sws_freeContext) \
	X(swscale, sws_setColorspaceDetails) \
	X(swscale, sws_getCoefficients) \
	X(avcodec, avcodec_version) \
	X(avcodec, avcodec_alloc_context3) \
	X(avcodec, avcodec_parameters_to_context) \
	X(avcodec, avcodec_open2) \
	X(avcodec, avcodec_send_packet) \
	X(avcodec, avcodec_receive_frame) \
	X(avcodec, avcodec_free_context) \
	X(avcodec, av_packet_alloc) \
	X(avcodec, av_packet_free) \
	X(avcodec, av_packet_unref) \
	X(avcodec, av_packet_move_ref) \
	X(avcodec, avcodec_flush_buffers) \
	X(avformat, avformat_version) \
	X(avformat, avformat_open_input) \
	X(avformat, avformat_find_stream_info) \
	X(avformat, av_find_best_stream) \
	X(avformat, av_read_frame) \
	X(avformat, avformat_close_input)

struct FFLib
{
#define FF_DECLARE(lib, name) decltype(&::name) name = nullptr;
	FF_FUNCTIONS(FF_DECLARE)
#undef FF_DECLARE

	bool loaded = false;
	std::string error;

	// Order matters: each library's dependencies are loaded first.
	enum { AVUTIL, SWRESAMPLE, SWSCALE, AVCODEC, AVFORMAT, COUNT };
	FFLibHandle handles[COUNT] = {};

	static const char* LibName(int i)
	{
		static const char* names[COUNT] = { "avutil", "swresample", "swscale", "avcodec", "avformat" };
		return names[i];
	}

	static int ExpectedMajor(int i)
	{
		static const int majors[COUNT] = { LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR, LIBSWSCALE_VERSION_MAJOR,
			LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR };
		return majors[i];
	}

	FFLibHandle Handle(const char* lib)
	{
		for (int i = 0; i < COUNT; i++)
			if (strcmp(lib, LibName(i)) == 0)
				return handles[i];
		return nullptr;
	}

	static void* Symbol(FFLibHandle h, const char* name)
	{
#ifdef _WIN32
		return (void*)GetProcAddress(h, name);
#else
		return dlsym(h, name);
#endif
	}

	// directory: folder containing the libraries, with a trailing separator.
	bool Load(const std::wstring& directory)
	{
		if (loaded)
			return true;

		for (int i = 0; i < COUNT; i++)
		{
			char file[64];
#ifdef _WIN32
			snprintf(file, sizeof(file), "%s-%d.dll", LibName(i), ExpectedMajor(i));
			std::wstring path = directory;
			for (const char* c = file; *c; c++)
				path += (wchar_t)*c;
			handles[i] = LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
			snprintf(file, sizeof(file), "lib%s.so.%d", LibName(i), ExpectedMajor(i));
			std::string path(directory.begin(), directory.end());
			handles[i] = dlopen((path + file).c_str(), RTLD_NOW | RTLD_LOCAL);
			if (!handles[i])
				handles[i] = dlopen(file, RTLD_NOW | RTLD_LOCAL);
#endif
			if (!handles[i])
			{
				error = std::string("could not load ") + file;
				Unload();
				return false;
			}
		}

#define FF_RESOLVE(lib, name) \
		name = (decltype(name))Symbol(Handle(#lib), #name); \
		if (!name) { error = "missing function " #name; Unload(); return false; }
		FF_FUNCTIONS(FF_RESOLVE)
#undef FF_RESOLVE

		const unsigned versions[COUNT] = { avutil_version(), swresample_version(), swscale_version(),
			avcodec_version(), avformat_version() };
		for (int i = 0; i < COUNT; i++)
		{
			if ((int)(versions[i] >> 16) != ExpectedMajor(i))
			{
				char message[128];
				snprintf(message, sizeof(message), "%s major version %u, expected %d", LibName(i), versions[i] >> 16, ExpectedMajor(i));
				error = message;
				Unload();
				return false;
			}
		}

		av_log_set_level(AV_LOG_ERROR);
		loaded = true;
		return true;
	}

	void Unload()
	{
		for (int i = COUNT - 1; i >= 0; i--)
		{
			if (handles[i])
			{
#ifdef _WIN32
				FreeLibrary(handles[i]);
#else
				dlclose(handles[i]);
#endif
				handles[i] = nullptr;
			}
		}
		loaded = false;
	}

	std::string ErrorString(int code)
	{
		char buffer[128] = "?";
		if (av_strerror)
			av_strerror(code, buffer, sizeof(buffer));
		return buffer;
	}
};
