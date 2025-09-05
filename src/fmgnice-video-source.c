/*
 * fmgNICE Video Source - A video source with synchronized timeline support
 * Uses custom FFmpeg decoder for reliable seeking
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <string.h>
#include "ffmpeg-decoder.h"

/* External functions from plugin-main.c */
extern void fmgnice_register_source(void *source);
extern void fmgnice_unregister_source(void *source);

/* FFmpeg headers for duration detection */
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#define S_PLAYLIST                     "playlist"
#define S_LOOP                         "loop"
#define S_HW_DECODE                    "hw_decode"
#define S_HW_DECODER                   "hw_decoder"
#define S_BUFFER_SIZE                  "buffer_size"
#define S_PREBUFFER_MS                 "prebuffer_ms"
#define S_SYNC_MODE                    "sync_mode"
#define S_SYNC_OFFSET                  "sync_offset"
#define S_SEEK_MODE                    "seek_mode"
#define S_FRAME_DROP                   "frame_drop"
#define S_AUDIO_BUFFER_MS              "audio_buffer_ms"
#define S_CACHE_SIZE_MB                "cache_size_mb"
#define S_PERFORMANCE_MODE             "performance_mode"
#define S_OUTPUT_FORMAT                "output_format"

#define T_PLAYLIST                     "Playlist"
#define T_LOOP                         "Loop Playlist"
#define T_HW_DECODE                    "Use Hardware Decoding"
#define T_HW_DECODER                   "Hardware Decoder"
#define T_BUFFER_SIZE                  "Frame Buffer Size"
#define T_PREBUFFER_MS                 "Pre-buffer Time (ms)"
#define T_SYNC_MODE                    "Sync Mode"
#define T_SYNC_OFFSET                  "Sync Offset (ms)"
#define T_SEEK_MODE                    "Seek Mode"
#define T_FRAME_DROP                   "Allow Frame Drop"
#define T_AUDIO_BUFFER_MS              "Audio Buffer (ms)"
#define T_CACHE_SIZE_MB                "Cache Size (MB)"
#define T_PERFORMANCE_MODE             "Performance Mode"
#define T_OUTPUT_FORMAT                "Output Format"

struct fvs_source {
	obs_source_t *source;
	struct ffmpeg_decoder *decoder;
	
	DARRAY(char*) playlist;
	size_t current_index;
	bool loop;
	
	/* Hardware decoding settings */
	bool hw_decode;
	int hw_decoder; /* 0=auto, 1=d3d11va, 2=dxva2, 3=cuda, 4=qsv */
	
	/* Buffer settings */
	int buffer_size;
	int prebuffer_ms;
	int audio_buffer_ms;
	int cache_size_mb;
	
	/* Sync settings */
	int sync_mode; /* 0=global, 1=local, 2=disabled */
	int sync_offset;
	
	/* Performance settings */
	int seek_mode; /* 0=accurate, 1=fast */
	bool frame_drop;
	int performance_mode; /* 0=quality, 1=balanced, 2=performance */
	int output_format; /* 0=BGRA (compatibility), 1=NV12 (performance) */
	
	/* Timeline tracking */
	uint64_t timeline_start_time;    /* When playlist started (wall clock) */
	uint64_t timeline_pause_time;    /* Deprecated - not used anymore */
	uint64_t timeline_total_offset;  /* Deprecated - not used anymore */
	bool timeline_active;             /* Whether source is currently visible */
	
	/* Duration cache */
	DARRAY(int64_t) durations;
	int64_t total_duration;
	
	/* Loop detection */
	int64_t last_expected_offset;
	
	/* Saved state for resume */
	int64_t saved_position;
	size_t saved_index;
	
	/* Deferred shutdown timer for rapid scene switches */
	pthread_t deactivate_timer_thread;
	bool deactivate_timer_active;
	uint64_t deactivate_time;
	#define DECODER_SHUTDOWN_DELAY_MS 2000  /* 2 second grace period */
	
	pthread_mutex_t mutex;
};

/* Global timeline for synchronization across all sources */
static uint64_t g_global_timeline_start = 0;
static pthread_mutex_t g_timeline_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Reset global timeline - call this to start a new synchronized show/event */
void fmgnice_reset_global_timeline(void)
{
	pthread_mutex_lock(&g_timeline_mutex);
	uint64_t old_timeline = g_global_timeline_start;
	g_global_timeline_start = 0;
	pthread_mutex_unlock(&g_timeline_mutex);
	
	if (old_timeline != 0) {
		blog(LOG_INFO, "[fmgNICE Video] Global timeline reset (was %llu ms)", 
			(unsigned long long)old_timeline);
	}
}

