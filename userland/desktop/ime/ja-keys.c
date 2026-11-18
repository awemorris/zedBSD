/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the Japanese engine, in the Windows input method's way
 * (plan/ws095/design.md section 7.2; the user chose this way on
 * 2026-09-29).
 *
 * Kana are typed in romaji; Space converts the whole composition and the
 * engine splits it into segments; Space and the arrows then move among the
 * candidates and the segments, Shift with the arrows makes the segment in
 * focus longer or shorter, Enter commits, Escape goes back to the kana,
 * and F6 to F10 give the forms.  A letter typed with shift starts a word
 * in letters.  This file binds keys to the operations of ja-engine.c and
 * nothing else, so that another way of converting replaces it alone.
 */

#include "ja.h"

static void keys_type(struct ja_core *core, const struct ime_key *key, struct ime_output *out);
static bool keys_is_printable(const struct ime_key *key);
static bool keys_is_upper(uint32_t character);
static bool keys_form(struct ja_core *core, uint32_t code);
static void keys_converting(struct ja_core *core, const struct ime_key *key, struct ime_output *out);
static void keys_composing(struct ja_core *core, const struct ime_key *key, struct ime_output *out);

/*
 * Acts on one key: types, converts, chooses or commits, or gives the key
 * back to the application.
 */
void
ja_keys_handle(
	struct ja_core *core,
	const struct ime_key *key,
	struct ime_output *out)
{
	bool composing;
	bool printable;

	composing = ja_core_composing(core);

	/* A shortcut commits what is composed and goes on to the application. */
	if ((key->modifiers & (IME_MOD_CTRL | IME_MOD_ALT | IME_MOD_SUPER)) != 0U) {
		if (composing)
			ja_core_commit(core, out);
		out->pass_key = true;
		return;
	}

	/* A conversion has its own keys. */
	if (core->converting) {
		keys_converting(core, key, out);
		return;
	}

	/* So does a composition. */
	if (composing) {
		keys_composing(core, key, out);
		return;
	}

	/* With nothing composed, a character starts a composition (the space stays a space). */
	printable = keys_is_printable(key);
	if (printable && key->character != ' ') {
		keys_type(core, key, out);
		return;
	}

	/* Every other key is the application's. */
	out->pass_key = true;
}

/*
 * Types a printable key into the composition; a letter typed with shift
 * starts a word in letters.
 */
static void
keys_type(
	struct ja_core *core,
	const struct ime_key *key,
	struct ime_output *out)
{
	bool upper;
	bool latched;

	UNUSED_PARAMETER(out);

	/* A capital letter keeps what follows as letters until it is converted or committed. */
	upper = keys_is_upper(key->character);
	latched = ja_core_literal_latched(core);
	if (upper && !latched)
		ja_core_latch_literal(core);

	/* The character goes in; a full composition drops it. */
	(void)ja_core_type(core, (char)key->character);
}

/*
 * Tells whether a key types a printable ASCII character.
 */
static bool
keys_is_printable(
	const struct ime_key *key)
{
	/* Below the space. */
	if (key->character < 0x20U)
		return false;

	/* Past the tilde. */
	if (key->character > 0x7eU)
		return false;

	/* A printable character. */
	return true;
}

/*
 * Tells whether a character is a capital letter.
 */
static bool
keys_is_upper(
	uint32_t character)
{
	/* Below A. */
	if (character < 'A')
		return false;

	/* Past Z. */
	if (character > 'Z')
		return false;

	/* A capital letter. */
	return true;
}

/*
 * Gives the form a function key asks for (F6 hiragana, F7 katakana, F8
 * half-width katakana, F9 full-width letters, F10 half-width letters).
 *
 * Returns whether the key was one of them.
 */
