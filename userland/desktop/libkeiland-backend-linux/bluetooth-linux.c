/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bluetooth on Linux (ws143-p007, plan/ws143/phase007/phase.md section 9):
 * BlueZ's org.bluez on the system bus, behind the backend's Bluetooth
 * calls (keiland-backend.h).
 *
 * One connection of its own, opened and authenticated before anything
 * else and never waited on after that: every call goes out with
 * dbus_send_call() and its answer comes back through the dispatch, where
 * the table of calls sent says what it answers.  The signals of org.bluez
 * (objects added and removed, properties changed) and the bus's word on
 * who owns org.bluez keep the adapter and the devices current; the
 * adapter's properties are read again every two seconds while someone
 * watches, otherwise every thirty.  The connection exports the agent
 * that answers BlueZ's pairing questions (DisplayYesNo), asked of the
 * user through the backend's questions.
 *
 * A lost connection is opened again after a wait that doubles up to
 * thirty seconds; a lost or replaced org.bluez ends the request and the
 * question outstanding and empties the tables.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend-linux/dbus-linux.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* The system bus's socket (a host test builds with a private bus's). */
#ifndef BT_DBUS_SOCKET
#define BT_DBUS_SOCKET		"/run/dbus/system_bus_socket"
#endif

/* The waits: the first and the longest before opening again, how long a connection lasts before its wait resets. */
#define BT_RETRY_MS		1000U
#define BT_RETRY_MAX_MS		30000U
#define BT_STABLE_MS		60000U

/* The readings of the adapter while watched and not, and the agent's next try after a failure. */
#define BT_WATCH_MS		2000U
#define BT_IDLE_MS		30000U
#define BT_AGENT_RETRY_MS	30000U

/* How long a call is waited for, a pairing's call, an agent's question held, and a scan's next try. */
#define BT_CALL_MS		25000U
#define BT_PAIR_MS		90000U
#define BT_PENDING_MS		30000U
#define BT_DISCOVERY_RETRY_MS	5000U

/* How often a change of signal strength alone is told. */
#define BT_RSSI_MS		2000U

/* The devices kept inside, the calls outstanding, the readings among them, an object path's and a bus name's lengths. */
#define BT_TABLE_MAX		128U
#define BT_CALLS_MAX		16U
#define BT_READS_MAX		4U
#define BT_PATH_MAX		96U
#define BT_NAME_MAX		128U

/* The answers and the questions not taken yet. */
#define BT_RESULTS_MAX		8U
#define BT_QUESTIONS_MAX	4U

/* BlueZ's names. */
#define BT_SERVICE		"org.bluez"
#define BT_ROOT_PATH		"/org/bluez"
#define BT_ADAPTER		"org.bluez.Adapter1"
#define BT_DEVICE		"org.bluez.Device1"
#define BT_BATTERY		"org.bluez.Battery1"
#define BT_AGENT		"org.bluez.Agent1"
#define BT_AGENT_MANAGER	"org.bluez.AgentManager1"
#define BT_PROPERTIES		"org.freedesktop.DBus.Properties"
#define BT_INTROSPECTABLE	"org.freedesktop.DBus.Introspectable"
#define BT_OBJECT_MANAGER	"org.freedesktop.DBus.ObjectManager"
#define BT_BUS			"org.freedesktop.DBus"
#define BT_BUS_PATH		"/org/freedesktop/DBus"

/* The agent this connection exports, and its capability: it shows numbers and says yes or no, it types nothing. */
#define BT_AGENT_PATH		"/org/keiland/bluetooth/agent"
#define BT_CAPABILITY		"DisplayYesNo"

/* The services a paired device that is not trusted may be allowed to: HID, and HID over GATT. */
#define BT_HID_UUID		"00001124-0000-1000-8000-00805f9b34fb"
#define BT_HOGP_UUID		"00001812-0000-1000-8000-00805f9b34fb"

/* What a call sent asks, so that its answer goes to the right place. */
enum bt_call_kind {
	BT_CALL_IGNORED,
	BT_CALL_OWNER,
	BT_CALL_REGISTER,
	BT_CALL_DEFAULT,
	BT_CALL_ROOT,
	BT_CALL_NODES,
	BT_CALL_ADAPTER,
	BT_CALL_DEVICE,
	BT_CALL_BATTERY,
	BT_CALL_REQUEST,
	BT_CALL_DISCOVERY
};

/* The steps of a request: a single call, or a pairing's three. */
enum bt_step {
	BT_STEP_NONE,
	BT_STEP_SINGLE,
	BT_STEP_PAIR,
	BT_STEP_TRUST,
	BT_STEP_CONNECT
};

/* Where the agent's registration stands. */
enum bt_agent_state {
	BT_AGENT_NONE,
	BT_AGENT_REGISTERING,
	BT_AGENT_DEFAULTING,
	BT_AGENT_HELD
};

/*
 * One call sent and not answered yet: its serial, what it asks, the
 * object it concerns (a device's path for its reading), and when it is
 * given up.
 */
struct bt_call {
	uint32_t serial;
	unsigned kind;
	char path[BT_PATH_MAX];
	uint64_t deadline_ms;
};

/*
 * One value of a property, as far as it is read here: an unsigned or a
 * Boolean (number), a signed integer (integer), or a string or an object
 * path (string, inside the message being read).  kind is the variant's
 * type character, 0 for a type not read.
 */
struct bt_value {
	char kind;
	uint32_t number;
	int32_t integer;
	const char *string;
};

/*
 * One device BlueZ knows under the adapter: its object path (its identity
 * here), what its properties said, and whether its properties and its
 * battery still have to be read.
 */
struct bt_device {
	char path[BT_PATH_MAX];
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	char alias[KL_BACKEND_BT_NAME_MAX];
	unsigned random;
	unsigned has_name;
	unsigned has_class;
	unsigned has_appearance;
	unsigned kind;
	unsigned paired;
	unsigned bonded;
	unsigned trusted;
	unsigned legacy;
	unsigned connected;
	int battery;
	int rssi;
	unsigned need_read;
	unsigned need_battery;
};

/* One answer to a request: its number, its errno value and the reason in words. */
struct bt_result {
	uint32_t id;
	int error;
	char reason[KL_BACKEND_BT_REASON_MAX];
};

/*
 * The following of BlueZ: the connection and its waits, who owns
 * org.bluez, the calls sent, the adapter and the devices, the agent and
 * the question it holds, the request going on, the scan, and what was
 * last told to the caller with the answers and questions not taken yet.
 */
struct kl_backend_bluetooth {
	/* The connection: open or not, when to try again, the wait after the next failure, and when it was opened. */
	struct linux_dbus bus;
	unsigned open;
	uint64_t retry_ms;
	uint64_t retry_wait_ms;
	uint64_t opened_ms;

	/* The unique name owning org.bluez ("" for none), and the calls sent to anyone. */
	char owner[BT_NAME_MAX];
	struct bt_call calls[BT_CALLS_MAX];
	size_t call_count;

	/* The adapter: whether one is chosen, its path and properties, and what is still to be read of it. */
	unsigned adapter;
	char adapter_path[BT_PATH_MAX];
	char adapter_address[KL_BACKEND_BT_ADDRESS_MAX];
	char adapter_alias[KL_BACKEND_BT_NAME_MAX];
	unsigned powered;
	unsigned power_state;
	unsigned discovering;
	unsigned need_root;
	unsigned need_nodes;
	uint64_t adapter_due_ms;

	/* The devices under the adapter. */
	struct bt_device table[BT_TABLE_MAX];
	size_t table_count;

	/* The agent: its registration and when to try again. */
	unsigned agent_state;
	uint64_t agent_due_ms;

	/*
	 * The question asked now (0: none), its kind and device, and the
	 * agent's call held for its answer: the caller's name and serial,
	 * and when it is given up.  input_rejected says the agent refused to
	 * type a code during the request's pairing.
	 */
	uint32_t question_id;
	unsigned question_kind;
	char question_path[BT_PATH_MAX];
	unsigned pending;
	char pending_sender[BT_NAME_MAX];
	uint32_t pending_serial;
	uint64_t pending_ms;
	unsigned input_rejected;

	/* Whether someone watches, whether devices are looked for, and the scan asked of BlueZ. */
	unsigned watching;
	unsigned scanning;
	unsigned discovery_sent;
	uint64_t discovery_due_ms;

	/* The request going on: its kind (0: none), number, step, device, and what its pairing's later steps report. */
	unsigned request_kind;
	uint32_t request_id;
	unsigned request_step;
	char request_path[BT_PATH_MAX];
	char request_reason[KL_BACKEND_BT_REASON_MAX];
	uint32_t next_id;
	uint32_t next_question;

	/* What was told last, and when a change of signal strength alone may be told again. */
	struct kl_backend_bluetooth_state state;
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	size_t count;
	uint64_t rssi_due_ms;
	unsigned changed;

	/* The answers and the questions not taken yet. */
	struct bt_result results[BT_RESULTS_MAX];
	size_t result_count;
	struct kl_backend_bluetooth_question questions[BT_QUESTIONS_MAX];
	size_t question_count;
};

/* What a property callback is given: the record, the device (NULL for the adapter), the name and the value. */
typedef void (*bt_property_fn)(struct kl_backend_bluetooth *bluetooth, struct bt_device *device, const char *name, const struct bt_value *value);

