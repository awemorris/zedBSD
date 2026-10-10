/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system as an application sees it (system-private.h's struct
 * system_view; WS131 p010): what the compositor sends is kept pending and
 * put into effect at its done, so that an application never sees half of
 * a change; the answered requests wait in a ring until they are taken.
 * Nothing here knows Wayland.
 */

#include "system-private.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void system_view_phone_mark(struct system_view *view, unsigned kind);

/*
 * Starts a view with nothing told: the network not reached, the sound not
 * reached, the power unknown with no action, no device.
 */
void
system_view_init(
	struct system_view *view)
{
	/* Everything zero, the battery's charge unknown. */
	memset(view, 0, sizeof(*view));
	view->power.percent = -1;
	view->power_pending.percent = -1;
}

/*
 * Keeps the network's state pending until its done.
 */
void
system_view_network_state(
	struct system_view *view,
	const struct kl_network_state *state)
{
	/* The state, pending. */
	view->network_pending = *state;
	view->network_touched = 1U;
}

/*
 * Adds a network of a scan to the pending list, starting a new list after
 * the last one's end.
 */
void
system_view_access_point(
	struct system_view *view,
	const struct kl_network_ap *ap)
{
	/* The first network of a new scan starts its list. */
	if (!view->scan_open) {
		view->scan_open = 1U;
		view->scan_pending_count = 0U;
	}

	/* The network, while there is room (the compositor sends no more than the list holds). */
	if (view->scan_pending_count >= KL_NETWORK_SCAN_MAX)
		return;
	view->scan_pending[view->scan_pending_count] = *ap;
	view->scan_pending_count++;
}

/*
 * Ends a scan's pending list (an empty scan has no network before it).
 */
void
system_view_scan_done(
	struct system_view *view)
{
	/* A scan that found nothing. */
	if (!view->scan_open)
		view->scan_pending_count = 0U;

	/* The list is whole, and waits for the done. */
	view->scan_open = 0U;
	view->scan_touched = 1U;
}

/*
 * Puts the network's pending state and scan into effect.
 */
void
system_view_network_done(
	struct system_view *view)
{
	int differs;

	/* The state, when it changed. */
	if (view->network_touched) {
		view->network_touched = 0U;
		differs = memcmp(&view->network, &view->network_pending, sizeof(view->network));
		view->network = view->network_pending;
		if (differs != 0)
			view->changed |= KL_SYSTEM_CHANGED_NETWORK;
	}

	/* The scan, as a new list. */
	if (view->scan_touched) {
		view->scan_touched = 0U;
		memcpy(view->scan, view->scan_pending, view->scan_pending_count * sizeof(view->scan[0]));
		view->scan_count = view->scan_pending_count;
		view->changed |= KL_SYSTEM_CHANGED_SCAN;
	}
}

/*
 * Adds an interface to the pending details, starting new details after the
 * last ones' end.
 */
void
system_view_link(
	struct system_view *view,
	const struct kl_network_link *link)
{
	/* The first item of new details starts them. */
	if (!view->details_open) {
		view->details_open = 1U;
		view->links_pending_count = 0U;
		view->dns_pending_count = 0U;
		view->saved_pending_count = 0U;
	}

	/* The interface, while there is room. */
	if (view->links_pending_count >= KL_NETWORK_LINKS_MAX)
		return;
	view->links_pending[view->links_pending_count] = *link;
	view->links_pending_count++;
}

/*
 * Gives a pending interface its wired configuration (ws089-p022): how it
 * is configured and its router; a name not among them changes nothing.
 */
void
system_view_wired(
	struct system_view *view,
	const char *name,
	unsigned mode,
	const char *router)
{
	struct kl_network_link *link;
	size_t index;
	int differs;

	/* Only within details being received. */
	if (!view->details_open)
		return;

	/* The pending interface of that name. */
	for (index = 0; index < view->links_pending_count; index++) {
		link = &view->links_pending[index];
		differs = strcmp(link->name, name);
		if (differs != 0)
			continue;
		link->wired_mode = mode;
		system_view_copy(link->router, sizeof(link->router), router);
		return;
	}
}

/*
 * Gives a pending interface of the details its link's speed in Mb/s.
 *
 * The speed comes after the interface's link, as its wired configuration
 * does, and is kept only while those details are being received (BUG-222).
 */
void
system_view_link_speed(
	struct system_view *view,
	const char *name,
	unsigned mbps)
{
	struct kl_network_link *link;
	size_t index;
	int differs;

	/* Only within details being received. */
	if (!view->details_open)
		return;

	/* The pending interface of that name. */
	for (index = 0; index < view->links_pending_count; index++) {
		link = &view->links_pending[index];
		differs = strcmp(link->name, name);
		if (differs != 0)
			continue;
		link->link_mbps = mbps;
		return;
	}
}

/*
 * Adds a DNS server to the pending details.
 */
void
system_view_dns(
	struct system_view *view,
	const char *address)
{
	/* The first item of new details starts them. */
	if (!view->details_open) {
		view->details_open = 1U;
		view->links_pending_count = 0U;
		view->dns_pending_count = 0U;
		view->saved_pending_count = 0U;
	}

	/* The server, while there is room. */
	if (view->dns_pending_count >= KL_NETWORK_DNS_MAX)
		return;
	system_view_copy(view->dns_pending[view->dns_pending_count], KL_NETWORK_ADDRESS_MAX, address);
	view->dns_pending_count++;
}

/*
 * Adds a saved network to the pending details.
 */
