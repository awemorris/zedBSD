/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The on-screen keyboard's layouts (keyboard-layout.c, ws102-p003): the
 * flick panel's faces and keys, which character a flick in a direction
 * types, and the voiced and small forms of the kana; the QWERTY panel's
 * rows (p006); and the handwriting face's ink and recognizer
 * (keyboard-hand.c, p008).  It knows nothing of Wayland or drawing, so the
 * host's tests read it directly.
 */

#ifndef KWL_KEYBOARD_H
#define KWL_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

/* The flick panel's grid: four columns (the last is the fixed keys) and four rows. */
#define KWL_FLICK_COLUMNS	4U
#define KWL_FLICK_ROWS		4U

/* The flick panel's faces, in the order the face key goes through them. */
#define KWL_FLICK_KANA		0U
#define KWL_FLICK_ALPHA		1U
#define KWL_FLICK_NUMBER	2U
#define KWL_FLICK_FACES		3U

/* The directions of a flick: none (a tap), then left, up, right and down. */
#define KWL_FLICK_CENTER	0U
#define KWL_FLICK_LEFT		1U
#define KWL_FLICK_UP		2U
#define KWL_FLICK_RIGHT		3U
#define KWL_FLICK_DOWN		4U
#define KWL_FLICK_DIRECTIONS	5U

/* The evdev codes of the keys the keyboard sends besides the characters'. */
#define KWL_FLICK_KEY_BACKSPACE	14U
#define KWL_FLICK_KEY_ENTER	28U
#define KWL_FLICK_KEY_SPACE	57U

/*
 * What a key does besides typing its characters: nothing more (a
 * character key), delete the character before the cursor, a space, a new
 * line, go to the next face, cycle the last kana through its voiced and
 * small forms, or change the last letter's case.
 */
#define KWL_FLICK_TYPE		0U
#define KWL_FLICK_BACKSPACE	1U
#define KWL_FLICK_SPACE		2U
#define KWL_FLICK_ENTER		3U
#define KWL_FLICK_FACE		4U
#define KWL_FLICK_VOICE		5U
#define KWL_FLICK_CASE		6U
#define KWL_FLICK_SHIFT		7U
#define KWL_FLICK_ARROW		8U
#define KWL_FLICK_CTRL		9U
#define KWL_FLICK_ALT		10U

/*
 * The QWERTY panel's faces (ws102-p006): the letters (with Shift, the
 * capitals and the digits' symbols) and the symbols; the letters for an
 * email address (@ beside the space) and for a web address (/ beside the
 * space), and the digits' pad (q893, the field's kind); its rows, the most
 * keys a row has, and a row's width in quarter keys.  The rows count from
 * the top: the extra keys (Esc, Tab, Ctrl, Alt, a few symbols, Home, End,
 * PgUp, PgDn; ws102-p020), the digits, three rows of letters (or symbols)
 * and the space row; the pad's middle four rows are its keys.
 */
#define KWL_QWERTY_LETTERS	0U
#define KWL_QWERTY_SYMBOLS	1U
#define KWL_QWERTY_EMAIL	2U
#define KWL_QWERTY_URL		3U
#define KWL_QWERTY_NUMBER	4U
#define KWL_QWERTY_FACES	5U
#define KWL_QWERTY_ROWS		6U
#define KWL_QWERTY_EXTRA_ROW	0U
#define KWL_QWERTY_ROW_KEYS	12U
#define KWL_QWERTY_ROW_UNITS	40U

/* The evdev codes of the keys sent by their code (KWL_FLICK_ARROW): the arrows, Esc, Tab, Home, End, PgUp, PgDn. */
#define KWL_KEY_ESC		1U
#define KWL_KEY_TAB		15U
#define KWL_KEY_HOME		102U
#define KWL_KEY_UP		103U
#define KWL_KEY_PAGE_UP		104U
#define KWL_KEY_LEFT		105U
#define KWL_KEY_RIGHT		106U
#define KWL_KEY_END		107U
#define KWL_KEY_DOWN		108U
#define KWL_KEY_PAGE_DOWN	109U

/*
 * One key of a face: its label, what it does, and the characters (UTF-8)
 * it types in each direction, NULL where a direction types nothing.
 */
struct kwl_flick_key {
	const char *label;
	unsigned action;
	const char *text[KWL_FLICK_DIRECTIONS];
};

/*
 * One key of the QWERTY panel: its label and its label with Shift, what it
 * types without and with Shift (NULL for a key that only acts), what it
 * does (KWL_FLICK_*), the evdev code of a key sent by its code (an arrow,
 * Esc, Tab, ...), and its width in quarter keys.
 */
