/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws197-p004a: the host test of the compositor's phone messages and
 * libkeiland's view of them (plan/ws197/phase004/phase.md section 12).
 *
 * 1. The compositor's phone-shell.c with capture clients and a fake
 *    kl_backend_phone (this file): the app_id gate (listen answers why
 *    "not-phone", a sync of another program is REFUSED, watch_link and
 *    link_set are anyone's), a page whose items and answer come in the
 *    same update reach the client as items, page_end, done; a second
 *    sync waits its turn and one that waits 150 seconds is TIMEOUT; a
 *    text's SENT that comes before its answer is held and told after it;
 *    a client that reads too little loses the page's items, is owed the
 *    page_end and a done NO_ROOM, and gets them in order once it reads;
 *    a message that came reaches the listening phone program (request
 *    0), nobody when none listens; the setting turned away from the
 *    paired phone makes the requests waiting UNAVAILABLE; the backend
 *    reaching bluetoothd again forgets the texts' numbers; a message that
 *    came while no phone program listens is the compositor's own
 *    notification with the lock screen's words and a command of the
 *    number (ws197-p004c), none while the phone's notifications are off,
 *    none for one read on the phone.
 * 2. libkeiland's view (system-view.c): the item queue told by one
 *    KL_PHONE_ITEMS, items taken after the mark fell out of the event
 *    ring, a full queue's KL_PHONE_DROPPED, the pages' ends by request,
 *    a sync's EBUSY ended by its done or by 180 seconds, the new result
 *    codes' errno values, the link's size checks.
 *
 * Prints "PASS name" or "FAIL name" for each check; the last line is
 * "phone-shell-host-test: PASS" or "... FAIL".
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland/system/system-private.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most events the capture clients keep, and the largest one. */
#define TEST_EVENTS_MAX		64U
#define TEST_EVENT_BYTES	20000U

/* The most items, results and states the fake backend holds. */
#define TEST_QUEUE_MAX		8U

/* One event a capture client was sent. */
struct test_event {
	uint64_t client;
	uint32_t opcode;
	unsigned char payload[TEST_EVENT_BYTES];
	size_t size;
};

/* The fake backend: its state, what it was asked, and what it gives at the next update. */
struct kl_backend_phone {
	struct kl_backend_phone_state state;
	uint32_t next_id;
	uint32_t last_page_id;
	uint32_t last_send_id;
	uint32_t last_link_id;
	unsigned pages;
	int refuse;
	unsigned changed;
	struct kl_backend_phone_item items[TEST_QUEUE_MAX];
	unsigned item_count;
	struct kl_backend_phone_result results[TEST_QUEUE_MAX];
	unsigned result_count;
	uint32_t sents[TEST_QUEUE_MAX];
	unsigned sent_states[TEST_QUEUE_MAX];
	unsigned sent_count;
};

/* The events sent to the capture clients, in order. */
static struct test_event test_events[TEST_EVENTS_MAX];
static unsigned test_event_count;

/* The client that reads too little (its number, 0 for none). */
static uint64_t test_full_client;

/* The fake desktop's phone.backend, notify.allow.phone and clock. */
static int test_backend = 2;
static int test_notify_allowed = 1;
static uint64_t test_now = 1000U;

/* The compositor's notifications posted: how many, and the last one's words and command. */
static unsigned test_notify_count;
static char test_notify_title[160];
static char test_notify_lock[160];
static char test_notify_command[256];

/* The one fake backend. */
static struct kl_backend_phone test_phone;

/* The checks that failed. */
static int test_failures;

int main(void);
static void test_shell(void);
static void test_view(void);
static void test_check(const char *name, int passed);
static size_t test_put_string(unsigned char *bytes, size_t offset, const char *text);
static size_t test_put_word(unsigned char *bytes, size_t offset, uint32_t word);
static uint32_t test_get_word(const struct test_event *event, size_t offset);
static const char *test_get_string(const struct test_event *event, size_t offset, size_t *next);
static int test_word_request(struct kwl_object *object, uint32_t opcode, uint32_t word);
static int test_sync(struct kwl_object *object, uint32_t request);
static int test_send_text(struct kwl_object *object, uint32_t request, const char *to, const char *text);
static void test_item(uint32_t id, const char *key);
static void test_result(uint32_t id, int error, uint32_t sent_request);
static int test_find(uint64_t client, uint32_t opcode, unsigned from, unsigned *at);
static void test_surface(struct kwl_client *client, uint32_t id, const char *app_id);

/* The capture client's events (kwl_emit of the compositor); the client that reads too little refuses. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	(void)object;

	/* A client that reads too little. */
	if (client->number == test_full_client)
		return ENOBUFS;

	/* Kept while there is room. */
	if (test_event_count >= TEST_EVENTS_MAX || size > TEST_EVENT_BYTES)
		return ENOSPC;
	test_events[test_event_count].client = client->number;
	test_events[test_event_count].opcode = opcode;
	if (size > 0U)
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
	struct kwl_object *same;

	/* A new ID only. */
	same = kwl_find(client, id);
	if (id == 0U || same != NULL)
		return NULL;
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

/* Marks an object dead and lets the phone forget it (the test frees nothing). */
void
kwl_object_destroy(
	struct kwl_object *object)
{
	object->dead = 1U;
	if (object->kind == KWL_SYSTEM_PHONE)
		kwl_phone_gone(object);
}

/* The compositor's clock. */
uint64_t
kwl_milliseconds(
	void)
{
	return test_now;
}

/* The fake desktop's settings: only phone.backend exists. */
int
kwl_settings_number(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	int differs;

	/* The settings of no server in particular. */
	(void)server;

	/* The phone's notifications. */
	differs = strcmp(name, "notify.allow.phone");
	if (differs == 0) {
		*number = test_notify_allowed;
		return 0;
	}

	/* The phone's backend. */
	differs = strcmp(name, "phone.backend");
	if (differs != 0)
		return ENOENT;
	*number = test_backend;
	return 0;
}

/* The compositor's notification of an application (notify-system.c): kept for the checks. */
uint32_t
kwl_notify_app_post(
	struct kwl_server *server,
	const char *app,
	const char *title,
	const char *body,
	const char *command,
	const char *lock_text)
{
	/* Kept. */
	(void)server;
	(void)app;
	(void)body;
	test_notify_count++;
	(void)snprintf(test_notify_title, sizeof(test_notify_title), "%s", title);
	(void)snprintf(test_notify_lock, sizeof(test_notify_lock), "%s", lock_text);
	(void)snprintf(test_notify_command, sizeof(test_notify_command), "%s", command);
	return test_notify_count;
}

/* Copies words as notify.c does, without its mending (the tests' words are plain). */
size_t
kwl_notify_clean(
	char *out,
	size_t room,
	const char *text,
	int lines)
{
	/* The copy. */
	(void)lines;
	(void)snprintf(out, room, "%s", text);
	return strlen(out);
}

/* The fake backend: opened once. */
struct kl_backend_phone *
kl_backend_phone_open(
	void)
{
	test_phone.next_id = 1U;
	test_phone.state.reachable = 1U;
	test_phone.state.subscribed = 1U;
	test_phone.state.messages = KL_BACKEND_PHONE_MESSAGES_READY;
	return &test_phone;
}

/* Closed: nothing to do. */
void
kl_backend_phone_close(
	struct kl_backend_phone *phone)
{
	(void)phone;
}

/* Gives what the test put for this update. */
int
kl_backend_phone_update(
	struct kl_backend_phone *phone,
	unsigned *changed)
{
	*changed = phone->changed;
	phone->changed = 0U;
	return 0;
}

/* The state. */
void
kl_backend_phone_get_state(
	const struct kl_backend_phone *phone,
	struct kl_backend_phone_state *state)
{
	*state = phone->state;
}

/* A page asked: its id. */
int
kl_backend_phone_page(
	struct kl_backend_phone *phone,
	unsigned what,
	int64_t since,
	unsigned limit,
	const char *cursor,
	unsigned count,
	uint32_t *id)
{
	(void)what;
	(void)since;
	(void)limit;
	(void)cursor;
	(void)count;
	if (phone->refuse != 0)
		return phone->refuse;
	*id = phone->next_id++;
	phone->last_page_id = *id;
	phone->pages++;
	return 0;
}

/* A message marked read: its id. */
int
kl_backend_phone_read(
	struct kl_backend_phone *phone,
	const char *handle,
	uint32_t *id)
{
	(void)handle;
	if (phone->refuse != 0)
		return phone->refuse;
	*id = phone->next_id++;
	return 0;
}

/* A text sent: its id. */
int
kl_backend_phone_send(
	struct kl_backend_phone *phone,
	const char *to,
	const uint8_t *text,
	size_t length,
	uint32_t *id)
{
	(void)to;
	(void)text;
	(void)length;
	if (phone->refuse != 0)
		return phone->refuse;
	*id = phone->next_id++;
	phone->last_send_id = *id;
	return 0;
}

/* The phone's switch: its id. */
int
kl_backend_phone_link_set(
	struct kl_backend_phone *phone,
	const char *address,
	unsigned on,
	unsigned profiles,
	uint32_t *id)
{
	(void)address;
	(void)on;
	(void)profiles;
	if (phone->refuse != 0)
		return phone->refuse;
	*id = phone->next_id++;
	phone->last_link_id = *id;
	return 0;
}

/* The oldest item. */
int
kl_backend_phone_take_item(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_item *item)
{
	if (phone->item_count == 0U)
		return 0;
	*item = phone->items[0];
	memmove(&phone->items[0], &phone->items[1], (phone->item_count - 1U) * sizeof(phone->items[0]));
	phone->item_count--;
	return 1;
}

/* The oldest result. */
int
kl_backend_phone_take_result(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_result *result)
{
	if (phone->result_count == 0U)
		return 0;
	*result = phone->results[0];
	memmove(&phone->results[0], &phone->results[1], (phone->result_count - 1U) * sizeof(phone->results[0]));
	phone->result_count--;
	return 1;
}

/* The oldest state of a text. */
int
kl_backend_phone_take_sent(
	struct kl_backend_phone *phone,
	uint32_t *sent_request,
	unsigned *state)
{
	if (phone->sent_count == 0U)
		return 0;
	*sent_request = phone->sents[0];
	*state = phone->sent_states[0];
	memmove(&phone->sents[0], &phone->sents[1], (phone->sent_count - 1U) * sizeof(phone->sents[0]));
	memmove(&phone->sent_states[0], &phone->sent_states[1], (phone->sent_count - 1U) * sizeof(phone->sent_states[0]));
	phone->sent_count--;
	return 1;
}

/* Reads again: nothing to do. */
void
kl_backend_phone_refresh(
	struct kl_backend_phone *phone)
{
	(void)phone;
}

/*
 * Runs both parts and prints the outcome.
 */
int
main(void)
{
	test_shell();
	test_view();
	if (test_failures != 0) {
		printf("phone-shell-host-test: FAIL (%d)\n", test_failures);
		return 1;
	}

	/* Succeeded. */
	printf("phone-shell-host-test: PASS\n");
	return 0;
}

/* The compositor's part. */
static void
test_shell(void)
{
	static struct kwl_server server;
	static struct kwl_client app;
	static struct kwl_client other;
	static struct kwl_client second;
	struct kwl_object manager;
	struct kwl_object *phone;
	struct kwl_object *stranger;
	struct kwl_object *later;
	unsigned char bytes[128];
	const char *why;
	size_t offset;
	unsigned at;
	unsigned item_at;
	unsigned end_at;
	unsigned done_at;
	uint32_t first_id;
	int found;
	int error;

	/* Three clients of the user: the phone program, another program, a second phone program. */
	app.server = &server;
	app.number = 1U;
	app.fd = -1;
	other.server = &server;
	other.number = 2U;
	other.fd = -1;
	second.server = &server;
	second.number = 3U;
	second.fd = -1;
	app.next = &other;
	other.next = &second;
	server.clients = &app;
	test_surface(&app, 100U, "phone");
	test_surface(&other, 100U, "mailer");
	test_surface(&second, 100U, "phone");

	/* A phone object of version 27 for each. */
	memset(&manager, 0, sizeof(manager));
	manager.version = KL_SYSTEM_SINCE_PHONE_SYNC;
	manager.client = &app;
	(void)test_put_word(bytes, 0U, 10U);
	error = kwl_phone_create(&manager, bytes, 4U);
	manager.client = &other;
	error |= kwl_phone_create(&manager, bytes, 4U);
	manager.client = &second;
	error |= kwl_phone_create(&manager, bytes, 4U);
	phone = kwl_find(&app, 10U);
	stranger = kwl_find(&other, 10U);
	later = kwl_find(&second, 10U);
	test_check("create", error == 0 && phone != NULL && stranger != NULL && later != NULL);
	if (phone == NULL || stranger == NULL || later == NULL)
		return;

	/* The first tick opens the backend. */
	kwl_phone_tick(&server);
	test_check("open", test_phone.next_id == 1U && test_phone.state.reachable == 1U);

	/* The gate: another program's listen hears "not-phone", its sync is refused; its watch_link hears the state. */
	test_event_count = 0U;
	(void)test_word_request(stranger, KL_SYSTEM_PHONE_LISTEN, 1U);
	why = NULL;
	if (test_event_count == 1U) {
		offset = 9U * 4U;
		why = test_get_string(&test_events[0], offset, &offset);
		why = test_get_string(&test_events[0], offset, &offset);
	}

	/* Told why. */
	test_check("gate-listen", test_event_count == 1U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_LINK && why != NULL &&
	    strcmp(why, "not-phone") == 0);
	test_event_count = 0U;
	(void)test_sync(stranger, 5U);
	test_check("gate-sync", test_event_count == 1U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_DONE &&
	    test_get_word(&test_events[0], 0U) == 5U && test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_REFUSED &&
	    test_phone.pages == 0U);
	test_event_count = 0U;
	(void)test_word_request(stranger, KL_SYSTEM_PHONE_WATCH_LINK, 1U);
	test_check("gate-watch", test_event_count == 1U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_LINK &&
	    test_get_word(&test_events[0], 0U) == 2U && test_get_word(&test_events[0], 8U) == KL_BACKEND_PHONE_MESSAGES_READY);
	(void)test_word_request(stranger, KL_SYSTEM_PHONE_WATCH_LINK, 0U);
	(void)test_word_request(stranger, KL_SYSTEM_PHONE_LISTEN, 0U);

	/* The phone program listens and hears the state. */
	test_event_count = 0U;
	(void)test_word_request(phone, KL_SYSTEM_PHONE_LISTEN, 1U);
	test_check("listen", test_event_count == 1U && test_get_word(&test_events[0], 20U) == 1U);

	/* A page whose items and answer come in the same update: items, page_end, done. */
	test_event_count = 0U;
	(void)test_sync(phone, 7U);
	first_id = test_phone.last_page_id;
	test_item(first_id, "0000000000000001");
	test_item(first_id, "0000000000000002");
	test_result(first_id, 0, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM | KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	found = test_find(1U, KL_SYSTEM_PHONE_EVENT_ITEM, 0U, &item_at);
	found &= test_find(1U, KL_SYSTEM_PHONE_EVENT_PAGE_END, 0U, &end_at);
	found &= test_find(1U, KL_SYSTEM_PHONE_EVENT_DONE, 0U, &done_at);
	test_check("page", found && test_event_count == 4U && item_at == 0U && end_at == 2U && done_at == 3U &&
	    test_get_word(&test_events[0], 0U) == 7U && test_get_word(&test_events[2], 0U) == 7U &&
	    test_get_word(&test_events[3], 4U) == KL_SYSTEM_RESULT_OK);

	/* A second sync waits for the first; it starts once the first is answered. */
	test_event_count = 0U;
	(void)test_sync(phone, 8U);
	first_id = test_phone.last_page_id;
	(void)test_word_request(later, KL_SYSTEM_PHONE_LISTEN, 1U);
	(void)test_sync(later, 9U);
	test_check("sync-waits", test_phone.pages == 2U);
	test_result(first_id, ECONNRESET, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	found = test_find(1U, KL_SYSTEM_PHONE_EVENT_DONE, 0U, &at);
	test_check("sync-lost", found && test_get_word(&test_events[at], 4U) == KL_SYSTEM_RESULT_LOST &&
	    !test_find(1U, KL_SYSTEM_PHONE_EVENT_PAGE_END, 0U, &at));
	test_check("sync-next", test_phone.pages == 3U);

	/* Another waits while that one runs, and gives up after 150 seconds. */
	test_event_count = 0U;
	(void)test_sync(phone, 10U);
	test_now += 150000U;
	kwl_phone_tick(&server);
	found = test_find(1U, KL_SYSTEM_PHONE_EVENT_DONE, 0U, &at);
	test_check("sync-timeout", found && test_get_word(&test_events[at], 0U) == 10U &&
	    test_get_word(&test_events[at], 4U) == KL_SYSTEM_RESULT_TIMEOUT && test_phone.pages == 3U);
	test_result(test_phone.last_page_id, 0, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);

	/* A text: its SENT before its answer is held, told after the done. */
	test_event_count = 0U;
	(void)test_send_text(phone, 11U, "+15550100", "hello");
	test_phone.sents[0] = 77U;
	test_phone.sent_states[0] = KL_BACKEND_PHONE_SENT;
	test_phone.sent_count = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_SENT;
	kwl_phone_tick(&server);
	test_check("sent-held", test_event_count == 0U);
	test_result(test_phone.last_send_id, 0, 77U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	test_check("sent-told", test_event_count == 2U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_DONE &&
	    test_events[1].opcode == KL_SYSTEM_PHONE_EVENT_STATUS && test_get_word(&test_events[1], 0U) == 11U &&
	    test_get_word(&test_events[1], 4U) == KL_SYSTEM_PHONE_SENT);
	test_event_count = 0U;
	test_phone.sents[0] = 77U;
	test_phone.sent_states[0] = KL_BACKEND_PHONE_DELIVERED;
	test_phone.sent_count = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_SENT;
	kwl_phone_tick(&server);
	test_check("delivered", test_event_count == 1U && test_get_word(&test_events[0], 4U) == KL_SYSTEM_PHONE_DELIVERED);

	/* A state of no text is dropped after 10 seconds. */
	test_event_count = 0U;
	test_phone.sents[0] = 99U;
	test_phone.sent_states[0] = KL_BACKEND_PHONE_SENT;
	test_phone.sent_count = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_SENT;
	kwl_phone_tick(&server);
	test_now += 10000U;
	kwl_phone_tick(&server);
	(void)test_send_text(phone, 12U, "+15550100", "again");
	test_result(test_phone.last_send_id, 0, 99U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	test_check("held-expires", test_event_count == 1U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_DONE);

	/* A client that reads too little: its page's items are lost, page_end and done owed (NO_ROOM), sent in order later. */
	test_event_count = 0U;
	(void)test_sync(phone, 13U);
	first_id = test_phone.last_page_id;
	test_full_client = 1U;
	test_item(first_id, "0000000000000003");
	test_result(first_id, 0, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM | KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	(void)test_send_text(phone, 14U, "+1", "x");
	test_result(test_phone.last_send_id, EINVAL, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	test_check("owed", test_event_count == 0U);
	test_full_client = 0U;
	kwl_phone_tick(&server);
	test_check("owed-order", test_event_count == 3U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_PAGE_END &&
	    test_events[1].opcode == KL_SYSTEM_PHONE_EVENT_DONE && test_get_word(&test_events[1], 0U) == 13U &&
	    test_get_word(&test_events[1], 4U) == KL_SYSTEM_RESULT_NO_ROOM &&
	    test_events[2].opcode == KL_SYSTEM_PHONE_EVENT_DONE && test_get_word(&test_events[2], 0U) == 14U &&
	    test_get_word(&test_events[2], 4U) == KL_SYSTEM_RESULT_INVALID);

	/* A message that came: to both phone programs listening (request 0), none to the other program. */
	test_event_count = 0U;
	test_item(0U, "0000000000000004");
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM;
	kwl_phone_tick(&server);
	test_check("live", test_event_count == 2U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_ITEM &&
	    test_get_word(&test_events[0], 0U) == 0U && !test_find(2U, KL_SYSTEM_PHONE_EVENT_ITEM, 0U, &at));

	/* bluetoothd dropped an event: the phone programs hear dropped. */
	test_event_count = 0U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_DROPPED;
	kwl_phone_tick(&server);
	test_check("dropped", test_event_count == 2U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_DROPPED);

	/* The switch is anyone's, through the backend whatever the setting. */
	test_event_count = 0U;
	offset = test_put_word(bytes, 0U, 15U);
	offset = test_put_string(bytes, offset, "AA:BB:CC:DD:EE:FF");
	offset = test_put_word(bytes, offset, 1U);
	offset = test_put_word(bytes, offset, KL_PHONE_PROFILE_MESSAGES);
	(void)kwl_phone_request(stranger, KL_SYSTEM_PHONE_LINK_SET, bytes, offset);
	test_result(test_phone.last_link_id, EACCES, 0U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	test_check("link-set", test_event_count == 1U && test_events[0].client == 2U && test_get_word(&test_events[0], 0U) == 15U &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_REFUSED);

	/* The setting away from the paired phone: the sync waiting for its answer is UNAVAILABLE, and the link is told. */
	test_event_count = 0U;
	(void)test_sync(phone, 16U);
	test_backend = 0;
	kwl_phone_tick(&server);
	found = test_find(1U, KL_SYSTEM_PHONE_EVENT_DONE, 0U, &at);
	test_check("unavailable", found && test_get_word(&test_events[at], 0U) == 16U &&
	    test_get_word(&test_events[at], 4U) == KL_SYSTEM_RESULT_UNAVAILABLE && test_find(1U, KL_SYSTEM_PHONE_EVENT_LINK, 0U, &at));
	test_event_count = 0U;
	(void)test_sync(phone, 17U);
	test_check("no-backend", test_event_count == 1U && test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_UNAVAILABLE);
	test_backend = 2;
	kwl_phone_tick(&server);

	/* bluetoothd again: the texts' numbers are forgotten. */
	test_event_count = 0U;
	(void)test_send_text(phone, 18U, "+1", "y");
	test_result(test_phone.last_send_id, 0, 5U);
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_RESULT;
	kwl_phone_tick(&server);
	test_phone.state.reachable = 0U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_STATE;
	kwl_phone_tick(&server);
	test_phone.state.reachable = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_STATE;
	kwl_phone_tick(&server);
	test_event_count = 0U;
	test_phone.sents[0] = 5U;
	test_phone.sent_states[0] = KL_BACKEND_PHONE_SENT;
	test_phone.sent_count = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_SENT;
	kwl_phone_tick(&server);
	test_check("forget", !test_find(1U, KL_SYSTEM_PHONE_EVENT_STATUS, 0U, &at));

	/* Version 16's send from another program is refused with the paired phone. */
	test_event_count = 0U;
	offset = test_put_word(bytes, 0U, 19U);
	offset = test_put_word(bytes, offset, KL_PHONE_SMS);
	offset = test_put_string(bytes, offset, "+1");
	offset = test_put_string(bytes, offset, "z");
	(void)kwl_phone_request(stranger, KL_SYSTEM_PHONE_SEND, bytes, offset);
	test_check("v1-gate", test_event_count == 1U && test_events[0].opcode == KL_SYSTEM_PHONE_EVENT_RESULT &&
	    test_get_word(&test_events[0], 4U) == KL_SYSTEM_RESULT_REFUSED);

	/* The phone program's object goes: nothing more for it. */
	kwl_object_destroy(phone);
	test_event_count = 0U;
	test_item(0U, "0000000000000005");
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM;
	kwl_phone_tick(&server);
	test_check("gone", test_event_count == 1U && test_events[0].client == 3U && test_notify_count == 0U);

	/* No phone program listens: the compositor's own notification, the lock screen's words, the number's command. */
	kwl_object_destroy(later);
	test_event_count = 0U;
	test_item(0U, "0000000000000006");
	(void)snprintf(test_phone.items[0].name, sizeof(test_phone.items[0].name), "%s", "Mother");
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM;
	kwl_phone_tick(&server);
	test_check("notify", test_event_count == 0U && test_notify_count == 1U && strcmp(test_notify_title, "Mother") == 0 &&
	    strcmp(test_notify_lock, "New message from Mother") == 0 && strstr(test_notify_command, "--peer '+15550100'") != NULL);

	/* Read on the phone, or the phone's notifications off: none. */
	test_item(0U, "0000000000000007");
	test_phone.items[0].read = 1U;
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM;
	kwl_phone_tick(&server);
	test_notify_allowed = 0;
	test_item(0U, "0000000000000008");
	test_phone.changed = KL_BACKEND_PHONE_CHANGED_ITEM;
	kwl_phone_tick(&server);
	test_check("notify-none", test_notify_count == 1U);
}

/* libkeiland's part. */
static void
test_view(void)
{
	static struct system_view view;
	struct system_view_page_end end;
	struct kl_phone_event event;
	struct kl_phone_item item;
	struct kl_phone_link link;
	uint32_t request;
	unsigned index;
	unsigned items;
	int error;
	int got;

	/* Items: one KL_PHONE_ITEMS for the first of an empty queue, the texts owned. */
	memset(&item, 0, sizeof(item));
	item.request = 3U;
	system_view_phone_item(&view, &item, "abc", 3U);
	system_view_phone_item(&view, &item, "de", 2U);
	got = system_view_take_phone_event(&view, &event);
	test_check("items-mark", got == 1 && event.kind == KL_PHONE_ITEMS && system_view_take_phone_event(&view, &event) == 0);
	got = system_view_take_phone_item(&view, &item, sizeof(item));
	test_check("item-take", got == 1 && item.length == 3U && strcmp(item.text, "abc") == 0 && item.request == 3U);
	got = system_view_take_phone_item(&view, &item, sizeof(item));
	test_check("item-take2", got == 1 && strcmp(item.text, "de") == 0 && system_view_take_phone_item(&view, &item, sizeof(item)) == 0);
	got = system_view_take_phone_item(&view, &item, sizeof(item) - 4U);
	test_check("item-size", got == 0);

	/* The mark fell out of the event ring: the items are still there, the loss told. */
	system_view_phone_item(&view, &item, "x", 1U);
	for (index = 0U; index < 20U; index++) {
		memset(&event, 0, sizeof(event));
		event.kind = KL_PHONE_STATUS;
		system_view_phone_event(&view, &event);
	}

	/* The loss first, the item still waiting; the events emptied. */
	got = system_view_take_phone_event(&view, &event);
	items = (unsigned)system_view_take_phone_item(&view, &item, sizeof(item));
	test_check("mark-lost", got == 1 && event.kind == KL_PHONE_DROPPED && items == 1U);
	do {
		got = system_view_take_phone_event(&view, &event);
	} while (got == 1);

	/* A full queue drops its oldest and tells KL_PHONE_DROPPED. */
	for (index = 0U; index < 65U; index++) {
		item.request = index;
		system_view_phone_item(&view, &item, "y", 1U);
	}

	/* The mark, the drop, and the newest 64 from the second. */
	got = system_view_take_phone_event(&view, &event);
	test_check("queue-mark", got == 1 && event.kind == KL_PHONE_ITEMS);
	got = system_view_take_phone_event(&view, &event);
	test_check("queue-full", got == 1 && event.kind == KL_PHONE_DROPPED);
	items = 0U;
	for (;;) {
		got = system_view_take_phone_item(&view, &item, sizeof(item));
		if (got != 1)
			break;
		if (items == 0U)
			test_check("queue-oldest", item.request == 1U);
		items++;
	}

	/* All of them. */
	test_check("queue-count", items == 64U);

	/* The pages' ends by request. */
	memset(&end, 0, sizeof(end));
	end.request = 9U;
	(void)snprintf(end.cursor, sizeof(end.cursor), "%s", "c1");
	end.more = 1U;
	system_view_phone_page_end(&view, &end);
	memset(&end, 0, sizeof(end));
	error = system_view_phone_page_end_of(&view, 9U, &end);
	test_check("page-end", error == 0 && end.more == 1U && strcmp(end.cursor, "c1") == 0);
	test_check("page-end-none", system_view_phone_page_end_of(&view, 8U, &end) == ENOENT &&
	    system_view_phone_page_end_of(&view, 0U, &end) == ENOENT);

	/* One sync at a time: EBUSY until its done, or for 180 seconds. */
	error = system_view_phone_sync_start(&view, 20U, 1000U);
	test_check("sync-start", error == 0 && system_view_phone_sync_start(&view, 21U, 2000U) == EBUSY);
	system_view_phone_done(&view, 20U, KL_SYSTEM_RESULT_LOST);
	got = system_view_take_result(&view, &request, &error);
	test_check("sync-done", got == 1 && request == 20U && error == ECONNRESET && system_view_phone_sync_start(&view, 22U, 3000U) == 0);
	test_check("sync-180", system_view_phone_sync_start(&view, 23U, 3000U + 179999U) == EBUSY &&
	    system_view_phone_sync_start(&view, 24U, 3000U + 180000U) == 0);

	/* The new codes' errno values. */
	test_check("codes", system_view_error_of(KL_SYSTEM_RESULT_TIMEOUT) == ETIMEDOUT && system_view_error_of(KL_SYSTEM_RESULT_TOO_LARGE) == EMSGSIZE &&
	    system_view_error_of(KL_SYSTEM_RESULT_NO_ROOM) == ENOBUFS && system_view_error_of(KL_SYSTEM_RESULT_NOT_CONNECTED) == ENOTCONN &&
	    system_view_error_of(KL_SYSTEM_RESULT_NO_KEY) == ENOENT);

	/* The link: not told yet, then told with KL_PHONE_LINK_CHANGED; a size too small. */
	do {
		got = system_view_take_phone_event(&view, &event);
	} while (got == 1);
	test_check("link-unknown", system_view_phone_link_get(&view, &link, sizeof(link)) == ENOENT);
	memset(&link, 0, sizeof(link));
	link.messages = 2U;
	system_view_phone_link(&view, &link);
	memset(&link, 0, sizeof(link));
	got = system_view_take_phone_event(&view, &event);
	test_check("link", got == 1 && event.kind == KL_PHONE_LINK_CHANGED && system_view_phone_link_get(&view, &link, sizeof(link)) == 0 &&
	    link.messages == 2U && system_view_phone_link_get(&view, &link, 8U) == EINVAL);

	/* What the items own goes with the view. */
	system_view_phone_item(&view, &item, "z", 1U);
	system_view_phone_release(&view);
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed)
{
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed. */
	printf("FAIL %s\n", name);
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
	memcpy(bytes + offset, &word, sizeof(word));
	return offset + 4U;
}

/* Reads a word of an event. */
static uint32_t
test_get_word(
	const struct test_event *event,
	size_t offset)
{
	uint32_t word;

	/* Within the event. */
	if (offset + 4U > event->size)
		return 0xffffffffU;
	memcpy(&word, event->payload + offset, sizeof(word));
	return word;
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
	*next = offset + 4U + (((size_t)length + 3U) & ~(size_t)3U);
	return (const char *)(event->payload + offset + 4U);
}

/* Sends a request of one word (listen, watch_link). */
static int
test_word_request(
	struct kwl_object *object,
	uint32_t opcode,
	uint32_t word)
{
	unsigned char bytes[4];

	/* The word alone. */
	(void)test_put_word(bytes, 0U, word);
	return kwl_phone_request(object, opcode, bytes, sizeof(bytes));
}

/* Sends sync(request, MESSAGES, 0, 0, 500, "", 32). */
static int
test_sync(
	struct kwl_object *object,
	uint32_t request)
{
	unsigned char bytes[64];
	size_t offset;

	/* Since 1700000000, at most 500 a folder, from the start, 32 items. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_word(bytes, offset, KL_SYSTEM_PHONE_MESSAGES);
	offset = test_put_word(bytes, offset, 0U);
	offset = test_put_word(bytes, offset, 1700000000U);
	offset = test_put_word(bytes, offset, 500U);
	offset = test_put_string(bytes, offset, "");
	offset = test_put_word(bytes, offset, 32U);
	return kwl_phone_request(object, KL_SYSTEM_PHONE_SYNC, bytes, offset);
}

/* Sends send_text(request, SMS, to, text). */
static int
test_send_text(
	struct kwl_object *object,
	uint32_t request,
	const char *to,
	const char *text)
{
	unsigned char bytes[128];
	size_t offset;
	uint32_t length;

	/* The number, then the text as an array. */
	offset = test_put_word(bytes, 0U, request);
	offset = test_put_word(bytes, offset, KL_SYSTEM_PHONE_SMS);
	offset = test_put_string(bytes, offset, to);
	length = (uint32_t)strlen(text);
	offset = test_put_word(bytes, offset, length);
	memset(bytes + offset, 0, (length + 3U) & ~3U);
	memcpy(bytes + offset, text, length);
	offset += (length + 3U) & ~3U;
	return kwl_phone_request(object, KL_SYSTEM_PHONE_SEND_TEXT, bytes, offset);
}

/* Puts an item for the next update (id 0: one that came). */
static void
test_item(
	uint32_t id,
	const char *key)
{
	struct kl_backend_phone_item *item;

	/* After the others. */
	item = &test_phone.items[test_phone.item_count++];
	memset(item, 0, sizeof(*item));
	item->id = id;
	(void)snprintf(item->handle, sizeof(item->handle), "00000001.%s", key);
	(void)snprintf(item->key, sizeof(item->key), "%s", key);
	(void)snprintf(item->peer, sizeof(item->peer), "%s", "+15550100");
	item->time = 1700000000;
	item->text = "Hi";
	item->length = 2U;
}

/* Puts a result for the next update. */
static void
test_result(
	uint32_t id,
	int error,
	uint32_t sent_request)
{
	struct kl_backend_phone_result *result;

	/* After the others. */
	result = &test_phone.results[test_phone.result_count++];
	memset(result, 0, sizeof(*result));
	result->id = id;
	result->error = error;
	result->count = 2U;
	result->sent_request = sent_request;
}

/* Finds the first event of a client and an opcode from an index: 1 with its index. */
static int
test_find(
	uint64_t client,
	uint32_t opcode,
	unsigned from,
	unsigned *at)
{
	unsigned index;

	/* Each event from there. */
	for (index = from; index < test_event_count; index++) {
		if (test_events[index].client == client && test_events[index].opcode == opcode) {
			*at = index;
			return 1;
		}
	}

	/* None. */
	return 0;
}

/* Gives a client a live surface of an application ID. */
static void
test_surface(
	struct kwl_client *client,
	uint32_t id,
	const char *app_id)
{
	struct kwl_object *surface;

	/* The surface and its ID. */
	surface = kwl_create(client, id, KWL_SURFACE, 1U);
	if (surface != NULL)
		(void)snprintf(surface->app_id, sizeof(surface->app_id), "%s", app_id);
}
