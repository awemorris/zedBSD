/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libwayland's generic event dispatch: the path that calls a
 * listener of an interface the library has no typed table for.  The test
 * plays the compositor on one end of a socket pair, writes two events of a
 * made-up interface onto it by hand, and lets the library on the other end
 * deliver them.  The listeners compare every argument with what was sent:
 * uints of 2^31 and more, negative ints and fixed values, an fd, a string,
 * an array, objects, a null object and a server-created object, in
 * registers and on the stack.
 *
 * Built at -O2 so that a listener compares a 32-bit argument with the whole
 * register, as a compiler may when the ABI defines the register's upper
 * half; at -O0 a listener reloads the argument from its own stack slot and
 * hides a wrongly extended register.  The Wayland headers are the only
 * zedBSD headers on the include path (INCLUDE is a directory of links to
 * userland/desktop/include/wayland and the wayland-client.h,
 * wayland-client-core.h, wayland-client-protocol.h and wayland-util.h
 * beside it; SOURCES is every .c file of userland/desktop/libwayland):
 *
 *   CC -std=gnu99 -O2 -Wall -Wextra -Werror -IINCLUDE -I. SOURCES \
 *      tests/libwayland-generic-host-test.c \
 *      -lpthread -o build/tmp/libwayland-generic-host-test
 *
 * CC is the host's cc, or a cross compiler (riscv64-linux-gnu-gcc,
 * aarch64-linux-gnu-gcc) whose program runs under qemu-riscv64 or
 * qemu-aarch64 with -L /usr/<triple>.  It prints one line for each failed
 * check and "libwayland-generic-host-test: PASS (N checks)" or "... FAIL"
 * at the end.
 */

#include <wayland/wayland-client.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* The identity the test's client object gets: the first after wl_display. */
#define PROBE_OBJECT_ID		2U

/* The identity of the object the compositor creates: the first in its range. */
#define PROBE_CHILD_ID		0xff000000U

/* The opcodes of the probe's events, in the order of probe_events. */
#define PROBE_EVENT_SCALARS	0U
#define PROBE_EVENT_REFERENCES	1U

/* The most 32-bit words one hand-written event takes. */
#define PROBE_EVENT_WORDS	32U

/* How long the test may wait for the library before it gives up, in seconds. */
#define PROBE_TIME_LIMIT	10U

/* How many times the test lets the library read and dispatch the socket. */
#define PROBE_DISPATCH_TRIES	4

/*
 * One event the test writes on the compositor's end of the socket.
 *
 * It is filled one word at a time in the wire's native byte order and sent
 * whole; the header's size is written when the event is complete.
 */
struct probe_event {
	uint32_t words[PROBE_EVENT_WORDS];
	size_t count;
};

/*
 * What the listeners saw, for main to judge after the dispatch.
 *
 * One instance lives on main's stack and is the listeners' data.
 */
struct probe_state {
	struct wl_proxy *probe;
	struct wl_proxy *child;
	int scalars_seen;
	int references_seen;
	int checks;
	int failures;
};

/*
 * The listener of the probe interface, in the form wayland-scanner gives it.
 *
 * Its callbacks take narrow 32-bit parameters, which is what the generic
 * path must deliver exactly.
 */
struct probe_listener {
	void (*scalars)(void *data, struct wl_proxy *probe,
			uint32_t a0, int32_t a1, uint32_t a2, wl_fixed_t a3,
			uint32_t a4, int32_t a5, uint32_t a6, int32_t a7,
			int32_t a8, uint32_t a9, int32_t a10, uint32_t a11,
			wl_fixed_t a12);
	void (*references)(void *data, struct wl_proxy *probe,
			   const char *text, struct wl_array *bytes,
			   struct wl_proxy *self, struct wl_proxy *none,
			   struct wl_proxy *child, uint32_t tag);
};

static void probe_scalars(void *data, struct wl_proxy *probe, uint32_t a0, int32_t a1, uint32_t a2, wl_fixed_t a3, uint32_t a4, int32_t a5, uint32_t a6, int32_t a7, int32_t a8, uint32_t a9, int32_t a10, uint32_t a11, wl_fixed_t a12);
static void probe_references(void *data, struct wl_proxy *probe, const char *text, struct wl_array *bytes, struct wl_proxy *self, struct wl_proxy *none, struct wl_proxy *child, uint32_t tag);
static void probe_check(struct probe_state *state, int passed, const char *what);
static void probe_event_start(struct probe_event *event);
static void probe_event_word(struct probe_event *event, uint32_t word);
static void probe_event_bytes(struct probe_event *event, const void *bytes, uint32_t length);
static void probe_event_finish(struct probe_event *event, uint32_t opcode);
static int probe_send(int fd, const struct probe_event *event, int descriptor);

