/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in functions of awk, the formats of printf and sprintf, and
 * the regular expressions.
 *
 * A regex written as /ERE/ is compiled once with the program.  A regex
 * that comes from a string is compiled when it is used, and the last ones
 * are kept, since a program uses the same few again and again.
 */

#include "userland/base/awk/awk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The number of compiled regexes kept for strings. */
#define REGEX_CACHE_SIZE	32

/* The multiplier and the increment of rand's generator (drand48's). */
#define RANDOM_MULTIPLIER	0x5DEECE66DULL
#define RANDOM_INCREMENT	0xBULL
#define RANDOM_MASK		0xFFFFFFFFFFFFULL

/* A regex compiled from a string, with the string it came from. */
struct cached_regex {
	char *text;
	size_t length;
	regex_t regex;
};

/*
 * The regexes compiled from strings, used in turn: the next one to be
 * replaced is at regex_cache_next.  An entry with text NULL is free.
 * They live until awk ends or are replaced.
 */
static struct cached_regex regex_cache[REGEX_CACHE_SIZE];
static size_t regex_cache_next;

static void string_argument(struct node *argument, struct value *value);
static double number_argument(struct node *argument);
static void builtin_length(struct node *call, struct value *value);
static void builtin_substr(struct node *call, struct value *value);
static void builtin_index(struct node *call, struct value *value);
static void builtin_split(struct node *call, struct value *value);
static void builtin_substitute(struct node *call, int global, struct value *value);
static void append_replacement(struct buffer *buffer, const struct value *replacement, const char *matched, size_t matched_length);
static void builtin_match(struct node *call, struct value *value);
static void builtin_sprintf(struct node *call, struct value *value);
static void builtin_math(struct node *call, struct value *value);
static void builtin_random(struct node *call, struct value *value);
static void builtin_case(struct node *call, int upper, struct value *value);
static void format_one(struct buffer *buffer, const char *specification, size_t specification_length, char conversion, const struct value *argument);
static void format_text(struct buffer *buffer, const char *flags, size_t flags_length, long width, long precision, const char *text, size_t length);
static void append_formatted(struct buffer *buffer, const char *format, double number, int kind);
static const struct value *next_argument(struct value *arguments, size_t count, size_t *used);
static void builtin_io(struct node *call, struct value *value);
static int is_flag(char character);
static int is_conversion(char character);

/*
 * Calls a built-in function.
 */
void
builtin_call(
	struct node *call,
	struct value *value)
{
	/* The function. */
	switch (call->builtin) {
	case BUILTIN_LENGTH:
		builtin_length(call, value);
		break;
	case BUILTIN_SUBSTR:
		builtin_substr(call, value);
		break;
	case BUILTIN_INDEX:
		builtin_index(call, value);
		break;
	case BUILTIN_SPLIT:
		builtin_split(call, value);
		break;
	case BUILTIN_SUB:
		builtin_substitute(call, 0, value);
		break;
	case BUILTIN_GSUB:
		builtin_substitute(call, 1, value);
		break;
	case BUILTIN_MATCH:
		builtin_match(call, value);
		break;
	case BUILTIN_SPRINTF:
		builtin_sprintf(call, value);
		break;
	case BUILTIN_SIN:
	case BUILTIN_COS:
	case BUILTIN_ATAN2:
	case BUILTIN_EXP:
	case BUILTIN_LOG:
	case BUILTIN_SQRT:
	case BUILTIN_INT:
		builtin_math(call, value);
		break;
	case BUILTIN_RAND:
	case BUILTIN_SRAND:
		builtin_random(call, value);
		break;
	case BUILTIN_TOLOWER:
		builtin_case(call, 0, value);
		break;
	case BUILTIN_TOUPPER:
		builtin_case(call, 1, value);
		break;
	default:
		builtin_io(call, value);
		break;
	}
}

/*
 * Formats values as printf does, appending to a buffer.  A conversion
 * with no argument left is an error.
 */
