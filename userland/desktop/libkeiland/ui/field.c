/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one-line text field of the library (ws090-p005), in Settings' look
 * (settings/page-network.c: white with 8-pixel corners, the accent's edge
 * while it has the keyboard, a thin caret) and with the editing of the
 * file chooser's and Files' fields: characters of the US layout, Left and
 * Right (with Shift, the selection), Home and End, Backspace and Delete,
 * Ctrl+A; Enter submits and Esc cancels.  A click or a tap takes the
 * keyboard and puts the caret at the character nearest the point.  A
 * secret field shows its characters as dots.
 *
 * Since BUG-203 a field that is not secret also takes the text an input
 * method or the on-screen keyboard sends (kl_ui_text): a commit goes in
 * place of the selection, the bytes around the caret are deleted, and the
 * text being composed shows underlined at the caret until it is committed.
 * A plain field (KL_VERSION 47: an address, a key shown as typed) shows
 * its characters but takes no input method, as a secret one.  A field's
 * limit (KL_VERSION 64, ws177-p004) is the most bytes its text holds: a
 * key or a commit past it puts in nothing more, or what fits.
 */

#include "internal.h"

#include <string.h>

/* The text's size and its margin inside the field. */
#define FIELD_TEXT		14U
#define FIELD_SIDE		12

/* The dot a secret field shows for each character, in UTF-8. */
#define FIELD_DOT		"\xe2\x80\xa2"

static int field_wants(uint32_t code, unsigned modifiers);
static unsigned field_key(struct kl_field *field, uint32_t code, unsigned modifiers);
static void field_erase(struct kl_field *field);
static size_t field_prev(const struct kl_field *field, size_t at);
static size_t field_next(const struct kl_field *field, size_t at);
static size_t field_shown(const struct kl_field *field, char *out, size_t size, size_t through);
static size_t field_at(const struct kl_style *style, const struct kl_field *field, int x);
static unsigned field_input(struct kl_field *field, const struct keiui_input *input);
static void field_insert(struct kl_field *field, const char *text);
static void field_delete_around(struct kl_field *field, size_t before, size_t after);
static size_t field_room(const struct kl_field *field);
static size_t field_boundary(const char *text, size_t at);

/*
 * Sets a field's text, with the caret at its end and nothing selected.
 */
void
kl_field_set(
	struct kl_field *field,
	const char *text)
{
	size_t room;
	size_t length;

	/* The text, cut to the field's limit at a character's start. */
	room = field_room(field);
	length = strlen(text);
	if (length > room)
		length = field_boundary(text, room);
	memcpy(field->text, text, length);
	field->text[length] = '\0';
	field->length = length;

	/* The caret at the end. */
	field->caret = field->length;
	field->anchor = field->length;
	field->scroll = 0;
}

/*
 * Sets the most bytes a field's text may hold (0: the field's room); a
 * text longer than that now is cut at a character's start, with the caret
 * at its end.
 */
void
kl_field_set_limit(
	struct kl_field *field,
	size_t limit)
{
	size_t room;

	/* The limit, no more than the field's room. */
	if (limit > sizeof(field->text) - 1U)
		limit = sizeof(field->text) - 1U;
	field->limit = limit;

	/* A text that fits stays as it is. */
	room = field_room(field);
	if (field->length <= room)
		return;

	/* A longer one is cut, and the caret goes to its end. */
	field->length = field_boundary(field->text, room);
	field->text[field->length] = '\0';
	field->caret = field->length;
	field->anchor = field->length;
	field->scroll = 0;
}

/*
 * Draws a text field in a rectangle and takes its input; reports what
 * happened (KL_FIELD_* bits).  placeholder (may be NULL) shows while it
 * is empty.
 */
