/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the last component of a pathname (POSIX XCU basename).
 *
 *	basename string [suffix]
 *	basename -a [-s suffix] [-z] string...	(GNU)
 *
 * Trailing slashes are dropped, then everything up to the last slash; a
 * suffix is removed when it is not the whole of what is left.  GNU's -a
 * (--multiple) takes every operand as a string, -s suffix (--suffix)
 * removes the suffix from each and implies -a, and -z (--zero) ends each
 * name with a NUL byte instead of a newline.  The options are read here,
 * not with the shared scanner, so that the file builds on its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the command line asks for. */
struct options {
	int multiple;
	const char *suffix;
	int end;
};

static int read_options(int argc, char **argv, struct options *options);
static int apply_long(int argc, char **argv, int index, struct options *options);
static int apply_letters(int argc, char **argv, int index, struct options *options);
static int long_option(const char *word, const char *name, const char **value);
static int write_base(const char *string, const char *suffix, int end);
static void usage(void);

/*
 * Runs the basename command.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	int first;
	int count;
	int index;
	int failed;
	int result;

	/* The options. */
	memset(&options, 0, sizeof(options));
	options.end = '\n';
	first = read_options(argc, argv, &options);
	count = argc - first;

	/* POSIX's form: a string and perhaps a suffix. */
	if (!options.multiple) {
		if (count != 1 && count != 2)
			usage();
		if (count == 2)
			options.suffix = argv[first + 1];
		result = write_base(argv[first], options.suffix, options.end);
		if (result != 0)
			return 1;
		return 0;
	}

	/* -a: every operand. */
	if (count < 1)
		usage();
	failed = 0;
	for (index = first; index < argc; index++) {
		result = write_base(argv[index], options.suffix, options.end);
		if (result != 0)
			failed = 1;
	}

	/* A name that could not be written. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads GNU's options (-a, -s suffix, -z and their long forms) before the
 * operands; -- ends them.  Returns the index of the first operand.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	int index;

	/* Each word that is an option. */
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends the options. */
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* A long option, or a run of letters. */
		if (word[1] == '-')
			index = apply_long(argc, argv, index, options);
		else
			index = apply_letters(argc, argv, index, options);
	}

	/* Succeeded: the first operand. */
	return index;
}

/*
 * Applies a long option (--multiple, --zero, --suffix[=value]); returns
 * the index of the last word it took.
 */
static int
apply_long(
	int argc,
	char **argv,
	int index,
	struct options *options)
{
	const char *value;
	int found;

	/* --multiple. */
	found = long_option(argv[index], "multiple", NULL);
	if (found) {
		options->multiple = 1;
		return index;
	}

	/* --zero. */
	found = long_option(argv[index], "zero", NULL);
	if (found) {
		options->end = '\0';
		return index;
	}

	/* --suffix=value, or --suffix and the next word. */
	found = long_option(argv[index], "suffix", &value);
	if (!found)
		usage();
	if (value == NULL) {
		if (index + 1 >= argc)
			usage();
		index++;
		value = argv[index];
	}

	/* Succeeded: the suffix, which implies -a. */
	options->suffix = value;
	options->multiple = 1;
	return index;
}

/*
 * Applies a run of letters (-a, -z, -s suffix); returns the index of the
 * last word it took.
 */
static int
apply_letters(
	int argc,
	char **argv,
	int index,
	struct options *options)
{
	const char *letter;
	const char *value;

	/* Each letter; -s takes the rest of the word or the next one. */
	for (letter = argv[index] + 1; *letter != '\0'; letter++) {
		switch (*letter) {
		case 'a':
			options->multiple = 1;
			break;
		case 'z':
			options->end = '\0';
			break;
		case 's':
			/* The suffix, which implies -a. */
			value = letter + 1;
			if (*value == '\0') {
				if (index + 1 >= argc)
					usage();
				index++;
				value = argv[index];
			}

			/* The rest of the word is the suffix's. */
			options->suffix = value;
			options->multiple = 1;
			return index;
		default:
			usage();
		}
	}

	/* Succeeded. */
	return index;
}

/*
 * Reports whether a word is --name or --name=value (value set to the text
 * after the =, or NULL).
 */
static int
long_option(
	const char *word,
	const char *name,
	const char **value)
{
	size_t length;
	int differs;

	/* --name, then its end or an =. */
	length = strlen(name);
	differs = strncmp(word + 2, name, length);
	if (differs != 0)
		return 0;
	if (value != NULL)
		*value = NULL;
	if (word[2 + length] == '\0')
		return 1;
	if (word[2 + length] != '=' || value == NULL)
		return 0;

	/* Succeeded: with a value. */
	*value = word + 3 + length;
	return 1;
}

/* Writes the base of a string, less a suffix.  Returns 0, or 1 when writing failed. */
static int
write_base(
	const char *string,
	const char *suffix,
	int end)
{
	char *copy;
	char *base;
	size_t length;
	size_t suffix_length;
	int written;
	int flushed;
	int same;

	/* A copy to cut. */
	length = strlen(string);
	copy = malloc(length + 1U);
	if (copy == NULL) {
		fprintf(stderr, "basename: out of memory\n");
		return 1;
	}

	/* The string itself. */
	memcpy(copy, string, length + 1U);

	/* The trailing slashes go; a name of slashes alone is /. */
	while (length > 1 && copy[length - 1U] == '/') {
		length--;
		copy[length] = '\0';
	}

	/* The part after the last slash. */
	base = strrchr(copy, '/');
	if (base == NULL)
		base = copy;
	else if (base[1] != '\0')
		base++;
	length = strlen(base);

	/* The suffix goes when it is there and not the whole base. */
	if (suffix != NULL) {
		suffix_length = strlen(suffix);
		if (suffix_length != 0 && suffix_length < length) {
			same = memcmp(base + length - suffix_length, suffix, suffix_length);
			if (same == 0)
				base[length - suffix_length] = '\0';
		}
	}

	/* The base and its end. */
	written = printf("%s%c", base, end);
	flushed = fflush(stdout);
	free(copy);
	if (written < 0 || flushed == EOF)
		return 1;

	/* Succeeded. */
	return 0;
}

/* Reports the usage and ends basename. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: basename string [suffix]\n"
		"       basename -a [-s suffix] [-z] string...\n");
	exit(1);
}
