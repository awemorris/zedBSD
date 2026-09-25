/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shell's input: a stack of sources the parser reads a character at a
 * time.
 *
 * The base of the stack is where commands come from (a string for -c, eval and
 * a trap action; a file for a script or a dot command; the terminal).  Alias
 * text is pushed on top of it when an alias is expanded, and read before the
 * rest of the line.  More text is read only when the parser asks for it, so a
 * command is run before the lines after it are read.
 *
 * While the parser reads a command substitution it captures what it reads:
 * that text is the word's, to be parsed again when the substitution runs.
 */

#include "userland/base/sh/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How much a file source reads at once. */
#define INPUT_BLOCK 4096U

/* How many characters may be given back at once. */
#define INPUT_PUSHBACK 8

/* How deeply captures nest ($( inside $( ...). */
#define INPUT_CAPTURE_MAX 32

/* The kinds of source. */
#define SOURCE_STRING		0	/* all its text is there from the start */
#define SOURCE_FILE		1	/* read as the parser asks */
#define SOURCE_TERMINAL		2	/* read a line at a time, with a prompt */
#define SOURCE_ALIAS		3	/* an alias's text, over what it came from */
#define SOURCE_PUSHBACK		4	/* text read too far, to be read again */

/*
 * One source of input.
 *
 * buffer holds what has been read and not yet consumed from position on.  A
 * file is read into it a block at a time (a byte at a time for standard
 * input, which the commands the shell runs read too); the terminal a line at
 * a time.  The source owns the buffer, and closes a file's descriptor when
 * it is popped.
 */
struct source {
	struct source *previous;
	int kind;
	char *buffer;
	size_t length;
	size_t position;
	int descriptor;
	int at_end;
	int line;
	char *alias_name;
	int end_reported;
};

/* A buffer that grows as characters are captured into it. */
struct capture {
	char *text;
	size_t length;
	size_t capacity;
};

/* The innermost source: the one read next. */
static struct source *input_top;

/* How many sources are stacked: the depth handlers unwind to. */
static int input_depth;

/*
 * Characters given back by the parser, read again before the source; the
 * newest is read first.  Pushing a source moves them under it.
 */
static int input_pushback[INPUT_PUSHBACK];
static int input_pushback_count;

/* The captures in progress, outermost first. */
static struct capture input_captures[INPUT_CAPTURE_MAX];
static int input_capture_count;

/* Which prompt the terminal shows next: 1 for PS1, 2 for PS2. */
static int input_prompt = 1;

/*
 * Set when an alias whose text ends in a blank has just been read out: the
 * next word is checked for an alias too.  The parser clears it.
 */
int sh_input_alias_blank;

static struct source *source_new(int kind, const char *text, size_t length);
static void source_push(struct source *source);
static void pushback_to_source(void);
static int alias_ends_in_blank(const struct source *source);
static int source_fill(struct source *source);
static int fill_file(struct source *source);
static int fill_line(struct source *source);
static int fill_terminal(struct source *source);
static void capture_add(int value);

/*
 * Pushes a string to be read, starting at a line number.  The text is
 * copied.
 */
void
sh_input_push_string(
	const char *text,
	size_t length,
	int line)
{
	struct source *source;

	/* A string source holds all of its text from the start. */
	source = source_new(SOURCE_STRING, text, length);
	if (line > 0)
		source->line = line;
	source_push(source);
}

/*
 * Pushes a file to be read.  The source owns the descriptor, and closes it
 * when it is popped (except standard input).  interactive makes it the
 * terminal, read a line at a time through the line editor.
 */
void
sh_input_push_file(
	int descriptor,
	int interactive)
{
	struct source *source;

	/* A file source starts empty and reads as the parser asks. */
	if (interactive)
		source = source_new(SOURCE_TERMINAL, NULL, 0);
	else
		source = source_new(SOURCE_FILE, NULL, 0);
	source->descriptor = descriptor;
	source_push(source);
}

/*
 * Pushes the text of an alias, read before the rest of the input.
 */
