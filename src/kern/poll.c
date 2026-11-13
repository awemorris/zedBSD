/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The poll event channel and file readiness scanning.
 *
 * A readiness change names the object that changed (a pipe, a socket, a
 * device; poll_notify_object), and wakes only the polls that wait for that
 * object.  A poll learns the objects it waits for while it scans: each
 * file type's poll operation names the objects its answer reads
 * (poll_wait_object).  A descriptor whose operation names none, and a poll
 * that follows more objects than it can hold, are woken by every change, as
 * are all polls by a change that names no object (poll_notify).
 *
 * Every change counts the channel's sequence.  A poll observes the
 * sequence before its scan and sleeps only when it is unchanged, so a
 * change that races the scan is never missed; it rescans after each wakeup
 * until a descriptor is ready or the deadline expires.
 */

#include "kern/poll.h"
#include "kern/file.h"
#include "kern/handle.h"
#include "kern/filedesc.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/waitq.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>

/* The objects one poll follows by name; a poll with more is woken by every change. */
#define POLL_OBJECTS	16U

/*
 * One sleeping poll: its own wait queue, and the objects whose changes wake
 * it (every change when every is set).  It lives on the polling thread's
 * stack, linked on the channel only while the thread sleeps; noted tells
 * the scan whether the descriptor just asked named an object.
 */
struct poll_waiter {
	struct poll_waiter *next;
	struct wait_queue waitq;
	const void *objects[POLL_OBJECTS];
	unsigned count;
	unsigned every;
	unsigned noted;
};

/* The sleeping polls, and the count of the changes so far, under the lock. */
struct poll_channel {
	struct spinlock lock;
	struct poll_waiter *waiters;
	uint64_t sequence;
};

static struct poll_channel channel;
static atomic_uint_t channel_ready;

static int poll_scan(struct process *process, struct pollfd *fds, nfds_t count, struct poll_waiter *waiter, int *ready);
static int poll_follows(const struct poll_waiter *waiter, const void *object);
static uint64_t poll_sequence(void);
static int poll_sleep(struct poll_waiter *waiter, uint64_t observed, uint64_t deadline, unsigned flags);
static int poll_wait_ready(struct process *process, struct pollfd *fds, nfds_t count, uint64_t deadline, int immediate, int *ready);

/*
 * Initializes the poll event channel.
 */
void
poll_init(
	void)
{
	/* Publishes the channel only after its lock exists. */
	spin_init(&channel.lock, LOCK_RANK_POLL, "poll event channel");
	channel.waiters = NULL;
	channel.sequence = 0;
	atomic_store_release(&channel_ready, 1U);
}

/*
 * Wakes every poll after a readiness change that names no object.
 */
void
poll_notify(
	void)
{
	struct poll_waiter *waiter;
	unsigned long irq;

	/* Ignores a change before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return;

	/* Counts the change and wakes every sleeping poll. */
	irq = spin_lock_irqsave(&channel.lock);

	channel.sequence++;
	for (waiter = channel.waiters; waiter != NULL; waiter = waiter->next)
		waitq_wake_all(&waiter->waitq);

	spin_unlock_irqrestore(&channel.lock, irq);
}

/*
 * Wakes the polls that wait for one object after its readiness changed,
 * and those that wait for every change.
 */
void
poll_notify_object(
	const void *object)
{
	struct poll_waiter *waiter;
	unsigned long irq;

	/* Ignores a change before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return;

	/* Counts the change and wakes the polls that follow the object. */
	irq = spin_lock_irqsave(&channel.lock);

	channel.sequence++;
	for (waiter = channel.waiters; waiter != NULL; waiter = waiter->next) {
		if (waiter->every || poll_follows(waiter, object))
			waitq_wake_all(&waiter->waitq);
	}

	spin_unlock_irqrestore(&channel.lock, irq);
}

/*
 * Names an object whose changes are to wake the poll the calling thread is
 * scanning for.  A file type's poll operation calls it for every object its
 * answer reads, before it reads them; outside a poll's scan it does nothing.
 */
void
poll_wait_object(
	const void *object)
{
	struct poll_waiter *waiter;
	struct thread *thread;

	/* Only a thread scanning for a poll follows objects. */
	thread = thread_current();
	if (thread == NULL || thread->poll_waiter == NULL)
		return;
	waiter = thread->poll_waiter;

	/* The descriptor named an object. */
	waiter->noted = 1U;

	/* Each object once; one too many makes the poll follow every change. */
	if (object == NULL || poll_follows(waiter, object))
		return;
	if (waiter->count >= POLL_OBJECTS) {
		waiter->every = 1U;
		return;
	}
	waiter->objects[waiter->count] = object;
	waiter->count++;
}

