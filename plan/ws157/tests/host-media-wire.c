/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises both production media ends over host Wayland with the real database CLI. */
#include <keiland/keiland.h>
#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/system/system-protocol.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/paths.h"
#include <wayland-server.h>
#include <wayland-client.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test objects associate production compositor state with host protocol resources. */
struct wire_object {
	struct kwl_object object;
	struct wl_resource *resource;
};

/* Only the server thread mutates the relay and its deliberately blocked first snapshot. */
static struct kwl_server *wire_server;
static struct wl_display *wire_display;
static struct wl_event_source *wire_timer;
static int wire_block_snapshot = 1;

static int wire_dispatch(const void *implementation, void *target, uint32_t opcode, const struct wl_message *message, union wl_argument *arguments);
static void wire_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
static void wire_destroy(struct wl_resource *resource);
static int wire_tick(void *data);
static int wire_stop(int descriptor, uint32_t mask, void *data);
static void *wire_run(void *data);
static struct wire_object *wire_find(struct kwl_client *client, uint32_t id);

/*
 * Creates a production relay object backed by a host Wayland resource.
 */
struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	struct wire_object *item;
	struct wl_client *native;
	struct kwl_object *manager;

	/* The manager's resource identifies the one native client in this fixture. */
	item = calloc(1U, sizeof(*item));
	assert(item != NULL);
	manager = client->objects;
	assert(manager != NULL);
	native = wl_resource_get_client(((struct wire_object *)manager)->resource);
	item->object.client = client;
	item->object.id = id;
	item->object.kind = kind;
	item->object.version = version;
	item->object.next = client->objects;
	client->objects = &item->object;
	item->resource = wl_resource_create(native, &kl_system_media_v1_interface, 1, id);
	assert(item->resource != NULL);
	wl_resource_set_dispatcher(item->resource, wire_dispatch, NULL, item, wire_destroy);

	/* Succeeded: the production relay can emit through this protocol resource. */
	return &item->object;
}

/*
 * Destroys a fixture resource using the same relay detachment as the compositor.
 */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	struct wire_object *item;

	/* The resource destructor clears outstanding relay references before freeing the object. */
	item = (struct wire_object *)object;
	wl_resource_destroy(item->resource);
}

/*
 * Transfers the one received metadata descriptor to the production request handler.
 */
int
kwl_take_fd(
	struct kwl_client *client)
{
	int descriptor;

	/* The host protocol dispatcher gave this descriptor to the receiving resource. */
	assert(client->right_count == 1U);
	descriptor = client->rights[0];
	client->right_count = 0U;
	return descriptor;
}

