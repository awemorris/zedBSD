/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reading of makefiles.
 *
 * A makefile is read a logical line at a time: a line that ends with a
 * backslash goes on on the next.  A line that starts with a tab after a
 * rule is a line of that rule's recipe and is kept as it is.  Any other
 * line loses its comment and is a directive (include, the conditionals,
 * define, export, unexport, override), an assignment, or a rule, told
 * apart by the first : or = outside any $(...).  Targets and
 * prerequisites are expanded when the rule is read; recipes when they
 * run.
 */

#include "make.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* How deep conditionals may nest in one makefile. */
#define READ_CONDITIONAL_MAX 64

/* The conditional keywords, in the order conditional_keyword() knows them. */
enum condition_keyword {
	CONDITION_IFEQ,
	CONDITION_IFNEQ,
	CONDITION_IFDEF,
	CONDITION_IFNDEF
};

/*
 * One open conditional.  Lines are used only when every open conditional
 * is in a branch that was taken.
 */
struct conditional {
	int taken;			/* the lines of the current branch are used */
	int any_taken;			/* a branch before this one was taken */
	int seen_else;			/* the final else was seen */
	int outer_taken;		/* the lines around the conditional are used */
};

/*
 * The state of reading one makefile: its stream, where the reader is,
 * the rule whose recipe it is reading, and the open conditionals.  It
 * lives on the stack of read_makefile() for one file.
 */
struct reader {
	const char *file;
	FILE *stream;
	const char *text;		/* the built-in rules, read instead of a stream */
	size_t position;
	int builtin;			/* the built-in rules: their variables are defaults */
	long line;			/* the first physical line of the current logical line */
	long next_line;			/* the next physical line to read */
	struct target **rule_targets;	/* the targets of the rule being read, or NULL */
	size_t rule_count;
	struct recipe *recipe;		/* that rule's recipe */
	struct conditional conditionals[READ_CONDITIONAL_MAX];
	int depth;
};

/* How many makefiles (not the built-in rules) were read. */
static size_t makefiles_read;

/*
 * The include files that were not found, for main() to try to make: the
 * names, and whether each came from include (1) or -include (0).
 */
static char **missing_includes;
static int *missing_must_exist;
static size_t missing_count;

/* The directories -I names, searched for an include file that is not found as named. */
static char **include_directories;
static size_t include_directory_count;

static void read_all_lines(struct reader *reader);
static int next_character(struct reader *reader);
static enum variable_origin file_origin(const struct reader *reader);
static int read_logical_line(struct reader *reader, struct buffer *line);
static void process_line(struct reader *reader, char *line);
static void end_rule(struct reader *reader);
static void add_recipe_line(struct reader *reader, const char *line);
static void join_continuations(char *line);
static void strip_comment(char *line);
static char *skip_blanks(char *text);
static void trim_trailing(char *text);
static int word_is(const char *text, const char *word, char **rest);
static int is_used(const struct reader *reader);
static int conditional_directive(struct reader *reader, char *text);
static int conditional_keyword(const char *text, size_t *length);
static int evaluate_condition(struct reader *reader, int keyword, char *arguments);
static int split_comparison(char *arguments, char **first, char **second);
static void read_define(struct reader *reader, char *rest, enum variable_origin origin, int export);
static void skip_define(struct reader *reader);
static void process_include(struct reader *reader, char *rest, int must_exist);
static void process_export(struct reader *reader, char *rest, int export);
static int find_separator(const char *text, const char **separator, enum assign_kind *kind, size_t *operator_length);
static void process_assignment(struct reader *reader, char *text, const char *separator, enum assign_kind kind, size_t operator_length, enum variable_origin origin, int export);
static void process_rule(struct reader *reader, char *text, const char *colon);
static void process_target_variable(struct reader *reader, char **targets, size_t target_count, char *text);
static void process_vpath(struct reader *reader, char *rest);
static void process_expanded(struct reader *reader, char *line);
static char **split_words(const char *text, size_t *count);
static void free_words(char **words, size_t count);
static void add_to_makefile_list(const char *path);

/*
 * Reads one makefile ("-" is standard input).  Returns 1, or 0 when it
 * does not exist and must_exist is 0; any other failure ends make.
 */
int
read_makefile(
	const char *path,
	int must_exist)
{
	struct reader reader;
	int compare;
	int error;

	/* The stream: standard input for -, or the file. */
	memset(&reader, 0, sizeof(reader));
	reader.file = path;
	reader.next_line = 1;
	compare = strcmp(path, "-");
	if (compare == 0) {
		reader.stream = stdin;
	} else {
		reader.stream = fopen(path, "r");
	}

	/* A makefile that is not there. */
	if (reader.stream == NULL) {
		error = errno;
		if (!must_exist && error == ENOENT)
			return 0;
		make_fatal("%s: %s", path, strerror(error));
	}

	/* $(MAKEFILE_LIST) names every makefile read, in order. */
	add_to_makefile_list(path);
	makefiles_read++;

	/* Every line. */
	read_all_lines(&reader);
	if (reader.stream != stdin)
		fclose(reader.stream);

	/* Succeeded. */
	return 1;
}

