/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p024: the host test of the compositor's printers object
 * (userland/desktop/wayland/printers-shell.c) on the wire's bytes, with
 * libkeiland-backend's print.c and a stand-in daemon (fake-printd.py,
 * started as KEILAND_LIBEXECDIR/keiland-printd): the first state and
 * done, add answered once the writer thread made it, titles that break
 * the rules refused INVALID with their documents closed (bad UTF-8, an
 * overlong form, a surrogate, a C1 control; past 127 bytes the string is
 * past the wire's bound, EPROTO), queued
 * before a print's result, EAGAIN for a print whose descriptor has not
 * come, EPROTO for an opcode not known.
 *
 *     host-printers-shell HOME DOCUMENT
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The events the printers object was sent, kept for the checks. */
#define EVENTS_MAX	256U

/* One event: the object, its opcode and its first words. */
struct event {
	uint32_t object;
	uint32_t opcode;
	uint32_t words[4];
	size_t size;
	unsigned char bytes[512];
};

/* The checks that failed, the events sent, the descriptors the client "sent", and the home. */
static int failures;
static struct event events[EVENTS_MAX];
static size_t event_count;
static int fds[8];
static size_t fd_count;
static const char *home_dir;

int main(int argc, char **argv);
static void check(const char *name, int passed, const char *detail);
static size_t put_word(unsigned char *bytes, size_t offset, uint32_t word);
static size_t put_string(unsigned char *bytes, size_t offset, const char *text);
static int print_request(struct kwl_object *object, uint32_t request, const char *title, int fd);
static int wait_event(struct kwl_server *server, uint32_t opcode, uint32_t request, int seconds, size_t *found);
static int fd_open(int fd);
static int edit_request(struct kwl_object *object, uint32_t request, uint32_t printer, const char *name, const char *path);
static int printer_told_before(const char *text, size_t since, size_t before);
static int printer_told(const char *text, size_t since);
static void pause_ms(unsigned ms);

/* The compositor's calls the printers object makes, stood in for. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct event *event;

	/* Kept, its first words copied. */
	(void)client;
	if (event_count == EVENTS_MAX)
		return 0;
	event = &events[event_count];
	event_count++;
	memset(event, 0, sizeof(*event));
	event->object = object;
	event->opcode = opcode;
	event->size = size;
	memcpy(event->words, payload, size < sizeof(event->words) ? size : sizeof(event->words));
	memcpy(event->bytes, payload, size < sizeof(event->bytes) ? size : sizeof(event->bytes));
	return 0;
}

/* The next descriptor the client sent (-1 for none). */
int
kwl_take_fd(
	struct kwl_client *client)
{
	int fd;

	/* The oldest. */
	(void)client;
	if (fd_count == 0U)
		return -1;
	fd = fds[0];
	memmove(&fds[0], &fds[1], (fd_count - 1U) * sizeof(fds[0]));
	fd_count--;
	return fd;
}

/* Makes an object on the client's list. */
struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	struct kwl_object *object;

	/* A fresh object at the list's head. */
	object = calloc(1, sizeof(*object));
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	return object;
}

/* Marks an object dead. */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	/* Dead, left on the list. */
	object->dead = 1;
}

