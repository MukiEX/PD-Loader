#pragma once

// Platform-independent movie decoding core: demux + decode with FFmpeg on a worker
// thread, converting video to BGRA into a small queue that the renderer picks frames
// from by presentation time. Audio (optional) is resampled to 16-bit stereo and
// handed to an AudioSink.

#include "FFLoader.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- Minimal threading shim ----

#ifdef _WIN32
struct FFMutex
{
	SRWLOCK lock = SRWLOCK_INIT;
	void Lock() { AcquireSRWLockExclusive(&lock); }
	void Unlock() { ReleaseSRWLockExclusive(&lock); }
};
struct FFCond
{
	CONDITION_VARIABLE cond = CONDITION_VARIABLE_INIT;
	void Wait(FFMutex& m) { SleepConditionVariableSRW(&cond, &m.lock, INFINITE, 0); }
	void WakeAll() { WakeAllConditionVariable(&cond); }
};
struct FFThread
{
	HANDLE handle = NULL;
	template <class T> bool Start(T* object)
	{
		handle = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { ((T*)p)->ThreadMain(); return 0; }, object, 0, NULL);
		return handle != NULL;
	}
	void Join()
	{
		if (handle)
		{
			WaitForSingleObject(handle, INFINITE);
			CloseHandle(handle);
			handle = NULL;
		}
	}
};
inline double FFNow()
{
	static LARGE_INTEGER frequency = {};
	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)frequency.QuadPart;
}
#else
#include <pthread.h>
#include <time.h>
struct FFMutex
{
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	void Lock() { pthread_mutex_lock(&lock); }
	void Unlock() { pthread_mutex_unlock(&lock); }
};
struct FFCond
{
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	void Wait(FFMutex& m) { pthread_cond_wait(&cond, &m.lock); }
	void WakeAll() { pthread_cond_broadcast(&cond); }
};
struct FFThread
{
	pthread_t thread;
	bool started = false;
	template <class T> bool Start(T* object)
	{
		started = pthread_create(&thread, NULL, [](void* p) -> void* { ((T*)p)->ThreadMain(); return nullptr; }, object) == 0;
		return started;
	}
	void Join()
	{
		if (started)
		{
			pthread_join(thread, NULL);
			started = false;
		}
	}
};
inline double FFNow()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

struct FFLockGuard
{
	FFMutex& m;
	FFLockGuard(FFMutex& m) : m(m) { m.Lock(); }
	~FFLockGuard() { m.Unlock(); }
};

// ---- Audio output interface ----

struct AudioSink
{
	static const int SAMPLE_RATE = 48000;
	static const int CHANNELS = 2;
	// Interleaved signed 16-bit stereo at SAMPLE_RATE. May block briefly when full.
	virtual void Write(const int16_t* samples, int frames) = 0;
	virtual ~AudioSink() {}
};

// ---- Decoder ----

class MovieDecoder
{
public:
	struct Frame
	{
		uint8_t* pixels = nullptr; // BGRA, stride = width * 4
		double pts = 0.0;          // seconds from the start of the movie
		double duration = 0.0;
	};

	// Video properties, valid after Open() succeeds
	int width = 0, height = 0;
	double fps = 0.0;
	double pixelAspect = 1.0;
	double duration = 0.0;      // seconds, 0 if unknown
	bool hasAudio = false;
	std::string error;

	MovieDecoder(FFLib& ff, AudioSink* audio) : ff(ff), audioSink(audio) {}

	~MovieDecoder()
	{
		Stop();
		for (auto& f : queue)
			free(f.pixels);
		if (frame) ff.av_frame_free(&frame);
		if (packet) ff.av_packet_free(&packet);
		if (sws) ff.sws_freeContext(sws);
		if (swr) ff.swr_free(&swr);
		free(audioBuffer);
		if (videoCodec) ff.avcodec_free_context(&videoCodec);
		if (audioCodec) ff.avcodec_free_context(&audioCodec);
		if (format) ff.avformat_close_input(&format);
	}

	// Opens the file and decodes the first video frame (synchronous).
	bool Open(const char* utf8Path)
	{
		int r;
		if ((r = ff.avformat_open_input(&format, utf8Path, nullptr, nullptr)) < 0)
			return Fail("avformat_open_input", r);
		if ((r = ff.avformat_find_stream_info(format, nullptr)) < 0)
			return Fail("avformat_find_stream_info", r);

		const AVCodec* decoder = nullptr;
		videoIndex = ff.av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
		if (videoIndex < 0)
			return Fail("no decodable video stream", videoIndex);
		if (!OpenCodec(decoder, videoIndex, &videoCodec))
			return false;

		if (audioSink)
		{
			const AVCodec* audioDecoder = nullptr;
			audioIndex = ff.av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoIndex, &audioDecoder, 0);
			if (audioIndex >= 0 && OpenCodec(audioDecoder, audioIndex, &audioCodec))
				hasAudio = true;
			else
				audioIndex = -1;
		}

