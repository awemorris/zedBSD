/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The search for one global of a display (WS131 p015), shared by
 * libkeiland's objects (the menu service, the titlebar, the glass, the
 * keyboard inset, the editing operations).  On an application's display
 * the application's table has every global already, and the object binds
 * from the application's registry without a search.  On any other display
 * the search runs a registry of its own on a queue of its own, so that no
 * event of the application's is dispatched by its roundtrip; the binding
 * is moved to the display's default queue.
 */

#include "internal.h"

#include <wayland-client.h>

#include <errno.h>
#include <string.h>

static void globals_announced(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void globals_removed(void *data, struct wl_registry *registry, uint32_t name);

/* The search registry's callbacks. */
static const struct wl_registry_listener globals_listener = {
	globals_announced,
	globals_removed
};

/*
 * Finds a display's global of an interface.  Returns 0 (search->name is 0
 * when the compositor has none) or ENOMEM.
 */
int
keiui_global_find(
	struct keiui_global_search *search,
	struct wl_display *display,
	const char *interface)
{
	int status;

	/* Nothing found yet. */
	memset(search, 0, sizeof(*search));
	search->interface = interface;

	/* An application's table, without a search. */
	search->registry = keiui_app_global(display, interface, &search->name, &search->version);
	if (search->registry != NULL)
		return 0;

	/* The search's own queue, and the display as seen from it. */
	search->queue = wl_display_create_queue(display);
	if (search->queue == NULL)
		return ENOMEM;
	search->wrapper = wl_proxy_create_wrapper(display);
	if (search->wrapper == NULL)
		return ENOMEM;

	/* What the wrapper makes lives on the search's queue. */
	wl_proxy_set_queue((struct wl_proxy *)search->wrapper, search->queue);

	/* The globals, announced to this search alone. */
	search->own = wl_display_get_registry(search->wrapper);
	if (search->own == NULL)
		return ENOMEM;
	status = wl_registry_add_listener(search->own, &globals_listener, search);
	if (status == 0)
		(void)wl_display_roundtrip_queue(display, search->queue);

	/* Bound from the search's registry. */
	search->registry = search->own;
	return 0;
}

/*
 * Binds the global found at a version, on the display's default queue;
 * NULL when nothing was found or the binding could not be made.
 */
void *
keiui_global_bind(
	struct keiui_global_search *search,
	const struct wl_interface *type,
	uint32_t version)
{
	void *proxy;

	/* Nothing found. */
	if (search->name == 0U || search->registry == NULL)
		return NULL;

	/* The binding, moved to the default queue (where an application's registry binds already). */
	proxy = wl_registry_bind(search->registry, search->name, type, version);
	if (proxy != NULL)
		wl_proxy_set_queue((struct wl_proxy *)proxy, NULL);
	return proxy;
}

/*
 * Ends a search: its own registry, wrapper and queue go (an application's
 * registry stays).
 */
void
keiui_global_end(
	struct keiui_global_search *search)
{
	/* The search's own objects. */
	if (search->own != NULL)
		wl_registry_destroy(search->own);
	if (search->wrapper != NULL)
		wl_proxy_wrapper_destroy(search->wrapper);
	if (search->queue != NULL)
		wl_event_queue_destroy(search->queue);

	/* Nothing to bind from any more. */
	search->own = NULL;
	search->wrapper = NULL;
	search->queue = NULL;
	search->registry = NULL;
}

/* Notes the first global of the interface looked for. */
static void
globals_announced(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct keiui_global_search *search;
	int same;

	/* The first of the interface. */
	(void)registry;
	search = data;
	same = strcmp(interface, search->interface);
	if (same == 0 && search->name == 0U) {
		search->name = name;
		search->version = version;
	}
}

/* A global going away during the short search changes nothing. */
static void
globals_removed(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to forget. */
	(void)data;
	(void)registry;
	(void)name;
}
