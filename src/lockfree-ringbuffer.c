/*
 * Lock-free ring buffer implementation
 * Achieves zero contention between producer and consumer threads
 */

#include "lockfree-ringbuffer.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <string.h>

/* Atomic operations for cross-platform compatibility */
#ifdef _MSC_VER
#define atomic_store_32(ptr, val) InterlockedExchange((volatile LONG*)(ptr), (val))
#define atomic_load_32(ptr) InterlockedCompareExchange((volatile LONG*)(ptr), 0, 0)
#define atomic_store_64(ptr, val) InterlockedExchange64((volatile LONGLONG*)(ptr), (val))
#define atomic_load_64(ptr) InterlockedCompareExchange64((volatile LONGLONG*)(ptr), 0, 0)
#define atomic_fetch_add_64(ptr, val) InterlockedAdd64((volatile LONGLONG*)(ptr), (val))
#define atomic_compare_exchange_32(ptr, expected, desired) \
	(InterlockedCompareExchange((volatile LONG*)(ptr), (desired), *(expected)) == *(expected))
#else
#define atomic_store_32(ptr, val) atomic_store(ptr, val)
#define atomic_load_32(ptr) atomic_load(ptr)
#define atomic_store_64(ptr, val) atomic_store(ptr, val)
#define atomic_load_64(ptr) atomic_load(ptr)
#define atomic_fetch_add_64(ptr, val) atomic_fetch_add(ptr, val)
#define atomic_compare_exchange_32(ptr, expected, desired) \
	atomic_compare_exchange_strong(ptr, expected, desired)
#endif

