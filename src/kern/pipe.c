/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Anonymous pipes and FIFOs.
 *
 * A pipe is a ring buffer with reader and writer counts, guarded by a
 * spin lock, and two wait queues.  An anonymous pipe is created with both
 * ends open; a FIFO attaches one pipe to its inode on first open and
 * follows the POSIX open semantics for the missing side.
 */

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
#include <string.h>

struct pipe {
	uint8_t data[KERN_PIPE_CAPACITY];
	size_t read_pos;
	size_t write_pos;
	size_t used;
	unsigned readers;
	unsigned writers;
	refcount_t endpoints;
	struct spinlock lock;
	struct wait_queue read_waitq;
	struct wait_queue write_waitq;
};

static atomic_uint_t pipe_live;

static struct pipe *pipe_allocate(unsigned references);
static void pipe_release(struct pipe *pipe);
static ssize_t pipe_read_file(struct file *file, void *buffer, size_t length);
static ssize_t pipe_write_file(struct file *file, const void *buffer, size_t length);
static int pipe_close_file(struct file *file);
static int pipe_poll_file(struct file *file, short events, short *revents);
static void fifo_special_destroy(void *pointer);
static int fifo_open_file(struct file *file);

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

/*
 * Tests whether a file is a pipe or FIFO end.
 */
int
pipe_file_is_pipe(
	const struct file *file)
{
	/* A missing file is not a pipe. */
	if (file == NULL)
		return 0;

	/* Both anonymous pipes and FIFOs use the pipe operations. */
	if (file->f_ops == &pipe_file_ops)
		return 1;
	if (file->f_ops == &fifo_file_ops)
		return 1;

	/* Reports another kind of file. */
	return 0;
}

/*
 * Creates an anonymous pipe with a read end and a write end.
 *
 * Only O_NONBLOCK, O_CLOEXEC, and O_CLOFORK are accepted; the descriptor
 * flags are applied by the caller.
 */
int
pipe_create(
	int flags,
	struct file **read_file,
	struct file **write_file)
{
	struct pipe *pipe;
	int error;

	/* Rejects a missing result or an unsupported flag. */
	if (read_file == NULL ||
	    write_file == NULL ||
	    (flags & ~(O_NONBLOCK | O_CLOEXEC | O_CLOFORK)) != 0)
		return EINVAL;

	/* Allocates the pipe with one endpoint reference per end. */
	pipe = pipe_allocate(2);
	if (pipe == NULL)
		return ENOMEM;
	pipe->readers = 1;
	pipe->writers = 1;

	/* Creates the read end; failure drops both references. */
	error = file_create_pseudo(&pipe_file_ops,
	    O_RDONLY | (flags & O_NONBLOCK), pipe, read_file);
	if (error != 0) {
		pipe_release(pipe);
		pipe_release(pipe);
		return error;
	}

	/* Creates the write end; failure closes the read end. */
	error = file_create_pseudo(&pipe_file_ops,
	    O_WRONLY | (flags & O_NONBLOCK), pipe, write_file);
	if (error != 0) {
		pipe->writers = 0;
		pipe_release(pipe);
		(void)file_close(*read_file);
		return error;
	}

	/* Reports the created pipe. */
	return 0;
}

/*
 * Reports the number of live pipes.
 */
unsigned
pipe_count(
	void)
{
	unsigned count;

	count = atomic_load_acquire(&pipe_live);

	/* Reports the sampled count. */
	return count;
}

/* Allocates an empty pipe with a number of endpoint references. */
static struct pipe *
pipe_allocate(
	unsigned references)
{
	struct pipe *pipe;

	/* Allocates the ring and its bookkeeping. */
	pipe = kern_calloc(1, sizeof(*pipe));
	if (pipe == NULL)
		return NULL;
	refcount_init(&pipe->endpoints, references);
	(void)atomic_fetch_add_relaxed(&pipe_live, 1U);
	spin_init(&pipe->lock, LOCK_RANK_FILE, "pipe");
	waitq_init(&pipe->read_waitq, "pipe readers");
	waitq_init(&pipe->write_waitq, "pipe writers");

	/* Reports the new pipe. */
	return pipe;
}

/* Drops an endpoint reference and frees the pipe with the last one. */
static void
pipe_release(
	struct pipe *pipe)
{
	/* Frees the pipe when its last endpoint goes away. */
	if (pipe != NULL && refcount_put(&pipe->endpoints)) {
		(void)atomic_raw_fetch_add_relaxed(&pipe_live.value, (unsigned)-1);
		kern_free(pipe);
	}
}

