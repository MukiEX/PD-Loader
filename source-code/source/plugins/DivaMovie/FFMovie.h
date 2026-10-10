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
	void WaitFor(FFMutex& m, int ms) { SleepConditionVariableSRW(&cond, &m.lock, ms, 0); }
	void WakeAll() { WakeAllConditionVariable(&cond); }
};
struct FFThread
{
	HANDLE handle = NULL;
	void (*entry)(void*) = nullptr;
	void* argument = nullptr;
	bool Start(void (*function)(void*), void* object)
	{
		entry = function;
		argument = object;
		handle = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { FFThread* t = (FFThread*)p; t->entry(t->argument); return 0; }, this, 0, NULL);
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
	void WaitFor(FFMutex& m, int ms)
	{
		timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)ms * 1000000;
		ts.tv_sec += ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
		pthread_cond_timedwait(&cond, &m.lock, &ts);
	}
	void WakeAll() { pthread_cond_broadcast(&cond); }
};
struct FFThread
{
	pthread_t thread;
	bool started = false;
	void (*entry)(void*) = nullptr;
	void* argument = nullptr;
	bool Start(void (*function)(void*), void* object)
	{
		entry = function;
		argument = object;
		started = pthread_create(&thread, NULL, [](void* p) -> void* { FFThread* t = (FFThread*)p; t->entry(t->argument); return nullptr; }, this) == 0;
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
	// Interleaved signed 16-bit stereo at SAMPLE_RATE. May block briefly while playing and full.
	virtual void Write(const int16_t* samples, int frames) = 0;
	// Audio accepted but not yet played, in seconds.
	virtual double BufferedSeconds() = 0;
	// No more audio will be written.
	virtual void EndOfStream() {}
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

	// Statistics
	volatile long framesShown = 0, framesSkipped = 0, keyframeJumps = 0;

	MovieDecoder(FFLib& ff, AudioSink* audio) : ff(ff), audioSink(audio) {}

	~MovieDecoder()
	{
		Stop();
		for (auto& f : queue)
			free(f.pixels);
		while (fifoCount > 0)
			PopPacket();
		if (pendingVideo)
			ff.av_packet_free(&pendingVideo);
		if (frame) ff.av_frame_free(&frame);
		if (audioFrame) ff.av_frame_free(&audioFrame);
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
		audioFrame = ff.av_frame_alloc();
		packet = ff.av_packet_alloc();
		if (!frame || !audioFrame || !packet)
			return Fail("out of memory", 0);

		// Preroll: decode until the first video frame is queued
		while (count == 0 && !eof)
		{
			bool progress = DemuxOnce();
			progress |= VideoOnce();
			if (!progress && demuxDone && !eof)
				break; // nothing decodable
		}
		if (count == 0)
		{
			if (error.empty())
				error = "no video frames decoded";
			return false;
		}
		return true;
	}

	// Starts the demux/audio thread and the video decoding thread.
	void Start()
	{
		stopping = false;
		demuxThread.Start([](void* p) { ((MovieDecoder*)p)->DemuxMain(); }, this);
		videoThread.Start([](void* p) { ((MovieDecoder*)p)->VideoMain(); }, this);
	}

	void Stop()
	{
		{
			FFLockGuard guard(mutex);
			stopping = true;
			cond.WakeAll();
		}
		demuxThread.Join();
		videoThread.Join();
	}

	// Picks the frame to show at time t (seconds). Returns the frame and whether it
	// differs from the one returned last time. Only call from one consumer thread.
	const Frame* FrameFor(double t, bool* changed)
	{
		FFLockGuard guard(mutex);
		requestedTime = t;
		consumerStarted = true;
		bool popped = false;
		while (count >= 2 && queue[(head + 1) % CAPACITY].pts <= t)
		{
			head = (head + 1) % CAPACITY;
			count--;
			popped = true;
		}
		if (popped)
			cond.WakeAll();
		if (count == 0)
			return nullptr;
		const Frame* f = &queue[head];
		bool isNew = (f != lastShown) || popped || !shownOnce;
		if (isNew)
			framesShown++;
		if (changed)
			*changed = isNew;
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

	// Reads the file and decodes audio. Audio is cheap, so it always stays ahead
	// regardless of how long video frames take to decode.
	void DemuxMain()
	{
		for (;;)
		{
			{
				FFLockGuard guard(mutex);
				if (stopping || demuxDone)
					return;
			}
			if (!DemuxOnce())
			{
				FFLockGuard guard(mutex);
				if (!stopping)
					cond.WaitFor(mutex, 5); // poll for audio consumption / room in the packet queue
			}
		}
	}

	// Decodes video packets and converts frames into the queue.
	void VideoMain()
	{
		for (;;)
		{
			{
				FFLockGuard guard(mutex);
				if (stopping || eof)
					return;
			}
			if (!VideoOnce())
			{
				FFLockGuard guard(mutex);
				if (!stopping && !eof)
					cond.WaitFor(mutex, 5);
			}
		}
	}

private:
	static const int CAPACITY = 6;              // decoded video frames
	static const int FIFO_MAX = 2048;           // compressed video packets waiting for the decoder
	static const int MIN_VIDEO_PACKETS = 8;     // keep at least this many queued for the video thread
	static constexpr double AUDIO_AHEAD = 1.0;  // seconds of audio to keep buffered

	FFLib& ff;
	AudioSink* audioSink;

	AVFormatContext* format = nullptr;
	AVCodecContext* videoCodec = nullptr;
	AVCodecContext* audioCodec = nullptr;
	AVFrame* frame = nullptr;       // video thread
	AVFrame* audioFrame = nullptr;  // demux thread
	AVPacket* packet = nullptr;     // demux thread
	SwsContext* sws = nullptr;
	SwrContext* swr = nullptr;
	int16_t* audioBuffer = nullptr;
	int audioBufferFrames = 0;

	int videoIndex = -1, audioIndex = -1;
	double timeBase = 0.0, startTime = -1.0;
	double lastPts = -1.0, lastDuration = 0.0;

	// Demuxed video packets not yet given to the video decoder (guarded by mutex)
	AVPacket* fifo[FIFO_MAX] = {};
	int fifoHead = 0, fifoCount = 0;
	bool demuxDone = false;
	bool flushSent = false;
	bool videoFlushed = false;

	FFMutex mutex;
	FFCond cond;
	FFThread demuxThread, videoThread;
	AVPacket* pendingVideo = nullptr; // packet the video decoder refused (video thread only)
	bool reducedQuality = false;      // video thread only
	bool stopping = false;
	bool eof = false;

	Frame queue[CAPACITY];
	int head = 0, count = 0;
	const Frame* lastShown = nullptr;
	bool shownOnce = false;
	bool consumerStarted = false;
	double requestedTime = 0.0;

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

	void PopPacket()
	{
		ff.av_packet_free(&fifo[fifoHead]);
		fifoHead = (fifoHead + 1) % FIFO_MAX;
		fifoCount--;
	}

	bool QueueFull()
	{
		FFLockGuard guard(mutex);
		return count >= CAPACITY;
	}

	// Read further while audio needs data or the video decoder is short of packets.
	bool CanDemux()
	{
		if (demuxDone)
			return false;
		int packets;
		{
			FFLockGuard guard(mutex);
			packets = fifoCount;
		}
		if (packets >= FIFO_MAX)
			return false;
		if (packets < MIN_VIDEO_PACKETS)
			return true;
		return hasAudio && audioSink->BufferedSeconds() < AUDIO_AHEAD;
	}

	// Demux thread: reads one packet. Returns false when it chose not to read.
	bool DemuxOnce()
	{
		if (!CanDemux())
			return false;

		if (ff.av_read_frame(format, packet) < 0)
		{
			if (audioCodec)
			{
				ff.avcodec_send_packet(audioCodec, nullptr);
				ReceiveAudio();
			}
			if (audioSink && hasAudio)
				audioSink->EndOfStream();
			FFLockGuard guard(mutex);
			demuxDone = true;
			cond.WakeAll();
			return true;
		}

		if (packet->stream_index == videoIndex)
		{
			AVPacket* copy = ff.av_packet_alloc();
			if (copy)
			{
				ff.av_packet_move_ref(copy, packet); // keeps the reference-counted data
				FFLockGuard guard(mutex);
				fifo[(fifoHead + fifoCount) % FIFO_MAX] = copy;
				fifoCount++;
				cond.WakeAll();
			}
			else
				ff.av_packet_unref(packet);
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
				ReceiveAudio();
			}
			ff.av_packet_unref(packet);
		}
		return true;
	}

	double PacketSeconds(const AVPacket* p)
	{
		int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
		if (ts == AV_NOPTS_VALUE)
			return -1.0;
		return ts * timeBase - (startTime > 0.0 ? startTime : 0.0);
	}

	// When decoding cannot keep up with playback: first make the decoder cheaper,
	// and if that is not enough, jump to the next keyframe.
	void CatchUp()
	{
		double now, behind;
		{
			FFLockGuard guard(mutex);
			if (!consumerStarted || lastPts < 0.0)
				return;
			now = requestedTime;
			behind = now - lastPts;
		}

		bool skip = behind > 0.15;
		if (skip != reducedQuality)
		{
			reducedQuality = skip;
			ff.av_opt_set(videoCodec, "skip_frame", skip ? "noref" : "default", 0);
			ff.av_opt_set(videoCodec, "skip_loop_filter", skip ? "all" : "default", 0);
		}

		if (behind < 0.5)
			return;

		// Find the newest queued keyframe that is not in the future
		FFLockGuard guard(mutex);
		int target = -1;
		for (int i = 0; i < fifoCount; i++)
		{
			const AVPacket* p = fifo[(fifoHead + i) % FIFO_MAX];
			if (!(p->flags & AV_PKT_FLAG_KEY))
				continue;
			double t = PacketSeconds(p);
			if (t >= 0.0 && t <= now + 0.1)
				target = i;
		}
		if (target <= 0)
			return;
		for (int i = 0; i < target; i++)
		{
			ff.av_packet_free(&fifo[fifoHead]);
			fifoHead = (fifoHead + 1) % FIFO_MAX;
			fifoCount--;
		}
		if (pendingVideo)
			ff.av_packet_free(&pendingVideo);
		ff.avcodec_flush_buffers(videoCodec);
		keyframeJumps++;
		cond.WakeAll();
	}

	// Video thread: feeds packets to the decoder and collects frames. Returns false when idle.
	bool VideoOnce()
	{
		bool progress = false;
		if (!flushSent)
			CatchUp();
		for (;;)
		{
			if (!pendingVideo)
			{
				FFLockGuard guard(mutex);
				if (fifoCount == 0)
					break;
				pendingVideo = fifo[fifoHead];
				fifo[fifoHead] = nullptr;
				fifoHead = (fifoHead + 1) % FIFO_MAX;
				fifoCount--;
				cond.WakeAll(); // room for the demuxer
			}
			if (ff.avcodec_send_packet(videoCodec, pendingVideo) == AVERROR(EAGAIN))
				break; // decoder output must be drained first
			ff.av_packet_free(&pendingVideo);
			progress = true;
			if (ReceiveVideo())
				break; // a frame came out: let the queue logic run
		}

		bool finishedInput;
		{
			FFLockGuard guard(mutex);
			finishedInput = demuxDone && fifoCount == 0 && !pendingVideo;
		}
		if (finishedInput && !flushSent)
		{
			if (ff.avcodec_send_packet(videoCodec, nullptr) != AVERROR(EAGAIN))
				flushSent = true;
			progress = true;
		}

		if (ReceiveVideo())
			progress = true;

		if (finishedInput && videoFlushed)
		{
			FFLockGuard guard(mutex);
			eof = true;
			cond.WakeAll();
			progress = true;
		}
		return progress;
	}

	// Returns true if any frame was received.
	bool ReceiveVideo()
	{
		bool received = false;
		while (!videoFlushed)
		{
			if (QueueFull())
				break; // leave remaining frames in the decoder for later
			int r = ff.avcodec_receive_frame(videoCodec, frame);
			if (r == AVERROR(EAGAIN))
				break;
			if (r == AVERROR_EOF)
			{
				videoFlushed = true;
				break;
			}
			if (r < 0)
				continue; // corrupt data: skip it
			received = true;
#ifdef FFMOVIE_TEST_DECODE_DELAY_US
			usleep(FFMOVIE_TEST_DECODE_DELAY_US); // test hook: simulate a slow CPU
#endif
			QueueVideoFrame();
			ff.av_frame_unref(frame);
		}
		return received;
	}

	void QueueVideoFrame()
	{
		if (frame->width <= 0 || frame->height <= 0 || !frame->data[0])
			return;
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

		// If decoding has fallen behind playback, skip converting frames that are
		// already late (the consumer would drop them anyway).
		{
			FFLockGuard guard(mutex);
			// count includes the frame on screen; only skip when another newer frame is already waiting
			bool late = consumerStarted && count >= 2 && pts + 2 * frameDuration < requestedTime;
			lastPts = pts;
			lastDuration = frameDuration;
			if (late)
			{
				framesSkipped++;
				return;
			}
		}

		sws = ff.sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
			width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws)
			return;

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
			slot = &queue[(head + count) % CAPACITY];
		}
		if (!slot->pixels)
		{
			slot->pixels = (uint8_t*)malloc((size_t)width * height * 4);
			if (!slot->pixels)
				return;
		}