/*
 * Reads the built-in rules and variables from a text.  Their variables
 * have the default origin, so that the environment and the makefiles
 * replace them, and their rules never give the default goal.
 */
void
read_builtin(
	const char *text)
{
	struct reader reader;

	/* A reader of the text. */
	memset(&reader, 0, sizeof(reader));
	reader.file = "<builtin>";
	reader.text = text;
	reader.builtin = 1;
	reader.next_line = 1;

	/* Every line, with the rules marked as built in. */
	rule_set_reading_builtin(1);
	read_all_lines(&reader);
	rule_set_reading_builtin(0);
}

/*
 * Reads the text of $(eval) as makefile lines, as if at a line of a
 * makefile.
 */
void
read_eval(
	const char *text,
	const char *file,
	long line)
{
	struct reader reader;

	/* A reader of the text, numbered from the line of the eval. */
	memset(&reader, 0, sizeof(reader));
	reader.file = file;
	if (reader.file == NULL)
		reader.file = "<eval>";
	reader.text = text;
	reader.next_line = line;

	/* Every line. */
	read_all_lines(&reader);
}

/*
 * Gives the include files that were not found: their names, and whether
 * each must exist (include) or may be missing (-include).
 */
void
read_remake_list(
	char ***names,
	int **must_exist,
	size_t *count)
{
	/* The lists process_include() kept. */
	*names = missing_includes;
	*must_exist = missing_must_exist;
	*count = missing_count;
}

/*
 * Returns how many makefiles were read.
 */
size_t
read_makefile_count(
	void)
{
	/* read_makefile() counts them. */
	return makefiles_read;
}

/*
 * Adds a directory (-I) to those searched for an include file.
 */
void
read_add_include_directory(
	const char *directory)
{
	/* One more entry in the list. */
	include_directories = make_realloc(include_directories, (include_directory_count + 1U) * sizeof(*include_directories));
	include_directories[include_directory_count] = make_strdup(directory);
	include_directory_count++;
}

/* Reads and handles every logical line of a makefile, then checks that nothing is left open. */
static void
read_all_lines(
	struct reader *reader)
{
	struct buffer line;
	int more;

	/* Each logical line, until the end. */
	memset(&line, 0, sizeof(line));
	for (;;) {
		more = read_logical_line(reader, &line);
		if (!more)
			break;
		process_line(reader, line.text);
	}

	/* The line buffer is done with. */
	free(line.text);

	/* A recipe does not go on into the next file, nor does a conditional. */
	end_rule(reader);
	if (reader->depth > 0)
		make_file_fatal(reader->file, reader->next_line - 1, "missing 'endif'");
}

/* Returns the next character of the makefile, or EOF. */
static int
next_character(
	struct reader *reader)
{
	int character;

	/* A stream. */
	if (reader->text == NULL) {
		character = getc(reader->stream);
		return character;
	}

	/* The end of a text. */
	if (reader->text[reader->position] == '\0')
		return EOF;

	/* Succeeded: the next character of the text. */
	character = (unsigned char)reader->text[reader->position];
	reader->position++;
	return character;
}

/* Returns the origin of the variables a makefile defines: the default for the built-in rules. */
static enum variable_origin
file_origin(
	const struct reader *reader)
{
	/* The built-in rules only give defaults. */
	if (reader->builtin)
		return ORIGIN_DEFAULT;

	/* Succeeded: a makefile. */
	return ORIGIN_FILE;
}

/*
 * Reads the next logical line into a buffer, backslash-newlines included;
 * returns 0 at the end of the file.
 */
static int
read_logical_line(
	struct reader *reader,
	struct buffer *line)
{
	size_t backslashes;
	size_t index;
	int character;
	int any;

	/* The buffer starts empty, and the line where the reader is. */
	line->length = 0;
	if (line->text != NULL)
		line->text[0] = '\0';
	reader->line = reader->next_line;
	any = 0;

	/* Physical lines until one does not end with an odd number of backslashes. */
	for (;;) {
		character = next_character(reader);
		if (character == EOF)
			break;
		any = 1;
		if (character != '\n') {
			buffer_add_char(line, (char)character);
			continue;
		}

		/* A newline: the line goes on when a backslash escapes it. */
		reader->next_line++;
		backslashes = 0;
		for (index = line->length; index > 0 && line->text[index - 1U] == '\\'; index--)
			backslashes++;
		if (backslashes % 2U == 0)
			break;
		buffer_add_char(line, '\n');
	}

	/* The end of the file with nothing read. */
	if (!any)
		return 0;

	/* Succeeded: a line, possibly empty. */
	if (line->text == NULL)
		buffer_add(line, "", 0);
	return 1;
}

/*
 * Handles one logical line: a recipe line, a conditional (always, to keep
 * the nesting), or, in a branch that is used, anything else.
 */
