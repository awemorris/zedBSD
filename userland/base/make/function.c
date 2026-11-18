/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The functions of make: $(name arguments), GNU make's text, file-name,
 * conditional, looping, call and eval, variable-inquiry, shell and
 * message functions.
 *
 * A reference is a function when its first word, followed by a blank,
 * names one.  The arguments are separated by the commas outside any
 * brackets; a function takes at most a fixed number of them, and the last
 * one keeps any further commas.  Most functions expand every argument
 * first; if, or, and, foreach and call expand only what they use, when
 * they use it.
 */

#include "make.h"

#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* How many arguments a function may take at most. */
#define FUNCTION_ARGUMENT_MAX 64

/*
 * A function of the table: its name, how many arguments it needs and may
 * take, whether its arguments are expanded before it runs, and what it
 * does.
 */
struct function {
	const char *name;
	size_t minimum;
	size_t maximum;
	int expand_arguments;
	void (*run)(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
};

static const struct function *find_function(const char *name, size_t length);
static size_t split_arguments(const char *text, size_t length, size_t maximum, char **arguments);
static void add_word(struct buffer *out, const char *word, size_t length, int *first);
static int match_pattern(const char *word, size_t length, const char *prefix, size_t prefix_length, const char *suffix, size_t suffix_length);
static int matches_any(const char *word, size_t length, const char *patterns);
static char *stripped(const char *text);
static long word_number(const char *text, const char *function);
static int compare_strings(const void *left, const void *right);
static void run_subst(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_patsubst(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_strip(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_findstring(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_filter(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_filter_out(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_sort(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_word(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_wordlist(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_words(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_firstword(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_lastword(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_dir(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_notdir(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_suffix(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_basename(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_addsuffix(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_addprefix(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_join(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_wildcard(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_realpath(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_abspath(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_if(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_or(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_and(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_foreach(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_call(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_eval(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_value(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_origin(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_flavor(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_shell(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_info(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_warning(const struct expansion *context, char **arguments, size_t count, struct buffer *out);
static void run_error(const struct expansion *context, char **arguments, size_t count, struct buffer *out);

/* The functions, by name. */
static const struct function functions[] = {
	{"subst", 3, 3, 1, run_subst},
	{"patsubst", 3, 3, 1, run_patsubst},
	{"strip", 1, 1, 1, run_strip},
	{"findstring", 2, 2, 1, run_findstring},
	{"filter", 2, 2, 1, run_filter},
	{"filter-out", 2, 2, 1, run_filter_out},
	{"sort", 1, 1, 1, run_sort},
	{"word", 2, 2, 1, run_word},
	{"wordlist", 3, 3, 1, run_wordlist},
	{"words", 1, 1, 1, run_words},
	{"firstword", 1, 1, 1, run_firstword},
	{"lastword", 1, 1, 1, run_lastword},
	{"dir", 1, 1, 1, run_dir},
	{"notdir", 1, 1, 1, run_notdir},
	{"suffix", 1, 1, 1, run_suffix},
	{"basename", 1, 1, 1, run_basename},
	{"addsuffix", 2, 2, 1, run_addsuffix},
	{"addprefix", 2, 2, 1, run_addprefix},
	{"join", 2, 2, 1, run_join},
	{"wildcard", 1, 1, 1, run_wildcard},
	{"realpath", 1, 1, 1, run_realpath},
	{"abspath", 1, 1, 1, run_abspath},
	{"if", 2, 3, 0, run_if},
	{"or", 1, FUNCTION_ARGUMENT_MAX, 0, run_or},
	{"and", 1, FUNCTION_ARGUMENT_MAX, 0, run_and},
	{"foreach", 3, 3, 0, run_foreach},
	{"call", 1, FUNCTION_ARGUMENT_MAX, 1, run_call},
	{"eval", 1, 1, 1, run_eval},
	{"value", 1, 1, 1, run_value},
	{"origin", 1, 1, 1, run_origin},
	{"flavor", 1, 1, 1, run_flavor},
	{"shell", 1, 1, 1, run_shell},
	{"info", 1, 1, 1, run_info},
	{"warning", 1, 1, 1, run_warning},
	{"error", 1, 1, 1, run_error}
};

/*
 * Expands a function call when the reference names one; returns 0 when it
 * does not, and the reference is a variable.
 */
int
function_call(
	const struct expansion *context,
	const char *text,
	size_t length,
	struct buffer *out)
{
	const struct function *function;
	char *arguments[FUNCTION_ARGUMENT_MAX];
	char *expanded;
	size_t name_length;
	size_t count;
	size_t index;

	/* The name runs to the first blank, which must be there. */
	name_length = 0;
	while (name_length < length && text[name_length] != ' ' && text[name_length] != '\t')
		name_length++;
	if (name_length == length)
		return 0;
	function = find_function(text, name_length);
	if (function == NULL)
		return 0;

	/* The arguments after the blanks. */
	while (name_length < length && (text[name_length] == ' ' || text[name_length] == '\t'))
		name_length++;
	count = split_arguments(text + name_length, length - name_length, function->maximum, arguments);
	if (count < function->minimum) {
		if (context->file != NULL)
			make_file_fatal(context->file, context->line, "insufficient number of arguments (%lu) to function '%s'", (unsigned long)count, function->name);
		make_fatal("insufficient number of arguments (%lu) to function '%s'", (unsigned long)count, function->name);
	}

	/* Most functions have their arguments expanded first. */
	if (function->expand_arguments) {
		for (index = 0; index < count; index++) {
			expanded = expand(context, arguments[index]);
			free(arguments[index]);
			arguments[index] = expanded;
		}
	}

	/* The function, then its arguments are done with. */
	function->run(context, arguments, count, out);
	for (index = 0; index < count; index++)
		free(arguments[index]);

	/* Succeeded: it was a function. */
	return 1;
}

/*
 * Appends the words of a value with each word that matches a pattern (one
 * % standing for any text) replaced by the replacement, whose % becomes
 * the matched text.  Words are joined by single spaces.  A pattern
 * without % matches only the whole word.
 */
void
function_patsubst_words(
	struct buffer *out,
	const char *value,
	const char *pattern,
	const char *replacement)
{
	const char *percent;
	const char *replacement_percent;
	const char *cursor;
	const char *word;
	size_t length;
	size_t prefix_length;
	size_t suffix_length;
	size_t replacement_prefix;
	int matched;
	int first;

	/* The pattern around its %; without one the whole pattern is a prefix to match. */
	percent = strchr(pattern, '%');
	prefix_length = strlen(pattern);
	suffix_length = 0;
	if (percent != NULL) {
		prefix_length = (size_t)(percent - pattern);
		suffix_length = strlen(percent + 1);
	}

	/* The replacement's own %, where the matched text goes. */
	replacement_percent = strchr(replacement, '%');

	/* Each word, replaced when it matches. */
	cursor = value;
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (!first)
			buffer_add_char(out, ' ');
		first = 0;

		/* A pattern without % matches the word exactly. */
		if (percent == NULL) {
			matched = 0;
			if (length == prefix_length)
				matched = match_pattern(word, length, pattern, prefix_length, "", 0);
		} else {
			matched = match_pattern(word, length, pattern, prefix_length, percent + 1, suffix_length);
		}

		/* A word that does not match stays as it is. */
		if (!matched) {
			buffer_add(out, word, length);
			continue;
		}

		/* The replacement, with the matched text in place of its %. */
		if (replacement_percent == NULL || percent == NULL) {
			buffer_add_string(out, replacement);
			continue;
		}

		/* The replacement around the matched text. */
		replacement_prefix = (size_t)(replacement_percent - replacement);
		buffer_add(out, replacement, replacement_prefix);
		buffer_add(out, word + prefix_length, length - prefix_length - suffix_length);
		buffer_add_string(out, replacement_percent + 1);
	}
}

/* Returns the function of a name, or NULL. */
static const struct function *
find_function(
	const char *name,
	size_t length)
{
	size_t index;
	int compare;

	/* Each function of the table. */
	for (index = 0; index < sizeof(functions) / sizeof(functions[0]); index++) {
		compare = strncmp(functions[index].name, name, length);
		if (compare == 0 && functions[index].name[length] == '\0')
			return &functions[index];
	}

	/* No such function. */
	return NULL;
}

/*
 * Splits the arguments of a function at the commas outside brackets, into
 * allocated copies; the last of maximum arguments keeps the commas after
 * it.  Returns how many there are (an empty text is one empty argument).
 */
static size_t
split_arguments(
	const char *text,
	size_t length,
	size_t maximum,
	char **arguments)
{
	size_t start;
	size_t index;
	size_t count;
	int depth;

	/* Each comma at the top level ends an argument, until the last one. */
	count = 0;
	start = 0;
	depth = 0;
	for (index = 0; index < length; index++) {
		if (text[index] == '(' || text[index] == '{') {
			depth++;
		} else if (text[index] == ')' || text[index] == '}') {
			if (depth > 0)
				depth--;
		} else if (text[index] == ',' && depth == 0 && count + 1U < maximum) {
			arguments[count] = make_strndup(text + start, index - start);
			count++;
			start = index + 1U;
		}
	}

	/* Succeeded: the last argument, which runs to the end. */
	arguments[count] = make_strndup(text + start, length - start);
	count++;
	return count;
}

/* Appends a word to a space-separated list, *first telling whether it is the first. */
static void
add_word(
	struct buffer *out,
	const char *word,
	size_t length,
	int *first)
{
	/* A space between words. */
	if (!*first)
		buffer_add_char(out, ' ');
	*first = 0;

	/* The word. */
	buffer_add(out, word, length);
}

/* Reports whether a word starts with a prefix and ends with a suffix that do not overlap. */
static int
match_pattern(
	const char *word,
	size_t length,
	const char *prefix,
	size_t prefix_length,
	const char *suffix,
	size_t suffix_length)
{
	int compare;

	/* The word must hold both. */
	if (length < prefix_length + suffix_length)
		return 0;

	/* The prefix at the start. */
	compare = strncmp(word, prefix, prefix_length);
	if (compare != 0)
		return 0;

	/* The suffix at the end. */
	compare = strncmp(word + length - suffix_length, suffix, suffix_length);
	if (compare != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Reports whether a word matches any of the patterns of a list (each with at most one %). */
static int
matches_any(
	const char *word,
	size_t length,
	const char *patterns)
{
	const char *cursor;
	const char *pattern;
	const char *percent;
	size_t pattern_length;
	size_t prefix_length;
	int matched;

	/* Each pattern. */
	cursor = patterns;
	for (;;) {
		pattern = make_next_word(&cursor, &pattern_length);
		if (pattern == NULL)
			break;

		/* The % divides the pattern into a prefix and a suffix; without one it is the word itself. */
		percent = memchr(pattern, '%', pattern_length);
		if (percent == NULL) {
			matched = 0;
			if (pattern_length == length)
				matched = match_pattern(word, length, pattern, pattern_length, "", 0);
		} else {
			prefix_length = (size_t)(percent - pattern);
			matched = match_pattern(word, length, pattern, prefix_length, percent + 1, pattern_length - prefix_length - 1U);
		}

		/* One pattern that matches is enough. */
		if (matched)
			return 1;
	}

	/* None matches. */
	return 0;
}

/* Returns an allocated copy of a text with its words joined by single spaces. */
static char *
stripped(
	const char *text)
{
	struct buffer out;
	const char *cursor;
	const char *word;
	size_t length;
	int first;

	/* Each word. */
	memset(&out, 0, sizeof(out));
	cursor = text;
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		add_word(&out, word, length, &first);
	}

	/* Succeeded. */
	return buffer_finish(&out);
}

/* Reads the number argument of word or wordlist; ends make when it is not a number. */
static long
word_number(
	const char *text,
	const char *function)
{
	char *clean;
	char *end;
	long number;

	/* A decimal number, blanks around it allowed. */
	clean = stripped(text);
	number = strtol(clean, &end, 10);
	if (clean[0] == '\0' || *end != '\0') {
		free(clean);
		make_fatal("non-numeric argument to '%s' function", function);
	}

	/* Succeeded. */
	free(clean);
	return number;
}

/* Orders two strings for qsort. */
static int
compare_strings(
	const void *left,
	const void *right)
{
	const char *const *first;
	const char *const *second;
	int order;

	/* By their bytes. */
	first = left;
	second = right;
	order = strcmp(*first, *second);

	/* Succeeded. */
	return order;
}

/* $(subst from,to,text): every occurrence of from replaced by to. */
static void
run_subst(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *found;
	size_t from_length;

	/* An empty from changes nothing. */
	(void)context;
	(void)count;
	from_length = strlen(arguments[0]);
	if (from_length == 0) {
		buffer_add_string(out, arguments[2]);
		return;
	}

	/* Each occurrence, and the text between them. */
	cursor = arguments[2];
	for (;;) {
		found = strstr(cursor, arguments[0]);
		if (found == NULL)
			break;
		buffer_add(out, cursor, (size_t)(found - cursor));
		buffer_add_string(out, arguments[1]);
		cursor = found + from_length;
	}

	/* The text after the last one. */
	buffer_add_string(out, cursor);
}

/* $(patsubst pattern,replacement,text). */
static void
run_patsubst(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	/* The shared pattern substitution. */
	(void)context;
	(void)count;
	function_patsubst_words(out, arguments[2], arguments[0], arguments[1]);
}

/* $(strip text): the words joined by single spaces. */
static void
run_strip(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	char *clean;

	/* The words. */
	(void)context;
	(void)count;
	clean = stripped(arguments[0]);
	buffer_add_string(out, clean);
	free(clean);
}

/* $(findstring find,in): find when it occurs in in, else nothing. */
static void
run_findstring(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *found;

	/* The text itself when it is there. */
	(void)context;
	(void)count;
	found = strstr(arguments[1], arguments[0]);
	if (found != NULL)
		buffer_add_string(out, arguments[0]);
}

/* $(filter patterns,text): the words that match a pattern. */
static void
run_filter(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	int first;
	int matched;

	/* Each word that matches. */
	(void)context;
	(void)count;
	cursor = arguments[1];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		matched = matches_any(word, length, arguments[0]);
		if (matched)
			add_word(out, word, length, &first);
	}
}

/* $(filter-out patterns,text): the words that match no pattern. */
static void
run_filter_out(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	int first;
	int matched;

	/* Each word that does not match. */
	(void)context;
	(void)count;
	cursor = arguments[1];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		matched = matches_any(word, length, arguments[0]);
		if (!matched)
			add_word(out, word, length, &first);
	}
}

/* $(sort list): the words in order, each once. */
static void
run_sort(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	char **words;
	size_t length;
	size_t word_count;
	size_t capacity;
	size_t index;
	int first;
	int compare;

	/* The words into an array. */
	(void)context;
	(void)count;
	words = NULL;
	word_count = 0;
	capacity = 0;
	cursor = arguments[0];
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (word_count == capacity) {
			capacity = capacity * 2U + 16U;
			words = make_realloc(words, capacity * sizeof(*words));
		}

		/* The word. */
		words[word_count] = make_strndup(word, length);
		word_count++;
	}

	/* Sorted, and each once. */
	if (word_count > 0)
		qsort(words, word_count, sizeof(*words), compare_strings);
	first = 1;
	for (index = 0; index < word_count; index++) {
		compare = 1;
		if (index > 0)
			compare = strcmp(words[index], words[index - 1U]);
		if (compare != 0)
			add_word(out, words[index], strlen(words[index]), &first);
	}

	/* The array is done with. */
	for (index = 0; index < word_count; index++)
		free(words[index]);
	free(words);
}

/* $(word n,text): the nth word, counting from 1. */
static void
run_word(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	long number;
	long index;

	/* The number must be positive. */
	(void)context;
	(void)count;
	number = word_number(arguments[0], "word");
	if (number <= 0)
		make_fatal("first argument to 'word' function must be greater than 0");

	/* The word at that place, if there is one. */
	cursor = arguments[1];
	for (index = 1; ; index++) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (index == number) {
			buffer_add(out, word, length);
			break;
		}
	}
}

/* $(wordlist s,e,text): the words from the sth to the eth. */
static void
run_wordlist(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	long start;
	long end;
	long index;
	int first;

	/* The start must be positive. */
	(void)context;
	(void)count;
	start = word_number(arguments[0], "wordlist");
	end = word_number(arguments[1], "wordlist");
	if (start <= 0)
		make_fatal("invalid first argument to 'wordlist' function: '%ld'", start);

	/* The words in the range. */
	cursor = arguments[2];
	first = 1;
	for (index = 1; index <= end; index++) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (index >= start)
			add_word(out, word, length, &first);
	}
}

/* $(words text): how many words. */
static void
run_words(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	char number[32];
	size_t length;
	unsigned long words;

	/* Each word counts. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	words = 0;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		words++;
	}

	/* The count. */
	snprintf(number, sizeof(number), "%lu", words);
	buffer_add_string(out, number);
}

/* $(firstword text). */
static void
run_firstword(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;

	/* The first word, if any. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	word = make_next_word(&cursor, &length);
	if (word != NULL)
		buffer_add(out, word, length);
}

/* $(lastword text). */
static void
run_lastword(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	const char *last;
	size_t length;
	size_t last_length;

	/* Each word, keeping the last. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	last = NULL;
	last_length = 0;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		last = word;
		last_length = length;
	}

	/* The last word, if any. */
	if (last != NULL)
		buffer_add(out, last, last_length);
}

/* $(dir names): each name's directory part with its slash, or ./ when there is none. */
static void
run_dir(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	size_t end;
	int first;

	/* Each name up to and with its last slash. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		end = length;
		while (end > 0 && word[end - 1U] != '/')
			end--;
		if (end == 0) {
			add_word(out, "./", 2, &first);
		} else {
			add_word(out, word, end, &first);
		}
	}
}

/* $(notdir names): each name after its last slash. */
static void
run_notdir(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	size_t start;
	int first;

	/* Each name from after its last slash. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		start = length;
		while (start > 0 && word[start - 1U] != '/')
			start--;
		add_word(out, word + start, length - start, &first);
	}
}

/* $(suffix names): each name's suffix (from its last dot after its last slash); names without one give nothing. */
static void
run_suffix(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	size_t dot;
	int first;

	/* Each name with a dot in its file part. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		dot = length;
		while (dot > 0 && word[dot - 1U] != '.' && word[dot - 1U] != '/')
			dot--;
		if (dot > 0 && word[dot - 1U] == '.')
			add_word(out, word + dot - 1U, length - dot + 1U, &first);
	}
}

/* $(basename names): each name without its suffix. */
static void
run_basename(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	size_t dot;
	int first;

	/* Each name up to its suffix's dot, or whole. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		dot = length;
		while (dot > 0 && word[dot - 1U] != '.' && word[dot - 1U] != '/')
			dot--;
		if (dot > 0 && word[dot - 1U] == '.') {
			add_word(out, word, dot - 1U, &first);
		} else {
			add_word(out, word, length, &first);
		}
	}
}

/* $(addsuffix suffix,names). */
static void
run_addsuffix(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	int first;

	/* Each name, then the suffix. */
	(void)context;
	(void)count;
	cursor = arguments[1];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		add_word(out, word, length, &first);
		buffer_add_string(out, arguments[0]);
	}
}

/* $(addprefix prefix,names). */
static void
run_addprefix(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	size_t length;
	int first;

	/* The prefix, then each name. */
	(void)context;
	(void)count;
	cursor = arguments[1];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		add_word(out, arguments[0], strlen(arguments[0]), &first);
		buffer_add(out, word, length);
	}
}

/* $(join list1,list2): the words joined pairwise; the longer list's extra words stay. */
static void
run_join(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *left_cursor;
	const char *right_cursor;
	const char *left;
	const char *right;
	size_t left_length;
	size_t right_length;
	int first;

	/* Word by word from both lists. */
	(void)context;
	(void)count;
	left_cursor = arguments[0];
	right_cursor = arguments[1];
	first = 1;
	for (;;) {
		left = make_next_word(&left_cursor, &left_length);
		right = make_next_word(&right_cursor, &right_length);
		if (left == NULL && right == NULL)
			break;
		if (left == NULL) {
			add_word(out, right, right_length, &first);
			continue;
		}

		/* The left word, then the right one after it. */
		add_word(out, left, left_length, &first);
		if (right != NULL)
			buffer_add(out, right, right_length);
	}
}

/* $(wildcard patterns): the existing files each pattern matches, in order. */
static void
run_wildcard(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	glob_t found;
	char *pattern;
	size_t length;
	size_t index;
	int first;
	int error;

	/* Each pattern through glob. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		pattern = make_strndup(word, length);
		memset(&found, 0, sizeof(found));
		error = glob(pattern, 0, NULL, &found);
		free(pattern);
		if (error != 0) {
			globfree(&found);
			continue;
		}

		/* The matches, sorted by glob. */
		for (index = 0; index < found.gl_pathc; index++)
			add_word(out, found.gl_pathv[index], strlen(found.gl_pathv[index]), &first);
		globfree(&found);
	}
}

/* $(realpath names): the canonical absolute name of each that exists. */
static void
run_realpath(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	const char *cursor;
	const char *word;
	char resolved[PATH_MAX];
	char *name;
	char *result;
	size_t length;
	int first;

	/* Each name the system can resolve. */
	(void)context;
	(void)count;
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		name = make_strndup(word, length);
		result = realpath(name, resolved);
		free(name);
		if (result != NULL)
			add_word(out, resolved, strlen(resolved), &first);
	}
}

/*
 * $(abspath names): each name made absolute against the current directory
 * and cleaned of ., .. and repeated slashes, without looking at the files.
 */
static void
run_abspath(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	struct variable *directory;
	struct buffer path;
	const char *cursor;
	const char *word;
	const char *part;
	const char *part_cursor;
	size_t length;
	size_t part_length;
	size_t kept;
	int first;
	int compare;

	/* Relative names start from $(CURDIR). */
	(void)count;
	directory = variable_lookup(context->scope, "CURDIR", 6);
	cursor = arguments[0];
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;

		/* The name after the directory, as one path. */
		memset(&path, 0, sizeof(path));
		if (word[0] != '/' && directory != NULL)
			buffer_add_string(&path, directory->value);
		buffer_add_char(&path, '/');
		buffer_add(&path, word, length);

		/* Each component of it, dropping . and going back for .. */
		part_cursor = path.text;
		if (!first)
			buffer_add_char(out, ' ');
		first = 0;
		kept = out->length;
		for (;;) {
			while (*part_cursor == '/')
				part_cursor++;
			if (*part_cursor == '\0')
				break;
			part = part_cursor;
			while (*part_cursor != '\0' && *part_cursor != '/' && *part_cursor != ' ')
				part_cursor++;
			part_length = (size_t)(part_cursor - part);
			compare = strncmp(part, ".", part_length);
			if (part_length == 1 && compare == 0)
				continue;
			compare = strncmp(part, "..", part_length);
			if (part_length == 2 && compare == 0) {
				while (out->length > kept && out->text[out->length - 1U] != '/')
					out->length--;
				if (out->length > kept)
					out->length--;
				out->text[out->length] = '\0';
				continue;
			}

			/* The component stays. */
			buffer_add_char(out, '/');
			buffer_add(out, part, part_length);
		}

		/* The root alone is /. */
		if (out->length == kept)
			buffer_add_char(out, '/');
		free(path.text);
	}
}

/* $(if condition,then[,else]): then when the condition expands to anything but blanks, else else. */
static void
run_if(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	char *condition;
	char *clean;
	int holds;

	/* The condition, blanks around it removed. */
	condition = expand(context, arguments[0]);
	clean = stripped(condition);
	holds = 0;
	if (clean[0] != '\0')
		holds = 1;
	free(condition);
	free(clean);

	/* Only the chosen branch is expanded. */
	if (holds) {
		expand_into(context, arguments[1], strlen(arguments[1]), out);
	} else if (count > 2) {
		expand_into(context, arguments[2], strlen(arguments[2]), out);
	}
}

/* $(or a,b,...): the first argument that expands to anything but blanks. */
static void
run_or(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	char *value;
	char *clean;
	size_t index;
	int empty;

	/* Each argument in turn, until one is not empty. */
	for (index = 0; index < count; index++) {
		value = expand(context, arguments[index]);
		clean = stripped(value);
		empty = 0;
		if (clean[0] == '\0')
			empty = 1;
		free(clean);
		if (!empty) {
			buffer_add_string(out, value);
			free(value);
			return;
		}

		/* An empty one is skipped. */
		free(value);
	}
}

/* $(and a,b,...): nothing when an argument is empty, else the last one. */
static void
run_and(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	char *value;
	char *clean;
	size_t index;
	int empty;

	/* Each argument in turn, stopping at an empty one. */
	value = NULL;
	for (index = 0; index < count; index++) {
		free(value);
		value = expand(context, arguments[index]);
		clean = stripped(value);
		empty = 0;
		if (clean[0] == '\0')
			empty = 1;
		free(clean);
		if (empty) {
			free(value);
			return;
		}
	}

	/* Succeeded: the last argument's value. */
	buffer_add_string(out, value);
	free(value);
}

/* $(foreach name,list,text): text expanded once for each word, with name set to it. */
static void
run_foreach(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	struct variable_scope scope;
	struct expansion inner;
	char *name;
	char *list;
	char *word_copy;
	const char *cursor;
	const char *word;
	size_t length;
	int first;

	/* The name and the list, expanded; the text is expanded for each word. */
	(void)count;
	name = expand(context, arguments[0]);
	list = expand(context, arguments[1]);
	scope.set = variable_new_set();
	scope.parent = context->scope;
	inner = *context;
	inner.scope = &scope;

	/* Each word. */
	cursor = list;
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		word_copy = make_strndup(word, length);
		variable_set_value(scope.set, name, word_copy, FLAVOR_SIMPLE, ORIGIN_AUTOMATIC);
		free(word_copy);
		if (!first)
			buffer_add_char(out, ' ');
		first = 0;
		expand_into(&inner, arguments[2], strlen(arguments[2]), out);
	}

	/* The loop's variable is gone after it. */
	variable_free_set(scope.set);
	free(name);
	free(list);
}

/* $(call name,a,b,...): the variable's value expanded with $(0) the name and $(1)... the arguments. */
static void
run_call(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	struct variable_scope scope;
	struct expansion inner;
	struct variable *variable;
	char *name;
	char number[32];
	size_t index;

	/* The variable. */
	name = stripped(arguments[0]);
	variable = variable_lookup(context->scope, name, strlen(name));
	if (variable == NULL) {
		free(name);
		return;
	}

	/* The parameters in a table of their own, over the caller's scope. */
	scope.set = variable_new_set();
	scope.parent = context->scope;
	for (index = 0; index < count; index++) {
		snprintf(number, sizeof(number), "%lu", (unsigned long)index);
		if (index == 0) {
			variable_set_value(scope.set, number, name, FLAVOR_SIMPLE, ORIGIN_AUTOMATIC);
		} else {
			variable_set_value(scope.set, number, arguments[index], FLAVOR_SIMPLE, ORIGIN_AUTOMATIC);
		}
	}

	/* The value, expanded in that scope when it is recursive. */
	inner = *context;
	inner.scope = &scope;
	if (variable->flavor == FLAVOR_SIMPLE) {
		buffer_add_string(out, variable->value);
	} else {
		expand_into(&inner, variable->value, strlen(variable->value), out);
	}

	/* The parameters are gone after the call. */
	variable_free_set(scope.set);
	free(name);
}

/* $(eval text): the text is read as makefile lines; the value is empty. */
static void
run_eval(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	/* The lines, as if in the makefile where the eval is. */
	(void)count;
	(void)out;
	read_eval(arguments[0], context->file, context->line);
}

/* $(value name): the variable's value, unexpanded. */
static void
run_value(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	struct variable *variable;
	char *name;

	/* The variable, if there is one. */
	(void)count;
	name = stripped(arguments[0]);
	variable = variable_lookup(context->scope, name, strlen(name));
	if (variable != NULL)
		buffer_add_string(out, variable->value);
	free(name);
}

/* $(origin name): where the variable came from, in GNU make's words. */
static void
run_origin(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	static const char *const words[] = {
		"default", "environment", "file", "environment override",
		"command line", "override", "automatic"
	};
	struct variable *variable;
	const char *automatic;
	char *name;
	size_t length;

	/* An automatic variable of the recipe, or one of the scope. */
	(void)count;
	name = stripped(arguments[0]);
	length = strlen(name);
	variable = variable_lookup(context->scope, name, length);
	automatic = NULL;
	if (length == 1)
		automatic = strchr("@<?^+*|%", name[0]);
	if (variable == NULL && context->automatic != NULL && automatic != NULL) {
		buffer_add_string(out, "automatic");
		free(name);
		return;
	}

	/* The name is done with. */
	free(name);

	/* An undefined variable. */
	if (variable == NULL) {
		buffer_add_string(out, "undefined");
		return;
	}

	/* Succeeded: the word of its origin. */
	buffer_add_string(out, words[variable->origin]);
}

/* $(flavor name): undefined, recursive or simple. */
static void
run_flavor(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	struct variable *variable;
	char *name;

	/* The variable. */
	(void)count;
	name = stripped(arguments[0]);
	variable = variable_lookup(context->scope, name, strlen(name));
	free(name);

	/* Its flavor. */
	if (variable == NULL) {
		buffer_add_string(out, "undefined");
	} else if (variable->flavor == FLAVOR_SIMPLE) {
		buffer_add_string(out, "simple");
	} else {
		buffer_add_string(out, "recursive");
	}
}

/* $(shell command): the command's output, trailing newlines dropped and the others made spaces. */
static void
run_shell(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	char *output;
	size_t length;
	size_t index;
	int status;

	/* The command's output. */
	(void)context;
	(void)count;
	output = job_shell_output(arguments[0], &status);

	/* Trailing newlines go. */
	length = strlen(output);
	while (length > 0 && output[length - 1U] == '\n')
		length--;

	/* Each other newline becomes a space. */
	for (index = 0; index < length; index++) {
		if (output[index] == '\n')
			output[index] = ' ';
	}

	/* The output, less its trailing newlines. */
	buffer_add(out, output, length);
	free(output);
}

/* $(info text): the text on standard output. */
static void
run_info(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	/* The text and a newline. */
	(void)context;
	(void)count;
	(void)out;
	printf("%s\n", arguments[0]);
}

/* $(warning text): the text on standard error, after the makefile's place. */
static void
run_warning(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	/* What make wrote so far goes first. */
	(void)count;
	(void)out;
	fflush(stdout);

	/* The place, when there is one, then the text. */
	if (context->file != NULL) {
		fprintf(stderr, "%s:%ld: %s\n", context->file, context->line, arguments[0]);
	} else {
		fprintf(stderr, "%s\n", arguments[0]);
	}
}

/* $(error text): make stops with the text. */
static void
run_error(
	const struct expansion *context,
	char **arguments,
	size_t count,
	struct buffer *out)
{
	/* The place, when there is one, then the text and Stop. */
	(void)count;
	(void)out;
	if (context->file != NULL)
		make_file_fatal(context->file, context->line, "%s", arguments[0]);
	make_fatal("%s", arguments[0]);
}
