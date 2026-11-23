/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: X's CLIPBOARD (and, when asked, its
 * PRIMARY) and the Windows clipboard's text.
 *
 * X to Windows: when an X client takes the selection, the backend asks
 * the core for its text at once and puts it on the Windows clipboard, so
 * it stays there after the client ends.  Windows to X: a hidden window
 * hears when another program changes the clipboard and tells the core,
 * which reads the text when an X client asks for it.
 *
 * A client that is not trusted reaches the clipboard only just after the
 * user pasted (reading) or copied (setting) in one of the server's
 * windows (X11_WIN32_CLIPBOARD_GRACE), so a remote host cannot read or
 * change it behind the user's back.
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The class of the hidden window. */
#define CLIPBOARD_CLASS_NAME	L"zedBSD.X11Clipboard"

/* How often and how long the clipboard is tried while another program holds it open. */
#define CLIPBOARD_TRIES		5
#define CLIPBOARD_PAUSE_MS	2U

/* The longest text passed either way (16 MiB). */
#define CLIPBOARD_TEXT_MAX	(16U * 1024U * 1024U)

static LRESULT CALLBACK clipboard_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static int clipboard_open(struct x11_win32 *backend);
static int clipboard_allowed(struct x11_win32 *backend, int paste);

/*
 * Opens the hidden window that hears of the Windows clipboard.  Returns
 * 0, or an errno value.
 */
int
win32_clipboard_start(
	struct x11_win32 *backend)
{
	WNDCLASSEXW window_class;
	BOOL listening;

	/* Registers the class of the hidden window. */
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = clipboard_procedure;
	window_class.hInstance = backend->instance;
	window_class.lpszClassName = CLIPBOARD_CLASS_NAME;
	backend->message_class = RegisterClassExW(&window_class);
	if (backend->message_class == 0)
		return EIO;

	/* Makes the window, which only receives messages. */
	backend->message_window = CreateWindowExW(0, CLIPBOARD_CLASS_NAME, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
	    backend->instance, NULL);
	if (backend->message_window == NULL)
		return EIO;
	SetWindowLongPtrW(backend->message_window, GWLP_USERDATA, (LONG_PTR)backend);

	/* Listens for the clipboard's changes. */
	listening = AddClipboardFormatListener(backend->message_window);
	if (!listening)
		return EIO;

	/* Succeeded: the clipboard's changes reach the backend. */
	backend->clipboard_sequence = GetClipboardSequenceNumber();
	return 0;
}

/*
 * Closes the hidden window and unregisters its class.
 */
void
win32_clipboard_stop(
	struct x11_win32 *backend)
{
	/* Stops listening and closes the window. */
	if (backend->message_window != NULL) {
		RemoveClipboardFormatListener(backend->message_window);
		SetWindowLongPtrW(backend->message_window, GWLP_USERDATA, 0);
		DestroyWindow(backend->message_window);
		backend->message_window = NULL;
	}

	/* Unregisters the class. */
	if (backend->message_class != 0) {
		UnregisterClassW(CLIPBOARD_CLASS_NAME, backend->instance);
		backend->message_class = 0;
	}
}

/*
 * Follows a change of the Windows clipboard: another program's change
 * tells the core whether there is text (the backend's own change is not
 * told back).
 */
void
win32_clipboard_changed(
	struct x11_win32 *backend)
{
	DWORD sequence;
	BOOL text;

	/* The backend's own change. */
	sequence = GetClipboardSequenceNumber();
	if (sequence == backend->clipboard_sequence)
		return;

	/* Tells the core whether another program's text is there. */
	text = IsClipboardFormatAvailable(CF_UNICODETEXT);
	win32_log(backend, "X11 WIN32 clipboard changed text=%d", (int)(text != FALSE));
	if (backend->server != NULL)
		x11server_backend_clipboard(backend->server, X11_SELECTION_CLIPBOARD, text != FALSE);
}

/*
 * Follows an X client's taking or giving up of a selection (the
 * backend's clipboard_own).
 *
 * Taking CLIPBOARD (or PRIMARY when asked) asks the core for the owner's
 * text at once (x11server_backend_clipboard_request), which comes back
 * through clipboard_answer and goes on the Windows clipboard.  A client
 * that is not trusted is asked only just after a copy.  Giving the
 * selection up leaves the Windows clipboard as it is.  Returns 0.
 */
