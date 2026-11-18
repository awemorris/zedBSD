/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Editing in Text Editor (plan/ws092/design.md section 9): the cursor's
 * moves, the selection, typing and deleting with their undo steps,
 * indenting, the clipboard's copy, cut and paste, undo and redo, and
 * finding.
 *
 * Every change goes through edit_insert_raw or edit_delete_raw, which keep
 * the rows laid out, and records a step in the undo history (except when
 * undo or redo itself replays one).  All text typed or pasted, and an
 * input method's committed text later (WS095), goes through
 * te_edit_insert_text.
 */

#include "textedit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of character a word is made of, for the word moves. */
#define EDIT_CLASS_BLANK	0
#define EDIT_CLASS_WORD		1
#define EDIT_CLASS_MARK		2
#define EDIT_CLASS_HIRAGANA	3
#define EDIT_CLASS_KATAKANA	4
#define EDIT_CLASS_KANJI	5
#define EDIT_CLASS_NEWLINE	6

/* The moves of the cursor. */
#define EDIT_MOVE_LEFT		0
#define EDIT_MOVE_RIGHT		1
#define EDIT_MOVE_UP		2
#define EDIT_MOVE_DOWN		3
#define EDIT_MOVE_WORD_LEFT	4
#define EDIT_MOVE_WORD_RIGHT	5
#define EDIT_MOVE_HOME		6
#define EDIT_MOVE_END		7
#define EDIT_MOVE_PAGE_UP	8
#define EDIT_MOVE_PAGE_DOWN	9
#define EDIT_MOVE_START		10
#define EDIT_MOVE_FINISH	11

/* The most a paste takes. */
#define EDIT_PASTE_MAX		TE_FILE_MAX

/* The most text copied to the clipboard or the primary selection at once. */
#define EDIT_COPY_MAX		TE_FILE_MAX

static int edit_insert_raw(struct te_app *app, size_t position, const char *text, size_t length);
static void edit_delete_raw(struct te_app *app, size_t start, size_t end);
static int edit_record(struct te_app *app, int kind, size_t position, const char *text, size_t length, size_t cursor_before, size_t anchor_before, unsigned group, int merge);
static void edit_changed(struct te_app *app);
static int edit_command_key(struct te_app *app, const struct te_event *event);
static int edit_move_key(struct te_app *app, const struct te_event *event);
static void edit_move(struct te_app *app, int move, int extend);
static size_t edit_target(struct te_app *app, int move);
static size_t edit_vertical(struct te_app *app, long rows);
static size_t edit_home(struct te_app *app);
static size_t edit_end(struct te_app *app);
static int edit_class(uint32_t codepoint);
static size_t edit_word_left(const struct te_app *app, size_t position);
static size_t edit_word_right(const struct te_app *app, size_t position);
static void edit_backspace(struct te_app *app, int word);
static void edit_forward_delete(struct te_app *app, int word);
static void edit_newline(struct te_app *app);
static void edit_indent(struct te_app *app, int outdent);
static int edit_type(struct te_app *app, uint32_t codepoint);
static size_t edit_encode(uint32_t codepoint, char *out);
static size_t edit_normalize(char *text, size_t length);
static int edit_page_rows(const struct te_app *app);

/*
 * Puts text in place of the selection (or at the cursor), as one undo
 * step; merge says how it may join the typing before it.  The cursor
 * follows the text.
 *
 * Returns 0, or ENOMEM (the text is then not inserted).
 */
int
te_edit_insert_text(
	struct te_app *app,
	const char *text,
	size_t length,
	int merge)
{
	char *removed;
	size_t start;
	size_t end;
	size_t cursor_before;
	size_t anchor_before;
	unsigned group;
	int error;

	/* The selection, and where the cursor was, for the undo step. */
	te_edit_selection(app, &start, &end);
	cursor_before = app->cursor;
	anchor_before = app->anchor;
	group = te_undo_group(&app->undo);

	/* A selection goes first, in the same step; typing over it does not join earlier typing. */
	if (end > start) {
		removed = malloc(end - start);
		if (removed == NULL)
			return ENOMEM;
		te_buffer_copy(&app->buffer, start, end, removed);
		edit_delete_raw(app, start, end);
		app->cursor = start;
		app->anchor = start;
		(void)edit_record(app, TE_UNDO_DELETE, start, removed, end - start, cursor_before, anchor_before, group, TE_MERGE_NONE);
		free(removed);
		merge = TE_MERGE_NONE;
	}

	/* Nothing more when there is no text. */
	if (length == 0U) {
		edit_changed(app);
		return 0;
	}

	/* The text at the cursor. */
	error = edit_insert_raw(app, start, text, length);
	if (error != 0) {
		edit_changed(app);
		return error;
	}

	/* The cursor after it, and the step. */
	app->cursor = start + length;
	app->anchor = app->cursor;
	(void)edit_record(app, TE_UNDO_INSERT, start, text, length, cursor_before, anchor_before, group, merge);
	edit_changed(app);

	/* Succeeded: the text is in. */
	return 0;
}

/*
 * Deletes the text from start up to end as one undo step; merge says how
 * it may join the deleting before it.  The cursor goes to start.
 *
 * Returns 0, or ENOMEM (nothing is then deleted).
 */
int
te_edit_delete(
	struct te_app *app,
	size_t start,
	size_t end,
	int merge)
{
	char *removed;
	size_t cursor_before;
	size_t anchor_before;
	unsigned group;

	/* Nothing to delete. */
	if (end <= start)
		return 0;

	/* The text, kept for undo. */
	removed = malloc(end - start);
	if (removed == NULL)
		return ENOMEM;
	te_buffer_copy(&app->buffer, start, end, removed);

	/* The deletion, and the step. */
	cursor_before = app->cursor;
	anchor_before = app->anchor;
	group = te_undo_group(&app->undo);
	edit_delete_raw(app, start, end);
	app->cursor = start;
	app->anchor = start;
	(void)edit_record(app, TE_UNDO_DELETE, start, removed, end - start, cursor_before, anchor_before, group, merge);
	free(removed);
	edit_changed(app);

	/* Succeeded: the text is gone. */
	return 0;
}

