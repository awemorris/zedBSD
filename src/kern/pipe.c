/* Anonymous pipes. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/pipe.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/waitq.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

struct pipe {
	uint8_t data[KERN_PIPE_CAPACITY];
	size_t read_pos, write_pos, used;
	unsigned readers, writers;
	refcount_t endpoints;
	struct spinlock lock;
	struct wait_queue read_waitq;
	struct wait_queue write_waitq;
};

static atomic_uint_t pipe_live;

static struct pipe *
pipe_allocate(unsigned references)
{
	struct pipe *pipe = kern_calloc(1, sizeof(*pipe));
	if (pipe == NULL)
		return NULL;
	refcount_init(&pipe->endpoints, references);
	(void)atomic_fetch_add_relaxed(&pipe_live, 1U);
	spin_init(&pipe->lock, LOCK_RANK_FILE, "pipe");
	waitq_init(&pipe->read_waitq, "pipe readers");
	waitq_init(&pipe->write_waitq, "pipe writers");
	return pipe;
}

static void
pipe_release(struct pipe *pipe)
{
	if (pipe != NULL && refcount_put(&pipe->endpoints)) {
		(void)atomic_raw_fetch_add_relaxed(&pipe_live.value, (unsigned)-1);
		kern_free(pipe);
	}
}

static ssize_t
pipe_read_file(struct file *file, void *buffer, size_t length)
{
	struct pipe *pipe = file->f_data;
	uint8_t *out = buffer;
	size_t done = 0;
	unsigned long irq = spin_lock_irqsave(&pipe->lock);

	while (done < length) {
		while (pipe->used != 0 && done < length) {
			out[done++] = pipe->data[pipe->read_pos];
			pipe->read_pos = (pipe->read_pos + 1U) % KERN_PIPE_CAPACITY;
			pipe->used--;
		}
		waitq_wake_all(&pipe->write_waitq);
		poll_notify();
		if (done != 0 || pipe->writers == 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return (ssize_t)done;
		}
		if ((file->f_flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return -EAGAIN;
		}
		{
			uint64_t sequence = waitq_sequence(&pipe->read_waitq);
			int error = waitq_sleep(&pipe->read_waitq, &pipe->lock,
			    sequence, 0, WAITQ_INTERRUPTIBLE);
			if (error == EINTR) {
				spin_unlock_irqrestore(&pipe->lock, irq);
				return done != 0 ? (ssize_t)done : -EINTR;
			}
		}
	}
	spin_unlock_irqrestore(&pipe->lock, irq);
	return (ssize_t)done;
}

static ssize_t
pipe_write_file(struct file *file, const void *buffer, size_t length)
{
	struct pipe *pipe = file->f_data;
	const uint8_t *in = buffer;
	size_t done = 0;
	unsigned long irq = spin_lock_irqsave(&pipe->lock);

	while (done < length) {
		size_t free_space = KERN_PIPE_CAPACITY - pipe->used;
		if (pipe->readers == 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			if (curthread != NULL && curthread->proc != NULL)
				(void)signal_send_process(curthread->proc, SIGPIPE);
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
		waitq_wake_all(&pipe->read_waitq);
		poll_notify();
		if (done == length) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return (ssize_t)done;
		}
		if ((file->f_flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return done != 0 ? (ssize_t)done : -EAGAIN;
		}
		{
			uint64_t sequence = waitq_sequence(&pipe->write_waitq);
			int error = waitq_sleep(&pipe->write_waitq, &pipe->lock,
			    sequence, 0, WAITQ_INTERRUPTIBLE);
			if (error == EINTR) {
				spin_unlock_irqrestore(&pipe->lock, irq);
				return done != 0 ? (ssize_t)done : -EINTR;
			}
		}
	}
	spin_unlock_irqrestore(&pipe->lock, irq);
	return (ssize_t)done;
}

static int
pipe_close_file(struct file *file)
{
	struct pipe *pipe = file->f_data;
	unsigned long irq = spin_lock_irqsave(&pipe->lock);
	int destroy;

	if ((file->f_flags & O_ACCMODE) == O_RDONLY ||
	    (file->f_flags & O_ACCMODE) == O_RDWR) {
		if (pipe->readers != 0)
			pipe->readers--;
		waitq_wake_all(&pipe->write_waitq);
		poll_notify();
	}
	if ((file->f_flags & O_ACCMODE) == O_WRONLY ||
	    (file->f_flags & O_ACCMODE) == O_RDWR) {
		if (pipe->writers != 0)
			pipe->writers--;
		waitq_wake_all(&pipe->read_waitq);
		poll_notify();
	}
	if (pipe->readers == 0 && pipe->writers == 0)
		pipe->read_pos = pipe->write_pos = pipe->used = 0;
	destroy = refcount_put(&pipe->endpoints);
	spin_unlock_irqrestore(&pipe->lock, irq);
	if (destroy) {
		(void)atomic_raw_fetch_add_relaxed(&pipe_live.value, (unsigned)-1);
		kern_free(pipe);
	}
	return 0;
}

static int
pipe_poll_file(struct file *file, short events, short *revents)
{
	struct pipe *pipe = file != NULL ? file->f_data : NULL;
	short result = 0;
	unsigned long irq;

	if (pipe == NULL || revents == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&pipe->lock);
	if ((file->f_flags & O_ACCMODE) != O_WRONLY) {
		if (pipe->used != 0 || pipe->writers == 0)
			result |= events & (POLLIN | POLLRDNORM);
		if (pipe->writers == 0)
			result |= POLLHUP;
	}
	if ((file->f_flags & O_ACCMODE) != O_RDONLY) {
		if (pipe->readers == 0)
			result |= POLLERR;
		else if (pipe->used < KERN_PIPE_CAPACITY)
			result |= events & (POLLOUT | POLLWRNORM);
	}
	spin_unlock_irqrestore(&pipe->lock, irq);
	*revents = result;
	return 0;
}

static void
fifo_special_destroy(void *pointer)
{
	pipe_release(pointer);
}

static int
fifo_open_file(struct file *file)
{
	struct inode *inode = file != NULL ? file->f_inode : NULL;
	struct pipe *pipe, *candidate = NULL;
	unsigned long irq;
	int access, error = 0;
	if (inode == NULL || inode->i_type != INODE_FIFO)
		return EINVAL;
	access = file->f_flags & O_ACCMODE;
	if (access != O_RDONLY && access != O_WRONLY && access != O_RDWR)
		return EINVAL;
	for (;;) {
		mutex_lock(&inode->i_lock);
		pipe = inode->i_special;
		if (pipe != NULL) {
			refcount_get(&pipe->endpoints);
			mutex_unlock(&inode->i_lock);
			if (candidate != NULL)
				pipe_release(candidate);
			break;
		}
		mutex_unlock(&inode->i_lock);
		if (candidate == NULL) {
			candidate = pipe_allocate(1);
			if (candidate == NULL)
				return ENOMEM;
		}
		mutex_lock(&inode->i_lock);
		if (inode->i_special == NULL) {
			inode->i_special = candidate;
			inode->i_special_destroy = fifo_special_destroy;
			pipe = candidate;
			candidate = NULL;
			refcount_get(&pipe->endpoints);
			mutex_unlock(&inode->i_lock);
			break;
		}
		mutex_unlock(&inode->i_lock);
	}
	file->f_data = pipe;
	irq = spin_lock_irqsave(&pipe->lock);
	if (access == O_WRONLY && (file->f_flags & O_NONBLOCK) != 0 &&
	    pipe->readers == 0) {
		error = ENXIO;
		goto fail_locked;
	}
	if (access == O_RDONLY || access == O_RDWR)
		pipe->readers++;
	if (access == O_WRONLY || access == O_RDWR)
		pipe->writers++;
	waitq_wake_all(&pipe->read_waitq);
	waitq_wake_all(&pipe->write_waitq);
	while (access == O_RDONLY && (file->f_flags & O_NONBLOCK) == 0 &&
	    pipe->writers == 0) {
		uint64_t sequence = waitq_sequence(&pipe->read_waitq);
		error = waitq_sleep(&pipe->read_waitq, &pipe->lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			goto undo_locked;
	}
	while (access == O_WRONLY && pipe->readers == 0) {
		uint64_t sequence = waitq_sequence(&pipe->write_waitq);
		error = waitq_sleep(&pipe->write_waitq, &pipe->lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			goto undo_locked;
	}
	spin_unlock_irqrestore(&pipe->lock, irq);
	poll_notify();
	return 0;
undo_locked:
	if (access == O_RDONLY && pipe->readers != 0)
		pipe->readers--;
	if (access == O_WRONLY && pipe->writers != 0)
		pipe->writers--;
	waitq_wake_all(&pipe->read_waitq);
	waitq_wake_all(&pipe->write_waitq);
fail_locked:
	spin_unlock_irqrestore(&pipe->lock, irq);
	file->f_data = NULL;
	pipe_release(pipe);
	poll_notify();
	return error;
}

static const struct file_ops pipe_file_ops = {
	.read = pipe_read_file,
	.write = pipe_write_file,
	.poll = pipe_poll_file,
	.close = pipe_close_file,
};

const struct file_ops fifo_file_ops = {
	.open = fifo_open_file,
	.read = pipe_read_file,
	.write = pipe_write_file,
	.poll = pipe_poll_file,
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
	pipe = pipe_allocate(2);
	if (pipe == NULL)
		return ENOMEM;
	pipe->readers = pipe->writers = 1;
	error = file_create_pseudo(&pipe_file_ops,
	    O_RDONLY | (flags & O_NONBLOCK), pipe, read_file);
	if (error != 0) {
		pipe_release(pipe);
		pipe_release(pipe);
		return error;
	}
	error = file_create_pseudo(&pipe_file_ops,
	    O_WRONLY | (flags & O_NONBLOCK), pipe, write_file);
	if (error != 0) {
		pipe->writers = 0;
		pipe_release(pipe);
		(void)file_close(*read_file);
		return error;
	}
	return 0;
}

unsigned pipe_count(void)
{ return atomic_load_acquire(&pipe_live); }