int
win32_clipboard_own(
	void *context,
	unsigned selection,
	int own)
{
	struct x11_win32 *backend;
	int allowed;

	/* PRIMARY stays in X unless it is to be the Windows clipboard too. */
	backend = context;
	if (selection == X11_SELECTION_PRIMARY && !backend->options.primary_to_clipboard)
		return 0;

	/* Giving the selection up leaves the text on the Windows clipboard. */
	if (!own)
		return 0;

	/* An untrusted client sets the clipboard only just after a copy. */
	allowed = clipboard_allowed(backend, 0);
	if (!allowed) {
		win32_log(backend, "X11 WIN32 clipboard set refused (no copy just before)");
		return 0;
	}

	/* Asks the core for the owner's text, under a new request number. */
	backend->clipboard_request++;
	if (backend->clipboard_request == 0)
		backend->clipboard_request = 1;
	backend->clipboard_selection = selection;
	if (backend->server != NULL)
		x11server_backend_clipboard_request(backend->server, selection, backend->clipboard_request);

	/* Succeeded: the text comes through clipboard_answer. */
	return 0;
}

/*
 * Tells whether another program's text is on the Windows clipboard (the
 * backend's clipboard_has_text); PRIMARY never has Windows' text.
 */
int
win32_clipboard_has_text(
	void *context,
	unsigned selection)
{
	struct x11_win32 *backend;
	DWORD sequence;
	BOOL text;

	/* Windows has no primary selection. */
	backend = context;
	if (selection == X11_SELECTION_PRIMARY)
		return 0;

	/* The backend's own text is the X owner's, not another program's. */
	sequence = GetClipboardSequenceNumber();
	if (sequence == backend->clipboard_sequence)
		return 0;

	/* Succeeded: whether there is text. */
	text = IsClipboardFormatAvailable(CF_UNICODETEXT);
	if (text)
		return 1;

	return 0;
}

/*
 * Reads another program's text of the Windows clipboard for an X client
 * (the backend's clipboard_read): UTF-8 with LF line ends, malloc'd with
 * a NUL after it, which the core frees.  A client that is not trusted
 * reads only just after a paste.  Returns 0, EACCES, ENOENT (no text),
 * EBUSY (another program holds the clipboard) or ENOMEM.
 */
int
win32_clipboard_read(
	void *context,
	unsigned selection,
	uint8_t **text,
	size_t *size)
{
	struct x11_win32 *backend;
	HANDLE data;
	const uint16_t *units;
	size_t count;
	size_t needed;
	uint8_t *bytes;
	int allowed;
	int error;

	/* Nothing to read for PRIMARY. */
	backend = context;
	*text = NULL;
	*size = 0;
	if (selection == X11_SELECTION_PRIMARY)
		return ENOENT;

	/* An untrusted client reads only just after a paste. */
	allowed = clipboard_allowed(backend, 1);
	if (!allowed) {
		win32_log(backend, "X11 WIN32 clipboard read refused (no paste just before)");
		return EACCES;
	}

	/* Opens the clipboard and finds its text. */
	error = clipboard_open(backend);
	if (error != 0)
		return error;
	data = GetClipboardData(CF_UNICODETEXT);
	if (data == NULL) {
		CloseClipboard();
		return ENOENT;
	}
	units = GlobalLock(data);
	if (units == NULL) {
		CloseClipboard();
		return ENOENT;
	}
	count = GlobalSize(data) / sizeof(uint16_t);

	/* Measures it in UTF-8 and refuses a text too long to pass. */
	needed = x11_win32_utf16_to_utf8(units, count, 1, NULL, 0);
	if (needed > CLIPBOARD_TEXT_MAX) {
		GlobalUnlock(data);
		CloseClipboard();
		return ENOMEM;
	}

	/* Converts it. */
	bytes = malloc(needed + 1);
	if (bytes == NULL) {
		GlobalUnlock(data);
		CloseClipboard();
		return ENOMEM;
	}
	(void)x11_win32_utf16_to_utf8(units, count, 1, bytes, needed + 1);
	GlobalUnlock(data);
	CloseClipboard();

	/* Succeeded: the text is the core's. */
	win32_log(backend, "X11 WIN32 clipboard read bytes=%lu", (unsigned long)needed);
	*text = bytes;
	*size = needed;
	return 0;
}

