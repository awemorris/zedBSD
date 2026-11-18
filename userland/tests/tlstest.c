/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD tlstest userland behavior.
 */

#include <stdint.h>
#include <dlfcn.h>

static _Thread_local int plugin_tls_initialized = 70;
static _Thread_local int plugin_tls_zero;
static int constructor_seen;
static int *destructor_counter;
static void *recursive_handle;
extern int rpath_dependency_value(void);

static void __attribute__((constructor)) plugin_constructor(void);
static void __attribute__((destructor)) plugin_destructor(void);

/*
 * Implements the tlstest constructor seen operation.
 */
int
tlstest_constructor_seen(
	void)
{
	/* Returns the computed result. */
	return constructor_seen;
}

/*
 * Implements the tlstest get operation.
 */
int
tlstest_get(
	void)
{
	/* Returns the computed result. */
	return plugin_tls_initialized + plugin_tls_zero;
}

/*
 * Implements the tlstest set operation.
 */
void
tlstest_set(
	int initialized,
	int zero)
{
	plugin_tls_initialized = initialized;
	plugin_tls_zero = zero;
}

/*
 * Implements the tlstest set destructor counter operation.
 */
void
tlstest_set_destructor_counter(
	int *counter)
{
	destructor_counter = counter;
}

/*
 * Implements the tlstest address operation.
 */
uintptr_t
tlstest_address(
	void)
{
	/* Returns the computed result. */
	return (uintptr_t)&plugin_tls_initialized;
}

/*
 * Implements the tlstest rpath value operation.
 */
int
tlstest_rpath_value(
	void)
{
	int function_result;

	/* Obtains the rpath dependency value result. */
	function_result = rpath_dependency_value();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the plugin constructor operation. */
static void __attribute__((constructor))
plugin_constructor(
	void)
{
	recursive_handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
	constructor_seen = recursive_handle != 0;
}

/* Supports the plugin destructor operation. */
static void __attribute__((destructor))
plugin_destructor(
	void)
{
	/* Handles the destructor counter condition. */
	if (destructor_counter != 0)
		(*destructor_counter)++;

	/* Handles the recursive handle condition. */
	if (recursive_handle != 0) {
		(void)dlclose(recursive_handle);
		recursive_handle = 0;
	}
}
