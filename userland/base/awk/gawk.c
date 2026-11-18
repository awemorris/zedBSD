/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in functions of gawk that scripts written for it use:
 * gensub, the time functions (systime, strftime, mktime), the bit
 * functions (and, or, xor, lshift, rshift, compl), asort and asorti, and
 * the array of match's third argument.
 *
 * They follow gawk's descriptions; POSIX awk has none of them.
 */

#include "userland/base/awk/awk.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The groups a match reports: the whole match and \1 to \9. */
#define MATCH_GROUPS 10

/* The bits the bit functions work on, as gawk's (a double's mantissa). */
#define BIT_MASK ((1ULL << 53) - 1ULL)

/* The format of strftime without one. */
#define STRFTIME_DEFAULT "%a %b %e %H:%M:%S %Z %Y"

/* One value of an array being sorted, with the key it came from. */
struct sorted_item {
	struct value value;
	char *key;
	size_t key_length;
};

static void string_of(struct node *argument, struct value *value);
static double number_of(struct node *argument);
static void gawk_gensub(struct node *call, struct value *value);
static void add_gensub_replacement(struct buffer *buffer, const struct value *replacement, const char *text, const regmatch_t *matches);
static void gawk_strftime(struct node *call, struct value *value);
static void gawk_mktime(struct node *call, struct value *value);
static void gawk_bits(struct node *call, struct value *value);
static void gawk_sort(struct node *call, int keys, struct value *value);
static int compare_items(const void *left, const void *right);
static int compare_keys(const void *left, const void *right);
static void set_element(struct array *array, const char *key, size_t length, const struct value *value);
static void set_text_element(struct array *array, const char *key, size_t length, const char *text, size_t text_length);
static void set_number_element(struct array *array, const char *key, size_t length, double number);

/*
 * Calls one of gawk's built-in functions.
 */
void
gawk_call(
	struct node *call,
	struct value *value)
{
	/* The function. */
	switch (call->builtin) {
	case BUILTIN_GENSUB:
		gawk_gensub(call, value);
		break;
	case BUILTIN_SYSTIME:
		value_set_number(value, (double)time(NULL));
		break;
	case BUILTIN_STRFTIME:
		gawk_strftime(call, value);
		break;
	case BUILTIN_MKTIME:
		gawk_mktime(call, value);
		break;
	case BUILTIN_ASORT:
		gawk_sort(call, 0, value);
		break;
	case BUILTIN_ASORTI:
		gawk_sort(call, 1, value);
		break;
	default:
		gawk_bits(call, value);
		break;
	}
}

/*
 * Fills the array of match's third argument: the whole match at 0 and each
 * group that took part at its number, with (n, "start") and (n, "length")
 * after them, keyed with SUBSEP between.  The array is emptied first.
 */
void
gawk_match_groups(
	struct node *array_node,
	regex_t *regex,
	const char *text)
{
	regmatch_t matches[MATCH_GROUPS];
	struct array *array;
	struct buffer key;
	const char *separator;
	size_t separator_length;
	char number[24];
	size_t group;
	size_t length;
	int result;

	/* The array, emptied. */
	array = node_array(array_node);
	array_clear(array);

	/* The groups of the leftmost match; nothing when there is none. */
	result = regexec(regex, text, MATCH_GROUPS, matches, 0);
	if (result != 0)
		return;

	/* Each group that took part. */
	separator = special_text(SPECIAL_SUBSEP, &separator_length);
	memset(&key, 0, sizeof(key));
	for (group = 0; group < MATCH_GROUPS; group++) {
		if (matches[group].rm_so < 0)
			continue;

		/* The text of the group. */
		length = (size_t)snprintf(number, sizeof(number), "%lu", (unsigned long)group);
		set_text_element(array, number, length, text + matches[group].rm_so,
				 (size_t)(matches[group].rm_eo - matches[group].rm_so));

		/* Where it starts, from 1. */
		key.length = 0;
		buffer_append(&key, number, length);
		buffer_append(&key, separator, separator_length);
		buffer_append(&key, "start", 5);
		set_number_element(array, key.data, key.length,
				   (double)matches[group].rm_so + 1.0);

		/* How long it is. */
		key.length = 0;
		buffer_append(&key, number, length);
		buffer_append(&key, separator, separator_length);
		buffer_append(&key, "length", 6);
		set_number_element(array, key.data, key.length,
				   (double)(matches[group].rm_eo - matches[group].rm_so));
	}

	/* The key's buffer goes. */
	free(key.data);
}