/*
 * Carries out a key in the text: a move, an edit, or a command with
 * Control.  Returns 1 when the key did something, 0 when it is not the
 * text's.
 */
int
te_edit_key(
	struct te_app *app,
	const struct te_event *event)
{
	uint32_t codepoint;
	int done;

	/* Only presses. */
	if (!event->pressed)
		return 0;

	/* The commands with Control (the menus choose most of them first). */
	done = edit_command_key(app, event);
	if (done)
		return 1;

	/* The moves. */
	done = edit_move_key(app, event);
	if (done)
		return 1;

	/* The editing keys, and a typed character. */
	switch (event->key) {
	case TE_KEY_BACKSPACE:
		edit_backspace(app, (event->modifiers & TE_MOD_CTRL) != 0U);
		return 1;
	case TE_KEY_DELETE:
		/* Shift+Delete cuts, as on older systems. */
		if ((event->modifiers & TE_MOD_SHIFT) != 0U) {
			te_edit_cut(app);
			return 1;
		}

		/* Otherwise it deletes forward. */
		edit_forward_delete(app, (event->modifiers & TE_MOD_CTRL) != 0U);
		return 1;
	case TE_KEY_ENTER:
	case TE_KEY_KP_ENTER:
		edit_newline(app);
		return 1;
	case TE_KEY_TAB:
		edit_indent(app, (event->modifiers & TE_MOD_SHIFT) != 0U);
		return 1;
	case TE_KEY_INSERT:
		/* Shift+Insert pastes and Ctrl+Insert copies. */
		if ((event->modifiers & TE_MOD_SHIFT) != 0U)
			te_edit_paste(app, 0);
		else if ((event->modifiers & TE_MOD_CTRL) != 0U)
			te_edit_copy(app);
		return 1;
	default:
		break;
	}

	/* A character. */
	codepoint = kl_key_character(event->key, event->modifiers);
	if (codepoint == 0U)
		return 0;
	done = edit_type(app, codepoint);

	/* Reports whether the character was typed. */
	return done;
}

/*
 * Gives the selection from its start up to its end (both the cursor when
 * nothing is selected).
 */
void
te_edit_selection(
	const struct te_app *app,
	size_t *start,
	size_t *end)
{
	/* The nearer end first. */
	if (app->anchor < app->cursor) {
		*start = app->anchor;
		*end = app->cursor;
	} else {
		*start = app->cursor;
		*end = app->anchor;
	}
}

/*
 * Selects from an anchor to the cursor, and shows the cursor.
 */
void
te_edit_select(
	struct te_app *app,
	size_t anchor,
	size_t cursor)
{
	size_t length;

	/* Both within the text. */
	length = te_buffer_length(&app->buffer);
	if (anchor > length)
		anchor = length;
	if (cursor > length)
		cursor = length;

	/* The selection; a new one is offered as the primary selection. */
	app->anchor = anchor;
	app->cursor = cursor;
	app->goal_valid = 0;
	if (anchor != cursor)
		app->primary_changed = 1;
	app->blink_start = app->now;
	app->dirty = 1;
}

/*
 * Copies the selection to the clipboard.
 */
void
te_edit_copy(
	struct te_app *app)
{
	size_t start;
	size_t end;
	char *text;

	/* Nothing selected, or nowhere to copy to. */
	te_edit_selection(app, &start, &end);
	if (end <= start || app->host.copy == NULL)
		return;
	if (end - start > EDIT_COPY_MAX)
		end = start + EDIT_COPY_MAX;

	/* The selected text, which the window keeps its own copy of. */
	text = malloc(end - start);
	if (text == NULL) {
		te_app_message(app, "Not enough memory to copy.");
		return;
	}

	/* The text goes to the clipboard. */
	te_buffer_copy(&app->buffer, start, end, text);
	app->host.copy(app->host.data, text, end - start);
	free(text);
}

/*
 * Copies the selection to the clipboard and deletes it.
 */
void
te_edit_cut(
	struct te_app *app)
{
	size_t start;
	size_t end;

	/* The copy, then the deletion. */
	te_edit_selection(app, &start, &end);
	if (end <= start)
		return;
	te_edit_copy(app);
	(void)te_edit_delete(app, start, end, TE_MERGE_NONE);
}

/*
 * Pastes the clipboard's text (or the primary selection's) in place of the
 * selection.
 */
void
te_edit_paste(
	struct te_app *app,
	int primary)
{
	size_t length;
	char *text;
	int error;

	/* Nowhere to paste from. */
	if (!primary && app->host.paste == NULL)
		return;
	if (primary && app->host.paste_primary == NULL)
		return;

	/* Room for the text. */
	text = malloc(EDIT_PASTE_MAX);
	if (text == NULL) {
		te_app_message(app, "Not enough memory to paste.");
		return;
	}

	/* The text, with CR LF made LF. */
	if (primary)
		length = app->host.paste_primary(app->host.data, text, EDIT_PASTE_MAX);
	else
		length = app->host.paste(app->host.data, text, EDIT_PASTE_MAX);
	length = edit_normalize(text, length);

	/* The text in place of the selection. */
	error = 0;
	if (length != 0U)
		error = te_edit_insert_text(app, text, length, TE_MERGE_NONE);
	free(text);
	if (error != 0)
		te_app_message(app, "Not enough memory to paste.");
}

/*
 * Selects the whole text.
 */
void
te_edit_select_all(
	struct te_app *app)
{
	size_t length;

	/* From the start to the end, the cursor at the end. */
	length = te_buffer_length(&app->buffer);
	te_edit_select(app, 0, length);
}

/*
 * Takes back the last change (a group of steps), and puts the cursor and
 * the selection back where they were before it.
 */
void
te_edit_undo(
	struct te_app *app)
{
	const struct te_undo_step *step;
	size_t first;
	size_t last;
	size_t index;
	int found;
	int error;