static uint64_t bt_milliseconds(void);
static void bt_open(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static void bt_lost(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static void bt_forget_service(struct kl_backend_bluetooth *bluetooth);
static void bt_owner_arrived(struct kl_backend_bluetooth *bluetooth, const char *owner);
static int bt_send(struct kl_backend_bluetooth *bluetooth, unsigned kind, const char *record, const char *destination, const char *path, const char *interface, const char *member, const char *signature, const struct dbus_arg *args, size_t count, uint64_t wait_ms);
static int bt_message(struct dbus_reply *message, void *data);
static void bt_answer(struct kl_backend_bluetooth *bluetooth, const struct bt_call *call, struct dbus_reply *reply);
static void bt_expire(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static void bt_work(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static size_t bt_outstanding(const struct kl_backend_bluetooth *bluetooth, unsigned kind);
static void bt_signal(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *signal);
static void bt_owner_changed(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *signal);
static void bt_interfaces_added(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *signal);
static void bt_interfaces_removed(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *signal);
static void bt_properties_changed(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *signal);
static int bt_read_properties(struct kl_backend_bluetooth *bluetooth, struct bt_device *device, struct dbus_reply *reply, bt_property_fn apply);
static int bt_read_value(struct dbus_reply *reply, const char *signature, struct bt_value *value);
static void bt_adapter_property(struct kl_backend_bluetooth *bluetooth, struct bt_device *device, const char *name, const struct bt_value *value);
static void bt_device_property(struct kl_backend_bluetooth *bluetooth, struct bt_device *device, const char *name, const struct bt_value *value);
static void bt_battery_property(struct kl_backend_bluetooth *bluetooth, struct bt_device *device, const char *name, const struct bt_value *value);
static void bt_choose_adapter(struct kl_backend_bluetooth *bluetooth, const char *path);
static void bt_drop_adapter(struct kl_backend_bluetooth *bluetooth);
static void bt_introspect_answer(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *reply, unsigned root);
static int bt_is_device_path(const struct kl_backend_bluetooth *bluetooth, const char *path);
static int bt_is_adapter_path(const char *path);
static struct bt_device *bt_find_path(struct kl_backend_bluetooth *bluetooth, const char *path);
static struct bt_device *bt_find_address(struct kl_backend_bluetooth *bluetooth, const char *address);
static struct bt_device *bt_add_device(struct kl_backend_bluetooth *bluetooth, const char *path);
static void bt_remove_device(struct kl_backend_bluetooth *bluetooth, const char *path);
static void bt_request_answer(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *reply, const char *error_name, const char *error_text);
static void bt_request_finish(struct kl_backend_bluetooth *bluetooth, int error, const char *reason);
static void bt_request_give_up(struct kl_backend_bluetooth *bluetooth, int error, const char *reason);
static void bt_pair_question_end(struct kl_backend_bluetooth *bluetooth);
static int bt_request_send(struct kl_backend_bluetooth *bluetooth, unsigned step);
static int bt_errno(const char *error_name, const char *error_text, unsigned request, unsigned step);
static int bt_says_timeout(const char *text);
static int bt_error_is_unknown(const char *error_name);
static void bt_reason(const char *error_name, const char *error_text, char *reason, size_t size);
static void bt_agent_call(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *call);
static void bt_agent_hold(struct kl_backend_bluetooth *bluetooth, struct dbus_reply *call, unsigned kind, uint32_t number, const char *path);
static void bt_agent_release_pending(struct kl_backend_bluetooth *bluetooth, const char *error_name);
static void bt_question_push(struct kl_backend_bluetooth *bluetooth, unsigned kind, uint32_t number, const char *path);
static void bt_question_end(struct kl_backend_bluetooth *bluetooth);
static void bt_export(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static int bt_select_order(const void *left, const void *right);
static int bt_show_order(const void *left, const void *right);
static unsigned bt_kind(const char *icon);
static unsigned bt_power_state(const char *power_state);
static unsigned bt_flag(const struct bt_value *value);
static unsigned bt_paired(const struct bt_device *device);
static void bt_copy_text(char *destination, size_t size, const char *source);
static int bt_six_digits(const char *text, uint32_t *number);

/*
 * Starts following BlueZ.  Returns NULL only without memory.
 */
struct kl_backend_bluetooth *
kl_backend_bluetooth_open(
	void)
{
	struct kl_backend_bluetooth *bluetooth;

	/* The record, with no connection. */
	bluetooth = calloc(1, sizeof(*bluetooth));
	if (bluetooth == NULL)
		return NULL;
	bluetooth->bus.fd = -1;

	/* Unreachable until org.bluez is found; the first update opens the connection. */
	bluetooth->state.state = KL_BACKEND_BT_ABSENT;
	bluetooth->retry_wait_ms = BT_RETRY_MS;
	bluetooth->next_id = 1U;
	bluetooth->next_question = 1U;

	/* Succeeded: the first update connects. */
	return bluetooth;
}

/*
 * Stops following BlueZ: refuses the agent's question held, gives up this
 * program's pairing, stops its scan and withdraws its agent, without
 * waiting for any answer, then closes the connection.
 */
void
kl_backend_bluetooth_close(
	struct kl_backend_bluetooth *bluetooth)
{
	struct dbus_arg argument;
	uint32_t serial;

	/* Nothing followed. */
	if (bluetooth == NULL)
		return;

	/* What this program asked of BlueZ is taken back while the connection lasts. */
	if (bluetooth->open && bluetooth->owner[0] != '\0') {
		bt_agent_release_pending(bluetooth, "org.bluez.Error.Rejected");

		/* A pairing of this program's is given up. */
		if (bluetooth->request_step == BT_STEP_PAIR) {
			(void)dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, BT_SERVICE, bluetooth->request_path, BT_DEVICE, "CancelPairing", "", NULL, 0, &serial);
		}

		/* The scan this program asked for stops. */
		if (bluetooth->discovery_sent && bluetooth->adapter) {
			(void)dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, BT_SERVICE, bluetooth->adapter_path, BT_ADAPTER, "StopDiscovery", "", NULL, 0, &serial);
		}

		/* The agent is withdrawn (the connection's end withdraws it too). */
		memset(&argument, 0, sizeof(argument));
		argument.string = BT_AGENT_PATH;
		(void)dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, BT_SERVICE, BT_ROOT_PATH, BT_AGENT_MANAGER, "UnregisterAgent", "o", &argument, 1, &serial);
	}

	/* The connection, then the record. */
	dbus_close(&bluetooth->bus);
	free(bluetooth);
}

/*
 * Reads what has arrived without waiting, sends what is due, and says in
 * *changed (KL_BACKEND_BT_CHANGED_*) what changed.  Returns 0 or EINVAL.
 */
int
kl_backend_bluetooth_update(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	uint64_t now;
	int fd;

	/* A record and somewhere to say what changed. */
	if (bluetooth == NULL || changed == NULL)
		return EINVAL;
	*changed = 0U;
	now = bt_milliseconds();

	/* A connection that closed since the last update is lost. */
	fd = dbus_fd(&bluetooth->bus);
	if (bluetooth->open && fd < 0)
		bt_lost(bluetooth, now);

	/* A closed connection is opened when its wait is over. */
	if (!bluetooth->open && now >= bluetooth->retry_ms)
		bt_open(bluetooth, now);

	/* What has arrived on an open one: answers, signals and the agent's calls. */
	if (bluetooth->open) {
		(void)dbus_dispatch_messages(&bluetooth->bus, bt_message, bluetooth);

		/* A connection the dispatch closed is lost now. */
		fd = dbus_fd(&bluetooth->bus);
		if (fd < 0)
			bt_lost(bluetooth, now);
	}

	/* A connection that has lasted earns the shortest wait again. */
	if (bluetooth->open && now >= bluetooth->opened_ms + BT_STABLE_MS)
		bluetooth->retry_wait_ms = BT_RETRY_MS;

	/* Calls not answered in time, and what is due to be sent. */
	if (bluetooth->open) {
		bt_expire(bluetooth, now);
		bt_work(bluetooth, now);
	}

	/* What the caller sees, and what changed in it. */
	bt_export(bluetooth, now);

	/* Answers and questions not taken yet are told again. */
	if (bluetooth->result_count > 0U)
		bluetooth->changed |= KL_BACKEND_BT_CHANGED_RESULT;
	if (bluetooth->question_count > 0U)
		bluetooth->changed |= KL_BACKEND_BT_CHANGED_QUESTION;

	/* Succeeded: what changed since the last update. */
	*changed = bluetooth->changed;
	bluetooth->changed = 0U;
	return 0;
}

/*
 * Copies the state as last told.
 */
void
kl_backend_bluetooth_get_state(
	const struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_state *state)
{
	/* Nothing followed: unreachable. */
	memset(state, 0, sizeof(*state));
	if (bluetooth == NULL)
		return;

	/* Succeeded: the copy. */
	*state = bluetooth->state;
}

/*
 * Copies up to capacity devices (the paired first) and returns how many
 * there are.
 */
size_t
kl_backend_bluetooth_get_devices(
	const struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_device *devices,
	size_t capacity)
{
	size_t count;

	/* Nothing followed, or nowhere to copy. */
	if (bluetooth == NULL || devices == NULL)
		return 0U;

	/* As many as fit. */
	count = bluetooth->count;
	if (count > capacity)
		count = capacity;
	memcpy(devices, bluetooth->devices, count * sizeof(devices[0]));

	/* Succeeded: how many there are. */
	return bluetooth->count;
}

/*
 * Reads the adapter often while on is 1 (someone shows it).
 */
void
kl_backend_bluetooth_set_watching(
	struct kl_backend_bluetooth *bluetooth,
	unsigned on)
{
	/* Nothing followed. */
	if (bluetooth == NULL)
		return;

	/* A new watch reads now. */
	if (on && !bluetooth->watching)
		bluetooth->adapter_due_ms = 0U;

	/* The wish, as 0 or 1. */
	bluetooth->watching = 0U;
	if (on)
		bluetooth->watching = 1U;
}

/*
 * Looks for devices while on is 1: BlueZ's discovery is started for this
 * program while the adapter is on, and stopped when the wish ends.
 */
void
kl_backend_bluetooth_set_scanning(
	struct kl_backend_bluetooth *bluetooth,
	unsigned on)
{
	/* Nothing followed. */
	if (bluetooth == NULL)
		return;

	/* A new wish scans at once. */
	if (on && !bluetooth->scanning)
		bluetooth->discovery_due_ms = 0U;

	/* The wish, as 0 or 1; the update sends what it needs. */
	bluetooth->scanning = 0U;
	if (on)
		bluetooth->scanning = 1U;
}

/*
 * Sends a request (KL_BACKEND_BT_*; the power's take a NULL address) and
 * numbers its answer in *id.  Returns 0, EBUSY while one is outstanding
 * (or no call can be sent now), ENOTCONN without BlueZ, ENOTSUP for a
 * phone, or EINVAL for an unknown device or request.
 */
int
kl_backend_bluetooth_request(
	struct kl_backend_bluetooth *bluetooth,
	unsigned request,
	const char *address,
	unsigned type,
	uint32_t *id)
{
	struct bt_device *device;
	unsigned step;
	int error;

	/* A record, BlueZ, and no request going on. */
	if (bluetooth == NULL || id == NULL)
		return EINVAL;
	if (!bluetooth->state.reachable)
		return ENOTCONN;
	if (bluetooth->request_kind != 0U)
		return EBUSY;

	/* A phone of the user's is zedBSD's alone (ws197). */
	if (request == KL_BACKEND_BT_PAIR_PHONE)
		return ENOTSUP;

	/* The power is the adapter's. */
	step = BT_STEP_SINGLE;
	bluetooth->request_path[0] = '\0';
	if (request == KL_BACKEND_BT_POWER_ON || request == KL_BACKEND_BT_POWER_OFF) {
		if (!bluetooth->adapter)
			return EINVAL;
	} else {
		/* A device's request names a device in the table. */
		if (request < KL_BACKEND_BT_PAIR || request > KL_BACKEND_BT_DISCONNECT)
			return EINVAL;
		if (address == NULL || type > KL_BACKEND_BT_LE_RANDOM)
			return EINVAL;
		device = bt_find_address(bluetooth, address);
		if (device == NULL)
			return EINVAL;
		bt_copy_text(bluetooth->request_path, sizeof(bluetooth->request_path), device->path);

		/* A pairing starts with Pair. */
		if (request == KL_BACKEND_BT_PAIR)
			step = BT_STEP_PAIR;
	}

	/* The request's first call. */
	bluetooth->request_kind = request;
	bluetooth->request_reason[0] = '\0';
	bluetooth->input_rejected = 0U;
	error = bt_request_send(bluetooth, step);
	if (error != 0) {
		bluetooth->request_kind = 0U;
		bluetooth->request_step = BT_STEP_NONE;
		return error;
	}

	/* Succeeded: numbered, its answer to come. */
	bluetooth->request_id = bluetooth->next_id;
	bluetooth->next_id++;
	if (bluetooth->next_id == 0U)
		bluetooth->next_id = 1U;
	*id = bluetooth->request_id;
	return 0;
}

/*
 * Takes the oldest answer.  Returns 1 with one, 0 when none waits.
 */
int
kl_backend_bluetooth_take_result(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t *id,
	int *error,
	char *reason,
	size_t size)
{
	/* None waiting. */
	if (bluetooth == NULL || bluetooth->result_count == 0U)
		return 0;

	/* The oldest. */
	*id = bluetooth->results[0].id;
	*error = bluetooth->results[0].error;
	if (reason != NULL && size > 0U)
		bt_copy_text(reason, size, bluetooth->results[0].reason);

	/* The others move up. */
	bluetooth->result_count--;
	memmove(&bluetooth->results[0], &bluetooth->results[1], bluetooth->result_count * sizeof(bluetooth->results[0]));

	/* Succeeded: one answer. */
	return 1;
}

/*
 * Takes the oldest question.  Returns 1 with one, 0 when none waits.
 */
int
kl_backend_bluetooth_take_question(
	struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_question *question)
{
	/* None waiting. */
	if (bluetooth == NULL || question == NULL || bluetooth->question_count == 0U)
		return 0;

	/* The oldest, then the others move up. */
	*question = bluetooth->questions[0];
	bluetooth->question_count--;
	memmove(&bluetooth->questions[0], &bluetooth->questions[1], bluetooth->question_count * sizeof(bluetooth->questions[0]));

	/* Succeeded: one question. */
	return 1;
}

/*
 * Answers the question of an id (CONFIRM and CONSENT): 1 yes, 0 no; the
 * agent's call held for it gets its return or BlueZ's Rejected.  No to
 * this program's own pairing also ends that pairing at once, as refused
 * (ECONNREFUSED, "rejected", as zedBSD's bluetoothd answers it), and
 * BlueZ is asked to stop it: BlueZ would otherwise wait for the device's
 * side to end it (T1-604).  Returns 0, ENOENT when that question is not
 * the one asked now, or the error of sending.
 */
int
kl_backend_bluetooth_answer(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t id,
	unsigned yes)
{
	unsigned own;
	int error;
	int same;

	/* The question asked now, of that id, with a call waiting for it. */
	if (bluetooth == NULL)
		return ENOENT;
	if (!bluetooth->pending || bluetooth->question_id != id)
		return ENOENT;

	/* Whether it is this program's own pairing's question, asked while Pair runs. */
	own = 0U;
	same = strcmp(bluetooth->question_path, bluetooth->request_path);
	if (bluetooth->request_kind == KL_BACKEND_BT_PAIR && bluetooth->request_step == BT_STEP_PAIR && same == 0)
		own = 1U;

	/* Yes is an empty return; no is BlueZ's refusal. */
	bluetooth->pending = 0U;
	bluetooth->question_id = 0U;
	bluetooth->question_path[0] = '\0';
	if (yes) {
		error = dbus_send_return_to(&bluetooth->bus, bluetooth->pending_sender, bluetooth->pending_serial, "", NULL, 0);
	} else {
		error = dbus_send_error_to(&bluetooth->bus, bluetooth->pending_sender, bluetooth->pending_serial, "org.bluez.Error.Rejected", "Rejected by the user");
	}

	/* No to this program's own pairing: the pairing ends here, refused. */
	if (!yes && own)
		bt_request_give_up(bluetooth, ECONNREFUSED, "rejected");

	/* A connection that broke takes the pairing with it. */
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Gives up this program's own pairing going on: BlueZ is asked to cancel
 * it, and its answer is ECANCELED at once.  Returns 0, or ENOENT.
 */
int
kl_backend_bluetooth_cancel(
	struct kl_backend_bluetooth *bluetooth)
{
	/* Only a pairing of this program's going on. */
	if (bluetooth == NULL)
		return ENOENT;
	if (bluetooth->request_kind != KL_BACKEND_BT_PAIR)
		return ENOENT;

	/* Succeeded: given up, said as a result. */
	bt_request_give_up(bluetooth, ECANCELED, "cancelled");
	return 0;
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
bt_milliseconds(
	void)
{
	struct timespec now;

	/* A clock that fails reads as the start, which only delays what is due. */
	memset(&now, 0, sizeof(now));
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Opens the connection and asks, without waiting, for the signals that
 * follow org.bluez and then for its owner: the signals are subscribed
 * before anything is read, so that no change between the reading and the
 * subscription is missed.  A failure sets the next try after the wait,
 * which doubles.
 */
static void
bt_open(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	static const char *const rules[] = {
		"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
		"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
		"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
		"type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path_namespace='/org/bluez'"
	};
	struct dbus_arg argument;
	size_t index;
	int error;

	/* The connection itself: connect, authenticate and Hello are the only waits. */
	error = dbus_open_address(&bluetooth->bus, BT_DBUS_SOCKET);
	if (error != 0) {
		dbus_close(&bluetooth->bus);
		bluetooth->retry_ms = now + bluetooth->retry_wait_ms;
		bluetooth->retry_wait_ms *= 2U;
		if (bluetooth->retry_wait_ms > BT_RETRY_MAX_MS)
			bluetooth->retry_wait_ms = BT_RETRY_MAX_MS;
		return;
	}
	bluetooth->open = 1U;
	bluetooth->opened_ms = now;

	/* The subscriptions, in order, before the owner is asked. */
	memset(&argument, 0, sizeof(argument));
	for (index = 0; index < sizeof(rules) / sizeof(rules[0]); index++) {
		argument.string = rules[index];
		error = bt_send(bluetooth, BT_CALL_IGNORED, "", BT_BUS, BT_BUS_PATH, BT_BUS, "AddMatch", "s", &argument, 1, BT_CALL_MS);
		if (error != 0)
			return;
	}

	/* Who owns org.bluez now; NameOwnerChanged says it from then on. */
	argument.string = BT_SERVICE;
	(void)bt_send(bluetooth, BT_CALL_OWNER, "", BT_BUS, BT_BUS_PATH, BT_BUS, "GetNameOwner", "s", &argument, 1, BT_CALL_MS);
}

/*
 * Takes the loss of the connection: whatever BlueZ was doing for this
 * program is over, and the connection is opened again after the wait,
 * which doubles.
 */
static void
bt_lost(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	/* Only an open connection is lost. */
	if (!bluetooth->open)
		return;

	/* BlueZ is as gone as if it had stopped. */
	bt_forget_service(bluetooth);
	bluetooth->owner[0] = '\0';
	bluetooth->call_count = 0U;

	/* The connection, and the next try after the wait. */
	dbus_close(&bluetooth->bus);
	bluetooth->open = 0U;
	bluetooth->retry_ms = now + bluetooth->retry_wait_ms;
	bluetooth->retry_wait_ms *= 2U;
	if (bluetooth->retry_wait_ms > BT_RETRY_MAX_MS)
		bluetooth->retry_wait_ms = BT_RETRY_MAX_MS;
	fprintf(stderr, "wayland: bluetooth: connection lost\n");
}

/*
 * Forgets everything that came from the owner of org.bluez: the request
 * outstanding ends with ENOTCONN, the question with its end, the agent's
 * call held is dropped (it cannot be answered to a new owner), and the
 * calls sent to BlueZ, the adapter, the devices and the agent and scan
 * are cleared.
 */
static void
bt_forget_service(
	struct kl_backend_bluetooth *bluetooth)
{
	size_t index;
	size_t kept;

	/* The call held for the question is dropped first (no answer goes to an owner gone). */
	bluetooth->pending = 0U;

	/* The request outstanding. */
	if (bluetooth->request_kind != 0U)
		bt_request_finish(bluetooth, ENOTCONN, "lost");

	/* The question asked. */
	bt_question_end(bluetooth);

	/* The calls sent to BlueZ are answered to no one; the bus's own are kept. */
	kept = 0U;
	for (index = 0; index < bluetooth->call_count; index++) {
		if (bluetooth->calls[index].kind != BT_CALL_OWNER && bluetooth->calls[index].kind != BT_CALL_IGNORED)
			continue;
		bluetooth->calls[kept] = bluetooth->calls[index];
		kept++;
	}
	bluetooth->call_count = kept;

	/* The adapter, the devices, the agent and the scan. */
	bt_drop_adapter(bluetooth);
	bluetooth->need_root = 0U;
	bluetooth->agent_state = BT_AGENT_NONE;
	bluetooth->agent_due_ms = 0U;
	bluetooth->discovery_sent = 0U;
}

/*
 * Takes a new owner of org.bluez: its agent is registered and its adapter
 * looked for.
 */
static void
bt_owner_arrived(
	struct kl_backend_bluetooth *bluetooth,
	const char *owner)
{
	/* The owner, by its unique name, which checks the agent's callers. */
	bt_copy_text(bluetooth->owner, sizeof(bluetooth->owner), owner);

	/* The agent and the adapter, from the next update's work. */
	bluetooth->agent_state = BT_AGENT_NONE;
	bluetooth->agent_due_ms = 0U;
	bluetooth->need_root = 1U;
	bluetooth->adapter_due_ms = 0U;
	fprintf(stderr, "wayland: bluetooth: org.bluez is %s\n", owner);
}

/*
 * Sends a call without waiting and keeps it in the table with what it
 * asks (kind), the object it concerns (record) and when it is given up.
 * A call to org.bluez never starts it.  Returns 0, EBUSY when the table is
 * full, or the error of sending.
 */
static int
bt_send(
	struct kl_backend_bluetooth *bluetooth,
	unsigned kind,
	const char *record,
	const char *destination,
	const char *path,
	const char *interface,
	const char *member,
	const char *signature,
	const struct dbus_arg *args,
	size_t count,
	uint64_t wait_ms)
{
	struct bt_call *call;
	uint32_t serial;
	int error;

	/* Room for the call. */
	if (bluetooth->call_count >= BT_CALLS_MAX)
		return EBUSY;

	/* The call, which never starts a service no one owns. */
	error = dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, destination, path, interface, member, signature, args, count, &serial);
	if (error != 0)
		return error;

	/* Succeeded: kept until its answer or its deadline. */
	call = &bluetooth->calls[bluetooth->call_count];
	memset(call, 0, sizeof(*call));
	call->serial = serial;
	call->kind = kind;
	bt_copy_text(call->path, sizeof(call->path), record);
	call->deadline_ms = bt_milliseconds() + wait_ms;
	bluetooth->call_count++;
	return 0;
}

/* Takes one message of the dispatch: an answer, a signal or a call to the agent. */
static int
bt_message(
	struct dbus_reply *message,
	void *data)
{
	struct kl_backend_bluetooth *bluetooth;
	struct bt_call call;
	size_t index;

	/* What the message is decides where it goes. */
	bluetooth = data;
	switch (message->type) {
	case LINUX_DBUS_METHOD_CALL:
		bt_agent_call(bluetooth, message);
		break;
	case LINUX_DBUS_SIGNAL:
		bt_signal(bluetooth, message);
		break;
	case LINUX_DBUS_METHOD_RETURN:
	case LINUX_DBUS_ERROR:
		/* The call it answers leaves the table first; an answer to none is dropped. */
		for (index = 0; index < bluetooth->call_count; index++) {
			if (bluetooth->calls[index].serial != message->reply_serial)
				continue;
			call = bluetooth->calls[index];
			bluetooth->call_count--;
			memmove(&bluetooth->calls[index], &bluetooth->calls[index + 1], (bluetooth->call_count - index) * sizeof(bluetooth->calls[0]));
			bt_answer(bluetooth, &call, message);
			break;
		}
		break;
	default:
		break;
	}

	/* Succeeded: the dispatch goes on whatever this message said. */
	return 0;
}

/*
 * Takes the answer to a call of the table, or its giving up (reply NULL):
 * each kind goes to what asked it.
 */
static void
bt_answer(
	struct kl_backend_bluetooth *bluetooth,
	const struct bt_call *call,
	struct dbus_reply *reply)
{
	struct bt_device *device;
	const char *error_name;
	const char *error_text;
	const char *owner;
	struct dbus_arg argument;
	unsigned root;
	int registered;
	int gone;
	int same;
	int error;

	/* The error the answer carries, if it is one; a call given up is a timeout. */
	error_name = NULL;
	error_text = "";
	if (reply == NULL) {
		error_name = "org.freedesktop.DBus.Error.Timeout";
	} else if (reply->type == LINUX_DBUS_ERROR) {
		error_name = reply->error;
		if (reply->signature[0] == 's') {
			error = dbus_read_string(reply, &error_text);
			if (error != 0)
				error_text = "";
		}
	}

	/* What the call asked. */
	switch (call->kind) {
	case BT_CALL_OWNER:
		/* Nobody owns org.bluez: BlueZ is not running. */
		if (error_name != NULL)
			break;
		error = dbus_read_string(reply, &owner);
		if (error != 0)
			break;

		/* An owner found before NameOwnerChanged said otherwise. */
		if (bluetooth->owner[0] == '\0')
			bt_owner_arrived(bluetooth, owner);
		break;
	case BT_CALL_REGISTER:
		/* Registered, or registered already. */
		registered = 1;
		if (error_name != NULL) {
			same = strcmp(error_name, "org.bluez.Error.AlreadyExists");
			if (same != 0)
				registered = 0;
		}

		/* Registered: asked to be the default agent next. */
		if (registered) {
			memset(&argument, 0, sizeof(argument));
			argument.string = BT_AGENT_PATH;
			bluetooth->agent_state = BT_AGENT_DEFAULTING;
			error = bt_send(bluetooth, BT_CALL_DEFAULT, "", BT_SERVICE, BT_ROOT_PATH, BT_AGENT_MANAGER, "RequestDefaultAgent", "o", &argument, 1, BT_CALL_MS);
			if (error == 0)
				break;
		}

		/* Not registered (or no room to go on): tried again later. */
		if (error_name == NULL)
			error_name = "no room";
		fprintf(stderr, "wayland: bluetooth: agent not registered: %s\n", error_name);
		bluetooth->agent_state = BT_AGENT_NONE;
		bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_RETRY_MS;
		break;
	case BT_CALL_DEFAULT:
		/* The default agent: the pairings others start come here too. */
		if (error_name == NULL) {
			bluetooth->agent_state = BT_AGENT_HELD;
			break;
		}

		/* Registered but not the default: tried again later. */
		fprintf(stderr, "wayland: bluetooth: not the default agent: %s\n", error_name);
		bluetooth->agent_state = BT_AGENT_NONE;
		bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_RETRY_MS;
		break;
	case BT_CALL_ROOT:
	case BT_CALL_NODES:
		/* An introspection that failed is tried again when the adapter is read. */
		if (error_name != NULL) {
			if (call->kind == BT_CALL_ROOT)
				bluetooth->need_root = 1U;
			else
				bluetooth->need_nodes = 1U;
			break;
		}
		root = 0U;
		if (call->kind == BT_CALL_ROOT)
			root = 1U;

		/* An adapter's list is only of the adapter followed now. */
		same = strcmp(call->path, bluetooth->adapter_path);
		if (!root && same != 0)
			break;
		bt_introspect_answer(bluetooth, reply, root);
		break;
	case BT_CALL_ADAPTER:
		/* The adapter's properties, unless it went away meanwhile. */
		if (!bluetooth->adapter || error_name != NULL)
			break;
		same = strcmp(call->path, bluetooth->adapter_path);
		if (same != 0)
			break;
		(void)bt_read_properties(bluetooth, NULL, reply, bt_adapter_property);
		break;
	case BT_CALL_DEVICE:
		/* A device that went away meanwhile is not brought back. */
		device = bt_find_path(bluetooth, call->path);
		if (device == NULL)
			break;

		/* A device BlueZ no longer has goes; another failure is read again. */
		if (error_name != NULL) {
			gone = bt_error_is_unknown(error_name);
			if (gone) {
				bt_remove_device(bluetooth, call->path);
			} else {
				device->need_read = 1U;
			}
			break;
		}
		(void)bt_read_properties(bluetooth, device, reply, bt_device_property);
		break;
	case BT_CALL_BATTERY:
		/* A device without a battery to tell keeps -1. */
		device = bt_find_path(bluetooth, call->path);
		if (device == NULL || error_name != NULL)
			break;
		(void)bt_read_properties(bluetooth, device, reply, bt_battery_property);
		break;
	case BT_CALL_REQUEST:
		bt_request_answer(bluetooth, reply, error_name, error_text);
		break;
	case BT_CALL_DISCOVERY:
		/* A scan going on already is fine. */
		if (error_name == NULL)
			break;
		same = strcmp(error_name, "org.bluez.Error.InProgress");
		if (same == 0)
			break;

		/* A scan BlueZ refused is asked again a little later. */
		fprintf(stderr, "wayland: bluetooth: discovery: %s\n", error_name);
		bluetooth->discovery_sent = 0U;
		bluetooth->discovery_due_ms = bt_milliseconds() + BT_DISCOVERY_RETRY_MS;
		break;
	default:
		break;
	}
}

/*
 * Gives up the calls whose deadline has passed (each answered as a
 * timeout), and refuses the agent's call held too long.
 */
static void
bt_expire(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	struct bt_call call;
	size_t index;

	/* Each call past its deadline leaves the table, then is answered as a timeout. */
	index = 0U;
	while (index < bluetooth->call_count) {
		if (now < bluetooth->calls[index].deadline_ms) {
			index++;
			continue;
		}
		call = bluetooth->calls[index];
		bluetooth->call_count--;
		memmove(&bluetooth->calls[index], &bluetooth->calls[index + 1], (bluetooth->call_count - index) * sizeof(bluetooth->calls[0]));

		/* A pairing given up is cancelled at BlueZ too. */
		if (call.kind == BT_CALL_REQUEST && bluetooth->request_step == BT_STEP_PAIR) {
			(void)dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, BT_SERVICE, bluetooth->request_path, BT_DEVICE, "CancelPairing", "", NULL, 0, &call.serial);
		}
		bt_answer(bluetooth, &call, NULL);
	}

	/* An agent's call nobody answered in time is refused, and its question ends. */
	if (bluetooth->pending && now >= bluetooth->pending_ms + BT_PENDING_MS) {
		bt_agent_release_pending(bluetooth, "org.bluez.Error.Rejected");
		bt_question_end(bluetooth);
	}
}

/*
 * Sends what is due while org.bluez has an owner: the agent's
 * registration, the adapter's search and reading, the devices' readings
 * (a few at a time), and the scan started or stopped.
 */
static void
bt_work(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	struct dbus_arg arguments[2];
	struct bt_device *device;
	uint64_t wait;
	size_t index;
	size_t reading;
	size_t outstanding;
	int error;

	/* Nothing is sent to an org.bluez no one owns. */
	if (bluetooth->owner[0] == '\0')
		return;

	/* The agent, registered once per owner (again after a failure's wait). */
	if (bluetooth->agent_state == BT_AGENT_NONE && now >= bluetooth->agent_due_ms) {
		memset(arguments, 0, sizeof(arguments));
		arguments[0].string = BT_AGENT_PATH;
		arguments[1].string = BT_CAPABILITY;
		error = bt_send(bluetooth, BT_CALL_REGISTER, "", BT_SERVICE, BT_ROOT_PATH, BT_AGENT_MANAGER, "RegisterAgent", "os", arguments, 2, BT_CALL_MS);
		if (error == 0)
			bluetooth->agent_state = BT_AGENT_REGISTERING;
	}

	/* The adapters under BlueZ's root, when one is to be chosen. */
	memset(arguments, 0, sizeof(arguments));
	if (bluetooth->need_root) {
		error = bt_send(bluetooth, BT_CALL_ROOT, "", BT_SERVICE, BT_ROOT_PATH, BT_INTROSPECTABLE, "Introspect", "", NULL, 0, BT_CALL_MS);
		if (error == 0)
			bluetooth->need_root = 0U;
	}

	/* Nothing more without an adapter. */
	if (!bluetooth->adapter)
		return;

	/* The devices under the adapter, when they are to be listed. */
	if (bluetooth->need_nodes) {
		error = bt_send(bluetooth, BT_CALL_NODES, bluetooth->adapter_path, BT_SERVICE, bluetooth->adapter_path, BT_INTROSPECTABLE, "Introspect", "", NULL, 0, BT_CALL_MS);
		if (error == 0)
			bluetooth->need_nodes = 0U;
	}

	/* The adapter's properties, when the reading is due and the last one has been answered. */
	outstanding = bt_outstanding(bluetooth, BT_CALL_ADAPTER);
	if (now >= bluetooth->adapter_due_ms && outstanding == 0U) {
		arguments[0].string = BT_ADAPTER;
		error = bt_send(bluetooth, BT_CALL_ADAPTER, bluetooth->adapter_path, BT_SERVICE, bluetooth->adapter_path, BT_PROPERTIES, "GetAll", "s", arguments, 1, BT_CALL_MS);
		wait = BT_IDLE_MS;
		if (bluetooth->watching)
			wait = BT_WATCH_MS;
		if (error == 0)
			bluetooth->adapter_due_ms = now + wait;
	}

	/* How many readings of devices are out. */
	reading = bt_outstanding(bluetooth, BT_CALL_DEVICE);
	reading += bt_outstanding(bluetooth, BT_CALL_BATTERY);

	/* The devices still to be read, a few at a time. */
	for (index = 0; index < bluetooth->table_count && reading < BT_READS_MAX; index++) {
		device = &bluetooth->table[index];

		/* Its properties first. */
		if (device->need_read) {
			arguments[0].string = BT_DEVICE;
			error = bt_send(bluetooth, BT_CALL_DEVICE, device->path, BT_SERVICE, device->path, BT_PROPERTIES, "GetAll", "s", arguments, 1, BT_CALL_MS);
			if (error != 0)
				break;
			device->need_read = 0U;
			reading++;
			continue;
		}

		/* Then its battery (a device without one answers with an error). */
		if (device->need_battery) {
			arguments[0].string = BT_BATTERY;
			error = bt_send(bluetooth, BT_CALL_BATTERY, device->path, BT_SERVICE, device->path, BT_PROPERTIES, "GetAll", "s", arguments, 1, BT_CALL_MS);
			if (error != 0)
				break;
			device->need_battery = 0U;
			reading++;
		}
	}

	/* A scan wished for while the adapter is on: BlueZ's discovery for this program. */
	if (bluetooth->scanning &&
	    !bluetooth->discovery_sent &&
	    bluetooth->powered &&
	    now >= bluetooth->discovery_due_ms) {
		error = bt_send(bluetooth, BT_CALL_DISCOVERY, "", BT_SERVICE, bluetooth->adapter_path, BT_ADAPTER, "StartDiscovery", "", NULL, 0, BT_CALL_MS);
		if (error == 0)
			bluetooth->discovery_sent = 1U;
	} else if (!bluetooth->scanning && bluetooth->discovery_sent) {
		/* The wish ended: the discovery stops. */
		error = bt_send(bluetooth, BT_CALL_IGNORED, "", BT_SERVICE, bluetooth->adapter_path, BT_ADAPTER, "StopDiscovery", "", NULL, 0, BT_CALL_MS);
		if (error == 0)
			bluetooth->discovery_sent = 0U;
	}
}

/* Counts the calls of a kind sent and not answered yet. */
static size_t
bt_outstanding(
	const struct kl_backend_bluetooth *bluetooth,
	unsigned kind)
{
	size_t count;
	size_t index;

	/* Each call of the table. */
	count = 0U;
	for (index = 0; index < bluetooth->call_count; index++) {
		if (bluetooth->calls[index].kind == kind)
			count++;
	}

	/* Succeeded: how many. */
	return count;
}

/*
 * Takes a signal: the bus's word on org.bluez's owner, or org.bluez's own
 * from its present owner (a signal from an owner no longer known is
 * stale).
 */
static void
bt_signal(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *signal)
{
	int from_bus;
	int same;

	/* A signal names its sender on a bus. */
	if (signal->sender == NULL)
		return;

	/* The bus's own signal says who owns org.bluez. */
	same = strcmp(signal->member, "NameOwnerChanged");
	from_bus = strcmp(signal->sender, BT_BUS);
	if (same == 0 && from_bus == 0) {
		bt_owner_changed(bluetooth, signal);
		return;
	}

	/* Only org.bluez's present owner is listened to. */
	if (bluetooth->owner[0] == '\0')
		return;
	same = strcmp(signal->sender, bluetooth->owner);
	if (same != 0)
		return;

	/* Objects added, objects removed, properties changed. */
	same = strcmp(signal->member, "InterfacesAdded");
	if (same == 0) {
		bt_interfaces_added(bluetooth, signal);
		return;
	}
	same = strcmp(signal->member, "InterfacesRemoved");
	if (same == 0) {
		bt_interfaces_removed(bluetooth, signal);
		return;
	}
	same = strcmp(signal->member, "PropertiesChanged");
	if (same == 0)
		bt_properties_changed(bluetooth, signal);
}

/*
 * Takes NameOwnerChanged(name, old, new) of org.bluez: an owner that went
 * or was replaced takes everything it held with it, and a new one is
 * followed from the start.
 */
static void
bt_owner_changed(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *signal)
{
	const char *name;
	const char *old_owner;
	const char *new_owner;
	int error;
	int same;

	/* The three names. */
	error = dbus_read_string(signal, &name);
	if (error != 0)
		return;
	error = dbus_read_string(signal, &old_owner);
	if (error != 0)
		return;
	error = dbus_read_string(signal, &new_owner);
	if (error != 0)
		return;

	/* Only org.bluez, and only a real change of who owns it. */
	same = strcmp(name, BT_SERVICE);
	if (same != 0)
		return;
	same = strcmp(new_owner, bluetooth->owner);
	if (same == 0)
		return;

	/* The owner known so far is gone, with what it held. */
	if (bluetooth->owner[0] != '\0') {
		fprintf(stderr, "wayland: bluetooth: org.bluez %s left\n", bluetooth->owner);
		bt_forget_service(bluetooth);
		bluetooth->owner[0] = '\0';
	}

	/* A new owner is followed from the start. */
	if (new_owner[0] != '\0')
		bt_owner_arrived(bluetooth, new_owner);
}

/*
 * Takes InterfacesAdded(o, a{sa{sv}}): an adapter while none is chosen,
 * or a device under the chosen one, with its properties.
 */
static void
bt_interfaces_added(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *signal)
{
	struct bt_device *device;
	const char *path;
	const char *interface;
	size_t end;
	size_t entry_end;
	int is_adapter;
	int is_device;
	int error;
	int same;

	/* The object and the dictionary of its interfaces. */
	error = dbus_read_string(signal, &path);
	if (error != 0)
		return;
	error = dbus_read_array(signal, 8, &end);
	if (error != 0)
		return;

	/* Which object it is. */
	is_adapter = bt_is_adapter_path(path);
	is_device = bt_is_device_path(bluetooth, path);

	/* Each interface and its properties. */
	while (signal->cursor < end) {
		error = dbus_align(signal, 8);
		if (error != 0)
			return;
		error = dbus_read_string(signal, &interface);
		if (error != 0)
			return;

		/* An adapter, chosen when there is none; its properties are read by the reading. */
		same = strcmp(interface, BT_ADAPTER);
		if (same == 0 && is_adapter && !bluetooth->adapter)
			bt_choose_adapter(bluetooth, path);

		/* A device's properties (it is added if it is new). */
		same = strcmp(interface, BT_DEVICE);
		if (same == 0 && is_device) {
			device = bt_add_device(bluetooth, path);
			if (device != NULL) {
				device->need_read = 0U;
				device->need_battery = 0U;
				error = bt_read_properties(bluetooth, device, signal, bt_device_property);
				if (error != 0)
					return;
				continue;
			}
		}

		/* A battery of a known device. */
		same = strcmp(interface, BT_BATTERY);
		if (same == 0 && is_device) {
			device = bt_find_path(bluetooth, path);
			if (device != NULL) {
				error = bt_read_properties(bluetooth, device, signal, bt_battery_property);
				if (error != 0)
					return;
				continue;
			}
		}

		/* Any other interface's properties are stepped over. */
		error = dbus_read_array(signal, 8, &entry_end);
		if (error != 0)
			return;
		signal->cursor = entry_end;
	}
}

/*
 * Takes InterfacesRemoved(o, as): a device or its battery, or the chosen
 * adapter, gone.
 */
static void
bt_interfaces_removed(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *signal)
{
	struct bt_device *device;
	const char *path;
	const char *interface;
	size_t end;
	int chosen;
	int error;
	int same;

	/* The object and the list of its interfaces gone. */
	error = dbus_read_string(signal, &path);
	if (error != 0)
		return;
	error = dbus_read_array(signal, 4, &end);
	if (error != 0)
		return;

	/* Each interface. */
	while (signal->cursor < end) {
		error = dbus_read_string(signal, &interface);
		if (error != 0)
			return;

		/* The chosen adapter: everything under it goes, and another is looked for. */
		same = strcmp(interface, BT_ADAPTER);
		chosen = strcmp(path, bluetooth->adapter_path);
		if (same == 0 && bluetooth->adapter && chosen == 0) {
			bt_drop_adapter(bluetooth);
			bluetooth->need_root = 1U;
			return;
		}

		/* A device. */
		same = strcmp(interface, BT_DEVICE);
		if (same == 0) {
			bt_remove_device(bluetooth, path);
			return;
		}

		/* A device's battery: unknown again. */
		same = strcmp(interface, BT_BATTERY);
		if (same == 0) {
			device = bt_find_path(bluetooth, path);
			if (device != NULL)
				device->battery = -1;
		}
	}
}

/*
 * Takes PropertiesChanged(s, a{sv}, as) of the adapter, a device or its
 * battery: the changed values, and the invalidated signal strength as
 * unknown.  A device not in the table is read whole.
 */
static void
bt_properties_changed(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *signal)
{
	struct bt_device *device;
	const char *interface;
	const char *name;
	size_t end;
	int is_device;
	int chosen;
	int error;
	int same;

	/* The interface whose properties changed. */
	error = dbus_read_string(signal, &interface);
	if (error != 0)
		return;

	/* The chosen adapter's. */
	same = strcmp(interface, BT_ADAPTER);
	if (same == 0) {
		chosen = strcmp(signal->path, bluetooth->adapter_path);
		if (!bluetooth->adapter || chosen != 0)
			return;
		(void)bt_read_properties(bluetooth, NULL, signal, bt_adapter_property);
		return;
	}

	/* Otherwise only a device's or its battery's. */
	is_device = bt_is_device_path(bluetooth, signal->path);
	if (!is_device)
		return;
	device = bt_find_path(bluetooth, signal->path);

	/* A device not known yet is read whole (what changed alone may not say its address). */
	if (device == NULL) {
		device = bt_add_device(bluetooth, signal->path);
		return;
	}

	/* A battery's. */
	same = strcmp(interface, BT_BATTERY);
	if (same == 0) {
		(void)bt_read_properties(bluetooth, device, signal, bt_battery_property);
		return;
	}

	/* Only the device's own beyond this. */
	same = strcmp(interface, BT_DEVICE);
	if (same != 0)
		return;
	error = bt_read_properties(bluetooth, device, signal, bt_device_property);
	if (error != 0)
		return;

	/* The properties that have no value any more: the signal strength is unknown again. */
	error = dbus_read_array(signal, 4, &end);
	if (error != 0)
		return;
	while (signal->cursor < end) {
		error = dbus_read_string(signal, &name);
		if (error != 0)
			return;
		same = strcmp(name, "RSSI");
		if (same == 0)
			device->rssi = 0;
	}
}

/*
 * Reads a dictionary of properties (a{sv}) and gives each value read here
 * to apply; values of other types are stepped over.  Returns 0, or EPROTO
 * for a malformed dictionary.
 */
static int
bt_read_properties(
	struct kl_backend_bluetooth *bluetooth,
	struct bt_device *device,
	struct dbus_reply *reply,
	bt_property_fn apply)
{
	struct bt_value value;
	const char *name;
	const char *signature;
	size_t end;
	int error;

	/* The dictionary's extent. */
	error = dbus_read_array(reply, 8, &end);
	if (error != 0)
		return error;

	/* Each entry: its name, then its variant. */
	while (reply->cursor < end) {
		error = dbus_align(reply, 8);
		if (error != 0)
			return error;
		error = dbus_read_string(reply, &name);
		if (error != 0)
			return error;
		error = dbus_read_variant(reply, &signature);
		if (error != 0)
			return error;
		error = bt_read_value(reply, signature, &value);
		if (error != 0)
			return error;

		/* A value of a type read here goes to its owner. */
		if (value.kind != 0)
			apply(bluetooth, device, name, &value);
	}

	/* Succeeded: past the dictionary. */
	reply->cursor = end;
	return 0;
}

/*
 * Reads one value of a variant's type: a byte, a Boolean, a sixteen- or
 * thirty-two-bit integer, a string or an object path; any other type is
 * stepped over with value->kind 0.  Returns 0, or EPROTO.
 */
static int
bt_read_value(
	struct dbus_reply *reply,
	const char *signature,
	struct bt_value *value)
{
	uint16_t half;
	uint8_t byte;
	int error;

	/* Nothing read yet. */
	memset(value, 0, sizeof(*value));

	/* Which type it is. */
	switch (signature[0]) {
	case 'y':
		error = dbus_read_byte(reply, &byte);
		value->number = byte;
		value->integer = byte;
		break;
	case 'b':
	case 'u':
		error = dbus_read_number(reply, &value->number);
		value->integer = (int32_t)value->number;
		break;
	case 'i':
		error = dbus_read_number(reply, &value->number);
		value->integer = (int32_t)value->number;
		break;
	case 'n':
		error = dbus_read_int16(reply, &half);
		value->number = half;
		value->integer = (int16_t)half;
		break;
	case 'q':
		error = dbus_read_int16(reply, &half);
		value->number = half;
		value->integer = half;
		break;
	case 's':
	case 'o':
		error = dbus_read_string(reply, &value->string);
		break;
	default:
		/* A type not read here is stepped over. */
		error = dbus_skip_value(reply, &signature);
		if (error != 0)
			return error;

		/* Succeeded: nothing read. */
		return 0;
	}

	/* A value that does not fit the message. */
	if (error != 0)
		return error;

	/* Succeeded: the value and its type. */
	value->kind = signature[0];
	return 0;
}

/* Takes one property of the adapter. */
static void
bt_adapter_property(
	struct kl_backend_bluetooth *bluetooth,
	struct bt_device *device,
	const char *name,
	const struct bt_value *value)
{
	int same;

	(void)device;

	/* Its address. */
	same = strcmp(name, "Address");
	if (same == 0 && value->kind == 's') {
		bt_copy_text(bluetooth->adapter_address, sizeof(bluetooth->adapter_address), value->string);
		return;
	}

	/* Its name as the user knows it. */
	same = strcmp(name, "Alias");
	if (same == 0 && value->kind == 's') {
		bt_copy_text(bluetooth->adapter_alias, sizeof(bluetooth->adapter_alias), value->string);
		return;
	}

	/* The user's switch: a scan of this program's ends with it (BlueZ stops discovery when powered off). */
	same = strcmp(name, "Powered");
	if (same == 0 && value->kind == 'b') {
		bluetooth->powered = bt_flag(value);
		if (!bluetooth->powered)
			bluetooth->discovery_sent = 0U;
		return;
	}

	/* The power's state, where BlueZ tells it (experimental). */
	same = strcmp(name, "PowerState");
	if (same == 0 && value->kind == 's') {
		bluetooth->power_state = bt_power_state(value->string);
		return;
	}

	/* Whether it looks for devices. */
	same = strcmp(name, "Discovering");
	if (same == 0 && value->kind == 'b')
		bluetooth->discovering = bt_flag(value);
}

/* Takes one property of a device. */
static void
bt_device_property(
	struct kl_backend_bluetooth *bluetooth,
	struct bt_device *device,
	const char *name,
	const struct bt_value *value)
{
	int asked;
	int same;

	/* Its address, and the kind of address. */
	same = strcmp(name, "Address");
	if (same == 0 && value->kind == 's') {
		bt_copy_text(device->address, sizeof(device->address), value->string);
		return;
	}
	same = strcmp(name, "AddressType");
	if (same == 0 && value->kind == 's') {
		same = strcmp(value->string, "random");
		device->random = 0U;
		if (same == 0)
			device->random = 1U;
		return;
	}

	/* Its names: the name it gives, and the name shown (the user's, or that one). */
	same = strcmp(name, "Name");
	if (same == 0 && value->kind == 's') {
		device->has_name = 1U;
		return;
	}
	same = strcmp(name, "Alias");
	if (same == 0 && value->kind == 's') {
		bt_copy_text(device->alias, sizeof(device->alias), value->string);
		return;
	}

	/* A class says BR/EDR; an appearance alone says LE. */
	same = strcmp(name, "Class");
	if (same == 0) {
		device->has_class = 1U;
		return;
	}
	same = strcmp(name, "Appearance");
	if (same == 0) {
		device->has_appearance = 1U;
		return;
	}

	/* What it is, for its icon. */
	same = strcmp(name, "Icon");
	if (same == 0 && value->kind == 's') {
		device->kind = bt_kind(value->string);
		return;
	}

	/* Paired: a pairing's number shown for it is over (the pairing ended). */
	same = strcmp(name, "Paired");
	if (same == 0 && value->kind == 'b') {
		device->paired = bt_flag(value);
		asked = strcmp(bluetooth->question_path, device->path);
		if (device->paired &&
		    bluetooth->question_id != 0U &&
		    bluetooth->question_kind == KL_BACKEND_BT_ASK_PASSKEY &&
		    asked == 0)
			bt_question_end(bluetooth);
		return;
	}

	/* Bonded (its keys kept), trusted, paired the legacy way, connected. */
	same = strcmp(name, "Bonded");
	if (same == 0 && value->kind == 'b') {
		device->bonded = bt_flag(value);
		return;
	}
	same = strcmp(name, "Trusted");
	if (same == 0 && value->kind == 'b') {
		device->trusted = bt_flag(value);
		return;
	}
	same = strcmp(name, "LegacyPairing");
	if (same == 0 && value->kind == 'b') {
		device->legacy = bt_flag(value);
		return;
	}
	same = strcmp(name, "Connected");
	if (same == 0 && value->kind == 'b') {
		device->connected = bt_flag(value);
		return;
	}

	/* Its signal, seen while discovering. */
	same = strcmp(name, "RSSI");
	if (same == 0 && value->kind == 'n')
		device->rssi = value->integer;
}

/* Takes one property of a device's battery. */
static void
bt_battery_property(
	struct kl_backend_bluetooth *bluetooth,
	struct bt_device *device,
	const char *name,
	const struct bt_value *value)
{
	int same;

	(void)bluetooth;

	/* The charge in percent, a byte. */
	same = strcmp(name, "Percentage");
	if (same == 0 && value->kind == 'y' && value->number <= 100U)
		device->battery = (int)value->number;
}

/* Chooses an adapter: its properties and its devices are read next. */
static void
bt_choose_adapter(
	struct kl_backend_bluetooth *bluetooth,
	const char *path)
{
	/* Nothing of the one before is kept. */
	bt_drop_adapter(bluetooth);

	/* The adapter, read at once. */
	bluetooth->adapter = 1U;
	bt_copy_text(bluetooth->adapter_path, sizeof(bluetooth->adapter_path), path);
	bluetooth->need_nodes = 1U;
	bluetooth->adapter_due_ms = 0U;
	fprintf(stderr, "wayland: bluetooth: adapter %s\n", path);
}

/* Forgets the adapter and its devices; a scan of this program's ended with it. */
static void
bt_drop_adapter(
	struct kl_backend_bluetooth *bluetooth)
{
	/* The adapter's properties. */
	bluetooth->adapter = 0U;
	bluetooth->adapter_path[0] = '\0';
	bluetooth->adapter_address[0] = '\0';
	bluetooth->adapter_alias[0] = '\0';
	bluetooth->powered = 0U;
	bluetooth->power_state = 0U;
	bluetooth->discovering = 0U;
	bluetooth->need_nodes = 0U;
	bluetooth->discovery_sent = 0U;

	/* Its devices. */
	bluetooth->table_count = 0U;
}

/*
 * Takes an introspection's XML: of BlueZ's root (root 1), the first
 * adapter node ("hciN", the lowest number) is chosen when none is; of the
 * adapter, each device node ("dev_...") is added to the table to be read.
 */
static void
bt_introspect_answer(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *reply,
	unsigned root)
{
	const char *xml;
	const char *cursor;
	const char *name;
	const char *name_end;
	char path[BT_PATH_MAX];
	char best[BT_PATH_MAX];
	size_t length;
	size_t best_length;
	size_t index;
	int valid;
	int order;
	int status;
	int error;

	/* The XML. */
	error = dbus_read_string(reply, &xml);
	if (error != 0)
		return;

	/* Each child node, by its name. */
	best[0] = '\0';
	best_length = 0U;
	cursor = xml;
	for (;;) {
		cursor = strstr(cursor, "<node name=\"");
		if (cursor == NULL)
			break;
		name = cursor + 12;
		name_end = strchr(name, '"');
		if (name_end == NULL)
			break;
		cursor = name_end;
		length = (size_t)(name_end - name);

		/* A name of letters, digits and underscores, short enough. */
		valid = 0;
		if (length > 0U && length < 40U)
			valid = 1;
		for (index = 0; valid && index < length; index++) {
			if (name[index] >= 'a' && name[index] <= 'z')
				continue;
			if (name[index] >= 'A' && name[index] <= 'Z')
				continue;
			if (name[index] >= '0' && name[index] <= '9')
				continue;
			if (name[index] == '_')
				continue;
			valid = 0;
		}
		if (!valid)
			continue;

		/* The root's adapters: the lowest number wins (a shorter name, then the lower text). */
		if (root) {
			if (length < 4U)
				continue;
			order = memcmp(name, "hci", 3);
			if (order != 0)
				continue;
			if (best[0] != '\0' && length > best_length)
				continue;
			order = -1;
			if (best[0] != '\0' && length == best_length)
				order = memcmp(name, best, length);
			if (order >= 0)
				continue;
			memcpy(best, name, length);
			best[length] = '\0';
			best_length = length;
			continue;
		}

		/* The adapter's devices. */
		if (length < 5U)
			continue;
		order = memcmp(name, "dev_", 4);
		if (order != 0)
			continue;
		status = snprintf(path, sizeof(path), "%s/%.*s", bluetooth->adapter_path, (int)length, name);
		if (status < 0 || (size_t)status >= sizeof(path))
			continue;
		(void)bt_add_device(bluetooth, path);
	}

	/* The adapter chosen, if one was found and none is followed yet. */
	if (root && best[0] != '\0' && !bluetooth->adapter) {
		status = snprintf(path, sizeof(path), "%s/%s", BT_ROOT_PATH, best);
		if (status > 0 && (size_t)status < sizeof(path))
			bt_choose_adapter(bluetooth, path);
	}
}

/* Says whether a path is a device directly under the chosen adapter (".../hciN/dev_..."). */
static int
bt_is_device_path(
	const struct kl_backend_bluetooth *bluetooth,
	const char *path)
{
	const char *deeper;
	size_t length;
	int same;

	/* Only under an adapter. */
	if (!bluetooth->adapter)
		return 0;
	length = strlen(bluetooth->adapter_path);
	same = strncmp(path, bluetooth->adapter_path, length);
	if (same != 0)
		return 0;

	/* One more component, a device's. */
	same = strncmp(path + length, "/dev_", 5);
	if (same != 0)
		return 0;
	deeper = strchr(path + length + 1, '/');
	if (deeper != NULL)
		return 0;

	/* Succeeded: a device's path. */
	return 1;
}

/* Says whether a path is an adapter's ("/org/bluez/hciN"). */
static int
bt_is_adapter_path(
	const char *path)
{
	const char *deeper;
	int same;

	/* Under the root, as hci-something with nothing after it. */
	same = strncmp(path, BT_ROOT_PATH "/hci", sizeof(BT_ROOT_PATH "/hci") - 1U);
	if (same != 0)
		return 0;
	deeper = strchr(path + sizeof(BT_ROOT_PATH), '/');
	if (deeper != NULL)
		return 0;

	/* Succeeded: an adapter's path. */
	return 1;
}

/* Finds a device of the table by its path, or NULL. */
static struct bt_device *
bt_find_path(
	struct kl_backend_bluetooth *bluetooth,
	const char *path)
{
	size_t index;
	int same;

	/* Each device. */
	for (index = 0; index < bluetooth->table_count; index++) {
		same = strcmp(bluetooth->table[index].path, path);
		if (same == 0)
			return &bluetooth->table[index];
	}

	/* None. */
	return NULL;
}

/* Finds a device of the table by its address, or NULL. */
static struct bt_device *
bt_find_address(
	struct kl_backend_bluetooth *bluetooth,
	const char *address)
{
	size_t index;
	int same;

	/* Each device whose address is known. */
	for (index = 0; index < bluetooth->table_count; index++) {
		if (bluetooth->table[index].address[0] == '\0')
			continue;
		same = strcasecmp(bluetooth->table[index].address, address);
		if (same == 0)
			return &bluetooth->table[index];
	}

	/* None. */
	return NULL;
}

/*
 * Adds a device by its path, to be read, or finds it when it is there.
 * A full table gives the place of the weakest device that is not paired;
 * a table of paired devices alone takes no more.  Returns the device, or
 * NULL.
 */
static struct bt_device *
bt_add_device(
	struct kl_backend_bluetooth *bluetooth,
	const char *path)
{
	struct bt_device *device;
	size_t index;
	size_t weakest;
	int found;

	/* One already known. */
	device = bt_find_path(bluetooth, path);
	if (device != NULL)
		return device;

	/* A place: a new one, or the weakest device not paired. */
	if (bluetooth->table_count < BT_TABLE_MAX) {
		device = &bluetooth->table[bluetooth->table_count];
		bluetooth->table_count++;
	} else {
		found = 0;
		weakest = 0U;
		for (index = 0; index < bluetooth->table_count; index++) {
			if (bluetooth->table[index].paired)
				continue;
			if (found && bluetooth->table[index].rssi >= bluetooth->table[weakest].rssi)
				continue;
			weakest = index;
			found = 1;
		}
		if (!found)
			return NULL;
		device = &bluetooth->table[weakest];
	}

	/* Succeeded: a device known only by its path, to be read whole. */
	memset(device, 0, sizeof(*device));
	bt_copy_text(device->path, sizeof(device->path), path);
	device->battery = -1;
	device->need_read = 1U;
	device->need_battery = 1U;
	return device;
}

/* Removes a device by its path; a question about it is over. */
static void
bt_remove_device(
	struct kl_backend_bluetooth *bluetooth,
	const char *path)
{
	size_t index;
	int same;

	/* A question of its pairing ends with it. */
	same = strcmp(bluetooth->question_path, path);
	if (bluetooth->question_id != 0U && same == 0) {
		bt_agent_release_pending(bluetooth, "org.bluez.Error.Canceled");
		bt_question_end(bluetooth);
	}

	/* The device leaves the table; the others keep their order. */
	for (index = 0; index < bluetooth->table_count; index++) {
		same = strcmp(bluetooth->table[index].path, path);
		if (same != 0)
			continue;
		bluetooth->table_count--;
		memmove(&bluetooth->table[index], &bluetooth->table[index + 1], (bluetooth->table_count - index) * sizeof(bluetooth->table[0]));
		return;
	}
}

/*
 * Takes the answer to the request's call: a single call ends the request;
 * a pairing's Pair is followed by Trusted and Connect, and its result is
 * the pairing's, whatever Connect answered.
 */
static void
bt_request_answer(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *reply,
	const char *error_name,
	const char *error_text)
{
	char reason[KL_BACKEND_BT_REASON_MAX];
	int error;

	(void)reply;

	/* What the error means for this request and step. */
	error = 0;
	reason[0] = '\0';
	if (error_name != NULL) {
		error = bt_errno(error_name, error_text, bluetooth->request_kind, bluetooth->request_step);
		bt_reason(error_name, error_text, reason, sizeof(reason));
	}

	/* Which step answered. */
	switch (bluetooth->request_step) {
	case BT_STEP_PAIR:
		/* The pairing's question about this device is over either way. */
		bt_pair_question_end(bluetooth);

		/* A pairing that failed is the request's end; a code the agent would not type is what this computer cannot do. */
		if (error != 0) {
			if (bluetooth->input_rejected) {
				error = ENOTSUP;
				bt_copy_text(reason, sizeof(reason), "input-unsupported");
			}
			bt_request_finish(bluetooth, error, reason);
			return;
		}

		/* Paired: trusted next, so that the device may connect again by itself. */
		error = bt_request_send(bluetooth, BT_STEP_TRUST);
		if (error == 0)
			return;

		/* Not sent: connected without it. */
		error = bt_request_send(bluetooth, BT_STEP_CONNECT);
		if (error != 0)
			bt_request_finish(bluetooth, 0, "not-connected");
		return;
	case BT_STEP_TRUST:
		/* A trust refused is told in the log; the pairing stands. */
		if (error_name != NULL)
			fprintf(stderr, "wayland: bluetooth: trust %s: %s\n", bluetooth->request_path, error_name);

		/* Connected next, as zedBSD's pairing takes the link over. */
		error = bt_request_send(bluetooth, BT_STEP_CONNECT);
		if (error != 0)
			bt_request_finish(bluetooth, 0, "not-connected");
		return;
	case BT_STEP_CONNECT:
		/* The pairing succeeded; a connection that failed is said in the log and the reason. */
		if (error_name != NULL)
			fprintf(stderr, "wayland: bluetooth: connect after pairing %s: %s\n", bluetooth->request_path, error_name);
		if (error == 0)
			reason[0] = '\0';
		bt_request_finish(bluetooth, 0, reason);
		return;
	default:
		/* A single call's answer is the request's. */
		bt_request_finish(bluetooth, error, reason);
		return;
	}
}

/*
 * Ends the request with its result (kept for the caller), and reads the
 * adapter again at once.
 */
static void
bt_request_finish(
	struct kl_backend_bluetooth *bluetooth,
	int error,
	const char *reason)
{
	struct bt_result *result;

	/* Only a request going on. */
	if (bluetooth->request_kind == 0U)
		return;

	/* A Pair's question is over with it, as zedBSD's ends with its request (the window goes). */
	if (bluetooth->request_kind == KL_BACKEND_BT_PAIR && bluetooth->request_step == BT_STEP_PAIR)
		bt_pair_question_end(bluetooth);

	/* Room (the oldest answer goes when they pile up). */
	if (bluetooth->result_count == BT_RESULTS_MAX) {
		bluetooth->result_count--;
		memmove(&bluetooth->results[0], &bluetooth->results[1], bluetooth->result_count * sizeof(bluetooth->results[0]));
	}

	/* The answer, last. */
	result = &bluetooth->results[bluetooth->result_count];
	memset(result, 0, sizeof(*result));
	result->id = bluetooth->request_id;
	result->error = error;
	bt_copy_text(result->reason, sizeof(result->reason), reason);
	bluetooth->result_count++;
	bluetooth->changed |= KL_BACKEND_BT_CHANGED_RESULT;
	fprintf(stderr, "wayland: bluetooth: request %u kind=%u error=%d reason=%s\n", bluetooth->request_id, bluetooth->request_kind, error, reason);

	/* Succeeded: no request, and the state read again now. */
	bluetooth->request_kind = 0U;
	bluetooth->request_step = BT_STEP_NONE;
	bluetooth->adapter_due_ms = 0U;
}

/*
 * Ends this program's request at once with its result: a Pair still
 * running is cancelled at BlueZ, and the request's call outstanding is
 * answered to no one (its late answer is dropped).
 */
static void
bt_request_give_up(
	struct kl_backend_bluetooth *bluetooth,
	int error,
	const char *reason)
{
	uint32_t serial;
	size_t index;

	/* Pair is cancelled while it runs; a pairing's later steps are only dropped. */
	if (bluetooth->request_step == BT_STEP_PAIR) {
		(void)dbus_send_call(&bluetooth->bus, LINUX_DBUS_NO_AUTO_START, BT_SERVICE, bluetooth->request_path, BT_DEVICE, "CancelPairing", "", NULL, 0, &serial);
	}

	/* The request's call outstanding is answered to no one. */
	for (index = 0; index < bluetooth->call_count; index++) {
		if (bluetooth->calls[index].kind == BT_CALL_REQUEST)
			bluetooth->calls[index].kind = BT_CALL_IGNORED;
	}

	/* Succeeded: ended, said as a result. */
	bt_request_finish(bluetooth, error, reason);
}

/*
 * Ends the question about the device of this program's pairing, if one is
 * asked now: the agent's call held for it is refused as cancelled (BlueZ
 * may not cancel it itself), and its END is kept, which closes the window.
 */
static void
bt_pair_question_end(
	struct kl_backend_bluetooth *bluetooth)
{
	int same;

	/* Only a question asked about the pairing's device. */
	if (bluetooth->question_id == 0U)
		return;
	same = strcmp(bluetooth->question_path, bluetooth->request_path);
	if (same != 0)
		return;

	/* Succeeded: the call held let go, and the question ended. */
	bt_agent_release_pending(bluetooth, "org.bluez.Error.Canceled");
	bt_question_end(bluetooth);
}

/*
 * Sends the call of a step of the request: the power's Set, Forget's
 * RemoveDevice, Connect, Disconnect, or a pairing's Pair, Trusted and
 * Connect.  Returns 0, EBUSY when no call can be sent now, or ENOTCONN.
 */
static int
bt_request_send(
	struct kl_backend_bluetooth *bluetooth,
	unsigned step)
{
	struct dbus_arg arguments[3];
	const char *member;
	int error;

	/* The request's step, whatever is sent. */
	bluetooth->request_step = step;
	memset(arguments, 0, sizeof(arguments));

	/* A pairing's steps. */
	if (step == BT_STEP_PAIR) {
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->request_path, BT_SERVICE, bluetooth->request_path, BT_DEVICE, "Pair", "", NULL, 0, BT_PAIR_MS);
	} else if (step == BT_STEP_TRUST) {
		arguments[0].string = BT_DEVICE;
		arguments[1].string = "Trusted";
		arguments[2].variant = "b";
		arguments[2].number = 1U;
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->request_path, BT_SERVICE, bluetooth->request_path, BT_PROPERTIES, "Set", "ssv", arguments, 3, BT_CALL_MS);
	} else if (step == BT_STEP_CONNECT) {
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->request_path, BT_SERVICE, bluetooth->request_path, BT_DEVICE, "Connect", "", NULL, 0, BT_CALL_MS);
	} else if (bluetooth->request_kind == KL_BACKEND_BT_POWER_ON || bluetooth->request_kind == KL_BACKEND_BT_POWER_OFF) {
		/* The adapter's switch. */
		arguments[0].string = BT_ADAPTER;
		arguments[1].string = "Powered";
		arguments[2].variant = "b";
		arguments[2].number = 0U;
		if (bluetooth->request_kind == KL_BACKEND_BT_POWER_ON)
			arguments[2].number = 1U;
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->adapter_path, BT_SERVICE, bluetooth->adapter_path, BT_PROPERTIES, "Set", "ssv", arguments, 3, BT_CALL_MS);
	} else if (bluetooth->request_kind == KL_BACKEND_BT_FORGET) {
		/* The device and its keys, removed through the adapter. */
		arguments[0].string = bluetooth->request_path;
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->request_path, BT_SERVICE, bluetooth->adapter_path, BT_ADAPTER, "RemoveDevice", "o", arguments, 1, BT_CALL_MS);
	} else {
		/* A connection made or ended. */
		member = "Connect";
		if (bluetooth->request_kind == KL_BACKEND_BT_DISCONNECT)
			member = "Disconnect";
		error = bt_send(bluetooth, BT_CALL_REQUEST, bluetooth->request_path, BT_SERVICE, bluetooth->request_path, BT_DEVICE, member, "", NULL, 0, BT_CALL_MS);
	}

	/* A full table is a busy moment; anything else is a lost connection. */
	if (error == EBUSY)
		return EBUSY;
	if (error != 0)
		return ENOTCONN;

	/* Succeeded: the step's answer to come. */
	return 0;
}