void
system_view_saved(
	struct system_view *view,
	const char *ssid)
{
	/* The first item of new details starts them. */
	if (!view->details_open) {
		view->details_open = 1U;
		view->links_pending_count = 0U;
		view->dns_pending_count = 0U;
		view->saved_pending_count = 0U;
	}

	/* The network, while there is room. */
	if (view->saved_pending_count >= KL_NETWORK_SAVED_MAX)
		return;
	system_view_copy(view->saved_pending[view->saved_pending_count], KL_NETWORK_SSID_MAX, ssid);
	view->saved_pending_count++;
}

/*
 * Puts the pending details into effect (details with no item are empty).
 */
void
system_view_details_done(
	struct system_view *view)
{
	/* Details that had no item. */
	if (!view->details_open) {
		view->links_pending_count = 0U;
		view->dns_pending_count = 0U;
		view->saved_pending_count = 0U;
	}

	/* The three lists, as one state; the next item starts new details. */
	view->details_open = 0U;
	memcpy(view->links, view->links_pending, view->links_pending_count * sizeof(view->links[0]));
	view->link_count = view->links_pending_count;
	memcpy(view->dns, view->dns_pending, view->dns_pending_count * sizeof(view->dns[0]));
	view->dns_count = view->dns_pending_count;
	memcpy(view->saved, view->saved_pending, view->saved_pending_count * sizeof(view->saved[0]));
	view->saved_count = view->saved_pending_count;
	view->changed |= KL_SYSTEM_CHANGED_DETAILS;
}

/*
 * Keeps the sound's state pending until its done.
 */
void
system_view_audio_state(
	struct system_view *view,
	const struct kl_audio_state *state)
{
	/* The state, pending. */
	view->audio_pending = *state;
	view->audio_touched = 1U;
}

/*
 * Puts the sound's pending state into effect.
 */
void
system_view_audio_done(
	struct system_view *view)
{
	int differs;

	/* Nothing pending. */
	if (!view->audio_touched)
		return;

	/* The state, told when it changed. */
	view->audio_touched = 0U;
	differs = memcmp(&view->audio, &view->audio_pending, sizeof(view->audio));
	view->audio = view->audio_pending;
	if (differs != 0)
		view->changed |= KL_SYSTEM_CHANGED_AUDIO;
}

/*
 * Keeps the power's state pending until its done.
 */
void
system_view_power_state(
	struct system_view *view,
	const struct kl_power_state *state)
{
	/* The state, pending. */
	view->power_pending = *state;
	view->power_touched = 1U;
}

/*
 * Puts the power's pending state into effect.
 */
void
system_view_power_done(
	struct system_view *view)
{
	int differs;

	/* Nothing pending. */
	if (!view->power_touched)
		return;

	/* The state, told when it changed. */
	view->power_touched = 0U;
	differs = memcmp(&view->power, &view->power_pending, sizeof(view->power));
	view->power = view->power_pending;
	if (differs != 0)
		view->changed |= KL_SYSTEM_CHANGED_POWER;
}

/*
 * Keeps Remote Login's state (ws089-p025) until its done.
 */
void
system_view_sharing_state(
	struct system_view *view,
	const struct kl_sharing_state *state)
{
	/* The state, pending. */
	view->sharing_pending = *state;
	view->sharing_touched = 1U;
}

/*
 * Puts Remote Login's pending state into effect.
 */
void
system_view_sharing_done(
	struct system_view *view)
{
	int differs;

	/* Nothing pending. */
	if (!view->sharing_touched)
		return;

	/* The state, told when it changed. */
	view->sharing_touched = 0U;
	differs = memcmp(&view->sharing, &view->sharing_pending, sizeof(view->sharing));
	view->sharing = view->sharing_pending;
	if (differs != 0)
		view->changed |= KL_SYSTEM_CHANGED_SHARING;
}

/*
 * Adds a device to the pending list, starting a new list after the last
 * done.
 */
void
system_view_device(
	struct system_view *view,
	const struct kl_device *device)
{
	/* The first device after a done starts the list. */
	if (!view->devices_open) {
		view->devices_open = 1U;
		view->devices_pending_count = 0U;
	}

	/* The device, while there is room, with no file system or size until its volume event. */
	if (view->devices_pending_count >= KL_DEVICES_MAX)
		return;
	view->devices_pending[view->devices_pending_count] = *device;
	memset(&view->device_infos_pending[view->devices_pending_count], 0, sizeof(view->device_infos_pending[0]));
	view->devices_pending_count++;
}

/*
 * Keeps a pending device's file system and size (the volume event after its
 * device, version 9); a device not in the pending list is passed over.
 */
void
system_view_device_info(
	struct system_view *view,
	const char *id,
	const char *fs,
	uint64_t bytes)
{
	size_t index;
	int same;

	/* The pending device of that ID. */
	if (!view->devices_open)
		return;
	for (index = 0U; index < view->devices_pending_count; index++) {
		same = strcmp(view->devices_pending[index].id, id);
		if (same == 0) {
			system_view_copy(view->device_infos_pending[index].fs, sizeof(view->device_infos_pending[index].fs), fs);
			view->device_infos_pending[index].bytes = bytes;
			return;
		}
	}
}

/*
 * Puts the pending devices into effect: the compositor sends the whole list
 * before each done, so a done with no device before it empties the list
 * (ws132-p004: the last volume went).
 */