		AVStream* stream = format->streams[videoIndex];
		timeBase = av_q2d(stream->time_base);
		startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * timeBase : -1.0;
		if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
			fps = av_q2d(stream->avg_frame_rate);
		else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0)
			fps = av_q2d(stream->r_frame_rate);
		if (fps <= 0.0 || fps > 240.0)
			fps = 30.0;

		if (format->duration > 0)
			duration = format->duration / (double)AV_TIME_BASE;
		else if (stream->duration > 0)
			duration = stream->duration * timeBase;

		frame = ff.av_frame_alloc();
		packet = ff.av_packet_alloc();
		if (!frame || !packet)
			return Fail("out of memory", 0);

		// Preroll: decode until the first video frame is queued
		capacity = hasAudio ? 16 : 6;
		while (count == 0 && !eof)
			if (!Step())
				break;
		if (count == 0)
		{
			if (error.empty())
				error = "no video frames decoded";
			return false;
		}
		return true;
	}

	void Start()
	{
		stopping = false;
		thread.Start(this);
	}

	void Stop()
	{
		{
			FFLockGuard guard(mutex);
			stopping = true;
			cond.WakeAll();
		}
		thread.Join();
	}

	// Picks the frame to show at time t (seconds). Returns the frame and whether it
	// differs from the one returned last time. Only call from one consumer thread.
	const Frame* FrameFor(double t, bool* changed)
	{
		FFLockGuard guard(mutex);
		bool popped = false;
		while (count >= 2 && queue[(head + 1) % capacity].pts <= t)
		{
			head = (head + 1) % capacity;
			count--;
			popped = true;
		}
		if (popped)
			cond.WakeAll();
		if (count == 0)
			return nullptr;
		const Frame* f = &queue[head];
		if (changed)
			*changed = (f != lastShown) || popped || !shownOnce;
		lastShown = f;
		shownOnce = true;
		return f;
	}

	// True once the last frame has been shown for its full duration.
	bool Finished(double t)
	{
		FFLockGuard guard(mutex);
		if (!eof)
			return false;
		if (count == 0)
			return true;
		if (count > 1)
			return false;
		const Frame& last = queue[head];
		return t >= last.pts + last.duration;
	}

	double EndTime()
	{
		FFLockGuard guard(mutex);
		return lastPts + lastDuration;
	}

	// Worker thread
	void ThreadMain()
	{
		for (;;)
		{
			{
				FFLockGuard guard(mutex);
				while (!stopping && count >= capacity)
					cond.Wait(mutex);
				if (stopping || eof)
					return;
			}
			if (!Step())
				return;
		}
	}

