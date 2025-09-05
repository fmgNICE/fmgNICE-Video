/*
 * SIMD-optimized color conversion functions
 * Uses SSE4.2 and AVX2 for fast YUV to BGRA conversion
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _MSC_VER
#include <intrin.h>
#include <immintrin.h>
#else
#include <x86intrin.h>
#endif

/* Check CPU capabilities */
bool simd_check_sse42(void);
bool simd_check_avx2(void);

/* YUV420 to BGRA conversion functions */
void yuv420_to_bgra_sse42(
    const uint8_t* y, int y_stride,
    const uint8_t* u, int u_stride,
    const uint8_t* v, int v_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

void yuv420_to_bgra_avx2(
    const uint8_t* y, int y_stride,
    const uint8_t* u, int u_stride,
    const uint8_t* v, int v_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

/* NV12 to BGRA conversion functions */
void nv12_to_bgra_sse42(
    const uint8_t* y, int y_stride,
    const uint8_t* uv, int uv_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

void nv12_to_bgra_avx2(
    const uint8_t* y, int y_stride,
    const uint8_t* uv, int uv_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

/* Auto-select best conversion based on CPU */
typedef void (*yuv_convert_func)(
    const uint8_t* y, int y_stride,
    const uint8_t* u, int u_stride,
    const uint8_t* v, int v_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

yuv_convert_func simd_get_best_yuv420_converter(void);

typedef void (*nv12_convert_func)(
    const uint8_t* y, int y_stride,
    const uint8_t* uv, int uv_stride,
    uint8_t* bgra, int bgra_stride,
    int width, int height);

nv12_convert_func simd_get_best_nv12_converter(void);