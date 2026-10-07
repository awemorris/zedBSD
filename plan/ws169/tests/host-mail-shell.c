/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws169-p002: the host test of the arrivals of mail, both ends apart.
 *
 * 1. The compositor's mail-shell.c with a capture client: a listener the
 *    settings know is kept and one they do not is refused, an arrival is
 *    told only while the reader's setting is on, the event carries the
 *    sender, the subject and the code, a listener whose object went is
 *    skipped, and a malformed request ends the client (EPROTO).
 * 2. libkeiland's view (system-view.c): the ring of arrivals keeps the
 *    newest eight and tells KL_SYSTEM_CHANGED_MAIL.
 * 3. ws177-p005: an arrival from a client without the mail program's
 *    window is refused; a reader on a version 20 object is told allowed
 *    after its listen and when its setting changes (only then); a listen
 *    with every row taken frees the rows of readers that went; the view
 *    keeps the reader's permission, and a full ring of notification events
 *    tells how many it lost first.
 *
 * Prints "PASS name" or "FAIL name ..." for each check; exits with 1 when
 * one failed.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland/system/system-private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most events the capture client keeps, and the largest one. */
#define TEST_EVENTS_MAX		16U
#define TEST_EVENT_BYTES	1024U

/* One event the capture client was sent. */
struct test_event {
	uint32_t object;
	uint32_t opcode;
	unsigned char payload[TEST_EVENT_BYTES];
	size_t size;
};

/* The events sent to the capture client, in order. */
static struct test_event test_events[TEST_EVENTS_MAX];
static unsigned test_event_count;

/* The fake desktop's mail.codes.browser: -1 for no settings, else its number. */
static int test_codes_browser = 0;

/* The checks that failed. */
static int test_failures;

int main(void);
static void test_check(const char *name, int passed, const char *detail);
static size_t test_put_string(unsigned char *bytes, size_t offset, const char *text);
static size_t test_put_word(unsigned char *bytes, size_t offset, uint32_t word);
static const char *test_get_string(const struct test_event *event, size_t offset, size_t *next);
static uint32_t test_get_word(const struct test_event *event, size_t offset);
static int test_listen(struct kwl_object *object, uint32_t request, const char *app);
static int test_arrived(struct kwl_object *object, uint32_t request, const char *from, const char *subject, const char *code);

/* The capture client's events (kwl_emit of the compositor). */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	(void)client;

	/* Kept while there is room. */
	if (test_event_count >= TEST_EVENTS_MAX || size > TEST_EVENT_BYTES)
		return ENOSPC;
	test_events[test_event_count].object = object;
	test_events[test_event_count].opcode = opcode;
	memcpy(test_events[test_event_count].payload, payload, size);
	test_events[test_event_count].size = size;
	test_event_count++;
	return 0;
}

/* Finds a live object of a client. */
struct kwl_object *
kwl_find(
	struct kwl_client *client,
	uint32_t id)
{
	struct kwl_object *object;

	/* Each object of the client. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->id == id && !object->dead)
			return object;
	}

	/* None. */
	return NULL;
}

/* Makes an object of a client. */
struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	struct kwl_object *object;

	/* A new ID only. */
	object = kwl_find(client, id);
	if (id == 0U || object != NULL)
		return NULL;

	/* The object, first in the client's list. */
	object = calloc(1U, sizeof(*object));
	if (object == NULL)
		return NULL;
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	return object;
}

/* Marks an object dead (the test frees nothing). */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	/* Dead: never found again. */
	object->dead = 1U;
}

/* The fake desktop's settings: only mail.codes.browser exists. */
int
kwl_settings_number(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	int same;

	(void)server;

	/* No settings (the login screen). */
	if (test_codes_browser < 0)
		return ENOENT;

	/* The one setting. */
	same = strcmp(name, "mail.codes.browser");
	if (same != 0)
		return ENOENT;

	/* Its number. */
	*number = test_codes_browser;
	return 0;
}

/*
 * Drives mail-shell.c and the view's ring and checks what they did.
 */
