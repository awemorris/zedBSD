/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard on Windows: text as CF_UNICODETEXT (UTF-16 with its NUL,
 * in GMEM_MOVEABLE memory the clipboard then owns), converted from and to
 * UTF-8 with wide.h.  The window's frame owns what it copies.
 *
 * Windows has no primary selection (the text last selected, pasted with
 * the middle button).  The application keeps its own: what one of its
 * windows selects is the primary selection of all of them, which the
 * others hear of (KL_WINDOW_SELECTION); other programs neither see it nor
 * set it.
 */

#include "apps/libkeiland/win32/window-win32.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Copies text to the clipboard.
 */
void
kl_window_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	/* A window and text. */
	if (window == NULL || (text == NULL && length != 0U))
		return;

	/* The text, put on the clipboard. */
	(void)keiui_win32_clipboard_put(window, text == NULL ? "" : text, length);
}

/*
 * Reads the clipboard's text into text (size bytes, NUL-terminated).
 * Returns its length, 0 when it has none.
 */
size_t
kl_window_paste(
	struct kl_window *window,
	char *text,
	size_t size)
{
	size_t length;

	/* A window and room. */
	if (window == NULL || text == NULL || size == 0U)
		return 0U;

	/* The text. */
	length = keiui_win32_clipboard_get(window, text, size);
	return length;
}

/*
 * Reports whether the clipboard has text.
 */
int
kl_window_can_paste(
	const struct kl_window *window)
{
	BOOL available;

	/* A window. */
	if (window == NULL)
		return 0;

	/* Text on the clipboard. */
	available = IsClipboardFormatAvailable(CF_UNICODETEXT);
	if (!available)
		return 0;
	return 1;
}

/*
 * Makes the selected text the application's primary selection (its own
 * copy); without memory the old one stays.
 */
void
kl_window_select(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	struct kl_window_event *input;
	struct kl_window *other;
	struct kl_app *app;
	char *copy;

	/* A window, and text. */
	if (window == NULL ||
	    (text == NULL &&
	     length != 0U))
		return;
	app = window->app;

	/* The copy, which replaces the old one. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return;
	if (length != 0U)
		memcpy(copy, text, length);
	copy[length] = '\0';
	free(app->primary);
	app->primary = copy;
	app->primary_length = length;

	/* The application's other windows hear that it changed, and that it has text. */
	for (other = app->windows; other != NULL; other = other->next) {
		if (other == window)
			continue;

		/* The change, as the compositor would tell it. */
		input = keiui_win32_push(other, KL_WINDOW_SELECTION);
		if (input == NULL)
			continue;
		input->code = KL_SELECTION_PRIMARY;
		input->pressed = 1;
	}
}

/*
 * Reads the application's primary selection into text (size bytes, not
 * ended with a NUL).  Returns its length, 0 when there is none.
 */
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
	length = window->app->primary_length;
	if (length > size)
		length = size;
	if (length != 0U)
		memcpy(text, window->app->primary, length);

	/* Succeeded: the bytes copied. */
	return length;
}

/*
 * Tells whether the window's own text is a selection (KL_SELECTION_*): the
 * primary selection always is (it is the application's), the clipboard
 * when the window's frame put the text there last.
 */
int
kl_window_selection_own(
	const struct kl_window *window,
	unsigned which)
{
	HWND owner;

	/* A window. */
	if (window == NULL)
		return 0;

	/* The application's own primary selection. */
	if (which == KL_SELECTION_PRIMARY)
		return 1;

	/* The clipboard's owner. */
	owner = GetClipboardOwner();
	if (owner != window->frame)
		return 0;

	/* Succeeded: the window copied what the clipboard has. */
	return 1;
}

/* Puts UTF-8 text on the clipboard as CF_UNICODETEXT; 0, or an errno value. */
int
keiui_win32_clipboard_put(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	HGLOBAL memory;
	HANDLE given;
	uint16_t *units;
	size_t count;
	int error;
	BOOL opened;

	/* The UTF-16 form, in memory the clipboard can own. */
	memory = GlobalAlloc(GMEM_MOVEABLE, (length + 1U) * sizeof(uint16_t));
	if (memory == NULL)
		return ENOMEM;
	units = GlobalLock(memory);
	if (units == NULL) {
		(void)GlobalFree(memory);
		return ENOMEM;
	}

	/* Converts the text into the memory. */
	error = keiui_win32_utf16_from_utf8(text, length, units, length + 1U, &count);
	(void)GlobalUnlock(memory);
	if (error != 0) {
		(void)GlobalFree(memory);
		return error;
	}

	/* The clipboard, emptied and given the text (it owns the memory then). */
	opened = OpenClipboard(window->frame);
	if (!opened) {
		(void)GlobalFree(memory);
		return EBUSY;
	}

	/* Empties the clipboard. */
	(void)EmptyClipboard();
	given = SetClipboardData(CF_UNICODETEXT, memory);
	(void)CloseClipboard();
	if (given == NULL) {
		(void)GlobalFree(memory);
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Reads the clipboard's CF_UNICODETEXT as UTF-8 (whole characters that fit); its length, 0 without text. */
size_t
keiui_win32_clipboard_get(
	struct kl_window *window,
	char *text,
	size_t size)
{
	const uint16_t *units;
	HANDLE memory;
	SIZE_T bytes;
	size_t count;
	BOOL opened;
	size_t result;

	/* Nothing yet. */
	text[0] = '\0';
	opened = OpenClipboard(window->frame);
	if (!opened)
		return 0U;

	/* The text, converted. */
	memory = GetClipboardData(CF_UNICODETEXT);
	if (memory != NULL) {
		units = GlobalLock(memory);
		if (units != NULL) {
			bytes = GlobalSize(memory);
			count = (size_t)bytes / sizeof(uint16_t);
			(void)keiui_win32_utf8_from_utf16(units, count, text, size, NULL);
			(void)GlobalUnlock(memory);
		}
	}

	/* Closes the clipboard. */
	(void)CloseClipboard();

	/* Its length. */
	result = strlen(text);
	return result;
}