	/* The steps of the last change. */
	found = te_undo_undo_range(&app->undo, &first, &last);
	if (!found)
		return;

	/* Each, from the last to the first, done the other way round. */
	for (index = last; index > first; index--) {
		step = te_undo_step(&app->undo, index - 1U);
		if (step->kind == TE_UNDO_INSERT) {
			edit_delete_raw(app, step->position, step->position + step->length);
		} else {
			error = edit_insert_raw(app, step->position, step->text, step->length);
			if (error != 0)
				te_app_message(app, "Not enough memory to undo.");
		}
	}

	/* The cursor and the selection before the change. */
	step = te_undo_step(&app->undo, first);
	te_edit_select(app, step->anchor_before, step->cursor_before);
	edit_changed(app);
}

/*
 * Does the change that was taken back last again.
 */
void
te_edit_redo(
	struct te_app *app)
{
	const struct te_undo_step *step;
	size_t first;
	size_t last;
	size_t index;
	int found;
	int error;

	/* The steps of the next change. */
	found = te_undo_redo_range(&app->undo, &first, &last);
	if (!found)
		return;

	/* Each, in order. */
	for (index = first; index < last; index++) {
		step = te_undo_step(&app->undo, index);
		if (step->kind == TE_UNDO_INSERT) {
			error = edit_insert_raw(app, step->position, step->text, step->length);
			if (error != 0)
				te_app_message(app, "Not enough memory to redo.");
		} else {
			edit_delete_raw(app, step->position, step->position + step->length);
		}
	}

	/* The cursor and the selection after the change. */
	step = te_undo_step(&app->undo, last - 1U);
	te_edit_select(app, step->anchor_after, step->cursor_after);
	edit_changed(app);
}

/*
 * Gives the word around a position: the run of characters of one kind
 * (letters and digits, marks, blanks, or one kind of Japanese script).
 */
void
te_edit_word(
	const struct te_app *app,
	size_t position,
	size_t *start,
	size_t *end)
{
	uint32_t codepoint;
	size_t length;
	size_t next;
	size_t before;
	int kind;
	int other;

	/* The kind of the character at the position (or before it, at a line's end). */
	length = te_buffer_length(&app->buffer);
	codepoint = te_buffer_char(&app->buffer, position, &next);
	if (position >= length || codepoint == '\n') {
		if (position == 0U) {
			*start = position;
			*end = position;
			return;
		}

		/* The character before is the word's. */
		position = te_buffer_prev_char(&app->buffer, position);
		codepoint = te_buffer_char(&app->buffer, position, &next);
	}

	/* The word is the run of that character's kind. */
	kind = edit_class(codepoint);

	/* Back over the same kind. */
	*start = position;
	while (*start > 0U) {
		before = te_buffer_prev_char(&app->buffer, *start);
		codepoint = te_buffer_char(&app->buffer, before, &next);
		other = edit_class(codepoint);
		if (other != kind)
			break;
		*start = before;
	}

	/* Forward over it. */
	*end = position;
	while (*end < length) {
		codepoint = te_buffer_char(&app->buffer, *end, &next);
		other = edit_class(codepoint);
		if (other != kind)
			break;
		*end = next;
	}
}

/*
 * Gives the line around a position, with its newline.
 */
void
te_edit_line(
	const struct te_app *app,
	size_t position,
	size_t *start,
	size_t *end)
{
	size_t line;
	size_t length;

	/* The line's start, and the start of the next (or the end). */
	line = te_buffer_line_of(&app->buffer, position);
	*start = te_buffer_line_start(&app->buffer, line);
	*end = te_buffer_line_start(&app->buffer, line + 1U);
	length = te_buffer_length(&app->buffer);
	if (*end > length)
		*end = length;
}

/*
 * Reports the position nearest to a point of the window.
 */
size_t
te_edit_position_at(
	struct te_app *app,
	int x,
	int y)
{
	struct te_rect text;
	double row_place;
	double column_place;
	size_t row;
	size_t column;
	size_t position;

	/* The point in rows and cells of the text. */
	te_app_text_rect(app, &text);
	row_place = ((double)(y - text.y) + app->scroll_y) / (double)app->row_height;
	column_place = ((double)(x - text.x) + app->scroll_x) / (double)app->cell;

	/* Above the text is its first row; left of it, its first column. */
	row = 0;
	if (row_place > 0.0)
		row = (size_t)row_place;
	if (app->layout.total > 0U && row >= app->layout.total)
		row = app->layout.total - 1U;
	column = 0;
	if (column_place > 0.0)
		column = (size_t)(column_place + 0.5);

	/* Succeeded: the boundary nearest to that cell. */
	position = te_layout_position(&app->layout, &app->buffer, row, column);
	return position;
}

/*
 * Scrolls the view so that the cursor shows.
 */
void
te_edit_reveal(
	struct te_app *app)
{
	struct te_rect text;
	size_t row;
	size_t column;
	double top;
	double left;

	/* Where the cursor is. */
	te_app_text_rect(app, &text);
	te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);
	top = (double)row * (double)app->row_height;
	left = (double)column * (double)app->cell;

	/* Up or down until its row is in the view. */
	if (top < app->scroll_y)
		app->scroll_y = top;
	if (top + (double)app->row_height > app->scroll_y + (double)text.height)
		app->scroll_y = top + (double)app->row_height - (double)text.height;

	/* Across, when lines are not wrapped. */
	if (!app->wrap) {
		if (left < app->scroll_x)
			app->scroll_x = left;
		if (left + (double)app->cell > app->scroll_x + (double)text.width)
			app->scroll_x = left + (double)app->cell - (double)text.width;
	}

	/* The scroll stops where the view is now (a glide or a flight ends there). */
	te_app_clamp(app);
	app->dirty = 1;
}

/*
 * Finds the find text again: forward from the selection's end (or its
 * start while the text is being typed), or backward from its start, and
 * selects it.
 */