/*
 * The server-created interface: it has no events of its own.
 *
 * Its name is unknown to the library, so nothing about it is typed.
 */
static const struct wl_interface probe_child_interface = {
	"bug_generic_child_v1", 1,
	0, NULL,
	0, NULL,
};

/*
 * The argument interfaces of the references event, in argument order.
 *
 * The probe's own interface is named before it is defined, as the code
 * wayland-scanner makes names it.
 */
extern const struct wl_interface probe_interface;
static const struct wl_interface *probe_reference_types[] = {
	NULL,
	NULL,
	&probe_interface,
	&probe_interface,
	&probe_child_interface,
	NULL,
};

/*
 * The argument interfaces of the scalars event: it has no object.
 *
 * Every entry is null; the table only gives each argument its slot.
 */
static const struct wl_interface *probe_scalar_types[] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL,
};

/*
 * The probe's events.
 *
 * scalars has thirteen arguments, so that on every 64-bit ABI some uints
 * and negative ints arrive in registers and some on the stack; references
 * has every kind that is a pointer and a uint after them.
 */
static const struct wl_message probe_events[] = {
	{ "scalars", "uiufuiuhiuiuf", probe_scalar_types },
	{ "references", "sao?onu", probe_reference_types },
};

/*
 * The probe interface, which the library has no typed table for.
 *
 * Its listener is therefore called through the generic path.
 */
const struct wl_interface probe_interface = {
	"bug_generic_probe_v1", 1,
	0, NULL,
	2, probe_events,
};

/*
 * The listener the test registers on its probe object.
 *
 * Constant for the whole run.
 */
static const struct probe_listener probe_listener = {
	probe_scalars,
	probe_references,
};

/*
 * Runs the generic dispatch against hand-written events and reports the result.
 */
int
main(void)
{
	struct probe_state state;
	struct probe_event event;
	struct wl_display *display;
	uint32_t probe_id;
	int sockets[2];
	int pipe_ends[2];
	int error;
	int tries;
	int dispatched;
	static const unsigned char array_bytes[5] = { 1, 2, 3, 4, 5 };

	/* Bounds the run, so a library that never delivers cannot hang the test. */
	alarm(PROBE_TIME_LIMIT);

	/* Connects the library to one end of a socket pair; the test is the other end. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	if (error != 0) {
		perror("socketpair");
		return 1;
	}

	display = wl_display_connect_to_fd(sockets[0]);
	if (display == NULL) {
		perror("wl_display_connect_to_fd");
		return 1;
	}

	/* Creates the client's probe object and listens on it through the generic path. */
	memset(&state, 0, sizeof(state));
	state.probe = wl_proxy_create((struct wl_proxy *)display, &probe_interface);
	if (state.probe == NULL) {
		perror("wl_proxy_create");
		return 1;
	}

	error = wl_proxy_add_listener(state.probe, (void (**)(void))&probe_listener, &state);
	if (error != 0) {
		fprintf(stderr, "wl_proxy_add_listener failed\n");
		return 1;
	}

	/* The probe must be the object the hand-written events name. */
	probe_id = wl_proxy_get_id(state.probe);
	probe_check(&state, probe_id == PROBE_OBJECT_ID, "probe object id");

	/* Makes the descriptor the scalars event carries. */
	error = pipe(pipe_ends);
	if (error != 0) {
		perror("pipe");
		return 1;
	}

	/* Writes the scalars event: uints of 2^31 and more and negative values in every position. */
	probe_event_start(&event);
	probe_event_word(&event, 0x80000000U);
	probe_event_word(&event, (uint32_t)-1);
	probe_event_word(&event, 0xffffffffU);
	probe_event_word(&event, (uint32_t)-256);
	probe_event_word(&event, 0xdeadbeefU);
	probe_event_word(&event, 0x80000000U);
	probe_event_word(&event, 0x7fffffffU);
	probe_event_word(&event, (uint32_t)-2);
	probe_event_word(&event, 0x80000001U);
	probe_event_word(&event, 0x7fffffffU);
	probe_event_word(&event, 0xfedcba98U);
	probe_event_word(&event, (uint32_t)-1);
	probe_event_finish(&event, PROBE_EVENT_SCALARS);
	error = probe_send(sockets[1], &event, pipe_ends[0]);
	if (error != 0)
		return 1;

	/* The library holds its own copy of the descriptor now. */
	close(pipe_ends[0]);
	close(pipe_ends[1]);

	/* Writes the references event: a string, an array, the probe, a null object, a new object and a uint. */
	probe_event_start(&event);
	probe_event_word(&event, 8U);
	probe_event_bytes(&event, "generic", 8U);
	probe_event_word(&event, (uint32_t)sizeof(array_bytes));
	probe_event_bytes(&event, array_bytes, (uint32_t)sizeof(array_bytes));
	probe_event_word(&event, PROBE_OBJECT_ID);
	probe_event_word(&event, 0U);
	probe_event_word(&event, PROBE_CHILD_ID);
	probe_event_word(&event, 0xc0000000U);
	probe_event_finish(&event, PROBE_EVENT_REFERENCES);
	error = probe_send(sockets[1], &event, -1);
	if (error != 0)
		return 1;

	/* Lets the library read and deliver both events. */
	for (tries = 0; tries < PROBE_DISPATCH_TRIES; tries++) {
		/* Stops once both events have reached their listeners. */
		if (state.scalars_seen != 0 && state.references_seen != 0)
			break;

		/* Reads what the socket holds and calls the listeners. */
		dispatched = wl_display_dispatch(display);
		if (dispatched < 0) {
			perror("wl_display_dispatch");
			return 1;
		}
	}

	/* Both listeners must have run, each once. */
	probe_check(&state, state.scalars_seen == 1, "scalars delivered once");
	probe_check(&state, state.references_seen == 1, "references delivered once");

	/* Releases the objects and the connection. */
	if (state.child != NULL)
		wl_proxy_destroy(state.child);

	wl_proxy_destroy(state.probe);
	wl_display_disconnect(display);
	close(sockets[1]);

	/* Reports the verdict. */
	if (state.failures != 0) {
		printf("libwayland-generic-host-test: FAIL (%d of %d checks)\n", state.failures, state.checks);
		return 1;
	}

	/* Succeeded: every argument arrived as its listener's type says. */
	printf("libwayland-generic-host-test: PASS (%d checks)\n", state.checks);
	return 0;
}

