/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input devices on Linux and FreeBSD (libkeiland-backend since
 * ws131-p007): evdev nodes opened through the seat, with monotonic event
 * timestamps.
 */
#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-evdev.h"
#include <time.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The directory whose eventN nodes are the evdev devices. */
#define INPUT_DIRECTORY "/dev/input"

static int event_node_name(const char *name);
static void probe_device(struct kl_backend *backend, const char *path);
static int read_capabilities(int descriptor, struct kl_backend_input_caps *capabilities);

/*
 * Opens every evdev pointer and keyboard not already open.
 *
 * A missing directory leaves the seat without devices; it is not an error.
 * The event loop calls this periodically so late devices are picked up.
 */
void
kl_backend_input_scan(
	struct kl_backend *backend)
{
	DIR *directory;
	struct dirent *entry;
	char path[KL_BACKEND_INPUT_PATH_MAX];
	int length;
	int valid;
	int open_already;
	int paused;

	/* A service-paused seat cannot acquire newly discovered input devices. */
	paused = kl_backend_seat_paused(backend);
	if (paused != 0)
		return;

	/* Without the directory there is nothing to read. */
	directory = opendir(INPUT_DIRECTORY);
	if (directory == NULL)
		return;

	/* Every eventN entry is a candidate device. */
	while (1) {
		/* The end of the directory ends the scan. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only eventN nodes speak evdev. */
		valid = event_node_name(entry->d_name);
		if (!valid)
			continue;

		/* An overlong name cannot be an ordinary device node. */
		length = snprintf(path, sizeof(path), "%s/%s", INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;

		/* A node the compositor already reads is left alone. */
		open_already = 0;
		if (backend->host.input_known != NULL)
			open_already = backend->host.input_known(backend->host.data, path);
		if (open_already)
			continue;

		/* The compositor classifies the node and keeps it if it is a pointer or a keyboard. */
		probe_device(backend, path);
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: every present pointer and keyboard is open. */
	return;
}

/*
 * Reads an input axis range.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_absinfo(
	int descriptor,
	uint32_t axis,
	struct input_absinfo *info)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGABS(axis), info);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads an input device's name.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_name(
	int descriptor,
	char *name,
	size_t size)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGNAME(size), name);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads an input device's identity.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_id(
	int descriptor,
	struct input_id *id)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGID, id);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads whole input events without blocking.
 *
 * EOF returns zero; failures preserve read's errno, or use EIO for torn events.
 * logind can revoke the kernel file before its ordered bus notification
 * arrives: the read then fails with ENODEV, and the compositor asks
 * kl_backend_seat_device_revoked whether the seat keeps the device.
 */
ssize_t
kl_backend_input_read(
	int descriptor,
	struct input_event *events,
	size_t capacity)
{
	ssize_t bytes;

	/* Reads as many whole events as the buffer holds; ENODEV (revoked) is for the caller to ask the seat about. */
	bytes = read(descriptor, events, capacity * sizeof(events[0]));
	if (bytes < 0)
		return -1;

	/* Reports the end of the device. */
	if (bytes == 0)
		return 0;

	/* Refuses an incomplete evdev event. */
	if (((size_t)bytes % sizeof(events[0])) != 0) {
		errno = EIO;
		return -1;
	}

	/* Succeeded: returns the number of complete events. */
	return bytes / (ssize_t)sizeof(events[0]);
}

/*
 * Tells that the nodes are opened through the seat service, whose state is
 * the event loop's: the compositor scans on its own thread
 * (kl_backend_input_scan).
 */
int
kl_backend_input_find_supported(void)
{
	/* The seat opens the nodes. */
	return 0;
}

/*
 * Prepares a finding that opens nothing (the seat opens the nodes).
 */
void
kl_backend_input_find_begin(
	struct kl_backend *backend,
	struct kl_backend_input_find *find)
{
	(void)backend;

	/* Nothing is opened. */
	find->count = 0U;
	find->events_heard = 0U;
	find->refused_generation = 0U;
}

/*
 * Refuses a finding away from the event loop: the seat's devices are
 * opened by kl_backend_input_scan.
 */
int
kl_backend_input_find(
	struct kl_backend *backend,
	struct kl_backend_input_find *find)
{
	(void)backend;

	/* Nothing is opened here. */
	find->count = 0U;
	return ENOTSUP;
}

/*
 * Refuses the devices' thread: the seat's service tells the devices that
 * come, on the event loop's thread.
 */
int
kl_backend_devices_start(
	struct kl_backend *backend)
{
	(void)backend;

	/* No thread hears the devices here. */
	return ENOTSUP;
}

/*
 * Refuses a wait of the devices' thread, which never starts here.
 */
int
kl_backend_devices_wait(
	struct kl_backend *backend,
	struct kl_backend_device_batch *batch)
{
	(void)backend;

	/* Nothing is heard here. */
	memset(batch, 0, sizeof(*batch));
	return ENOTSUP;
}

/*
 * Takes nothing: no batch is made here.
 */
void
kl_backend_devices_take(
	struct kl_backend *backend,
	const struct kl_backend_device_batch *batch)
{
	(void)backend;
	(void)batch;

	/* Nothing to act on. */
	return;
}

/*
 * Stops nothing: the devices' thread never starts here.
 */
void
kl_backend_devices_stop(
	struct kl_backend *backend)
{
	(void)backend;

	/* Nothing started. */
	return;
}

/*
 * Cancels nothing: no wait of the devices' thread or for an open runs here.
 */
void
kl_backend_input_cancel(
	struct kl_backend *backend)
{
	(void)backend;

	/* Nothing waits. */
	return;
}

/*
 * Closes an input device descriptor.
 *
 * The seat module returns both descriptor and service ownership.
 */
void
kl_backend_input_close(
	struct kl_backend *backend,
	int descriptor)
{
	/* Releases the node through its seat owner. */
	kl_backend_seat_device_close(backend, descriptor);

	/* Succeeded: the descriptor is no longer owned by the seat. */
	return;
}

/*
 * Offers a device the seat has opened, with its capability bitmaps, to the
 * compositor; a node it does not keep goes back to the seat.
 */
void
kl_backend_input_offer(
	struct kl_backend *backend,
	int descriptor,
	const char *path)
{
	struct kl_backend_input_caps capabilities;
	int clock_id;
	int error;
	int kept;

	/* Matches the common compositor's monotonic clock before interpreting event timestamps. */
	clock_id = CLOCK_MONOTONIC;
	error = ioctl(descriptor, EVIOCSCLOCKID, &clock_id);
	if (error != 0) {
		kl_backend_seat_device_close(backend, descriptor);
		return;
	}

	/* Reads the bits that determine the device's role. */
	error = read_capabilities(descriptor, &capabilities);
	if (error != 0) {
		kl_backend_seat_device_close(backend, descriptor);
		return;
	}

	/* The compositor's classification keeps it, or its lease returns here. */
	kept = 0;
	if (backend->host.input_found != NULL)
		kept = backend->host.input_found(backend->host.data, descriptor, path, &capabilities);
	if (!kept)
		kl_backend_seat_device_close(backend, descriptor);
}

/* Reports whether a directory entry is named eventN. */
static int
event_node_name(
	const char *name)
{
	const char *cursor;
	int prefix;

	/* The name starts with "event" and has at least one more character. */
	prefix = strncmp(name, "event", 5);
	if (prefix != 0 || name[5] == '\0')
		return 0;

	/* Everything after the prefix is a decimal digit. */
	for (cursor = name + 5;
	     *cursor != '\0';
	     cursor++) {
		/* Any other character makes it some other kind of node. */
		if (*cursor < '0' || *cursor > '9')
			return 0;
	}

	/* Succeeded: the entry is an evdev node. */
	return 1;
}

/* Opens one node through the seat and offers it to the compositor. */
static void
probe_device(
	struct kl_backend *backend,
	const char *path)
{
	int descriptor;

	/* Opens a nonblocking descriptor that children cannot inherit; a seat that answers later offers it then (EINPROGRESS). */
	descriptor = kl_backend_seat_device_open(backend, path);
	if (descriptor < 0)
		return;

	/* The compositor classifies it now. */
	kl_backend_input_offer(backend, descriptor, path);
}

/* Reads the event, key, relative and absolute capability bitmaps and the properties of one node. */
static int
read_capabilities(
	int descriptor,
	struct kl_backend_input_caps *capabilities)
{
	int error;

	/* Absent bitmaps read as empty. */
	memset(capabilities, 0, sizeof(*capabilities));

	/* The event types the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(0, sizeof(capabilities->event)), capabilities->event);
	if (error < 0)
		return errno;

	/* The key and button codes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(capabilities->key)), capabilities->key);
	if (error < 0)
		return errno;

	/* The relative axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_REL, sizeof(capabilities->relative)), capabilities->relative);
	if (error < 0)
		return errno;

	/* The absolute axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(capabilities->absolute)), capabilities->absolute);
	if (error < 0)
		return errno;

	/*
	 * Its properties (WS217, decision D11: a touch pad is a pointer, a pen
	 * on the display is direct); a kernel without EVIOCGPROP leaves them
	 * empty, as the zedBSD backend does.
	 */
	error = ioctl(descriptor, EVIOCGPROP(sizeof(capabilities->properties)), capabilities->properties);
	if (error < 0)
		memset(capabilities->properties, 0, sizeof(capabilities->properties));

	/* Succeeded: the bitmaps describe the node. */
	return 0;
}