void
te_edit_find(
	struct te_app *app,
	int forward,
	int from_selection)
{
	char message[TE_FIND_MAX + 48];
	size_t start;
	size_t end;
	size_t from;
	size_t found;
	int wrapped;
	int matched;

	/* Nothing to find. */
	if (app->find_length == 0U)
		return;

	/* Where the search starts. */
	te_edit_selection(app, &start, &end);
	from = start;
	if (forward && !from_selection)
		from = end;

	/* The next place, going round. */
	matched = te_find(&app->buffer, app->find, app->find_length, from, forward, &found, &wrapped);
	if (!matched) {
		snprintf(message, sizeof(message), "No matches for \"%s\"", app->find);
		te_app_message(app, message);
		return;
	}

	/* The place is selected and shown. */
	te_edit_select(app, found, found + app->find_length);
	te_edit_reveal(app);
	if (wrapped)
		te_app_message(app, "Search wrapped");
}

/*
 * Replaces the place found and moves on (Edit > Replace, ws128-p003): when
 * the selection is the find text (in either case of ASCII letters, as the
 * search finds it), it becomes the replacement as one undo step, and the
 * next place is selected; otherwise the next place is only selected, so
 * that the next Replace replaces what is shown.  Returns 1 when a place
 * was replaced, 0 when not (nothing to find, or only a place selected).
 */
int
te_edit_replace(
	struct te_app *app,
	const char *with,
	size_t with_length)
{
	size_t start;
	size_t end;
	int matched;
	int error;

	/* Nothing to find. */
	if (app->find_length == 0U)
		return 0;

	/* The selection is the place to replace when it holds the find text. */
	te_edit_selection(app, &start, &end);
	matched = 0;
	if (end - start == app->find_length)
		matched = te_find_at(&app->buffer, app->find, app->find_length, start);

	/* Not a place found: the next one is selected and shown. */
	if (!matched) {
		te_edit_find(app, 1, 1);
		return 0;
	}

	/* The replacement over the selection, as one step (an empty one deletes it). */
	error = te_edit_insert_text(app, with, with_length, TE_MERGE_NONE);
	if (error != 0) {
		te_app_message(app, "Not enough memory to replace.");
		return 0;
	}

	/* The next place after it, selected. */
	te_edit_find(app, 1, 0);

	/* Succeeded: one place was replaced. */
	return 1;
}

/*
 * Replaces every place the find text occurs (Edit > Replace All,
 * ws128-p003), as one undo group, so that one Undo puts them all back.
 * The places are found first, from the start and not overlapping, then
 * replaced from the last back, so that each place found stays where it
 * was.  The cursor goes after the first replacement.  Returns how many
 * were replaced (0 when the text does not occur, or memory runs out
 * before anything changed).
 */
size_t
te_edit_replace_all(
	struct te_app *app,
	const char *with,
	size_t with_length)
{
	size_t *places;
	size_t *grown;
	char *removed;
	size_t capacity;
	size_t count;
	size_t total;
	size_t position;
	size_t index;
	size_t cursor_before;
	size_t anchor_before;
	unsigned group;
	int matched;
	int error;

	/* Nothing to find. */
	if (app->find_length == 0U)
		return 0;

	/* The places, from the start, each after the one before. */
	places = NULL;
	capacity = 0;
	count = 0;
	total = te_buffer_length(&app->buffer);
	position = 0;
	while (position + app->find_length <= total) {
		matched = te_find_at(&app->buffer, app->find, app->find_length, position);
		if (!matched) {
			position++;
			continue;
		}

		/* A full list grows (doubling). */
		if (count == capacity) {
			capacity = capacity * 2U + 16U;
			grown = realloc(places, capacity * sizeof(places[0]));
			if (grown == NULL) {
				free(places);
				te_app_message(app, "Not enough memory to replace.");
				return 0;
			}

			/* The list from now on is the grown one. */
			places = grown;
		}

		/* The place, and the search goes on after it. */
		places[count] = position;
		count++;
		position += app->find_length;
	}

	/* The text occurs nowhere. */
	if (count == 0U) {
		free(places);
		return 0;
	}

	/* Room for each place's text, kept for Undo. */
	removed = malloc(app->find_length);
	if (removed == NULL) {
		free(places);
		te_app_message(app, "Not enough memory to replace.");
		return 0;
	}

	/* Where the cursor was (Undo puts it back), and the group the steps share. */
	cursor_before = app->cursor;
	anchor_before = app->anchor;
	group = te_undo_group(&app->undo);
	for (index = count; index > 0U; index--) {
		/* The place's text goes, kept for Undo. */
		position = places[index - 1U];
		te_buffer_copy(&app->buffer, position, position + app->find_length, removed);
		edit_delete_raw(app, position, position + app->find_length);
		app->cursor = position;
		app->anchor = position;
		(void)edit_record(app, TE_UNDO_DELETE, position, removed, app->find_length, cursor_before, anchor_before, group, TE_MERGE_NONE);

		/* The replacement comes in its place (none for an empty one). */
		if (with_length == 0U)
			continue;
		error = edit_insert_raw(app, position, with, with_length);
		if (error != 0) {
			te_app_message(app, "Not enough memory: some places were not replaced.");
			break;
		}

		/* The cursor after it, and the step. */
		app->cursor = position + with_length;
		app->anchor = app->cursor;
		(void)edit_record(app, TE_UNDO_INSERT, position, with, with_length, cursor_before, anchor_before, group, TE_MERGE_NONE);
	}

	/* The cursor after the first replacement, and the frame drawn again. */
	app->cursor = places[0] + with_length;
	app->anchor = app->cursor;
	free(removed);
	free(places);
	edit_changed(app);

	/* Succeeded: how many places were replaced. */
	return count;
}