void
sh_input_push_alias(
	const char *text,
	void *name)
{
	struct source *source;

	/* The alias's text is copied, since the alias may be redefined. */
	source = source_new(SOURCE_ALIAS, text, strlen(text));
	source->alias_name = sh_strdup(name);
	source_push(source);
}

/*
 * Pushes text the parser read too far and must read again, which gives way
 * to the source under it when it is used up.
 */
void
sh_input_push_back_text(
	const char *text,
	size_t length)
{
	struct source *source;

	/* The text is copied and read before anything else. */
	source = source_new(SOURCE_PUSHBACK, text, length);
	source_push(source);
}

/*
 * Gives back text the parser read and decided to read again: it leaves
 * every capture in progress (as sh_input_ungetc does for a character) and
 * is read before anything else.
 */
void
sh_input_give_back_text(
	const char *text,
	size_t length)
{
	int index;

	/* The characters leave the captures they were added to. */
	for (index = 0; index < input_capture_count; index++) {
		if (input_captures[index].length >= length)
			input_captures[index].length -= length;
		else
			input_captures[index].length = 0;
	}

	/* Read again first. */
	sh_input_push_back_text(text, length);
}

/*
 * Pops the innermost source.
 */
void
sh_input_pop(
	void)
{
	struct source *source;

	/* Nothing to pop. */
	source = input_top;
	if (source == NULL)
		return;
	input_top = source->previous;
	input_depth--;

	/* Text read again carries its line count on into what is under it. */
	if ((source->kind == SOURCE_ALIAS ||
	     source->kind == SOURCE_PUSHBACK) && input_top != NULL)
		input_top->line = source->line;

	/* A file's descriptor goes with it, standard input excepted. */
	if ((source->kind == SOURCE_FILE || source->kind == SOURCE_TERMINAL) &&
	    source->descriptor > 2)
		(void)close(source->descriptor);
	free(source->buffer);
	free(source->alias_name);
	free(source);

	/* Characters given back belonged to the source that went. */
	input_pushback_count = 0;
}

/*
 * Reports how many sources are stacked.
 */
int
sh_input_depth(
	void)
{
	/* Succeeded: the depth. */
	return input_depth;
}

/*
 * Pops sources down to a depth.
 */
void
sh_input_unwind(
	int depth)
{
	/* Pops the sources above the depth. */
	while (input_depth > depth)
		sh_input_pop();
}

/*
 * Reads the next character: a byte, EOF at the end of the base source, or
 * SH_INPUT_END_OF_ALIAS once at the end of an alias whose text does not end
 * in a blank, so that its last word ends there.
 */
int
sh_input_getc(
	void)
{
	struct source *source;
	int value;
	int filled;
	int blank;

	/* A character given back comes first. */
	if (input_pushback_count > 0) {
		value = input_pushback[--input_pushback_count];
		capture_add(value);
		return value;
	}

	/* Reads from the innermost source, moving out as each runs dry. */
	for (;;) {
		source = input_top;
		if (source == NULL)
			return EOF;

		/* The next character of the source; verbose echoes the input. */
		if (source->position < source->length) {
			value = (unsigned char)source->buffer[source->position++];
			if (value == '\n')
				source->line++;
			if (sh_option[SH_OPT_VERBOSE] &&
			    source->kind != SOURCE_ALIAS &&
			    source->kind != SOURCE_PUSHBACK)
				fputc(value, stderr);
			capture_add(value);
			return value;
		}

		/*
		 * An alias ends its last word, then gives way; one that ends
		 * in a blank has the next word checked for an alias too.
		 */
		if (source->kind == SOURCE_ALIAS) {
			blank = alias_ends_in_blank(source);
			if (!source->end_reported && source->length > 0 &&
			    !blank) {
				source->end_reported = 1;
				return SH_INPUT_END_OF_ALIAS;
			}

			/* A blank at the end of the alias lets the next word be an alias too. */
			if (blank)
				sh_input_alias_blank = 1;
			sh_input_pop();
			continue;
		}

		/* Text read again gives way to what is under it. */
		if (source->kind == SOURCE_PUSHBACK) {
			sh_input_pop();
			continue;
		}

		/* A string has nothing more, nor has a file that ended. */
		if (source->kind == SOURCE_STRING || source->at_end)
			return EOF;

		/* A file or the terminal is asked for more. */
		filled = source_fill(source);
		if (!filled) {
			source->at_end = 1;
			return EOF;
		}
	}
}