unsigned
kl_field(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_field *field,
	const char *placeholder)
{
	const struct kl_theme *theme;
	const char *preedit;
	struct keiui_input input;
	struct kl_rect inside;
	struct kl_rect caret;
	char shown[KL_FIELD_MAX * 3U];
	unsigned changes;
	unsigned state;
	size_t length;
	size_t preedit_length;
	size_t caret_offset;
	size_t start;
	size_t end;
	int32_t preedit_begin;
	int32_t preedit_end;
	int caret_x;
	int caret_shown;
	int left_x;
	int right_x;
	int preedit_x;
	int preedit_right;
	int baseline;
	int width;
	int taken;
	int focused;
	double pointer_x;
	double pointer_y;

	/* The record: it takes the keyboard. */
	theme = style->theme;
	state = keiui_ui_widget(ui, id, 0U, rect, KEIUI_FOCUSABLE);
	focused = 0;
	if ((state & KL_HIT_FOCUSED) != 0U)
		focused = 1;

	/* A click or a tap puts the caret at the point (twice: the whole text selected). */
	changes = 0;
	if ((state & KL_HIT_CLICKED) != 0U) {
		kl_ui_pointer(ui, &pointer_x, &pointer_y);
		field->caret = field_at(style, field, (int)pointer_x - rect->x - FIELD_SIDE + field->scroll);
		field->anchor = field->caret;
		if ((state & KL_HIT_DOUBLE) != 0U) {
			field->anchor = 0;
			field->caret = field->length;
		}
	}

	/* The keys while it has the keyboard, and the text an input method sent for it, in the order they came. */
	for (;;) {
		taken = keiui_ui_take_input(ui, id, 0U, field_wants, &input);
		if (!taken)
			break;
		changes |= field_input(field, &input);
	}

	/* The text an input method is composing for it (a secret field's characters do not show, so neither does it). */
	preedit_begin = -1;
	preedit_end = -1;
	preedit = keiui_ui_preedit(ui, id, 0U, focused, &preedit_begin, &preedit_end);
	if (field->secret || field->plain)
		preedit = NULL;

	/* The ground: white, with the accent's edge while it has the keyboard. */
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, theme->panel);
	if (focused)
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, 1.5f, theme->accent);
	else
		kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, theme->control_radius, 1.0f, theme->control_edge);

	/* The text as shown (dots for a secret one), and where the caret and the selection's ends are in it. */
	length = field_shown(field, shown, sizeof(shown), field->length);
	start = field->anchor;
	end = field->caret;
	if (start > end) {
		start = field->caret;
		end = field->anchor;
	}

	/*
	 * The text being composed shows at the caret, inside the text as shown
	 * (a field that is not secret shows its own bytes, so the caret's
	 * offset is the same in both); no selection shows meanwhile, since the
	 * commit replaces it.
	 */
	preedit_length = 0;
	if (preedit != NULL) {
		preedit_length = strlen(preedit);
		memmove(shown + field->caret + preedit_length, shown + field->caret, length - field->caret + 1U);
		memcpy(shown + field->caret, preedit, preedit_length);
		length += preedit_length;
		start = field->caret;
		end = field->caret;
	}

	/*
	 * The caret's offset in the text as shown: at the composed text's
	 * cursor while one is composed, and hidden when the input method hides
	 * that cursor (the end of the composed text is kept in sight then).
	 */
	caret_offset = field_shown(field, NULL, 0U, field->caret);
	caret_shown = focused;
	if (preedit != NULL) {
		if (preedit_begin < 0 || (size_t)preedit_begin > preedit_length) {
			caret_offset += preedit_length;
			caret_shown = 0;
		} else {
			caret_offset += (size_t)preedit_begin;
		}
	}

	/* Where the caret and the selection's ends stand across. */
	caret_x = kl_text_width(style->text, shown, caret_offset, FIELD_TEXT, 0);
	left_x = kl_text_width(style->text, shown, field_shown(field, NULL, 0U, start), FIELD_TEXT, 0);
	right_x = kl_text_width(style->text, shown, field_shown(field, NULL, 0U, end), FIELD_TEXT, 0);

	/* The text scrolls across just enough to keep the caret inside. */
	width = rect->width - 2 * FIELD_SIDE;
	if (caret_x - field->scroll > width)
		field->scroll = caret_x - width;
	if (caret_x < field->scroll)
		field->scroll = caret_x;

	/* Inside the field: the selection, the text or the placeholder, and the caret while it has the keyboard. */
	inside.x = rect->x + FIELD_SIDE / 2;
	inside.y = rect->y;
	inside.width = rect->width - FIELD_SIDE;
	inside.height = rect->height;
	kl_canvas_clip_push(style->canvas, &inside);
	baseline = kl_text_center(FIELD_TEXT, rect->y, rect->height);
	if (start != end && focused)
		kl_canvas_round(style->canvas, (float)(rect->x + FIELD_SIDE + left_x - field->scroll), (float)rect->y + 7.0f, (float)(right_x - left_x), (float)rect->height - 14.0f, 2.0f, theme->selection);
	if (field->length == 0 && preedit == NULL && placeholder != NULL)
		(void)kl_text_draw(style->text, style->canvas, rect->x + FIELD_SIDE, baseline, placeholder, strlen(placeholder), FIELD_TEXT, 0, theme->text_faint);
	else
		(void)kl_text_draw(style->text, style->canvas, rect->x + FIELD_SIDE - field->scroll, baseline, shown, length, FIELD_TEXT, 0, theme->text);

	/* The text being composed is underlined, so that it reads as not yet written. */
	if (preedit != NULL) {
		preedit_x = kl_text_width(style->text, shown, field->caret, FIELD_TEXT, 0);
		preedit_right = kl_text_width(style->text, shown, field->caret + preedit_length, FIELD_TEXT, 0);
		kl_canvas_line(style->canvas, (float)(rect->x + FIELD_SIDE + preedit_x - field->scroll), (float)baseline + 3.5f, (float)(rect->x + FIELD_SIDE + preedit_right - field->scroll), (float)baseline + 3.5f, 1.0f, theme->text);
	}

	/* The caret, while it has the keyboard and the input method does not hide it. */
	if (caret_shown)
		kl_canvas_line(style->canvas, (float)(rect->x + FIELD_SIDE + caret_x - field->scroll) + 0.75f, (float)rect->y + 8.0f, (float)(rect->x + FIELD_SIDE + caret_x - field->scroll) + 0.75f, (float)(rect->y + rect->height) - 8.0f, 1.5f, theme->accent);
	kl_canvas_clip_pop(style->canvas);

	/* With the keyboard, a field that is neither secret nor plain takes an input method's text at its caret (kl_ui_text_wanted). */
	if (focused &&
	    !field->secret &&
	    !field->plain) {
		caret.x = rect->x + FIELD_SIDE + caret_x - field->scroll;
		caret.y = rect->y + 8;
		caret.width = 2;
		caret.height = rect->height - 16;
		keiui_ui_text_caret(ui, &caret);
	}

	/* Reports what happened. */
	return changes;
}

