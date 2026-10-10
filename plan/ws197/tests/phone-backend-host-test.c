/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the desktop's phone backend (ws197-p004a,
 * plan/ws197/phase004/phase.md section 12): libkeiland-backend-zedbsd's
 * phone-zedbsd.c built with its socket at a path of the test's and the
 * test's clock, and a fake bluetoothd in the same thread that accepts the
 * backend's connections and answers the PHONE lines.
 *
 * It checks: no daemon (unreachable, ENOTCONN); SUBSCRIBE and the state;
 * a live message with escapes and its text in two reads; a page with two
 * items, an empty text and PAGE-END; a stale cursor (ESTALE); a text's
 * SENT on the events before the answer's pushed; the arguments refused;
 * the pages full (EBUSY); an answer overdue (ETIMEDOUT, a new
 * connection); DROPPED; a connection lost during a request (ECONNRESET);
 * SUBSCRIBE refused, SHOW of another user's record (not-owner), then of
 * this user's (SUBSCRIBE again); busy (asked again after ten seconds); no
 * record; PHONE LINK; a text longer than bluetoothd sends; the daemon
 * gone (unreachable).
 * usage: phone-backend-host-test FOLDER   (a new folder for the socket)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The backend, with its socket where the test puts it and the test's clock. */
static char test_socket[108];
static uint64_t test_clock = 1000U;
static uint64_t test_now(void);
#define PHONE_SOCKET_PATH test_socket
#define PHONE_NOW_MS test_now
#include "userland/desktop/libkeiland-backend-zedbsd/phone-zedbsd.c"

#define FAKE_CLIENTS	8

/* One connection the fake daemon accepted: its socket (-1 when none) and what it read but not taken. */
struct fake_client {
	int fd;
	char input[32768];
	size_t used;
};

/* The fake daemon: its listener and its clients. */
static struct {
	int listener;
	struct fake_client clients[FAKE_CLIENTS];
} fake;

static int failures;

/* Gives the test's clock. */
static uint64_t
test_now(
	void)
{
	return test_clock;
}

