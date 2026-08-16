/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>

static _Thread_local int plugin_tls_initialized = 70;
static _Thread_local int plugin_tls_zero;
static int constructor_seen;

static void __attribute__((constructor))
plugin_constructor(void)
{
	constructor_seen = 1;
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

uintptr_t
tlstest_address(void)
{
	return (uintptr_t)&plugin_tls_initialized;
}
