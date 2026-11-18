/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's primary selection through the compositor's (ws090-p004, Text
 * Editor's primary.c moved here, itself Terminal's): kl_window_select
 * makes the text selected the primary selection (a source offering UTF-8
 * and plain text), and kl_window_paste_primary receives it (a middle
 * click).  While the window's own text is it, a paste takes it directly
 * (asking itself to write into a pipe it reads would wait on itself).
 *
 * Without the compositor's primary selection manager the primary selection
 * is the window's own.
 */

#include "window.h"

#include <primary-selection-unstable-v1-client-protocol.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The text types the window offers and takes. */
#define PRIMARY_TYPE_UTF8	"text/plain;charset=utf-8"
#define PRIMARY_TYPE_PLAIN	"text/plain"

/* How long a paste waits for the text. */
#define PRIMARY_RECEIVE_MS	2000U

static void primary_offer(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void primary_selection(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void primary_type(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime_type);
static void primary_send(void *data, struct zwp_primary_selection_source_v1 *source, const char *mime_type, int32_t fd);
static void primary_cancelled(void *data, struct zwp_primary_selection_source_v1 *source);

/* The device's events. */
static const struct zwp_primary_selection_device_v1_listener device_listener = {
	primary_offer,
	primary_selection
};

/* Every offer's events. */
static const struct zwp_primary_selection_offer_v1_listener offer_listener = {
	primary_type
};

/* The window's source's events. */
static const struct zwp_primary_selection_source_v1_listener source_listener = {
	primary_send,
	primary_cancelled
};

/*
 * Binds the compositor's primary selection manager (from the registry).
 */
void
keiui_primary_bind(
	struct kl_window *window,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Version 1 is the only one. */
	window->primary_manager = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1U);
}

/*
 * Gets the seat's primary selection device, once the globals are bound.
 */
void
keiui_primary_start(
	struct kl_window *window)
{
	/* Nothing to share through. */
	if (window->primary_manager == NULL || window->seat == NULL)
		return;

	/* The device, and its events. */
	window->primary_device = zwp_primary_selection_device_manager_v1_get_device(window->primary_manager, window->seat);
	if (window->primary_device != NULL)
		(void)zwp_primary_selection_device_v1_add_listener(window->primary_device, &device_listener, window);
}

/*
 * Makes the selected text the primary selection.  The window keeps its own
 * copy, sent from there.
 */
void
kl_window_select(
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
	free(window->primary_text);
	window->primary_text = copy;
	window->primary_length = length;
	if (window->primary_device == NULL)
		return;

	/* The last source goes. */
	if (window->primary_source != NULL)
		zwp_primary_selection_source_v1_destroy(window->primary_source);

	/* A source with the two text types, as the primary selection. */
	window->primary_source = zwp_primary_selection_device_manager_v1_create_source(window->primary_manager);
	if (window->primary_source == NULL)
		return;
	(void)zwp_primary_selection_source_v1_add_listener(window->primary_source, &source_listener, window);
	zwp_primary_selection_source_v1_offer(window->primary_source, PRIMARY_TYPE_UTF8);
	zwp_primary_selection_source_v1_offer(window->primary_source, PRIMARY_TYPE_PLAIN);
	zwp_primary_selection_device_v1_set_selection(window->primary_device, window->primary_source, window->serial);
	(void)wl_display_flush(window->display);
}

/*
 * Receives the primary selection's text into a buffer (a middle click):
 * the window's own directly, another client's through a pipe (read until
 * its end or PRIMARY_RECEIVE_MS).  Returns the bytes (0 for none).
 */
size_t
kl_window_paste_primary(
	struct kl_window *window,
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

	/* The window's own text (or the only one, without the compositor's). */
	if (window->primary_device == NULL || window->primary_source != NULL) {
		length = window->primary_length;
		if (length > size)
			length = size;
		if (length != 0U)
			memcpy(text, window->primary_text, length);
		return length;
	}

	/* No text to receive. */
	if (window->primary_offer == NULL || !window->primary_offer_text)
		return 0;

	/* The pipe; its writing end goes to the offer's client. */
	error = pipe(pipes);
	if (error != 0)
		return 0;
	zwp_primary_selection_offer_v1_receive(window->primary_offer, PRIMARY_TYPE_UTF8, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(window->display);

	/* The data, until the writer closes (or the time is up). */
	length = 0;
	deadline = keiui_clock_ms() + PRIMARY_RECEIVE_MS;
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

	/* Succeeded: the bytes received. */
	return length;
}

/*
 * Destroys the primary selection's objects (before the seat they belong to).
 */
void
keiui_primary_close(
	struct kl_window *window)
{
	/* The offer, the source, the device and the manager. */
	if (window->primary_offer != NULL)
		zwp_primary_selection_offer_v1_destroy(window->primary_offer);
	if (window->primary_source != NULL)
		zwp_primary_selection_source_v1_destroy(window->primary_source);
	if (window->primary_device != NULL)
		zwp_primary_selection_device_v1_destroy(window->primary_device);
	if (window->primary_manager != NULL)
		zwp_primary_selection_device_manager_v1_destroy(window->primary_manager);
	window->primary_offer = NULL;
	window->primary_source = NULL;
	window->primary_device = NULL;
	window->primary_manager = NULL;
}

/* Takes a new offer: its types are heard next. */
static void
primary_offer(
	void *data,
	struct zwp_primary_selection_device_v1 *device,
	struct zwp_primary_selection_offer_v1 *offer)
{
	struct kl_window *window;

	/* The offer being described, with no text type yet. */
	(void)device;
	window = data;
	window->primary_pending_text = 0;
	(void)zwp_primary_selection_offer_v1_add_listener(offer, &offer_listener, window);
}

/* The primary selection changed: its offer (the last one described), or none; the window hears it changed. */
static void
primary_selection(
	void *data,
	struct zwp_primary_selection_device_v1 *device,
	struct zwp_primary_selection_offer_v1 *offer)
{
	struct kl_window_event *event;
	struct kl_window *window;

	/* The offer before goes. */
	(void)device;
	window = data;
	if (window->primary_offer != NULL && window->primary_offer != offer)
		zwp_primary_selection_offer_v1_destroy(window->primary_offer);

	/* The new one, and whether it has text. */
	window->primary_offer = offer;
	window->primary_offer_text = 0;
	if (offer != NULL)
		window->primary_offer_text = window->primary_pending_text;

	/* The window's input (WS131 p018). */
	event = keiui_window_push(window, KL_WINDOW_SELECTION);
	if (event != NULL) {
		event->code = KL_SELECTION_PRIMARY;
		event->pressed = window->primary_offer_text;
	}
}

/* Notes an offer's type: text is what the window takes. */
static void
primary_type(
	void *data,
	struct zwp_primary_selection_offer_v1 *offer,
	const char *mime_type)
{
	struct kl_window *window;
	int utf8;
	int plain;

	/* Either text type. */
	(void)offer;
	window = data;
	utf8 = strcmp(mime_type, PRIMARY_TYPE_UTF8);
	plain = strcmp(mime_type, PRIMARY_TYPE_PLAIN);
	if (utf8 == 0 || plain == 0)
		window->primary_pending_text = 1;
}

/* Writes the window's selected text into another client's descriptor. */
static void
primary_send(
	void *data,
	struct zwp_primary_selection_source_v1 *source,
	const char *mime_type,
	int32_t fd)
{
	struct kl_window *window;
	size_t written;
	ssize_t count;

	/* The whole text, whatever the text type. */
	(void)source;
	(void)mime_type;
	window = data;
	written = 0;
	while (written < window->primary_length) {
		count = write(fd, window->primary_text + written, window->primary_length - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		written += (size_t)count;
	}

	/* The reader sees the end. */
	close(fd);
}

/* Another client's text is the primary selection now: the source goes. */
static void
primary_cancelled(
	void *data,
	struct zwp_primary_selection_source_v1 *source)
{
	struct kl_window *window;

	/* The source. */
	window = data;
	zwp_primary_selection_source_v1_destroy(source);
	if (window->primary_source == source)
		window->primary_source = NULL;
}
