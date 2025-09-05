/*
 * Zero-copy GPU pipeline for direct texture sharing with OBS
 * Eliminates GPU->CPU->GPU memory copies for hardware decoded frames
 */

#pragma once

#include <obs-module.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#endif

struct gpu_zero_copy_ctx {
	/* D3D11 device contexts */
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	
	/* Shared texture for zero-copy */
	ID3D11Texture2D *shared_texture;
	HANDLE shared_handle;
	
	/* OBS graphics context */
	gs_device_t *obs_device;
	gs_texture_t *obs_texture;
	
	/* Statistics */
	uint64_t frames_zero_copied;
	uint64_t frames_fallback;
};

/* Initialize zero-copy context */
bool gpu_zero_copy_init(struct gpu_zero_copy_ctx *ctx, obs_source_t *source);

/* Cleanup zero-copy context */
void gpu_zero_copy_cleanup(struct gpu_zero_copy_ctx *ctx);

/* Check if frame can be zero-copied */
bool gpu_zero_copy_can_handle(AVFrame *frame);

/* Perform zero-copy frame delivery to OBS */
bool gpu_zero_copy_deliver_frame(
	struct gpu_zero_copy_ctx *ctx,
	AVFrame *hw_frame,
	obs_source_t *source,
	uint64_t timestamp);

/* Get D3D11 texture from AVFrame */
ID3D11Texture2D* gpu_zero_copy_get_d3d11_texture(AVFrame *frame);

/* Create shared texture for OBS */
bool gpu_zero_copy_create_shared_texture(
	struct gpu_zero_copy_ctx *ctx,
	int width, int height,
	DXGI_FORMAT format);

/* Map hardware frame directly to OBS */
bool gpu_zero_copy_map_to_obs(
	struct gpu_zero_copy_ctx *ctx,
	ID3D11Texture2D *src_texture,
	obs_source_t *source);

/* Log zero-copy statistics */
void gpu_zero_copy_log_stats(struct gpu_zero_copy_ctx *ctx);