static void
process_line(
	struct reader *reader,
	char *line)
{
	const char *separator;
	enum assign_kind kind;
	size_t operator_length;
	char *text;
	char *rest;
	int directive;
	int found;
	int used;
	int is_word;

	/* A tab after a rule starts a recipe line, kept as it is. */
	used = is_used(reader);
	if (line[0] == '\t' && reader->recipe != NULL) {
		if (used)
			add_recipe_line(reader, line + 1);
		return;
	}

	/* Any other line: its backslash-newlines become spaces, its comment goes. */
	join_continuations(line);
	strip_comment(line);
	text = skip_blanks(line);
	trim_trailing(text);
	if (text[0] == '\0')
		return;

	/* The conditionals are followed even in a branch that is not used. */
	directive = conditional_directive(reader, text);
	if (directive)
		return;

	/* In a branch that is not used, a define's body is skipped whole. */
	if (!used) {
		is_word = word_is(text, "define", &rest);
		if (is_word)
			skip_define(reader);
		return;
	}

	/* Anything else ends the rule before it. */
	end_rule(reader);

	/* include makes a missing file an error; -include and sinclude do not. */
	is_word = word_is(text, "include", &rest);
	if (is_word) {
		process_include(reader, rest, 1);
		return;
	}

	/* -include and sinclude. */
	is_word = word_is(text, "-include", &rest);
	if (!is_word)
		is_word = word_is(text, "sinclude", &rest);
	if (is_word) {
		process_include(reader, rest, 0);
		return;
	}

	/* define reads a variable's lines. */
	is_word = word_is(text, "define", &rest);
	if (is_word) {
		read_define(reader, rest, file_origin(reader), 0);
		return;
	}

	/* override, before define or an assignment. */
	is_word = word_is(text, "override", &rest);
	if (is_word) {
		/* override define, or override and an assignment. */
		is_word = word_is(rest, "define", &text);
		if (is_word) {
			read_define(reader, text, ORIGIN_OVERRIDE, 0);
			return;
		}

		/* override and an assignment. */
		found = find_separator(rest, &separator, &kind, &operator_length);
		if (found != 1)
			make_file_fatal(reader->file, reader->line, "missing separator");
		process_assignment(reader, rest, separator, kind, operator_length, ORIGIN_OVERRIDE, 0);
		return;
	}

	/* export. */
	is_word = word_is(text, "export", &rest);
	if (is_word) {
		process_export(reader, rest, 1);
		return;
	}

	/* unexport. */
	is_word = word_is(text, "unexport", &rest);
	if (is_word) {
		process_export(reader, rest, -1);
		return;
	}

	/* vpath. */
	is_word = word_is(text, "vpath", &rest);
	if (is_word) {
		process_vpath(reader, rest);
		return;
	}

	/* An assignment or a rule, by the first : or = outside $(...). */
	found = find_separator(text, &separator, &kind, &operator_length);
	if (found == 1) {
		process_assignment(reader, text, separator, kind, operator_length, file_origin(reader), 0);
		return;
	}

	/* A rule. */
	if (found == 2) {
		process_rule(reader, text, separator);
		return;
	}

	/* A line of only references (a function such as $(eval ...)) is read after it expands. */
	process_expanded(reader, text);
}

/*
 * Handles a line with no : or = outside references: it is expanded, and
 * what it becomes is read as a line; nothing (as from $(eval ...) or
 * $(info ...)) is fine.
 */
static void
process_expanded(
	struct reader *reader,
	char *line)
{
	struct expansion context;
	const char *separator;
	enum assign_kind kind;
	size_t operator_length;
	char *expanded;
	char *text;
	int found;

	/* The expansion, without its blanks around it. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	expanded = expand(&context, line);
	text = skip_blanks(expanded);
	trim_trailing(text);
	if (text[0] == '\0') {
		free(expanded);
		return;
	}

	/* What it became must be an assignment or a rule. */
	found = find_separator(text, &separator, &kind, &operator_length);
	if (found == 1) {
		process_assignment(reader, text, separator, kind, operator_length, file_origin(reader), 0);
		free(expanded);
		return;
	}

	/* A rule. */
	if (found == 2) {
		process_rule(reader, text, separator);
		free(expanded);
		return;
	}

	/* Anything else is not make. */
	make_file_fatal(reader->file, reader->line, "missing separator");
}

/*
 * Ends the rule being read: its recipe goes to its targets.
 */
static void
end_rule(
	struct reader *reader)
{
	/* No rule is open. */
	if (reader->rule_targets == NULL)
		return;

	/* The recipe, empty or not, and the reader is outside any rule. */
	rule_set_recipe(reader->rule_targets, reader->rule_count, reader->recipe);
	free(reader->rule_targets);
	reader->rule_targets = NULL;
	reader->rule_count = 0;
	reader->recipe = NULL;
}

/*
 * Adds a recipe line (after its tab) to the rule being read.  Each
 * backslash-newline stays, and the tab that starts the line after it
 * goes, as POSIX says.
 */
static void
add_recipe_line(
	struct reader *reader,
	const char *line)
{
	struct buffer text;
	const char *cursor;

	/* The characters, less a tab after each newline. */
	memset(&text, 0, sizeof(text));
	for (cursor = line; *cursor != '\0'; cursor++) {
		buffer_add_char(&text, *cursor);
		if (cursor[0] == '\n' && cursor[1] == '\t')
			cursor++;
	}

	/* The line goes to the recipe. */
	recipe_add_line(reader->recipe, buffer_text(&text), text.length, reader->line);
	free(text.text);
}

