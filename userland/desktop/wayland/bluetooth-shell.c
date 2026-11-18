/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bluetooth on the wire and in the compositor (ws143-p006,
 * plan/ws143/phase006/phase.md section 5): kl_system_bluetooth_v1, made by
 * the system manager's get_bluetooth (since its version 23).  The state,
 * the devices and the requests are libkeiland-backend's
 * (kl_backend_bluetooth_*), opened once on the desktop's first tick (never
 * by the login screen's compositor) and kept for the compositor's life: it
 * answers the pairings' questions whether or not anyone looks, and the
 * system bar's icon shows its state.
 *
 *   watch(on)                  the state read often while some object watches (a page or a menu shown)
 *   scan(on)                   the devices around looked for; an asking holds a minute unless asked again
 *   power(request, on)         device(request, action, address, type)
 *
 * Every Bluetooth object is sent the state, the devices and done when it is
 * made and whenever they change; the object that asked gets the result.
 * The backend numbers its requests; a table pairs them with the client's
 * (the compositor's own, the system bar's, have client 0).  A pairing's
 * questions go to the compositor's window (bluetooth-ask.c), never to a
 * client.
 * Log: "KWL BT state ...", "KWL BT device ...", "KWL BT request ...",
 * "KWL BT result ...", "KWL BT watch on=N", "KWL BT scan on=N".
 */

#include "kwl.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The longest event: a device's (an address, a type, a name, four words). */
#define BT_EVENT_MAX		(4U + KL_SYSTEM_BT_ADDRESS_MAX + 4U + 4U + KL_SYSTEM_BT_NAME_MAX + 4U * 4U + 8U)

/* The most requests waiting for the backend's answer. */
#define BT_WAITING_MAX		16U

/* How long an object's asking for scans holds unless asked again (milliseconds). */
#define BT_SCAN_MS		60000U

/*
 * A request waiting: the backend's number, the client (by its number; 0
 * for the compositor's own) and object, and the client's number.
 */
struct bt_waiting {
	uint32_t backend;
	uint64_t client;
	uint32_t object;
	uint32_t request;
	int used;
};

/*
 * Bluetooth in the compositor: the backend (opened once), the state and
 * the devices as last read, the requests waiting, how many objects watch
 * and scan (and the system bar's watching), what the backend was told,
 * and the serial of the last done.
 */
static struct {
	struct kl_backend_bluetooth *backend;
	int opened;
	struct kl_backend_bluetooth_state state;
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	size_t count;
	struct bt_waiting waiting[BT_WAITING_MAX];
	unsigned watchers;
	unsigned scanners;
	unsigned bar_watching;
	unsigned told_watching;
	unsigned told_scanning;
	uint32_t serial;
	int bar_error;
	unsigned bar_answered;
} bt_state;

static int bt_watch(struct kwl_object *object, unsigned on);
static int bt_scan(struct kwl_object *object, unsigned on);
static int bt_power(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int bt_device(struct kwl_object *object, const unsigned char *bytes, size_t size);
static void bt_ask(struct kwl_client *client, uint32_t id, uint32_t request, unsigned what, const char *address, unsigned type);
static void bt_wait(uint64_t client, uint32_t object, uint32_t backend, uint32_t request);
static void bt_holders(struct kwl_server *server);
static void bt_state_to(struct kwl_client *client, uint32_t id);
static void bt_tell(struct kwl_server *server);
static void bt_answers(struct kwl_server *server);
static void bt_questions(struct kwl_server *server);
static void bt_result(struct kwl_client *client, uint32_t id, uint32_t request, uint32_t applied);
static uint32_t bt_applied(int error);
static void bt_log_state(void);
static const char *bt_action_name(unsigned what);
static int bt_string(const unsigned char *bytes, size_t size, size_t offset, size_t bound, const char **text, size_t *next);
static size_t bt_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t bt_word(const unsigned char *bytes, size_t offset);

/*
 * Makes a Bluetooth object for a manager's get_bluetooth (new id), and
 * sends it the state and the devices.  Returns 0, or EPROTO.
 */
int
kwl_bluetooth_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = bt_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_BLUETOOTH, manager->version);
	if (created == NULL)
		return EPROTO;

	/* Its first state. */
	bt_state_to(manager->client, id);
	printf("KWL BT object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);
	return 0;
}

