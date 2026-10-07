/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's clipboard through the compositor's (ws090-p004, Text Editor's
 * clipboard.c moved here, itself Terminal's; its drag and drop since WS131
 * p018): kl_window_copy makes the application's text the selection (a
 * wl_data_source offering UTF-8 and plain text), and kl_window_paste
 * receives the selection's text through a pipe.  While the window's own
 * text is the selection, a paste takes it directly (asking itself to write
 * into a pipe it reads would wait on itself).  A new selection is told as
 * a KL_WINDOW_SELECTION input.
 *
 * A drag over the window is taken when it has a type the window accepts
 * (kl_window_accept_drops): file names ("text/uri-list") first, then text,
 * as a copy until the application answers otherwise
 * (kl_window_answer_drop: move, copy or ask, or nothing).  Its enter,
 * motions, leave, the compositor's choice of action and the drop are the
 * window's inputs; kl_window_receive_drop reads what was dropped through a
 * pipe, or the window's own drag's data directly (its source would be
 * asked to send in a dispatch the reader is waiting in), and
 * kl_window_finish_drop ends it (kl_window_take_drop does both, as a
 * copy).  kl_window_start_drag starts a drag out of the window of the data
 * of a few types (kl_window_drag_text: of text), whose end is an input.
 *
 * Without a data device manager (another compositor) the clipboard is the
 * window's own.
 */

#include "window.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The text types the window offers and takes. */
#define CLIPBOARD_TYPE_UTF8	"text/plain;charset=utf-8"
#define CLIPBOARD_TYPE_PLAIN	"text/plain"
#define CLIPBOARD_TYPE_URIS	"text/uri-list"

/* The most a drop's data may be, in bytes (WS131 p020: Files' file names). */
#define CLIPBOARD_DROP_MAX	(1024U * 1024U)

/* The data device manager version the window uses, and how long a paste waits for the text. */
#define CLIPBOARD_VERSION	3U
#define CLIPBOARD_RECEIVE_MS	2000U

static void clipboard_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void clipboard_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer);
static void clipboard_leave(void *data, struct wl_data_device *device);
static void clipboard_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void clipboard_drop(void *data, struct wl_data_device *device);
static void clipboard_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void clipboard_type(void *data, struct wl_data_offer *offer, const char *mime_type);
static void clipboard_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions);
static void clipboard_action(void *data, struct wl_data_offer *offer, uint32_t action);
static void clipboard_target(void *data, struct wl_data_source *source, const char *mime_type);
static void clipboard_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd);
static void clipboard_cancelled(void *data, struct wl_data_source *source);
static void clipboard_dropped(void *data, struct wl_data_source *source);
static void clipboard_finished(void *data, struct wl_data_source *source);
static void clipboard_source_action(void *data, struct wl_data_source *source, uint32_t action);
static size_t clipboard_read(struct kl_window *window, struct wl_data_offer *offer, const char *type, char *text, size_t size);
static void clipboard_drop_input(struct kl_window *window, unsigned kind, unsigned code, double x, double y);
static const char *clipboard_drop_type(const struct kl_window *window, unsigned *type);
static int clipboard_own_data(const struct kl_window *window, const char *type, const char **data, size_t *length);
static void clipboard_drag_free(struct kl_window *window);
static void clipboard_drag_end(struct kl_window *window, unsigned dropped);

/* The data device's events. */
static const struct wl_data_device_listener device_listener = {
	clipboard_offer,
	clipboard_enter,
	clipboard_leave,
	clipboard_motion,
	clipboard_drop,
	clipboard_selection
};

/* Every offer's events. */
static const struct wl_data_offer_listener offer_listener = {
	clipboard_type,
	clipboard_source_actions,
	clipboard_action
};

/* The window's source's events. */
static const struct wl_data_source_listener source_listener = {
	clipboard_target,
	clipboard_send,
	clipboard_cancelled,
	clipboard_dropped,
	clipboard_finished,
	clipboard_source_action
};