/* Inserts text at a position and keeps the rows laid out; returns 0 or ENOMEM. */
static int
edit_insert_raw(
	struct te_app *app,
	size_t position,
	const char *text,
	size_t length)
{
	size_t line;
	size_t newlines;
	size_t index;
	int error;

	/* The line it goes into, and how many lines it adds. */
	line = te_buffer_line_of(&app->buffer, position);
	newlines = 0;
	for (index = 0; index < length; index++) {
		if (text[index] == '\n')
			newlines++;
	}

	/* The text. */
	error = te_buffer_insert(&app->buffer, position, text, length);
	if (error != 0)
		return error;

	/* The rows of the lines it touched; without memory the whole text is laid out again. */
	error = te_layout_update(&app->layout, &app->buffer, line, 1U, 1U + newlines);
	if (error != 0)
		te_app_relayout(app);

	/* Succeeded: the text is in. */
	return 0;
}

/* Deletes text and keeps the rows laid out. */
static void
edit_delete_raw(
	struct te_app *app,
	size_t start,
	size_t end)
{
	size_t first;
	size_t last;
	int error;

	/* The lines it spans. */
	first = te_buffer_line_of(&app->buffer, start);
	last = te_buffer_line_of(&app->buffer, end);

	/* The text goes, and the lines become one. */
	te_buffer_delete(&app->buffer, start, end);
	error = te_layout_update(&app->layout, &app->buffer, first, last - first + 1U, 1U);
	if (error != 0)
		te_app_relayout(app);
}

/* Records a change in the undo history; a history without memory says so. */
static int
edit_record(
	struct te_app *app,
	int kind,
	size_t position,
	const char *text,
	size_t length,
	size_t cursor_before,
	size_t anchor_before,
	unsigned group,
	int merge)
{
	struct te_undo_step step;
	int error;

	/* The step. */
	memset(&step, 0, sizeof(step));
	step.kind = kind;
	step.position = position;
	step.text = (char *)text;
	step.length = length;
	step.cursor_before = cursor_before;
	step.anchor_before = anchor_before;
	step.cursor_after = app->cursor;
	step.anchor_after = app->anchor;
	step.group = group;
	step.merge = merge;
	step.time = app->now;

	/* Into the history. */
	error = te_undo_record(&app->undo, &step);
	if (error != 0) {
		te_app_message(app, "Not enough memory: this change can't be undone.");
		return error;
	}

	/* Succeeded: the change can be undone. */
	return 0;
}

/* After a change: the cursor shows, the title may change, and the frame is drawn again. */
static void
edit_changed(
	struct te_app *app)
{
	/* The column up and down keep is measured anew; the cursor shows and does not blink yet. */
	app->goal_valid = 0;
	app->blink_start = app->now;
	app->title_changed = 1;
	app->dirty = 1;
	te_edit_reveal(app);
}

/* Carries out the commands with Control; returns 1 when the key was one. */
static int
edit_command_key(
	struct te_app *app,
	const struct te_event *event)
{
	int shift;

	/* Only with Control. */
	if ((event->modifiers & TE_MOD_CTRL) == 0U)
		return 0;
	shift = 0;
	if ((event->modifiers & TE_MOD_SHIFT) != 0U)
		shift = 1;

	/* The command of the key. */
	switch (event->key) {
	case TE_KEY_A:
		te_app_action(app, TE_ACTION_SELECT_ALL);
		return 1;
	case TE_KEY_C:
		te_app_action(app, TE_ACTION_COPY);
		return 1;
	case TE_KEY_X:
		te_app_action(app, TE_ACTION_CUT);
		return 1;
	case TE_KEY_V:
		te_app_action(app, TE_ACTION_PASTE);
		return 1;
	case TE_KEY_Z:
		if (shift)
			te_app_action(app, TE_ACTION_REDO);
		else
			te_app_action(app, TE_ACTION_UNDO);
		return 1;
	case TE_KEY_Y:
		te_app_action(app, TE_ACTION_REDO);
		return 1;
	case TE_KEY_F:
		te_app_action(app, TE_ACTION_FIND);
		return 1;
	case TE_KEY_G:
		if (shift)
			te_app_action(app, TE_ACTION_FIND_PREVIOUS);
		else
			te_app_action(app, TE_ACTION_FIND_NEXT);
		return 1;
	case TE_KEY_H:
		te_app_action(app, TE_ACTION_REPLACE);
		return 1;
	case TE_KEY_N:
		te_app_action(app, TE_ACTION_NEW);
		return 1;
	case TE_KEY_O:
		te_app_action(app, TE_ACTION_OPEN);
		return 1;
	case TE_KEY_S:
		if (shift)
			te_app_action(app, TE_ACTION_SAVE_AS);
		else
			te_app_action(app, TE_ACTION_SAVE);
		return 1;
	case TE_KEY_W:
		te_app_action(app, TE_ACTION_CLOSE);
		return 1;
	case TE_KEY_Q:
		te_app_action(app, TE_ACTION_QUIT);
		return 1;
	case TE_KEY_EQUAL:
		te_app_action(app, TE_ACTION_BIGGER);
		return 1;
	case TE_KEY_MINUS:
		te_app_action(app, TE_ACTION_SMALLER);
		return 1;
	case TE_KEY_0:
		te_app_action(app, TE_ACTION_ACTUAL_SIZE);
		return 1;
	default:
		break;
	}

	/* Control with a key that is no command. */
	return 0;
}

/* Carries out the keys that move the cursor; returns 1 when the key was one. */
static int
edit_move_key(
	struct te_app *app,
	const struct te_event *event)
{
	int extend;
	int word;
	int move;

	/* Shift extends the selection; Control moves by words and to the document's ends. */
	extend = 0;
	if ((event->modifiers & TE_MOD_SHIFT) != 0U)
		extend = 1;
	word = 0;
	if ((event->modifiers & TE_MOD_CTRL) != 0U)
		word = 1;

	/* The move of the key. */
	switch (event->key) {
	case TE_KEY_LEFT:
		move = EDIT_MOVE_LEFT;
		if (word)
			move = EDIT_MOVE_WORD_LEFT;
		break;
	case TE_KEY_RIGHT:
		move = EDIT_MOVE_RIGHT;
		if (word)
			move = EDIT_MOVE_WORD_RIGHT;
		break;
	case TE_KEY_UP:
		move = EDIT_MOVE_UP;
		break;
	case TE_KEY_DOWN:
		move = EDIT_MOVE_DOWN;
		break;
	case TE_KEY_HOME:
		move = EDIT_MOVE_HOME;
		if (word)
			move = EDIT_MOVE_START;
		break;
	case TE_KEY_END:
		move = EDIT_MOVE_END;
		if (word)
			move = EDIT_MOVE_FINISH;
		break;
	case TE_KEY_PAGE_UP:
		move = EDIT_MOVE_PAGE_UP;
		break;
	case TE_KEY_PAGE_DOWN:
		move = EDIT_MOVE_PAGE_DOWN;
		break;
	default:
		return 0;
	}

	/* Succeeded: the cursor moves. */
	edit_move(app, move, extend);
	return 1;
}

