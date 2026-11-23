/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: the text an input method types into an
 * X window.
 *
 * Keys reach X by their place on the keyboard (input.c), and Windows does
 * not translate them; only the keys an input method took (VK_PROCESSKEY)
 * and the characters sent as keys (VK_PACKET: the touch keyboard, the
 * emoji panel) are translated, so the input method composes and Windows
 * makes their characters.  The characters an input method settled on
 * (WM_IME_COMPOSITION's result string) and the characters of WM_CHAR are
 * given to the core one code point at a time, which types each through a
 * keycode it lends to the character's keysym.  The text not yet settled
 * is the input method's own window's, placed where the pointer is in the
 * X window, or at the window's lower left: X tells the server nothing of
 * a client's text cursor without an input method protocol.
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/scale.h"

#include <imm.h>
#include <stdlib.h>

/* The flags of WM_IME_COMPOSITION that carry the settled text, which the backend takes itself. */
#define IME_RESULT_FLAGS	(GCS_RESULTSTR | GCS_RESULTCLAUSE | GCS_RESULTREADSTR | GCS_RESULTREADCLAUSE)

/* The UTF-16 surrogates: the first and last high (leading) unit, the first and last low (trailing) one. */
#define IME_HIGH_FIRST		0xD800U
#define IME_HIGH_LAST		0xDBFFU
#define IME_LOW_FIRST		0xDC00U
#define IME_LOW_LAST		0xDFFFU
#define IME_PLANE_BASE		0x10000U

/* The height of a line of the unsettled text that the window's lower left leaves room for (X's pixels). */
#define IME_LINE_HEIGHT		24

static void ime_result(struct x11_surface *surface);
static void ime_type(struct x11_surface *surface, uint32_t character);
static void ime_type_units(struct x11_surface *surface, const WCHAR *units, size_t count);

/*
 * Tells whether a window message is to be translated (TranslateMessage):
 * a key an input method took, and a character sent as a key.  Other keys
 * are not, so that Windows' dead-key state stays untouched.
 */
int
win32_ime_translates(
	const MSG *message)
{
	/* Key messages only. */
	if (message->message != WM_KEYDOWN &&
	    message->message != WM_KEYUP &&
	    message->message != WM_SYSKEYDOWN &&
	    message->message != WM_SYSKEYUP)
		return 0;

	/* A key the input method took: it composes on translation. */
	if (message->wParam == VK_PROCESSKEY)
		return 1;

	/* A character sent as a key: translation makes its WM_CHAR. */
	if (message->wParam == VK_PACKET)
		return 1;

	/* Any other key reaches X by its place on the keyboard. */
	return 0;
}

/*
 * Places the input method's windows when it starts composing in an X
 * window (WM_IME_STARTCOMPOSITION): the unsettled text where the pointer
 * is in the window, or at the window's lower left, and the candidates
 * below it.
 */
void
win32_ime_place(
	struct x11_surface *surface)
{
	struct x11_win32 *backend;
	COMPOSITIONFORM composition;
	CANDIDATEFORM candidate;
	RECT client;
	POINT point;
	HIMC context;
	int line;

	/* The window's input context (none: the input method is off for it). */
	backend = surface->backend;
	context = ImmGetContext(surface->hwnd);
	if (context == NULL)
		return;

	/* The place: the pointer's in this window, else the lower left with room for a line. */
	line = x11_win32_scale_to_screen(IME_LINE_HEIGHT, backend->scale);
	if (backend->pointer_surface == surface) {
		point.x = x11_win32_scale_to_screen(backend->pointer_x - surface->x, backend->scale);
		point.y = x11_win32_scale_to_screen(backend->pointer_y - surface->y, backend->scale);
	} else {
		GetClientRect(surface->hwnd, &client);
		point.x = 0;
		point.y = client.bottom - line;
		if (point.y < 0)
			point.y = 0;
	}

	/* The unsettled text at the place. */
	composition.dwStyle = CFS_POINT;
	composition.ptCurrentPos = point;
	SetRectEmpty(&composition.rcArea);
	ImmSetCompositionWindow(context, &composition);

	/* The candidates a line below it. */
	candidate.dwIndex = 0;
	candidate.dwStyle = CFS_CANDIDATEPOS;
	candidate.ptCurrentPos.x = point.x;
	candidate.ptCurrentPos.y = point.y + line;
	SetRectEmpty(&candidate.rcArea);
	ImmSetCandidateWindow(context, &candidate);

	/* Succeeded: the context goes back to the window. */
	ImmReleaseContext(surface->hwnd, context);
}

/*
 * Answers WM_IME_COMPOSITION of an X window: the settled text (its result
 * string) is typed into X, and the rest goes to Windows without the
 * result's flags, so that the default window does not make WM_IME_CHAR of
 * the same text.  Returns the message's answer.
 */
LRESULT
win32_ime_composition(
	struct x11_surface *surface,
	WPARAM wparam,
	LPARAM lparam)
{
	LRESULT answer;

	/* Only the unsettled text changed: the default window shows it. */
	if ((lparam & GCS_RESULTSTR) == 0) {
		answer = DefWindowProcW(surface->hwnd, WM_IME_COMPOSITION, wparam, lparam);
		return answer;
	}

	/* The settled text is typed into X. */
	ime_result(surface);

	/* Nothing but the result: the message is answered. */
	lparam &= ~(LPARAM)IME_RESULT_FLAGS;
	if (lparam == 0)
		return 0;

	/* Succeeded: the unsettled text that came with it is the default window's. */
	answer = DefWindowProcW(surface->hwnd, WM_IME_COMPOSITION, wparam, lparam);
	return answer;
}

/*
 * Types the character of a WM_CHAR (one UTF-16 unit: a character sent as
 * a key, or one an input method sent by WM_IME_CHAR): a high surrogate
 * waits for its low one, and a low one without it is dropped.
 */
void
win32_ime_char(
	struct x11_surface *surface,
	WPARAM wparam)
{
	struct x11_win32 *backend;
	uint32_t unit;
	uint32_t character;

	/* The unit. */
	backend = surface->backend;
	unit = (uint32_t)(wparam & 0xFFFFU);

	/* A high surrogate waits for its pair. */
	if (unit >= IME_HIGH_FIRST && unit <= IME_HIGH_LAST) {
		backend->text_high = (uint16_t)unit;
		return;
	}

	/* A low surrogate completes its pair, or is dropped alone. */
	if (unit >= IME_LOW_FIRST && unit <= IME_LOW_LAST) {
		if (backend->text_high == 0)
			return;
		character = IME_PLANE_BASE + (((uint32_t)backend->text_high - IME_HIGH_FIRST) << 10) + (unit - IME_LOW_FIRST);
		backend->text_high = 0;
		ime_type(surface, character);
		return;
	}

	/* Succeeded: a character of one unit. */
	backend->text_high = 0;
	ime_type(surface, unit);
}

/* Reads the settled text from the window's input context and types it. */
static void
ime_result(
	struct x11_surface *surface)
{
	HIMC context;
	LONG size;
	LONG read;
	WCHAR *units;

	/* The window's input context. */
	context = ImmGetContext(surface->hwnd);
	if (context == NULL)
		return;

	/* The text's size in bytes (none: nothing settled). */
	size = ImmGetCompositionStringW(context, GCS_RESULTSTR, NULL, 0);
	if (size <= 0) {
		ImmReleaseContext(surface->hwnd, context);
		return;
	}

	/* Room for it. */
	units = malloc((size_t)size);
	if (units == NULL) {
		ImmReleaseContext(surface->hwnd, context);
		return;
	}

	/* The text, typed. */
	read = ImmGetCompositionStringW(context, GCS_RESULTSTR, units, (DWORD)size);
	if (read > 0)
		ime_type_units(surface, units, (size_t)read / sizeof(WCHAR));

	/* Succeeded: the text and the context are let go. */
	free(units);
	ImmReleaseContext(surface->hwnd, context);
}

/* Types a run of UTF-16 units, character by character (a lone surrogate is dropped). */
static void
ime_type_units(
	struct x11_surface *surface,
	const WCHAR *units,
	size_t count)
{
	size_t index;
	uint32_t unit;
	uint32_t next;
	uint32_t character;

	/* Each unit, or each surrogate pair. */
	index = 0;
	while (index < count) {
		unit = units[index];
		index++;

		/* A unit that is not a surrogate is the character. */
		if (unit < IME_HIGH_FIRST || unit > IME_LOW_LAST) {
			ime_type(surface, unit);
			continue;
		}

		/* A high surrogate and the low one after it make one character; anything else is dropped. */
		if (unit > IME_HIGH_LAST || index >= count)
			continue;
		next = units[index];
		if (next < IME_LOW_FIRST || next > IME_LOW_LAST)
			continue;
		index++;
		character = IME_PLANE_BASE + ((unit - IME_HIGH_FIRST) << 10) + (next - IME_LOW_FIRST);
		ime_type(surface, character);
	}
}

/* Gives one character to the core, which types it into the focused X window. */
static void
ime_type(
	struct x11_surface *surface,
	uint32_t character)
{
	struct x11_win32 *backend;
	DWORD time;

	/* No core yet. */
	backend = surface->backend;
	if (backend->server == NULL)
		return;

	/* Succeeded: the core types it (the log says that text came, never what it is). */
	time = (DWORD)GetMessageTime();
	win32_log(backend, "X11 WIN32 text window=0x%lx", (unsigned long)surface->window);
	x11server_backend_text(backend->server, character, (uint32_t)time);
}
