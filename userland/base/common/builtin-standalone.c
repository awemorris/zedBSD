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

/*
 * Resizes memory, ending the command when there is none.
 */
void *
sh_realloc(
	void *memory,
	size_t size)
{
	void *resized;

	/* At least one byte, so that NULL means failure. */
	if (size == 0)
		size = 1;

	/* Resizes the block; a command without memory ends. */
	resized = realloc(memory, size);
	if (resized == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}

	/* Succeeded: the resized block. */
	return resized;
}

/*
 * Reports whether a string names a shell variable.
 *
 * A utility has no shell variables, so printf -v names none.
 */
int
sh_var_name(
	const char *name)
{
	(void)name;

	/* No string names a variable here. */
	return 0;
}

/*
 * Refuses to set a shell variable, since a utility has none.
 */
int
sh_var_set(
	const char *name,
	const char *value,
	int flags)
{
	(void)name;
	(void)value;
	(void)flags;

	/* Reports the variable as one that cannot be set. */
	return -1;
}