/* Reads from a pipe, sleeping for data while writers remain. */
static ssize_t
pipe_read_file(
	struct file *file,
	void *buffer,
	size_t length)
{
	struct pipe *pipe;
	uint8_t *out;
	size_t done;
	size_t count;
	size_t contiguous;
	uint64_t sequence;
	unsigned long irq;
	int error;

	pipe = file->f_data;
	out = buffer;
	done = 0;

	irq = spin_lock_irqsave(&pipe->lock);

	/* Copies what is buffered, then waits for more or for the end. */
	while (done < length) {
		/* Drains the ring in at most two contiguous pieces. */
		while (pipe->used != 0 && done < length) {
			count = pipe->used;
			contiguous = KERN_PIPE_CAPACITY - pipe->read_pos;
			if (count > length - done)
				count = length - done;
			if (contiguous > count)
				contiguous = count;
			memcpy(out + done, pipe->data + pipe->read_pos, contiguous);
			if (contiguous < count)
				memcpy(out + done + contiguous, pipe->data,
				    count - contiguous);
			pipe->read_pos = (pipe->read_pos + count) %
			    KERN_PIPE_CAPACITY;
			pipe->used -= count;
			done += count;
		}
		waitq_wake_all(&pipe->write_waitq);
		poll_notify();

		/* Reports what was read, or end of file without writers. */
		if (done != 0 || pipe->writers == 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return (ssize_t)done;
		}

		/* A non-blocking read does not wait for data. */
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return -EAGAIN;
		}

		/* Sleeps until a writer adds data or closes. */
		sequence = waitq_sequence(&pipe->read_waitq);
		error = waitq_sleep(&pipe->read_waitq, &pipe->lock,
		    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -EINTR;
		}
	}
	spin_unlock_irqrestore(&pipe->lock, irq);

	/* Reports the bytes read. */
	return (ssize_t)done;
}

/* Writes to a pipe, sleeping for room while readers remain. */
static ssize_t
pipe_write_file(
	struct file *file,
	const void *buffer,
	size_t length)
{
	struct pipe *pipe;
	const uint8_t *in;
	size_t done;
	size_t free_space;
	size_t count;
	size_t contiguous;
	uint64_t sequence;
	unsigned long irq;
	int error;

	pipe = file->f_data;
	in = buffer;
	done = 0;

	irq = spin_lock_irqsave(&pipe->lock);

	/* Fills the ring, then waits for room, until everything is written. */
	while (done < length) {
		free_space = KERN_PIPE_CAPACITY - pipe->used;

		/* Writing without readers raises SIGPIPE and fails. */
		if (pipe->readers == 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			if (curthread != NULL)
				(void)signal_send_thread(curthread, SIGPIPE);
			if (done != 0)
				return (ssize_t)done;
			return -EPIPE;
		}

		/* A small write is atomic: it waits until it fits whole. */
		if (length <= KERN_PIPE_BUF && done == 0 && free_space < length)
			free_space = 0;

		/* Fills the ring in at most two contiguous pieces. */
		while (free_space != 0 && done < length) {
			count = free_space;
			contiguous = KERN_PIPE_CAPACITY - pipe->write_pos;
			if (count > length - done)
				count = length - done;
			if (contiguous > count)
				contiguous = count;
			memcpy(pipe->data + pipe->write_pos, in + done, contiguous);
			if (contiguous < count)
				memcpy(pipe->data, in + done + contiguous,
				    count - contiguous);
			pipe->write_pos = (pipe->write_pos + count) %
			    KERN_PIPE_CAPACITY;
			pipe->used += count;
			done += count;
			free_space -= count;
		}
		waitq_wake_all(&pipe->read_waitq);
		poll_notify();

		/* Reports a complete write. */
		if (done == length) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			return (ssize_t)done;
		}

		/* A non-blocking write does not wait for room. */
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -EAGAIN;
		}

		/* Sleeps until a reader makes room or closes. */
		sequence = waitq_sequence(&pipe->write_waitq);
		error = waitq_sleep(&pipe->write_waitq, &pipe->lock,
		    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&pipe->lock, irq);
			if (done != 0)
				return (ssize_t)done;
			return -EINTR;
		}
	}
	spin_unlock_irqrestore(&pipe->lock, irq);

	/* Reports the bytes written. */
	return (ssize_t)done;
}

/* Closes one end of a pipe and wakes the other side. */
static int
pipe_close_file(
	struct file *file)
{
	struct pipe *pipe;
	unsigned long irq;
	int destroy;

	pipe = file->f_data;

	irq = spin_lock_irqsave(&pipe->lock);

