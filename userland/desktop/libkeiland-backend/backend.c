/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backend object every operating system shares (WS131 p003).
 *
 * It keeps the compositor's callbacks and options for the areas that
 * report through them (struct kl_backend is in backend-private.h, which the
 * operating systems' areas share).  The network and the sound are read by
 * the compositor's own updates, and the session's manager by the tick, as
 * the compositor read it before; the poll is the seat's (ws131-p006).
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

/*
 * The compositor's threads lent to the backend's own (a copy of the
 * options' threads).
 *
 * Written by kl_backend_open and kl_backend_close on the event loop's
 * thread, before any of the backend's threads starts and after the last
 * has been joined; read on the event loop's thread by the code that starts
 * them.  It means something only while backend_threads_set is 1.
 */
static struct kl_backend_threads backend_threads;

/*
 * Whether backend_threads holds the compositor's threads (1 from
 * kl_backend_open to kl_backend_close, 0 otherwise).
 */
static unsigned backend_threads_set;

/*
 * Opens the backend.
 *
 * The backend copies the callbacks and the options; the compositor's
 * structures need not outlive the call.
 */
int
kl_backend_open(
	const struct kl_backend_options *options,
	const struct kl_backend_host *host,
	struct kl_backend **backend)
{
	struct kl_backend *opened;

	/* Allocates the backend. */
	opened = malloc(sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;

	/* Keeps the compositor's callbacks and options. */
	memset(opened, 0, sizeof(*opened));
	opened->host = *host;
	opened->options = *options;

	/* The compositor's threads, for the threads the areas make of their own. */
	backend_threads = options->threads;
	backend_threads_set = 1U;

	/* The system's events, when the system has them (ws132-p003). */
	opened->events_descriptor = -1;
	kl_backend_events_open(opened);

	/* Succeeded: the compositor owns the backend until it closes it. */
	*backend = opened;
	return 0;
}

/*
 * Closes the backend.
 */
void
kl_backend_close(
	struct kl_backend *backend)
{
	/* A backend that did not open has nothing to close. */
	if (backend == NULL)
		return;

	/* The bus's notifications (ws156-p006), the system's events, then the backend. */
	kl_backend_notify_close(backend);
	kl_backend_events_close(backend);
	free(backend);

	/* The compositor's threads are no longer lent. */
	memset(&backend_threads, 0, sizeof(backend_threads));
	backend_threads_set = 0U;
}

/*
 * Gives the compositor's threads lent to the backend's own: those of the
 * open backend, or none (no start, no wake descriptor) when no backend is
 * open.
 */
void
kl_backend_threads(
	struct kl_backend_threads *threads)
{
	/* No backend is open: the threads are made with pthread_create and wake nothing. */
	if (!backend_threads_set) {
		memset(threads, 0, sizeof(*threads));
		threads->wake_descriptor = -1;
		return;
	}

	/* Succeeded: the open backend's copy. */
	*threads = backend_threads;
	return;
}

/*
 * Counts the descriptors the backend needs in the next poll.
 */
size_t
kl_backend_poll_count(
	const struct kl_backend *backend)
{
	size_t count;

	/* A compositor whose backend did not open polls nothing of it. */
	if (backend == NULL)
		return 0;

	/* The seat's service (ws131-p006), the system's events (ws132-p003), then the bus's notifications (ws156-p006). */
	count = kl_backend_seat_poll_count(backend);
	count += kl_backend_events_poll_count(backend);
	count += kl_backend_notify_poll_count(backend);
	return count;
}

/*
 * Fills the backend's poll descriptors.
 */
void
kl_backend_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	size_t seat;
	size_t events;

	/* A compositor whose backend did not open has nothing to fill. */
	if (backend == NULL)
		return;

	/* The seat's service (ws131-p006), the system's events after it (ws132-p003), then the bus's notifications (ws156-p006). */
	seat = kl_backend_seat_poll_count(backend);
	events = kl_backend_events_poll_count(backend);
	kl_backend_seat_poll_fill(backend, descriptors);
	kl_backend_events_poll_fill(backend, descriptors + seat);
	kl_backend_notify_poll_fill(backend, descriptors + seat + events);
}

/*
 * Handles what poll reported for the backend's descriptors.
 */
void
kl_backend_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	size_t seat;
	size_t events;

	/* A compositor whose backend did not open was told nothing. */
	if (backend == NULL)
		return;

	/*
	 * The seat's pauses and resumes (ws131-p006), the system's events after
	 * them (ws132-p003), then the bus's notifications (ws156-p006).  The
	 * counts are taken before any is handled: a handler may close its
	 * descriptor, and the places filled stay where they were.
	 */
	seat = kl_backend_seat_poll_count(backend);
	events = kl_backend_events_poll_count(backend);
	kl_backend_seat_poll_done(backend, descriptors);
	kl_backend_events_poll_done(backend, descriptors + seat);
	kl_backend_notify_poll_done(backend, descriptors + seat + events);
}

/*
 * Lets the backend do the work that waits for time.
 */
void
kl_backend_tick(
	struct kl_backend *backend,
	uint64_t now_ms)
{
	/* A compositor whose backend did not open has nothing to do here. */
	if (backend == NULL)
		return;

	/* The session manager's lines and the Log Out's deadline (ws131-p006). */
	kl_backend_session_tick(backend, now_ms);
}

/*
 * Tells what kl_backend_tick waits for: the session manager's descriptor
 * and the Log Out's deadline.
 */
int
kl_backend_wait(
	const struct kl_backend *backend,
	uint64_t *due_ms)
{
	int descriptor;

	/* A compositor whose backend did not open waits for nothing here. */
	*due_ms = UINT64_MAX;
	if (backend == NULL)
		return -1;

	/* Asks the session manager's descriptor and deadline. */
	descriptor = kl_backend_session_wait(backend, due_ms);

	/* The session manager's descriptor, or -1 when it has none. */
	return descriptor;
}