/*
 * Carries out a request of a Bluetooth object.  Returns 0, or EPROTO.
 */
int
kwl_bluetooth_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t word;
	unsigned on;
	int error;

	/* The object goes (what it held goes with it, kwl_bluetooth_gone). */
	if (opcode == KL_SYSTEM_BLUETOOTH_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* watch and scan take one word, 1 or 0. */
	if (opcode == KL_SYSTEM_BLUETOOTH_WATCH || opcode == KL_SYSTEM_BLUETOOTH_SCAN) {
		if (size != 4U)
			return EPROTO;
		on = 0U;
		word = bt_word(bytes, 0U);
		if (word != 0U)
			on = 1U;
		if (opcode == KL_SYSTEM_BLUETOOTH_WATCH)
			error = bt_watch(object, on);
		else
			error = bt_scan(object, on);
		return error;
	}

	/* The requests the backend answers. */
	if (opcode == KL_SYSTEM_BLUETOOTH_POWER)
		error = bt_power(object, bytes, size);
	else if (opcode == KL_SYSTEM_BLUETOOTH_DEVICE)
		error = bt_device(object, bytes, size);
	else
		error = EPROTO;
	return error;
}

/*
 * Looks after Bluetooth once a pass: opens the backend on the desktop's
 * first tick, lets go of the askings for scans that ran out, tells the
 * backend who watches and scans, reads what came, sends the new state to
 * every Bluetooth object and the answers to those that asked, and hands a
 * pairing's questions to the window.
 */
void
kwl_bluetooth_tick(
	struct kwl_server *server)
{
	unsigned changed;

	/* The login screen's compositor has no Bluetooth. */
	if (server->greeter)
		return;

	/* Opened once. */
	if (!bt_state.opened) {
		bt_state.opened = 1;
		bt_state.backend = kl_backend_bluetooth_open();
		printf("KWL BT open ok=%d\n", bt_state.backend != NULL);
	}

	/* Without memory there is nothing to follow. */
	if (bt_state.backend == NULL)
		return;

	/* Who watches and scans. */
	bt_holders(server);

	/* What came. */
	changed = 0U;
	(void)kl_backend_bluetooth_update(bt_state.backend, &changed);
	if ((changed & (KL_BACKEND_BT_CHANGED_STATE | KL_BACKEND_BT_CHANGED_DEVICES)) != 0U) {
		kl_backend_bluetooth_get_state(bt_state.backend, &bt_state.state);
		bt_state.count = kl_backend_bluetooth_get_devices(bt_state.backend, bt_state.devices, KL_BACKEND_BT_DEVICES_MAX);
		if (bt_state.count > KL_BACKEND_BT_DEVICES_MAX)
			bt_state.count = KL_BACKEND_BT_DEVICES_MAX;
		bt_log_state();
		bt_tell(server);
		server->dirty = 1;
	}

	/* The answers, and a pairing's questions. */
	if ((changed & KL_BACKEND_BT_CHANGED_RESULT) != 0U)
		bt_answers(server);
	if ((changed & KL_BACKEND_BT_CHANGED_QUESTION) != 0U)
		bt_questions(server);

	/* The window follows the service: it goes with it. */
	kwl_bluetooth_ask_tick(server, bt_state.state.reachable);
}

/*
 * Lets go of what a Bluetooth object held (its watching, its scanning) as
 * it goes.
 */
void
kwl_bluetooth_gone(
	struct kwl_object *object)
{
	/* What it held. */
	(void)bt_watch(object, 0U);
	(void)bt_scan(object, 0U);
}

/* Closes the backend at the compositor's end (an outstanding pairing ends with it). */
void
kwl_bluetooth_close(
	struct kwl_server *server)
{
	UNUSED_PARAMETER(server);

	/* Only an open one. */
	if (bt_state.backend == NULL)
		return;
	kl_backend_bluetooth_close(bt_state.backend);
	bt_state.backend = NULL;
}

/*
 * Copies the state and up to capacity devices as last read, for the system
 * bar; returns how many devices there are (0 and an unreachable state
 * before the backend is open).
 */
size_t
kwl_bluetooth_view(
	struct kl_backend_bluetooth_state *state,
	struct kl_backend_bluetooth_device *devices,
	size_t capacity)
{
	size_t count;

	/* The state. */
	*state = bt_state.state;

	/* The devices that fit. */
	count = bt_state.count;
	if (count > capacity)
		count = capacity;
	if (count > 0U)
		memcpy(devices, bt_state.devices, count * sizeof(devices[0]));
	return bt_state.count;
}