/*
 * Binds the compositor's data device manager (from the registry).
 */
void
keiui_clipboard_bind(
	struct kl_window *window,
	struct wl_registry *registry,
	uint32_t name,
	uint32_t version)
{
	/* Version 3 is enough. */
	if (version > CLIPBOARD_VERSION)
		version = CLIPBOARD_VERSION;
	window->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version);
}

/*
 * Gets the seat's data device, once the globals are bound (without a
 * manager or a seat the clipboard stays the window's own).
 */
void
keiui_clipboard_start(
	struct kl_window *window)
{
	/* Nothing to share through. */
	if (window->data_manager == NULL || window->seat == NULL)
		return;

	/* The device, and its events. */
	window->data_device = wl_data_device_manager_get_data_device(window->data_manager, window->seat);
	if (window->data_device != NULL)
		(void)wl_data_device_add_listener(window->data_device, &device_listener, window);
}

/*
 * Makes a text the selection (Copy, Cut).  The window keeps its own copy,
 * sent from there.
 */
void
kl_window_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	char *copy;

	/* The window's copy of the text; without memory the old one stays. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return;
	memcpy(copy, text, length);
	free(window->clipboard);
	window->clipboard = copy;
	window->clipboard_length = length;
	if (window->data_device == NULL)
		return;

	/* The last source goes. */
	if (window->data_source != NULL)
		wl_data_source_destroy(window->data_source);

	/* A source with the two text types, as the selection. */
	window->data_source = wl_data_device_manager_create_data_source(window->data_manager);
	if (window->data_source == NULL)
		return;
	(void)wl_data_source_add_listener(window->data_source, &source_listener, window);
	wl_data_source_offer(window->data_source, CLIPBOARD_TYPE_UTF8);
	wl_data_source_offer(window->data_source, CLIPBOARD_TYPE_PLAIN);
	wl_data_device_set_selection(window->data_device, window->data_source, window->serial);
	(void)wl_display_flush(window->display);
}

/*
 * Receives the selection's text into a buffer (Paste): the window's own
 * directly, another client's through a pipe read until its end or
 * CLIPBOARD_RECEIVE_MS.  Returns the bytes (0 for none).
 */
size_t
kl_window_paste(
	struct kl_window *window,
	char *text,
	size_t size)
{
	size_t length;

	/* The window's own text (or the only one, without the compositor's clipboard). */
	if (window->data_device == NULL || window->data_source != NULL) {
		length = window->clipboard_length;
		if (length > size)
			length = size;
		if (length != 0U)
			memcpy(text, window->clipboard, length);
		return length;
	}

	/* No text to receive. */
	if (window->data_offer == NULL || !window->offer_text)
		return 0;

	/* The text, through a pipe. */
	length = clipboard_read(window, window->data_offer, CLIPBOARD_TYPE_UTF8, text, size);

	/* Succeeded: the bytes received. */
	return length;
}

/*
 * Tells whether the selection offers text to paste (the window's own
 * text, or another program's).
 */
int
kl_window_can_paste(
	const struct kl_window *window)
{
	/* The window's own text. */
	if (window->data_source != NULL && window->clipboard_length != 0U)
		return 1;

	/* Another program's offer of text. */
	if (window->data_offer != NULL && window->offer_text)
		return 1;

	/* Nothing to paste. */
	return 0;
}

/*
 * Tells whether the window's own text is a selection (KL_SELECTION_*): a
 * paste then takes it directly.  Without the compositor's selection the
 * window's own is the only one.
 */
int
kl_window_selection_own(
	const struct kl_window *window,
	unsigned which)
{
	/* The primary selection's source. */
	if (which == KL_SELECTION_PRIMARY) {
		if (window->primary_device == NULL || window->primary_source != NULL)
			return 1;
		return 0;
	}

	/* The clipboard's source, while it is the selection (it goes when cancelled). */
	if (window->data_device == NULL || window->data_source != NULL)
		return 1;

	/* Another program's. */
	return 0;
}

