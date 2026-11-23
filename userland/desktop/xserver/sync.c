/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SYNC extension (version 3.1): counters, alarms and fences, and
 * clients that wait for them.
 *
 * A counter is a 64-bit value a client makes and changes; the server's
 * own SERVERTIME counts the server's milliseconds.  A client may wait
 * (Await) until one of a list of conditions on counters holds, and none
 * of its requests is handled meanwhile (client.c leaves a blocked client
 * alone).  An alarm sends AlarmNotify to the clients that want it each
 * time its condition becomes true, then moves its test value on by its
 * delta.  A fence is a flag a client triggers and resets, and AwaitFence
 * waits until one of a list of fences is triggered.
 *
 * The window manager uses a client's counter to resize a window in step
 * with the client's drawing (_NET_WM_SYNC_REQUEST, wm.c), so every change
 * of a counter is told to it.  The scheduling priorities are kept for
 * their answers only: the core handles its clients in turn.
 *
 * Wire layouts and the conditions' rules follow the public SYNC extension
 * protocol (version 3.1); where it leaves a choice, the answers are those
 * of the X.Org server.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The requests' minor opcodes. */
#define SYNC_INITIALIZE			0U
#define SYNC_LIST_SYSTEM_COUNTERS	1U
#define SYNC_CREATE_COUNTER		2U
#define SYNC_SET_COUNTER		3U
#define SYNC_CHANGE_COUNTER		4U
#define SYNC_QUERY_COUNTER		5U
#define SYNC_DESTROY_COUNTER		6U
#define SYNC_AWAIT			7U
#define SYNC_CREATE_ALARM		8U
#define SYNC_CHANGE_ALARM		9U
#define SYNC_QUERY_ALARM		10U
#define SYNC_DESTROY_ALARM		11U
#define SYNC_SET_PRIORITY		12U
#define SYNC_GET_PRIORITY		13U
#define SYNC_CREATE_FENCE		14U
#define SYNC_TRIGGER_FENCE		15U
#define SYNC_RESET_FENCE		16U
#define SYNC_DESTROY_FENCE		17U
#define SYNC_QUERY_FENCE		18U
#define SYNC_AWAIT_FENCE		19U

/* The version the extension speaks. */
#define SYNC_VERSION_MAJOR		3U
#define SYNC_VERSION_MINOR		1U

/* The events' offsets from the first. */
#define SYNC_COUNTER_NOTIFY		0U
#define SYNC_ALARM_NOTIFY		1U

/* A condition's value: given as it is, or added to the counter's value now. */
#define SYNC_ABSOLUTE			0U
#define SYNC_RELATIVE			1U

/* A condition's test: the counter crosses the value upward or downward, or is at or above, or at or below it. */
#define SYNC_POSITIVE_TRANSITION	0U
#define SYNC_NEGATIVE_TRANSITION	1U
#define SYNC_POSITIVE_COMPARISON	2U
#define SYNC_NEGATIVE_COMPARISON	3U

/* An alarm's states. */
#define SYNC_ALARM_ACTIVE		0U
#define SYNC_ALARM_INACTIVE		1U
#define SYNC_ALARM_DESTROYED		2U

/* The values an alarm's request may carry, by their bits. */
#define SYNC_CA_COUNTER			(1U << 0)
#define SYNC_CA_VALUE_TYPE		(1U << 1)
#define SYNC_CA_VALUE			(1U << 2)
#define SYNC_CA_TEST_TYPE		(1U << 3)
#define SYNC_CA_DELTA			(1U << 4)
#define SYNC_CA_EVENTS			(1U << 5)
#define SYNC_CA_ALL			0x3fU

/* The bytes of one condition of Await: the counter, the value's kind, the value, the test, the event threshold. */
#define SYNC_CONDITION_BYTES		28U

/*
 * The server's own counter SERVERTIME: its id (one no client chooses),
 * its name and its resolution (milliseconds, as the X.Org server says).
 */
#define SYNC_SERVERTIME_XID		0x00000110U
#define SYNC_SERVERTIME_NAME		"SERVERTIME"
#define SYNC_SERVERTIME_RESOLUTION	4U

/* The most clients one alarm tells. */
#define SYNC_ALARM_CLIENTS_MAX		X11_CLIENTS_LIMIT

/*
 * A counter a client made: its id, its owner and its value.
 */
struct sync_counter {
	uint32_t id;
	unsigned owner;
	int64_t value;
};

/*
 * An alarm: its id and owner, its condition (the counter, None as 0; how
 * its value was given and the value; the value the counter is tested
 * against; the test), the delta the test value moves by each time it
 * fires, its state, and the clients it tells (their slots).
 */
struct sync_alarm {
	uint32_t id;
	unsigned owner;
	uint32_t counter;
	unsigned value_type;
	int64_t wait_value;
	int64_t test_value;
	unsigned test_type;
	int64_t delta;
	unsigned state;
	unsigned clients[SYNC_ALARM_CLIENTS_MAX];
	unsigned client_count;
};

/*
 * A fence: its id, its owner and whether it is triggered.
 */
struct sync_fence {
	uint32_t id;
	unsigned owner;
	int triggered;
};

/*
 * One condition of a wait: the counter, the value it is tested against,
 * the test, the threshold of its CounterNotify, and whether its counter
 * was destroyed meanwhile.
 */
struct sync_condition {
	uint32_t counter;
	int64_t test_value;
	unsigned test_type;
	int64_t threshold;
	int destroyed;
};

/*
 * A client's wait (Await or AwaitFence): the client, its conditions on
 * counters or the fences it waits for (one of the two lists, count long).
 */
struct sync_wait {
	unsigned client;
	struct sync_condition *conditions;
	uint32_t *fences;
	size_t count;
};

/*
 * SYNC's state of a server: SERVERTIME's value and the server's time it
 * was last brought up to, the waits under way, whether a wait ended since
 * the last tick (its client goes on), and each client's priority.
 */
struct x11_sync {
	int64_t servertime;
	uint32_t servertime_at;
	struct sync_wait **waits;
	unsigned wait_count;
	unsigned wait_capacity;
	int woke;
	int32_t *priorities;
};

