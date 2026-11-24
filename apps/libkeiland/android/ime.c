/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text input on Android.  The soft keyboard and the input methods belong
 * to Java's invisible view (InputView); what they commit, compose and delete
 * comes through the bridge as KEILAND_BRIDGE_COMMIT, _COMPOSE and _DELETE and
 * is turned here into the window's text inputs.  The program tells whether the
 * window wants text (kl_window_text_input) and where its caret is
 * (kl_window_text_cursor); those go to Java.
 *
 * A window that does not want text (a secret is being typed, or the input
 * method is switched off) still gets what the keyboard commits, because a soft
 * keyboard has no other way to type: Java is told to take letters only, and the
 * text still being composed is dropped.
 *
 * The terminal's text after the cursor cannot be taken back, so a deletion
 * is sent as Backspace and Delete keys, as the terminal's own keyboard does.
 */

#include "apps/libkeiland/android/android.h"

#include "apps/libkeiland/android/bridge-jni.h"

#include <errno.h>
#include <string.h>

/* The kinds of input Java's view is asked for: letters only, and a language's input. */
#define IME_MODE_LETTERS	0
#define IME_MODE_LANGUAGE	1

/* Whether text is being composed now (a commit first takes the composing text away). */
static int ime_composing;

static void ime_commit(struct kl_window *window, const char *text);
static void ime_compose(struct kl_window *window, const char *text, int32_t cursor);
static void ime_key(struct kl_window *window, uint32_t code, int pressed);

/*
 * Takes whether the window wants text: the soft keyboard is shown for a window that
 * does (its input is a language's), and a window that does not keeps it as it is, for
 * letters only.  Only the window on top has the keyboard.
 */
void
kl_window_text_input(
	struct kl_window *window,
	int enabled)
{
	struct kl_window *top;

	/* What the window wants, even when it is not the one shown. */
	window->text_input = (enabled != 0);
	if (window->app == NULL)
		return;
	top = android_window_top(window->app);
	if (top != window)
		return;

	/* The keyboard's kind, and shown when the window wants text. */
	(void)keiland_java_set_input_mode(enabled ? IME_MODE_LANGUAGE : IME_MODE_LETTERS);
	if (enabled) {
		(void)keiland_java_show_keyboard(1);
	} else {
		/* The composing text goes with the language's input. */
		ime_composing = 0;
	}
}

/* Takes where the caret is, in the window's pixels, and tells the input method where it is on the screen. */
void
kl_window_text_cursor(
	struct kl_window *window,
	int x,
	int y,
	int width,
	int height)
{
	struct kl_window *top;

	/* Only the window on top has the input method. */
	if (window->app == NULL)
		return;
	top = android_window_top(window->app);
	if (top != window)
		return;

	/* The window is the native window less the insets. */
	(void)keiland_java_set_cursor_anchor((float)(x + window->app->insets[ANDROID_INSET_LEFT]),
	    (float)(y + window->app->insets[ANDROID_INSET_TOP] + (int32_t)android_titlebar_height(window)), (float)width, (float)height);
}

/* Takes the text around the caret: the input method is not shown it (the editable stays empty). */
void
kl_window_text_context(
	struct kl_window *window,
	const char *text,
	size_t cursor,
	size_t anchor,
	unsigned hint,
	unsigned purpose)
{
	(void)window;
	(void)text;
	(void)cursor;
	(void)anchor;
	(void)hint;
	(void)purpose;
}

/* Turns what the input method committed, composed or deleted into the inputs of the window on top. */
void
android_ime_bridge(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	struct kl_window *window;
	char part[KL_WINDOW_TEXT_MAX];
	const char *text;
	size_t length;
	size_t take;
	int32_t index;

	/* The window that has the input. */
	window = android_window_top(app);
	if (window == NULL)
		return;

	/* By kind. */
	switch (event->kind) {
	case KEILAND_BRIDGE_COMMIT:
		/* Cut into inputs of as many bytes as one carries, never within a character. */
		text = event->text != NULL ? event->text : "";
		length = strlen(text);
		while (length > 0U) {
			take = length;
			if (take > sizeof(part) - 1U) {
				take = sizeof(part) - 1U;
				while (take > 0U && ((unsigned char)text[take] & 0xc0U) == 0x80U)
					take--;
				if (take == 0U)
					break;
			}

			memcpy(part, text, take);
			part[take] = '\0';
			ime_commit(window, part);
			text += take;
			length -= take;
		}

		break;
	case KEILAND_BRIDGE_COMPOSE:
		/* A window that wants no text has nothing composed (letters only). */
		if (!window->text_input)
			break;
		ime_compose(window, event->text != NULL ? event->text : "", event->a);
		break;
	case KEILAND_BRIDGE_DELETE:
		/* Backspaces for what is before the caret and Deletes for what is after it. */
		for (index = 0; index < event->a && index < 64; index++) {
			ime_key(window, KL_KEY_BACKSPACE, 1);
			ime_key(window, KL_KEY_BACKSPACE, 0);
		}

		for (index = 0; index < event->b && index < 64; index++) {
			ime_key(window, KL_KEY_DELETE, 1);
			ime_key(window, KL_KEY_DELETE, 0);
		}

		break;
	default:
		break;
	}
}

/* Sends committed text (at most KL_WINDOW_TEXT_MAX - 1 bytes), first taking away the text that was composing. */
static void
ime_commit(
	struct kl_window *window,
	const char *text)
{
	struct kl_window_event input;

	/* What was composing is now the text. */
	if (ime_composing) {
		ime_compose(window, "", -1);
		ime_composing = 0;
	}

	/* The text, as the input. */
	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_TEXT_COMMIT;
	(void)memcpy(input.text, text, strlen(text) + 1U);
	(void)android_window_queue(window, &input);
}

/*
 * Sends the text being composed.  cursor counts code points from the start of the text, and -1 hides
 * the cursor; the input says it in bytes.  An empty text takes the composing text away.
 */
static void
ime_compose(
	struct kl_window *window,
	const char *text,
	int32_t cursor)
{
	struct kl_window_event input;
	size_t length;
	size_t at;
	int32_t position;

	/* The text, cut at a character's start when it is too long. */
	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_TEXT_PREEDIT;
	length = strlen(text);
	if (length > sizeof(input.text) - 1U) {
		length = sizeof(input.text) - 1U;
		while (length > 0U && ((unsigned char)text[length] & 0xc0U) == 0x80U)
			length--;
	}

	memcpy(input.text, text, length);
	input.text[length] = '\0';

	/* The cursor, in bytes: the start of the character that many code points in (the end when there are fewer). */
	input.begin = -1;
	input.end = -1;
	if (length > 0U && cursor >= 0) {
		at = 0U;
		for (position = 0; position < cursor && at < length; position++) {
			at++;
			while (at < length && ((unsigned char)input.text[at] & 0xc0U) == 0x80U)
				at++;
		}

		input.begin = (int32_t)at;
		input.end = (int32_t)at;
	}

	ime_composing = (length > 0U);
	(void)android_window_queue(window, &input);
}

/* Sends a key's press or release. */
static void
ime_key(
	struct kl_window *window,
	uint32_t code,
	int pressed)
{
	struct kl_window_event input;

	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_KEY;
	input.code = code;
	input.pressed = pressed;
	(void)android_window_queue(window, &input);
}
