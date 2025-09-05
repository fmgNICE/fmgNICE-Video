/*
 * Simplified FFmpeg decoder with proper frame buffering and pacing
 */

#include "ffmpeg-decoder.h"
#include "simd-convert.h"
#include "simd-nv12-copy.h"
#include "gpu-zero-copy.h"
#include "aligned-memory.h"
#include "cpu-affinity.h"
#include "performance-monitor.h"
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <media-io/video-io.h>
/* Include Windows atomics through ffmpeg-decoder.h */

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define blog(level, format, ...) \
	blog(level, "[FFmpeg Decoder] " format, ##__VA_ARGS__)

/* Memory pool for frame buffers - eliminates per-frame allocations */
#define FRAME_POOL_SIZE 10
#define MAX_FRAME_SIZE (3840 * 2160 * 4)  /* 4K BGRA max */

static struct {
	uint8_t* buffers[FRAME_POOL_SIZE];
	atomic_bool used[FRAME_POOL_SIZE];
	size_t buffer_size;
	bool initialized;
	atomic_int allocation_count;
	atomic_int pool_hits;
	atomic_int pool_misses;
} frame_pool = {0};

static void init_frame_pool(size_t frame_size)
{
	if (frame_pool.initialized)
		return;
	
	/* Align frame size to 32 bytes for AVX2 */
	frame_size = align_size(frame_size, SIMD_ALIGNMENT);
	frame_pool.buffer_size = frame_size;
	
	for (int i = 0; i < FRAME_POOL_SIZE; i++) {
		/* Use aligned allocation for SIMD operations */
		frame_pool.buffers[i] = aligned_alloc_simd(frame_size);
		if (!frame_pool.buffers[i]) {
			blog(LOG_ERROR, "Failed to allocate aligned buffer %d", i);
			/* Fallback to regular allocation */
			frame_pool.buffers[i] = bmalloc(frame_size);
		}
		atomic_store(&frame_pool.used[i], false);
	}
	atomic_store(&frame_pool.allocation_count, 0);
	atomic_store(&frame_pool.pool_hits, 0);
	atomic_store(&frame_pool.pool_misses, 0);
	frame_pool.initialized = true;
	blog(LOG_INFO, "Frame pool initialized with %d aligned buffers of %zu bytes", 
		FRAME_POOL_SIZE, frame_size);
}

static uint8_t* get_frame_buffer(size_t size)
{
	if (!frame_pool.initialized || size > frame_pool.buffer_size) {
		if (!frame_pool.initialized && size <= MAX_FRAME_SIZE) {
			init_frame_pool(MAX_FRAME_SIZE);
		} else {
			atomic_fetch_add(&frame_pool.pool_misses, 1);
			/* Allocate aligned buffer for out-of-pool requests */
			return aligned_alloc_simd(align_size(size, SIMD_ALIGNMENT));
		}
	}
	
	/* Try to get a buffer from the pool */
	for (int i = 0; i < FRAME_POOL_SIZE; i++) {
		bool expected = false;
		if (atomic_compare_exchange_strong(&frame_pool.used[i], &expected, true)) {
			atomic_fetch_add(&frame_pool.pool_hits, 1);
			/* Prefetch buffer for writing */
			prefetch_write(frame_pool.buffers[i]);
			return frame_pool.buffers[i];
		}
	}
	
	/* Pool exhausted, allocate new aligned buffer */
	atomic_fetch_add(&frame_pool.pool_misses, 1);
	atomic_fetch_add(&frame_pool.allocation_count, 1);
	return aligned_alloc_simd(align_size(size, SIMD_ALIGNMENT));
}

static void release_frame_buffer(uint8_t* buffer)
{
	if (!buffer)
		return;
	
	/* Check if this buffer belongs to the pool */
	for (int i = 0; i < FRAME_POOL_SIZE; i++) {
		if (frame_pool.buffers[i] == buffer) {
			atomic_store(&frame_pool.used[i], false);
			return;
		}
	}
	
	/* Not from pool, free aligned memory */
	aligned_free(buffer);
}

static void cleanup_frame_pool(void)
{
	if (!frame_pool.initialized)
		return;
	
	int hits = atomic_load(&frame_pool.pool_hits);
	int misses = atomic_load(&frame_pool.pool_misses);
	int allocations = atomic_load(&frame_pool.allocation_count);
	
	if (hits + misses > 0) {
		blog(LOG_INFO, "Frame pool statistics: hits=%d (%.1f%%), misses=%d, extra allocations=%d",
			hits, (float)hits / (hits + misses) * 100.0f, misses, allocations);
	}
	
	for (int i = 0; i < FRAME_POOL_SIZE; i++) {
		if (frame_pool.buffers[i]) {
			aligned_free(frame_pool.buffers[i]);
			frame_pool.buffers[i] = NULL;
		}
	}
	frame_pool.initialized = false;
}

/* Cache line size for alignment */
#define CACHE_LINE_SIZE 64

/* Inline hints for hot path functions */
#ifdef _MSC_VER
#define INLINE __forceinline
#else
#define INLINE inline __attribute__((always_inline))
#endif

/* Forward declarations */
static void *decoder_thread(void *opaque);
static void *display_thread(void *opaque);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
static bool init_hw_decoder(struct ffmpeg_decoder *decoder, const AVCodec *codec);

/* Hot path inline functions */
static INLINE int64_t get_frame_pts(AVFrame* frame) {
	return frame->best_effort_timestamp != AV_NOPTS_VALUE 
		? frame->best_effort_timestamp 
		: frame->pts;
}

static INLINE bool is_buffer_full(struct ffmpeg_decoder* decoder) {
	return decoder->buffer.count >= 3;
}

static INLINE uint64_t get_system_time_ns(void) {
	return os_gettime_ns();
}

static INLINE uint64_t get_system_time_ms(void) {
	return os_gettime_ns() / 1000000;
}

/* Hardware decoding support functions */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	struct ffmpeg_decoder *decoder = ctx->opaque;
	const enum AVPixelFormat *p;
	
	for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == decoder->hw_pix_fmt) {
			blog(LOG_INFO, "Hardware pixel format selected: %s", av_get_pix_fmt_name(*p));
			return *p;
		}
	}
	
	blog(LOG_WARNING, "Failed to get HW surface format, falling back to software decoding");
	return AV_PIX_FMT_NONE;
}

static bool init_hw_decoder(struct ffmpeg_decoder *decoder, const AVCodec *codec)
{
	blog(LOG_INFO, "[FFmpeg Decoder] Checking hardware decoder support for codec: %s", codec->name);
	
	/* Try hardware decoders in order of preference */
	static const enum AVHWDeviceType hw_priority[] = {
		AV_HWDEVICE_TYPE_D3D11VA,  /* Direct3D 11 (Windows) */
		AV_HWDEVICE_TYPE_DXVA2,    /* DirectX Video Acceleration 2 (Windows) */
		AV_HWDEVICE_TYPE_CUDA,     /* NVIDIA CUDA */
		AV_HWDEVICE_TYPE_QSV,      /* Intel Quick Sync Video */
		AV_HWDEVICE_TYPE_NONE
	};
	
	/* First, enumerate all available hardware configs */
	int num_configs = 0;
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (!config)
			break;
		num_configs++;
		/* Log hardware config details - enable for debugging */
		/* blog(LOG_DEBUG, "[FFmpeg Decoder] HW Config %d: %s (pix_fmt=%d, methods=0x%x)",
			i, av_hwdevice_get_type_name(config->device_type),
			config->pix_fmt, config->methods); */
	}
	
	if (num_configs == 0) {
		blog(LOG_INFO, "[FFmpeg Decoder] No hardware decoder configs available for %s", codec->name);
		return false;
	}
	
	blog(LOG_INFO, "[FFmpeg Decoder] Found %d hardware decoder configs, checking compatibility...", num_configs);
	
	/* Now try each priority hardware type */
	for (int j = 0; hw_priority[j] != AV_HWDEVICE_TYPE_NONE; j++) {
		enum AVHWDeviceType hw_type = hw_priority[j];
		const char *hw_name = av_hwdevice_get_type_name(hw_type);
		
		/* Check if this hardware type is supported by the codec */
		for (int i = 0;; i++) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
			if (!config)
				break;
				
			if (config->device_type == hw_type &&
			    (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				decoder->hw_device_type = config->device_type;
				decoder->hw_pix_fmt = config->pix_fmt;
				
				blog(LOG_INFO, "[FFmpeg Decoder] Attempting to initialize %s hardware decoder...", hw_name);
				
				/* Create hardware device context */
				int ret = av_hwdevice_ctx_create(&decoder->hw_device_ctx, 
					decoder->hw_device_type, NULL, NULL, 0);
				
				if (ret < 0) {
					char errbuf[AV_ERROR_MAX_STRING_SIZE];
					av_strerror(ret, errbuf, sizeof(errbuf));
					blog(LOG_WARNING, "[FFmpeg Decoder] Failed to create %s device: %s", 
						hw_name, errbuf);
					decoder->hw_device_ctx = NULL;
					break; /* Try next hardware type */
				}
				
				/* Assign hardware context to codec */
				decoder->video_codec_ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device_ctx);
				if (!decoder->video_codec_ctx->hw_device_ctx) {
					blog(LOG_WARNING, "[FFmpeg Decoder] Failed to reference hardware device context");
					av_buffer_unref(&decoder->hw_device_ctx);
					decoder->hw_device_ctx = NULL;
					break; /* Try next hardware type */
				}
				
				decoder->video_codec_ctx->opaque = decoder;
				decoder->video_codec_ctx->get_format = get_hw_format;
				
				decoder->hw_decoding_enabled = true;
				blog(LOG_INFO, "[FFmpeg Decoder] Successfully initialized %s hardware decoder", hw_name);
				return true;
			}
		}
	}
	
	blog(LOG_INFO, "[FFmpeg Decoder] No compatible hardware decoder found, using software decoding");
	return false;
}

/* FFmpeg interrupt callback to allow cancelling blocking operations */
static int interrupt_callback(void *opaque)
{
	struct ffmpeg_decoder *decoder = opaque;
	/* Return 1 to interrupt FFmpeg operations when we want to stop */
	return decoder->interrupt_request ? 1 : 0;
}

/* Clock system implementation (VLC-style frame pacing) */
static inline uint64_t clock_get_system_time_for_pts(struct ffmpeg_decoder *decoder, int64_t pts)
{
	pthread_mutex_lock(&decoder->clock.lock);
	
	/* Convert PTS to system time */
	int64_t pts_delta = pts - decoder->clock.media_start_pts;
	
	/* Apply playback rate */
	double system_delta_ms = (double)pts_delta / (1000.0 * decoder->clock.playback_rate);
	
	/* Calculate target system time */
	uint64_t target_time = decoder->clock.system_start + (uint64_t)system_delta_ms;
	
	pthread_mutex_unlock(&decoder->clock.lock);
	
	return target_time;
}

static inline void clock_reset(struct ffmpeg_decoder *decoder, int64_t start_pts)
{
	pthread_mutex_lock(&decoder->clock.lock);
	
	decoder->clock.system_start = os_gettime_ns() / 1000000;  /* Current time in ms */
	decoder->clock.media_start_pts = start_pts;
	decoder->clock.last_pts = start_pts;
	decoder->clock.last_system = decoder->clock.system_start;
	
	blog(LOG_INFO, "Clock reset: system_start=%llu ms, media_start=%lld us", 
		(unsigned long long)decoder->clock.system_start,
		(long long)start_pts);
	
	pthread_mutex_unlock(&decoder->clock.lock);
}

static inline void clock_update(struct ffmpeg_decoder *decoder, int64_t pts)
{
	pthread_mutex_lock(&decoder->clock.lock);
	
	decoder->clock.last_pts = pts;
	decoder->clock.last_system = os_gettime_ns() / 1000000;
	
	pthread_mutex_unlock(&decoder->clock.lock);
}

/* Fast P010 to NV12 conversion - converts 10-bit to 8-bit by shifting right by 2 */
static void convert_p010_to_nv12(uint8_t *dst_y, uint8_t *dst_uv, 
                                  const uint8_t *src_y, const uint8_t *src_uv,
                                  int width, int height, 
                                  int src_linesize_y, int src_linesize_uv,
                                  int dst_linesize_y, int dst_linesize_uv)
{
	/* Safety check for NULL pointers */
	if (!dst_y || !dst_uv || !src_y || !src_uv) {
		blog(LOG_ERROR, "[P010->NV12] NULL pointer passed to conversion function");
		return;
	}
	
	/* Debug logging */
	static int conversion_count = 0;
	if (conversion_count < 3) {
		blog(LOG_INFO, "[P010->NV12] Converting frame %d: size=%dx%d, src_linesize=[%d,%d], dst_linesize=[%d,%d]",
			conversion_count, width, height, src_linesize_y, src_linesize_uv, dst_linesize_y, dst_linesize_uv);
		conversion_count++;
	}
	
	/* Validate dimensions */
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		blog(LOG_ERROR, "[P010->NV12] Invalid dimensions: %dx%d", width, height);
		return;
	}
	
	/* P010 format: 10-bit values stored in 16-bit words (little endian)
	 * Each pixel uses 2 bytes in P010, 1 byte in NV12 */
	
	/* Convert Y plane - simple per-row conversion */
	for (int y = 0; y < height; y++) {
		const uint16_t *src_row = (const uint16_t *)(src_y + y * src_linesize_y);
		uint8_t *dst_row = dst_y + y * dst_linesize_y;
		
		/* Convert each pixel from 10-bit to 8-bit */
		for (int x = 0; x < width; x++) {
			/* Read 16-bit value, shift right by 2 to convert 10-bit to 8-bit */
			dst_row[x] = (uint8_t)(src_row[x] >> 2);
		}
	}
	
	/* Convert UV plane (interleaved U and V, half height) */
	int uv_height = height / 2;
	
	for (int y = 0; y < uv_height; y++) {
		const uint16_t *src_row = (const uint16_t *)(src_uv + y * src_linesize_uv);
		uint8_t *dst_row = dst_uv + y * dst_linesize_uv;
		
		/* NV12 has interleaved UV: U0 V0 U1 V1...
		 * P010 also has interleaved UV but in 16-bit: U0 V0 U1 V1...
		 * Width is the same for both (full width, not half) */
		for (int x = 0; x < width; x++) {
			dst_row[x] = (uint8_t)(src_row[x] >> 2);
		}
	}
}

