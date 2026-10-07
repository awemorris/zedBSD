/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop surface (ws094-p003, plan/ws094/design.md §3): the wrapper
 * of the compositor's kl_desktop_v1 protocol, which gives the program the
 * compositor started for the desktop's icons the surface over the
 * wallpaper and under every window.
 *
 * The protocol's interfaces are described here, as wayland-scanner would
 * make them, over libwayland's marshalling (the protocol is the compositor's
 * alone and has no generated code in libwayland).
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Marks protocol callback arguments that this client does not inspect. */
#define UNUSED_PARAMETER(name) ((void)(name))

/* The version of the protocol this library speaks, and its requests. */
#define DESKTOP_VERSION		1U
#define DESKTOP_MANAGER_DESTROY	0U
#define DESKTOP_MANAGER_GET	1U
#define DESKTOP_SURFACE_DESTROY	0U
#define DESKTOP_SURFACE_ACK	1U

/*
 * The desktop surface of a program: its kl_desktop_surface_v1 and the
 * program's listener with its data.
 */
struct kl_desktop {
	struct wl_proxy *proxy;
	const struct kl_desktop_listener *listener;
	void *data;
};

/* What the registry search found: the manager's global name, 0 for none. */
struct desktop_search {
	uint32_t name;
};

/*
 * The listener of kl_desktop_surface_v1's one event, as libwayland
 * calls it.
 */
struct desktop_proxy_listener {
	void (*configure)(void *data, struct wl_proxy *proxy, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
};

static void desktop_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void desktop_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void desktop_configure(void *data, struct wl_proxy *proxy, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);

extern const struct wl_interface kl_desktop_manager_v1_interface;
extern const struct wl_interface kl_desktop_surface_v1_interface;

/* get_desktop_surface: the new desktop surface, the surface and the token. */
static const struct wl_interface *desktop_get_types[] = {
	&kl_desktop_surface_v1_interface,
	&wl_surface_interface,
	NULL,
};

/* The arguments of messages that name no interface (at most five). */
static const struct wl_interface *desktop_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of kl_desktop_manager_v1. */
static const struct wl_message desktop_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_desktop_surface", "nos", desktop_get_types },
};

/* kl_desktop_manager_v1. */
const struct wl_interface kl_desktop_manager_v1_interface = {
	"kl_desktop_manager_v1",
	1,
	2,
	desktop_manager_requests,
	0,
	NULL
};

/* The requests of kl_desktop_surface_v1. */
static const struct wl_message desktop_surface_requests[] = {
	{ "destroy", "", NULL },
	{ "ack_configure", "u", desktop_plain_types },
};

/* The events of kl_desktop_surface_v1. */
static const struct wl_message desktop_surface_events[] = {
	{ "configure", "uiiii", desktop_plain_types },
};

/* kl_desktop_surface_v1. */
const struct wl_interface kl_desktop_surface_v1_interface = {
	"kl_desktop_surface_v1",
	1,
	2,
	desktop_surface_requests,
	1,
	desktop_surface_events
};

/* The registry's callbacks while the manager is looked for. */
static const struct wl_registry_listener desktop_registry_listener = {
	desktop_global,
	desktop_global_remove
};

/* The desktop surface's listener. */
static const struct desktop_proxy_listener desktop_proxy_listener = {
	desktop_configure
};

static struct wl_proxy *desktop_bind(struct wl_display *display);

/*
 * Gives a surface the desktop's role with the token the compositor gave
 * the program.  The listener hears configure (where the surface is and its
 * size) before anything is drawn; the program acknowledges it with
 * kl_desktop_ack.  Returns NULL with errno set: EINVAL without a
 * token, ENOTSUP for a compositor without the desktop, ENOMEM.  A token
 * the compositor does not know ends the connection (a protocol error).
 */
