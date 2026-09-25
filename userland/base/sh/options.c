/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shell's options, the set builtin, and the getopts builtin.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * An option: its long name and its letter (0 for an option with no letter).
 * One entry per option, in the order set -o lists them.
 */
struct option_name {
	const char *name;
	char letter;
};

/* The options, indexed as sh_option is, in the order dash lists them. */
static const struct option_name option_names[SH_OPT_COUNT] = {
	{ "errexit", 'e' },
	{ "noglob", 'f' },
	{ "ignoreeof", 0 },
	{ "interactive", 'i' },
	{ "monitor", 'm' },
	{ "noexec", 'n' },
	{ "stdin", 's' },
	{ "xtrace", 'x' },
	{ "verbose", 'v' },
	{ "vi", 0 },
	{ "emacs", 0 },
	{ "noclobber", 'C' },
	{ "allexport", 'a' },
	{ "notify", 'b' },
	{ "nounset", 'u' },
	{ "privileged", 'p' },
	{ "nolog", 0 },
	{ "pipefail", 0 },
	{ "debug", 0 },
	{ "hashall", 'h' },
};

/*
 * The value of each option: 0 off, 1 on.  Set at startup and by set; the
 * rest of the shell reads it.
 */
int sh_option[SH_OPT_COUNT];

/*
 * Where getopts is within the operand it is reading: the index of the next
 * letter, or 0 at the start of the next operand.  An assignment to OPTIND
 * resets it, except one getopts makes itself, which getopts_assigning marks
 * for the length of the assignment.
 */
static int getopts_offset;
static int getopts_assigning;

static int parse_word(int argc, char **argv, int *index, int startup);
static int option_by_letter(char letter);
static int option_by_name(const char *name);
static void set_option(int option, int value);
static void print_options(int as_commands);
static void set_getopts_var(const char *name, const char *value);
static int getopts_letter(const char *optstring, char **operands, int operand_count, int current, const char *name);
static int getopts_end(const char *name, int optind);
static int getopts_result(const char *name, const char *value, int optind);

/*
 * Returns the letters of the options that are set, which is what $- is.
 */
const char *
sh_option_letters(
	void)
{
	static char letters[SH_OPT_COUNT + 1];
	size_t count;
	int index;

	/* Collected in the reverse of the listing order, as dash does. */
	count = 0;
	for (index = SH_OPT_COUNT - 1; index >= 0; index--) {
		if (!sh_option[index] || option_names[index].letter == 0)
			continue;
		if (index == SH_OPT_HASHALL)
			continue;
		letters[count++] = option_names[index].letter;
	}

	/* The letters end with a null. */
	letters[count] = '\0';

	/* Succeeded: the letters. */
	return letters;
}

/*
 * Reads options from argv starting at *index: -abc and +abc, -o name and
 * +o name.  At startup the letters c and l are accepted too (the caller
 * looks at them).  A lone + is ignored.  Stops at the first operand, at "--"
 * (consumed) and at "-" (consumed; it turns x and v off).  Returns 0, or 2
 * after a bad option; *index is left at the first operand.
 */
int
sh_options_parse(
	int argc,
	char **argv,
	int *index,
	int startup)
{
	const char *word;
	int parsed;

	/* Each word that starts with - or +. */
	for (; *index < argc; (*index)++) {
		word = argv[*index];

		/* A lone + is an option word with no letters. */
		if (word[0] == '+' && word[1] == '\0')
			continue;

		/* A lone - ends the options and turns x and v off. */
		if (word[0] == '-' && word[1] == '\0') {
			sh_option[SH_OPT_XTRACE] = 0;
			sh_option[SH_OPT_VERBOSE] = 0;
			(*index)++;
			break;
		}

		/* An operand ends the options; so does --, which is consumed. */
		if (word[0] != '-' && word[0] != '+')
			break;
		if (word[0] == '-' && word[1] == '-' && word[2] == '\0') {
			(*index)++;
			break;
		}

		/* The letters of the word. */
		parsed = parse_word(argc, argv, index, startup);
		if (parsed != 0)
			return parsed;
	}

	/* Succeeded: every option word was read. */
	return 0;
}

/*
 * Implements the set builtin: options, then the positional parameters.
 */
