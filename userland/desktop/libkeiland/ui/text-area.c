/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text area of the library (KL_VERSION 47, ws090-p022): text of
 * several lines in the one-line field's look (field.c), for a message's
 * body or a note.  The lines wrap at the area's width (at a space when
 * the line has one) and at each newline.  The keys: characters of the US
 * layout, Enter (a newline), Left and Right, Up and Down (keeping the
 * place across), Home and End (of the line), with Shift the selection,
 * Backspace and Delete, Ctrl+A; Esc cancels.  A click or a tap takes the
 * keyboard and puts the caret at the character nearest the point.
 *
 * It takes the text an input method or the on-screen keyboard sends
 * (kl_ui_text): a commit goes in place of the selection (its newlines
 * kept), the bytes around the caret are deleted, and the text being
 * composed shows underlined at the caret until it is committed.  The area
 * scrolls down just enough to keep the caret in sight.
 *
 * KL_VERSION 67 (ws177-p013): as a field, Ctrl+Z and Ctrl+Shift+Z or
 * Ctrl+Y take a change back and do it again, Ctrl+C, Ctrl+X and Ctrl+V
 * copy, cut and paste through the window's clipboard, and Ctrl+Left and
 * Ctrl+Right move a word.
 *
 * KL_VERSION 74 (ws190-p002): a finger's double tap selects the word there
 * with the fingers' selection, as in a field (field.c): handles at the
 * ends, which a finger drags across lines (the area scrolls itself near
 * its top and bottom), and the bar of editing buttons.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* The text's size, a line's height and the margins inside the area. */
#define AREA_TEXT		14U
#define AREA_LINE		20
#define AREA_SIDE		12
#define AREA_TOP		8

/* The most lines laid out (the rest of a longer text is not shown). */
#define AREA_LINES		1024U

/*
 * The text as shown (the text with the one being composed at the caret)
 * laid out in lines: each line's first byte and its end (before its
 * newline, or where it wrapped).
 */
struct area_layout {
	char shown[KL_TEXT_AREA_MAX + KL_WINDOW_TEXT_MAX];
	size_t length;
	size_t starts[AREA_LINES];
	size_t ends[AREA_LINES];
	size_t count;
};

static int area_wants(uint32_t code, unsigned modifiers);
static unsigned area_input(struct kl_text_area *area, const struct kl_style *style, int width, const struct keiui_input *input);
static unsigned area_take(struct kl_ui *ui, uint32_t id, struct kl_text_area *area, const struct kl_style *style, int width, const struct keiui_input *input);
static unsigned area_edit(struct kl_ui *ui, uint32_t id, struct kl_text_area *area, unsigned command, unsigned modifiers);
static unsigned area_key(struct kl_text_area *area, const struct kl_style *style, int width, uint32_t code, unsigned modifiers);
static void area_vertical(struct kl_text_area *area, const struct kl_style *style, int width, int lines, unsigned shift);
static void area_erase(struct kl_text_area *area);
static size_t area_prev(const char *text, size_t at);
static size_t area_next(const char *text, size_t length, size_t at);
static void area_insert(struct kl_text_area *area, const char *text, size_t length);
static void area_delete_around(struct kl_text_area *area, size_t before, size_t after);
static void area_lay_out(struct area_layout *layout, const struct kl_style *style, const char *text, size_t length, int width);
static size_t area_line_of(const struct area_layout *layout, size_t offset);
static int area_x_of(const struct area_layout *layout, const struct kl_style *style, size_t line, size_t offset);
static size_t area_at_x(const struct area_layout *layout, const struct kl_style *style, size_t line, int x);
static void area_select_click(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_text_area *area, unsigned state, const struct area_layout *layout, int top);
static void area_select_after(struct kl_ui *ui, uint32_t id, struct kl_text_area *area, const struct keiui_input *input);
static int area_select_check(struct kl_ui *ui, uint32_t id, struct kl_text_area *area);
static void area_select_drawn(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, const struct kl_text_area *area, const struct area_layout *layout);
static size_t area_view_position(void *data, double x, double y);
static void area_view_caret(void *data, size_t position, struct kl_rect *rect);
static void area_view_word(void *data, size_t position, size_t *start, size_t *end);

/* The answers of a text area's view to the fingers' selection, from its copy and lines in kl_ui (ws190-p002). */
static const struct kl_text_view area_view = {
	area_view_position,
	area_view_caret,
	area_view_word
};

/*
 * Sets a text area's text, with the caret at its end, nothing selected
 * and the area scrolled to its top.
 */
void
kl_text_area_set(
	struct kl_text_area *area,
	const char *text)
{
	/* The text, cut to the area at a character's start. */
	strncpy(area->text, text, sizeof(area->text) - 1U);
	area->text[sizeof(area->text) - 1U] = '\0';
	area->length = strlen(area->text);
	while (area->length > 0U && ((unsigned char)area->text[area->length] & 0xc0U) == 0x80U)
		area->length--;
	area->text[area->length] = '\0';

	/* The caret at the end, the top in sight. */
	area->caret = area->length;
	area->anchor = area->length;
	area->scroll = 0;
	area->goal_x = -1;
}