struct kl_desktop *
kl_desktop_create(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *token,
	const struct kl_desktop_listener *listener,
	void *data)
{
	struct kl_desktop *desktop;
	struct wl_proxy *manager;
	int status;

	/* A token is needed. */
	if (token == NULL || token[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}

	/* The compositor's manager, bound for this surface. */
	manager = desktop_bind(display);
	if (manager == NULL)
		return NULL;

	/* The record. */
	desktop = calloc(1, sizeof(*desktop));
	if (desktop == NULL) {
		wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
		wl_proxy_destroy(manager);
		errno = ENOMEM;
		return NULL;
	}

	/* The program's listener and its data. */
	desktop->listener = listener;
	desktop->data = data;

	/* The protocol object; the binding is not needed after it. */
	desktop->proxy = wl_proxy_marshal_constructor(manager, DESKTOP_MANAGER_GET, &kl_desktop_surface_v1_interface, NULL, surface, token);
	if (desktop->proxy == NULL) {
		wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
		wl_proxy_destroy(manager);
		free(desktop);
		errno = ENOMEM;
		return NULL;
	}

	/* Releases the binding once the new protocol object owns its role request. */
	wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
	wl_proxy_destroy(manager);

	/* The configure comes to the record. */
	status = wl_proxy_add_listener(desktop->proxy, (void (**)(void))&desktop_proxy_listener, desktop);
	if (status != 0) {
		wl_proxy_destroy(desktop->proxy);
		free(desktop);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the surface is the desktop's once the compositor agrees. */
	return desktop;
}

/*
 * Acknowledges a configure: the next commit is drawn for it.
 */
void
kl_desktop_ack(
	struct kl_desktop *desktop,
	uint32_t serial)
{
	/* The request, sent with the next flush. */
	wl_proxy_marshal(desktop->proxy, DESKTOP_SURFACE_ACK, serial);

	/* Succeeded: the configure acknowledgement is queued. */
	return;
}

/*
 * Gives the desktop's role up; the surface shows nothing more.
 */
void
kl_desktop_destroy(
	struct kl_desktop *desktop)
{
	/* No desktop, nothing to destroy. */
	if (desktop == NULL)
		return;

	/* The protocol object, then the record. */
	wl_proxy_marshal(desktop->proxy, DESKTOP_SURFACE_DESTROY);
	wl_proxy_destroy(desktop->proxy);
	free(desktop);

	/* Succeeded: the desktop role and record are released. */
	return;
}

/* Binds the compositor's desktop manager through a registry of the library's own (as glass.c does). */
static struct wl_proxy *
desktop_bind(
	struct wl_display *display)
{
	struct desktop_search search;
	struct wl_event_queue *queue;
	struct wl_display *wrapper;
	struct wl_registry *registry;
	struct wl_proxy *manager;
	int status;

	/* The search's own queue. */
	queue = wl_display_create_queue(display);
	if (queue == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The display as the search sees it. */
	wrapper = wl_proxy_create_wrapper(display);
	if (wrapper == NULL) {
		wl_event_queue_destroy(queue);
		errno = ENOMEM;
		return NULL;
	}

	/* What the wrapper makes lives on the search's queue. */
	wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);

	/* The globals, announced to this search alone. */
	search.name = 0;
	registry = wl_display_get_registry(wrapper);
	if (registry == NULL) {
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* Receives globals before the search begins dispatching its private queue. */
	status = wl_registry_add_listener(registry, &desktop_registry_listener, &search);
	if (status != 0) {
		wl_registry_destroy(registry);
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* A failed roundtrip cannot supply a usable manager, even after partial announcements. */
	status = wl_display_roundtrip_queue(display, queue);
	if (status < 0) {
		wl_registry_destroy(registry);
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* The manager, bound when announced, is moved to the application's default queue. */
	manager = NULL;
	if (registry != NULL && search.name != 0U) {
		manager = wl_registry_bind(registry, search.name, &kl_desktop_manager_v1_interface, DESKTOP_VERSION);
		if (manager != NULL)
			wl_proxy_set_queue(manager, NULL);
	}

	/* The search's objects go. */
	if (registry != NULL)
		wl_registry_destroy(registry);
	wl_proxy_wrapper_destroy(wrapper);
	wl_event_queue_destroy(queue);

	/* A compositor without the desktop. */
	if (manager == NULL) {
		errno = ENOTSUP;
		return NULL;
	}

	/* Succeeded: the manager. */
	return manager;
}

/* Keeps the desktop manager's global name when it is announced. */
static void
desktop_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct desktop_search *search;
	int match;

	UNUSED_PARAMETER(registry);
	UNUSED_PARAMETER(version);

	/* Only the desktop manager is looked for. */
	search = data;
	match = strcmp(interface, "kl_desktop_manager_v1");
	if (match == 0)
		search->name = name;

	/* Succeeded: any matching manager name has been recorded. */
	return;
}

/* A global going away does not matter to the search. */
static void
desktop_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(registry);
	UNUSED_PARAMETER(name);

	/* Succeeded: the completed search needs no removal bookkeeping. */
	return;
}

/* Passes a configure on to the program's listener. */
static void
desktop_configure(
	void *data,
	struct wl_proxy *proxy,
	uint32_t serial,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	struct kl_desktop *desktop;

	UNUSED_PARAMETER(proxy);

	/* The record the listener was added with. */
	desktop = data;

	/* The program's listener, when it has one. */
	if (desktop->listener != NULL && desktop->listener->configure != NULL)
		desktop->listener->configure(desktop->data, desktop, serial, x, y, width, height);

	/* Succeeded: any registered listener has received the configure. */
	return;
}