/*
 * Reports whether the input at a depth has nothing left but blanks and
 * newlines: the command just parsed was the last, and may replace the shell.
 * Only a string can tell without reading further.
 */
int
sh_input_at_end(
	int depth)
{
	struct source *source;
	size_t position;
	char value;

	/* The source must be the one at the depth, and a string. */
	source = input_top;
	if (source == NULL || input_depth != depth)
		return 0;
	if (source->kind != SOURCE_STRING || input_pushback_count > 0)
		return 0;

	/* Nothing but blanks and newlines may be left. */
	for (position = source->position; position < source->length;
	     position++) {
		value = source->buffer[position];
		if (value != ' ' && value != '\t' && value != '\n')
			return 0;
	}

	/* Succeeded: the input has ended. */
	return 1;
}

/*
 * Gives a character back, to be read again next.
 */
void
sh_input_ungetc(
	int value)
{
	int index;

	/* EOF and the end of an alias are not characters; nothing to give. */
	if (value < 0)
		return;

	/* The character leaves every capture it was added to. */
	for (index = 0; index < input_capture_count; index++) {
		if (input_captures[index].length > 0)
			input_captures[index].length--;
	}

	/* The newest given back is read first. */
	if (input_pushback_count < INPUT_PUSHBACK)
		input_pushback[input_pushback_count++] = value;
}

/*
 * Reports whether an alias's text is being read, so that it is not expanded
 * again inside itself.
 */
int
sh_input_alias_active(
	const void *name)
{
	struct source *source;
	int compare;

	/* Looks for the alias among the stacked sources. */
	for (source = input_top; source != NULL; source = source->previous) {
		if (source->kind != SOURCE_ALIAS)
			continue;
		compare = strcmp(source->alias_name, name);
		if (compare == 0)
			return 1;
	}

	/* Not being read. */
	return 0;
}

/*
 * Reports the line the input is on.
 */
int
sh_input_line(
	void)
{
	/* No input is on line 0. */
	if (input_top == NULL)
		return 0;

	/* Succeeded: the innermost source's line. */
	return input_top->line;
}

/*
 * Chooses the prompt the terminal shows when it is asked for a line.
 */
void
sh_input_set_prompt(
	int which)
{
	/* 1 is PS1, at the start of a command; 2 is PS2, within one. */
	input_prompt = which;
}

/*
 * Starts capturing the characters read.
 */
void
sh_input_capture_start(
	void)
{
	struct capture *capture;

	/* A capture nested too deeply is refused. */
	if (input_capture_count == INPUT_CAPTURE_MAX)
		sh_error("command substitution nested too deeply");

	/* Starts an empty buffer. */
	capture = &input_captures[input_capture_count++];
	capture->text = NULL;
	capture->length = 0;
	capture->capacity = 0;
}

/*
 * Stops the innermost capture and returns what it read.
 */
char *
sh_input_capture_stop(
	size_t *length)
{
	struct capture *capture;
	char *text;

	/* Takes the buffer; an empty capture never allocated one. */
	capture = &input_captures[--input_capture_count];
	if (capture->text == NULL)
		text = sh_strdup("");
	else
		text = capture->text;
	text[capture->length] = '\0';
	*length = capture->length;
	capture->text = NULL;

	/* Succeeded: the text, which the caller frees. */
	return text;
}

/*
 * Abandons the innermost capture.
 */
void
sh_input_capture_drop(
	void)
{
	/* Frees the buffer. */
	input_capture_count--;
	free(input_captures[input_capture_count].text);
	input_captures[input_capture_count].text = NULL;
}

/*
 * Reports how many captures are in progress.
 */
int
sh_input_capture_depth(
	void)
{
	/* Succeeded: the depth. */
	return input_capture_count;
}

/*
 * Stops every capture for a while (text read that is not the word's), and
 * returns how many there were, for sh_input_capture_resume.
 */
