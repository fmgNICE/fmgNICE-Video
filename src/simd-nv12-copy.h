#pragma once

#include <stdint.h>
#include <string.h>
#include <immintrin.h>

/* SIMD-optimized NV12 plane copy functions */

static inline void copy_nv12_plane_avx2(uint8_t *dst, const uint8_t *src, 
                                        int dst_stride, int src_stride,
                                        int width, int height)
{
	/* If strides match and are aligned, we can do a single bulk copy */
	if (dst_stride == src_stride && (width == dst_stride)) {
		size_t total_size = (size_t)dst_stride * height;
		
		/* Use AVX2 for bulk copy if size is large enough */
		if (total_size >= 64) {
			size_t aligned_size = total_size & ~63ULL;
			size_t i;
			
			/* Copy 64 bytes at a time using AVX2 */
			for (i = 0; i < aligned_size; i += 64) {
				__m256i v0 = _mm256_loadu_si256((const __m256i*)(src + i));
				__m256i v1 = _mm256_loadu_si256((const __m256i*)(src + i + 32));
				_mm256_storeu_si256((__m256i*)(dst + i), v0);
				_mm256_storeu_si256((__m256i*)(dst + i + 32), v1);
			}
			
			/* Copy remaining bytes */
			if (i < total_size) {
				memcpy(dst + i, src + i, total_size - i);
			}
		} else {
			memcpy(dst, src, total_size);
		}
	} else {
		/* Line-by-line copy with AVX2 optimization */
		for (int y = 0; y < height; y++) {
			const uint8_t *src_line = src + y * src_stride;
			uint8_t *dst_line = dst + y * dst_stride;
			
			if (width >= 32) {
				int x;
				/* Copy 32 bytes at a time using AVX2 */
				for (x = 0; x <= width - 32; x += 32) {
					__m256i v = _mm256_loadu_si256((const __m256i*)(src_line + x));
					_mm256_storeu_si256((__m256i*)(dst_line + x), v);
				}
				/* Copy remaining bytes */
				if (x < width) {
					memcpy(dst_line + x, src_line + x, width - x);
				}
			} else {
				memcpy(dst_line, src_line, width);
			}
		}
	}
}

static inline void copy_nv12_planes_avx2(uint8_t *dst_y, uint8_t *dst_uv,
                                         const uint8_t *src_y, const uint8_t *src_uv,
                                         int dst_y_stride, int dst_uv_stride,
                                         int src_y_stride, int src_uv_stride,
                                         int width, int height)
{
	/* Copy Y plane */
	copy_nv12_plane_avx2(dst_y, src_y, dst_y_stride, src_y_stride, width, height);
	
	/* Copy UV plane (half height) */
	copy_nv12_plane_avx2(dst_uv, src_uv, dst_uv_stride, src_uv_stride, width, height / 2);
}

/* SSE2 fallback for older CPUs */
static inline void copy_nv12_plane_sse2(uint8_t *dst, const uint8_t *src,
                                        int dst_stride, int src_stride,
                                        int width, int height)
{
	if (dst_stride == src_stride && (width == dst_stride)) {
		memcpy(dst, src, (size_t)dst_stride * height);
	} else {
		for (int y = 0; y < height; y++) {
			const uint8_t *src_line = src + y * src_stride;
			uint8_t *dst_line = dst + y * dst_stride;
			
			if (width >= 16) {
				int x;
				/* Copy 16 bytes at a time using SSE2 */
				for (x = 0; x <= width - 16; x += 16) {
					__m128i v = _mm_loadu_si128((const __m128i*)(src_line + x));
					_mm_storeu_si128((__m128i*)(dst_line + x), v);
				}
				/* Copy remaining bytes */
				if (x < width) {
					memcpy(dst_line + x, src_line + x, width - x);
				}
			} else {
				memcpy(dst_line, src_line, width);
			}
		}
	}
}

/* CPU feature detection and function selection */
#ifdef _WIN32
#include <intrin.h>
static inline int has_avx2(void)
{
	int cpu_info[4];
	__cpuid(cpu_info, 7);
	return (cpu_info[1] & (1 << 5)) != 0;  /* AVX2 is bit 5 of EBX */
}
#else
static inline int has_avx2(void)
{
	return __builtin_cpu_supports("avx2");
}
#endif

/* Main copy function that selects best implementation */
static inline void copy_nv12_optimized(uint8_t *dst_y, uint8_t *dst_uv,
                                       const uint8_t *src_y, const uint8_t *src_uv,
                                       int dst_y_stride, int dst_uv_stride,
                                       int src_y_stride, int src_uv_stride,
                                       int width, int height)
{
	static int use_avx2 = -1;
	
	/* Check CPU capabilities once */
	if (use_avx2 == -1) {
		use_avx2 = has_avx2();
	}
	
	if (use_avx2) {
		copy_nv12_planes_avx2(dst_y, dst_uv, src_y, src_uv,
		                     dst_y_stride, dst_uv_stride,
		                     src_y_stride, src_uv_stride,
		                     width, height);
	} else {
		/* Fallback to SSE2/memcpy */
		copy_nv12_plane_sse2(dst_y, src_y, dst_y_stride, src_y_stride, width, height);
		copy_nv12_plane_sse2(dst_uv, src_uv, dst_uv_stride, src_uv_stride, width, height / 2);
	}
}