int
main(void)
{
	static struct kwl_server server;
	static struct kwl_client mailer;
	static struct kwl_client reader;
	struct system_view view;
	struct kl_mail_event taken;
	struct kwl_object manager;
	struct kwl_object *mail_object;
	struct kwl_object *read_object;
	struct kwl_object *window;
	struct kwl_object *rows[16];
	struct kwl_object *late_object;
	struct kl_notify_event notify;
	unsigned char bytes[64];
	const char *from;
	const char *subject;
	const char *code;
	char name[32];
	size_t offset;
	unsigned index;
	unsigned changed;
	int error;
	int got;

	/* The server with two clients: the mail program and a reader. */
	mailer.server = &server;
	mailer.number = 1U;
	mailer.fd = -1;
	reader.server = &server;
	reader.number = 2U;
	reader.fd = -1;
	mailer.next = &reader;
	server.clients = &mailer;

	/* The mail program's window (ws177-p005: only it is heard telling of arrivals). */
	window = kwl_create(&mailer, 5U, KWL_SURFACE, 6U);
	if (window == NULL)
		return 1;
	(void)snprintf(window->app_id, sizeof(window->app_id), "mailer");

	/* The mail program's mail object, made by get_mail of its manager. */
	memset(&manager, 0, sizeof(manager));
	manager.client = &mailer;
	manager.version = KL_SYSTEM_SINCE_MAIL;
	(void)test_put_word(bytes, 0U, 10U);
	error = kwl_mail_create(&manager, bytes, 4U);
	mail_object = kwl_find(&mailer, 10U);
	test_check("create-mailer", error == 0 && mail_object != NULL && mail_object->kind == KWL_SYSTEM_MAIL, "");

	/* The reader's. */
	manager.client = &reader;
	(void)test_put_word(bytes, 0U, 20U);
	error = kwl_mail_create(&manager, bytes, 4U);
	read_object = kwl_find(&reader, 20U);
	test_check("create-reader", error == 0 && read_object != NULL, "");
	if (mail_object == NULL || read_object == NULL)
		return 1;

	/* A name the settings do not know is refused. */
	test_event_count = 0U;
	error = test_listen(read_object, 1U, "spy");
	test_check("listen-unknown", error == 0 && test_event_count == 1U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_RESULT &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_INVALID, "");

	/* The browser is kept. */
	test_event_count = 0U;
	error = test_listen(read_object, 2U, "browser");
	test_check("listen-browser", error == 0 && test_event_count == 1U &&
	    test_get_word(&test_events[0], 0U) == 2U &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_OK, "");

	/* The setting off: the arrival is answered but not told. */
	test_codes_browser = 0;
	test_event_count = 0U;
	error = test_arrived(mail_object, 3U, "Example Bank <no-reply@bank.example>", "Your sign-in code", "482913");
	test_check("arrived-not-allowed", error == 0 && test_event_count == 1U &&
	    test_events[0].object == 10U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_RESULT, "");

	/* The setting on: the reader hears mail(from, subject, code), then the mail program its result. */
	test_codes_browser = 1;
	test_event_count = 0U;
	error = test_arrived(mail_object, 4U, "Example Bank <no-reply@bank.example>", "Your sign-in code", "482913");
	from = NULL;
	subject = NULL;
	code = NULL;
	if (test_event_count == 2U) {
		from = test_get_string(&test_events[0], 0U, &offset);
		subject = test_get_string(&test_events[0], offset, &offset);
		code = test_get_string(&test_events[0], offset, &offset);
	}
	test_check("arrived-told", error == 0 && test_event_count == 2U &&
	    test_events[0].object == 20U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_MAIL &&
	    from != NULL && strcmp(from, "Example Bank <no-reply@bank.example>") == 0 &&
	    subject != NULL && strcmp(subject, "Your sign-in code") == 0 &&
	    code != NULL && strcmp(code, "482913") == 0 &&
	    test_events[1].object == 10U, "");

	/* No settings at all (the login screen): not told. */
	test_codes_browser = -1;
	test_event_count = 0U;
	error = test_arrived(mail_object, 5U, "a", "b", "");
	test_check("arrived-no-settings", error == 0 && test_event_count == 1U && test_events[0].object == 10U, "");

	/* The reader's object went: skipped, and its row freed. */
	test_codes_browser = 1;
	kwl_object_destroy(read_object);
	test_event_count = 0U;
	error = test_arrived(mail_object, 6U, "a", "b", "123456");
	test_check("arrived-reader-gone", error == 0 && test_event_count == 1U && test_events[0].object == 10U, "");

	/* A string without its NUL ends the client. */
	(void)test_put_word(bytes, 0U, 7U);
	(void)test_put_word(bytes, 4U, 4U);
	memcpy(bytes + 8U, "abcd", 4U);
	error = kwl_mail_request(mail_object, KL_SYSTEM_MAIL_LISTEN, bytes, 12U);
	test_check("malformed", error == EPROTO, "");

	/* An arrival missing its last string ends the client too. */
	offset = test_put_word(bytes, 0U, 8U);
	offset = test_put_string(bytes, offset, "a");
	offset = test_put_string(bytes, offset, "b");
	offset = test_put_string(bytes, offset, "c");
	error = kwl_mail_request(mail_object, KL_SYSTEM_MAIL_ARRIVED, bytes, offset);
	test_check("short-arrived", error == EPROTO, "");

	/* The view's ring: nine arrivals keep the newest eight, in order, and tell the change. */
	memset(&view, 0, sizeof(view));
	for (index = 0U; index < 9U; index++) {
		(void)snprintf(name, sizeof(name), "sender %u", index);
		system_view_mail_event(&view, name, "subject", "1234");
	}
	changed = system_view_take_changed(&view);
	got = system_view_take_mail_event(&view, &taken);
	test_check("ring-oldest-dropped", got == 1 && strcmp(taken.from, "sender 1") == 0 && (changed & KL_SYSTEM_CHANGED_MAIL) != 0U, taken.from);
	for (index = 0U; index < 7U; index++)
		got = system_view_take_mail_event(&view, &taken);
	test_check("ring-newest-last", got == 1 && strcmp(taken.from, "sender 8") == 0, taken.from);
	got = system_view_take_mail_event(&view, &taken);
	test_check("ring-empty", got == 0, "");

	/* A code longer than its room is cut. */
	system_view_mail_event(&view, "x", "y", "12345678901234567890");
	got = system_view_take_mail_event(&view, &taken);
	test_check("ring-cut", got == 1 && strlen(taken.code) == KL_MAIL_CODE_MAX - 1U, taken.code);

	/* ws177-p005: an arrival from the reader, which has no mail program's window, is refused and told to nobody. */
	manager.client = &reader;
	(void)test_put_word(bytes, 0U, 30U);
	error = kwl_mail_create(&manager, bytes, 4U);
	read_object = kwl_find(&reader, 30U);
	test_check("create-reader-again", error == 0 && read_object != NULL, "");
	if (read_object == NULL)
		return 1;
	test_codes_browser = 1;
	test_event_count = 0U;
	error = test_arrived(read_object, 9U, "a", "b", "123456");
	test_check("arrived-not-mailer", error == 0 && test_event_count == 1U &&
	    test_events[0].object == 30U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_RESULT &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_INVALID, "");

	/* A reader on a version 20 object: listen answered, then allowed(1). */
	manager.version = KL_SYSTEM_SINCE_MAIL_ALLOWED;
	(void)test_put_word(bytes, 0U, 40U);
	error = kwl_mail_create(&manager, bytes, 4U);
	late_object = kwl_find(&reader, 40U);
	test_check("create-reader-20", error == 0 && late_object != NULL, "");
	if (late_object == NULL)
		return 1;
	test_event_count = 0U;
	error = test_listen(late_object, 10U, "browser");
	test_check("allowed-after-listen", error == 0 && test_event_count == 2U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_RESULT &&
	    test_events[1].opcode == KL_SYSTEM_MAIL_EVENT_ALLOWED &&
	    test_get_word(&test_events[1], 0U) == 1U, "");

	/* The setting turned off: allowed(0); the same again: nothing; the old reader (version 15) hears nothing. */
	test_codes_browser = 0;
	test_event_count = 0U;
	kwl_mail_settings_changed(&server);
	test_check("allowed-changed", test_event_count == 1U &&
	    test_events[0].object == 40U &&
	    test_events[0].opcode == KL_SYSTEM_MAIL_EVENT_ALLOWED &&
	    test_get_word(&test_events[0], 0U) == 0U, "");
	test_event_count = 0U;
	kwl_mail_settings_changed(&server);
	test_check("allowed-unchanged", test_event_count == 0U, "");

	/* Every row taken by readers that are there: busy. */
	manager.version = KL_SYSTEM_SINCE_MAIL;
	for (index = 0U; index < 16U; index++) {
		(void)test_put_word(bytes, 0U, 100U + index);
		(void)kwl_mail_create(&manager, bytes, 4U);
		rows[index] = kwl_find(&reader, 100U + index);
		if (rows[index] != NULL)
			(void)test_listen(rows[index], 20U + index, "browser");
	}
	(void)test_put_word(bytes, 0U, 200U);
	(void)kwl_mail_create(&manager, bytes, 4U);
	test_event_count = 0U;
	error = test_listen(kwl_find(&reader, 200U), 50U, "browser");
	test_check("listen-busy", error == 0 && test_event_count == 1U &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_BUSY, "");

	/* Readers that went free their rows when a listen finds none free. */
	for (index = 0U; index < 16U; index++) {
		if (rows[index] != NULL)
			kwl_object_destroy(rows[index]);
	}
	test_event_count = 0U;
	error = test_listen(kwl_find(&reader, 200U), 51U, "browser");
	test_check("listen-swept", error == 0 && test_event_count == 1U &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_OK, "");

	/* The view keeps the reader's permission. */
	memset(&view, 0, sizeof(view));
	system_view_mail_allowed(&view, 1U);
	changed = system_view_take_changed(&view);
	test_check("view-allowed", view.mail_allowed == 2U && (changed & KL_SYSTEM_CHANGED_MAIL) != 0U, "");

	/* A full ring of notification events: the next take tells how many were lost, then the oldest kept. */
	memset(&view, 0, sizeof(view));
	memset(&notify, 0, sizeof(notify));
	for (index = 0U; index < SYSTEM_VIEW_NOTIFY_EVENTS + 3U; index++) {
		notify.kind = KL_NOTIFY_POSTED;
		notify.id = index + 1U;
		system_view_notify_event(&view, &notify);
	}
	got = system_view_take_notify_event(&view, &notify);
	test_check("notify-lost", got == 1 && notify.kind == KL_NOTIFY_LOST && notify.id == 3U, "");
	got = system_view_take_notify_event(&view, &notify);
	test_check("notify-after-lost", got == 1 && notify.kind == KL_NOTIFY_POSTED && notify.id == 4U, "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-shell: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-shell: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s %s\n", name, detail);
	test_failures++;
}

