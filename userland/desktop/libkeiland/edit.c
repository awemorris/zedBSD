/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editing operations (ws102-p017, plan/ws102/design.md section 2.10):
 * the wrapper of zdesktop's kl_edit_v1 protocol.  A window says which
 * editing operations it carries out and its state, and hears the
 * operations the on-screen keyboard's buttons ask for.  With a compositor
 * that does not have the protocol nothing is made (ENOTSUP) and the
 * keyboard sends such a window the keys instead.
 */

#include <keiland/keiland.h>

#include "ui/internal.h"

#include <wayland-client.h>
#include "userland/desktop/libwayland/keiland-edit-v1-client-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The version of the protocol this library speaks. */
#define EDIT_VERSION		1U

/*
 * One window's edit object: its kl_edit_v1, the application's
 * callback and its data, and the operations and the state last sent (so
 * that an unchanged state is not sent again).
 */
struct kl_edit {
	struct kl_edit_v1 *proxy;
	kl_edit_fn callback;
	void *data;
	int sent;
	uint32_t actions;
	uint32_t state;
};

static struct kl_edit_manager_v1 *edit_bind(struct wl_display *display);
static void edit_event(void *data, struct kl_edit_v1 *object, uint32_t action);

/* The edit object's callback. */
static const struct kl_edit_v1_listener edit_listener = {
	edit_event
};

/*
 * Asks for a window's edit object: callback hears each operation on the
 * application's default queue.  The window takes no operation until
 * kl_edit_set_state says which.  Returns NULL with errno set: ENOTSUP
 * for a compositor without the protocol, EINVAL, ENOMEM.
 */
struct kl_edit *
kl_edit_create(
	struct wl_display *display,
	struct xdg_toplevel *toplevel,
	kl_edit_fn callback,
	void *data)
{
	struct kl_edit_manager_v1 *manager;
	struct kl_edit *edit;
	int error;

	/* A window and a callback. */
	if (display == NULL || toplevel == NULL || callback == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* zdesktop's manager, bound for this window. */
	manager = edit_bind(display);
	if (manager == NULL)
		return NULL;

	/* The record. */
	edit = calloc(1, sizeof(*edit));
	if (edit == NULL) {
		kl_edit_manager_v1_destroy(manager);
		errno = ENOMEM;
		return NULL;
	}
	edit->callback = callback;
	edit->data = data;

	/* The protocol object; the binding is not needed after it (the edit object stays). */
	edit->proxy = kl_edit_manager_v1_get_edit(manager, toplevel);
	kl_edit_manager_v1_destroy(manager);
	if (edit->proxy == NULL) {
		free(edit);
		errno = ENOMEM;
		return NULL;
	}

	/* Its events come to the callback. */
	error = kl_edit_v1_add_listener(edit->proxy, &edit_listener, edit);
	if (error != 0) {
		kl_edit_v1_destroy(edit->proxy);
		free(edit);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the window can say what it does. */
	return edit;
}

/*
 * Says which operations the window carries out (bit 1 <<
 * KL_EDIT_*) and its state (KL_EDIT_HAS_SELECTION ...); an
 * unchanged pair is not sent again.
 */
void
kl_edit_set_state(
	struct kl_edit *edit,
	uint32_t actions,
	uint32_t state)
{
	/* No edit object, or nothing new. */
	if (edit == NULL)
		return;
	if (edit->sent && edit->actions == actions && edit->state == state)
		return;

	/* Sent, and kept. */
	kl_edit_v1_set_state(edit->proxy, actions, state);
	edit->sent = 1;
	edit->actions = actions;
	edit->state = state;
}

/*
 * Stops taking operations: the protocol object and the record go.
 */
void
kl_edit_destroy(
	struct kl_edit *edit)
{
	/* No edit object, nothing to destroy. */
	if (edit == NULL)
		return;

	/* The protocol object, then the record. */
	kl_edit_v1_destroy(edit->proxy);
	free(edit);
}

/* Passes an operation to the application's callback. */
static void
edit_event(
	void *data,
	struct kl_edit_v1 *object,
	uint32_t action)
{
	struct kl_edit *edit;

	/* The record the listener was given. */
	(void)object;
	edit = data;

	/* The callback. */
	edit->callback(edit->data, action);
}

/* Binds zdesktop's edit manager: from an application's registry, or found by a search of the library's own. */
static struct kl_edit_manager_v1 *
edit_bind(
	struct wl_display *display)
{
	struct kl_edit_manager_v1 *manager;
	struct keiui_global_search search;
	int error;

	/* The manager's global. */
	error = keiui_global_find(&search, display, "kl_edit_manager_v1");
	if (error != 0) {
		keiui_global_end(&search);
		errno = error;
		return NULL;
	}

	/* The manager, bound when announced; the search ends. */
	manager = keiui_global_bind(&search, &kl_edit_manager_v1_interface, EDIT_VERSION);
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