/*
 * Draws a text area in a rectangle and takes its input; reports what
 * happened (KL_FIELD_CHANGED, KL_FIELD_CANCELLED).  placeholder (may be
 * NULL) shows while it is empty.
 */
unsigned
kl_text_area(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_text_area *area,
	const char *placeholder)
{
	static struct area_layout layout;
	const struct kl_theme *theme;
	const char *preedit;
	struct keiui_input input;
	struct kl_rect inside;
	struct kl_rect caret;
	char text[KL_TEXT_AREA_MAX + KL_WINDOW_TEXT_MAX];
	unsigned changes;
	unsigned state;
	size_t preedit_length;
	size_t caret_offset;
	size_t start;
	size_t end;
	size_t line;
	size_t from;
	size_t to;
	size_t caret_line;
	int32_t preedit_begin;
	int32_t preedit_end;
	int width;
	int top;
	int y;
	int baseline;
	int left_x;
	int right_x;
	int caret_x;
	int caret_shown;
	int taken;
	int focused;
	int holding;
	int owned;

	/* The record: it takes the keyboard. */
	theme = style->theme;
	state = keiui_ui_widget(ui, id, 0U, rect, KEIUI_FOCUSABLE);
	focused = 0;
	if ((state & KL_HIT_FOCUSED) != 0U)
		focused = 1;

	/* The width the lines wrap at, and where the first line stands (scrolled). */
	width = rect->width - 2 * AREA_SIDE;
	if (width < 1)
		width = 1;
	top = rect->y + AREA_TOP - area->scroll;

	/* A click or a tap puts the caret at the point (twice: the whole text selected; a finger's: the fingers' selection, ws190-p002). */
	if ((state & KL_HIT_CLICKED) != 0U) {
		area_lay_out(&layout, style, area->text, area->length, width);
		area_select_click(ui, style, id, rect, area, state, &layout, top);
	}

	/* The keys while it has the keyboard, and the text an input method sent for it, in the order they came. */
	changes = 0;
	for (;;) {
		taken = keiui_ui_take_input(ui, id, 0U, area_wants, &input);
		if (!taken)
			break;
		changes |= area_take(ui, id, area, style, width, &input);
		area_select_after(ui, id, area, &input);
	}

	/* The fingers' selection's ends, as the area shows them (another text the program set ends it). */
	holding = area_select_check(ui, id, area);

	/* The text an input method is composing for it; it ends the fingers' selection. */
	preedit_begin = -1;
	preedit_end = -1;
	preedit = keiui_ui_preedit(ui, id, 0U, focused, &preedit_begin, &preedit_end);
	if (preedit != NULL) {
		owned = keiui_ui_select_owned(ui, id, 0U);
		if (owned)
			keiui_ui_select_end(ui);
		holding = 0;
	}

	/*
	 * The text as shown: the one being composed in it at the caret (no
	 * selection shows meanwhile, since the commit replaces it), and the
	 * caret's offset in it: at the composed text's cursor, hidden when the
	 * input method hides that cursor.
	 */
	memcpy(text, area->text, area->length + 1U);
	start = area->anchor;
	end = area->caret;
	if (start > end) {
		start = area->caret;
		end = area->anchor;
	}

	/* Initializes the preedit composition. */
	preedit_length = 0;
	caret_offset = area->caret;
	caret_shown = focused;
	if (preedit != NULL) {
		preedit_length = strlen(preedit);
		memmove(text + area->caret + preedit_length, text + area->caret, area->length - area->caret + 1U);
		memcpy(text + area->caret, preedit, preedit_length);
		start = area->caret;
		end = area->caret;
		if (preedit_begin < 0 || (size_t)preedit_begin > preedit_length) {
			caret_offset += preedit_length;
			caret_shown = 0;
		} else {
			caret_offset += (size_t)preedit_begin;
		}
	}

	/* Lays out the text with preedit composition for measurement. */
	area_lay_out(&layout, style, text, area->length + preedit_length, width);

	/* The area scrolls down or up just enough to keep the caret's line inside (a finger dragging a handle scrolls it itself). */
	caret_line = area_line_of(&layout, caret_offset);
	if (!holding && (int)caret_line * AREA_LINE + AREA_LINE + 2 * AREA_TOP - area->scroll > rect->height)
		area->scroll = (int)caret_line * AREA_LINE + AREA_LINE + 2 * AREA_TOP - rect->height;
	if (!holding && (int)caret_line * AREA_LINE < area->scroll)
		area->scroll = (int)caret_line * AREA_LINE;
	if (area->scroll < 0)
		area->scroll = 0;
	top = rect->y + AREA_TOP - area->scroll;

	/* The ground: white, with the accent's edge while it has the keyboard. */
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, theme->panel);
	if (focused)
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, 1.5f, theme->accent);
	else
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, 1.0f, theme->control_edge);

	/* Inside the area: the placeholder while it is empty. */
	inside.x = rect->x + AREA_SIDE / 2;
	inside.y = rect->y + 2;
	inside.width = rect->width - AREA_SIDE;
	inside.height = rect->height - 4;
	kl_canvas_clip_push(style->canvas, &inside);
	if (area->length == 0U && preedit == NULL && placeholder != NULL) {
		baseline = kl_text_center(AREA_TEXT, top, AREA_LINE);
		(void)kl_text_draw(style->text, style->canvas, rect->x + AREA_SIDE, baseline, placeholder, strlen(placeholder), AREA_TEXT, 0, theme->text_faint);
	}

	/* Each line in sight: the selection's part of it, its text, and the text being composed underlined. */
	for (line = 0; line < layout.count; line++) {
		y = top + (int)line * AREA_LINE;
		if (y + AREA_LINE < rect->y || y > rect->y + rect->height)
			continue;
		baseline = kl_text_center(AREA_TEXT, y, AREA_LINE);

		/* The selection, while it has the keyboard. */
		from = start;
		to = end;
		if (from < layout.starts[line])
			from = layout.starts[line];
		if (to > layout.ends[line])
			to = layout.ends[line];
		if (focused && from < to) {
			left_x = area_x_of(&layout, style, line, from);
			right_x = area_x_of(&layout, style, line, to);
			kl_canvas_round(style->canvas, (float)(rect->x + AREA_SIDE + left_x), (float)y + 1.0f, (float)(right_x - left_x), (float)AREA_LINE - 2.0f, 2.0f, theme->selection);
		}

		/* Its text. */
		(void)kl_text_draw(style->text, style->canvas, rect->x + AREA_SIDE, baseline, layout.shown + layout.starts[line], layout.ends[line] - layout.starts[line], AREA_TEXT, 0, theme->text);

		/* The part of the text being composed on it. */
		if (preedit == NULL)
			continue;
		from = area->caret;
		to = area->caret + preedit_length;
		if (from < layout.starts[line])
			from = layout.starts[line];
		if (to > layout.ends[line])
			to = layout.ends[line];
		if (from >= to)
			continue;
		left_x = area_x_of(&layout, style, line, from);
		right_x = area_x_of(&layout, style, line, to);
		kl_canvas_line(style->canvas, (float)(rect->x + AREA_SIDE + left_x), (float)baseline + 3.5f, (float)(rect->x + AREA_SIDE + right_x), (float)baseline + 3.5f, 1.0f, theme->text);
	}

	/* The caret, while it has the keyboard and the input method does not hide it. */
	caret_x = area_x_of(&layout, style, caret_line, caret_offset);
	y = top + (int)caret_line * AREA_LINE;
	if (caret_shown)
		kl_canvas_line(style->canvas, (float)(rect->x + AREA_SIDE + caret_x) + 0.75f, (float)y + 2.0f, (float)(rect->x + AREA_SIDE + caret_x) + 0.75f, (float)(y + AREA_LINE) - 2.0f, 1.5f, theme->accent);
	kl_canvas_clip_pop(style->canvas);

	/* With the keyboard it takes an input method's text at its caret (kl_ui_text_wanted). */
	if (focused) {
		caret.x = rect->x + AREA_SIDE + caret_x;
		caret.y = y + 2;
		caret.width = 2;
		caret.height = AREA_LINE - 4;
		keiui_ui_text_caret(ui, &caret);
	}

	/* The fingers' selection's copy of the area and its lines, drawn in this frame (no text is composed in it then). */
	if (preedit == NULL)
		area_select_drawn(ui, style, id, rect, area, &layout);

	/* Reports what happened. */
	return changes;
}

