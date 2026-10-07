/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xserver: an X11 server for the compositor, rootless (each top-level
 * X window a window of the desktop, as Xwayland does).
 *
 * The server is a value with an event loop of its caller's: the caller
 * asks which descriptors to wait for, waits, and hands the result back.
 * main.c does that with poll(2); the compositor can do it inside its own loop
 * later, with no global state to share.
 */

#ifndef KEILAND_X11SERVER_H
#define KEILAND_X11SERVER_H

#include <poll.h>

/* The most descriptors x11server_pollfds fills: the listening socket, the compositor's connection and the clients. */
#define X11SERVER_POLLFDS_MAX	10U

/*
 * One running server.
 */
struct x11server;

/*
 * What a server is started with.  A NULL string takes the default.
 */
struct x11server_options {
	/* The listening socket's path (/tmp/.X11-unix/X0). */
	const char *socket_path;

	/* The monospaced TrueType font the core font is drawn with. */
	const char *font_path;

	/* The Wayland display to connect to (WAYLAND_DISPLAY). */
	const char *wayland_display;

	/* The root window's size (1280x800 when zero). */
	unsigned width;
	unsigned height;

	/* Nonzero shows the windows through wl_shm instead of Vulkan (--shm). */
	int shm;
};

int x11server_create(const struct x11server_options *options, struct x11server **result);
unsigned x11server_pollfds(struct x11server *server, struct pollfd *descriptors, unsigned capacity, int *timeout_ms);
void x11server_dispatch(struct x11server *server, const struct pollfd *descriptors, unsigned count);
int x11server_stopped(const struct x11server *server);
void x11server_destroy(struct x11server *server);

#endif
