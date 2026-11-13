/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the seat socket and the devices' thread's wait: sessiond's
 * seat.c and libkeiland-backend's backend.c, events-zedbsd.c and
 * input-zedbsd.c on the host.  It checks:
 *   - sessiond answers OPEN with FAIL errno for a name that is not an evdev
 *     node's, for a node that is not there, and for a compositor that does
 *     not hold the seat (EPERM); a request split over two writes is one;
 *     a socket whose compositor ended leaves the poll;
 *   - the backend's open through the seat socket writes OPEN eventN, takes
 *     FAIL's errno and keeps the socket, takes OK's descriptor (a pipe here,
 *     whose capabilities cannot be read: ENOTTY and the descriptor closed),
 *     and gives the socket up at its end (EPIPE, and EPIPE without asking
 *     after it); the cancel ends a wait for an answer that never comes;
 *   - the devices' wait (a pipe for /dev/system): an INPUT ADD tells the
 *     device came and opens it (ENOENT on the host), a FIDO node and a smart
 *     card change the keys, OVERFLOW asks for a finding, a USB device that
 *     is no card asks nothing, and the cancel ends the wait; the batch's
 *     take calls keys_changed and, for an overflow, input_changed.
 *
 *   sh plan/ws353/tests/seat-host.sh
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "userland/desktop/sessiond/sessiond.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uapi/system.h>

/* What the host's callbacks were told. */
struct told {
	int input;
	int keys;
};

/* One open through the seat socket on a thread of its own: the backend, the path, and what came of it. */
struct opening {
	struct kl_backend *backend;
	const char *path;
	struct kl_backend_input_node node;
	int error;
};

static void input_changed(void *data);
static void keys_changed(void *data);
static void check(int condition, const char *what);
static void serve(void);
static void serve_wait(void);
static int ask(int socket, const char *request, char *answer, size_t size);
static void post(int descriptor, uint32_t class_bit, uint32_t action, const char *subject, const char *detail);
static void *open_run(void *argument);
static struct kl_backend *backend_make(struct told *told);
static void sessiond_part(void);
static void backend_part(void);
static void devices_part(void);

/* The checks that failed, and those that ran. */
static int failures;
static int checks;

/* sessiond's log, which the test does not read. */
void
sessiond_log(
	const char *format,
	...)
{
	(void)format;
}

/* An input device came or went. */
static void
input_changed(
	void *data)
{
	struct told *told;

	/* Counted. */
	told = data;
	told->input++;
}

/* A security key or a smart card came or went. */
static void
keys_changed(
	void *data)
{
	struct told *told;

	/* Counted. */
	told = data;
	told->keys++;
}

/* Counts one check and says when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Each check. */
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* Answers what sessiond's seat sockets have waiting, as its loops do after a poll. */
static void
serve(void)
{
	struct pollfd entries[SESSIOND_SEAT_SOCKETS];
	nfds_t count;

	/* The sockets, looked at without waiting. */
	count = sessiond_seat_poll_fill(entries, SESSIOND_SEAT_SOCKETS);
	(void)poll(entries, count, 0);
	sessiond_seat_poll_done(entries, count);
}

/* Waits up to two seconds for a request on sessiond's seat sockets, then answers what came. */
static void
serve_wait(void)
{
	struct pollfd entries[SESSIOND_SEAT_SOCKETS];
	nfds_t count;

	/* The sockets, waited on. */
	count = sessiond_seat_poll_fill(entries, SESSIOND_SEAT_SOCKETS);
	(void)poll(entries, count, 2000);
	sessiond_seat_poll_done(entries, count);
}

/* Writes a request as a compositor would and reads sessiond's answer line: 0, or -1. */
static int
ask(
	int socket,
	const char *request,
	char *answer,
	size_t size)
{
	ssize_t length;

	/* The request, served, then the answer. */
	if (write(socket, request, strlen(request)) < 0)
		return -1;
	serve();
	length = read(socket, answer, size - 1U);
	if (length <= 0)
		return -1;
	answer[length] = '\0';

	/* Succeeded: the answer. */
	return 0;
}

/* Writes one system event record into the pipe that stands for /dev/system. */
static void
post(
	int descriptor,
	uint32_t class_bit,
	uint32_t action,
	const char *subject,
	const char *detail)
{
	struct system_event event;
	ssize_t written;

	/* The record. */
	memset(&event, 0, sizeof(event));
	event.class_bit = class_bit;
	event.action = action;
	snprintf(event.subject, sizeof(event.subject), "%s", subject);
	snprintf(event.detail, sizeof(event.detail), "%s", detail);
	written = write(descriptor, &event, sizeof(event));
	check(written == (ssize_t)sizeof(event), "a record written");
}

/* Opens a node through the backend, on a thread of its own. */
static void *
open_run(
	void *argument)
{
	struct opening *opening;

	/* The open, which waits for the answer. */
	opening = argument;
	opening->error = kl_backend_input_open(opening->backend, opening->path, &opening->node);
	return NULL;
}