/* Checks the scalars event: every uint, int, fixed and fd as sent. */
static void
probe_scalars(
	void *data,
	struct wl_proxy *probe,
	uint32_t a0,
	int32_t a1,
	uint32_t a2,
	wl_fixed_t a3,
	uint32_t a4,
	int32_t a5,
	uint32_t a6,
	int32_t a7,
	int32_t a8,
	uint32_t a9,
	int32_t a10,
	uint32_t a11,
	wl_fixed_t a12)
{
	struct probe_state *state;
	int flags;

	state = data;
	state->scalars_seen++;

	/* The event is the probe's. */
	probe_check(state, probe == state->probe, "scalars: probe object");

	/* The uints of 2^31 and more, compared with the whole register where the ABI defines it. */
	probe_check(state, a0 == 0x80000000U, "scalars: uint 0x80000000 (register)");
	probe_check(state, a2 == 0xffffffffU, "scalars: uint 0xffffffff (register)");
	probe_check(state, a4 == 0xdeadbeefU, "scalars: uint 0xdeadbeef");
	probe_check(state, a9 == 0x80000001U, "scalars: uint 0x80000001 (stack)");
	probe_check(state, a11 == 0xfedcba98U, "scalars: uint 0xfedcba98 (stack)");
	probe_check(state, a6 == 0x7fffffffU, "scalars: uint 0x7fffffff");

	/* The same uints widened by the listener, which must not see a sign. */
	probe_check(state, (uint64_t)a0 == (uint64_t)0x80000000U, "scalars: uint 0x80000000 widened");
	probe_check(state, (uint64_t)a2 == (uint64_t)0xffffffffU, "scalars: uint 0xffffffff widened");

	/* Unsigned order between two uints the generic path passed. */
	probe_check(state, a0 < a2, "scalars: uint order");
	probe_check(state, a6 < a0, "scalars: uint order across 2^31");

	/* The negative ints and fixed values, and the largest int. */
	probe_check(state, a1 == -1, "scalars: int -1");
	probe_check(state, a5 == INT32_MIN, "scalars: int INT32_MIN");
	probe_check(state, a8 == -2, "scalars: int -2 (stack)");
	probe_check(state, a10 == INT32_MAX, "scalars: int INT32_MAX (stack)");
	probe_check(state, a1 < 0, "scalars: int -1 is negative");
	probe_check(state, (int64_t)a5 == (int64_t)INT32_MIN, "scalars: int INT32_MIN widened");
	probe_check(state, a3 == wl_fixed_from_int(-1), "scalars: fixed -1.0");
	probe_check(state, a12 == -1, "scalars: fixed -1/256 (stack)");

	/* The descriptor is open and now the listener's to close. */
	flags = fcntl(a7, F_GETFD);
	probe_check(state, flags != -1, "scalars: fd is open");
	if (flags != -1)
		close(a7);
}

