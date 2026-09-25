/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_GETOPT_H
#define LIBC_GETOPT_H

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether a long option takes a value, and whether it must have one. */
#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
	const char *name;
	int has_arg;

	/*
	 * When flag is null the option's val is returned; otherwise val is
	 * stored through flag and zero is returned, which lets a table of
	 * options set variables without a switch over them.
	 */
	int *flag;
	int val;
};

/*
 * Reads options written in full, as --name or --name=value, alongside the
 * single-letter ones getopt reads.  An unambiguous abbreviation of a name
 * is accepted, as it is everywhere this call exists.
 *
 * Arguments are not reordered: the first operand ends the options, which is
 * what POSIX specifies for getopt and what this keeps to.
 */
int getopt_long(int, char *const *, const char *, const struct option *,
	int *);

/* The same, but a single dash may also introduce a name written in full. */
int getopt_long_only(int, char *const *, const char *, const struct option *,
	int *);

#ifdef __cplusplus
}
#endif

#endif