/*
 * Takes the drags of some types (KL_DROP_*; 0 refuses every drag).
 * Returns 0, or EINVAL for another type.
 */
int
kl_window_accept_drops(
	struct kl_window *window,
	unsigned types)
{
	/* A window, and only the types known. */
	if (window == NULL || (types & ~(KL_DROP_TEXT | KL_DROP_URIS)) != 0U)
		return EINVAL;

	/* Taken from the next drag that comes over the window. */
	window->drop_types = types;
	return 0;
}

/*
 * Answers the compositor for the drag over the window (KL_VERSION 46): the
 * actions taken (KL_DND_*; 0 takes nothing) and the one preferred.  Until
 * the application answers, a drag it accepts is taken as a copy.
 */
void
kl_window_answer_drop(
	struct kl_window *window,
	unsigned actions,
	unsigned preferred)
{
	const char *mime;
	unsigned type;
	uint32_t version;

	/* Only a drag over the window. */
	if (window->drop_offer == NULL)
		return;

	/* Its type taken, or none. */
	mime = clipboard_drop_type(window, &type);
	if (actions == 0U)
		mime = NULL;
	wl_data_offer_accept(window->drop_offer, window->drop_serial, mime);

	/* The actions taken and the one preferred (version 3). */
	version = wl_proxy_get_version((struct wl_proxy *)window->drop_offer);
	if (version >= CLIPBOARD_VERSION)
		wl_data_offer_set_actions(window->drop_offer, actions, preferred);
	(void)wl_display_flush(window->display);
}

/*
 * Reads what was dropped on the window into memory of its own (the caller
 * frees it): the file names ("text/uri-list" as it is) or the text, and
 * tells which (KL_DROP_*).  The drop waits for kl_window_finish_drop.
 * Returns 0, ENOENT without a drop, ENOMEM, E2BIG past CLIPBOARD_DROP_MAX,
 * or ETIMEDOUT when the source wrote nothing in time.
 */
int
kl_window_receive_drop(
	struct kl_window *window,
	char **data,
	size_t *length,
	unsigned *type)
{
	struct pollfd descriptor;
	const char *mime;
	const char *own;
	size_t own_length;
	size_t capacity;
	char *grown;
	ssize_t got;
	int pipes[2];
	int status;
	int error;

	/* Nothing yet. */
	*data = NULL;
	*length = 0;
	*type = 0U;
	if (window->drop_offer == NULL)
		return ENOENT;

	/* The type it is read as. */
	mime = clipboard_drop_type(window, type);
	if (mime == NULL)
		return ENOENT;

	/*
	 * The window's own drag's data is taken as it is: reading it through
	 * the pipe would wait for the send this same dispatch has not run yet
	 * (ws035-p093).
	 */
	status = clipboard_own_data(window, mime, &own, &own_length);
	if (status) {
		*data = malloc(own_length + 1U);
		if (*data == NULL)
			return ENOMEM;
		memcpy(*data, own, own_length);
		(*data)[own_length] = '\0';
		*length = own_length;
		return 0;
	}

	/* The pipe the source writes into; the window closes its write end. */
	status = pipe(pipes);
	if (status != 0)
		return errno;
	wl_data_offer_receive(window->drop_offer, mime, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(window->display);

	/* Everything the source writes, up to its end, the limit or the time allowed. */
	capacity = 0;
	error = 0;
	for (;;) {
		/* Waits for more. */
		descriptor.fd = pipes[0];
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, (int)CLIPBOARD_RECEIVE_MS);
		if (status <= 0) {
			error = ETIMEDOUT;
			break;
		}

		/* Room for more, and a NUL after. */
		if (*length + 4096U + 1U > capacity) {
			capacity = *length + 4096U + 1U;
			grown = realloc(*data, capacity);
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}
			*data = grown;
		}

		/* The bytes; the end ends the reading. */
		got = read(pipes[0], *data + *length, 4096U);
		if (got <= 0)
			break;
		*length += (size_t)got;
		if (*length > CLIPBOARD_DROP_MAX) {
			error = E2BIG;
			break;
		}
	}
	close(pipes[0]);

	/* A failure keeps nothing. */
	if (error != 0) {
		free(*data);
		*data = NULL;
		*length = 0;
		return error;
	}

	/* Succeeded: the data, ended by a NUL. */
	if (*data != NULL)
		(*data)[*length] = '\0';
	return 0;
}

