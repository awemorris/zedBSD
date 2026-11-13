/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A daemon's connection on a thread of its own (daemon-link-zedbsd.h):
 * the thread's loop, the operations handed to it, and its start and stop.
 */

#include "userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.h"

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The compositor's threads lent to the backend (backend.c).
 *
 * Weak because a host test of one daemon's connection builds this file
 * with that daemon's alone and no backend object: kl_backend_threads is
 * then NULL, and every link makes its thread with pthread_create and wakes
 * nothing.
 */
extern void kl_backend_threads(struct kl_backend_threads *threads) __attribute__((weak));

static void *link_run(void *argument);
static void link_ops_run(struct kl_daemon_link *link, struct kl_daemon_link_op *op);
static void link_ops_drop(struct kl_daemon_link_op *op);
static void link_pipe_drain(struct kl_daemon_link *link);
static void link_wake(struct kl_daemon_link *link);
static int link_thread_start(struct kl_daemon_link *link, const struct kl_backend_threads *threads);

/*
 * Starts a daemon's link: its thread runs ops's step at once (which makes
 * the first connection) and after every operation, wake and tick.
 * Returns 0, or an errno value with nothing started.
 */
int
kl_daemon_link_start(
	struct kl_daemon_link *link,
	const char *name,
	const struct kl_daemon_link_ops *ops,
	void *owner)
{
	struct kl_backend_threads threads;
	unsigned index;
	int error;

	/* The record, with no operation and nothing started. */
	memset(link, 0, sizeof(*link));
	link->ops = ops;
	link->owner = owner;
	(void)snprintf(link->name, sizeof(link->name), "%s", name);
	link->pipe[0] = -1;
	link->pipe[1] = -1;

	/* The compositor's wake pipe and thread maker, when a backend lends them. */
	memset(&threads, 0, sizeof(threads));
	threads.wake_descriptor = -1;
	if (kl_backend_threads != NULL)
		kl_backend_threads(&threads);
	link->wake = threads.wake_descriptor;

	/* The lock of the operations and of the published state. */
	error = pthread_mutex_init(&link->lock, NULL);
	if (error != 0)
		return error;

	/* The thread's own pipe; neither end blocks or goes to a program the compositor starts. */
	error = pipe(link->pipe);
	if (error != 0) {
		error = errno;
		(void)pthread_mutex_destroy(&link->lock);
		return error;
	}

	/* Sets both ends of the pipe to neither block nor be inherited. */
	for (index = 0U; index < 2U; index++) {
		(void)fcntl(link->pipe[index], F_SETFL, O_NONBLOCK);
		(void)fcntl(link->pipe[index], F_SETFD, FD_CLOEXEC);
	}

	/* The thread. */
	error = link_thread_start(link, &threads);
	if (error != 0) {
		(void)close(link->pipe[0]);
		(void)close(link->pipe[1]);
		(void)pthread_mutex_destroy(&link->lock);
		return error;
	}

	/* Succeeded: the thread runs the protocol until kl_daemon_link_stop. */
	return 0;
}

/*
 * Stops a daemon's link and waits for its thread to end: an operation it
 * runs finishes, the ones not taken yet are dropped.  The protocol's
 * connections are then the owner's to close, on the calling thread.
 */
void
kl_daemon_link_stop(
	struct kl_daemon_link *link)
{
	struct kl_daemon_link_op *left;
	static const unsigned char byte = 1U;

	/* Nothing started. */
	if (!link->started)
		return;

	/* Tells the thread to stop after its step, and wakes it from its wait. */
	kl_daemon_link_lock(link);

	link->stopping = 1U;

	kl_daemon_link_unlock(link);

	/* A full pipe already wakes the thread, so a failed write needs nothing more. */
	(void)write(link->pipe[1], &byte, 1U);

	/* The thread ends; its name leaves the compositor's register. */
	(void)pthread_join(link->thread, NULL);
	link->started = 0U;
	if (link->ended != NULL)
		link->ended(link->name);

	/* The operations given after the thread's last look are dropped. */
	left = link->head;
	link->head = NULL;
	link->tail = NULL;
	link_ops_drop(left);

	/* The pipe and the lock. */
	(void)close(link->pipe[0]);
	(void)close(link->pipe[1]);
	link->pipe[0] = -1;
	link->pipe[1] = -1;
	(void)pthread_mutex_destroy(&link->lock);

	/* Succeeded: no thread uses the owner any more. */
	return;
}

/*
 * Gives an operation to the link's thread, which runs it before its next
 * step; the operation is the link's from now on.
 */
void
kl_daemon_link_submit(
	struct kl_daemon_link *link,
	struct kl_daemon_link_op *op)
{
	static const unsigned char byte = 1U;
	unsigned poke;

	/* Appends the operation; the thread's pipe gets one byte per batch. */
	op->next = NULL;
	kl_daemon_link_lock(link);

	if (link->tail != NULL)
		link->tail->next = op;
	else
		link->head = op;
	link->tail = op;
	poke = 0U;
	if (!link->poked) {
		/* poked: the pipe holds a byte the thread has not drained, which already wakes it. */
		link->poked = 1U;
		poke = 1U;
	}

	kl_daemon_link_unlock(link);

	/* Wakes the thread from its wait (a full pipe wakes it already). */
	if (poke)
		(void)write(link->pipe[1], &byte, 1U);
}