/* Opens a backend with the two callbacks (no /dev/system on the host). */
static struct kl_backend *
backend_make(
	struct told *told)
{
	struct kl_backend_options options;
	struct kl_backend_host host;
	struct kl_backend *backend;
	int error;

	/* The options and the callbacks. */
	memset(&options, 0, sizeof(options));
	options.greeter_descriptor = -1;
	options.session_descriptor = -1;
	options.seat_descriptor = -1;
	memset(&host, 0, sizeof(host));
	host.data = told;
	host.input_changed = input_changed;
	host.keys_changed = keys_changed;
	backend = NULL;
	error = kl_backend_open(&options, &host, &backend);
	check(error == 0, "the backend opens");
	check(backend != NULL && backend->cancel[0] >= 0, "the cancel pipe is made");

	/* The backend. */
	return backend;
}

/* sessiond's side: the answers and the holder. */
static void
sessiond_part(void)
{
	char answer[64];
	int first_own;
	int first;
	int second_own;
	int second;
	int error;
	struct pollfd entries[SESSIOND_SEAT_SOCKETS];
	nfds_t count;

	/* Two compositors' sockets; the first holds the seat. */
	error = sessiond_seat_socket(&first_own, &first);
	check(error == 0, "the first seat socket");
	error = sessiond_seat_socket(&second_own, &second);
	check(error == 0, "the second seat socket");
	sessiond_seat_hold(first_own);
	count = sessiond_seat_poll_fill(entries, SESSIOND_SEAT_SOCKETS);
	check(count == 2, "both sockets are polled");

	/* Names that are not an evdev node's. */
	check(ask(first, "OPEN ../input/event0\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 22\n") == 0, "a path is refused");
	check(ask(first, "OPEN event12345\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 22\n") == 0, "five digits are refused");
	check(ask(first, "OPEN event\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 22\n") == 0, "no digit is refused");
	check(ask(first, "CLOSE event1\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 22\n") == 0, "another request is refused");

	/* A node that is not there, asked in two writes. */
	check(write(first, "OPEN eve", 8U) == 8, "the first part");
	serve();
	check(ask(first, "nt9999\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 2\n") == 0, "a missing node, asked in two parts");

	/* The compositor that does not hold the seat. */
	check(ask(second, "OPEN event9999\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 1\n") == 0, "not the holder: EPERM");

	/* The holder moves: the first is refused now. */
	sessiond_seat_hold(second_own);
	check(ask(first, "OPEN event9999\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 1\n") == 0, "the old holder: EPERM");
	check(ask(second, "OPEN event9999\n", answer, sizeof(answer)) == 0 && strcmp(answer, "FAIL 2\n") == 0, "the new holder is answered");

	/* The compositors end: their sockets leave the poll. */
	(void)close(first);
	serve();
	count = sessiond_seat_poll_fill(entries, SESSIOND_SEAT_SOCKETS);
	check(count == 1, "the ended socket left the poll");
	(void)close(second);
	serve();
	count = sessiond_seat_poll_fill(entries, SESSIOND_SEAT_SOCKETS);
	check(count == 0, "no socket left");
}

/* The backend's side of the seat socket. */
static void
backend_part(void)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *message_control;
	struct kl_backend_input_node node;
	struct kl_backend *backend;
	struct opening opening;
	struct msghdr header;
	struct iovec vector;
	struct told told;
	pthread_t thread;
	char request[64];
	char text[8];
	int pipes[2];
	int pair[2];
	int own;
	int child;
	ssize_t length;
	int error;

	/* A backend whose seat socket is sessiond's seat.c, holding the seat. */
	memset(&told, 0, sizeof(told));
	backend = backend_make(&told);
	error = sessiond_seat_socket(&own, &child);
	check(error == 0, "the backend's seat socket");
	sessiond_seat_hold(own);
	backend->options.seat_descriptor = child;

	/* sessiond's refusal: its errno, and the socket kept (served on a thread of the test's own). */
	opening.backend = backend;
	opening.path = "/dev/input/event9999";
	opening.error = -1;
	error = pthread_create(&thread, NULL, open_run, &opening);
	check(error == 0, "the opening thread");
	serve_wait();
	(void)pthread_join(thread, NULL);
	check(opening.error == ENOENT, "a missing node: ENOENT from sessiond");
	check(!backend->seat_gone, "the socket is kept after FAIL");
	kl_backend_close(backend);
	(void)close(child);
	serve();

	/* A raw peer for OK: the descriptor comes, and the capabilities of a pipe cannot be read. */
	memset(&told, 0, sizeof(told));
	backend = backend_make(&told);
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	check(error == 0, "a raw pair");
	error = pipe(pipes);
	check(error == 0, "a pipe to give");
	backend->options.seat_descriptor = pair[1];
	opening.backend = backend;
	opening.path = "/dev/input/event7";
	opening.error = -1;
	error = pthread_create(&thread, NULL, open_run, &opening);
	check(error == 0, "the opening thread");
	length = read(pair[0], request, sizeof(request) - 1U);
	check(length > 0, "the request came");
	if (length > 0)
		request[length] = '\0';
	check(length > 0 && strcmp(request, "OPEN event7\n") == 0, "the request is OPEN event7");
	memset(&header, 0, sizeof(header));
	memset(&control, 0, sizeof(control));
	snprintf(text, sizeof(text), "OK\n");
	vector.iov_base = text;
	vector.iov_len = 3U;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = sizeof(control.space);
	message_control = CMSG_FIRSTHDR(&header);
	message_control->cmsg_level = SOL_SOCKET;
	message_control->cmsg_type = SCM_RIGHTS;
	message_control->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(message_control), &pipes[0], sizeof(int));
	check(sendmsg(pair[0], &header, 0) == 3, "OK sent with a descriptor");
	(void)pthread_join(thread, NULL);
	check(opening.error == ENOTTY, "OK's descriptor taken: a pipe has no capabilities (ENOTTY)");
	check(!backend->seat_gone, "the socket is kept after OK");

	/* The socket's end: EPIPE, and the socket given up. */
	(void)close(pair[0]);
	error = kl_backend_input_open(backend, "/dev/input/event8", &node);
	check(error == EPIPE, "the end: EPIPE");
	check(backend->seat_gone, "the socket is gone");
	error = kl_backend_input_open(backend, "/dev/input/event8", &node);
	check(error == EPIPE, "nothing asked on a gone socket");
	kl_backend_close(backend);
	(void)close(pair[1]);
	(void)close(pipes[0]);
	(void)close(pipes[1]);

	/* An answer that never comes: the cancel ends the wait. */
	memset(&told, 0, sizeof(told));
	backend = backend_make(&told);
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	check(error == 0, "a silent pair");
	backend->options.seat_descriptor = pair[1];
	opening.backend = backend;
	opening.path = "/dev/input/event7";
	opening.error = -1;
	error = pthread_create(&thread, NULL, open_run, &opening);
	check(error == 0, "the opening thread");
	usleep(100000);
	kl_backend_input_cancel(backend);
	(void)pthread_join(thread, NULL);
	check(opening.error == ECANCELED, "the cancel: ECANCELED");
	kl_backend_close(backend);
	(void)close(pair[0]);
	(void)close(pair[1]);
}