int
sh_set_builtin(
	int argc,
	char **argv)
{
	int index;
	int status;
	int explicit;

	/* Without operands, lists the variables. */
	if (argc < 2) {
		sh_var_print(0, NULL);
		return 0;
	}

	/* The options; an error in them ends a shell that is not interactive. */
	index = 1;
	status = sh_options_parse(argc, argv, &index, 0);
	if (status != 0)
		sh_raise_error(2);

	/* "--" replaces the positional parameters even with nothing after it. */
	explicit = 0;
	if (index > 1) {
		if (argv[index - 1][0] == '-' && argv[index - 1][1] == '-' &&
		    argv[index - 1][2] == '\0')
			explicit = 1;
	}

	/* Monitor mode takes effect at once. */
	sh_job_control_init();

	/*
	 * The operands, or "--", replace the positional parameters, and
	 * getopts starts again at the first.
	 */
	if (index < argc || explicit) {
		sh_parameters_set(argc - index, argv + index);
		(void)sh_var_set("OPTIND", "1", 0);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Implements the getopts builtin.
 *
 * OPTIND is the index of the next operand to read; while an operand holding
 * several letters (-ab) is being read, OPTIND is already past it, and where
 * getopts is within it is kept here.
 */
int
sh_getopts_builtin(
	int argc,
	char **argv)
{
	const char *optind_text;
	const char *word;
	char **operands;
	size_t length;
	int operand_count;
	int current;
	int status;

	/* getopts optstring name [arg...] */
	if (argc < 3) {
		fprintf(stderr, "getopts: usage: getopts optstring var "
			"[arg...]\n");
		return 2;
	}

	/* The operands: those given, or the positional parameters. */
	if (argc > 3) {
		operands = argv + 3;
		operand_count = argc - 3;
	} else {
		operands = sh_parameters.values;
		operand_count = sh_parameters.count;
	}

	/* The operand being read: OPTIND's, or the one before it mid-word. */
	optind_text = sh_var_get("OPTIND");
	current = 1;
	if (optind_text != NULL)
		current = atoi(optind_text);
	if (current < 1)
		current = 1;
	if (getopts_offset != 0) {
		current--;
		length = 0;
		if (current >= 1 && current <= operand_count)
			length = strlen(operands[current - 1]);
		if ((size_t)getopts_offset >= length) {
			getopts_offset = 0;
			current++;
		}
	}

	/* The end: no operand, a non-option, "-", or "--" (consumed). */
	if (getopts_offset == 0) {
		if (current > operand_count) {
			status = getopts_end(argv[2], current);
			return status;
		}

		/* An operand that is no option, or --, ends the options. */
		word = operands[current - 1];
		if (word[0] != '-' || word[1] == '\0') {
			status = getopts_end(argv[2], current);
			return status;
		}

		/* -- ends the options and is skipped. */
		if (word[1] == '-' && word[2] == '\0') {
			status = getopts_end(argv[2], current + 1);
			return status;
		}

		/* The letters of the operand start after its dash. */
		getopts_offset = 1;
	}

	/* The letter at the offset. */
	status = getopts_letter(argv[1], operands, operand_count, current,
				argv[2]);

	/* Succeeded: the status of reading the letter. */
	return status;
}

/*
 * Tells getopts that OPTIND was assigned: it starts again at the operand
 * OPTIND names.  OPTIND must be a number.
 */
void
sh_getopts_reset(
	const char *name)
{
	const char *value;
	const char *cursor;

	/* An assignment by getopts itself changes nothing. */
	(void)name;
	if (getopts_assigning)
		return;
	getopts_offset = 0;

	/* Any value but a string of digits is an error. */
	value = sh_var_get("OPTIND");
	if (value == NULL)
		return;
	for (cursor = value; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			sh_error("Illegal number: %s", value);
	}
}

/*
 * Reads the letters of one option word: sets each option it names, and
 * takes the next word as the name of each o.  Returns 0, or 2 after a bad
 * option.
 */
static int
parse_word(
	int argc,
	char **argv,
	int *index,
	int startup)
{
	const char *word;
	int value;
	int option;
	int position;

	/* - sets, + clears. */
	word = argv[*index];
	value = word[0] == '-';

	/* Each letter of the word. */
	for (position = 1; word[position] != '\0'; position++) {
		/* o alone lists the options (not at startup); o name sets one. */
		if (word[position] == 'o') {
			if (*index + 1 >= argc) {
				if (!startup)
					print_options(!value);
				continue;
			}

			/* The option's name is the next argument. */
			(*index)++;
			option = option_by_name(argv[*index]);
			if (option < 0) {
				fprintf(stderr, "set: Illegal option %co %s\n",
					word[0], argv[*index]);
				return 2;
			}

			/* The named option is set. */
			set_option(option, value);
			continue;
		}

		/* c and l are the caller's at startup. */
		if (startup && (word[position] == 'c' || word[position] == 'l'))
			continue;

		/* Any other letter names an option. */
		option = option_by_letter(word[position]);
		if (option < 0) {
			fprintf(stderr, "set: Illegal option %c%c\n", word[0],
				word[position]);
			return 2;
		}

		/* The option the letter names is set. */
		set_option(option, value);
	}

	/* Succeeded: every letter named an option. */
	return 0;
}

/* Finds an option by its letter. */
static int
option_by_letter(
	char letter)
{
	int index;

	/* Looks through the table. */
	for (index = 0; index < SH_OPT_COUNT; index++) {
		if (option_names[index].letter == letter)
			return index;
	}

	/* No option has that letter. */
	return -1;
}

/* Finds an option by its long name. */
static int
option_by_name(
	const char *name)
{
	int index;
	int compare;

	/* Looks through the table. */
	for (index = 0; index < SH_OPT_COUNT; index++) {
		compare = strcmp(option_names[index].name, name);
		if (compare == 0)
			return index;
	}

	/* No option has that name. */
	return -1;
}

/*
 * Sets an option.  vi and emacs are the two line-editing modes, and turning
 * one on turns the other off, as ksh and bash do.
 */
static void
set_option(
	int option,
	int value)
{
	/* The option itself. */
	sh_option[option] = value;

	/* The other editing mode goes off when one comes on. */
	if (option == SH_OPT_VI && value)
		sh_option[SH_OPT_EMACS] = 0;
	else if (option == SH_OPT_EMACS && value)
		sh_option[SH_OPT_VI] = 0;
}

/* Prints the options: as a table (set -o), or as commands (set +o). */
static void
print_options(
	int as_commands)
{
	int index;

	/* The table has a heading. */
	if (!as_commands)
		printf("Current option settings\n");

	/* Every option but the hidden one. */
	for (index = 0; index < SH_OPT_COUNT; index++) {
		if (index == SH_OPT_HASHALL)
			continue;
		if (as_commands && sh_option[index])
			printf("set -o %s\n", option_names[index].name);
		else if (as_commands)
			printf("set +o %s\n", option_names[index].name);
		else if (sh_option[index])
			printf("%-16son\n", option_names[index].name);
		else
			printf("%-16soff\n", option_names[index].name);
	}
}

/* Sets a variable for getopts, reporting a read-only one. */
static void
set_getopts_var(
	const char *name,
	const char *value)
{
	int set;

	/* getopts's own OPTIND does not reset it. */
	getopts_assigning = 1;
	set = sh_var_set(name, value, 0);
	getopts_assigning = 0;
	if (set != 0)
		sh_error("%s: is read only", name);
}

/*
 * Reads the letter at getopts_offset of the operand current, and its
 * argument when it takes one.
 */
static int
getopts_letter(
	const char *optstring,
	char **operands,
	int operand_count,
	int current,
	const char *name)
{
	const char *spec;
	const char *word;
	char name_value[2];
	int next;
	int silent;
	int status;
	char letter;

	/* The letter; OPTIND moves past this operand. */
	word = operands[current - 1];
	letter = word[getopts_offset++];
	if (word[getopts_offset] == '\0')
		getopts_offset = 0;
	next = current + 1;
	name_value[0] = letter;
	name_value[1] = '\0';
	silent = optstring[0] == ':';

	/* A letter the string does not name. */
	spec = NULL;
	if (letter != ':')
		spec = strchr(optstring, letter);
	if (spec == NULL) {
		if (silent) {
			set_getopts_var("OPTARG", name_value);
		} else {
			fprintf(stderr, "Illegal option -%c\n", letter);
			(void)sh_var_unset("OPTARG");
		}

		/* The name is ? for an unknown option. */
		status = getopts_result(name, "?", next);
		return status;
	}

	/* A letter that takes no argument leaves OPTARG empty. */
	if (spec[1] != ':') {
		set_getopts_var("OPTARG", "");
		status = getopts_result(name, name_value, next);
		return status;
	}

	/* The argument: the rest of the operand, or the next operand. */
	if (getopts_offset != 0) {
		set_getopts_var("OPTARG", word + getopts_offset);
		getopts_offset = 0;
		status = getopts_result(name, name_value, next);
		return status;
	}

	/* The argument is the next operand. */
	if (next <= operand_count) {
		set_getopts_var("OPTARG", operands[next - 1]);
		status = getopts_result(name, name_value, next + 1);
		return status;
	}

	/* The argument is missing: : in silent mode, ? with a message. */
	if (silent) {
		set_getopts_var("OPTARG", name_value);
		status = getopts_result(name, ":", next);
		return status;
	}

	/* A missing argument is reported, and the name is ?. */
	fprintf(stderr, "No arg for -%c option\n", letter);
	(void)sh_var_unset("OPTARG");
	status = getopts_result(name, "?", next);

	/* Succeeded: the missing argument was reported as ?. */
	return status;
}

/* Ends getopts: OPTIND at the first operand, the name ?, status 1. */
static int
getopts_end(
	const char *name,
	int optind)
{
	char text[24];
	int valid;

	/* OPTIND names the first operand. */
	getopts_offset = 0;
	snprintf(text, sizeof(text), "%d", optind);
	set_getopts_var("OPTIND", text);

	/* A bad name fails after OPTIND is set. */
	valid = sh_var_name(name);
	if (!valid) {
		fprintf(stderr, "getopts: %s: bad variable name\n", name);
		return 2;
	}

	/* The name is ? at the end. */
	set_getopts_var(name, "?");

	/* Succeeded: the end of the options. */
	return 1;
}

/* Finishes a getopts that read an option: OPTIND, then the name. */
static int
getopts_result(
	const char *name,
	const char *value,
	int optind)
{
	char text[24];
	int valid;

	/* OPTIND moves on. */
	snprintf(text, sizeof(text), "%d", optind);
	set_getopts_var("OPTIND", text);

	/* A bad name fails after OPTIND is set. */
	valid = sh_var_name(name);
	if (!valid) {
		fprintf(stderr, "getopts: %s: bad variable name\n", name);
		return 2;
	}

	/* The name gets the letter. */
	set_getopts_var(name, value);

	/* Succeeded: an option was read. */
	return 0;
}
