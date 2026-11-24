/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server process's side of the link (mikuterm --x11-server), the
 * same on every system: the frames from and to the parent, the parent's
 * HELLO with the settings, the X clients the parent opens (each checked
 * against the fake cookie by the core), their bytes both ways within the
 * link's credit, and the core fonts from mikuterm's folder.
 *
 * The system's part (Windows' x11-child.c, the POSIX systems'
 * apps/terminal-linux/x11-child.c) opens the link, waits on it and on the
 * backend's windows, and starts the backend and the core when the HELLO
 * has come; it hands this side its link's reads and writes, its start,
 * and the tabs of X windows when it shows windows in mikuterm's tabs
 * (struct x11_serve_system).  Each round it calls x11_serve_round when
 * the link may have something, and x11_serve_flush after the core's
 * tick.
 */

#ifndef MIKUTERM_X11_SERVE_H
#define MIKUTERM_X11_SERVE_H

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-link.h"

#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/* The fonts of the core fonts, inside mikuterm's folder. */
#define X11_SERVE_FONTS		4U

/* How the process ends (serve->ending): not yet, as the parent asked or the link ended, or for a parent that broke the rules. */
#define X11_SERVE_RUNNING	0
#define X11_SERVE_ENDED		1
#define X11_SERVE_BROKEN	2

struct x11_serve;

/*
 * The system's part as this side calls it, each with the context:
 *
 * receive reads what the link has, without waiting: 0 with *received
 * (0: the link ended), EAGAIN when nothing waits, or another errno value
 * (the link failed).  send writes what it can of the bytes the same way
 * (0 with *sent, EAGAIN, or an errno value).
 *
 * start starts the backend and the core once the parent's HELLO is in
 * the serve's settings (x11_serve_options gives the core its fonts), and
 * sets serve->server.  Returns 0, or an errno value (the process ends).
 *
 * tab_take takes a tab's frame from the parent, and tab_send sends the
 * tabs' frames; both NULL for a system that shows no X window in a tab.
 * tab_take returns 0, or an errno value for a frame against the rules.
 */
struct x11_serve_system {
	int (*receive)(void *context, void *buffer, size_t size, size_t *received);
	int (*send)(void *context, const void *bytes, size_t size, size_t *sent);
	int (*start)(void *context, struct x11_serve *serve);
	int (*tab_take)(void *context, const struct x11_link_frame *frame);
	void (*tab_send)(void *context);
	void *context;
};

/*
 * The link's state as the X server process keeps it: the system's part,
 * the frames read and those waiting, the link's table and the core's
 * client of each slot of it, whether the HELLO came and how the process
 * ends, the parent's settings, the core, and the core fonts' bytes.
 */
struct x11_serve {
	const struct x11_serve_system *system;
	uint8_t *in;
	size_t in_size;
	uint8_t *out;
	size_t out_size;
	size_t out_capacity;
	struct x11_link_table table;
	uint32_t clients[X11_LINK_CHANNELS];
	int hello;
	int ending;
	uint8_t cookie[X11_HOST_COOKIE];
	char host[X11_HOST_NAME_MAX];
	unsigned scale;
	unsigned flags;
	unsigned root_width;
	unsigned root_height;
	struct x11server *server;
	char *fonts[X11_SERVE_FONTS];
	size_t font_sizes[X11_SERVE_FONTS];
	struct x11server_font_file font_files[X11_SERVE_FONTS];
	char fallback[MIKUTERM_PATH_MAX];
};

int x11_serve_init(struct x11_serve *serve, const struct x11_serve_system *system);
void x11_serve_round(struct x11_serve *serve);
void x11_serve_flush(struct x11_serve *serve);
void x11_serve_options(struct x11_serve *serve, struct x11server_options *options);
int x11_serve_frame(struct x11_serve *serve, unsigned type, uint32_t id, const void *body, uint32_t size);
size_t x11_serve_waiting(const struct x11_serve *serve);
void x11_serve_end(struct x11_serve *serve);

#endif
