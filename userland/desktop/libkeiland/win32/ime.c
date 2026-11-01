/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input method on Windows through IMM32 (the first step; TSF comes
 * later).  While the program asks for text (kl_window_text_input) or the
 * title bar's field is edited, the frame has the input method's default
 * context (ImmAssociateContextEx); otherwise it has none.  The composition
 * is libkeiland's to show: the system's composition window is not shown
 * (WM_IME_SETCONTEXT without ISC_SHOWUICOMPOSITIONWINDOW, and
 * WM_IME_COMPOSITION not passed on), the text being composed is a
 * KL_WINDOW_TEXT_PREEDIT (its cursor in UTF-8 bytes) and the text
 * finished a KL_WINDOW_TEXT_COMMIT, or the field's while it is edited.
 * The candidates' window is put at the caret (kl_window_text_cursor's
 * rectangle, or the field's caret) and kept off it.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"

#include <imm.h>

#include <stdlib.h>
#include <string.h>

/* The longest composition read (UTF-16 units). */
#define IME_UNITS	1024U

static int ime_string(HIMC context, DWORD kind, char *text, size_t size, LONG cursor, size_t *offset);
static void ime_copy(char *to, size_t size, const char *from);

/*
 * Asks for text from the input method, or stops asking.
 */
void
kl_window_text_input(
	struct kl_window *window,
	int enabled)
{
	/* A window. */
	if (window == NULL)
		return;

	/* The wish, and the context that follows it. */
	window->text_wanted = enabled != 0;
	keiui_win32_ime_place(window);
}

/*
 * Tells where the caret is (the content's pixels), for the input method's
 * windows.
 */
void
kl_window_text_cursor(
	struct kl_window *window,
	int x,
	int y,
	int width,
	int height)
{
	/* A window. */
	if (window == NULL)
		return;

	/* The rectangle, and the windows put there. */
	window->text_caret.left = x;
	window->text_caret.top = y;
	window->text_caret.right = x + width;
	window->text_caret.bottom = y + height;
	keiui_win32_ime_place(window);
}

/*
 * Takes the text around the caret: kept for nothing yet (the surrounding
 * text is TSF's).
 */
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

/*
 * Gives the frame the input method's context when text is wanted (or the
 * field is edited), and puts its windows at the caret.
 */
void
keiui_win32_ime_place(
	struct kl_window *window)
{
	COMPOSITIONFORM composition;
	CANDIDATEFORM candidate;
	HIMC context;
	RECT caret;
	uint32_t caption;
	const struct keiui_win32_control *control;

	/* No text wanted: no context. */
	if (!window->text_wanted && !window->editing) {
		(void)ImmAssociateContextEx(window->frame, NULL, 0U);
		return;
	}
	(void)ImmAssociateContextEx(window->frame, NULL, IACE_DEFAULT);

	/* The caret in the frame's pixels: the field's, or the program's below the title bar. */
	caption = keiui_win32_caption_pixels(window);
	caret = window->text_caret;
	caret.top += (LONG)caption;
	caret.bottom += (LONG)caption;
	if (window->editing && window->field.control < window->control_count) {
		control = &window->controls[window->field.control];
		caret.left = control->x;
		caret.top = control->y;
		caret.right = control->x + (LONG)control->width;
		caret.bottom = control->y + (LONG)control->height;
	}

	/* The composition's and the candidates' windows there. */
	context = ImmGetContext(window->frame);
	if (context == NULL)
		return;
	memset(&composition, 0, sizeof(composition));
	composition.dwStyle = CFS_POINT;
	composition.ptCurrentPos.x = caret.left;
	composition.ptCurrentPos.y = caret.top;
	(void)ImmSetCompositionWindow(context, &composition);
	memset(&candidate, 0, sizeof(candidate));
	candidate.dwIndex = 0U;
	candidate.dwStyle = CFS_EXCLUDE;
	candidate.ptCurrentPos.x = caret.left;
	candidate.ptCurrentPos.y = caret.bottom;
	candidate.rcArea = caret;
	(void)ImmSetCandidateWindow(context, &candidate);
	(void)ImmReleaseContext(window->frame, context);
}