/*
 * Ends the drop on the window: finished with the action carried out
 * (KL_DND_*), or given up with 0 (the source's drag is then cancelled).
 */
void
kl_window_finish_drop(
	struct kl_window *window,
	unsigned action)
{
	uint32_t version;

	/* Only a drop. */
	window->drop_pending = 0;
	if (window->drop_offer == NULL)
		return;

	/* The action carried out, then finished (version 3). */
	version = wl_proxy_get_version((struct wl_proxy *)window->drop_offer);
	if (action != 0U && version >= CLIPBOARD_VERSION) {
		wl_data_offer_set_actions(window->drop_offer, action, action);
		wl_data_offer_finish(window->drop_offer);
	}

	/* The offer goes either way. */
	wl_data_offer_destroy(window->drop_offer);
	window->drop_offer = NULL;
	(void)wl_display_flush(window->display);
}

/*
 * Reads what was dropped on the window into a buffer and finishes the drop
 * as a copy: the file names ("text/uri-list" as it is) or the text, and
 * tells which (KL_DROP_*, 0 with nothing).  Returns the bytes.
 */
size_t
kl_window_take_drop(
	struct kl_window *window,
	char *text,
	size_t size,
	unsigned *type)
{
	char *data;
	size_t length;
	int error;

	/* Only a drop waiting, taken once. */
	*type = 0U;
	if (!window->drop_pending)
		return 0;

	/* What was dropped, as much as the buffer holds. */
	length = 0;
	error = kl_window_receive_drop(window, &data, &length, type);
	if (error == 0) {
		if (length > size)
			length = size;
		if (length != 0U)
			memcpy(text, data, length);
		free(data);
	}

	/* The drop is finished as a copy. */
	kl_window_finish_drop(window, KL_DND_COPY);

	/* Succeeded: the bytes. */
	return length;
}

/*
 * Starts a drag out of the window from the press with the serial
 * (KL_VERSION 46): a source offering each type's data (copied), with the
 * actions it allows (KL_DND_*).  Its end comes as KL_WINDOW_DRAG_DONE.
 * Returns 0, EINVAL, ENOTSUP without the compositor's drag and drop, EBUSY
 * while a drag goes on, or ENOMEM.
 */
int
kl_window_start_drag(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial)
{
	uint32_t version;
	size_t index;

	/* Some types, as many as are kept. */
	if (window == NULL ||
	    data == NULL ||
	    count == 0U ||
	    count > KEIUI_DRAG_TYPES)
		return EINVAL;

	/* Only with the compositor's drag and drop, and one drag at a time. */
	if (window->data_device == NULL)
		return ENOTSUP;
	if (window->drag_source != NULL)
		return EBUSY;

	/* A copy of each type and its data. */
	for (index = 0; index < count; index++) {
		window->drag_types[index] = strdup(data[index].type);
		window->drag_data[index] = malloc(data[index].length + 1U);
		window->drag_count = (unsigned)index + 1U;
		if (window->drag_types[index] == NULL || window->drag_data[index] == NULL) {
			clipboard_drag_free(window);
			return ENOMEM;
		}
		memcpy(window->drag_data[index], data[index].data, data[index].length);
		window->drag_lengths[index] = data[index].length;
	}

	/* The source, offering the types. */
	window->drag_source = wl_data_device_manager_create_data_source(window->data_manager);
	if (window->drag_source == NULL) {
		clipboard_drag_free(window);
		return ENOMEM;
	}
	(void)wl_data_source_add_listener(window->drag_source, &source_listener, window);
	for (index = 0; index < count; index++)
		wl_data_source_offer(window->drag_source, data[index].type);

	/* Its actions (version 3). */
	window->drag_action = 0U;
	version = wl_proxy_get_version((struct wl_proxy *)window->drag_source);
	if (version >= CLIPBOARD_VERSION)
		wl_data_source_set_actions(window->drag_source, actions);

	/* Succeeded: the drag starts from the window. */
	wl_data_device_start_drag(window->data_device, window->drag_source, window->surface, NULL, serial);
	(void)wl_display_flush(window->display);
	return 0;
}