/*
 * Turns each backslash-newline of a line that is not a recipe line into
 * one space, together with the blanks around it.
 */
static void
join_continuations(
	char *line)
{
	char *read;
	char *write;

	/* Copies the line onto itself, closing up at each backslash-newline. */
	read = line;
	write = line;
	while (*read != '\0') {
		if (read[0] != '\\' || read[1] != '\n') {
			*write = *read;
			write++;
			read++;
			continue;
		}

		/* The blanks before, the backslash-newline and the blanks after become one space. */
		while (write > line && (write[-1] == ' ' || write[-1] == '\t'))
			write--;
		read += 2;
		while (*read == ' ' || *read == '\t' || (read[0] == '\\' && read[1] == '\n')) {
			if (read[0] == '\\')
				read++;
			read++;
		}

		/* One space for all of it. */
		*write = ' ';
		write++;
	}

	/* The copy ends. */
	*write = '\0';
}

/*
 * Cuts a line at its comment: the first # that no backslash escapes.
 * \# is a #.
 */
static void
strip_comment(
	char *line)
{
	char *read;
	char *write;

	/* Copies the line onto itself up to the comment. */
	read = line;
	write = line;
	while (*read != '\0') {
		if (read[0] == '\\' && read[1] == '#') {
			*write = '#';
			write++;
			read += 2;
			continue;
		}

		/* The comment starts at an unescaped #. */
		if (*read == '#')
			break;
		*write = *read;
		write++;
		read++;
	}

	/* The copy ends. */
	*write = '\0';
}

/* Returns the text after its leading blanks. */
static char *
skip_blanks(
	char *text)
{
	/* Past spaces and tabs. */
	while (*text == ' ' || *text == '\t')
		text++;

	/* Succeeded. */
	return text;
}

/* Removes the blanks at the end of a text. */
static void
trim_trailing(
	char *text)
{
	size_t length;

	/* Back over spaces and tabs. */
	length = strlen(text);
	while (length > 0 && (text[length - 1U] == ' ' || text[length - 1U] == '\t'))
		length--;
	text[length] = '\0';
}

/*
 * Reports whether a text starts with a directive word followed by a blank
 * (or ends there); *rest, when given, gets the text after the blanks.
 */
static int
word_is(
	const char *text,
	const char *word,
	char **rest)
{
	size_t length;
	int compare;

	/* The word, then a blank or the end. */
	length = strlen(word);
	compare = strncmp(text, word, length);
	if (compare != 0)
		return 0;
	if (text[length] != '\0' && text[length] != ' ' && text[length] != '\t')
		return 0;

	/* Succeeded: what follows. */
	if (rest != NULL)
		*rest = skip_blanks((char *)text + length);
	return 1;
}

/* Reports whether the lines where the reader is are used (every conditional is in a taken branch). */
static int
is_used(
	const struct reader *reader)
{
	const struct conditional *innermost;

	/* Outside any conditional, every line is used. */
	if (reader->depth == 0)
		return 1;

	/* Succeeded: the innermost conditional knows. */
	innermost = &reader->conditionals[reader->depth - 1];
	if (innermost->taken && innermost->outer_taken)
		return 1;
	return 0;
}

/*
 * Handles ifeq, ifneq, ifdef, ifndef, else and endif; returns 0 when the
 * line is not one of them.
 */
static int
conditional_directive(
	struct reader *reader,
	char *text)
{
	struct conditional *innermost;
	char *rest;
	size_t length;
	int keyword;
	int is_word;
	int outer;
	int result;

	/* endif closes the innermost conditional. */
	is_word = word_is(text, "endif", &rest);
	if (is_word) {
		if (reader->depth == 0)
			make_file_fatal(reader->file, reader->line, "extraneous 'endif'");
		reader->depth--;
		return 1;
	}

	/* else starts the next branch, which may be another condition. */
	is_word = word_is(text, "else", &rest);
	if (is_word) {
		if (reader->depth == 0)
			make_file_fatal(reader->file, reader->line, "extraneous 'else'");
		innermost = &reader->conditionals[reader->depth - 1];
		if (innermost->seen_else)
			make_file_fatal(reader->file, reader->line, "only one 'else' per conditional");
		if (innermost->taken)
			innermost->any_taken = 1;
		if (rest[0] == '\0') {
			innermost->seen_else = 1;
			innermost->taken = !innermost->any_taken;
			return 1;
		}

		/* else ifeq ...: taken when nothing was and its condition holds. */
		keyword = conditional_keyword(rest, &length);
		if (keyword < 0)
			make_file_fatal(reader->file, reader->line, "extraneous text after 'else' directive");
		result = 0;
		if (!innermost->any_taken && innermost->outer_taken)
			result = evaluate_condition(reader, keyword, rest + length);
		innermost->taken = result;
		return 1;
	}

	/* ifeq, ifneq, ifdef and ifndef open a conditional. */
	keyword = conditional_keyword(text, &length);
	if (keyword < 0)
		return 0;

	/* A new conditional, whose condition matters only where lines are used. */
	if (reader->depth == READ_CONDITIONAL_MAX)
		make_file_fatal(reader->file, reader->line, "conditionals nested too deeply");
	outer = is_used(reader);
	result = 0;
	if (outer)
		result = evaluate_condition(reader, keyword, text + length);
	innermost = &reader->conditionals[reader->depth];
	innermost->taken = result;
	innermost->any_taken = 0;
	innermost->seen_else = 0;
	innermost->outer_taken = outer;
	reader->depth++;

	/* Succeeded: the line was a conditional. */
	return 1;
}

