/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD dyntest userland behavior.
 */

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

static void __attribute__((constructor)) dynamic_test_constructor(void);
static void *tls_thread(void *argument);
static void *environment_thread(void *argument);

/*
 * Runs the tests command.
 */
int
main(
	int argc,
	char **argv,
	char **envp)
{
	unsigned char bad_magic;
	int fd;
	char *saved;
	void *thread_result;
	char io_buffer[8], result[4];
	FILE *file;
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
	int plugin_destructor_count;
	unsigned reload_iteration;
	pthread_t first, second;
	void *first_result, *second_result;
	char putenv_entry[sizeof("ZEDBSD_PUTENV=first")];

	plugin_destructor_count = 0;

	/* Validates the command-line arguments. */
	if (argc < 1 || argv == NULL || argv[0] == NULL || envp == NULL)
		return 10;

	/* Handles the constructor seen condition. */
	if (!constructor_seen)
		return 11;
	puts("DL:01:STARTUP");
	memory = malloc(32);

	/* Handles the memory availability. */
	if (memory == NULL)
		return 12;
	memcpy(memory, "dynamic relocation", 19);

	/* Selects the matching value. */
	if (strcmp(memory, "dynamic relocation") != 0)
		return 13;
	free(memory);
	puts("DL:02:RELOC");
	handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the handle availability. */
	if (handle == NULL)
		return 14;
	compare = (int (*)(const char *, const char *))dlsym(handle, "strcmp");

	/* Handles a failed compare operation. */
	if (compare == NULL || compare("zedBSD", "zedBSD") != 0)
		return 15;

	/* Handles a failed dlclose operation. */
	if (dlclose(handle) != 0)
		return 16;

	/* Handles an operation failure. */
	if (dlsym(handle, "strcmp") != NULL || dlerror() == NULL ||
	    dlerror() != NULL)

		/* Returns the computed result. */
		return 17;
	puts("DL:03:DLFCN");

	/* Handles the reported system error. */
	if (errno != 0)
		return 18;
	errno = 41;

	/* Handles the reported system error. */
	if (errno != 41)
		return 19;
	puts("DL:04:TLS");

	/* Handles a failed pthread create operation. */
	if (pthread_create(&first, NULL, tls_thread, (void *)(uintptr_t)51) !=
	    0)

		/* Returns the computed result. */
		return 20;

	/* Handles a failed pthread create operation. */
	if (pthread_create(&second, NULL, tls_thread, (void *)(uintptr_t)52) !=
	    0)

		/* Returns the computed result. */
		return 20;

	/* Handles a failed pthread join operation. */
	if (pthread_join(first, &first_result) != 0 ||
	    pthread_join(second, &second_result) != 0)

		/* Returns the computed result. */
		return 21;

	/* Handles the reported system error. */
	if ((uintptr_t)first_result != 51 || (uintptr_t)second_result != 52 ||
	    errno != 41 || dlerror() != NULL)

		/* Returns the computed result. */
		return 22;
	puts("DL:05:PTHREAD-TLS");
	puts("DL:05A:PLUGIN-OPEN");
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the plugin availability. */
	if (plugin == NULL)
		return 23;
	puts("DL:05B:PLUGIN-OPENED");
	plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
	plugin_set = (void (*)(int, int))dlsym(plugin, "tlstest_set");
	plugin_constructor_seen =
	    (int (*)(void))dlsym(plugin, "tlstest_constructor_seen");
	plugin_set_destructor_counter =
	    (void (*)(int *))dlsym(plugin, "tlstest_set_destructor_counter");
	plugin_rpath_value =
	    (int (*)(void))dlsym(plugin, "tlstest_rpath_value");

	/* Handles a failed plugin constructor seen operation. */
	if (plugin_get == NULL || plugin_set == NULL ||
	    plugin_constructor_seen == NULL ||
	    plugin_set_destructor_counter == NULL ||
	    !plugin_constructor_seen() || plugin_get() != 70 ||
	    plugin_rpath_value == NULL || plugin_rpath_value() != 82)

		/* Returns the computed result. */
		return 24;
	puts("DL:05C:PLUGIN-READY");
	plugin_set_destructor_counter(&plugin_destructor_count);
	plugin_set(80, 2);

	/* Handles a failed plugin get operation. */
	if (plugin_get() != 82)
		return 25;

	/* Handles a failed dlclose operation. */
	if (dlclose(plugin) != 0)
		return 26;
	puts("DL:05D:PLUGIN-CLOSED");

	/* Handles the plugin destructor count condition. */
	if (plugin_destructor_count != 1)
		return 27;

	/* Handles an operation failure. */
	if (dlsym(plugin, "tlstest_get") != NULL || dlerror() == NULL ||
	    dlerror() != NULL)

		/* Returns the computed result. */
		return 28;
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the plugin availability. */
	if (plugin == NULL)
		return 29;
	puts("DL:05E:PLUGIN-REOPENED");
	plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
	plugin_set_destructor_counter =
	    (void (*)(int *))dlsym(plugin, "tlstest_set_destructor_counter");

	/* Handles a failed plugin get operation. */
	if (plugin_get == NULL || plugin_set_destructor_counter == NULL ||
	    plugin_get() != 70)

		/* Returns the computed result. */
		return 30;
	puts("DL:05F:PLUGIN-RELOAD-READY");
	plugin_set_destructor_counter(&plugin_destructor_count);

	/* Handles a failed dlclose operation. */
	if (dlclose(plugin) != 0 || plugin_destructor_count != 2)
		return 31;
	puts("DL:05G:PLUGIN-RECLOSED");

	/*
 * Exceed both the handle-table and TLS-module slot counts to prove that
	 * close/reopen recycles the complete loader transaction. */
	/* Process each element required by the operation. */
	for (reload_iteration = 0; reload_iteration < 70; reload_iteration++) {
		plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);

		/* Handles the plugin availability. */
		if (plugin == NULL)
			return 32;
		plugin_get = (int (*)(void))dlsym(plugin, "tlstest_get");
		plugin_set_destructor_counter = (void (*)(int *))dlsym(
		    plugin, "tlstest_set_destructor_counter");

		/* Handles a failed plugin get operation. */
		if (plugin_get == NULL ||
		    plugin_set_destructor_counter == NULL || plugin_get() != 70)

			/* Returns the computed result. */
			return 33;
		plugin_set_destructor_counter(&plugin_destructor_count);

		/* Handles a failed dlclose operation. */
		if (dlclose(plugin) != 0 ||
		    plugin_destructor_count != (int)reload_iteration + 3)

			/* Returns the computed result. */
			return 34;
	}
	puts("DL:05H:PLUGIN-RECYCLE");

	/* Process each element required by the operation. */
	for (reload_iteration = 0; reload_iteration < 64; reload_iteration++) {
		exhausted_handles[reload_iteration] =
		    dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);

		/* Handles the exhausted handles condition. */
		if (exhausted_handles[reload_iteration] == NULL)
			return 35;
	}

	/* Handles an operation failure. */
	if (dlopen("libc.so", RTLD_NOW | RTLD_LOCAL) != NULL ||
	    dlerror() == NULL || dlerror() != NULL)

		/* Returns the computed result. */
		return 36;

	/* Process each element required by the operation. */
	for (reload_iteration = 0; reload_iteration < 64; reload_iteration++) {
		/* Handles a failed dlclose operation. */
		if (dlclose(exhausted_handles[reload_iteration]) != 0)
			return 37;
	}
	handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles a failed dlclose operation. */
	if (handle == NULL || dlclose(handle) != 0)
		return 38;
	puts("DL:05I:HANDLE-OOM-RECOVERED");
	handle = dlopen("rpthtest.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the handle availability. */
	if (handle == NULL)
		return 39;
	plugin_rpath_value = (int (*)(void))dlsym(handle, "rpathtest_value");

	/* Handles a failed plugin rpath value operation. */
	if (plugin_rpath_value == NULL || plugin_rpath_value() != 82 ||
	    dlclose(handle) != 0)

		/* Returns the computed result. */
		return 40;
	puts("DL:05J:RPATH");
	handle = dlopen("versuse.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the handle availability. */
	if (handle == NULL)
		return 41;
	version_value = (int (*)(void))dlsym(handle, "versionuse_value");

	/* Handles a failed version value operation. */
	if (version_value == NULL || version_value() != 83 ||
	    dlclose(handle) != 0)

		/* Returns the computed result. */
		return 42;
	handle = dlopen("verstest.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles the handle availability. */
	if (handle == NULL)
		return 43;
	version_value = (int (*)(void))dlsym(handle, "versioned_value");

	/* Handles a failed version value operation. */
	if (version_value == NULL || version_value() != 42)
		return 44;
	version_value =
	    (int (*)(void))dlvsym(handle, "versioned_value", "ZEDBSD_1.0");

	/* Handles a failed version value operation. */
	if (version_value == NULL || version_value() != 41)
		return 45;
	version_value =
	    (int (*)(void))dlvsym(handle, "versioned_value", "ZEDBSD_2.0");

	/* Handles an operation failure. */
	if (version_value == NULL || version_value() != 42 ||
	    dlvsym(handle, "versioned_value", "ZEDBSD_MISSING") != NULL ||
	    dlerror() == NULL || dlerror() != NULL || dlclose(handle) != 0)

		/* Returns the computed result. */
		return 46;
	puts("DL:05K:SYMBOL-VERSION");

	bad_magic = 0;
	fd = open("/lib/tlstest.so", O_RDWR);

	/* Handles a failed pwrite operation. */
	if (fd < 0 || pwrite(fd, &bad_magic, 1, 0) != 1 ||
	    close(fd) != 0)

		/* Returns the computed result. */
		return 47;
	plugin = dlopen("tlstest.so", RTLD_NOW | RTLD_LOCAL);

	/* Handles an operation failure. */
	if (plugin != NULL || dlerror() == NULL || dlerror() != NULL)
		return 48;
	puts("DL:05L:CORRUPT-DSO-REJECTED");
		strcpy(putenv_entry, "ZEDBSD_PUTENV=first");

	/* Handles a failed setenv operation. */
	if (setenv("ZEDBSD_ENV_RACE", "old", 1) != 0 ||
	    (saved = getenv("ZEDBSD_ENV_RACE")) == NULL ||
	    pthread_create(&first, NULL, environment_thread, NULL) !=
		0 ||
	    pthread_join(first, &thread_result) != 0 ||
	    thread_result != NULL || strcmp(saved, "old") != 0 ||
	    strcmp(getenv("ZEDBSD_ENV_RACE"), "new") != 0)

		/* Returns the computed result. */
		return 49;

	/* Handles a failed putenv operation. */
	if (putenv(putenv_entry) != 0 ||
	    strcmp(getenv("ZEDBSD_PUTENV"), "first") != 0)

		/* Returns the computed result. */
		return 50;
	memcpy(strchr(putenv_entry, '=') + 1, "other", 6);

	/* Handles a failed getenv operation. */
	if (strcmp(getenv("ZEDBSD_PUTENV"), "other") != 0 ||
	    clearenv() != 0 || getenv("ZEDBSD_ENV_RACE") != NULL)

		/* Returns the computed result. */
		return 51;
	puts("DL:05M:LIBC-THREAD-SAFETY");

	file = fopen("/stdio-r2.tmp", "w+");

	/* Handles a failed setvbuf operation. */
	if (file == NULL ||
	    setvbuf(file, io_buffer, _IOFBF, sizeof(io_buffer)) != 0 ||
	    fwrite("abcdef", 1, 6, file) != 6 || ftell(file) != 6 ||
	    fseek(file, 0, SEEK_SET) != 0 ||
	    fread(result, 1, 3, file) != 3 ||
	    memcmp(result, "abc", 3) != 0 || ungetc('Z', file) != 'Z' ||
	    fgetc(file) != 'Z' || fgetc(file) != 'd' ||
	    fclose(file) != 0 || unlink("/stdio-r2.tmp") != 0)

		/* Returns the computed result. */
		return 52;
	puts("DL:05N:STDIO-BUFFERING");
	puts("DL:06:PLUGIN-TLS");

	/* Reports successful completion. */
	return 0;
}

/* Supports the dynamic test constructor operation. */
static void __attribute__((constructor))
dynamic_test_constructor(
	void)
{
	constructor_seen = 1;
}

/* Supports the tls thread operation. */
static void *
tls_thread(
	void *argument)
{
	int value;

	value = (int)(uintptr_t)argument;

	/* Handles the reported system error. */
	if (errno != 0)
		return (void *)(uintptr_t)100;
	errno = value;

	/* Handles an operation failure. */
	if (dlsym((void *)(uintptr_t)1, "missing") != NULL ||
	    dlerror() == NULL || dlerror() != NULL)

		/* Returns the computed result. */
		return (void *)(uintptr_t)102;

	/* Returns the computed result. */
	return (void *)(uintptr_t)(errno == value ? value : 101);
}

/* Supports the environment thread operation. */
static void *
environment_thread(
	void *argument)
{
	void *function_result;

	(void)argument;

	/* Computes the function result. */
	function_result = (void *)(uintptr_t)(setenv("ZEDBSD_ENV_RACE", "new", 1) == 0
				       ? 0
				       : 1);

	/* Returns the computed result. */
	return function_result;
}