/* Notes a check. */
static void
check(
	int good,
	const char *format,
	...)
{
	va_list arguments;

	if (good)
		return;
	failures++;
	va_start(arguments, format);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

/* Starts the fake daemon's listener. */
static void
fake_start(
	void)
{
	struct sockaddr_un address;
	int flags;

	fake.listener = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", test_socket);
	(void)unlink(test_socket);
	if (bind(fake.listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fake.listener, 8) != 0) {
		perror("fake listener");
		exit(2);
	}
	flags = fcntl(fake.listener, F_GETFL);
	(void)fcntl(fake.listener, F_SETFL, flags | O_NONBLOCK);
}

/* Closes one client of the fake daemon. */
static void
fake_close(
	int client)
{
	if (fake.clients[client].fd >= 0)
		(void)close(fake.clients[client].fd);
	fake.clients[client].fd = -1;
	fake.clients[client].used = 0U;
}

/* Stops the fake daemon: every client and the listener (the socket's path goes, so a connect fails). */
static void
fake_stop(
	void)
{
	int index;

	for (index = 0; index < FAKE_CLIENTS; index++)
		fake_close(index);
	(void)close(fake.listener);
	fake.listener = -1;
	(void)unlink(test_socket);
}

/* Accepts the new clients and reads what each has written. */
static void
fake_pump(
	void)
{
	ssize_t count;
	int descriptor;
	int index;
	int flags;

	for (;;) {
		if (fake.listener < 0)
			break;
		descriptor = accept(fake.listener, NULL, NULL);
		if (descriptor < 0)
			break;
		flags = fcntl(descriptor, F_GETFL);
		(void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
		for (index = 0; index < FAKE_CLIENTS; index++) {
			if (fake.clients[index].fd < 0) {
				fake.clients[index].fd = descriptor;
				fake.clients[index].used = 0U;
				break;
			}
		}
		if (index == FAKE_CLIENTS)
			(void)close(descriptor);
	}
	for (index = 0; index < FAKE_CLIENTS; index++) {
		struct fake_client *client = &fake.clients[index];

		while (client->fd >= 0 && client->used < sizeof(client->input)) {
			count = recv(client->fd, client->input + client->used, sizeof(client->input) - client->used, MSG_DONTWAIT);
			if (count <= 0) {
				if (count == 0)
					fake_close(index);
				break;
			}
			client->used += (size_t)count;
		}
	}
}

/* Runs the backend and the fake daemon a few rounds; returns the changes seen. */
static unsigned
step(
	struct kl_backend_phone *phone)
{
	unsigned changed;
	unsigned all;
	int round;

	all = 0U;
	for (round = 0; round < 4; round++) {
		changed = 0U;
		(void)kl_backend_phone_update(phone, &changed);
		all |= changed;
		fake_pump();
	}
	return all;
}

/*
 * Finds the client whose next line starts with prefix, takes that line
 * (into line) and, when body is not NULL, the length bytes after it.
 * Returns the client, or -1.
 */
static int
fake_take(
	const char *prefix,
	char *line,
	size_t size,
	char *body,
	size_t length)
{
	struct fake_client *client;
	char *end;
	size_t taken;
	int index;

	for (index = 0; index < FAKE_CLIENTS; index++) {
		client = &fake.clients[index];
		if (client->fd < 0 || client->used == 0U)
			continue;
		if (strncmp(client->input, prefix, strlen(prefix)) != 0)
			continue;
		end = memchr(client->input, '\n', client->used);
		if (end == NULL)
			continue;
		taken = (size_t)(end - client->input) + 1U;
		if (body != NULL && client->used < taken + length)
			continue;
		(void)snprintf(line, size, "%.*s", (int)(taken - 1U), client->input);
		if (body != NULL) {
			memcpy(body, client->input + taken, length);
			taken += length;
		}
		memmove(client->input, client->input + taken, client->used - taken);
		client->used -= taken;
		return index;
	}
	return -1;
}

/* Writes to a client of the fake daemon. */
static void
fake_write(
	int client,
	const char *format,
	...)
{
	char text[4096];
	va_list arguments;
	int length;

	if (client < 0 || fake.clients[client].fd < 0)
		return;
	va_start(arguments, format);
	length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	(void)send(fake.clients[client].fd, text, (size_t)length, MSG_NOSIGNAL);
}

/* Waits for a result of an id, running the backend; returns 1 with it. */
static int
wait_result(
	struct kl_backend_phone *phone,
	uint32_t id,
	struct kl_backend_phone_result *result)
{
	int round;

	for (round = 0; round < 20; round++) {
		(void)step(phone);
		while (kl_backend_phone_take_result(phone, result)) {
			if (result->id == id)
				return 1;
		}
	}
	return 0;
}

/* The state line of the owner, ready. */
#define STATE_READY "PHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m present=1 link=ready messages=ready send=1 notify=1\n"

int
main(
	int argc,
	char **argv)
{
	struct kl_backend_phone_state state;
	struct kl_backend_phone_result result;
	struct kl_backend_phone_item item;
	struct kl_backend_phone *phone;
	char line[4096];
	char body[64];
	char *big;
	unsigned changed;
	uint32_t ids[6];
	uint32_t id;
	uint32_t sent_request;
	unsigned sent_state;
	int events;
	int requests;
	int pages;
	int error;
	int index;
	int got;

	if (argc != 2) {
		fprintf(stderr, "usage: phone-backend-host-test FOLDER\n");
		return 2;
	}
	(void)snprintf(test_socket, sizeof(test_socket), "%s/bluetoothd.sock", argv[1]);
	for (index = 0; index < FAKE_CLIENTS; index++)
		fake.clients[index].fd = -1;
	fake.listener = -1;

	/* The backend's own clock is not the test's (built, not used). */
	check(phone_milliseconds() > 0U, "the monotonic clock");

	/* No daemon: unreachable, and the requests refused. */
	phone = kl_backend_phone_open();
	check(phone != NULL, "open");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.reachable == 0U, "no daemon: reachable %u", state.reachable);
	check(strcmp(state.why, "unreachable") == 0, "no daemon: why %s", state.why);
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 0U, "", 32U, &id);
	check(error == ENOTCONN, "no daemon: page %d", error);

	/* The daemon starts: SUBSCRIBE in a second, SHOW beside it (the events are not followed yet). */
	fake_start();
	test_clock += 1001U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "SUBSCRIBE asked");
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "SHOW asked");
	fake_write(events, "DONE\n" STATE_READY);
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m present=1 link=ready messages=ready send=1 notify=1\nDONE\n");
	changed = step(phone);
	kl_backend_phone_get_state(phone, &state);
	check((changed & KL_BACKEND_PHONE_CHANGED_STATE) != 0U, "state changed");
	check(state.reachable && state.subscribed && state.have_record, "state: reachable %u subscribed %u record %u", state.reachable, state.subscribed, state.have_record);
	check(state.linked && state.messages == KL_BACKEND_PHONE_MESSAGES_READY, "state: linked %u messages %u", state.linked, state.messages);
	check(state.can_send && state.notify && state.present && state.enabled, "state: send %u notify %u present %u enabled %u", state.can_send, state.notify, state.present, state.enabled);
	check(state.profiles == KL_BACKEND_PHONE_PROFILE_MESSAGES, "state: profiles %u", state.profiles);
	check(strcmp(state.address, "AA:BB:CC:DD:EE:FF") == 0 && state.why[0] == '\0', "state: address %s why %s", state.address, state.why);

	/* A live message with escapes, its text in two reads. */
	fake_write(events, "PHONE MESSAGE handle=0000000a.00000000000000ff key=0123456789abcdef folder=inbox dir=in time=1791000000 zone=phone datetime=\"20261010T101010\" peer=\"+819012345678\" name=\"Kei \\\"K\\\" \\\\ \\x01\" read=0 partial=0 truncated=0 length=11\nhello");
	changed = step(phone);
	check((changed & KL_BACKEND_PHONE_CHANGED_ITEM) == 0U, "a text half read is not an item");
	fake_write(events, " world");
	changed = step(phone);
	check((changed & KL_BACKEND_PHONE_CHANGED_ITEM) != 0U, "the live item came");
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1, "live item taken");
	check(item.id == 0U && strcmp(item.handle, "0000000a.00000000000000ff") == 0, "live: id %u handle %s", item.id, item.handle);
	check(strcmp(item.key, "0123456789abcdef") == 0 && item.folder == KL_BACKEND_PHONE_FOLDER_INBOX && item.direction == KL_BACKEND_PHONE_DIRECTION_IN, "live: key %s folder %u dir %u", item.key, item.folder, item.direction);
	check(item.time == 1791000000 && item.zone == KL_BACKEND_PHONE_ZONE_PHONE, "live: time %lld zone %u", (long long)item.time, item.zone);
	check(strcmp(item.peer, "+819012345678") == 0 && strcmp(item.datetime, "20261010T101010") == 0, "live: peer %s datetime %s", item.peer, item.datetime);
	check(strcmp(item.name, "Kei \"K\" \\ ?") == 0, "live: name [%s]", item.name);
	check(item.length == 11U && strcmp(item.text, "hello world") == 0 && !item.read && !item.partial, "live: text [%s] %zu", item.text, item.length);
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 0, "one live item only");

	/* A name and a datetime posing as fields (" length=1", " read=1", an escaped quote): the fields outside the quotes count (ws197-p005 review M1). */
	fake_write(events, "PHONE MESSAGE handle=0000000a.00000000000000fe key=- folder=inbox dir=in time=7 zone=phone datetime=\"a length=1 \\\" read=1\" peer=\"5\" name=\"x length=1 dir=out\" read=0 partial=0 truncated=0 length=4\nabcd");
	changed = step(phone);
	check((changed & KL_BACKEND_PHONE_CHANGED_ITEM) != 0U, "the posing item came");
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.length == 4U && strcmp(item.text, "abcd") == 0, "posing: length %zu text [%s]", item.length, item.text);
	check(!item.read && item.direction == KL_BACKEND_PHONE_DIRECTION_IN, "posing: read %u dir %u", item.read, item.direction);
	check(strcmp(item.name, "x length=1 dir=out") == 0 && strcmp(item.datetime, "a length=1 \" read=1") == 0, "posing: name [%s] datetime [%s]", item.name, item.datetime);
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 0, "the posing item alone");

	/* A page: two items (the second with an empty text), the end. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 100, 500U, "", 32U, &id);
	check(error == 0, "page %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE messages since=100 limit=500 count=32", line, sizeof(line), NULL, 0U);
	check(pages >= 0, "the page's line");
	fake_write(pages, "PHONE MESSAGE handle=0000000a.0000000000000001 key=- folder=sent dir=out time=5 zone=received datetime=\"\" peer=\"123\" name=\"\" read=1 partial=1 truncated=1 length=3\nabc");
	fake_write(pages, "PHONE MESSAGE handle=0000000a.0000000000000002 key=00000000000000aa folder=inbox dir=in time=6 zone=local datetime=\"x\" peer=\"Amazon\" name=\"\" read=1 partial=0 truncated=0 length=0\n");
	fake_write(pages, "PHONE PAGE-END cursor=0000000a.64.1.2.0 more=1 count=2 skipped=1 capped=0\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == 0, "page result %d %d", got, result.error);
	check(strcmp(result.cursor, "0000000a.64.1.2.0") == 0 && result.more == 1U && result.count == 2U && result.skipped == 1U && result.capped == 0U, "page end: %s %u %u %u %u", result.cursor, result.more, result.count, result.skipped, result.capped);
	got = kl_backend_phone_take_item(phone, &item);
	check(got && item.id == id && strcmp(item.key, "-") == 0 && item.folder == KL_BACKEND_PHONE_FOLDER_SENT && item.direction == KL_BACKEND_PHONE_DIRECTION_OUT, "page item 1: %u %s %u %u", item.id, item.key, item.folder, item.direction);
	check(item.partial && item.truncated && item.read && item.zone == KL_BACKEND_PHONE_ZONE_RECEIVED && strcmp(item.text, "abc") == 0, "page item 1 flags");
	got = kl_backend_phone_take_item(phone, &item);
	check(got && item.id == id && item.length == 0U && item.text[0] == '\0' && item.zone == KL_BACKEND_PHONE_ZONE_LOCAL, "page item 2 (empty text)");

	/* The next page with the cursor, which is stale. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 100, 500U, "0000000a.64.1.2.0", 32U, &id);
	check(error == 0, "page 2 %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE messages since=100 limit=500 cursor=0000000a.64.1.2.0 count=32", line, sizeof(line), NULL, 0U);
	check(pages >= 0, "page 2's line");
	fake_write(pages, "ERROR stale-cursor\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == ESTALE, "stale cursor %d %d", got, result.error);

	/* A text: its state on the events comes before the answer's pushed. */
	error = kl_backend_phone_send(phone, "+819012345678", (const uint8_t *)"hi", 2U, &id);
	check(error == 0, "send %d", error);
	(void)step(phone);
	requests = fake_take("PHONE SEND to=\"+819012345678\" length=2", line, sizeof(line), body, 2U);
	check(requests >= 0 && memcmp(body, "hi", 2U) == 0, "the send's line and text");
	fake_write(events, "PHONE SENT request=77 handle=0000000a.0000000000000003 state=sent\n");
	(void)step(phone);
	fake_write(requests, "PHONE SENT request=77 handle=0000000a.0000000000000003 state=pushed\nDONE\n");
	got = kl_backend_phone_take_sent(phone, &sent_request, &sent_state);
	check(got && sent_request == 77U && sent_state == KL_BACKEND_PHONE_SENT, "sent state first: %d %u %u", got, sent_request, sent_state);
	got = wait_result(phone, id, &result);
	check(got && result.error == 0 && result.sent_request == 77U, "send result %d %d %u", got, result.error, result.sent_request);

	/* The arguments refused. */
	error = kl_backend_phone_send(phone, "+81", (const uint8_t *)"", 0U, &id);
	check(error == EINVAL, "send empty %d", error);
	error = kl_backend_phone_send(phone, "090-1234", (const uint8_t *)"a", 1U, &id);
	check(error == EINVAL, "send a number with '-' %d", error);
	error = kl_backend_phone_send(phone, "090", (const uint8_t *)"a\0b", 3U, &id);
	check(error == EINVAL, "send a NUL %d", error);
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 0U, "", 33U, &id);
	check(error == EINVAL, "page 33 %d", error);
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 501U, "", 1U, &id);
	check(error == EINVAL, "page limit 501 %d", error);
	error = kl_backend_phone_page(phone, 1U, 0, 0U, "", 1U, &id);
	check(error == EINVAL, "page contacts %d", error);
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 0U, "a b", 1U, &id);
	check(error == EINVAL, "page cursor with a space %d", error);
	error = kl_backend_phone_read(phone, "a b", &id);
	check(error == EINVAL, "read handle with a space %d", error);
	error = kl_backend_phone_link_set(phone, "AA:BB", 1U, 1U, &id);
	check(error == EINVAL, "link bad address %d", error);

	/* The pages full: four, the fifth busy; then an answer overdue gives up the first and the next goes on a new connection. */
	for (index = 0; index < 4; index++) {
		error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 0U, "", 1U, &ids[index]);
		check(error == 0, "page %d of four %d", index, error);
	}
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_MESSAGES, 0, 0U, "", 1U, &id);
	check(error == EBUSY, "the fifth page %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE messages since=0 limit=0 count=1", line, sizeof(line), NULL, 0U);
	check(pages >= 0, "the first of four written");
	test_clock += 120001U;
	got = wait_result(phone, ids[0], &result);
	check(got && result.error == ETIMEDOUT, "overdue %d %d", got, result.error);
	check(fake.clients[pages].fd < 0, "the overdue connection was closed");
	pages = fake_take("PHONE PAGE messages since=0 limit=0 count=1", line, sizeof(line), NULL, 0U);
	check(pages >= 0, "the second written on a new connection");

	/* The page's connection lost: every page waiting fails. */
	fake_close(pages);
	for (index = 1; index < 4; index++) {
		got = wait_result(phone, ids[index], &result);
		check(got && result.error == ECONNRESET, "lost page %d: %d %d", index, got, result.error);
	}

	/* DROPPED on the events. */
	fake_write(events, "PHONE DROPPED\n");
	changed = step(phone);
	check((changed & KL_BACKEND_PHONE_CHANGED_DROPPED) != 0U, "dropped told");

	/* PHONE LINK. */
	error = kl_backend_phone_link_set(phone, "AA:BB:CC:DD:EE:FF", 1U, KL_BACKEND_PHONE_PROFILE_MESSAGES | KL_BACKEND_PHONE_PROFILE_CONTACTS, &id);
	check(error == 0, "link %d", error);
	(void)step(phone);
	requests = fake_take("PHONE LINK AA:BB:CC:DD:EE:FF on profiles=m,c", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "the link's line");
	fake_write(requests, "DONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == 0, "link result %d %d", got, result.error);

	/* A text longer than bluetoothd sends: the connection is made again, the request lost. */
	error = kl_backend_phone_read(phone, "0000000a.0000000000000001", &id);
	check(error == 0, "read %d", error);
	(void)step(phone);
	requests = fake_take("PHONE READ handle=0000000a.0000000000000001", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "the read's line");
	fake_write(requests, "PHONE MESSAGE handle=x key=- folder=inbox dir=in time=0 zone=phone datetime=\"\" peer=\"\" name=\"\" read=0 partial=1 truncated=0 length=16385\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == ECONNRESET, "too long a text %d %d", got, result.error);

	/* The events refused: SHOW of another user's record says not-owner; then this user's asks for them again. */
	fake_close(events);
	(void)step(phone);
	test_clock += 1001U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "SUBSCRIBE again after a second");
	fake_write(events, "ERROR permission\nDONE\n");
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "SHOW after the refusal");
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF mine=0 enabled=1\nDONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.subscribed == 0U && strcmp(state.why, "not-owner") == 0 && state.enabled == 1U && !state.linked, "not the owner: %u %s", state.subscribed, state.why);
	test_clock += 29000U;
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests < 0, "no SHOW before thirty seconds");
	test_clock += 1001U;
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "SHOW after thirty seconds");
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m present=1 link=ready messages=connecting send=0 notify=0\nDONE\n");
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "SUBSCRIBE when the record became this user's");

	/* Busy: asked again after ten seconds, not before. */
	fake_write(events, "ERROR busy\nDONE\n");
	(void)step(phone);
	test_clock += 9000U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events < 0, "no SUBSCRIBE before ten seconds");
	test_clock += 1001U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "SUBSCRIBE after ten seconds");

	/* No record: refused, then SHOW with no line. */
	fake_write(events, "ERROR permission\nDONE\n");
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "SHOW after the second refusal");
	fake_write(requests, "DONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.have_record == 0U && strcmp(state.why, "no-record") == 0, "no record: %u %s", state.have_record, state.why);

	/* A refresh asks for the events at once. */
	kl_backend_phone_refresh(phone);
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "SUBSCRIBE after a refresh");
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "SHOW after a refresh");
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m present=1 link=ready messages=ready send=1 notify=1\nDONE\n");
	fake_write(events, "DONE\n" STATE_READY);
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.subscribed && state.have_record && state.why[0] == '\0', "followed again: %u %u %s", state.subscribed, state.have_record, state.why);

	/* A request lost when the daemon goes, then unreachable. */
	error = kl_backend_phone_read(phone, "0000000a.0000000000000002", &id);
	check(error == 0, "read 2 %d", error);
	(void)step(phone);
	requests = fake_take("PHONE READ", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "the second read's line");
	fake_stop();
	got = wait_result(phone, id, &result);
	check(got && result.error == ECONNRESET, "daemon gone during a read %d %d", got, result.error);
	test_clock += 1001U;
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.reachable == 0U && strcmp(state.why, "unreachable") == 0, "gone: %u %s", state.reachable, state.why);
	error = kl_backend_phone_send(phone, "1", (const uint8_t *)"a", 1U, &id);
	check(error == ENOTCONN, "gone: send %d", error);

	/* A text whole of the most a text sent may be is taken (the line and 8192 bytes). */
	big = malloc(KL_BACKEND_PHONE_SEND_MAX + 1U);
	memset(big, 'x', KL_BACKEND_PHONE_SEND_MAX + 1U);
	phone->state.reachable = 1U;
	error = kl_backend_phone_send(phone, "1", (const uint8_t *)big, KL_BACKEND_PHONE_SEND_MAX + 1U, &id);
	check(error == EINVAL, "a text too long %d", error);
	error = kl_backend_phone_send(phone, "1", (const uint8_t *)big, KL_BACKEND_PHONE_SEND_MAX, &id);
	check(error == 0, "the longest text %d", error);
	free(big);

	kl_backend_phone_close(phone);
	if (failures != 0) {
		printf("phone-backend-host-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("phone-backend-host-test: PASS\n");
	return 0;
}