/*
 * Returns which conditional keyword a text starts with (CONDITION_IFEQ
 * and the others), with its length, or -1 when it starts with none.  The
 * keyword must be followed by a blank, a parenthesis or a quote.
 */
static int
conditional_keyword(
	const char *text,
	size_t *length)
{
	static const char *const keywords[] = {"ifeq", "ifneq", "ifdef", "ifndef"};
	size_t index;
	size_t keyword_length;
	char after;
	int compare;
	int blank;

	/* Each keyword in turn. */
	for (index = 0; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
		keyword_length = strlen(keywords[index]);
		compare = strncmp(text, keywords[index], keyword_length);
		if (compare != 0)
			continue;

		/* What follows the keyword must end it. */
		after = text[keyword_length];
		blank = make_is_blank(after);
		if (!blank && after != '(' && after != '"' && after != '\'')
			continue;
		*length = keyword_length;
		return (int)index;
	}

	/* None of them. */
	return -1;
}

/* Evaluates the condition of ifeq, ifneq, ifdef or ifndef; returns 1 when it holds. */
static int
evaluate_condition(
	struct reader *reader,
	int keyword,
	char *arguments)
{
	struct expansion context;
	struct variable *variable;
	char *first;
	char *second;
	char *name;
	int split;
	int equal;
	int compare;
	int defined;

	/* The arguments expand in the global scope. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;

	/* ifdef and ifndef: whether the variable has a non-empty value, unexpanded. */
	arguments = skip_blanks(arguments);
	if (keyword == CONDITION_IFDEF || keyword == CONDITION_IFNDEF) {
		name = expand(&context, arguments);
		trim_trailing(name);
		variable = variable_lookup(&make_global_scope, name, strlen(name));
		free(name);
		defined = 0;
		if (variable != NULL && variable->value[0] != '\0')
			defined = 1;
		if (keyword == CONDITION_IFDEF)
			return defined;
		return !defined;
	}

	/* ifeq and ifneq: the two texts, expanded and compared. */
	first = NULL;
	second = NULL;
	split = split_comparison(arguments, &first, &second);
	if (!split)
		make_file_fatal(reader->file, reader->line, "invalid syntax in conditional");
	first = expand(&context, first);
	second = expand(&context, second);
	compare = strcmp(first, second);
	free(first);
	free(second);
	equal = 0;
	if (compare == 0)
		equal = 1;

	/* Succeeded: ifeq holds when they are equal, ifneq when they differ. */
	if (keyword == CONDITION_IFEQ)
		return equal;
	return !equal;
}

/*
 * Splits the arguments of ifeq or ifneq, (a,b) or "a" "b" (either quote),
 * in place; returns 0 when they have neither form.
 */
static int
split_comparison(
	char *arguments,
	char **first,
	char **second)
{
	char *cursor;
	char *comma;
	char quote;
	int depth;

	/* (a,b): the comma outside any parentheses, and the last parenthesis. */
	if (arguments[0] == '(') {
		depth = 0;
		comma = NULL;
		for (cursor = arguments + 1; *cursor != '\0'; cursor++) {
			if (*cursor == '(') {
				depth++;
			} else if (*cursor == ')') {
				if (depth == 0)
					break;
				depth--;
			} else if (*cursor == ',' && depth == 0 && comma == NULL) {
				comma = cursor;
			}
		}

		/* Both a comma and the closing parenthesis are needed. */
		if (comma == NULL || *cursor != ')')
			return 0;
		*comma = '\0';
		*cursor = '\0';
		*first = skip_blanks(arguments + 1);
		trim_trailing(*first);
		*second = skip_blanks(comma + 1);
		trim_trailing(*second);
		return 1;
	}

	/* "a" "b": each text inside its own quotes. */
	quote = arguments[0];
	if (quote != '"' && quote != '\'')
		return 0;
	cursor = strchr(arguments + 1, quote);
	if (cursor == NULL)
		return 0;
	*cursor = '\0';
	*first = arguments + 1;
	cursor = skip_blanks(cursor + 1);
	quote = cursor[0];
	if (quote != '"' && quote != '\'')
		return 0;
	*second = cursor + 1;
	cursor = strchr(cursor + 1, quote);
	if (cursor == NULL)
		return 0;
	*cursor = '\0';

	/* Succeeded. */
	return 1;
}

/*
 * Reads define NAME [op] ... endef: the lines between are the value,
 * joined by newlines, assigned with the operator (= when there is none).
 */
