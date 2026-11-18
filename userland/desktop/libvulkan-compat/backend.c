/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The process-wide Vulkan backend, opened by absolute path before the first call.
 */

#include "compat.h"
#include "userland/desktop/paths.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One immutable backend table, published by backend_once and never unloaded. */
struct compat_backend compat_backend;

/* Serializes the publication of the backend and its function tables. */
static pthread_once_t backend_once = PTHREAD_ONCE_INIT;

static void backend_open(void);
static void *backend_try(const char *path);

/*
 * Initializes the backend once and reports whether it is usable.
 */
int
compat_backend_ready(
	void)
{
	/* Publishes all trampolines before any thread can use them. */
	(void)pthread_once(&backend_once, backend_open);

	/* Reports the unavailable backend after the single diagnostic. */
	if (compat_backend.handle == NULL)
		return 0;

	/* Succeeded: the backend is loaded and its table is ready. */
	return 1;
}

/* Opens the selected backend and publishes its direct interception entry points. */
static void
backend_open(
	void)
{
	const char *override;
	char configured[PATH_MAX];
	char paths[] = KEILAND_VULKAN_BACKEND_PATHS;
	char *path;
	char *following;
	char *line;
	char *newline;
	FILE *file;
	void *handle;

	/* An explicit override is authoritative, including an invalid selection. */
	handle = NULL;
	override = getenv("KEILAND_VULKAN_BACKEND");
	if (override != NULL && override[0] != '\0') {
		handle = backend_try(override);
	} else {
		/* Reads the optional installed backend selection. */
		file = fopen(KEILAND_SYSCONFDIR "/vulkan-backend", "r");
		if (file != NULL) {
			/* Keeps the configured path independent of the file buffer lifetime. */
			line = fgets(configured, sizeof(configured), file);
			(void)fclose(file);
			if (line != NULL) {
				/* Removes the configuration line's terminator. */
				newline = strchr(configured, '\n');
				if (newline != NULL)
					*newline = '\0';

				/* Opens only the explicitly configured backend. */
				handle = backend_try(configured);
			}
		} else {
			/* Tries the compiled absolute paths until a usable backend is found. */
			path = paths;
			while (path != NULL) {
				/* Separates the next candidate without changing the configured defaults. */
				following = strchr(path, ':');
				if (following != NULL) {
					*following = '\0';
					following++;
				}

				/* Stops at the first usable backend. */
				handle = backend_try(path);
				if (handle != NULL)
					break;

				/* Advances to the remaining compiled paths. */
				path = following;
			}
		}
	}

	/* Reports one failed selection; enumeration still has its specified empty fallback. */
	if (handle == NULL) {
		(void)fprintf(stderr, "libvulkan-compat: no backend libvulkan (KEILAND_VULKAN_BACKEND, %s/vulkan-backend, %s)\n", KEILAND_SYSCONFDIR, KEILAND_VULKAN_BACKEND_PATHS);
		return;
	}

	/* Resolves intercepted calls directly so procedure-query interposition cannot form a cycle. */
	compat_backend.handle = handle;
	compat_backend.get_instance_proc = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
	compat_backend.get_device_proc = (PFN_vkGetDeviceProcAddr)dlsym(handle, "vkGetDeviceProcAddr");
	compat_backend.create_instance = (PFN_vkCreateInstance)dlsym(handle, "vkCreateInstance");
	compat_backend.destroy_instance = (PFN_vkDestroyInstance)dlsym(handle, "vkDestroyInstance");
	compat_backend.instance_extensions = (PFN_vkEnumerateInstanceExtensionProperties)dlsym(handle, "vkEnumerateInstanceExtensionProperties");
	compat_backend.instance_layers = (PFN_vkEnumerateInstanceLayerProperties)dlsym(handle, "vkEnumerateInstanceLayerProperties");
	compat_backend.instance_version = (PFN_vkEnumerateInstanceVersion)dlsym(handle, "vkEnumerateInstanceVersion");
	compat_backend.device_extensions = (PFN_vkEnumerateDeviceExtensionProperties)dlsym(handle, "vkEnumerateDeviceExtensionProperties");
	compat_backend.create_device = (PFN_vkCreateDevice)dlsym(handle, "vkCreateDevice");
	compat_backend.destroy_device = (PFN_vkDestroyDevice)dlsym(handle, "vkDestroyDevice");
	compat_backend.get_queue = (PFN_vkGetDeviceQueue)dlsym(handle, "vkGetDeviceQueue");
	compat_backend.get_queue2 = (PFN_vkGetDeviceQueue2)dlsym(handle, "vkGetDeviceQueue2");

	/* Publishes every unmodified core trampoline from the same backend handle. */
	compat_forward_fill(handle);

	/* Succeeded: pthread_once now publishes the selected backend. */
	return;
}

/* Refuses self-loading and opens a backend with local binding when supported. */
static void *
backend_try(
	const char *path)
{
	Dl_info self;
	char own_path[PATH_MAX];
	char candidate[PATH_MAX];
	char *resolved;
	const char *disable;
	void *handle;
	void *get_proc;
	int found;
	int differs;
	int flags;

	/* A relative name can resolve to this very library. */
	if (path[0] != '/')
		return NULL;

	/* Requires a real candidate before comparing the library identities. */
	resolved = realpath(path, candidate);
	if (resolved == NULL)
		return NULL;

	/* Locates this loaded library, including symlink aliases. */
	found = dladdr((void *)vkGetInstanceProcAddr, &self);
	if (found == 0)
		return NULL;

	/* Requires an unambiguous identity for the calling library. */
	resolved = realpath(self.dli_fname, own_path);
	if (resolved == NULL)
		return NULL;

	/* Refuses to chain the library to itself. */
	differs = strcmp(own_path, candidate);
	if (differs == 0)
		return NULL;

	/* Keeps backend relocations from choosing our similarly named functions. */
	flags = RTLD_NOW | RTLD_LOCAL;
	disable = getenv("KEILAND_VULKAN_NO_DEEPBIND");
#ifdef RTLD_DEEPBIND
	if (disable == NULL || disable[0] != '1')
		flags |= RTLD_DEEPBIND;
#else
	(void)disable;
#endif

	/* Loads the candidate through its canonical absolute path. */
	handle = dlopen(candidate, flags);
	if (handle == NULL)
		return NULL;

	/* Confirms that its entry point belongs to a different implementation. */
	get_proc = dlsym(handle, "vkGetInstanceProcAddr");
	if (get_proc == NULL || get_proc == (void *)vkGetInstanceProcAddr) {
		/* Releases a rejected candidate before another selection is attempted. */
		(void)dlclose(handle);
		return NULL;
	}

	/* Succeeded: the caller owns the loaded backend for the process lifetime. */
	return handle;
}
