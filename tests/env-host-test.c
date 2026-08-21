/*
 * zedBSD persistent environment store tests
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/env.h"

#include <stdio.h>
#include <string.h>

static int
basic_test(void)
{
	struct environment environment;
	const char *name;
	const char *value;

	env_init(&environment);
	if (env_count(&environment) != 0 ||
	    env_get(&environment, "MODE") != NULL ||
	    !env_set(&environment, "MODE", "safe") ||
	    !env_set(&environment, "ROOT", "/dev/sda2") ||
	    strcmp(env_get(&environment, "MODE"), "safe") != 0 ||
	    !env_set(&environment, "MODE", "normal") ||
	    strcmp(env_get(&environment, "MODE"), "normal") != 0 ||
	    env_count(&environment) != 2 ||
	    !env_at(&environment, 1, &name, &value) ||
	    strcmp(name, "ROOT") != 0 || strcmp(value, "/dev/sda2") != 0 ||
	    !env_unset(&environment, "MODE") ||
	    env_get(&environment, "MODE") != NULL ||
	    env_unset(&environment, "MODE"))
		return 0;
	return 1;
}

static int
limits_test(void)
{
	struct environment environment;
	char name[ZEDBSD_ENV_NAME_MAX + 2U];
	char value[ZEDBSD_ENV_VALUE_MAX + 2U];
	unsigned index;

	memset(value, 'x', sizeof(value));
	value[ZEDBSD_ENV_VALUE_MAX] = '\0';
	env_init(&environment);
	if (env_set(&environment, "", "x") ||
	    env_set(&environment, "9BAD", "x") ||
	    env_set(&environment, "BAD-NAME", "x"))
		return 0;
	memset(name, 'A', sizeof(name));
	name[ZEDBSD_ENV_NAME_MAX + 1U] = '\0';
	if (env_set(&environment, name, "x"))
		return 0;
	value[ZEDBSD_ENV_VALUE_MAX] = 'x';
	value[ZEDBSD_ENV_VALUE_MAX + 1U] = '\0';
	if (env_set(&environment, "TOO_LONG", value))
		return 0;

	env_init(&environment);
	for (index = 0; index < ZEDBSD_ENV_MAX_ENTRIES; index++) {
		name[0] = 'V';
		name[1] = (char)('A' + index / 26U);
		name[2] = (char)('A' + index % 26U);
		name[3] = '\0';
		if (!env_set(&environment, name, "x"))
			return 0;
	}
	if (env_set(&environment, "EXTRA", "x") ||
	    env_count(&environment) != ZEDBSD_ENV_MAX_ENTRIES)
		return 0;

	env_init(&environment);
	value[ZEDBSD_ENV_VALUE_MAX] = '\0';
	if (!env_set(&environment, "KEEP", "old"))
		return 0;
	for (index = 0; ; index++) {
		name[0] = 'F';
		name[1] = (char)('A' + index / 26U);
		name[2] = (char)('A' + index % 26U);
		name[3] = '\0';
		if (!env_set(&environment, name, value))
			break;
	}
	if (index == 0 || env_set(&environment, "KEEP", value) ||
	    strcmp(env_get(&environment, "KEEP"), "old") != 0)
		return 0;
	return 1;
}

int
main(void)
{
	if (!basic_test() || !limits_test())
		return 1;
	puts("zedBSD environment host tests: PASS");
	return 0;
}
