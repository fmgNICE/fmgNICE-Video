/*
 * Custom FFmpeg Video Decoder with Seeking Support
 * Provides direct control over video decoding and seeking for synchronized timeline
 */

#pragma once

#include <obs-module.h>
#include <util/threading.h>
#include <util/darray.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>

#ifdef __cplusplus
}
#endif

/* Use Windows atomics for MSVC, standard atomics otherwise */
#ifdef _MSC_VER
#include <windows.h>
#define atomic_bool volatile LONG
#define atomic_uint64_t volatile ULONGLONG
#define atomic_int volatile LONG
#define atomic_store(ptr, val) InterlockedExchange((volatile LONG*)(ptr), (val))
#define atomic_load(ptr) InterlockedCompareExchange((volatile LONG*)(ptr), 0, 0)
#define atomic_fetch_add(ptr, val) InterlockedAdd64((volatile LONGLONG*)(ptr), (val))
#define atomic_compare_exchange_strong(ptr, expected, desired) \
    (InterlockedCompareExchange((volatile LONG*)(ptr), (desired), *(expected)) == *(expected))
#else
#include <stdatomic.h>
#endif

/* Forward declaration for zero-copy context */
struct gpu_zero_copy_ctx;

/* Forward declaration for lock-free ring buffer */
struct lockfree_ringbuffer;

struct ffmpeg_decoder {
	/* Source reference */
	obs_source_t *source;
	
	/* FFmpeg contexts */
	AVFormatContext *format_ctx;
	AVCodecContext *video_codec_ctx;
	AVCodecContext *audio_codec_ctx;
	struct SwsContext *sws_ctx;
	struct SwsContext *p010_sws_ctx;  /* P010 to NV12 converter */
	struct SwrContext *swr_ctx;  /* Audio resampler */
	
	/* Hardware decoding */
	AVBufferRef *hw_device_ctx;
	enum AVHWDeviceType hw_device_type;
	enum AVPixelFormat hw_pix_fmt;
	bool hw_decoding_enabled;
	bool hw_decoding_active;
	bool prefer_hw_decode;
	
	/* Stream indices */
	int video_stream_idx;
	int audio_stream_idx;
	
	/* Current state */
	bool initialized;
	atomic_bool playing;        /* Frequently accessed - make atomic */
	bool looping;
	int64_t start_time;
	int64_t duration;
	char *current_path;  /* Currently loaded file path */
	
	/* Frame data */
	AVFrame *frame;
	AVFrame *hw_frame;
	AVFrame *audio_frame;  /* Separate frame for audio processing */
	uint8_t *video_data[4];
	uint32_t video_linesize[4];
	
	/* Aspect ratio correction */
	int adjusted_width;
	int adjusted_height;
	bool needs_aspect_correction;
	
	/* Output format selection */
	bool use_nv12_output; /* true = NV12, false = BGRA */
	
	/* Audio resampling buffers */
	uint8_t *resampled_audio_data[8];  /* Resampled audio data pointers */
	int resampled_audio_linesize;      /* Linesize for resampled audio */
	int max_resampled_samples;         /* Max samples allocated for resampling */
	
	/* Seeking */
	atomic_bool seek_request;   /* Frequently checked - make atomic */
	int64_t seek_target;
	bool seek_flush;
	bool waiting_for_first_frame;  /* Track first frame after seek */
	uint64_t seek_start_time;      /* When seek was initiated */
	uint32_t seek_generation;      /* Incremented on each seek to discard old frames */
	
	/* Threading */
	pthread_t thread;
	pthread_t display_thread;  /* Separate thread for frame display */
	bool display_thread_created;  /* Track if display thread was created */
	pthread_mutex_t mutex;
	atomic_bool thread_running;  /* Frequently checked - make atomic */
	atomic_bool stopping;        /* Frequently checked - make atomic */
	bool reading_frame;  /* Flag to indicate when av_read_frame is active */
	volatile bool interrupt_request;  /* Flag for FFmpeg interrupt callback */
	
	/* Clock System (VLC-style) */
	struct {
		uint64_t system_start;   /* System time when playback started */
		int64_t media_start_pts; /* Media PTS at start of playback */
		int64_t last_pts;        /* Last presented PTS */
		uint64_t last_system;    /* System time of last frame */
		double playback_rate;    /* Playback speed multiplier */
		pthread_mutex_t lock;    /* Clock-specific lock */
	} clock;
	
	/* Frame Buffer - Using lock-free ring buffer for zero contention */
	struct lockfree_ringbuffer *frame_buffer;
	