static const char *fvs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "fmgNICE Video Source";
}

static void free_playlist(struct fvs_source *s)
{
	for (size_t i = 0; i < s->playlist.num; i++) {
		bfree(s->playlist.array[i]);
	}
	da_free(s->playlist);
}

static void fvs_destroy(void *data)
{
	struct fvs_source *s = data;
	
	if (!s)
		return;
	
	/* Unregister from global tracking */
	fmgnice_unregister_source(s);
	
	/* Stop outputting frames immediately */
	if (s->source) {
		obs_source_output_video(s->source, NULL);
		obs_source_output_audio(s->source, NULL);
	}
	
	if (s->decoder) {
		/* Stop decoder and wait for it to finish */
		ffmpeg_decoder_stop(s->decoder);
		ffmpeg_decoder_stop_thread(s->decoder);
		
		/* Wait for OBS to finish processing frames */
		os_sleep_ms(100);
		
		ffmpeg_decoder_destroy(s->decoder);
		s->decoder = NULL;
	}
	
	free_playlist(s);
	da_free(s->durations);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
	
	/* Instance counter no longer used */
}

static void get_frame(void *opaque, struct obs_source_frame *frame)
{
	struct fvs_source *s = opaque;
	if (!s || !s->source || !frame)
		return;
	
	/* Validate frame data */
	if (!frame->data[0] || frame->width == 0 || frame->height == 0)
		return;
		
	obs_source_output_video(s->source, frame);
}

static void get_audio(void *opaque, struct obs_source_audio *audio)
{
	struct fvs_source *s = opaque;
	if (!s || !s->source || !audio)
		return;
	
	/* Validate audio data */
	if (!audio->data[0] || audio->frames == 0)
		return;
		
	obs_source_output_audio(s->source, audio);
}

/* Removed media_stopped callback - decoder handles EOF internally */

static void cache_durations(struct fvs_source *s)
{
	da_resize(s->durations, 0);
	s->total_duration = 0;
	
	for (size_t i = 0; i < s->playlist.num; i++) {
		const char *path = s->playlist.array[i];
		int64_t duration = 0;
		
		/* Get actual duration from video file using FFmpeg */
		AVFormatContext *fmt_ctx = NULL;
		if (avformat_open_input(&fmt_ctx, path, NULL, NULL) == 0) {
			if (avformat_find_stream_info(fmt_ctx, NULL) >= 0) {
				/* Duration is in AV_TIME_BASE units (microseconds) */
				duration = fmt_ctx->duration;
				if (duration == AV_NOPTS_VALUE || duration <= 0) {
					/* Fallback to 30 minutes if duration not available */
					duration = 30 * 60 * 1000000;
					blog(LOG_WARNING, "[fmgNICE Video] Could not get duration for %s, using 30 min default", path);
				}
			} else {
				/* Could not find stream info, use default */
				duration = 30 * 60 * 1000000;
				blog(LOG_WARNING, "[fmgNICE Video] Could not find stream info for %s, using 30 min default", path);
			}
			avformat_close_input(&fmt_ctx);
		} else {
			/* Could not open file, use default */
			duration = 30 * 60 * 1000000;
			blog(LOG_WARNING, "[fmgNICE Video] Could not open %s, using 30 min default", path);
		}
		
		da_push_back(s->durations, &duration);
		s->total_duration += duration;
		
		double duration_minutes = (double)duration / (1000000.0 * 60.0);
		blog(LOG_INFO, "[fmgNICE Video] File %zu: %s, duration=%.2f minutes (%lld us)", 
			i, path, duration_minutes, (long long)duration);
	}
	
	double total_hours = (double)s->total_duration / (1000000.0 * 3600.0);
	blog(LOG_INFO, "[fmgNICE Video] Total playlist duration: %.2f hours (%lld ms)", 
		total_hours, (long long)(s->total_duration / 1000));
}

