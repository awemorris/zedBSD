/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The echo and printf builtins (POSIX XCU echo, printf).
 *
 * echo is the XSI echo dash has: -n alone as the first operand, and the
 * backslash escapes in every operand.  printf applies its format again and
 * again while arguments are left; a width or precision given as * is taken
 * from the arguments and written into the conversion before it is made.
 */

#include "userland/base/sh/shell.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a conversion may be once its * are written out. */
#define SPEC_MAX 64

/*
 * The state of one printf: the arguments not yet used, the status so far,
 * and whether \c (in %b) asked for the output to stop.
 */
struct printf_state {
	char **arguments;
	int count;
	int used;
	int status;
	int stop;
};

static int echo_escape(const char **text);
static int echo_octal(const char **text, int digits_before);
static int printf_format(struct printf_state *state, const char *format);
static const char *printf_conversion(struct printf_state *state, const char *cursor);
static const char *copy_digits(const char *cursor, char *spec, size_t *out);
static void write_star(struct printf_state *state, char *spec, size_t *out, int precision);
static int convert(struct printf_state *state, char *spec, size_t out, char conversion);
static void convert_string(struct printf_state *state, const char *spec, char conversion);
static const char *printf_next(struct printf_state *state);
static intmax_t printf_integer(struct printf_state *state, const char *text);
static uintmax_t printf_unsigned(struct printf_state *state, const char *text);
static double printf_double(struct printf_state *state, const char *text);
static void number_error(struct printf_state *state, const char *text, const char *end, int error);
static char *expand_b(struct printf_state *state, const char *text);
static size_t read_escape(const char *text, char *value, int in_b);
static int is_flag(char value);

/*
 * Implements echo.
 */
int
sh_builtin_echo(
	int argc,
	char **argv)
{
	const char *text;
	int newline;
	int index;
	int stop;

	/* -n, alone and first, leaves out the newline. */
	newline = 1;
	index = 1;
	if (index < argc && argv[index][0] == '-' && argv[index][1] == 'n' &&
	    argv[index][2] == '\0') {
		newline = 0;
		index++;
	}

	/* Each operand, with its escapes, separated by spaces. */
	for (; index < argc; index++) {
		if (index > 2 - newline)
			putchar(' ');
		for (text = argv[index]; *text != '\0'; text++) {
			if (*text != '\\' || text[1] == '\0') {
				putchar(*text);
				continue;
			}

			/* The escape after the backslash. */
			text++;
			stop = echo_escape(&text);
			if (stop)
				return 0;
		}
	}

	/* The newline, unless -n. */
	if (newline)
		putchar('\n');

	/* Succeeded. */
	return 0;
}

/*
 * Implements printf: the format is applied to the arguments, again and
 * again while arguments are left.
 */
int
sh_builtin_printf(
	int argc,
	char **argv)
{
	struct printf_state state;
	int index;
	int used;

	/* printf format [argument...]; it takes no options but --. */
	index = 1;
	if (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		if (argv[index][1] != '-' || argv[index][2] != '\0') {
			fprintf(stderr, "printf: Illegal option %s\n",
				argv[index]);
			return 2;
		}

		/* -- ends the options. */
		index++;
	}

	/* The format is required. */
	if (index >= argc) {
		fprintf(stderr, "printf: usage: printf format [arg ...]\n");
		return 2;
	}

	/* The arguments after the format. */
	state.arguments = argv + index + 1;
	state.count = argc - index - 1;
	state.used = 0;
	state.status = 0;
	state.stop = 0;

	/* Applies the format until the arguments run out, or it uses none. */
	do {
		used = printf_format(&state, argv[index]);
		if (state.stop || used == 0)
			break;
	} while (state.used < state.count);

	/* Succeeded: 1 when an argument was not a number, 2 for a bad format. */
	return state.status;
}

/*
 * Writes the escape after a backslash in an echo operand; *text is at the
 * character after the backslash, and is left at the escape's last
 * character.  Returns 1 for \c, which ends the output.
 */