	/* Retires the reader side of the file. */
	if ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	    (file_status_flags_get(file) & O_ACCMODE) == O_RDWR) {
		if (pipe->readers != 0)
			pipe->readers--;
		waitq_wake_all(&pipe->write_waitq);
		poll_notify();
	}

	/* Retires the writer side of the file. */
	if ((file_status_flags_get(file) & O_ACCMODE) == O_WRONLY ||
	    (file_status_flags_get(file) & O_ACCMODE) == O_RDWR) {
		if (pipe->writers != 0)
			pipe->writers--;
		waitq_wake_all(&pipe->read_waitq);
		poll_notify();
	}

	/* Empties the ring once nobody is left on either side. */
	if (pipe->readers == 0 && pipe->writers == 0) {
		pipe->read_pos = 0;
		pipe->write_pos = 0;
		pipe->used = 0;
	}
	destroy = refcount_put(&pipe->endpoints);
	spin_unlock_irqrestore(&pipe->lock, irq);

	/* Frees the pipe with its last endpoint. */
	if (destroy) {
		(void)atomic_raw_fetch_add_relaxed(&pipe_live.value, (unsigned)-1);
		kern_free(pipe);
	}

	/* Reports the closed end. */
	return 0;
}

/* Reports the readiness of a pipe end. */
static int
pipe_poll_file(
	struct file *file,
	short events,
	short *revents)
{
	struct pipe *pipe;
	short result;
	unsigned long irq;

	pipe = NULL;
	if (file != NULL)
		pipe = file->f_data;
	result = 0;

	/* Rejects a missing pipe or result. */
	if (pipe == NULL || revents == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&pipe->lock);

	/* A readable end is ready with data or when the writers are gone. */
	if ((file_status_flags_get(file) & O_ACCMODE) != O_WRONLY) {
		if (pipe->used != 0 || pipe->writers == 0)
			result |= events & (POLLIN | POLLRDNORM);
		if (pipe->writers == 0)
			result |= POLLHUP;
	}

	/* A writable end is ready with room, or in error without readers. */
	if ((file_status_flags_get(file) & O_ACCMODE) != O_RDONLY) {
		if (pipe->readers == 0)
			result |= POLLERR;
		else if (pipe->used < KERN_PIPE_CAPACITY)
			result |= events & (POLLOUT | POLLWRNORM);
	}
	spin_unlock_irqrestore(&pipe->lock, irq);

	*revents = result;

	/* Reports the readiness. */
	return 0;
}

/* Releases the pipe attached to a FIFO inode when the inode goes away. */
static void
fifo_special_destroy(
	void *pointer)
{
	pipe_release(pointer);
}

/* Opens a FIFO, attaching a pipe to the inode and waiting for the peer. */
static int
fifo_open_file(
	struct file *file)
{
	struct inode *inode;
	struct pipe *pipe;
	struct pipe *candidate;
	uint64_t read_sequence;
	uint64_t write_sequence;
	unsigned long irq;
	int access;
	int error;

	inode = NULL;
	if (file != NULL)
		inode = file->f_inode;
	candidate = NULL;
	error = 0;

	/* Rejects anything but a FIFO opened with a valid access mode. */
	if (inode == NULL || inode->i_type != INODE_FIFO)
		return EINVAL;
	access = file_status_flags_get(file) & O_ACCMODE;
	if (access != O_RDONLY && access != O_WRONLY && access != O_RDWR)
		return EINVAL;

	/* Takes the inode's pipe, creating it when the FIFO is first opened. */
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

		/* Allocates a candidate outside the inode lock. */
		if (candidate == NULL) {
			candidate = pipe_allocate(1);
			if (candidate == NULL)
				return ENOMEM;
		}

		/* Attaches the candidate unless another opener won the race. */
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

	/* A non-blocking write open needs a reader already. */
	if (access == O_WRONLY &&
	    (file_status_flags_get(file) & O_NONBLOCK) != 0 &&
	    pipe->readers == 0) {
		error = ENXIO;
		goto fail_locked;
	}

	/* Joins the pipe and wakes the openers waiting for this side. */
	if (access == O_RDONLY || access == O_RDWR)
		pipe->readers++;
	if (access == O_WRONLY || access == O_RDWR)
		pipe->writers++;
	waitq_wake_all(&pipe->read_waitq);
	waitq_wake_all(&pipe->write_waitq);

	/* A blocking read open waits for a writer. */
	while (access == O_RDONLY &&
	    (file_status_flags_get(file) & O_NONBLOCK) == 0 &&
	    pipe->writers == 0) {
		read_sequence = waitq_sequence(&pipe->read_waitq);
		error = waitq_sleep(&pipe->read_waitq, &pipe->lock, read_sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			goto undo_locked;
	}

	/* A write open waits for a reader. */
	while (access == O_WRONLY && pipe->readers == 0) {
		write_sequence = waitq_sequence(&pipe->write_waitq);
		error = waitq_sleep(&pipe->write_waitq, &pipe->lock, write_sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EINTR)
			goto undo_locked;
	}
	spin_unlock_irqrestore(&pipe->lock, irq);
	poll_notify();

	/* Reports the opened FIFO. */
	return 0;

undo_locked:
	/* Leaves the pipe again after an interrupted wait. */
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

	/* Reports the failed open. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
