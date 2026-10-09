/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws170-p004: the host test of the phone on the wire, both ends apart.
 *
 * 1. The compositor's phone-shell.c with two capture clients: without a
 *    backend (phone.backend 0) a send and a call are UNAVAILABLE; with the
 *    loopback backend a send is taken, sent and delivered, and comes back
 *    as "Echo: ..." from the same number to both clients' phone objects;
 *    a call is not answered; a channel of the wrong kind is INVALID; a
 *    malformed request ends the client.
 * 2. libkeiland's view (system-view.c): the ring of phone events keeps
 *    the newest sixteen and tells KL_SYSTEM_CHANGED_PHONE.
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

/* The most events the capture clients keep, and the largest one. */
#define TEST_EVENTS_MAX		16U
#define TEST_EVENT_BYTES	2048U

/* One event a capture client was sent. */
struct test_event {
	uint64_t client;
	uint32_t object;
	uint32_t opcode;
	unsigned char payload[TEST_EVENT_BYTES];
	size_t size;
};

/* The events sent to the capture clients, in order. */
static struct test_event test_events[TEST_EVENTS_MAX];
static unsigned test_event_count;

/* The fake desktop's phone.backend: -1 for no settings, else its number. */
static int test_backend = 0;

/* The checks that failed. */
static int test_failures;

int main(void);
static void test_check(const char *name, int passed, const char *detail);
static size_t test_put_string(unsigned char *bytes, size_t offset, const char *text);
static size_t test_put_word(unsigned char *bytes, size_t offset, uint32_t word);
static const char *test_get_string(const struct test_event *event, size_t offset, size_t *next);
static uint32_t test_get_word(const struct test_event *event, size_t offset);
static int test_send(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to, const char *text);
static int test_call(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to);
static unsigned test_count(uint32_t opcode);

/* The capture client's events (kwl_emit of the compositor). */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	/* Kept while there is room. */
	if (test_event_count >= TEST_EVENTS_MAX || size > TEST_EVENT_BYTES)
		return ENOSPC;
	test_events[test_event_count].client = client->number;
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

/* The compositor's clock (ws197-p004a's phone-shell.c reads it): the test's time stands still. */
uint64_t
kwl_milliseconds(
	void)
{
	/* A fixed time. */
	return 1000U;
}

/* The fake desktop's settings: only phone.backend exists. */
int
kwl_settings_number(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	int same;

	(void)server;

	/* No settings (the login screen). */
	if (test_backend < 0)
		return ENOENT;

	/* The one setting. */
	same = strcmp(name, "phone.backend");
	if (same != 0)
		return ENOENT;

	/* Its number. */
	*number = test_backend;
	return 0;
}

/*
 * Drives phone-shell.c and the view's ring and checks what they did.
 */