static bool
keys_form(
	struct ja_core *core,
	uint32_t code)
{
	/* Each function key's form. */
	switch (code) {
	case IME_KEY_F6:
		ja_core_apply_form(core, JA_FORM_HIRAGANA);
		return true;
	case IME_KEY_F7:
		ja_core_apply_form(core, JA_FORM_KATAKANA);
		return true;
	case IME_KEY_F8:
		ja_core_apply_form(core, JA_FORM_HALF_KATAKANA);
		return true;
	case IME_KEY_F9:
		ja_core_apply_form(core, JA_FORM_FULL_ASCII);
		return true;
	case IME_KEY_F10:
		ja_core_apply_form(core, JA_FORM_HALF_ASCII);
		return true;
	default:
		break;
	}

	/* Not a form key. */
	return false;
}

/*
 * Acts on a key while a conversion is shown.
 */
static void
keys_converting(
	struct ja_core *core,
	const struct ime_key *key,
	struct ime_output *out)
{
	bool shift;
	bool handled;
	bool printable;

	shift = false;
	if ((key->modifiers & IME_MOD_SHIFT) != 0U)
		shift = true;

	/* The function keys give forms. */
	handled = keys_form(core, key->code);
	if (handled)
		return;

	/* The keys of a conversion. */
	switch (key->code) {
	case IME_KEY_SPACE:
		/* The next candidate; with shift, the one before. */
		if (shift) {
			ja_core_next_candidate(core, -1);
		} else {
			ja_core_next_candidate(core, 1);
		}

		return;
	case IME_KEY_DOWN:
	case IME_KEY_HENKAN:
		ja_core_next_candidate(core, 1);
		return;
	case IME_KEY_UP:
		ja_core_next_candidate(core, -1);
		return;
	case IME_KEY_PAGE_DOWN:
		ja_core_next_candidate(core, 9);
		return;
	case IME_KEY_PAGE_UP:
		ja_core_next_candidate(core, -9);
		return;
	case IME_KEY_LEFT:
		/* The segment before; with shift, a shorter segment. */
		if (shift) {
			ja_core_resize_focus(core, -1);
		} else {
			ja_core_move_focus(core, -1);
		}

		return;
	case IME_KEY_RIGHT:
		/* The segment after; with shift, a longer segment. */
		if (shift) {
			ja_core_resize_focus(core, 1);
		} else {
			ja_core_move_focus(core, 1);
		}

		return;
	case IME_KEY_ENTER:
	case IME_KEY_KP_ENTER:
		ja_core_commit(core, out);
		return;
	case IME_KEY_ESCAPE:
	case IME_KEY_BACKSPACE:
		/* Back to the kana. */
		ja_core_cancel_conversion(core);
		return;
	default:
		break;
	}

	/* A number chooses a candidate on the open window's page. */
	if (key->code >= IME_KEY_1 && key->code <= IME_KEY_9 && !shift) {
		handled = ja_core_select_on_page(core, key->code - IME_KEY_1);
		if (handled)
			return;
	}

	/* A character commits the conversion and starts the next composition. */
	printable = keys_is_printable(key);
	if (printable && key->character != ' ') {
		ja_core_commit(core, out);
		keys_type(core, key, out);
		return;
	}

	/* Any other key is swallowed, so that it does not act beside the conversion. */
}

/*
 * Acts on a key while kana are being typed.
 */
static void
keys_composing(
	struct ja_core *core,
	const struct ime_key *key,
	struct ime_output *out)
{
	bool handled;
	bool printable;

	/* The function keys give forms. */
	handled = keys_form(core, key->code);
	if (handled)
		return;

	/* The keys of a composition. */
	switch (key->code) {
	case IME_KEY_SPACE:
	case IME_KEY_HENKAN:
		/* Converts the whole composition. */
		(void)ja_core_convert(core);
		return;
	case IME_KEY_ENTER:
	case IME_KEY_KP_ENTER:
		/* Commits the kana as they are. */
		ja_core_commit(core, out);
		return;
	case IME_KEY_ESCAPE:
		/* Drops the composition. */
		ja_core_clear(core);
		return;
	case IME_KEY_BACKSPACE:
		ja_core_backspace(core);
		return;
	default:
		break;
	}

	/* A character goes into the composition. */
	printable = keys_is_printable(key);
	if (printable) {
		keys_type(core, key, out);
		return;
	}

	/* Any other key (the arrows among them) is swallowed while composing. */
}
