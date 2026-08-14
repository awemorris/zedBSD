/*
 * Anonymous pipes
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/pipe.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/signal.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>

struct pipe_waiter {
	struct thread *thread;
	struct pipe_waiter *next;
};

struct pipe {
	uint8_t data[KERN_PIPE_CAPACITY];
	size_t read_pos, write_pos, used;
	unsigned readers, writers;
	struct pipe_waiter *read_waiters, *write_waiters;
};

static void
pipe_wake_all(struct pipe_waiter *waiter)
{
	while (waiter != NULL) {
		sched_wakeup(waiter->thread);
		waiter = waiter->next;
	}
}

static void
pipe_wait(struct pipe_waiter **list, bool enable_after)
{
	struct pipe_waiter waiter;
	struct pipe_waiter **link;

	waiter.thread = curthread;
	waiter.next = *list;
	*list = &waiter;
	curthread->state = THREAD_SLEEPING;
	sched_yield();
	link = list;
	while (*link != NULL && *link != &waiter)
		link = &(*link)->next;
	if (*link == &waiter)
		*link = waiter.next;
	if (enable_after)
		hal_irq_enable();
}

static ssize_t
pipe_read_file(struct file *file, void *buffer, size_t length)
{
	struct pipe *pipe = file->f_data;
	size_t done = 0;
	uint8_t *out = buffer;

	while (done < length) {
		bool enabled = hal_irq_disable();
		while (pipe->used != 0 && done < length) {
			out[done++] = pipe->data[pipe->read_pos];
			pipe->read_pos = (pipe->read_pos + 1U) % KERN_PIPE_CAPACITY;
			pipe->used--;
		}
		pipe_wake_all(pipe->write_waiters);
		if (done != 0 || pipe->writers == 0) {
			if (enabled)
				hal_irq_enable();
			return (ssize_t)done;
		}
		if ((file->f_flags & O_NONBLOCK) != 0) {
			if (enabled)
				hal_irq_enable();
			return -EAGAIN;
		}
		if (signal_pending_unblocked(curthread)) {
			if (enabled)
				hal_irq_enable();
			return -EINTR;
		}
		pipe_wait(&pipe->read_waiters, enabled);
		if (signal_pending_unblocked(curthread))
			return -EINTR;
	}
	return (ssize_t)done;
}

static ssize_t
pipe_write_file(struct file *file, const void *buffer, size_t length)
{
	struct pipe *pipe = file->f_data;
	const uint8_t *in = buffer;
	size_t done = 0;

	while (done < length) {
		bool enabled = hal_irq_disable();
		size_t free_space = KERN_PIPE_CAPACITY - pipe->used;
		if (pipe->readers == 0) {
			if (curthread != NULL && curthread->proc != NULL)
				(void)signal_send_process(curthread->proc, SIGPIPE);
			if (enabled)
				hal_irq_enable();
			return done != 0 ? (ssize_t)done : -EPIPE;
		}
		if (length <= KERN_PIPE_BUF && done == 0 && free_space < length)
			free_space = 0;
		while (free_space != 0 && done < length) {
			pipe->data[pipe->write_pos] = in[done++];
			pipe->write_pos = (pipe->write_pos + 1U) % KERN_PIPE_CAPACITY;
			pipe->used++;
			free_space--;
		}
		pipe_wake_all(pipe->read_waiters);
		if (done == length) {
			if (enabled)
				hal_irq_enable();
			return (ssize_t)done;
		}
		if ((file->f_flags & O_NONBLOCK) != 0) {
			if (enabled)
				hal_irq_enable();
			return done != 0 ? (ssize_t)done : -EAGAIN;
		}
		if (signal_pending_unblocked(curthread)) {
			if (enabled)
				hal_irq_enable();
			return done != 0 ? (ssize_t)done : -EINTR;
		}
		pipe_wait(&pipe->write_waiters, enabled);
		if (signal_pending_unblocked(curthread))
			return done != 0 ? (ssize_t)done : -EINTR;
	}
	return (ssize_t)done;
}

static int
pipe_close_file(struct file *file)
{
	struct pipe *pipe = file->f_data;
	bool enabled = hal_irq_disable();

	if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
		if (pipe->readers != 0)
			pipe->readers--;
		pipe_wake_all(pipe->write_waiters);
	} else {
		if (pipe->writers != 0)
			pipe->writers--;
		pipe_wake_all(pipe->read_waiters);
	}
	if (pipe->readers == 0 && pipe->writers == 0)
		kern_free(pipe);
	if (enabled)
		hal_irq_enable();
	return 0;
}

static const struct file_ops pipe_file_ops = {
	.read = pipe_read_file,
	.write = pipe_write_file,
	.close = pipe_close_file,
};

int
pipe_create(int flags, struct file **read_file, struct file **write_file)
{
	struct pipe *pipe;
	int error;

	if (read_file == NULL || write_file == NULL ||
	    (flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0)
		return EINVAL;
	pipe = kern_calloc(1, sizeof(*pipe));
	if (pipe == NULL)
		return ENOMEM;
	pipe->readers = pipe->writers = 1;
	error = file_create_pseudo(&pipe_file_ops,
	    O_RDONLY | (flags & O_NONBLOCK), pipe, read_file);
	if (error != 0) {
		kern_free(pipe);
		return error;
	}
	error = file_create_pseudo(&pipe_file_ops,
	    O_WRONLY | (flags & O_NONBLOCK), pipe, write_file);
	if (error != 0) {
		pipe->writers = 0;
		(void)file_close(*read_file);
		return error;
	}
	return 0;
}
