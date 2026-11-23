/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xserver's core: an X11 server that does no input or output of its own.
 *
 * The core is a value.  It holds the protocol, the resources, the window
 * tree, the drawing and the selections, and it neither opens a file nor
 * touches a socket: whoever runs it moves the bytes.  A transport hands
 * each client's bytes in with x11server_input and takes the bytes for it
 * out with x11server_output and x11server_written; the requests are
 * handled in x11server_tick, which says when it wants to be called again.
 * The windows, the input and the clipboard of the system the server shows
 * itself on are reached through a backend, a table of functions
 * (x11backend.h).
 *
 * zedBSD's program (main.c) runs it over an AF_UNIX socket (unix.c) with
 * the Wayland desktop as its backend (wayland.c); another program can run
 * the same core over another transport and backend.  Nothing is global,
 * so one process may hold several servers.
 */

#ifndef KEILAND_X11SERVER_H
#define KEILAND_X11SERVER_H

#include <stddef.h>
#include <stdint.h>

/*
 * One running server.
 */
struct x11server;

/* The backend's table of functions (x11backend.h). */
struct x11_backend_ops;

/*
 * The role of a font file: the monospaced face the core fonts of fixed
 * width (fixed, courier, the tree's zed-unicode) are drawn with and its
 * bold, and the proportional face of the others (helvetica, times) and its
 * bold.  Only the monospaced one is needed: a role without a file is drawn
 * with another's face, made bold for a bold role.
 */
#define X11SERVER_FONT_MONO		0U
#define X11SERVER_FONT_MONO_BOLD	1U
#define X11SERVER_FONT_SANS		2U
#define X11SERVER_FONT_SANS_BOLD	3U

/*
 * How the server shows itself: rootless, each top-level X window a window
 * of the system (the server is their window manager), or rooted, the
 * whole root window one window of the system, inside which an X client
 * may manage the windows (a remote desktop's window manager).
 */
#define X11SERVER_ROOTLESS	0U
#define X11SERVER_ROOTED	1U

/*
 * One font file: its bytes, which the caller keeps unchanged for the
 * server's life, and the role it plays.
 */
struct x11server_font_file {
	const uint8_t *bytes;
	size_t size;
	unsigned role;
};

/*
 * What a server is started with.  A zero or a NULL takes the default.
 */
struct x11server_options {
	/* The root window's size (1280x800 when zero). */
	unsigned width;
	unsigned height;

	/*
	 * X11SERVER_ROOTLESS (zero) or X11SERVER_ROOTED.  A rooted server
	 * keeps the root window's pixels and shows them on one surface of the
	 * backend (X11_SURFACE_ROOT) while a client is connected.
	 */
	unsigned mode;

	/* The backend: its functions and the context they are called with. */
	const struct x11_backend_ops *backend;
	void *backend_context;

	/*
	 * The Vulkan WSI instance extension of the backend's surfaces
	 * (VK_KHR_wayland_surface on Wayland), through which windows are
	 * shown while Vulkan can show them; NULL shows every window through
	 * the backend's own present.
	 */
	const char *vulkan_wsi;

	/* The font files (none: no text is drawn). */
	const struct x11server_font_file *fonts;
	unsigned font_count;

	/*
	 * A file with the characters the monospaced faces lack, which the font
	 * library reads by its path (NULL: none).
	 */
	const char *font_fallback_path;

	/*
	 * The clients the server takes at once (64 when zero, at most 127),
	 * and the memory one client and the whole server may hold for their
	 * windows, pixmaps, properties and pixels (256 MiB and 512 MiB when
	 * zero): a request past them is refused with BadAlloc.
	 */
	unsigned clients_max;
	size_t client_memory_max;
	size_t server_memory_max;

	/* Where the core's log lines go, one at a time without a newline (NULL: nowhere). */
	void (*log)(void *context, const char *line);
	void *log_context;
};

/*
 * The authorization a connection must present (MIT-MAGIC-COOKIE-1 and its
 * data); NULL accepts any.
 */
struct x11server_auth {
	const char *name;
	const uint8_t *data;
	size_t size;
};

/*
 * The longest request (BIG-REQUESTS' longest, in bytes), and so the most
 * bytes of a client's input the core holds unhandled: a transport must
 * let the core have that many before it holds a client back, or let in
 * the rest of the request the core holds a part of (x11server_held), or
 * a long request is left half in.  A request longer than that is refused
 * (BadLength) and its bytes are dropped as they come.
 */
#define X11SERVER_REQUEST_MAX	(16U * 1024U * 1024U)

/* What x11server_next reports: nothing, a closed client, input handled, output waiting. */
#define X11SERVER_EVENT_NONE		0U
#define X11SERVER_EVENT_CLOSED		1U
#define X11SERVER_EVENT_CONSUMED	2U
#define X11SERVER_EVENT_OUTPUT		3U

/*
 * One thing the core tells its transport: a client the core has closed
 * (its connection goes too), bytes of a client's input the core has
 * handled (the transport may take as many more from it), or a client with
 * bytes waiting to be sent.
 */
struct x11server_event {
	unsigned type;
	uint32_t client;
	size_t bytes;
};

int x11server_create(const struct x11server_options *options, uint32_t now, struct x11server **result);
void x11server_destroy(struct x11server *server);

int x11server_connect(struct x11server *server, const struct x11server_auth *auth, uint32_t *client);
int x11server_input(struct x11server *server, uint32_t client, const uint8_t *bytes, size_t size);
const uint8_t *x11server_output(struct x11server *server, uint32_t client, size_t *size);
void x11server_written(struct x11server *server, uint32_t client, size_t size);
size_t x11server_held(struct x11server *server, uint32_t client, size_t *wanted);
void x11server_hangup(struct x11server *server, uint32_t client);
int x11server_next(struct x11server *server, struct x11server_event *event);
int x11server_tick(struct x11server *server, uint32_t now, uint32_t *wake);

#endif