/*
 * Emits production relay events using the host server's real wire encoder.
 */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const void *data,
	size_t size)
{
	struct wire_object *item;
	const uint32_t *words;
	union wl_argument arguments[2];

	/* The fixture serves one client; the object ID selects its native resource. */

	/* Media events contain one generation or two completion words. */
	item = wire_find(client, id);
	assert(item != NULL);
	words = data;
	arguments[0].u = words[0];
	if (size == 8U)
		arguments[1].u = words[1];
	wl_resource_post_event_array(item->resource, opcode, arguments);
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/*
 * Transfers snapshot descriptors through SCM_RIGHTS, including a consuming failed attempt.
 */
int
kwl_emit_fd(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const void *data,
	size_t size,
	int descriptor)
{
	struct wire_object *item;
	union wl_argument arguments[2];

	/* The fixture serves one client; the object ID selects its native resource. */
	(void)size;

	/* Backpressure consumes its duplicate; the production relay must retain and retry its original. */
	if (wire_block_snapshot) {
		wire_block_snapshot = 0;
		close(descriptor);
		return ENOBUFS;
	}

	/* The native encoder duplicates the descriptor into its own pending wire output. */
	item = wire_find(client, id);
	memcpy(&arguments[0].u, data, 4U);
	arguments[1].h = descriptor;
	wl_resource_post_event_array(item->resource, opcode, arguments);
	close(descriptor);
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/*
 * Verifies list, add, apply, watching and FD ownership across both real media implementations.
 */
int
main(
	int argc,
	char **argv)
{
	struct wl_event_loop *loop;
	struct wl_display *display;
	struct kl_system *system;
	struct wl_global *global;
	struct wl_event_source *stop;
	pthread_t thread;
	int sockets[2];
	int stopping[2];
	char source[4096];
	char text[8192];
	char update[96];
	const char *paths[1];
	FILE *original;
	FILE *changes;
	int descriptor;
	int error;
	int present;
	ssize_t bytes;
	unsigned changed;

	pid_t external;
	int external_status;
	unsigned tries;

	/* All persistence and session state is private to this fixture. */
	assert(argc == 2);
	setenv("HOME", argv[1], 1);
	wire_server = calloc(1U, sizeof(*wire_server));
	assert(wire_server != NULL);
	snprintf(wire_server->socket_path, sizeof(wire_server->socket_path), "%s/wayland-test", argv[1]);
	setenv("WAYLAND_DISPLAY", wire_server->socket_path, 1);
	setenv("XDG_RUNTIME_DIR", argv[1], 1);
	wire_display = wl_display_create();
	assert(wire_display != NULL);
	error = wl_display_add_socket(wire_display, "wayland-test");
	assert(error == 0);
	global = wl_global_create(wire_display, &kl_system_manager_v1_interface, 29, NULL, wire_bind);
	assert(global != NULL);
	loop = wl_display_get_event_loop(wire_display);
	wire_timer = wl_event_loop_add_timer(loop, wire_tick, NULL);
	wl_event_source_timer_update(wire_timer, 1);
	error = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets);
	assert(error == 0);
	assert(wl_client_create(wire_display, sockets[0]) != NULL);
	pipe(stopping);
	stop = wl_event_loop_add_fd(loop, stopping[0], WL_EVENT_READABLE, wire_stop, NULL);
	pthread_create(&thread, NULL, wire_run, NULL);

	/* Production libkeiland binds the version-29 manager and receives its media capability. */
	display = wl_display_connect_to_fd(sockets[1]);
	assert(display != NULL);
	system = kl_system_open(display);
	assert(system != NULL);
	present = kl_system_capabilities(system);
	assert((present & KL_SYSTEM_HAS_MEDIA) != 0U);
	error = kl_system_media_watch(system, 1U);
	assert(error == 0);
	error = kl_system_media_list(system, &descriptor);
	assert(error == 0 && descriptor >= 0);
	bytes = read(descriptor, text, sizeof(text));
	assert(bytes > 0);
	close(descriptor);
	kl_system_dispatch(system, &changed);

	/* A real path-list request causes a CLI copy and a metadata-only FD response. */
	snprintf(source, sizeof(source), "%s/photo space.png", argv[1]);
	original = fopen(source, "wb");
	assert(original != NULL);
	fwrite("\x89PNG\r\n\x1a\n", 1U, 8U, original);
	fclose(original);
	paths[0] = source;
	error = kl_system_media_add_paths(system, paths, 1U, &descriptor);
	assert(error == 0);
	bytes = read(descriptor, text, sizeof(text) - 1U);
	assert(bytes > 0);
	text[bytes] = 0;
	close(descriptor);
	assert(strstr(text, "photo space.png") != NULL);
	kl_system_dispatch(system, &changed);
	assert((changed & KL_SYSTEM_CHANGED_MEDIA) != 0U);

	/* Dirty updates travel as metadata FD input and remain visible in the subsequent response. */
	changes = tmpfile();
	assert(changes != NULL);
	memcpy(update, strstr(text, "\nP\t") + 3U, 32U);
	update[32] = 0;
	fprintf(changes, "P\t%s\t1\t2\n", update);
	fflush(changes);
	fseek(changes, 0L, SEEK_SET);
	error = kl_system_media_apply(system, fileno(changes), &descriptor);
	assert(error == 0);
	close(descriptor);
	fclose(changes);
	/* A separately invoked CLI publishes its commit through libkeiland, not a daemon socket. */
	kl_system_dispatch(system, &changed);
	external = fork();
	assert(external >= 0);
	if (external == 0) {
		descriptor = open("/dev/null", O_WRONLY);
		dup2(descriptor, STDOUT_FILENO);
		close(descriptor);
		execl(KEILAND_BINDIR "/mediastorage", "mediastorage", "add", source, NULL);
		_exit(127);
	}

	/* The external command completes its notification round trip before it exits. */
	waitpid(external, &external_status, 0);
	assert(WIFEXITED(external_status) && WEXITSTATUS(external_status) == 0);
	for (tries = 0U; tries < 25U; tries++) {
		wl_display_roundtrip(display);
		kl_system_dispatch(system, &changed);
		if ((changed & KL_SYSTEM_CHANGED_MEDIA) != 0U)
			break;
		usleep(1000U);
	}

	assert((changed & KL_SYSTEM_CHANGED_MEDIA) != 0U);
	kl_system_close(system);
	wl_display_disconnect(display);
	write(stopping[1], "x", 1U);
	pthread_join(thread, NULL);
	kwl_media_library_close();
	wl_event_source_remove(stop);
	wl_event_source_remove(wire_timer);
	wl_display_destroy_clients(wire_display);
	wl_display_destroy(wire_display);
	close(stopping[0]);
	close(stopping[1]);
	free(wire_server);
	printf("PASS media wire: real client/compositor/CLI, list, path add, apply FD, external CLI notification/watch, consumed backpressure retry\n");
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/* Finds the native resource associated with a live production object. */
static struct wire_object *
wire_find(
	struct kwl_client *client,
	uint32_t id)
{
	struct kwl_object *object;

	/* The fixture has one native client with the same object IDs as its production relay. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->id == id)
			return (struct wire_object *)object;
	}

	/* No live object owns this protocol ID. */
	return NULL;
}

/* Dispatches decoded host requests into the production compositor's media relay. */
static int
wire_dispatch(
	const void *implementation,
	void *target,
	uint32_t opcode,
	const struct wl_message *message,
	union wl_argument *arguments)
{
	struct wl_resource *resource;
	struct wire_object *item;
	unsigned char bytes[65536];
	uint32_t length;
	size_t size;
	int error;

	/* The dispatcher uses the resource owner and typed arguments supplied by Wayland. */
	(void)implementation;
	(void)message;

	/* Host decoding supplies the exact arguments the compositor normally sees on its wire. */
	resource = target;
	item = wl_resource_get_user_data(resource);
	if (item->object.kind != KWL_SYSTEM_MEDIA) {
		assert(opcode == KL_SYSTEM_MANAGER_GET_MEDIA || opcode == 0U);
		if (opcode == 0U) {
			wl_resource_destroy(resource);
			return 0;
		}

		/* Relays the manager's new object ID to production creation. */
		memcpy(bytes, &arguments[0].n, 4U);
		error = kwl_media_library_create(&item->object, bytes, 4U);
		assert(error == 0);
		return 0;
	}

	/* Requests use the native request word followed by a string or separately owned descriptor. */
	size = 0U;
	if (opcode != KL_SYSTEM_MEDIA_DESTROY && opcode != KL_SYSTEM_MEDIA_NOTIFY) {
		memcpy(bytes, &arguments[0].u, 4U);
		size = 4U;
	}

	/* Preserves the production request's string padding or separate FD ownership. */
	if (opcode == KL_SYSTEM_MEDIA_ADD) {
		length = (uint32_t)strlen(arguments[1].s) + 1U;
		memcpy(bytes + 4U, &length, 4U);
		memcpy(bytes + 8U, arguments[1].s, length);
		size = 8U + ((length + 3U) & ~3U);
	} else if (opcode == KL_SYSTEM_MEDIA_APPLY) {
		item->object.client->rights[0] = arguments[1].h;
		item->object.client->right_count = 1U;
	}

	/* Executes the decoded operation through the actual compositor relay. */
	error = kwl_media_library_request(&item->object, opcode, bytes, size);
	assert(error == 0);
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/* Advertises only media so the fixture has no unrelated OS-service dependencies. */
static void
wire_bind(
	struct wl_client *client,
	void *data,
	uint32_t version,
	uint32_t id)
{
	struct wire_object *item;
	struct kwl_client *relay;

	/* Fixture state is process-local and owned by the server thread. */
	(void)data;

	/* The production client first receives capabilities, then creates its media object. */
	relay = calloc(1U, sizeof(*relay));
	assert(relay != NULL);
	relay->server = wire_server;
	relay->next = wire_server->clients;
	wire_server->clients = relay;
	item = calloc(1U, sizeof(*item));
	assert(item != NULL);
	item->object.id = id;
	item->object.client = relay;
	item->object.kind = KWL_SYSTEM_MANAGER;
	item->object.next = relay->objects;
	relay->objects = &item->object;
	item->resource = wl_resource_create(client, &kl_system_manager_v1_interface, (int)version, id);
	assert(item->resource != NULL);
	wl_resource_set_dispatcher(item->resource, wire_dispatch, NULL, item, wire_destroy);
	wl_resource_post_event(item->resource, 0U, KL_SYSTEM_CAPABILITY_MEDIA);
}

/* Drops relay references before the host frees its native resource. */
static void
wire_destroy(
	struct wl_resource *resource)
{
	struct wire_object *item;
	struct kwl_object **link;
	struct kwl_client *client;
	struct kwl_client **clients;

	/* A destroyed client cannot receive a late helper response. */
	item = wl_resource_get_user_data(resource);
	client = item->object.client;
	kwl_media_library_gone(&item->object);
	for (link = &client->objects; *link != NULL; link = &(*link)->next) {
		if (*link == &item->object) {
			*link = item->object.next;
			break;
		}
	}

	/* No linked object or pending request retains this allocation. */
	free(item);

	/* A client record outlives its manager while another resource still refers to it. */
	if (client->objects == NULL) {
		for (clients = &wire_server->clients; *clients != NULL; clients = &(*clients)->next) {
			if (*clients == client) {
				*clients = client->next;
				break;
			}
		}

		/* The final resource releases its native client projection. */
		free(client);
	}
}

/* Runs the production asynchronous helper queue on the server event loop. */
static int
wire_tick(
	void *data)
{
	/* Fixture state is process-local and owned by the server thread. */
	(void)data;

	/* Frequent bounded ticks let list/add/apply complete without rendering or GPU initialization. */
	kwl_media_library_tick(wire_server);
	wl_event_source_timer_update(wire_timer, 2);
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/* Stops the server through its event loop rather than a cross-thread library call. */
static int
wire_stop(
	int descriptor,
	uint32_t mask,
	void *data)
{
	char byte;

	/* Only the pipe's stop byte matters to this callback. */
	(void)mask;
	(void)data;

	/* The main test writes one stop byte after its client connection has closed. */
	read(descriptor, &byte, 1U);
	wl_display_terminate(wire_display);
	/* Succeeded: this fixture operation completed. */
	return 0;
}

/* Serves real protocol traffic while the main thread runs the client API. */
static void *
wire_run(
	void *data)
{
	/* Fixture state is process-local and owned by the server thread. */
	(void)data;

	/* All compositor and backend state stays on this server thread. */
	wl_display_run(wire_display);
	return NULL;
}
