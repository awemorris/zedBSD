/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD syslogd userland command.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LOG_SOCKET "/run/log"
#define LOG_PATH "/var/log/messages"
#define BOOT_LOG_PATH "/run/dmesg.boot"

/* The most kernel log text the boot log keeps (the dmesg command's limit). */
#define BOOT_LOG_LIMIT (1024U * 1024U)

/*
 * The kernel log kept on disk as it grows (BUG-158): the ring lives only in
 * memory, so a machine stopped by the power button kept no kernel line.  The
 * previous boot's file is kept beside the new one.
 */
#define KERNEL_LOG_PATH "/var/log/kernel.log"
#define KERNEL_LOG_OLD_PATH "/var/log/kernel.log.old"

/* How often the kernel log is followed and the logs are flushed, in seconds. */
#define KERNEL_LOG_INTERVAL_SECONDS 2

/*
 * The most message bytes held for the writer.  The socket's queue is only a
 * few messages deep, and a sender whose message does not fit waits or loses
 * it; while the disk is slow (a flush can take seconds) the receiver keeps
 * taking messages into memory up to this much.  A message past it is
 * dropped and counted, and the count is written to the log in its place.
 */
#define LOG_QUEUE_LIMIT (1024U * 1024U)

/*
 * The part of the kernel log already on disk.
 *
 * The ring drops its oldest bytes when full and counts them, so the bytes it
 * ever took are the dropped count plus what it holds; written is that sum at
 * the last copy.  The descriptor is -1 while the file cannot be written.
 */
struct kernel_log_follow {
	int descriptor;
	uint64_t written;
};

/* One received message waiting to be written, its newline included. */
struct log_message {
	struct log_message *next;
	size_t length;
	char text[];
};

/*
 * The messages the receiver has taken and the writer has not yet written.
 *
 * The receiver only appends; the writer takes the whole list at once and
 * writes it outside the lock, so a slow write or flush never keeps the
 * receiver from the socket.  Everything here is protected by lock.
 */
struct log_queue {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct log_message *head;
	struct log_message *tail;
	size_t bytes;
	uint64_t dropped;
	int reopen;
	int stop;
};

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reopening;

/* The queue between the receiving main thread and the writer thread. */
static struct log_queue queue;

static void save_boot_log(void);
static char *read_kernel_log(size_t *length);
static int write_all(int descriptor, const void *buffer, size_t length);
static int open_output(void);
static void handle_signal(int number);
static void kernel_log_open(struct kernel_log_follow *follow);
static void kernel_log_copy(struct kernel_log_follow *follow);
static int kernel_log_total(uint64_t *total);
static int kernel_log_dropped(uint64_t *dropped);
static time_t monotonic_seconds(void);
static int queue_start(pthread_t *writer, int output);
static void queue_add(const char *text, size_t length);
static void queue_signal(int stop, int reopen);
static void *writer_main(void *argument);
static void writer_take(time_t deadline, struct log_message **list, uint64_t *dropped, int *reopen, int *stop);
static int writer_write(int output, struct log_message *list, uint64_t dropped);

/*
 * Runs the syslogd command.
 *
 * The main thread receives the messages and hands them to a writer thread,
 * which writes them, follows the kernel log and flushes both files.
 */