void
system_view_devices_done(
	struct system_view *view)
{
	/* A done after no device: an empty list. */
	if (!view->devices_open)
		view->devices_pending_count = 0U;

	/* The list, as one state. */
	view->devices_open = 0U;
	memcpy(view->devices, view->devices_pending, view->devices_pending_count * sizeof(view->devices[0]));
	memcpy(view->device_infos, view->device_infos_pending, view->devices_pending_count * sizeof(view->device_infos[0]));
	view->device_count = view->devices_pending_count;
	view->changed |= KL_SYSTEM_CHANGED_DEVICES;
}

/*
 * Keeps an answered request with its error until it is taken; when the
 * ring is full the oldest is dropped.
 */
void
system_view_result(
	struct system_view *view,
	uint32_t request,
	uint32_t applied)
{
	unsigned slot;

	/* A full ring drops its oldest. */
	if (view->result_count == SYSTEM_VIEW_RESULTS) {
		view->result_head = (view->result_head + 1U) % SYSTEM_VIEW_RESULTS;
		view->result_count--;
	}

	/* The answer after the newest. */
	slot = (view->result_head + view->result_count) % SYSTEM_VIEW_RESULTS;
	view->results[slot].request = request;
	view->results[slot].error = system_view_error_of(applied);
	view->result_count++;
	view->changed |= KL_SYSTEM_CHANGED_RESULT;
}

/*
 * Takes the oldest answered request: 1 with it, 0 when none waits.
 */
int
system_view_take_result(
	struct system_view *view,
	uint32_t *request,
	int *error)
{
	const struct system_view_result *oldest;

	/* None waits. */
	if (view->result_count == 0U)
		return 0;

	/* The oldest, out of the ring. */
	oldest = &view->results[view->result_head];
	*request = oldest->request;
	*error = oldest->error;
	view->result_head = (view->result_head + 1U) % SYSTEM_VIEW_RESULTS;
	view->result_count--;

	/* Succeeded: one answer taken. */
	return 1;
}

/*
 * Keeps a notification's event (ws156-p002): its number posted, its body
 * clicked, or why it closed, for kl_system_take_notify_event; a full ring
 * drops its oldest.
 */
void
system_view_notify_event(
	struct system_view *view,
	const struct kl_notify_event *event)
{
	unsigned slot;

	/* A full ring drops its oldest, counted for the KL_NOTIFY_LOST the next take gives first (ws177-p005). */
	if (view->notify_count == SYSTEM_VIEW_NOTIFY_EVENTS) {
		view->notify_head = (view->notify_head + 1U) % SYSTEM_VIEW_NOTIFY_EVENTS;
		view->notify_count--;
		view->notify_lost++;
	}

	/* The event after the newest. */
	slot = (view->notify_head + view->notify_count) % SYSTEM_VIEW_NOTIFY_EVENTS;
	view->notify_events[slot] = *event;
	view->notify_count++;
	view->changed |= KL_SYSTEM_CHANGED_NOTIFY;
}

/*
 * Takes the oldest notification event: 1 with it, 0 when none waits.
 * Events a full ring dropped are told first, as one KL_NOTIFY_LOST whose
 * id is how many (ws177-p005).
 */
int
system_view_take_notify_event(
	struct system_view *view,
	struct kl_notify_event *event)
{
	/* The events lost come before the oldest kept. */
	if (view->notify_lost != 0U) {
		event->kind = KL_NOTIFY_LOST;
		event->request = 0U;
		event->id = view->notify_lost;
		event->reason = 0U;
		view->notify_lost = 0U;
		return 1;
	}

	/* None waits. */
	if (view->notify_count == 0U)
		return 0;

	/* The oldest, out of the ring. */
	*event = view->notify_events[view->notify_head];
	view->notify_head = (view->notify_head + 1U) % SYSTEM_VIEW_NOTIFY_EVENTS;
	view->notify_count--;

	/* Succeeded: one event taken. */
	return 1;
}

/*
 * Keeps whether this reader is allowed to hear the arrivals now, as the
 * compositor told it (ws177-p005), a change of the mail for the program.
 */
void
system_view_mail_allowed(
	struct system_view *view,
	unsigned on)
{
	/* 2 allowed, 1 not. */
	view->mail_allowed = 1U;
	if (on != 0U)
		view->mail_allowed = 2U;
	view->changed |= KL_SYSTEM_CHANGED_MAIL;
}

/*
 * Keeps an arrival of mail told to this listener (ws169-p002) for
 * kl_system_take_mail_event, its strings cut to their rooms; a full ring
 * drops its oldest.
 */
void
system_view_mail_event(
	struct system_view *view,
	const char *from,
	const char *subject,
	const char *code)
{
	struct kl_mail_event *event;
	unsigned slot;

	/* A full ring drops its oldest. */
	if (view->mail_count == SYSTEM_VIEW_MAIL_EVENTS) {
		view->mail_head = (view->mail_head + 1U) % SYSTEM_VIEW_MAIL_EVENTS;
		view->mail_count--;
	}

	/* The arrival after the newest. */
	slot = (view->mail_head + view->mail_count) % SYSTEM_VIEW_MAIL_EVENTS;
	event = &view->mail_events[slot];
	system_view_copy(event->from, sizeof(event->from), from);
	system_view_copy(event->subject, sizeof(event->subject), subject);
	system_view_copy(event->code, sizeof(event->code), code);

	/* Counted, and told as a change. */
	view->mail_count++;
	view->changed |= KL_SYSTEM_CHANGED_MAIL;
}