/* Tells whether a field takes a key: its characters, its editing keys, Enter and Esc (with Control, only A). */
static int
field_wants(
	uint32_t code,
	unsigned modifiers)
{
	uint32_t character;

	/* Control's only key is A (select all); Alt and Super are commands. */
	if ((modifiers & (KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return 0;
	if ((modifiers & KL_MOD_CTRL) != 0U) {
		if (code == 30U)
			return 1;
		return 0;
	}

	/* The editing keys. */
	switch (code) {
	case KL_KEY_LEFT:
	case KL_KEY_RIGHT:
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

/* Carries out one key in a field; reports what happened (KL_FIELD_* bits). */
static unsigned
field_key(
	struct kl_field *field,
	uint32_t code,
	unsigned modifiers)
{
	uint32_t character;
	unsigned shift;
	size_t room;
	size_t at;

	/* Shift keeps the other end of the selection where it is. */
	shift = modifiers & KL_MOD_SHIFT;

	/* The keys that move, erase, submit and cancel. */
	switch (code) {
	case KL_KEY_LEFT:
		at = field_prev(field, field->caret);
		if (field->caret != field->anchor && shift == 0U && field->anchor < field->caret)
			at = field->anchor;
		field->caret = at;
		if (shift == 0U)
			field->anchor = at;
		return 0;
	case KL_KEY_RIGHT:
		at = field_next(field, field->caret);
		if (field->caret != field->anchor && shift == 0U && field->anchor > field->caret)
			at = field->anchor;
		field->caret = at;
		if (shift == 0U)
			field->anchor = at;
		return 0;
	case KL_KEY_HOME:
		field->caret = 0;
		if (shift == 0U)
			field->anchor = 0;
		return 0;
	case KL_KEY_END:
		field->caret = field->length;
		if (shift == 0U)
			field->anchor = field->length;
		return 0;
	case KL_KEY_BACKSPACE:
		if (field->caret == field->anchor)
			field->anchor = field_prev(field, field->caret);
		field_erase(field);
		return KL_FIELD_CHANGED;
	case KL_KEY_DELETE:
		if (field->caret == field->anchor)
			field->anchor = field_next(field, field->caret);
		field_erase(field);
		return KL_FIELD_CHANGED;
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
		return KL_FIELD_SUBMITTED;
	case KL_KEY_ESC:
		return KL_FIELD_CANCELLED;
	default:
		break;
	}

	/* Ctrl+A selects the whole text. */
	if ((modifiers & KL_MOD_CTRL) != 0U) {
		field->anchor = 0;
		field->caret = field->length;
		return 0;
	}

	/* A character replaces the selection (a field at its limit takes no more). */
	character = kl_key_character(code, modifiers);
	if (character == 0U)
		return 0;
	field_erase(field);
	room = field_room(field);
	if (field->length + 1U > room)
		return KL_FIELD_CHANGED;
	memmove(field->text + field->caret + 1U, field->text + field->caret, field->length - field->caret + 1U);
	field->text[field->caret] = (char)character;
	field->length++;
	field->caret++;
	field->anchor = field->caret;

	/* Succeeded: the text changed. */
	return KL_FIELD_CHANGED;
}

/* Erases the selection (nothing when the caret and the anchor meet). */
static void
field_erase(
	struct kl_field *field)
{
	size_t start;
	size_t end;

	/* The selection's ends in order. */
	start = field->anchor;
	end = field->caret;
	if (start > end) {
		start = field->caret;
		end = field->anchor;
	}

	/* The bytes after it close up. */
	memmove(field->text + start, field->text + end, field->length - end + 1U);
	field->length -= end - start;
	field->caret = start;
	field->anchor = start;
}

/* Reports the start of the character before an offset. */
static size_t
field_prev(
	const struct kl_field *field,
	size_t at)
{
	/* Nothing before the start. */
	if (at == 0U)
		return 0U;

	/* Back over the continuation bytes. */
	at--;
	while (at > 0U && ((unsigned char)field->text[at] & 0xc0U) == 0x80U)
		at--;

	/* Reports the character's start. */
	return at;
}

/* Reports the offset after the character at an offset. */
static size_t
field_next(
	const struct kl_field *field,
	size_t at)
{
	/* Nothing after the end. */
	if (at >= field->length)
		return field->length;

	/* On over the continuation bytes. */
	at++;
	while (at < field->length && ((unsigned char)field->text[at] & 0xc0U) == 0x80U)
		at++;

	/* Reports the next character's start. */
	return at;
}

/*
 * Writes the text as shown (a dot for each character of a secret field)
 * into out (when not NULL), up to the byte offset through of the text, and
 * reports the length shown up to there.
 */
static size_t
field_shown(
	const struct kl_field *field,
	char *out,
	size_t size,
	size_t through)
{
	size_t length;
	size_t at;

	/* A plain field shows its bytes. */
	if (!field->secret) {
		if (out != NULL) {
			memcpy(out, field->text, field->length + 1U);
			return field->length;
		}

		/* Without an output: the offset itself. */
		return through;
	}

	/* A secret one a dot for each character up to the offset. */
	length = 0;
	at = 0;
	while (at < through && at < field->length) {
		at = field_next(field, at);
		if (out != NULL && length + sizeof(FIELD_DOT) < size)
			memcpy(out + length, FIELD_DOT, sizeof(FIELD_DOT));
		length += sizeof(FIELD_DOT) - 1U;
	}

	/* Reports the shown length. */
	return length;
}

/* Reports the byte offset of the character boundary nearest a place (pixels from the text's start). */
static size_t
field_at(
	const struct kl_style *style,
	const struct kl_field *field,
	int x)
{
	char shown[KL_FIELD_MAX * 3U];
	size_t best;
	size_t at;
	int width;
	int distance;
	int nearest;

	/* The text as shown. */
	(void)field_shown(field, shown, sizeof(shown), field->length);

	/* Each boundary, the nearest one kept. */
	best = 0;
	nearest = -1;
	at = 0;
	for (;;) {
		width = kl_text_width(style->text, shown, field_shown(field, NULL, 0U, at), FIELD_TEXT, 0);
		distance = width - x;
		if (distance < 0)
			distance = -distance;
		if (nearest < 0 || distance < nearest) {
			nearest = distance;
			best = at;
		}

		/* Stops at the end, else the next character. */
		if (at >= field->length)
			break;
		at = field_next(field, at);
	}

	/* Reports the nearest boundary. */
	return best;
}

/* Carries out one input in a field: a key, a text to commit, or bytes to delete; reports what happened (KL_FIELD_* bits). */
static unsigned
field_input(
	struct kl_field *field,
	const struct keiui_input *input)
{
	unsigned changes;

	/* Each kind of input. */
	switch (input->kind) {
	case KEIUI_INPUT_KEY:
		changes = field_key(field, input->code, input->modifiers);
		break;
	case KEIUI_INPUT_COMMIT:
		field_insert(field, input->text);
		changes = KL_FIELD_CHANGED;
		break;
	case KEIUI_INPUT_DELETE:
		field_delete_around(field, (size_t)input->before, (size_t)input->after);
		changes = KL_FIELD_CHANGED;
		break;
	default:
		changes = 0;
		break;
	}

	/* Reports what the input did. */
	return changes;
}

/* Puts a text an input method committed in place of the selection, as much of it as the field has room for. */
static void
field_insert(
	struct kl_field *field,
	const char *text)
{
	char clean[KL_WINDOW_TEXT_MAX];
	size_t length;
	size_t room;
	size_t at;
	unsigned char byte;

	/* The text without control characters, which a one-line field does not hold. */
	length = 0;
	for (at = 0; text[at] != '\0' && length + 1U < sizeof(clean); at++) {
		byte = (unsigned char)text[at];
		if (byte < 0x20U || byte == 0x7fU)
			continue;
		clean[length] = (char)byte;
		length++;
	}
	clean[length] = '\0';

	/* The selection goes first. */
	field_erase(field);

	/* As much of the text as the limit has room for, cut at a character's start. */
	room = field_room(field) - field->length;
	if (length > room)
		length = field_boundary(clean, room);

	/* The bytes after the caret move on, and the text goes in before them; the caret follows it. */
	memmove(field->text + field->caret + length, field->text + field->caret, field->length - field->caret + 1U);
	memcpy(field->text + field->caret, clean, length);
	field->length += length;
	field->caret += length;
	field->anchor = field->caret;
}

/* Deletes bytes before and after the selection (whole characters), as an input method asks; the selection stays. */
static void
field_delete_around(
	struct kl_field *field,
	size_t before,
	size_t after)
{
	size_t start;
	size_t end;
	size_t low;
	size_t high;

	/* The selection's ends in order: the bytes are counted out from them. */
	start = field->anchor;
	end = field->caret;
	if (start > end) {
		start = field->caret;
		end = field->anchor;
	}

	/* The first byte deleted before it, back to a character's start. */
	low = 0;
	if (before < start)
		low = start - before;
	while (low > 0U && ((unsigned char)field->text[low] & 0xc0U) == 0x80U)
		low--;

	/* The first byte kept after it, on to a character's start. */
	high = field->length;
	if (after < field->length - end)
		high = end + after;
	while (high < field->length && ((unsigned char)field->text[high] & 0xc0U) == 0x80U)
		high++;

	/* The bytes after the selection close up first, then those before it. */
	memmove(field->text + end, field->text + high, field->length - high + 1U);
	field->length -= high - end;
	memmove(field->text + low, field->text + start, field->length - start + 1U);
	field->length -= start - low;

	/* The selection moves back by what went before it. */
	field->caret -= start - low;
	field->anchor -= start - low;
}

/* Gives the most bytes a field's text may hold: its limit, or its room when it has none. */
static size_t
field_room(
	const struct kl_field *field)
{
	/* No limit, or one past the room: the room. */
	if (field->limit == 0U)
		return sizeof(field->text) - 1U;
	if (field->limit > sizeof(field->text) - 1U)
		return sizeof(field->text) - 1U;

	/* Succeeded: the limit. */
	return field->limit;
}

/* Gives the start of the character at or before a byte offset of a text: where it may be cut. */
static size_t
field_boundary(
	const char *text,
	size_t at)
{
	/* Back over the continuation bytes. */
	while (at > 0U && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at--;

	/* Succeeded: a character's start. */
	return at;
}
