/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <dlfcn.h>

static _Thread_local int plugin_tls_initialized = 70;
static _Thread_local int plugin_tls_zero;
static int constructor_seen;
static int *destructor_counter;
static void *recursive_handle;
extern int rpath_dependency_value(void);

static void __attribute__((constructor))
plugin_constructor(void)
{
	recursive_handle = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
	constructor_seen = recursive_handle != 0;
}

static void __attribute__((destructor))
plugin_destructor(void)
{
	if (destructor_counter != 0)
		(*destructor_counter)++;
	if (recursive_handle != 0) {
		(void)dlclose(recursive_handle);
		recursive_handle = 0;
	}
}

int
tlstest_constructor_seen(void)
{
	return constructor_seen;
}

int
tlstest_get(void)
{
	return plugin_tls_initialized + plugin_tls_zero;
}

void
tlstest_set(int initialized, int zero)
{
	plugin_tls_initialized = initialized;
	plugin_tls_zero = zero;
}

void
tlstest_set_destructor_counter(int *counter)
{
	destructor_counter = counter;
}

uintptr_t
tlstest_address(void)
{
	return (uintptr_t)&plugin_tls_initialized;
}

int
tlstest_rpath_value(void)
{
	return rpath_dependency_value();
}
