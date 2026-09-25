/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The umask and ulimit builtins: the file mode creation mask (POSIX XCU
 * umask) and the resource limits (ulimit, with the options and words dash
 * has).
 */

#include "userland/base/sh/shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* A kind of resource limit ulimit sets, with the unit it is written in. */
struct ulimit_kind {
	char option;
	int resource;
	rlim_t scale;
	const char *label;
};

/* ulimit's choices: which limit, which of its values, and whether all. */
struct ulimit_request {
	const struct ulimit_kind *kind;
	int hard;
	int soft;
	int all;
};

/* The resource limits ulimit knows, in the order and words dash lists them. */
static const struct ulimit_kind ulimit_kinds[] = {
	{ 't', RLIMIT_CPU, 1U, "time(seconds)" },
	{ 'f', RLIMIT_FSIZE, 512U, "file(blocks)" },
	{ 'd', RLIMIT_DATA, 1024U, "data(kbytes)" },
	{ 's', RLIMIT_STACK, 1024U, "stack(kbytes)" },
	{ 'c', RLIMIT_CORE, 512U, "coredump(blocks)" },
#ifdef RLIMIT_RSS
	{ 'm', RLIMIT_RSS, 1024U, "memory(kbytes)" },
#endif
#ifdef RLIMIT_MEMLOCK
	{ 'l', RLIMIT_MEMLOCK, 1024U, "locked memory(kbytes)" },
#endif
#ifdef RLIMIT_NPROC
	{ 'p', RLIMIT_NPROC, 1U, "process" },
#endif
	{ 'n', RLIMIT_NOFILE, 1U, "nofiles" },
	{ 'v', RLIMIT_AS, 1024U, "vmemory(kbytes)" },
#ifdef RLIMIT_LOCKS
	{ 'w', RLIMIT_LOCKS, 1U, "locks" },
#endif
#ifdef RLIMIT_RTPRIO
	{ 'r', RLIMIT_RTPRIO, 1U, "rtprio" },
#endif
	{ 0, 0, 0, NULL }
};

static void umask_print_symbolic(mode_t mask);
static void umask_print_class(const char *prefix, mode_t allowed, int shift);
static int umask_octal(const char *text);
static int umask_symbolic(const char *text, mode_t *mask);
static mode_t umask_who(const char **text);
static mode_t umask_permissions(const char **text, mode_t original);
static mode_t umask_apply(char op, mode_t allowed, mode_t who, mode_t permissions);
static int ulimit_options(int argc, char **argv, int *index, struct ulimit_request *request);
static void ulimit_list(const struct ulimit_request *request);
static int ulimit_set(const char *text, struct ulimit_request *request, struct rlimit *limit);
static const struct ulimit_kind *ulimit_find(char option);
static rlim_t ulimit_shown(const struct rlimit *limit, const struct ulimit_request *request);
static void ulimit_print(rlim_t value, rlim_t scale);

/*
 * Implements umask: prints the mask (octal, or -S symbolic), or sets it
 * from octal or from a symbolic mode.
 */