/* Display thread - consumes frames from buffer with VLC-style timing */
static void *display_thread(void *opaque)
{
	struct ffmpeg_decoder *decoder = opaque;
	
	/* Set thread name and CPU affinity for optimal performance */
	set_thread_name("fmgnice-display");
	optimize_display_thread_placement();
	
	blog(LOG_INFO, "Display thread started with optimized CPU affinity");
	
	int frames_displayed = 0;
	
	while (!atomic_load(&decoder->stopping)) {
		/* Check if we're playing */
		pthread_mutex_lock(&decoder->mutex);
		bool playing = atomic_load(&decoder->playing);
		pthread_mutex_unlock(&decoder->mutex);
		
		if (!playing) {
			os_sleep_ms(20); /* Sleep longer when not playing to reduce CPU usage */
			continue;
		}
		
		pthread_mutex_lock(&decoder->buffer.lock);
		
		/* Log buffer status periodically for debugging */
		#ifdef DEBUG
		if (frames_displayed % 100 == 0) {
			blog(LOG_DEBUG, "[FFmpeg Decoder] Display thread: buffer count = %d, stopping = %d", 
				decoder->buffer.count, decoder->stopping);
		}
		#endif
		
		/* Wait for frames in buffer - use condition wait to avoid spinning */
		if (decoder->buffer.count == 0 && !atomic_load(&decoder->stopping)) {
			/* Use condition wait instead of polling to save CPU */
			pthread_cond_wait(&decoder->buffer.cond, &decoder->buffer.lock);
			
			/* After waking up, check if we should stop */
			if (atomic_load(&decoder->stopping)) {
				pthread_mutex_unlock(&decoder->buffer.lock);
				break;
			}
			
			/* If still no frames, continue to top of loop */
			if (decoder->buffer.count == 0) {
				pthread_mutex_unlock(&decoder->buffer.lock);
				continue;
			}
		}
		
		if (atomic_load(&decoder->stopping)) {
			pthread_mutex_unlock(&decoder->buffer.lock);
			break;
		}
		
		/* Get next frame to display */
		int current_read_idx = decoder->buffer.read_idx;  /* Save the index */
		struct buffered_frame *buf_frame = &decoder->buffer.frames[current_read_idx];
		
		if (!buf_frame->ready || decoder->buffer.count == 0) {
			pthread_mutex_unlock(&decoder->buffer.lock);
			os_sleep_ms(1);
			continue;
		}
		
		/* Get frame info */
		uint64_t display_time = buf_frame->system_time;
		int64_t pts = buf_frame->pts;
		
		/* Check if frame is still valid (not from before a loop) */
		if (pts < 0 || display_time == 0) {
			/* Mark as consumed and move to next */
			buf_frame->ready = false;
			decoder->buffer.read_idx = (decoder->buffer.read_idx + 1) % 3;
			decoder->buffer.count--;
			pthread_mutex_unlock(&decoder->buffer.lock);
			/* Skip invalid frame after loop */
			os_sleep_ms(1);
			continue;
		}
		
		/* Calculate time until frame should be displayed */
		/* Note: display_time is in milliseconds from os_gettime_ns()/1000000 */
		uint64_t current_time_ms = os_gettime_ns() / 1000000;  /* Current time in ms */
		int64_t time_until_display_ms = (int64_t)(display_time - current_time_ms);
		/* Convert to nanoseconds for compatibility with rest of code */
		int64_t time_until_display = time_until_display_ms * 1000000;
		
		/* Remove debug logging - was causing performance issues */
		
		/* If frame is way too late (more than 500ms), drop it */
		if (time_until_display < -500000000) {
			/* Drop late frame */
			static int drop_count = 0;
			if (drop_count++ % 100 == 0) {
				blog(LOG_WARNING, "[FFmpeg Decoder] Dropping late frame: PTS=%lld ms, late by %lld ms",
					(long long)(pts / 1000), (long long)(-time_until_display / 1000000));
			}
			if (decoder->perf_monitor) {
				((perf_monitor_t*)decoder->perf_monitor)->frames_dropped++;
			}
			/* Clean up zero-copy frame reference if used */
			if (buf_frame->zero_copy && buf_frame->frame) {
				av_frame_unref(buf_frame->frame);
				av_frame_free(&buf_frame->frame);
				buf_frame->frame = NULL;
			}
			/* Mark frame as consumed and skip to next */
			buf_frame->ready = false;
			buf_frame->zero_copy = false;
			decoder->buffer.read_idx = (decoder->buffer.read_idx + 1) % 3;
			decoder->buffer.count--;
			pthread_mutex_unlock(&decoder->buffer.lock);
			continue;
		}
		
		/* Wait until it's time to display the frame */
		/* For frames within 3ms of their display time, show immediately */
		/* This accounts for Windows timer resolution */
		if (time_until_display > 3000000) {  /* More than 3ms early */
			/* Frame is early - wait but release lock to allow decoder to continue */
			pthread_mutex_unlock(&decoder->buffer.lock);
			
			/* Use adaptive sleeping to reduce CPU usage */
			/* Note: Windows timer resolution means sleeps may be longer than requested */
			if (time_until_display > 15000000) {  /* More than 15ms early */
				os_sleep_ms(10);  /* Sleep 10ms */
			} else if (time_until_display > 8000000) {  /* 8-15ms early */
				os_sleep_ms(4);  /* Sleep 4ms */
			} else {
				/* For fine timing, use a busy-wait with minimal CPU usage */
				/* This prevents Windows timer resolution issues */
				uint64_t spin_until = os_gettime_ns() + (time_until_display - 3000000);
				while (os_gettime_ns() < spin_until && !atomic_load(&decoder->stopping)) {
					/* Use Windows SwitchToThread to yield CPU time */
					#ifdef _WIN32
					SwitchToThread();
					#else
					sched_yield();
					#endif
				}
			}
			continue;  /* Re-check timing on next iteration */
		}
		
		/* Display the frame if callbacks are still valid */
		pthread_mutex_lock(&decoder->mutex);
		if (!atomic_load(&decoder->stopping) && decoder->video_cb && decoder->opaque) {
			void (*cb)(void *, struct obs_source_frame *) = decoder->video_cb;
			void *opaque_cb = decoder->opaque;
			pthread_mutex_unlock(&decoder->mutex);
			
			/* Get the buffered frame data - we already have the buffer lock */
			struct buffered_frame *current_frame = &decoder->buffer.frames[decoder->buffer.read_idx];
			
			/* Create OBS frame */
			struct obs_source_frame obs_frame;
			memset(&obs_frame, 0, sizeof(obs_frame));
			/* Use corrected dimensions if aspect ratio correction is needed */
			if (decoder->needs_aspect_correction) {
				obs_frame.width = decoder->adjusted_width;
				obs_frame.height = decoder->adjusted_height;
			} else {
				obs_frame.width = decoder->video_codec_ctx->width;
				obs_frame.height = decoder->video_codec_ctx->height;
			}
			/* Use current time for display - OBS handles timing internally */
			obs_frame.timestamp = os_gettime_ns();
			
			/* Set format and data based on frame type */
			if (current_frame->zero_copy && current_frame->frame) {
				/* Zero-copy path: Use frame reference directly */
				/* Check if this is a P010 frame (10-bit) */
				if (current_frame->frame->format == AV_PIX_FMT_P010LE) {
					obs_frame.format = VIDEO_FORMAT_P010;
				} else {
					obs_frame.format = VIDEO_FORMAT_NV12;
				}
				obs_frame.data[0] = current_frame->frame->data[0];  /* Y plane */
				obs_frame.data[1] = current_frame->frame->data[1];  /* UV plane */
				obs_frame.linesize[0] = current_frame->frame->linesize[0];
				obs_frame.linesize[1] = current_frame->frame->linesize[1];
				
				/* YUV formats - limited range by default */
				obs_frame.full_range = false;
				/* Set proper color matrix using OBS helper function */
				enum video_range_type range = obs_frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
				video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, range, obs_frame.format,
				                                       obs_frame.color_matrix, 
				                                       obs_frame.color_range_min,
				                                       obs_frame.color_range_max);
			} else if (current_frame->is_hw_frame) {
				/* Hardware frame with memory copy - use NV12 format */
				obs_frame.format = VIDEO_FORMAT_NV12;
				obs_frame.data[0] = current_frame->nv12_data[0];  /* Y plane */
				obs_frame.data[1] = current_frame->nv12_data[1];  /* UV plane */
				obs_frame.linesize[0] = current_frame->nv12_linesize[0];
				obs_frame.linesize[1] = current_frame->nv12_linesize[1];
				
				/* YUV formats - limited range by default */
				obs_frame.full_range = false;
				/* Set proper color matrix using OBS helper function */
				enum video_range_type range = obs_frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
				video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, range, obs_frame.format,
				                                       obs_frame.color_matrix, 
				                                       obs_frame.color_range_min,
				                                       obs_frame.color_range_max);
				
				/* Verify NV12 data is valid */
				if (!obs_frame.data[0] || !obs_frame.data[1]) {
					blog(LOG_ERROR, "[FFmpeg Decoder] NV12 data pointers are NULL! data[0]=%p, data[1]=%p",
						obs_frame.data[0], obs_frame.data[1]);
					pthread_mutex_unlock(&decoder->buffer.lock);
					continue;
				}
			} else {
				/* Software frame - use BGRA format */
				obs_frame.format = VIDEO_FORMAT_BGRA;
				for (int i = 0; i < 4; i++) {
					obs_frame.data[i] = current_frame->bgra_data[i];
					obs_frame.linesize[i] = current_frame->bgra_linesize[i];
				}
				obs_frame.full_range = true;  /* BGRA uses full range */
				/* Set proper color matrix for BGRA format */
				enum video_range_type range = obs_frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
				video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, range, VIDEO_FORMAT_BGRA,
				                                       obs_frame.color_matrix,
				                                       obs_frame.color_range_min,
				                                       obs_frame.color_range_max);
			}
			pthread_mutex_unlock(&decoder->buffer.lock);
			
			/* Log frame info before sending - disabled for performance */
			/* if (frames_displayed == 0) {
				const char *format_name;
				if (obs_frame.format == VIDEO_FORMAT_P010) {
					format_name = "P010";
				} else if (obs_frame.format == VIDEO_FORMAT_NV12) {
					format_name = "NV12";
				} else if (obs_frame.format == VIDEO_FORMAT_BGRA) {
					format_name = "BGRA";
				} else {
					format_name = "Unknown";
				}
				blog(LOG_INFO, "[FFmpeg Decoder] First frame to OBS: %dx%d, format=%s, data[0]=%p, linesize[0]=%d",
					obs_frame.width, obs_frame.height, format_name,
					obs_frame.data[0], obs_frame.linesize[0]);
				
				if ((obs_frame.format == VIDEO_FORMAT_P010 || obs_frame.format == VIDEO_FORMAT_NV12) && obs_frame.data[1]) {
					blog(LOG_INFO, "[FFmpeg Decoder] %s UV plane: data[1]=%p, linesize[1]=%d",
						format_name, obs_frame.data[1], obs_frame.linesize[1]);
				}
			} */
			
			/* Output frame */
		cb(opaque_cb, &obs_frame);
		
		frames_displayed++;
		
		/* Periodic performance reporting */
		if (decoder->perf_monitor && frames_displayed % 300 == 0) {
			const char *source_name = obs_source_get_name(decoder->source);
			perf_monitor_report((perf_monitor_t*)decoder->perf_monitor, source_name);
		}
		
		/* Verbose frame logging - disabled for performance */
		/* if (frames_displayed % 300 == 1) {
			const char *format_str = obs_frame.format == VIDEO_FORMAT_P010 ? "P010" : 
			                        (obs_frame.format == VIDEO_FORMAT_NV12 ? "NV12" : "BGRA");
			blog(LOG_INFO, "[FFmpeg Decoder] Display: frame %d, PTS=%lld ms, format=%s, size=%dx%d", 
				frames_displayed, (long long)(pts / 1000), format_str, 
				obs_frame.width, obs_frame.height);
		} */
			
			/* Update decoder frame pts for legacy code */
			decoder->frame_pts = pts;
			
			/* Update clock */
			clock_update(decoder, pts);
		} else {
			pthread_mutex_unlock(&decoder->mutex);
		}
		
		/* Mark frame as consumed */
		pthread_mutex_lock(&decoder->buffer.lock);
		/* Use the saved index to clean up the correct frame */
		struct buffered_frame *consumed_frame = &decoder->buffer.frames[current_read_idx];
		
		/* Clean up zero-copy frame reference if used */
		if (consumed_frame->zero_copy && consumed_frame->frame) {
			av_frame_unref(consumed_frame->frame);
			av_frame_free(&consumed_frame->frame);
			consumed_frame->frame = NULL;
		}
		consumed_frame->ready = false;
		consumed_frame->zero_copy = false;
		decoder->buffer.read_idx = (decoder->buffer.read_idx + 1) % 3;
		int new_count = --decoder->buffer.count;
		
		/* Verbose buffer logging - disabled for performance */
		/* if (frames_displayed % 100 == 0) {
			blog(LOG_INFO, "[FFmpeg Decoder] Consumed frame, new count=%d", new_count);
		} */
		
		/* Signal decoder thread if buffer was full */
		if (new_count == 2) {
			pthread_cond_signal(&decoder->buffer.cond);
		}
		pthread_mutex_unlock(&decoder->buffer.lock);
	}
	
	blog(LOG_INFO, "Display thread stopped");
	return NULL;
}

