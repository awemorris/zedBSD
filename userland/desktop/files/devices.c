/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable devices (ws132-p005, plan/ws132/phase004/phase.md).
 *
 * The desktop tells Files the USB sticks' volumes (main.c follows
 * libkeiland's kl_system_devices_*); they are shown in the sidebar's
 * Devices section and on the Today page.  A volume inserted and never
 * mounted since blinks three times when it comes (and again when Files is
 * started from the bar's media icon); a double click asks whether to mount
 * it (its name, size and file system, ws132-p009), and Mount mounts it
 * under /media and opens it; an eject unmounts it.  This file knows nothing
 * of libkeiland: it keeps the list, asks main.c through a request, and
 * says how bright a blinking device is.
 */

#include "files.h"

#include <stdio.h>
#include <string.h>

static const struct fm_device *devices_find(const struct fm_device *list, int count, const char *id);

/*
 * Takes the desktop's list of devices: a device new since the last list
 * (or every new one with blink_all) starts its blinks, and a device a
 * mount was asked for opens once it is mounted.  The sidebar is built
 * again.
 */
void
fm_devices_set(
	struct fm_app *app,
	const struct fm_device *list,
	int count,
	int blink_all)
{
	struct fm_device before[FM_DEVICES_MAX];
	struct fm_location location;
	const struct fm_device *old;
	struct fm_device *device;
	int before_count;
	int index;

	/* As many as are kept. */
	if (count > FM_DEVICES_MAX)
		count = FM_DEVICES_MAX;
	if (count < 0)
		count = 0;

	/* The list before, which the blinks are carried from. */
	before_count = app->places.device_count;
	if (before_count < 0 || before_count > FM_DEVICES_MAX)
		before_count = 0;
	memcpy(before, app->places.devices, (size_t)before_count * sizeof(before[0]));

	/* Each device: the blinks it had, or new ones when it is new and was not before (or all are asked). */
	for (index = 0; index < count; index++) {
		old = devices_find(before, before_count, list[index].id);
		app->places.devices[index] = list[index];
		device = &app->places.devices[index];
		device->blink_at = 0U;
		if (old != NULL)
			device->blink_at = old->blink_at;
		if (device->fresh && !device->mounted && (old == NULL || !old->fresh || blink_all))
			device->blink_at = app->now;
	}

	/* The list's length; its rows and cards are logged again when drawn. */
	app->places.device_count = count;
	app->device_rows_logged = 0;
	app->device_cards_logged = 0;

	/* The sidebar with them. */
	fm_places_init(&app->places, app->home);
	app->dirty = 1;
	fm_log("DEVICES count=%d", count);
	for (index = 0; index < count; index++) {
		fm_log("DEVICE id=%s name=%s mounted=%d new=%d path=%s blink=%d", app->places.devices[index].id, app->places.devices[index].name,
		    app->places.devices[index].mounted, app->places.devices[index].fresh, app->places.devices[index].path,
		    app->places.devices[index].blink_at != 0U);
	}

	/* The device a mount was asked for opens once it is mounted. */
	if (app->device_open[0] == '\0')
		return;
	old = devices_find(app->places.devices, count, app->device_open);
	if (old == NULL || !old->mounted)
		return;

	/* Its folder; it is opened once. */
	memset(&location, 0, sizeof(location));
	location.kind = FM_LOCATION_FOLDER;
	(void)snprintf(location.path, sizeof(location.path), "%s", old->path);
	app->device_open[0] = '\0';
	fm_log("DEVICE open id=%s path=%s", old->id, old->path);
	fm_ui_go(app, &location);
}

/*
 * Says how bright a device is drawn: 1 normally, less and back again over
 * each of its three blinks.
 */
float
fm_devices_blink(
	const struct fm_app *app,
	const struct fm_device *device)
{
	uint64_t elapsed;
	uint64_t phase;
	float half;

	/* Not blinking, or the blinks are over. */
	if (device->blink_at == 0U || app->now < device->blink_at)
		return 1.0f;
	elapsed = app->now - device->blink_at;
	if (elapsed >= FM_DEVICE_BLINK_MS)
		return 1.0f;

	/* Fading in the first half of a blink, back in the second. */
	phase = elapsed % FM_DEVICE_BLINK_PERIOD;
	half = (float)(FM_DEVICE_BLINK_PERIOD / 2U);
	if ((float)phase < half)
		return 1.0f - 0.8f * (float)phase / half;

	/* Coming back. */
	return 0.2f + 0.8f * ((float)phase - half) / half;
}

/*
 * Tells whether a device is blinking now (the main loop then draws again soon).
 */
int
fm_devices_blinking(
	const struct fm_app *app)
{
	const struct fm_device *device;
	int index;

	/* Each device. */
	for (index = 0; index < app->places.device_count; index++) {
		device = &app->places.devices[index];
		if (device->blink_at != 0U && app->now >= device->blink_at && app->now - device->blink_at < FM_DEVICE_BLINK_MS)
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Opens a device: a mounted one's folder at once; for one not mounted, the
 * question whether to mount it (ws132-p009: a medium is not mounted on a
 * double click alone).
 */
void
fm_devices_mount(
	struct fm_app *app,
	int index)
{
	struct fm_location location;
	const struct fm_device *device;

	/* A device of the list. */
	if (index < 0 || index >= app->places.device_count)
		return;
	device = &app->places.devices[index];

	/* Mounted already: its folder. */
	if (device->mounted) {
		memset(&location, 0, sizeof(location));
		location.kind = FM_LOCATION_FOLDER;
		(void)snprintf(location.path, sizeof(location.path), "%s", device->path);
		fm_ui_go(app, &location);
		return;
	}

	/* The question over the window, answered with Mount or Cancel (fm_devices_mount_answer). */
	(void)snprintf(app->device_confirm, sizeof(app->device_confirm), "%s", device->id);
	app->dialog = FM_DIALOG_MOUNT;
	app->dirty = 1;
	fm_log("DEVICE mount confirm id=%s fs=%s bytes=%llu", device->id, device->fs, (unsigned long long)device->bytes);
}

/*
 * Answers the question whether to mount a device: Mount asks for it through
 * the main loop (the device opens when the desktop says it is mounted),
 * Cancel leaves it as it is.  A device gone meanwhile is passed over, and
 * one mounted meanwhile opens.
 */
void
fm_devices_mount_answer(
	struct fm_app *app,
	int confirmed)
{
	struct fm_location location;
	const struct fm_device *device;

	/* The question is over. */
	app->dialog = FM_DIALOG_NONE;
	app->dirty = 1;
	fm_log("DEVICE mount answer id=%s confirmed=%d", app->device_confirm, confirmed);

	/* Cancelled, or the device has gone. */
	device = fm_devices_find(app, app->device_confirm);
	app->device_confirm[0] = '\0';
	if (confirmed == 0 || device == NULL)
		return;

	/* Mounted meanwhile: its folder. */
	if (device->mounted) {
		memset(&location, 0, sizeof(location));
		location.kind = FM_LOCATION_FOLDER;
		(void)snprintf(location.path, sizeof(location.path), "%s", device->path);
		fm_ui_go(app, &location);
		return;
	}

	/* Asked through the main loop, and opened when the desktop says it is mounted. */
	(void)snprintf(app->device_asked, sizeof(app->device_asked), "%s", device->id);
	(void)snprintf(app->device_open, sizeof(app->device_open), "%s", device->id);
	app->request = FM_REQUEST_DEVICE_MOUNT;
	fm_log("DEVICE mount asked id=%s", device->id);
}

/*
 * Finds a device of the list by its ID; NULL when it is not there.
 */
const struct fm_device *
fm_devices_find(
	const struct fm_app *app,
	const char *id)
{
	const struct fm_device *device;

	/* The list's device of that ID. */
	device = devices_find(app->places.devices, app->places.device_count, id);
	return device;
}

/*
 * Asks for a device to be ejected.
 */
void
fm_devices_eject(
	struct fm_app *app,
	int index)
{
	const struct fm_device *device;

	/* A device of the list. */
	if (index < 0 || index >= app->places.device_count)
		return;
	device = &app->places.devices[index];

	/* Asked through the main loop. */
	(void)snprintf(app->device_asked, sizeof(app->device_asked), "%s", device->id);
	app->request = FM_REQUEST_DEVICE_EJECT;
	fm_log("DEVICE eject asked id=%s", device->id);
}

/* Finds a device of a list by its ID. */
static const struct fm_device *
devices_find(
	const struct fm_device *list,
	int count,
	const char *id)
{
	int index;
	int same;

	/* Each device. */
	for (index = 0; index < count; index++) {
		same = strcmp(list[index].id, id);
		if (same == 0)
			return &list[index];
	}

	/* None. */
	return NULL;
}