static int
echo_escape(
	const char **text)
{
	int value;

	/* Dispatches on the escape character. */
	switch (**text) {
	case 'a':
		putchar('\a');
		return 0;
	case 'b':
		putchar('\b');
		return 0;
	case 'c':
		return 1;
	case 'e':
		putchar('\033');
		return 0;
	case 'f':
		putchar('\f');
		return 0;
	case 'n':
		putchar('\n');
		return 0;
	case 'r':
		putchar('\r');
		return 0;
	case 't':
		putchar('\t');
		return 0;
	case 'v':
		putchar('\v');
		return 0;
	case '\\':
		putchar('\\');
		return 0;
	case '0':
		value = echo_octal(text, 0);
		putchar(value);
		return 0;
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
		value = echo_octal(text, 1);
		putchar(value);
		return 0;
	default:
		putchar('\\');
		putchar(**text);
		return 0;
	}
}

/*
 * Reads the octal digits of an echo escape: \0 and up to three digits, or
 * (digits_before 1) a digit and up to two more.  *text is left at the last.
 */
static int
echo_octal(
	const char **text,
	int digits_before)
{
	int value;
	int digits;

	/* The first digit counts when it is not the 0 of \0. */
	value = 0;
	if (digits_before)
		value = **text - '0';

	/* Up to three digits in all. */
	for (digits = digits_before;
	     digits < 3 && (*text)[1] >= '0' && (*text)[1] <= '7';
	     digits++) {
		(*text)++;
		value = value * 8 + (**text - '0');
	}

	/* Succeeded: the byte. */
	return value;
}

/*
 * Applies the format once.  Returns how many arguments it used (0 means it
 * has no conversion that takes one, and is applied once only).
 */
static int
printf_format(
	struct printf_state *state,
	const char *format)
{
	const char *cursor;
	size_t length;
	int before;
	char value;

	/* Copies ordinary characters and escapes, and converts at each %. */
	before = state->used;
	for (cursor = format; *cursor != '\0'; cursor++) {
		/* A backslash escape. */
		if (*cursor == '\\') {
			length = read_escape(cursor + 1, &value, 0);
			putchar(value);
			cursor += length;
			continue;
		}

		/* An ordinary character, or %% for a percent sign. */
		if (*cursor != '%') {
			putchar(*cursor);
			continue;
		}

		/* %% is a percent sign. */
		if (cursor[1] == '%') {
			putchar('%');
			cursor++;
			continue;
		}

		/* A conversion; a bad one stops the output. */
		cursor = printf_conversion(state, cursor);
		if (state->stop)
			return state->used - before;
	}

	/* Succeeded: the arguments this pass used. */
	return state->used - before;
}

/*
 * Makes one conversion; cursor is at its %.  Returns where it ended (its
 * conversion character).
 */
static const char *
printf_conversion(
	struct printf_state *state,
	const char *cursor)
{
	char spec[SPEC_MAX];
	const char *start;
	size_t out;
	char conversion;
	int converted;
	int flag;

	/* The flags. */
	start = cursor;
	out = 0;
	spec[out++] = '%';
	cursor++;
	for (;;) {
		flag = is_flag(*cursor);
		if (!flag || out >= SPEC_MAX - 24)
			break;
		spec[out++] = *cursor++;
	}

	/* The width: digits, or * from the arguments. */
	if (*cursor == '*') {
		write_star(state, spec, &out, 0);
		cursor++;
	} else {
		cursor = copy_digits(cursor, spec, &out);
	}

	/* The precision: . and digits, or .* from the arguments. */
	if (*cursor == '.') {
		spec[out++] = '.';
		cursor++;
		if (*cursor == '*') {
			write_star(state, spec, &out, 1);
			cursor++;
		} else {
			cursor = copy_digits(cursor, spec, &out);
		}
	}

	/* The conversion character; a missing one is an error. */
	conversion = *cursor;
	if (conversion == '\0') {
		fprintf(stderr, "printf: %%: missing format character\n");
		state->status = 2;
		state->stop = 1;
		return cursor - 1;
	}

	/* The conversion with its argument. */
	converted = convert(state, spec, out, conversion);

	/* An unknown conversion is named as the format wrote it. */
	if (!converted) {
		fprintf(stderr, "printf: %.*s: invalid directive\n",
			(int)(cursor - start + 1), start);
		state->status = 2;
		state->stop = 1;
	}

	/* Succeeded: the conversion character. */
	return cursor;
}