/*
 * Starts a drag of text out of the window, from the press with the serial:
 * the two text types, as a copy.  Returns as kl_window_start_drag does.
 */
int
kl_window_drag_text(
	struct kl_window *window,
	const char *text,
	size_t length,
	uint32_t serial)
{
	struct kl_drag_data data[2];
	int error;

	/* UTF-8 and plain text, the same bytes. */
	data[0].type = CLIPBOARD_TYPE_UTF8;
	data[0].data = text;
	data[0].length = length;
	data[1].type = CLIPBOARD_TYPE_PLAIN;
	data[1].data = text;
	data[1].length = length;

	/* The drag, which copies. */
	error = kl_window_start_drag(window, data, 2U, KL_DND_COPY, serial);
	return error;
}

/*
 * Destroys the clipboard's objects (before the seat they belong to).
 */
void
keiui_clipboard_close(
	struct kl_window *window)
{
	/* The offers, the sources, the device and the manager. */
	if (window->drop_offer != NULL)
		wl_data_offer_destroy(window->drop_offer);
	if (window->drag_source != NULL)
		wl_data_source_destroy(window->drag_source);
	window->drop_offer = NULL;
	window->drag_source = NULL;
	clipboard_drag_free(window);
	if (window->data_offer != NULL)
		wl_data_offer_destroy(window->data_offer);
	if (window->data_source != NULL)
		wl_data_source_destroy(window->data_source);
	if (window->data_device != NULL)
		wl_data_device_release(window->data_device);
	if (window->data_manager != NULL)
		wl_data_device_manager_destroy(window->data_manager);
	window->data_offer = NULL;
	window->data_source = NULL;
	window->data_device = NULL;
	window->data_manager = NULL;
}

/* Takes a new offer: its types are heard next. */
static void
clipboard_offer(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct kl_window *window;

	/* The offer being described, with no type yet. */
	(void)device;
	window = data;
	window->pending_text = 0;
	window->pending_uris = 0;
	(void)wl_data_offer_add_listener(offer, &offer_listener, window);
}

/*
 * A drag comes over the window: taken as a copy when it has a type the
 * window accepts (file names first, then text), refused otherwise; the
 * window hears it came (whether it is its own drag in pressed).
 */
static void
clipboard_enter(
	void *data,
	struct wl_data_device *device,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y,
	struct wl_data_offer *offer)
{
	struct kl_window_event *event;
	struct kl_window *window;
	unsigned offered;

	/* An offer left from an earlier drag goes. */
	(void)device;
	window = data;
	if (window->drop_offer != NULL && window->drop_offer != offer)
		wl_data_offer_destroy(window->drop_offer);
	window->drop_offer = NULL;
	window->drop_pending = 0;
	if (offer == NULL)
		return;

	/* A drag over another surface of the program, or of nothing the window takes, is refused. */
	offered = 0U;
	if (window->pending_uris)
		offered |= KL_DROP_URIS;
	if (window->pending_text)
		offered |= KL_DROP_TEXT;
	offered &= window->drop_types;
	if (surface != window->surface || offered == 0U) {
		wl_data_offer_accept(offer, serial, NULL);
		wl_data_offer_destroy(offer);
		return;
	}

