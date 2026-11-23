/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: X's windows as Win32 windows.
 *
 * A program that runs an X server on Windows makes the backend, gives its
 * table (x11_win32_ops) and the backend to x11server_create, and hands
 * the server back with x11_win32_attach.  The backend opens a Windows
 * window for each top-level X window (a framed window, or a borderless
 * one that is never activated for an override-redirect menu or tooltip),
 * or one framed window for the whole root window of a rooted server, shows the X pixels in it through GDI or gives the core a Vulkan surface
 * of it, scales X's pixels to the screen's, turns the keyboard and the
 * pointer into X's input, and passes the clipboard across.
 *
 * The program's thread that made the backend runs the loop: it calls the
 * core (x11server_tick, its transport) and then x11_win32_wait, which
 * waits for the next window message, one of the program's handles or the
 * time the core asked for, and dispatches the messages.  The windows
 * belong to that thread.  While the user moves or sizes a window, Windows
 * runs a loop of its own; the backend then calls the program's service
 * function on a timer, so the core and the transport keep going.
 *
 * The program sets the process's DPI awareness to per monitor (v2) before
 * making the backend (mikuterm's manifest does).
 */

#ifndef KEILAND_X11_WIN32_BACKEND_H
#define KEILAND_X11_WIN32_BACKEND_H

#include "userland/desktop/xserver/win32/keysyms.h"
#include "userland/desktop/xserver/x11backend.h"
#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/* The most windows one backend opens (a remote host cannot fill the desktop). */
#define X11_WIN32_SURFACES_MAX		256U

/* How long after a paste or copy in an X window an untrusted client may read or set the clipboard (ms). */
#define X11_WIN32_CLIPBOARD_GRACE	2000U

/* The most handles x11_win32_wait waits for besides the messages. */
#define X11_WIN32_HANDLES_MAX		32U

/* What x11_win32_wait reports besides a handle's index: the time ran out, or the thread was asked to quit. */
#define X11_WIN32_WAIT_TIMEOUT		0xFFFFFFFFU
#define X11_WIN32_WAIT_QUIT		0xFFFFFFFEU

/*
 * One backend: the Windows windows of one X server.
 */
struct x11_win32;

/*
 * What a backend is made with.
 *
 * host is the remote host's name (UTF-8), put in brackets in front of
 * every title so a remote window cannot pass for one of this computer
 * (NULL: none).  scale_percent is the scale (100 to 400 in steps of 25; 0
 * takes the main monitor's).  trusted lets the remote clients read and set
 * the clipboard at any time; otherwise only just after a paste or a copy
 * in one of their windows.  primary_to_clipboard also makes X's PRIMARY
 * selection the Windows clipboard.  rooted says the server is rooted: its
 * root window is one window, whose size the user changes and the root
 * window's follows (one monitor, the whole of it), instead of the virtual
 * screen with the system's monitors on it.  service is called (with
 * service_context) on a timer while Windows runs its own loop; log gets
 * the backend's log lines (NULL: none).
 */
struct x11_win32_options {
	const char *host;
	unsigned scale_percent;
	int trusted;
	int primary_to_clipboard;
	int rooted;
	void (*service)(void *context);
	void *service_context;
	void (*log)(void *context, const char *line);
	void *log_context;
};

/* The backend's functions, for x11server_options.backend (the context is the struct x11_win32). */
extern const struct x11_backend_ops x11_win32_ops;

int x11_win32_create(const struct x11_win32_options *options, struct x11_win32 **result);
void x11_win32_attach(struct x11_win32 *backend, struct x11server *server);
void x11_win32_destroy(struct x11_win32 *backend);
unsigned x11_win32_wait(struct x11_win32 *backend, void *const *handles, unsigned count, uint32_t timeout);
unsigned x11_win32_scale(const struct x11_win32 *backend);
void x11_win32_root_size(const struct x11_win32 *backend, unsigned *width, unsigned *height);
const struct x11_win32_keymap *x11_win32_keymap(const struct x11_win32 *backend);
void *x11_win32_window_handle(const struct x11_surface *surface);

#endif