/*
 * Takes the oldest arrival of mail: 1 with it, 0 when none waits.
 */
int
system_view_take_mail_event(
	struct system_view *view,
	struct kl_mail_event *event)
{
	/* None waits. */
	if (view->mail_count == 0U)
		return 0;

	/* The oldest, out of the ring. */
	*event = view->mail_events[view->mail_head];
	view->mail_head = (view->mail_head + 1U) % SYSTEM_VIEW_MAIL_EVENTS;
	view->mail_count--;

	/* Succeeded: one arrival taken. */
	return 1;
}

/*
 * Keeps a phone event (ws170-p004) for kl_system_take_phone_event; a full
 * ring drops its oldest.
 */
void
system_view_phone_event(
	struct system_view *view,
	const struct kl_phone_event *event)
{
	unsigned slot;

	/*
	 * A full ring drops its oldest; the next take tells KL_PHONE_DROPPED
	 * first (ws197-p004a), so that the program synchronises again.
	 */
	if (view->phone_count == SYSTEM_VIEW_PHONE_EVENTS) {
		view->phone_head = (view->phone_head + 1U) % SYSTEM_VIEW_PHONE_EVENTS;
		view->phone_count--;
		view->phone_lost = 1U;
	}

	/* The event after the newest, counted and told as a change. */
	slot = (view->phone_head + view->phone_count) % SYSTEM_VIEW_PHONE_EVENTS;
	view->phone_events[slot] = *event;
	view->phone_count++;
	view->changed |= KL_SYSTEM_CHANGED_PHONE;
}

/*
 * Takes the oldest phone event: 1 with it, 0 when none waits.
 */
int
system_view_take_phone_event(
	struct system_view *view,
	struct kl_phone_event *event)
{
	/* Events the ring dropped are told first, as one. */
	if (view->phone_lost) {
		view->phone_lost = 0U;
		memset(event, 0, sizeof(*event));
		event->kind = KL_PHONE_DROPPED;
		return 1;
	}

	/* None waits. */
	if (view->phone_count == 0U)
		return 0;

	/* The oldest, out of the ring. */
	*event = view->phone_events[view->phone_head];
	view->phone_head = (view->phone_head + 1U) % SYSTEM_VIEW_PHONE_EVENTS;
	view->phone_count--;

	/* Succeeded: one event taken. */
	return 1;
}

/*
 * Keeps a phone item (ws197-p004a) with its own copy of the text (ended by
 * a NUL) for kl_system_take_phone_item; a full queue drops its oldest and
 * tells KL_PHONE_DROPPED.  The first item of an empty queue is told by one
 * KL_PHONE_ITEMS.
 */
void
system_view_phone_item(
	struct system_view *view,
	const struct kl_phone_item *item,
	const void *text,
	size_t length)
{
	struct system_view_phone_item *kept;
	unsigned slot;
	char *copy;

	/* The text's copy; without memory the item is lost as a drop. */
	copy = malloc(length + 1U);
	if (copy == NULL) {
		if (item->has_mime)
			close(item->mime_descriptor);
		system_view_phone_mark(view, KL_PHONE_DROPPED);
		return;
	}

	/* The bytes and the NUL. */
	if (length > 0U)
		memcpy(copy, text, length);
	copy[length] = '\0';

	/* A full queue drops its oldest, and the program synchronises again. */
	if (view->phone_item_count == SYSTEM_VIEW_PHONE_ITEMS) {
		if (view->phone_items[view->phone_item_head].item.has_mime)
			close(view->phone_items[view->phone_item_head].item.mime_descriptor);
		free(view->phone_items[view->phone_item_head].text);
		view->phone_items[view->phone_item_head].text = NULL;
		view->phone_item_head = (view->phone_item_head + 1U) % SYSTEM_VIEW_PHONE_ITEMS;
		view->phone_item_count--;
		system_view_phone_mark(view, KL_PHONE_DROPPED);
	}

	/* The first item of an empty queue is told once. */
	if (view->phone_item_count == 0U)
		system_view_phone_mark(view, KL_PHONE_ITEMS);

	/* The item after the newest, owning its text. */
	slot = (view->phone_item_head + view->phone_item_count) % SYSTEM_VIEW_PHONE_ITEMS;
	kept = &view->phone_items[slot];
	kept->item = *item;
	kept->item.text = NULL;
	kept->item.length = length;
	kept->text = copy;
	view->phone_item_count++;
	view->changed |= KL_SYSTEM_CHANGED_PHONE;
}

/*
 * Takes the oldest phone item into the caller's structure of size bytes
 * (a later version's larger one has the rest zeroed, a smaller one gets
 * the fields it has): 1 with it, 0 when none waits or the size is less
 * than KL_VERSION 79's.  Its text lives until the next take.
 */