int
main(void)
{
	static struct kwl_server server;
	static struct kwl_client first;
	static struct kwl_client second;
	struct system_view view;
	struct kl_phone_event taken;
	struct kl_phone_event event;
	struct kwl_object manager;
	struct kwl_object *phone;
	struct kwl_object *other;
	unsigned char bytes[64];
	const char *from;
	const char *text;
	size_t offset;
	unsigned index;
	unsigned changed;
	int got;
	int error;

	/* Two clients of the user. */
	first.server = &server;
	first.number = 1U;
	first.fd = -1;
	second.server = &server;
	second.number = 2U;
	second.fd = -1;
	first.next = &second;
	server.clients = &first;

	/* A phone object for each. */
	memset(&manager, 0, sizeof(manager));
	manager.version = KL_SYSTEM_SINCE_PHONE;
	manager.client = &first;
	(void)test_put_word(bytes, 0U, 10U);
	error = kwl_phone_create(&manager, bytes, 4U);
	phone = kwl_find(&first, 10U);
	manager.client = &second;
	(void)test_put_word(bytes, 0U, 20U);
	error |= kwl_phone_create(&manager, bytes, 4U);
	other = kwl_find(&second, 20U);
	test_check("create", error == 0 && phone != NULL && other != NULL && phone->kind == KWL_SYSTEM_PHONE, "");
	if (phone == NULL || other == NULL)
		return 1;

	/* No backend: unavailable. */
	test_backend = 0;
	test_event_count = 0U;
	error = test_send(phone, 1U, KL_PHONE_SMS, "+15550100", "hi");
	test_check("send-no-backend", error == 0 && test_event_count == 1U &&
	    test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_RESULT &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_UNAVAILABLE, "");
	test_event_count = 0U;
	error = test_call(phone, 2U, KL_PHONE_LINE, "+15550100");
	test_check("call-no-backend", error == 0 && test_event_count == 1U && test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_UNAVAILABLE, "");

	/* Loopback: taken, sent, delivered, and back to both clients. */
	test_backend = 1;
	test_event_count = 0U;
	error = test_send(phone, 3U, KL_PHONE_RCS, "+15550100", "Hello there");
	from = NULL;
	text = NULL;
	if (test_event_count == 5U) {
		from = test_get_string(&test_events[3], 4U, &offset);
		text = test_get_string(&test_events[3], offset, &offset);
	}
	test_check("send-loopback", error == 0 && test_event_count == 5U &&
	    test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_RESULT && test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_OK &&
	    test_events[1].opcode == KL_SYSTEM_PHONE_EVENT_STATUS && test_get_word(&test_events[1], 4U) == KL_SYSTEM_PHONE_SENT &&
	    test_events[2].opcode == KL_SYSTEM_PHONE_EVENT_STATUS && test_get_word(&test_events[2], 4U) == KL_SYSTEM_PHONE_DELIVERED, "");
	test_check("echo", test_count(KL_SYSTEM_PHONE_EVENT_RECEIVED) == 2U &&
	    test_get_word(&test_events[3], 0U) == KL_PHONE_RCS &&
	    from != NULL && strcmp(from, "+15550100") == 0 &&
	    text != NULL && strcmp(text, "Echo: Hello there") == 0 &&
	    test_events[3].client != test_events[4].client, text != NULL ? text : "");

	/* A call: not answered. */
	test_event_count = 0U;
	error = test_call(phone, 4U, KL_PHONE_VOIP, "+15550100");
	test_check("call-loopback", error == 0 && test_event_count == 2U &&
	    test_get_word(&test_events[1], 0U) == 4U && test_get_word(&test_events[1], 4U) == KL_SYSTEM_PHONE_NO_ANSWER, "");

	/* A message on a call's channel, and a call on a message's: invalid. */
	test_event_count = 0U;
	(void)test_send(phone, 5U, KL_PHONE_LINE, "+1", "x");
	(void)test_call(phone, 6U, KL_PHONE_SMS, "+1");
	test_check("wrong-channel", test_event_count == 2U &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_INVALID &&
	    test_get_word(&test_events[1], 4U) == KL_SYSTEM_RESULT_INVALID, "");

	/* A call missing its number ends the client. */
	(void)test_put_word(bytes, 0U, 7U);
	(void)test_put_word(bytes, 4U, KL_PHONE_LINE);
	error = kwl_phone_request(phone, KL_SYSTEM_PHONE_CALL, bytes, 8U);
	test_check("malformed", error == EPROTO, "");

	/* The view's ring: seventeen events keep the newest sixteen and tell the change; the loss is told first (ws197-p004a). */
	memset(&view, 0, sizeof(view));
	for (index = 0; index < 17U; index++) {
		memset(&event, 0, sizeof(event));
		event.kind = KL_PHONE_STATUS;
		event.request = index;
		system_view_phone_event(&view, &event);
	}
	changed = system_view_take_changed(&view);
	got = system_view_take_phone_event(&view, &taken);
	test_check("ring-dropped", got == 1 && taken.kind == KL_PHONE_DROPPED, "");
	got = system_view_take_phone_event(&view, &taken);
	test_check("ring", got == 1 && taken.request == 1U && (changed & KL_SYSTEM_CHANGED_PHONE) != 0U, "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-phone-shell: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-phone-shell: PASS\n");
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

/* Sends send(request, channel, to, text). */
static int
test_send(
	struct kwl_object *object,
	uint32_t request,
	uint32_t channel,
	const char *to,
	const char *text)
{
	unsigned char bytes[256];
	size_t offset;

	/* The request. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_word(bytes, offset, channel);
	offset = test_put_string(bytes, offset, to);
	offset = test_put_string(bytes, offset, text);
	return kwl_phone_request(object, KL_SYSTEM_PHONE_SEND, bytes, offset);
}

/* Sends call(request, channel, to). */
static int
test_call(
	struct kwl_object *object,
	uint32_t request,
	uint32_t channel,
	const char *to)
{
	unsigned char bytes[128];
	size_t offset;

	/* The request. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_word(bytes, offset, channel);
	offset = test_put_string(bytes, offset, to);
	return kwl_phone_request(object, KL_SYSTEM_PHONE_CALL, bytes, offset);
}

/* Counts the events of an opcode. */
static unsigned
test_count(
	uint32_t opcode)
{
	unsigned count;
	unsigned index;

	/* Each event. */
	count = 0;
	for (index = 0; index < test_event_count; index++) {
		if (test_events[index].opcode == opcode)
			count++;
	}
	return count;
}