/* Writes a string argument and reports the offset after it. */
static size_t
test_put_string(
	unsigned char *bytes,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, zeros to a word. */
	length = (uint32_t)strlen(text) + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(bytes + offset, &length, sizeof(length));
	memset(bytes + offset + 4U, 0, padded);
	memcpy(bytes + offset + 4U, text, length - 1U);
	return offset + 4U + padded;
}

/* Writes a word and reports the offset after it. */
static size_t
test_put_word(
	unsigned char *bytes,
	size_t offset,
	uint32_t word)
{
	/* The word in the wire's order. */
	memcpy(bytes + offset, &word, sizeof(word));
	return offset + 4U;
}

/* Reads a string argument of an event; NULL when it does not fit. */
static const char *
test_get_string(
	const struct test_event *event,
	size_t offset,
	size_t *next)
{
	uint32_t length;

	/* The length and the bytes within the event. */
	if (offset + 4U > event->size)
		return NULL;
	length = test_get_word(event, offset);
	if (offset + 4U + length > event->size)
		return NULL;

	/* The text, and where the next starts. */
	*next = offset + 4U + (((size_t)length + 3U) & ~(size_t)3U);
	return (const char *)(event->payload + offset + 4U);
}

/* Reads a word of an event. */
static uint32_t
test_get_word(
	const struct test_event *event,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, event->payload + offset, sizeof(word));
	return word;
}

/* Sends listen(request, app). */
static int
test_listen(
	struct kwl_object *object,
	uint32_t request,
	const char *app)
{
	unsigned char bytes[64];
	size_t offset;

	/* The request. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_string(bytes, offset, app);
	return kwl_mail_request(object, KL_SYSTEM_MAIL_LISTEN, bytes, offset);
}

/* Sends arrived(request, account, from, subject, code). */
static int
test_arrived(
	struct kwl_object *object,
	uint32_t request,
	const char *from,
	const char *subject,
	const char *code)
{
	unsigned char bytes[512];
	size_t offset;

	/* The request. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_string(bytes, offset, "Personal");
	offset = test_put_string(bytes, offset, from);
	offset = test_put_string(bytes, offset, subject);
	offset = test_put_string(bytes, offset, code);
	return kwl_mail_request(object, KL_SYSTEM_MAIL_ARRIVED, bytes, offset);
}
