/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The poll event channel and file readiness scanning.
 *
 * Every readiness change in the kernel wakes one process-wide wait queue.
 * A poll waits on that channel with the sequence it observed before its
 * scan, so a change that races the scan is never missed, and it rescans
 * after each wakeup until a descriptor is ready or the deadline expires.
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

#include <uapi/errno.h>
#include <uapi/fcntl.h>

struct poll_channel {
	struct spinlock lock;
	struct wait_queue waitq;
};

static struct poll_channel channel;
static atomic_uint_t channel_ready;

static int poll_scan(struct process *process, struct pollfd *fds, nfds_t count, int *ready);
static int poll_wait_ready(struct process *process, struct pollfd *fds, nfds_t count, uint64_t deadline, int immediate, int *ready);

/*
 * Initializes the poll event channel.
 */
void
poll_init(
	void)
{
	/* Publishes the channel only after its lock and queue exist. */
	spin_init(&channel.lock, LOCK_RANK_POLL, "poll event channel");
	waitq_init(&channel.waitq, "poll event channel");
	atomic_store_release(&channel_ready, 1U);
}

/*
 * Wakes every poll after a readiness change.
 */
void
poll_notify(
	void)
{
	unsigned long irq;

	/* Ignores a change before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return;

	/* Wakes every waiting poll. */
	irq = spin_lock_irqsave(&channel.lock);

	waitq_wake_all(&channel.waitq);

	spin_unlock_irqrestore(&channel.lock, irq);
}

/*
 * Reads the channel sequence a poll must observe before scanning.
 */
uint64_t
poll_sequence(
	void)
{
	uint64_t sequence;

	/* Reports a placeholder before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return 0;

	/* Reads the wait queue sequence. */
	sequence = waitq_sequence(&channel.waitq);

	/* Reports the observed sequence. */
	return sequence;
}

/*
 * Sleeps on the channel until a readiness change, interruption, or
 * deadline.
 */
int
poll_wait(
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	unsigned long irq;
	int error;

	/* Refuses to wait before the channel exists. */
	if (atomic_load_acquire(&channel_ready) == 0)
		return EAGAIN;

	/* Sleeps under the channel lock. */
	irq = spin_lock_irqsave(&channel.lock);

	error = waitq_sleep(&channel.waitq, &channel.lock, observed, deadline, flags);

	spin_unlock_irqrestore(&channel.lock, irq);

	/* Reports why the sleep failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	struct thread *thread;
	uint64_t observed;
	int error;

	thread = thread_current();

	/* Scans, then sleeps for the next change, until something is ready. */
	observed = poll_sequence();
	for (;;) {
		/* Stops on a scan error, a ready descriptor, or an immediate poll. */
		error = poll_scan(process, fds, count, ready);
		if (error != 0 || *ready != 0 || immediate)
			return error;

		/* Surfaces a pending signal before sleeping. */
		if (thread != NULL && signal_pending_unblocked(thread))
			return EINTR;

		/* Sleeps until the channel changes. */
		error = poll_wait(observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			return EINTR;
		if (error == ETIMEDOUT) {
			*ready = 0;
			return 0;
		}

		if (error != 0 && error != EAGAIN)
			return error;

		/* Reports an expired deadline as no descriptor ready. */
		if (deadline != 0 && sched_ticks() >= deadline) {
			*ready = 0;
			return 0;
		}

		observed = poll_sequence();
	}
}

/* Scans every polled descriptor once and counts the ready ones. */
static int
poll_scan(
	struct process *process,
	struct pollfd *fds,
	nfds_t count,
	int *ready)
{
	struct fd_object object;
	nfds_t i;
	short revents;
	int total;
	int error;

	/* Count only descriptors whose requested or terminal readiness is observable. */
	total = 0;
	for (i = 0; i < count; i++) {
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
	}

	/* Publish the complete scan after every temporary object reference has been released. */
	*ready = total;

	/* Succeeded: the caller receives this scan's readiness count. */
	return 0;
}