/* Evaluates an argument as a string. */
static void
string_of(
	struct node *argument,
	struct value *value)
{
	/* The value, then its string form. */
	run_expression(argument, value);
	value_string(value, 0, value);
}

/* Evaluates an argument as a number. */
static double
number_of(
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

/*
 * gensub(re, replacement, how[, target]): target ($0 by default) with the
 * matches of re replaced, every one when how starts with g or G and else
 * the how-th; & and \0 are the match and \1 to \9 its groups.  The target
 * is not changed; the new text is the value.
 */
static void
gawk_gensub(
	struct node *call,
	struct value *value)
{
	regmatch_t matches[MATCH_GROUPS];
	struct value scratch;
	struct value replacement;
	struct value how;
	struct value target;
	struct buffer buffer;
	struct node *target_node;
	regex_t *regex;
	size_t position;
	size_t count;
	size_t match_start;
	size_t match_end;
	long which;
	int global;
	int flags;
	int result;

	/* The regex, the replacement and which matches. */
	memset(&scratch, 0, sizeof(scratch));
	memset(&replacement, 0, sizeof(replacement));
	memset(&how, 0, sizeof(how));
	memset(&target, 0, sizeof(target));
	regex = regex_of(call->arguments, &scratch);
	string_of(call->arguments->next, &replacement);
	string_of(call->arguments->next->next, &how);
	global = 0;
	which = 1;
	if (how.length > 0 && (how.text[0] == 'g' || how.text[0] == 'G')) {
		global = 1;
	} else {
		which = (long)text_number(how.text);
		if (which < 1)
			which = 1;
	}

	/* The target: the fourth argument, or $0. */
	target_node = call->arguments->next->next->next;
	if (target_node != NULL)
		string_of(target_node, &target);
	else
		field_read(0, &target);
	value_string(&target, 0, &target);

	/* Each match, replaced or kept. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	position = 0;
	count = 0;
	while (position <= target.length) {
		/* The next match and its groups. */
		flags = 0;
		if (position > 0)
			flags = REG_NOTBOL;
		result = regexec(regex, target.text + position, MATCH_GROUPS, matches, flags);
		if (result != 0)
			break;
		match_start = position + (size_t)matches[0].rm_so;
		match_end = position + (size_t)matches[0].rm_eo;
		count++;

		/* The text before it, then the replacement or the match. */
		buffer_append(&buffer, target.text + position, match_start - position);
		if (global || count == (size_t)which)
			add_gensub_replacement(&buffer, &replacement, target.text + position, matches);
		else
			buffer_append(&buffer, target.text + match_start, match_end - match_start);

		/* On past the match; past one character after an empty one. */
		position = match_end;
		if (match_end == match_start) {
			if (match_start < target.length)
				buffer_append_byte(&buffer, target.text[match_start]);
			position = match_start + 1U;
		}

		/* Only the how-th without g. */
		if (!global && count == (size_t)which)
			break;
	}

	/* The rest after the last match. */
	if (position < target.length)
		buffer_append(&buffer, target.text + position, target.length - position);

	/* Succeeded: the new text. */
	value_set_text(value, buffer.data, buffer.length);
	free(buffer.data);
	value_free(&scratch);
	value_free(&replacement);
	value_free(&how);
	value_free(&target);
}

/*
 * Appends gensub's replacement: & and \0 are the match, \1 to \9 the
 * groups (offsets from text), \& is &, \\ is \.
 */
static void
add_gensub_replacement(
	struct buffer *buffer,
	const struct value *replacement,
	const char *text,
	const regmatch_t *matches)
{
	const regmatch_t *group;
	size_t position;
	char character;
	char next;

	/* Each character of the replacement. */
	for (position = 0; position < replacement->length; position++) {
		character = replacement->text[position];

		/* & is the match. */
		if (character == '&') {
			buffer_append(buffer, text + matches[0].rm_so,
				      (size_t)(matches[0].rm_eo - matches[0].rm_so));
			continue;
		}

		/* An ordinary character. */
		if (character != '\\' || position + 1U >= replacement->length) {
			buffer_append_byte(buffer, character);
			continue;
		}

		/* \0 to \9: the match or a group, empty when it took no part. */
		next = replacement->text[position + 1U];
		if (next >= '0' && next <= '9') {
			position++;
			group = &matches[next - '0'];
			if (group->rm_so >= 0) {
				buffer_append(buffer, text + group->rm_so,
					      (size_t)(group->rm_eo - group->rm_so));
			}

			/* On to the next character. */
			continue;
		}

		/* \& and \\: the character itself. */
		if (next == '&' || next == '\\') {
			position++;
			buffer_append_byte(buffer, next);
			continue;
		}

		/* Any other backslash is itself. */
		buffer_append_byte(buffer, character);
	}
}