static void calculate_timeline_position(struct fvs_source *s, 
                                       size_t *out_index, 
                                       int64_t *out_offset)
{
	if (s->timeline_start_time == 0 || s->durations.num == 0) {
		blog(LOG_INFO, "[fmgNICE Video] No timeline (start_time=%llu, durations=%zu)", 
			(unsigned long long)s->timeline_start_time, s->durations.num);
		*out_index = 0;
		*out_offset = 0;
		return;
	}
	
	/* Calculate elapsed time since timeline start
	 * For synchronized timeline, time always advances regardless of visibility
	 */
	uint64_t current_time = os_gettime_ns() / 1000000; /* Convert to ms */
	uint64_t elapsed_ms = current_time - s->timeline_start_time;
	int64_t elapsed_us = elapsed_ms * 1000; /* Convert to microseconds for FFmpeg */
	
	/* Calculate loop information for debug */
	int64_t loop_count = 0;
	int64_t original_elapsed_us = elapsed_us;
	
	/* Handle looping */
	if (s->loop && s->total_duration > 0) {
		loop_count = elapsed_us / s->total_duration;
		elapsed_us = elapsed_us % s->total_duration;
		
		double hours_elapsed = (double)original_elapsed_us / (1000000.0 * 3600.0);
		double playlist_hours = (double)s->total_duration / (1000000.0 * 3600.0);
		
		/* Only log timeline position occasionally to avoid spam */
		if ((elapsed_ms % 10000) < 50) { /* Log roughly every 10 seconds */
			blog(LOG_INFO, "[fmgNICE Video] Timeline: %.2f hours elapsed, playlist is %.2f hours, loop #%lld, position in loop: %lld ms",
				hours_elapsed, playlist_hours, (long long)loop_count + 1,
				(long long)(elapsed_us / 1000));
		}
	}
	/* Removed verbose timeline calc logging to improve performance */
	
	/* Find which file and offset */
	int64_t accumulated = 0;
	for (size_t i = 0; i < s->durations.num; i++) {
		if (elapsed_us < accumulated + s->durations.array[i]) {
			*out_index = i;
			*out_offset = elapsed_us - accumulated;
			
			/* Removed per-frame timeline position logging to improve performance */
			return;
		}
		accumulated += s->durations.array[i];
	}
	
	/* Past the end */
	if (s->loop && s->total_duration > 0) {
		/* This shouldn't happen with proper modulo, but handle it gracefully */
		*out_index = 0;
		*out_offset = 0;
		blog(LOG_INFO, "[fmgNICE Video] Timeline looping: restarting from file 0");
	} else {
		/* Not looping - stay at the last file */
		*out_index = s->durations.num > 0 ? s->durations.num - 1 : 0;
		*out_offset = s->durations.num > 0 ? s->durations.array[*out_index] : 0;
		blog(LOG_INFO, "[fmgNICE Video] Timeline past end (no loop): staying at file %zu", *out_index);
	}
}

static void start_playback(struct fvs_source *s)
{
	if (s->playlist.num == 0)
		return;
	
	size_t index = 0;
	int64_t offset = 0;
	
	/* Mark timeline as active */
	s->timeline_active = true;
	
	/* Calculate synchronized position based on continuous timeline */
	if (s->timeline_start_time > 0) {
		calculate_timeline_position(s, &index, &offset);
		s->current_index = index;
		blog(LOG_INFO, "[fmgNICE Video] Using synchronized position: file %zu, offset %lld ms", 
			index, (long long)(offset / 1000));
	} else {
		s->current_index = 0;
		blog(LOG_INFO, "[fmgNICE Video] Starting from beginning");
	}
	
	if (s->current_index >= s->playlist.num)
		s->current_index = 0;
	
	blog(LOG_INFO, "[fmgNICE Video] Playing file %zu: %s", 
		s->current_index, s->playlist.array[s->current_index]);
	
	/* Initialize decoder if needed */
	if (!s->decoder) {
		s->decoder = ffmpeg_decoder_create(s->source);
		ffmpeg_decoder_set_callbacks(s->decoder, get_frame, get_audio, s);
		/* Set output format based on user preference */
		ffmpeg_decoder_set_output_format(s->decoder, s->output_format == 1);
	}
	
	/* Check if we need to load a different file */
	const char *current_path = ffmpeg_decoder_get_current_path(s->decoder);
	const char *target_path = s->playlist.array[s->current_index];
	bool need_reinit = !current_path || strcmp(current_path, target_path) != 0;
	
	if (need_reinit) {
		blog(LOG_INFO, "[fmgNICE Video] Loading new file: %s", target_path);
		
		/* Stop any current playback */
		ffmpeg_decoder_stop(s->decoder);
		
		/* Initialize with new file */
		if (!ffmpeg_decoder_initialize(s->decoder, target_path)) {
			blog(LOG_ERROR, "[fmgNICE Video] Failed to initialize decoder");
			return;
		}
	} else {
		blog(LOG_INFO, "[fmgNICE Video] File already loaded, seeking to position");
	}
	
	/* Seek to synchronized position BEFORE starting playback */
	if (offset > 0) {
		/* Sanity check - don't seek past 95% of video duration */
		int64_t duration = ffmpeg_decoder_get_duration(s->decoder);
		int64_t max_seek = duration > 0 ? (duration * 95 / 100) : ((int64_t)30 * 60 * 1000000 * 95 / 100);
		
		if (offset > max_seek) {
			blog(LOG_WARNING, "[fmgNICE Video] Clamping seek from %lld ms to %lld ms (95%% of duration)", 
				(long long)(offset / 1000), (long long)(max_seek / 1000));
			offset = max_seek;
		}
		
		blog(LOG_INFO, "[fmgNICE Video] Seeking to synchronized position: %lld us (%lld ms)", 
			(long long)offset, (long long)(offset / 1000));
		ffmpeg_decoder_seek(s->decoder, offset);
	}
	
	/* Start playback at the seeked position with global timeline */
	ffmpeg_decoder_play_with_timeline(s->decoder, s->timeline_start_time);
}

