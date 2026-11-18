/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes its arguments (POSIX XCU echo): the shell's echo builtin as a command of its own.
 * The builtin and the command are the same source
 * (userland/base/sh/printf.c), so they behave alike.
 *
 * As a command it also takes GNU's options, as /bin/echo does on GNU
 * systems (scripts run it with env or exec to get them): leading words made
 * of the letters n, e and E only.  -n leaves out the newline, -e keeps the
 * escapes (which the XSI echo interprets anyway) and -E writes the
 * operands as they are.  With POSIXLY_CORRECT set, only the builtin's -n
 * is an option.
 */

#include "userland/base/sh/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int option_word(const char *word);
static void write_plain(int argc, char **argv, int first, int newline);

/*
 * Runs the command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *posix;
	const char *letter;
	int newline;
	int escapes;
	int index;
	int option;
	int status;

	/* GNU's options, unless POSIXLY_CORRECT keeps the builtin's alone. */
	newline = 1;
	escapes = 1;
	index = 1;
	posix = getenv("POSIXLY_CORRECT");
	while (posix == NULL && index < argc) {
		option = option_word(argv[index]);
		if (!option)
			break;

		/* Each letter of the word. */
		for (letter = argv[index] + 1; *letter != '\0'; letter++) {
			if (*letter == 'n')
				newline = 0;
			if (*letter == 'e')
				escapes = 1;
			if (*letter == 'E')
				escapes = 0;
		}

		/* The next word. */
		index++;
	}

	/* -E: the operands as they are. */
	if (!escapes) {
		write_plain(argc, argv, index, newline);
		fflush(stdout);
		return 0;
	}

	/*
	 * The builtin, over the words from the last option on: that word
	 * becomes the program's name, and with -n the one before it becomes
	 * -n (there is room: the options took at least one word).
	 */
	index--;
	argv[index] = argv[0];
	if (!newline) {
		argv[index] = (char *)"-n";
		index--;
		argv[index] = argv[0];
	}

	/* The operands, with their escapes. */
	status = sh_builtin_echo(argc - index, argv + index);
	fflush(stdout);

	/* Succeeded: the builtin's status. */
	return status;
}

/* Reports whether a word is one of GNU's option words: - and n, e, E only. */
static int
option_word(
	const char *word)
{
	const char *letter;

	/* A dash and at least one letter. */
	if (word[0] != '-' || word[1] == '\0')
		return 0;

	/* Only n, e and E. */
	for (letter = word + 1; *letter != '\0'; letter++) {
		if (*letter != 'n' && *letter != 'e' && *letter != 'E')
			return 0;
	}

	/* Succeeded: an option word. */
	return 1;
}

/* Writes the operands from first on, separated by spaces, as they are. */
static void
write_plain(
	int argc,
	char **argv,
	int first,
	int newline)
{
	int index;

	/* Each operand, after a space when one came before. */
	for (index = first; index < argc; index++) {
		if (index > first)
			putchar(' ');
		fputs(argv[index], stdout);
	}

	/* The newline, unless -n. */
	if (newline)
		putchar('\n');
}
