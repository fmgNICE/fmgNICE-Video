/*
 * Frame caching system for efficient looping and seek optimization
 * Caches decoded frames to avoid redundant decoding
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>
#include <util/threading.h>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <stdatomic.h>
#endif

/* Cache configuration */
#define FRAME_CACHE_SIZE 30        /* Cache 1 second at 30fps */
#define CACHE_LINE_SIZE 64          /* CPU cache line size */
#define CACHE_ALIGNED __declspec(align(CACHE_LINE_SIZE))

/* Atomic types */
#ifdef _MSC_VER
typedef volatile LONG atomic_uint32_t;
typedef volatile LONGLONG atomic_uint64_t;
#else
typedef _Atomic(uint32_t) atomic_uint32_t;
typedef _Atomic(uint64_t) atomic_uint64_t;
#endif

struct cached_frame {
	AVFrame *frame;              /* Cached decoded frame */
	int64_t pts;                 /* Presentation timestamp */
	uint32_t ref_count;          /* Reference count for safe access */
	atomic_uint32_t state;       /* 0=empty, 1=loading, 2=ready */
	
	/* Pre-converted BGRA data */
	uint8_t *bgra_data;
	uint32_t bgra_linesize[4];
	uint32_t width;
	uint32_t height;
	
	/* LRU tracking */
	uint64_t last_access_time;
	uint32_t access_count;
};

struct frame_cache {
	/* Cache entries */
	CACHE_ALIGNED struct cached_frame entries[FRAME_CACHE_SIZE];
	
	/* Cache management */
	atomic_uint32_t current_gen;    /* Generation counter for cache invalidation */
	pthread_mutex_t lock;            /* Protects LRU and insertion */
	
	/* Statistics */
	atomic_uint64_t hits;
	atomic_uint64_t misses;
	atomic_uint64_t evictions;
	atomic_uint64_t insertions;
	
	/* Configuration */
	bool enabled;
	bool cache_converted_frames;    /* Cache BGRA converted frames */
	uint32_t max_entries;
};

/* Initialize frame cache */
void frame_cache_init(struct frame_cache *cache, bool enable_converted_cache);

/* Cleanup frame cache */
void frame_cache_destroy(struct frame_cache *cache);

/* Lookup frame in cache by PTS */
struct cached_frame* frame_cache_get(struct frame_cache *cache, int64_t pts);

/* Add frame to cache */
bool frame_cache_put(struct frame_cache *cache, AVFrame *frame, int64_t pts,
                     uint8_t *bgra_data, uint32_t *bgra_linesize,
                     uint32_t width, uint32_t height);

/* Invalidate cache (e.g., after seek) */
void frame_cache_invalidate(struct frame_cache *cache);

/* Prefetch frames for smooth playback */
void frame_cache_prefetch_range(struct frame_cache *cache, 
                                int64_t start_pts, int64_t end_pts);

/* Release reference to cached frame */
void frame_cache_release(struct frame_cache *cache, struct cached_frame *entry);

/* Get cache statistics */
void frame_cache_get_stats(struct frame_cache *cache, 
                           uint64_t *hits, uint64_t *misses,
                           uint64_t *evictions, float *hit_rate);

/* Log cache performance */
void frame_cache_log_stats(struct frame_cache *cache);