static void fvs_activate(void *data)
{
	struct fvs_source *s = data;
	
	if (!s)
		return;
	
	blog(LOG_INFO, "[fmgNICE Video] Source activated");
	
	pthread_mutex_lock(&s->mutex);
	
	/* Cancel any pending deactivation timer */
	if (s->deactivate_timer_active) {
		s->deactivate_timer_active = false;
		blog(LOG_INFO, "[fmgNICE Video] Cancelled deactivation timer");
	}
	
	/* Use global timeline for synchronization */
	pthread_mutex_lock(&g_timeline_mutex);
	if (g_global_timeline_start == 0) {
		/* First source to activate sets the global timeline */
		g_global_timeline_start = os_gettime_ns() / 1000000;
		blog(LOG_INFO, "[fmgNICE Video] Initialized global timeline at %llu ms", 
			(unsigned long long)g_global_timeline_start);
	}
	/* All sources use the same global timeline */
	s->timeline_start_time = g_global_timeline_start;
	pthread_mutex_unlock(&g_timeline_mutex);
	
	/* Cache durations if not already done */
	if (s->durations.num == 0) {
		cache_durations(s);
	}
	
	blog(LOG_INFO, "[fmgNICE Video] Using global timeline: %llu ms", 
		(unsigned long long)s->timeline_start_time);
	
	s->timeline_active = true;
	
	/* Check if decoder is in paused ready state and can resume */
	if (s->decoder && ffmpeg_decoder_is_paused_ready(s->decoder)) {
		blog(LOG_INFO, "[fmgNICE Video] Resuming from paused state - instant restart!");
		if (ffmpeg_decoder_resume(s->decoder)) {
			/* Successfully resumed - no need to restart */
			pthread_mutex_unlock(&s->mutex);
			return;
		}
	}
	
	/* Otherwise restart cleanly */
	start_playback(s);
	
	pthread_mutex_unlock(&s->mutex);
}