struct ffmpeg_decoder *ffmpeg_decoder_create(obs_source_t *source)
{
	struct ffmpeg_decoder *decoder = bzalloc(sizeof(struct ffmpeg_decoder));
	if (!decoder)
		return NULL;
	
	decoder->source = source;
	decoder->state = DECODER_STATE_STOPPED;
	pthread_mutex_init(&decoder->mutex, NULL);
	
	/* Initialize performance monitor */
	decoder->perf_monitor = bzalloc(sizeof(perf_monitor_t));
	if (decoder->perf_monitor) {
		perf_monitor_init((perf_monitor_t*)decoder->perf_monitor);
	}
	
	/* Initialize clock system */
	pthread_mutex_init(&decoder->clock.lock, NULL);
	decoder->clock.playback_rate = 1.0;
	
	/* Initialize frame buffer */
	pthread_mutex_init(&decoder->buffer.lock, NULL);
	pthread_cond_init(&decoder->buffer.cond, NULL);
	decoder->buffer.write_idx = 0;
	decoder->buffer.read_idx = 0;
	decoder->buffer.count = 0;
	
	/* Allocate buffer frames */
	for (int i = 0; i < 3; i++) {
		decoder->buffer.frames[i].frame = av_frame_alloc();
		if (!decoder->buffer.frames[i].frame) {
			blog(LOG_ERROR, "Failed to allocate buffer frame %d", i);
			/* Clean up already allocated frames */
			for (int j = 0; j < i; j++) {
				av_frame_free(&decoder->buffer.frames[j].frame);
			}
			ffmpeg_decoder_destroy(decoder);
			return NULL;
		}
		decoder->buffer.frames[i].ready = false;
		/* BGRA buffers will be allocated when we know the video size */
		for (int j = 0; j < 4; j++) {
			decoder->buffer.frames[i].bgra_data[j] = NULL;
			decoder->buffer.frames[i].bgra_linesize[j] = 0;
		}
	}
	
	/* Allocate working frames */
	decoder->frame = av_frame_alloc();
	decoder->audio_frame = av_frame_alloc();
	if (!decoder->frame || !decoder->audio_frame) {
		blog(LOG_ERROR, "Failed to allocate working frames");
		ffmpeg_decoder_destroy(decoder);
		return NULL;
	}
	
	/* Display thread will be created when playback starts */
	decoder->display_thread_created = false;
	
	return decoder;
}

void ffmpeg_decoder_destroy(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return;
	
	blog(LOG_INFO, "Destroying decoder");
	
	/* Signal threads to stop */
	pthread_mutex_lock(&decoder->mutex);
	atomic_store(&decoder->stopping, true);
	atomic_store(&decoder->playing, false);
	pthread_mutex_unlock(&decoder->mutex);
	
	/* Wake up display thread */
	pthread_mutex_lock(&decoder->buffer.lock);
	pthread_cond_broadcast(&decoder->buffer.cond);
	pthread_mutex_unlock(&decoder->buffer.lock);
	
	/* Wait for display thread */
	if (decoder->display_thread_created) {
		pthread_join(decoder->display_thread, NULL);
		decoder->display_thread_created = false;
		blog(LOG_INFO, "Display thread stopped");
	}
	
	/* Stop decoder thread */
	if (atomic_load(&decoder->thread_running)) {
		/* Wait for thread to exit */
		pthread_join(decoder->thread, NULL);
		atomic_store(&decoder->thread_running, false);
		blog(LOG_INFO, "Decoder thread stopped");
	}
	
	/* Now it's safe to clear callbacks - threads are stopped, no need for mutex */
	decoder->video_cb = NULL;
	decoder->audio_cb = NULL;
	decoder->opaque = NULL;
	
	/* Wait longer for all frames to be processed */
	os_sleep_ms(100);
	
	/* Free resources */
	if (decoder->frame) {
		av_frame_free(&decoder->frame);
		decoder->frame = NULL;
	}
	if (decoder->audio_frame) {
		av_frame_free(&decoder->audio_frame);
		decoder->audio_frame = NULL;
	}
	
	/* Free buffer frames and clear all references */
	for (int i = 0; i < 3; i++) {
		/* Free frame reference (zero-copy or regular) */
		if (decoder->buffer.frames[i].frame) {
			av_frame_unref(decoder->buffer.frames[i].frame);
			av_frame_free(&decoder->buffer.frames[i].frame);
			decoder->buffer.frames[i].frame = NULL;  /* Prevent double-free */
		}
		/* Free BGRA buffers (allocated with av_image_alloc) */
		if (decoder->buffer.frames[i].bgra_data[0]) {
			av_freep(&decoder->buffer.frames[i].bgra_data[0]);
			/* Clear all pointers to prevent double-free */
			for (int j = 0; j < 4; j++) {
				decoder->buffer.frames[i].bgra_data[j] = NULL;
				decoder->buffer.frames[i].bgra_linesize[j] = 0;
			}
		}
		/* Free NV12 buffer for hardware frames (single contiguous allocation) */
		if (decoder->buffer.frames[i].nv12_data[0]) {
			aligned_free(decoder->buffer.frames[i].nv12_data[0]);
			decoder->buffer.frames[i].nv12_data[0] = NULL;
			decoder->buffer.frames[i].nv12_data[1] = NULL; /* UV was part of same allocation */
			decoder->buffer.frames[i].nv12_linesize[0] = 0;
			decoder->buffer.frames[i].nv12_linesize[1] = 0;
		}
		/* Reset frame state */
		decoder->buffer.frames[i].ready = false;
		decoder->buffer.frames[i].is_hw_frame = false;
	}
	
	if (decoder->sws_ctx)
		sws_freeContext(decoder->sws_ctx);
	if (decoder->p010_sws_ctx)
		sws_freeContext(decoder->p010_sws_ctx);
	if (decoder->swr_ctx)
		swr_free(&decoder->swr_ctx);
	
	/* Free resampled audio buffers */
	if (decoder->resampled_audio_data[0]) {
		av_freep(&decoder->resampled_audio_data[0]);
	}
	
	/* Free video buffer allocated in ffmpeg_decoder_initialize */
	if (decoder->video_data[0]) {
		bfree(decoder->video_data[0]);
		memset(decoder->video_data, 0, sizeof(decoder->video_data));
	}
	
	/* CRITICAL: Free hardware device context IMMEDIATELY to release GPU resources */
	if (decoder->hw_device_ctx) {
		blog(LOG_WARNING, "[CRITICAL] Releasing hardware device context %p to free GPU resources", 
			decoder->hw_device_ctx);
		av_buffer_unref(&decoder->hw_device_ctx);
		decoder->hw_device_ctx = NULL;
		/* Force GPU to flush any pending operations */
		os_sleep_ms(50);
	}
	
	/* Free hardware frame if allocated */
	if (decoder->hw_frame) {
		av_frame_free(&decoder->hw_frame);
		decoder->hw_frame = NULL;
	}
	
	/* Free codec contexts - this also releases hardware references */
	if (decoder->video_codec_ctx) {
		/* Ensure hardware context is cleared from codec first */
		if (decoder->video_codec_ctx->hw_device_ctx) {
			blog(LOG_WARNING, "[CRITICAL] Releasing codec hardware context");
			av_buffer_unref(&decoder->video_codec_ctx->hw_device_ctx);
			decoder->video_codec_ctx->hw_device_ctx = NULL;
		}
		avcodec_free_context(&decoder->video_codec_ctx);
		decoder->video_codec_ctx = NULL;
	}
	
	if (decoder->audio_codec_ctx) {
		avcodec_free_context(&decoder->audio_codec_ctx);
		decoder->audio_codec_ctx = NULL;
	}
	
	if (decoder->format_ctx) {
		avformat_close_input(&decoder->format_ctx);
		decoder->format_ctx = NULL;
	}
	
	bfree(decoder->current_path);
	
	/* Free performance monitor */
	if (decoder->perf_monitor) {
		bfree(decoder->perf_monitor);
	}
	
	/* Destroy synchronization primitives */
	pthread_mutex_destroy(&decoder->mutex);
	pthread_mutex_destroy(&decoder->clock.lock);
	pthread_mutex_destroy(&decoder->buffer.lock);
	pthread_cond_destroy(&decoder->buffer.cond);
	
	bfree(decoder);
}