/*
 * Handles the input method's messages to the frame.  Returns 1 with
 * *result when it did (the message is not passed on), 0 to pass it on.
 */
int
keiui_win32_ime_message(
	struct kl_window *window,
	UINT message,
	WPARAM wparam,
	LPARAM lparam,
	LRESULT *result)
{
	struct kl_window_event *input;
	char *text;
	HIMC context;
	size_t offset;
	int found;

	(void)wparam;

	/* What the message is. */
	switch (message) {
	case WM_IME_SETCONTEXT:
		/* The system shows no composition window of its own. */
		*result = DefWindowProcW(window->frame, message, wparam, lparam & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
		return 1;
	case WM_IME_STARTCOMPOSITION:
		/* The composition starts at the caret. */
		keiui_win32_ime_place(window);
		*result = 0;
		return 1;
	case WM_IME_COMPOSITION:
		break;
	case WM_IME_ENDCOMPOSITION:
		/* The composing text goes. */
		if (window->editing) {
			keiui_win32_field_compose(window, "", 0U);
		} else {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
			if (input != NULL) {
				input->begin = -1;
				input->end = -1;
			}
		}
		*result = 0;
		return 1;
	default:
		return 0;
	}

	/* The composition's text: finished, then being composed. */
	text = malloc(KEIUI_WIN32_CONTROL_TEXT);
	context = ImmGetContext(window->frame);
	if (text == NULL || context == NULL) {
		free(text);
		if (context != NULL)
			(void)ImmReleaseContext(window->frame, context);
		*result = 0;
		return 1;
	}
	if (lparam & GCS_RESULTSTR) {
		found = ime_string(context, GCS_RESULTSTR, text, KEIUI_WIN32_CONTROL_TEXT, -1, &offset);
		if (found && window->editing) {
			keiui_win32_field_insert(window, text);
		} else if (found) {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_COMMIT);
			if (input != NULL)
				ime_copy(input->text, sizeof(input->text), text);
		}
	}
	if (lparam & GCS_COMPSTR) {
		found = ime_string(context, GCS_COMPSTR, text, KEIUI_WIN32_CONTROL_TEXT,
		    (lparam & GCS_CURSORPOS) ? ImmGetCompositionStringW(context, GCS_CURSORPOS, NULL, 0U) : -1, &offset);
		if (!found)
			text[0] = '\0';
		if (window->editing) {
			keiui_win32_field_compose(window, text, offset);
		} else {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
			if (input != NULL) {
				ime_copy(input->text, sizeof(input->text), text);
				input->begin = (int32_t)offset;
				input->end = (int32_t)offset;
			}
		}
	}
	(void)ImmReleaseContext(window->frame, context);
	free(text);

	/* Handled: the system's composition window stays away. */
	*result = 0;
	return 1;
}

/* One of the composition's strings as UTF-8, and a cursor (UTF-16 units, -1 for none) as its UTF-8 offset; 1 when read. */
static int
ime_string(
	HIMC context,
	DWORD kind,
	char *text,
	size_t size,
	LONG cursor,
	size_t *offset)
{
	uint16_t units[IME_UNITS];
	LONG bytes;
	size_t count;
	size_t consumed;
	int error;

	/* The string's units. */
	text[0] = '\0';
	*offset = 0U;
	bytes = ImmGetCompositionStringW(context, kind, units, sizeof(units));
	if (bytes < 0)
		return 0;
	count = (size_t)bytes / sizeof(units[0]);

	/* The cursor's offset: the UTF-8 of the units before it. */
	if (cursor >= 0 && (size_t)cursor <= count) {
		error = keiui_win32_utf8_from_utf16(units, (size_t)cursor, text, size, &consumed);
		if (error == 0)
			*offset = strlen(text);
	}

	/* The whole string. */
	(void)keiui_win32_utf8_from_utf16(units, count, text, size, &consumed);
	if (cursor < 0)
		*offset = strlen(text);
	return 1;
}

/* Copies UTF-8 text, as many whole characters as fit. */
static void
ime_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* As much as fits, not cutting a character. */
	length = strlen(from);
	if (length + 1U > size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}
	memcpy(to, from, length);
	to[length] = '\0';
}
