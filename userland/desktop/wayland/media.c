/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media (ws132-p004).
 *
 * The compositor follows the volumes through libkeiland-backend on its event
 * loop's thread (the system extension's tick): the list goes to the devices
 * objects (system.c), and a volume that comes not mounted is told by a
 * notification of the compositor's own (WS156, H7 of
 * plan/ws156/phase001/phase.md: it replaces the bar's USB icon of
 * ws132-p004), "USB drive connected" with the volume's label, whose click
 * starts Files on its devices (files --devices).  The bar shows no media
 * icon any more.
 */

#include "media.h"
#include "glass.h"
#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* What Files is started with by a click on the notification. */
#define MEDIA_FILES		KEILAND_BINDIR "/files --devices"

/*
 * The one view of the media: the backend's following (opened on the first
 * tick) and the volumes of its last report.  Only the event loop's thread
 * touches it.
 */
struct media_view {
	unsigned opened;
	struct kl_backend_volumes *volumes;
	struct kl_backend_volume list[KL_BACKEND_VOLUMES_MAX];
	size_t count;
};

static int media_fresh(const char *id);

/* The view. */
static struct media_view media_view;

/*
 * Reads what the backend reported; a new volume not mounted yet is told by
 * a notification.  Returns the changed bits.
 */
unsigned
kwl_media_tick(
	struct kwl_server *server)
{
	struct kl_backend_volume list[KL_BACKEND_VOLUMES_MAX];
	const char *name;
	unsigned changed;
	unsigned fresh;
	size_t count;
	size_t index;
	int known;

	/* The following, once (the backend connects to volumed when it can). */
	if (!media_view.opened) {
		media_view.opened = 1U;
		media_view.volumes = kl_backend_volumes_open();
	}

	/* No following (no memory): no media. */
	if (media_view.volumes == NULL)
		return 0U;

	/* What arrived. */
	changed = 0U;
	(void)kl_backend_volumes_update(media_view.volumes, &changed);

	/* The list did not change. */
	if ((changed & KL_BACKEND_VOLUMES_CHANGED_LIST) == 0U)
		return changed;

	/* The new list; a fresh volume that was not fresh before is told, by its label (or its ID). */
	count = kl_backend_volumes_get(media_view.volumes, list, KL_BACKEND_VOLUMES_MAX);
	fresh = 0U;
	for (index = 0U; index < count; index++) {
		if (list[index].fresh == 0U || list[index].path[0] != '\0')
			continue;
		fresh++;
		known = media_fresh(list[index].id);
		if (known)
			continue;
		printf("KWL MEDIA new id=%s label=%s\n", list[index].id, list[index].label);
		name = list[index].label;
		if (name[0] == '\0')
			name = list[index].id;
		(void)kwl_notify_system_post(server, kl_tr("USB drive connected"), name, 0U, MEDIA_FILES);
	}

	/* Kept, and drawn. */
	memcpy(media_view.list, list, count * sizeof(list[0]));
	media_view.count = count;
	server->dirty = 1;
	printf("KWL MEDIA volumes=%zu fresh=%u\n", count, fresh);

	/* Succeeded: what changed. */
	return changed;
}

/*
 * Stops following volumed at the compositor's end: its connection's
 * thread ends.
 */
void
kwl_media_close(
	void)
{
	/* Only an open one. */
	if (media_view.volumes == NULL)
		return;
	kl_backend_volumes_close(media_view.volumes);
	media_view.volumes = NULL;
}

/*
 * Copies the volumes of the last report.
 */
size_t
kwl_media_volumes(
	struct kl_backend_volume *list,
	size_t capacity)
{
	size_t count;

	/* As many as fit. */
	count = media_view.count;
	if (count > capacity)
		count = capacity;
	memcpy(list, media_view.list, count * sizeof(list[0]));

	/* Succeeded. */
	return count;
}

/*
 * Asks for a volume to be mounted (mount 1) or ejected.  Returns 0 with the
 * backend's request number, or an errno value.
 */
int
kwl_media_ask(
	int mount,
	const char *id,
	uint32_t *request)
{
	int error;

	/* No following: nothing can be asked. */
	if (media_view.volumes == NULL)
		return ENOTCONN;

	/* The request. */
	if (mount) {
		error = kl_backend_volumes_mount(media_view.volumes, id, request);
	} else {
		error = kl_backend_volumes_eject(media_view.volumes, id, request);
	}

	/* Reports a request that could not be sent. */
	if (error != 0)
		return error;

	/* Succeeded: the answer comes as a result. */
	return 0;
}

/*
 * Takes the oldest answer of the backend; 1 with one, 0 without.
 */
int
kwl_media_take_result(
	uint32_t *request,
	int *error,
	char *user,
	size_t size)
{
	int taken;

	/* No following: no answer. */
	if (media_view.volumes == NULL)
		return 0;

	/* The answer. */
	taken = kl_backend_volumes_take_result(media_view.volumes, request, error, user, size);
	return taken;
}

/*
 * The bar's room for a media icon: none, the notification tells of a
 * volume (WS156 H7).
 */
int32_t
kwl_media_width(
	void)
{
	/* No icon. */
	return 0;
}

/* Draws the bar's media icon: there is none (WS156 H7). */
void
kwl_media_draw_icon(
	struct kwl_server *server,
	struct kwl_list *command,
	int32_t x,
	const float *ink)
{
	(void)server;
	(void)command;
	(void)x;
	(void)ink;
}

/* Takes a press on the bar's media icon: there is none, so never (WS156 H7). */
int
kwl_media_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	(void)server;
	(void)button;
	(void)state;

	/* Not the media's. */
	return 0;
}

/* Tells whether a volume was already fresh in the last report. */
static int
media_fresh(
	const char *id)
{
	size_t index;
	int same;

	/* Each volume of the last report. */
	for (index = 0U; index < media_view.count; index++) {
		same = strcmp(media_view.list[index].id, id);
		if (same == 0 && media_view.list[index].fresh != 0U)
			return 1;
	}

	/* Not before. */
	return 0;
}
