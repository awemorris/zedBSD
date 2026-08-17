/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

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

static void *
environment_thread(void *argument)
{
	(void)argument;
	return (void *)(uintptr_t)(setenv("ZEDBSD_ENV_RACE", "new", 1) == 0 ?
	    0 : 1);
}

int
main(int argc, char **argv, char **envp)
{
	char *memory;
	void *handle;
	int (*compare)(const char *, const char *);
	int (*plugin_get)(void);
	void (*plugin_set)(int, int);
	void (*plugin_set_destructor_counter)(int *);
	int (*plugin_constructor_seen)(void);
	int (*plugin_rpath_value)(void);
	int (*version_value)(void);
	void *plugin;
	void *exhausted_handles[64];
	int plugin_destructor_count = 0;
	unsigned reload_iteration;
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
	puts("DL:05A:PLUGIN-OPEN");
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL)
		return 23;
	puts("DL:05B:PLUGIN-OPENED");
	plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
	plugin_set = (void (*)(int, int))dlsym(plugin, "tlstest_set");
	plugin_constructor_seen = (int (*)(void))dlsym(plugin,
	    "tlstest_constructor_seen");
	plugin_set_destructor_counter = (void (*)(int *))dlsym(plugin,
	    "tlstest_set_destructor_counter");
	plugin_rpath_value = (int (*)(void))dlsym(plugin,
	    "tlstest_rpath_value");
	if (plugin_get == NULL || plugin_set == NULL ||
	    plugin_constructor_seen == NULL ||
	    plugin_set_destructor_counter == NULL ||
	    !plugin_constructor_seen() ||
	    plugin_get() != 70 || plugin_rpath_value == NULL ||
	    plugin_rpath_value() != 82)
		return 24;
	puts("DL:05C:PLUGIN-READY");
	plugin_set_destructor_counter(&plugin_destructor_count);
	plugin_set(80, 2);
	if (plugin_get() != 82)
		return 25;
	if (dlclose(plugin) != 0)
		return 26;
	puts("DL:05D:PLUGIN-CLOSED");
	if (plugin_destructor_count != 1)
		return 27;
	if (dlsym(plugin, "tlstest_get") != NULL || dlerror() == NULL ||
	    dlerror() != NULL)
		return 28;
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL)
		return 29;
	puts("DL:05E:PLUGIN-REOPENED");
	plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
	plugin_set_destructor_counter = (void (*)(int *))dlsym(plugin,
	    "tlstest_set_destructor_counter");
	if (plugin_get == NULL || plugin_set_destructor_counter == NULL ||
	    plugin_get() != 70)
		return 30;
	puts("DL:05F:PLUGIN-RELOAD-READY");
	plugin_set_destructor_counter(&plugin_destructor_count);
	if (dlclose(plugin) != 0 || plugin_destructor_count != 2)
		return 31;
	puts("DL:05G:PLUGIN-RECLOSED");
	/* Exceed both the handle-table and TLS-module slot counts to prove that
	 * close/reopen recycles the complete loader transaction. */
	for (reload_iteration = 0; reload_iteration < 70; reload_iteration++) {
		plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);
		if (plugin == NULL)
			return 32;
		plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
		plugin_set_destructor_counter = (void (*)(int *))dlsym(plugin,
		    "tlstest_set_destructor_counter");
		if (plugin_get == NULL || plugin_set_destructor_counter == NULL ||
		    plugin_get() != 70)
			return 33;
		plugin_set_destructor_counter(&plugin_destructor_count);
		if (dlclose(plugin) != 0 ||
		    plugin_destructor_count != (int)reload_iteration + 3)
			return 34;
	}
	puts("DL:05H:PLUGIN-RECYCLE");
	for (reload_iteration = 0; reload_iteration < 64; reload_iteration++) {
		exhausted_handles[reload_iteration] = dlopen("libc.so",
		    RTLD_NOW | RTLD_LOCAL);
		if (exhausted_handles[reload_iteration] == NULL)
			return 35;
	}
	if (dlopen("libc.so", RTLD_NOW | RTLD_LOCAL) != NULL ||
	    dlerror() == NULL || dlerror() != NULL)
		return 36;
	for (reload_iteration = 0; reload_iteration < 64; reload_iteration++)
		if (dlclose(exhausted_handles[reload_iteration]) != 0)
			return 37;
	handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL || dlclose(handle) != 0)
		return 38;
	puts("DL:05I:HANDLE-OOM-RECOVERED");
	handle = dlopen("rpthtest.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		return 39;
	plugin_rpath_value = (int (*)(void))dlsym(handle, "rpathtest_value");
	if (plugin_rpath_value == NULL || plugin_rpath_value() != 82 ||
	    dlclose(handle) != 0)
		return 40;
	puts("DL:05J:RPATH");
	handle = dlopen("versuse.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		return 41;
	version_value = (int (*)(void))dlsym(handle, "versionuse_value");
	if (version_value == NULL || version_value() != 83 ||
	    dlclose(handle) != 0)
		return 42;
	handle = dlopen("verstest.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		return 43;
	version_value = (int (*)(void))dlsym(handle, "versioned_value");
	if (version_value == NULL || version_value() != 42)
		return 44;
	version_value = (int (*)(void))dlvsym(handle, "versioned_value",
	    "ZEDBSD_1.0");
	if (version_value == NULL || version_value() != 41)
		return 45;
	version_value = (int (*)(void))dlvsym(handle, "versioned_value",
	    "ZEDBSD_2.0");
	if (version_value == NULL || version_value() != 42 ||
	    dlvsym(handle, "versioned_value", "ZEDBSD_MISSING") != NULL ||
	    dlerror() == NULL || dlerror() != NULL || dlclose(handle) != 0)
		return 46;
	puts("DL:05K:SYMBOL-VERSION");
	{
		unsigned char bad_magic = 0;
		int fd = open("/lib/tlstest.so", O_RDWR);
		if (fd < 0 || pwrite(fd, &bad_magic, 1, 0) != 1 || close(fd) != 0)
			return 47;
		plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);
		if (plugin != NULL || dlerror() == NULL || dlerror() != NULL)
			return 48;
	}
	puts("DL:05L:CORRUPT-DSO-REJECTED");
	{
		char putenv_entry[] = "ZEDBSD_PUTENV=first";
		char *saved;
		void *thread_result;

		if (setenv("ZEDBSD_ENV_RACE", "old", 1) != 0 ||
		    (saved = getenv("ZEDBSD_ENV_RACE")) == NULL ||
		    pthread_create(&first, NULL, environment_thread, NULL) != 0 ||
		    pthread_join(first, &thread_result) != 0 || thread_result != NULL ||
		    strcmp(saved, "old") != 0 ||
		    strcmp(getenv("ZEDBSD_ENV_RACE"), "new") != 0)
			return 49;
		if (putenv(putenv_entry) != 0 ||
		    strcmp(getenv("ZEDBSD_PUTENV"), "first") != 0)
			return 50;
		memcpy(strchr(putenv_entry, '=') + 1, "other", 6);
		if (strcmp(getenv("ZEDBSD_PUTENV"), "other") != 0 ||
		    clearenv() != 0 || getenv("ZEDBSD_ENV_RACE") != NULL)
			return 51;
	}
	puts("DL:05M:LIBC-THREAD-SAFETY");
	{
		char io_buffer[8], result[4];
		FILE *file = fopen("/stdio-r2.tmp", "w+");
		if (file == NULL || setvbuf(file, io_buffer, _IOFBF,
		    sizeof(io_buffer)) != 0 || fwrite("abcdef", 1, 6, file) != 6 ||
		    ftell(file) != 6 || fseek(file, 0, SEEK_SET) != 0 ||
		    fread(result, 1, 3, file) != 3 || memcmp(result, "abc", 3) != 0 ||
		    ungetc('Z', file) != 'Z' || fgetc(file) != 'Z' ||
		    fgetc(file) != 'd' || fclose(file) != 0 ||
		    unlink("/stdio-r2.tmp") != 0)
			return 52;
	}
	puts("DL:05N:STDIO-BUFFERING");
	puts("DL:06:PLUGIN-TLS");
	return 0;
}