int
main(
	int argc,
	char **argv)
{
	pthread_t writer;
	ssize_t length;
	struct sockaddr_un address;
	struct pollfd waiting;
	char message[2048];
	int socket_descriptor, output;
	int ready;
	int error;

	(void)argv;

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: syslogd\n");

		/* Reports operation failure. */
		return 2;
	}
	(void)signal(SIGHUP, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	save_boot_log();

	output = open_output();

	/* Handles the output condition. */
	if (output < 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_PATH, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	socket_descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);

	/* Handles the socket descriptor condition. */
	if (socket_descriptor < 0)
		return 1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, LOG_SOCKET);
	(void)unlink(LOG_SOCKET);

	/* Handles a failed bind operation. */
	if (bind(socket_descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0 ||
	    chmod(LOG_SOCKET, 0666) != 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_SOCKET,
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Starts the writer, which owns the log files from here on. */
	error = queue_start(&writer, output);
	if (error != 0) {
		fprintf(stderr, "syslogd: writer: %s\n", strerror(error));
		return 1;
	}

	/* Takes each message off the socket and queues it for the writer. */
	while (!stopping) {
		/* Passes a reopen request (SIGHUP) on to the writer. */
		if (reopening) {
			reopening = 0;
			queue_signal(0, 1);
		}

		/* Waits for a message; a signal ends the wait early. */
		waiting.fd = socket_descriptor;
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, KERNEL_LOG_INTERVAL_SECONDS * 1000);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			break;

		/* Nothing arrived within the interval. */
		if (ready == 0)
			continue;

		/* Receives one message. */
		length =
		    recv(socket_descriptor, message, sizeof(message) - 1, 0);
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0)
			break;
		message[length] = '\0';

		/* Refuses a message with a NUL inside, which no line can hold. */
		if (memchr(message, '\0', (size_t)length) != NULL)
			continue;

		/* Hands it to the writer. */
		queue_add(message, (size_t)length);
	}

	/* Has the writer write what is queued, copy the kernel's last lines and flush. */
	queue_signal(1, 0);
	(void)pthread_join(writer, NULL);
	close(socket_descriptor);
	unlink(LOG_SOCKET);

	/* Reports successful completion. */
	return 0;
}