static void fvs_video_tick(void *data, float seconds)
{
	struct fvs_source *s = data;
	
	UNUSED_PARAMETER(seconds);
	
	if (!s || !s->decoder)
		return;
	
	/* Skip processing if source is not active/visible to save CPU */
	if (!obs_source_active(s->source)) {
		return;
	}
	
	/* Check if decoder is playing and request next frame */
	if (ffmpeg_decoder_is_playing(s->decoder)) {
		/* The decoder thread will output frames at the right time */
		/* We just need to ensure it's running */
	}
	
	pthread_mutex_lock(&s->mutex);
	
	if (s->timeline_active && s->timeline_start_time > 0) {
		/* Calculate where we should be on the timeline */
		size_t expected_index = 0;
		int64_t expected_offset = 0;
		calculate_timeline_position(s, &expected_index, &expected_offset);
		
		/* Get current playback position */
		int64_t current_position = ffmpeg_decoder_get_position(s->decoder);
		
		/* Check if we need to loop back within the same file */
		bool needs_loop_seek = false;
		
		/* Detect loop: when timeline position jumps from near end back to near beginning */
		if (s->loop && expected_index == 0 && s->current_index == 0) {
			/* Check if we've looped: was near end (>60s), now near beginning (<5s) */
			if (s->last_expected_offset > 60000000 && expected_offset < 5000000) {
				needs_loop_seek = true;
				blog(LOG_INFO, "[fmgNICE Video] Loop detected: restarting from %lld ms (was at %lld ms)",
					(long long)(expected_offset / 1000), (long long)(s->last_expected_offset / 1000));
			}
		}
		
		s->last_expected_offset = expected_offset;
		
		/* Check if we should be playing a different file (including looping back to first) */
		if (expected_index != s->current_index || needs_loop_seek) {
			if (expected_index != s->current_index) {
				blog(LOG_INFO, "[fmgNICE Video] Timeline sync: switching from file %zu to %zu (looping: %s)",
					s->current_index, expected_index, s->loop ? "yes" : "no");
			}
			
			s->current_index = expected_index;
			
			/* Load the new file or seek within current file */
			if (s->current_index < s->playlist.num) {
				const char *path = s->playlist.array[s->current_index];
				const char *current_path = ffmpeg_decoder_get_current_path(s->decoder);
				
				if (!current_path || strcmp(current_path, path) != 0) {
					if (ffmpeg_decoder_initialize(s->decoder, path)) {
						ffmpeg_decoder_seek(s->decoder, expected_offset);
						ffmpeg_decoder_play_with_timeline(s->decoder, s->timeline_start_time);
						blog(LOG_INFO, "[fmgNICE Video] Loaded file for timeline sync: %s at %lld ms",
							path, (long long)(expected_offset / 1000));
					}
				} else if (needs_loop_seek) {
					/* Same file but looping - just seek */
					ffmpeg_decoder_seek(s->decoder, expected_offset);
					blog(LOG_INFO, "[fmgNICE Video] Looping within same file: seeking to %lld ms",
						(long long)(expected_offset / 1000));
				}
			}
		}
	}
	
	pthread_mutex_unlock(&s->mutex);
}

/* Timer thread for deferred decoder shutdown */
static void *deactivate_timer_thread(void *data)
{
	struct fvs_source *s = data;
	uint64_t target_time;
	
	pthread_mutex_lock(&s->mutex);
	target_time = s->deactivate_time + DECODER_SHUTDOWN_DELAY_MS;
	pthread_mutex_unlock(&s->mutex);
	
	/* Wait for grace period */
	while (os_gettime_ns() / 1000000 < target_time) {
		/* Check every 100ms if we should cancel */
		os_sleep_ms(100);
		
		pthread_mutex_lock(&s->mutex);
		bool should_cancel = !s->deactivate_timer_active;
		pthread_mutex_unlock(&s->mutex);
		
		if (should_cancel) {
			blog(LOG_INFO, "[fmgNICE Video] Deactivation timer cancelled - source reactivated");
			return NULL;
		}
	}
	
	/* Timer expired - actually stop the decoder */
	blog(LOG_INFO, "[fmgNICE Video] Deactivation timer expired - stopping decoder");
	
	if (s->decoder) {
		ffmpeg_decoder_stop_thread(s->decoder);
	}
	
	pthread_mutex_lock(&s->mutex);
	s->deactivate_timer_active = false;
	pthread_mutex_unlock(&s->mutex);
	
	return NULL;
}

static void fvs_deactivate(void *data)
{
	struct fvs_source *s = data;
	
	if (!s)
		return;
	
	blog(LOG_INFO, "[fmgNICE Video] Source deactivated");
	
	/* Clear frame output first */
	if (s->source) {
		obs_source_output_video(s->source, NULL);
	}
	
	pthread_mutex_lock(&s->mutex);
	
	/* Mark timeline as inactive but keep timeline position */
	s->timeline_active = false;
	
	/* Start deactivation timer */
	s->deactivate_time = os_gettime_ns() / 1000000;
	s->deactivate_timer_active = true;
	
	pthread_mutex_unlock(&s->mutex);
	
	/* Pause decoder output but keep it ready */
	if (s->decoder) {
		ffmpeg_decoder_pause_ready(s->decoder);
		
		/* Start timer thread for deferred shutdown */
		pthread_create(&s->deactivate_timer_thread, NULL, 
		               deactivate_timer_thread, s);
		pthread_detach(s->deactivate_timer_thread);
	}
}

