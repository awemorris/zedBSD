/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's X server process on the POSIX systems (mikuterm
 * --x11-server): one X server of one SSH connection, its windows on the
 * desktop, and its clients over the link to the mikuterm that started it
 * (x11-spawn-posix.c).
 *
 * Its standard input is its end of the link.  It says HELLO and takes
 * the parent's HELLO with the settings (x11-serve.h), then starts the
 * backend and the core, and one poll(2) loop runs the link, the backend's
 * connection to the desktop and the core's time.  The process ends with
 * the link, on SHUTDOWN, when the parent's frames break the rules, or
 * when the desktop is lost.
 *
 * The backend is the first of child_backends that opens: the Wayland
 * desktop (the X server's wayland.c, each top-level X window an xdg
 * toplevel).  A system or a display of another kind (one with a display
 * of Vulkan's own and no desktop, Haiku's) is a backend added to the
 * table.  Every X window is a window of the desktop: the windows are not
 * shown in mikuterm's tabs here (X11WindowTabify is not heard).
 */

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-link.h"
#include "x11-serve.h"

#include "userland/desktop/xserver/program.h"
#include "userland/desktop/xserver/x11server.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The longest wait of the loop (ms). */
#define CHILD_WAIT_MS		1000U

/* The descriptors the loop waits on: the link and the backend's. */
#define CHILD_LINK		0U
#define CHILD_BACKEND		1U
#define CHILD_POLLFDS		2U

/* A send to a link the parent closed must not raise a signal, where the system can say so. */
#ifdef MSG_NOSIGNAL
#define CHILD_SEND_FLAGS	MSG_NOSIGNAL
#else
#define CHILD_SEND_FLAGS	0
#endif

/*
 * A backend the X server can show its windows with: its name, the core's
 * table of its functions and the Vulkan instance extension its windows
 * are shown through (NULL: the core draws in memory and hands the
 * backend its pixels), and the functions the loop calls: open (0, or an
 * errno value when this system has no such display), attach the core,
 * the descriptor to wait on, the dispatch of what came (0, or -1 when the
 * display is lost) and close.
 */
struct child_backend {
	const char *name;
	const struct x11_backend_ops *ops;
	const char *vulkan_wsi;
	int (*open)(void **backend);
	void (*attach)(void *backend, struct x11server *server);
	int (*fd)(const void *backend);
	int (*dispatch)(void *backend, int readable);
	void (*close)(void *backend);
};

/*
 * The X server process's state: the link's descriptor, the link's side
 * (x11-serve.h), and the backend open (NULL until the HELLO).
 */
struct child {
	int link;
	struct x11_serve serve;
	const struct child_backend *kind;
	void *backend;
};

static int child_wayland_open(void **backend);
static void child_wayland_attach(void *backend, struct x11server *server);
static int child_wayland_fd(const void *backend);
static int child_wayland_dispatch(void *backend, int readable);
static void child_wayland_close(void *backend);
static int child_receive(void *context, void *buffer, size_t size, size_t *received);
static int child_send(void *context, const void *bytes, size_t size, size_t *sent);
static int child_start_server(void *context, struct x11_serve *serve);
static uint32_t child_now(void);

/* The backends, the first that opens chosen. */
static const struct child_backend child_backends[] = {
	{
		"wayland", &x11_wayland_ops, X11_WAYLAND_WSI,
		child_wayland_open, child_wayland_attach, child_wayland_fd, child_wayland_dispatch, child_wayland_close
	}
};

/*
 * Runs mikuterm's X server process until its link ends.  Returns the
 * process's exit code: 0 for an end the parent asked for or the link's
 * end, 1 for a start that failed, 2 for a parent that broke the rules or
 * a backend that failed.
 */
