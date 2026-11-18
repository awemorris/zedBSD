/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The xserver program: the options, the signals, and the poll
 * loop around the server (x11server.h).
 *
 *   xserver [:0] [--size WIDTHxHEIGHT] [--shm] [--socket=PATH] [--font=PATH]
 */

#include "userland/desktop/xserver/x11server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest root window side. */
#define MAIN_SIZE_MAX		16384UL

/*
 * Nonzero once SIGINT or SIGTERM has come: the loop ends and the server is
 * destroyed (its socket unlinked).  Written only by the handler.
 */
static volatile sig_atomic_t main_stopped;

static int main_options(int argc, char **argv, struct x11server_options *options);
static int main_size(const char *text, unsigned *width, unsigned *height);
static void main_signal(int number);

/*
 * Runs the server until a signal or the loss of the desktop.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd descriptors[X11SERVER_POLLFDS_MAX];
	struct x11server_options options;
	struct x11server *server;
	unsigned count;
	int timeout_ms;
	int ready;
	int lost;
	int error;

	/* The options. */
	error = main_options(argc, argv, &options);
	if (error != 0) {
		fprintf(stderr, "usage: xserver [:0] [--size WIDTHxHEIGHT] [--socket=PATH] [--font=PATH]\n");
		return 2;
	}

	/* SIGINT and SIGTERM end the loop; a client gone mid-write is found by the next read, not by SIGPIPE. */
	(void)signal(SIGINT, main_signal);
	(void)signal(SIGTERM, main_signal);
	(void)signal(SIGPIPE, SIG_IGN);

	/* The server. */
	error = x11server_create(&options, &server);
	if (error != 0) {
		fprintf(stderr, "xserver: %s\n", strerror(error));
		return 1;
	}

	/* Wait, and hand what came to the server, until told to stop or the desktop is lost. */
	while (!main_stopped) {
		lost = x11server_stopped(server);
		if (lost)
			break;

		/* The wait. */
		count = x11server_pollfds(server, descriptors, X11SERVER_POLLFDS_MAX, &timeout_ms);
		ready = poll(descriptors, count, timeout_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			break;

		/* What came. */
		x11server_dispatch(server, descriptors, count);
	}

	/* The server and its socket go. */
	x11server_destroy(server);

	/* Succeeded: stopped. */
	return 0;
}

/* Reads the options; returns 0, or -1 for one that is not known. */
static int
main_options(
	int argc,
	char **argv,
	struct x11server_options *options)
{
	int argument;
	int differs;
	int valid;

	/* The defaults. */
	memset(options, 0, sizeof(*options));

	/* Each option. */
	for (argument = 1; argument < argc; argument++) {
		/* The display name is always :0. */
		differs = strcmp(argv[argument], ":0");
		if (differs == 0)
			continue;

		/* The root window's size. */
		differs = strcmp(argv[argument], "--size");
		if (differs == 0 && argument + 1 < argc) {
			argument++;
			valid = main_size(argv[argument], &options->width, &options->height);
			if (!valid)
				return -1;
			continue;
		}

		/* The windows through wl_shm instead of Vulkan. */
		differs = strcmp(argv[argument], "--shm");
		if (differs == 0) {
			options->shm = 1;
			continue;
		}

		/* The socket's path. */
		differs = strncmp(argv[argument], "--socket=", 9U);
		if (differs == 0) {
			options->socket_path = argv[argument] + 9;
			continue;
		}

		/* The font's path. */
		differs = strncmp(argv[argument], "--font=", 7U);
		if (differs == 0) {
			options->font_path = argv[argument] + 7;
			continue;
		}

		/* Anything else is not known. */
		return -1;
	}

	/* Succeeded: the options. */
	return 0;
}

/* Reads WIDTHxHEIGHT; returns nonzero when it is a size. */
static int
main_size(
	const char *text,
	unsigned *width,
	unsigned *height)
{
	unsigned long across;
	unsigned long down;
	char *end;

	/* The width, then an x. */
	across = strtoul(text, &end, 10);
	if (end == text ||
	    (*end != 'x' && *end != 'X') ||
	    across == 0UL ||
	    across > MAIN_SIZE_MAX)
		return 0;

	/* The height, to the end. */
	down = strtoul(end + 1, &end, 10);
	if (*end != '\0' ||
	    down == 0UL ||
	    down > MAIN_SIZE_MAX)
		return 0;

	/* Succeeded: the size. */
	*width = (unsigned)across;
	*height = (unsigned)down;
	return 1;
}

/* Ends the loop at its next turn. */
static void
main_signal(
	int number)
{
	/* The flag the loop reads. */
	(void)number;
	main_stopped = 1;
}