bool ffmpeg_decoder_initialize(struct ffmpeg_decoder *decoder, const char *path)
{
	if (!decoder || !path)
		return false;
	
	/* Handle long file paths on Windows */
	#ifdef _WIN32
	size_t path_len = strlen(path);
	if (path_len > 260) {
		/* Use extended-length path prefix for long paths */
		char *long_path = bmalloc(path_len + 5);  /* \\?\ prefix + path + null */
		if (long_path) {
			sprintf(long_path, "\\\\?\\%s", path);
			/* Convert forward slashes to backslashes for Windows */
			for (char *p = long_path; *p; p++) {
				if (*p == '/') *p = '\\';
			}
			blog(LOG_INFO, "Using extended-length path for long filename");
			/* Use long_path for file operations */
			path = long_path;
			/* Note: We'll free this after opening the file */
		} else {
			blog(LOG_ERROR, "Failed to allocate memory for long path");
			return false;
		}
	}
	#endif
	
	/* Check if file exists and is accessible */
	if (os_file_exists(path) == false) {
		blog(LOG_ERROR, "File does not exist or is not accessible: %s", path);
		return false;
	}
	
	blog(LOG_INFO, "Initializing decoder with file: %s", path);
	
	/* Stop any existing playback */
	ffmpeg_decoder_stop_thread(decoder);
	
	/* Clear old buffer frames before reinitializing */
	for (int i = 0; i < 3; i++) {
		/* Free BGRA buffers (return to pool) */
		if (decoder->buffer.frames[i].bgra_data[0]) {
			release_frame_buffer(decoder->buffer.frames[i].bgra_data[0]);
			for (int j = 0; j < 4; j++) {
				decoder->buffer.frames[i].bgra_data[j] = NULL;
				decoder->buffer.frames[i].bgra_linesize[j] = 0;
			}
		}
		/* Free NV12 buffers if allocated (single contiguous allocation) */
		if (decoder->buffer.frames[i].nv12_data[0]) {
			aligned_free(decoder->buffer.frames[i].nv12_data[0]);
			decoder->buffer.frames[i].nv12_data[0] = NULL;
			decoder->buffer.frames[i].nv12_data[1] = NULL;
			decoder->buffer.frames[i].nv12_linesize[0] = 0;
			decoder->buffer.frames[i].nv12_linesize[1] = 0;
		}
		decoder->buffer.frames[i].ready = false;
		decoder->buffer.frames[i].is_hw_frame = false;
	}
	
	/* Clear old state */
	if (decoder->hw_device_ctx) {
		av_buffer_unref(&decoder->hw_device_ctx);
		decoder->hw_device_ctx = NULL;
	}
	if (decoder->hw_frame) {
		av_frame_free(&decoder->hw_frame);
		decoder->hw_frame = NULL;
	}
	if (decoder->format_ctx)
		avformat_close_input(&decoder->format_ctx);
	if (decoder->video_codec_ctx)
		avcodec_free_context(&decoder->video_codec_ctx);
	if (decoder->audio_codec_ctx)
		avcodec_free_context(&decoder->audio_codec_ctx);
	
	/* Allocate format context and set up interrupt callback */
	decoder->format_ctx = avformat_alloc_context();
	if (!decoder->format_ctx) {
		blog(LOG_ERROR, "Failed to allocate format context");
		return false;
	}
	
	/* Set up interrupt callback to allow cancelling blocking operations */
	decoder->interrupt_request = false;
	decoder->format_ctx->interrupt_callback.callback = interrupt_callback;
	decoder->format_ctx->interrupt_callback.opaque = decoder;
	
	/* Open input file with timeout for network files */
	AVDictionary *opts = NULL;
	av_dict_set(&opts, "timeout", "5000000", 0);  /* 5 second timeout */
	
	int ret = avformat_open_input(&decoder->format_ctx, path, NULL, &opts);
	av_dict_free(&opts);
	
	/* Free long_path if we allocated it */
	#ifdef _WIN32
	if (path_len > 260 && strncmp(path, "\\\\?\\", 4) == 0) {
		bfree((char *)path);
		path = NULL;  /* Avoid using freed pointer */
	}
	#endif
	
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		blog(LOG_ERROR, "Failed to open file: %s - Error: %s", path, errbuf);
		return false;
	}
	
	/* Find stream info */
	if (avformat_find_stream_info(decoder->format_ctx, NULL) < 0) {
		blog(LOG_ERROR, "Failed to find stream info");
		avformat_close_input(&decoder->format_ctx);
		return false;
	}
	
	/* Find video and audio streams */
	decoder->video_stream_idx = -1;
	decoder->audio_stream_idx = -1;
	
	for (unsigned i = 0; i < decoder->format_ctx->nb_streams; i++) {
		AVStream *stream = decoder->format_ctx->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && decoder->video_stream_idx < 0) {
			decoder->video_stream_idx = i;
		} else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && decoder->audio_stream_idx < 0) {
			decoder->audio_stream_idx = i;
		}
	}
	
	if (decoder->video_stream_idx < 0) {
		blog(LOG_ERROR, "No video stream found");
		avformat_close_input(&decoder->format_ctx);
		return false;
	}
	
	/* Initialize video decoder */
	AVStream *video_stream = decoder->format_ctx->streams[decoder->video_stream_idx];
	const AVCodec *video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
	if (!video_codec) {
		blog(LOG_ERROR, "Video codec not found");
		avformat_close_input(&decoder->format_ctx);
		return false;
	}
	
	/* Calculate FPS for performance monitoring */
	double fps = 30.0;  /* Default to 30fps */
	if (video_stream->avg_frame_rate.num && video_stream->avg_frame_rate.den) {
		fps = av_q2d(video_stream->avg_frame_rate);
	} else if (video_stream->r_frame_rate.num && video_stream->r_frame_rate.den) {
		fps = av_q2d(video_stream->r_frame_rate);
	}
	blog(LOG_INFO, "Video FPS: %.2f (avg_frame_rate: %d/%d)", fps, 
		video_stream->avg_frame_rate.num, video_stream->avg_frame_rate.den);
	
	decoder->video_codec_ctx = avcodec_alloc_context3(video_codec);
	avcodec_parameters_to_context(decoder->video_codec_ctx, video_stream->codecpar);
	
	/* Get sample aspect ratio (SAR) for proper display */
	AVRational sar = decoder->video_codec_ctx->sample_aspect_ratio;
	if (sar.num == 0 || sar.den == 0) {
		/* Try stream SAR if codec SAR is not set */
		sar = video_stream->sample_aspect_ratio;
	}
	if (sar.num == 0 || sar.den == 0) {
		/* Default to square pixels if no SAR specified */
		sar.num = 1;
		sar.den = 1;
	}
	
	/* Validate video resolution */
	int width = decoder->video_codec_ctx->width;
	int height = decoder->video_codec_ctx->height;
	
	if (width <= 0 || height <= 0) {
		blog(LOG_ERROR, "Invalid video resolution: %dx%d", width, height);
		avcodec_free_context(&decoder->video_codec_ctx);
		avformat_close_input(&decoder->format_ctx);
		return false;
	}
	
	/* Adaptive performance settings based on resolution */
	if (width > 3840 || height > 2160) {  /* 4K or higher */
		if (width > 7680 || height > 4320) {  /* 8K */
			blog(LOG_WARNING, "8K video detected (%dx%d), adjusting performance settings", width, height);
			/* For 8K, prioritize hardware decoding */
			decoder->prefer_hw_decode = true;
		} else {
			blog(LOG_INFO, "4K video detected (%dx%d), optimizing for high resolution", width, height);
		}
		/* For 4K+, ensure hardware acceleration is attempted first */
		decoder->prefer_hw_decode = true;
	}
	
	/* Calculate Display Aspect Ratio (DAR) from Sample Aspect Ratio (SAR) */
	float pixel_aspect_ratio = (float)sar.num / (float)sar.den;
	float display_aspect_ratio = ((float)width * pixel_aspect_ratio) / (float)height;
	
	blog(LOG_INFO, "Video dimensions: %dx%d, SAR: %d:%d (%.3f), DAR: %.3f",
		width, height, sar.num, sar.den, pixel_aspect_ratio, display_aspect_ratio);
	
	/* Calculate corrected display dimensions */
	decoder->adjusted_width = width;
	decoder->adjusted_height = height;
	decoder->needs_aspect_correction = false;
	
	/* Apply aspect ratio correction for non-square pixels */
	if (pixel_aspect_ratio > 1.01f || pixel_aspect_ratio < 0.99f) {
		/* Non-square pixels detected */
		decoder->needs_aspect_correction = true;
		
		if (display_aspect_ratio > 16.0f/9.0f) {
			/* Wider than 16:9 - letterbox (black bars top/bottom) */
			int corrected_height = (int)(width / display_aspect_ratio);
			if (corrected_height <= height) {
				decoder->adjusted_height = corrected_height;
				blog(LOG_INFO, "Letterboxing: %dx%d -> %dx%d for %.2f:1 aspect ratio",
					width, height, decoder->adjusted_width, decoder->adjusted_height, display_aspect_ratio);
			}
		} else if (display_aspect_ratio < 4.0f/3.0f) {
			/* Taller than 4:3 - pillarbox (black bars left/right) */
			int corrected_width = (int)(height * display_aspect_ratio);
			if (corrected_width <= width) {
				decoder->adjusted_width = corrected_width;
				blog(LOG_INFO, "Pillarboxing: %dx%d -> %dx%d for %.2f:1 aspect ratio",
					width, height, decoder->adjusted_width, decoder->adjusted_height, display_aspect_ratio);
			}
		}
	}
	
	/* Check and handle extreme aspect ratios */
	if (display_aspect_ratio < 0.1f || display_aspect_ratio > 10.0f) {
		blog(LOG_WARNING, "Extreme aspect ratio detected: %.2f", display_aspect_ratio);
		
		/* Clamp to reasonable limits */
		if (display_aspect_ratio < 0.25f) {
			/* Very tall video - limit to 1:4 */
			decoder->adjusted_width = height / 4;
			decoder->needs_aspect_correction = true;
		} else if (display_aspect_ratio > 4.0f) {
			/* Very wide video - limit to 4:1 */
			decoder->adjusted_height = width / 4;
			decoder->needs_aspect_correction = true;
		}
	}
	
	/* Try to initialize hardware decoding */
	decoder->hw_decoding_enabled = false;
	decoder->hw_decoding_active = false;
	
	/* Check if this is HEVC */
	bool is_hevc = (strcmp(video_codec->name, "hevc") == 0 || 
	                strcmp(video_codec->name, "h265") == 0);
	
	if (is_hevc) {
		blog(LOG_INFO, "[HEVC] HEVC/H.265 codec detected - hardware decoding enabled by default");
	}
	
	/* Check if user wants to force software decoding for HEVC (useful for debugging) */
	bool force_hevc_software = false;
	const char *hevc_mode = getenv("FMGNICE_HEVC_MODE");
	if (hevc_mode && strcmp(hevc_mode, "software") == 0) {
		force_hevc_software = true;
		blog(LOG_WARNING, "[HEVC] Forcing software decoding (FMGNICE_HEVC_MODE=software) - hardware disabled by user");
	}
	
	/* HEVC Dual-Path: Try hardware first, fall back to software if needed */
	bool hw_init_success = false;
	if (is_hevc && !force_hevc_software) {
		blog(LOG_INFO, "[HEVC] Attempting hardware decoding for HEVC content...");
		
		/* Try hardware decoding first for HEVC */
		hw_init_success = init_hw_decoder(decoder, video_codec);
		if (hw_init_success) {
			blog(LOG_INFO, "[HEVC] Hardware decoder initialized successfully");
			decoder->hw_decoding_active = true;
			
			/* Try to open with hardware support */
			if (avcodec_open2(decoder->video_codec_ctx, video_codec, NULL) < 0) {
				blog(LOG_WARNING, "[HEVC] Failed to open codec with hardware support, falling back to software");
				
				/* Clean up hardware context */
				if (decoder->video_codec_ctx->hw_device_ctx) {
					av_buffer_unref(&decoder->video_codec_ctx->hw_device_ctx);
					decoder->video_codec_ctx->hw_device_ctx = NULL;
				}
				if (decoder->hw_device_ctx) {
					av_buffer_unref(&decoder->hw_device_ctx);
					decoder->hw_device_ctx = NULL;
				}
				decoder->hw_decoding_enabled = false;
				decoder->hw_decoding_active = false;
				hw_init_success = false;
				
				/* Reset codec context for software decoding */
				avcodec_free_context(&decoder->video_codec_ctx);
				decoder->video_codec_ctx = avcodec_alloc_context3(video_codec);
				AVStream *video_stream = decoder->format_ctx->streams[decoder->video_stream_idx];
				avcodec_parameters_to_context(decoder->video_codec_ctx, video_stream->codecpar);
			}
		}
		
		/* If hardware failed or wasn't available, use software with 8-bit output */
		if (!hw_init_success) {
			blog(LOG_WARNING, "[HEVC] Hardware decoding unavailable or failed - falling back to software decoding with 8-bit output");
			/* Force 8-bit output for software HEVC to simplify handling */
			decoder->video_codec_ctx->sw_pix_fmt = AV_PIX_FMT_YUV420P;
			
			if (avcodec_open2(decoder->video_codec_ctx, video_codec, NULL) < 0) {
				blog(LOG_ERROR, "[HEVC] Failed to open HEVC codec in software mode");
				avcodec_free_context(&decoder->video_codec_ctx);
				avformat_close_input(&decoder->format_ctx);
				return false;
			}
		}
	} else {
		/* Non-HEVC codecs: standard hardware/software path */
		if (init_hw_decoder(decoder, video_codec)) {
			blog(LOG_INFO, "Hardware decoding enabled for %s", video_codec->name);
			decoder->hw_decoding_active = true;
		} else {
			blog(LOG_INFO, "Using software decoding for %s", video_codec->name);
		}
		
		if (avcodec_open2(decoder->video_codec_ctx, video_codec, NULL) < 0) {
			blog(LOG_ERROR, "Failed to open video codec");
			avcodec_free_context(&decoder->video_codec_ctx);
			avformat_close_input(&decoder->format_ctx);
			return false;
		}
	}
	
	blog(LOG_INFO, "Video stream: %dx%d, codec: %s", width, height, video_codec->name);
	
	/* Set FPS in performance monitor for accurate late frame detection */
	if (decoder->perf_monitor) {
		perf_monitor_set_fps((perf_monitor_t*)decoder->perf_monitor, fps);
		blog(LOG_INFO, "Performance monitor configured for %.2f fps (%.1f ms/frame)", 
			fps, 1000.0 / fps);
	}
	
	/* Initialize audio decoder if present */
	if (decoder->audio_stream_idx >= 0) {
		blog(LOG_INFO, "Audio stream found at index %d", decoder->audio_stream_idx);
		AVStream *audio_stream = decoder->format_ctx->streams[decoder->audio_stream_idx];
		const AVCodec *audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
		if (audio_codec) {
			decoder->audio_codec_ctx = avcodec_alloc_context3(audio_codec);
			avcodec_parameters_to_context(decoder->audio_codec_ctx, audio_stream->codecpar);
			if (avcodec_open2(decoder->audio_codec_ctx, audio_codec, NULL) < 0) {
				blog(LOG_WARNING, "Failed to open audio codec");
				avcodec_free_context(&decoder->audio_codec_ctx);
				decoder->audio_stream_idx = -1;
			} else {
				/* Setup audio resampler if needed */
				if (decoder->audio_codec_ctx->sample_fmt != AV_SAMPLE_FMT_FLTP ||
				    decoder->audio_codec_ctx->ch_layout.nb_channels != 2) {
					
					AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
					decoder->swr_ctx = swr_alloc();
					av_opt_set_chlayout(decoder->swr_ctx, "in_chlayout", &decoder->audio_codec_ctx->ch_layout, 0);
					av_opt_set_int(decoder->swr_ctx, "in_sample_rate", decoder->audio_codec_ctx->sample_rate, 0);
					av_opt_set_sample_fmt(decoder->swr_ctx, "in_sample_fmt", decoder->audio_codec_ctx->sample_fmt, 0);
					av_opt_set_chlayout(decoder->swr_ctx, "out_chlayout", &stereo_layout, 0);
					av_opt_set_int(decoder->swr_ctx, "out_sample_rate", decoder->audio_codec_ctx->sample_rate, 0);
					av_opt_set_sample_fmt(decoder->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
					swr_init(decoder->swr_ctx);
					
					/* Allocate resampled audio buffer */
					decoder->max_resampled_samples = 4096;  /* Conservative max samples per frame */
					int ret = av_samples_alloc(decoder->resampled_audio_data, 
						&decoder->resampled_audio_linesize,
						2,  /* stereo output */
						decoder->max_resampled_samples,
						AV_SAMPLE_FMT_FLTP,
						0);
					if (ret < 0) {
						blog(LOG_WARNING, "Failed to allocate resampled audio buffer");
						swr_free(&decoder->swr_ctx);
					} else {
						blog(LOG_INFO, "Audio resampler initialized: %s %dHz %dch -> FLTP %dHz stereo",
							av_get_sample_fmt_name(decoder->audio_codec_ctx->sample_fmt),
							decoder->audio_codec_ctx->sample_rate,
							decoder->audio_codec_ctx->ch_layout.nb_channels,
							decoder->audio_codec_ctx->sample_rate);
					}
				}
				
				blog(LOG_INFO, "Audio codec opened: %s, %d Hz, %d channels",
					audio_codec->name,
					decoder->audio_codec_ctx->sample_rate,
					decoder->audio_codec_ctx->ch_layout.nb_channels);
			}
		}
	} else {
		blog(LOG_INFO, "No audio stream found in file - video only playback");
		/* Clear any stale audio state */
		if (decoder->audio_codec_ctx) {
			avcodec_free_context(&decoder->audio_codec_ctx);
		}
		if (decoder->swr_ctx) {
			swr_free(&decoder->swr_ctx);
		}
	}
	
	/* Create scaler for video format conversion 
	 * Note: When hardware decoding is enabled, the codec's pix_fmt will be the HW format.
	 * We'll create the scaler later when we know the actual software format.
	 */
	if (!decoder->hw_decoding_enabled) {
		enum AVPixelFormat src_pix_fmt = decoder->video_codec_ctx->pix_fmt;
		
		/* Use corrected dimensions for output if aspect ratio correction is needed */
		int output_width = decoder->needs_aspect_correction ? decoder->adjusted_width : decoder->video_codec_ctx->width;
		int output_height = decoder->needs_aspect_correction ? decoder->adjusted_height : decoder->video_codec_ctx->height;
		
		blog(LOG_INFO, "[FFmpeg Decoder] Creating scaler: %dx%d -> %dx%d, pix_fmt=%s -> BGRA",
			decoder->video_codec_ctx->width, decoder->video_codec_ctx->height,
			output_width, output_height,
			av_get_pix_fmt_name(src_pix_fmt));
		
		decoder->sws_ctx = sws_getContext(
			decoder->video_codec_ctx->width, decoder->video_codec_ctx->height,
			src_pix_fmt,
			output_width, output_height,
			AV_PIX_FMT_BGRA,  /* BGRA format - known to work */
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
		
		if (!decoder->sws_ctx) {
			blog(LOG_ERROR, "[FFmpeg Decoder] Failed to create scaler context");
			ffmpeg_decoder_destroy(decoder);
			return false;
		}
	} else {
		blog(LOG_INFO, "[FFmpeg Decoder] Hardware decoding enabled, scaler will be created after first frame");
	}
	
	/* Allocate video buffer for BGRA with corrected dimensions */
	int buffer_width = decoder->needs_aspect_correction ? decoder->adjusted_width : decoder->video_codec_ctx->width;
	int buffer_height = decoder->needs_aspect_correction ? decoder->adjusted_height : decoder->video_codec_ctx->height;
	
	int size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, buffer_width, buffer_height, SIMD_ALIGNMENT);
	
	/* Free any existing video buffer before allocating new one */
	if (decoder->video_data[0]) {
		aligned_free(decoder->video_data[0]);
		memset(decoder->video_data, 0, sizeof(decoder->video_data));
	}
	
	/* Use aligned allocation for optimal SIMD performance */
	uint8_t *video_buffer = aligned_alloc_simd(align_size(size, SIMD_ALIGNMENT));
	if (!video_buffer) {
		blog(LOG_ERROR, "Failed to allocate aligned video buffer, falling back to regular allocation");
		video_buffer = bmalloc(size);
	}
	av_image_fill_arrays(decoder->video_data, decoder->video_linesize,
		video_buffer, AV_PIX_FMT_BGRA, buffer_width, buffer_height, SIMD_ALIGNMENT);
	
	/* Store path and duration */
	bfree(decoder->current_path);
	decoder->current_path = bstrdup(path);
	decoder->duration = decoder->format_ctx->duration;
	
	decoder->initialized = true;
	blog(LOG_INFO, "Initialized: %s", path);
	
	return true;
}