static struct x11_sync *sync_state(struct x11server *server);
static unsigned sync_initialize(struct x11_ext_request *request);
static unsigned sync_list_system_counters(struct x11_ext_request *request);
static unsigned sync_create_counter(struct x11_ext_request *request);
static unsigned sync_set_counter(struct x11_ext_request *request, int change);
static unsigned sync_query_counter(struct x11_ext_request *request);
static unsigned sync_destroy_counter(struct x11_ext_request *request);
static unsigned sync_await(struct x11_ext_request *request);
static unsigned sync_create_alarm(struct x11_ext_request *request);
static unsigned sync_change_alarm(struct x11_ext_request *request);
static unsigned sync_query_alarm(struct x11_ext_request *request);
static unsigned sync_destroy_alarm(struct x11_ext_request *request);
static unsigned sync_set_priority(struct x11_ext_request *request);
static unsigned sync_get_priority(struct x11_ext_request *request);
static unsigned sync_create_fence(struct x11_ext_request *request);
static unsigned sync_fence_request(struct x11_ext_request *request);
static unsigned sync_await_fence(struct x11_ext_request *request);
static unsigned sync_new_id(struct x11_ext_request *request, uint32_t id);
static int sync_alarm_fit(uint32_t mask, size_t length);
static void sync_alarm_start(struct x11server *server, struct sync_alarm *alarm);
static int sync_counter_find(struct x11server *server, uint32_t id, int64_t *value, struct sync_counter **counter);
static unsigned sync_alarm_values(struct x11_ext_request *request, struct sync_alarm *alarm, uint32_t mask, const uint8_t *values);
static int sync_test(unsigned test_type, int64_t test_value, int64_t old_value, int64_t new_value);
static int sync_add(int64_t a, int64_t b, int64_t *sum);
static int64_t sync_read64(const uint8_t *bytes, int msb);
static void sync_write64(uint8_t *bytes, int64_t value, int msb);
static void sync_counter_changed(struct x11server *server, uint32_t counter, int64_t old_value, int64_t new_value);
static void sync_counter_destroyed(struct x11server *server, uint32_t counter, int64_t value);
static int sync_compare_down(const void *a, const void *b);
static void sync_alarm_check(struct x11server *server, struct sync_alarm *alarm, int64_t old_value);
static void sync_alarm_fire(struct x11server *server, struct sync_alarm *alarm);
static void sync_alarm_notify(struct x11server *server, struct sync_alarm *alarm, int64_t alarm_value, int64_t counter_value);
static void sync_alarm_free(struct x11server *server, struct sync_alarm *alarm);
static int sync_wait_add(struct x11_sync *state, struct sync_wait *wait);
static void sync_wait_check(struct x11server *server, struct sync_wait *wait, uint32_t counter, int64_t old_value, int64_t new_value);
static void sync_wait_end(struct x11server *server, struct sync_wait *wait, int notify);
static void sync_wait_free(struct sync_wait *wait);
static void sync_servertime(struct x11server *server);
static void sync_release(struct x11server *server, unsigned owner, unsigned type);

/*
 * Answers a SYNC request by its minor opcode.  Returns 0, or the error to
 * send (request->bad names the value that was wrong).
 */
unsigned
x11_sync_request(
	struct x11_ext_request *request)
{
	struct x11_sync *state;
	unsigned code;

	/* The state, made on the first request, with SERVERTIME brought up to now. */
	state = sync_state(request->server);
	if (state == NULL)
		return X11_BAD_ALLOC;
	sync_servertime(request->server);