int
sh_builtin_umask(
	int argc,
	char **argv)
{
	mode_t mask;
	int symbolic;
	int index;
	int valid;

	/* -S prints symbolically; -- ends the options. */
	symbolic = 0;
	index = 1;
	if (index < argc && argv[index][0] == '-' && argv[index][1] == 'S' &&
	    argv[index][2] == '\0') {
		symbolic = 1;
		index++;
	}

	/* -- ends the options. */
	if (index < argc && argv[index][0] == '-' && argv[index][1] == '-' &&
	    argv[index][2] == '\0')
		index++;

	/* Any other option is an error. */
	if (index < argc && argv[index][0] == '-') {
		fprintf(stderr, "umask: Illegal option %s\n", argv[index]);
		return 2;
	}

	/* The mask, which can only be read by setting it. */
	mask = umask(0);
	(void)umask(mask);

	/* With no operand, prints it. */
	if (index >= argc && symbolic) {
		umask_print_symbolic(mask);
		return 0;
	}

	/* Otherwise in octal. */
	if (index >= argc) {
		printf("%04o\n", (unsigned int)mask);
		return 0;
	}

	/* An empty operand changes nothing. */
	if (argv[index][0] == '\0')
		return 0;

	/* An octal mask. */
	if (argv[index][0] >= '0' && argv[index][0] <= '7')
		return umask_octal(argv[index]);

	/* A symbolic mode of what is allowed. */
	valid = umask_symbolic(argv[index], &mask);
	if (!valid) {
		fprintf(stderr, "umask: Illegal mode: %s\n", argv[index]);
		return 2;
	}

	/* The mask is set. */
	(void)umask(mask);

	/* Succeeded. */
	return 0;
}

/*
 * Implements ulimit: ulimit [-HSa] [-cdfnstv...] [limit].  -H and -S choose
 * the hard or soft limit (a new limit sets both when neither is given).
 */
int
sh_builtin_ulimit(
	int argc,
	char **argv)
{
	struct ulimit_request request;
	struct rlimit limit;
	int index;
	int valid;
	int error;

	/* The options. */
	valid = ulimit_options(argc, argv, &index, &request);
	if (!valid)
		return 2;

	/* One limit at most, none with -a, and not -H and -S at once. */
	if (argc - index > 1) {
		fprintf(stderr, "ulimit: too many arguments\n");
		return 2;
	}

	/* An operand with -a, or with both -H and -S, is one too many. */
	if (index < argc && (request.all || (request.hard && request.soft))) {
		fprintf(stderr, "ulimit: too many arguments\n");
		return 2;
	}

	/* -a lists every limit. */
	if (request.all) {
		ulimit_list(&request);
		return 0;
	}

	/* The limit selected. */
	error = getrlimit(request.kind->resource, &limit);
	if (error != 0) {
		fprintf(stderr, "ulimit: %s\n", strerror(errno));
		return 1;
	}

	/* With no operand, printed. */
	if (index >= argc) {
		ulimit_print(ulimit_shown(&limit, &request),
			     request.kind->scale);
		return 0;
	}

	/* Succeeded: otherwise set. */
	return ulimit_set(argv[index], &request, &limit);
}

/* Prints a mask as what it allows: u=rwx,g=rx,o=rx. */
static void
umask_print_symbolic(
	mode_t mask)
{
	mode_t allowed;

	/* The three classes, each from its three bits. */
	allowed = ~mask & 0777;
	umask_print_class("u=", allowed, 6);
	umask_print_class(",g=", allowed, 3);
	umask_print_class(",o=", allowed, 0);
	putchar('\n');
}

/* Prints one class of a symbolic mask. */
static void
umask_print_class(
	const char *prefix,
	mode_t allowed,
	int shift)
{
	mode_t bits;

	/* The prefix, then r, w and x as they are allowed. */
	fputs(prefix, stdout);
	bits = (allowed >> shift) & 7;
	if ((bits & 4) != 0)
		putchar('r');
	if ((bits & 2) != 0)
		putchar('w');
	if ((bits & 1) != 0)
		putchar('x');
}

/* Sets the mask from an octal number (bits past 0777 are dropped). */
static int
umask_octal(
	const char *text)
{
	char *end;
	long value;

	/* Octal digits and nothing else. */
	value = strtol(text, &end, 8);
	if (*end != '\0') {
		fprintf(stderr, "umask: Illegal number: %s\n", text);
		return 2;
	}

	/* Succeeded: set. */
	(void)umask((mode_t)(value & 0777));
	return 0;
}

/*
 * Applies a symbolic mode (as chmod reads one: [ugoa]*[+-=][rwxXst]* or
 * [+-=][ugo], clauses joined by commas) to what a mask allows.  u, g and o
 * after the operator copy what that class was allowed before the mode.
 */