static void *decoder_thread(void *opaque)
{
	struct ffmpeg_decoder *decoder = opaque;
	
	/* Set thread name and CPU affinity for optimal performance */
	set_thread_name("fmgnice-decoder");
	optimize_decoder_thread_placement();
	
	blog(LOG_INFO, "Decoder thread started with optimized CPU affinity");
	
	AVPacket *packet = av_packet_alloc();
	uint64_t last_video_pts = 0;
	uint64_t frames_decoded = 0;
	
	blog(LOG_INFO, "Decoder thread started - format_ctx: %p, video_codec_ctx: %p",
		decoder->format_ctx, decoder->video_codec_ctx);
	
	while (atomic_load(&decoder->thread_running)) {
		pthread_mutex_lock(&decoder->mutex);
		bool playing = atomic_load(&decoder->playing);
		bool stopping = atomic_load(&decoder->stopping);
		pthread_mutex_unlock(&decoder->mutex);
		
		if (stopping)
			break;
		
		if (!playing) {
			os_sleep_ms(20); /* Sleep longer when not playing */
			continue;
		}
		
		/* Continue decoding */
		
		/* Check stopping flag frequently to avoid hanging */
		if (atomic_load(&decoder->stopping))
			break;
		
		/* Check for seek request */
		pthread_mutex_lock(&decoder->mutex);
		if (atomic_load(&decoder->seek_request)) {
			atomic_store(&decoder->seek_request, false);
			int64_t seek_target = decoder->seek_target;
			pthread_mutex_unlock(&decoder->mutex);
			
			/* Clear frame buffer before seeking */
			pthread_mutex_lock(&decoder->buffer.lock);
			for (int i = 0; i < 3; i++) {
				decoder->buffer.frames[i].ready = false;
				if (decoder->buffer.frames[i].frame) {
					av_frame_unref(decoder->buffer.frames[i].frame);
				}
			}
			decoder->buffer.write_idx = 0;
			decoder->buffer.read_idx = 0;
			decoder->buffer.count = 0;
			pthread_mutex_unlock(&decoder->buffer.lock);
			
			/* Seek to target position */
			int64_t seek_pts = av_rescale_q(seek_target, AV_TIME_BASE_Q,
				decoder->format_ctx->streams[decoder->video_stream_idx]->time_base);
			av_seek_frame(decoder->format_ctx, decoder->video_stream_idx, seek_pts, AVSEEK_FLAG_BACKWARD);
			
			/* Flush codec buffers */
			avcodec_flush_buffers(decoder->video_codec_ctx);
			if (decoder->audio_codec_ctx)
				avcodec_flush_buffers(decoder->audio_codec_ctx);
			
			/* Reset for seek - clock will be reset on first frame */
			decoder->waiting_for_first_frame = true;
			decoder->waiting_for_first_audio = true;
			
			blog(LOG_INFO, "Seek requested to %lld us, clock will reset on first frame", 
				(long long)seek_target);
		} else {
			pthread_mutex_unlock(&decoder->mutex);
		}
		
		/* Check stopping before blocking read */
		if (atomic_load(&decoder->stopping))
			break;
		
		/* Read packet - will be interrupted if interrupt_request is set */
		int ret = av_read_frame(decoder->format_ctx, packet);
		if (ret < 0) {
			/* Check if we were interrupted */
			if (ret == AVERROR_EXIT || decoder->interrupt_request) {
				blog(LOG_INFO, "Decoder thread interrupted");
				break;
			}
			/* End of file or error */
			if (decoder->looping && ret == AVERROR_EOF) {
				blog(LOG_INFO, "End of file reached, looping back to start");
				
				/* Clear frame buffer before looping */
				pthread_mutex_lock(&decoder->buffer.lock);
				for (int i = 0; i < 3; i++) {
					decoder->buffer.frames[i].ready = false;
					decoder->buffer.frames[i].pts = -1;  /* Mark as invalid */
					decoder->buffer.frames[i].system_time = 0;
					if (decoder->buffer.frames[i].frame) {
						av_frame_unref(decoder->buffer.frames[i].frame);
					}
				}
				decoder->buffer.write_idx = 0;
				decoder->buffer.read_idx = 0;
				decoder->buffer.count = 0;
				/* Signal display thread that buffer changed */
				pthread_cond_broadcast(&decoder->buffer.cond);
				pthread_mutex_unlock(&decoder->buffer.lock);
				
				/* Longer delay to ensure display thread processes the clear */
				os_sleep_ms(30);
				
				/* Loop back to start */
				av_seek_frame(decoder->format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(decoder->video_codec_ctx);
				if (decoder->audio_codec_ctx)
					avcodec_flush_buffers(decoder->audio_codec_ctx);
				
				/* Reset for loop - clock will be reset on first frame */
				decoder->waiting_for_first_frame = true;
				decoder->waiting_for_first_audio = true;
				
				blog(LOG_INFO, "Looping: seek complete, waiting for first frame");
				continue;
			} else if (ret == AVERROR_EOF) {
				/* End of file, no loop - stop playback and wait */
				pthread_mutex_lock(&decoder->mutex);
				atomic_store(&decoder->playing, false);
				pthread_mutex_unlock(&decoder->mutex);
				/* Sleep to avoid busy loop */
				os_sleep_ms(100);
				continue;
			} else if (ret < 0) {
				/* Error reading - sleep briefly and try again */
				os_sleep_ms(10);
				continue;
			}
		}
		
		/* Decode video packet */
		if (packet->stream_index == decoder->video_stream_idx) {
			ret = avcodec_send_packet(decoder->video_codec_ctx, packet);
			if (ret >= 0) {
				while (avcodec_receive_frame(decoder->video_codec_ctx, decoder->frame) >= 0) {
					/* Start performance tracking for this frame */
					if (decoder->perf_monitor) {
						perf_monitor_frame_start((perf_monitor_t*)decoder->perf_monitor);
					}
					/* Handle hardware frame transfer if needed */
					AVFrame *sw_frame = decoder->frame;
					if (decoder->hw_decoding_active && decoder->frame->format == decoder->hw_pix_fmt) {
						/* Allocate hardware frame if not already done */
						if (!decoder->hw_frame) {
							decoder->hw_frame = av_frame_alloc();
							/* Allocated software frame for HW transfer */
						}
						
						/* Unref previous hw_frame data before new transfer */
						if (decoder->hw_frame) {
							av_frame_unref(decoder->hw_frame);
						}
						
						/* Transfer data from GPU to CPU */
						ret = av_hwframe_transfer_data(decoder->hw_frame, decoder->frame, 0);
						if (ret < 0) {
							char errbuf[AV_ERROR_MAX_STRING_SIZE];
							av_strerror(ret, errbuf, sizeof(errbuf));
							blog(LOG_WARNING, "[FFmpeg Decoder] Hardware frame transfer failed: %s", errbuf);
							
							/* Track hardware failures for recovery */
							static int hw_failure_count = 0;
							hw_failure_count++;
							
							if (hw_failure_count > 5) {
								/* Persistent failures - fall back to software */
								decoder->hw_decoding_active = false;
								blog(LOG_WARNING, "[FFmpeg Decoder] Multiple HW failures, switching to software decoding");
								hw_failure_count = 0;
							} else {
								/* Temporary failure - retry */
								blog(LOG_INFO, "[FFmpeg Decoder] HW transfer failed (attempt %d/5), retrying", hw_failure_count);
							}
							continue;
						}
						
						/* Copy metadata from original frame to transferred frame */
						av_frame_copy_props(decoder->hw_frame, decoder->frame);
						sw_frame = decoder->hw_frame;
						
						/* Create scaler on first hardware frame if needed (only for non-NV12/P010 formats) */
						static int hw_frame_count = 0;
						hw_frame_count++;
						
						/* Only create scaler if we're not going to pass NV12/P010 directly */
						if (!decoder->sws_ctx && 
						    sw_frame->format != AV_PIX_FMT_NV12 && 
						    sw_frame->format != AV_PIX_FMT_P010LE) {
							enum AVPixelFormat sw_pix_fmt = sw_frame->format;
							
							/* Use corrected dimensions for output if aspect ratio correction is needed */
							int output_width = decoder->needs_aspect_correction ? decoder->adjusted_width : decoder->video_codec_ctx->width;
							int output_height = decoder->needs_aspect_correction ? decoder->adjusted_height : decoder->video_codec_ctx->height;
							
							blog(LOG_INFO, "[FFmpeg Decoder] Creating HW scaler: %dx%d -> %dx%d, %s -> BGRA",
								sw_frame->width, sw_frame->height,
								output_width, output_height,
								av_get_pix_fmt_name(sw_pix_fmt));
							
							/* Use BILINEAR for hardware decoded frames for performance
							 * Hardware decoding + complex scaling can overload the system */
							decoder->sws_ctx = sws_getContext(
								sw_frame->width, sw_frame->height, sw_pix_fmt,
								output_width, output_height,
								AV_PIX_FMT_BGRA,  /* BGRA format - known to work */
								SWS_BILINEAR | SWS_ACCURATE_RND, NULL, NULL, NULL);
							
							if (!decoder->sws_ctx) {
								blog(LOG_ERROR, "[FFmpeg Decoder] Failed to create HW scaler context for format %s",
									av_get_pix_fmt_name(sw_pix_fmt));
								decoder->hw_decoding_active = false;
								continue;
							}
						}
						
						/* Log successful HW decode periodically */
						if (hw_frame_count % 300 == 1) { /* Log every 300 frames (~10 seconds at 30fps) */
							blog(LOG_INFO, "[FFmpeg Decoder] HW decode frame %d: format=%s, width=%d, height=%d",
								hw_frame_count, av_get_pix_fmt_name(sw_frame->format),
								sw_frame->width, sw_frame->height);
						}
					}
					
					/* Calculate PTS in microseconds */
					AVStream *stream = decoder->format_ctx->streams[decoder->video_stream_idx];
					int64_t pts_us = AV_NOPTS_VALUE;
					
					if (sw_frame->pts != AV_NOPTS_VALUE) {
						double pts_seconds = sw_frame->pts * av_q2d(stream->time_base);
						pts_us = (int64_t)(pts_seconds * 1000000.0);
					}
					
					/* Mark decode complete for performance tracking */
					if (decoder->perf_monitor) {
						perf_monitor_decode_complete((perf_monitor_t*)decoder->perf_monitor);
					}
					
					/* On first frame after start/seek, reset clock */
					if (decoder->waiting_for_first_frame && pts_us != AV_NOPTS_VALUE) {
						clock_reset(decoder, pts_us);
						decoder->waiting_for_first_frame = false;
						decoder->pts_offset = pts_us * 1000;  /* Video PTS offset in ns */
						
						/* Only set start time if audio hasn't set it yet */
						if (decoder->waiting_for_first_audio) {
							decoder->start_time_ns = os_gettime_ns();
						}
						
						blog(LOG_INFO, "First video frame after seek/start, PTS %lld us, start_time set: %s", 
							(long long)pts_us, decoder->waiting_for_first_audio ? "yes" : "no");
					}
					
					if (pts_us != AV_NOPTS_VALUE) {
						/* Check if scaler is ready for formats that need it */
						bool needs_scaler = !(decoder->hw_decoding_active && 
						                     (sw_frame->format == AV_PIX_FMT_NV12 || sw_frame->format == AV_PIX_FMT_P010LE));
						
						if (needs_scaler && !decoder->sws_ctx) {
							blog(LOG_WARNING, "[FFmpeg Decoder] Scaler not ready for frame format %s with PTS %lld, skipping", 
								av_get_pix_fmt_name(sw_frame->format), (long long)pts_us);
							if (decoder->frame) {
								av_frame_unref(decoder->frame);
							}
							continue;
						}
						
						if (frames_decoded % 100 == 0) {
							blog(LOG_INFO, "[FFmpeg Decoder] Processing frame %llu with PTS %lld us", 
								(unsigned long long)frames_decoded, (long long)pts_us);
						}
						
						/* Get target display time using clock system */
						uint64_t display_time = clock_get_system_time_for_pts(decoder, pts_us);
						
						if (frames_decoded % 100 == 0) {
							blog(LOG_INFO, "[FFmpeg Decoder] Display time calculated: %llu", 
								(unsigned long long)display_time);
						}
						
						/* Track if we created a temp frame that needs freeing */
						AVFrame *temp_frame_to_free = NULL;
						
						/* Decode ahead and buffer frames */
						pthread_mutex_lock(&decoder->buffer.lock);
						
						if (frames_decoded % 100 == 0) {
							blog(LOG_INFO, "[FFmpeg Decoder] About to store frame %llu, buffer count=%d", 
								(unsigned long long)frames_decoded, decoder->buffer.count);
						}
						
						/* Wait if buffer is full */
						while (decoder->buffer.count >= 3 && !atomic_load(&decoder->stopping)) {
							/* Signal display thread that frames are available */
							pthread_cond_signal(&decoder->buffer.cond);
							/* Wait with condition variable instead of polling */
							pthread_cond_wait(&decoder->buffer.cond, &decoder->buffer.lock);
						}
						
						if (!atomic_load(&decoder->stopping)) {
							/* Get next buffer slot */
							struct buffered_frame *buf_frame = &decoder->buffer.frames[decoder->buffer.write_idx];
							
							/* Check if we have 10-bit formats */
							bool is_p010 = (sw_frame->format == AV_PIX_FMT_P010LE);
							bool is_yuv420p10 = (sw_frame->format == AV_PIX_FMT_YUV420P10LE);
							bool is_10bit = is_p010 || is_yuv420p10;
							
							/* Log HEVC format details on first frame */
							if (frames_decoded == 0 && (strcmp(decoder->video_codec_ctx->codec->name, "hevc") == 0 ||
							                            strcmp(decoder->video_codec_ctx->codec->name, "h265") == 0)) {
								blog(LOG_INFO, "[HEVC] First frame format: %s (%d), hw_active: %s, 10-bit: %s",
									av_get_pix_fmt_name(sw_frame->format), sw_frame->format,
									decoder->hw_decoding_active ? "yes" : "no",
									is_10bit ? "yes" : "no");
								
								if (is_p010) {
									blog(LOG_INFO, "[HEVC] P010LE format from hardware decoder detected");
								} else if (is_yuv420p10) {
									blog(LOG_INFO, "[HEVC] YUV420P10LE format from software decoder detected");
								}
							}
							
							/* For P010 hardware format, pass directly to OBS - no conversion needed */
							/* For YUV420P10 software format, we still need to convert */
							if (is_yuv420p10) {
								if (frames_decoded == 0) {
									blog(LOG_INFO, "[FFmpeg Decoder] Detected 10-bit format: %s (%d), will convert to 8-bit",
										av_get_pix_fmt_name(sw_frame->format), sw_frame->format);
								}
								
								/* Create a scaler for 10-bit to 8-bit conversion if needed */
								if (!decoder->p010_sws_ctx) {
									blog(LOG_INFO, "[FFmpeg Decoder] Creating optimized 10-bit to NV12 scaler for format %s",
										av_get_pix_fmt_name(sw_frame->format));
									
									/* Convert directly to NV12 to avoid double conversion
									 * Use BICUBIC for good quality/performance balance
									 * LANCZOS is too slow for real-time playback */
									decoder->p010_sws_ctx = sws_getContext(
										sw_frame->width, sw_frame->height, sw_frame->format,
										sw_frame->width, sw_frame->height, AV_PIX_FMT_NV12,
										SWS_BICUBIC | SWS_ACCURATE_RND, NULL, NULL, NULL);
									
									if (!decoder->p010_sws_ctx) {
										blog(LOG_ERROR, "[FFmpeg Decoder] Failed to create 10-bit to 8-bit scaler");
										pthread_mutex_unlock(&decoder->buffer.lock);
										continue;
									}
								}
								
								/* Allocate temporary frame for 8-bit YUV420P */
								AVFrame *temp_frame = av_frame_alloc();
								if (!temp_frame) {
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to allocate temp frame");
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								/* Set up frame parameters - output as NV12 to avoid double conversion */
								temp_frame->format = AV_PIX_FMT_NV12;
								temp_frame->width = sw_frame->width;
								temp_frame->height = sw_frame->height;
								
								/* Allocate buffer for the frame with proper alignment */
								int ret = av_frame_get_buffer(temp_frame, 0);  /* Use default alignment */
								if (ret < 0) {
									char errbuf[AV_ERROR_MAX_STRING_SIZE];
									av_strerror(ret, errbuf, sizeof(errbuf));
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to allocate temp frame buffer: %s", errbuf);
									av_frame_free(&temp_frame);
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								/* Make frame writable */
								ret = av_frame_make_writable(temp_frame);
								if (ret < 0) {
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to make temp frame writable");
									av_frame_free(&temp_frame);
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								/* Convert 10-bit to 8-bit */
								ret = sws_scale(decoder->p010_sws_ctx,
									(const uint8_t * const *)sw_frame->data, sw_frame->linesize,
									0, sw_frame->height,
									temp_frame->data, temp_frame->linesize);
								
								if (ret != sw_frame->height) {
									blog(LOG_ERROR, "[FFmpeg Decoder] 10-bit to 8-bit conversion failed: expected %d lines, got %d",
										sw_frame->height, ret);
									av_frame_free(&temp_frame);
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								/* Copy important metadata from source frame */
								temp_frame->pts = sw_frame->pts;
								temp_frame->pkt_dts = sw_frame->pkt_dts;
								temp_frame->best_effort_timestamp = sw_frame->best_effort_timestamp;
								
								/* Use the 8-bit frame instead */
								sw_frame = temp_frame;
								temp_frame_to_free = temp_frame;  /* Remember to free it later */
							}
							
							/* Always output NV12 or P010 for hardware frames */
							/* For BGRA output mode with hardware frames, we still output NV12/P010 to OBS */
							bool is_hw_format = (sw_frame->format == AV_PIX_FMT_NV12 || 
							                     sw_frame->format == AV_PIX_FMT_P010LE);
							buf_frame->is_hw_frame = is_hw_format;
							
							/* Check if we can use zero-copy (direct frame reference) */
							/* P010LE MUST use zero-copy since we can't convert it */
							bool can_zero_copy = false;
							if (is_p010) {
								/* P010 must always use zero-copy - we can't convert it */
								can_zero_copy = !decoder->needs_aspect_correction;
							} else if (is_hw_format) {
								/* NV12 can use zero-copy if configured for NV12 output */
								can_zero_copy = decoder->use_nv12_output && !decoder->needs_aspect_correction;
							}
							buf_frame->zero_copy = can_zero_copy;
							
							/* Allocate BGRA buffer for this frame if needed */
							if (!buf_frame->bgra_data[0]) {
								/* Use corrected dimensions for buffer allocation */
								int buffer_width = decoder->needs_aspect_correction ? decoder->adjusted_width : decoder->video_codec_ctx->width;
								int buffer_height = decoder->needs_aspect_correction ? decoder->adjusted_height : decoder->video_codec_ctx->height;
								
								int ret = av_image_alloc(buf_frame->bgra_data, (int*)buf_frame->bgra_linesize,
									buffer_width, buffer_height,
									AV_PIX_FMT_BGRA, 32);
								if (ret < 0) {
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to allocate BGRA buffer for frame");
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
							}
							
							/* Create scaler if needed for software frames (but not for hardware formats) */
							if (!decoder->sws_ctx && !buf_frame->is_hw_frame) {
								enum AVPixelFormat src_pix_fmt = sw_frame->format;
								
								/* Use corrected dimensions for output if aspect ratio correction is needed */
								int output_width = decoder->needs_aspect_correction ? decoder->adjusted_width : decoder->video_codec_ctx->width;
								int output_height = decoder->needs_aspect_correction ? decoder->adjusted_height : decoder->video_codec_ctx->height;
								
								blog(LOG_INFO, "[FFmpeg Decoder] Creating software scaler: %dx%d -> %dx%d, %s -> BGRA",
									sw_frame->width, sw_frame->height,
									output_width, output_height,
									av_get_pix_fmt_name(src_pix_fmt));
								
								/* Use BILINEAR for real-time performance
								 * BICUBIC is better quality but can be too slow for multiple streams */
								decoder->sws_ctx = sws_getContext(
									sw_frame->width, sw_frame->height, src_pix_fmt,
									output_width, output_height,
									AV_PIX_FMT_BGRA,
									SWS_BILINEAR | SWS_ACCURATE_RND, NULL, NULL, NULL);
								
								if (!decoder->sws_ctx) {
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to create software scaler");
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
							}
							
							/* Release any previous frame reference */
							if (buf_frame->frame) {
								av_frame_unref(buf_frame->frame);
								av_frame_free(&buf_frame->frame);
								buf_frame->frame = NULL;
							}
							
							/* Check if we should output NV12/P010 directly */
							int scale_ret = 0;
							if (buf_frame->zero_copy) {
								/* Zero-copy path: Clone the frame (creates new reference to same data) */
								buf_frame->frame = av_frame_clone(sw_frame);
								if (!buf_frame->frame) {
									blog(LOG_ERROR, "[FFmpeg Decoder] Failed to clone frame for zero-copy");
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								scale_ret = sw_frame->height; /* Success */
								
								if (frames_decoded % 100 == 0) {
									const char *fmt = is_p010 ? "P010" : "NV12";
									blog(LOG_INFO, "[FFmpeg Decoder] Using %s zero-copy (no memcpy)", fmt);
								}
							} else if (buf_frame->is_hw_frame && !is_p010) {
								/* Output NV12 with memory copy (for compatibility) - but NOT for P010! */
								
								/* Safety check - P010 should never reach here */
								if (is_p010) {
									blog(LOG_ERROR, "[FFmpeg Decoder] P010 frames must use zero-copy path!");
									pthread_mutex_unlock(&decoder->buffer.lock);
									continue;
								}
								
								/* Allocate NV12 buffers if not already done */
								if (!buf_frame->nv12_data[0]) {
									/* For P010LE, we need to allocate based on output NV12 size */
									int y_linesize, uv_linesize;
									if (is_p010) {
										/* P010 has 16-bit per sample, but we're converting to 8-bit NV12 */
										/* Use proper alignment for NV12 output */
										y_linesize = FFALIGN(sw_frame->width, 32);
										uv_linesize = y_linesize;  /* NV12 has same linesize for UV as Y */
									} else {
										/* Use source frame's linesize for proper alignment */
										y_linesize = sw_frame->linesize[0];
										uv_linesize = sw_frame->linesize[1];
									}
									
									int y_size = y_linesize * sw_frame->height;
									int uv_size = uv_linesize * (sw_frame->height / 2);
									
									/* NV12 requires contiguous memory: Y plane followed by UV plane 
									 * Add extra padding to prevent buffer overruns */
									int total_size = y_size + uv_size + 64; /* 64 bytes extra for safety */
									buf_frame->nv12_data[0] = aligned_alloc_simd(total_size);
									if (!buf_frame->nv12_data[0]) {
										blog(LOG_ERROR, "[FFmpeg Decoder] Failed to allocate NV12 buffer");
										pthread_mutex_unlock(&decoder->buffer.lock);
										continue;
									}
									/* Clear the buffer to prevent uninitialized memory issues */
									memset(buf_frame->nv12_data[0], 0, total_size);
									
									/* UV plane immediately follows Y plane in memory */
									buf_frame->nv12_data[1] = buf_frame->nv12_data[0] + y_size;
									/* Store the linesize for our output buffer */
									buf_frame->nv12_linesize[0] = y_linesize;
									buf_frame->nv12_linesize[1] = uv_linesize;
								}
								
								/* With software HEVC decoding, we shouldn't get P010 frames */
								{
									/* Use SIMD-optimized copy for NV12 planes */
									copy_nv12_optimized(buf_frame->nv12_data[0], buf_frame->nv12_data[1],
									                   sw_frame->data[0], sw_frame->data[1],
									                   buf_frame->nv12_linesize[0], buf_frame->nv12_linesize[1],
									                   sw_frame->linesize[0], sw_frame->linesize[1],
									                   sw_frame->width, sw_frame->height);
									
									if (frames_decoded % 100 == 0) {
										blog(LOG_INFO, "[FFmpeg Decoder] Using NV12 output (no conversion)");
									}
								}
								
								scale_ret = sw_frame->height; /* Success */
							} else if (!buf_frame->is_hw_frame) {
								/* Convert frame to BGRA into this frame's buffer */
								
								/* Try SIMD conversion first for YUV420P only if no aspect correction needed */
								yuv_convert_func simd_converter = NULL;
								if (sw_frame->format == AV_PIX_FMT_YUV420P && !decoder->needs_aspect_correction) {
									simd_converter = simd_get_best_yuv420_converter();
								}
								
								if (simd_converter) {
									/* Use optimized SIMD conversion (only for 1:1 conversion) */
									simd_converter(
										sw_frame->data[0], sw_frame->linesize[0],
										sw_frame->data[1], sw_frame->linesize[1],
										sw_frame->data[2], sw_frame->linesize[2],
										buf_frame->bgra_data[0], buf_frame->bgra_linesize[0],
										sw_frame->width, sw_frame->height);
									scale_ret = sw_frame->height;
								} else {
									/* Use swscale for aspect ratio correction or format conversion */
									int scale_height = sw_frame->height;  /* Use actual frame height for input */
									scale_ret = sws_scale(decoder->sws_ctx, 
										(const uint8_t * const *)sw_frame->data, sw_frame->linesize,
										0, scale_height,
										buf_frame->bgra_data, (int*)buf_frame->bgra_linesize);
								}
							}
							
							/* Mark conversion complete for performance tracking */
							/* Only count actual conversions, not zero-copy */
							if (decoder->perf_monitor && !buf_frame->zero_copy) {
								perf_monitor_convert_complete((perf_monitor_t*)decoder->perf_monitor);
							}
							
							if (scale_ret <= 0) {
								blog(LOG_ERROR, "[FFmpeg Decoder] sws_scale failed, returned %d", scale_ret);
								pthread_mutex_unlock(&decoder->buffer.lock);
								continue;
							}
							
							/* Store frame in buffer */
							if (buf_frame->frame) {
								av_frame_unref(buf_frame->frame);
								av_frame_free(&buf_frame->frame);
							}
							
							/* Clone the frame instead of just creating a reference.
							 * This is crucial when sw_frame is our temp_frame from 10-bit conversion,
							 * as we'll be freeing temp_frame_to_free after this. */
							buf_frame->frame = av_frame_clone(sw_frame);
							if (!buf_frame->frame) {
								blog(LOG_ERROR, "[FFmpeg Decoder] Failed to clone frame for buffer storage");
								pthread_mutex_unlock(&decoder->buffer.lock);
								continue;
							}
							buf_frame->pts = pts_us;
							buf_frame->system_time = display_time;
							buf_frame->ready = true;
							
							/* Mark frame complete for performance tracking */
							if (decoder->perf_monitor) {
								perf_monitor_frame_complete((perf_monitor_t*)decoder->perf_monitor);
							}
							
							/* Update buffer indices */
							decoder->buffer.write_idx = (decoder->buffer.write_idx + 1) % 3;
							int old_count = decoder->buffer.count;
							decoder->buffer.count++;
							
							if (frames_decoded % 100 == 0) {
								blog(LOG_INFO, "[FFmpeg Decoder] Stored frame %llu in buffer, count=%d (was %d), is_hw=%d", 
									(unsigned long long)frames_decoded, decoder->buffer.count, old_count, buf_frame->is_hw_frame);
							}
							
							/* Always signal display thread when adding frames */
							pthread_cond_broadcast(&decoder->buffer.cond);
							
							if (frames_decoded < 5 || frames_decoded % 100 == 0) {
								blog(LOG_INFO, "[FFmpeg Decoder] Broadcast signal sent, count=%d", decoder->buffer.count);
							}
							
							frames_decoded++;
							if (frames_decoded % 300 == 1) { /* Log every 300 frames (~10 seconds at 30fps) */
								blog(LOG_INFO, "[FFmpeg Decoder] Decoded frame %lld, PTS=%lld ms, buffer: %d/3, size: %dx%d", 
									frames_decoded, (long long)(pts_us / 1000), decoder->buffer.count,
									decoder->video_codec_ctx->width, decoder->video_codec_ctx->height);
							}
						}
						
						pthread_mutex_unlock(&decoder->buffer.lock);
						
						/* Free temp frame if we created one for 10-bit conversion */
						if (temp_frame_to_free) {
							av_frame_free(&temp_frame_to_free);
						}
					}
					/* Safely unref the decoder frame */
					if (decoder->frame) {
						av_frame_unref(decoder->frame);
					}
					/* Note: hw_frame is reused, don't unref it here */
				}
			}
		}
		/* Decode audio packet */
		else if (packet->stream_index == decoder->audio_stream_idx && decoder->audio_codec_ctx) {
			ret = avcodec_send_packet(decoder->audio_codec_ctx, packet);
			if (ret >= 0) {
				while (avcodec_receive_frame(decoder->audio_codec_ctx, decoder->audio_frame) >= 0) {
					/* Check if we're stopping */
					if (atomic_load(&decoder->stopping))
						break;
					
					/* Output audio frame */
					if (decoder->audio_cb && decoder->audio_frame && decoder->audio_frame->nb_samples > 0) {
						struct obs_source_audio audio = {0};
						audio.samples_per_sec = decoder->audio_codec_ctx->sample_rate;
						audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
						audio.speakers = SPEAKERS_STEREO;
						audio.frames = decoder->audio_frame->nb_samples;
						
						/* Calculate timestamp - use frame PTS directly */
						AVStream *stream = decoder->format_ctx->streams[decoder->audio_stream_idx];
						if (decoder->audio_frame->pts != AV_NOPTS_VALUE) {
							double pts_seconds = decoder->audio_frame->pts * av_q2d(stream->time_base);
							uint64_t pts_ns = (uint64_t)(pts_seconds * 1000000000.0);
							
							/* On first audio frame, establish audio baseline */
							if (decoder->waiting_for_first_audio) {
								decoder->audio_pts_offset = pts_ns;  /* Audio PTS offset */
								decoder->waiting_for_first_audio = false;
								
								/* Only set start time if video hasn't set it yet */
								if (decoder->waiting_for_first_frame) {
									decoder->start_time_ns = os_gettime_ns();
								}
								
								blog(LOG_INFO, "First audio frame, PTS: %lld ns, start_time set: %s", 
									pts_ns, decoder->waiting_for_first_frame ? "yes" : "no");
							}
							
							/* Use audio-specific PTS offset for audio timestamp */
							/* Apply same timeline sync as video for perfect A/V sync */
							audio.timestamp = decoder->start_time_ns + (pts_ns - decoder->audio_pts_offset);
							
							/* Log audio sync periodically for debugging */
							static int audio_frame_count = 0;
							if (++audio_frame_count % 1000 == 0) {
								int64_t video_pts = decoder->frame_pts;
								int64_t audio_pts = pts_ns / 1000;  /* Convert to microseconds */
								int64_t av_diff = (video_pts - audio_pts) / 1000;  /* Diff in ms */
								if (abs((int)av_diff) > 50) {
									blog(LOG_INFO, "A/V sync: video=%lld ms, audio=%lld ms, diff=%lld ms",
										(long long)(video_pts / 1000), (long long)(audio_pts / 1000), (long long)av_diff);
								}
							}
						} else {
							audio.timestamp = os_gettime_ns();
						}
						
						/* Handle audio resampling if needed */
						bool audio_ready = false;
						if (decoder->swr_ctx) {
							/* Check if input samples exceed our buffer capacity */
							int expected_out_samples = swr_get_out_samples(decoder->swr_ctx, 
								decoder->audio_frame->nb_samples);
							
							if (expected_out_samples > decoder->max_resampled_samples) {
								/* Dynamically resize buffer if needed */
								int new_size = expected_out_samples * 2;  /* Double for safety */
								blog(LOG_WARNING, "Audio buffer too small (%d samples needed, %d available), resizing to %d",
									expected_out_samples, decoder->max_resampled_samples, new_size);
								
								/* Free old buffer */
								av_freep(&decoder->resampled_audio_data[0]);
								
								/* Allocate new larger buffer */
								int ret = av_samples_alloc(decoder->resampled_audio_data, 
									&decoder->resampled_audio_linesize,
									2, new_size, AV_SAMPLE_FMT_FLTP, 0);
								
								if (ret < 0) {
									blog(LOG_ERROR, "Failed to resize audio buffer: %s", av_err2str(ret));
									/* Skip this frame as fallback */
									continue;
								}
								
								decoder->max_resampled_samples = new_size;
								blog(LOG_INFO, "Audio buffer resized successfully to %d samples", new_size);
							}
							
							/* Now safe to resample */
							if (expected_out_samples <= decoder->max_resampled_samples) {
								/* Safe to resample audio to FLTP stereo */
								int out_samples = swr_convert(decoder->swr_ctx,
									decoder->resampled_audio_data,
									decoder->max_resampled_samples,
									(const uint8_t **)decoder->audio_frame->data,
									decoder->audio_frame->nb_samples);
								
								if (out_samples > 0) {
									/* Additional safety check */
									if (out_samples > decoder->max_resampled_samples) {
										blog(LOG_ERROR, "Audio buffer overflow detected: %d samples > %d max",
											out_samples, decoder->max_resampled_samples);
										out_samples = decoder->max_resampled_samples;
									}
									
									/* Use resampled audio */
									audio.frames = out_samples;
									for (int i = 0; i < 2; i++) {  /* Stereo output */
										audio.data[i] = decoder->resampled_audio_data[i];
									}
									audio_ready = true;
								} else if (out_samples < 0) {
									blog(LOG_WARNING, "Audio resampling failed: %s", av_err2str(out_samples));
								}
							}
						} else {
							/* Audio is already in correct format (FLTP stereo) */
							int valid_channels = 0;
							for (int i = 0; i < 2 && i < AV_NUM_DATA_POINTERS; i++) {
								if (decoder->audio_frame->data[i]) {
									audio.data[i] = decoder->audio_frame->data[i];
									valid_channels++;
								} else {
									break;
								}
							}
							audio_ready = (valid_channels == 2);
						}
						
						/* Only output if we have valid audio data and callbacks */
						if (audio_ready) {
							/* Get callback under lock and check stopping flag */
							pthread_mutex_lock(&decoder->mutex);
							if (!atomic_load(&decoder->stopping) && decoder->audio_cb && decoder->opaque) {
								void (*cb)(void *, struct obs_source_audio *) = decoder->audio_cb;
								void *opaque = decoder->opaque;
								pthread_mutex_unlock(&decoder->mutex);
								cb(opaque, &audio);
							} else {
								pthread_mutex_unlock(&decoder->mutex);
							}
						}
					}
					if (decoder->audio_frame) {
						av_frame_unref(decoder->audio_frame);
					}
				}
			}
		}
		
		av_packet_unref(packet);
	}
	
	av_packet_free(&packet);
	
	/* Mark thread as not running */
	pthread_mutex_lock(&decoder->mutex);
	atomic_store(&decoder->thread_running, false);
	pthread_mutex_unlock(&decoder->mutex);
	
	blog(LOG_INFO, "Decoder thread stopped");
	return NULL;
}

void ffmpeg_decoder_play(struct ffmpeg_decoder *decoder)
{
	/* Call with 0 timeline for backward compatibility */
	ffmpeg_decoder_play_with_timeline(decoder, 0);
}

void ffmpeg_decoder_play_with_timeline(struct ffmpeg_decoder *decoder, uint64_t timeline_start_ms)
{
	if (!decoder || !decoder->initialized)
		return;
	
	blog(LOG_INFO, "ffmpeg_decoder_play_with_timeline called, timeline_start_ms=%llu", 
		(unsigned long long)timeline_start_ms);
	
	/* Clear interrupt flag to allow normal operation */
	decoder->interrupt_request = false;
	
	pthread_mutex_lock(&decoder->mutex);
	decoder->global_timeline_start_ms = timeline_start_ms;
	decoder->state = DECODER_STATE_PLAYING;
	atomic_store(&decoder->playing, true);
	decoder->looping = true;  /* Enable looping by default */
	decoder->waiting_for_first_frame = true;
	decoder->waiting_for_first_audio = true;
	atomic_store(&decoder->stopping, false);  /* Reset stopping flag */
	/* Don't set timing here - let first frame establish it */
	pthread_mutex_unlock(&decoder->mutex);
	
	/* Start display thread if not running */
	if (!decoder->display_thread_created) {
		blog(LOG_INFO, "Starting display thread");
		if (pthread_create(&decoder->display_thread, NULL, display_thread, decoder) == 0) {
			decoder->display_thread_created = true;
		}
	} else {
		blog(LOG_INFO, "Display thread already running");
	}
	
	/* Start decoder thread if not running */
	if (!decoder->thread_running) {
		blog(LOG_INFO, "Starting decoder thread");
		atomic_store(&decoder->thread_running, true);
		pthread_create(&decoder->thread, NULL, decoder_thread, decoder);
	} else {
		blog(LOG_INFO, "Decoder thread already running");
	}
	
	/* Wake up display thread in case it's waiting */
	pthread_mutex_lock(&decoder->buffer.lock);
	pthread_cond_signal(&decoder->buffer.cond);
	pthread_mutex_unlock(&decoder->buffer.lock);
	
	blog(LOG_INFO, "Playback started - decoder initialized: %d, playing: %d", 
		decoder->initialized, decoder->playing);
}

void ffmpeg_decoder_pause(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return;
	
	atomic_store(&decoder->playing, false);
	
	blog(LOG_INFO, "Playback paused");
}

void ffmpeg_decoder_stop(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return;
	
	pthread_mutex_lock(&decoder->mutex);
	decoder->state = DECODER_STATE_STOPPED;
	pthread_mutex_unlock(&decoder->mutex);
	
	atomic_store(&decoder->playing, false);
	
	blog(LOG_INFO, "Playback stopped");
}

void ffmpeg_decoder_stop_thread(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return;
	
	blog(LOG_INFO, "Stopping decoder threads (aggressive cleanup)...");
	
	/* CRITICAL: Set interrupt flag FIRST to interrupt FFmpeg blocking calls */
	decoder->interrupt_request = true;
	
	/* Signal threads to stop */
	pthread_mutex_lock(&decoder->mutex);
	atomic_store(&decoder->stopping, true);
	atomic_store(&decoder->playing, false);
	/* Don't clear callbacks here - they should persist across stop/play cycles */
	pthread_mutex_unlock(&decoder->mutex);
	
	/* Wake up display thread */
	pthread_mutex_lock(&decoder->buffer.lock);
	pthread_cond_broadcast(&decoder->buffer.cond);
	pthread_mutex_unlock(&decoder->buffer.lock);
	
	/* Stop display thread first with adaptive timeout */
	if (decoder->display_thread_created) {
		/* Use shorter initial waits, then longer ones */
		int total_wait_ms = 0;
		const int max_wait_ms = 5000;  /* 5 seconds max - more reasonable */
		
		/* Phase 1: Quick checks (first 100ms) */
		for (int i = 0; i < 20 && total_wait_ms < 100; i++) {
			pthread_mutex_lock(&decoder->mutex);
			bool display_active = decoder->display_thread_created && !atomic_load(&decoder->stopping);
			pthread_mutex_unlock(&decoder->mutex);
			
			if (!display_active) goto thread_stopped;
			
			os_sleep_ms(5);
			total_wait_ms += 5;
		}
		
		/* Phase 2: Medium waits (100ms to 1s) */
		while (total_wait_ms < 1000) {
			pthread_mutex_lock(&decoder->mutex);
			bool display_active = decoder->display_thread_created && !atomic_load(&decoder->stopping);
			pthread_mutex_unlock(&decoder->mutex);
			
			if (!display_active) goto thread_stopped;
			
			os_sleep_ms(50);
			total_wait_ms += 50;
		}
		
		/* Phase 3: Long waits (1s to max) */
		while (total_wait_ms < max_wait_ms) {
			pthread_mutex_lock(&decoder->mutex);
			bool display_active = decoder->display_thread_created && !atomic_load(&decoder->stopping);
			pthread_mutex_unlock(&decoder->mutex);
			
			if (!display_active) goto thread_stopped;
			
			os_sleep_ms(100);
			total_wait_ms += 100;
		}
		
		blog(LOG_WARNING, "Display thread did not stop within %d ms timeout", max_wait_ms);
		
thread_stopped:
		
		/* Always attempt join to clean up resources */
		pthread_join(decoder->display_thread, NULL);
		decoder->display_thread_created = false;
		blog(LOG_INFO, "Display thread stopped");
	}
	
	/* Stop decoder thread with timeout */
	if (atomic_load(&decoder->thread_running)) {
		/* Wait for thread with timeout - portable approach */
		int wait_count = 0;
		const int max_wait = 300;  /* 3 seconds (300 * 10ms) */
		
		while (wait_count < max_wait) {
			/* Check if thread has marked itself as not running */
			pthread_mutex_lock(&decoder->mutex);
			bool still_running = atomic_load(&decoder->thread_running);
			pthread_mutex_unlock(&decoder->mutex);
			
			if (!still_running)
				break;
			
			os_sleep_ms(10);
			wait_count++;
		}
		
		if (wait_count >= max_wait) {
			blog(LOG_ERROR, "Decoder thread did not stop within 3 second timeout!");
			blog(LOG_WARNING, "Thread may be stuck in FFmpeg call");
		}
		
		/* Always attempt join to clean up resources */
		pthread_join(decoder->thread, NULL);
		atomic_store(&decoder->thread_running, false);
		blog(LOG_INFO, "Decoder thread stopped");
	}
	
	/* Reset stopping flag for next play */
	atomic_store(&decoder->stopping, false);
}

void ffmpeg_decoder_free_scalers(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return;
	
	/* Free scalers to save memory when scene is inactive */
	if (decoder->sws_ctx) {
		sws_freeContext(decoder->sws_ctx);
		decoder->sws_ctx = NULL;
	}
	if (decoder->p010_sws_ctx) {
		sws_freeContext(decoder->p010_sws_ctx);
		decoder->p010_sws_ctx = NULL;
	}
	
	blog(LOG_INFO, "[FFmpeg Decoder] Freed scalers for inactive scene");
}

void ffmpeg_decoder_seek(struct ffmpeg_decoder *decoder, int64_t position_us)
{
	if (!decoder || !decoder->initialized)
		return;
	
	/* Simplified seek - just set flag for decoder thread to handle */
	pthread_mutex_lock(&decoder->mutex);
	atomic_store(&decoder->seek_request, true);
	decoder->seek_target = position_us;
	pthread_mutex_unlock(&decoder->mutex);
	
	blog(LOG_INFO, "Seek requested to %lld us", (long long)position_us);
}

int64_t ffmpeg_decoder_get_position(struct ffmpeg_decoder *decoder)
{
	if (!decoder || !decoder->initialized)
		return 0;
	
	return decoder->frame_pts;
}

int64_t ffmpeg_decoder_get_duration(struct ffmpeg_decoder *decoder)
{
	if (!decoder || !decoder->initialized)
		return 0;
	
	return decoder->duration;
}

bool ffmpeg_decoder_is_initialized(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return false;
	
	return decoder->initialized;
}

bool ffmpeg_decoder_is_playing(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return false;
	
	pthread_mutex_lock(&decoder->mutex);
	bool playing = atomic_load(&decoder->playing);
	pthread_mutex_unlock(&decoder->mutex);
	
	return playing;
}

const char *ffmpeg_decoder_get_current_path(struct ffmpeg_decoder *decoder)
{
	if (!decoder)
		return NULL;
	
	return decoder->current_path;
}

void ffmpeg_decoder_set_callbacks(struct ffmpeg_decoder *decoder,
	void (*video_cb)(void *opaque, struct obs_source_frame *frame),
	void (*audio_cb)(void *opaque, struct obs_source_audio *audio),
	void *opaque)
{
	if (!decoder)
		return;
	
	decoder->video_cb = video_cb;
	decoder->audio_cb = audio_cb;
	decoder->opaque = opaque;
}

void ffmpeg_decoder_set_output_format(struct ffmpeg_decoder *decoder, bool use_nv12)
{
	if (!decoder)
		return;
	
	pthread_mutex_lock(&decoder->mutex);
	decoder->use_nv12_output = use_nv12;
	pthread_mutex_unlock(&decoder->mutex);
	
	blog(LOG_INFO, "[FFmpeg Decoder] Output format set to: %s", 
		use_nv12 ? "NV12 (no conversion)" : "BGRA (with conversion)");
}

/* Pause decoder but keep threads alive for quick resume */
void ffmpeg_decoder_pause_ready(struct ffmpeg_decoder *decoder)
{
	if (!decoder || !decoder->initialized)
		return;
	
	pthread_mutex_lock(&decoder->mutex);
	
	/* Save current state */
	decoder->state = DECODER_STATE_PAUSED_READY;
	decoder->state_preserved_time = os_gettime_ns() / 1000000;
	decoder->preserved_playback_position = decoder->frame_pts;
	
	/* Check if seek was in progress */
	if (atomic_load(&decoder->seek_request)) {
		decoder->seek_was_in_progress = true;
		decoder->preserved_seek_position = decoder->seek_target;
		blog(LOG_INFO, "[FFmpeg Decoder] Preserving interrupted seek to %lld us",
			(long long)decoder->preserved_seek_position);
	} else {
		decoder->seek_was_in_progress = false;
	}
	
	/* Pause playback but keep threads alive */
	atomic_store(&decoder->playing, false);
	
	pthread_mutex_unlock(&decoder->mutex);
	
	blog(LOG_INFO, "[FFmpeg Decoder] Paused in ready state - threads kept alive");
}

/* Resume from paused ready state */
bool ffmpeg_decoder_resume(struct ffmpeg_decoder *decoder)
{
	if (!decoder || !decoder->initialized)
		return false;
	
	pthread_mutex_lock(&decoder->mutex);
	
	/* Check if we're in the right state */
	if (decoder->state != DECODER_STATE_PAUSED_READY) {
		pthread_mutex_unlock(&decoder->mutex);
		return false;
	}
	
	/* Check if state is recent (less than 10 seconds old) */
	uint64_t current_time = os_gettime_ns() / 1000000;
	if (current_time - decoder->state_preserved_time > 10000) {
		blog(LOG_INFO, "[FFmpeg Decoder] Preserved state too old, need full restart");
		decoder->state = DECODER_STATE_STOPPED;
		pthread_mutex_unlock(&decoder->mutex);
		return false;
	}
	
	/* Resume playback */
	decoder->state = DECODER_STATE_PLAYING;
	atomic_store(&decoder->playing, true);
	
	/* Restore interrupted seek if there was one */
	if (decoder->seek_was_in_progress) {
		atomic_store(&decoder->seek_request, true);
		decoder->seek_target = decoder->preserved_seek_position;
		blog(LOG_INFO, "[FFmpeg Decoder] Resuming interrupted seek to %lld us",
			(long long)decoder->preserved_seek_position);
	}
	
	pthread_mutex_unlock(&decoder->mutex);
	
	/* Wake up display thread */
	pthread_mutex_lock(&decoder->buffer.lock);
	pthread_cond_signal(&decoder->buffer.cond);
	pthread_mutex_unlock(&decoder->buffer.lock);
	
	blog(LOG_INFO, "[FFmpeg Decoder] Resumed from paused state - instant restart!");
	return true;
}

/* Check if decoder is in paused ready state */
bool ffmpeg_decoder_is_paused_ready(struct ffmpeg_decoder *decoder)
{
	if (!decoder || !decoder->initialized)
		return false;
	
	pthread_mutex_lock(&decoder->mutex);
	bool is_paused = (decoder->state == DECODER_STATE_PAUSED_READY);
	pthread_mutex_unlock(&decoder->mutex);
	
	return is_paused;
}