int
system_view_take_phone_item(
	struct system_view *view,
	struct kl_phone_item *item,
	size_t size)
{
	struct system_view_phone_item *kept;
	struct kl_phone_item copy;
	size_t copied;

	/* The text of the item taken last goes now. */
	free(view->phone_taken_text);
	view->phone_taken_text = NULL;

	/* A structure of KL_VERSION 79 at least, and an item waiting. */
	if (size < SYSTEM_VIEW_PHONE_ITEM_SIZE_79)
		return 0;
	if (view->phone_item_count == 0U)
		return 0;

	/* The oldest, with its text. */
	kept = &view->phone_items[view->phone_item_head];
	copy = kept->item;
	copy.text = kept->text;

	/* Older callers receive the caption and release descriptors they cannot represent. */
	if (copy.has_mime && size < sizeof(copy)) {
		close(copy.mime_descriptor);
		copy.has_mime = 0U;
		copy.mime_descriptor = -1;
	}

	/* Removes the queue's ownership after transfer or legacy cleanup. */
	kept->item.has_mime = 0U;

	/* As much as the caller's structure holds, the rest of it zero. */
	copied = sizeof(copy);
	if (copied > size)
		copied = size;
	memset(item, 0, size);
	memcpy(item, &copy, copied);

	/* Its text now the one taken last. */
	view->phone_taken_text = kept->text;
	kept->text = NULL;
	view->phone_item_head = (view->phone_item_head + 1U) % SYSTEM_VIEW_PHONE_ITEMS;
	view->phone_item_count--;

	/* Succeeded: one item taken. */
	return 1;
}

/*
 * Keeps a page's end (ws197-p004a) among the last ones, by its request.
 */
void
system_view_phone_page_end(
	struct system_view *view,
	const struct system_view_page_end *end)
{
	/* Over the oldest. */
	view->phone_ends[view->phone_end_next] = *end;
	view->phone_end_next = (view->phone_end_next + 1U) % SYSTEM_VIEW_PHONE_ENDS;
}

/*
 * Finds the end of a sync's page by its request: 0 with it, ENOENT when it
 * did not come (or is older than the last ones kept).
 */
int
system_view_phone_page_end_of(
	const struct system_view *view,
	uint32_t request,
	struct system_view_page_end *end)
{
	unsigned index;

	/* The request 0 is never a sync's. */
	if (request == 0U)
		return ENOENT;

	/* Each end kept. */
	for (index = 0U; index < SYSTEM_VIEW_PHONE_ENDS; index++) {
		if (view->phone_ends[index].request == request) {
			*end = view->phone_ends[index];
			return 0;
		}
	}

	/* Not among them. */
	return ENOENT;
}

/*
 * Keeps the phone link's state (ws197-p004a) and tells
 * KL_PHONE_LINK_CHANGED.  The contacts' part comes from the link_contacts
 * told just before (ws197-p005); a link without one has none.
 */
void
system_view_phone_link(
	struct system_view *view,
	const struct kl_phone_link *link)
{
	/* The state, known from now on. */
	view->phone_link = *link;
	view->phone_link_known = 1U;

	/* The contacts' part told for this link (none from a compositor that did not tell it). */
	view->phone_link.contacts = 0U;
	view->phone_link.record = 0U;
	view->phone_link.contacts_why[0] = '\0';
	if (view->phone_contacts_told) {
		view->phone_link.contacts = view->phone_contacts;
		view->phone_link.record = view->phone_record;
		(void)snprintf(view->phone_link.contacts_why, sizeof(view->phone_link.contacts_why), "%s", view->phone_contacts_why);
	}

	/* Taken: the next link needs its own. */
	view->phone_contacts_told = 0U;

	/* Told to the program. */
	system_view_phone_mark(view, KL_PHONE_LINK_CHANGED);
}

/*
 * Keeps the contacts' part of the link that follows (ws197-p005): the
 * contacts' state, whether the phone's record is known, and why the
 * contacts stopped.  A second one before the link replaces the first.
 */
void
system_view_phone_link_contacts(
	struct system_view *view,
	unsigned contacts,
	unsigned record,
	const char *why)
{
	/* Kept for the next link. */
	view->phone_contacts = contacts;
	view->phone_record = record;
	(void)snprintf(view->phone_contacts_why, sizeof(view->phone_contacts_why), "%s", why);
	view->phone_contacts_told = 1U;
}

/*
 * Copies the phone link's state into the caller's structure of size bytes
 * (a later version's larger one has the rest zeroed; KL_VERSION 79's
 * smaller one gets the fields it has, ws197-p005 review-1 M6): 0, ENOENT
 * before it was told, or EINVAL for a size less than KL_VERSION 79's.
 */
int
system_view_phone_link_get(
	const struct system_view *view,
	struct kl_phone_link *link,
	size_t size)
{
	size_t copied;

	/* A structure of KL_VERSION 79 at least. */
	if (size < SYSTEM_VIEW_PHONE_LINK_SIZE_79)
		return EINVAL;

	/* Not told yet. */
	if (!view->phone_link_known)
		return ENOENT;

	/* As much as the caller's structure holds, the rest of it zero. */
	copied = sizeof(view->phone_link);
	if (copied > size)
		copied = size;
	memset(link, 0, size);
	memcpy(link, &view->phone_link, copied);

	/* Succeeded: the copy. */
	return 0;
}

/*
 * Tells KL_PHONE_DROPPED (ws197-p004a): items that came were lost on the
 * way, the program synchronises again.
 */
void
system_view_phone_dropped(
	struct system_view *view)
{
	/* The mark. */
	system_view_phone_mark(view, KL_PHONE_DROPPED);
}

/*
 * Starts the program's sync of a request (ws197-p004a): 0, or EBUSY while
 * another is under way and not older than SYSTEM_VIEW_PHONE_SYNC_MS.
 */
int
system_view_phone_sync_start(
	struct system_view *view,
	uint32_t request,
	uint64_t now_ms)
{
	uint64_t age;

	/* One under way, answered or given up after its time. */
	if (view->phone_sync_request != 0U) {
		age = now_ms - view->phone_sync_started_ms;
		if (age < SYSTEM_VIEW_PHONE_SYNC_MS)
			return EBUSY;
	}

	/* Succeeded: this one is under way. */
	view->phone_sync_request = request;
	view->phone_sync_started_ms = now_ms;
	return 0;
}