void
format_values(
	const char *format,
	size_t length,
	struct value *arguments,
	size_t count,
	struct buffer *buffer)
{
	struct buffer specification;
	const struct value *argument;
	const struct value *star;
	char number[32];
	size_t position;
	size_t used;
	size_t end;
	char character;
	long star_value;
	int written;
	int flag;
	int conversion;

	/* Each character; % starts a conversion. */
	memset(&specification, 0, sizeof(specification));
	used = 0;
	for (position = 0; position < length; position++) {
		character = format[position];
		if (character != '%') {
			buffer_append_byte(buffer, character);
			continue;
		}

		/* %% is a percent sign. */
		if (position + 1U < length && format[position + 1U] == '%') {
			buffer_append_byte(buffer, '%');
			position++;
			continue;
		}

		/* The flags. */
		specification.length = 0;
		buffer_append_byte(&specification, '%');
		end = position + 1U;
		while (end < length) {
			flag = is_flag(format[end]);
			if (!flag)
				break;
			buffer_append_byte(&specification, format[end]);
			end++;
		}

		/* The width: digits, or * for the next argument. */
		if (end < length && format[end] == '*') {
			star = next_argument(arguments, count, &used);
			star_value = (long)value_number(star);
			written = snprintf(number, sizeof(number), "%ld", star_value);
			buffer_append(&specification, number, (size_t)written);
			end++;
		} else {
			while (end < length && format[end] >= '0' && format[end] <= '9') {
				buffer_append_byte(&specification, format[end]);
				end++;
			}
		}

		/* The precision: digits, or * for the next argument. */
		if (end < length && format[end] == '.') {
			buffer_append_byte(&specification, '.');
			end++;
			if (end < length && format[end] == '*') {
				star = next_argument(arguments, count, &used);
				star_value = (long)value_number(star);
				written = snprintf(number, sizeof(number), "%ld", star_value);
				buffer_append(&specification, number, (size_t)written);
				end++;
			} else {
				while (end < length && format[end] >= '0' && format[end] <= '9') {
					buffer_append_byte(&specification, format[end]);
					end++;
				}
			}
		}

		/* A format that ends early is written as it is. */
		if (end >= length) {
			buffer_append(buffer, format + position, length - position);
			break;
		}

		/* A conversion that is not one is written as it is. */
		character = format[end];
		conversion = is_conversion(character);
		if (!conversion) {
			buffer_append(buffer, format + position, end - position + 1U);
			position = end;
			continue;
		}

		/* Succeeded: the argument converted. */
		argument = next_argument(arguments, count, &used);
		format_one(buffer, specification.data, specification.length, character, argument);
		position = end;
	}

	/* The specification's buffer goes. */
	free(specification.data);
}

/*
 * Compiles an ERE.  A cached regex is kept among the last ones compiled
 * from strings and may be replaced by a later compilation; any other lives
 * with the program.
 */
regex_t *
regex_compile(
	const char *text,
	size_t length,
	int cached)
{
	struct cached_regex *entry;
	regex_t *regex;
	char *pattern;
	size_t index;
	int error;

	/* A regex compiled from the same string before. */
	if (cached) {
		for (index = 0; index < REGEX_CACHE_SIZE; index++) {
			entry = &regex_cache[index];
			if (entry->text == NULL || entry->length != length)
				continue;
			error = memcmp(entry->text, text, length);
			if (error == 0)
				return &entry->regex;
		}
	}

	/* Where it goes: a cache entry, replacing the oldest, or its own. */
	if (cached) {
		entry = &regex_cache[regex_cache_next];
		regex_cache_next = (regex_cache_next + 1U) % REGEX_CACHE_SIZE;
		if (entry->text != NULL) {
			regfree(&entry->regex);
			free(entry->text);
			entry->text = NULL;
		}

		/* The regex goes into the entry. */
		regex = &entry->regex;
	} else {
		entry = NULL;
		regex = awk_allocate(sizeof(*regex));
	}

	/* The ERE; an empty one matches everywhere. */
	pattern = awk_copy(text, length);
	if (length == 0) {
		free(pattern);
		pattern = awk_copy("()", 2);
	}

	/* Compiled as an ERE. */
	error = regcomp(regex, pattern, REG_EXTENDED);
	if (error != 0)
		awk_fatal("invalid regular expression /%s/", pattern);
	free(pattern);

	/* A cached regex is remembered by its string. */
	if (entry != NULL) {
		entry->text = awk_copy(text, length);
		entry->length = length;
	}

	/* Succeeded. */
	return regex;
}

