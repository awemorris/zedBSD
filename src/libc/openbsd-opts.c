/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements options written in full, and file modes written as text.
 *
 * Written from what each interface is defined to do, not from another
 * system's source.
 */

#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Supports the match long option operation.
 *
 * Reports the option a name selects.  A name written in full wins outright;
 * otherwise an abbreviation is accepted when exactly one option begins with
 * it, because two would leave the caller's meaning unknown.
 */
static int
match_long_option(
	const struct option *options,
	const char *name,
	size_t length,
	int *ambiguous)
{
	int candidate;
	int index;

	candidate = -1;
	*ambiguous = 0;

	/* Process each element required by the operation. */
	for (index = 0; options != NULL && options[index].name != NULL;
	     index++) {
		/* Skips a name this one is not the beginning of. */
		if (strncmp(options[index].name, name, length) != 0)
			continue;

		/* A name written in full needs no further choosing. */
		if (options[index].name[length] == '\0')
			return index;

		/* Handles a second option the abbreviation also fits. */
		if (candidate >= 0) {
			*ambiguous = 1;
			return -1;
		}
		candidate = index;
	}

	/* Returns the computed result. */
	return candidate;
}

/*
 * Where the next single letter is, inside a run such as -abc.
 *
 * The single letters are read here rather than by handing the argument to
 * getopt, because how far getopt has advanced optind in the middle of a run
 * is its own business and cannot be told from outside.
 */
static const char *option_cursor;

/*
 * Supports the short option operation.
 *
 * Reads one letter from the run the cursor is in, taking its value from the
 * rest of the run or from the next argument as the description requires.
 */
static int
short_option(
	int argc,
	char *const *argv,
	const char *short_options)
{
	const char *found;
	int letter;
	int quiet;

	/* A leading colon asks for a missing value to be reported as such. */
	quiet = short_options != NULL && short_options[0] == ':';
	letter = (unsigned char)*option_cursor++;
	found = short_options != NULL ?
		strchr(short_options + (quiet ? 1 : 0), letter) : NULL;

	/* Reports a letter this program does not accept. */
	if (found == NULL || letter == ':') {
		optopt = letter;
		if (*option_cursor == '\0') {
			option_cursor = NULL;
			optind++;
		}
		if (opterr != 0 && !quiet) {
			(void)fprintf(stderr, "%s: illegal option -- %c\n",
				      argv[0], letter);
		}

		/* Reports operation failure. */
		return '?';
	}

	/* A letter that takes no value leaves the run where it is. */
	if (found[1] != ':') {
		if (*option_cursor == '\0') {
			option_cursor = NULL;
			optind++;
		}

		/* Returns the computed result. */
		return letter;
	}

	/* The rest of the run is the value, when there is any left. */
	if (*option_cursor != '\0') {
		optarg = (char *)option_cursor;
		option_cursor = NULL;
		optind++;

		/* Returns the computed result. */
		return letter;
	}
	option_cursor = NULL;
	optind++;

	/* A value written as :: is never taken from the next argument. */
	if (found[2] == ':')
		return letter;

	/* Handles a value that was not supplied at all. */
	if (optind >= argc) {
		optopt = letter;
		if (opterr != 0 && !quiet) {
			(void)fprintf(stderr,
			    "%s: option requires an argument -- %c\n",
			    argv[0], letter);
		}

		/* Reports operation failure. */
		return quiet ? ':' : '?';
	}
	optarg = argv[optind];
	optind++;

	/* Returns the computed result. */
	return letter;
}

/*
 * Supports the getopt long internal operation.
 */