static void fvs_update(void *data, obs_data_t *settings)
{
	struct fvs_source *s = data;
	
	if (!s || !settings)
		return;
	
	pthread_mutex_lock(&s->mutex);
	
	/* Store old playlist to check if it changed */
	size_t old_count = s->playlist.num;
	bool playlist_changed = false;
	
	/* Update playlist */
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	if (array) {
		size_t new_count = obs_data_array_count(array);
		if (new_count != old_count) {
			playlist_changed = true;
		}
		
		/* Free old playlist */
		free_playlist(s);
		
		/* Build new playlist */
		for (size_t i = 0; i < new_count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);
			const char *path = obs_data_get_string(item, "value");
			if (path && *path) {
				char *copy = bstrdup(path);
				da_push_back(s->playlist, &copy);
			}
			obs_data_release(item);
		}
		obs_data_array_release(array);
	} else if (s->playlist.num > 0) {
		/* No array in settings but we have a playlist - clear it */
		playlist_changed = true;
		free_playlist(s);
	}
	
	/* Update settings */
	s->loop = obs_data_get_bool(settings, S_LOOP);
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	s->hw_decoder = (int)obs_data_get_int(settings, S_HW_DECODER);
	s->buffer_size = (int)obs_data_get_int(settings, S_BUFFER_SIZE);
	s->prebuffer_ms = (int)obs_data_get_int(settings, S_PREBUFFER_MS);
	s->sync_mode = (int)obs_data_get_int(settings, S_SYNC_MODE);
	s->sync_offset = (int)obs_data_get_int(settings, S_SYNC_OFFSET);
	s->seek_mode = (int)obs_data_get_int(settings, S_SEEK_MODE);
	s->frame_drop = obs_data_get_bool(settings, S_FRAME_DROP);
	s->audio_buffer_ms = (int)obs_data_get_int(settings, S_AUDIO_BUFFER_MS);
	s->cache_size_mb = (int)obs_data_get_int(settings, S_CACHE_SIZE_MB);
	s->performance_mode = (int)obs_data_get_int(settings, S_PERFORMANCE_MODE);
	s->output_format = (int)obs_data_get_int(settings, S_OUTPUT_FORMAT);
	
	/* Handle timeline initialization and resets */
	if (playlist_changed) {
		blog(LOG_INFO, "[fmgNICE Video] Playlist changed - updating durations");
		
		/* Save playback state */
		bool was_playing = false;
		bool was_active = s->timeline_active;
		int64_t elapsed_time = 0;
		
		if (s->timeline_start_time > 0) {
			/* Calculate how far we've progressed */
			elapsed_time = (os_gettime_ns() / 1000000) - s->timeline_start_time;
			was_playing = s->decoder && ffmpeg_decoder_is_playing(s->decoder);
			blog(LOG_INFO, "[fmgNICE Video] Preserving playback state: elapsed=%lld ms, playing=%s",
				(long long)elapsed_time, was_playing ? "yes" : "no");
		}
		
		/* Update durations for new playlist */
		da_resize(s->durations, 0);
		cache_durations(s);
		
		/* Restore timeline position if we were playing */
		if (was_playing || was_active) {
			/* Keep using the global timeline - don't adjust it */
			blog(LOG_INFO, "[fmgNICE Video] Maintaining global timeline after playlist change");
			
			/* Continue playback if we were playing */
			if (was_playing && s->decoder && s->playlist.num > 0) {
				/* Recalculate position with new playlist */
				size_t new_index = 0;
				int64_t new_offset = 0;
				calculate_timeline_position(s, &new_index, &new_offset);
				
				/* Load appropriate file if needed */
				if (new_index < s->playlist.num) {
					const char *path = s->playlist.array[new_index];
					s->current_index = new_index;
					
					if (ffmpeg_decoder_initialize(s->decoder, path)) {
						ffmpeg_decoder_seek(s->decoder, new_offset);
						ffmpeg_decoder_play_with_timeline(s->decoder, s->timeline_start_time);
						blog(LOG_INFO, "[fmgNICE Video] Resumed playback after playlist change: file %zu at %lld ms",
							new_index, (long long)(new_offset / 1000));
					}
				}
			}
		}
	}
	
	/* Initialize timeline if needed (first time only) */
	if (s->timeline_start_time == 0 && s->playlist.num > 0) {
		/* Use global timeline for synchronization */
		pthread_mutex_lock(&g_timeline_mutex);
		if (g_global_timeline_start == 0) {
			/* First source to be created/updated sets the global timeline */
			g_global_timeline_start = os_gettime_ns() / 1000000;
			blog(LOG_INFO, "[fmgNICE Video] Initialized global timeline at %llu ms", 
				(unsigned long long)g_global_timeline_start);
		}
		/* All sources use the same global timeline */
		s->timeline_start_time = g_global_timeline_start;
		pthread_mutex_unlock(&g_timeline_mutex);
		
		cache_durations(s);
		blog(LOG_INFO, "[fmgNICE Video] Timeline initialized at source creation/update: %llu ms",
			(unsigned long long)s->timeline_start_time);
		
		/* Don't start playback here - wait for activate callback */
		/* This prevents inactive scenes from consuming CPU */
		blog(LOG_INFO, "[fmgNICE Video] Timeline ready, waiting for source activation");
	}
	
	/* Update decoder output format if it exists */
	if (s->decoder) {
		ffmpeg_decoder_set_output_format(s->decoder, s->output_format == 1);
	}
	
	pthread_mutex_unlock(&s->mutex);
}

