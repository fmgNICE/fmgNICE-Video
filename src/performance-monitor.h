/*
 * Performance monitoring and diagnostics
 * Tracks CPU usage, frame timing, and identifies bottlenecks
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <obs-module.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

typedef struct {
	/* Frame timing */
	uint64_t frame_start_time;
	uint64_t decode_time_ns;
	uint64_t convert_time_ns;
	uint64_t render_time_ns;
	
	/* Rolling averages (last 100 frames) */
	uint64_t avg_decode_time;
	uint64_t avg_convert_time;
	uint64_t avg_render_time;
	
	/* Performance counters */
	uint32_t frames_processed;
	uint32_t frames_dropped;
	uint32_t frames_late;
	
	/* Video frame rate for accurate late detection */
	double fps;
	uint64_t frame_duration_ns;
	
	/* CPU usage */
	float cpu_usage_percent;
	float process_cpu_percent;
	
	/* Memory usage */
	size_t memory_used_mb;
	size_t peak_memory_mb;
	
	/* Bottleneck detection */
	bool is_cpu_bound;
	bool is_memory_bound;
	bool is_decoder_bound;
	
	/* Last log time */
	uint64_t last_report_time;
} perf_monitor_t;

/* Forward declaration for processor count */
static inline int GetSystemInfo_ProcessorCount(void);

static inline void perf_monitor_init(perf_monitor_t *monitor)
{
	if (!monitor) return;
	memset(monitor, 0, sizeof(*monitor));
	monitor->last_report_time = os_gettime_ns();
	/* Default to 30fps if not set */
	monitor->fps = 30.0;
	monitor->frame_duration_ns = 33333333; /* 33.33ms */
}

static inline void perf_monitor_set_fps(perf_monitor_t *monitor, double fps)
{
	if (!monitor || fps <= 0) return;
	monitor->fps = fps;
	monitor->frame_duration_ns = (uint64_t)(1000000000.0 / fps);
}

static inline void perf_monitor_frame_start(perf_monitor_t *monitor)
{
	if (!monitor) return;
	monitor->frame_start_time = os_gettime_ns();
}

static inline void perf_monitor_decode_complete(perf_monitor_t *monitor)
{
	if (!monitor) return;
	uint64_t now = os_gettime_ns();
	monitor->decode_time_ns = now - monitor->frame_start_time;
	
	/* Update rolling average */
	monitor->avg_decode_time = (monitor->avg_decode_time * 99 + monitor->decode_time_ns) / 100;
}

static inline void perf_monitor_convert_complete(perf_monitor_t *monitor)
{
	if (!monitor) return;
	uint64_t now = os_gettime_ns();
	monitor->convert_time_ns = now - monitor->frame_start_time - monitor->decode_time_ns;
	
	/* Update rolling average */
	monitor->avg_convert_time = (monitor->avg_convert_time * 99 + monitor->convert_time_ns) / 100;
}

static inline void perf_monitor_frame_complete(perf_monitor_t *monitor)
{
	if (!monitor) return;
	uint64_t now = os_gettime_ns();
	monitor->render_time_ns = now - monitor->frame_start_time;
	monitor->frames_processed++;
	
	/* Update rolling average */
	monitor->avg_render_time = (monitor->avg_render_time * 99 + monitor->render_time_ns) / 100;
	
	/* Check if frame was late based on actual FPS */
	/* Allow 10% tolerance for timing variations */
	uint64_t late_threshold = monitor->frame_duration_ns + (monitor->frame_duration_ns / 10);
	if (monitor->render_time_ns > late_threshold) {
		monitor->frames_late++;
	}
	
	/* Detect bottlenecks - scale with FPS */
	/* Decoder should use less than 25% of frame time */
	monitor->is_decoder_bound = monitor->avg_decode_time > (monitor->frame_duration_ns / 4);
	/* Total time should be less than 80% of frame time for smooth playback */
	monitor->is_cpu_bound = monitor->avg_render_time > (monitor->frame_duration_ns * 4 / 5);
}