static int
getopt_long_internal(
	int argc,
	char *const *argv,
	const char *short_options,
	const struct option *long_options,
	int *index_result,
	int single_dash)
{
	const char *argument;
	const char *name;
	const char *equals;
	size_t length;
	int ambiguous;
	int index;
	int dashes;

	optarg = NULL;

	/*
	 * A caller starts a new scan either by asking for one or by setting
	 * the index back to where a scan begins.
	 */
	if (optreset != 0 || optind == 0) {
		optreset = 0;
		if (optind == 0)
			optind = 1;
		option_cursor = NULL;
	}

	/* Continues a run of single letters before looking at a new argument. */
	if (option_cursor != NULL && *option_cursor != '\0')
		return short_option(argc, argv, short_options);
	option_cursor = NULL;

	/* Handles a caller that has run out of arguments. */
	if (optind >= argc || argv == NULL || argv[optind] == NULL)
		return -1;
	argument = argv[optind];

	/* The first operand ends the options. */
	if (argument[0] != '-' || argument[1] == '\0')
		return -1;

	/* Two dashes alone end them explicitly, and are consumed. */
	if (argument[1] == '-' && argument[2] == '\0') {
		optind++;
		return -1;
	}

	/* Decides whether this argument carries a name written in full. */
	dashes = 0;
	if (argument[1] == '-')
		dashes = 2;
	else if (single_dash)
		dashes = 1;

	/*
	 * A lone letter that is a single-letter option is that option, not
	 * an abbreviation of every name beginning with it; otherwise the
	 * letter could never be given once two such names existed.
	 */
	if (dashes == 1 && argument[2] == '\0' && short_options != NULL &&
	    argument[1] != ':' && strchr(short_options, argument[1]) != NULL)
		dashes = 0;
	if (dashes == 0) {
		option_cursor = argument + 1;

		/* Returns the computed result. */
		return short_option(argc, argv, short_options);
	}

	/* Separates the name from a value written after an equals sign. */
	name = argument + dashes;
	equals = strchr(name, '=');
	length = equals != NULL ? (size_t)(equals - name) : strlen(name);
	index = match_long_option(long_options, name, length, &ambiguous);

	/*
	 * With one dash a name that matches nothing is a run of single
	 * letters after all, which is the whole of the difference.
	 */
	if (index < 0 && !ambiguous && dashes == 1) {
		option_cursor = argument + 1;

		/* Returns the computed result. */
		return short_option(argc, argv, short_options);
	}

	/* Reports a name that selects nothing, or more than one thing. */
	if (index < 0) {
		if (opterr != 0) {
			(void)fprintf(stderr, "%s: %s option -- %.*s\n",
				      argv[0],
				      ambiguous ? "ambiguous" : "unrecognized",
				      (int)length, name);
		}
		optind++;
		optopt = 0;

		/* Reports operation failure. */
		return '?';
	}
	optind++;

	/* Takes the value from after the equals sign, or the next argument. */
	if (long_options[index].has_arg == required_argument) {
		if (equals != NULL) {
			optarg = (char *)(equals + 1);
		} else if (optind < argc) {
			optarg = argv[optind];
			optind++;
		} else {
			if (opterr != 0) {
				(void)fprintf(stderr,
				    "%s: option requires an argument -- %s\n",
				    argv[0], long_options[index].name);
			}
			optopt = long_options[index].val;

			/* Reports operation failure. */
			return short_options != NULL &&
			       short_options[0] == ':' ? ':' : '?';
		}
	} else if (long_options[index].has_arg == optional_argument) {
		/*
		 * An optional value is only ever the text after the equals
		 * sign: the next argument could as easily be an operand.
		 */
		if (equals != NULL)
			optarg = (char *)(equals + 1);
	} else if (equals != NULL) {
		if (opterr != 0) {
			(void)fprintf(stderr,
			    "%s: option does not take an argument -- %s\n",
			    argv[0], long_options[index].name);
		}
		optopt = long_options[index].val;

		/* Reports operation failure. */
		return '?';
	}

	/* Handles the index result availability. */
	if (index_result != NULL)
		*index_result = index;

	/* A table with a flag sets it instead of reporting the option. */
	if (long_options[index].flag != NULL) {
		*long_options[index].flag = long_options[index].val;
		return 0;
	}

	/* Returns the computed result. */
	return long_options[index].val;
}

/*
 * Implements the getopt long operation.
 */
int
getopt_long(
	int argc,
	char *const *argv,
	const char *short_options,
	const struct option *long_options,
	int *index_result)
{
	/* Returns the computed result. */
	return getopt_long_internal(argc, argv, short_options, long_options,
				    index_result, 0);
}

/*
 * Implements the getopt long only operation.
 */
int
getopt_long_only(
	int argc,
	char *const *argv,
	const char *short_options,
	const struct option *long_options,
	int *index_result)
{
	/* Returns the computed result. */
	return getopt_long_internal(argc, argv, short_options, long_options,
				    index_result, 1);
}

/*
 * One step of a mode written as text.
 *
 * Each clause of a mode such as "u+rw,go-w,a=rx" becomes one of these, and
 * they are applied in the order they were written, because a later clause
 * may undo an earlier one.
 */
struct mode_step {
	mode_t who;
	mode_t bits;
	char action;

	/* 'u', 'g' or 'o' when the bits are taken from the file itself. */
	char copy;

	/* The X permission: execute, but only where execute already is. */
	unsigned conditional;
};

/* One compiled mode, ending in a step whose action is zero. */
#define MODE_STEPS_MAX 32

struct mode_program {
	struct mode_step steps[MODE_STEPS_MAX];
	unsigned count;
};

/* Every bit a mode written as text can name. */
#define MODE_ALL_BITS \
	(mode_t)(S_ISUID | S_ISGID | S_ISVTX | 0777)

/*
 * Supports the mode file creation mask operation.
 *
 * A clause that names nobody applies to everyone except the bits the file
 * creation mask withholds, which is what makes "+x" mean what a person
 * expects rather than granting more than the mask allows.
 */
static mode_t
mode_creation_mask(
	void)
{
	mode_t mask;

	mask = umask(0);
	(void)umask(mask);

	/* Returns the computed result. */
	return mask;
}

/*
 * Implements the setmode operation.
 *
 * Compiles the text once so that it can be applied to many files without
 * being parsed again, which is what a caller walking a tree needs.
 */
