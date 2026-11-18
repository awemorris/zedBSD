/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dispatches the selected typed listeners without a foreign-function dependency.
 *
 * A listener of an interface without a typed table here (the code
 * wayland-scanner makes for a toolkit's protocols) is called through one
 * generic call that passes every argument as one machine word (see
 * wlc_generic_callback).
 */

#include "internal.h"
#include <unistd.h>

/* The most arguments an event called through the generic path may carry. */
#define WLC_GENERIC_ARGUMENTS	20U

/*
 * A listener callback as the generic path calls it.
 *
 * Every Wayland argument (int, uint, fixed, fd, string, object, new_id,
 * array) is an integer or a pointer.  On the ABIs zedBSD builds for (amd64,
 * i386, AArch64's AAPCS64, SPARC V9) such an argument takes one register or
 * one word-sized stack slot, extended to the word, in the same place
 * whatever the argument's declared integer or pointer type.  A callback
 * declared with its own, possibly narrower, parameters of those kinds
 * therefore reads exactly its arguments from a call that passes each as one
 * word, and ignores the words after them.  This is what lets the library
 * call listeners of protocols it has no table for without libffi.
 */
typedef void (*wlc_generic_callback)(void *, void *,
	uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
	uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
	uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
	uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static int wlc_event_generic(struct wlc_event *event, const void *listener, void *data);

/*
 * Releases payload, undelivered rights and object references held by one event.
 */
void
wlc_event_destroy(
	struct wlc_event *event)
{
	const char *signature;
	size_t index;
	uint32_t version;
	char type;
	int nullable;

	/* Only initialized arguments own references in a partially decoded event. */
	signature = wlc_signature_start(event->message->signature, &version);
	for (index = 0; index < event->argument_count; index++) {
		signature = wlc_signature_next(signature, &type, &nullable);

		/* A listener takes ownership of received fds only when actually invoked. */
		if (type == 'h' && !event->delivered) {
			if (event->arguments[index].h >= 0)
				close(event->arguments[index].h);
		}

		/* A server-created object no listener received is destroyed: nobody else can. */
		if (type == 'n' && !event->delivered) {
			if (event->objects != NULL && event->objects[index] != NULL)
				wlc_proxy_destroy(event->objects[index]);
		}

		/* Object holds remain independent of any nulled listener argument. */
		if (event->objects != NULL) {
			if (event->objects[index] != NULL)
				wlc_proxy_unref(event->objects[index]);
		}
	}

	/* Event payload and decoding tables are never owned by the listener. */
	free(event->objects);
	free(event->arrays);
	free(event->arguments);
	free(event->bytes);
	wlc_proxy_unref(event->proxy);
	free(event);

	/* Succeeded: no event-owned references remain. */
	return;
}

/*
 * Invokes one selected listener outside the connection mutex.
 */
int
wlc_event_dispatch(
	struct wlc_event *event)
{
	struct wl_proxy *proxy;
	struct wl_display *display;
	const void *listener;
	wl_dispatcher_func_t dispatcher;
	const void *implementation;
	void *data;
	union wl_argument *arguments;
	size_t index;
	int same;
	int error;
	const struct wl_registry_listener *wl_registry_callbacks;
	const struct wl_callback_listener *wl_callback_callbacks;
	const struct wl_buffer_listener *wl_buffer_callbacks;
	const struct wl_shm_listener *wl_shm_callbacks;
	const struct wl_surface_listener *wl_surface_callbacks;
	const struct wl_output_listener *wl_output_callbacks;
	const struct xdg_wm_base_listener *xdg_wm_base_callbacks;
	const struct xdg_surface_listener *xdg_surface_callbacks;
	const struct xdg_toplevel_listener *xdg_toplevel_callbacks;
	const struct xdg_popup_listener *xdg_popup_callbacks;
	const struct wl_seat_listener *wl_seat_callbacks;
	const struct wl_pointer_listener *wl_pointer_callbacks;
	const struct wl_keyboard_listener *wl_keyboard_callbacks;

	/* Captures listener metadata while retaining the exact queued generation. */
	proxy = event->proxy;
	display = proxy->display;
	pthread_mutex_lock(&display->mutex);

	/* Local destruction suppresses even events received before that destruction. */
	if (proxy->destroyed) {
		pthread_mutex_unlock(&display->mutex);
		return 0;
	}

	/* What the event is delivered with, read while the display is locked. */
	listener = proxy->listener;
	dispatcher = proxy->dispatcher;
	implementation = proxy->dispatcher_data;
	data = proxy->user_data;
	arguments = event->arguments;

	/* Destroyed referenced objects are exposed as null without losing their holds. */
	for (index = 0; index < event->argument_count; index++) {
		if (event->objects[index] != NULL) {
			if (event->objects[index]->destroyed)
				arguments[index].o = NULL;
		}
	}

	/* The lock is not held while the client's code runs. */
	pthread_mutex_unlock(&display->mutex);

	/* A custom binding interprets its own event arguments directly. */
	if (dispatcher != NULL) {
		event->delivered = 1;
		error = dispatcher(implementation, proxy, event->opcode, event->message, arguments);
		if (error != 0)
			return EPROTO;

		/* Succeeded: the binding took the event. */
		return 0;
	}

	/* Unobserved events still release their payload and any received descriptors. */
	if (listener == NULL)
		return 0;

	/* Dispatches wl_registry events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_registry");
	if (same == 0) {
		wl_registry_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_registry_callbacks->global == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_registry_callbacks->global(data, (struct wl_registry *)proxy, arguments[0].u, arguments[1].s, arguments[2].u);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_registry_callbacks->global_remove == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_registry_callbacks->global_remove(data, (struct wl_registry *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_callback events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_callback");
	if (same == 0) {
		wl_callback_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_callback_callbacks->done == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_callback_callbacks->done(data, (struct wl_callback *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_shm events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_shm");
	if (same == 0) {
		wl_shm_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_shm_callbacks->format == NULL)
				return 0;

			/* Delivers the accepted format. */
			event->delivered = 1;
			wl_shm_callbacks->format(data, (struct wl_shm *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_buffer events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_buffer");
	if (same == 0) {
		wl_buffer_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_buffer_callbacks->release == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_buffer_callbacks->release(data, (struct wl_buffer *)proxy);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_surface events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_surface");
	if (same == 0) {
		wl_surface_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_surface_callbacks->enter == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_surface_callbacks->enter(data, (struct wl_surface *)proxy, (struct wl_output *)arguments[0].o);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_surface_callbacks->leave == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_surface_callbacks->leave(data, (struct wl_surface *)proxy, (struct wl_output *)arguments[0].o);
			return 0;
		case 2:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_surface_callbacks->preferred_buffer_scale == NULL)
				return 0;

			/* Delivers the scale the compositor prefers (version 6). */
			event->delivered = 1;
			wl_surface_callbacks->preferred_buffer_scale(data, (struct wl_surface *)proxy, arguments[0].i);
			return 0;
		case 3:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_surface_callbacks->preferred_buffer_transform == NULL)
				return 0;

			/* Delivers the transform the compositor prefers (version 6). */
			event->delivered = 1;
			wl_surface_callbacks->preferred_buffer_transform(data, (struct wl_surface *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_output events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_output");
	if (same == 0) {
		wl_output_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->geometry == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->geometry(data, (struct wl_output *)proxy, arguments[0].i, arguments[1].i, arguments[2].i, arguments[3].i, arguments[4].i, arguments[5].s, arguments[6].s, arguments[7].i);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->mode == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->mode(data, (struct wl_output *)proxy, arguments[0].u, arguments[1].i, arguments[2].i, arguments[3].i);
			return 0;
		case 2:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->done == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->done(data, (struct wl_output *)proxy);
			return 0;
		case 3:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->scale == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->scale(data, (struct wl_output *)proxy, arguments[0].i);
			return 0;
		case 4:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->name == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->name(data, (struct wl_output *)proxy, arguments[0].s);
			return 0;
		case 5:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_output_callbacks->description == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_output_callbacks->description(data, (struct wl_output *)proxy, arguments[0].s);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches xdg_wm_base events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "xdg_wm_base");
	if (same == 0) {
		xdg_wm_base_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_wm_base_callbacks->ping == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_wm_base_callbacks->ping(data, (struct xdg_wm_base *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches xdg_surface events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "xdg_surface");
	if (same == 0) {
		xdg_surface_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_surface_callbacks->configure == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_surface_callbacks->configure(data, (struct xdg_surface *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches xdg_toplevel events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "xdg_toplevel");
	if (same == 0) {
		xdg_toplevel_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_toplevel_callbacks->configure == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_toplevel_callbacks->configure(data, (struct xdg_toplevel *)proxy, arguments[0].i, arguments[1].i, arguments[2].a);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_toplevel_callbacks->close == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_toplevel_callbacks->close(data, (struct xdg_toplevel *)proxy);
			return 0;
		case 2:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_toplevel_callbacks->configure_bounds == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_toplevel_callbacks->configure_bounds(data, (struct xdg_toplevel *)proxy, arguments[0].i, arguments[1].i);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches xdg_popup events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "xdg_popup");
	if (same == 0) {
		xdg_popup_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_popup_callbacks->configure == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_popup_callbacks->configure(data, (struct xdg_popup *)proxy, arguments[0].i, arguments[1].i, arguments[2].i, arguments[3].i);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (xdg_popup_callbacks->popup_done == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			xdg_popup_callbacks->popup_done(data, (struct xdg_popup *)proxy);
			return 0;
		case 2:
			/* Version 3's repositioned; an optional listener slot deliberately ignores it. */
			if (xdg_popup_callbacks->repositioned == NULL)
				return 0;

			/* Delivers the token of the reposition the next configure answers. */
			event->delivered = 1;
			xdg_popup_callbacks->repositioned(data, (struct xdg_popup *)proxy, arguments[0].u);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_seat events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_seat");
	if (same == 0) {
		wl_seat_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_seat_callbacks->capabilities == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_seat_callbacks->capabilities(data, (struct wl_seat *)proxy, arguments[0].u);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_seat_callbacks->name == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_seat_callbacks->name(data, (struct wl_seat *)proxy, arguments[0].s);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_pointer events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_pointer");
	if (same == 0) {
		wl_pointer_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->enter == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->enter(data, (struct wl_pointer *)proxy, arguments[0].u, (struct wl_surface *)arguments[1].o, arguments[2].f, arguments[3].f);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->leave == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->leave(data, (struct wl_pointer *)proxy, arguments[0].u, (struct wl_surface *)arguments[1].o);
			return 0;
		case 2:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->motion == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->motion(data, (struct wl_pointer *)proxy, arguments[0].u, arguments[1].f, arguments[2].f);
			return 0;
		case 3:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->button == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->button(data, (struct wl_pointer *)proxy, arguments[0].u, arguments[1].u, arguments[2].u, arguments[3].u);
			return 0;
		case 4:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->axis == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->axis(data, (struct wl_pointer *)proxy, arguments[0].u, arguments[1].u, arguments[2].f);
			return 0;
		case 5:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->frame == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->frame(data, (struct wl_pointer *)proxy);
			return 0;
		case 6:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->axis_source == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->axis_source(data, (struct wl_pointer *)proxy, arguments[0].u);
			return 0;
		case 7:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->axis_stop == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->axis_stop(data, (struct wl_pointer *)proxy, arguments[0].u, arguments[1].u);
			return 0;
		case 8:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_pointer_callbacks->axis_discrete == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_pointer_callbacks->axis_discrete(data, (struct wl_pointer *)proxy, arguments[0].u, arguments[1].i);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches wl_keyboard events through the exact published callback types. */
	same = strcmp(proxy->interface->name, "wl_keyboard");
	if (same == 0) {
		wl_keyboard_callbacks = listener;

		/* Selects the callback using the stable protocol event opcode. */
		switch (event->opcode) {
		case 0:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->keymap == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->keymap(data, (struct wl_keyboard *)proxy, arguments[0].u, arguments[1].h, arguments[2].u);
			return 0;
		case 1:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->enter == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->enter(data, (struct wl_keyboard *)proxy, arguments[0].u, (struct wl_surface *)arguments[1].o, arguments[2].a);
			return 0;
		case 2:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->leave == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->leave(data, (struct wl_keyboard *)proxy, arguments[0].u, (struct wl_surface *)arguments[1].o);
			return 0;
		case 3:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->key == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->key(data, (struct wl_keyboard *)proxy, arguments[0].u, arguments[1].u, arguments[2].u, arguments[3].u);
			return 0;
		case 4:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->modifiers == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->modifiers(data, (struct wl_keyboard *)proxy, arguments[0].u, arguments[1].u, arguments[2].u, arguments[3].u, arguments[4].u);
			return 0;
		case 5:
			/* An optional listener slot deliberately ignores this event. */
			if (wl_keyboard_callbacks->repeat_info == NULL)
				return 0;

			/* Delivers payload ownership according to this callback contract. */
			event->delivered = 1;
			wl_keyboard_callbacks->repeat_info(data, (struct wl_keyboard *)proxy, arguments[0].i, arguments[1].i);
			return 0;
		default:
			return EPROTO;
		}
	}

	/* Dispatches xdg_toplevel_menu_v1 events (the System Menu, menu-protocol.c). */
	same = strcmp(proxy->interface->name, "xdg_toplevel_menu_v1");
	if (same == 0) {
		error = wlc_menu_dispatch(event, listener, data);
		return error;
	}

	/* Dispatches xdg_context_menu_v1 events (a context menu, menu-protocol.c). */
	same = strcmp(proxy->interface->name, "xdg_context_menu_v1");
	if (same == 0) {
		error = wlc_context_menu_dispatch(event, listener, data);
		return error;
	}

	/* Dispatches kl_titlebar_v1 events (the Titlebar Presentation, titlebar-protocol.c). */
	same = strcmp(proxy->interface->name, "kl_titlebar_v1");
	if (same == 0) {
		error = wlc_titlebar_dispatch(event, listener, data);
		return error;
	}

	/* Any other interface's listener is called through the generic path. */
	error = wlc_event_generic(event, listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener has run. */
	return 0;
}

/* Calls a listener of an interface without a typed table here, passing each argument as one word. */
static int
wlc_event_generic(
	struct wlc_event *event,
	const void *listener,
	void *data)
{
	void (*const *callbacks)(void);
	wlc_generic_callback callback;
	uintptr_t words[WLC_GENERIC_ARGUMENTS];
	union wl_argument *argument;
	const char *signature;
	uint32_t version;
	size_t index;
	char type;
	int nullable;

	/* An event with more arguments than the call passes cannot be delivered. */
	if (event->argument_count > WLC_GENERIC_ARGUMENTS)
		return EPROTO;

	/* The listener is the callbacks in event order; an empty slot ignores its event. */
	callbacks = listener;
	if (callbacks[event->opcode] == NULL)
		return 0;

	/* The callback, through a plain function pointer to the one-word-per-argument type. */
	callback = (wlc_generic_callback)(void (*)(void))callbacks[event->opcode];

	/* Each argument as one word; the signed kinds (int, fixed, fd) are extended with their sign. */
	memset(words, 0, sizeof(words));
	signature = wlc_signature_start(event->message->signature, &version);
	for (index = 0; index < event->argument_count; index++) {
		signature = wlc_signature_next(signature, &type, &nullable);
		argument = &event->arguments[index];

		/* The word for the argument's kind. */
		switch (type) {
		case 'i':
			words[index] = (uintptr_t)(intptr_t)argument->i;
			break;
		case 'f':
			words[index] = (uintptr_t)(intptr_t)argument->f;
			break;
		case 'h':
			words[index] = (uintptr_t)(intptr_t)argument->h;
			break;
		case 'u':
			words[index] = (uintptr_t)argument->u;
			break;
		case 's':
			words[index] = (uintptr_t)argument->s;
			break;
		case 'a':
			words[index] = (uintptr_t)argument->a;
			break;
		default:
			/* An object or a new object: its proxy (NULL for a null object). */
			words[index] = (uintptr_t)argument->o;
			break;
		}
	}

	/* The listener takes the payload's ownership (fds, new objects) as a typed listener does. */
	(void)nullable;
	event->delivered = 1;
	callback(data, event->proxy,
		 words[0], words[1], words[2], words[3], words[4],
		 words[5], words[6], words[7], words[8], words[9],
		 words[10], words[11], words[12], words[13], words[14],
		 words[15], words[16], words[17], words[18], words[19]);

	/* Succeeded: the callback has run. */
	return 0;
}