/* Checks the references event: the pointer kinds and a uint after them. */
static void
probe_references(
	void *data,
	struct wl_proxy *probe,
	const char *text,
	struct wl_array *bytes,
	struct wl_proxy *self,
	struct wl_proxy *none,
	struct wl_proxy *child,
	uint32_t tag)
{
	static const unsigned char array_bytes[5] = { 1, 2, 3, 4, 5 };
	struct probe_state *state;
	const char *child_class;
	uint32_t child_id;
	int same;

	state = data;
	state->references_seen++;

	/* The event is the probe's, and names the probe and nothing. */
	probe_check(state, probe == state->probe, "references: probe object");
	probe_check(state, self == state->probe, "references: object");
	probe_check(state, none == NULL, "references: null object");

	/* The string, with its text. */
	probe_check(state, text != NULL, "references: string present");
	if (text != NULL) {
		same = strcmp(text, "generic");
		probe_check(state, same == 0, "references: string");
	}

	/* The array, with its bytes. */
	probe_check(state, bytes != NULL, "references: array present");
	if (bytes != NULL) {
		probe_check(state, bytes->size == sizeof(array_bytes), "references: array size");
	}

	/* The bytes are compared only when the size says they are all there. */
	if (bytes != NULL && bytes->size == sizeof(array_bytes)) {
		same = memcmp(bytes->data, array_bytes, sizeof(array_bytes));
		probe_check(state, same == 0, "references: array bytes");
	}

	/* The server-created object, which the listener now owns. */
	state->child = child;
	probe_check(state, child != NULL, "references: new object");
	if (child != NULL) {
		child_id = wl_proxy_get_id(child);
		probe_check(state, child_id == PROBE_CHILD_ID, "references: new object id");

		child_class = wl_proxy_get_class(child);
		same = strcmp(child_class, "bug_generic_child_v1");
		probe_check(state, same == 0, "references: new object interface");
	}

	/* The uint after the pointers, of 2^31 and more. */
	probe_check(state, tag == 0xc0000000U, "references: uint 0xc0000000");
}

/* Counts and names one failed check. */
static void
probe_check(
	struct probe_state *state,
	int passed,
	const char *what)
{
	/* Every check is counted; a passed one leaves no other trace. */
	state->checks++;
	if (passed)
		return;

	printf("FAIL: %s\n", what);
	state->failures++;
}

/* Starts an event to the probe object with room for its header. */
static void
probe_event_start(
	struct probe_event *event)
{
	memset(event, 0, sizeof(*event));
	event->words[0] = PROBE_OBJECT_ID;
	event->count = 2;
}

/* Appends one 32-bit argument word. */
static void
probe_event_word(
	struct probe_event *event,
	uint32_t word)
{
	event->words[event->count] = word;
	event->count++;
}

/* Appends the bytes of a string or an array, padded to whole words. */
static void
probe_event_bytes(
	struct probe_event *event,
	const void *bytes,
	uint32_t length)
{
	size_t words;

	words = ((size_t)length + 3U) / 4U;
	memcpy(&event->words[event->count], bytes, length);
	event->count += words;
}

/* Writes the header's size and opcode once every argument is in. */
static void
probe_event_finish(
	struct probe_event *event,
	uint32_t opcode)
{
	uint32_t size;

	size = (uint32_t)(event->count * sizeof(uint32_t));
	event->words[1] = (size << 16) | opcode;
}

/* Sends one event, with a descriptor beside it when there is one. */
static int
probe_send(
	int fd,
	const struct probe_event *event,
	int descriptor)
{
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *control;
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control_buffer;
	ssize_t sent;

	/* The event's words are the whole payload. */
	memset(&message, 0, sizeof(message));
	vector.iov_base = (void *)event->words;
	vector.iov_len = event->count * sizeof(uint32_t);
	message.msg_iov = &vector;
	message.msg_iovlen = 1;

	/* A descriptor travels as SCM_RIGHTS with the first byte of its event. */
	if (descriptor >= 0) {
		memset(&control_buffer, 0, sizeof(control_buffer));
		message.msg_control = control_buffer.bytes;
		message.msg_controllen = sizeof(control_buffer.bytes);
		control = CMSG_FIRSTHDR(&message);
		control->cmsg_level = SOL_SOCKET;
		control->cmsg_type = SCM_RIGHTS;
		control->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(control), &descriptor, sizeof(int));
	}

	sent = sendmsg(fd, &message, 0);
	if (sent != (ssize_t)vector.iov_len) {
		perror("sendmsg");
		return -1;
	}

	/* Succeeded: the event is on the socket. */
	return 0;
}