/* Copies the kernel's messages so far to the boot log file. */
static void
save_boot_log(
	void)
{
	char *buffer;
	size_t length;
	int descriptor;

	/* Reads the whole kernel log; without it there is no boot log. */
	buffer = read_kernel_log(&length);
	if (buffer == NULL)
		return;

	/* Writes the boot log file and flushes it. */
	descriptor = open(BOOT_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (descriptor >= 0) {
		(void)write_all(descriptor, buffer, length);
		(void)fsync(descriptor);
		(void)close(descriptor);
	}

	free(buffer);
}

/*
 * Reads the kernel log into a buffer of its own size.
 *
 * The log may grow between sizing and reading it, so a read the log has
 * outgrown is sized and tried once more.  Returns the buffer, which the
 * caller frees, or NULL when the log cannot be read.
 */
static char *
read_kernel_log(
	size_t *length)
{
	char *buffer;
	size_t size;
	int attempt;
	int error;

	/* Tries the sized read at most twice. */
	buffer = NULL;
	for (attempt = 0; attempt < 2; attempt++) {
		/* Asks how long the log is now. */
		size = 0;
		error = sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0);
		if (error != 0)
			return NULL;

		/* Refuses a log beyond what the boot log keeps. */
		if (size > BOOT_LOG_LIMIT)
			return NULL;

		/* Allocates room for that length; an empty log still gets a byte. */
		buffer = malloc(size != 0 ? size : 1U);
		if (buffer == NULL)
			return NULL;

		/* Reads the log; a log that grew past the room is sized again. */
		error = sysctlbyname("kern.msgbuf", buffer, &size, NULL, 0);
		if (error == 0)
			break;

		free(buffer);
		buffer = NULL;

		/* Anything but a log that outgrew the room ends the attempt. */
		if (errno != ENOMEM)
			return NULL;
	}

	/* The log kept outgrowing the room. */
	if (buffer == NULL)
		return NULL;

	/* Succeeded: the buffer holds the whole log. */
	*length = size;
	return buffer;
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const void *buffer,
	size_t length)
{
	ssize_t written;
	const char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		written = write(descriptor, cursor, length);

		/* Handles the reported system error. */
		if (written < 0 && errno == EINTR)
			continue;

		/* Handles the written condition. */
		if (written <= 0)
			return -1;
		cursor += written;
		length -= (size_t)written;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the open output operation. */
static int
open_output(
	void)
{
	int function_result;

	/* Obtains the open result. */
	function_result = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the handle signal operation. */
static void
handle_signal(
	int number)
{
	/* Handles the number condition. */
	if (number == SIGHUP)
		reopening = 1;
	else
		stopping = 1;
}

/*
 * Opens the on-disk kernel log for this boot.
 *
 * The previous boot's file becomes the old one first, so the lines written
 * before a machine was stopped by the power button survive the next start.
 */
static void
kernel_log_open(
	struct kernel_log_follow *follow)
{
	int renamed;

	/* Nothing is on disk yet. */
	follow->descriptor = -1;
	follow->written = 0;

	/* Keeps the previous boot's file; a first boot has none. */
	renamed = rename(KERNEL_LOG_PATH, KERNEL_LOG_OLD_PATH);
	if (renamed != 0 && errno != ENOENT) {
		fprintf(stderr, "syslogd: %s: %s\n", KERNEL_LOG_OLD_PATH,
			strerror(errno));
	}

	/* Starts this boot's file. */
	follow->descriptor = open(KERNEL_LOG_PATH,
				  O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
				  0640);
	if (follow->descriptor < 0) {
		fprintf(stderr, "syslogd: %s: %s\n", KERNEL_LOG_PATH,
			strerror(errno));
	}
}

/*
 * Appends the kernel log's new bytes to the file and flushes it.
 *
 * Bytes the ring dropped before they could be copied are gone; a line in the
 * file says how many.
 */
static void
kernel_log_copy(
	struct kernel_log_follow *follow)
{
	char note[96];
	char *buffer;
	uint64_t dropped;
	uint64_t total;
	uint64_t fresh;
	uint64_t lost;
	size_t length;
	int error;
	int written;

	/* A file that could not be opened takes nothing. */
	if (follow->descriptor < 0)
		return;

	/* Asks how many bytes the ring ever took; nothing new needs no read. */
	error = kernel_log_total(&total);
	if (error != 0)
		return;
	if (total == follow->written)
		return;

	/*
	 * Reads the dropped count, then what the ring holds.  Bytes dropped
	 * between the two reads make the sum smaller than the truth, which
	 * copies a few bytes twice; the other order would skip some.
	 */
	error = kernel_log_dropped(&dropped);
	if (error != 0)
		return;
	buffer = read_kernel_log(&length);
	if (buffer == NULL)
		return;
	total = dropped + (uint64_t)length;

	/* Counts the bytes not yet on disk, and those the ring dropped first. */
	fresh = 0;
	if (total > follow->written)
		fresh = total - follow->written;
	lost = 0;
	if (fresh > (uint64_t)length) {
		lost = fresh - (uint64_t)length;
		fresh = (uint64_t)length;
	}

	/* Says how much of the kernel log never reached the file. */
	if (lost != 0) {
		(void)snprintf(note, sizeof(note),
			       "syslogd: %llu kernel log bytes were lost\n",
			       (unsigned long long)lost);
		(void)write_all(follow->descriptor, note, strlen(note));
	}

	/* Appends the newest bytes, which are at the end of the ring's copy. */
	written = write_all(follow->descriptor,
			    buffer + (length - (size_t)fresh),
			    (size_t)fresh);
	free(buffer);
	if (written != 0) {
		fprintf(stderr, "syslogd: %s: %s\n", KERNEL_LOG_PATH,
			strerror(errno));
		return;
	}

	/* Puts the copy on the disk before the next interval. */
	(void)fsync(follow->descriptor);
	follow->written = total;
}

/*
 * Reports how many bytes the kernel log ever took: dropped plus held.
 *
 * Returns 0, or -1 when the kernel does not answer.
 */
static int
kernel_log_total(
	uint64_t *total)
{
	uint64_t dropped;
	size_t size;
	int error;

	/* Asks how many bytes the ring dropped. */
	error = kernel_log_dropped(&dropped);
	if (error != 0)
		return -1;

	/* Asks how many bytes it holds. */
	size = 0;
	error = sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0);
	if (error != 0)
		return -1;

	/* Succeeded: the sum only grows while the dropped count can. */
	*total = dropped + (uint64_t)size;
	return 0;
}

/*
 * Reports how many bytes the kernel log dropped since the boot.
 *
 * Returns 0, or -1 when the kernel does not answer.
 */
static int
kernel_log_dropped(
	uint64_t *dropped)
{
	size_t size;
	int error;

	/* Asks the kernel for the count. */
	size = sizeof(*dropped);
	error = sysctlbyname("kern.msgbuf_dropped", dropped, &size, NULL, 0);
	if (error != 0)
		return -1;

	/* Succeeded: the count is filled in. */
	return 0;
}

/* Reports seconds on a clock that never steps back. */
static time_t
monotonic_seconds(
	void)
{
	struct timespec now;
	int error;

	/* Reads the monotonic clock. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: whole seconds are enough for the interval. */
	return now.tv_sec;
}

/*
 * Starts the writer thread over the empty queue.
 *
 * The thread is started with the signals blocked, so that they reach the
 * receiving main thread and end its wait.  Returns 0 or the error number.
 */
static int
queue_start(
	pthread_t *writer,
	int output)
{
	pthread_condattr_t attributes;
	sigset_t blocked;
	sigset_t previous;
	int error;

	/* Prepares the lock and the wakeup, the latter timed on the monotonic clock. */
	memset(&queue, 0, sizeof(queue));
	error = pthread_mutex_init(&queue.lock, NULL);
	if (error != 0)
		return error;
	error = pthread_condattr_init(&attributes);
	if (error != 0)
		return error;
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error != 0) {
		(void)pthread_condattr_destroy(&attributes);
		return error;
	}
	error = pthread_cond_init(&queue.wake, &attributes);
	(void)pthread_condattr_destroy(&attributes);
	if (error != 0)
		return error;

	/* Starts the writer with the signals blocked, then lets this thread take them again. */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGINT);
	(void)pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	error = pthread_create(writer, NULL, writer_main, (void *)(intptr_t)output);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the writer waits for messages. */
	return 0;
}

