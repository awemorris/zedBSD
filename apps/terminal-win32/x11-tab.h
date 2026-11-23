/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An X window shown in a tab of mikuterm (a Host's X11WindowTabify):
 * the picture's connection (connection.h) of one top-level X window of an
 * SSH connection's X server.
 *
 * The X server's child process draws the window and sends its pixels
 * over the link (x11-link.h); the connection's network thread
 * (x11-host.c) keeps them here, with the window's title and size, and
 * wakes the application's thread with a post.  The tab draws the pixels
 * at the X server's scale in the grid's place, and hands the user's keys,
 * text and pointer, the tab's size and its closing back here, which the
 * network thread sends to the child.  The tab never reads the remote
 * host's bytes as anything but pixels of a size it checked, and nothing
 * of the child's runs in mikuterm's process: the child keeps its lower
 * integrity level.
 *
 * Two hold a tab: the X server's side (until the window ends, or the tab
 * is closed or refused) and the application's thread (from the post that
 * gives it the tab until the tab closes).  What both use is under the
 * tab's lock; the last to let go frees it.
 */

#ifndef MIKUTERM_X11_TAB_H
#define MIKUTERM_X11_TAB_H

#include "x11-link.h"

#include "userland/desktop/terminal/connection.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

struct mikuterm_session;

/* The user's inputs that wait for the network thread, at most. */
#define X11_TAB_INPUTS		128U

/* The longest text one input carries (an input method's commit), with its NUL. */
#define X11_TAB_TEXT_MAX	256U

/* The longest host's name put in front of the title, with its NUL. */
#define X11_TAB_HOST_MAX	256U

/*
 * One of the user's inputs for the X window: its kind
 * (X11_LINK_INPUT_*), its three numbers, and the text of a commit.
 */
struct x11_tab_input {
	unsigned kind;
	uint32_t numbers[3];
	char text[X11_TAB_TEXT_MAX];
};

/*
 * One X window's tab.  The fields up to the lock are set when it is made
 * and only read afterwards; the rest are under the lock, but those the
 * application's thread keeps alone (the wheel's rest).
 */
struct x11_tab {
	/* The core's handle: the tab's connection (first, so the core's pointer is the tab's). */
	struct terminal_connection connection;

	/* The X window, the X server's scale in percent, and the remote host's name for the title. */
	uint32_t window;
	unsigned scale;
	char host[X11_TAB_HOST_MAX];

	struct kl_mutex lock;

	/* Who still holds the tab: the X server's side and the application's thread; 0 frees it. */
	unsigned references;

	/* The session that wakes the network thread and the application's (NULL until it is given one), and the posts' number. */
	struct mikuterm_session *session;
	uint32_t number;

	/* A post is on its way and not taken yet. */
	int posted;

	/* The window's pixels (0xAARRGGBB, width by height, rows of the width) and its title (UTF-8). */
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	char title[X11_LINK_TAB_TITLE_MAX + 1U];

	/* New pixels, a new title or a new size the tab has not taken yet, and the window's end. */
	int news;
	int ended;

	/* The user closed the tab, or the run had no room for it: the child is told once. */
	int closed;
	int refused;

	/* The size the tab gives the window (X's pixels), and whether the child is still to hear it. */
	unsigned wanted_width;
	unsigned wanted_height;
	int wanted;

	/* The user's inputs for the child, oldest first. */
	struct x11_tab_input inputs[X11_TAB_INPUTS];
	unsigned input_count;

	/* The wheels' distance not yet a whole notch (the application's thread alone). */
	double wheel_x;
	double wheel_y;
};

int x11_tab_create(uint32_t window, unsigned width, unsigned height, unsigned scale, const char *host, const uint8_t *title, size_t title_size, struct x11_tab **result);
void x11_tab_attach(struct x11_tab *tab, struct mikuterm_session *session, uint32_t number);
void x11_tab_release(struct x11_tab *tab);
void x11_tab_set_title(struct x11_tab *tab, const uint8_t *title, size_t size);
int x11_tab_set_size(struct x11_tab *tab, unsigned width, unsigned height);
int x11_tab_put(struct x11_tab *tab, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *bytes);
void x11_tab_end(struct x11_tab *tab);
void x11_tab_wake(struct x11_tab *tab);
int x11_tab_leaving(struct x11_tab *tab, int *refused);
int x11_tab_take_size(struct x11_tab *tab, unsigned *width, unsigned *height);
int x11_tab_take_input(struct x11_tab *tab, struct x11_tab_input *input);

#endif
