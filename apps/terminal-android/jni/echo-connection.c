/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tab's connection that shows back what is typed (echo-connection.h): the
 * bytes written to it are the bytes it gives to read, with a Return made a new
 * line and a Backspace made an erasing, kept in a ring of a fixed size.
 */

#include "apps/terminal-android/jni/echo-connection.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ring's size in bytes. */
#define ECHO_RING	16384U

/* What the connection says when it opens: a red bar a test can find, and a line. */
#define ECHO_BANNER	"\033[41m        \033[0m\r\necho ready\r\n"

/* One connection: its functions and the bytes waiting to be read. */
struct echo {
	struct terminal_connection connection;
	unsigned char ring[ECHO_RING];
	size_t head;
	size_t count;
};

static size_t echo_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int echo_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int echo_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int echo_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned echo_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned echo_pending(const struct terminal_connection *connection);
static void echo_close(struct terminal_connection *connection);
static size_t echo_put(struct echo *echo, const char *bytes, size_t size);

/* The functions every echo connection has. */
static const struct terminal_connection_ops echo_ops = {
	.read = echo_read,
	.write = echo_write,
	.resize = echo_resize,
	.mode = echo_mode,
	.event = echo_event,
	.pending = echo_pending,
	.close = echo_close,
	.take = NULL,
	.picture = NULL,
	.view = NULL
};

/*
 * Opens a tab's connection: empty but for the banner.
 */
int
echo_connector_open(
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
	struct echo *echo;

	/* The program's data, the application and the grid are not needed. */
	(void)data;
	(void)app;
	(void)id;
	(void)columns;
	(void)rows;
	(void)width;
	(void)height;

	/* The connection with its banner. */
	echo = calloc(1U, sizeof(*echo));
	if (echo == NULL)
		return ENOMEM;
	echo->connection.ops = &echo_ops;
	(void)echo_put(echo, ECHO_BANNER, sizeof(ECHO_BANNER) - 1U);

	/* The tab's first title. */
	(void)snprintf(title, size, "%s", "echo");

	/* Succeeded. */
	*connection = &echo->connection;
	return 0;
}

/* Gives up to size of the bytes waiting. */
static size_t
echo_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct echo *echo;
	size_t count;
	size_t index;

	/* As many as fit. */
	echo = (struct echo *)connection;
	count = echo->count < size ? echo->count : size;
	for (index = 0U; index < count; index++)
		buffer[index] = echo->ring[(echo->head + index) % ECHO_RING];
	echo->head = (echo->head + count) % ECHO_RING;
	echo->count -= count;

	/* The number given. */
	return count;
}

/* Takes bytes typed and shows them back; a byte that finds no room is not taken. */
static int
echo_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	struct echo *echo;
	size_t index;
	size_t put;

	/* Each byte, in the way the terminal shows it. */
	echo = (struct echo *)connection;
	for (index = 0U; index < size; index++) {
		if (bytes[index] == '\r')
			put = echo_put(echo, "\r\n", 2U);
		else if (bytes[index] == 0x7fU)
			put = echo_put(echo, "\b \b", 3U);
		else
			put = echo_put(echo, (const char *)&bytes[index], 1U);

		/* No room: the rest is for later. */
		if (put == 0U)
			break;
	}

	/* The number taken. */
	*written = index;
	return 0;
}

/* Takes the grid's size: nothing depends on it. */
static int
echo_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	(void)connection;
	(void)columns;
	(void)rows;
	(void)width;
	(void)height;

	/* Succeeded. */
	return 0;
}

/* Tells the input modes: the echo is on. */
static int
echo_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	(void)connection;

	/* A connection that shows keys back. */
	*mode = TERMINAL_MODE_ECHO;
	return 0;
}

/* Tells whether an application's event is this connection's: none is. */
static unsigned
echo_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	(void)connection;
	(void)event;

	/* None. */
	return 0U;
}

/* Tells what it has now: output when bytes wait. */
static unsigned
echo_pending(
	const struct terminal_connection *connection)
{
	const struct echo *echo;

	/* The bytes waiting. */
	echo = (const struct echo *)connection;
	if (echo->count != 0U)
		return TERMINAL_READY_OUTPUT;
	return 0U;
}

/* Hangs up. */
static void
echo_close(
	struct terminal_connection *connection)
{
	/* The connection and its ring are one allocation. */
	free(connection);
}

/* Appends bytes to the ring when all of them fit.  Returns their number, or 0. */
static size_t
echo_put(
	struct echo *echo,
	const char *bytes,
	size_t size)
{
	size_t index;

	/* Room for all or none. */
	if (echo->count + size > ECHO_RING)
		return 0U;
	for (index = 0U; index < size; index++)
		echo->ring[(echo->head + echo->count + index) % ECHO_RING] = (unsigned char)bytes[index];
	echo->count += size;

	/* The number put. */
	return size;
}
