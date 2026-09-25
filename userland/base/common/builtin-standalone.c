/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the shell's echo, printf and test need when they are built as
 * commands of their own (/bin/echo, /bin/printf, /bin/test): memory that
 * ends the command when there is none, and temporary memory, which a
 * command that runs once simply keeps until it exits.  There are no shell
 * variables, so printf -v names none.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Allocates memory, ending the command when there is none.
 */
void *
sh_malloc(
	size_t size)
{
	void *memory;

	/* At least one byte, so that NULL means failure. */
	if (size == 0)
		size = 1;
	memory = malloc(size);
	if (memory == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}

	/* Succeeded. */
	return memory;
}

/*
 * Takes ownership of temporary memory: the command exits soon, so it is
 * kept until then.
 */
void *
sh_temp_own(
	void *memory)
{
	/* Kept as it is. */
	return memory;
}

void *
sh_realloc(
	void *memory,
	size_t size)
{
	/* At least one byte, so that NULL means failure. */
	if (size == 0)
		size = 1;
	memory = realloc(memory, size);
	if (memory == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}

	/* Succeeded. */
	return memory;
}

int
sh_var_name(
	const char *name)
{
	/* A utility has no shell variables: printf -v names none. */
	(void)name;
	return 0;
}

int
sh_var_set(
	const char *name,
	const char *value,
	int flags)
{
	/* Nothing to set outside a shell. */
	(void)name;
	(void)value;
	(void)flags;
	return -1;
}
