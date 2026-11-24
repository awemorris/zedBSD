/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tab's connection to a shell on a pseudo-terminal: a process of the
 * application on a terminal of its own (kl_process), whose output and end
 * come as the application's events.  The process tells what it has as
 * often as it has it, so nothing is pending between the events.
 */

#include "connection.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shell run when no command is given. */
#define PROCESS_SHELL		"/bin/sh"

/* The terminal's type the shells are given unless the program chooses another. */
#define PROCESS_TERM		"xterm"

/*
 * One tab's process: the connection the main loop holds and the process
 * it stands for.  It lives until the connection is closed.
 */
struct process_connection {
	struct terminal_connection connection;
	struct kl_process *process;
};

static int process_open(void *data, struct kl_app *app, uint32_t id, unsigned columns, unsigned rows, unsigned width, unsigned height, char *title, size_t size, struct terminal_connection **connection);
static size_t process_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int process_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int process_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int process_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned process_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned process_pending(const struct terminal_connection *connection);
static void process_close(struct terminal_connection *connection);

/* What a process connection does. */
static const struct terminal_connection_ops process_ops = {
	process_read,
	process_write,
	process_resize,
	process_mode,
	process_event,
	process_pending,
	process_close,
	NULL,
	NULL,
	NULL
};

/*
 * Makes a connector that starts a shell for each tab, or the shell running
 * a command (NULL for an interactive shell), with TERM=xterm until the
 * program sets another term.
 */
void
terminal_process_connector_init(
	struct terminal_process_connector *process,
	const char *command)
{
	/* The connector opens process connections with the command. */
	memset(process, 0, sizeof(*process));
	process->command = command;
	(void)snprintf(process->term, sizeof(process->term), "%s", PROCESS_TERM);
	process->connector.open = process_open;
	process->connector.operation = "kl_process_start";
	process->connector.data = process;
}

/*
 * Starts the shell (or the command) as a process of the application on a
 * terminal of its own, at the grid's size, with the terminal's type in its
 * environment; the tab's title is "Shell" and its ID.  Returns 0, or an
 * errno value.
 */
static int
process_open(
	void *data,
	struct kl_app *app,
	uint32_t id,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height,
	char *title,
	size_t size,
	struct terminal_connection **connection)
{
	struct terminal_process_connector *connector;
	const char *arguments[4];

	(void)width;
	(void)height;

	/* The shell, or the shell running the command. */
	connector = data;
	arguments[0] = PROCESS_SHELL;
	arguments[1] = NULL;
	if (connector->command != NULL) {
		arguments[1] = "-c";
		arguments[2] = connector->command;
		arguments[3] = NULL;
	}

	/* Sets the default shell tab title and opens the process. */
	(void)snprintf(title, size, "Shell %u", id);
	return terminal_process_open_argv(app, arguments, connector->term, connector->ambiguous_wide,
	    columns, rows, NULL, title, size, connection);
}

/*
 * Starts an arbitrary argv directly on a pseudo-terminal without a shell.
 */
int
terminal_process_open_argv(
	struct kl_app *app,
	const char *const *argv,
	const char *term_name,
	int ambiguous_wide,
	unsigned columns,
	unsigned rows,
	const char *tab_title,
	char *title,
	size_t size,
	struct terminal_connection **connection)
{
	const char *environment[3];
	const char *effective_term;
	char term[TERMINAL_TERM_MAX + 5U];
	struct process_connection *made;
	struct kl_process_options process_options;
	int error;

	/* Validates required arguments. */
	if (app == NULL || argv == NULL || argv[0] == NULL || connection == NULL)
		return EINVAL;

	/* The terminal's type and ambiguous-width setting in the process environment. */
	effective_term = PROCESS_TERM;
	if (term_name != NULL && term_name[0] != '\0')
		effective_term = term_name;
	(void)snprintf(term, sizeof(term), "TERM=%s", effective_term);
	environment[0] = term;
	environment[1] = NULL;
	if (ambiguous_wide) {
		environment[1] = "ZEDBSD_AMBIGUOUS_WIDE=1";
		environment[2] = NULL;
	}

