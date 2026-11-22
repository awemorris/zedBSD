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

/* With TERMINAL_READY_EXIT: the user asked the connection for the settings, which take the tab's place. */
#define TERMINAL_READY_SETTINGS	0x10U

/* A connection's input modes: the echo is on, the input is read by the line. */
#define TERMINAL_MODE_ECHO	0x1U
#define TERMINAL_MODE_LINES	0x2U

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
 */
struct terminal_connection_ops {
	size_t (*read)(struct terminal_connection *connection, unsigned char *buffer, size_t size);
	int (*write)(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
	int (*resize)(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
	int (*mode)(const struct terminal_connection *connection, unsigned *mode);
	unsigned (*event)(struct terminal_connection *connection, const struct kl_app_event *event);
	unsigned (*pending)(const struct terminal_connection *connection);
	void (*close)(struct terminal_connection *connection);
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

/*
 * A connector that runs a shell on a pseudo-terminal for each tab
 * (connection-process.c): /bin/sh, or the shell running a command when
 * one is given.
 */
struct terminal_process_connector {
	struct terminal_connector connector;
	const char *command;
};

void terminal_process_connector_init(struct terminal_process_connector *process, const char *command);

#endif
