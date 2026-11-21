/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the programs built into the single program.
 *
 * The build writes the table (crunch-table.c in the package's build
 * directory) from the programs the configuration puts in the single
 * program; main.c reads it.
 */

#ifndef USERLAND_BASE_CRUNCH_CRUNCH_H
#define USERLAND_BASE_CRUNCH_CRUNCH_H

/*
 * One program built into the single program.
 *
 * The name is the command's name, which is also the name of its link to
 * the single program, and the entry is the program's own main, renamed
 * when the program was put in.  A main that takes fewer arguments is
 * called the same way, as the C startup calls it.
 */
struct crunch_program {
	const char *name;
	int (*entry)(int argc, char **argv, char **envp);
};

/*
 * The programs, in the byte order of their names (strcmp), so that a name
 * is found by halving the table.
 */
extern const struct crunch_program crunch_programs[];

/* How many programs the table has. */
extern const unsigned int crunch_program_count;

#endif
