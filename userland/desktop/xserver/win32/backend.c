/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: its life, its table of functions and
 * the loop of window messages.
 */

#include "userland/desktop/xserver/win32/internal.h"
#include "userland/desktop/xserver/win32/scale.h"

#include <shellscalingapi.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest log line the backend writes. */
#define BACKEND_LOG_MAX		512U

/*
 * The backend's functions as the core calls them.
 *
 * The table is constant for the life of the program; every function takes
 * the struct x11_win32 as its context.
 */
const struct x11_backend_ops x11_win32_ops = {
	win32_surface_open,
	win32_surface_update,
	win32_surface_close,
	win32_surface_vulkan,
	win32_surface_present,
	win32_clipboard_offer,
	win32_clipboard_read,
	win32_input_capture,
	win32_input_place,
	win32_input_warp,
	win32_input_bell,
	win32_surface_cursor,
};

static unsigned backend_main_scale(void);

/*
 * Makes a backend: its window classes, its hidden window of the
 * clipboard, the scale, and the keyboard map of the thread's layout.
 * Returns 0 with *result set, or an errno value.  The calling thread owns
 * the windows and runs x11_win32_wait.
 */
int
x11_win32_create(
	const struct x11_win32_options *options,
	struct x11_win32 **result)
{
	struct x11_win32 *backend;
	size_t length;
	int error;

	/* Allocates the backend. */
	*result = NULL;
	backend = calloc(1, sizeof(*backend));
	if (backend == NULL)
		return ENOMEM;

	/* Keeps the options and its own copy of the host's name. */
	backend->options = *options;
	backend->options.host = NULL;
	if (options->host != NULL) {
		length = strlen(options->host);
		backend->host = malloc(length + 1);
		if (backend->host == NULL) {
			free(backend);
			return ENOMEM;
		}
		memcpy(backend->host, options->host, length + 1);
	}
	backend->instance = GetModuleHandleW(NULL);

	/* The scale: the one asked for, or the main monitor's. */
	backend->scale = options->scale_percent;
	if (backend->scale == 0)
		backend->scale = backend_main_scale();

	/* The root window covers the virtual screen, all monitors' bounding box. */
	backend->virtual_screen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	backend->virtual_screen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	backend->virtual_screen.right = backend->virtual_screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	backend->virtual_screen.bottom = backend->virtual_screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

	/* Registers the class of the X windows. */
	error = win32_window_register(backend);
	if (error != 0) {
		x11_win32_destroy(backend);
		return error;
	}

	/* Opens the hidden window that hears of the clipboard. */
	error = win32_clipboard_start(backend);
	if (error != 0) {
		x11_win32_destroy(backend);
		return error;
	}

	/* Reads the thread's keyboard layout into the map. */
	win32_layout_read(backend);

	/* Succeeded: the backend waits for its server. */
	win32_log(backend, "X11 WIN32 backend scale=%u root=%ldx%ld altgr=%d", backend->scale,
	    (long)(backend->virtual_screen.right - backend->virtual_screen.left),
	    (long)(backend->virtual_screen.bottom - backend->virtual_screen.top), backend->keymap.altgr);
	*result = backend;
	return 0;
}

/*
 * Gives the backend the server it shows, which the input and the
 * clipboard are passed to from then on.
 */
void
x11_win32_attach(
	struct x11_win32 *backend,
	struct x11server *server)
{
	/* The server hears the input from now on, and takes the layout's map. */
	backend->server = server;
	win32_layout_give(backend);

	/* It hears whether another program's text is already on the clipboard. */
	win32_clipboard_attach(backend);
}

/*
 * Ends a backend: its windows (which the core should have closed), its
 * hidden window, its classes and the Vulkan loader.
 */
void
x11_win32_destroy(
	struct x11_win32 *backend)
{
	/* Closes the windows the core left open. */
	while (backend->surfaces != NULL)
		win32_surface_close(backend, backend->surfaces);

	/* Closes the hidden window of the clipboard. */
	win32_clipboard_stop(backend);

	/* Unregisters the classes. */
	win32_window_unregister(backend);

	/* Lets the Vulkan loader go. */
	if (backend->vulkan_loader != NULL)
		FreeLibrary(backend->vulkan_loader);

	/* Frees the backend. */
	free(backend->host);
	free(backend);
}