/*
 * Returns the regex an expression stands for: a /ERE/, or the string
 * form of any other value.  The value is evaluated into scratch.
 */
regex_t *
regex_of(
	struct node *expression,
	struct value *scratch)
{
	regex_t *regex;

	/* A /ERE/ is compiled already. */
	if (expression->kind == NODE_REGEX)
		return expression->regex;

	/* Succeeded: the string, compiled or found in the cache. */
	run_expression(expression, scratch);
	value_string(scratch, 0, scratch);
	regex = regex_compile(scratch->text, scratch->length, 1);
	return regex;
}

/*
 * Finds the leftmost longest match of a regex in a string from a
 * position; ^ matches only at the start of the string.  Returns 0 when
 * there is none.
 */
int
regex_search(
	regex_t *regex,
	const char *text,
	size_t length,
	size_t from,
	size_t *match_start,
	size_t *match_end)
{
	regmatch_t match[1];
	int flags;
	int error;

	/* Nothing past the end. */
	if (from > length)
		return 0;

	/* The match, not at the beginning of a line past the start. */
	flags = 0;
	if (from > 0)
		flags = REG_NOTBOL;
	error = regexec(regex, text + from, 1, match, flags);
	if (error != 0)
		return 0;

	/* Succeeded: where it is in the whole string. */
	*match_start = from + (size_t)match[0].rm_so;
	*match_end = from + (size_t)match[0].rm_eo;
	return 1;
}

/* Evaluates an argument as a string. */
static void
string_argument(
	struct node *argument,
	struct value *value)
{
	/* The value, then its string form. */
	run_expression(argument, value);
	value_string(value, 0, value);
}

/* Evaluates an argument as a number. */
static double
number_argument(
	struct node *argument)
{
	struct value value;
	double number;

	/* The value, then its number. */
	memset(&value, 0, sizeof(value));
	run_expression(argument, &value);
	number = value_number(&value);
	value_free(&value);

	/* Succeeded. */
	return number;
}

/* length: of a string ($0 without an argument), or of an array. */
static void
builtin_length(
	struct node *call,
	struct value *value)
{
	struct value string;
	struct cell *cell;

	/* Without an argument, $0. */
	memset(&string, 0, sizeof(string));
	if (call->arguments == NULL) {
		field_read(0, &string);
		value_string(&string, 0, &string);
		value_set_number(value, (double)string.length);
		value_free(&string);
		return;
	}

	/* An array, by its elements. */
	cell = node_cell(call->arguments);
	while (cell != NULL && cell->kind == CELL_REFERENCE)
		cell = cell->target;
	if (cell != NULL && cell->kind == CELL_ARRAY) {
		value_set_number(value, (double)cell->array->count);
		return;
	}

	/* Succeeded: a string, by its bytes. */
	string_argument(call->arguments, &string);
	value_set_number(value, (double)string.length);
	value_free(&string);
}

/*
 * substr(s, m[, n]): the characters of s from position m, n of them.  As
 * gawk does, m and n are truncated, a start before 1 is 1, and a length
 * below 1 is nothing.
 */
static void
builtin_substr(
	struct node *call,
	struct value *value)
{
	struct value string;
	double start_number;
	double length_number;
	size_t start;
	size_t length;

	/* The string and where to start. */
	memset(&string, 0, sizeof(string));
	string_argument(call->arguments, &string);
	start_number = number_argument(call->arguments->next);
	start_number = trunc(start_number);
	if (!(start_number >= 1))
		start_number = 1;

