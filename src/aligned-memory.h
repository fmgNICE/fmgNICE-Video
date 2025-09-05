/*
 * Aligned memory allocation for optimal SIMD performance
 * Ensures all buffers are aligned to 32-byte boundaries for AVX2
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <malloc.h>
#include <immintrin.h>  /* For AVX2 intrinsics */
#endif

/* Alignment requirements */
#define SIMD_ALIGNMENT 32      /* 32-byte alignment for AVX2 */
#define CACHE_ALIGNMENT 64     /* 64-byte alignment for cache lines */

/* Aligned allocation functions */
static inline void* aligned_alloc_simd(size_t size)
{
#ifdef _MSC_VER
	return _aligned_malloc(size, SIMD_ALIGNMENT);
#else
	void *ptr = NULL;
	if (posix_memalign(&ptr, SIMD_ALIGNMENT, size) != 0)
		return NULL;
	return ptr;
#endif
}

static inline void* aligned_alloc_cache(size_t size)
{
#ifdef _MSC_VER
	return _aligned_malloc(size, CACHE_ALIGNMENT);
#else
	void *ptr = NULL;
	if (posix_memalign(&ptr, CACHE_ALIGNMENT, size) != 0)
		return NULL;
	return ptr;
#endif
}

static inline void aligned_free(void *ptr)
{
	if (!ptr)
		return;
		
#ifdef _MSC_VER
	_aligned_free(ptr);
#else
	free(ptr);
#endif
}

/* Check if pointer is aligned */
static inline int is_aligned(const void *ptr, size_t alignment)
{
	return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

/* Align size to boundary */
static inline size_t align_size(size_t size, size_t alignment)
{
	return (size + alignment - 1) & ~(alignment - 1);
}

/* Aligned memory copy optimized for large buffers */
static inline void aligned_memcpy(void *dst, const void *src, size_t size)
{
	/* Check if both pointers are 32-byte aligned for AVX2 */
	if (is_aligned(dst, 32) && is_aligned(src, 32) && size >= 64) {
#ifdef _MSC_VER
		/* Use AVX2 intrinsics for aligned copy */
		__m256i *d = (__m256i*)dst;
		const __m256i *s = (const __m256i*)src;
		size_t chunks = size / 32;
		
		for (size_t i = 0; i < chunks; i++) {
			_mm256_store_si256(d + i, _mm256_load_si256(s + i));
		}
		
		/* Copy remaining bytes */
		size_t remaining = size & 31;
		if (remaining) {
			memcpy((uint8_t*)dst + (chunks * 32), (const uint8_t*)src + (chunks * 32), remaining);
		}
#else
		memcpy(dst, src, size);
#endif
	} else {
		/* Fallback to standard memcpy */
		memcpy(dst, src, size);
	}
}

/* Prefetch memory for reading */
static inline void prefetch_read(const void *ptr)
{
#ifdef _MSC_VER
	_mm_prefetch((const char*)ptr, _MM_HINT_T0);
#else
	__builtin_prefetch(ptr, 0, 3);
#endif
}

/* Prefetch memory for writing */
static inline void prefetch_write(void *ptr)
{
#ifdef _MSC_VER
	_mm_prefetch((char*)ptr, _MM_HINT_T0);
#else
	__builtin_prefetch(ptr, 1, 3);
#endif
}