	/* The request of the minor opcode. */
	switch (request->bytes[1]) {
	case SYNC_INITIALIZE:
		code = sync_initialize(request);
		break;
	case SYNC_LIST_SYSTEM_COUNTERS:
		code = sync_list_system_counters(request);
		break;
	case SYNC_CREATE_COUNTER:
		code = sync_create_counter(request);
		break;
	case SYNC_SET_COUNTER:
		code = sync_set_counter(request, 0);
		break;
	case SYNC_CHANGE_COUNTER:
		code = sync_set_counter(request, 1);
		break;
	case SYNC_QUERY_COUNTER:
		code = sync_query_counter(request);
		break;
	case SYNC_DESTROY_COUNTER:
		code = sync_destroy_counter(request);
		break;
	case SYNC_AWAIT:
		code = sync_await(request);
		break;
	case SYNC_CREATE_ALARM:
		code = sync_create_alarm(request);
		break;
	case SYNC_CHANGE_ALARM:
		code = sync_change_alarm(request);
		break;
	case SYNC_QUERY_ALARM:
		code = sync_query_alarm(request);
		break;
	case SYNC_DESTROY_ALARM:
		code = sync_destroy_alarm(request);
		break;
	case SYNC_SET_PRIORITY:
		code = sync_set_priority(request);
		break;
	case SYNC_GET_PRIORITY:
		code = sync_get_priority(request);
		break;
	case SYNC_CREATE_FENCE:
		code = sync_create_fence(request);
		break;
	case SYNC_TRIGGER_FENCE:
	case SYNC_RESET_FENCE:
	case SYNC_DESTROY_FENCE:
	case SYNC_QUERY_FENCE:
		code = sync_fence_request(request);
		break;
	case SYNC_AWAIT_FENCE:
		code = sync_await_fence(request);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Forgets a closed client: its waits end without events, its counters go
 * (their waiters and alarms hear of it), its alarms and fences go, and no
 * alarm tells it any more.
 */
void
x11_sync_forget_client(
	struct x11server *server,
	unsigned index)
{
	struct x11_sync *state;
	struct sync_alarm *alarm;
	uint32_t *ids;
	size_t count;
	size_t id_index;
	unsigned slot;
	unsigned listed;
	int error;

	/* Its waits, without events. */
	state = server->sync;
	if (state != NULL) {
		slot = 0U;
		while (slot < state->wait_count) {
			if (state->waits[slot]->client != index) {
				slot++;
				continue;
			}
			sync_wait_free(state->waits[slot]);
			state->waits[slot] = state->waits[state->wait_count - 1U];
			state->wait_count--;
		}
		state->priorities[index] = 0;
	}

	/* Its counters, alarms and fences. */
	sync_release(server, index, X11_RESOURCE_COUNTER);
	sync_release(server, index, X11_RESOURCE_ALARM);
	sync_release(server, index, X11_RESOURCE_FENCE);

	/* The other clients' alarms no longer tell it. */
	for (slot = 0U; slot < server->clients_max; slot++) {
		error = x11_resource_owned(server, slot, X11_RESOURCE_ALARM, &ids, &count);
		if (error != 0)
			continue;
		for (id_index = 0U; id_index < count; id_index++) {
			alarm = x11_resource_find(server, ids[id_index], X11_RESOURCE_ALARM);
			if (alarm == NULL)
				continue;
			for (listed = 0U; listed < alarm->client_count; listed++) {
				if (alarm->clients[listed] != index)
					continue;
				alarm->clients[listed] = alarm->clients[alarm->client_count - 1U];
				alarm->client_count--;
				break;
			}
		}
		free(ids);
	}
}

/*
 * Brings SERVERTIME up to the server's time and checks what waits on it,
 * and reports whether a client's wait ended (it goes on now).  *timed is
 * nonzero while a wait or an active alarm is on SERVERTIME, so that the
 * caller looks again soon.  Returns nonzero when a client woke.
 */
int
x11_sync_tick(
	struct x11server *server,
	int *timed)
{
	struct x11_sync *state;
	struct sync_alarm *alarm;
	uint32_t *ids;
	size_t count;
	size_t id_index;
	unsigned slot;
	unsigned condition;
	int woke;
	int error;

	/* Nothing made. */
	*timed = 0;
	state = server->sync;
	if (state == NULL)
		return 0;

	/* SERVERTIME moves on, and what waits on it is checked. */
	sync_servertime(server);

	/* A wait on SERVERTIME has the caller look again soon. */
	for (slot = 0U; slot < state->wait_count; slot++) {
		for (condition = 0U; state->waits[slot]->conditions != NULL && condition < state->waits[slot]->count; condition++) {
			if (state->waits[slot]->conditions[condition].counter == SYNC_SERVERTIME_XID)
				*timed = 1;
		}
	}

	/* So does an active alarm on it. */
	for (slot = 0U; slot < server->clients_max && !*timed; slot++) {
		error = x11_resource_owned(server, slot, X11_RESOURCE_ALARM, &ids, &count);
		if (error != 0)
			continue;
		for (id_index = 0U; id_index < count; id_index++) {
			alarm = x11_resource_find(server, ids[id_index], X11_RESOURCE_ALARM);
			if (alarm != NULL && alarm->counter == SYNC_SERVERTIME_XID && alarm->state == SYNC_ALARM_ACTIVE)
				*timed = 1;
		}
		free(ids);
	}

	/* Succeeded: whether a wait ended since the last tick. */
	woke = state->woke;
	state->woke = 0;
	return woke;
}

/*
 * Frees SYNC's state when the server ends (after every client is closed).
 */
void
x11_sync_fini(
	struct x11server *server)
{
	struct x11_sync *state;
	unsigned slot;

	/* Nothing made. */
	state = server->sync;
	if (state == NULL)
		return;

	/* Succeeded: the waits left and the state. */
	for (slot = 0U; slot < state->wait_count; slot++)
		sync_wait_free(state->waits[slot]);
	free(state->waits);
	free(state->priorities);
	free(state);
	server->sync = NULL;
}

/*
 * Reads a counter's value (SERVERTIME's included).  Returns 1 with it, 0
 * when no counter has the id.
 */
int
x11_sync_counter_value(
	struct x11server *server,
	uint32_t id,
	int64_t *value)
{
	struct sync_counter *counter;
	int found;

	/* The counter. */
	found = sync_counter_find(server, id, value, &counter);
	if (!found)
		return 0;

	/* Succeeded: its value. */
	return 1;
}

/* Returns SYNC's state, made when first asked for (NULL without memory). */
static struct x11_sync *
sync_state(
	struct x11server *server)
{
	struct x11_sync *state;

	/* Already made. */
	if (server->sync != NULL)
		return server->sync;

	/* The state, a priority for each client slot, and SERVERTIME from the server's time. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return NULL;
	state->priorities = calloc(server->clients_max, sizeof(*state->priorities));
	if (state->priorities == NULL) {
		free(state);
		return NULL;
	}

	/* Succeeded: SERVERTIME starts at the server's time. */
	state->servertime = (int64_t)server->now;
	state->servertime_at = server->now;
	server->sync = state;
	return state;
}

/* Initialize: 3.1. */
static unsigned
sync_initialize(
	struct x11_ext_request *request)
{
	uint8_t reply[32];

	/* The client's version. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;

	/* Succeeded: the server's. */
	memset(reply, 0, sizeof(reply));
	reply[8] = SYNC_VERSION_MAJOR;
	reply[9] = SYNC_VERSION_MINOR;
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* ListSystemCounters: SERVERTIME, its id, resolution and name. */
static unsigned
sync_list_system_counters(
	struct x11_ext_request *request)
{
	uint8_t reply[32U + 28U];
	size_t name_length;
	size_t size;

	/* The header alone. */
	if (request->length != 4U)
		return X11_BAD_LENGTH;

	/* The one counter: its id, resolution (high and low), name's length and name, padded to a word. */
	memset(reply, 0, sizeof(reply));
	name_length = sizeof(SYNC_SERVERTIME_NAME) - 1U;
	x11_write32(reply + 8, 1U, request->msb);
	x11_write32(reply + 32, SYNC_SERVERTIME_XID, request->msb);
	x11_write32(reply + 36, 0U, request->msb);
	x11_write32(reply + 40, SYNC_SERVERTIME_RESOLUTION, request->msb);
	x11_write16(reply + 44, (uint16_t)name_length, request->msb);
	memcpy(reply + 46, SYNC_SERVERTIME_NAME, name_length);
	size = 32U + ((14U + name_length + 3U) & ~(size_t)3U);

	/* Succeeded: sent. */
	x11_reply(request->server, request->client, reply, size);
	return 0U;
}

/* CreateCounter: a counter of the client's with a value. */
static unsigned
sync_create_counter(
	struct x11_ext_request *request)
{
	struct sync_counter *counter;
	uint32_t id;
	unsigned code;
	int error;

	/* The id and the value. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = sync_new_id(request, id);
	if (code != 0U)
		return code;

	/* The counter, charged to the client. */
	error = x11_memory_charge(request->server, request->index, sizeof(*counter));
	if (error != 0)
		return X11_BAD_ALLOC;
	counter = calloc(1U, sizeof(*counter));
	if (counter == NULL) {
		x11_memory_release(request->server, request->index, sizeof(*counter));
		return X11_BAD_ALLOC;
	}
	counter->id = id;
	counter->owner = request->index;
	counter->value = sync_read64(request->bytes + 8, request->msb);
	error = x11_resource_add(request->server, id, X11_RESOURCE_COUNTER, counter);
	if (error != 0) {
		x11_memory_release(request->server, request->index, sizeof(*counter));
		free(counter);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/*
 * SetCounter and ChangeCounter: a counter of a client's takes a value, or
 * moves by an amount (BadValue when it would leave the 64-bit range); the
 * server's own counters cannot be set.  What waits on it is checked.
 */
static unsigned
sync_set_counter(
	struct x11_ext_request *request,
	int change)
{
	struct sync_counter *counter;
	int64_t old_value;
	int64_t new_value;
	int64_t amount;
	uint32_t id;
	int found;
	int overflow;

	/* The counter and the value or amount. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	found = sync_counter_find(request->server, id, &old_value, &counter);
	if (!found) {
		request->bad = id;
		return X11_BAD_COUNTER;
	}
	if (counter == NULL)
		return X11_BAD_ACCESS;
	amount = sync_read64(request->bytes + 8, request->msb);

	/* The new value. */
	new_value = amount;
	if (change) {
		overflow = sync_add(old_value, amount, &new_value);
		if (overflow) {
			request->bad = x11_read32(request->bytes + 8, request->msb);
			return X11_BAD_VALUE;
		}
	}

	/* Succeeded: set, and what waits on it is checked. */
	counter->value = new_value;
	sync_counter_changed(request->server, id, old_value, new_value);
	return 0U;
}

/* QueryCounter: a counter's value. */
static unsigned
sync_query_counter(
	struct x11_ext_request *request)
{
	struct sync_counter *counter;
	uint8_t reply[32];
	int64_t value;
	uint32_t id;
	int found;

	/* The counter. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	found = sync_counter_find(request->server, id, &value, &counter);
	if (!found) {
		request->bad = id;
		return X11_BAD_COUNTER;
	}

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	sync_write64(reply + 8, value, request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* DestroyCounter: a client's counter goes (BadAccess for the server's own). */
static unsigned
sync_destroy_counter(
	struct x11_ext_request *request)
{
	struct sync_counter *counter;
	int64_t value;
	uint32_t id;
	int found;

	/* The counter. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	found = sync_counter_find(request->server, id, &value, &counter);
	if (!found) {
		request->bad = id;
		return X11_BAD_COUNTER;
	}
	if (counter == NULL)
		return X11_BAD_ACCESS;

	/* Succeeded: gone, and what waits on it hears so. */
	x11_resource_remove(request->server, id);
	x11_memory_release(request->server, counter->owner, sizeof(*counter));
	free(counter);
	sync_counter_destroyed(request->server, id, value);
	return 0U;
}

/*
 * Await: the client waits until one of the conditions holds (at once when
 * one does now).  Each condition names a counter, a value (absolute, or
 * relative to the counter now), a test and an event threshold.
 */
static unsigned
sync_await(
	struct x11_ext_request *request)
{
	struct sync_condition *condition;
	struct sync_counter *counter;
	struct sync_wait *wait;
	const uint8_t *bytes;
	int64_t value;
	int64_t wait_value;
	uint32_t id;
	uint32_t value_type;
	size_t count;
	size_t index;
	int found;
	int overflow;
	int error;

	/* At least one whole condition. */
	if (request->length < 4U || (request->length - 4U) % SYNC_CONDITION_BYTES != 0U)
		return X11_BAD_LENGTH;
	count = (request->length - 4U) / SYNC_CONDITION_BYTES;
	if (count == 0U)
		return X11_BAD_VALUE;

	/* The wait, with room for its conditions. */
	wait = calloc(1U, sizeof(*wait));
	if (wait == NULL)
		return X11_BAD_ALLOC;
	wait->conditions = calloc(count, sizeof(*wait->conditions));
	if (wait->conditions == NULL) {
		free(wait);
		return X11_BAD_ALLOC;
	}
	wait->client = request->index;
	wait->count = count;

	/* Each condition, checked. */
	for (index = 0U; index < count; index++) {
		bytes = request->bytes + 4U + index * SYNC_CONDITION_BYTES;
		condition = &wait->conditions[index];
		id = x11_read32(bytes, request->msb);
		found = sync_counter_find(request->server, id, &value, &counter);
		if (!found) {
			sync_wait_free(wait);
			request->bad = id;
			return X11_BAD_COUNTER;
		}
		value_type = x11_read32(bytes + 4, request->msb);
		if (value_type > SYNC_RELATIVE) {
			sync_wait_free(wait);
			request->bad = value_type;
			return X11_BAD_VALUE;
		}
		condition->test_type = x11_read32(bytes + 16, request->msb);
		if (condition->test_type > SYNC_NEGATIVE_COMPARISON) {
			request->bad = condition->test_type;
			sync_wait_free(wait);
			return X11_BAD_VALUE;
		}
		condition->counter = id;
		wait_value = sync_read64(bytes + 8, request->msb);
		condition->test_value = wait_value;
		if (value_type == SYNC_RELATIVE) {
			overflow = sync_add(value, wait_value, &condition->test_value);
			if (overflow) {
				request->bad = x11_read32(bytes + 8, request->msb);
				sync_wait_free(wait);
				return X11_BAD_VALUE;
			}
		}
		condition->threshold = sync_read64(bytes + 20, request->msb);
	}

	/* The wait kept, and the client blocked until it ends. */
	error = sync_wait_add(request->server->sync, wait);
	if (error != 0) {
		sync_wait_free(wait);
		return X11_BAD_ALLOC;
	}
	request->client->blocked = 1;

	/* Succeeded: a condition that holds now ends it at once. */
	for (index = 0U; index < count; index++) {
		found = sync_counter_find(request->server, wait->conditions[index].counter, &value, &counter);
		if (!found)
			continue;
		sync_wait_check(request->server, wait, wait->conditions[index].counter, value, value);
		if (!request->client->blocked)
			break;
	}
	return 0U;
}

/* CreateAlarm: an alarm of the client's with the values given and the defaults, which it tells. */
static unsigned
sync_create_alarm(
	struct x11_ext_request *request)
{
	struct sync_alarm *alarm;
	uint32_t id;
	uint32_t mask;
	unsigned code;
	int fit;
	int error;

	/* The id, the mask and its values. */
	if (request->length < 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	mask = x11_read32(request->bytes + 8, request->msb);
	if ((mask & ~SYNC_CA_ALL) != 0U) {
		request->bad = mask;
		return X11_BAD_VALUE;
	}
	fit = sync_alarm_fit(mask, request->length);
	if (!fit)
		return X11_BAD_LENGTH;
	code = sync_new_id(request, id);
	if (code != 0U)
		return code;

	/* The alarm with the protocol's defaults, telling its client. */
	error = x11_memory_charge(request->server, request->index, sizeof(*alarm));
	if (error != 0)
		return X11_BAD_ALLOC;
	alarm = calloc(1U, sizeof(*alarm));
	if (alarm == NULL) {
		x11_memory_release(request->server, request->index, sizeof(*alarm));
		return X11_BAD_ALLOC;
	}
	alarm->id = id;
	alarm->owner = request->index;
	alarm->value_type = SYNC_ABSOLUTE;
	alarm->test_type = SYNC_POSITIVE_COMPARISON;
	alarm->delta = 1;
	alarm->clients[0] = request->index;
	alarm->client_count = 1U;

	/* The values given. */
	code = sync_alarm_values(request, alarm, mask, request->bytes + 12);
	if (code != 0U) {
		x11_memory_release(request->server, request->index, sizeof(*alarm));
		free(alarm);
		return code;
	}

	/* Known by its id. */
	error = x11_resource_add(request->server, id, X11_RESOURCE_ALARM, alarm);
	if (error != 0) {
		x11_memory_release(request->server, request->index, sizeof(*alarm));
		free(alarm);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: active unless it has no counter, and fired at once when its condition holds. */
	sync_alarm_start(request->server, alarm);
	return 0U;
}

/* ChangeAlarm: an alarm's values change, and it is active again (fired at once when its condition holds). */
static unsigned
sync_change_alarm(
	struct x11_ext_request *request)
{
	struct sync_alarm *alarm;
	uint32_t id;
	uint32_t mask;
	unsigned code;
	int fit;

	/* The alarm, the mask and its values. */
	if (request->length < 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	alarm = x11_resource_find(request->server, id, X11_RESOURCE_ALARM);
	if (alarm == NULL) {
		request->bad = id;
		return X11_BAD_ALARM;
	}
	mask = x11_read32(request->bytes + 8, request->msb);
	if ((mask & ~SYNC_CA_ALL) != 0U) {
		request->bad = mask;
		return X11_BAD_VALUE;
	}
	fit = sync_alarm_fit(mask, request->length);
	if (!fit)
		return X11_BAD_LENGTH;

	/* The values given. */
	code = sync_alarm_values(request, alarm, mask, request->bytes + 12);
	if (code != 0U)
		return code;

	/* Succeeded: active unless it has no counter, and fired at once when its condition holds. */
	sync_alarm_start(request->server, alarm);
	return 0U;
}

/* QueryAlarm: an alarm's condition, delta, whether it tells the client, and its state. */
static unsigned
sync_query_alarm(
	struct x11_ext_request *request)
{
	struct sync_alarm *alarm;
	uint8_t reply[40];
	uint32_t id;
	unsigned slot;
	int events;

	/* The alarm. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	alarm = x11_resource_find(request->server, id, X11_RESOURCE_ALARM);
	if (alarm == NULL) {
		request->bad = id;
		return X11_BAD_ALARM;
	}

	/* Whether it tells this client. */
	events = 0;
	for (slot = 0U; slot < alarm->client_count; slot++) {
		if (alarm->clients[slot] == request->index)
			events = 1;
	}

	/* Succeeded: the reply (the value tested against now). */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, alarm->counter, request->msb);
	x11_write32(reply + 12, alarm->value_type, request->msb);
	sync_write64(reply + 16, alarm->test_value, request->msb);
	x11_write32(reply + 24, alarm->test_type, request->msb);
	sync_write64(reply + 28, alarm->delta, request->msb);
	reply[36] = (uint8_t)events;
	reply[37] = (uint8_t)alarm->state;
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* DestroyAlarm: the alarm goes, its clients told (state Destroyed). */
static unsigned
sync_destroy_alarm(
	struct x11_ext_request *request)
{
	struct sync_alarm *alarm;
	uint32_t id;

	/* The alarm. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	alarm = x11_resource_find(request->server, id, X11_RESOURCE_ALARM);
	if (alarm == NULL) {
		request->bad = id;
		return X11_BAD_ALARM;
	}

	/* Succeeded: gone. */
	x11_resource_remove(request->server, id);
	sync_alarm_free(request->server, alarm);
	return 0U;
}

/* SetPriority: the priority of a client (None: this one), kept for GetPriority. */
static unsigned
sync_set_priority(
	struct x11_ext_request *request)
{
	struct x11_client *owner;
	uint32_t id;
	unsigned taken;
	unsigned slot;

	/* The client a resource of it names. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	slot = request->index;
	if (id != 0U) {
		taken = x11_resource_type(request->server, id);
		slot = (id >> X11_ID_SHIFT) - 1U;
		owner = NULL;
		if (taken != 0U && slot < request->server->clients_max)
			owner = x11_client_of(request->server, slot);
		if (owner == NULL) {
			request->bad = id;
			return X11_BAD_VALUE;
		}
	}

	/* Succeeded: kept. */
	request->server->sync->priorities[slot] = (int32_t)x11_read32(request->bytes + 8, request->msb);
	return 0U;
}

/* GetPriority: the priority of a client (None: this one). */
static unsigned
sync_get_priority(
	struct x11_ext_request *request)
{
	struct x11_client *owner;
	uint8_t reply[32];
	uint32_t id;
	unsigned taken;
	unsigned slot;

	/* The client a resource of it names. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	slot = request->index;
	if (id != 0U) {
		taken = x11_resource_type(request->server, id);
		slot = (id >> X11_ID_SHIFT) - 1U;
		owner = NULL;
		if (taken != 0U && slot < request->server->clients_max)
			owner = x11_client_of(request->server, slot);
		if (owner == NULL) {
			request->bad = id;
			return X11_BAD_VALUE;
		}
	}

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, (uint32_t)request->server->sync->priorities[slot], request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* CreateFence: a fence of the client's on a drawable's screen, triggered or not. */
static unsigned
sync_create_fence(
	struct x11_ext_request *request)
{
	struct sync_fence *fence;
	uint32_t drawable;
	uint32_t id;
	unsigned code;
	unsigned type;
	int error;

	/* The drawable, the id and the first state. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	drawable = x11_read32(request->bytes + 4, request->msb);
	type = x11_resource_type(request->server, drawable);
	if (drawable != X11_ROOT_XID && type != X11_RESOURCE_WINDOW && type != X11_RESOURCE_PIXMAP) {
		request->bad = drawable;
		return X11_BAD_DRAWABLE;
	}
	id = x11_read32(request->bytes + 8, request->msb);
	code = sync_new_id(request, id);
	if (code != 0U)
		return code;

	/* The fence, charged to the client. */
	error = x11_memory_charge(request->server, request->index, sizeof(*fence));
	if (error != 0)
		return X11_BAD_ALLOC;
	fence = calloc(1U, sizeof(*fence));
	if (fence == NULL) {
		x11_memory_release(request->server, request->index, sizeof(*fence));
		return X11_BAD_ALLOC;
	}
	fence->id = id;
	fence->owner = request->index;
	fence->triggered = 0;
	if (request->bytes[12] != 0U)
		fence->triggered = 1;
	error = x11_resource_add(request->server, id, X11_RESOURCE_FENCE, fence);
	if (error != 0) {
		x11_memory_release(request->server, request->index, sizeof(*fence));
		free(fence);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/*
 * TriggerFence, ResetFence, DestroyFence and QueryFence: a fence is
 * triggered (its waiters go on), reset, destroyed (its waiters go on), or
 * asked whether it is triggered.
 */
static unsigned
sync_fence_request(
	struct x11_ext_request *request)
{
	struct x11_sync *state;
	struct sync_fence *fence;
	struct sync_wait *wait;
	uint8_t reply[32];
	uint32_t id;
	unsigned slot;
	size_t index;
	int ended;

	/* The fence. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	fence = x11_resource_find(request->server, id, X11_RESOURCE_FENCE);
	if (fence == NULL) {
		request->bad = id;
		return X11_BAD_FENCE;
	}

	/* Reset, or asked. */
	if (request->bytes[1] == SYNC_RESET_FENCE) {
		fence->triggered = 0;
		return 0U;
	}
	if (request->bytes[1] == SYNC_QUERY_FENCE) {
		memset(reply, 0, sizeof(reply));
		reply[8] = (uint8_t)fence->triggered;
		x11_reply(request->server, request->client, reply, sizeof(reply));
		return 0U;
	}

	/* Triggered, or destroyed. */
	if (request->bytes[1] == SYNC_TRIGGER_FENCE) {
		fence->triggered = 1;
	} else {
		x11_resource_remove(request->server, id);
		x11_memory_release(request->server, fence->owner, sizeof(*fence));
		free(fence);
	}

	/* Succeeded: each wait for it ends. */
	state = request->server->sync;
	slot = 0U;
	while (slot < state->wait_count) {
		wait = state->waits[slot];
		ended = 0;
		for (index = 0U; wait->fences != NULL && index < wait->count; index++) {
			if (wait->fences[index] == id)
				ended = 1;
		}
		if (!ended) {
			slot++;
			continue;
		}
		state->waits[slot] = state->waits[state->wait_count - 1U];
		state->wait_count--;
		sync_wait_end(request->server, wait, 0);
	}
	return 0U;
}

/* AwaitFence: the client waits until one of the fences is triggered (at once when one is). */
static unsigned
sync_await_fence(
	struct x11_ext_request *request)
{
	struct sync_fence *fence;
	struct sync_wait *wait;
	uint32_t id;
	size_t count;
	size_t index;
	int triggered;
	int error;

	/* Whole fence ids. */
	if (request->length < 4U || (request->length - 4U) % 4U != 0U)
		return X11_BAD_LENGTH;
	count = (request->length - 4U) / 4U;
	if (count == 0U)
		return X11_BAD_VALUE;

	/* The wait, with its fences, each checked. */
	wait = calloc(1U, sizeof(*wait));
	if (wait == NULL)
		return X11_BAD_ALLOC;
	wait->fences = calloc(count, sizeof(*wait->fences));
	if (wait->fences == NULL) {
		free(wait);
		return X11_BAD_ALLOC;
	}
	wait->client = request->index;
	wait->count = count;
	triggered = 0;
	for (index = 0U; index < count; index++) {
		id = x11_read32(request->bytes + 4U + index * 4U, request->msb);
		fence = x11_resource_find(request->server, id, X11_RESOURCE_FENCE);
		if (fence == NULL) {
			sync_wait_free(wait);
			request->bad = id;
			return X11_BAD_FENCE;
		}
		if (fence->triggered)
			triggered = 1;
		wait->fences[index] = id;
	}

	/* One triggered already: no wait. */
	if (triggered) {
		sync_wait_free(wait);
		return 0U;
	}

	/* Succeeded: the client waits. */
	error = sync_wait_add(request->server->sync, wait);
	if (error != 0) {
		sync_wait_free(wait);
		return X11_BAD_ALLOC;
	}
	request->client->blocked = 1;
	return 0U;
}

/* Checks a new resource's id: the client's range, not in use.  Returns 0, or BadIDChoice. */
static unsigned
sync_new_id(
	struct x11_ext_request *request,
	uint32_t id)
{
	unsigned taken;
	int chosen;

	/* The client's own, and free. */
	chosen = x11_id_chosen(request->client, id);
	taken = x11_resource_type(request->server, id);
	if (!chosen || taken != 0U) {
		request->bad = id;
		return X11_BAD_ID_CHOICE;
	}

	/* Succeeded: free. */
	return 0U;
}

/*
 * Tells whether an alarm's request is its fixed part and the values its
 * mask names (the value and the delta are two 32-bit words each).
 */
static int
sync_alarm_fit(
	uint32_t mask,
	size_t length)
{
	size_t extra;
	int fit;

	/* The 64-bit values' second words. */
	extra = 0U;
	if ((mask & SYNC_CA_VALUE) != 0U)
		extra += 4U;
	if ((mask & SYNC_CA_DELTA) != 0U)
		extra += 4U;
	if (length < 12U + extra)
		return 0;

	/* Succeeded: whether the rest is one word for each bit. */
	fit = x11_values_fit(mask, 12U, length - extra);
	return fit;
}

/*
 * Makes an alarm active (inactive without a counter) and fires it at once
 * when its condition holds for the counter's value now (a transition does
 * not, as no change was seen).
 */
static void
sync_alarm_start(
	struct x11server *server,
	struct sync_alarm *alarm)
{
	struct sync_counter *counter;
	int64_t value;
	int found;

	/* No counter: inactive. */
	alarm->state = SYNC_ALARM_ACTIVE;
	if (alarm->counter == 0U) {
		alarm->state = SYNC_ALARM_INACTIVE;
		return;
	}

	/* Succeeded: checked against the counter as it is. */
	found = sync_counter_find(server, alarm->counter, &value, &counter);
	if (found)
		sync_alarm_check(server, alarm, value);
}

/*
 * Finds a counter: SERVERTIME (*counter NULL) or a client's.  Returns 1
 * with its value, 0 when no counter has the id.
 */
static int
sync_counter_find(
	struct x11server *server,
	uint32_t id,
	int64_t *value,
	struct sync_counter **counter)
{
	/* The server's own. */
	*counter = NULL;
	if (id == SYNC_SERVERTIME_XID && server->sync != NULL) {
		*value = server->sync->servertime;
		return 1;
	}

	/* A client's. */
	*counter = x11_resource_find(server, id, X11_RESOURCE_COUNTER);
	if (*counter == NULL)
		return 0;

	/* Succeeded: its value. */
	*value = (*counter)->value;
	return 1;
}

/*
 * Reads an alarm's values a mask names (in bit order) into it: the
 * counter (None or a counter), the value's kind and the value, the test,
 * the delta, and whether it tells the client asking.  A relative value is
 * added to the counter's value now.  Returns 0, or the error (the alarm
 * may be changed in part).
 */
static unsigned
sync_alarm_values(
	struct x11_ext_request *request,
	struct sync_alarm *alarm,
	uint32_t mask,
	const uint8_t *values)
{
	struct sync_counter *counter;
	int64_t value;
	uint32_t id;
	uint32_t word;
	unsigned slot;
	int found;
	int overflow;

	/* The counter. */
	if ((mask & SYNC_CA_COUNTER) != 0U) {
		id = x11_read32(values, request->msb);
		values += 4;
		if (id != 0U) {
			found = sync_counter_find(request->server, id, &value, &counter);
			if (!found) {
				request->bad = id;
				return X11_BAD_COUNTER;
			}
		}
		alarm->counter = id;
	}

	/* The value's kind. */
	if ((mask & SYNC_CA_VALUE_TYPE) != 0U) {
		word = x11_read32(values, request->msb);
		values += 4;
		if (word > SYNC_RELATIVE) {
			request->bad = word;
			return X11_BAD_VALUE;
		}
		alarm->value_type = word;
	}

	/* The value. */
	if ((mask & SYNC_CA_VALUE) != 0U) {
		alarm->wait_value = sync_read64(values, request->msb);
		values += 8;
	}

	/* The test. */
	if ((mask & SYNC_CA_TEST_TYPE) != 0U) {
		word = x11_read32(values, request->msb);
		values += 4;
		if (word > SYNC_NEGATIVE_COMPARISON) {
			request->bad = word;
			return X11_BAD_VALUE;
		}
		alarm->test_type = word;
	}

	/* The delta. */
	if ((mask & SYNC_CA_DELTA) != 0U) {
		alarm->delta = sync_read64(values, request->msb);
		values += 8;
	}

	/* Whether it tells the client asking: on or off the list. */
	if ((mask & SYNC_CA_EVENTS) != 0U) {
		word = x11_read32(values, request->msb);
		for (slot = 0U; slot < alarm->client_count; slot++) {
			if (alarm->clients[slot] == request->index)
				break;
		}
		if (word != 0U && slot == alarm->client_count && alarm->client_count < SYNC_ALARM_CLIENTS_MAX) {
			alarm->clients[alarm->client_count] = request->index;
			alarm->client_count++;
		} else if (word == 0U && slot < alarm->client_count) {
			alarm->clients[slot] = alarm->clients[alarm->client_count - 1U];
			alarm->client_count--;
		}
	}

	/* The value tested against, when the value or its kind changed. */
	if ((mask & (SYNC_CA_VALUE_TYPE | SYNC_CA_VALUE)) != 0U) {
		alarm->test_value = alarm->wait_value;
		if (alarm->value_type == SYNC_RELATIVE) {
			found = 0;
			if (alarm->counter != 0U)
				found = sync_counter_find(request->server, alarm->counter, &value, &counter);
			if (found) {
				overflow = sync_add(value, alarm->wait_value, &alarm->test_value);
				if (overflow)
					return X11_BAD_VALUE;
			}
		}
	}

	/* Succeeded: read. */
	return 0U;
}

/*
 * Tells whether a condition holds as a counter went from one value to
 * another: a transition holds when the counter crossed the test value in
 * its direction, a comparison when the counter is at or beyond it.
 */
static int
sync_test(
	unsigned test_type,
	int64_t test_value,
	int64_t old_value,
	int64_t new_value)
{
	/* Each test. */
	switch (test_type) {
	case SYNC_POSITIVE_TRANSITION:
		if (old_value < test_value && new_value >= test_value)
			return 1;
		break;
	case SYNC_NEGATIVE_TRANSITION:
		if (old_value > test_value && new_value <= test_value)
			return 1;
		break;
	case SYNC_POSITIVE_COMPARISON:
		if (new_value >= test_value)
			return 1;
		break;
	default:
		if (new_value <= test_value)
			return 1;
		break;
	}

	/* It does not hold. */
	return 0;
}

/* Adds two 64-bit values into *sum.  Returns nonzero when the sum leaves the range (*sum is then unchanged). */
static int
sync_add(
	int64_t a,
	int64_t b,
	int64_t *sum)
{
	/* Past the largest. */
	if (b > 0 && a > INT64_MAX - b)
		return 1;

	/* Past the smallest. */
	if (b < 0 && a < INT64_MIN - b)
		return 1;

	/* Succeeded: the sum. */
	*sum = a + b;
	return 0;
}

/* Reads a 64-bit value (its high 32 bits, signed, then its low) in a client's byte order. */
static int64_t
sync_read64(
	const uint8_t *bytes,
	int msb)
{
	uint64_t value;

	/* The two halves. */
	value = (uint64_t)x11_read32(bytes, msb) << 32;
	value |= (uint64_t)x11_read32(bytes + 4, msb);

	/* Succeeded: as a signed value. */
	return (int64_t)value;
}

/* Writes a 64-bit value as its high 32 bits then its low, in a client's byte order. */
static void
sync_write64(
	uint8_t *bytes,
	int64_t value,
	int msb)
{
	/* Succeeded: the two halves. */
	x11_write32(bytes, (uint32_t)((uint64_t)value >> 32), msb);
	x11_write32(bytes + 4, (uint32_t)((uint64_t)value & 0xffffffffU), msb);
}

/*
 * Checks what waits on a counter that changed: the waits, the alarms, and
 * the window manager's resizes.
 */
static void
sync_counter_changed(
	struct x11server *server,
	uint32_t counter,
	int64_t old_value,
	int64_t new_value)
{
	struct x11_sync *state;
	struct sync_alarm *alarm;
	struct sync_wait *wait;
	uint32_t *ids;
	size_t count;
	size_t id_index;
	unsigned slot;
	unsigned total;
	int error;

	/* Each wait (one that ends leaves the list, the last taking its place). */
	state = server->sync;
	slot = 0U;
	while (slot < state->wait_count) {
		wait = state->waits[slot];
		total = state->wait_count;
		sync_wait_check(server, wait, counter, old_value, new_value);
		if (state->wait_count < total)
			continue;
		slot++;
	}

	/* Each active alarm on it. */
	for (slot = 0U; slot < server->clients_max; slot++) {
		error = x11_resource_owned(server, slot, X11_RESOURCE_ALARM, &ids, &count);
		if (error != 0)
			continue;
		for (id_index = 0U; id_index < count; id_index++) {
			alarm = x11_resource_find(server, ids[id_index], X11_RESOURCE_ALARM);
			if (alarm == NULL || alarm->counter != counter)
				continue;
			sync_alarm_check(server, alarm, old_value);
		}
		free(ids);
	}

	/* Succeeded: the window manager hears of it. */
	x11_wm_sync_counter(server, counter, new_value);
}

/*
 * Tells what waits on a counter that was destroyed: a wait ends (its
 * client hears CounterNotify with the destroyed flag), an alarm has no
 * counter and is inactive (its clients told).
 */
static void
sync_counter_destroyed(
	struct x11server *server,
	uint32_t counter,
	int64_t value)
{
	struct x11_sync *state;
	struct sync_alarm *alarm;
	struct sync_wait *wait;
	uint32_t *ids;
	size_t count;
	size_t index;
	unsigned slot;
	int ended;
	int error;

	/* Each wait with a condition on it ends. */
	state = server->sync;
	slot = 0U;
	while (state != NULL && slot < state->wait_count) {
		wait = state->waits[slot];
		ended = 0;
		for (index = 0U; wait->conditions != NULL && index < wait->count; index++) {
			if (wait->conditions[index].counter == counter) {
				wait->conditions[index].destroyed = 1;
				ended = 1;
			}
		}
		if (!ended) {
			slot++;
			continue;
		}
		state->waits[slot] = state->waits[state->wait_count - 1U];
		state->wait_count--;
		sync_wait_end(server, wait, 1);
	}

	/* Succeeded: each alarm on it is inactive and told, the newest first (as the X.Org server tells them). */
	for (slot = 0U; slot < server->clients_max; slot++) {
		error = x11_resource_owned(server, slot, X11_RESOURCE_ALARM, &ids, &count);
		if (error != 0)
			continue;
		if (count > 1U)
			qsort(ids, count, sizeof(*ids), sync_compare_down);
		for (index = 0U; index < count; index++) {
			alarm = x11_resource_find(server, ids[index], X11_RESOURCE_ALARM);
			if (alarm == NULL || alarm->counter != counter)
				continue;
			alarm->state = SYNC_ALARM_INACTIVE;
			alarm->counter = 0U;
			sync_alarm_notify(server, alarm, alarm->test_value, value);
		}
		free(ids);
	}
}

/* Fires an active alarm whose condition holds as its counter went from a value to the one it has. */
static void
sync_alarm_check(
	struct x11server *server,
	struct sync_alarm *alarm,
	int64_t old_value)
{
	struct sync_counter *counter;
	int64_t value;
	int found;
	int holds;

	/* Only an active alarm with a counter. */
	if (alarm->state != SYNC_ALARM_ACTIVE || alarm->counter == 0U)
		return;
	found = sync_counter_find(server, alarm->counter, &value, &counter);
	if (!found)
		return;

	/* Succeeded: fired when its condition holds. */
	holds = sync_test(alarm->test_type, alarm->test_value, old_value, value);
	if (holds)
		sync_alarm_fire(server, alarm);
}

/*
 * Fires an alarm as the X.Org server does: a comparison with no delta
 * makes it inactive; otherwise its test value moves on by the delta until
 * the condition no longer holds (inactive when that leaves the 64-bit
 * range).  Its clients hear AlarmNotify with the value it fired at and its
 * new state.
 */
static void
sync_alarm_fire(
	struct x11server *server,
	struct sync_alarm *alarm)
{
	struct sync_counter *counter;
	int64_t value;
	int64_t fired_at;
	int64_t next;
	int found;
	int overflow;
	int holds;

	/* The counter's value now. */
	found = sync_counter_find(server, alarm->counter, &value, &counter);
	if (!found)
		return;

	/* A comparison that never stops holding by itself ends the alarm. */
	fired_at = alarm->test_value;
	if (alarm->delta == 0 && (alarm->test_type == SYNC_POSITIVE_COMPARISON || alarm->test_type == SYNC_NEGATIVE_COMPARISON))
		alarm->state = SYNC_ALARM_INACTIVE;

	/* The test value moves on until the condition no longer holds. */
	next = fired_at;
	if (alarm->state == SYNC_ALARM_ACTIVE) {
		do {
			overflow = sync_add(next, alarm->delta, &next);
			if (overflow)
				break;
			holds = sync_test(alarm->test_type, next, value, value);
		} while (holds);
		if (overflow) {
			next = fired_at;
			alarm->state = SYNC_ALARM_INACTIVE;
		}
	}

	/* Succeeded: its clients told, and the new test value kept. */
	sync_alarm_notify(server, alarm, fired_at, value);
	alarm->test_value = next;
}

/*
 * Sends AlarmNotify to an alarm's clients: the alarm, its counter's value,
 * the value it fired at, the time and its state.
 */
static void
sync_alarm_notify(
	struct x11server *server,
	struct sync_alarm *alarm,
	int64_t alarm_value,
	int64_t value)
{
	struct x11_client *client;
	uint8_t event[32];
	unsigned slot;
	int msb;

	/* Each client it tells, in its byte order. */
	for (slot = 0U; slot < alarm->client_count; slot++) {
		client = x11_client_of(server, alarm->clients[slot]);
		if (client == NULL)
			continue;
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = (uint8_t)(X11_SYNC_EVENT + SYNC_ALARM_NOTIFY);
		event[1] = SYNC_ALARM_NOTIFY;
		x11_write32(event + 4, alarm->id, msb);
		sync_write64(event + 8, value, msb);
		sync_write64(event + 16, alarm_value, msb);
		x11_write32(event + 24, server->now, msb);
		event[28] = (uint8_t)alarm->state;
		x11_extension_event(server, client, event);
	}
}

/* Frees an alarm whose id is gone: its clients hear it is destroyed. */
static void
sync_alarm_free(
	struct x11server *server,
	struct sync_alarm *alarm)
{
	struct sync_counter *counter;
	int64_t value;
	int found;

	/* Its clients told, with its counter's value (0 without one). */
	alarm->state = SYNC_ALARM_DESTROYED;
	value = 0;
	if (alarm->counter != 0U) {
		found = sync_counter_find(server, alarm->counter, &value, &counter);
		if (!found)
			value = 0;
	}
	sync_alarm_notify(server, alarm, alarm->test_value, value);

	/* Succeeded: its charge and itself. */
	x11_memory_release(server, alarm->owner, sizeof(*alarm));
	free(alarm);
}

/* Keeps a wait.  Returns 0, or ENOMEM. */
static int
sync_wait_add(
	struct x11_sync *state,
	struct sync_wait *wait)
{
	struct sync_wait **grown;
	unsigned capacity;

	/* Room, grown when full. */
	if (state->wait_count == state->wait_capacity) {
		capacity = state->wait_capacity * 2U;
		if (capacity == 0U)
			capacity = 8U;
		grown = realloc(state->waits, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		state->waits = grown;
		state->wait_capacity = capacity;
	}

	/* Succeeded: kept. */
	state->waits[state->wait_count] = wait;
	state->wait_count++;
	return 0;
}

/*
 * Ends a wait (taking it off the list) when one of its conditions on a
 * counter holds as the counter went from a value to another.
 */
static void
sync_wait_check(
	struct x11server *server,
	struct sync_wait *wait,
	uint32_t counter,
	int64_t old_value,
	int64_t new_value)
{
	struct x11_sync *state;
	size_t index;
	unsigned slot;
	int holds;

	/* A wait for fences is not about counters. */
	if (wait->conditions == NULL)
		return;

	/* A condition on the counter that holds. */
	holds = 0;
	for (index = 0U; index < wait->count; index++) {
		if (wait->conditions[index].counter != counter)
			continue;
		holds = sync_test(wait->conditions[index].test_type, wait->conditions[index].test_value, old_value, new_value);
		if (holds)
			break;
	}
	if (!holds)
		return;

	/* Off the list. */
	state = server->sync;
	for (slot = 0U; slot < state->wait_count; slot++) {
		if (state->waits[slot] == wait)
			break;
	}
	if (slot < state->wait_count) {
		state->waits[slot] = state->waits[state->wait_count - 1U];
		state->wait_count--;
	}

	/* Succeeded: ended, with its events. */
	sync_wait_end(server, wait, 1);
}

/*
 * Ends a wait that is off the list: with notify, its client hears
 * CounterNotify for each condition whose counter went or whose difference
 * from its test value reached the threshold (at least it for a positive
 * test, at most for a negative one); the client goes on, and the wait is
 * freed.
 */
static void
sync_wait_end(
	struct x11server *server,
	struct sync_wait *wait,
	int notify)
{
	struct sync_condition *condition;
	struct sync_counter *counter;
	struct x11_client *client;
	uint8_t event[32];
	unsigned char *chosen;
	int64_t value;
	int64_t difference;
	size_t index;
	size_t count;
	size_t sent;
	int found;
	int overflow;
	int positive;
	int msb;

	/* The client, while it is still there. */
	client = x11_client_of(server, wait->client);
	if (client == NULL || wait->conditions == NULL)
		notify = 0;

	/* The conditions that send an event. */
	chosen = NULL;
	count = 0U;
	if (notify)
		chosen = calloc(wait->count, 1U);
	for (index = 0U; chosen != NULL && index < wait->count; index++) {
		condition = &wait->conditions[index];
		if (condition->destroyed) {
			chosen[index] = 1U;
			count++;
			continue;
		}
		found = sync_counter_find(server, condition->counter, &value, &counter);
		if (!found)
			continue;
		if (condition->test_value == INT64_MIN)
			continue;
		overflow = sync_add(value, -condition->test_value, &difference);
		if (overflow)
			continue;
		positive = 0;
		if (condition->test_type == SYNC_POSITIVE_TRANSITION || condition->test_type == SYNC_POSITIVE_COMPARISON)
			positive = 1;
		if (positive && difference >= condition->threshold) {
			chosen[index] = 1U;
			count++;
		} else if (!positive && difference <= condition->threshold) {
			chosen[index] = 1U;
			count++;
		}
	}

	/* Each event, with how many follow it. */
	sent = 0U;
	for (index = 0U; chosen != NULL && index < wait->count; index++) {
		if (!chosen[index])
			continue;
		condition = &wait->conditions[index];
		value = 0;
		if (!condition->destroyed)
			(void)sync_counter_find(server, condition->counter, &value, &counter);
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = (uint8_t)(X11_SYNC_EVENT + SYNC_COUNTER_NOTIFY);
		event[1] = SYNC_COUNTER_NOTIFY;
		x11_write32(event + 4, condition->counter, msb);
		sync_write64(event + 8, condition->test_value, msb);
		sync_write64(event + 16, value, msb);
		x11_write32(event + 24, server->now, msb);
		sent++;
		x11_write16(event + 28, (uint16_t)(count - sent), msb);
		event[30] = (uint8_t)condition->destroyed;
		x11_extension_event(server, client, event);
	}
	free(chosen);

	/* Succeeded: the client goes on, and the wait is freed. */
	if (client != NULL) {
		client->blocked = 0;
		server->sync->woke = 1;
	}
	sync_wait_free(wait);
}

/* Frees a wait. */
static void
sync_wait_free(
	struct sync_wait *wait)
{
	/* Succeeded: its lists and itself. */
	free(wait->conditions);
	free(wait->fences);
	free(wait);
}

/* Brings SERVERTIME up to the server's time, checking what waits on it. */
static void
sync_servertime(
	struct x11server *server)
{
	struct x11_sync *state;
	int64_t old_value;
	uint32_t elapsed;

	/* The time since it was last brought up. */
	state = server->sync;
	elapsed = server->now - state->servertime_at;
	if (elapsed == 0U || elapsed > 0x7fffffffU)
		return;

	/* Succeeded: moved on, and what waits on it checked. */
	old_value = state->servertime;
	state->servertime += (int64_t)elapsed;
	state->servertime_at = server->now;
	sync_counter_changed(server, SYNC_SERVERTIME_XID, old_value, state->servertime);
}

/* Frees a closed client's resources of a kind (counters, alarms or fences). */
static void
sync_release(
	struct x11server *server,
	unsigned owner,
	unsigned type)
{
	struct sync_counter *counter;
	struct sync_fence *fence;
	void *object;
	int64_t value;
	uint32_t *ids;
	size_t count;
	size_t index;
	int error;

	/* The client's resources of the kind, listed. */
	error = x11_resource_owned(server, owner, type, &ids, &count);
	if (error != 0)
		return;

	/* Each one, and its charge; a counter's waiters and alarms hear it went. */
	for (index = 0U; index < count; index++) {
		object = x11_resource_find(server, ids[index], type);
		x11_resource_remove(server, ids[index]);
		if (type == X11_RESOURCE_COUNTER) {
			counter = object;
			value = counter->value;
			x11_memory_release(server, counter->owner, sizeof(*counter));
			free(counter);
			sync_counter_destroyed(server, ids[index], value);
		} else if (type == X11_RESOURCE_ALARM) {
			sync_alarm_free(server, object);
		} else {
			fence = object;
			x11_memory_release(server, fence->owner, sizeof(*fence));
			free(fence);
		}
	}

	/* Succeeded: none is left. */
	free(ids);
}

/* Orders ids from the largest down (the newest a client made first). */
static int
sync_compare_down(
	const void *a,
	const void *b)
{
	uint32_t first;
	uint32_t second;

	/* The two ids. */
	first = *(const uint32_t *)a;
	second = *(const uint32_t *)b;

	/* Succeeded: the larger first. */
	if (first > second)
		return -1;
	if (first < second)
		return 1;
	return 0;
}