/*
 * Queues one message and its newline for the writer.
 *
 * A message that would take the queue past its limit, or that no memory can
 * hold, is dropped and counted; the writer says how many in the log.
 */
static void
queue_add(
	const char *text,
	size_t length)
{
	struct log_message *message;
	size_t size;
	int full;

	/* Decides under the lock whether the message fits; the count of drops lives there too. */
	size = length + 1U;
	pthread_mutex_lock(&queue.lock);

	full = 0;
	if (queue.bytes + size > LOG_QUEUE_LIMIT)
		full = 1;
	if (full)
		queue.dropped++;

	pthread_mutex_unlock(&queue.lock);

	/* A message past the limit is only counted. */
	if (full)
		return;

	/* Copies the message with its newline. */
	message = malloc(sizeof(*message) + size);
	if (message == NULL) {
		pthread_mutex_lock(&queue.lock);
		queue.dropped++;
		pthread_mutex_unlock(&queue.lock);
		return;
	}

	message->next = NULL;
	message->length = size;
	memcpy(message->text, text, length);
	message->text[length] = '\n';

	/* Appends it and wakes the writer. */
	pthread_mutex_lock(&queue.lock);

	if (queue.tail != NULL)
		queue.tail->next = message;
	else
		queue.head = message;
	queue.tail = message;
	queue.bytes += size;
	pthread_cond_signal(&queue.wake);

	pthread_mutex_unlock(&queue.lock);
}

/* Asks the writer to stop after what is queued, or to reopen the log file. */
static void
queue_signal(
	int stop,
	int reopen)
{
	/* Records the request where the writer looks for it and wakes the writer. */
	pthread_mutex_lock(&queue.lock);

	if (stop)
		queue.stop = 1;
	if (reopen)
		queue.reopen = 1;
	pthread_cond_signal(&queue.wake);

	pthread_mutex_unlock(&queue.lock);
}

/*
 * Writes the queued messages, follows the kernel log and flushes both files.
 *
 * Every interval the kernel log's new lines are copied and the files are
 * flushed; a flush on a slow disk holds only this thread.  On a stop the
 * remaining messages are written and both files flushed before it returns.
 */