/*
 * Says whether an error means the object or its interface does not exist
 * (any more): unknown object, unknown method, or invalid arguments.
 */
static int
bt_error_is_unknown(
	const char *error_name)
{
	int same;

	/* No such object. */
	same = strcmp(error_name, "org.freedesktop.DBus.Error.UnknownObject");
	if (same == 0)
		return 1;

	/* No such method on it, or no such interface to read. */
	same = strcmp(error_name, "org.freedesktop.DBus.Error.UnknownMethod");
	if (same == 0)
		return 1;
	same = strcmp(error_name, "org.freedesktop.DBus.Error.InvalidArgs");
	if (same == 0)
		return 1;

	/* Another error. */
	return 0;
}

/*
 * Gives the errno value of a BlueZ or bus error for a request and its
 * step; what the request wanted already being so is 0.  The values mean
 * what zedBSD's bluetoothd's reasons mean (bluetooth-zedbsd.c): a pairing
 * the device or the user refused is ECONNREFUSED ("rejected"), a device
 * that did not answer ETIMEDOUT ("timeout"), and EACCES only a caller the
 * system does not permit ("permission").  A connection to a device that
 * has no profile this computer uses (BlueZ's NotAvailable,
 * br-connection-profile-unavailable) is ENOTSUP.
 */