static void
read_define(
	struct reader *reader,
	char *rest,
	enum variable_origin origin,
	int export)
{
	struct expansion context;
	struct buffer line;
	struct buffer value;
	struct variable *variable;
	enum assign_kind kind;
	const char *separator;
	char *name;
	char *text;
	char *expanded;
	size_t operator_length;
	int depth;
	int found;
	int more;
	int is_word;

	/* The name, and the operator after it if there is one. */
	kind = ASSIGN_RECURSIVE;
	found = find_separator(rest, &separator, &kind, &operator_length);
	if (found == 1) {
		name = make_strndup(rest, (size_t)(separator - rest));
	} else {
		name = make_strdup(rest);
		kind = ASSIGN_RECURSIVE;
	}

	/* The name without its trailing blanks. */
	trim_trailing(name);

	/* The body, up to the endef that matches (a define inside nests). */
	memset(&line, 0, sizeof(line));
	memset(&value, 0, sizeof(value));
	depth = 0;
	for (;;) {
		more = read_logical_line(reader, &line);
		if (!more)
			make_file_fatal(reader->file, reader->line, "missing 'endef', unterminated 'define'");
		text = skip_blanks(line.text);
		is_word = word_is(text, "endef", NULL);
		if (is_word && depth == 0)
			break;
		if (is_word)
			depth--;
		is_word = word_is(text, "define", NULL);
		if (is_word)
			depth++;
		if (value.text != NULL)
			buffer_add_char(&value, '\n');
		buffer_add_string(&value, line.text);
	}

	/* The line buffer is done with. */
	free(line.text);

	/* The name expands; the value is assigned as the operator says. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	expanded = expand(&context, name);
	trim_trailing(expanded);
	variable_assign(&make_global_variables, expanded, buffer_text(&value), kind, origin, &context);
	if (export) {
		variable = variable_lookup(&make_global_scope, expanded, strlen(expanded));
		if (variable != NULL)
			variable->export_state = 1;
	}

	/* The texts are done with. */
	free(expanded);
	free(name);
	free(value.text);
}

/* Skips a define's body in a branch that is not used. */
static void
skip_define(
	struct reader *reader)
{
	struct buffer line;
	char *text;
	int depth;
	int more;
	int is_word;

	/* Lines up to the endef that matches. */
	memset(&line, 0, sizeof(line));
	depth = 0;
	for (;;) {
		more = read_logical_line(reader, &line);
		if (!more)
			make_file_fatal(reader->file, reader->line, "missing 'endef', unterminated 'define'");
		text = skip_blanks(line.text);
		is_word = word_is(text, "endef", NULL);
		if (is_word && depth == 0)
			break;
		if (is_word)
			depth--;
		is_word = word_is(text, "define", NULL);
		if (is_word)
			depth++;
	}

	/* The line buffer is done with. */
	free(line.text);
}

/*
 * Reads each file an include directive names, looking in the -I
 * directories for a relative name that is not found as it is.
 */
static void
process_include(
	struct reader *reader,
	char *rest,
	int must_exist)
{
	struct expansion context;
	struct buffer path;
	char **words;
	size_t count;
	size_t index;
	size_t directory;
	int found;

	/* The names, expanded. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	rest = expand(&context, rest);
	words = split_words(rest, &count);
	free(rest);

	/* Each file, as named, then in each -I directory. */
	for (index = 0; index < count; index++) {
		found = read_makefile(words[index], 0);
		for (directory = 0; !found && words[index][0] != '/' && directory < include_directory_count; directory++) {
			memset(&path, 0, sizeof(path));
			buffer_add_string(&path, include_directories[directory]);
			buffer_add_char(&path, '/');
			buffer_add_string(&path, words[index]);
			found = read_makefile(path.text, 0);
			free(path.text);
		}

		/* A file that is nowhere, for include. */
		if (found)
			continue;

		/* A file that is nowhere may be made by a rule: main() tries after reading. */
		missing_includes = make_realloc(missing_includes, (missing_count + 1U) * sizeof(*missing_includes));
		missing_must_exist = make_realloc(missing_must_exist, (missing_count + 1U) * sizeof(*missing_must_exist));
		missing_includes[missing_count] = make_strdup(words[index]);
		missing_must_exist[missing_count] = must_exist;
		missing_count++;
	}

	/* The names are done with. */
	free_words(words, count);
}

/*
 * Handles export and unexport: alone (export only), every variable;
 * with names, those variables; with an assignment, that assignment and
 * its variable.
 */
static void
process_export(
	struct reader *reader,
	char *rest,
	int export)
{
	struct expansion context;
	struct variable *variable;
	const char *separator;
	enum assign_kind kind;
	size_t operator_length;
	char **words;
	char *names;
	size_t count;
	size_t index;
	int found;
	int is_word;

	/* export alone exports everything. */
	if (rest[0] == '\0') {
		if (export > 0)
			variable_export_all();
		return;
	}

	/* export define NAME ... endef. */
	is_word = word_is(rest, "define", &names);
	if (is_word && export > 0) {
		read_define(reader, names, file_origin(reader), 1);
		return;
	}

	/* export NAME = value: the assignment, then the export. */
	found = find_separator(rest, &separator, &kind, &operator_length);
	if (found == 1 && export > 0) {
		process_assignment(reader, rest, separator, kind, operator_length, file_origin(reader), 1);
		return;
	}

	/* The names, expanded; a name not yet defined is defined empty to hold the mark. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	names = expand(&context, rest);
	words = split_words(names, &count);
	free(names);
	for (index = 0; index < count; index++) {
		variable = variable_lookup(&make_global_scope, words[index], strlen(words[index]));
		if (variable == NULL)
			variable = variable_set_value(&make_global_variables, words[index], "", FLAVOR_RECURSIVE, ORIGIN_DEFAULT);
		variable->export_state = export;
	}

	/* The names are done with. */
	free_words(words, count);
}