/*
 * Takes a phone request's done (ws197-p004a): its result, and the end of
 * the program's sync when it is the sync's.
 */
void
system_view_phone_done(
	struct system_view *view,
	uint32_t request,
	uint32_t code)
{
	/* The sync under way ends with its answer. */
	if (request != 0U && request == view->phone_sync_request)
		view->phone_sync_request = 0U;

	/* The result for kl_system_take_result. */
	system_view_result(view, request, code);
}

/*
 * Frees what the phone's items own (ws197-p004a), when the view goes.
 */
void
system_view_phone_release(
	struct system_view *view)
{
	unsigned index;
	unsigned slot;

	/* The texts of the items waiting. */
	for (index = 0U; index < view->phone_item_count; index++) {
		slot = (view->phone_item_head + index) % SYSTEM_VIEW_PHONE_ITEMS;
		if (view->phone_items[slot].item.has_mime)
			close(view->phone_items[slot].item.mime_descriptor);
		free(view->phone_items[slot].text);
		view->phone_items[slot].text = NULL;
	}

	/* None waits any more. */
	view->phone_item_count = 0U;

	/* The text of the item taken last. */
	free(view->phone_taken_text);
	view->phone_taken_text = NULL;
}

/*
 * Gives what changed since the last take, and starts again from nothing.
 */
unsigned
system_view_take_changed(
	struct system_view *view)
{
	unsigned changed;

	/* The bits, cleared. */
	changed = view->changed;
	view->changed = 0U;
	return changed;
}

/*
 * Gives the errno value a request's result means.
 */
int
system_view_error_of(
	uint32_t applied)
{
	/* Each result the compositor gives. */
	switch (applied) {
	case KL_SYSTEM_RESULT_OK:
		return 0;
	case KL_SYSTEM_RESULT_DENIED:
		return EPERM;
	case KL_SYSTEM_RESULT_UNSUPPORTED:
		return ENOTSUP;
	case KL_SYSTEM_RESULT_BUSY:
		return EBUSY;
	case KL_SYSTEM_RESULT_INVALID:
		return EINVAL;
	case KL_SYSTEM_RESULT_UNAVAILABLE:
		return ENODEV;
	case KL_SYSTEM_RESULT_NO_KEY:
		return ENOENT;
	case KL_SYSTEM_RESULT_REFUSED:
		return EACCES;
	case KL_SYSTEM_RESULT_UNREACHABLE:
		return ENETUNREACH;
	case KL_SYSTEM_RESULT_STALE:
		return ESTALE;
	case KL_SYSTEM_RESULT_LOST:
		return ECONNRESET;
	case KL_SYSTEM_RESULT_TIMEOUT:
		return ETIMEDOUT;
	case KL_SYSTEM_RESULT_TOO_LARGE:
		return EMSGSIZE;
	case KL_SYSTEM_RESULT_NO_ROOM:
		return ENOBUFS;
	case KL_SYSTEM_RESULT_NOT_CONNECTED:
		return ENOTCONN;
	default:
		break;
	}

	/* Anything else failed. */
	return EIO;
}

/*
 * Copies a string into room of its own, cut to fit.
 */
void
system_view_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* As much as fits, with the NUL. */
	length = strlen(from);
	if (length >= size)
		length = size - 1U;
	memcpy(to, from, length);
	to[length] = '\0';
}

/*
 * Adds a printer to the pending list (ws145-p003), starting a new list
 * (of the printers and the jobs) after the last done.
 */
void
system_view_printer(
	struct system_view *view,
	const struct kl_printer *printer)
{
	/* The first after a done starts the lists. */
	if (!view->printers_open) {
		view->printers_open = 1U;
		view->printers_pending_count = 0U;
		view->print_jobs_pending_count = 0U;
	}

	/* The printer, while there is room. */
	if (view->printers_pending_count >= KL_PRINTERS_MAX)
		return;
	view->printers_pending[view->printers_pending_count] = *printer;
	view->printers_pending_count++;
}

/*
 * Adds a print job to the pending list (ws145-p003).
 */
void
system_view_print_job(
	struct system_view *view,
	const struct kl_print_job *job)
{
	/* The first after a done starts the lists. */
	if (!view->printers_open) {
		view->printers_open = 1U;
		view->printers_pending_count = 0U;
		view->print_jobs_pending_count = 0U;
	}

	/* The job, while there is room. */
	if (view->print_jobs_pending_count >= KL_PRINT_JOBS_MAX)
		return;
	view->print_jobs_pending[view->print_jobs_pending_count] = *job;
	view->print_jobs_pending_count++;
}

/*
 * Puts the pending printers and jobs into effect (a done after neither
 * empties both).
 */
void
system_view_printers_done(
	struct system_view *view)
{
	/* A done after nothing: empty lists. */
	if (!view->printers_open) {
		view->printers_pending_count = 0U;
		view->print_jobs_pending_count = 0U;
	}

	/* The lists, as one state. */
	view->printers_open = 0U;
	memcpy(view->printers, view->printers_pending, view->printers_pending_count * sizeof(view->printers[0]));
	view->printer_count = view->printers_pending_count;
	memcpy(view->print_jobs, view->print_jobs_pending, view->print_jobs_pending_count * sizeof(view->print_jobs[0]));
	view->print_job_count = view->print_jobs_pending_count;
	view->changed |= KL_SYSTEM_CHANGED_PRINTERS;
}