static inline void perf_monitor_update_cpu_usage(perf_monitor_t *monitor)
{
#ifdef _WIN32
	static ULARGE_INTEGER lastCPU, lastSysCPU, lastUserCPU;
	static HANDLE self = NULL;
	
	if (!self) {
		self = GetCurrentProcess();
	}
	
	FILETIME ftime, fsys, fuser;
	ULARGE_INTEGER now, sys, user;
	
	GetSystemTimeAsFileTime(&ftime);
	memcpy(&now, &ftime, sizeof(FILETIME));
	
	GetProcessTimes(self, &ftime, &ftime, &fsys, &fuser);
	memcpy(&sys, &fsys, sizeof(FILETIME));
	memcpy(&user, &fuser, sizeof(FILETIME));
	
	if (lastCPU.QuadPart > 0) {
		float percent = (float)((sys.QuadPart - lastSysCPU.QuadPart) + 
		                        (user.QuadPart - lastUserCPU.QuadPart));
		percent /= (now.QuadPart - lastCPU.QuadPart);
		percent /= GetSystemInfo_ProcessorCount();
		monitor->process_cpu_percent = percent * 100.0f;
	}
	
	lastCPU = now;
	lastUserCPU = user;
	lastSysCPU = sys;
	
	/* Get memory usage */
	PROCESS_MEMORY_COUNTERS_EX pmc;
	if (GetProcessMemoryInfo(self, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
		monitor->memory_used_mb = pmc.WorkingSetSize / (1024 * 1024);
		monitor->peak_memory_mb = pmc.PeakWorkingSetSize / (1024 * 1024);
		
		/* Detect memory pressure */
		MEMORYSTATUSEX memInfo;
		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
		GlobalMemoryStatusEx(&memInfo);
		
		DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
		DWORDLONG physMemUsed = totalPhysMem - memInfo.ullAvailPhys;
		monitor->cpu_usage_percent = (float)(physMemUsed * 100) / totalPhysMem;
		
		/* Check if we're using too much memory */
		monitor->is_memory_bound = (monitor->memory_used_mb > 2048) || 
		                           (monitor->cpu_usage_percent > 90.0f);
	}
#endif
}

static inline int GetSystemInfo_ProcessorCount(void)
{
#ifdef _WIN32
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	return sysinfo.dwNumberOfProcessors;
#else
	return 1;
#endif
}

static inline void perf_monitor_report(perf_monitor_t *monitor, const char *source_name)
{
	if (!monitor) return;
	
	uint64_t now = os_gettime_ns();
	uint64_t elapsed = now - monitor->last_report_time;
	
	/* Report every 10 seconds */
	if (elapsed < 10000000000ULL) return;
	
	monitor->last_report_time = now;
	perf_monitor_update_cpu_usage(monitor);
	
	blog(LOG_INFO, "[%s Performance] Frames: %u processed, %u late (%.1f%%), %u dropped",
		source_name,
		monitor->frames_processed,
		monitor->frames_late,
		monitor->frames_processed > 0 ? (float)monitor->frames_late * 100.0f / monitor->frames_processed : 0.0f,
		monitor->frames_dropped);
	
	blog(LOG_INFO, "[%s Timing] Avg: decode=%.1fms, convert=%.1fms, total=%.1fms",
		source_name,
		monitor->avg_decode_time / 1000000.0,
		monitor->avg_convert_time / 1000000.0,
		monitor->avg_render_time / 1000000.0);
	
	blog(LOG_INFO, "[%s Resources] CPU: %.1f%% process, Memory: %zuMB (peak: %zuMB)",
		source_name,
		monitor->process_cpu_percent,
		monitor->memory_used_mb,
		monitor->peak_memory_mb);
	
	if (monitor->is_decoder_bound) {
		blog(LOG_WARNING, "[%s] Performance bottleneck: DECODER BOUND - consider using hardware decoding", source_name);
	}
	if (monitor->is_cpu_bound) {
		blog(LOG_WARNING, "[%s] Performance bottleneck: CPU BOUND - reduce resolution or framerate", source_name);
	}
	if (monitor->is_memory_bound) {
		blog(LOG_WARNING, "[%s] Performance bottleneck: MEMORY BOUND - close other applications", source_name);
	}
	
	/* Reset frame counters */
	monitor->frames_processed = 0;
	monitor->frames_late = 0;
	monitor->frames_dropped = 0;
}