/* The devices' wait on a pipe that stands for /dev/system. */
static void
devices_part(void)
{
	struct kl_backend_device_batch batch;
	struct kl_backend *backend;
	struct told told;
	int pipes[2];
	int error;

	/* The backend, the pipe as the devices' descriptor, no seat socket (the node is opened here). */
	memset(&told, 0, sizeof(told));
	backend = backend_make(&told);
	error = pipe(pipes);
	check(error == 0, "the pipe");
	(void)fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	backend->devices_descriptor = pipes[0];
	check(kl_backend_input_events(backend), "the devices are told");

	/* An input device that came: told, and opened (not there on the host). */
	post(pipes[1], KERN_SYSTEM_EVENT_INPUT, KERN_SYSTEM_EVENT_ADD, "event9999", "bus=3 props=0 name=test");
	error = kl_backend_devices_wait(backend, &batch);
	check(error == 0 && batch.input_changed && !batch.keys_changed && !batch.overflow && batch.count == 0U, "an ADD: input changed, its open failed");

	/* A FIDO node, and a smart card: the keys. */
	post(pipes[1], KERN_SYSTEM_EVENT_INPUT, KERN_SYSTEM_EVENT_ADD, "hidraw2", "usage=f1d0:0001");
	post(pipes[1], KERN_SYSTEM_EVENT_USB, KERN_SYSTEM_EVENT_CHANGE, "smartcard0", "");
	error = kl_backend_devices_wait(backend, &batch);
	check(error == 0 && batch.input_changed && batch.keys_changed && !batch.overflow && batch.count == 0U, "a FIDO node and a card: the keys");
	kl_backend_devices_take(backend, &batch);
	check(told.keys == 1 && told.input == 0, "the take: keys_changed only");

	/* Lost records: a finding. */
	post(pipes[1], KERN_SYSTEM_EVENT_OVERFLOW, 0, "", "");
	error = kl_backend_devices_wait(backend, &batch);
	check(error == 0 && batch.overflow && batch.keys_changed, "OVERFLOW: a finding and the keys");
	kl_backend_devices_take(backend, &batch);
	check(told.keys == 2 && told.input == 1, "the take: keys_changed and input_changed");

	/* A USB device that is no card asks nothing: the wait goes on until the cancel. */
	post(pipes[1], KERN_SYSTEM_EVENT_USB, KERN_SYSTEM_EVENT_ADD, "ugen0.2", "");
	kl_backend_input_cancel(backend);
	error = kl_backend_devices_wait(backend, &batch);
	check(error == ECANCELED, "the cancel ends the wait");
	backend->devices_descriptor = -1;
	kl_backend_close(backend);
	(void)close(pipes[0]);
	(void)close(pipes[1]);
}

int
main(void)
{
	/* The three parts. */
	sessiond_part();
	backend_part();
	devices_part();

	/* The verdict. */
	if (failures != 0) {
		printf("seat-host: FAIL %d of %d checks\n", failures, checks);
		return 1;
	}
	printf("seat-host: PASS %d checks\n", checks);
	return 0;
}
