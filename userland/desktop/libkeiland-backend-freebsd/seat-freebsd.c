/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat on FreeBSD: seatd through libseat (libkeiland-backend since
 * ws131-p006).
 *
 * The seat is the native service's (LIBSEAT_BACKEND=seatd; the noop
 * provider is never chosen): it activates the session, opens the primary
 * display node and each input device for the compositor (one kernel file
 * and one device ID each, kept together as a lease), and disables the
 * session while another has the display.  Disabled, nothing of the old
 * leases survives: the compositor stops drawing (session_paused), each
 * input is forgotten (input_gone), every lease is closed, and only then is
 * seatd told the compositor has let go.  Enabled again, the primary node is
 * opened afresh and the compositor opens its output and finds its inputs
 * again (session_resumed).
 *
 * One compositor runs one seat, so the state lives for the process.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <fcntl.h>
#include <libseat.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The leases: slot zero is the primary node's, the others the input devices' (the compositor reads at most 32, KWL_INPUT_MAX). */
#define SEAT_LEASE_MAX 33U

/* The longest device path kept. */
#define SEAT_PATH_MAX 4096U

/* The tries, and the milliseconds of each, of the first activation's wait. */
#define SEAT_ENABLE_TRIES 100
#define SEAT_ENABLE_MS 20

/*
 * One device: its kernel file and its seatd device ID, owned together
 * until closed, and its path (the compositor knows the input by it).
 */
struct seat_lease {
	int descriptor;
	int device;
	unsigned live;
	char path[SEAT_PATH_MAX];
};

/* The service's client, until the seat is closed. */
static struct libseat *seat_client;

/* libseat borrows the listener; it outlives every dispatch and the final close. */
static struct libseat_seat_listener seat_listener;

/* The leases. */
static struct seat_lease seat_leases[SEAT_LEASE_MAX];

/* A disabled seat admits no device request. */
static unsigned seat_paused = 1;

/* Callbacks drained by the final close change nothing. */
static unsigned seat_closing;

/* The callbacks are not passed on while the seat is being opened (the compositor asks for the state after). */
static unsigned seat_opening;

/* An activation reopens the primary node once it has been taken. */
static unsigned primary_requested;

/* A failure inside a libseat callback, reported to the compositor after the dispatch. */
static unsigned seat_failed;

/* The primary node's absolute path, fixed for the client's lifetime. */
static char primary_path[SEAT_PATH_MAX];

static int lease_open(unsigned slot, const char *path);
static void lease_close(unsigned slot);
static void seat_enable(struct libseat *seat, void *data);
static void seat_disable(struct libseat *seat, void *data);

/*
 * Connects to seatd, waits for the first activation and takes the primary
 * node.
 */
int
kl_backend_seat_open(
	struct kl_backend *backend)
{
	const char *requested;
	const char *selection;
	size_t length;
	int error;
	int attempt;
	int same;

	/* One seat per compositor, chosen explicitly: seatd or nothing. */
	if (backend == NULL)
		return EINVAL;
	if (seat_client != NULL)
		return EBUSY;
	selection = getenv("KEILAND_SEAT");
	if (selection != NULL) {
		same = strcmp(selection, "seatd");
		if (same != 0)
			return EINVAL;
	}

	/* The primary node the Vulkan inquiry will use too, absolute and whole. */
	requested = getenv("KEILAND_DRM_DEVICE");
	if (requested == NULL)
		requested = "/dev/dri/card0";
	length = strlen(requested);
	if (requested[0] != '/' || length >= sizeof(primary_path))
		return EINVAL;
	memcpy(primary_path, requested, length + 1);
	primary_requested = 0;
	seat_closing = 0;
	seat_failed = 0;
	seat_paused = 1;

	/* Only the seatd provider supplies the authority. */
	error = setenv("LIBSEAT_BACKEND", "seatd", 1);
	if (error != 0)
		return errno;

	/* The client; the listener and the backend outlive it. */
	seat_listener.enable_seat = seat_enable;
	seat_listener.disable_seat = seat_disable;
	seat_opening = 1;
	seat_client = libseat_open_seat(&seat_listener, backend);
	if (seat_client == NULL) {
		error = errno;
		seat_opening = 0;
		return error;
	}

	/* The first activation, within a bounded wait. */
	for (attempt = 0; attempt < SEAT_ENABLE_TRIES; attempt++) {
		error = libseat_dispatch(seat_client, SEAT_ENABLE_MS);
		if (error < 0) {
			error = errno;
			seat_opening = 0;
			return error;
		}
		if (seat_paused == 0)
			break;
	}
	seat_opening = 0;