static int
bt_errno(
	const char *error_name,
	const char *error_text,
	unsigned request,
	unsigned step)
{
	static const struct {
		const char *name;
		int error;
	} table[] = {
		{ "org.bluez.Error.AuthenticationFailed", ECONNREFUSED },
		{ "org.bluez.Error.AuthenticationRejected", ECONNREFUSED },
		{ "org.bluez.Error.AuthenticationCanceled", ECONNREFUSED },
		{ "org.bluez.Error.Rejected", ECONNREFUSED },
		{ "org.bluez.Error.Canceled", ECONNREFUSED },
		{ "org.bluez.Error.NotAuthorized", EACCES },
		{ "org.freedesktop.DBus.Error.AccessDenied", EACCES },
		{ "org.bluez.Error.InProgress", EBUSY },
		{ "org.bluez.Error.AlreadyExists", EBUSY },
		{ "org.bluez.Error.Busy", EBUSY },
		{ "org.bluez.Error.AuthenticationTimeout", ETIMEDOUT },
		{ "org.freedesktop.DBus.Error.NoReply", ETIMEDOUT },
		{ "org.freedesktop.DBus.Error.Timeout", ETIMEDOUT },
		{ "org.bluez.Error.ConnectionAttemptFailed", ECONNREFUSED },
		{ "org.bluez.Error.Failed", ECONNREFUSED },
		{ "org.bluez.Error.NotAvailable", ECONNREFUSED },
		{ "org.bluez.Error.NotReady", ENETDOWN },
		{ "org.bluez.Error.DoesNotExist", EINVAL },
		{ "org.bluez.Error.InvalidArguments", EINVAL },
		{ "org.freedesktop.DBus.Error.UnknownObject", EINVAL },
		{ "org.freedesktop.DBus.Error.UnknownMethod", EINVAL },
		{ "org.freedesktop.DBus.Error.InvalidArgs", EINVAL },
		{ "org.bluez.Error.NotSupported", ENOTSUP },
		{ "org.freedesktop.DBus.Error.ServiceUnknown", ENOTCONN },
		{ "org.freedesktop.DBus.Error.NameHasNoOwner", ENOTCONN }
	};
	size_t index;
	int connecting;
	int timeout;
	int same;

	/* Whether the call connects (Connect, or the one after a pairing). */
	connecting = 0;
	if (request == KL_BACKEND_BT_CONNECT || step == BT_STEP_CONNECT)
		connecting = 1;

	/* Connecting what is connected, ending what is not, pairing what is paired: done. */
	same = strcmp(error_name, "org.bluez.Error.AlreadyConnected");
	if (same == 0 && connecting)
		return 0;
	same = strcmp(error_name, "org.bluez.Error.NotConnected");
	if (same == 0 && request == KL_BACKEND_BT_DISCONNECT)
		return 0;
	same = strcmp(error_name, "org.bluez.Error.AlreadyExists");
	if (same == 0 && step == BT_STEP_PAIR)
		return 0;

	/* A device with nothing this computer connects to: not a refusal. */
	same = strcmp(error_name, "org.bluez.Error.NotAvailable");
	if (same == 0 && connecting)
		return ENOTSUP;

	/* A failure whose text says the device did not answer (br-connection-page-timeout, Page Timeout). */
	timeout = bt_says_timeout(error_text);
	same = strcmp(error_name, "org.bluez.Error.Failed");
	if (same == 0 && timeout)
		return ETIMEDOUT;
	same = strcmp(error_name, "org.bluez.Error.ConnectionAttemptFailed");
	if (same == 0 && timeout)
		return ETIMEDOUT;

	/* The table. */
	for (index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
		same = strcmp(error_name, table[index].name);
		if (same == 0)
			return table[index].error;
	}

	/* Anything else, LimitsExceeded among them. */
	return EIO;
}