#define blog(level, format, ...) \
	blog(level, "[Lock-Free RB] " format, ##__VA_ARGS__)

/* State transitions for slots */
enum slot_state {
	SLOT_EMPTY = 0,
	SLOT_WRITING = 1,
	SLOT_READY = 2,
	SLOT_READING = 3
};

void lockfree_ringbuffer_init(struct lockfree_ringbuffer *rb)
{
	if (!rb) return;
	
	memset(rb, 0, sizeof(*rb));
	
	/* Initialize all slots to empty */
	for (int i = 0; i < RING_BUFFER_SIZE; i++) {
		atomic_store_32(&rb->slots[i].state, SLOT_EMPTY);
		rb->slots[i].frame = NULL;
		rb->slots[i].timestamp = 0;
	}
	
	atomic_store_32(&rb->producer.write_pos, 0);
	atomic_store_32(&rb->consumer.read_pos, 0);
	atomic_store_64(&rb->frames_written, 0);
	atomic_store_64(&rb->frames_read, 0);
	atomic_store_64(&rb->write_failures, 0);
	atomic_store_64(&rb->read_failures, 0);
	
	blog(LOG_INFO, "Initialized lock-free ring buffer with %d slots", RING_BUFFER_SIZE);
}

void lockfree_ringbuffer_destroy(struct lockfree_ringbuffer *rb)
{
	if (!rb) return;
	
	/* Log final statistics */
	lockfree_ringbuffer_log_stats(rb);
	
	/* Free any remaining frames */
	for (int i = 0; i < RING_BUFFER_SIZE; i++) {
		if (rb->slots[i].frame) {
			av_frame_free(&rb->slots[i].frame);
		}
	}
	
	memset(rb, 0, sizeof(*rb));
}

bool lockfree_ringbuffer_write_begin(struct lockfree_ringbuffer *rb, uint32_t *slot)
{
	if (!rb || !slot) return false;
	
	uint32_t write_pos = atomic_load_32(&rb->producer.write_pos);
	uint32_t next_pos = (write_pos + 1) & (RING_BUFFER_SIZE - 1);
	
	/* Check if slot is available */
	uint32_t expected = SLOT_EMPTY;
	if (!atomic_compare_exchange_32(&rb->slots[write_pos].state, &expected, SLOT_WRITING)) {
		/* Slot not empty, buffer is full */
		atomic_fetch_add_64(&rb->write_failures, 1);
		return false;
	}
	
	/* Successfully claimed slot for writing */
	*slot = write_pos;
	
	/* Advance write position for next writer */
	atomic_store_32(&rb->producer.write_pos, next_pos);
	
	return true;
}

void lockfree_ringbuffer_write_commit(struct lockfree_ringbuffer *rb, uint32_t slot, AVFrame *frame, uint64_t timestamp)
{
	if (!rb || slot >= RING_BUFFER_SIZE) return;
	
	/* Store frame data */
	rb->slots[slot].frame = frame;
	rb->slots[slot].timestamp = timestamp;
	
	/* Memory barrier to ensure frame data is visible before state change */
	memory_barrier_release();
	
	/* Mark slot as ready for reading */
	atomic_store_32(&rb->slots[slot].state, SLOT_READY);
	
	atomic_fetch_add_64(&rb->frames_written, 1);
}

void lockfree_ringbuffer_write_abort(struct lockfree_ringbuffer *rb, uint32_t slot)
{
	if (!rb || slot >= RING_BUFFER_SIZE) return;
	
	/* Return slot to empty state */
	atomic_store_32(&rb->slots[slot].state, SLOT_EMPTY);
}

bool lockfree_ringbuffer_read_begin(struct lockfree_ringbuffer *rb, uint32_t *slot, AVFrame **frame, uint64_t *timestamp)
{
	if (!rb || !slot || !frame || !timestamp) return false;
	
	uint32_t read_pos = atomic_load_32(&rb->consumer.read_pos);
	uint32_t next_pos = (read_pos + 1) & (RING_BUFFER_SIZE - 1);
	
	/* Check if slot has data ready */
	uint32_t expected = SLOT_READY;
	if (!atomic_compare_exchange_32(&rb->slots[read_pos].state, &expected, SLOT_READING)) {
		/* No data ready */
		atomic_fetch_add_64(&rb->read_failures, 1);
		return false;
	}
	
	/* Memory barrier to ensure we see the frame data */
	memory_barrier_acquire();
	
	/* Successfully claimed slot for reading */
	*slot = read_pos;
	*frame = rb->slots[read_pos].frame;
	*timestamp = rb->slots[read_pos].timestamp;
	
	/* Clear slot data */
	rb->slots[read_pos].frame = NULL;
	rb->slots[read_pos].timestamp = 0;
	
	/* Advance read position for next reader */
	atomic_store_32(&rb->consumer.read_pos, next_pos);
	
	atomic_fetch_add_64(&rb->frames_read, 1);
	
	return true;
}

void lockfree_ringbuffer_read_complete(struct lockfree_ringbuffer *rb, uint32_t slot)
{
	if (!rb || slot >= RING_BUFFER_SIZE) return;
	
	/* Return slot to empty state */
	atomic_store_32(&rb->slots[slot].state, SLOT_EMPTY);
}

uint32_t lockfree_ringbuffer_available_slots(struct lockfree_ringbuffer *rb)
{
	if (!rb) return 0;
	
	uint32_t count = 0;
	for (int i = 0; i < RING_BUFFER_SIZE; i++) {
		if (atomic_load_32(&rb->slots[i].state) == SLOT_EMPTY) {
			count++;
		}
	}
	
	return count;
}

void lockfree_ringbuffer_log_stats(struct lockfree_ringbuffer *rb)
{
	if (!rb) return;
	
	uint64_t written = atomic_load_64(&rb->frames_written);
	uint64_t read = atomic_load_64(&rb->frames_read);
	uint64_t write_fails = atomic_load_64(&rb->write_failures);
	uint64_t read_fails = atomic_load_64(&rb->read_failures);
	
	blog(LOG_INFO, "Ring buffer stats: written=%llu, read=%llu, write_fails=%llu, read_fails=%llu",
		(unsigned long long)written,
		(unsigned long long)read,
		(unsigned long long)write_fails,
		(unsigned long long)read_fails);
	
	if (write_fails > 0) {
		float fail_rate = (float)write_fails / (written + write_fails) * 100.0f;
		blog(LOG_INFO, "Write failure rate: %.2f%% (buffer full)", fail_rate);
	}
	
	if (read_fails > 0) {
		float fail_rate = (float)read_fails / (read + read_fails) * 100.0f;
		blog(LOG_INFO, "Read failure rate: %.2f%% (buffer empty)", fail_rate);
	}
}