	/* An inactive seat does not fall back to root access. */
	if (seat_paused != 0)
		return ETIMEDOUT;

	/* The primary node, before Vulkan opens its own inquiry file. */
	primary_requested = 1;
	error = lease_open(0, primary_path);
	if (error != 0)
		return error;

	/* Succeeded: seatd owns the activated session. */
	return 0;
}

/*
 * Closes every lease and the client.
 */
void
kl_backend_seat_close(
	struct kl_backend *backend)
{
	unsigned slot;

	/* No new device while the leases go back. */
	seat_paused = 1;
	primary_requested = 0;
	seat_closing = 1;

	/* Every lease, the partial ones too. */
	for (slot = 0; slot < SEAT_LEASE_MAX; slot++)
		lease_close(slot);

	/* The client, which may still drain callbacks (they change nothing now); the compositor is ending anyway. */
	(void)backend;
	if (seat_client != NULL) {
		(void)libseat_close_seat(seat_client);
		seat_client = NULL;
	}
}

/*
 * The primary node's descriptor.
 */
int
kl_backend_seat_primary_fd(
	const struct kl_backend *backend)
{
	/* No lease, no descriptor. */
	(void)backend;
	if (seat_leases[0].live == 0)
		return -1;
	return seat_leases[0].descriptor;
}

/*
 * The primary node's path.
 */
const char *
kl_backend_seat_primary_path(
	const struct kl_backend *backend)
{
	/* Fixed for the client's lifetime. */
	(void)backend;
	return primary_path;
}

/*
 * Tells whether the seat is disabled.
 */
int
kl_backend_seat_paused(
	const struct kl_backend *backend)
{
	/* A disabled seat admits no device. */
	(void)backend;
	if (seat_paused != 0)
		return 1;
	return 0;
}

/*
 * Opens an input device through seatd.
 */
int
kl_backend_seat_device_open(
	struct kl_backend *backend,
	const char *path)
{
	unsigned slot;
	int error;

	/* No device while disabled or without the service. */
	(void)backend;
	if (seat_client == NULL || seat_paused != 0) {
		errno = EAGAIN;
		return -1;
	}

	/* A free slot. */
	for (slot = 1; slot < SEAT_LEASE_MAX; slot++) {
		if (seat_leases[slot].live == 0)
			break;
	}
	if (slot == SEAT_LEASE_MAX) {
		errno = EMFILE;
		return -1;
	}

	/* The lease, with seatd's own error on refusal. */
	error = lease_open(slot, path);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: the compositor reads this descriptor. */
	return seat_leases[slot].descriptor;
}

/*
 * Closes the lease of an input descriptor.
 */
void
kl_backend_seat_device_close(
	struct kl_backend *backend,
	int descriptor)
{
	unsigned slot;

	/* Only a descriptor this seat published. */
	(void)backend;
	if (descriptor < 0)
		return;
	for (slot = 1; slot < SEAT_LEASE_MAX; slot++) {
		if (seat_leases[slot].live != 0 && seat_leases[slot].descriptor == descriptor) {
			lease_close(slot);
			return;
		}
	}
}

/*
 * Keeps no revoked device: seatd wants fresh opens after an activation.
 */
int
kl_backend_seat_device_revoked(
	struct kl_backend *backend,
	int descriptor)
{
	/* The compositor closes it as usual. */
	(void)backend;
	(void)descriptor;
	return 0;
}

/*
 * Counts seatd's descriptor.
 */
size_t
kl_backend_seat_poll_count(
	const struct kl_backend *backend)
{
	int descriptor;

	/* Polled while disabled too (the activation comes on it). */
	(void)backend;
	if (seat_client == NULL)
		return 0;
	descriptor = libseat_get_fd(seat_client);
	if (descriptor < 0)
		return 0;
	return 1;
}

/*
 * Fills seatd's descriptor.
 */
void
kl_backend_seat_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* The one counted above. */
	(void)backend;
	descriptors[0].fd = libseat_get_fd(seat_client);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
}

/*
 * Dispatches seatd's notifications (the callbacks below run inside it).
 */
void
kl_backend_seat_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	int error;

	/* Nothing was counted without the client. */
	if (seat_client == NULL)
		return;

	/* Dispatches what arrived and what synchronous requests left queued. */
	error = libseat_dispatch(seat_client, 0);

	/* A failed dispatch, a failed callback or a hung-up service ends the compositor. */
	if (error < 0 || seat_failed != 0 || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		seat_failed = 0;
		if (backend->host.session_stop != NULL)
			backend->host.session_stop(backend->host.data, KL_BACKEND_SESSION_LOST);
	}
}

