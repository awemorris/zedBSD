/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The xserver program: the options, the signals, and the poll loop that
 * runs the core (x11server.h) with the Wayland desktop as its backend
 * (wayland.c) and the AF_UNIX socket as its transport (unix.c).
 *
 *   xserver [:0] [--size WIDTHxHEIGHT] [--shm] [--socket=PATH] [--font=PATH]
 */

#include "userland/desktop/xserver/program.h"
#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The largest root window side. */
#define MAIN_SIZE_MAX		16384UL

/* The defaults of the socket's path and the font's. */
#define MAIN_SOCKET_PATH	"/tmp/.X11-unix/X0"
#define MAIN_FONT_PATH		KEILAND_DATADIR "/fonts/keiland-mono.ttf"

/* The largest font file read. */
#define MAIN_FONT_MAX		(32U * 1024U * 1024U)

/* The longest the loop waits between ticks (milliseconds). */
#define MAIN_WAIT_MS		20U

/* The descriptors the loop waits on: the desktop's connection and the transport's. */
#define MAIN_POLLFDS		(1U + X11_UNIX_POLLFDS)

/*
 * What the program is started with: the root window's size (zero for the
 * core's default), whether windows are shown through wl_shm only, and the
 * paths of the socket and the font.
 */
struct main_options {
	unsigned width;
	unsigned height;
	int shm;
	const char *socket_path;
	const char *font_path;
};

/*
 * Nonzero once SIGINT or SIGTERM has come: the loop ends and the server is
 * destroyed (its socket unlinked).  Written only by the handler.
 */
static volatile sig_atomic_t main_stopped;

static int main_options(int argc, char **argv, struct main_options *options);
static int main_size(const char *text, unsigned *width, unsigned *height);
static int main_font(const char *path, uint8_t **bytes, size_t *size);
static uint32_t main_now(void);
static void main_log(void *context, const char *line);
static void main_signal(int number);

