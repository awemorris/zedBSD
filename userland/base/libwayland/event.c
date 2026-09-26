/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dispatches the selected typed listeners without a foreign-function dependency.
 */

#include "internal.h"
#include <unistd.h>

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

	pthread_mutex_unlock(&display->mutex);

	/* A custom binding interprets its own event arguments directly. */
	if (dispatcher != NULL) {
		event->delivered = 1;
		error = dispatcher(implementation, proxy, event->opcode, event->message, arguments);
		if (error != 0)
			return EPROTO;

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

	/* Unknown typed listeners require an explicit generic dispatcher binding. */
	return ENOTSUP;
}
