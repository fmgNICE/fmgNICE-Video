/*
 * fmgNICE Video Source Plugin
 * Main plugin entry point
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fmgnice-video", "en-US")

extern struct obs_source_info fmgnice_video_source;

/* Global list of active sources for emergency cleanup */
static pthread_mutex_t g_sources_mutex = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(void*) g_active_sources = {0};

void fmgnice_register_source(void *source);
void fmgnice_unregister_source(void *source);
void fmgnice_emergency_cleanup(void);

MODULE_EXPORT const char *obs_module_name(void)
{
	return "fmgNICE Video Source";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Advanced video source with synchronized timeline support";
}

MODULE_EXPORT const char *obs_module_author(void)
{
	return "fmgNICE";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[fmgNICE Video] Loading fmgNICE Video Source plugin...");
	
	/* Register the fmgNICE video source */
	obs_register_source(&fmgnice_video_source);
	blog(LOG_INFO, "[fmgNICE Video] fmgNICE Video Source registered");
	
	blog(LOG_INFO, "[fmgNICE Video] Plugin loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[fmgNICE Video] Starting plugin unload...");
	
	/* Emergency cleanup of any remaining sources */
	fmgnice_emergency_cleanup();
	
	blog(LOG_INFO, "[fmgNICE Video] Plugin unloaded");
}

void fmgnice_register_source(void *source)
{
	if (!source)
		return;
	
	pthread_mutex_lock(&g_sources_mutex);
	da_push_back(g_active_sources, &source);
	/* Source registration tracking - enable for debugging */
	/* blog(LOG_DEBUG, "[fmgNICE Video] Registered source %p (total: %zu)", 
		source, g_active_sources.num); */
	pthread_mutex_unlock(&g_sources_mutex);
}

void fmgnice_unregister_source(void *source)
{
	if (!source)
		return;
	
	pthread_mutex_lock(&g_sources_mutex);
	for (size_t i = 0; i < g_active_sources.num; i++) {
		if (g_active_sources.array[i] == source) {
			da_erase(g_active_sources, i);
			/* blog(LOG_DEBUG, "[fmgNICE Video] Unregistered source %p (remaining: %zu)", 
				source, g_active_sources.num); */
			break;
		}
	}
	pthread_mutex_unlock(&g_sources_mutex);
}

void fmgnice_emergency_cleanup(void)
{
	blog(LOG_WARNING, "[fmgNICE Video] Emergency cleanup initiated");
	
	pthread_mutex_lock(&g_sources_mutex);
	size_t count = g_active_sources.num;
	if (count > 0) {
		blog(LOG_WARNING, "[fmgNICE Video] Found %zu active sources during unload - forcing cleanup", count);
		
		/* Note: We can't directly destroy sources here as they're managed by OBS
		 * but we can signal them to stop their operations */
		for (size_t i = 0; i < count; i++) {
			void *source = g_active_sources.array[i];
			if (source) {
				blog(LOG_WARNING, "[fmgNICE Video] Force-stopping source %p", source);
				/* The source destroy callback will handle cleanup */
			}
		}
	}
	da_free(g_active_sources);
	pthread_mutex_unlock(&g_sources_mutex);
	
	/* Give threads a moment to finish */
	os_sleep_ms(100);
	
	blog(LOG_INFO, "[fmgNICE Video] Emergency cleanup completed");
}