/*
 * The system bar's own watching (while its menu is open), counted with the
 * objects'.
 */
void
kwl_bluetooth_bar_watch(
	unsigned on)
{
	/* 1 or 0, told at the next tick. */
	bt_state.bar_watching = on != 0U;
}

/*
 * Sends a request of the system bar (KL_BACKEND_BT_*; the power's take a
 * NULL address): its answer is the bar's, kwl_bluetooth_bar_answer.
 * Returns 0 or the backend's errno value.
 */
int
kwl_bluetooth_bar_request(
	unsigned request,
	const char *address,
	unsigned type)
{
	uint32_t id;
	int error;

	/* No backend (the login screen, or no memory). */
	if (bt_state.backend == NULL)
		return ENOTCONN;

	/* Asked of the backend, answered at a later tick. */
	id = 0U;
	error = kl_backend_bluetooth_request(bt_state.backend, request, address, type, &id);
	printf("KWL BT request client=0 action=%u error=%d\n", request, error);
	if (error != 0)
		return error;
	bt_wait(0U, 0U, id, 0U);
	return 0;
}

/*
 * Takes the answer of the system bar's last request: 1 with its errno
 * value in *error, 0 when none came since.
 */
int
kwl_bluetooth_bar_answer(
	int *error)
{
	/* None. */
	if (!bt_state.bar_answered)
		return 0;

	/* Taken. */
	bt_state.bar_answered = 0U;
	*error = bt_state.bar_error;
	return 1;
}

/*
 * Answers the question of an id from the pairing window (bluetooth-ask.c):
 * 1 yes, 0 no.  Returns 0, or ENOENT.
 */
int
kwl_bluetooth_answer(
	uint32_t id,
	unsigned yes)
{
	/* No backend. */
	if (bt_state.backend == NULL)
		return ENOENT;
	return kl_backend_bluetooth_answer(bt_state.backend, id, yes);
}

/* Gives up this desktop's own pairing going on (the window's Cancel while a number shows).  Returns 0, or ENOENT. */
int
kwl_bluetooth_cancel(
	void)
{
	/* No backend. */
	if (bt_state.backend == NULL)
		return ENOENT;
	return kl_backend_bluetooth_cancel(bt_state.backend);
}

/* Counts an object in or out of the watching. */
static int
bt_watch(
	struct kwl_object *object,
	unsigned on)
{
	/* No change: an object is counted once however often it asks. */
	if (on == object->bluetooth_watch)
		return 0;

	/* Counted. */
	object->bluetooth_watch = on;
	if (on)
		bt_state.watchers++;
	else if (bt_state.watchers > 0U)
		bt_state.watchers--;
	return 0;
}

/* Counts an object in or out of the scanning; an asking holds BT_SCAN_MS from now. */
static int
bt_scan(
	struct kwl_object *object,
	unsigned on)
{
	/* The asking runs on from now. */
	if (on)
		object->bluetooth_scan_until = kwl_milliseconds() + BT_SCAN_MS;

	/* No change: an object is counted once however often it asks. */
	if (on == object->bluetooth_scan)
		return 0;

	/* Counted. */
	object->bluetooth_scan = on;
	if (on)
		bt_state.scanners++;
	else if (bt_state.scanners > 0U)
		bt_state.scanners--;
	return 0;
}

/* Carries out power(request, on). */
static int
bt_power(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t request;
	uint32_t on;
	unsigned what;

	/* request, on. */
	if (size != 8U)
		return EPROTO;
	request = bt_word(bytes, 0U);
	on = bt_word(bytes, 4U);
	what = KL_BACKEND_BT_POWER_OFF;
	if (on != 0U)
		what = KL_BACKEND_BT_POWER_ON;

	/* Asked of the backend. */
	bt_ask(object->client, object->id, request, what, NULL, 0U);
	return 0;
}

