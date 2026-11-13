/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * polltool: a guest check that poll(2) wakes for each change it waits for
 * (ws352: the kernel wakes only the polls that wait for the object that
 * changed).  Each case starts a thread that polls one descriptor with a
 * long timeout (2 s), makes the change from the main thread after 50 ms,
 * and checks that the poll returned with the expected event well before
 * its timeout (within 500 ms of the change):
 *   pipe-in      a byte written to the pipe
 *   pipe-out     a full pipe read by the other end
 *   pipe-hup     the write end closed
 *   stream-in    a byte sent on a unix stream pair
 *   stream-out   a full stream drained by the peer
 *   stream-eof   the peer's shutdown(SHUT_WR)
 *   stream-hup   the peer closed
 *   listen-in    a connection to a listening unix socket
 *   dgram-in     a datagram on a connected unix datagram pair
 *   dgram-out    a full datagram queue drained by the peer
 *   other        a poll of an idle pipe while another pipe changes 200
 *                times: it stays asleep until its own pipe is written
 * Prints one line a case and "polltool: PASS" or "polltool: FAIL n".
 *
 *   polltool [case...]
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS		2000
#define CHANGE_MS	50U
#define LATE_MS		500U

struct poller {
	int fd;
	short events;
	short revents;
	int result;
	uint64_t returned_us;
};

static int failures;

static uint64_t
now_us(void)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

static void
sleep_ms(
	unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = (time_t)(ms / 1000U);
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {
	}
}

static void *
poller_run(
	void *argument)
{
	struct poller *poller;
	struct pollfd entry;

	poller = argument;
	entry.fd = poller->fd;
	entry.events = poller->events;
	entry.revents = 0;
	poller->result = poll(&entry, 1, WAIT_MS);
	poller->revents = entry.revents;
	poller->returned_us = now_us();
	return NULL;
}