	/* How many characters: the rest when no length is given. */
	length = string.length;
	if (call->arguments->next->next != NULL) {
		length_number = number_argument(call->arguments->next->next);
		length_number = trunc(length_number);
		if (!(length_number >= 1))
			length_number = 0;
		if (length_number < (double)string.length)
			length = (size_t)length_number;
	}

	/* Nothing past the end. */
	if (start_number > (double)string.length) {
		value_set_text(value, "", 0);
		value_free(&string);
		return;
	}

	/* The index of the first character, from 0. */
	start = (size_t)start_number - 1U;
	if (length > string.length - start)
		length = string.length - start;

	/* Succeeded. */
	value_set_text(value, string.text + start, length);
	value_free(&string);
}

/* index(s, t): the position of the first t in s, or 0. */
static void
builtin_index(
	struct node *call,
	struct value *value)
{
	struct value string;
	struct value target;
	size_t position;
	int compare;

	/* Both strings. */
	memset(&string, 0, sizeof(string));
	memset(&target, 0, sizeof(target));
	string_argument(call->arguments, &string);
	string_argument(call->arguments->next, &target);

	/* Each position t fits at. */
	value_set_number(value, 0);
	for (position = 0; position + target.length <= string.length; position++) {
		compare = memcmp(string.text + position, target.text, target.length);
		if (compare == 0) {
			value_set_number(value, (double)(position + 1U));
			break;
		}
	}

	/* Succeeded. */
	value_free(&string);
	value_free(&target);
}

/* split(s, a[, fs]): the pieces of s into a, as FS or fs splits. */
static void
builtin_split(
	struct node *call,
	struct value *value)
{
	struct value string;
	struct value separator;
	struct node *separator_node;
	struct array *array;
	regex_t *regex;
	const char *separator_text;
	size_t separator_length;
	size_t count;

	/* The string, copied before the array is cleared. */
	memset(&string, 0, sizeof(string));
	memset(&separator, 0, sizeof(separator));
	string_argument(call->arguments, &string);

	/* The separator: a regex, a string, or FS. */
	regex = NULL;
	separator_node = call->arguments->next->next;
	if (separator_node != NULL && separator_node->kind == NODE_REGEX) {
		regex = separator_node->regex;
		separator_text = "";
		separator_length = 0;
	} else if (separator_node != NULL) {
		string_argument(separator_node, &separator);
		separator_text = separator.text;
		separator_length = separator.length;
	} else {
		separator_text = special_text(SPECIAL_FS, &separator_length);
	}

	/* Succeeded: the elements, and how many. */
	array = node_array(call->arguments->next);
	count = split_text(string.text, string.length, separator_text, separator_length, regex, array);
	value_set_number(value, (double)count);
	value_free(&string);
	value_free(&separator);
}

/*
 * sub(re, repl[, target]) and gsub(re, repl[, target]): the first match,
 * or every match, of re in target ($0 by default) replaced by repl, where
 * & is the matched text.  An empty match right after a match is not used.
 * The value is the number of replacements.
 */
static void
builtin_substitute(
	struct node *call,
	int global,
	struct value *value)
{
	struct value scratch;
	struct value replacement;
	struct value target;
	struct value result;
	struct buffer buffer;
	struct place place;
	struct node *target_node;
	regex_t *regex;
	size_t position;
	size_t match_start;
	size_t match_end;
	size_t count;
	size_t last_end;
	int have_last;
	int found;
	int lvalue;

	/* The regex and the replacement. */
	memset(&scratch, 0, sizeof(scratch));
	memset(&replacement, 0, sizeof(replacement));
	memset(&target, 0, sizeof(target));
	memset(&result, 0, sizeof(result));
	regex = regex_of(call->arguments, &scratch);
	string_argument(call->arguments->next, &replacement);