/* The user's home: the test's folder. */
int
kwl_settings_home(
	char *home,
	size_t size)
{
	/* The folder given. */
	(void)snprintf(home, size, "%s", home_dir);
	return 0;
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	struct kwl_server *server;
	struct kwl_client *client;
	struct kwl_object manager;
	struct kwl_object *object;
	unsigned char bytes[512];
	char path[1024];
	char long_title[160];
	size_t length;
	size_t found;
	size_t since;
	size_t queued_at;
	int tries;
	int status;
	int fd;
	int ok;

	/* The arguments; the home's .config. */
	if (argc != 3) {
		fprintf(stderr, "usage: host-printers-shell HOME DOCUMENT\n");
		return 2;
	}
	home_dir = argv[1];
	(void)snprintf(path, sizeof(path), "%s/.config", home_dir);
	(void)mkdir(path, 0700);

	/* A server with one client, and its manager object. */
	server = calloc(1, sizeof(*server));
	client = calloc(1, sizeof(*client));
	client->server = server;
	client->number = 1;
	server->clients = client;
	memset(&manager, 0, sizeof(manager));
	manager.client = client;
	manager.id = 3;
	manager.kind = KWL_SYSTEM_MANAGER;
	manager.version = 30;

	/* 1. The printers object: its first state, done. */
	length = put_word(bytes, 0, 5U);
	status = kwl_printers_create(&manager, bytes, length);
	object = client->objects;
	check("create", status == 0 && object != NULL && event_count == 1U && events[0].opcode == KL_SYSTEM_PRINTERS_EVENT_DONE,
	    "the state is done alone");

	/* 2. add(1, LPD, 127.0.0.1, 515, ""): answered OK once the writer made it, the printer told. */
	length = put_word(bytes, 0, 1U);
	length = put_word(bytes, length, KL_SYSTEM_PRINTER_LPD);
	length = put_string(bytes, length, "127.0.0.1");
	length = put_word(bytes, length, 515U);
	length = put_string(bytes, length, "");
	status = kwl_printers_request(object, KL_SYSTEM_PRINTERS_ADD, bytes, length);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 1U, 5, &found);
	check("add", ok && events[found].words[1] == KL_SYSTEM_RESULT_OK, "result OK");
	ok = wait_event(server, KL_SYSTEM_PRINTERS_EVENT_PRINTER, 1U, 5, &found);
	check("add-told", ok, "a printer event with id 1");

	/* 2b. edit (since 24, ws177-p025): the name and the queue changed and told; names and paths of the rules only. */
	since = event_count;
	status = edit_request(object, 20U, 1U, "Front Desk", "q2");
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 20U, 5, &found);
	check("edit", ok && events[found].words[1] == KL_SYSTEM_RESULT_OK, "result OK");
	check("edit-told-first", ok && printer_told_before("Front Desk", since, found), "the new name told before the answer (T1-459)");
	for (tries = 0; tries < 100 && !printer_told("Front Desk", since); tries++) {
		kwl_printers_tick(server);
		pause_ms(20);
	}
	check("edit-told", printer_told("Front Desk", since) && printer_told("q2", since), "a printer event with the new name and queue");
	status = edit_request(object, 21U, 1U, "c1 \xc2\x85", "");
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 21U, 1, &found);
	check("edit-name-c1", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID, "INVALID");
	status = edit_request(object, 22U, 1U, "", "a b");
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 22U, 1, &found);
	check("edit-path-space", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID, "INVALID");
	status = edit_request(object, 23U, 9U, "Nowhere", "");
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 23U, 5, &found);
	check("edit-unknown-printer", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID, "INVALID");
	object->version = 23U;
	status = edit_request(object, 24U, 1U, "Old", "");
	check("edit-before-24", status == EPROTO, "EPROTO from an object of version 23");
	object->version = 30U;

	/* 3. Titles that break the rules: INVALID, the document closed. */
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 10U, "bad \xc3", fd);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 10U, 1, &found);
	check("title-cut-utf8", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID && !fd_open(fd), "INVALID, closed");
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 11U, "over \xc0\xaf", fd);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 11U, 1, &found);
	check("title-overlong", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID && !fd_open(fd), "INVALID, closed");
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 12U, "half \xed\xa0\x80", fd);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 12U, 1, &found);
	check("title-surrogate", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID && !fd_open(fd), "INVALID, closed");
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 13U, "c1 \xc2\x85", fd);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 13U, 1, &found);
	check("title-c1", ok && events[found].words[1] == KL_SYSTEM_RESULT_INVALID && !fd_open(fd), "INVALID, closed");
	memset(long_title, 'a', 128U);
	long_title[128] = '\0';
	/* Past 127 bytes the string itself is past the wire's bound: the protocol is broken (the client ends). */
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 14U, long_title, fd);
	check("title-long", status == EPROTO, "EPROTO");
	(void)close(kwl_take_fd(client));

	/* 4. A print whose descriptor has not come: EAGAIN, nothing answered. */
	status = print_request(object, 15U, "later", -1);
	check("no-descriptor-yet", status == EAGAIN, "EAGAIN");

	/* 5. A good title in many scripts: queued before its result OK, then done. */
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	status = print_request(object, 16U, "R\xc3\xa9sum\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80", fd);
	ok = status == 0 && wait_event(server, KL_SYSTEM_PRINTERS_EVENT_RESULT, 16U, 5, &found);
	queued_at = EVENTS_MAX;
	if (ok)
		ok = wait_event(server, KL_SYSTEM_PRINTERS_EVENT_QUEUED, 16U, 0, &queued_at);
	check("print-queued-then-ok", ok && queued_at < found && events[found].words[1] == KL_SYSTEM_RESULT_OK, "queued before OK");

	/* 6. An opcode not known: EPROTO. */
	status = kwl_printers_request(object, 99U, bytes, 0U);
	check("unknown-opcode", status == EPROTO, "EPROTO");

	/* The outcome. */
	printf("host-printers-shell: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

/* Prints a check's outcome. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS, or FAIL with why. */
	if (passed) {
		printf("PASS %s\n", name);
	} else {
		printf("FAIL %s: %s\n", name, detail);
		failures++;
	}
	fflush(stdout);
}

