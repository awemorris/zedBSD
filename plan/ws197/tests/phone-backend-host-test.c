/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the desktop's phone backend (ws197-p004a,
 * plan/ws197/phase004/phase.md section 12; ws197-p005,
 * plan/ws197/phase005/phase.md section 9.1): libkeiland-backend-zedbsd's
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
 *
 * ws197-p005: whether the record is known (unknown until SHOW answers,
 * again unknown while bluetoothd is gone and after it is back until SHOW
 * says, not known from a SHOW that failed); PHONE LINK off names no
 * profiles; the pages of the contacts and the calls (their lines, a
 * contact's reduced vCard, a call's length=0 with no text, the item's
 * what, the kinds, the zones' words and partial, capped's bits); the
 * contacts' state and why on the events, and none for another user's
 * record, no record or bluetoothd gone.
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

/* How many connections the fake daemon keeps. */
#define FAKE_CLIENTS	8

/* One connection the fake daemon accepted: its socket (-1 when none) and what it read but not taken. */
struct fake_client {
	int fd;
	char input[32768];
	size_t used;
};

/* The fake daemon: its listener (-1 when none) and its clients, for the test's life. */
static struct {
	int listener;
	struct fake_client clients[FAKE_CLIENTS];
} fake;

/* How many checks failed. */
static int failures;

static void check(int good, const char *format, ...);
static void fake_start(void);
static void fake_close(int client);
static void fake_stop(void);
static void fake_pump(void);
static unsigned step(struct kl_backend_phone *phone);
static int fake_take(const char *prefix, char *line, size_t size, char *body, size_t length);
static void fake_write(int client, const char *format, ...);
static int wait_result(struct kl_backend_phone *phone, uint32_t id, struct kl_backend_phone_result *result);
static void test_pbap(void);
static void test_pbap_pages(struct kl_backend_phone *phone);

/* Gives the test's clock. */
static uint64_t
test_now(
	void)
{
	/* The milliseconds the test set. */
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

	/* Passed. */
	if (good)
		return;

	/* Failed: counted and told. */
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
	int status;

	/* The socket at the test's path. */
	fake.listener = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", test_socket);
	(void)unlink(test_socket);

	/* Bound. */
	status = bind(fake.listener, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		perror("fake bind");
		exit(2);
	}

	/* Listening. */
	status = listen(fake.listener, 8);
	if (status != 0) {
		perror("fake listen");
		exit(2);
	}

	/* Never waiting. */
	flags = fcntl(fake.listener, F_GETFL);
	(void)fcntl(fake.listener, F_SETFL, flags | O_NONBLOCK);
}

/* Closes one client of the fake daemon. */
static void
fake_close(
	int client)
{
	/* Its socket, and nothing read. */
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

	/* Each client. */
	for (index = 0; index < FAKE_CLIENTS; index++)
		fake_close(index);

	/* The listener and its path. */
	(void)close(fake.listener);
	fake.listener = -1;
	(void)unlink(test_socket);
}