void *
setmode(
	const char *text)
{
	struct mode_program *program;
	struct mode_step *step;
	mode_t who;
	mode_t bits;
	mode_t value;
	char action;
	char copy;
	unsigned conditional;
	int saw_who;

	/* An absent mode asks for one that changes nothing. */
	program = calloc(1, sizeof(*program));
	if (program == NULL)
		return NULL;
	if (text == NULL)
		return program;

	/* A mode written as a number replaces every bit at once. */
	if (*text >= '0' && *text <= '7') {
		value = 0;
		while (*text >= '0' && *text <= '7') {
			value = (mode_t)((value << 3) |
					 (mode_t)(*text - '0'));
			text++;

			/* Rejects a number too large to be a mode. */
			if (value > MODE_ALL_BITS) {
				free(program);
				errno = EINVAL;
				return NULL;
			}
		}
		if (*text != '\0') {
			free(program);
			errno = EINVAL;
			return NULL;
		}
		step = &program->steps[program->count++];
		step->action = '=';
		step->who = MODE_ALL_BITS;
		step->bits = value;

		/* Returns the computed result. */
		return program;
	}

	/* Continue while the operation condition remains true. */
	for (;;) {
		/* Reads who the clause is about. */
		who = 0;
		saw_who = 0;
		for (;;) {
			if (*text == 'u')
				who |= (mode_t)(S_ISUID | 0700);
			else if (*text == 'g')
				who |= (mode_t)(S_ISGID | 0070);
			else if (*text == 'o')
				who |= (mode_t)(S_ISVTX | 0007);
			else if (*text == 'a')
				who |= MODE_ALL_BITS;
			else
				break;
			saw_who = 1;
			text++;
		}

		/* A clause that names nobody spares the creation mask. */
		if (!saw_who)
			who = MODE_ALL_BITS & ~mode_creation_mask();

		/* Continue while the operation condition remains true. */
		for (;;) {
			action = *text;
			if (action != '+' && action != '-' && action != '=') {
				free(program);
				errno = EINVAL;
				return NULL;
			}
			text++;

			/* Reads what is being granted or taken away. */
			bits = 0;
			copy = 0;
			conditional = 0;
			for (;;) {
				if (*text == 'r')
					bits |= (mode_t)0444;
				else if (*text == 'w')
					bits |= (mode_t)0222;
				else if (*text == 'x')
					bits |= (mode_t)0111;
				else if (*text == 'X')
					conditional = 1;
				else if (*text == 's')
					bits |= (mode_t)(S_ISUID | S_ISGID);
				else if (*text == 't')
					bits |= (mode_t)S_ISVTX;
				else if (*text == 'u' || *text == 'g' ||
					 *text == 'o') {
					/* The bits the file itself has. */
					if (bits != 0 || copy != 0) {
						free(program);
						errno = EINVAL;
						return NULL;
					}
					copy = *text;
				} else
					break;
				text++;
			}

			/* Handles a compiled mode with no room left. */
			if (program->count >= MODE_STEPS_MAX) {
				free(program);
				errno = EINVAL;
				return NULL;
			}
			step = &program->steps[program->count++];
			step->action = action;
			step->who = who;
			step->bits = bits & who;
			step->copy = copy;
			step->conditional = conditional;

			/* A further action applies to the same who. */
			if (*text != '+' && *text != '-' && *text != '=')
				break;
		}

		/* Clauses are separated by commas. */
		if (*text != ',')
			break;
		text++;
	}

	/* Rejects anything left over, which the caller did not mean. */
	if (*text != '\0') {
		free(program);
		errno = EINVAL;

		/* Reports operation failure. */
		return NULL;
	}

	/* Returns the computed result. */
	return program;
}

/*
 * Implements the getmode operation.
 */
mode_t
getmode(
	const void *compiled,
	mode_t mode)
{
	const struct mode_program *program;
	const struct mode_step *step;
	mode_t bits;
	mode_t source;
	unsigned index;

	program = compiled;

	/* Handles the program availability. */
	if (program == NULL)
		return mode;

	/* Process each remaining element. */
	for (index = 0; index < program->count; index++) {
		step = &program->steps[index];
		bits = step->bits;

		/* Takes the bits from the file's own u, g or o. */
		if (step->copy != 0) {
			if (step->copy == 'u')
				source = (mode & (mode_t)0700) >> 6;
			else if (step->copy == 'g')
				source = (mode & (mode_t)0070) >> 3;
			else
				source = mode & (mode_t)0007;
			bits = (mode_t)((source | (source << 3) |
					 (source << 6)) & step->who);
		}

		/*
		 * X grants execute only where the file is a directory or
		 * already executable by somebody, so that a mode applied to
		 * a whole tree does not make every ordinary file runnable.
		 */
		if (step->conditional != 0 &&
		    (S_ISDIR(mode) || (mode & (mode_t)0111) != 0))
			bits |= (mode_t)0111 & step->who;

		/* Dispatch the selected action. */
		if (step->action == '+')
			mode |= bits;
		else if (step->action == '-')
			mode &= (mode_t)~bits;
		else
			mode = (mode & ~step->who) | bits;
	}

	/* Returns the computed result. */
	return mode;
}