/*
 * Runs the server until a signal or the loss of the desktop.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd descriptors[MAIN_POLLFDS];
	struct x11server_font_file font;
	struct x11server_options server_options;
	struct main_options options;
	struct x11server *server;
	struct x11_wayland *wayland;
	struct x11_unix *transport;
	const char *path;
	uint8_t *font_bytes;
	size_t font_size;
	uint32_t wake;
	uint32_t now;
	unsigned count;
	int timeout_ms;
	int readable;
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

	/* The core font's file; without one, text is not drawn. */
	path = options.font_path;
	if (path == NULL)
		path = MAIN_FONT_PATH;
	font_bytes = NULL;
	font_size = 0U;
	error = main_font(path, &font_bytes, &font_size);
	if (error != 0)
		fprintf(stderr, "xserver: %s: %s (no text)\n", path, strerror(error));

	/* The connection to the desktop, which shows the windows and gives the input. */
	error = x11_wayland_open(&wayland, NULL);
	if (error != 0) {
		fprintf(stderr, "xserver: %s\n", strerror(error));
		free(font_bytes);
		return 1;
	}

	/* The core, on the desktop, shown through Vulkan unless --shm. */
	memset(&server_options, 0, sizeof(server_options));
	server_options.width = options.width;
	server_options.height = options.height;
	server_options.backend = &x11_wayland_ops;
	server_options.backend_context = wayland;
	if (!options.shm)
		server_options.vulkan_wsi = X11_WAYLAND_WSI;
	if (font_bytes != NULL) {
		font.bytes = font_bytes;
		font.size = font_size;
		font.role = X11SERVER_FONT_MONO;
		server_options.fonts = &font;
		server_options.font_count = 1U;
	}
	server_options.font_fallback_path = KEILAND_FONT_FALLBACK_MONO;
	server_options.log = main_log;
	now = main_now();
	error = x11server_create(&server_options, now, &server);
	if (error != 0) {
		fprintf(stderr, "xserver: %s\n", strerror(error));
		x11_wayland_close(wayland);
		free(font_bytes);
		return 1;
	}

	/* The desktop tells the core from now on. */
	x11_wayland_attach(wayland, server);

	/* The socket the clients connect to. */
	path = options.socket_path;
	if (path == NULL)
		path = MAIN_SOCKET_PATH;
	error = x11_unix_open(&transport, path);
	if (error != 0) {
		fprintf(stderr, "xserver: %s\n", strerror(error));
		x11server_destroy(server);
		x11_wayland_close(wayland);
		free(font_bytes);
		return 1;
	}

	/* Wait, hand what came to the core, and let it work, until told to stop or the desktop is lost. */
	wake = now;
	while (!main_stopped) {
		/* The desktop's connection and the transport's descriptors. */
		descriptors[0].fd = x11_wayland_fd(wayland);
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		count = x11_unix_pollfds(transport, server, descriptors + 1, MAIN_POLLFDS - 1U);

		/* Until the core's next tick, and never longer than the loop's longest wait. */
		now = main_now();
		timeout_ms = (int)MAIN_WAIT_MS;
		if ((int32_t)(wake - now) < (int32_t)MAIN_WAIT_MS)
			timeout_ms = (int)(int32_t)(wake - now);
		if (timeout_ms < 0)
			timeout_ms = 0;

		/* The wait. */
		ready = poll(descriptors, count + 1U, timeout_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			break;

		/* New clients. */
		x11_unix_accept(transport, server, descriptors + 1, count);

		/* The desktop's events (those already read, too); a lost connection ends the server. */
		readable = 0;
		if ((descriptors[0].revents & POLLIN) != 0)
			readable = 1;
		lost = x11_wayland_dispatch(wayland, readable);
		if (lost != 0)
			break;

		/* The clients' bytes, the core's work, and what it has for the clients. */
		x11_unix_read(transport, server);
		now = main_now();
		(void)x11server_tick(server, now, &wake);
		x11_unix_write(transport, server);
	}

	/* The socket and its connections, the core, the desktop's connection, and the font go. */
	x11_unix_close(transport);
	x11server_destroy(server);
	x11_wayland_close(wayland);
	free(font_bytes);

	/* Succeeded: stopped. */
	return 0;
}

/* Reads the options; returns 0, or -1 for one that is not known. */
static int
main_options(
	int argc,
	char **argv,
	struct main_options *options)
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

/* Reads a whole font file into memory the core keeps using; returns 0, or an errno value. */
static int
main_font(
	const char *path,
	uint8_t **bytes,
	size_t *size)
{
	struct stat status;
	uint8_t *data;
	ssize_t count;
	size_t done;
	int descriptor;
	int error;

	/* The file. */
	*bytes = NULL;
	*size = 0U;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Its size, which must be sensible. */
	error = fstat(descriptor, &status);
	if (error != 0 ||
	    status.st_size <= 0 ||
	    (unsigned long)status.st_size > MAIN_FONT_MAX) {
		(void)close(descriptor);
		return EINVAL;
	}

	/* The memory for it. */
	data = malloc((size_t)status.st_size);
	if (data == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Read to the end. */
	done = 0U;
	while (done < (size_t)status.st_size) {
		count = read(descriptor, data + done, (size_t)status.st_size - done);
		if (count <= 0) {
			(void)close(descriptor);
			free(data);
			return EIO;
		}

		/* What was read counts towards the whole. */
		done += (size_t)count;
	}

	/* Succeeded: the font is in memory. */
	(void)close(descriptor);
	*bytes = data;
	*size = done;
	return 0;
}

/* Returns the monotonic time in milliseconds (its low 32 bits: the core's clock). */
static uint32_t
main_now(void)
{
	struct timespec now;
	uint64_t milliseconds;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	milliseconds = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;

	/* Succeeded: the low bits, which wrap as the core expects. */
	return (uint32_t)milliseconds;
}

/* Writes one of the core's log lines to the standard output at once. */
static void
main_log(
	void *context,
	const char *line)
{
	/* The line and its end. */
	(void)context;
	printf("%s\n", line);
	fflush(stdout);
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