/* Tells whether an error's text says a timeout (in any case). */
static int
bt_says_timeout(
	const char *text)
{
	size_t length;
	size_t index;
	int same;

	/* Each place the word may start. */
	length = strlen(text);
	for (index = 0; index + 7U <= length; index++) {
		same = strncasecmp(&text[index], "timeout", 7U);
		if (same == 0)
			return 1;
	}

	/* Not said. */
	return 0;
}

/*
 * Puts the reason of an error in words: the last word of its name, or for
 * BlueZ's plain Failed its text (br-connection-page-timeout and the like),
 * cut to the reason's length.
 */
static void
bt_reason(
	const char *error_name,
	const char *error_text,
	char *reason,
	size_t size)
{
	const char *word;
	int same;

	/* Failed says what failed in its text. */
	same = strcmp(error_name, "org.bluez.Error.Failed");
	if (same == 0 && error_text[0] != '\0') {
		bt_copy_text(reason, size, error_text);
		return;
	}

	/* The last word of the name. */
	word = strrchr(error_name, '.');
	if (word == NULL)
		word = error_name;
	else
		word++;
	bt_copy_text(reason, size, word);
}

/*
 * Takes a call to the agent from BlueZ: checks who calls and what, then
 * shows a number, asks a question (holding the call for its answer), or
 * refuses what this agent cannot do.
 */
