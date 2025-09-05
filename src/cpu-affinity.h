/*
 * CPU affinity management for optimal thread placement
 * Reduces context switches and improves cache locality
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

/* CPU core types */
typedef enum {
	CPU_CORE_ANY = 0,
	CPU_CORE_PERFORMANCE = 1,  /* P-cores on Intel 12th gen+ */
	CPU_CORE_EFFICIENCY = 2,   /* E-cores on Intel 12th gen+ */
	CPU_CORE_PHYSICAL = 3,     /* Physical cores only (no hyperthreading) */
	CPU_CORE_LOGICAL = 4       /* All logical cores including HT */
} cpu_core_type_t;

/* Get number of CPU cores */
static inline int get_cpu_count(void)
{
#ifdef _WIN32
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	return sysinfo.dwNumberOfProcessors;
#else
	return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/* Set thread affinity to specific CPU */
static inline bool set_thread_cpu(int cpu_id)
{
#ifdef _WIN32
	HANDLE thread = GetCurrentThread();
	DWORD_PTR mask = 1ULL << cpu_id;
	
	if (SetThreadAffinityMask(thread, mask) != 0) {
		/* Also set ideal processor for better scheduling */
		SetThreadIdealProcessor(thread, cpu_id);
		return true;
	}
	return false;
#else
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu_id, &cpuset);
	
	pthread_t thread = pthread_self();
	return pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) == 0;
#endif
}

/* Set thread affinity to range of CPUs */
static inline bool set_thread_cpu_range(int start_cpu, int end_cpu)
{
#ifdef _WIN32
	HANDLE thread = GetCurrentThread();
	DWORD_PTR mask = 0;
	
	for (int i = start_cpu; i <= end_cpu && i < 64; i++) {
		mask |= (1ULL << i);
	}
	
	return SetThreadAffinityMask(thread, mask) != 0;
#else
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	
	for (int i = start_cpu; i <= end_cpu; i++) {
		CPU_SET(i, &cpuset);
	}
	
	pthread_t thread = pthread_self();
	return pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) == 0;
#endif
}

/* Set thread priority */
static inline bool set_thread_priority_high(void)
{
#ifdef _WIN32
	HANDLE thread = GetCurrentThread();
	return SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
#else
	struct sched_param param;
	param.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
	
	pthread_t thread = pthread_self();
	return pthread_setschedparam(thread, SCHED_FIFO, &param) == 0;
#endif
}

/* Optimize thread placement for decoder */
static inline void optimize_decoder_thread_placement(void)
{
	int cpu_count = get_cpu_count();
	
	if (cpu_count >= 8) {
		/* On 8+ core systems, use cores 2-3 for decoder (avoid core 0) */
		set_thread_cpu_range(2, 3);
		set_thread_priority_high();
	} else if (cpu_count >= 4) {
		/* On 4+ core systems, use core 1 for decoder */
		set_thread_cpu(1);
		set_thread_priority_high();
	}
	/* On dual-core or less, let OS schedule */
}

/* Optimize thread placement for display */
static inline void optimize_display_thread_placement(void)
{
	int cpu_count = get_cpu_count();
	
	if (cpu_count >= 8) {
		/* On 8+ core systems, use cores 4-5 for display */
		set_thread_cpu_range(4, 5);
		set_thread_priority_high();
	} else if (cpu_count >= 4) {
		/* On 4+ core systems, use core 2 for display */
		set_thread_cpu(2);
		set_thread_priority_high();
	}
	/* On dual-core or less, let OS schedule */
}

/* Set thread name for debugging */
static inline void set_thread_name(const char* name)
{
#ifdef _WIN32
	/* Windows 10+ SetThreadDescription */
	typedef HRESULT (WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);
	static SetThreadDescription_t pSetThreadDescription = NULL;
	
	if (!pSetThreadDescription) {
		HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
		if (kernel32) {
			pSetThreadDescription = (SetThreadDescription_t)GetProcAddress(kernel32, "SetThreadDescription");
		}
	}
	
	if (pSetThreadDescription) {
		wchar_t wname[256];
		MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
		pSetThreadDescription(GetCurrentThread(), wname);
	}
#elif defined(__linux__)
	pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
	pthread_setname_np(name);
#endif
}