/*
 * Adds a display to the pending snapshot (ws113-p005), starting a new one
 * after the last done.
 */
void
system_view_display(
	struct system_view *view,
	const struct kl_display *display)
{
	/* A new snapshot after the last done. */
	if (!view->displays_open) {
		view->displays_open = 1U;
		view->displays_pending_count = 0U;
	}

	/* The display, while there is room. */
	if (view->displays_pending_count >= KL_DISPLAYS_MAX)
		return;
	view->displays_pending[view->displays_pending_count] = *display;
	view->displays_pending_count++;
}

/*
 * Puts the pending snapshot of the displays into effect with its serial
 * and mode (a done after no display empties the list).
 */
void
system_view_displays_done(
	struct system_view *view,
	uint32_t serial,
	uint32_t mode)
{
	/* A done after nothing: no display. */
	if (!view->displays_open)
		view->displays_pending_count = 0U;

	/* The snapshot, as one state. */
	view->displays_open = 0U;
	memcpy(view->displays, view->displays_pending, view->displays_pending_count * sizeof(view->displays[0]));
	view->display_count = view->displays_pending_count;
	view->displays_serial = serial;
	view->displays_mode = mode;
	view->changed |= KL_SYSTEM_CHANGED_DISPLAYS;
}

/*
 * Keeps a print's job for its request (the oldest kept is replaced when
 * the ring is full).
 */
void
system_view_print_queued(
	struct system_view *view,
	uint32_t request,
	uint32_t job)
{
	/* The next slot of the ring. */
	view->queued[view->queued_next].request = request;
	view->queued[view->queued_next].job = job;
	view->queued_next = (view->queued_next + 1U) % SYSTEM_VIEW_PRINT_QUEUED;
}

/*
 * Finds a print's job by its request: 1 with it, 0 when none is kept.
 */
int
system_view_print_job_of(
	const struct system_view *view,
	uint32_t request,
	uint32_t *job)
{
	unsigned index;

	/* Each kept. */
	for (index = 0; index < SYSTEM_VIEW_PRINT_QUEUED; index++) {
		if (view->queued[index].request == request && view->queued[index].job != 0U) {
			*job = view->queued[index].job;
			return 1;
		}
	}

	/* None. */
	return 0;
}

/*
 * Starts receiving an answer of the computer's query (ws188-p002): its
 * request and the parts it holds, whose pending copies start empty.  An
 * answer that was still open is dropped (it never came whole).
 */
void
system_view_machine_parts(
	struct system_view *view,
	uint32_t request,
	uint32_t what)
{
	/* The answer, open until its result, with nothing of it yet. */
	view->machine_open = 1U;
	view->machine_request = request;
	view->machine_parts = what & KL_SYSTEM_MACHINE_PARTS;
	memset(&view->machine_about_pending, 0, sizeof(view->machine_about_pending));
	view->machine_filesystems_pending_count = 0U;
	view->machine_users_pending_count = 0U;
	view->machine_language_pending[0] = '\0';
	view->machine_mounts_pending_count = 0U;
}

/*
 * Keeps the system's names of the answer being received.
 */
void
system_view_machine_about(
	struct system_view *view,
	const struct kl_machine_about *about)
{
	/* Only an open answer that holds them. */
	if (!view->machine_open || (view->machine_parts & KL_SYSTEM_MACHINE_ABOUT) == 0U)
		return;

	/* The names. */
	view->machine_about_pending = *about;
}

/*
 * Adds a file system to the answer being received, while there is room.
 */
void
system_view_machine_filesystem(
	struct system_view *view,
	const struct kl_machine_filesystem *filesystem)
{
	/* Only an open answer that holds them. */
	if (!view->machine_open || (view->machine_parts & KL_SYSTEM_MACHINE_FILESYSTEMS) == 0U)
		return;

	/* A file system more than the room is not kept. */
	if (view->machine_filesystems_pending_count >= KL_MACHINE_FILESYSTEMS_MAX)
		return;

	/* The file system. */
	view->machine_filesystems_pending[view->machine_filesystems_pending_count] = *filesystem;
	view->machine_filesystems_pending_count++;
}

/*
 * Adds an account to the answer being received, while there is room.
 */
void
system_view_machine_user(
	struct system_view *view,
	const struct kl_machine_user *user)
{
	/* Only an open answer that holds them. */
	if (!view->machine_open || (view->machine_parts & KL_SYSTEM_MACHINE_USERS) == 0U)
		return;

	/* An account more than the room is not kept. */
	if (view->machine_users_pending_count >= KL_MACHINE_USERS_MAX)
		return;

	/* The account. */
	view->machine_users_pending[view->machine_users_pending_count] = *user;
	view->machine_users_pending_count++;
}

/*
 * Keeps the login screen's language of the answer being received.
 */
void
system_view_machine_login_language(
	struct system_view *view,
	const char *code)
{
	/* Only an open answer that holds it. */
	if (!view->machine_open || (view->machine_parts & KL_SYSTEM_MACHINE_LOGIN_LANGUAGE) == 0U)
		return;

	/* The language's code. */
	system_view_copy(view->machine_language_pending, sizeof(view->machine_language_pending), code);
}

/*
 * Ends an answer of the computer's query with its result: an answer that
 * came whole (ok, for the request its parts named) puts its parts into
 * effect, each one's serial grows and the change is told; any other
 * result drops what was received.  Then the result waits to be taken like
 * every request's.
 */