/*
 * Tells whether the backend wakes the compositor by itself: zedBSD's
 * daemons are read on the links' threads, which write the wake pipe the
 * compositor lent, and its other descriptors are in the compositor's poll.
 */
int
kl_backend_wakes(
	const struct kl_backend *backend)
{
	struct kl_backend_threads threads;

	/* Without a backend nothing is read. */
	if (backend == NULL)
		return 1;

	/* The links wake only with the pipe the compositor lent. */
	memset(&threads, 0, sizeof(threads));
	threads.wake_descriptor = -1;
	if (kl_backend_threads != NULL)
		kl_backend_threads(&threads);
	return threads.wake_descriptor >= 0;
}

/*
 * Takes the link's lock (the operations and the published state); held
 * only to move or copy, never to wait.
 */
void
kl_daemon_link_lock(
	struct kl_daemon_link *link)
{
	/* The lock is never held across a wait, so this is short. */
	(void)pthread_mutex_lock(&link->lock);
}

/*
 * Gives the link's lock back.
 */
void
kl_daemon_link_unlock(
	struct kl_daemon_link *link)
{
	/* What was moved or copied is visible to the other thread from here on. */
	(void)pthread_mutex_unlock(&link->lock);
}

/* The link's thread: runs the operations given, steps the protocol, publishes, and waits. */
static void *
link_run(
	void *argument)
{
	struct pollfd descriptors[1U + KL_DAEMON_LINK_POLL_MAX];
	struct kl_daemon_link *link;
	struct kl_daemon_link_op *taken;
	unsigned stopping;
	size_t count;
	int timeout;
	int fresh;

	/* The link this thread runs. */
	link = argument;

	/* Until the link stops. */
	for (;;) {
		/* Takes the operations given since the last look, and whether to stop. */
		kl_daemon_link_lock(link);

		taken = link->head;
		link->head = NULL;
		link->tail = NULL;
		link->poked = 0U;
		stopping = link->stopping;

		kl_daemon_link_unlock(link);

		/* A stop drops what was given and ends the thread. */
		if (stopping) {
			link_ops_drop(taken);
			break;
		}

		/* The operations in the order given, then one step of the protocol. */
		link_ops_run(link, taken);
		link->ops->step(link->owner);

		/* Publishes what the step found to the event loop's side. */
		kl_daemon_link_lock(link);

		fresh = link->ops->publish(link->owner);

		kl_daemon_link_unlock(link);

		/* The event loop takes it at once. */
		if (fresh)
			link_wake(link);

		/* Waits for the connections, an operation or the stop, and the protocol's next timer when it has one. */
		descriptors[0].fd = link->pipe[0];
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		timeout = KL_DAEMON_LINK_NO_TIMER;
		count = link->ops->poll_fill(link->owner, descriptors + 1, KL_DAEMON_LINK_POLL_MAX, &timeout);
		if (timeout == KL_DAEMON_LINK_NO_TIMER)
			timeout = -1;
		(void)poll(descriptors, (nfds_t)(1U + count), timeout);

		/* The pipe's bytes are spent: the operations are taken at the top of the loop. */
		link_pipe_drain(link);
	}

	/* Succeeded: the link stopped. */
	return NULL;
}

/* Runs a list of operations in their order, freeing each. */
static void
link_ops_run(
	struct kl_daemon_link *link,
	struct kl_daemon_link_op *op)
{
	struct kl_daemon_link_op *next;

	/* Each operation, in the order the event loop gave them. */
	while (op != NULL) {
		next = op->next;
		op->run(link->owner, op);
		free(op);
		op = next;
	}
}

/* Drops a list of operations not run, freeing each. */
static void
link_ops_drop(
	struct kl_daemon_link_op *op)
{
	struct kl_daemon_link_op *next;

	/* Each operation, with what it owns. */
	while (op != NULL) {
		next = op->next;
		if (op->drop != NULL)
			op->drop(op);
		free(op);
		op = next;
	}
}

/* Empties the thread's own pipe (it never blocks). */
static void
link_pipe_drain(
	struct kl_daemon_link *link)
{
	unsigned char bytes[16];
	ssize_t got;

	/* Every byte waiting. */
	do {
		got = read(link->pipe[0], bytes, sizeof(bytes));
	} while (got > 0 || (got < 0 && errno == EINTR));
}

/* Writes one byte to the compositor's wake pipe, when there is one. */
static void
link_wake(
	struct kl_daemon_link *link)
{
	static const unsigned char byte = 1U;

	/* No wake pipe: the event loop's update finds the state on its own. */
	if (link->wake < 0)
		return;

	/* A full pipe already wakes the event loop, so a failed write needs nothing more. */
	(void)write(link->wake, &byte, 1U);
}

/* Starts the link's thread through the compositor's maker, or with every signal blocked; 0 or an errno value. */
static int
link_thread_start(
	struct kl_daemon_link *link,
	const struct kl_backend_threads *threads)
{
	sigset_t all;
	sigset_t previous;
	int error;

	/* The compositor's maker names the thread in its register and blocks the signals itself. */
	if (threads->start != NULL) {
		error = threads->start(link->name, link_run, link, &link->thread);
	} else {
		/* The new thread inherits a mask that blocks every signal; this thread's own mask is put back after. */
		(void)sigfillset(&all);
		(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
		error = pthread_create(&link->thread, NULL, link_run, link);
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	}

	/* Reports a thread that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: started tells the stop that a thread is to be joined, and ended who takes its name off. */
	link->started = 1U;
	link->ended = threads->ended;
	return 0;
}