/* Copies the digits of a width or precision into a conversion. */
static const char *
copy_digits(
	const char *cursor,
	char *spec,
	size_t *out)
{
	/* The digits, as many as fit. */
	while (*cursor >= '0' && *cursor <= '9' && *out < SPEC_MAX - 24)
		spec[(*out)++] = *cursor++;

	/* Succeeded: after the digits. */
	return cursor;
}

/*
 * Writes the next argument into a conversion, where a * stood.  A negative
 * precision is as if none were given, so its . is taken back out.
 */
static void
write_star(
	struct printf_state *state,
	char *spec,
	size_t *out,
	int precision)
{
	char number[24];
	int value;
	int written;

	/* The argument, as an int, as printf's * takes it. */
	value = (int)printf_integer(state, printf_next(state));
	if (precision && value < 0) {
		(*out)--;
		return;
	}

	/* Written in decimal, when it fits. */
	written = snprintf(number, sizeof(number), "%d", value);
	if (written <= 0 || *out + (size_t)written > SPEC_MAX - 8)
		return;
	memcpy(spec + *out, number, (size_t)written);
	*out += (size_t)written;
}

/* Makes a conversion of one argument; returns 0 for an unknown one. */
static int
convert(
	struct printf_state *state,
	char *spec,
	size_t out,
	char conversion)
{
	const char *text;
	intmax_t signed_value;
	uintmax_t unsigned_value;
	double real_value;

	/* Dispatches on the conversion character. */
	switch (conversion) {
	case 'd':
	case 'i':
		spec[out++] = 'j';
		spec[out++] = conversion;
		spec[out] = '\0';
		signed_value = printf_integer(state, printf_next(state));
		printf(spec, signed_value);
		return 1;
	case 'o':
	case 'u':
	case 'x':
	case 'X':
		spec[out++] = 'j';
		spec[out++] = conversion;
		spec[out] = '\0';
		unsigned_value = printf_unsigned(state, printf_next(state));
		printf(spec, unsigned_value);
		return 1;
	case 'e':
	case 'E':
	case 'f':
	case 'F':
	case 'g':
	case 'G':
	case 'a':
	case 'A':
		spec[out++] = conversion;
		spec[out] = '\0';
		real_value = printf_double(state, printf_next(state));
		printf(spec, real_value);
		return 1;
	case 'c':
		spec[out++] = 'c';
		spec[out] = '\0';
		text = printf_next(state);
		printf(spec, text[0]);
		return 1;
	case 's':
	case 'b':
		spec[out++] = 's';
		spec[out] = '\0';
		convert_string(state, spec, conversion);
		return 1;
	default:
		break;
	}

	/* Not a conversion printf knows. */
	return 0;
}

/* Makes a %s or %b conversion; %b expands the argument's escapes. */
static void
convert_string(
	struct printf_state *state,
	const char *spec,
	char conversion)
{
	const char *text;

	/* The argument, with its escapes expanded for %b. */
	text = printf_next(state);
	if (conversion == 'b')
		text = expand_b(state, text);

	/* Written as a string. */
	printf(spec, text);
}

/* Returns the next argument, or "" when none is left. */
static const char *
printf_next(
	struct printf_state *state)
{
	/* Missing arguments are empty. */
	if (state->used >= state->count)
		return "";

	/* Succeeded: the argument. */
	return state->arguments[state->used++];
}

/* Reads a signed number: decimal, octal, hexadecimal, or 'c for a character. */
static intmax_t
printf_integer(
	struct printf_state *state,
	const char *text)
{
	char *end;
	intmax_t value;
	int error;

	/* 'c and "c are the character's value; an empty argument is 0. */
	if (text[0] == '\'' || text[0] == '"')
		return (unsigned char)text[1];
	if (*text == '\0')
		return 0;

	/* A number, all of it, within range. */
	errno = 0;
	value = strtoimax(text, &end, 0);
	error = errno;
	number_error(state, text, end, error);

	/* Succeeded: the value (clamped when out of range). */
	return value;
}

/* Reads an unsigned number (a negative one wraps, as strtoumax does). */
static uintmax_t
printf_unsigned(
	struct printf_state *state,
	const char *text)
{
	char *end;
	uintmax_t value;
	int error;

	/* 'c is the character's value; an empty argument is 0. */
	if (text[0] == '\'' || text[0] == '"')
		return (unsigned char)text[1];
	if (*text == '\0')
		return 0;

	/* A number, all of it, within range. */
	errno = 0;
	value = strtoumax(text, &end, 0);
	error = errno;
	number_error(state, text, end, error);

	/* Succeeded: the value (clamped when out of range). */
	return value;
}