int
sh_input_capture_suspend(
	void)
{
	int count;

	/* The captures stay in their array; only the count goes to 0. */
	count = input_capture_count;
	input_capture_count = 0;

	/* Succeeded: the count to resume with. */
	return count;
}

/*
 * Resumes the captures sh_input_capture_suspend stopped.
 */
void
sh_input_capture_resume(
	int count)
{
	/* The same captures go on. */
	input_capture_count = count;
}

/*
 * Abandons captures down to a depth, those of a parse an exception left.
 */
void
sh_input_capture_unwind(
	int depth)
{
	/* Drops the captures above the depth. */
	while (input_capture_count > depth)
		sh_input_capture_drop();
}

/*
 * Reports whether the input is the terminal.
 */
int
sh_input_interactive(
	void)
{
	struct source *source;

	/* Looks for the base source under any text read again. */
	source = input_top;
	while (source != NULL && (source->kind == SOURCE_ALIAS ||
				  source->kind == SOURCE_PUSHBACK))
		source = source->previous;

	/* Succeeded: whether it is the terminal. */
	if (source == NULL)
		return 0;
	return source->kind == SOURCE_TERMINAL;
}

/*
 * Throws away what is left of the line being read, after an error.
 */
void
sh_input_discard_line(
	void)
{
	/* Pops any text read again over the terminal. */
	while (input_top != NULL && (input_top->kind == SOURCE_ALIAS ||
				     input_top->kind == SOURCE_PUSHBACK))
		sh_input_pop();
	input_pushback_count = 0;

	/* The rest of the terminal's line goes. */
	if (input_top != NULL && input_top->kind == SOURCE_TERMINAL)
		input_top->position = input_top->length;
}

/* Makes a source of a kind, holding a copy of the text given (if any). */
static struct source *
source_new(
	int kind,
	const char *text,
	size_t length)
{
	struct source *source;

	/* A zeroed source on line 1, or on the line of what is under it. */
	source = sh_malloc(sizeof(*source));
	memset(source, 0, sizeof(*source));
	source->kind = kind;
	source->descriptor = -1;
	source->line = 1;
	if (input_top != NULL && (kind == SOURCE_ALIAS ||
				  kind == SOURCE_PUSHBACK))
		source->line = input_top->line;

	/* The text, copied. */
	if (text != NULL) {
		source->buffer = sh_strndup(text, length);
		source->length = length;
	}

	/* Succeeded: the source, not yet pushed. */
	return source;
}

/* Makes a source the innermost. */
static void
source_push(
	struct source *source)
{
	/* Characters given back stay before the new source's text is read. */
	if (input_pushback_count > 0)
		pushback_to_source();

	/* Links it on top. */
	source->previous = input_top;
	input_top = source;
	input_depth++;
}

/*
 * Turns the characters given back into a source of their own, under a
 * source about to be pushed: they belong to what was being read before it.
 */
static void
pushback_to_source(
	void)
{
	struct source *held;
	int index;

	/* The characters, oldest first, as the text of the source. */
	held = sh_malloc(sizeof(*held));
	memset(held, 0, sizeof(*held));
	held->kind = SOURCE_PUSHBACK;
	held->buffer = sh_malloc((size_t)input_pushback_count + 1U);
	for (index = 0; index < input_pushback_count; index++)
		held->buffer[index] = (char)input_pushback[input_pushback_count - 1 - index];
	held->length = (size_t)input_pushback_count;
	held->descriptor = -1;
	held->line = 1;
	if (input_top != NULL)
		held->line = input_top->line;
	input_pushback_count = 0;

	/* Links it on top. */
	held->previous = input_top;
	input_top = held;
	input_depth++;
}

/* Reports whether an alias's text ends in a blank. */
static int
alias_ends_in_blank(
	const struct source *source)
{
	char last;

	/* Empty text ends in nothing. */
	if (source->length == 0)
		return 0;

	/* A space or a tab. */
	last = source->buffer[source->length - 1];
	if (last == ' ' || last == '\t')
		return 1;

	/* Not a blank. */
	return 0;
}

/* Reads more of a file or the terminal into a source. */
static int
source_fill(
	struct source *source)
{
	int filled;