static void *fvs_create(obs_data_t *settings, obs_source_t *source)
{
	/* No longer limiting to one instance - thread safety issues have been fixed */
	
	struct fvs_source *s = bzalloc(sizeof(struct fvs_source));
	if (!s) {
		return NULL;
	}
	
	s->source = source;
	
	pthread_mutex_init(&s->mutex, NULL);
	
	fvs_update(s, settings);
	
	/* Register with global tracking for emergency cleanup */
	fmgnice_register_source(s);
	
	return s;
}

static void fvs_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_int(settings, S_HW_DECODER, 0); /* Auto */
	obs_data_set_default_int(settings, S_BUFFER_SIZE, 3);
	obs_data_set_default_int(settings, S_PREBUFFER_MS, 200);
	obs_data_set_default_int(settings, S_SYNC_MODE, 0); /* Global */
	obs_data_set_default_int(settings, S_SYNC_OFFSET, 0);
	obs_data_set_default_int(settings, S_SEEK_MODE, 0); /* Accurate */
	obs_data_set_default_bool(settings, S_FRAME_DROP, false);
	obs_data_set_default_int(settings, S_AUDIO_BUFFER_MS, 100);
	obs_data_set_default_int(settings, S_CACHE_SIZE_MB, 256);
	obs_data_set_default_int(settings, S_PERFORMANCE_MODE, 1); /* Balanced */
	obs_data_set_default_int(settings, S_OUTPUT_FORMAT, 0); /* BGRA by default for compatibility */
}

static void fvs_save(void *data, obs_data_t *settings)
{
	struct fvs_source *s = data;
	if (!s)
		return;
	
	/* Save current playback position for resume */
	if (s->decoder) {
		int64_t current_pos = ffmpeg_decoder_get_position(s->decoder);
		obs_data_set_int(settings, "last_position", current_pos);
		obs_data_set_int(settings, "last_index", s->current_index);
	}
	
	/* Save timeline info */
	obs_data_set_int(settings, "timeline_start", s->timeline_start_time);
	obs_data_set_bool(settings, "timeline_active", s->timeline_active);
}

static void fvs_load(void *data, obs_data_t *settings)
{
	struct fvs_source *s = data;
	if (!s)
		return;
	
	/* Restore saved position if available */
	if (obs_data_has_user_value(settings, "last_position")) {
		s->saved_position = obs_data_get_int(settings, "last_position");
		s->saved_index = obs_data_get_int(settings, "last_index");
		/* Position will be restored on next activate */
	}
}

static bool playlist_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(settings);
	
	/* Could update playlist info here */
	return false;
}