/*
 * strftime([format[, timestamp[, utc]]]): the time (now by default) as the
 * format writes it, in local time or with utc in UTC.
 */
static void
gawk_strftime(
	struct node *call,
	struct value *value)
{
	struct value format;
	struct tm fields;
	struct node *argument;
	char text[4096];
	time_t seconds;
	size_t length;
	double flag;
	int utc;

	/* The format. */
	memset(&format, 0, sizeof(format));
	argument = call->arguments;
	if (argument != NULL) {
		string_of(argument, &format);
		argument = argument->next;
	} else {
		value_set_text(&format, STRFTIME_DEFAULT, strlen(STRFTIME_DEFAULT));
	}

	/* The time. */
	seconds = time(NULL);
	if (argument != NULL) {
		seconds = (time_t)number_of(argument);
		argument = argument->next;
	}

	/* Local time, or UTC with a third argument that is not zero. */
	utc = 0;
	if (argument != NULL) {
		flag = number_of(argument);
		if (flag != 0.0)
			utc = 1;
	}

	/* The fields of the time. */
	if (utc)
		gmtime_r(&seconds, &fields);
	else
		localtime_r(&seconds, &fields);

	/* Succeeded: the text. */
	length = strftime(text, sizeof(text), format.text, &fields);
	value_set_text(value, text, length);
	value_free(&format);
}

/*
 * mktime("YYYY MM DD HH MM SS [DST]"): the seconds since the epoch of a
 * local time, or -1 when the text is not one.
 */
static void
gawk_mktime(
	struct node *call,
	struct value *value)
{
	struct value text;
	struct tm fields;
	long parts[7];
	char *cursor;
	char *end;
	size_t count;
	time_t seconds;

	/* The numbers of the text. */
	memset(&text, 0, sizeof(text));
	string_of(call->arguments, &text);
	cursor = text.text;
	for (count = 0; count < 7U; count++) {
		parts[count] = strtol(cursor, &end, 10);
		if (end == cursor)
			break;
		cursor = end;
	}

	/* The text is done with. */
	value_free(&text);

	/* Six at least. */
	if (count < 6U) {
		value_set_number(value, -1);
		return;
	}

	/* The fields; DST unknown unless given. */
	memset(&fields, 0, sizeof(fields));
	fields.tm_year = (int)parts[0] - 1900;
	fields.tm_mon = (int)parts[1] - 1;
	fields.tm_mday = (int)parts[2];
	fields.tm_hour = (int)parts[3];
	fields.tm_min = (int)parts[4];
	fields.tm_sec = (int)parts[5];
	fields.tm_isdst = -1;
	if (count == 7U)
		fields.tm_isdst = (int)parts[6];

	/* Succeeded: the seconds. */
	seconds = mktime(&fields);
	value_set_number(value, (double)seconds);
}

/*
 * The bit functions: and, or and xor of two or more numbers, lshift and
 * rshift of a number by a count, and compl of a number, on 53 bits.
 */
static void
gawk_bits(
	struct node *call,
	struct value *value)
{
	unsigned long long result;
	unsigned long long operand;
	unsigned long long count;
	struct node *argument;

	/* The first number. */
	result = (unsigned long long)number_of(call->arguments) & BIT_MASK;

	/* compl: the bits turned round. */
	if (call->builtin == BUILTIN_COMPL) {
		value_set_number(value, (double)(~result & BIT_MASK));
		return;
	}

	/* The shifts, by the second number. */
	if (call->builtin == BUILTIN_LSHIFT || call->builtin == BUILTIN_RSHIFT) {
		count = (unsigned long long)number_of(call->arguments->next);
		if (count > 63U)
			result = 0;
		else if (call->builtin == BUILTIN_LSHIFT)
			result = (result << count) & BIT_MASK;
		else
			result = result >> count;
		value_set_number(value, (double)result);
		return;
	}

	/* and, or and xor over every argument after the first. */
	for (argument = call->arguments->next; argument != NULL;
	     argument = argument->next) {
		operand = (unsigned long long)number_of(argument) & BIT_MASK;
		if (call->builtin == BUILTIN_AND)
			result &= operand;
		else if (call->builtin == BUILTIN_OR)
			result |= operand;
		else
			result ^= operand;
	}