static void *
writer_main(
	void *argument)
{
	struct kernel_log_follow follow;
	struct log_message *list;
	uint64_t dropped;
	time_t next_flush;
	time_t now;
	int output;
	int replacement;
	int output_dirty;
	int reopen;
	int stop;
	int failed;

	/* Starts the on-disk kernel log with everything the ring holds now. */
	output = (int)(intptr_t)argument;
	kernel_log_open(&follow);
	kernel_log_copy(&follow);
	next_flush = monotonic_seconds() + KERNEL_LOG_INTERVAL_SECONDS;
	output_dirty = 0;

	/* Writes each batch of messages until asked to stop. */
	for (;;) {
		/* Takes what the receiver queued, waiting until the next flush at most. */
		writer_take(next_flush, &list, &dropped, &reopen, &stop);

		/* Changes to a new log file on SIGHUP, keeping the old one if none opens. */
		if (reopen) {
			replacement = open_output();
			if (replacement >= 0) {
				(void)fsync(output);
				close(output);
				output = replacement;
				output_dirty = 0;
			}
		}

		/* Writes the batch; it waits for the next flush. */
		if (list != NULL || dropped != 0) {
			failed = writer_write(output, list, dropped);
			if (failed != 0) {
				fprintf(stderr, "syslogd: log write failed: %s\n",
					strerror(errno));
			}

			output_dirty = 1;
		}

		/* Copies new kernel lines and flushes both logs every interval, and at the stop. */
		now = monotonic_seconds();
		if (stop || now >= next_flush) {
			kernel_log_copy(&follow);
			if (output_dirty)
				(void)fsync(output);
			output_dirty = 0;
			next_flush = now + KERNEL_LOG_INTERVAL_SECONDS;
		}

		/* The last batch is written and flushed. */
		if (stop)
			break;
	}

	/* Closes both files. */
	if (follow.descriptor >= 0)
		(void)close(follow.descriptor);
	close(output);

	/* Succeeded: everything received is on the disk. */
	return NULL;
}

/*
 * Takes the queued messages, the count of those dropped and the requests,
 * waiting for any of them until the deadline (monotonic seconds).
 */
static void
writer_take(
	time_t deadline,
	struct log_message **list,
	uint64_t *dropped,
	int *reopen,
	int *stop)
{
	struct timespec until;
	int error;

	/* Waits for work or the next flush, then empties the queue. */
	until.tv_sec = deadline;
	until.tv_nsec = 0;
	pthread_mutex_lock(&queue.lock);

	while (queue.head == NULL &&
	       queue.dropped == 0 &&
	       !queue.reopen &&
	       !queue.stop) {
		error = pthread_cond_timedwait(&queue.wake, &queue.lock, &until);
		if (error == ETIMEDOUT)
			break;
	}

	*list = queue.head;
	*dropped = queue.dropped;
	*reopen = queue.reopen;
	*stop = queue.stop;
	queue.head = NULL;
	queue.tail = NULL;
	queue.bytes = 0;
	queue.dropped = 0;
	queue.reopen = 0;

	pthread_mutex_unlock(&queue.lock);
}

/*
 * Writes a count of dropped messages, then the messages, freeing each.
 *
 * Returns 0, or -1 when a write failed (the rest are still freed).
 */
static int
writer_write(
	int output,
	struct log_message *list,
	uint64_t dropped)
{
	struct log_message *next;
	char note[96];
	int failed;
	int written;

	/* Says how many messages the queue could not hold, where they would have been. */
	failed = 0;
	if (dropped != 0) {
		(void)snprintf(note, sizeof(note),
			       "syslogd: %llu messages were dropped while the log was behind\n",
			       (unsigned long long)dropped);
		written = write_all(output, note, strlen(note));
		if (written != 0)
			failed = -1;
	}

	/* Writes each message in the order received. */
	while (list != NULL) {
		next = list->next;
		if (failed == 0) {
			written = write_all(output, list->text, list->length);
			if (written != 0)
				failed = -1;
		}

		free(list);
		list = next;
	}

	/* Reports a write the file refused. */
	if (failed != 0)
		return -1;

	/* Succeeded: the messages are in the file. */
	return 0;
}