struct kwl_qwerty_key {
	const char *label;
	const char *shifted_label;
	const char *text;
	const char *shifted;
	unsigned action;
	unsigned code;
	unsigned width;
};

/*
 * Handwriting (keyboard-hand.c, ws102-p008): the most strokes and points
 * a written character keeps, the most candidates a recognizer returns, and
 * the longest candidate (UTF-8, with its end).
 */
#define KWL_HAND_STROKES	64U
#define KWL_HAND_POINTS		512U
#define KWL_HAND_CANDIDATES	4U
#define KWL_HAND_TEXT		32U

/* One point of a stroke, in output pixels. */
struct kwl_hand_point {
	int16_t x;
	int16_t y;
};

/*
 * One stroke of the pen or finger, from its press to its release: its
 * points in order (the last ones dropped past KWL_HAND_POINTS).
 */
struct kwl_hand_stroke {
	unsigned count;
	struct kwl_hand_point points[KWL_HAND_POINTS];
};

/*
 * The ink written on the handwriting face since it was last cleared: its
 * strokes in order (a stroke past KWL_HAND_STROKES is not kept).
 */
struct kwl_hand_ink {
	unsigned count;
	struct kwl_hand_stroke strokes[KWL_HAND_STROKES];
};

/*
 * What a recognizer answers: its candidates (UTF-8, the likeliest first)
 * and a note to show over them (empty for none).
 */
struct kwl_hand_result {
	unsigned count;
	char candidates[KWL_HAND_CANDIDATES][KWL_HAND_TEXT];
	char note[KWL_HAND_TEXT];
};

/*
 * The kinds of field the keyboard follows (q893), from the text input's
 * purpose (text-input-v3's numbers): text, digits (digits, a number, a
 * phone number), an email address and a web address.  A text field keeps
 * the faces the user chose; the others open the faces made for them.
 */
#define KWL_FIELD_TEXT		0U
#define KWL_FIELD_NUMBER	1U
#define KWL_FIELD_EMAIL		2U
#define KWL_FIELD_URL		3U

/*
 * The emoji face's categories (ws102-p022): faces, hands and people,
 * things, symbols; and how many emoji a category holds at most.
 */
#define KWL_EMOJI_FACES		0U
#define KWL_EMOJI_PEOPLE	1U
#define KWL_EMOJI_THINGS	2U
#define KWL_EMOJI_SYMBOLS	3U
#define KWL_EMOJI_CATEGORIES	4U
#define KWL_EMOJI_PER_CATEGORY	20U

const struct kwl_flick_key *kwl_flick_key(unsigned face, unsigned row, unsigned column);
const char *kwl_flick_face_name(unsigned face);
unsigned kwl_flick_face_next(unsigned face);
unsigned kwl_flick_direction(int dx, int dy, int key_size);
const char *kwl_flick_text(const struct kwl_flick_key *key, unsigned direction);
int kwl_flick_voice(const char *previous, char *next, size_t size);
int kwl_flick_case(const char *previous, char *next, size_t size);
const char *kwl_flick_direction_name(unsigned direction);
int kwl_flick_us_key(const char *text, unsigned *code, int *shift);
const struct kwl_qwerty_key *kwl_qwerty_row(unsigned face, unsigned row, unsigned *count);
const char *kwl_qwerty_face_name(unsigned face);
unsigned kwl_qwerty_face_next(unsigned face, unsigned field_face);
unsigned kwl_field_kind(uint32_t purpose);
unsigned kwl_field_qwerty_face(unsigned kind);
unsigned kwl_field_flick_face(unsigned kind, unsigned chosen);
const char *kwl_field_kind_name(unsigned kind);
void kwl_hand_clear(struct kwl_hand_ink *ink);
int kwl_hand_begin(struct kwl_hand_ink *ink, int32_t x, int32_t y);
int kwl_hand_add(struct kwl_hand_ink *ink, int32_t x, int32_t y);
unsigned kwl_hand_points(const struct kwl_hand_ink *ink);
void kwl_hand_bounds(const struct kwl_hand_ink *ink, int32_t *rect);
int kwl_hand_load(const char *path);
void kwl_hand_preload(struct kwl_hand_result *result);
void kwl_hand_recognize(const struct kwl_hand_ink *ink, int32_t area, struct kwl_hand_result *result);
void kwl_hand_recognize_on(const struct kwl_hand_ink *ink, int32_t top, int32_t height, struct kwl_hand_result *result);
unsigned kwl_emoji_count(unsigned category);
const char *kwl_emoji(unsigned category, unsigned index);
const char *kwl_emoji_category_name(unsigned category);


#endif
