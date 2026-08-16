/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/poll.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/waitq.h"

#include <errno.h>
#include <fcntl.h>

struct poll_channel {
	struct spinlock lock;
	struct wait_queue waitq;
};

static struct poll_channel channel;
static unsigned channel_ready;

void
poll_init(void)
{
	spin_init(&channel.lock, LOCK_RANK_POLL, "poll event channel");
	waitq_init(&channel.waitq, "poll event channel");
	__atomic_store_n(&channel_ready, 1U, __ATOMIC_RELEASE);
}

void
poll_notify(void)
{
	if (__atomic_load_n(&channel_ready, __ATOMIC_ACQUIRE) == 0)
		return;
	unsigned long irq = spin_lock_irqsave(&channel.lock);
	waitq_wake_all(&channel.waitq);
	spin_unlock_irqrestore(&channel.lock, irq);
}

uint64_t
poll_sequence(void)
{
	if (__atomic_load_n(&channel_ready, __ATOMIC_ACQUIRE) == 0)
		return 0;
	return waitq_sequence(&channel.waitq);
}

int
poll_wait(uint64_t observed, uint64_t deadline, unsigned flags)
{
	int error;
	if (__atomic_load_n(&channel_ready, __ATOMIC_ACQUIRE) == 0)
		return EAGAIN;
	unsigned long irq = spin_lock_irqsave(&channel.lock);
	error = waitq_sleep(&channel.waitq, &channel.lock, observed, deadline,
	    flags);
	spin_unlock_irqrestore(&channel.lock, irq);
	return error;
}

int
file_poll(struct file *file, short events, short *revents)
{
	short result = 0;
	int access;

	if (file == NULL || revents == NULL)
		return EINVAL;
	if (file->f_ops != NULL && file->f_ops->poll != NULL)
		return file->f_ops->poll(file, events, revents);
	if (file->f_inode == NULL) {
		*revents = 0;
		return 0;
	}
	access = file->f_flags & O_ACCMODE;
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
	return 0;
}

static int
poll_scan(struct process *process, struct pollfd *fds, nfds_t count,
	int *ready)
{
	nfds_t i;
	int total = 0;

	for (i = 0; i < count; i++) {
		struct file *file;
		short revents = 0;
		int error;
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;
		file = filedesc_get_ref(process->fd, fds[i].fd);
		if (file == NULL) {
			fds[i].revents = POLLNVAL;
			total++;
			continue;
		}
		error = file_poll(file, fds[i].events, &revents);
		(void)file_close(file);
		fds[i].revents = error == 0 ? revents : POLLERR;
		if (fds[i].revents != 0)
			total++;
	}
	*ready = total;
	return 0;
}

int
kern_poll_wait(struct process *process, struct pollfd *fds, nfds_t count,
	uint64_t deadline, int immediate, int *ready)
{
	struct thread *thread = thread_current();
	uint64_t observed;
	int error;

	if (process == NULL || process->fd == NULL || fds == NULL ||
	    count > KERN_OPEN_MAX || ready == NULL)
		return EINVAL;
	observed = poll_sequence();
	for (;;) {
		error = poll_scan(process, fds, count, ready);
		if (error != 0 || *ready != 0 || immediate)
			return error;
		if (thread != NULL && signal_pending_unblocked(thread))
			return EINTR;
		error = poll_wait(observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			return EINTR;
		if (error == ETIMEDOUT) {
			*ready = 0;
			return 0;
		}
		if (error != 0 && error != EAGAIN)
			return error;
		if (deadline != 0 && sched_ticks() >= deadline) {
			*ready = 0;
			return 0;
		}
		observed = poll_sequence();
	}
}