/* Moves the cursor, extending the selection or dropping it. */
static void
edit_move(
	struct te_app *app,
	int move,
	int extend)
{
	size_t start;
	size_t end;
	size_t target;
	size_t row;
	size_t column;
	int vertical;

	/* Left and right without Shift go to the selection's ends first. */
	te_edit_selection(app, &start, &end);
	if (!extend && end > start && (move == EDIT_MOVE_LEFT || move == EDIT_MOVE_RIGHT)) {
		target = start;
		if (move == EDIT_MOVE_RIGHT)
			target = end;
		te_edit_select(app, target, target);
		te_edit_reveal(app);
		return;
	}

	/* Up and down keep the column the cursor had when they began. */
	vertical = 0;
	if (move == EDIT_MOVE_UP ||
	    move == EDIT_MOVE_DOWN ||
	    move == EDIT_MOVE_PAGE_UP ||
	    move == EDIT_MOVE_PAGE_DOWN)
		vertical = 1;
	if (vertical && !app->goal_valid) {
		te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);
		app->goal = column;
	}

	/* Where the move goes. */
	target = edit_target(app, move);

	/* The cursor there, with the selection's anchor kept or brought along. */
	if (extend)
		te_edit_select(app, app->anchor, target);
	else
		te_edit_select(app, target, target);
	app->goal_valid = vertical;
	te_edit_reveal(app);
}

/* Reports where a move takes the cursor. */
static size_t
edit_target(
	struct te_app *app,
	int move)
{
	size_t target;
	int rows;

	/* The move's place. */
	switch (move) {
	case EDIT_MOVE_LEFT:
		target = te_buffer_prev_char(&app->buffer, app->cursor);
		break;
	case EDIT_MOVE_RIGHT:
		target = te_buffer_next_char(&app->buffer, app->cursor);
		break;
	case EDIT_MOVE_UP:
		target = edit_vertical(app, -1);
		break;
	case EDIT_MOVE_DOWN:
		target = edit_vertical(app, 1);
		break;
	case EDIT_MOVE_WORD_LEFT:
		target = edit_word_left(app, app->cursor);
		break;
	case EDIT_MOVE_WORD_RIGHT:
		target = edit_word_right(app, app->cursor);
		break;
	case EDIT_MOVE_HOME:
		target = edit_home(app);
		break;
	case EDIT_MOVE_END:
		target = edit_end(app);
		break;
	case EDIT_MOVE_PAGE_UP:
		/* The view moves up by a page with the cursor. */
		rows = edit_page_rows(app);
		target = edit_vertical(app, -(long)rows);
		app->scroll_y -= (double)rows * (double)app->row_height;
		te_app_clamp(app);
		break;
	case EDIT_MOVE_PAGE_DOWN:
		/* The view moves down by a page with the cursor. */
		rows = edit_page_rows(app);
		target = edit_vertical(app, (long)rows);
		app->scroll_y += (double)rows * (double)app->row_height;
		te_app_clamp(app);
		break;
	case EDIT_MOVE_START:
		target = 0;
		break;
	default:
		target = te_buffer_length(&app->buffer);
		break;
	}

	/* Succeeded: the place. */
	return target;
}

/* Reports the place rows down (or up) from the cursor at the kept column; past the ends, the ends. */
static size_t
edit_vertical(
	struct te_app *app,
	long rows)
{
	size_t row;
	size_t column;
	size_t target;
	size_t length;

	/* The cursor's row. */
	te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);

	/* Above the first row is the start of the text. */
	if (rows < 0 && (size_t)(-rows) > row)
		return 0;

	/* Below the last row is its end. */
	if (rows > 0 && row + (size_t)rows >= app->layout.total) {
		length = te_buffer_length(&app->buffer);
		return length;
	}

	/* Succeeded: the place in that row nearest the kept column. */
	target = te_layout_position(&app->layout, &app->buffer, (size_t)((long)row + rows), app->goal);
	return target;
}

/*
 * Reports where Home goes: on a line's first row, the end of its indent,
 * or its start when the cursor is already there; on a wrapped row, the
 * row's start.
 */
static size_t
edit_home(
	struct te_app *app)
{
	size_t row;
	size_t column;
	size_t start;
	size_t end;
	size_t indent;
	size_t line;
	size_t line_start;
	unsigned char byte;

	/* The row's start. */
	te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);
	te_layout_row_range(&app->layout, &app->buffer, row, &start, &end);
	line = te_buffer_line_of(&app->buffer, app->cursor);
	line_start = te_buffer_line_start(&app->buffer, line);
	if (start != line_start)
		return start;

	/* The end of the line's indent. */
	indent = start;
	for (;;) {
		byte = te_buffer_byte(&app->buffer, indent);
		if (indent >= end || (byte != ' ' && byte != '\t'))
			break;
		indent++;
	}

	/* From the indent's end (or inside it) to the line's start; otherwise to the indent's end. */
	if (app->cursor == indent)
		return start;

	/* Succeeded: the indent's end. */
	return indent;
}

/* Reports where End goes: the row's end (before a wrapped row's last character). */
static size_t
edit_end(
	struct te_app *app)
{
	size_t row;
	size_t column;
	size_t target;

	/* The farthest column of the cursor's row. */
	te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);
	target = te_layout_position(&app->layout, &app->buffer, row, (size_t)-1 / 2U);

	/* Succeeded: the row's end. */
	return target;
}

