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
 * ws189-p002: a picture ("image/png", KL_DROP_IMAGE) is taken after file
 * names and before text; the application answers for each place the drag
 * is over (a repeated answer is not sent again); a drag out of the window
 * may carry a picture under the pointer (kl_window_start_drag_icon), a
 * surface of no role placed by its attach offset; and a source's writes
 * to a reader that went away end with EPIPE, not the program.
 *
 * Without a data device manager (another compositor) the clipboard is the
 * window's own.
 */

#include "window.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The text types the window offers and takes. */
#define CLIPBOARD_TYPE_UTF8	"text/plain;charset=utf-8"
#define CLIPBOARD_TYPE_PLAIN	"text/plain"
#define CLIPBOARD_TYPE_URIS	"text/uri-list"
#define CLIPBOARD_TYPE_PNG	"image/png"

/* The most a drop's data may be, in bytes, by its type (WS131 p020: Files' file names; ws189: a picture, text). */
#define CLIPBOARD_URIS_MAX	((size_t)1024U * 1024U)
#define CLIPBOARD_IMAGE_MAX	((size_t)64U * 1024U * 1024U)
#define CLIPBOARD_TEXT_MAX	((size_t)16U * 1024U * 1024U)

/* The first room a drop's data is read into, in bytes (doubled as it grows). */
#define CLIPBOARD_DROP_CHUNK	((size_t)16384U)