	/* The target: $0, an lvalue, or a value that is thrown away. */
	target_node = call->arguments->next->next;
	lvalue = 0;
	if (target_node == NULL) {
		memset(&place, 0, sizeof(place));
		place.kind = PLACE_FIELD;
		place.field = 0;
		lvalue = 1;
	} else if (target_node->kind == NODE_VARIABLE ||
		   target_node->kind == NODE_LOCAL ||
		   target_node->kind == NODE_ELEMENT ||
		   target_node->kind == NODE_FIELD) {
		place_resolve(target_node, &place);
		lvalue = 1;
	}

	/* The target's value, as a string. */
	if (lvalue)
		place_read(&place, &target);
	else
		run_expression(target_node, &target);
	value_string(&target, 0, &target);

	/* Each match, with the text before it and the replacement. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	position = 0;
	count = 0;
	last_end = 0;
	have_last = 0;
	while (position <= target.length) {
		found = regex_search(regex, target.text, target.length, position, &match_start, &match_end);
		if (!found)
			break;

		/* An empty match just after a match is passed over. */
		if (match_start == match_end && have_last && match_start == last_end) {
			buffer_append(&buffer, target.text + position, match_start - position);
			if (match_start < target.length)
				buffer_append_byte(&buffer, target.text[match_start]);
			position = match_start + 1U;
			continue;
		}

		/* The text before the match, and the replacement. */
		buffer_append(&buffer, target.text + position, match_start - position);
		append_replacement(&buffer, &replacement, target.text + match_start, match_end - match_start);
		count++;
		have_last = 1;
		last_end = match_end;

		/* After an empty match, the character it stood before. */
		if (match_end == match_start) {
			if (match_start < target.length)
				buffer_append_byte(&buffer, target.text[match_start]);
			position = match_start + 1U;
		} else {
			position = match_end;
		}

		/* sub stops after the first. */
		if (!global)
			break;
	}

	/* The rest after the last match. */
	if (position < target.length)
		buffer_append(&buffer, target.text + position, target.length - position);

	/* The target changes only when something was replaced. */
	if (count > 0 && lvalue) {
		value_set_text(&result, buffer.data, buffer.length);
		place_write(&place, &result);
	}

	/* What resolving the place took goes. */
	if (lvalue)
		place_release(&place);

	/* Succeeded: the number of replacements. */
	free(buffer.data);
	value_free(&result);
	value_free(&target);
	value_free(&replacement);
	value_free(&scratch);
	value_set_number(value, (double)count);
}

/*
 * Appends a replacement: & is the matched text, \& is &, \\& is \ and the
 * matched text, \\\& is \&, and any other backslash is itself (the rules
 * POSIX gives).
 */
static void
append_replacement(
	struct buffer *buffer,
	const struct value *replacement,
	const char *matched,
	size_t matched_length)
{
	const char *text;
	size_t length;
	size_t position;
	char character;

	/* Each character of the replacement. */
	text = replacement->text;
	length = replacement->length;
	for (position = 0; position < length; position++) {
		character = text[position];

		/* & is the matched text. */
		if (character == '&') {
			buffer_append(buffer, matched, matched_length);
			continue;
		}

		/* \\ is a backslash, and \& is &. */
		if (character == '\\' && position + 1U < length) {
			if (text[position + 1U] == '\\' || text[position + 1U] == '&') {
				position++;
				buffer_append_byte(buffer, text[position]);
				continue;
			}
		}

		/* Any other character is itself. */
		buffer_append_byte(buffer, character);
	}
}

/* match(s, re): the position of the leftmost longest match, in RSTART and RLENGTH too. */
static void
builtin_match(
	struct node *call,
	struct value *value)
{
	struct value string;
	struct value scratch;
	struct value start;
	struct value length;
	regex_t *regex;
	size_t match_start;
	size_t match_end;
	int found;

	/* The string, then the regex. */
	memset(&string, 0, sizeof(string));
	memset(&scratch, 0, sizeof(scratch));
	memset(&start, 0, sizeof(start));
	memset(&length, 0, sizeof(length));
	string_argument(call->arguments, &string);
	regex = regex_of(call->arguments->next, &scratch);
	found = regex_search(regex, string.text, string.length, 0, &match_start, &match_end);