/* Carries out device(request, action, address, type). */
static int
bt_device(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	static const unsigned actions[] = {
		0U,
		KL_BACKEND_BT_PAIR,
		KL_BACKEND_BT_FORGET,
		KL_BACKEND_BT_CONNECT,
		KL_BACKEND_BT_DISCONNECT,
		KL_BACKEND_BT_PAIR_PHONE
	};
	const char *address;
	uint32_t request;
	uint32_t action;
	uint32_t type;
	size_t offset;
	int error;

	/* request, action, address, type. */
	if (size < 8U)
		return EPROTO;
	request = bt_word(bytes, 0U);
	action = bt_word(bytes, 4U);
	error = bt_string(bytes, size, 8U, KL_SYSTEM_BT_ADDRESS_MAX, &address, &offset);
	if (error != 0 || offset + 4U != size)
		return EPROTO;
	type = bt_word(bytes, offset);

	/* An action there is no such is refused, not the client's end. */
	if (action == 0U || action >= sizeof(actions) / sizeof(actions[0])) {
		bt_result(object->client, object->id, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Asked of the backend. */
	bt_ask(object->client, object->id, request, actions[action], address, type);
	return 0;
}

/* Sends a client's request to the backend: refused at once, or answered at a later tick. */
static void
bt_ask(
	struct kwl_client *client,
	uint32_t id,
	uint32_t request,
	unsigned what,
	const char *address,
	unsigned type)
{
	uint32_t backend_request;
	int error;

	/* No backend. */
	if (bt_state.backend == NULL) {
		bt_result(client, id, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return;
	}

	/* Asked of the backend (addresses are not secret: the log has the action only). */
	backend_request = 0U;
	error = kl_backend_bluetooth_request(bt_state.backend, what, address, type, &backend_request);
	printf("KWL BT request client=%llu action=%s error=%d\n", (unsigned long long)client->number, bt_action_name(what), error);
	if (error != 0) {
		bt_result(client, id, request, bt_applied(error));
		return;
	}

	/* Answered at a later tick. */
	bt_wait(client->number, id, backend_request, request);
}

/* Keeps a request waiting for its answer (with the table full, its answer is lost: the state still shows the outcome). */
static void
bt_wait(
	uint64_t client,
	uint32_t object,
	uint32_t backend,
	uint32_t request)
{
	size_t index;

	/* A free slot. */
	for (index = 0; index < BT_WAITING_MAX; index++) {
		if (bt_state.waiting[index].used)
			continue;
		bt_state.waiting[index].used = 1;
		bt_state.waiting[index].backend = backend;
		bt_state.waiting[index].client = client;
		bt_state.waiting[index].object = object;
		bt_state.waiting[index].request = request;
		return;
	}

	/* None. */
	printf("KWL BT waiting full\n");
}

/*
 * Lets go of the askings for scans that ran out, and tells the backend
 * whether anyone watches and scans when that changed.
 */
static void
bt_holders(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	unsigned watching;
	unsigned scanning;
	uint64_t now;

	/* Each asking for scans not asked again within its minute. */
	if (bt_state.scanners > 0U) {
		now = kwl_milliseconds();
		for (client = server->clients; client != NULL; client = client->next) {
			for (object = client->objects; object != NULL; object = object->next) {
				if (object->kind != KWL_SYSTEM_BLUETOOTH || object->dead || !object->bluetooth_scan)
					continue;
				if (now < object->bluetooth_scan_until)
					continue;
				printf("KWL BT scan expired client=%llu id=%u\n", (unsigned long long)client->number, object->id);
				(void)bt_scan(object, 0U);
			}
		}
	}

	/* Watching: an object, or the bar's menu; scanning: an object (it is watched too). */
	watching = 0U;
	if (bt_state.watchers > 0U || bt_state.scanners > 0U || bt_state.bar_watching)
		watching = 1U;
	scanning = 0U;
	if (bt_state.scanners > 0U)
		scanning = 1U;

	/* Told when it changed. */
	if (watching != bt_state.told_watching) {
		bt_state.told_watching = watching;
		kl_backend_bluetooth_set_watching(bt_state.backend, watching);
		printf("KWL BT watch on=%u\n", watching);
	}

	/* And the scanning. */
	if (scanning != bt_state.told_scanning) {
		bt_state.told_scanning = scanning;
		kl_backend_bluetooth_set_scanning(bt_state.backend, scanning);
		printf("KWL BT scan on=%u\n", scanning);
	}
}

/* Sends a client's Bluetooth object the state, the devices and done. */
static void
bt_state_to(
	struct kwl_client *client,
	uint32_t id)
{
	static unsigned char payload[BT_EVENT_MAX];
	const struct kl_backend_bluetooth_device *device;
	const struct kl_backend_bluetooth_state *state;
	uint32_t words[4];
	int32_t numbers[2];
	size_t length;
	size_t index;

	/* The state: reachable, state, flags, features, address, name. */
	state = &bt_state.state;
	words[0] = state->reachable;
	words[1] = state->state;
	words[2] = 0U;
	if (state->scanning)
		words[2] |= KL_SYSTEM_BT_SCANNING;
	if (state->pairing)
		words[2] |= KL_SYSTEM_BT_PAIRING;
	if (state->power)
		words[2] |= KL_SYSTEM_BT_POWERED;
	if (state->agent)
		words[2] |= KL_SYSTEM_BT_ANSWERS;
	words[3] = state->features;
	memcpy(payload, words, 16U);
	length = bt_put_string(payload, 16U, state->address);
	length = bt_put_string(payload, length, state->name);
	(void)kwl_emit(client, id, KL_SYSTEM_BLUETOOTH_EVENT_STATE, payload, length);

	/* Each device: address, type, name, kind, flags, battery, rssi. */
	for (index = 0; index < bt_state.count; index++) {
		device = &bt_state.devices[index];
		length = bt_put_string(payload, 0U, device->address);
		words[0] = device->type;
		memcpy(payload + length, words, 4U);
		length = bt_put_string(payload, length + 4U, device->name);
		words[0] = device->kind;
		words[1] = 0U;
		if (device->paired)
			words[1] |= KL_SYSTEM_BT_PAIRED;
		if (device->legacy)
			words[1] |= KL_SYSTEM_BT_LEGACY;
		if (device->connected)
			words[1] |= KL_SYSTEM_BT_CONNECTED;
		numbers[0] = device->battery;
		numbers[1] = device->rssi;
		memcpy(payload + length, words, 8U);
		memcpy(payload + length + 8U, numbers, 8U);
		length += 16U;
		(void)kwl_emit(client, id, KL_SYSTEM_BLUETOOTH_EVENT_DEVICE, payload, length);
	}

	/* The whole state. */
	bt_state.serial++;
	words[0] = bt_state.serial;
	(void)kwl_emit(client, id, KL_SYSTEM_BLUETOOTH_EVENT_DONE, words, 4U);
}

/* Sends every Bluetooth object of every client the state. */
static void
bt_tell(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;

	/* Each live Bluetooth object. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->dead || object->kind != KWL_SYSTEM_BLUETOOTH)
				continue;
			bt_state_to(client, object->id);
		}
	}
}

/* Sends the backend's answers to the clients that asked (the bar's to the bar). */
static void
bt_answers(
	struct kwl_server *server)
{
	struct bt_waiting *waiting;
	struct kwl_client *client;
	char reason[KL_BACKEND_BT_REASON_MAX];
	uint32_t backend_request;
	size_t index;
	int error;
	int taken;

	/* Each answer. */
	for (;;) {
		taken = kl_backend_bluetooth_take_result(bt_state.backend, &backend_request, &error, reason, sizeof(reason));
		if (!taken)
			break;
		printf("KWL BT result id=%u error=%d reason=%s\n", backend_request, error, reason);

		/* A pairing or a forgetting may change the user's phone: its record read again at once (ws197-p004 section 4.3). */
		kwl_phone_refresh();

		/* The request it answers. */
		waiting = NULL;
		for (index = 0; index < BT_WAITING_MAX; index++) {
			if (bt_state.waiting[index].used && bt_state.waiting[index].backend == backend_request)
				waiting = &bt_state.waiting[index];
		}

		/* None waits for it. */
		if (waiting == NULL)
			continue;
		waiting->used = 0;

		/* The bar's own. */
		if (waiting->client == 0U) {
			bt_state.bar_error = error;
			bt_state.bar_answered = 1U;
			server->dirty = 1;
			continue;
		}

		/* Its client, still there. */
		for (client = server->clients; client != NULL; client = client->next) {
			if (client->number == waiting->client && !client->fatal)
				break;
		}

		/* The client went. */
		if (client == NULL)
			continue;
		bt_result(client, waiting->object, waiting->request, bt_applied(error));
	}
}

/* Hands each pairing's question to the window (bluetooth-ask.c). */
static void
bt_questions(
	struct kwl_server *server)
{
	struct kl_backend_bluetooth_question question;
	int taken;

	/* Each question, oldest first. */
	for (;;) {
		memset(&question, 0, sizeof(question));
		taken = kl_backend_bluetooth_take_question(bt_state.backend, &question);
		if (!taken)
			break;
		kwl_bluetooth_ask_take(server, &question);
	}
}

/* Answers a request: result(request, applied, saved), never saved (the service keeps its own). */
static void
bt_result(
	struct kwl_client *client,
	uint32_t id,
	uint32_t request,
	uint32_t applied)
{
	uint32_t words[3];

	/* request, applied, saved. */
	words[0] = request;
	words[1] = applied;
	words[2] = 0U;
	(void)kwl_emit(client, id, KL_SYSTEM_BLUETOOTH_EVENT_RESULT, words, sizeof(words));
}

/* The result's number for a backend's errno value. */
static uint32_t
bt_applied(
	int error)
{
	static const struct {
		int error;
		uint32_t applied;
	} map[] = {
		{ 0, KL_SYSTEM_RESULT_OK },
		{ EACCES, KL_SYSTEM_RESULT_DENIED },
		{ EBUSY, KL_SYSTEM_RESULT_BUSY },
		{ ENOTSUP, KL_SYSTEM_RESULT_UNSUPPORTED },
		{ EINVAL, KL_SYSTEM_RESULT_INVALID },
		{ ENOTCONN, KL_SYSTEM_RESULT_UNAVAILABLE },
		{ ENETDOWN, KL_SYSTEM_RESULT_UNAVAILABLE },
		{ ETIMEDOUT, KL_SYSTEM_RESULT_UNREACHABLE },
		{ ECONNREFUSED, KL_SYSTEM_RESULT_REFUSED }
	};
	size_t index;

	/* A known value. */
	for (index = 0; index < sizeof(map) / sizeof(map[0]); index++) {
		if (map[index].error == error)
			return map[index].applied;
	}

	/* Any other. */
	return KL_SYSTEM_RESULT_FAILED;
}

/* Logs the state and the devices for the tests (the names are the devices', not secret). */
static void
bt_log_state(
	void)
{
	const struct kl_backend_bluetooth_device *device;
	const struct kl_backend_bluetooth_state *state;
	size_t index;

	/* The state. */
	state = &bt_state.state;
	printf("KWL BT state reachable=%u state=%u power=%u scanning=%u pairing=%u agent=%u features=%u devices=%zu\n",
	    state->reachable, state->state, state->power, state->scanning, state->pairing, state->agent, state->features, bt_state.count);

	/* Each device. */
	for (index = 0; index < bt_state.count; index++) {
		device = &bt_state.devices[index];
		printf("KWL BT device address=%s type=%u kind=%u paired=%u connected=%u name=%s\n",
		    device->address, device->type, device->kind, device->paired, device->connected, device->name);
	}
}

/* Names a backend request for the log. */
static const char *
bt_action_name(
	unsigned what)
{
	static const char *const names[] = { "none", "power-on", "power-off", "pair", "forget", "connect", "disconnect", "pair-phone" };

	/* A known one. */
	if (what < sizeof(names) / sizeof(names[0]))
		return names[what];
	return "other";
}

/*
 * Reads a string argument in place, at most bound bytes with its NUL;
 * *next is the offset after it.  Returns 0, or EPROTO.
 */
static int
bt_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	size_t bound,
	const char **text,
	size_t *next)
{
	const void *inner;
	uint32_t length;
	size_t padded;

	/* The length, with the NUL, within the request and the bound. */
	if (offset + 4U > size)
		return EPROTO;
	length = bt_word(bytes, offset);
	if (length == 0U || length > bound)
		return EPROTO;

	/* The bytes, padded to a word, within the request. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;

	/* The text ends with its NUL and has no other. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;
	inner = memchr(bytes + offset + 4U, '\0', length - 1U);
	if (inner != NULL)
		return EPROTO;

	/* Succeeded: the text where it is. */
	*text = (const char *)(bytes + offset + 4U);
	*next = offset + 4U + padded;
	return 0;
}

/* Writes a string argument and reports the offset after it. */
static size_t
bt_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, and zeros to a four-byte boundary. */
	length = (uint32_t)strlen(text) + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length - 1U);
	return offset + 4U + padded;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
bt_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