		uint8_t* dst[4] = { slot->pixels, nullptr, nullptr, nullptr };
		int dstStride[4] = { width * 4, 0, 0, 0 };
		ff.sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
		slot->pts = pts;
		slot->duration = frameDuration;

		FFLockGuard guard(mutex);
		count++;
	}

	void ReceiveAudio()
	{
		for (;;)
		{
			int r = ff.avcodec_receive_frame(audioCodec, audioFrame);
			if (r < 0)
				return;
			WriteAudioFrame();
			ff.av_frame_unref(audioFrame);
		}
	}

	void WriteAudioFrame()
	{
		if (!swr)
		{
			AVChannelLayout outLayout;
			ff.av_channel_layout_default(&outLayout, AudioSink::CHANNELS);
			int r = ff.swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, AudioSink::SAMPLE_RATE,
				&audioFrame->ch_layout, (AVSampleFormat)audioFrame->format, audioFrame->sample_rate, 0, nullptr);
			ff.av_channel_layout_uninit(&outLayout);
			if (r < 0 || ff.swr_init(swr) < 0)
			{
				ff.swr_free(&swr);
				audioIndex = -1; // give up on audio
				return;
			}
		}

		int outFrames = ff.swr_get_out_samples(swr, audioFrame->nb_samples);
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
		int converted = ff.swr_convert(swr, out, outFrames, (const uint8_t**)audioFrame->extended_data, audioFrame->nb_samples);
		if (converted > 0)
			audioSink->Write(audioBuffer, converted);
	}
};
