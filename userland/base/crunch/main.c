/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs one of the programs built into this single program.
 *
 *	name [argument...]		through a link named after the program
 *	crunch name [argument...]	naming the program first
 *	crunch				lists the programs
 *
 * Every command of the single program is a link to this one file, so the
 * commands share one text in memory and one file on the disk.  The program
 * is chosen by the last component of the name it was started under; a
 * leading '-' (a login shell's mark) is not part of the name.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crunch.h"

/* The name the single program has itself. */
#define CRUNCH_NAME "crunch"

static const char *command_name(const char *path);
static const struct crunch_program *find_program(const char *name);
static void list_programs(FILE *stream);

/*
 * Runs the program the command's name chooses.
 */
int
main(
	int argc,
	char **argv,
	char **envp)
{
	const struct crunch_program *program;
	const char *name;
	int order;
	int status;

	/* A process started without even a name has nothing to choose by. */
	if (argc < 1 || argv[0] == NULL) {
		list_programs(stderr);
		return 127;
	}

	/* Takes the program's name from the name this one was started under. */
	name = command_name(argv[0]);
	order = strcmp(name, CRUNCH_NAME);

	/* The single program's own name: the program is named by the first argument. */
	if (order == 0) {
		if (argc < 2) {
			list_programs(stdout);
			return 0;
		}
		argc--;
		argv++;
		name = command_name(argv[0]);
		setprogname(name);
	}

	/* Finds the program; a name no program has is refused with the list. */
	program = find_program(name);
	if (program == NULL) {
		fprintf(stderr, "%s: %s: not a program of this file\n", CRUNCH_NAME, name);
		list_programs(stderr);
		return 127;
	}

	/* Runs the program, whose status is the process's. */
	status = program->entry(argc, argv, envp);

	/* Succeeded: reports the program's own status. */
	return status;
}

/* Returns the command name in a path: its last component, without a login shell's '-'. */
static const char *
command_name(
	const char *path)
{
	const char *slash;
	const char *name;

	/* Drops the directories. */
	slash = strrchr(path, '/');
	name = path;
	if (slash != NULL)
		name = slash + 1;

	/* Drops the mark a login shell's name carries. */
	if (name[0] == '-')
		name++;

	/* The command name. */
	return name;
}

/* Finds a program by its name in the sorted table, or returns NULL. */
static const struct crunch_program *
find_program(
	const char *name)
{
	unsigned int low;
	unsigned int high;
	unsigned int middle;
	int order;

	/* Halves the range of the table that may hold the name until it is empty. */
	low = 0;
	high = crunch_program_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		order = strcmp(name, crunch_programs[middle].name);
		if (order == 0)
			return &crunch_programs[middle];
		if (order < 0) {
			high = middle;
		} else {
			low = middle + 1;
		}
	}

	/* No program has the name. */
	return NULL;
}

/* Writes the names of the programs, one per line. */
static void
list_programs(
	FILE *stream)
{
	unsigned int index;

	/* Writes every name in the table's order. */
	for (index = 0; index < crunch_program_count; index++)
		fprintf(stream, "%s\n", crunch_programs[index].name);
}