	/* The terminal is read a line at a time through the line editor. */
	if (source->kind == SOURCE_TERMINAL)
		filled = fill_terminal(source);
	else
		filled = fill_file(source);

	/* Succeeded: whether there was more. */
	return filled;
}

/* Reads more of a file: a block, or a line of standard input. */
static int
fill_file(
	struct source *source)
{
	ssize_t count;
	int filled;

	/*
	 * Standard input is shared with the commands the script runs, so the
	 * shell reads no further than the line it needs.
	 */
	if (source->descriptor == 0) {
		filled = fill_line(source);
		return filled;
	}

	/* Makes room for a block. */
	if (source->buffer == NULL)
		source->buffer = sh_malloc(INPUT_BLOCK);

	/* Reads a block, retrying an interrupted read. */
	do
		count = read(source->descriptor, source->buffer, INPUT_BLOCK);
	while (count < 0 && errno == EINTR);
	if (count <= 0)
		return 0;
	source->length = (size_t)count;
	source->position = 0;

	/* Succeeded: there is more to read. */
	return 1;
}

/* Reads one line of a file, a byte at a time. */
static int
fill_line(
	struct source *source)
{
	size_t capacity;
	ssize_t count;
	char value;

	/* A fresh buffer. */
	capacity = 128;
	free(source->buffer);
	source->buffer = sh_malloc(capacity);
	source->length = 0;
	source->position = 0;

	/* Reads up to and with the newline. */
	for (;;) {
		do
			count = read(source->descriptor, &value, 1);
		while (count < 0 && errno == EINTR);
		if (count <= 0)
			break;
		if (source->length + 1U >= capacity) {
			capacity *= 2U;
			source->buffer = sh_realloc(source->buffer, capacity);
		}

		/* The byte joins the line, which ends at a newline. */
		source->buffer[source->length++] = value;
		if (value == '\n')
			break;
	}

	/* Succeeded when anything was read. */
	if (source->length == 0)
		return 0;
	return 1;
}

/* Reads a line from the terminal, keeping it in the histories. */
static int
fill_terminal(
	struct source *source)
{
	const char *prompt;
	char *line;
	size_t length;
	int filled;
	int terminal;

	/* Shows the prompt that fits where the parser is. */
	fflush(stdout);
	prompt = sh_prompt_text(input_prompt);
	input_prompt = 2;

	/* Without a terminal, the prompt goes to standard error. */
	terminal = isatty(source->descriptor);
	if (!terminal) {
		fputs(prompt, stderr);
		fflush(stderr);
		filled = fill_line(source);
		if (filled)
			sh_history_add(source->buffer);
		return filled;
	}

	/* The line editor reads the line, in vi mode after set -o vi. */
	rl_editing_mode = 1;
	if (sh_option[SH_OPT_VI])
		rl_editing_mode = 0;
	line = readline(prompt);
	if (line == NULL)
		return 0;

	/* Keeps the line in the histories, unless it is empty. */
	if (line[0] != '\0') {
		add_history(line);
		sh_history_add(line);
	}

	/* The line is the source's text, with its newline. */
	length = strlen(line);
	free(source->buffer);
	source->buffer = sh_malloc(length + 2U);
	memcpy(source->buffer, line, length);
	source->buffer[length] = '\n';
	source->buffer[length + 1U] = '\0';
	source->length = length + 1U;
	source->position = 0;
	free(line);

	/* Succeeded: a line to read. */
	return 1;
}

/* Adds a character to every capture in progress. */
static void
capture_add(
	int value)
{
	struct capture *capture;
	size_t capacity;
	int index;

	/* Only characters are captured. */
	if (value < 0)
		return;

	/* Appends the character to each capture, growing it as needed. */
	for (index = 0; index < input_capture_count; index++) {
		capture = &input_captures[index];
		if (capture->length + 2U > capture->capacity) {
			if (capture->capacity == 0)
				capacity = 64U;
			else
				capacity = capture->capacity * 2U;
			capture->text = sh_realloc(capture->text, capacity);
			capture->capacity = capacity;
		}

		/* The byte is kept in the capture. */
		capture->text[capture->length++] = (char)value;
	}
}