	/* Succeeded. */
	value_set_number(value, (double)result);
}

/*
 * asort(source[, destination]) and asorti (keys set): the values (or the
 * keys) of source sorted, into destination (source itself without one) at
 * 1 to n.  Numbers come before strings.  The value is n.
 */
static void
gawk_sort(
	struct node *call,
	int keys,
	struct value *value)
{
	struct sorted_item *items;
	struct element *element;
	struct array *source;
	struct array *destination;
	char number[24];
	size_t count;
	size_t index;
	size_t length;

	/* The source's values and keys, copied. */
	source = node_array(call->arguments);
	count = source->count;
	items = awk_allocate(sizeof(*items) * (count + 1U));
	index = 0;
	for (element = source->first; element != NULL; element = element->next) {
		memset(&items[index].value, 0, sizeof(items[index].value));
		value_copy(&items[index].value, &element->value);
		items[index].key = awk_copy(element->key, element->key_length);
		items[index].key_length = element->key_length;
		index++;
	}

	/* Sorted by value, or by key. */
	if (keys)
		qsort(items, count, sizeof(*items), compare_keys);
	else
		qsort(items, count, sizeof(*items), compare_items);

	/* The destination, emptied, gets them at 1 to n. */
	destination = source;
	if (call->arguments->next != NULL)
		destination = node_array(call->arguments->next);
	array_clear(destination);
	for (index = 0; index < count; index++) {
		length = (size_t)snprintf(number, sizeof(number), "%lu", (unsigned long)index + 1UL);
		if (keys) {
			set_text_element(destination, number, length,
					 items[index].key, items[index].key_length);
		} else {
			set_element(destination, number, length, &items[index].value);
		}

		/* The copies are done with. */
		value_free(&items[index].value);
		free(items[index].key);
	}

	/* Succeeded: how many. */
	free(items);
	value_set_number(value, (double)count);
}

/* Compares two values for asort: numbers first, numerically, then strings. */
static int
compare_items(
	const void *left,
	const void *right)
{
	const struct sorted_item *a;
	const struct sorted_item *b;
	struct value text_a;
	struct value text_b;
	double number_a;
	double number_b;
	int numeric_a;
	int numeric_b;
	int result;

	/* Which of them are numbers. */
	a = left;
	b = right;
	numeric_a = value_is_numeric(&a->value);
	numeric_b = value_is_numeric(&b->value);
	if (numeric_a && !numeric_b)
		return -1;
	if (!numeric_a && numeric_b)
		return 1;

	/* Two numbers. */
	if (numeric_a) {
		number_a = value_number(&a->value);
		number_b = value_number(&b->value);
		if (number_a < number_b)
			return -1;
		if (number_a > number_b)
			return 1;
		return 0;
	}

	/* Two strings. */
	memset(&text_a, 0, sizeof(text_a));
	memset(&text_b, 0, sizeof(text_b));
	value_string(&a->value, 0, &text_a);
	value_string(&b->value, 0, &text_b);
	result = strcmp(text_a.text, text_b.text);
	value_free(&text_a);
	value_free(&text_b);

	/* Succeeded. */
	return result;
}

/* Compares two keys for asorti, as strings. */
static int
compare_keys(
	const void *left,
	const void *right)
{
	const struct sorted_item *a;
	const struct sorted_item *b;
	int result;

	/* The keys' bytes. */
	a = left;
	b = right;
	result = strcmp(a->key, b->key);

	/* Succeeded. */
	return result;
}

/* Sets an element of an array to a value (NULL leaves it unset). */
static void
set_element(
	struct array *array,
	const char *key,
	size_t length,
	const struct value *value)
{
	struct element *element;

	/* The element, made when it is not there. */
	element = array_find(array, key, length, 1);
	if (value != NULL)
		value_copy(&element->value, value);
}

/* Sets an element of an array to a string. */
static void
set_text_element(
	struct array *array,
	const char *key,
	size_t length,
	const char *text,
	size_t text_length)
{
	struct element *element;

	/* The element, made when it is not there. */
	element = array_find(array, key, length, 1);
	value_set_text(&element->value, text, text_length);
}

/* Sets an element of an array to a number. */
static void
set_number_element(
	struct array *array,
	const char *key,
	size_t length,
	double number)
{
	struct element *element;

	/* The element, made when it is not there. */
	element = array_find(array, key, length, 1);
	value_set_number(&element->value, number);
}
