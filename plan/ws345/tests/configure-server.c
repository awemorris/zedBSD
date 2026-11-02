/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server of configure-test.c, on a thread of its own: wl_compositor 4
 * (surfaces and regions), wl_shm (libwayland-server's) and xdg_wm_base 4
 * with one toplevel.  The first commit of the toplevel's surface gets the
 * first configure; the test asks for more (server_configure, through a
 * pipe the server's loop reads).  Every acknowledgement must name a serial
 * sent and not consumed yet, and every commit with a buffer must have the
 * size of the configure acknowledged last (xdg-shell: a frame committed
 * after an ack_configure is the client's answer to it).
 */

#include "configure-server.h"
#include "xdg-shell-server.h"

#include <wayland-server.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIGURES_MAX	256U

/* A configure sent, by its serial. */
struct sent {
	uint32_t serial;
	uint32_t width;
	uint32_t height;
};

/* What the test asks the server's thread for. */
struct command {
	uint32_t width;
	uint32_t height;
	int maximized;
};

/* The server, its loop's thread and the pipe of commands; the toplevel's objects (server's thread). */
static struct wl_display *server;
static pthread_t server_thread;
static _Atomic int server_stop_asked;
static int command_pipe[2];
static struct wl_resource *toplevel_surface;
static struct wl_resource *toplevel_role;
static struct wl_resource *toplevel;
static struct wl_resource *pending_buffer;
static int first_sent;
static uint32_t first_width;
static uint32_t first_height;
static uint32_t next_serial = 100U;

/* What the test reads, and the configures sent (lock). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct server_state state;
static struct sent sent[CONFIGURES_MAX];
static unsigned sent_count;
static int acked_index = -1;
static uint32_t last_sent_serial;

static void
violation(
	const char *what)
{
	state.violations++;
	if (state.violation[0] == '\0')
		snprintf(state.violation, sizeof(state.violation), "%s", what);
}

/* Sends a configure of the toplevel and keeps it (server's thread). */
static void
send_configure(
	uint32_t width,
	uint32_t height,
	int maximized)
{
	struct wl_array states;
	uint32_t *slot;
	uint32_t serial;

	if (toplevel == NULL || toplevel_role == NULL)
		return;
	wl_array_init(&states);
	if (maximized) {
		slot = wl_array_add(&states, sizeof(*slot));
		*slot = XDG_TOPLEVEL_STATE_MAXIMIZED;
	}
	slot = wl_array_add(&states, sizeof(*slot));
	*slot = XDG_TOPLEVEL_STATE_ACTIVATED;
	serial = next_serial++;
	xdg_toplevel_send_configure(toplevel, (int32_t)width, (int32_t)height, &states);
	xdg_surface_send_configure(toplevel_role, serial);
	wl_array_release(&states);

	pthread_mutex_lock(&lock);
	if (sent_count < CONFIGURES_MAX) {
		sent[sent_count].serial = serial;
		sent[sent_count].width = width;
		sent[sent_count].height = height;
		sent_count++;
	}
	last_sent_serial = serial;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}

/* A resource whose requests are all ignored but its destructor (opcode 0). */
static int
dispatch_plain(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	(void)implementation;
	(void)message;
	(void)arguments;
	if (opcode == 0U)
		wl_resource_destroy(target);
	return 0;
}

static void
surface_gone(
	struct wl_resource *resource)
{
	if (resource == toplevel_surface) {
		toplevel_surface = NULL;
		pending_buffer = NULL;
	}
}

static void
role_gone(
	struct wl_resource *resource)
{
	if (resource == toplevel_role)
		toplevel_role = NULL;
}

static void
toplevel_gone(
	struct wl_resource *resource)
{
	if (resource == toplevel)
		toplevel = NULL;
}

/* A commit of the toplevel's surface: the first configure, or a frame checked against the size acknowledged. */
static void
surface_commit(
	struct wl_resource *surface)
{
	struct wl_shm_buffer *shm;
	uint32_t width;
	uint32_t height;
	char text[200];

	if (surface != toplevel_surface || toplevel == NULL)
		return;
	if (!first_sent) {
		first_sent = 1;
		send_configure(first_width, first_height, 0);
		return;
	}
	if (pending_buffer == NULL)
		return;
	shm = wl_shm_buffer_get(pending_buffer);
	if (shm == NULL) {
		pthread_mutex_lock(&lock);
		violation("a buffer not of wl_shm");
		pthread_mutex_unlock(&lock);
		pending_buffer = NULL;
		return;
	}
	width = (uint32_t)wl_shm_buffer_get_width(shm);
	height = (uint32_t)wl_shm_buffer_get_height(shm);
	wl_buffer_send_release(pending_buffer);
	pending_buffer = NULL;

	pthread_mutex_lock(&lock);
	state.commits++;
	state.commit_width = width;
	state.commit_height = height;
	if (acked_index < 0) {
		violation("a frame before any acknowledgement");
	} else if (sent[acked_index].width != width || sent[acked_index].height != height) {
		snprintf(text, sizeof(text), "a frame %ux%u after the ack of %u (%ux%u)",
		    width, height, sent[acked_index].serial, sent[acked_index].width, sent[acked_index].height);
		violation(text);
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}

static int
dispatch_surface(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	struct wl_resource *callback;

	(void)implementation;
	(void)message;
	switch (opcode) {
	case 0:
		wl_resource_destroy(target);
		break;
	case 1:
		if (target == toplevel_surface)
			pending_buffer = (struct wl_resource *)arguments[0].o;
		break;
	case 3:
		callback = wl_resource_create(wl_resource_get_client(target), &wl_callback_interface, 1, arguments[0].n);
		if (callback != NULL) {
			wl_callback_send_done(callback, 0U);
			wl_resource_destroy(callback);
		}
		break;
	case 6:
		surface_commit(target);
		break;
	default:
		break;
	}
	return 0;
}

static int
dispatch_compositor(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	struct wl_resource *made;
	struct wl_client *client;
	uint32_t version;

	(void)implementation;
	(void)message;
	client = wl_resource_get_client(target);
	version = (uint32_t)wl_resource_get_version(target);
	if (opcode == 0U) {
		made = wl_resource_create(client, &wl_surface_interface, (int)version, arguments[0].n);
		if (made != NULL)
			wl_resource_set_dispatcher(made, dispatch_surface, NULL, NULL, surface_gone);
	} else if (opcode == 1U) {
		made = wl_resource_create(client, &wl_region_interface, 1, arguments[0].n);
		if (made != NULL)
			wl_resource_set_dispatcher(made, dispatch_plain, NULL, NULL, NULL);
	}
	return 0;
}

/* An acknowledgement: a serial sent, later than the one acknowledged last. */
static void
role_ack(
	uint32_t serial)
{
	unsigned index;
	int found;

	pthread_mutex_lock(&lock);
	found = -1;
	for (index = 0; index < sent_count; index++) {
		if (sent[index].serial == serial)
			found = (int)index;
	}
	if (found < 0 || found <= acked_index) {
		state.bad_acks++;
		violation("an acknowledgement of a serial not sent or consumed");
	} else {
		acked_index = found;
		state.acks++;
		state.last_ack = serial;
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}

static int
dispatch_role(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	struct wl_resource *made;

	(void)implementation;
	(void)message;
	switch (opcode) {
	case 0:
		wl_resource_destroy(target);
		break;
	case 1:
		made = wl_resource_create(wl_resource_get_client(target), &xdg_toplevel_interface,
		    wl_resource_get_version(target), arguments[0].n);
		if (made == NULL)
			break;
		wl_resource_set_dispatcher(made, dispatch_plain, NULL, NULL, toplevel_gone);
		toplevel = made;
		toplevel_role = target;
		toplevel_surface = wl_resource_get_user_data(target);
		break;
	case 4:
		if (target == toplevel_role)
			role_ack(arguments[0].u);
		break;
	default:
		break;
	}
	return 0;
}

static int
dispatch_shell(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	struct wl_resource *made;
	struct wl_client *client;
	int version;

	(void)implementation;
	(void)message;
	client = wl_resource_get_client(target);
	version = wl_resource_get_version(target);
	switch (opcode) {
	case 0:
		wl_resource_destroy(target);
		break;
	case 1:
		made = wl_resource_create(client, &xdg_positioner_interface, version, arguments[0].n);
		if (made != NULL)
			wl_resource_set_dispatcher(made, dispatch_plain, NULL, NULL, NULL);
		break;
	case 2:
		made = wl_resource_create(client, &xdg_surface_interface, version, arguments[0].n);
		if (made != NULL)
			wl_resource_set_dispatcher(made, dispatch_role, NULL, arguments[1].o, role_gone);
		break;
	default:
		break;
	}
	return 0;
}

static void
bind_compositor(
	struct wl_client *client,
	void *data,
	uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
	if (resource != NULL)
		wl_resource_set_dispatcher(resource, dispatch_compositor, NULL, NULL, NULL);
}

static void
bind_shell(
	struct wl_client *client,
	void *data,
	uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
	if (resource != NULL)
		wl_resource_set_dispatcher(resource, dispatch_shell, NULL, NULL, NULL);
}

/* The test's commands, read by the server's loop. */
static int
command_ready(
	int fd,
	uint32_t mask,
	void *data)
{
	struct command command;
	ssize_t length;

	(void)mask;
	(void)data;
	length = read(fd, &command, sizeof(command));
	if (length == (ssize_t)sizeof(command))
		send_configure(command.width, command.height, command.maximized);
	return 0;
}

static void *
server_run(
	void *argument)
{
	struct wl_event_loop *loop;

	(void)argument;
	loop = wl_display_get_event_loop(server);
	while (!server_stop_asked) {
		wl_event_loop_dispatch(loop, 50);
		wl_display_flush_clients(server);
	}
	return NULL;
}

/* Starts the server with the first configure's size; its socket's name, or NULL. */
const char *
server_start(
	uint32_t width,
	uint32_t height)
{
	const char *name;
	int status;

	first_width = width;
	first_height = height;
	server = wl_display_create();
	if (server == NULL)
		return NULL;
	name = wl_display_add_socket_auto(server);
	if (name == NULL)
		return NULL;
	status = wl_display_init_shm(server);
	if (status != 0)
		return NULL;
	if (wl_global_create(server, &wl_compositor_interface, 4, NULL, bind_compositor) == NULL)
		return NULL;
	if (wl_global_create(server, &xdg_wm_base_interface, 4, NULL, bind_shell) == NULL)
		return NULL;
	status = pipe(command_pipe);
	if (status != 0)
		return NULL;
	if (wl_event_loop_add_fd(wl_display_get_event_loop(server), command_pipe[0], WL_EVENT_READABLE, command_ready, NULL) == NULL)
		return NULL;
	status = pthread_create(&server_thread, NULL, server_run, NULL);
	if (status != 0)
		return NULL;
	return name;
}

void
server_stop(void)
{
	server_stop_asked = 1;
	pthread_join(server_thread, NULL);
}

/* Has the server's thread send a configure; returns its serial once it was sent (0 after 3 s). */
uint32_t
server_configure(
	uint32_t width,
	uint32_t height,
	int maximized)
{
	struct command command;
	struct timespec until;
	uint32_t before;
	uint32_t serial;
	ssize_t length;

	pthread_mutex_lock(&lock);
	before = last_sent_serial;
	pthread_mutex_unlock(&lock);
	command.width = width;
	command.height = height;
	command.maximized = maximized;
	length = write(command_pipe[1], &command, sizeof(command));
	if (length != (ssize_t)sizeof(command))
		return 0U;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 3;
	serial = 0U;
	pthread_mutex_lock(&lock);
	while (last_sent_serial == before) {
		if (pthread_cond_timedwait(&changed, &lock, &until) != 0)
			break;
	}
	if (last_sent_serial != before)
		serial = last_sent_serial;
	pthread_mutex_unlock(&lock);
	return serial;
}

void
server_state(
	struct server_state *copy)
{
	pthread_mutex_lock(&lock);
	*copy = state;
	pthread_mutex_unlock(&lock);
}