int
mikuterm_x11_child(void)
{
	static struct child child;
	static struct x11_serve_system system;
	struct pollfd waits[CHILD_POLLFDS];
	uint32_t now;
	uint32_t wake;
	nfds_t count;
	int timeout;
	int ready;
	int lost;
	int flags;
	int error;
	int status;

	/* A write to a link the parent closed must not end the process by a signal. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The folder (for the fonts), the link (standard input, not blocking), and its side with the HELLO. */
	memset(&child, 0, sizeof(child));
	child.link = STDIN_FILENO;
	(void)mikuterm_folder_init();
	flags = fcntl(child.link, F_GETFL);
	if (flags < 0 || fcntl(child.link, F_SETFL, flags | O_NONBLOCK) < 0)
		return 1;
	(void)fcntl(child.link, F_SETFD, FD_CLOEXEC);
	system.receive = child_receive;
	system.send = child_send;
	system.start = child_start_server;
	system.tab_take = NULL;
	system.tab_send = NULL;
	system.context = &child;
	error = x11_serve_init(&child.serve, &system);
	if (error != 0)
		return 1;

	/* Round after round until the link ends. */
	status = 0;
	for (;;) {
		x11_serve_round(&child.serve);
		if (child.serve.ending)
			break;

		/* The core's time, its replies and events to the parent, and its requests to the desktop, before the wait. */
		timeout = (int)CHILD_WAIT_MS;
		if (child.serve.server != NULL) {
			now = child_now();
			wake = now + CHILD_WAIT_MS;
			(void)x11server_tick(child.serve.server, now, &wake);
			if ((int32_t)(wake - now) < timeout)
				timeout = (int)(int32_t)(wake - now);
			if (timeout < 0)
				timeout = 0;
			x11_serve_flush(&child.serve);
			if (child.serve.ending)
				break;
		}
		if (child.backend != NULL) {
			lost = child.kind->dispatch(child.backend, 0);
			if (lost != 0) {
				child.serve.ending = X11_SERVE_BROKEN;
				break;
			}
		}

		/* The wait: the link (and its room while frames wait), the desktop. */
		waits[CHILD_LINK].fd = child.link;
		waits[CHILD_LINK].events = POLLIN;
		if (x11_serve_waiting(&child.serve) != 0)
			waits[CHILD_LINK].events |= POLLOUT;
		waits[CHILD_LINK].revents = 0;
		count = 1;
		if (child.backend != NULL) {
			waits[CHILD_BACKEND].fd = child.kind->fd(child.backend);
			waits[CHILD_BACKEND].events = POLLIN;
			waits[CHILD_BACKEND].revents = 0;
			count = 2;
		}
		ready = poll(waits, count, timeout);
		if (ready < 0 && errno != EINTR) {
			child.serve.ending = X11_SERVE_ENDED;
			break;
		}

		/* The desktop's events (those already read, too); a lost desktop ends the server. */
		if (child.backend != NULL) {
			lost = child.kind->dispatch(child.backend, ready > 0 && (waits[CHILD_BACKEND].revents & POLLIN) != 0);
			if (lost != 0) {
				child.serve.ending = X11_SERVE_BROKEN;
				break;
			}
		}
	}
	if (child.serve.ending == X11_SERVE_BROKEN)
		status = 2;

	/* The end: the core and the fonts' bytes, the backend, the link. */
	x11_serve_end(&child.serve);
	if (child.backend != NULL)
		child.kind->close(child.backend);
	(void)close(child.link);
	return status;
}