/* Reads a floating-point number. */
static double
printf_double(
	struct printf_state *state,
	const char *text)
{
	char *end;
	double value;
	int error;

	/* 'c is the character's value; an empty argument is 0. */
	if (text[0] == '\'' || text[0] == '"')
		return (unsigned char)text[1];
	if (*text == '\0')
		return 0.0;

	/* A number, all of it. */
	errno = 0;
	value = strtod(text, &end);
	error = errno;
	number_error(state, text, end, error);

	/* Succeeded: the value. */
	return value;
}

/* Reports an argument that was not wholly a number, or out of range. */
static void
number_error(
	struct printf_state *state,
	const char *text,
	const char *end,
	int error)
{
	/* No number at all, or something after it. */
	if (end == text) {
		fprintf(stderr, "printf: %s: expected numeric value\n", text);
		state->status = 1;
		return;
	}

	/* Characters left after the number are reported too. */
	if (*end != '\0') {
		fprintf(stderr, "printf: %s: not completely converted\n", text);
		state->status = 1;
		return;
	}

	/* A number too large. */
	if (error == ERANGE) {
		fprintf(stderr, "printf: %s: %s\n", text, strerror(error));
		state->status = 1;
	}
}

/*
 * Expands the escapes of a %b argument into a temporary string.  \c ends
 * the argument and all output after it.
 */
static char *
expand_b(
	struct printf_state *state,
	const char *text)
{
	char *expanded;
	size_t out;
	size_t step;
	char value;

	/* The expansion is never longer than the argument. */
	expanded = sh_temp_own(sh_malloc(strlen(text) + 1U));
	out = 0;
	for (; *text != '\0'; text++) {
		/* An ordinary character. */
		if (*text != '\\') {
			expanded[out++] = *text;
			continue;
		}

		/* \c stops everything. */
		if (text[1] == 'c') {
			state->stop = 1;
			break;
		}

		/* Any other escape. */
		step = read_escape(text + 1, &value, 1);
		expanded[out++] = value;
		text += step;
	}

	/* The expanded text ends with a null. */
	expanded[out] = '\0';

	/* Succeeded: the text, freed with the command. */
	return expanded;
}

/*
 * Reads the escape after a backslash; sets *value and returns how many
 * characters it took (0 for a backslash that escapes nothing, which stands
 * for itself).  In %b, \0 takes up to three more octal digits; in the
 * format, \ takes up to three.
 */
static size_t
read_escape(
	const char *text,
	char *value,
	int in_b)
{
	size_t length;
	int digits;
	int number;

	/* Dispatches on the one-character escapes. */
	switch (*text) {
	case 'a':
		*value = '\a';
		return 1;
	case 'b':
		*value = '\b';
		return 1;
	case 'e':
		*value = '\033';
		return 1;
	case 'f':
		*value = '\f';
		return 1;
	case 'n':
		*value = '\n';
		return 1;
	case 'r':
		*value = '\r';
		return 1;
	case 't':
		*value = '\t';
		return 1;
	case 'v':
		*value = '\v';
		return 1;
	case '\\':
		*value = '\\';
		return 1;
	case '\'':
	case '"':
		if (in_b)
			break;
		*value = *text;
		return 1;
	default:
		break;
	}

	/* Anything but an octal digit leaves the backslash as it is. */
	if (*text < '0' || *text > '7') {
		*value = '\\';
		return 0;
	}

	/* In %b a leading 0 is not one of the digits. */
	length = 0;
	if (in_b && *text == '0') {
		text++;
		length++;
	}

	/* Up to three octal digits. */
	number = 0;
	for (digits = 0; digits < 3 && *text >= '0' && *text <= '7';
	     digits++) {
		number = number * 8 + (*text - '0');
		text++;
		length++;
	}

	/* The octal value is the character. */
	*value = (char)number;

	/* Succeeded: the characters the escape took. */
	return length;
}

/* Reports whether a character is a flag of a conversion. */
static int
is_flag(
	char value)
{
	/* The flags printf knows. */
	if (value == '-' || value == '+' || value == ' ' || value == '#' ||
	    value == '0')
		return 1;

	/* Not a flag. */
	return 0;
}