/* Tells whether a text area takes a key: its characters, its editing keys, Enter and Esc (with Control, A and the editing commands). */
static int
area_wants(
	uint32_t code,
	unsigned modifiers)
{
	uint32_t character;
	unsigned command;

	/* Control's keys are A (select all) and the editing commands (ws177-p013); Alt and Super are commands. */
	if ((modifiers & (KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return 0;
	if ((modifiers & KL_MOD_CTRL) != 0U) {
		if (code == 30U)
			return 1;
		command = keiui_edit_command(code, modifiers);
		if (command != KEIUI_EDIT_NONE)
			return 1;
		return 0;
	}

	/* The editing keys. */
	switch (code) {
	case KL_KEY_LEFT:
	case KL_KEY_RIGHT:
	case KL_KEY_UP:
	case KL_KEY_DOWN:
	case KL_KEY_HOME:
	case KL_KEY_END:
	case KL_KEY_BACKSPACE:
	case KL_KEY_DELETE:
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
	case KL_KEY_ESC:
		return 1;
	default:
		break;
	}

	/* A key that types a character. */
	character = kl_key_character(code, modifiers);
	if (character != 0U)
		return 1;
	return 0;
}

/*
 * Takes one input in a text area: an editing command of the window's
 * input (area_edit), or a key, a text or a deletion, each change recorded
 * in the history.  Reports what happened (KL_FIELD_* bits).
 */
static unsigned
area_take(
	struct kl_ui *ui,
	uint32_t id,
	struct kl_text_area *area,
	const struct kl_style *style,
	int width,
	const struct keiui_input *input)
{
	static char before[KL_TEXT_AREA_MAX];
	size_t before_length;
	size_t caret;
	size_t anchor;
	unsigned command;
	unsigned changes;

	/* An editing command (ws177-p013). */
	command = KEIUI_EDIT_NONE;
	if (input->kind == KEIUI_INPUT_KEY)
		command = keiui_edit_command(input->code, input->modifiers);
	if (command != KEIUI_EDIT_NONE) {
		area->goal_x = -1;
		changes = area_edit(ui, id, area, command, input->modifiers);
		return changes;
	}

	/* The text before the input, for the history. */
	memcpy(before, area->text, area->length + 1U);
	before_length = area->length;
	caret = area->caret;
	anchor = area->anchor;

	/* The input; a change goes in the history. */
	changes = area_input(area, style, width, input);
	if ((changes & KL_FIELD_CHANGED) != 0U)
		keiui_edit_record(ui, id, before, before_length, caret, anchor, area->text, area->length);

	/* Reports what the input did. */
	return changes;
}

/*
 * Carries out an editing command in a text area (ws177-p013): undo and
 * redo, copy, cut and paste, a word left or right.  Reports what happened
 * (KL_FIELD_* bits).
 */
static unsigned
area_edit(
	struct kl_ui *ui,
	uint32_t id,
	struct kl_text_area *area,
	unsigned command,
	unsigned modifiers)
{
	static char before[KL_TEXT_AREA_MAX];
	static char pasted[KL_TEXT_AREA_MAX];
	size_t before_length;
	size_t caret;
	size_t anchor;
	size_t start;
	size_t end;
	size_t length;
	size_t at;
	size_t piece;
	int forward;
	int redo;
	int done;

	/* The selection's ends in order. */
	start = area->anchor;
	end = area->caret;
	if (start > end) {
		start = area->caret;
		end = area->anchor;
	}

	/* What the area had, for the history of a cut or a paste. */
	memcpy(before, area->text, area->length + 1U);
	before_length = area->length;
	caret = area->caret;
	anchor = area->anchor;

	/* Each command. */
	switch (command) {
	case KEIUI_EDIT_UNDO:
	case KEIUI_EDIT_REDO:
		/* The history's change. */
		redo = 0;
		if (command == KEIUI_EDIT_REDO)
			redo = 1;
		done = keiui_edit_undo(ui, id, redo, area->text, &area->length, sizeof(area->text), &area->caret, &area->anchor);
		if (!done)
			return 0;
		return KL_FIELD_CHANGED;
	case KEIUI_EDIT_COPY:
		/* The selection. */
		if (end > start)
			keiui_edit_copy(ui, area->text + start, end - start);
		return 0;
	case KEIUI_EDIT_CUT:
		/* The selection copied and erased. */
		if (end == start)
			return 0;
		keiui_edit_copy(ui, area->text + start, end - start);
		area_erase(area);
		break;
	case KEIUI_EDIT_PASTE:
		/* The clipboard's text in place of the selection, in pieces the insertion takes, as much as fits. */
		length = keiui_edit_paste(ui, pasted, sizeof(pasted));
		if (length == 0U)
			return 0;
		area_erase(area);
		for (at = 0; at < length; at += piece) {
			/* A piece the insertion takes whole, cut at a character's start. */
			piece = length - at;
			if (piece > KL_WINDOW_TEXT_MAX - 1U)
				piece = KL_WINDOW_TEXT_MAX - 1U;
			while (piece > 1U &&
			       at + piece < length &&
			       ((unsigned char)pasted[at + piece] & 0xc0U) == 0x80U)
				piece--;
			area_insert(area, pasted + at, piece);
		}

		/* The paste goes in the history below. */
		break;
	case KEIUI_EDIT_WORD_LEFT:
	case KEIUI_EDIT_WORD_RIGHT:
		/* The caret to the word's start or end; Shift keeps the selection's other end. */
		forward = 0;
		if (command == KEIUI_EDIT_WORD_RIGHT)
			forward = 1;
		area->caret = keiui_edit_word(area->text, area->length, area->caret, forward);
		if ((modifiers & KL_MOD_SHIFT) == 0U)
			area->anchor = area->caret;
		return 0;
	default:
		return 0;
	}

	/* A cut or a paste goes in the history. */
	keiui_edit_record(ui, id, before, before_length, caret, anchor, area->text, area->length);

	/* Succeeded: the text changed. */
	return KL_FIELD_CHANGED;
}

/* Carries out one input in a text area: a key, a text to commit, or bytes to delete; reports what happened (KL_FIELD_* bits). */
static unsigned
area_input(
	struct kl_text_area *area,
	const struct kl_style *style,
	int width,
	const struct keiui_input *input)
{
	unsigned changes;

	/* Each kind of input. */
	switch (input->kind) {
	case KEIUI_INPUT_KEY:
		changes = area_key(area, style, width, input->code, input->modifiers);
		break;
	case KEIUI_INPUT_COMMIT:
		area_insert(area, input->text, strlen(input->text));
		area->goal_x = -1;
		changes = KL_FIELD_CHANGED;
		break;
	case KEIUI_INPUT_DELETE:
		area_delete_around(area, (size_t)input->before, (size_t)input->after);
		area->goal_x = -1;
		changes = KL_FIELD_CHANGED;
		break;
	default:
		changes = 0;
		break;
	}

	/* Reports what the input did. */
	return changes;
}

/* Carries out one key in a text area; reports what happened (KL_FIELD_* bits). */
static unsigned
area_key(
	struct kl_text_area *area,
	const struct kl_style *style,
	int width,
	uint32_t code,
	unsigned modifiers)
{
	static struct area_layout layout;
	uint32_t character;
	unsigned shift;
	size_t line;
	size_t at;
	char byte;

	/* Up and Down keep the place across; every other key forgets it. */
	shift = modifiers & KL_MOD_SHIFT;

	/* Scrolls up one line, preserving the column. */
	if (code == KL_KEY_UP) {
		area_vertical(area, style, width, -1, shift);
		return 0;
	}

	/* Scrolls down one line, preserving the column. */
	if (code == KL_KEY_DOWN) {
		area_vertical(area, style, width, 1, shift);
		return 0;
	}

	/* Other keys forget the goal column. */
	area->goal_x = -1;

	/* The keys that move along, erase, break the line and cancel. */
	switch (code) {
	case KL_KEY_LEFT:
		at = area_prev(area->text, area->caret);
		if (area->caret != area->anchor &&
		    shift == 0U &&
		    area->anchor < area->caret)
			at = area->anchor;
		area->caret = at;
		if (shift == 0U)
			area->anchor = at;
		return 0;
	case KL_KEY_RIGHT:
		at = area_next(area->text, area->length, area->caret);
		if (area->caret != area->anchor &&
		    shift == 0U &&
		    area->anchor > area->caret)
			at = area->anchor;
		area->caret = at;
		if (shift == 0U)
			area->anchor = at;
		return 0;
	case KL_KEY_HOME:
	case KL_KEY_END:
		/* The start or the end of the caret's line as laid out. */
		area_lay_out(&layout, style, area->text, area->length, width);
		line = area_line_of(&layout, area->caret);
		area->caret = layout.starts[line];
		if (code == KL_KEY_END)
			area->caret = layout.ends[line];
		if (shift == 0U)
			area->anchor = area->caret;
		return 0;
	case KL_KEY_BACKSPACE:
		if (area->caret == area->anchor)
			area->anchor = area_prev(area->text, area->caret);
		area_erase(area);
		return KL_FIELD_CHANGED;
	case KL_KEY_DELETE:
		if (area->caret == area->anchor)
			area->anchor = area_next(area->text, area->length, area->caret);
		area_erase(area);
		return KL_FIELD_CHANGED;
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
		area_insert(area, "\n", 1U);
		return KL_FIELD_CHANGED;
	case KL_KEY_ESC:
		return KL_FIELD_CANCELLED;
	default:
		break;
	}

	/* Ctrl+A selects the whole text. */
	if ((modifiers & KL_MOD_CTRL) != 0U) {
		area->anchor = 0;
		area->caret = area->length;
		return 0;
	}

	/* A character replaces the selection. */
	character = kl_key_character(code, modifiers);
	if (character == 0U)
		return 0;
	byte = (char)character;
	area_insert(area, &byte, 1U);

	/* Succeeded: the text changed. */
	return KL_FIELD_CHANGED;
}

/* Moves the caret a line up (-1) or down (1), to the place across nearest where Up or Down began. */
static void
area_vertical(
	struct kl_text_area *area,
	const struct kl_style *style,
	int width,
	int lines,
	unsigned shift)
{
	static struct area_layout layout;
	size_t line;

	/* The caret's line, and the place across kept from the first Up or Down. */
	area_lay_out(&layout, style, area->text, area->length, width);
	line = area_line_of(&layout, area->caret);
	if (area->goal_x < 0)
		area->goal_x = area_x_of(&layout, style, line, area->caret);

	/* Above the first line: its start; below the last: its end. */
	if (lines < 0 && line == 0U) {
		area->caret = 0;
	} else if (lines > 0 && line + 1U >= layout.count) {
		area->caret = area->length;
	} else {
		if (lines < 0)
			line--;
		else
			line++;
		area->caret = area_at_x(&layout, style, line, area->goal_x);
	}

	/* Without Shift nothing stays selected. */
	if (shift == 0U)
		area->anchor = area->caret;
}

/* Erases the selection (nothing when the caret and the anchor meet). */
static void
area_erase(
	struct kl_text_area *area)
{
	size_t start;
	size_t end;

	/* The selection's ends in order. */
	start = area->anchor;
	end = area->caret;
	if (start > end) {
		start = area->caret;
		end = area->anchor;
	}

	/* The bytes after it close up. */
	memmove(area->text + start, area->text + end, area->length - end + 1U);
	area->length -= end - start;
	area->caret = start;
	area->anchor = start;
}

/* Reports the start of the character before an offset. */
static size_t
area_prev(
	const char *text,
	size_t at)
{
	/* Nothing before the start. */
	if (at == 0U)
		return 0U;

	/* Back over the continuation bytes. */
	at--;
	while (at > 0U && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at--;

	/* Reports the character's start. */
	return at;
}

/* Reports the offset after the character at an offset. */
static size_t
area_next(
	const char *text,
	size_t length,
	size_t at)
{
	/* Nothing after the end. */
	if (at >= length)
		return length;

	/* On over the continuation bytes. */
	at++;
	while (at < length && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at++;

	/* Reports the next character's start. */
	return at;
}

/*
 * Puts text in place of the selection, without control characters but
 * its newlines, as much of it as the area has room for; the caret follows
 * it.
 */
static void
area_insert(
	struct kl_text_area *area,
	const char *text,
	size_t length)
{
	char clean[KL_WINDOW_TEXT_MAX];
	size_t kept;
	size_t room;
	size_t at;
	unsigned char byte;

	/* The text without control characters (a newline stays). */
	kept = 0;
	for (at = 0; at < length && kept + 1U < sizeof(clean); at++) {
		byte = (unsigned char)text[at];
		if (byte == 0x7fU)
			continue;
		if (byte < 0x20U && byte != '\n')
			continue;
		clean[kept] = (char)byte;
		kept++;
	}

	/* The selection goes first. */
	area_erase(area);

	/* As much of the text as there is room for, cut at a character's start. */
	room = sizeof(area->text) - 1U - area->length;
	if (kept > room) {
		kept = room;
		while (kept > 0U && ((unsigned char)clean[kept] & 0xc0U) == 0x80U)
			kept--;
	}

	/* The bytes after the caret move on, and the text goes in before them. */
	memmove(area->text + area->caret + kept, area->text + area->caret, area->length - area->caret + 1U);
	memcpy(area->text + area->caret, clean, kept);
	area->length += kept;
	area->caret += kept;
	area->anchor = area->caret;
}

/* Deletes bytes before and after the selection (whole characters), as an input method asks; the selection stays. */
static void
area_delete_around(
	struct kl_text_area *area,
	size_t before,
	size_t after)
{
	size_t start;
	size_t end;
	size_t low;
	size_t high;

	/* The selection's ends in order: the bytes are counted out from them. */
	start = area->anchor;
	end = area->caret;
	if (start > end) {
		start = area->caret;
		end = area->anchor;
	}

	/* The first byte deleted before it, back to a character's start. */
	low = 0;
	if (before < start)
		low = start - before;
	while (low > 0U && ((unsigned char)area->text[low] & 0xc0U) == 0x80U)
		low--;

	/* The first byte kept after it, on to a character's start. */
	high = area->length;
	if (after < area->length - end)
		high = end + after;
	while (high < area->length && ((unsigned char)area->text[high] & 0xc0U) == 0x80U)
		high++;

	/* The bytes after the selection close up first, then those before it. */
	memmove(area->text + end, area->text + high, area->length - high + 1U);
	area->length -= high - end;
	memmove(area->text + low, area->text + start, area->length - start + 1U);
	area->length -= start - low;

	/* The selection moves back by what went before it. */
	area->caret -= start - low;
	area->anchor -= start - low;
}

/*
 * Lays out a text in lines of a width: a line ends at a newline, or where
 * the next character would pass the width (after the line's last space
 * when it has one, else before that character).  There is always one line.
 */
static void
area_lay_out(
	struct area_layout *layout,
	const struct kl_style *style,
	const char *text,
	size_t length,
	int width)
{
	size_t start;
	size_t at;
	size_t next;
	size_t space;
	int x;
	int advance;

	/* The text as laid out, and no lines yet. */
	memcpy(layout->shown, text, length);
	layout->shown[length] = '\0';
	layout->length = length;
	layout->count = 0;

	/* Each line from its first byte, until the text ends or the lines run out. */
	start = 0;
	for (;;) {
		x = 0;
		space = 0;
		at = start;
		for (;;) {
			/* The text's end or a newline ends the line. */
			if (at >= length || text[at] == '\n')
				break;

			/* The character's width; one past the width wraps (never the line's first). */
			next = area_next(text, length, at);
			advance = kl_text_width(style->text, text + at, next - at, AREA_TEXT, 0);
			if (x + advance > width && at > start)
				break;
			x += advance;
			if (text[at] == ' ')
				space = next;
			at = next;
		}

		/* A wrapped line ends after its last space, when it has one. */
		if (at < length &&
		    text[at] != '\n' &&
		    space > start)
			at = space;

		/* The line. */
		layout->starts[layout->count] = start;
		layout->ends[layout->count] = at;
		layout->count++;

		/* The next line begins after a newline, or where this one wrapped. */
		if (at >= length || layout->count == AREA_LINES)
			break;
		if (text[at] == '\n')
			at++;
		start = at;
	}
}

/* Reports the line an offset is on (an offset at a wrapped line's end is on the next line's start). */
static size_t
area_line_of(
	const struct area_layout *layout,
	size_t offset)
{
	size_t line;

	/* The last line that starts at or before it. */
	line = 0;
	while (line + 1U < layout->count && layout->starts[line + 1U] <= offset)
		line++;

	/* Reports the line. */
	return line;
}

/* Reports where an offset on a line stands across, in pixels from the line's start. */
static int
area_x_of(
	const struct area_layout *layout,
	const struct kl_style *style,
	size_t line,
	size_t offset)
{
	size_t start;
	int x;

	/* Within the line. */
	start = layout->starts[line];
	if (offset < start)
		offset = start;
	if (offset > layout->ends[line])
		offset = layout->ends[line];

	/* The width of the line's text before it. */
	x = kl_text_width(style->text, layout->shown + start, offset - start, AREA_TEXT, 0);
	return x;
}

/* Reports the byte offset of the character boundary on a line nearest a place across (pixels from the line's start). */
static size_t
area_at_x(
	const struct area_layout *layout,
	const struct kl_style *style,
	size_t line,
	int x)
{
	size_t best;
	size_t at;
	int width;
	int distance;
	int nearest;

	/* Each boundary of the line, the nearest one kept. */
	best = layout->starts[line];
	nearest = -1;
	at = layout->starts[line];
	for (;;) {
		width = area_x_of(layout, style, line, at);
		distance = width - x;
		if (distance < 0)
			distance = -distance;
		if (nearest < 0 || distance < nearest) {
			nearest = distance;
			best = at;
		}

		/* Stops at the line's end, else the next character. */
		if (at >= layout->ends[line])
			break;
		at = area_next(layout->shown, layout->length, at);
	}

	/* Reports the nearest boundary. */
	return best;
}

/*
 * Carries out a click or a tap on a text area: the caret at the point (a
 * double click: the whole text), or for a finger (ws190-p002) the word
 * there with the fingers' selection on a double tap, the bar shown or
 * hidden by a tap within the selection, and the selection's end by a tap
 * elsewhere or a click.  layout is the area's lines, top where its first
 * line stands in the window.
 */
static void
area_select_click(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_text_area *area,
	unsigned state,
	const struct area_layout *layout,
	int top)
{
	struct keiui_select *select;
	double pointer_x;
	double pointer_y;
	size_t position;
	size_t line;
	size_t start;
	size_t end;
	int owned;
	int enabled;

	/* Where the click is in the text: its line, and the place across it. */
	kl_ui_pointer(ui, &pointer_x, &pointer_y);
	line = 0;
	if ((int)pointer_y > top)
		line = (size_t)(((int)pointer_y - top) / AREA_LINE);
	if (line >= layout->count)
		line = layout->count - 1U;
	position = area_at_x(layout, style, line, (int)pointer_x - rect->x - AREA_SIDE);
	select = keiui_ui_select(ui);
	owned = keiui_ui_select_owned(ui, id, 0U);
	enabled = keiui_ui_select_enabled(ui);

	/* Room for the lines the fingers' selection keeps (made once a window's input). */
	if (enabled && select->layout == NULL) {
		select->layout = malloc(sizeof(struct area_layout));
		if (select->layout == NULL)
			enabled = 0;
	}

	/* A finger's double tap: the word there, in the fingers' selection. */
	if ((state & KL_HIT_TOUCHED) != 0U &&
	    (state & KL_HIT_DOUBLE) != 0U &&
	    enabled) {
		select->area = *area;
		select->style = *style;
		select->rect = *rect;
		memcpy(select->layout, layout, sizeof(*layout));
		keiui_ui_select_begin(ui, id, 0U, KEIUI_SELECT_AREA, &area_view, area, &keiui_text_bar_calls);
		kl_text_touch_tap(&select->touch, pointer_x - (double)(rect->x + AREA_SIDE), pointer_y - (double)top, 1);
		(void)kl_text_touch_take(&select->touch);
		area->anchor = select->touch.anchor;
		area->caret = select->touch.caret;
		area->goal_x = -1;
		return;
	}

	/* The selection's ends in order. */
	start = area->anchor;
	end = area->caret;
	if (start > end) {
		start = area->caret;
		end = area->anchor;
	}

	/* A finger's tap within the fingers' selection shows or hides the bar. */
	if ((state & KL_HIT_TOUCHED) != 0U &&
	    owned &&
	    start != end &&
	    position >= start &&
	    position <= end) {
		kl_text_touch_toggle_bar(&select->touch);
		return;
	}

	/* Anything else ends the fingers' selection and puts the caret at the point (twice: the whole text). */
	if (owned)
		keiui_ui_select_end(ui);
	area->caret = position;
	area->anchor = position;
	area->goal_x = -1;
	if ((state & KL_HIT_DOUBLE) != 0U) {
		area->anchor = 0;
		area->caret = area->length;
	}
}

/*
 * Follows an input the area took with the fingers' selection (ws190-p002):
 * the bar's Copy hides the bar, its Select All selects the whole text with
 * handles and the bar, and its Cut and Paste, and any key or text of the
 * keyboard's, end the selection.
 */
static void
area_select_after(
	struct kl_ui *ui,
	uint32_t id,
	struct kl_text_area *area,
	const struct keiui_input *input)
{
	struct keiui_select *select;
	int owned;

	/* Only an area in the fingers' selection. */
	owned = keiui_ui_select_owned(ui, id, 0U);
	if (!owned)
		return;

	/* The keyboard's input ends it. */
	select = keiui_ui_select(ui);
	if (!input->from_bar) {
		keiui_ui_select_end(ui);
		return;
	}

	/* Copy keeps the selection; the bar goes. */
	if (input->code == 46U) {
		kl_text_touch_hide_bar(&select->touch);
		return;
	}

	/* Select All: the whole text, as the fingers' selection. */
	if (input->code == 30U) {
		kl_text_touch_select(&select->touch, 0, area->length);
		(void)kl_text_touch_take(&select->touch);
		area->anchor = 0;
		area->caret = area->length;
		return;
	}

	/* Cut and Paste end it at the caret. */
	keiui_ui_select_end(ui);
}

/*
 * Brings the fingers' selection to the area before it is drawn: a text the
 * program set (other than the copy kept) or another area ends it;
 * otherwise the selection's ends the fingers moved become the area's, and
 * while a finger drags a handle the area shows the selection's scroll.
 * Reports whether a finger holds the area's scroll.
 */
static int
area_select_check(
	struct kl_ui *ui,
	uint32_t id,
	struct kl_text_area *area)
{
	struct keiui_select *select;
	int owned;
	int same;
	int difference;

	/* Only an area in the fingers' selection. */
	owned = keiui_ui_select_owned(ui, id, 0U);
	if (!owned)
		return 0;

	/* Whether it is the area the copy was made of, with the same text. */
	select = keiui_ui_select(ui);
	same = 0;
	if (select->widget == area && select->area.length == area->length) {
		difference = memcmp(select->area.text, area->text, area->length);
		if (difference == 0)
			same = 1;
	}

	/* Another widget under the id, or a text the program set: the selection ends. */
	if (!same) {
		keiui_ui_select_end(ui);
		return 0;
	}

	/* The ends the fingers moved, within the text. */
	if (select->touch.anchor > area->length)
		select->touch.anchor = area->length;
	if (select->touch.caret > area->length)
		select->touch.caret = area->length;
	area->anchor = select->touch.anchor;
	area->caret = select->touch.caret;
	area->goal_x = -1;
	(void)kl_text_touch_take(&select->touch);

	/* No finger on a handle: the area keeps its own scroll. */
	if (!select->touch.selecting)
		return 0;

	/* Succeeded: the finger's scroll is the area's. */
	area->scroll = (int)select->scroll.y;
	return 1;
}

/*
 * Gives the fingers' selection the area as it was drawn (ws190-p002): its
 * copy and lines, its rectangle and text box, its style and the scroll its
 * content is shown at.
 */
static void
area_select_drawn(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	const struct kl_text_area *area,
	const struct area_layout *layout)
{
	struct keiui_select *select;
	struct kl_rect box;
	int owned;
	int content;

	/* Only an area in the fingers' selection. */
	owned = keiui_ui_select_owned(ui, id, 0U);
	if (!owned)
		return;

	/* The copy and its lines, and where the text is. */
	select = keiui_ui_select(ui);
	select->area = *area;
	memcpy(select->layout, layout, sizeof(*layout));
	box.x = rect->x + AREA_SIDE;
	box.y = rect->y + AREA_TOP;
	box.width = rect->width - 2 * AREA_SIDE;
	box.height = rect->height - 2 * AREA_TOP;
	keiui_ui_select_drawn(ui, rect, &box, style);

	/* The content's scroll down: all the lines in the box's height, at the area's scroll unless a finger holds it. */
	content = (int)layout->count * AREA_LINE;
	kl_scroll_set_size(&select->scroll, (double)box.width, (double)content, (double)box.width, (double)box.height);
	if (!select->touch.selecting)
		kl_scroll_move_to(&select->scroll, 0.0, (double)area->scroll, 0, keiui_ui_now(ui));
}

/* Reports the text position nearest a point of the area's copy (from the text's start and the first line's top). */
static size_t
area_view_position(
	void *data,
	double x,
	double y)
{
	struct keiui_select *select;
	const struct area_layout *layout;
	size_t line;
	size_t position;

	/* The line at the point, within the lines. */
	select = data;
	layout = select->layout;
	line = 0;
	if (y > 0.0)
		line = (size_t)(y / (double)AREA_LINE);
	if (line >= layout->count)
		line = layout->count - 1U;

	/* The nearest boundary across that line. */
	position = area_at_x(layout, &select->style, line, (int)x);

	/* Reports the boundary. */
	return position;
}

/* Gives the caret's rectangle at a position of the area's copy (its line's top and height). */
static void
area_view_caret(
	void *data,
	size_t position,
	struct kl_rect *rect)
{
	struct keiui_select *select;
	const struct area_layout *layout;
	size_t line;

	/* The position's line, and its place across it. */
	select = data;
	layout = select->layout;
	line = area_line_of(layout, position);
	rect->x = area_x_of(layout, &select->style, line, position);
	rect->y = (int)line * AREA_LINE;
	rect->width = 2;
	rect->height = AREA_LINE;
}

/* Gives the word around a position of the area's copy. */
static void
area_view_word(
	void *data,
	size_t position,
	size_t *start,
	size_t *end)
{
	struct keiui_select *select;

	/* The word of the text. */
	select = data;
	keiui_select_word(select->area.text, select->area.length, position, start, end);
}
