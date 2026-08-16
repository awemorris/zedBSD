/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

static int constructor_seen;

static void __attribute__((constructor))
dynamic_test_constructor(void)
{
	constructor_seen = 1;
}

static void *
tls_thread(void *argument)
{
	int value = (int)(uintptr_t)argument;

	if (errno != 0)
		return (void *)(uintptr_t)100;
	errno = value;
	if (dlsym((void *)(uintptr_t)1, "missing") != NULL ||
	    dlerror() == NULL || dlerror() != NULL)
		return (void *)(uintptr_t)102;
	return (void *)(uintptr_t)(errno == value ? value : 101);
}

int
main(int argc, char **argv, char **envp)
{
	char *memory;
	void *handle;
	int (*compare)(const char *, const char *);
	int (*plugin_get)(void);
	void (*plugin_set)(int, int);
	int (*plugin_constructor_seen)(void);
	void *plugin;
	pthread_t first, second;
	void *first_result, *second_result;
	if (argc < 1 || argv == NULL || argv[0] == NULL || envp == NULL)
		return 10;
	if (!constructor_seen)
		return 11;
	puts("DL:01:STARTUP");
	memory = malloc(32);
	if (memory == NULL)
		return 12;
	memcpy(memory, "dynamic relocation", 19);
	if (strcmp(memory, "dynamic relocation") != 0)
		return 13;
	free(memory);
	puts("DL:02:RELOC");
	handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		return 14;
	compare = (int (*)(const char *, const char *))dlsym(handle, "strcmp");
	if (compare == NULL || compare("zedBSD", "zedBSD") != 0)
		return 15;
	if (dlclose(handle) != 0)
		return 16;
	if (dlsym(handle, "strcmp") != NULL || dlerror() == NULL ||
	    dlerror() != NULL)
		return 17;
	puts("DL:03:DLFCN");
	if (errno != 0)
		return 18;
	errno = 41;
	if (errno != 41)
		return 19;
	puts("DL:04:TLS");
	if (pthread_create(&first, NULL, tls_thread, (void *)(uintptr_t)51) != 0)
		return 20;
	if (pthread_create(&second, NULL, tls_thread, (void *)(uintptr_t)52) != 0)
		return 20;
	if (pthread_join(first, &first_result) != 0 ||
	    pthread_join(second, &second_result) != 0)
		return 21;
	if ((uintptr_t)first_result != 51 || (uintptr_t)second_result != 52 ||
	    errno != 41 || dlerror() != NULL)
		return 22;
	puts("DL:05:PTHREAD-TLS");
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL)
		return 23;
	plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
	plugin_set = (void (*)(int, int))dlsym(plugin, "tlstest_set");
	plugin_constructor_seen = (int (*)(void))dlsym(plugin,
	    "tlstest_constructor_seen");
	if (plugin_get == NULL || plugin_set == NULL ||
	    plugin_constructor_seen == NULL || !plugin_constructor_seen() ||
	    plugin_get() != 70)
		return 24;
	plugin_set(80, 2);
	if (plugin_get() != 82)
		return 25;
	if (dlclose(plugin) != 0)
		return 26;
	puts("DL:06:PLUGIN-TLS");
	return 0;
}