static int
umask_symbolic(
	const char *text,
	mode_t *mask)
{
	mode_t original;
	mode_t allowed;
	mode_t who;
	mode_t permissions;
	char op;

	/* Works on what is allowed, the complement of the mask. */
	original = ~*mask & 0777;
	allowed = original;
	for (;;) {
		/* Who: u, g, o, a (none means all). */
		who = umask_who(&text);

		/* One or more operations. */
		if (*text != '+' && *text != '-' && *text != '=')
			return 0;
		while (*text == '+' || *text == '-' || *text == '=') {
			op = *text;
			text++;
			permissions = umask_permissions(&text, original);
			allowed = umask_apply(op, allowed, who,
					      permissions & who);
		}

		/* The end, or a comma and another clause (or nothing). */
		if (*text == '\0')
			break;
		if (*text != ',')
			return 0;
		text++;
		if (*text == '\0')
			break;
	}

	/* Succeeded: the mask of what is not allowed. */
	*mask = ~allowed & 0777;
	return 1;
}

/* Reads the classes a clause applies to; none means all. */
static mode_t
umask_who(
	const char **text)
{
	mode_t who;

	/* Each of u, g, o and a adds its bits. */
	who = 0;
	for (;
	     ;
	     (*text)++) {
		/* Each class letter adds its bits. */
		switch (**text) {
		case 'u':
			who |= 0700;
			continue;
		case 'g':
			who |= 070;
			continue;
		case 'o':
			who |= 07;
			continue;
		case 'a':
			who |= 0777;
			continue;
		default:
			break;
		}

		break;
	}

	/* No class is every class. */
	if (who == 0)
		who = 0777;

	/* Succeeded. */
	return who;
}

/*
 * Reads the permissions after an operator: a class to copy (from what was
 * allowed before the mode), or the letters rwxXst.  X is x when anyone
 * could execute before; s and t mean nothing to a mask.
 */
static mode_t
umask_permissions(
	const char **text,
	mode_t original)
{
	mode_t permissions;
	mode_t copied;

	/* A class, copied to every class. */
	copied = 8;
	if (**text == 'u')
		copied = (original >> 6) & 7;
	else if (**text == 'g')
		copied = (original >> 3) & 7;
	else if (**text == 'o')
		copied = original & 7;
	if (copied != 8) {
		(*text)++;
		return copied | (copied << 3) | (copied << 6);
	}

	/* The permission letters. */
	permissions = 0;
	for (;; (*text)++) {
		switch (**text) {
		case 'r':
			permissions |= 0444;
			continue;
		case 'w':
			permissions |= 0222;
			continue;
		case 'x':
			permissions |= 0111;
			continue;
		case 'X':
			if ((original & 0111) != 0)
				permissions |= 0111;
			continue;
		case 's':
			continue;
		default:
			break;
		}

		break;
	}

	/* Succeeded. */
	return permissions;
}

/* Applies one operation to what is allowed. */
static mode_t
umask_apply(
	char op,
	mode_t allowed,
	mode_t who,
	mode_t permissions)
{
	/* + adds, - takes away. */
	if (op == '+')
		return allowed | permissions;
	if (op == '-')
		return allowed & ~permissions;

	/* = replaces the classes'. */
	return (allowed & ~who) | permissions;
}

/*
 * Reads ulimit's options; *index is left at the operand.  Returns 0 (after
 * a message) for an unknown option.
 */
static int
ulimit_options(
	int argc,
	char **argv,
	int *index,
	struct ulimit_request *request)
{
	const struct ulimit_kind *selected;
	const char *word;
	const char *option;

	/* The file size limit, unless another is chosen. */
	request->kind = ulimit_find('f');
	request->hard = 0;
	request->soft = 0;
	request->all = 0;

