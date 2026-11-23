/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard on Android: text.
 *
 * The program's paste (kl_window_paste) asks for the text and gets it at once,
 * but Android lets only an app that has the focus read the clipboard, and
 * only through Java.  So Java reads it whenever the window gains the focus
 * and whenever the clipboard changes, and tells it through the bridge
 * (android_clipboard_bridge); a paste is answered from the last text told,
 * which is also what a copy of this program set.  A copy is passed to Java to
 * put on the clipboard.
 *
 * Android has no primary selection (the text last selected, pasted with the
 * middle button).  The program keeps its own, as the Windows backend does: what
 * one of its windows selects is the primary selection of all of them, which the
 * others hear of (KL_WINDOW_SELECTION).
 *
 * What was on the clipboard may be a private key, so a text is overwritten
 * with zeros before its memory is given back.
 */

#include "apps/libkeiland/android/android.h"

#include "apps/libkeiland/android/bridge-jni.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The clipboard's text as last told or copied (NULL when it is empty), and the program's primary selection. */
static char *clipboard_text;
static size_t clipboard_length;
static char *clipboard_primary;
static size_t clipboard_primary_length;

/* Writes zeros through a pointer the compiler cannot see through (the C library has no explicit_bzero). */
static void *(*volatile clipboard_memset)(void *, int, size_t) = memset;

static int clipboard_keep(char **kept, size_t *kept_length, const char *text, size_t length);
static size_t clipboard_copy_out(const char *kept, size_t kept_length, char *text, size_t size);

/* Copies text to the clipboard (Java puts it there; this program's paste sees it at once). */
void
kl_window_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	int error;

	/* A window and text. */
	if (window == NULL || (text == NULL && length != 0U))
		return;

	/* The program's copy, then the system's. */
	error = clipboard_keep(&clipboard_text, &clipboard_length, text, length);
	if (error != 0)
		return;
	(void)keiland_java_clipboard_set(clipboard_text, clipboard_length);
}

/* Reads the clipboard's text into text (size bytes, NUL-terminated, whole characters).  Returns its length, 0 when there is none. */
size_t
kl_window_paste(
	struct kl_window *window,
	char *text,
	size_t size)
{
	/* A window and room. */
	if (window == NULL || text == NULL || size == 0U)
		return 0U;

	/* The text last told. */
	return clipboard_copy_out(clipboard_text, clipboard_length, text, size - 1U);
}

/* Says whether the clipboard has text. */
int
kl_window_can_paste(
	const struct kl_window *window)
{
	/* A window, and the text last told. */
	if (window == NULL)
		return 0;
	return clipboard_length != 0U;
}

/* Makes the selected text the program's primary selection; without memory the old one stays. */
void
kl_window_select(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	struct kl_window_event input;
	struct kl_window *other;
	int error;

	/* A window, and text. */
	if (window == NULL || (text == NULL && length != 0U))
		return;

	/* The copy, which replaces the old one. */
	error = clipboard_keep(&clipboard_primary, &clipboard_primary_length, text, length);
	if (error != 0)
		return;

	/* The program's other windows hear that it changed, and that it has text. */
	if (window->app == NULL)
		return;
	for (other = window->app->windows; other != NULL; other = other->next) {
		if (other == window)
			continue;
		memset(&input, 0, sizeof(input));
		input.kind = KL_WINDOW_SELECTION;
		input.code = KL_SELECTION_PRIMARY;
		input.pressed = (length != 0U);
		(void)android_window_queue(other, &input);
	}
}

/* Reads the program's primary selection into text (size bytes, not ended with a NUL).  Returns its length, 0 when there is none. */
size_t
kl_window_paste_primary(
	struct kl_window *window,
	char *text,
	size_t size)
{
	size_t length;

	/* A window and room. */
	if (window == NULL || text == NULL)
		return 0U;

	/* As much of the selection as fits. */
	length = clipboard_primary_length;
	if (length > size)
		length = size;
	if (length != 0U)
		memcpy(text, clipboard_primary, length);
	return length;
}

/*
 * Tells whether the window's own text is a selection: the primary selection
 * always is (it is the program's), the clipboard when this program copied
 * what it has (the text told by Java is another program's, or this one's).
 */
int
kl_window_selection_own(
	const struct kl_window *window,
	unsigned which)
{
	/* A window. */
	if (window == NULL)
		return 0;

	/* The program's own primary selection; the clipboard's owner is the system's. */
	return which == KL_SELECTION_PRIMARY;
}

/* Keeps the clipboard's text the system told (a NULL text is an empty clipboard). */
void
android_clipboard_bridge(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	const char *text;
	size_t length;

	(void)app;

	/* The text told; none empties the clipboard. */
	text = event->text;
	length = text != NULL ? strlen(text) : 0U;
	(void)clipboard_keep(&clipboard_text, &clipboard_length, text, length);
}

/* Replaces a kept text with a copy of the text, wiping the old one.  Returns 0 or ENOMEM (the old one stays). */
static int
clipboard_keep(
	char **kept,
	size_t *kept_length,
	const char *text,
	size_t length)
{
	char *copy;

	/* The copy, ended by a NUL (none for an empty text). */
	copy = NULL;
	if (length != 0U) {
		copy = malloc(length + 1U);
		if (copy == NULL)
			return ENOMEM;
		memcpy(copy, text, length);
		copy[length] = '\0';
	}

	/* The old text goes, wiped. */
	if (*kept != NULL) {
		(void)clipboard_memset(*kept, 0, *kept_length);
		free(*kept);
	}
	*kept = copy;
	*kept_length = length;
	return 0;
}

/* Copies a kept text into text (at most room bytes, never half a character) and ends it with a NUL.  Returns the length copied. */
static size_t
clipboard_copy_out(
	const char *kept,
	size_t kept_length,
	char *text,
	size_t room)
{
	size_t length;

	/* As much as fits, cut at a character's start. */
	length = kept_length;
	if (length > room) {
		length = room;
		while (length > 0U && ((unsigned char)kept[length] & 0xc0U) == 0x80U)
			length--;
	}
	if (length != 0U)
		memcpy(text, kept, length);
	text[length] = '\0';
	return length;
}