static int
nonblocking(
	int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Fills a descriptor that does not block until it takes no more. */
static void
fill(
	int fd,
	size_t chunk)
{
	char bytes[4096];
	ssize_t sent;

	memset(bytes, 'x', sizeof(bytes));
	if (chunk > sizeof(bytes))
		chunk = sizeof(bytes);
	for (;;) {
		sent = write(fd, bytes, chunk);
		if (sent <= 0)
			break;
	}
}

/* Reads what a descriptor that does not block has. */
static void
drain(
	int fd)
{
	char bytes[4096];
	ssize_t got;

	for (;;) {
		got = read(fd, bytes, sizeof(bytes));
		if (got <= 0)
			break;
	}
}

/*
 * Starts a poll of fd for events, makes the change after CHANGE_MS, and
 * checks the poll's return: the wanted bits, within LATE_MS of the change.
 */
static void
expect(
	const char *name,
	int fd,
	short events,
	short wanted,
	void (*change)(void *),
	void *context)
{
	struct poller poller;
	pthread_t thread;
	uint64_t changed_us;
	uint64_t late_us;
	int ok;

	memset(&poller, 0, sizeof(poller));
	poller.fd = fd;
	poller.events = events;
	if (pthread_create(&thread, NULL, poller_run, &poller) != 0) {
		printf("%s: FAIL thread\n", name);
		failures++;
		return;
	}
	sleep_ms(CHANGE_MS);
	changed_us = now_us();
	change(context);
	(void)pthread_join(thread, NULL);
	late_us = poller.returned_us > changed_us ? poller.returned_us - changed_us : 0U;
	ok = poller.result > 0 && (poller.revents & wanted) != 0 && late_us < (uint64_t)LATE_MS * 1000U;
	printf("%s: %s result=%d revents=0x%x after_us=%llu\n", name, ok ? "ok" : "FAIL", poller.result, (unsigned)poller.revents,
	    (unsigned long long)late_us);
	if (!ok)
		failures++;
}

static int pair[2];
static int other[2];

static void
change_write(
	void *context)
{
	(void)write(*(int *)context, "a", 1U);
}

static void
change_drain(
	void *context)
{
	drain(*(int *)context);
}

static void
change_close(
	void *context)
{
	(void)close(*(int *)context);
}

static void
change_shutdown(
	void *context)
{
	(void)shutdown(*(int *)context, SHUT_WR);
}

static void
change_connect(
	void *context)
{
	struct sockaddr_un address;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", (const char *)context);
	(void)connect(fd, (struct sockaddr *)&address, sizeof(address));
}

/* 200 writes and reads on another pipe, then the polled pipe's byte. */
static void
change_other(
	void *context)
{
	char byte;
	int index;

	for (index = 0; index < 200; index++) {
		(void)write(other[1], "b", 1U);
		(void)read(other[0], &byte, 1U);
	}
	(void)write(*(int *)context, "a", 1U);
}

static int
wanted(
	int count,
	char **names,
	const char *name)
{
	int index;

	if (count <= 1)
		return 1;
	for (index = 1; index < count; index++) {
		if (strcmp(names[index], name) == 0)
			return 1;
	}
	return 0;
}

int
main(
	int count,
	char **names)
{
	struct sockaddr_un address;
	char path[64];
	int listener;

	if (wanted(count, names, "pipe-in")) {
		(void)pipe(pair);
		expect("pipe-in", pair[0], POLLIN, POLLIN, change_write, &pair[1]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "pipe-out")) {
		(void)pipe(pair);
		(void)nonblocking(pair[0]);
		(void)nonblocking(pair[1]);
		fill(pair[1], 4096U);
		expect("pipe-out", pair[1], POLLOUT, POLLOUT, change_drain, &pair[0]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "pipe-hup")) {
		(void)pipe(pair);
		expect("pipe-hup", pair[0], POLLIN, POLLIN | POLLHUP, change_close, &pair[1]);
		(void)close(pair[0]);
	}
	if (wanted(count, names, "stream-in")) {
		(void)socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
		expect("stream-in", pair[0], POLLIN, POLLIN, change_write, &pair[1]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "stream-out")) {
		(void)socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
		(void)nonblocking(pair[0]);
		(void)nonblocking(pair[1]);
		fill(pair[1], 4096U);
		expect("stream-out", pair[1], POLLOUT, POLLOUT, change_drain, &pair[0]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "stream-eof")) {
		(void)socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
		expect("stream-eof", pair[0], POLLIN, POLLIN | POLLHUP, change_shutdown, &pair[1]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "stream-hup")) {
		(void)socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
		expect("stream-hup", pair[0], POLLIN, POLLIN | POLLHUP, change_close, &pair[1]);
		(void)close(pair[0]);
	}
	if (wanted(count, names, "listen-in")) {
		(void)snprintf(path, sizeof(path), "/tmp/polltool-%ld", (long)getpid());
		(void)unlink(path);
		listener = socket(AF_UNIX, SOCK_STREAM, 0);
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
		if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0) {
			printf("listen-in: FAIL bind errno=%d\n", errno);
			failures++;
		} else {
			expect("listen-in", listener, POLLIN, POLLIN, change_connect, path);
		}
		(void)close(listener);
		(void)unlink(path);
	}
	if (wanted(count, names, "dgram-in")) {
		(void)socketpair(AF_UNIX, SOCK_DGRAM, 0, pair);
		expect("dgram-in", pair[0], POLLIN, POLLIN, change_write, &pair[1]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "dgram-out")) {
		(void)socketpair(AF_UNIX, SOCK_DGRAM, 0, pair);
		(void)nonblocking(pair[0]);
		(void)nonblocking(pair[1]);
		fill(pair[1], 512U);
		expect("dgram-out", pair[1], POLLOUT, POLLOUT, change_drain, &pair[0]);
		(void)close(pair[0]);
		(void)close(pair[1]);
	}
	if (wanted(count, names, "other")) {
		(void)pipe(pair);
		(void)pipe(other);
		expect("other", pair[0], POLLIN, POLLIN, change_other, &pair[1]);
		(void)close(pair[0]);
		(void)close(pair[1]);
		(void)close(other[0]);
		(void)close(other[1]);
	}
	if (failures != 0) {
		printf("polltool: FAIL %d\n", failures);
		return 1;
	}
	printf("polltool: PASS\n");
	return 0;
}
