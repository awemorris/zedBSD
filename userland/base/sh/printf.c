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
 * printf -v name (bash) puts the output in a variable instead, and %q
 * (bash) quotes its argument so that the shell reads it back.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
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

/* The output of printf -v, collected until printf ends. */
struct printf_capture {
	char *text;
	size_t length;
	size_t capacity;
};

/* The output printf -v collects, while collecting is set. */
static struct printf_capture printf_capture;
static int printf_collecting;

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
static const char *quote_q(const char *text);
static size_t read_escape(const char *text, char *value, int in_b);
static int is_flag(char value);
static void out_char(int value);
static void out_format(const char *spec, ...) __attribute__((format(printf, 1, 2)));
static void capture_add(const char *text, size_t length);

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
	const char *variable;
	int index;
	int used;
	int set;

	/* -v name (or -vname) puts the output in the variable. */
	index = 1;
	variable = NULL;
	if (index < argc && strncmp(argv[index], "-v", 2) == 0) {
		variable = argv[index] + 2;
		if (*variable == '\0' && index + 1 < argc)
			variable = argv[++index];
		index++;
		if (!sh_var_name(variable)) {
			fprintf(stderr, "printf: %s: not a valid identifier\n",
				variable);
			return 2;
		}
	}

	/* printf format [argument...]; otherwise it takes no options but --. */
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

	/* The output is collected for -v. */
	memset(&printf_capture, 0, sizeof(printf_capture));
	printf_collecting = variable != NULL;

	/* Applies the format until the arguments run out, or it uses none. */
	do {
		used = printf_format(&state, argv[index]);
		if (state.stop || used == 0)
			break;
	} while (state.used < state.count);

	/* -v: the variable takes what was collected (up to a NUL byte). */
	if (variable != NULL) {
		capture_add("", 1);
		printf_collecting = 0;
		set = sh_var_set(variable, printf_capture.text, 0);
		free(printf_capture.text);
		printf_capture.text = NULL;
		if (set != 0) {
			fprintf(stderr, "printf: %s: is read only\n", variable);
			return 1;
		}
	}

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
			out_char(value);
			cursor += length;
			continue;
		}

		/* An ordinary character, or %% for a percent sign. */
		if (*cursor != '%') {
			out_char(*cursor);
			continue;
		}

		/* %% is a percent sign. */
		if (cursor[1] == '%') {
			out_char('%');
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
		out_format(spec, signed_value);
		return 1;
	case 'o':
	case 'u':
	case 'x':
	case 'X':
		spec[out++] = 'j';
		spec[out++] = conversion;
		spec[out] = '\0';
		unsigned_value = printf_unsigned(state, printf_next(state));
		out_format(spec, unsigned_value);
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
		out_format(spec, real_value);
		return 1;
	case 'c':
		spec[out++] = 'c';
		spec[out] = '\0';
		text = printf_next(state);
		out_format(spec, text[0]);
		return 1;
	case 'q':
		spec[out++] = 's';
		spec[out] = '\0';
		out_format(spec, quote_q(printf_next(state)));
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
	out_format(spec, text);
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
 * Quotes a %q argument into a temporary string as bash does: '' for an
 * empty one, $'...' with escapes when it holds a control character, and
 * otherwise a backslash before each character the shell treats specially
 * (# and ~ only at the start).
 */
static const char *
quote_q(
	const char *text)
{
	static const char special[] = " !\"$&'()*,;<>?[\\]^`{|}";
	static const char escapes[] = "\a" "a" "\b" "b" "\033" "e" "\f" "f"
				      "\n" "n" "\r" "r" "\t" "t" "\v" "v";
	const char *cursor;
	const char *escape;
	char *quoted;
	size_t out;
	int control;

	/* Nothing is ''. */
	if (*text == '\0')
		return "''";

	/* At most four bytes for each, and $'' around them. */
	quoted = sh_temp_own(sh_malloc(strlen(text) * 4U + 4U));
	out = 0;
	control = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f)
			control = 1;
	}

	/* With a control character: $'...'. */
	if (control) {
		quoted[out++] = '$';
		quoted[out++] = '\'';
		for (cursor = text; *cursor != '\0'; cursor++) {
			escape = memchr(escapes, *cursor, sizeof(escapes) - 1U);
			if (escape != NULL && (escape - escapes) % 2 == 0) {
				quoted[out++] = '\\';
				quoted[out++] = escape[1];
			} else if (*cursor == '\'' || *cursor == '\\') {
				quoted[out++] = '\\';
				quoted[out++] = *cursor;
			} else if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f) {
				out += (size_t)snprintf(quoted + out, 5, "\\%03o",
							(unsigned char)*cursor);
			} else {
				quoted[out++] = *cursor;
			}
		}
		quoted[out++] = '\'';
		quoted[out] = '\0';
		return quoted;
	}

	/* Otherwise a backslash before each special character. */
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (strchr(special, *cursor) != NULL ||
		    (cursor == text && (*cursor == '#' || *cursor == '~')))
			quoted[out++] = '\\';
		quoted[out++] = *cursor;
	}
	quoted[out] = '\0';

	/* Succeeded: the text, freed with the command. */
	return quoted;
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

/* Writes a character of printf's output. */
static void
out_char(
	int value)
{
	char byte;

	/* To the standard output, unless -v collects it. */
	if (!printf_collecting) {
		putchar(value);
		return;
	}
	byte = (char)value;
	capture_add(&byte, 1);
}

/* Writes one conversion of printf's output. */
static void
out_format(
	const char *spec,
	...)
{
	va_list arguments;
	char small[128];
	char *large;
	int length;

	/* To the standard output, unless -v collects it. */
	va_start(arguments, spec);
	if (!printf_collecting) {
		(void)vprintf(spec, arguments);
		va_end(arguments);
		return;
	}

	/* Formatted into a buffer, a larger one when it does not fit. */
	length = vsnprintf(small, sizeof(small), spec, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length < sizeof(small)) {
		capture_add(small, (size_t)length);
		return;
	}
	large = sh_malloc((size_t)length + 1U);
	va_start(arguments, spec);
	(void)vsnprintf(large, (size_t)length + 1U, spec, arguments);
	va_end(arguments);
	capture_add(large, (size_t)length);
	free(large);
}

/* Adds bytes to what printf -v collects. */
static void
capture_add(
	const char *text,
	size_t length)
{
	struct printf_capture *capture;
	size_t capacity;

	/* Grows the buffer, doubling it. */
	capture = &printf_capture;
	if (capture->length + length > capture->capacity) {
		capacity = capture->capacity == 0 ? 64 : capture->capacity;
		while (capacity < capture->length + length)
			capacity *= 2;
		capture->text = sh_realloc(capture->text, capacity);
		capture->capacity = capacity;
	}

	/* Appends them. */
	memcpy(capture->text + capture->length, text, length);
	capture->length += length;
}