/* Classifies a character for the word moves. */
static int
edit_class(
	uint32_t codepoint)
{
	/* A line's end, and the blanks. */
	if (codepoint == '\n')
		return EDIT_CLASS_NEWLINE;
	if (codepoint == ' ' || codepoint == '\t' || codepoint == 0x3000U)
		return EDIT_CLASS_BLANK;

	/* The letters, the digits and the underscore of ASCII. */
	if ((codepoint >= 'a' && codepoint <= 'z') ||
	    (codepoint >= 'A' && codepoint <= 'Z') ||
	    (codepoint >= '0' && codepoint <= '9') ||
	    codepoint == '_')
		return EDIT_CLASS_WORD;

	/* The rest of ASCII is marks. */
	if (codepoint < 0x80U)
		return EDIT_CLASS_MARK;

	/* Hiragana (with the prolonged sound mark), and katakana in both widths. */
	if (codepoint >= 0x3040U && codepoint <= 0x309fU)
		return EDIT_CLASS_HIRAGANA;
	if (codepoint >= 0x30a0U && codepoint <= 0x30ffU)
		return EDIT_CLASS_KATAKANA;
	if (codepoint >= 0xff66U && codepoint <= 0xff9fU)
		return EDIT_CLASS_KATAKANA;

	/* The CJK ideographs. */
	if ((codepoint >= 0x3400U && codepoint <= 0x9fffU) ||
	    (codepoint >= 0xf900U && codepoint <= 0xfaffU) ||
	    codepoint >= 0x20000U)
		return EDIT_CLASS_KANJI;

	/* CJK punctuation and the full width marks. */
	if ((codepoint >= 0x3000U && codepoint <= 0x303fU) ||
	    (codepoint >= 0xff00U && codepoint <= 0xff65U))
		return EDIT_CLASS_MARK;

	/* Other letters of other scripts make words. */
	return EDIT_CLASS_WORD;
}

/* Reports the start of the word before a position (blanks before it are passed over). */
static size_t
edit_word_left(
	const struct te_app *app,
	size_t position)
{
	uint32_t codepoint;
	size_t before;
	size_t next;
	int kind;
	int other;

	/* Nothing before the start. */
	if (position == 0U)
		return 0U;

	/* Back over the blanks (a line's end counts as a word of its own). */
	before = te_buffer_prev_char(&app->buffer, position);
	codepoint = te_buffer_char(&app->buffer, before, &next);
	kind = edit_class(codepoint);
	while (kind == EDIT_CLASS_BLANK && before > 0U) {
		position = before;
		before = te_buffer_prev_char(&app->buffer, position);
		codepoint = te_buffer_char(&app->buffer, before, &next);
		kind = edit_class(codepoint);
	}

	/* A line's end or a lone blank at the start is a step of its own. */
	if (kind == EDIT_CLASS_NEWLINE || kind == EDIT_CLASS_BLANK)
		return before;

	/* Back over the word. */
	position = before;
	while (position > 0U) {
		before = te_buffer_prev_char(&app->buffer, position);
		codepoint = te_buffer_char(&app->buffer, before, &next);
		other = edit_class(codepoint);
		if (other != kind)
			break;
		position = before;
	}

	/* Succeeded: the word's start. */
	return position;
}

/* Reports the end of the word after a position (blanks before it are passed over). */
static size_t
edit_word_right(
	const struct te_app *app,
	size_t position)
{
	uint32_t codepoint;
	size_t length;
	size_t next;
	int kind;
	int other;

	/* Nothing after the end. */
	length = te_buffer_length(&app->buffer);
	if (position >= length)
		return length;

	/* Forward over the blanks. */
	codepoint = te_buffer_char(&app->buffer, position, &next);
	kind = edit_class(codepoint);
	while (kind == EDIT_CLASS_BLANK && next < length) {
		position = next;
		codepoint = te_buffer_char(&app->buffer, position, &next);
		kind = edit_class(codepoint);
	}

	/* A line's end or blanks to the end are a step of their own. */
	if (kind == EDIT_CLASS_NEWLINE || kind == EDIT_CLASS_BLANK)
		return next;

	/* Forward over the word. */
	position = next;
	while (position < length) {
		codepoint = te_buffer_char(&app->buffer, position, &next);
		other = edit_class(codepoint);
		if (other != kind)
			break;
		position = next;
	}

	/* Succeeded: the word's end. */
	return position;
}

/* Deletes the selection, or the character (or word) before the cursor. */
static void
edit_backspace(
	struct te_app *app,
	int word)
{
	size_t start;
	size_t end;
	int merge;

	/* A selection goes as a whole. */
	te_edit_selection(app, &start, &end);
	if (end > start) {
		(void)te_edit_delete(app, start, end, TE_MERGE_NONE);
		return;
	}

	/* The character before (joining earlier Backspaces), or the word before. */
	start = te_buffer_prev_char(&app->buffer, app->cursor);
	merge = TE_MERGE_BACKSPACE;
	if (word) {
		start = edit_word_left(app, app->cursor);
		merge = TE_MERGE_NONE;
	}

	/* The deletion. */
	(void)te_edit_delete(app, start, app->cursor, merge);
}

/* Deletes the selection, or the character (or word) after the cursor. */
static void
edit_forward_delete(
	struct te_app *app,
	int word)
{
	size_t start;
	size_t end;
	int merge;

	/* A selection goes as a whole. */
	te_edit_selection(app, &start, &end);
	if (end > start) {
		(void)te_edit_delete(app, start, end, TE_MERGE_NONE);
		return;
	}

	/* The character after (joining earlier Deletes), or the word after. */
	end = te_buffer_next_char(&app->buffer, app->cursor);
	merge = TE_MERGE_FORWARD;
	if (word) {
		end = edit_word_right(app, app->cursor);
		merge = TE_MERGE_NONE;
	}

	/* The deletion. */
	(void)te_edit_delete(app, app->cursor, end, merge);
}

