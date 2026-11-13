/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A daemon's connection on a thread of its own (zedBSD's audiod, networkd,
 * bluetoothd, the phone over bluetoothd and volumed).
 *
 * The thread owns the connections and everything the protocol keeps: it
 * connects, writes and reads, and may wait in any of them.  The event
 * loop's thread never touches them.  It hands its requests over as
 * operations, which the thread runs in the order given before its next
 * step, and it reads a copy of what the thread published, made under the
 * link's lock.  The lock is held only to move a list of operations or to
 * copy a published state; nobody waits while holding it.
 *
 * After each step the thread publishes, and when the publication holds
 * something new it writes one byte to the compositor's wake pipe, so the
 * event loop's next update finds it at once.  Between steps the thread
 * waits in poll for its connections, its own pipe (an operation given or
 * the stop) and the protocol's next timer, and for nothing else: with no
 * timer due it waits until one of them wakes it.
 */

#ifndef KL_BACKEND_DAEMON_LINK_H
#define KL_BACKEND_DAEMON_LINK_H

#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>

/* The most descriptors a link's step waits for, besides the link's own pipe. */
#define KL_DAEMON_LINK_POLL_MAX	8U

/*
 * The wait a protocol's poll_fill is given (milliseconds): no timer.  It
 * lowers it to its next timer; a wait left at this value has no end.
 */
#define KL_DAEMON_LINK_NO_TIMER	INT_MAX

/* The longest name of a link's thread, with its terminating zero. */
#define KL_DAEMON_LINK_NAME	16U

struct kl_daemon_link_op;

/*
 * What one daemon's protocol does on the link's thread.
 *
 * step moves the protocol: it reads what came, connects again when it is
 * time, writes what waits and keeps the timers.  poll_fill gives the
 * descriptors the next wait is to wake for (at most capacity) and lowers
 * *timeout (milliseconds, KL_DAEMON_LINK_NO_TIMER on entry) to its next
 * timer, so that every timer the step keeps is due when the wait ends
 * (step is called after every wait, whatever ended it).  publish runs
 * under the lock: it copies to the event loop's side what changed and moves
 * there what waits to be taken, and returns 1 when the event loop has
 * something new.  All three get the owner given to kl_daemon_link_start.
 */
struct kl_daemon_link_ops {
	void (*step)(void *owner);
	size_t (*poll_fill)(void *owner, struct pollfd *descriptors, size_t capacity, int *timeout);
	int (*publish)(void *owner);
};

/*
 * One request handed to the link's thread.
 *
 * The event loop allocates it (with malloc, as the first member of the
 * request's own structure) and gives it to kl_daemon_link_submit; from then
 * on it is the link's.  The thread calls run with the owner, then frees it;
 * an operation left when the link stops is given to drop instead (when
 * drop is not NULL), then freed.  run and drop free what the operation owns
 * beyond itself.
 */
struct kl_daemon_link_op {
	struct kl_daemon_link_op *next;
	void (*run)(void *owner, struct kl_daemon_link_op *op);
	void (*drop)(struct kl_daemon_link_op *op);
};

/*
 * One daemon's link: the thread (and the compositor's call that takes its
 * name off once it is joined), the operations given and not taken yet (a
 * list from head to tail), whether the thread's pipe already holds a byte
 * for them, whether the link is stopping, the pipe that wakes the thread,
 * and the compositor's wake pipe.
 *
 * The record lives inside the daemon's record from kl_daemon_link_start to
 * kl_daemon_link_stop.  lock guards head, tail, poked and stopping, and
 * whatever the owner's publish writes and its update reads.
 */
struct kl_daemon_link {
	const struct kl_daemon_link_ops *ops;
	void *owner;
	char name[KL_DAEMON_LINK_NAME];
	pthread_t thread;
	unsigned started;
	void (*ended)(const char *name);
	pthread_mutex_t lock;
	struct kl_daemon_link_op *head;
	struct kl_daemon_link_op *tail;
	unsigned poked;
	unsigned stopping;
	int pipe[2];
	int wake;
};

int kl_daemon_link_start(struct kl_daemon_link *link, const char *name, const struct kl_daemon_link_ops *ops, void *owner);
void kl_daemon_link_stop(struct kl_daemon_link *link);
void kl_daemon_link_submit(struct kl_daemon_link *link, struct kl_daemon_link_op *op);
void kl_daemon_link_lock(struct kl_daemon_link *link);
void kl_daemon_link_unlock(struct kl_daemon_link *link);

#endif