/*
 * Puts an X owner's text on the Windows clipboard (the backend's
 * clipboard_answer to the request clipboard_own made): UTF-16 with CR LF
 * line ends.  An answer to an older request, or none, is dropped.
 * Returns the bytes taken.
 */
size_t
win32_clipboard_answer(
	void *context,
	uint32_t request,
	const uint8_t *text,
	size_t size)
{
	struct x11_win32 *backend;
	HGLOBAL data;
	uint16_t *units;
	size_t needed;
	HANDLE set;
	int error;

	/* Only the answer to the last request, with text, is taken. */
	backend = context;
	if (request != backend->clipboard_request || text == NULL)
		return 0;
	if (size > CLIPBOARD_TEXT_MAX)
		return 0;

	/* Converts the text into global memory, which the clipboard takes. */
	needed = x11_win32_utf8_to_utf16(text, size, 1, NULL, 0);
	data = GlobalAlloc(GMEM_MOVEABLE, (needed + 1) * sizeof(uint16_t));
	if (data == NULL)
		return 0;
	units = GlobalLock(data);
	if (units == NULL) {
		GlobalFree(data);
		return 0;
	}
	(void)x11_win32_utf8_to_utf16(text, size, 1, units, needed + 1);
	GlobalUnlock(data);

	/* Puts it on the clipboard, which then owns the memory. */
	error = clipboard_open(backend);
	if (error != 0) {
		GlobalFree(data);
		return 0;
	}
	EmptyClipboard();
	set = SetClipboardData(CF_UNICODETEXT, data);
	if (set == NULL) {
		CloseClipboard();
		GlobalFree(data);
		return 0;
	}
	CloseClipboard();

	/* Succeeded: the change is the backend's own, not told back to the core. */
	backend->clipboard_sequence = GetClipboardSequenceNumber();
	win32_log(backend, "X11 WIN32 clipboard set bytes=%lu", (unsigned long)size);
	return size;
}

/* The hidden window's procedure: the clipboard's changes. */
static LRESULT CALLBACK
clipboard_procedure(
	HWND hwnd,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	struct x11_win32 *backend;
	LRESULT answer;

	/* A change of the clipboard, once the backend is there. */
	backend = (struct x11_win32 *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (message == WM_CLIPBOARDUPDATE && backend != NULL) {
		win32_clipboard_changed(backend);
		return 0;
	}

	/* Everything else is Windows'. */
	answer = DefWindowProcW(hwnd, message, wparam, lparam);
	return answer;
}

/*
 * Opens the clipboard for the hidden window, trying a few times while
 * another program holds it.  Returns 0, or EBUSY.
 */
static int
clipboard_open(
	struct x11_win32 *backend)
{
	int tries;
	BOOL opened;

	/* Tries, pausing between the tries. */
	for (tries = 0; tries < CLIPBOARD_TRIES; tries++) {
		opened = OpenClipboard(backend->message_window);
		if (opened)
			return 0;

		Sleep(CLIPBOARD_PAUSE_MS);
	}

	/* Another program keeps it. */
	return EBUSY;
}

/*
 * Tells whether the clipboard may be read (paste nonzero) or set now: a
 * trusted server always, otherwise only within the grace after the user's
 * paste or copy in one of the server's windows.
 */
static int
clipboard_allowed(
	struct x11_win32 *backend,
	int paste)
{
	DWORD now;
	DWORD action;
	int noted;

	/* A trusted server reaches the clipboard at any time. */
	if (backend->options.trusted)
		return 1;

	/* The user's last paste or copy. */
	if (paste) {
		noted = backend->pasted;
		action = backend->paste_time;
	} else {
		noted = backend->copied;
		action = backend->copy_time;
	}
	if (!noted)
		return 0;

	/* Within the grace (the tick count wraps, the difference does not). */
	now = GetTickCount();
	if (now - action <= X11_WIN32_CLIPBOARD_GRACE)
		return 1;

	return 0;
}