	/* The drag's offer and place, taken as a copy until the application answers. */
	window->drop_offer = offer;
	window->drop_serial = serial;
	window->drop_offered = offered;
	window->drop_x = wl_fixed_to_double(x);
	window->drop_y = wl_fixed_to_double(y);
	kl_window_answer_drop(window, KL_DND_COPY, KL_DND_COPY);

	/* Succeeded: the window hears it came, and whether it is its own. */
	event = keiui_window_push(window, KL_WINDOW_DROP_ENTER);
	if (event == NULL)
		return;
	event->code = offered;
	event->x = window->drop_x;
	event->y = window->drop_y;
	if (window->drag_source != NULL)
		event->pressed = 1;
}

/* The drag left the window: its offer goes (not one that was dropped and waits to be taken). */
static void
clipboard_leave(
	void *data,
	struct wl_data_device *device)
{
	struct kl_window *window;

	/* Nothing over the window, or a drop waiting. */
	(void)device;
	window = data;
	if (window->drop_offer == NULL || window->drop_pending)
		return;

	/* The offer, and the window hears the drag went. */
	wl_data_offer_destroy(window->drop_offer);
	window->drop_offer = NULL;
	clipboard_drop_input(window, KL_WINDOW_DROP_LEAVE, 0U, window->drop_x, window->drop_y);
}

/* A drag moves over the window: its place is kept for the drop, and the window hears it. */
static void
clipboard_motion(
	void *data,
	struct wl_data_device *device,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct kl_window *window;

	/* The place, while the window takes the drag. */
	(void)device;
	(void)time;
	window = data;
	if (window->drop_offer == NULL)
		return;
	window->drop_x = wl_fixed_to_double(x);
	window->drop_y = wl_fixed_to_double(y);
	clipboard_drop_input(window, KL_WINDOW_DROP_MOTION, window->drop_offered, window->drop_x, window->drop_y);
}

/* The drag was dropped on the window: it waits for the application (a context menu now opens for it). */
static void
clipboard_drop(
	void *data,
	struct wl_data_device *device)
{
	struct kl_window *window;
	unsigned type;

	/* Only a drag the window took. */
	(void)device;
	window = data;
	if (window->drop_offer == NULL)
		return;

	/* The type it is read as. */
	(void)clipboard_drop_type(window, &type);

	/* Succeeded: it waits, the drop's serial stands for the press, and the window hears it. */
	window->drop_pending = 1;
	window->press_serial = window->drop_serial;
	clipboard_drop_input(window, KL_WINDOW_DROP, type, window->drop_x, window->drop_y);
}

/* Takes the selection: the offer a paste receives from (the last one goes); the window hears it changed. */
static void
clipboard_selection(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct kl_window_event *event;
	struct kl_window *window;

	/* The last offer goes. */
	(void)device;
	window = data;
	if (window->data_offer != NULL && window->data_offer != offer)
		wl_data_offer_destroy(window->data_offer);

	/* The new one, with or without text. */
	window->data_offer = offer;
	window->offer_text = 0;
	if (offer != NULL)
		window->offer_text = window->pending_text;

	/* The window's input. */
	event = keiui_window_push(window, KL_WINDOW_SELECTION);
	if (event != NULL) {
		event->code = KL_SELECTION_CLIPBOARD;
		event->pressed = window->offer_text;
	}
}