static obs_properties_t *fvs_properties(void *data)
{
	struct fvs_source *s = data;
	
	obs_properties_t *props = obs_properties_create();
	
	/* Playlist with enhanced file filter */
	obs_property_t *playlist = obs_properties_add_editable_list(props, S_PLAYLIST, T_PLAYLIST,
		OBS_EDITABLE_LIST_TYPE_FILES,
		"Common Video (*.mp4 *.mkv *.mov *.avi);;All Video Files (*.mp4 *.mkv *.webm *.avi *.mov *.flv *.ts *.m4v *.wmv);;All Files (*)",
		NULL);
	
	/* Add callback to handle playlist changes */
	obs_property_set_modified_callback(playlist, playlist_modified);
	
	/* Playback options group */
	obs_properties_add_bool(props, S_LOOP, T_LOOP);
	
	/* Hardware Decoding Options */
	obs_properties_t *hw_group = obs_properties_create();
	obs_properties_add_group(props, "hardware_group", "Hardware Decoding", OBS_GROUP_NORMAL, hw_group);
	
	obs_properties_add_bool(hw_group, S_HW_DECODE, T_HW_DECODE);
	
	obs_property_t *hw_decoder = obs_properties_add_list(hw_group, S_HW_DECODER, T_HW_DECODER,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(hw_decoder, "Auto", 0);
	obs_property_list_add_int(hw_decoder, "D3D11VA (Windows)", 1);
	obs_property_list_add_int(hw_decoder, "DXVA2 (Windows)", 2);
	obs_property_list_add_int(hw_decoder, "NVIDIA CUDA", 3);
	obs_property_list_add_int(hw_decoder, "Intel QuickSync", 4);
	
	/* Buffer Options */
	obs_properties_t *buffer_group = obs_properties_create();
	obs_properties_add_group(props, "buffer_group", "Buffering", OBS_GROUP_NORMAL, buffer_group);
	
	obs_properties_add_int_slider(buffer_group, S_BUFFER_SIZE, T_BUFFER_SIZE, 2, 10, 1);
	obs_properties_add_int_slider(buffer_group, S_PREBUFFER_MS, T_PREBUFFER_MS, 0, 2000, 50);
	obs_properties_add_int_slider(buffer_group, S_AUDIO_BUFFER_MS, T_AUDIO_BUFFER_MS, 50, 500, 10);
	obs_properties_add_int_slider(buffer_group, S_CACHE_SIZE_MB, T_CACHE_SIZE_MB, 64, 2048, 64);
	
	/* Synchronization Options */
	obs_properties_t *sync_group = obs_properties_create();
	obs_properties_add_group(props, "sync_group", "Synchronization", OBS_GROUP_NORMAL, sync_group);
	
	obs_property_t *sync_mode = obs_properties_add_list(sync_group, S_SYNC_MODE, T_SYNC_MODE,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sync_mode, "Global Timeline (Multi-source sync)", 0);
	obs_property_list_add_int(sync_mode, "Local Timeline (Independent)", 1);
	obs_property_list_add_int(sync_mode, "Disabled (Free-running)", 2);
	
	obs_properties_add_int_slider(sync_group, S_SYNC_OFFSET, T_SYNC_OFFSET, -5000, 5000, 10);
	
	/* Performance Options */
	obs_properties_t *perf_group = obs_properties_create();
	obs_properties_add_group(props, "perf_group", "Performance", OBS_GROUP_NORMAL, perf_group);
	
	obs_property_t *perf_mode = obs_properties_add_list(perf_group, S_PERFORMANCE_MODE, T_PERFORMANCE_MODE,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(perf_mode, "Quality (Best quality, higher CPU)", 0);
	obs_property_list_add_int(perf_mode, "Balanced (Recommended)", 1);
	obs_property_list_add_int(perf_mode, "Performance (Lower quality, less CPU)", 2);
	
	obs_property_t *seek_mode = obs_properties_add_list(perf_group, S_SEEK_MODE, T_SEEK_MODE,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(seek_mode, "Accurate (Frame-perfect)", 0);
	obs_property_list_add_int(seek_mode, "Fast (Nearest keyframe)", 1);
	
	obs_property_t *output_format = obs_properties_add_list(perf_group, S_OUTPUT_FORMAT, T_OUTPUT_FORMAT,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(output_format, "BGRA (Compatible, slower conversion)", 0);
	obs_property_list_add_int(output_format, "NV12 (Native GPU format, no conversion)", 1);
	
	obs_properties_add_bool(perf_group, S_FRAME_DROP, T_FRAME_DROP);
	
	/* Information text */
	if (s && s->playlist.num > 0) {
		char info[256];
		int64_t total_duration = 0;
		for (size_t i = 0; i < s->durations.num; i++) {
			total_duration += s->durations.array[i];
		}
		snprintf(info, sizeof(info), "Playlist: %zu files, Total duration: %02d:%02d:%02d", 
			s->playlist.num,
			(int)(total_duration / 3600000000),
			(int)((total_duration / 60000000) % 60),
			(int)((total_duration / 1000000) % 60));
		obs_properties_add_text(props, "info", info, OBS_TEXT_INFO);
	}
	
	return props;
}

struct obs_source_info fmgnice_video_source = {
	.id = "fmgnice_video_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.get_name = fvs_get_name,
	.create = fvs_create,
	.destroy = fvs_destroy,
	.update = fvs_update,
	.get_defaults = fvs_defaults,
	.get_properties = fvs_properties,
	.save = fvs_save,
	.load = fvs_load,
	.activate = fvs_activate,
	.deactivate = fvs_deactivate,
	.video_tick = fvs_video_tick,
};