	/* Each option word, which may join several letters. */
	for (*index = 1; *index < argc; (*index)++) {
		/* An operand, or - alone, ends the options. */
		word = argv[*index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends them too. */
		if (word[1] == '-' && word[2] == '\0') {
			(*index)++;
			break;
		}

		/* Each letter: H, S, a, or a limit. */
		for (option = word + 1; *option != '\0'; option++) {
			if (*option == 'H') {
				request->hard = 1;
				continue;
			}

			/* S: the soft limit. */
			if (*option == 'S') {
				request->soft = 1;
				continue;
			}

			/* a: every limit. */
			if (*option == 'a') {
				request->all = 1;
				continue;
			}

			/* Any other letter names a limit. */
			selected = ulimit_find(*option);
			if (selected == NULL) {
				fprintf(stderr, "ulimit: Illegal option -%c\n",
					*option);
				return 0;
			}

			/* The limit to act on. */
			request->kind = selected;
		}
	}

	/* Succeeded. */
	return 1;
}

/* Lists every limit, with its label. */
static void
ulimit_list(
	const struct ulimit_request *request)
{
	const struct ulimit_kind *kind;
	struct rlimit limit;
	int error;

	/* Each limit the system reports. */
	for (kind = ulimit_kinds; kind->label != NULL; kind++) {
		error = getrlimit(kind->resource, &limit);
		if (error != 0)
			continue;
		printf("%-20s ", kind->label);
		ulimit_print(ulimit_shown(&limit, request), kind->scale);
	}
}

/* Sets a limit from its operand: a number in the limit's unit, or unlimited. */
static int
ulimit_set(
	const char *text,
	struct ulimit_request *request,
	struct rlimit *limit)
{
	unsigned long long value;
	rlim_t new_value;
	char *end;
	int compare;
	int error;

	/* unlimited, or a number. */
	compare = strcmp(text, "unlimited");
	if (compare == 0) {
		new_value = RLIM_INFINITY;
	} else {
		/* Digits only, within range. */
		errno = 0;
		value = strtoull(text, &end, 10);
		error = errno;
		if (text[0] < '0' || text[0] > '9' || *end != '\0' ||
		    error == ERANGE) {
			fprintf(stderr, "ulimit: Illegal number: %s\n", text);
			return 1;
		}

		/* The value in the limit's own unit. */
		new_value = (rlim_t)value * request->kind->scale;
	}

	/* Both values when neither -H nor -S. */
	if (!request->hard && !request->soft) {
		request->hard = 1;
		request->soft = 1;
	}

	/* The hard limit, then the soft one. */
	if (request->hard)
		limit->rlim_max = new_value;
	if (request->soft)
		limit->rlim_cur = new_value;

	/* Set. */
	error = setrlimit(request->kind->resource, limit);
	if (error != 0) {
		fprintf(stderr, "ulimit: error setting limit (%s)\n",
			strerror(errno));
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Finds a resource limit by its option letter. */
static const struct ulimit_kind *
ulimit_find(
	char option)
{
	const struct ulimit_kind *kind;

	/* Looks through the table. */
	for (kind = ulimit_kinds; kind->label != NULL; kind++) {
		if (kind->option == option)
			return kind;
	}

	/* No such limit. */
	return NULL;
}

/* Returns the value to print: the hard limit with -H alone, else the soft. */
static rlim_t
ulimit_shown(
	const struct rlimit *limit,
	const struct ulimit_request *request)
{
	/* -H without -S. */
	if (request->hard && !request->soft)
		return limit->rlim_max;

	/* The soft limit. */
	return limit->rlim_cur;
}

/* Prints a limit in the unit ulimit writes it in. */
static void
ulimit_print(
	rlim_t value,
	rlim_t scale)
{
	/* No limit. */
	if (value == RLIM_INFINITY) {
		printf("unlimited\n");
		return;
	}

	/* The number. */
	printf("%llu\n", (unsigned long long)(value / scale));
}