void
system_view_machine_result(
	struct system_view *view,
	uint32_t request,
	uint32_t applied)
{
	unsigned parts;

	/* What the open answer holds, when it is this request's and came whole. */
	parts = 0U;
	if (view->machine_open &&
	    view->machine_request == request &&
	    applied == KL_SYSTEM_RESULT_OK)
		parts = view->machine_parts;
	view->machine_open = 0U;

	/* The system's names. */
	if ((parts & KL_SYSTEM_MACHINE_ABOUT) != 0U) {
		view->machine_about = view->machine_about_pending;
		view->machine_serials[0]++;
	}

	/* The file systems, as one list. */
	if ((parts & KL_SYSTEM_MACHINE_FILESYSTEMS) != 0U) {
		memcpy(view->machine_filesystems, view->machine_filesystems_pending, view->machine_filesystems_pending_count * sizeof(view->machine_filesystems[0]));
		view->machine_filesystem_count = view->machine_filesystems_pending_count;
		view->machine_serials[1]++;
	}

	/* The accounts, as one list. */
	if ((parts & KL_SYSTEM_MACHINE_USERS) != 0U) {
		memcpy(view->machine_users, view->machine_users_pending, view->machine_users_pending_count * sizeof(view->machine_users[0]));
		view->machine_user_count = view->machine_users_pending_count;
		view->machine_serials[2]++;
	}

	/* The login screen's language. */
	if ((parts & KL_SYSTEM_MACHINE_LOGIN_LANGUAGE) != 0U) {
		memcpy(view->machine_language, view->machine_language_pending, sizeof(view->machine_language));
		view->machine_serials[3]++;
	}

	/* The mounts, as one list (ws188-p004). */
	if ((parts & KL_SYSTEM_MACHINE_MOUNTS) != 0U) {
		memcpy(view->machine_mounts, view->machine_mounts_pending, view->machine_mounts_pending_count * sizeof(view->machine_mounts[0]));
		view->machine_mount_count = view->machine_mounts_pending_count;
		view->machine_serials[4]++;
	}

	/* The parts now known, and the change told. */
	if (parts != 0U) {
		view->machine_known |= parts;
		view->changed |= KL_SYSTEM_CHANGED_MACHINE;
	}

	/* The result, as every request's. */
	system_view_result(view, request, applied);
}

/*
 * Adds a mount to the answer being received, while there is room
 * (ws188-p004).
 */
void
system_view_machine_mount(
	struct system_view *view,
	const struct kl_machine_mount *mount)
{
	/* Only an open answer that holds them. */
	if (!view->machine_open || (view->machine_parts & KL_SYSTEM_MACHINE_MOUNTS) == 0U)
		return;

	/* A mount more than the room is not kept. */
	if (view->machine_mounts_pending_count >= KL_MACHINE_MOUNTS_MAX)
		return;

	/* The mount. */
	view->machine_mounts_pending[view->machine_mounts_pending_count] = *mount;
	view->machine_mounts_pending_count++;
}

/*
 * Takes Bluetooth's state into the pending copy (ws143-p006), starting a
 * new copy (the state and the devices) after the last done.
 */
void
system_view_bluetooth_state(
	struct system_view *view,
	const struct kl_bluetooth_state *state)
{
	/* The first after a done starts the copy. */
	if (!view->bluetooth_open) {
		view->bluetooth_open = 1U;
		view->bluetooth_pending_count = 0U;
	}

	/* The state. */
	view->bluetooth_pending = *state;
}

/*
 * Adds a Bluetooth device to the pending list (ws143-p006).
 */
void
system_view_bluetooth_device(
	struct system_view *view,
	const struct kl_bluetooth_device *device)
{
	/* The first after a done starts the copy (the state kept as it was). */
	if (!view->bluetooth_open) {
		view->bluetooth_open = 1U;
		view->bluetooth_pending = view->bluetooth;
		view->bluetooth_pending_count = 0U;
	}

	/* The device, while there is room. */
	if (view->bluetooth_pending_count >= KL_BLUETOOTH_DEVICES_MAX)
		return;
	view->bluetooth_devices_pending[view->bluetooth_pending_count] = *device;
	view->bluetooth_pending_count++;
}

/*
 * Puts the pending Bluetooth state and devices into effect (a done after
 * nothing keeps the state and empties the devices).
 */
void
system_view_bluetooth_done(
	struct system_view *view)
{
	/* A done after nothing: the same state, no devices. */
	if (!view->bluetooth_open) {
		view->bluetooth_pending = view->bluetooth;
		view->bluetooth_pending_count = 0U;
	}

	/* As one state. */
	view->bluetooth_open = 0U;
	view->bluetooth = view->bluetooth_pending;
	memcpy(view->bluetooth_devices, view->bluetooth_devices_pending, view->bluetooth_pending_count * sizeof(view->bluetooth_devices[0]));
	view->bluetooth_count = view->bluetooth_pending_count;
	view->changed |= KL_SYSTEM_CHANGED_BLUETOOTH;
}

/* Keeps a phone event of a kind alone (KL_PHONE_ITEMS, _LINK_CHANGED, _DROPPED) for kl_system_take_phone_event. */
static void
system_view_phone_mark(
	struct system_view *view,
	unsigned kind)
{
	struct kl_phone_event event;

	/* The event with its kind. */
	memset(&event, 0, sizeof(event));
	event.kind = kind;
	system_view_phone_event(view, &event);
}