/* Opens the Wayland desktop of WAYLAND_DISPLAY. */
static int
child_wayland_open(
	void **backend)
{
	struct x11_wayland *wayland;
	int error;

	/* The connection and its globals. */
	error = x11_wayland_open(&wayland, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	*backend = wayland;
	return 0;
}

/* Lets the Wayland backend tell the core what comes. */
static void
child_wayland_attach(
	void *backend,
	struct x11server *server)
{
	x11_wayland_attach(backend, server);
}

/* Gives the Wayland connection's descriptor. */
static int
child_wayland_fd(
	const void *backend)
{
	int descriptor;

	descriptor = x11_wayland_fd(backend);
	return descriptor;
}

/* Runs the Wayland events that came, and sends the requests made. */
static int
child_wayland_dispatch(
	void *backend,
	int readable)
{
	int lost;

	lost = x11_wayland_dispatch(backend, readable);
	return lost;
}

/* Closes the Wayland connection and its windows. */
static void
child_wayland_close(
	void *backend)
{
	x11_wayland_close(backend);
}

/* Reads what the link has, without waiting (x11-serve.h's receive). */
static int
child_receive(
	void *context,
	void *buffer,
	size_t size,
	size_t *received)
{
	struct child *child;
	ssize_t got;

	/* What waits; none is EAGAIN. */
	child = context;
	*received = 0;
	do {
		got = read(child->link, buffer, size);
	} while (got < 0 && errno == EINTR);
	if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return EAGAIN;
	if (got < 0)
		return EPIPE;

	/* Succeeded: the bytes (0: the link ended). */
	*received = (size_t)got;
	return 0;
}

/* Writes what the link takes, without waiting (x11-serve.h's send). */
static int
child_send(
	void *context,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	struct child *child;
	ssize_t put;

	/* What the socket takes; no room is EAGAIN. */
	child = context;
	*sent = 0;
	do {
		put = send(child->link, bytes, size, CHILD_SEND_FLAGS);
	} while (put < 0 && errno == EINTR);
	if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return EAGAIN;
	if (put <= 0)
		return EPIPE;

	/* Succeeded: the bytes sent. */
	*sent = (size_t)put;
	return 0;
}

/*
 * Starts the backend and the core: the first backend that opens, the
 * core rootless (each top-level X window a window of the desktop) at its
 * default size, or one window of the size the settings give, the core
 * fonts from mikuterm's folder, the windows drawn and shown through the
 * system's Vulkan (unless X11Render is cpu, or the backend has no Vulkan
 * surface), else drawn in memory.  Returns 0, or an errno value.
 */
static int
child_start_server(
	void *context,
	struct x11_serve *serve)
{
	struct child *child;
	struct x11server_options options;
	unsigned index;
	int error;

	/* The backend. */
	child = context;
	error = ENODEV;
	for (index = 0U; index < sizeof(child_backends) / sizeof(child_backends[0]); index++) {
		error = child_backends[index].open(&child->backend);
		if (error == 0) {
			child->kind = &child_backends[index];
			break;
		}
		child->backend = NULL;
	}
	if (child->backend == NULL)
		return error;

	/* The core, rootless, or one window of the size the settings give. */
	memset(&options, 0, sizeof(options));
	options.mode = X11SERVER_ROOTLESS;
	if (serve->root_width != 0 && serve->root_height != 0) {
		options.width = serve->root_width;
		options.height = serve->root_height;
		options.mode = X11SERVER_ROOTED;
	}
	options.backend = child->kind->ops;
	options.backend_context = child->backend;

	/* The system's Vulkan loader, unless the processor is asked to draw. */
	if ((serve->flags & X11_HOST_RENDER_CPU) == 0U && child->kind->vulkan_wsi != NULL) {
		options.vulkan_loader = (x11server_vulkan_function)vkGetInstanceProcAddr;
		options.vulkan_wsi = child->kind->vulkan_wsi;
	}
	x11_serve_options(serve, &options);
	error = x11server_create(&options, child_now(), &serve->server);
	if (error != 0)
		return error;

	/* Succeeded: the backend hears the core. */
	child->kind->attach(child->backend, serve->server);
	return 0;
}

/* Gives the time in milliseconds, for the core (it wraps). */
static uint32_t
child_now(void)
{
	struct timespec moment;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &moment);
	return (uint32_t)((uint64_t)moment.tv_sec * 1000U + (uint64_t)moment.tv_nsec / 1000000U);
}