/* Puts a word on the wire (the host's order, as the compositor reads it). */
static size_t
put_word(
	unsigned char *bytes,
	size_t offset,
	uint32_t word)
{
	/* Four bytes. */
	memcpy(bytes + offset, &word, 4U);
	return offset + 4U;
}

/* Puts a string on the wire: its length with the NUL, its bytes, padded to a word. */
static size_t
put_string(
	unsigned char *bytes,
	size_t offset,
	const char *text)
{
	size_t length;
	size_t padded;

	/* The length, then the bytes and the padding. */
	length = strlen(text) + 1U;
	offset = put_word(bytes, offset, (uint32_t)length);
	padded = (length + 3U) & ~(size_t)3U;
	memset(bytes + offset, 0, padded);
	memcpy(bytes + offset, text, length);
	return offset + padded;
}

/* Sends print(request, 0 (the default), title) with a descriptor beside it (-1 for none yet). */
static int
print_request(
	struct kwl_object *object,
	uint32_t request,
	const char *title,
	int fd)
{
	unsigned char bytes[512];
	size_t length;
	int status;

	/* The descriptor first, as it comes beside the bytes. */
	if (fd >= 0) {
		fds[fd_count] = fd;
		fd_count++;
	}

	/* The bytes. */
	length = put_word(bytes, 0, request);
	length = put_word(bytes, length, 0U);
	length = put_string(bytes, length, title);
	status = kwl_printers_request(object, KL_SYSTEM_PRINTERS_PRINT, bytes, length);
	return status;
}

/*
 * Ticks until an event of an opcode for a request (its first word; any
 * for a printer's event) was sent; *found its index.
 */
static int
wait_event(
	struct kwl_server *server,
	uint32_t opcode,
	uint32_t request,
	int seconds,
	size_t *found)
{
	size_t index;
	int tries;

	/* A tick every 20 ms (at least one look). */
	for (tries = 0; tries <= seconds * 50; tries++) {
		for (index = 0; index < event_count; index++) {
			if (events[index].opcode != opcode)
				continue;
			if (opcode != KL_SYSTEM_PRINTERS_EVENT_PRINTER && events[index].words[0] != request)
				continue;
			*found = index;
			return 1;
		}
		if (seconds == 0)
			break;
		kwl_printers_tick(server);
		pause_ms(20);
	}

	/* Not sent. */
	return 0;
}

/* Tells whether a descriptor is still open. */
static int
fd_open(
	int fd)
{
	int flags;

	/* Its flags, or EBADF. */
	flags = fcntl(fd, F_GETFD);
	return flags >= 0;
}

/* Sleeps a number of milliseconds. */
static void
pause_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The time. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Sends edit(request, printer, name, path). */
static int
edit_request(
	struct kwl_object *object,
	uint32_t request,
	uint32_t printer,
	const char *name,
	const char *path)
{
	unsigned char bytes[512];
	size_t length;
	int status;

	/* The bytes. */
	length = put_word(bytes, 0, request);
	length = put_word(bytes, length, printer);
	length = put_string(bytes, length, name);
	length = put_string(bytes, length, path);
	status = kwl_printers_request(object, KL_SYSTEM_PRINTERS_EDIT, bytes, length);
	return status;
}

/* Tells whether a printer event since an index carried a text. */
static int
printer_told(
	const char *text,
	size_t since)
{
	int told;

	/* Any printer event from the index on. */
	told = printer_told_before(text, since, event_count);
	return told;
}

/* Tells whether a printer event between two indexes (since, before) carried a text. */
static int
printer_told_before(
	const char *text,
	size_t since,
	size_t before)
{
	size_t index;
	size_t length;
	size_t at;

	/* Each printer event's bytes. */
	length = strlen(text);
	for (index = since; index < before && index < event_count; index++) {
		if (events[index].opcode != KL_SYSTEM_PRINTERS_EVENT_PRINTER)
			continue;
		for (at = 0; at + length <= sizeof(events[index].bytes); at++) {
			if (memcmp(events[index].bytes + at, text, length) == 0)
				return 1;
		}
	}
	return 0;
}