/* What the drag's picture is shown at: its whole alpha in 255ths, its corner radius and its edge's alpha. */
#define CLIPBOARD_ICON_ALPHA	217U
#define CLIPBOARD_ICON_RADIUS	6
#define CLIPBOARD_ICON_EDGE	64U

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
static size_t clipboard_drop_max(unsigned type);
static void clipboard_icon_make(struct kl_window *window, const struct kl_drag_icon *icon, int *hot_x, int *hot_y);
static void clipboard_icon_draw(uint32_t *out, int width, int height, const struct kl_drag_icon *icon);
static uint32_t clipboard_icon_edge(uint32_t pixel);
static void clipboard_icon_free(struct kl_window *window);

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
	if (window == NULL || (types & ~(KL_DROP_TEXT | KL_DROP_URIS | KL_DROP_IMAGE)) != 0U)
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

	/* The same answer as the last one is not sent again (an application answers at each motion). */
	if (window->drop_answered &&
	    window->drop_answer_actions == actions &&
	    window->drop_answer_preferred == preferred)
		return;
	window->drop_answered = 1;
	window->drop_answer_actions = actions;
	window->drop_answer_preferred = preferred;

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
 * frees it): the file names ("text/uri-list" as it is), the picture (a
 * PNG) or the text, and tells which (KL_DROP_*).  The drop waits for
 * kl_window_finish_drop.  Returns 0, ENOENT without a drop, ENOMEM, E2BIG
 * past the type's limit (clipboard_drop_max), or ETIMEDOUT when the
 * source wrote nothing in time.
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
	size_t limit;
	size_t room;
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

	/* The type it is read as, and the most of it that is read. */
	mime = clipboard_drop_type(window, type);
	if (mime == NULL)
		return ENOENT;
	limit = clipboard_drop_max(*type);

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

	/* Everything the source writes, up to its end, the limit or the time allowed (the room doubles as it fills). */
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

		/* Room for more, and a NUL after: the first chunk, then twice what there was. */
		if (*length + 1U >= capacity) {
			capacity = capacity * 2U;
			if (capacity == 0U)
				capacity = CLIPBOARD_DROP_CHUNK;

			/* Never more than one byte past the limit (which tells it was passed) and the NUL. */
			if (capacity > limit + 2U)
				capacity = limit + 2U;
			grown = realloc(*data, capacity);
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}

			/* The data lives in the grown room now. */
			*data = grown;
		}

		/* The bytes, as many as there is room for; the end ends the reading. */
		room = capacity - *length - 1U;
		got = read(pipes[0], *data + *length, room);
		if (got <= 0)
			break;
		*length += (size_t)got;
		if (*length > limit) {
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
	int error;

	/* The same drag without a picture. */
	error = kl_window_start_drag_icon(window, data, count, actions, serial, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the drag starts from the window. */
	return 0;
}

/*
 * Starts a drag out of the window as kl_window_start_drag does, carrying a
 * picture under the pointer (KL_VERSION 70; NULL for none): shrunk to
 * KL_DRAG_ICON_MAX on its longer side, a little see-through, with its hot
 * point under the pointer.  A picture that cannot be made (no shared
 * memory) leaves the drag without it.  Returns as kl_window_start_drag does.
 */
int
kl_window_start_drag_icon(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial,
	const struct kl_drag_icon *icon)
{
	uint32_t version;
	size_t index;
	int hot_x;
	int hot_y;

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

		/* The data copied, kept until the drag ends. */
		memcpy(window->drag_data[index], data[index].data, data[index].length);
		window->drag_lengths[index] = data[index].length;
	}

	/* The source, offering the types. */
	window->drag_source = wl_data_device_manager_create_data_source(window->data_manager);
	if (window->drag_source == NULL) {
		clipboard_drag_free(window);
		return ENOMEM;
	}

	/* Its events, and each type offered. */
	(void)wl_data_source_add_listener(window->drag_source, &source_listener, window);
	for (index = 0; index < count; index++)
		wl_data_source_offer(window->drag_source, data[index].type);

	/* Its actions (version 3). */
	window->drag_action = 0U;
	version = wl_proxy_get_version((struct wl_proxy *)window->drag_source);
	if (version >= CLIPBOARD_VERSION)
		wl_data_source_set_actions(window->drag_source, actions);

	/* The picture's surface, placed by its hot point, committed before the drag names it. */
	hot_x = 0;
	hot_y = 0;
	if (icon != NULL)
		clipboard_icon_make(window, icon, &hot_x, &hot_y);
	if (window->drag_icon != NULL) {
		wl_surface_attach(window->drag_icon, window->drag_icon_buffer.buffer, -hot_x, -hot_y);
		wl_surface_damage(window->drag_icon, 0, 0, window->drag_icon_buffer.width, window->drag_icon_buffer.height);
		wl_surface_commit(window->drag_icon);
	}

	/* Succeeded: the drag starts from the window. */
	wl_data_device_start_drag(window->data_device, window->drag_source, window->surface, window->drag_icon, serial);
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
	/* The offers, the sources, the drag's picture, the device and the manager. */
	if (window->drop_offer != NULL)
		wl_data_offer_destroy(window->drop_offer);
	if (window->drag_source != NULL)
		wl_data_source_destroy(window->drag_source);
	window->drop_offer = NULL;
	window->drag_source = NULL;
	clipboard_drag_free(window);
	clipboard_icon_free(window);
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
	window->pending_image = 0;
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
	window->drop_answered = 0;
	if (offer == NULL)
		return;

	/* A drag over another surface of the program, or of nothing the window takes, is refused. */
	offered = 0U;
	if (window->pending_uris)
		offered |= KL_DROP_URIS;
	if (window->pending_image)
		offered |= KL_DROP_IMAGE;
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

/* Notes a type of the offer being described: either text type, file names, or a picture. */
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
	int png;

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

	/* A picture. */
	png = strcmp(mime_type, CLIPBOARD_TYPE_PNG);
	if (png == 0)
		window->pending_image = 1;
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
	struct sigaction quiet;
	struct sigaction before;
	struct kl_window *window;
	const char *text;
	size_t length;
	size_t written;
	ssize_t count;
	int ignored;
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

	/* A reader that goes away ends the writing with EPIPE, not the program (ws189-p002). */
	memset(&quiet, 0, sizeof(quiet));
	quiet.sa_handler = SIG_IGN;
	(void)sigemptyset(&quiet.sa_mask);
	ignored = sigaction(SIGPIPE, &quiet, &before);

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

	/* SIGPIPE is handled as before again. */
	if (ignored == 0)
		(void)sigaction(SIGPIPE, &before, NULL);

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

/* The type the drag over the window is read as (file names first, then a picture, then text), and which (KL_DROP_*); NULL for none. */
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

	/* A picture. */
	if ((window->drop_offered & KL_DROP_IMAGE) != 0U) {
		*type = KL_DROP_IMAGE;
		return CLIPBOARD_TYPE_PNG;
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

	/* The source, its data and its picture. */
	wl_data_source_destroy(window->drag_source);
	window->drag_source = NULL;
	clipboard_drag_free(window);
	clipboard_icon_free(window);

	/* The window's input. */
	event = keiui_window_push(window, KL_WINDOW_DRAG_DONE);
	if (event == NULL)
		return;
	event->code = dropped;
	event->begin = (int32_t)window->drag_action;
}

/* The most a drop's data may be for its type (KL_DROP_*). */
static size_t
clipboard_drop_max(
	unsigned type)
{
	/* File names are short lines. */
	if (type == KL_DROP_URIS)
		return CLIPBOARD_URIS_MAX;

	/* A picture is the largest. */
	if (type == KL_DROP_IMAGE)
		return CLIPBOARD_IMAGE_MAX;

	/* Text. */
	return CLIPBOARD_TEXT_MAX;
}

/*
 * Makes the surface of the picture a drag carries: a buffer of the
 * picture shrunk to KL_DRAG_ICON_MAX on its longer side, and its hot
 * point shrunk with it.  Without shared memory or a surface the drag has
 * no picture (window->drag_icon stays NULL).
 */
static void
clipboard_icon_make(
	struct kl_window *window,
	const struct kl_drag_icon *icon,
	int *hot_x,
	int *hot_y)
{
	int longer;
	int width;
	int height;
	int error;

	/* A picture, and the means to show one. */
	if (window->compositor == NULL || window->shm == NULL)
		return;
	if (icon->pixels == NULL || icon->width <= 0 || icon->height <= 0)
		return;

	/* Its size, the longer side at most KL_DRAG_ICON_MAX. */
	width = icon->width;
	height = icon->height;
	longer = width;
	if (height > longer)
		longer = height;
	if (longer > KL_DRAG_ICON_MAX) {
		width = (int)(((long)icon->width * KL_DRAG_ICON_MAX + longer / 2) / longer);
		height = (int)(((long)icon->height * KL_DRAG_ICON_MAX + longer / 2) / longer);
	}

	/* A thin picture keeps a pixel of each side. */
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;

	/* The buffer, drawn from the picture. */
	error = keiui_shm_make(window, &window->drag_icon_buffer, width, height);
	if (error != 0)
		return;
	clipboard_icon_draw(window->drag_icon_buffer.pixels, width, height, icon);

	/* The surface it is shown on. */
	window->drag_icon = wl_compositor_create_surface(window->compositor);
	if (window->drag_icon == NULL) {
		keiui_shm_free(&window->drag_icon_buffer);
		return;
	}

	/* The hot point at the same share of the shrunk picture, inside it. */
	*hot_x = (int)(((long)icon->hot_x * width) / icon->width);
	*hot_y = (int)(((long)icon->hot_y * height) / icon->height);
	if (*hot_x < 0)
		*hot_x = 0;
	if (*hot_x >= width)
		*hot_x = width - 1;
	if (*hot_y < 0)
		*hot_y = 0;
	if (*hot_y >= height)
		*hot_y = height - 1;
}

/*
 * Draws the drag's picture into its buffer: each pixel the average of the
 * picture's pixels it covers (premultiplied, so averaging keeps the
 * colours right), the whole at CLIPBOARD_ICON_ALPHA, a faint dark edge,
 * and the corners rounded off.
 */
static void
clipboard_icon_draw(
	uint32_t *out,
	int width,
	int height,
	const struct kl_drag_icon *icon)
{
	unsigned long sums[4];
	unsigned long count;
	uint32_t pixel;
	unsigned channel;
	unsigned value;
	int source_x0;
	int source_x1;
	int source_y0;
	int source_y1;
	int corner_x;
	int corner_y;
	int x;
	int y;
	int sx;
	int sy;

	/* Each pixel of the buffer, row by row. */
	for (y = 0; y < height; y++) {
		/* The rows of the picture this row covers (at least one). */
		source_y0 = (int)(((long)y * icon->height) / height);
		source_y1 = (int)(((long)(y + 1) * icon->height) / height);
		if (source_y1 <= source_y0)
			source_y1 = source_y0 + 1;

		/* Each pixel of the row. */
		for (x = 0; x < width; x++) {
			/* The columns it covers (at least one). */
			source_x0 = (int)(((long)x * icon->width) / width);
			source_x1 = (int)(((long)(x + 1) * icon->width) / width);
			if (source_x1 <= source_x0)
				source_x1 = source_x0 + 1;

			/* The average of the covered pixels, channel by channel. */
			memset(sums, 0, sizeof(sums));
			count = 0;
			for (sy = source_y0; sy < source_y1; sy++) {
				/* Each covered pixel of the picture's row. */
				for (sx = source_x0; sx < source_x1; sx++) {
					pixel = icon->pixels[(size_t)sy * (size_t)icon->width + (size_t)sx];
					sums[0] += (pixel >> 24) & 0xffU;
					sums[1] += (pixel >> 16) & 0xffU;
					sums[2] += (pixel >> 8) & 0xffU;
					sums[3] += pixel & 0xffU;
					count++;
				}
			}

			/* Each channel averaged, then made a little see-through (premultiplied: every channel scales). */
			pixel = 0;
			for (channel = 0; channel < 4U; channel++) {
				value = (unsigned)(sums[channel] / count);
				value = (value * CLIPBOARD_ICON_ALPHA + 127U) / 255U;
				pixel |= (uint32_t)value << (24U - channel * 8U);
			}

			/* The edge: a faint black laid over the outermost pixels. */
			if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
				pixel = clipboard_icon_edge(pixel);

			/* The distance into a corner's square, from its outer edges. */
			corner_x = CLIPBOARD_ICON_RADIUS - x;
			if (width - 1 - x < x)
				corner_x = CLIPBOARD_ICON_RADIUS - (width - 1 - x);
			corner_y = CLIPBOARD_ICON_RADIUS - y;
			if (height - 1 - y < y)
				corner_y = CLIPBOARD_ICON_RADIUS - (height - 1 - y);

			/* A pixel in a corner's square, outside its quarter circle, is clear. */
			if (corner_x > 0 &&
			    corner_y > 0 &&
			    corner_x * corner_x + corner_y * corner_y > CLIPBOARD_ICON_RADIUS * CLIPBOARD_ICON_RADIUS)
				pixel = 0;

			/* The pixel into the buffer. */
			out[(size_t)y * (size_t)width + (size_t)x] = pixel;
		}
	}
}

/* Lays the drag's picture's faint black edge over one premultiplied pixel. */
static uint32_t
clipboard_icon_edge(
	uint32_t pixel)
{
	unsigned keep;
	unsigned channel;
	unsigned value;
	uint32_t blended;

	/* What shows through the edge's black: the pixel scaled by what the edge leaves. */
	keep = 255U - CLIPBOARD_ICON_EDGE;
	blended = 0;
	for (channel = 0; channel < 4U; channel++) {
		value = (unsigned)((pixel >> (24U - channel * 8U)) & 0xffU);
		value = (value * keep + 127U) / 255U;
		blended |= (uint32_t)value << (24U - channel * 8U);
	}

	/* Succeeded: the edge's own alpha added (its colour is black). */
	return blended + ((uint32_t)CLIPBOARD_ICON_EDGE << 24);
}

/* Frees the drag's picture: its surface and its buffer. */
static void
clipboard_icon_free(
	struct kl_window *window)
{
	/* The surface first, then the buffer it showed. */
	if (window->drag_icon != NULL)
		wl_surface_destroy(window->drag_icon);
	window->drag_icon = NULL;
	keiui_shm_free(&window->drag_icon_buffer);
}