/* Notes a type of the offer being described: either text type, or file names. */
static void
clipboard_type(
	void *data,
	struct wl_data_offer *offer,
	const char *mime_type)
{
	struct kl_window *window;
	int utf8;
	int plain;
	int uris;

	/* The UTF-8 text type, or plain text. */
	(void)offer;
	window = data;
	utf8 = strcmp(mime_type, CLIPBOARD_TYPE_UTF8);
	plain = strcmp(mime_type, CLIPBOARD_TYPE_PLAIN);
	if (utf8 == 0 || plain == 0)
		window->pending_text = 1;

	/* File names. */
	uris = strcmp(mime_type, CLIPBOARD_TYPE_URIS);
	if (uris == 0)
		window->pending_uris = 1;
}

/* The actions the drag's source offers are not needed (the compositor chooses). */
static void
clipboard_source_actions(
	void *data,
	struct wl_data_offer *offer,
	uint32_t actions)
{
	/* Nothing to do. */
	(void)data;
	(void)offer;
	(void)actions;
}

/* The compositor's choice of action for the drag over the window: the window hears it. */
static void
clipboard_action(
	void *data,
	struct wl_data_offer *offer,
	uint32_t action)
{
	struct kl_window *window;

	/* Only the drag over the window. */
	window = data;
	if (offer != window->drop_offer)
		return;
	clipboard_drop_input(window, KL_WINDOW_DROP_ACTION, action, window->drop_x, window->drop_y);
}

/* A drop target took a type of the window's source: nothing to do. */
static void
clipboard_target(
	void *data,
	struct wl_data_source *source,
	const char *mime_type)
{
	/* Nothing to do. */
	(void)data;
	(void)source;
	(void)mime_type;
}