/* Tells whether a poll follows an object by name. */
static int
poll_follows(
	const struct poll_waiter *waiter,
	const void *object)
{
	unsigned index;

	/* One of its objects. */
	for (index = 0; index < waiter->count; index++) {
		if (waiter->objects[index] == object)
			return 1;
	}

	/* Not followed by name. */
	return 0;
}

/*
 * Reads the channel sequence a poll must observe before scanning.
 */
static uint64_t
poll_sequence(
	void)
{
	unsigned long irq;
	uint64_t sequence;

	/* Read under the lock the changes count it under. */
	irq = spin_lock_irqsave(&channel.lock);

	sequence = channel.sequence;

	spin_unlock_irqrestore(&channel.lock, irq);

	/* Reports the observed sequence. */
	return sequence;
}

/*
 * Sleeps until a change of an object the poll follows, interruption, or
 * deadline; EAGAIN at once when a change came since the sequence was
 * observed (the scan may have missed it).
 */
static int
poll_sleep(
	struct poll_waiter *waiter,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct poll_waiter **link;
	unsigned long irq;
	int error;

	/* Refuses to wait before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return EAGAIN;

	/* A change since the scan began: the poll scans again. */
	irq = spin_lock_irqsave(&channel.lock);

	if (channel.sequence != observed) {
		spin_unlock_irqrestore(&channel.lock, irq);
		return EAGAIN;
	}

	/* Linked for the changes to find, asleep, then unlinked under the same lock. */
	waiter->next = channel.waiters;
	channel.waiters = waiter;
	error = waitq_sleep(&waiter->waitq, &channel.lock,
	    waitq_sequence(&waiter->waitq), deadline, flags);
	for (link = &channel.waiters; *link != NULL; link = &(*link)->next) {
		if (*link == waiter) {
			*link = waiter->next;
			break;
		}
	}
	waiter->next = NULL;

	spin_unlock_irqrestore(&channel.lock, irq);

	/* Reports why the sleep ended. */
	return error;
}

/*
 * Reports the readiness of one open file.
 *
 * A file type with its own poll operation answers for itself.  Regular
 * files, directories, and live block devices are always ready for the
 * accesses their open mode permits.
 */
int
file_poll(
	struct file *file,
	short events,
	short *revents)
{
	short result;
	int access;
	int error;

	result = 0;

	/* Rejects a missing file or result pointer. */
	if (file == NULL || revents == NULL)
		return EINVAL;

	/* Delegates to a file type that implements its own poll. */
	if (file->f_ops != NULL && file->f_ops->poll != NULL) {
		error = file->f_ops->poll(file, events, revents);
		return error;
	}

	/* Reports nothing for a file without an inode. */
	if (file->f_inode == NULL) {
		*revents = 0;
		return 0;
	}

	/* Derives readiness from the inode type and the open mode. */
	access = file_status_flags_get(file) & O_ACCMODE;
	switch (file->f_inode->i_type) {
	case INODE_REG:
		if (access != O_WRONLY)
			result |= events & (POLLIN | POLLRDNORM);
		if (access != O_RDONLY)
			result |= events & (POLLOUT | POLLWRNORM);
		break;
	case INODE_DIR:
		result |= events & (POLLIN | POLLRDNORM);
		break;
	case INODE_BLOCK:
		/* A detached block device reports an error instead. */
		if ((file->f_inode->i_flags & INODE_DEAD) != 0) {
			result |= POLLERR | POLLHUP;
			break;
		}

		if (access != O_WRONLY)
			result |= events & (POLLIN | POLLRDNORM);
		if (access != O_RDONLY)
			result |= events & (POLLOUT | POLLWRNORM);
		break;
	default:
		break;
	}

	*revents = result;

	/* Reports the derived readiness. */
	return 0;
}

/*
 * Waits until at least one polled descriptor is ready or the deadline
 * expires.
 *
 * An immediate poll scans once.  Otherwise the scan repeats after every
 * channel wakeup, and a pending signal ends the wait with EINTR.
 */
int
kern_poll_wait(
	struct process *process,
	struct pollfd *fds,
	nfds_t count,
	uint64_t deadline,
	int immediate,
	int *ready)
{
	struct filedesc *table;
	int error;

	/* Rejects a missing process, descriptor table, or result. */
	if (process == NULL ||
	    process->fd == NULL ||
	    fds == NULL ||
	    count > KERN_OPEN_MAX ||
	    ready == NULL)
		return EINVAL;

	/*
	 * Counts this poll on the table for its whole wait, so that changes to
	 * the table wake it; the table outlives the polling thread's process.
	 */
	table = process->fd;
	filedesc_poll_begin(table);
	error = poll_wait_ready(process, fds, count, deadline, immediate, ready);
	filedesc_poll_end(table);

	/* Reports the wait's result. */
	return error;
}