/*
 * Waits for a window message, one of the handles (at most
 * X11_WIN32_HANDLES_MAX) or the timeout (ms; INFINITE waits for ever),
 * then dispatches every message waiting.
 *
 * Returns the index of a handle that was signaled, X11_WIN32_WAIT_TIMEOUT
 * when none was (messages or the time), or X11_WIN32_WAIT_QUIT when the
 * thread got WM_QUIT.  Only the keys an input method took and the
 * characters sent as keys are translated (ime.c): X clients make the
 * other keys' characters from the keysyms themselves, and the dead keys'
 * state of Windows must stay untouched.
 */
unsigned
x11_win32_wait(
	struct x11_win32 *backend,
	void *const *handles,
	unsigned count,
	uint32_t timeout)
{
	DWORD waited;
	MSG message;
	unsigned signaled;
	BOOL found;
	int translates;

	/* Refuses more handles than the wait takes. */
	(void)backend;
	if (count > X11_WIN32_HANDLES_MAX)
		count = X11_WIN32_HANDLES_MAX;

	/* Waits for a message, a handle or the time. */
	waited = MsgWaitForMultipleObjectsEx(count, (const HANDLE *)handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	signaled = X11_WIN32_WAIT_TIMEOUT;
	if (waited < WAIT_OBJECT_0 + count)
		signaled = (unsigned)(waited - WAIT_OBJECT_0);

	/* Dispatches every message waiting, stopping at WM_QUIT. */
	for (;;) {
		found = PeekMessageW(&message, NULL, 0, 0, PM_REMOVE);
		if (!found)
			break;

		if (message.message == WM_QUIT)
			return X11_WIN32_WAIT_QUIT;

		/* The input method's keys and the characters sent as keys are translated into its text. */
		translates = win32_ime_translates(&message);
		if (translates)
			TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	/* Succeeded: what ended the wait. */
	return signaled;
}

/*
 * Gives the backend's scale in percent.
 */
unsigned
x11_win32_scale(
	const struct x11_win32 *backend)
{
	/* The scale chosen at the start. */
	return backend->scale;
}

/*
 * Gives the root window's size in X's pixels: the virtual screen (all
 * monitors' bounding box) divided by the scale.
 */
void
x11_win32_root_size(
	const struct x11_win32 *backend,
	unsigned *width,
	unsigned *height)
{
	int32_t screen_width;
	int32_t screen_height;

	/* The virtual screen's size. */
	screen_width = backend->virtual_screen.right - backend->virtual_screen.left;
	screen_height = backend->virtual_screen.bottom - backend->virtual_screen.top;

	/* Divided by the scale. */
	*width = (unsigned)x11_win32_scale_to_x(screen_width, backend->scale);
	*height = (unsigned)x11_win32_scale_to_x(screen_height, backend->scale);
}

/*
 * Gives the keyboard map the backend made of the thread's layout: the
 * keysyms and modifiers of each X keycode (the evdev code plus 8).
 */
const struct x11_win32_keymap *
x11_win32_keymap(
	const struct x11_win32 *backend)
{
	/* The map of the layout last read. */
	return &backend->keymap;
}

/*
 * Gives the Windows window (HWND) of a surface.
 */
void *
x11_win32_window_handle(
	const struct x11_surface *surface)
{
	/* The window the surface opened. */
	return surface->hwnd;
}

/*
 * Writes a log line of the backend through the program's function, if it
 * gave one.
 */
void
win32_log(
	struct x11_win32 *backend,
	const char *format,
	...)
{
	char line[BACKEND_LOG_MAX];
	va_list arguments;

	/* Nothing to do without a log. */
	if (backend->options.log == NULL)
		return;

	/* Formats the line and hands it on. */
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	backend->options.log(backend->options.log_context, line);
}

/* Gives the scale of the main monitor's DPI (100 % when it cannot be read). */
static unsigned
backend_main_scale(void)
{
	POINT origin;
	HMONITOR monitor;
	UINT dpi_x;
	UINT dpi_y;
	HRESULT read;
	unsigned scale;

	/* The main monitor is the one at the origin. */
	origin.x = 0;
	origin.y = 0;
	monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

	/* Its effective DPI, the one the display settings' scale gives. */
	read = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
	if (FAILED(read))
		return X11_WIN32_SCALE_MIN;

	/* Succeeded: the scale of the DPI. */
	scale = x11_win32_scale_of_dpi(dpi_x);
	return scale;
}