/* Opens a device through seatd and keeps its file and its ID together. */
static int
lease_open(
	unsigned slot,
	const char *path)
{
	size_t length;
	int descriptor;
	int device;
	int flags;
	int error;

	/* A live lease is not replaced, and the path is kept whole. */
	if (seat_leases[slot].live != 0)
		return EBUSY;
	length = strlen(path);
	if (length >= sizeof(seat_leases[slot].path))
		return ENAMETOOLONG;

	/* seatd opens the kernel device and passes its descriptor. */
	descriptor = -1;
	device = libseat_open_device(seat_client, path, &descriptor);
	if (device < 0)
		return errno;

	/* Owned from here, so that every failure below returns both. */
	seat_leases[slot].descriptor = descriptor;
	seat_leases[slot].device = device;
	seat_leases[slot].live = 1;
	memcpy(seat_leases[slot].path, path, length + 1);

	/* The file does not block the compositor's loop. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0) {
		error = errno;
		lease_close(slot);
		return error;
	}
	error = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (error != 0) {
		error = errno;
		lease_close(slot);
		return error;
	}

	/* No program the compositor starts inherits it. */
	flags = fcntl(descriptor, F_GETFD);
	if (flags < 0) {
		error = errno;
		lease_close(slot);
		return error;
	}
	error = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
	if (error != 0) {
		error = errno;
		lease_close(slot);
		return error;
	}

	/* Succeeded: the lease is published. */
	return 0;
}

/* Closes a lease's file and returns its ID to seatd, once. */
static void
lease_close(
	unsigned slot)
{
	int descriptor;
	int device;
	int error;

	/* A lease is returned once. */
	if (seat_leases[slot].live == 0)
		return;

	/* Unpublished before anything else can see it. */
	descriptor = seat_leases[slot].descriptor;
	device = seat_leases[slot].device;
	seat_leases[slot].live = 0;
	seat_leases[slot].descriptor = -1;

	/* The kernel file, even without the service. */
	error = close(descriptor);
	if (error != 0)
		seat_failed = 1;

	/* The ID, to a service that still listens. */
	if (seat_client != NULL) {
		error = libseat_close_device(seat_client, device);
		if (error != 0)
			seat_failed = 1;
	}
}

/* seatd activates the session: the primary node again, then the compositor opens its output and finds its inputs. */
static void
seat_enable(
	struct libseat *seat,
	void *data)
{
	struct kl_backend *backend;
	int error;

	/* The final close drains callbacks that change nothing. */
	(void)seat;
	backend = data;
	if (seat_closing != 0)
		return;

	/* A later activation reopens the primary node the compositor had. */
	if (primary_requested != 0) {
		error = lease_open(0, primary_path);
		if (error != 0) {
			seat_failed = 1;
			return;
		}
	}

	/* Active; the first activation is reported by kl_backend_seat_open itself. */
	seat_paused = 0;
	if (seat_opening == 0 && backend->host.session_resumed != NULL)
		backend->host.session_resumed(backend->host.data);
}

/* seatd disables the session: the compositor stops, its inputs are forgotten, every lease closes, then seatd hears it. */
static void
seat_disable(
	struct libseat *seat,
	void *data)
{
	struct kl_backend *backend;
	unsigned slot;
	int error;

	/* The final close owns the leases' end. */
	backend = data;
	if (seat_closing != 0)
		return;

	/* No new device from now on. */
	seat_paused = 1;

	/* The compositor stops drawing and closes its output (Vulkan's duplicate of the primary node goes). */
	if (seat_opening == 0 && backend->host.session_paused != NULL)
		backend->host.session_paused(backend->host.data);

	/* Each input is forgotten by the compositor, then its lease closes. */
	for (slot = 1; slot < SEAT_LEASE_MAX; slot++) {
		if (seat_leases[slot].live == 0)
			continue;
		if (seat_opening == 0 && backend->host.input_gone != NULL)
			backend->host.input_gone(backend->host.data, seat_leases[slot].path);
		lease_close(slot);
	}

	/* The primary node last, after the output that used it. */
	lease_close(0);

	/* seatd may switch only after every file has gone. */
	error = libseat_disable_seat(seat);
	if (error != 0)
		seat_failed = 1;
}
