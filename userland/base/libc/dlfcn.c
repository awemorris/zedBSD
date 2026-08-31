/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library dlfcn support.
 */

#include <dlfcn.h>
#include <zedbsd/rtld-abi.h>

#if !defined(ZEDBSD_DYNAMIC_LIBC)
extern void *__rtld_dlopen(const char *, int) __attribute__((weak));
extern void *__rtld_dlsym(void *, const char *) __attribute__((weak));
extern void *__rtld_dlvsym(void *, const char *, const char *)
    __attribute__((weak));
extern int __rtld_dladdr(const void *, Dl_info *) __attribute__((weak));
extern int __rtld_dlclose(void *) __attribute__((weak));
extern char *__rtld_dlerror(void) __attribute__((weak));
#endif

#if !defined(ZEDBSD_DYNAMIC_LIBC)
static char static_dlerror[] = "dynamic loading is unavailable";
static int static_error_pending;
#endif

/*
 * Implements the dlopen operation.
 */
void *
dlopen(
	const char *path,
	int flags)
{
	void *function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dlopen(path, flags);

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles the rtld dlopen condition. */
	if (__rtld_dlopen != 0) {
		/* Obtains the rtld dlopen result. */
		function_result = __rtld_dlopen(path, flags);

		/* Returns the computed result. */
		return function_result;
	}

	static_error_pending = 1;

	/* Reports successful completion. */
	return 0;
#endif
}

/*
 * Implements the dlvsym operation.
 */
void *
dlvsym(
	void *handle,
	const char *name,
	const char *version)
{
	void *function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dlvsym(handle, name, version);

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles the rtld dlvsym condition. */
	if (__rtld_dlvsym != 0) {
		/* Obtains the rtld dlvsym result. */
		function_result = __rtld_dlvsym(handle, name, version);

		/* Returns the computed result. */
		return function_result;
	}
	(void)handle;
	(void)name;
	(void)version;
	static_error_pending = 1;

	/* Reports successful completion. */
	return 0;
#endif
}

/*
 * Implements the dlsym operation.
 */
void *
dlsym(
	void *handle,
	const char *name)
{
	void *function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dlsym(handle, name);

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles the rtld dlsym condition. */
	if (__rtld_dlsym != 0) {
		/* Obtains the rtld dlsym result. */
		function_result = __rtld_dlsym(handle, name);

		/* Returns the computed result. */
		return function_result;
	}
	(void)handle;
	(void)name;
	static_error_pending = 1;

	/* Reports successful completion. */
	return 0;
#endif
}

/*
 * Implements the dladdr operation.
 */
int
dladdr(
	const void *address,
	Dl_info *information)
{
	int function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dladdr(address, information);

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles the rtld dladdr condition. */
	if (__rtld_dladdr != 0) {
		/* Obtains the rtld dladdr result. */
		function_result = __rtld_dladdr(address, information);

		/* Returns the computed result. */
		return function_result;
	}
	(void)address;
	(void)information;

	/* Reports successful completion. */
	return 0;
#endif
}

/*
 * Implements the dlclose operation.
 */
int
dlclose(
	void *handle)
{
	int function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dlclose(handle);

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles the rtld dlclose condition. */
	if (__rtld_dlclose != 0) {
		/* Obtains the rtld dlclose result. */
		function_result = __rtld_dlclose(handle);

		/* Returns the computed result. */
		return function_result;
	}
	(void)handle;
	static_error_pending = 1;

	/* Reports operation failure. */
	return -1;
#endif
}

/*
 * Implements the dlerror operation.
 */
char *
dlerror(
	void)
{
	char *function_result;

#if defined(ZEDBSD_DYNAMIC_LIBC)

	/* Computes the function result. */
	function_result = __rtld_exports.dlerror();

	/* Returns the computed result. */
	return function_result;

#else

	/* Handles an operation failure. */
	if (__rtld_dlerror != 0) {
		/* Obtains the rtld dlerror result. */
		function_result = __rtld_dlerror();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles an operation failure. */
	if (!static_error_pending)
		return 0;
	static_error_pending = 0;

	/* Returns the computed result. */
	return static_dlerror;
#endif
}