	/* The connection's memory. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* On a terminal of the grid's size. */
	memset(&process_options, 0, sizeof(process_options));
	process_options.argv = argv;
	process_options.environment = environment;
	process_options.streams = KL_PROCESS_TERMINAL;
	process_options.columns = columns;
	process_options.rows = rows;
	error = kl_process_start(app, &process_options, &made->process);
	if (error != 0) {
		free(made);
		return error;
	}

	/* Records an explicit tab title when provided. */
	if (tab_title != NULL && tab_title[0] != '\0')
		(void)snprintf(title, size, "%s", tab_title);

	/* Succeeded: the process runs. */
	made->connection.ops = &process_ops;
	*connection = &made->connection;
	return 0;
}

/* Takes what the shell wrote, up to size bytes; 0 when none waits. */
static size_t
process_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct process_connection *process;
	size_t count;

	/* The terminal's output. */
	process = (struct process_connection *)connection;
	count = kl_process_read(process->process, KL_PROCESS_OUTPUT, buffer, size);

	/* Reports how many bytes were read. */
	return count;
}

/* Types bytes into the shell's terminal; *written tells how many it took. */
static int
process_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	struct process_connection *process;
	int error;

	/* The terminal's input. */
	process = (struct process_connection *)connection;
	error = kl_process_write(process->process, bytes, size, written);
	if (error != 0)
		return error;

	/* Succeeded: what was taken is written. */
	return 0;
}

/* Tells the shell the grid's size (it gets SIGWINCH); the pixels are not a terminal's. */
static int
process_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	struct process_connection *process;
	int error;

	(void)width;
	(void)height;

	/* The size in cells. */
	process = (struct process_connection *)connection;
	error = kl_process_resize(process->process, columns, rows);
	if (error != 0)
		return error;

	/* Succeeded: the terminal has its new size. */
	return 0;
}

/* Reports the modes the shell's program set on its terminal. */
static int
process_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	const struct process_connection *process;
	unsigned terminal;
	int error;

	/* The terminal's modes; a terminal that cannot tell them says so. */
	process = (const struct process_connection *)connection;
	error = kl_process_terminal_mode(process->process, &terminal);
	if (error != 0)
		return error;

	/* The echo and the line, in the connection's bits. */
	*mode = 0U;
	if ((terminal & KL_PROCESS_TERMINAL_ECHO) != 0U)
		*mode |= TERMINAL_MODE_ECHO;
	if ((terminal & KL_PROCESS_TERMINAL_LINES) != 0U)
		*mode |= TERMINAL_MODE_LINES;

	/* Succeeded: the modes. */
	return 0;
}

/* Tells what an application's event says of this shell: output to read, room for the keys, its end. */
static unsigned
process_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct process_connection *process;

	/* Only this process's news. */
	process = (struct process_connection *)connection;
	if (event->kind != KL_APP_PROCESS)
		return 0U;
	if (event->process != process->process)
		return 0U;

	/* Reports what it told, in the same bits. */
	return event->ready;
}

/* Tells what the shell has besides its events: nothing, the process tells it again as long as it is there. */
static unsigned
process_pending(
	const struct terminal_connection *connection)
{
	(void)connection;

	/* Reports nothing pending. */
	return 0U;
}

/* Hangs up on the shell and closes it (the library reaps it). */
static void
process_close(
	struct terminal_connection *connection)
{
	struct process_connection *process;
	int error;

	/* Its terminal hung up, then the process closed. */
	process = (struct process_connection *)connection;
	error = kl_process_kill(process->process, KL_PROCESS_HANGUP);
	(void)error;
	kl_process_close(process->process);

	/* The connection's memory. */
	free(process);
}
