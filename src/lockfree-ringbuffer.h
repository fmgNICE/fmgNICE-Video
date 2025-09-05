/*
 * Lock-free ring buffer for zero-contention frame passing
 * Uses atomic operations to eliminate mutex overhead
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libavutil/frame.h>

/* Use Windows atomics for MSVC */
#ifdef _MSC_VER
#include <windows.h>
#else
#include <stdatomic.h>
#endif

#define RING_BUFFER_SIZE 4  /* Power of 2 for efficient modulo */
#define CACHE_LINE_SIZE 64   /* Typical x86_64 cache line */

/* Align to cache line to prevent false sharing */
#define CACHE_ALIGNED __declspec(align(CACHE_LINE_SIZE))

/* Atomic types for cross-platform compatibility */
#ifdef _MSC_VER
typedef volatile LONG atomic_uint32_t;
typedef volatile LONGLONG atomic_uint64_t;
#else
typedef _Atomic(uint32_t) atomic_uint32_t;
typedef _Atomic(uint64_t) atomic_uint64_t;
#endif

struct lockfree_frame_slot {
	AVFrame *frame;
	uint64_t timestamp;
	atomic_uint32_t state;  /* 0=empty, 1=writing, 2=ready, 3=reading */
	char padding[CACHE_LINE_SIZE - sizeof(AVFrame*) - sizeof(uint64_t) - sizeof(atomic_uint32_t)];
};

struct lockfree_ringbuffer {
	/* Producer side - cache line aligned */
	CACHE_ALIGNED struct {
		atomic_uint32_t write_pos;
		char padding[CACHE_LINE_SIZE - sizeof(atomic_uint32_t)];
	} producer;
	
	/* Consumer side - cache line aligned */
	CACHE_ALIGNED struct {
		atomic_uint32_t read_pos;
		char padding[CACHE_LINE_SIZE - sizeof(atomic_uint32_t)];
	} consumer;
	
	/* Shared data - each slot cache line aligned */
	CACHE_ALIGNED struct lockfree_frame_slot slots[RING_BUFFER_SIZE];
	
	/* Statistics */
	atomic_uint64_t frames_written;
	atomic_uint64_t frames_read;
	atomic_uint64_t write_failures;
	atomic_uint64_t read_failures;
};

/* Initialize ring buffer */
void lockfree_ringbuffer_init(struct lockfree_ringbuffer *rb);

/* Cleanup ring buffer */
void lockfree_ringbuffer_destroy(struct lockfree_ringbuffer *rb);

/* Producer operations */
bool lockfree_ringbuffer_write_begin(struct lockfree_ringbuffer *rb, uint32_t *slot);
void lockfree_ringbuffer_write_commit(struct lockfree_ringbuffer *rb, uint32_t slot, AVFrame *frame, uint64_t timestamp);
void lockfree_ringbuffer_write_abort(struct lockfree_ringbuffer *rb, uint32_t slot);

/* Consumer operations */
bool lockfree_ringbuffer_read_begin(struct lockfree_ringbuffer *rb, uint32_t *slot, AVFrame **frame, uint64_t *timestamp);
void lockfree_ringbuffer_read_complete(struct lockfree_ringbuffer *rb, uint32_t slot);

/* Utilities */
uint32_t lockfree_ringbuffer_available_slots(struct lockfree_ringbuffer *rb);
void lockfree_ringbuffer_log_stats(struct lockfree_ringbuffer *rb);

/* Memory barrier helpers for different compilers */
#ifdef _MSC_VER
#include <intrin.h>
#define memory_barrier_acquire() _ReadBarrier()
#define memory_barrier_release() _WriteBarrier()
#define memory_barrier_full() _ReadWriteBarrier()
#else
#define memory_barrier_acquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define memory_barrier_release() __atomic_thread_fence(__ATOMIC_RELEASE)
#define memory_barrier_full() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif