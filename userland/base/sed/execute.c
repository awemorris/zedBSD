/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs a compiled sed script over its input.
 *
 * Each cycle reads a line into the pattern space, runs the script from its
 * first command, and (unless -n) writes the pattern space out.  The input
 * is read one line ahead, so that $ knows the last line when it is read;
 * the line number runs on across the files.
 *
 * A line that ended without a newline (the last line of a file that lacks
 * one) is written without it, as GNU sed does; the newline is written only
 * if anything else follows on the same output.
 *
 * Text queued by a and r is written at the end of the cycle, or when n or N
 * reads the next line.
 */

#include "userland/base/sed/sed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What running the script asks of the cycle. */
#define ACTION_NEXT		0	/* go on with the next command */
#define ACTION_END		1	/* end of the script: write, next cycle */
#define ACTION_DELETE		2	/* next cycle without writing */
#define ACTION_RESTART		3	/* D: run again without reading */
#define ACTION_QUIT		4	/* write, then stop */
#define ACTION_QUIT_SILENT	5	/* stop without writing */

/* The groups a match reports: the whole match and \1 to \9. */
#define MATCH_GROUPS 10

/* How wide a line of l is, less its backslash. */
#define LIST_WIDTH 69U

/* A byte string that grows. */
struct buffer {
	char *data;
	size_t length;
	size_t capacity;
};

/* Text or a file queued by a or r. */
struct queued {
	const char *text;
	int is_file;
};

/* The state of a run. */
struct state {
	struct sed_program *program;
	int quiet;

	/* The input files, the one open, and the line read ahead. */
	char **files;
	int file_count;
	int next_file;
	int read_standard_input;
	FILE *stream;
	struct buffer ahead;
	int have_ahead;
	int ahead_newline;

	/* The pattern space (and whether its last line had a newline). */
	struct buffer space;
	int space_newline;
	struct buffer hold;
	unsigned long line;

	/* Set by a substitution, for t. */
	int replaced;

	/* The last regex used, which an empty regex stands for. */
	regex_t *last_regex;

	/* The text queued by a and r. */
	struct queued *queue;
	size_t queue_count;
	size_t queue_capacity;

	struct sed_output out;
	int status;
	int exit_status;
	int quit_requested;
};

static int run_script(struct state *state);
static int run_command(struct state *state, struct sed_command *command, size_t *index);
static int run_change(struct state *state, struct sed_command *command);
static int run_delete_first(struct state *state);
static int run_next(struct state *state);
static int run_append_next(struct state *state);
static int command_selected(struct state *state, struct sed_command *command);
static int range_selected(struct state *state, struct sed_command *command);
static int address_matches(struct state *state, const struct sed_address *address);
static int regex_match(struct state *state, regex_t *regex, const char *text, regmatch_t *matches, int flags);
static void substitute(struct state *state, struct sed_command *command);
static void add_replacement(struct buffer *out, const char *replacement, const char *text, const regmatch_t *matches);
static void translate(struct state *state, const unsigned char *map);
static void list(struct state *state);
static void list_add(struct buffer *out, const char *text, size_t length, size_t *width);
static void print_first_line(struct state *state);
static void print_line_number(struct state *state);
static int read_line(struct state *state, int append);
static int is_last(struct state *state);
static void fill_ahead(struct state *state);
static int read_ahead(struct state *state);
static void open_next(struct state *state);
static void queue_add(struct state *state, const char *text, int is_file);
static void flush_queue(struct state *state);
static void copy_file(struct state *state, const char *name);
static void output_line(struct sed_output *output, const char *data, size_t length, int newline);
static void output_pending_newline(struct sed_output *output);
static void buffer_set(struct buffer *buffer, const char *data, size_t length);
static void buffer_append(struct buffer *buffer, const char *data, size_t length);
static void buffer_add(struct buffer *buffer, char value);
static void buffer_reserve(struct buffer *buffer, size_t length);
static void close_outputs(struct state *state);

/*
 * Runs the script over the files (standard input when there are none).
 * Returns the exit status: 0, 2 when an input could not be read, or the
 * status given to q.
 */
int
sed_execute(
	struct sed_program *program,
	char **files,
	int count,
	int quiet)
{
	struct state state;
	int action;
	int restart;
	int read;

	/* The state; the pattern and hold spaces start empty. */
	memset(&state, 0, sizeof(state));
	state.program = program;
	state.quiet = quiet;
	state.files = files;
	state.file_count = count;
	state.out.stream = stdout;
	buffer_reserve(&state.space, 0);
	buffer_reserve(&state.hold, 0);

	/* Each cycle, until the input ends or q ends it. */
	restart = 0;
	for (;;) {
		/* A new line, unless D restarts the script on what is left. */
		if (!restart) {
			read = read_line(&state, 0);
			if (!read)
				break;
			state.replaced = 0;
		}

		/* The next cycle reads a line again. */
		restart = 0;

		/* The script, then the end of the cycle. */
		action = run_script(&state);
		if ((action == ACTION_END || action == ACTION_QUIT) && !state.quiet)
			output_line(&state.out, state.space.data, state.space.length, state.space_newline);
		flush_queue(&state);
		if (action == ACTION_RESTART)
			restart = 1;
		if (action == ACTION_QUIT || action == ACTION_QUIT_SILENT)
			break;
	}

	/* Succeeded: the outputs flushed and the status. */
	close_outputs(&state);
	if (state.quit_requested)
		return state.exit_status;
	return state.status;
}

/* Runs the script once over the pattern space; returns what to do next. */
static int
run_script(
	struct state *state)
{
	struct sed_command *command;
	size_t index;
	int selected;
	int action;

	/* The commands in order, from the first. */
	index = 0;
	while (index < state->program->count) {
		/* A command its addresses do not select; a block is skipped. */
		command = &state->program->commands[index];
		selected = command_selected(state, command);
		if (!selected) {
			if (command->name == '{')
				index = command->jump + 1U;
			else
				index++;
			continue;
		}

		/* The command. */
		index++;
		action = run_command(state, command, &index);
		if (action != ACTION_NEXT)
			return action;
	}

	/* The end of the script. */
	return ACTION_END;
}

/*
 * Runs one selected command.  *index is the next command, which a branch
 * changes.
 */
static int
run_command(
	struct state *state,
	struct sed_command *command,
	size_t *index)
{
	struct buffer swap;
	int action;

	/* The command of its name. */
	switch (command->name) {
	case '{':
	case '}':
	case ':':
		break;
	case '=':
		print_line_number(state);
		break;
	case 'a':
		queue_add(state, command->text, 0);
		break;
	case 'r':
		queue_add(state, command->text, 1);
		break;
	case 'i':
		output_line(&state->out, command->text, strlen(command->text), 1);
		break;
	case 'c':
		action = run_change(state, command);
		return action;
	case 'd':
		return ACTION_DELETE;
	case 'D':
		action = run_delete_first(state);
		return action;
	case 'g':
		buffer_set(&state->space, state->hold.data, state->hold.length);
		break;
	case 'G':
		buffer_add(&state->space, '\n');
		buffer_append(&state->space, state->hold.data, state->hold.length);
		break;
	case 'h':
		buffer_set(&state->hold, state->space.data, state->space.length);
		break;
	case 'H':
		buffer_add(&state->hold, '\n');
		buffer_append(&state->hold, state->space.data, state->space.length);
		break;
	case 'x':
		swap = state->space;
		state->space = state->hold;
		state->hold = swap;
		break;
	case 'l':
		list(state);
		break;
	case 'n':
		action = run_next(state);
		return action;
	case 'N':
		action = run_append_next(state);
		return action;
	case 'p':
		output_line(&state->out, state->space.data, state->space.length,
			    state->space_newline);
		break;
	case 'P':
		print_first_line(state);
		break;
	case 'q':
		state->exit_status = command->exit_status;
		state->quit_requested = 1;
		return ACTION_QUIT;
	case 's':
		substitute(state, command);
		break;
	case 't':
		if (state->replaced) {
			state->replaced = 0;
			*index = command->jump;
		}

		break;
	case 'b':
		*index = command->jump;
		break;
	case 'w':
		output_line(command->output, state->space.data,
			    state->space.length, state->space_newline);
		break;
	case 'y':
		translate(state, command->map);
		break;
	default:
		break;
	}

	/* Succeeded: on to the next command. */
	return ACTION_NEXT;
}

/*
 * c: deletes the pattern space and writes the text, at the end of a range
 * only (in its middle, the line is just deleted).
 */
static int
run_change(
	struct state *state,
	struct sed_command *command)
{
	/* Inside a range, nothing is written yet. */
	if (command->second.kind != SED_ADDRESS_NONE && command->in_range)
		return ACTION_DELETE;

	/* Succeeded: the text, and the next cycle. */
	output_line(&state->out, command->text, strlen(command->text), 1);
	return ACTION_DELETE;
}

/*
 * D: deletes the pattern space up to its first newline and runs the script
 * again on the rest without reading; with no newline it is d.
 */
static int
run_delete_first(
	struct state *state)
{
	char *newline;
	size_t cut;

	/* No newline: as d. */
	newline = memchr(state->space.data, '\n', state->space.length);
	if (newline == NULL)
		return ACTION_DELETE;

	/* Succeeded: the first line is dropped. */
	cut = (size_t)(newline - state->space.data) + 1U;
	memmove(state->space.data, state->space.data + cut,
		state->space.length - cut);
	state->space.length -= cut;
	state->space.data[state->space.length] = '\0';
	return ACTION_RESTART;
}

/*
 * n: writes the pattern space (unless -n) and replaces it with the next
 * line.  With no next line, sed ends (writing the pattern space).
 */
static int
run_next(
	struct state *state)
{
	int last;

	/* No next line. */
	last = is_last(state);
	if (last)
		return ACTION_QUIT;

	/* The pattern space out, the queue, and the next line in. */
	if (!state->quiet)
		output_line(&state->out, state->space.data, state->space.length, state->space_newline);
	flush_queue(state);
	(void)read_line(state, 0);

	/* Succeeded. */
	return ACTION_NEXT;
}

/*
 * N: appends a newline and the next line to the pattern space.  With no
 * next line, sed ends without writing the pattern space (POSIX).
 */
static int
run_append_next(
	struct state *state)
{
	int last;

	/* No next line. */
	last = is_last(state);
	if (last)
		return ACTION_QUIT_SILENT;

	/* Succeeded: the queue, and the next line appended. */
	flush_queue(state);
	(void)read_line(state, 1);
	return ACTION_NEXT;
}

/* Reports whether a command's addresses select the current line. */
static int
command_selected(
	struct state *state,
	struct sed_command *command)
{
	int selected;

	/* No address selects every line; one address, the lines it matches. */
	if (command->first.kind == SED_ADDRESS_NONE)
		selected = 1;
	else if (command->second.kind == SED_ADDRESS_NONE)
		selected = address_matches(state, &command->first);
	else
		selected = range_selected(state, command);

	/* ! turns the answer round. */
	if (command->negate && selected)
		return 0;
	if (command->negate)
		return 1;

	/* Succeeded: whether the command is selected. */
	return selected;
}

/*
 * Reports whether a range selects the current line.  A range starts at a
 * line its first address matches and ends at the next line its second
 * matches; a line number at or before the start ends it at once.
 */
static int
range_selected(
	struct state *state,
	struct sed_command *command)
{
	const struct sed_address *second;
	int matched;

	/* The address that ends the range. */
	second = &command->second;

	/* Not in the range: the first address may start it. */
	if (!command->in_range) {
		matched = address_matches(state, &command->first);
		if (!matched)
			return 0;
		if (second->kind == SED_ADDRESS_LINE) {
			if (second->line > state->line)
				command->in_range = 1;
			return 1;
		}

		/* $ ends the range on the last line itself. */
		if (second->kind == SED_ADDRESS_LAST) {
			matched = is_last(state);
			if (!matched)
				command->in_range = 1;
			return 1;
		}

		/* Any other second address is looked at from the next line on. */
		command->in_range = 1;
		return 1;
	}

	/* In the range: this line is in it, and may end it. */
	if (second->kind == SED_ADDRESS_LINE) {
		if (state->line >= second->line)
			command->in_range = 0;
		return 1;
	}

	/* The second address ends the range on this line. */
	matched = address_matches(state, second);
	if (matched)
		command->in_range = 0;

	/* Succeeded. */
	return 1;
}

/* Reports whether an address matches the current line. */
static int
address_matches(
	struct state *state,
	const struct sed_address *address)
{
	int matched;

	/* The address of its kind. */
	matched = 0;
	switch (address->kind) {
	case SED_ADDRESS_LINE:
		if (state->line == address->line)
			matched = 1;
		break;
	case SED_ADDRESS_LAST:
		matched = is_last(state);
		break;
	case SED_ADDRESS_REGEX:
		matched = regex_match(state, address->regex, state->space.data, NULL, 0);
		break;
	default:
		break;
	}

	/* Succeeded: whether it matched (no address matches nothing). */
	return matched;
}

/*
 * Matches a regex (NULL for the last one used) against text; matches, when
 * given, receives the groups.
 */
static int
regex_match(
	struct state *state,
	regex_t *regex,
	const char *text,
	regmatch_t *matches,
	int flags)
{
	int result;
	size_t groups;

	/* The empty regex is the last one used. */
	if (regex == NULL)
		regex = state->last_regex;
	if (regex == NULL)
		sed_fatal("no previous regular expression", NULL);
	state->last_regex = regex;

	/* Succeeded: whether it matched. */
	groups = 0;
	if (matches != NULL)
		groups = MATCH_GROUPS;
	result = regexec(regex, text, groups, matches, flags);
	if (result != 0)
		return 0;
	return 1;
}

/*
 * s: replaces the occurrence (or, with g, every occurrence from it on) of
 * the regex in the pattern space.  An empty match right after the previous
 * match is not one, so s/x*\/-/g on "abc" gives "-a-b-c-".
 */
static void
substitute(
	struct state *state,
	struct sed_command *command)
{
	const struct sed_substitute *s;
	regmatch_t matches[MATCH_GROUPS];
	struct buffer out;
	const char *text;
	size_t length;
	size_t start;
	size_t match_start;
	size_t match_end;
	size_t previous_end;
	unsigned long count;
	int have_previous;
	int replaced;
	int matched;
	int flags;

	/* The pattern space, and the output it is built into. */
	s = &command->substitute;
	text = state->space.data;
	length = state->space.length;
	memset(&out, 0, sizeof(out));
	buffer_reserve(&out, 0);
	start = 0;
	count = 0;
	replaced = 0;
	have_previous = 0;
	previous_end = 0;
	while (start <= length) {
		/* The next match; ^ matches only at the start of the space. */
		flags = 0;
		if (start > 0)
			flags = REG_NOTBOL;
		matched = regex_match(state, s->regex, text + start, matches,
				      flags);
		if (!matched)
			break;
		match_start = start + (size_t)matches[0].rm_so;
		match_end = start + (size_t)matches[0].rm_eo;

		/* An empty match right after the previous one is skipped. */
		if (match_start == match_end && have_previous &&
		    match_start == previous_end) {
			if (match_start >= length)
				break;
			buffer_add(&out, text[match_start]);
			start = match_start + 1U;
			continue;
		}

		/* The text before the match, then the match or its replacement. */
		count++;
		buffer_append(&out, text + start, match_start - start);
		if (count == s->occurrence || (s->global && count > s->occurrence)) {
			add_replacement(&out, s->replacement, text + start,
					matches);
			replaced = 1;
		} else {
			buffer_append(&out, text + match_start,
				      match_end - match_start);
		}

		/* The match, which an empty match right after it cannot be. */
		have_previous = 1;
		previous_end = match_end;

		/* On past the match; past one character for an empty one. */
		start = match_end;
		if (match_start == match_end) {
			if (match_start < length)
				buffer_add(&out, text[match_start]);
			start = match_start + 1U;
		}

		/* Without g, the first replacement is the only one. */
		if (replaced && !s->global)
			break;
	}

	/* Nothing replaced: the pattern space stays. */
	if (!replaced) {
		free(out.data);
		return;
	}

	/* The rest of the text, and the new pattern space. */
	if (start < length)
		buffer_append(&out, text + start, length - start);
	buffer_set(&state->space, out.data, out.length);
	free(out.data);
	state->replaced = 1;

	/* p and w write the result. */
	if (s->print)
		output_line(&state->out, state->space.data, state->space.length, state->space_newline);
	if (s->output != NULL) {
		output_line(s->output, state->space.data, state->space.length,
			    state->space_newline);
	}
}

/*
 * Adds a replacement: & is the match, \1 to \9 the groups, \& and \\ the
 * characters themselves; text is where the match's offsets start.
 */
static void
add_replacement(
	struct buffer *out,
	const char *replacement,
	const char *text,
	const regmatch_t *matches)
{
	const char *cursor;
	const regmatch_t *group;

	/* Each character of the replacement. */
	for (cursor = replacement; *cursor != '\0'; cursor++) {
		/* & is the whole match. */
		if (*cursor == '&') {
			buffer_append(out, text + matches[0].rm_so,
				      (size_t)(matches[0].rm_eo - matches[0].rm_so));
			continue;
		}

		/* An ordinary character. */
		if (*cursor != '\\' || cursor[1] == '\0') {
			buffer_add(out, *cursor);
			continue;
		}

		/* \1 to \9: a group, empty when it did not take part. */
		cursor++;
		if (*cursor >= '1' && *cursor <= '9') {
			group = &matches[*cursor - '0'];
			if (group->rm_so >= 0)
				buffer_append(out, text + group->rm_so, (size_t)(group->rm_eo - group->rm_so));
			continue;
		}

		/* Any other escaped character is itself. */
		buffer_add(out, *cursor);
	}
}

/* y: maps each byte of the pattern space. */
static void
translate(
	struct state *state,
	const unsigned char *map)
{
	size_t index;

	/* Each byte. */
	for (index = 0; index < state->space.length; index++) {
		state->space.data[index] =
		    (char)map[(unsigned char)state->space.data[index]];
	}
}

/*
 * l: writes the pattern space unambiguously: \\ and the C escapes, other
 * unprintable bytes in octal, long lines folded with a backslash, and $ at
 * the end.
 */
static void
list(
	struct state *state)
{
	static const char escapes[] = "\\\\\aa\bb\ff\nn\rr\tt\vv";
	struct buffer out;
	const char *found;
	char text[8];
	size_t index;
	size_t width;
	unsigned char value;

	/* The line, wrapped and escaped. */
	memset(&out, 0, sizeof(out));
	width = 0;
	for (index = 0; index < state->space.length; index++) {
		value = (unsigned char)state->space.data[index];

		/* A C escape. */
		found = NULL;
		if (value != '\0')
			found = memchr(escapes, value, sizeof(escapes) - 1U);
		if (found != NULL && ((size_t)(found - escapes) % 2U) == 0) {
			text[0] = '\\';
			text[1] = found[1];
			list_add(&out, text, 2, &width);
			continue;
		}

		/* An unprintable byte in octal, or a printable one as it is. */
		if (value < 0x20U || value >= 0x7fU) {
			(void)snprintf(text, sizeof(text), "\\%03o", value);
			list_add(&out, text, 4, &width);
		} else {
			text[0] = (char)value;
			list_add(&out, text, 1, &width);
		}
	}

	/* Succeeded: the end of the line. */
	buffer_add(&out, '$');
	output_line(&state->out, out.data, out.length, 1);
	free(out.data);
}

/* Adds a piece of l's output, folding the line before it would be too wide. */
static void
list_add(
	struct buffer *out,
	const char *text,
	size_t length,
	size_t *width)
{
	/* A backslash and a newline fold the line. */
	if (*width + length > LIST_WIDTH) {
		buffer_append(out, "\\\n", 2);
		*width = 0;
	}

	/* The piece. */
	buffer_append(out, text, length);
	*width += length;
}

/* P: writes the pattern space up to its first newline. */
static void
print_first_line(
	struct state *state)
{
	char *newline;

	/* The whole space when it has one line. */
	newline = memchr(state->space.data, '\n', state->space.length);
	if (newline == NULL) {
		output_line(&state->out, state->space.data, state->space.length,
			    state->space_newline);
		return;
	}

	/* The first line. */
	output_line(&state->out, state->space.data,
		    (size_t)(newline - state->space.data), 1);
}

/* =: writes the line number. */
static void
print_line_number(
	struct state *state)
{
	char text[32];
	int length;

	/* The number and a newline. */
	length = snprintf(text, sizeof(text), "%lu", state->line);
	output_line(&state->out, text, (size_t)length, 1);
}

/*
 * Reads the next input line into the pattern space, or (append) after a
 * newline at its end.  Returns 0 at the end of the input.
 */
static int
read_line(
	struct state *state,
	int append)
{
	/* The line read ahead. */
	fill_ahead(state);
	if (!state->have_ahead)
		return 0;

	/* Into the pattern space. */
	if (append) {
		buffer_add(&state->space, '\n');
		buffer_append(&state->space, state->ahead.data,
			      state->ahead.length);
	} else {
		buffer_set(&state->space, state->ahead.data,
			   state->ahead.length);
	}

	/* The newline it had, and the count of lines. */
	state->space_newline = state->ahead_newline;
	state->have_ahead = 0;
	state->line++;

	/* Succeeded. */
	return 1;
}

/* Reports whether the current line is the last of the input. */
static int
is_last(
	struct state *state)
{
	/* The last line is the one with nothing after it. */
	fill_ahead(state);
	if (state->have_ahead)
		return 0;
	return 1;
}

/* Reads the next line ahead, from the next file when one runs out. */
static void
fill_ahead(
	struct state *state)
{
	int read;

	/* A line read ahead already. */
	if (state->have_ahead)
		return;
	for (;;) {
		/* The next file that can be read. */
		if (state->stream == NULL)
			open_next(state);
		if (state->stream == NULL)
			return;

		/* A line of it, or the end of it. */
		read = read_ahead(state);
		if (read) {
			state->have_ahead = 1;
			return;
		}

		/* At its end, the next file. */
		if (state->stream != stdin)
			fclose(state->stream);
		state->stream = NULL;
	}
}

/*
 * Reads one line of the open file ahead.  Returns 0 at its end.  The
 * newline is not kept; ahead_newline says whether there was one.
 */
static int
read_ahead(
	struct state *state)
{
	int value;

	/* The line, emptied before it is read. */
	state->ahead.length = 0;
	state->ahead_newline = 0;
	for (;;) {
		/* The next byte. */
		value = getc(state->stream);
		if (value == EOF)
			break;
		if (value == '\n') {
			state->ahead_newline = 1;
			break;
		}

		/* A byte of the line. */
		buffer_add(&state->ahead, (char)value);
	}

	/* Succeeded: whether a line was read. */
	if (state->ahead.length > 0 || state->ahead_newline)
		return 1;
	return 0;
}

/*
 * Opens the next input file; - is standard input, and so is no file at
 * all.  A file that cannot be read is reported and skipped (status 2).
 */
static void
open_next(
	struct state *state)
{
	const char *name;
	int compare;

	/* No files: standard input, once. */
	if (state->file_count == 0) {
		if (!state->read_standard_input) {
			state->read_standard_input = 1;
			state->stream = stdin;
		}

		/* Nothing more to open. */
		return;
	}

	/* The named files in turn. */
	while (state->next_file < state->file_count) {
		/* The next name. */
		name = state->files[state->next_file];
		state->next_file++;

		/* - is standard input. */
		compare = strcmp(name, "-");
		if (compare == 0) {
			state->stream = stdin;
			return;
		}

		/* A file, or a message and the next one. */
		state->stream = fopen(name, "r");
		if (state->stream != NULL)
			return;
		fprintf(stderr, "sed: can't read %s: %s\n", name,
			strerror(errno));
		state->status = 2;
	}
}

/* Queues text (a) or a file (r) for the end of the cycle. */
static void
queue_add(
	struct state *state,
	const char *text,
	int is_file)
{
	/* Room for one more. */
	if (state->queue_count == state->queue_capacity) {
		state->queue_capacity = state->queue_capacity * 2U + 8U;
		state->queue = sed_realloc(state->queue,
		    state->queue_capacity * sizeof(*state->queue));
	}

	/* The item. */
	state->queue[state->queue_count].text = text;
	state->queue[state->queue_count].is_file = is_file;
	state->queue_count++;
}

/* Writes the queued text and files, and empties the queue. */
static void
flush_queue(
	struct state *state)
{
	size_t index;

	/* Each item, in order. */
	for (index = 0; index < state->queue_count; index++) {
		if (state->queue[index].is_file) {
			copy_file(state, state->queue[index].text);
		} else {
			output_line(&state->out, state->queue[index].text,
				    strlen(state->queue[index].text), 1);
		}
	}

	/* The queue is empty again. */
	state->queue_count = 0;
}

/* Copies a file to the output (r); a file that cannot be read adds nothing. */
static void
copy_file(
	struct state *state,
	const char *name)
{
	char chunk[4096];
	size_t count;
	FILE *stream;

	/* The file. */
	stream = fopen(name, "r");
	if (stream == NULL)
		return;

	/* Its bytes, after any newline owed. */
	output_pending_newline(&state->out);
	for (;;) {
		count = fread(chunk, 1, sizeof(chunk), stream);
		if (count == 0)
			break;
		fwrite(chunk, 1, count, state->out.stream);
	}

	/* The file is done with. */
	fclose(stream);
}

/*
 * Writes a line to an output; newline 0 leaves it off (the input lacked
 * it), and it is written before anything that follows.
 */
static void
output_line(
	struct sed_output *output,
	const char *data,
	size_t length,
	int newline)
{
	/* A newline owed, the line, and its own newline. */
	output_pending_newline(output);
	fwrite(data, 1, length, output->stream);
	if (newline)
		putc('\n', output->stream);
	else
		output->missing_newline = 1;
}

/* Writes the newline a line was written without, when one is owed. */
static void
output_pending_newline(
	struct sed_output *output)
{
	/* Only when owed. */
	if (!output->missing_newline)
		return;
	putc('\n', output->stream);
	output->missing_newline = 0;
}

/* Sets a buffer to a copy of bytes. */
static void
buffer_set(
	struct buffer *buffer,
	const char *data,
	size_t length)
{
	/* Emptied, then filled; data may be the buffer's own. */
	if (data == buffer->data) {
		buffer->length = length;
		buffer->data[length] = '\0';
		return;
	}

	/* Emptied, then filled. */
	buffer->length = 0;
	buffer_append(buffer, data, length);
}

/* Appends bytes to a buffer, which stays NUL-terminated. */
static void
buffer_append(
	struct buffer *buffer,
	const char *data,
	size_t length)
{
	/* Room, then the bytes. */
	buffer_reserve(buffer, buffer->length + length);
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
}

/* Appends one byte to a buffer. */
static void
buffer_add(
	struct buffer *buffer,
	char value)
{
	/* As a one-byte string. */
	buffer_append(buffer, &value, 1);
}

/* Makes room in a buffer for length bytes and a NUL. */
static void
buffer_reserve(
	struct buffer *buffer,
	size_t length)
{
	/* Enough room already. */
	if (buffer->data != NULL && length + 1U <= buffer->capacity)
		return;

	/* Succeeded: grown, and terminated when it was empty. */
	if (buffer->capacity < length + 1U)
		buffer->capacity = length + 1U;
	buffer->capacity = buffer->capacity * 2U + 32U;
	buffer->data = sed_realloc(buffer->data, buffer->capacity);
	if (buffer->length == 0)
		buffer->data[0] = '\0';
}

/* Flushes standard output and closes the files of w. */
static void
close_outputs(
	struct state *state)
{
	struct sed_output *output;

	/* Standard output, then each file. */
	fflush(stdout);
	for (output = state->program->outputs; output != NULL;
	     output = output->next) {
		if (output->stream != stdout && output->stream != stderr)
			fclose(output->stream);
	}
}