	/* Where it is, or 0 and -1. */
	if (found) {
		value_set_number(&start, (double)(match_start + 1U));
		value_set_number(&length, (double)(match_end - match_start));
	} else {
		value_set_number(&start, 0);
		value_set_number(&length, -1);
	}

	/* RSTART and RLENGTH tell where. */
	assign_variable(awk.specials[SPECIAL_RSTART], &start);
	assign_variable(awk.specials[SPECIAL_RLENGTH], &length);

	/* Succeeded: RSTART is the value. */
	value_copy(value, &start);
	value_free(&start);
	value_free(&length);
	value_free(&string);
	value_free(&scratch);
}

/* sprintf(format, ...): the text printf would write. */
static void
builtin_sprintf(
	struct node *call,
	struct value *value)
{
	struct node *argument;
	struct value *values;
	struct value format;
	struct buffer buffer;
	size_t count;
	size_t index;

	/* The format and the values. */
	count = call->argument_count;
	values = awk_allocate(sizeof(*values) * count);
	index = 0;
	for (argument = call->arguments; argument != NULL; argument = argument->next) {
		run_expression(argument, &values[index]);
		index++;
	}

	/* The format as a string. */
	memset(&format, 0, sizeof(format));
	value_string(&values[0], 0, &format);

	/* The text. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	format_values(format.text, format.length, values + 1, count - 1U, &buffer);

	/* Succeeded: the value takes the text. */
	value_free(value);
	value->type = VALUE_STRING;
	value->text = buffer.data;
	value->length = buffer.length;
	value_free(&format);
	for (index = 0; index < count; index++)
		value_free(&values[index]);
	free(values);
}

/* The functions of numbers: sin, cos, atan2, exp, log, sqrt and int. */
static void
builtin_math(
	struct node *call,
	struct value *value)
{
	double argument;
	double second;
	double result;

	/* The first argument, and the second of atan2. */
	argument = number_argument(call->arguments);
	second = 0;
	if (call->builtin == BUILTIN_ATAN2)
		second = number_argument(call->arguments->next);

	/* The function. */
	switch (call->builtin) {
	case BUILTIN_SIN:
		result = sin(argument);
		break;
	case BUILTIN_COS:
		result = cos(argument);
		break;
	case BUILTIN_ATAN2:
		result = atan2(argument, second);
		break;
	case BUILTIN_EXP:
		result = exp(argument);
		break;
	case BUILTIN_LOG:
		result = log(argument);
		break;
	case BUILTIN_SQRT:
		result = sqrt(argument);
		break;
	default:
		result = trunc(argument);
		break;
	}

	/* Succeeded. */
	value_set_number(value, result);
}

/*
 * rand(): the next number in [0, 1).  srand([seed]): starts the numbers
 * again from a seed (the time of day by default) and gives the last seed.
 */
static void
builtin_random(
	struct node *call,
	struct value *value)
{
	double previous;
	double seed;

	/* rand: the next state, as a fraction of the whole range. */
	if (call->builtin == BUILTIN_RAND) {
		awk.random_state = (awk.random_state * RANDOM_MULTIPLIER + RANDOM_INCREMENT) & RANDOM_MASK;
		value_set_number(value, (double)awk.random_state / (double)(RANDOM_MASK + 1ULL));
		return;
	}

	/* srand: the seed given, or the time. */
	if (call->arguments != NULL)
		seed = number_argument(call->arguments);
	else
		seed = (double)time(NULL);

	/* Succeeded: the state starts from the seed, and the old seed is the value. */
	previous = awk.random_seed;
	awk.random_seed = seed;
	awk.random_state = (((unsigned long long)(long long)seed << 16) | 0x330EULL) & RANDOM_MASK;
	value_set_number(value, previous);
}

/* tolower and toupper, for ASCII letters. */
static void
builtin_case(
	struct node *call,
	int upper,
	struct value *value)
{
	struct value string;
	size_t index;
	char character;