/* Writes the window's data of a type (the drag's, or the copied text) into the descriptor another client reads, and closes it. */
static void
clipboard_send(
	void *data,
	struct wl_data_source *source,
	const char *mime_type,
	int32_t fd)
{
	struct kl_window *window;
	const char *text;
	size_t length;
	size_t written;
	ssize_t count;
	int own;

	/* The drag's data of the type (nothing of another), or the copied text whatever the text type. */
	window = data;
	text = window->clipboard;
	length = window->clipboard_length;
	if (source == window->drag_source) {
		own = clipboard_own_data(window, mime_type, &text, &length);
		if (!own) {
			text = NULL;
			length = 0;
		}
	}

	/* All of it, written as the reader takes it. */
	written = 0;
	while (written < length) {
		count = write(fd, text + written, length - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		written += (size_t)count;
	}

	/* The end of the data. */
	close(fd);
}

/* A source is not the selection any more, or a drag ended without a drop: the source goes. */
static void
clipboard_cancelled(
	void *data,
	struct wl_data_source *source)
{
	struct kl_window *window;

	/* A drag ends not dropped. */
	window = data;
	if (window->drag_source == source) {
		clipboard_drag_end(window, 0U);
		return;
	}

	/* The selection's source is destroyed; a paste now takes the other client's selection. */
	wl_data_source_destroy(source);
	if (window->data_source == source)
		window->data_source = NULL;
}

/* A drop of the window's drag: its end comes with finished. */
static void
clipboard_dropped(
	void *data,
	struct wl_data_source *source)
{
	/* Nothing to do. */
	(void)data;
	(void)source;
}

/* The window's drag ended dropped. */
static void
clipboard_finished(
	void *data,
	struct wl_data_source *source)
{
	struct kl_window *window;

	/* Only the drag's source. */
	window = data;
	if (window->drag_source != source)
		return;
	clipboard_drag_end(window, 1U);
}

/* The compositor's choice of action for the window's drag, told with its end. */
static void
clipboard_source_action(
	void *data,
	struct wl_data_source *source,
	uint32_t action)
{
	struct kl_window *window;

	/* Only the drag's source. */
	window = data;
	if (window->drag_source != source)
		return;
	window->drag_action = action;
}

/*
 * Reads a type of an offer through a pipe until its end or
 * CLIPBOARD_RECEIVE_MS.  Returns the bytes read (0 for none).
 */
static size_t
clipboard_read(
	struct kl_window *window,
	struct wl_data_offer *offer,
	const char *type,
	char *text,
	size_t size)
{
	struct pollfd descriptor;
	uint64_t deadline;
	uint64_t now;
	size_t length;
	ssize_t got;
	int pipes[2];
	int error;
	int ready;

	/* The pipe; its writing end goes to the offer's client. */
	error = pipe(pipes);
	if (error != 0)
		return 0;
	wl_data_offer_receive(offer, type, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(window->display);

	/* The data, until the writer closes (or the time is up). */
	length = 0;
	deadline = keiui_clock_ms() + CLIPBOARD_RECEIVE_MS;
	while (length < size) {
		/* The time is up. */
		now = keiui_clock_ms();
		if (now >= deadline)
			break;

		/* The pipe becomes readable. */
		descriptor.fd = pipes[0];
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1, 100);
		if (ready <= 0)
			continue;

		/* What came; nothing more is the end. */
		got = read(pipes[0], text + length, size - length);
		if (got <= 0)
			break;
		length += (size_t)got;
	}

	/* The reading end goes. */
	close(pipes[0]);

	/* Succeeded: the bytes read. */
	return length;
}

/* Queues a drag's input of the window at a place (surface pixels). */
static void
clipboard_drop_input(
	struct kl_window *window,
	unsigned kind,
	unsigned code,
	double x,
	double y)
{
	struct kl_window_event *event;

	/* The input; a full queue drops it. */
	event = keiui_window_push(window, kind);
	if (event == NULL)
		return;
	event->code = code;
	event->x = x;
	event->y = y;
}

/* The type the drag over the window is read as (file names first), and which (KL_DROP_*); NULL for none. */
static const char *
clipboard_drop_type(
	const struct kl_window *window,
	unsigned *type)
{
	/* File names. */
	if ((window->drop_offered & KL_DROP_URIS) != 0U) {
		*type = KL_DROP_URIS;
		return CLIPBOARD_TYPE_URIS;
	}

	/* Text. */
	if ((window->drop_offered & KL_DROP_TEXT) != 0U) {
		*type = KL_DROP_TEXT;
		return CLIPBOARD_TYPE_UTF8;
	}

	/* Nothing the window takes. */
	*type = 0U;
	return NULL;
}

/* Finds the window's own drag's data of a type (1), or tells there is none (0). */
static int
clipboard_own_data(
	const struct kl_window *window,
	const char *type,
	const char **data,
	size_t *length)
{
	unsigned index;
	int same;

	/* No drag of the window's own. */
	if (window->drag_source == NULL)
		return 0;

	/* The type among the drag's. */
	for (index = 0; index < window->drag_count; index++) {
		same = strcmp(window->drag_types[index], type);
		if (same != 0)
			continue;
		*data = window->drag_data[index];
		*length = window->drag_lengths[index];
		return 1;
	}

	/* Not one of the drag's. */
	return 0;
}

/* Frees the window's own drag's types and data. */
static void
clipboard_drag_free(
	struct kl_window *window)
{
	unsigned index;

	/* Each type's copies. */
	for (index = 0; index < window->drag_count; index++) {
		free(window->drag_types[index]);
		free(window->drag_data[index]);
		window->drag_types[index] = NULL;
		window->drag_data[index] = NULL;
		window->drag_lengths[index] = 0;
	}
	window->drag_count = 0;
}

/* Ends the window's own drag: the source and its data go, and the window hears whether it was dropped and with which action. */
static void
clipboard_drag_end(
	struct kl_window *window,
	unsigned dropped)
{
	struct kl_window_event *event;

	/* The source and its data. */
	wl_data_source_destroy(window->drag_source);
	window->drag_source = NULL;
	clipboard_drag_free(window);

	/* The window's input. */
	event = keiui_window_push(window, KL_WINDOW_DRAG_DONE);
	if (event == NULL)
		return;
	event->code = dropped;
	event->begin = (int32_t)window->drag_action;
}