/* Accepts the new clients and reads what each has written. */
static void
fake_pump(
	void)
{
	struct fake_client *client;
	ssize_t count;
	int descriptor;
	int index;
	int flags;

	/* Each new connection, given a free client (closed when none is). */
	for (;;) {
		if (fake.listener < 0)
			break;
		descriptor = accept(fake.listener, NULL, NULL);
		if (descriptor < 0)
			break;

		/* Never waiting. */
		flags = fcntl(descriptor, F_GETFL);
		(void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);

		/* A free client. */
		for (index = 0; index < FAKE_CLIENTS; index++) {
			if (fake.clients[index].fd < 0) {
				fake.clients[index].fd = descriptor;
				fake.clients[index].used = 0U;
				break;
			}
		}

		/* None free. */
		if (index == FAKE_CLIENTS)
			(void)close(descriptor);
	}

	/* What each client wrote. */
	for (index = 0; index < FAKE_CLIENTS; index++) {
		client = &fake.clients[index];

		/* As much as there is room for; an end closes it. */
		while (client->fd >= 0 && client->used < sizeof(client->input)) {
			count = recv(client->fd, client->input + client->used, sizeof(client->input) - client->used, MSG_DONTWAIT);
			if (count <= 0) {
				if (count == 0)
					fake_close(index);
				break;
			}

			/* Kept. */
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

	/* Four rounds, the changes gathered. */
	all = 0U;
	for (round = 0; round < 4; round++) {
		changed = 0U;
		(void)kl_backend_phone_update(phone, &changed);
		all |= changed;
		fake_pump();
	}

	/* The changes. */
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
	int differs;

	/* Each client with something read. */
	for (index = 0; index < FAKE_CLIENTS; index++) {
		client = &fake.clients[index];
		if (client->fd < 0 || client->used == 0U)
			continue;

		/* Its next line starts so. */
		differs = strncmp(client->input, prefix, strlen(prefix));
		if (differs != 0)
			continue;

		/* A whole line, and its body when one is asked. */
		end = memchr(client->input, '\n', client->used);
		if (end == NULL)
			continue;
		taken = (size_t)(end - client->input) + 1U;
		if (body != NULL && client->used < taken + length)
			continue;

		/* The line and the body, taken off what was read. */
		(void)snprintf(line, size, "%.*s", (int)(taken - 1U), client->input);
		if (body != NULL) {
			memcpy(body, client->input + taken, length);
			taken += length;
		}

		/* The rest moves up. */
		memmove(client->input, client->input + taken, client->used - taken);
		client->used -= taken;
		return index;
	}

	/* None. */
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

	/* A client that is there. */
	if (client < 0 || fake.clients[client].fd < 0)
		return;

	/* The words, sent. */
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
	int taken;

	/* Twenty steps at most. */
	for (round = 0; round < 20; round++) {
		(void)step(phone);

		/* Each result that came, until the one asked. */
		for (;;) {
			taken = kl_backend_phone_take_result(phone, result);
			if (!taken)
				break;
			if (result->id == id)
				return 1;
		}
	}

	/* Not in time. */
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

	/* A folder for the socket. */
	if (argc != 2) {
		fprintf(stderr, "usage: phone-backend-host-test FOLDER\n");
		return 2;
	}

	/* The socket's path, and no client nor listener yet. */
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

	/* MMS type crosses the existing item field without changing its structure. */
	fake_write(events, "PHONE MESSAGE handle=0000000a.00000000000000fd key=- folder=sent dir=out time=7 zone=phone type=mms datetime=\"\" peer=\"5\" name=\"\" read=1 partial=1 truncated=0 length=4\ntext");
	changed = step(phone);
	check((changed & KL_BACKEND_PHONE_CHANGED_ITEM) != 0U, "MMS item delivered");
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.folder == (KL_BACKEND_PHONE_FOLDER_SENT | KL_BACKEND_PHONE_FOLDER_MMS), "MMS folder/type preserved");

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
	error = kl_backend_phone_page(phone, 3U, 0, 0U, "", 1U, &id);
	check(error == EINVAL, "page what 3 %d", error);
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

	/* The fifth. */
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

	/* Done with this backend. */
	kl_backend_phone_close(phone);

	/* ws197-p005's checks on a new backend and daemon. */
	test_pbap();

	/* The outcome. */
	if (failures != 0) {
		printf("phone-backend-host-test: FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("phone-backend-host-test: PASS\n");
	return 0;
}

/*
 * ws197-p005: whether the record is known, PHONE LINK off, the contacts'
 * state, and (test_pbap_pages) the pages of the contacts and the calls,
 * on a new backend and a new fake daemon.
 */
static void
test_pbap(
	void)
{
	struct kl_backend_phone_state state;
	struct kl_backend_phone_result result;
	struct kl_backend_phone *phone;
	char line[4096];
	uint32_t id;
	int events;
	int requests;
	int differs;
	int error;
	int got;

	/* A daemon and a new backend: reachable at once, whether there is a record not known yet (review-3 R2). */
	fake_start();
	phone = kl_backend_phone_open();
	check(phone != NULL, "pbap: open");
	if (phone == NULL)
		return;
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.reachable == 1U && state.record_known == 0U && state.have_record == 0U, "pbap: new: reachable %u known %u record %u", state.reachable,
	    state.record_known, state.have_record);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(events >= 0 && requests >= 0, "pbap: SUBSCRIBE and SHOW asked");

	/* SUBSCRIBE refused and SHOW failed: still not known. */
	fake_write(events, "ERROR permission\nDONE\n");
	fake_write(requests, "ERROR busy\nDONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.record_known == 0U, "pbap: a SHOW that failed: known %u", state.record_known);

	/* The next SHOW says there is none: known, none, no contacts. */
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(requests >= 0, "pbap: SHOW again after the refusal");
	fake_write(requests, "DONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.record_known == 1U && state.have_record == 0U && state.contacts == 0U && strcmp(state.why, "no-record") == 0,
	    "pbap: no record: known %u record %u contacts %u why %s", state.record_known, state.have_record, state.contacts, state.why);

	/* The record is this user's (SHOW first): the contacts' state as SHOW tells it. */
	kl_backend_phone_refresh(phone);
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(events >= 0 && requests >= 0, "pbap: SUBSCRIBE and SHOW after a refresh");
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=connecting\nDONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.record_known == 1U && state.have_record == 1U && state.contacts == KL_BACKEND_PHONE_MESSAGES_CONNECTING && state.contacts_why[0] == '\0',
	    "pbap: SHOW's contacts %u why %s", state.contacts, state.contacts_why);

	/* The events: the contacts failed for want of the phone's permission (the messages' why is apart). */
	fake_write(events, "DONE\nPHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=failed contacts_why=permission\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.subscribed == 1U && state.contacts == KL_BACKEND_PHONE_MESSAGES_FAILED && strcmp(state.contacts_why, "permission") == 0 &&
	    state.why[0] == '\0' && state.profiles == (KL_BACKEND_PHONE_PROFILE_MESSAGES | KL_BACKEND_PHONE_PROFILE_CONTACTS),
	    "pbap: STATE's contacts %u why %s messages' why %s", state.contacts, state.contacts_why, state.why);

	/* Then ready: no why. */
	fake_write(events, "PHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=ready\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.contacts == KL_BACKEND_PHONE_MESSAGES_READY && state.contacts_why[0] == '\0', "pbap: ready: %u %s", state.contacts, state.contacts_why);

	/* PHONE LINK off names no profiles (bluetoothd keeps the record's, review-3 minor 7). */
	error = kl_backend_phone_link_set(phone, "AA:BB:CC:DD:EE:FF", 0U, KL_BACKEND_PHONE_PROFILE_MESSAGES | KL_BACKEND_PHONE_PROFILE_CONTACTS, &id);
	check(error == 0, "pbap: link off %d", error);
	(void)step(phone);
	requests = fake_take("PHONE LINK ", line, sizeof(line), NULL, 0U);
	differs = strcmp(line, "PHONE LINK AA:BB:CC:DD:EE:FF off");
	check(requests >= 0 && differs == 0, "pbap: the line of off [%s]", line);
	fake_write(requests, "DONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == 0, "pbap: link off's result %d %d", got, result.error);

	/* The pages of the contacts and the calls. */
	test_pbap_pages(phone);

	/* Another user's record (the events refused, SHOW seen in part): no contacts. */
	fake_close(events);
	(void)step(phone);
	test_clock += 1001U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	fake_write(events, "ERROR permission\nDONE\n");
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(events >= 0 && requests >= 0, "pbap: SUBSCRIBE refused, SHOW");
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF mine=0 enabled=0\nDONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.contacts == 0U && state.contacts_why[0] == '\0' && strcmp(state.why, "not-owner") == 0 && state.record_known == 1U && state.have_record == 1U,
	    "pbap: not the owner: contacts %u why %s known %u record %u", state.contacts, state.why, state.record_known, state.have_record);

	/* The owner's again (SHOW after thirty seconds, then the events), the contacts ready. */
	test_clock += 30001U;
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=ready\nDONE\n");
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(requests >= 0 && events >= 0, "pbap: SHOW of the owner's, SUBSCRIBE");
	fake_write(events, "DONE\nPHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=ready\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.subscribed == 1U && state.contacts == KL_BACKEND_PHONE_MESSAGES_READY, "pbap: followed again: %u %u", state.subscribed, state.contacts);

	/* The record forgotten (the events refused, SHOW with no line): no contacts. */
	fake_close(events);
	(void)step(phone);
	test_clock += 1001U;
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	fake_write(events, "ERROR permission\nDONE\n");
	(void)step(phone);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	check(events >= 0 && requests >= 0, "pbap: SUBSCRIBE refused, SHOW of none");
	fake_write(requests, "DONE\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.contacts == 0U && state.contacts_why[0] == '\0' && state.record_known == 1U && state.have_record == 0U,
	    "pbap: forgotten: contacts %u known %u record %u", state.contacts, state.record_known, state.have_record);

	/* This user's phone again, followed, the contacts ready. */
	kl_backend_phone_refresh(phone);
	(void)step(phone);
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	requests = fake_take("PHONE SHOW", line, sizeof(line), NULL, 0U);
	fake_write(requests, "PHONE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=ready\nDONE\n");
	(void)step(phone);
	fake_write(events, "DONE\nPHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=ready\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.subscribed == 1U && state.contacts == KL_BACKEND_PHONE_MESSAGES_READY && state.record_known == 1U, "pbap: back: %u %u %u", state.subscribed,
	    state.contacts, state.record_known);

	/* bluetoothd gone: unreachable, nothing known, no contacts. */
	fake_stop();
	(void)step(phone);
	test_clock += 1001U;
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.reachable == 0U && state.record_known == 0U && state.contacts == 0U && state.contacts_why[0] == '\0',
	    "pbap: gone: reachable %u known %u contacts %u", state.reachable, state.record_known, state.contacts);

	/* bluetoothd back: reachable, but whether there is a record is not known until it says (review-3 R2). */
	fake_start();
	test_clock += 1001U;
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.reachable == 1U && state.record_known == 0U && state.have_record == 0U, "pbap: back, not said: reachable %u known %u record %u",
	    state.reachable, state.record_known, state.have_record);

	/* The events say: known, one. */
	events = fake_take("PHONE SUBSCRIBE", line, sizeof(line), NULL, 0U);
	check(events >= 0, "pbap: SUBSCRIBE when bluetoothd is back");
	fake_write(events, "DONE\nPHONE STATE address=AA:BB:CC:DD:EE:FF owner=1000 mine=1 enabled=1 profiles=m,c present=1 link=ready messages=ready send=1 notify=1 contacts=connecting\n");
	(void)step(phone);
	kl_backend_phone_get_state(phone, &state);
	check(state.record_known == 1U && state.have_record == 1U && state.contacts == KL_BACKEND_PHONE_MESSAGES_CONNECTING, "pbap: said: known %u record %u contacts %u",
	    state.record_known, state.have_record, state.contacts);

	/* Done with the daemon and the backend. */
	kl_backend_phone_close(phone);
	fake_stop();
}

/*
 * ws197-p005: the pages of the contacts (a contact with its reduced vCard,
 * capped's bits, the cursor, a stale one) and of the calls (each kind, the
 * zones' words, partial, no text), and a kind of page not known.
 */
static void
test_pbap_pages(
	struct kl_backend_phone *phone)
{
	static const char card[] = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Kei\r\nTEL:+819011112222\r\nTEL:0312345678\r\nEND:VCARD\r\n";
	struct kl_backend_phone_result result;
	struct kl_backend_phone_item item;
	char line[4096];
	uint32_t id;
	int pages;
	int differs;
	int error;
	int got;

	/* The contacts from the start: since and the limit are not written. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_CONTACTS, 0, 0U, "", 32U, &id);
	check(error == 0, "contacts: page %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE ", line, sizeof(line), NULL, 0U);
	differs = strcmp(line, "PHONE PAGE contacts count=32");
	check(pages >= 0 && differs == 0, "contacts: the line [%s]", line);

	/* A contact whose name poses as a field, its card, the page's end with capped 2 and 4. */
	fake_write(pages, "PHONE CONTACT key=00000000000000a1 tels=2 length=%u peer=\"+819011112222\" name=\"Kei length=3\"\n%s", (unsigned)strlen(card), card);
	fake_write(pages, "PHONE PAGE-END cursor=0a0b0c0d.0.33.1f4 more=1 count=1 skipped=0 capped=6\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == 0 && strcmp(result.cursor, "0a0b0c0d.0.33.1f4") == 0 && result.more == 1U && result.count == 1U && result.capped == 6U,
	    "contacts: the end %d %d %s %u %u %u", got, result.error, result.cursor, result.more, result.count, result.capped);
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.what == KL_BACKEND_PHONE_WHAT_CONTACTS && item.id == id && strcmp(item.key, "00000000000000a1") == 0 && item.folder == 2U,
	    "contacts: the item %d what %u key %s tels %u", got, item.what, item.key, item.folder);
	check(strcmp(item.peer, "+819011112222") == 0 && strcmp(item.name, "Kei length=3") == 0 && item.length == strlen(card) && strcmp(item.text, card) == 0,
	    "contacts: peer %s name [%s] length %zu", item.peer, item.name, item.length);

	/* The next page from the cursor, which is stale. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_CONTACTS, 0, 0U, "0a0b0c0d.0.33.1f4", 16U, &id);
	check(error == 0, "contacts: page 2 %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE ", line, sizeof(line), NULL, 0U);
	differs = strcmp(line, "PHONE PAGE contacts cursor=0a0b0c0d.0.33.1f4 count=16");
	check(pages >= 0 && differs == 0, "contacts: the line with the cursor [%s]", line);
	fake_write(pages, "ERROR stale-cursor\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == ESTALE, "contacts: stale %d %d", got, result.error);

	/* The calls since a time: no limit written. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_CALLS, 1700000000, 0U, "", 32U, &id);
	check(error == 0, "calls: page %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE ", line, sizeof(line), NULL, 0U);
	differs = strcmp(line, "PHONE PAGE calls since=1700000000 count=32");
	check(pages >= 0 && differs == 0, "calls: the line [%s]", line);

	/* A call received (the phone's zone), one dialed (this computer's), one missed with no time, one with no time but partial=0. */
	fake_write(pages, "PHONE CALL-LOG key=00000000000000b1 kind=received time=1704110400 zone=phone partial=0 length=0 datetime=\"20240101T120000Z\" peer=\"+819011112222\" name=\"Kei\"\n");
	fake_write(pages, "PHONE CALL-LOG key=00000000000000b2 kind=dialed time=1704078000 zone=local partial=0 length=0 datetime=\"20240101T120000\" peer=\"0312345678\" name=\"\"\n");
	fake_write(pages, "PHONE CALL-LOG key=00000000000000b3 kind=missed time=1791000000 zone=none partial=1 length=0 datetime=\"\" peer=\"\" name=\"\"\n");
	fake_write(pages, "PHONE CALL-LOG key=00000000000000b4 kind=received time=1791000001 zone=none partial=0 length=0 datetime=\"x\" peer=\"1\" name=\"\"\n");
	fake_write(pages, "PHONE PAGE-END cursor=0a0b0c0d.3.4.ffffffff more=0 count=4 skipped=1 capped=1\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == 0 && result.more == 0U && result.count == 4U && result.skipped == 1U && result.capped == 1U, "calls: the end %d %d %u %u %u %u", got,
	    result.error, result.more, result.count, result.skipped, result.capped);

	/* Received, the phone's zone. */
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.what == KL_BACKEND_PHONE_WHAT_CALLS && item.id == id && item.folder == KL_BACKEND_PHONE_CALL_RECEIVED &&
	    item.direction == KL_BACKEND_PHONE_DIRECTION_IN && item.time == 1704110400 && item.zone == KL_BACKEND_PHONE_ZONE_PHONE && item.partial == 0U,
	    "calls: received: %u %u %u %lld %u %u", item.what, item.folder, item.direction, (long long)item.time, item.zone, item.partial);
	check(strcmp(item.key, "00000000000000b1") == 0 && strcmp(item.datetime, "20240101T120000Z") == 0 && strcmp(item.peer, "+819011112222") == 0 &&
	    strcmp(item.name, "Kei") == 0 && item.length == 0U && item.text[0] == '\0', "calls: received's fields");

	/* Dialed, this computer's zone. */
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.folder == KL_BACKEND_PHONE_CALL_DIALED && item.direction == KL_BACKEND_PHONE_DIRECTION_OUT && item.zone == KL_BACKEND_PHONE_ZONE_LOCAL &&
	    item.partial == 0U && strcmp(item.peer, "0312345678") == 0, "calls: dialed: %u %u %u %u", item.folder, item.direction, item.zone, item.partial);

	/* Missed with no time: when it came, partial. */
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.folder == KL_BACKEND_PHONE_CALL_MISSED && item.direction == KL_BACKEND_PHONE_DIRECTION_IN && item.zone == KL_BACKEND_PHONE_ZONE_RECEIVED &&
	    item.partial == 1U && item.time == 1791000000 && item.peer[0] == '\0', "calls: missed: %u %u %u %u", item.folder, item.direction, item.zone, item.partial);

	/* No time but partial=0: partial all the same. */
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 1 && item.zone == KL_BACKEND_PHONE_ZONE_RECEIVED && item.partial == 1U, "calls: none is partial: %u %u", item.zone, item.partial);
	got = kl_backend_phone_take_item(phone, &item);
	check(got == 0, "calls: four items only");

	/* The calls from a cursor, the phone not ready. */
	error = kl_backend_phone_page(phone, KL_BACKEND_PHONE_WHAT_CALLS, 1700000000, 0U, "0a0b0c0d.1.0.ffffffff", 32U, &id);
	check(error == 0, "calls: page 2 %d", error);
	(void)step(phone);
	pages = fake_take("PHONE PAGE ", line, sizeof(line), NULL, 0U);
	differs = strcmp(line, "PHONE PAGE calls since=1700000000 cursor=0a0b0c0d.1.0.ffffffff count=32");
	check(pages >= 0 && differs == 0, "calls: the line with the cursor [%s]", line);
	fake_write(pages, "ERROR not-ready\nDONE\n");
	got = wait_result(phone, id, &result);
	check(got && result.error == ENOTCONN, "calls: not ready %d %d", got, result.error);

	/* A kind of page not known. */
	error = kl_backend_phone_page(phone, 3U, 0, 0U, "", 1U, &id);
	check(error == EINVAL, "page what 3 %d", error);
}