/* Breaks the line at the cursor, starting the new one with the old one's indent. */
static void
edit_newline(
	struct te_app *app)
{
	char *text;
	size_t start;
	size_t end;
	size_t line;
	size_t line_start;
	size_t indent;
	unsigned char byte;
	int error;

	/* The indent of the line the selection starts on (no further than the selection). */
	te_edit_selection(app, &start, &end);
	line = te_buffer_line_of(&app->buffer, start);
	line_start = te_buffer_line_start(&app->buffer, line);
	indent = line_start;
	for (;;) {
		byte = te_buffer_byte(&app->buffer, indent);
		if (indent >= start || (byte != ' ' && byte != '\t'))
			break;
		indent++;
	}

	/* A newline and the indent. */
	text = malloc(indent - line_start + 1U);
	if (text == NULL) {
		te_app_message(app, "Not enough memory.");
		return;
	}

	/* The newline, then the indent copied. */
	text[0] = '\n';
	te_buffer_copy(&app->buffer, line_start, indent, text + 1);

	/* In place of the selection. */
	error = te_edit_insert_text(app, text, indent - line_start + 1U, TE_MERGE_NONE);
	free(text);
	if (error != 0)
		te_app_message(app, "Not enough memory.");
}

/*
 * Tab: a tab character in place of the selection; with a selection over
 * lines (or Shift+Tab), a tab added at the start of each of its lines (or
 * one tab or up to four spaces taken off), as one undo step.
 */
static void
edit_indent(
	struct te_app *app,
	int outdent)
{
	size_t start;
	size_t end;
	size_t first;
	size_t last;
	size_t line;
	size_t at;
	size_t count;
	size_t cursor_before;
	size_t anchor_before;
	size_t last_start;
	unsigned group;
	unsigned char byte;
	const char *removed;

	/* Within one line without Shift, a tab character. */
	te_edit_selection(app, &start, &end);
	first = te_buffer_line_of(&app->buffer, start);
	last = te_buffer_line_of(&app->buffer, end);
	if (!outdent && first == last) {
		(void)te_edit_insert_text(app, "\t", 1U, TE_MERGE_TYPING);
		return;
	}

	/* A selection ending at a line's start leaves that line alone. */
	last_start = te_buffer_line_start(&app->buffer, last);
	if (last > first && end == last_start)
		last--;

	/* Each line from the last (so the earlier places stay put), as one change. */
	cursor_before = app->cursor;
	anchor_before = app->anchor;
	group = te_undo_group(&app->undo);
	for (line = last + 1U; line > first; line--) {
		at = te_buffer_line_start(&app->buffer, line - 1U);

		/* Indent: a tab at the line's start. */
		if (!outdent) {
			(void)edit_insert_raw(app, at, "\t", 1U);
			(void)edit_record(app, TE_UNDO_INSERT, at, "\t", 1U, cursor_before, anchor_before, group, TE_MERGE_NONE);
			continue;
		}

		/* Outdent: a tab, or up to four spaces, from the line's start. */
		count = 0;
		removed = "    ";
		byte = te_buffer_byte(&app->buffer, at);
		if (byte == '\t') {
			count = 1;
			removed = "\t";
		} else {
			while (count < 4U && byte == ' ') {
				count++;
				byte = te_buffer_byte(&app->buffer, at + count);
			}
		}

		/* A line without an indent keeps its text. */
		if (count == 0U)
			continue;
		edit_delete_raw(app, at, at + count);
		(void)edit_record(app, TE_UNDO_DELETE, at, removed, count, cursor_before, anchor_before, group, TE_MERGE_NONE);
	}

	/* The whole lines stay selected. */
	start = te_buffer_line_start(&app->buffer, first);
	end = te_buffer_line_end(&app->buffer, last);
	te_edit_select(app, start, end);
	edit_changed(app);
}

/* Types a character in place of the selection; returns 1. */
static int
edit_type(
	struct te_app *app,
	uint32_t codepoint)
{
	char bytes[4];
	size_t length;
	int error;

	/* The character in UTF-8, joining the typing before. */
	length = edit_encode(codepoint, bytes);
	error = te_edit_insert_text(app, bytes, length, TE_MERGE_TYPING);
	if (error != 0)
		te_app_message(app, "Not enough memory.");

	/* The key was the text's. */
	return 1;
}

/* Encodes a character in UTF-8; returns its length. */
static size_t
edit_encode(
	uint32_t codepoint,
	char *out)
{
	/* One byte for ASCII. */
	if (codepoint < 0x80U) {
		out[0] = (char)codepoint;
		return 1U;
	}

	/* Two bytes up to U+07FF. */
	if (codepoint < 0x800U) {
		out[0] = (char)(0xc0U | (codepoint >> 6));
		out[1] = (char)(0x80U | (codepoint & 0x3fU));
		return 2U;
	}

	/* Three bytes up to U+FFFF. */
	if (codepoint < 0x10000U) {
		out[0] = (char)(0xe0U | (codepoint >> 12));
		out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (codepoint & 0x3fU));
		return 3U;
	}

	/* Four bytes beyond. */
	out[0] = (char)(0xf0U | (codepoint >> 18));
	out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (codepoint & 0x3fU));
	return 4U;
}

/* Makes each CR LF of a pasted text LF, in place; returns the new length. */
static size_t
edit_normalize(
	char *text,
	size_t length)
{
	size_t from;
	size_t to;

	/* Each byte but the CR of CR LF. */
	to = 0;
	for (from = 0; from < length; from++) {
		if (text[from] == '\r' && from + 1U < length && text[from + 1U] == '\n')
			continue;
		text[to] = text[from];
		to++;
	}

	/* Succeeded: the new length. */
	return to;
}

/* Reports how many rows a page moves: the rows in view, less one. */
static int
edit_page_rows(
	const struct te_app *app)
{
	struct te_rect text;
	int rows;

	/* The rows that fit in the text's height. */
	te_app_text_rect(app, &text);
	rows = text.height / app->row_height - 1;
	if (rows < 1)
		rows = 1;

	/* Succeeded: the rows. */
	return rows;
}