	/* Legacy buffer structure (kept for compatibility during migration) */
	struct {
		struct buffered_frame {
			AVFrame *frame;      /* Reference to the decoded frame (for zero-copy) */
			int64_t pts;         /* Presentation timestamp */
			uint64_t system_time; /* When to display (system time) */
			bool ready;
			bool is_hw_frame;    /* True if this is a hardware decoded frame */
			bool zero_copy;      /* True if using zero-copy with frame reference */
			/* BGRA converted data for this frame (software decode only) */
			uint8_t *bgra_data[4];
			uint32_t bgra_linesize[4];
			/* NV12 data for hardware frames (used when not zero-copy) */
			uint8_t *nv12_data[2];
			uint32_t nv12_linesize[2];
		} frames[3];             /* Small ring buffer */
		int write_idx;           /* Where to write next decoded frame */
		int read_idx;            /* Next frame to display */
		int count;               /* Number of buffered frames */
		pthread_mutex_t lock;    /* Buffer lock */
		pthread_cond_t cond;     /* Signal new frame available */
	} buffer;
	
	/* Global Timeline Synchronization */
	uint64_t global_timeline_start_ms;  /* Global timeline start in milliseconds */
	
	/* Legacy Timing (kept for compatibility) */
	uint64_t frame_pts;
	uint64_t next_pts;
	uint64_t sys_ts;
	uint64_t start_time_ns;  /* System time when playback started minus initial PTS */
	int64_t pts_offset;      /* Video PTS offset to subtract from frame PTS after seeks */
	int64_t audio_pts_offset;/* Audio PTS offset to subtract from audio PTS after seeks */
	bool waiting_for_first_audio; /* Track first audio frame after seek */
	
	/* Callbacks */
	void (*video_cb)(void *opaque, struct obs_source_frame *frame);
	void (*audio_cb)(void *opaque, struct obs_source_audio *audio);
	void *opaque;
	
	/* Performance metrics */
	struct {
		atomic_uint64_t frames_decoded;
		atomic_uint64_t frames_dropped;
		atomic_uint64_t frames_displayed;
		atomic_uint64_t total_decode_time_ns;
		atomic_uint64_t total_convert_time_ns;
		atomic_uint64_t total_display_time_ns;
		uint64_t last_log_time;
	} perf;
	
	/* Performance monitoring */
	void* perf_monitor; /* struct perf_monitor_t* */
	
	/* Zero-copy GPU pipeline */
	void* gpu_zero_copy_ctx; /* struct gpu_zero_copy_ctx* */
	
	/* State preservation for rapid activate/deactivate cycles */
	enum {
		DECODER_STATE_STOPPED,
		DECODER_STATE_PAUSED_READY,  /* Threads alive but paused */
		DECODER_STATE_PLAYING
	} state;
	int64_t preserved_seek_position;    /* Last seek position if interrupted */
	int64_t preserved_playback_position;/* Current position when paused */
	uint64_t state_preserved_time;      /* When state was preserved */
	bool seek_was_in_progress;          /* True if seek was interrupted */
};

/* Create and destroy decoder */
struct ffmpeg_decoder *ffmpeg_decoder_create(obs_source_t *source);
void ffmpeg_decoder_destroy(struct ffmpeg_decoder *decoder);

/* Initialize decoder with file */
bool ffmpeg_decoder_initialize(struct ffmpeg_decoder *decoder, const char *path);

/* Playback control */
void ffmpeg_decoder_play(struct ffmpeg_decoder *decoder);
void ffmpeg_decoder_play_with_timeline(struct ffmpeg_decoder *decoder, uint64_t timeline_start_ms);
void ffmpeg_decoder_pause(struct ffmpeg_decoder *decoder);
void ffmpeg_decoder_stop(struct ffmpeg_decoder *decoder);
void ffmpeg_decoder_stop_thread(struct ffmpeg_decoder *decoder);
void ffmpeg_decoder_free_scalers(struct ffmpeg_decoder *decoder);
bool ffmpeg_decoder_is_initialized(struct ffmpeg_decoder *decoder);

/* Pause/Resume for rapid scene switches */
void ffmpeg_decoder_pause_ready(struct ffmpeg_decoder *decoder);
bool ffmpeg_decoder_resume(struct ffmpeg_decoder *decoder);
bool ffmpeg_decoder_is_paused_ready(struct ffmpeg_decoder *decoder);

/* Seeking - position in microseconds */
void ffmpeg_decoder_seek(struct ffmpeg_decoder *decoder, int64_t position_us);

/* Get current position in microseconds */
int64_t ffmpeg_decoder_get_position(struct ffmpeg_decoder *decoder);

/* Get total duration in microseconds */
int64_t ffmpeg_decoder_get_duration(struct ffmpeg_decoder *decoder);
const char *ffmpeg_decoder_get_current_path(struct ffmpeg_decoder *decoder);

/* Check if decoder is currently playing */
bool ffmpeg_decoder_is_playing(struct ffmpeg_decoder *decoder);

/* Set callbacks */
void ffmpeg_decoder_set_callbacks(struct ffmpeg_decoder *decoder,
	void (*video_cb)(void *opaque, struct obs_source_frame *frame),
	void (*audio_cb)(void *opaque, struct obs_source_audio *audio),
	void *opaque);

/* Set output format (NV12 or BGRA) */
void ffmpeg_decoder_set_output_format(struct ffmpeg_decoder *decoder, bool use_nv12);