/* Scans and sleeps until a polled descriptor is ready or the wait ends. */
static int
poll_wait_ready(
	struct process *process,
	struct pollfd *fds,
	nfds_t count,
	uint64_t deadline,
	int immediate,
	int *ready)
{
	struct poll_waiter waiter;
	struct thread *thread;
	uint64_t observed;
	uint64_t now;
	int error;

	thread = thread_current();

	/* The poll's own queue, and no object followed yet. */
	kern_memset(&waiter, 0, sizeof(waiter));
	waitq_init(&waiter.waitq, "poll event channel");

	/* Scans, then sleeps for the next change it follows, until something is ready. */
	for (;;) {
		/* Stops on a scan error, a ready descriptor, or an immediate poll. */
		observed = poll_sequence();
		error = poll_scan(process, fds, count, &waiter, ready);
		if (error != 0 || *ready != 0 || immediate)
			return error;

		/* Surfaces a pending signal before sleeping. */
		if (thread != NULL && signal_pending_unblocked(thread))
			return EINTR;

		/* Sleeps until an object it follows changes. */
		error = poll_sleep(&waiter, observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			return EINTR;

		/*
		 * An expired deadline scans once more: a change that came as it
		 * passed (the system busy past it) is still reported (T1-129).
		 */
		if (error == ETIMEDOUT)
			return poll_scan(process, fds, count, NULL, ready);

		/* Another failure of the sleep ends the poll. */
		if (error != 0 && error != EAGAIN)
			return error;

		/* The same for a deadline that passed while the wake was on its way. */
		now = sched_ticks();
		if (deadline != 0 && now >= deadline)
			return poll_scan(process, fds, count, NULL, ready);
	}
}

/*
 * Scans every polled descriptor once and counts the ready ones.  With a
 * waiter, the scan also gathers the objects whose changes are to wake the
 * poll: each descriptor's poll operation names them; a descriptor that
 * names none makes the poll follow every change.
 */
static int
poll_scan(
	struct process *process,
	struct pollfd *fds,
	nfds_t count,
	struct poll_waiter *waiter,
	int *ready)
{
	struct fd_object object;
	struct thread *thread;
	nfds_t i;
	short revents;
	int total;
	int error;

	/* The objects are gathered afresh: what the descriptors read may have changed. */
	thread = thread_current();
	if (waiter != NULL) {
		waiter->count = 0;
		waiter->every = 0;
		if (thread == NULL)
			waiter->every = 1U;
		else
			thread->poll_waiter = waiter;
	}

	/* Count only descriptors whose requested or terminal readiness is observable. */
	total = 0;
	for (i = 0; i < count; i++) {
		if (waiter != NULL)
			waiter->noted = 0;
		/* Negative poll entries intentionally carry no descriptor ownership. */
		revents = 0;
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;

		/* A closed descriptor is ready with POLLNVAL. */
		error = filedesc_get_object_ref(process->fd, fds[i].fd, &object);
		if (error != 0) {
			fds[i].revents = POLLNVAL;
			total++;
			continue;
		}

		/* A release-only handle is valid but has no I/O or fence readiness. */
		error = 0;
		if (object.type == FD_OBJECT_FILE)
			error = file_poll(object.data.file, fds[i].events, &revents);

		/* Optional typed readiness never gives a handle ordinary file semantics. */
		if (object.type == FD_OBJECT_HANDLE && object.data.handle->ops->poll != NULL)
			error = object.data.handle->ops->poll(object.data.handle->object, fds[i].events, &revents);

		/* Either object can outlive concurrent close until this scan finishes. */
		(void)fd_object_put(&object);

		/* A failed query reports the descriptor as ready with POLLERR. */
		if (error == 0)
			fds[i].revents = revents;
		else
			fds[i].revents = POLLERR;

		/* Valid release-only handles contribute no readiness to this count. */
		if (fds[i].revents != 0)
			total++;

		/* A descriptor whose operation named no object is followed by every change. */
		if (waiter != NULL && !waiter->noted)
			waiter->every = 1U;
	}

	/* The scan names no more objects. */
	if (waiter != NULL && thread != NULL)
		thread->poll_waiter = NULL;

	/* Publish the complete scan after every temporary object reference has been released. */
	*ready = total;

	/* Succeeded: the caller receives this scan's readiness count. */
	return 0;
}
