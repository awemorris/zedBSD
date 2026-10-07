/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard inset (ws102-p015, plan/ws102/design.md section 2.8): the
 * wrapper of zdesktop's kl_keyboard_inset_v1 protocol.  A window hears
 * how much of it the on-screen keyboard covers, from its right and bottom
 * edges, when the keyboard opens, closes or changes the window.  With a
 * compositor that does not have the protocol nothing is made (ENOTSUP) and
 * the window hears nothing, as before.
 */

#include <keiland/keiland.h>

#include "ui/internal.h"

#include <wayland-client.h>
#include "userland/desktop/libwayland/keiland-keyboard-inset-v1-client-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The version of the protocol this library speaks. */
#define INSET_VERSION		1U

/*
 * One window's inset: its kl_keyboard_inset_v1, and the application's
 * callback and its data.
 */
struct kl_keyboard_inset {
	struct kl_keyboard_inset_v1 *proxy;
	kl_keyboard_inset_fn callback;
	void *data;
};

static struct kl_keyboard_inset_manager_v1 *inset_bind(struct wl_display *display);
static void inset_event(void *data, struct kl_keyboard_inset_v1 *object, int32_t right, int32_t bottom, uint32_t reason);

/* The inset's callback. */
static const struct kl_keyboard_inset_v1_listener inset_listener = {
	inset_event
};

/*
 * Asks for a window's keyboard inset: callback hears each change on the
 * application's default queue.  Returns NULL with errno set: ENOTSUP for a
 * compositor without the protocol, EINVAL, ENOMEM.
 */
struct kl_keyboard_inset *
kl_keyboard_inset_create(
	struct wl_display *display,
	struct xdg_toplevel *toplevel,
	kl_keyboard_inset_fn callback,
	void *data)
{
	struct kl_keyboard_inset_manager_v1 *manager;
	struct kl_keyboard_inset *inset;
	int error;

	/* A window and a callback. */
	if (display == NULL || toplevel == NULL || callback == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* zdesktop's manager, bound for this window. */
	manager = inset_bind(display);
	if (manager == NULL)
		return NULL;

	/* The record. */
	inset = calloc(1, sizeof(*inset));
	if (inset == NULL) {
		kl_keyboard_inset_manager_v1_destroy(manager);
		errno = ENOMEM;
		return NULL;
	}
	inset->callback = callback;
	inset->data = data;

	/* The protocol object; the binding is not needed after it (the inset stays). */
	inset->proxy = kl_keyboard_inset_manager_v1_get_inset(manager, toplevel);
	kl_keyboard_inset_manager_v1_destroy(manager);
	if (inset->proxy == NULL) {
		free(inset);
		errno = ENOMEM;
		return NULL;
	}

	/* Its events come to the callback. */
	error = kl_keyboard_inset_v1_add_listener(inset->proxy, &inset_listener, inset);
	if (error != 0) {
		kl_keyboard_inset_v1_destroy(inset->proxy);
		free(inset);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the window hears the keyboard. */
	return inset;
}

/*
 * Stops hearing the keyboard: the protocol object and the record go.
 */
void
kl_keyboard_inset_destroy(
	struct kl_keyboard_inset *inset)
{
	/* No inset, nothing to destroy. */
	if (inset == NULL)
		return;

	/* The protocol object, then the record. */
	kl_keyboard_inset_v1_destroy(inset->proxy);
	free(inset);
}

/* Passes an inset event to the application's callback. */
static void
inset_event(
	void *data,
	struct kl_keyboard_inset_v1 *object,
	int32_t right,
	int32_t bottom,
	uint32_t reason)
{
	struct kl_keyboard_inset *inset;

	/* The record the listener was given. */
	(void)object;
	inset = data;

	/* The callback. */
	inset->callback(inset->data, right, bottom, reason);
}

/* Binds zdesktop's inset manager: from an application's registry, or found by a search of the library's own. */
static struct kl_keyboard_inset_manager_v1 *
inset_bind(
	struct wl_display *display)
{
	struct kl_keyboard_inset_manager_v1 *manager;
	struct keiui_global_search search;
	int error;

	/* The manager's global. */
	error = keiui_global_find(&search, display, "kl_keyboard_inset_manager_v1");
	if (error != 0) {
		keiui_global_end(&search);
		errno = error;
		return NULL;
	}

	/* The manager, bound when announced; the search ends. */
	manager = keiui_global_bind(&search, &kl_keyboard_inset_manager_v1_interface, INSET_VERSION);
	keiui_global_end(&search);

	/* A compositor without the protocol. */
	if (search.name == 0U) {
		errno = ENOTSUP;
		return NULL;
	}

	/* The binding could not be made. */
	if (manager == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the manager. */
	return manager;
}