/*
 * Finds the first : or = outside $(...) and ${...} in a line.  Returns 1
 * for an assignment (with its kind and the length of its operator, from
 * *separator), 2 for a rule (*separator at the colon), and 0 for neither.
 */
static int
find_separator(
	const char *text,
	const char **separator,
	enum assign_kind *kind,
	size_t *operator_length)
{
	const char *cursor;
	int depth;

	/* Brackets hide what is inside them. */
	depth = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor == '(' || *cursor == '{') {
			depth++;
			continue;
		}

		/* A closing bracket. */
		if (*cursor == ')' || *cursor == '}') {
			if (depth > 0)
				depth--;
			continue;
		}

		/* Inside brackets nothing separates. */
		if (depth > 0)
			continue;

		/* =, and the operators that end in it. */
		if (*cursor == '=') {
			*kind = ASSIGN_RECURSIVE;
			*separator = cursor;
			*operator_length = 1;
			if (cursor > text && cursor[-1] == '+') {
				*kind = ASSIGN_APPEND;
				*separator = cursor - 1;
				*operator_length = 2;
			} else if (cursor > text && cursor[-1] == '?') {
				*kind = ASSIGN_CONDITIONAL;
				*separator = cursor - 1;
				*operator_length = 2;
			} else if (cursor > text && cursor[-1] == '!') {
				*kind = ASSIGN_SHELL;
				*separator = cursor - 1;
				*operator_length = 2;
			}

			/* Succeeded: an assignment. */
			return 1;
		}

		/* := and ::= are assignments; any other colon makes a rule. */
		if (*cursor == ':') {
			if (cursor[1] == '=') {
				*kind = ASSIGN_SIMPLE;
				*separator = cursor;
				*operator_length = 2;
				return 1;
			}

			/* ::=, as POSIX spells it. */
			if (cursor[1] == ':' && cursor[2] == '=') {
				*kind = ASSIGN_SIMPLE;
				*separator = cursor;
				*operator_length = 3;
				return 1;
			}

			/* Any other colon: a rule. */
			*separator = cursor;
			return 2;
		}
	}

	/* Neither. */
	return 0;
}

/*
 * Carries out an assignment line: the name before the operator
 * (expanded), the value after it with its leading blanks gone.
 */
static void
process_assignment(
	struct reader *reader,
	char *text,
	const char *separator,
	enum assign_kind kind,
	size_t operator_length,
	enum variable_origin origin,
	int export)
{
	struct expansion context;
	struct variable *variable;
	char *name;
	char *expanded;
	char *name_start;
	const char *value;
	size_t length;

	/* The name, expanded, without the blanks around it. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	name = make_strndup(text, (size_t)(separator - text));
	trim_trailing(name);
	expanded = expand(&context, skip_blanks(name));
	trim_trailing(expanded);
	if (expanded[0] == '\0')
		make_file_fatal(reader->file, reader->line, "empty variable name");

	/* The value, and the assignment. */
	value = skip_blanks((char *)separator + operator_length);
	name_start = skip_blanks(expanded);
	variable_assign(&make_global_variables, name_start, value, kind, origin, &context);
	if (export) {
		length = strlen(name_start);
		variable = variable_lookup(&make_global_scope, name_start, length);
		if (variable != NULL)
			variable->export_state = 1;
	}

	/* The texts are done with. */
	free(expanded);
	free(name);
}

/*
 * Reads a rule line: targets, :, or :: for a double-colon rule, the
 * prerequisites (those after | are order-only), and a recipe after ;.
 * The recipe lines that follow are added until the rule ends.
 */