private:
	FFLib& ff;
	AudioSink* audioSink;

	AVFormatContext* format = nullptr;
	AVCodecContext* videoCodec = nullptr;
	AVCodecContext* audioCodec = nullptr;
	AVFrame* frame = nullptr;
	AVPacket* packet = nullptr;
	SwsContext* sws = nullptr;
	SwrContext* swr = nullptr;
	int16_t* audioBuffer = nullptr;
	int audioBufferFrames = 0;

	int videoIndex = -1, audioIndex = -1;
	double timeBase = 0.0, startTime = -1.0;
	double lastPts = -1.0, lastDuration = 0.0;
	bool demuxDone = false;
	bool packetPending = false;
	bool flushPending = false;
	bool videoFlushed = false;

	FFMutex mutex;
	FFCond cond;
	FFThread thread;
	bool stopping = false;
	bool eof = false;

	static const int MAX_CAPACITY = 16;
	Frame queue[MAX_CAPACITY];
	int capacity = 6, head = 0, count = 0;
	const Frame* lastShown = nullptr;
	bool shownOnce = false;

	bool Fail(const char* what, int code)
	{
		error = what;
		if (code < 0)
			error += ": " + ff.ErrorString(code);
		return false;
	}

	bool OpenCodec(const AVCodec* decoder, int index, AVCodecContext** out)
	{
		int r;
		AVCodecContext* c = ff.avcodec_alloc_context3(decoder);
		if (!c)
			return Fail("avcodec_alloc_context3", 0);
		if ((r = ff.avcodec_parameters_to_context(c, format->streams[index]->codecpar)) < 0)
		{
			ff.avcodec_free_context(&c);
			return Fail("avcodec_parameters_to_context", r);
		}
		ff.av_opt_set(c, "threads", "auto", 0);
		if ((r = ff.avcodec_open2(c, decoder, nullptr)) < 0)
		{
			ff.avcodec_free_context(&c);
			return Fail("avcodec_open2", r);
		}
		*out = c;
		return true;
	}

	// Reads and decodes one packet (or drains at end). Returns false on fatal error.
	bool Step()
	{
		if (!demuxDone && !packetPending)
		{
			if (ff.av_read_frame(format, packet) < 0)
			{
				demuxDone = true;
				flushPending = true;
				if (audioCodec)
					ff.avcodec_send_packet(audioCodec, nullptr);
			}
			else
				packetPending = true;
		}

		// A decoder returns EAGAIN while its output is undrained (our queue was full);
		// keep the packet and retry after the queue has room again.
		if (packetPending)
		{
			if (packet->stream_index == videoIndex)
			{
				if (ff.avcodec_send_packet(videoCodec, packet) != AVERROR(EAGAIN))
				{
					ff.av_packet_unref(packet);
					packetPending = false;
				}
			}
			else
			{
				if (packet->stream_index == audioIndex)
				{
					if (ff.avcodec_send_packet(audioCodec, packet) == AVERROR(EAGAIN))
					{
						ReceiveAudio();
						ff.avcodec_send_packet(audioCodec, packet);
					}
				}
				ff.av_packet_unref(packet);
				packetPending = false;
			}
		}
		if (flushPending && ff.avcodec_send_packet(videoCodec, nullptr) != AVERROR(EAGAIN))
			flushPending = false;

		if (!ReceiveVideo())
			return false;
		if (audioCodec)
			ReceiveAudio();

		if (demuxDone && videoFlushed)
		{
			FFLockGuard guard(mutex);
			eof = true;
			cond.WakeAll();
		}
		return true;
	}

	bool ReceiveVideo()
	{
		for (;;)
		{
			{
				FFLockGuard guard(mutex);
				if (count >= capacity || stopping)
					return true; // leave remaining frames in the decoder for later
			}
			int r = ff.avcodec_receive_frame(videoCodec, frame);
			if (r == AVERROR(EAGAIN))
				return true;
			if (r == AVERROR_EOF)
			{
				videoFlushed = true;
				return true;
			}
			if (r < 0)
			{
				// Corrupt data: skip it and keep going
				continue;
			}
			bool ok = QueueVideoFrame();
			ff.av_frame_unref(frame);
			if (!ok)
				return false;
		}
	}

	bool QueueVideoFrame()
	{
		if (frame->width <= 0 || frame->height <= 0)
			return true;
		if (width == 0)
		{
			width = frame->width;
			height = frame->height;
			if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
				pixelAspect = av_q2d(frame->sample_aspect_ratio);
		}

		// Timestamp in seconds from the start of the movie
		double pts;
		if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		{
			pts = frame->best_effort_timestamp * timeBase;
			if (startTime < 0.0)
				startTime = pts;
			pts -= startTime;
		}
		else
			pts = lastPts < 0.0 ? 0.0 : lastPts + lastDuration;
		if (pts < 0.0)
			pts = 0.0;
		double frameDuration = 1.0 / fps;

		sws = ff.sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
			width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws)
			return Fail("sws_getCachedContext", 0);

		// Colour matrix: honour the stream's tags, defaulting to BT.709 for HD
		int colorspace = frame->colorspace;
		if (colorspace == AVCOL_SPC_UNSPECIFIED)
			colorspace = height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
		int srcFullRange = frame->color_range == AVCOL_RANGE_JPEG ||
			frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;
		const int* coefficients = ff.sws_getCoefficients(colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
		ff.sws_setColorspaceDetails(sws, coefficients, srcFullRange, coefficients, 1, 0, 1 << 16, 1 << 16);

		Frame* slot;
		{
			FFLockGuard guard(mutex);
			slot = &queue[(head + count) % capacity];
		}
		if (!slot->pixels)
		{
			slot->pixels = (uint8_t*)malloc((size_t)width * height * 4);
			if (!slot->pixels)
				return Fail("out of memory", 0);
		}

		uint8_t* dst[4] = { slot->pixels, nullptr, nullptr, nullptr };
		int dstStride[4] = { width * 4, 0, 0, 0 };
		ff.sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
		slot->pts = pts;
		slot->duration = frameDuration;

		FFLockGuard guard(mutex);
		lastPts = pts;
		lastDuration = frameDuration;
		count++;
		return true;
	}

	void ReceiveAudio()
	{
		for (;;)
		{
			int r = ff.avcodec_receive_frame(audioCodec, frame);
			if (r < 0)
				return;
			WriteAudioFrame();
			ff.av_frame_unref(frame);
		}
	}

	void WriteAudioFrame()
	{
		if (!swr)
		{
			AVChannelLayout outLayout;
			ff.av_channel_layout_default(&outLayout, AudioSink::CHANNELS);
			int r = ff.swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, AudioSink::SAMPLE_RATE,
				&frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
			ff.av_channel_layout_uninit(&outLayout);
			if (r < 0 || ff.swr_init(swr) < 0)
			{
				ff.swr_free(&swr);
				audioIndex = -1; // give up on audio
				return;
			}
		}

		int outFrames = ff.swr_get_out_samples(swr, frame->nb_samples);
		if (outFrames <= 0)
			return;
		if (outFrames > audioBufferFrames)
		{
			free(audioBuffer);
			audioBuffer = (int16_t*)malloc((size_t)outFrames * AudioSink::CHANNELS * sizeof(int16_t));
			audioBufferFrames = audioBuffer ? outFrames : 0;
			if (!audioBuffer)
				return;
		}
		uint8_t* out[1] = { (uint8_t*)audioBuffer };
		int converted = ff.swr_convert(swr, out, outFrames, (const uint8_t**)frame->extended_data, frame->nb_samples);
		if (converted > 0)
			audioSink->Write(audioBuffer, converted);
	}
};