static void
bt_agent_call(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *call)
{
	struct bt_device *device;
	const char *member;
	const char *path;
	const char *text;
	uint32_t number;
	int from_owner;
	int error;
	int same;

	/* Only the present owner of org.bluez speaks to the agent. */
	from_owner = 0;
	if (bluetooth->owner[0] != '\0' && call->sender != NULL) {
		same = strcmp(call->sender, bluetooth->owner);
		if (same == 0)
			from_owner = 1;
	}
	if (!from_owner) {
		(void)dbus_send_error(&bluetooth->bus, call, "org.freedesktop.DBus.Error.AccessDenied", "Only BlueZ calls this agent");
		return;
	}

	/* Only the agent's own object and interface (a call may leave its interface out). */
	same = strcmp(call->path, BT_AGENT_PATH);
	if (same == 0 && call->interface != NULL)
		same = strcmp(call->interface, BT_AGENT);
	if (same != 0) {
		(void)dbus_send_error(&bluetooth->bus, call, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
		return;
	}
	member = call->member;

	/* BlueZ let the agent go: registered again later. */
	same = strcmp(member, "Release");
	if (same == 0) {
		bluetooth->agent_state = BT_AGENT_NONE;
		bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_RETRY_MS;
		(void)dbus_send_return(&bluetooth->bus, call, "", NULL, 0);
		return;
	}

	/* BlueZ gave up the request it asked: its question ends. */
	same = strcmp(member, "Cancel");
	if (same == 0) {
		bt_agent_release_pending(bluetooth, "org.bluez.Error.Canceled");
		bt_question_end(bluetooth);
		(void)dbus_send_return(&bluetooth->bus, call, "", NULL, 0);
		return;
	}

	/* Every other method names the device first. */
	error = dbus_read_string(call, &path);
	if (error != 0) {
		(void)dbus_send_error(&bluetooth->bus, call, "org.bluez.Error.Rejected", "Malformed call");
		return;
	}

	/* A number to compare on both sides. */
	same = strcmp(member, "RequestConfirmation");
	if (same == 0) {
		error = dbus_read_number(call, &number);
		if (error != 0)
			number = 0U;
		bt_agent_hold(bluetooth, call, KL_BACKEND_BT_ASK_CONFIRM, number, path);
		return;
	}

	/* A pairing without a number to compare: agreed or not. */
	same = strcmp(member, "RequestAuthorization");
	if (same == 0) {
		bt_agent_hold(bluetooth, call, KL_BACKEND_BT_ASK_CONSENT, 0U, path);
		return;
	}

	/* A service of a device: trusted ones at once, HID of a paired one asked, the rest refused. */
	same = strcmp(member, "AuthorizeService");
	if (same == 0) {
		device = bt_find_path(bluetooth, path);
		error = dbus_read_string(call, &text);
		if (device == NULL || error != 0 || !device->paired) {
			(void)dbus_send_error(&bluetooth->bus, call, "org.bluez.Error.Rejected", "Not paired");
			return;
		}

		/* A trusted device's services need no question. */
		if (device->trusted) {
			(void)dbus_send_return(&bluetooth->bus, call, "", NULL, 0);
			return;
		}

		/* HID or HID over GATT is asked; another service is refused. */
		same = strcasecmp(text, BT_HID_UUID);
		if (same != 0)
			same = strcasecmp(text, BT_HOGP_UUID);
		if (same == 0) {
			bt_agent_hold(bluetooth, call, KL_BACKEND_BT_ASK_CONSENT, 0U, path);
			return;
		}
		(void)dbus_send_error(&bluetooth->bus, call, "org.bluez.Error.Rejected", "Service not allowed");
		return;
	}

	/* A number to type on the device, shown once (BlueZ tells each key typed again). */
	same = strcmp(member, "DisplayPasskey");
	if (same == 0) {
		error = dbus_read_number(call, &number);
		same = strcmp(bluetooth->question_path, path);
		if (bluetooth->question_id != 0U && bluetooth->question_kind == KL_BACKEND_BT_ASK_PASSKEY && same == 0)
			error = EEXIST;
		if (error == 0)
			bt_question_push(bluetooth, KL_BACKEND_BT_ASK_PASSKEY, number, path);
		(void)dbus_send_return(&bluetooth->bus, call, "", NULL, 0);
		return;
	}

	/* A PIN to type on an old keyboard: shown when it is six digits, as a number. */
	same = strcmp(member, "DisplayPinCode");
	if (same == 0) {
		error = dbus_read_string(call, &text);
		if (error == 0)
			error = bt_six_digits(text, &number);
		if (error == 0) {
			bt_question_push(bluetooth, KL_BACKEND_BT_ASK_PASSKEY, number, path);
		} else {
			/* A PIN that is not a number of six digits has no window to show it. */
			fprintf(stderr, "wayland: bluetooth: PIN code not shown for %s\n", path);
		}
		(void)dbus_send_return(&bluetooth->bus, call, "", NULL, 0);
		return;
	}

	/* A code to type here: this agent types nothing (phase.md section 9.8). */
	same = strcmp(member, "RequestPinCode");
	if (same != 0)
		same = strcmp(member, "RequestPasskey");
	if (same == 0) {
		bluetooth->input_rejected = 1U;
		fprintf(stderr, "wayland: bluetooth: %s refused for %s (input-unsupported)\n", member, path);
		(void)dbus_send_error(&bluetooth->bus, call, "org.bluez.Error.Rejected", "input-unsupported");
		return;
	}

	/* No other method. */
	(void)dbus_send_error(&bluetooth->bus, call, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
}

/*
 * Holds an agent's call for the user's answer and asks the question; a
 * second call while one is held is refused.
 */
static void
bt_agent_hold(
	struct kl_backend_bluetooth *bluetooth,
	struct dbus_reply *call,
	unsigned kind,
	uint32_t number,
	const char *path)
{
	/* One question at a time. */
	if (bluetooth->pending) {
		(void)dbus_send_error(&bluetooth->bus, call, "org.bluez.Error.Rejected", "Another question is open");
		return;
	}

	/* The call, by its caller and serial, answered later. */
	bt_copy_text(bluetooth->pending_sender, sizeof(bluetooth->pending_sender), call->sender);
	bluetooth->pending_serial = call->serial;
	bluetooth->pending_ms = bt_milliseconds();
	bluetooth->pending = 1U;

	/* Succeeded: the question for the user. */
	bt_question_push(bluetooth, kind, number, path);
}

/* Answers the agent's call held, if any, with an error, and holds it no more. */
static void
bt_agent_release_pending(
	struct kl_backend_bluetooth *bluetooth,
	const char *error_name)
{
	/* Nothing held. */
	if (!bluetooth->pending)
		return;

	/* Refused (a broken connection takes it with it anyway). */
	bluetooth->pending = 0U;
	(void)dbus_send_error_to(&bluetooth->bus, bluetooth->pending_sender, bluetooth->pending_serial, error_name, "No answer");
}

/*
 * Asks a question of the user about a device: numbered, with the
 * device's address, type and name, this user, and whether this program's
 * own pairing asks it.
 */
static void
bt_question_push(
	struct kl_backend_bluetooth *bluetooth,
	unsigned kind,
	uint32_t number,
	const char *path)
{
	struct kl_backend_bluetooth_question *question;
	struct bt_device *device;
	struct passwd *account;
	int same;

	/* Room (the oldest goes). */
	if (bluetooth->question_count == BT_QUESTIONS_MAX) {
		bluetooth->question_count--;
		memmove(&bluetooth->questions[0], &bluetooth->questions[1], bluetooth->question_count * sizeof(bluetooth->questions[0]));
	}

	/* The question, numbered; it is the one asked now until its end. */
	question = &bluetooth->questions[bluetooth->question_count];
	memset(question, 0, sizeof(*question));
	question->id = bluetooth->next_question;
	bluetooth->next_question++;
	if (bluetooth->next_question == 0U)
		bluetooth->next_question = 1U;
	question->kind = kind;
	question->number = number;
	bluetooth->question_id = question->id;
	bluetooth->question_kind = kind;
	bt_copy_text(bluetooth->question_path, sizeof(bluetooth->question_path), path);

	/* The device, as far as known (its path's tail otherwise). */
	device = bt_find_path(bluetooth, path);
	if (device != NULL) {
		bt_copy_text(question->address, sizeof(question->address), device->address);
		question->type = KL_BACKEND_BT_BREDR;
		if (device->random)
			question->type = KL_BACKEND_BT_LE_RANDOM;
		else if (!device->has_class && device->has_appearance)
			question->type = KL_BACKEND_BT_LE_PUBLIC;
		bt_copy_text(question->name, sizeof(question->name), device->address);
		if (device->has_name && device->alias[0] != '\0')
			bt_copy_text(question->name, sizeof(question->name), device->alias);
	} else {
		bt_copy_text(question->name, sizeof(question->name), path);
	}

	/* Who asks: this user; this program's own when its pairing is of that device. */
	account = getpwuid(getuid());
	if (account != NULL)
		bt_copy_text(question->user, sizeof(question->user), account->pw_name);
	same = strcmp(bluetooth->request_path, path);
	if (bluetooth->request_kind == KL_BACKEND_BT_PAIR && same == 0)
		question->own = 1U;

	/* Succeeded: kept until taken. */
	bluetooth->question_count++;
	bluetooth->changed |= KL_BACKEND_BT_CHANGED_QUESTION;
	fprintf(stderr, "wayland: bluetooth: question %u kind=%u own=%u\n", question->id, kind, question->own);
}

/* Ends the question asked now, if any, with an END of its id. */
static void
bt_question_end(
	struct kl_backend_bluetooth *bluetooth)
{
	struct kl_backend_bluetooth_question *question;
	uint32_t id;

	/* Only a question asked. */
	id = bluetooth->question_id;
	bluetooth->question_id = 0U;
	bluetooth->question_path[0] = '\0';
	if (id == 0U)
		return;

	/* Room (the oldest goes). */
	if (bluetooth->question_count == BT_QUESTIONS_MAX) {
		bluetooth->question_count--;
		memmove(&bluetooth->questions[0], &bluetooth->questions[1], bluetooth->question_count * sizeof(bluetooth->questions[0]));
	}

	/* Succeeded: the END of that id. */
	question = &bluetooth->questions[bluetooth->question_count];
	memset(question, 0, sizeof(*question));
	question->id = id;
	question->kind = KL_BACKEND_BT_ASK_END;
	bluetooth->question_count++;
	bluetooth->changed |= KL_BACKEND_BT_CHANGED_QUESTION;
}

/*
 * Builds what the caller sees from the following, and notes what changed:
 * the state at once, the devices at once unless only a signal strength
 * changed, which is told every two seconds at most.
 */
static void
bt_export(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	struct kl_backend_bluetooth_state state;
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	struct kl_backend_bluetooth_device without[KL_BACKEND_BT_DEVICES_MAX];
	struct kl_backend_bluetooth_device before[KL_BACKEND_BT_DEVICES_MAX];
	const struct bt_device *order[BT_TABLE_MAX];
	const struct bt_device *device;
	struct kl_backend_bluetooth_device *shown;
	size_t chosen;
	size_t count;
	size_t index;
	int differs;

	/* The state. */
	memset(&state, 0, sizeof(state));
	state.state = KL_BACKEND_BT_ABSENT;
	if (bluetooth->open && bluetooth->owner[0] != '\0') {
		state.reachable = 1U;
		state.features = KL_BACKEND_BT_CAN_POWER | KL_BACKEND_BT_CAN_CONNECT;
		if (bluetooth->agent_state == BT_AGENT_HELD)
			state.agent = 1U;
		if (bluetooth->request_kind == KL_BACKEND_BT_PAIR)
			state.pairing = 1U;
		state.state = KL_BACKEND_BT_NONE;

		/* The adapter's: its power's state where told, else its switch. */
		if (bluetooth->adapter) {
			state.state = KL_BACKEND_BT_OFF;
			if (bluetooth->powered)
				state.state = KL_BACKEND_BT_ON;
			if (bluetooth->power_state != 0U)
				state.state = bluetooth->power_state;
			state.power = bluetooth->powered;
			state.scanning = bluetooth->discovering;
			bt_copy_text(state.address, sizeof(state.address), bluetooth->adapter_address);
			bt_copy_text(state.name, sizeof(state.name), bluetooth->adapter_alias);
		}
	}

	/* The devices whose address is known: the paired first, then the strongest. */
	count = 0U;
	for (index = 0; index < bluetooth->table_count; index++) {
		if (bluetooth->table[index].address[0] == '\0')
			continue;
		order[count] = &bluetooth->table[index];
		count++;
	}
	qsort(order, count, sizeof(order[0]), bt_select_order);
	chosen = count;
	if (chosen > KL_BACKEND_BT_DEVICES_MAX)
		chosen = KL_BACKEND_BT_DEVICES_MAX;

	/* Each chosen device as the caller sees it. */
	memset(devices, 0, sizeof(devices));
	for (index = 0; index < chosen; index++) {
		device = order[index];
		shown = &devices[index];
		bt_copy_text(shown->address, sizeof(shown->address), device->address);
		shown->type = KL_BACKEND_BT_BREDR;
		if (device->random)
			shown->type = KL_BACKEND_BT_LE_RANDOM;
		else if (!device->has_class && device->has_appearance)
			shown->type = KL_BACKEND_BT_LE_PUBLIC;
		bt_copy_text(shown->name, sizeof(shown->name), device->address);
		if (device->has_name && device->alias[0] != '\0')
			bt_copy_text(shown->name, sizeof(shown->name), device->alias);
		shown->kind = device->kind;
		shown->paired = bt_paired(device);
		shown->legacy = device->legacy;
		shown->connected = device->connected;
		shown->battery = device->battery;
		shown->rssi = device->rssi;
	}

	/* Shown with the paired first, then by name. */
	qsort(devices, chosen, sizeof(devices[0]), bt_show_order);

	/* The state, told when it changed. */
	differs = memcmp(&state, &bluetooth->state, sizeof(state));
	if (differs != 0) {
		bluetooth->state = state;
		bluetooth->changed |= KL_BACKEND_BT_CHANGED_STATE;
	}

	/* The devices are compared without their signal first. */
	memcpy(without, devices, sizeof(without));
	memcpy(before, bluetooth->devices, sizeof(before));
	for (index = 0; index < KL_BACKEND_BT_DEVICES_MAX; index++) {
		without[index].rssi = 0;
		before[index].rssi = 0;
	}
	differs = 0;
	if (chosen != bluetooth->count)
		differs = 1;
	if (!differs)
		differs = memcmp(without, before, sizeof(without));

	/* A change beyond the signal is told now; a change of the signal alone when its wait is over. */
	if (differs == 0) {
		differs = memcmp(devices, bluetooth->devices, sizeof(devices));
		if (differs != 0 && now < bluetooth->rssi_due_ms)
			return;
	}
	if (differs == 0)
		return;

	/* Succeeded: the devices told, and the signal's next wait. */
	memcpy(bluetooth->devices, devices, sizeof(devices));
	bluetooth->count = chosen;
	bluetooth->rssi_due_ms = now + BT_RSSI_MS;
	bluetooth->changed |= KL_BACKEND_BT_CHANGED_DEVICES;
}

/* Orders devices for the choice of those shown: the paired first, then the strongest signal. */
static int
bt_select_order(
	const void *left,
	const void *right)
{
	const struct bt_device *first;
	const struct bt_device *second;
	unsigned first_paired;
	unsigned second_paired;

	/* The two devices. */
	first = *(const struct bt_device *const *)left;
	second = *(const struct bt_device *const *)right;
	first_paired = bt_paired(first);
	second_paired = bt_paired(second);

	/* Paired before not. */
	if (first_paired != second_paired) {
		if (first_paired)
			return -1;
		return 1;
	}

	/* A stronger signal (closer to zero; zero is unknown and weakest) first. */
	if (first->rssi != second->rssi) {
		if (first->rssi == 0)
			return 1;
		if (second->rssi == 0)
			return -1;
		if (first->rssi > second->rssi)
			return -1;
		return 1;
	}

	/* Otherwise by path, so that the order is stable. */
	return strcmp(first->path, second->path);
}

/* Orders the devices shown: the paired first, then by name, then by address. */
static int
bt_show_order(
	const void *left,
	const void *right)
{
	const struct kl_backend_bluetooth_device *first;
	const struct kl_backend_bluetooth_device *second;
	int order;

	/* The two devices. */
	first = left;
	second = right;

	/* Paired before not. */
	if (first->paired != second->paired) {
		if (first->paired)
			return -1;
		return 1;
	}

	/* By name. */
	order = strcmp(first->name, second->name);
	if (order != 0)
		return order;

	/* By address. */
	order = strcmp(first->address, second->address);
	return order;
}

/* Gives the kind of a device from BlueZ's icon name. */
static unsigned
bt_kind(
	const char *icon)
{
	int same;

	/* A keyboard. */
	same = strcmp(icon, "input-keyboard");
	if (same == 0)
		return KL_BACKEND_BT_KIND_KEYBOARD;

	/* A mouse, or a tablet that points. */
	same = strcmp(icon, "input-mouse");
	if (same == 0)
		return KL_BACKEND_BT_KIND_MOUSE;
	same = strcmp(icon, "input-tablet");
	if (same == 0)
		return KL_BACKEND_BT_KIND_MOUSE;

	/* Audio of any kind. */
	same = strncmp(icon, "audio-", 6);
	if (same == 0)
		return KL_BACKEND_BT_KIND_AUDIO;

	/* A phone, a computer. */
	same = strcmp(icon, "phone");
	if (same == 0)
		return KL_BACKEND_BT_KIND_PHONE;
	same = strcmp(icon, "computer");
	if (same == 0)
		return KL_BACKEND_BT_KIND_COMPUTER;

	/* Anything else. */
	return KL_BACKEND_BT_KIND_OTHER;
}

/*
 * Gives the controller's state from BlueZ's PowerState: on (and turning
 * off, still on), turning on, or off (blocked by rfkill too).
 */
static unsigned
bt_power_state(
	const char *power_state)
{
	int same;

	/* On, or on until it has turned off. */
	same = strcmp(power_state, "on");
	if (same == 0)
		return KL_BACKEND_BT_ON;
	same = strcmp(power_state, "on-disabling");
	if (same == 0)
		return KL_BACKEND_BT_ON;

	/* Turning on. */
	same = strcmp(power_state, "off-enabling");
	if (same == 0)
		return KL_BACKEND_BT_STARTING;

	/* Off, by the user or by rfkill. */
	return KL_BACKEND_BT_OFF;
}

/* Says whether a device is paired: BlueZ says Paired, or Bonded for keys kept from before. */
static unsigned
bt_paired(
	const struct bt_device *device)
{
	/* Paired now. */
	if (device->paired)
		return 1U;

	/* Its keys kept. */
	if (device->bonded)
		return 1U;

	/* Neither. */
	return 0U;
}

/* Gives a Boolean property's value as 0 or 1. */
static unsigned
bt_flag(
	const struct bt_value *value)
{
	/* True. */
	if (value->number != 0U)
		return 1U;

	/* False. */
	return 0U;
}

/*
 * Copies a text into a buffer of size bytes, cut at a character's start
 * (UTF-8) when it does not fit, and always ended.
 */
static void
bt_copy_text(
	char *destination,
	size_t size,
	const char *source)
{
	size_t length;

	/* Nowhere to copy. */
	if (size == 0U)
		return;

	/* The length that fits. */
	length = strlen(source);
	if (length >= size) {
		length = size - 1U;

		/* A character cut in its middle goes whole. */
		while (length > 0U && ((unsigned char)source[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The text and its end. */
	memmove(destination, source, length);
	destination[length] = '\0';
}

/* Reads a PIN of exactly six digits as a number.  Returns 0, or EINVAL. */
static int
bt_six_digits(
	const char *text,
	uint32_t *number)
{
	size_t index;

	/* Six digits and nothing more. */
	*number = 0U;
	for (index = 0; index < 6U; index++) {
		if (text[index] < '0' || text[index] > '9')
			return EINVAL;
		*number = *number * 10U + (uint32_t)(text[index] - '0');
	}
	if (text[6] != '\0')
		return EINVAL;

	/* Succeeded: the number. */
	return 0;
}