static void
process_rule(
	struct reader *reader,
	char *text,
	const char *colon)
{
	struct expansion context;
	char **targets;
	char **prerequisites;
	char **order_only;
	char *target_text;
	char *prerequisite_text;
	char *bar;
	char *semicolon;
	char *recipe_text;
	size_t target_count;
	size_t prerequisite_count;
	size_t order_only_count;
	size_t operator_length;
	const char *separator;
	enum assign_kind kind;
	int double_colon;
	int found;

	/* The targets, expanded. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	target_text = make_strndup(text, (size_t)(colon - text));
	recipe_text = expand(&context, target_text);
	free(target_text);
	targets = split_words(recipe_text, &target_count);
	free(recipe_text);
	if (target_count == 0)
		make_file_fatal(reader->file, reader->line, "missing target");

	/* :: makes a double-colon rule. */
	double_colon = 0;
	if (colon[1] == ':') {
		double_colon = 1;
		colon++;
	}


	/* A recipe after ; on the same line. */
	prerequisite_text = make_strdup(colon + 1);
	semicolon = strchr(prerequisite_text, ';');
	recipe_text = NULL;
	if (semicolon != NULL) {
		*semicolon = '\0';
		recipe_text = skip_blanks(semicolon + 1);
	}

	/* targets: NAME = value is an assignment of those targets' own; its value may hold a ;. */
	found = find_separator(prerequisite_text, &separator, &kind, &operator_length);
	if (found == 1) {
		process_target_variable(reader, targets, target_count, (char *)colon + 1);
		free(prerequisite_text);
		free_words(targets, target_count);
		return;
	}

	/* The prerequisites, expanded; those after | are order-only. */
	bar = strchr(prerequisite_text, '|');
	order_only = NULL;
	order_only_count = 0;
	if (bar != NULL) {
		*bar = '\0';
		target_text = expand(&context, bar + 1);
		order_only = split_words(target_text, &order_only_count);
		free(target_text);
	}

	/* The ordinary prerequisites, expanded. */
	target_text = expand(&context, prerequisite_text);
	prerequisites = split_words(target_text, &prerequisite_count);
	free(target_text);

	/* The rule, and its recipe to come. */
	reader->rule_targets = rule_define(targets, target_count, prerequisites, prerequisite_count, order_only, order_only_count, double_colon);
	reader->rule_count = target_count;
	reader->recipe = recipe_new(reader->file, reader->line);
	if (recipe_text != NULL) {
		reader->recipe->semicolon = 1;
		if (recipe_text[0] != '\0')
			recipe_add_line(reader->recipe, recipe_text, strlen(recipe_text), reader->line);
	}

	/* The texts are done with. */
	free(prerequisite_text);
	free_words(targets, target_count);
	free_words(prerequisites, prerequisite_count);
	free_words(order_only, order_only_count);
}

/*
 * Assigns a variable of some targets' own (targets: NAME op value, with
 * export or override allowed before the name).
 */
static void
process_target_variable(
	struct reader *reader,
	char **targets,
	size_t target_count,
	char *text)
{
	struct expansion context;
	const char *separator;
	enum assign_kind kind;
	enum variable_origin origin;
	size_t operator_length;
	char *name;
	char *expanded;
	char *rest;
	int found;
	int is_word;

	/* export and override before the name. */
	text = skip_blanks(text);
	origin = file_origin(reader);
	is_word = word_is(text, "export", &rest);
	if (is_word)
		text = rest;
	is_word = word_is(text, "override", &rest);
	if (is_word) {
		text = rest;
		origin = ORIGIN_OVERRIDE;
	}

	/* The name, expanded, and the value after the operator. */
	found = find_separator(text, &separator, &kind, &operator_length);
	if (found != 1)
		make_file_fatal(reader->file, reader->line, "missing separator");
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	name = make_strndup(text, (size_t)(separator - text));
	trim_trailing(name);
	expanded = expand(&context, name);
	trim_trailing(expanded);

	/* Each target gets it. */
	rule_define_variable(targets, target_count, skip_blanks(expanded), skip_blanks((char *)separator + operator_length), kind, origin, &context);
	free(expanded);
	free(name);
}

/*
 * Reads vpath: alone it forgets every search path; with a pattern, it
 * forgets that pattern's; with directories (separated by : or blanks), it
 * adds them for the pattern.
 */
static void
process_vpath(
	struct reader *reader,
	char *rest)
{
	struct expansion context;
	char *expanded;
	char *cursor;
	char **words;
	size_t count;

	/* The words, expanded, with : as a separator too. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = reader->file;
	context.line = reader->line;
	expanded = expand(&context, rest);
	for (cursor = expanded; *cursor != '\0'; cursor++) {
		if (*cursor == ':')
			*cursor = ' ';
	}

	/* The words. */
	words = split_words(expanded, &count);
	free(expanded);

	/* The pattern and its directories, or what to forget. */
	if (count == 0) {
		rule_vpath_set(NULL, NULL, 0);
	} else {
		rule_vpath_set(words[0], words + 1, count - 1U);
	}

	/* The words are done with. */
	free_words(words, count);
}

/* Returns the words of a text as an allocated array of allocated strings. */
static char **
split_words(
	const char *text,
	size_t *count)
{
	const char *cursor;
	const char *word;
	char **words;
	size_t length;
	size_t capacity;

	/* Each word into a growing array. */
	words = NULL;
	*count = 0;
	capacity = 0;
	cursor = text;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (*count == capacity) {
			capacity = capacity * 2U + 8U;
			words = make_realloc(words, capacity * sizeof(*words));
		}

		/* The word. */
		words[*count] = make_strndup(word, length);
		(*count)++;
	}

	/* Succeeded. */
	return words;
}

/* Frees an array from split_words(). */
static void
free_words(
	char **words,
	size_t count)
{
	size_t index;

	/* Each word, then the array. */
	for (index = 0; index < count; index++)
		free(words[index]);
	free(words);
}

/* Appends a makefile's name to $(MAKEFILE_LIST). */
static void
add_to_makefile_list(
	const char *path)
{
	struct expansion context;

	/* An append in the file's origin, as GNU make does. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = NULL;
	context.line = 0;
	variable_assign(&make_global_variables, "MAKEFILE_LIST", path, ASSIGN_APPEND, ORIGIN_FILE, &context);
}