	/* The string, changed letter by letter. */
	memset(&string, 0, sizeof(string));
	string_argument(call->arguments, &string);
	for (index = 0; index < string.length; index++) {
		character = string.text[index];
		if (upper && character >= 'a' && character <= 'z')
			string.text[index] = (char)(character - 'a' + 'A');
		else if (!upper && character >= 'A' && character <= 'Z')
			string.text[index] = (char)(character - 'A' + 'a');
	}

	/* Succeeded. */
	value_move(value, &string);
}

/*
 * Formats one argument by a conversion.  The specification is the % and
 * the flags, width and precision before the conversion character.
 */
static void
format_one(
	struct buffer *buffer,
	const char *specification,
	size_t specification_length,
	char conversion,
	const struct value *argument)
{
	struct buffer format;
	struct value string;
	const char *cursor;
	const char *flags_end;
	char *width_end;
	char digits[32];
	double number;
	double whole;
	size_t flags_length;
	size_t first_length;
	long width;
	long precision;
	char character;
	int numeric;
	int flag;
	int written;

	/* The flags, width and precision, read back from the specification. */
	cursor = specification + 1;
	for (;;) {
		flag = is_flag(*cursor);
		if (!flag)
			break;
		cursor++;
	}

	/* The width, and the precision after a point. */
	flags_end = cursor;
	flags_length = (size_t)(flags_end - specification - 1);
	width = strtol(cursor, &width_end, 10);
	precision = -1;
	if (*width_end == '.')
		precision = strtol(width_end + 1, NULL, 10);

	/* %c: a number is a character code, a string its first character. */
	memset(&string, 0, sizeof(string));
	if (conversion == 'c') {
		numeric = 0;
		if (argument->type == VALUE_NUMBER || argument->type == VALUE_STRNUM)
			numeric = 1;
		if (numeric) {
			number = value_number(argument);
			character = (char)(unsigned char)(long long)number;
			format_text(buffer, specification + 1, flags_length, width, -1, &character, 1);
			return;
		}

		/* A string: its first character. */
		value_string(argument, 0, &string);
		first_length = 0;
		if (string.length > 0)
			first_length = 1;
		format_text(buffer, specification + 1, flags_length, width, -1, string.text, first_length);
		value_free(&string);
		return;
	}

	/* %s: the string form, cut to the precision. */
	if (conversion == 's') {
		value_string(argument, 0, &string);
		format_text(buffer, specification + 1, flags_length, width, precision, string.text, string.length);
		value_free(&string);
		return;
	}

	/* The C format of the specification and the conversion. */
	memset(&format, 0, sizeof(format));
	buffer_append(&format, specification, specification_length);
	number = value_number(argument);

	/* The integer conversions: in range as a long long, or as a float. */
	if (conversion == 'd' ||
	    conversion == 'i' ||
	    conversion == 'o' ||
	    conversion == 'x' ||
	    conversion == 'X' ||
	    conversion == 'u') {
		whole = trunc(number);
		if (whole > -9.2e18 && whole < 9.2e18) {
			buffer_append(&format, "ll", 2);
			buffer_append_byte(&format, conversion);
			if (conversion == 'd' || conversion == 'i')
				append_formatted(buffer, format.data, whole, 1);
			else
				append_formatted(buffer, format.data, whole, 2);
			free(format.data);
			return;
		}

		/* Out of range: all its digits, with the flags and the width. */
		format.length = 0;
		buffer_append(&format, specification, (size_t)(flags_end - specification));
		if (width > 0) {
			written = snprintf(digits, sizeof(digits), "%ld", width);
			buffer_append(&format, digits, (size_t)written);
		}

		/* No decimals. */
		buffer_append(&format, ".0f", 3);
		append_formatted(buffer, format.data, whole, 0);
		free(format.data);
		return;
	}

	/* Succeeded: the floating conversions. */
	buffer_append_byte(&format, conversion);
	append_formatted(buffer, format.data, number, 0);
	free(format.data);
}

