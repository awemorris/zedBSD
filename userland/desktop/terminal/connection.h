/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The other end of a tab, chosen when the program is linked: a program on
 * a pseudo-terminal of the application's (connection-process.c), or a
 * remote shell.  The main loop reads what the other end writes, writes the
 * keys typed, tells it the grid's size and asks whether it reads a secret;
 * the window's dispatch hands each connection the application's events to
 * learn what it has for the main loop.
 */

#ifndef TERMINAL_CONNECTION_H
#define TERMINAL_CONNECTION_H

#include "terminal.h"

/* What a connection has for the main loop (bits; the same values as KL_PROCESS_READY_*). */
#define TERMINAL_READY_OUTPUT	0x1U
#define TERMINAL_READY_INPUT	0x4U
#define TERMINAL_READY_EXIT	0x8U

/* The user asked the connection for the settings: they open in a tab of their own beside it (it goes on). */
#define TERMINAL_READY_SETTINGS	0x10U

/* The connection has a new tab's connection for the run to open (take gives it). */
#define TERMINAL_READY_TAB	0x20U

/* A connection's input modes: the echo is on, the input is read by the line. */
#define TERMINAL_MODE_ECHO	0x1U
#define TERMINAL_MODE_LINES	0x2U

/* What a picture's connection does besides (below). */
struct terminal_picture_ops;

/*
 * What a connection does, the implementation's own functions:
 *
 * read takes up to size bytes the other end wrote (0 when none waits);
 * write gives it bytes typed and tells how many it took (fewer when it is
 * full: TERMINAL_READY_INPUT comes when there is room), 0 or an errno
 * value once it has gone; resize tells it the grid's size in cells and in
 * pixels; mode reports its input modes (TERMINAL_MODE_*), 0 or ENOTSUP
 * where it cannot tell; event tells whether an application's event is
 * this connection's, by its TERMINAL_READY_* bits (0 when not); pending
 * tells what it still has now, whatever events came (the main loop then
 * does not wait); close hangs up and returns at once.
 *
 * take (NULL where a connection never has one) gives a new tab's
 * connection the other end opened, with TERMINAL_READY_TAB (NULL when
 * none is left); the run opens a tab for it and makes it active.  picture
 * (NULL for a grid's connection) makes the tab a picture's: the run shows
 * what it draws in the grid's place, gives it the window's inputs as they
 * are, and tells its size in pixels (resize, with no cells); its output
 * (TERMINAL_READY_OUTPUT) means a new picture or a new title, and read
 * takes no bytes but takes the news.
 */
struct terminal_connection_ops {
	size_t (*read)(struct terminal_connection *connection, unsigned char *buffer, size_t size);
	int (*write)(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
	int (*resize)(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
	int (*mode)(const struct terminal_connection *connection, unsigned *mode);
	unsigned (*event)(struct terminal_connection *connection, const struct kl_app_event *event);
	unsigned (*pending)(const struct terminal_connection *connection);
	void (*close)(struct terminal_connection *connection);
	struct terminal_connection *(*take)(struct terminal_connection *connection);
	const struct terminal_picture_ops *picture;
};

/*
 * What a picture's connection does besides: draw paints the whole of
 * what it shows into pixels (0xAARRGGBB, width by height, rows of the
 * width), 0 or an errno value; title gives its title (UTF-8, cut to
 * size); input takes one of the window's inputs (terminal.h); refuse
 * tells it that the run has no tab for it (a full window), before the
 * run closes it, so that the other end shows the picture some other way
 * instead of ending it.
 */
struct terminal_picture_ops {
	int (*draw)(struct terminal_connection *connection, uint32_t *pixels, uint32_t width, uint32_t height);
	void (*title)(struct terminal_connection *connection, char *title, size_t size);
	void (*input)(struct terminal_connection *connection, const struct terminal_picture_event *event);
	void (*refuse)(struct terminal_connection *connection);
};

/*
 * One tab's connection: its functions.  The implementation's own state
 * follows it in the same allocation; it lives until close.
 */
struct terminal_connection {
	const struct terminal_connection_ops *ops;
};

/*
 * What opens a tab's connection: open makes one for the tab with an ID,
 * at the grid's size, and writes the tab's first title; it returns 0 or an
 * errno value.  operation names it in the error line when the first tab
 * cannot be opened.  data is the program's own.
 */
struct terminal_connector {
	int (*open)(void *data, struct kl_app *app, uint32_t id, unsigned columns, unsigned rows, unsigned width, unsigned height, char *title, size_t size, struct terminal_connection **connection);
	const char *operation;
	void *data;
};

/* The longest terminal type a process connector gives its shells, with its NUL. */
#define TERMINAL_TERM_MAX	64U

/*
 * A connector that runs a shell on a pseudo-terminal for each tab
 * (connection-process.c): /bin/sh, or the shell running a command when
 * one is given, with term as its TERM ("xterm" unless the program changes
 * it; a tab opened after a change takes the new one).
 */
struct terminal_process_connector {
	struct terminal_connector connector;
	const char *command;
	char term[TERMINAL_TERM_MAX];
};

void terminal_process_connector_init(struct terminal_process_connector *process, const char *command);

#endif
