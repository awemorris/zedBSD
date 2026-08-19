/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/base/sh/arithmetic.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *lookup(void *context, const char *name)
{
	(void)context;
	return getenv(name);
}

static void expect(const char *source, long expected)
{
	const char *error;
	long result;
	assert(sh_arithmetic_eval(source, lookup, NULL, &result, &error));
	assert(result == expected);
}

int main(void)
{
	const char *error;
	long result;
	assert(setenv("COUNT", "7", 1) == 0);
	expect("1 + 2 * 3", 7);
	expect("(1 + 2) * 3", 9);
	expect("COUNT << 2", 28);
	expect("COUNT > 3 && COUNT < 9", 1);
	expect("0 ? 11 : 22", 22);
	expect("~0", -1);
	assert(!sh_arithmetic_eval("1 / 0", lookup, NULL, &result, &error));
	assert(strcmp(error, "division by zero") == 0);
	assert(!sh_arithmetic_eval("1 rubbish", lookup, NULL, &result, &error));
	puts("zedBSD shell arithmetic host test: PASS");
	return 0;
}