/* Appends text padded to a width, cut to a precision (-1 for none). */
static void
format_text(
	struct buffer *buffer,
	const char *flags,
	size_t flags_length,
	long width,
	long precision,
	const char *text,
	size_t length)
{
	size_t padding;
	size_t index;
	int left;

	/* The precision cuts the text. */
	if (precision >= 0 && (size_t)precision < length)
		length = (size_t)precision;

	/* - puts the padding after the text. */
	left = 0;
	for (index = 0; index < flags_length; index++) {
		if (flags[index] == '-')
			left = 1;
	}

	/* The padding before, the text, the padding after. */
	padding = 0;
	if (width > 0 && (size_t)width > length)
		padding = (size_t)width - length;
	if (!left) {
		for (index = 0; index < padding; index++)
			buffer_append_byte(buffer, ' ');
	}

	/* The text. */
	buffer_append(buffer, text, length);
	if (left) {
		for (index = 0; index < padding; index++)
			buffer_append_byte(buffer, ' ');
	}
}

/*
 * Appends a number formatted by a C format: kind 1 passes it as a long
 * long, 2 as an unsigned long long, and 0 as a double.
 */
static void
append_formatted(
	struct buffer *buffer,
	const char *format,
	double number,
	int kind)
{
	char small[128];
	char *text;
	int length;

	/* Into a small buffer. */
	if (kind == 1)
		length = snprintf(small, sizeof(small), format, (long long)number);
	else if (kind == 2)
		length = snprintf(small, sizeof(small), format, (unsigned long long)(long long)number);
	else
		length = snprintf(small, sizeof(small), format, number);
	if (length < 0)
		return;
	if ((size_t)length < sizeof(small)) {
		buffer_append(buffer, small, (size_t)length);
		return;
	}

	/* Succeeded: a long one, into a block of its size. */
	text = awk_allocate((size_t)length);
	if (kind == 1)
		snprintf(text, (size_t)length + 1U, format, (long long)number);
	else if (kind == 2)
		snprintf(text, (size_t)length + 1U, format, (unsigned long long)(long long)number);
	else
		snprintf(text, (size_t)length + 1U, format, number);
	buffer_append(buffer, text, (size_t)length);
	free(text);
}

/* system, close and fflush. */
static void
builtin_io(
	struct node *call,
	struct value *value)
{
	struct value argument;
	int result;

	/* fflush() flushes everything. */
	if (call->arguments == NULL) {
		result = io_flush(NULL, 0);
		value_set_number(value, (double)result);
		return;
	}

	/* The command or the name. */
	memset(&argument, 0, sizeof(argument));
	string_argument(call->arguments, &argument);

	/* The function. */
	if (call->builtin == BUILTIN_SYSTEM)
		result = io_system(argument.text);
	else if (call->builtin == BUILTIN_CLOSE)
		result = io_close(argument.text, argument.length);
	else
		result = io_flush(argument.text, argument.length);

	/* Succeeded. */
	value_free(&argument);
	value_set_number(value, (double)result);
}

/* Returns the next argument of a format; running out is an error. */
static const struct value *
next_argument(
	struct value *arguments,
	size_t count,
	size_t *used)
{
	const struct value *argument;

	/* One more than there are. */
	if (*used >= count)
		awk_fatal("not enough arguments to satisfy format string");

	/* Succeeded. */
	argument = &arguments[*used];
	(*used)++;
	return argument;
}

/* Returns whether a character is a flag of a conversion. */
static int
is_flag(
	char character)
{
	/* -, +, space, # and 0. */
	switch (character) {
	case '-':
	case '+':
	case ' ':
	case '#':
	case '0':
		return 1;
	default:
		return 0;
	}
}

/* Returns whether a character is a conversion printf knows. */
static int
is_conversion(
	char character)
{
	/* The conversions of C that awk takes. */
	switch (character) {
	case 'c':
	case 'd':
	case 'i':
	case 'e':
	case 'E':
	case 'f':
	case 'F':
	case 'g':
	case 'G':
	case 'o':
	case 's':
	case 'u':
	case 'x':
	case 'X':
	case 'a':
	case 'A':
		return 1;
	default:
		return 0;
	}
}
