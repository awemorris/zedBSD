/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bluetooth on zedBSD (ws143-p006, plan/ws143/phase006/phase.md section 3):
 * bluetoothd's text lines on /run/bluetoothd.sock (plan/ws143/phase004
 * section 5, phase005 section 5).
 *
 * Three conversations at most, each a connection of its own and none
 * waited for: the reading (SHOW, BONDS, DEVICES and STATUS, one after the
 * other, every two seconds while someone watches and after each answer,
 * otherwise every thirty), the request (POWER, PAIR, FORGET, CONNECT or
 * DISCONNECT, one at a time, answered up to its DONE) and the scan (SCAN
 * of a few seconds, again and again while the devices around are looked
 * for).  Beside them the agent: a connection that said AGENT and hears a
 * pairing's questions (CONFIRM, CONSENT, PASSKEY), answered YES or NO; a
 * pairing asked without the agent gets its questions on the request's own
 * connection.  bluetoothd keeps eight clients; this uses four.
 *
 * bluetoothd tells no change by itself (no SUBSCRIBE, Future Work F-086),
 * so the state is read again.  A daemon without POWER or without
 * CONNECT, DISCONNECT and STATUS (it answers ERROR request) is shown
 * without them.  A daemon that is not running is not a failure: the state
 * is unreachable and is read again every second.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/base/bluetoothd/protocol.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* bluetoothd's socket (a host test builds with another path). */
#ifndef BT_SOCKET_PATH
#define BT_SOCKET_PATH		BTD_SOCKET
#endif

/* The waits: before connecting again, between readings while watched and not, before the agent is tried again, between scans. */
#define BT_RETRY_MS		1000U
#define BT_WATCH_MS		2000U
#define BT_IDLE_MS		30000U
#define BT_AGENT_RETRY_MS	30000U
#define BT_AGENT_AGAIN_MS	5000U
#define BT_SCAN_AGAIN_MS	2000U
#define BT_SCAN_FAILED_MS	5000U

/* How long a device a scan saw stays in the list after it was last seen (milliseconds). */
#define BT_SEEN_MS		30000U

/* The longest line read (a name of 249 bytes escaped is 996), the answers and the questions kept. */
#define BT_INPUT_MAX		2048U
#define BT_RESULTS_MAX		8U
#define BT_QUESTIONS_MAX	4U

/* The steps of the reading. */
#define BT_STEP_NONE		0U
#define BT_STEP_SHOW		1U
#define BT_STEP_BONDS		2U
#define BT_STEP_DEVICES		3U
#define BT_STEP_STATUS		4U

/* The connections, as bt_read names them to bt_line. */
#define BT_LINK_READING		1U
#define BT_LINK_REQUEST		2U
#define BT_LINK_SCAN		3U
#define BT_LINK_AGENT		4U

/* Who waits for the answer to the question asked last. */
#define BT_ASKED_NONE		0U
#define BT_ASKED_AGENT		1U
#define BT_ASKED_REQUEST	2U

/* send's flag that keeps a broken connection from raising SIGPIPE, where there is one. */
#ifdef MSG_NOSIGNAL
#define BT_SEND_FLAGS		(MSG_DONTWAIT | MSG_NOSIGNAL)
#else
#define BT_SEND_FLAGS		MSG_DONTWAIT
#endif

/* One conversation with bluetoothd: the connection (-1 while none) and the bytes of a line not ended yet. */
struct bt_link {
	int socket;
	char input[BT_INPUT_MAX];
	size_t used;
};

/* One answer to a request: its number, its errno value and bluetoothd's reason. */
struct bt_result {
	uint32_t id;
	int error;
	char reason[KL_BACKEND_BT_REASON_MAX];
};

/* One device a scan saw, and when it was last seen (it stays a while after a new scan starts). */
struct bt_seen {
	struct kl_backend_bluetooth_device device;
	uint64_t seen_ms;
};

/*
 * The following of bluetoothd: the state and the devices of the last whole
 * reading, the reading going on (its step, what it has read so far, when
 * the next is due), whether someone watches and scans, the request going
 * on (its number, kind and device, how it ended so far), the scan and the
 * agent, which of them waits for an answer, what the daemon cannot do, and
 * the answers and questions not taken yet.
 */
struct kl_backend_bluetooth {
	/* The last whole reading. */
	struct kl_backend_bluetooth_state state;
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	size_t count;

	/* The reading going on. */
	struct bt_link reading;
	unsigned step;
	struct kl_backend_bluetooth_state next_state;
	struct kl_backend_bluetooth_device next[KL_BACKEND_BT_DEVICES_MAX];
	size_t next_count;
	uint64_t due_ms;

	/* Whether someone watches, and whether the devices around are looked for. */
	unsigned watching;
	unsigned scanning;

	/* The request going on. */
	struct bt_link request;
	uint32_t request_id;
	uint32_t next_id;
	unsigned request_kind;
	char request_address[KL_BACKEND_BT_ADDRESS_MAX];
	int request_error;
	char request_reason[KL_BACKEND_BT_REASON_MAX];

	/* The scan going on, and when the next may start. */
	struct bt_link scan;
	uint64_t scan_due_ms;

	/*
	 * The agent: its connection, when to try it again, whether it holds
	 * the role (not when another program of the user took it: it is not
	 * taken back, but asked again before a pairing of this program's),
	 * who waits for the answer to the question asked now, the question's
	 * id, and the next id.
	 */
	struct bt_link agent;
	uint64_t agent_due_ms;
	unsigned agent_held;
	unsigned asked;
	uint32_t question_id;
	uint32_t next_question;

	/* A request asked while a scan runs, sent when the scan ends (the daemon refuses it meanwhile). */
	char waiting_line[96];
	unsigned waiting;

	/* The devices scans saw, kept a while; and how many lines the reading had (none: the daemon was full). */
	struct bt_seen seen[KL_BACKEND_BT_DEVICES_MAX];
	size_t seen_count;
	unsigned reading_lines;

	/* What the daemon answered it cannot do (KL_BACKEND_BT_CAN_* bits), and whether its SHOW has the switch. */
	unsigned missing;
	unsigned power_known;

	/* The answers and the questions not taken yet. */
	struct bt_result results[BT_RESULTS_MAX];
	size_t result_count;
	struct kl_backend_bluetooth_question questions[BT_QUESTIONS_MAX];
	size_t question_count;
};

static uint64_t bt_milliseconds(void);
static int bt_connect(struct bt_link *link, const char *line);
static void bt_close(struct bt_link *link);
static int bt_send(struct bt_link *link, const char *line);
static int bt_read(struct bt_link *link, struct kl_backend_bluetooth *bluetooth, unsigned which, unsigned *changed);
static void bt_line(struct kl_backend_bluetooth *bluetooth, unsigned which, char *line, unsigned *changed);
static void bt_reading_start(struct kl_backend_bluetooth *bluetooth, uint64_t now, unsigned *changed);
static void bt_reading_line(struct kl_backend_bluetooth *bluetooth, char *line, unsigned *changed);
static void bt_reading_end(struct kl_backend_bluetooth *bluetooth, unsigned *changed);
static void bt_request_line(struct kl_backend_bluetooth *bluetooth, char *line, unsigned *changed);
static void bt_request_end(struct kl_backend_bluetooth *bluetooth, unsigned *changed);
static void bt_agent_line(struct kl_backend_bluetooth *bluetooth, char *line, unsigned *changed);
static void bt_scan_line(struct kl_backend_bluetooth *bluetooth, char *line);
static int bt_question_line(struct kl_backend_bluetooth *bluetooth, const char *line, unsigned asked, unsigned *changed);
static void bt_question_push(struct kl_backend_bluetooth *bluetooth, unsigned kind, uint32_t number, const char *line, unsigned *changed);
static void bt_question_end(struct kl_backend_bluetooth *bluetooth, unsigned *changed);
static void bt_seen_merge(struct kl_backend_bluetooth *bluetooth, uint64_t now);
static int bt_send_request(struct kl_backend_bluetooth *bluetooth, const char *line);
static void bt_unreachable(struct kl_backend_bluetooth *bluetooth, unsigned *changed);
static void bt_state_word(struct kl_backend_bluetooth_state *state, const char *word);
static void bt_device_line(struct kl_backend_bluetooth *bluetooth, const char *line, unsigned source);
static struct kl_backend_bluetooth_device *bt_device_find(struct kl_backend_bluetooth *bluetooth, const char *address, unsigned type);
static int bt_field(const char *line, const char *name, char *value, size_t size);
static void bt_unescape(const char *text, char *output, size_t size);
static unsigned bt_type(const char *word);
static const char *bt_type_name(unsigned type);
static unsigned bt_kind(unsigned long class_of_device, unsigned long appearance);
static int bt_error(const char *reason);
static void bt_commit(struct kl_backend_bluetooth *bluetooth, unsigned *changed);
static int bt_address_valid(const char *address);

/*
 * Starts following bluetoothd; one that is not running yet is reached by
 * a later update.  Returns NULL only without memory.
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
	bluetooth->reading.socket = -1;
	bluetooth->request.socket = -1;
	bluetooth->scan.socket = -1;
	bluetooth->agent.socket = -1;

	/* Unreachable until the first reading; that reading at once. */
	bluetooth->state.state = KL_BACKEND_BT_ABSENT;
	bluetooth->next_id = 1U;
	bluetooth->next_question = 1U;

	/* Succeeded: the first update reads the state. */
	return bluetooth;
}

/*
 * Stops following: every connection closes, which ends a request or a
 * question outstanding (bluetoothd takes a pairing's lost client as a no).
 */
void
kl_backend_bluetooth_close(
	struct kl_backend_bluetooth *bluetooth)
{
	/* Nothing to close. */
	if (bluetooth == NULL)
		return;

	/* Each connection, then the record. */
	bt_close(&bluetooth->reading);
	bt_close(&bluetooth->request);
	bt_close(&bluetooth->scan);
	bt_close(&bluetooth->agent);
	free(bluetooth);
}

/*
 * Reads what bluetoothd has sent on each connection, and starts what is
 * due: a reading, a scan, the agent.
 */
int
kl_backend_bluetooth_update(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	uint64_t now;
	int error;

	/* A record and somewhere to say what changed. */
	if (bluetooth == NULL || changed == NULL)
		return EINVAL;
	*changed = 0U;
	now = bt_milliseconds();

	/* What has come on each connection. */
	(void)bt_read(&bluetooth->reading, bluetooth, BT_LINK_READING, changed);
	(void)bt_read(&bluetooth->request, bluetooth, BT_LINK_REQUEST, changed);
	(void)bt_read(&bluetooth->scan, bluetooth, BT_LINK_SCAN, changed);
	(void)bt_read(&bluetooth->agent, bluetooth, BT_LINK_AGENT, changed);

	/* A reading that is due. */
	if (bluetooth->reading.socket < 0 && now >= bluetooth->due_ms)
		bt_reading_start(bluetooth, now, changed);

	/* The agent, while the daemon answers. */
	if (bluetooth->agent.socket < 0 && bluetooth->state.reachable && now >= bluetooth->agent_due_ms) {
		error = bt_connect(&bluetooth->agent, "AGENT\n");
		if (error != 0)
			bluetooth->agent_due_ms = now + BT_AGENT_AGAIN_MS;
	}

	/* A scan, while one is wanted and nothing stops it (Bluetooth off, a pairing, a request going on). */
	if (bluetooth->scanning &&
	    bluetooth->scan.socket < 0 &&
	    bluetooth->state.reachable &&
	    bluetooth->state.state == KL_BACKEND_BT_ON &&
	    !bluetooth->state.pairing &&
	    bluetooth->request.socket < 0 &&
	    !bluetooth->waiting &&
	    now >= bluetooth->scan_due_ms) {
		error = bt_connect(&bluetooth->scan, "SCAN 6\n");
		if (error != 0)
			bluetooth->scan_due_ms = now + BT_SCAN_FAILED_MS;
	}

	/*
	 * Answers and questions not taken yet are told again (a pairing given
	 * up by kl_backend_bluetooth_cancel ends between two updates).
	 */
	if (bluetooth->result_count > 0U)
		*changed |= KL_BACKEND_BT_CHANGED_RESULT;
	if (bluetooth->question_count > 0U)
		*changed |= KL_BACKEND_BT_CHANGED_QUESTION;

	/* Succeeded. */
	return 0;
}

/*
 * Copies the state of the last whole reading.
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
 * Copies the devices of the last whole reading, the paired first.
 */
size_t
kl_backend_bluetooth_get_devices(
	const struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_device *devices,
	size_t capacity)
{
	size_t count;

	/* As many as fit. */
	if (bluetooth == NULL || devices == NULL)
		return 0U;
	count = bluetooth->count;
	if (count > capacity)
		count = capacity;
	memcpy(devices, bluetooth->devices, count * sizeof(devices[0]));

	/* Succeeded: how many there are. */
	return bluetooth->count;
}

/*
 * Reads the state often while someone shows it; a watch that starts reads
 * at once.
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
		bluetooth->due_ms = 0U;
	bluetooth->watching = 0U;
	if (on)
		bluetooth->watching = 1U;
}

/*
 * Looks for the devices around while scanning is 1; a scan going on ends
 * by itself.
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
		bluetooth->scan_due_ms = 0U;
	bluetooth->scanning = 0U;
	if (on)
		bluetooth->scanning = 1U;
}

/*
 * Sends a request on a connection of its own; its answer comes as a
 * result numbered *id.
 */
int
kl_backend_bluetooth_request(
	struct kl_backend_bluetooth *bluetooth,
	unsigned request,
	const char *address,
	unsigned type,
	uint32_t *id)
{
	char line[96];
	const char *verb;
	int valid;
	int error;

	/* A record, a daemon, and no request going on. */
	if (bluetooth == NULL || id == NULL)
		return EINVAL;
	if (!bluetooth->state.reachable)
		return ENOTCONN;
	if (bluetooth->request.socket >= 0)
		return EBUSY;

	/* The power: what the daemon can do. */
	if (request == KL_BACKEND_BT_POWER_ON || request == KL_BACKEND_BT_POWER_OFF) {
		if ((bluetooth->missing & KL_BACKEND_BT_CAN_POWER) != 0U)
			return ENOTSUP;
		verb = "POWER on";
		if (request == KL_BACKEND_BT_POWER_OFF)
			verb = "POWER off";
		(void)snprintf(line, sizeof(line), "%s\n", verb);
	} else {
		/* A device's request: its verb. */
		switch (request) {
		case KL_BACKEND_BT_PAIR:
		case KL_BACKEND_BT_PAIR_PHONE:
			verb = "PAIR";
			break;
		case KL_BACKEND_BT_FORGET:
			verb = "FORGET";
			break;
		case KL_BACKEND_BT_CONNECT:
			verb = "CONNECT";
			break;
		case KL_BACKEND_BT_DISCONNECT:
			verb = "DISCONNECT";
			break;
		default:
			return EINVAL;
		}

		/* Connecting is what an older daemon cannot do. */
		if ((request == KL_BACKEND_BT_CONNECT || request == KL_BACKEND_BT_DISCONNECT) &&
		    (bluetooth->missing & KL_BACKEND_BT_CAN_CONNECT) != 0U)
			return ENOTSUP;

		/* The device, as written in the lines. */
		if (address == NULL || type > KL_BACKEND_BT_LE_RANDOM)
			return EINVAL;
		valid = bt_address_valid(address);
		if (!valid)
			return EINVAL;
		(void)snprintf(line, sizeof(line), "%s %s %s\n", verb, address, bt_type_name(type));

		/* A phone of the user's is paired over BR/EDR, as the user's (ws197-p004 section 5.1). */
		if (request == KL_BACKEND_BT_PAIR_PHONE) {
			if (type != KL_BACKEND_BT_BREDR)
				return EINVAL;
			(void)snprintf(line, sizeof(line), "%s %s %s phone=1\n", verb, address, bt_type_name(type));
		}
	}

	/* A request already waiting for the scan to end. */
	if (bluetooth->waiting)
		return EBUSY;

	/*
	 * This program's pairing asks to be the agent again first (another
	 * program of the user may have taken it): its AGENT goes before the
	 * PAIR, so that the pairing's questions come to it.
	 */
	if ((request == KL_BACKEND_BT_PAIR || request == KL_BACKEND_BT_PAIR_PHONE) && bluetooth->agent.socket < 0) {
		error = bt_connect(&bluetooth->agent, "AGENT\n");
		bluetooth->agent_due_ms = 0U;
		if (error == 0)
			bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_AGAIN_MS;
	}

	/* The line now, or when the scan going on ends (the daemon refuses it meanwhile). */
	if (bluetooth->scan.socket >= 0) {
		(void)snprintf(bluetooth->waiting_line, sizeof(bluetooth->waiting_line), "%s", line);
		bluetooth->waiting = 1U;
	} else {
		error = bt_send_request(bluetooth, line);
		if (error != 0)
			return ENOTCONN;
	}

	/* Succeeded: numbered, its answer to come. */
	bluetooth->request_id = bluetooth->next_id;
	bluetooth->next_id++;
	if (bluetooth->next_id == 0U)
		bluetooth->next_id = 1U;
	bluetooth->request_kind = request;
	bluetooth->request_address[0] = '\0';
	if (address != NULL)
		(void)snprintf(bluetooth->request_address, sizeof(bluetooth->request_address), "%s", address);
	bluetooth->request_error = 0;
	bluetooth->request_reason[0] = '\0';
	*id = bluetooth->request_id;
	return 0;
}

/*
 * Takes the oldest answer to a request.
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

	/* The oldest, then the others move up. */
	*id = bluetooth->results[0].id;
	*error = bluetooth->results[0].error;
	if (reason != NULL && size > 0U)
		(void)snprintf(reason, size, "%s", bluetooth->results[0].reason);
	bluetooth->result_count--;
	memmove(&bluetooth->results[0], &bluetooth->results[1], bluetooth->result_count * sizeof(bluetooth->results[0]));

	/* Succeeded: one answer. */
	return 1;
}

/*
 * Takes the oldest question of a pairing.
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
 * Answers the question of an id, on the connection it came on; an answer
 * to a question that is over (or not the one asked now) is not sent.
 */
int
kl_backend_bluetooth_answer(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t id,
	unsigned yes)
{
	struct bt_link *link;
	const char *line;
	int error;

	/* The question asked now, of that id. */
	if (bluetooth == NULL || bluetooth->asked == BT_ASKED_NONE || bluetooth->question_id != id)
		return ENOENT;

	/* The connection it came on. */
	link = &bluetooth->agent;
	if (bluetooth->asked == BT_ASKED_REQUEST)
		link = &bluetooth->request;
	bluetooth->asked = BT_ASKED_NONE;

	/* YES or NO. */
	line = "NO\n";
	if (yes)
		line = "YES\n";
	error = bt_send(link, line);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Gives up this program's own pairing: its connection closes, which the
 * daemon takes as the pairing cancelled (its answer is the result).
 */
int
kl_backend_bluetooth_cancel(
	struct kl_backend_bluetooth *bluetooth)
{
	unsigned changed;

	/* Only a pairing of this program's going on. */
	if (bluetooth == NULL)
		return ENOENT;
	if (bluetooth->request_kind != KL_BACKEND_BT_PAIR && bluetooth->request_kind != KL_BACKEND_BT_PAIR_PHONE)
		return ENOENT;

	/* Its connection, and its end as a result (cancelled). */
	changed = 0;
	bt_close(&bluetooth->request);
	bluetooth->waiting = 0U;
	(void)snprintf(bluetooth->request_reason, sizeof(bluetooth->request_reason), "%s", "cancelled");
	bluetooth->request_error = ECANCELED;
	bt_request_end(bluetooth, &changed);

	/* Succeeded: given up. */
	return 0;
}

/* Gives the monotonic clock in milliseconds. */
static uint64_t
bt_milliseconds(
	void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Connects to bluetoothd and writes a line; 0, or an errno value with no connection. */
static int
bt_connect(
	struct bt_link *link,
	const char *line)
{
	struct sockaddr_un address;
	int descriptor;
	int status;
	int error;

	/* The connection. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", BT_SOCKET_PATH);
	status = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		status = errno;
		(void)close(descriptor);
		return status;
	}

	/* Kept, nothing read yet. */
	link->socket = descriptor;
	link->used = 0U;

	/* The line. */
	error = bt_send(link, line);
	if (error != 0) {
		bt_close(link);
		return error;
	}

	/* Succeeded: connected and asked. */
	return 0;
}

/* Closes a connection (none is fine). */
static void
bt_close(
	struct bt_link *link)
{
	/* Only an open one. */
	if (link->socket < 0)
		return;
	(void)close(link->socket);
	link->socket = -1;
	link->used = 0U;
}

/* Writes a short line whole; 0, or EPIPE when it could not. */
static int
bt_send(
	struct bt_link *link,
	const char *line)
{
	ssize_t sent;
	size_t length;

	/* A connection. */
	if (link->socket < 0)
		return EPIPE;

	/* The line, at once (bluetoothd reads it at once; it is short). */
	length = strlen(line);
	sent = send(link->socket, line, length, BT_SEND_FLAGS);
	if (sent < 0 || (size_t)sent != length)
		return EPIPE;

	/* Succeeded: written. */
	return 0;
}

/*
 * Reads what has come on a connection and gives each whole line to its
 * conversation; a connection that ended or broke ends its conversation.
 * Returns 1 when the connection is still open.
 */
static int
bt_read(
	struct bt_link *link,
	struct kl_backend_bluetooth *bluetooth,
	unsigned which,
	unsigned *changed)
{
	ssize_t count;
	size_t length;
	char *end;

	/* Every byte there now, line by line. */
	while (link->socket >= 0) {
		count = recv(link->socket, link->input + link->used, sizeof(link->input) - 1U - link->used, MSG_DONTWAIT);
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;

		/* The end of the connection: its conversation ends. */
		if (count <= 0) {
			bt_close(link);
			bt_line(bluetooth, which, NULL, changed);
			break;
		}

		/* Each whole line; a conversation that closes its connection stops the loop. */
		link->used += (size_t)count;
		link->input[link->used] = '\0';
		for (;;) {
			end = strchr(link->input, '\n');
			if (end == NULL || link->socket < 0)
				break;
			*end = '\0';
			length = (size_t)(end + 1 - link->input);
			bt_line(bluetooth, which, link->input, changed);
			if (link->socket < 0)
				break;
			memmove(link->input, end + 1, link->used - length + 1U);
			link->used -= length;
		}

		/* A line longer than any is a broken connection. */
		if (link->socket >= 0 && link->used >= sizeof(link->input) - 1U) {
			bt_close(link);
			bt_line(bluetooth, which, NULL, changed);
		}
	}

	/* Succeeded: whether it is still open. */
	if (link->socket < 0)
		return 0;
	return 1;
}

/* Gives a line (NULL: the connection ended) to its conversation. */
static void
bt_line(
	struct kl_backend_bluetooth *bluetooth,
	unsigned which,
	char *line,
	unsigned *changed)
{
	/* The conversation by the connection. */
	switch (which) {
	case BT_LINK_READING:
		/* The reading: a line, or its end before DONE (the daemon went). */
		if (line != NULL) {
			bluetooth->reading_lines++;
			bt_reading_line(bluetooth, line, changed);
		} else if (bluetooth->reading_lines == 0U) {
			/* Closed before any line: the daemon had no room for another client; the state stays, read again soon. */
			bluetooth->step = BT_STEP_NONE;
			bluetooth->due_ms = bt_milliseconds() + BT_RETRY_MS;
		} else {
			bt_unreachable(bluetooth, changed);
		}

		/* Taken. */
		break;
	case BT_LINK_REQUEST:
		/* The request: a line, or its end. */
		if (line != NULL) {
			bt_request_line(bluetooth, line, changed);
		} else {
			bt_request_end(bluetooth, changed);
		}

		/* Taken. */
		break;
	case BT_LINK_AGENT:
		/* The agent: a question, or its end (tried again a little later). */
		if (line != NULL) {
			bt_agent_line(bluetooth, line, changed);
		} else {
			if (bluetooth->asked == BT_ASKED_AGENT)
				bt_question_end(bluetooth, changed);

			/* Tried again a little later. */
			bluetooth->agent_held = 0U;
			bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_AGAIN_MS;
		}

		/* Taken. */
		break;
	default:
		/* The scan: its end reads the state again. */
		if (line != NULL) {
			bt_scan_line(bluetooth, line);
		} else {
			bluetooth->due_ms = 0U;
			bluetooth->scan_due_ms = bt_milliseconds() + BT_SCAN_AGAIN_MS;
			if (bluetooth->waiting) {
				bluetooth->waiting = 0U;
				(void)bt_send_request(bluetooth, bluetooth->waiting_line);
			}
		}

		/* Taken. */
		break;
	}
}

/* Starts a reading with SHOW; a daemon that does not answer is unreachable, tried again in a second. */
static void
bt_reading_start(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now,
	unsigned *changed)
{
	int error;

	/* The next try, should this one fail. */
	bluetooth->due_ms = now + BT_RETRY_MS;

	/* The connection and SHOW. */
	error = bt_connect(&bluetooth->reading, "SHOW\n");
	if (error != 0) {
		bt_unreachable(bluetooth, changed);
		return;
	}

	/* Succeeded: the reading starts from nothing. */
	memset(&bluetooth->next_state, 0, sizeof(bluetooth->next_state));
	bluetooth->next_state.reachable = 1U;
	bluetooth->next_state.state = KL_BACKEND_BT_NONE;
	bluetooth->next_state.power = 1U;
	bluetooth->next_count = 0U;
	bluetooth->step = BT_STEP_SHOW;
	bluetooth->reading_lines = 0U;
}

/* Takes one line of the reading; DONE moves to the next step, or ends it. */
static void
bt_reading_line(
	struct kl_backend_bluetooth *bluetooth,
	char *line,
	unsigned *changed)
{
	char value[KL_BACKEND_BT_NAME_MAX * 4U];
	int unknown;
	int found;
	int same;
	int off;
	int yes;

	/* The state: its word. */
	same = strncmp(line, "STATE ", 6U);
	if (same == 0) {
		bt_state_word(&bluetooth->next_state, line + 6);
		return;
	}

	/* The controller: its address, its name, and whether a pairing runs. */
	same = strncmp(line, "CONTROLLER ", 11U);
	if (same == 0) {
		found = bt_field(line, "address=", value, sizeof(value));
		unknown = strcmp(value, "-");
		if (found && unknown != 0)
			(void)snprintf(bluetooth->next_state.address, sizeof(bluetooth->next_state.address), "%s", value);
		found = bt_field(line, "name=", value, sizeof(value));
		if (found)
			bt_unescape(value, bluetooth->next_state.name, sizeof(bluetooth->next_state.name));
		found = bt_field(line, "pairing=", value, sizeof(value));
		yes = strcmp(value, "1");
		if (found && yes == 0)
			bluetooth->next_state.pairing = 1U;
		return;
	}

	/* The user's switch (a daemon without POWER has no such line). */
	same = strncmp(line, "POWER ", 6U);
	if (same == 0) {
		off = strcmp(line + 6, "off");
		bluetooth->next_state.power = 1U;
		if (off == 0)
			bluetooth->next_state.power = 0U;
		bluetooth->power_known = 1U;
		return;
	}

	/* A bond, a device seen, a connected device. */
	same = strncmp(line, "BOND ", 5U);
	if (same == 0) {
		bt_device_line(bluetooth, line, BT_STEP_BONDS);
		return;
	}

	/* A device the last scan saw. */
	same = strncmp(line, "DEVICE ", 7U);
	if (same == 0) {
		bt_device_line(bluetooth, line, BT_STEP_DEVICES);
		return;
	}

	/* A device connected for its input. */
	same = strncmp(line, "HID ", 4U);
	if (same == 0) {
		bt_device_line(bluetooth, line, BT_STEP_STATUS);
		return;
	}

	/* A request the daemon does not know: STATUS (no connecting yet). */
	same = strcmp(line, "ERROR request");
	if (same == 0 && bluetooth->step == BT_STEP_STATUS) {
		bluetooth->missing |= KL_BACKEND_BT_CAN_CONNECT;
		return;
	}

	/* Anything but DONE is not the reading's. */
	same = strcmp(line, "DONE");
	if (same != 0)
		return;

	/* DONE: the next step, on the same connection. */
	switch (bluetooth->step) {
	case BT_STEP_SHOW:
		bluetooth->step = BT_STEP_BONDS;
		(void)bt_send(&bluetooth->reading, "BONDS\n");
		return;
	case BT_STEP_BONDS:
		bluetooth->step = BT_STEP_DEVICES;
		(void)bt_send(&bluetooth->reading, "DEVICES\n");
		return;
	case BT_STEP_DEVICES:
		/* STATUS only from a daemon that has it. */
		if ((bluetooth->missing & KL_BACKEND_BT_CAN_CONNECT) == 0U) {
			bluetooth->step = BT_STEP_STATUS;
			(void)bt_send(&bluetooth->reading, "STATUS\n");
			return;
		}

		/* Without STATUS the reading is whole. */
		break;
	default:
		break;
	}

	/* Succeeded: the reading is whole. */
	bt_reading_end(bluetooth, changed);
}

/* Ends a whole reading: it becomes the state, and the next is due. */
static void
bt_reading_end(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	uint64_t wait;

	/* The connection goes; the next reading after the wait. */
	bt_close(&bluetooth->reading);
	bluetooth->step = BT_STEP_NONE;
	wait = BT_IDLE_MS;
	if (bluetooth->watching)
		wait = BT_WATCH_MS;
	bluetooth->due_ms = bt_milliseconds() + wait;

	/* What the daemon can do, as far as known (the switch only from one whose SHOW tells it), and whether this is the agent. */
	bluetooth->next_state.features = KL_BACKEND_BT_CAN_CONNECT & ~bluetooth->missing;
	if (bluetooth->power_known && (bluetooth->missing & KL_BACKEND_BT_CAN_POWER) == 0U)
		bluetooth->next_state.features |= KL_BACKEND_BT_CAN_POWER;
	bluetooth->next_state.agent = bluetooth->agent_held;

	/* The devices scans saw a while ago, still listed. */
	bt_seen_merge(bluetooth, bt_milliseconds());

	/* Succeeded: the state and the devices, told when they changed. */
	bt_commit(bluetooth, changed);
}

/* Takes one line of the request's answer; DONE ends it. */
static void
bt_request_line(
	struct kl_backend_bluetooth *bluetooth,
	char *line,
	unsigned *changed)
{
	int taken;
	int same;

	/* A question of a pairing asked without the agent: answered on this connection. */
	taken = bt_question_line(bluetooth, line, BT_ASKED_REQUEST, changed);
	if (taken)
		return;

	/* A refusal: why. */
	same = strncmp(line, "ERROR ", 6U);
	if (same == 0) {
		(void)snprintf(bluetooth->request_reason, sizeof(bluetooth->request_reason), "%s", line + 6);
		bluetooth->request_error = bt_error(line + 6);
		return;
	}

	/* The end. */
	same = strcmp(line, "DONE");
	if (same == 0)
		bt_request_end(bluetooth, changed);
}

/* Ends the request: its answer is kept, the question it asked is over, and the state is read again. */
static void
bt_request_end(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	struct bt_result *result;
	int error;
	int same;

	/* A request: one that ended before DONE failed. */
	if (bluetooth->request_kind == 0U)
		return;
	error = bluetooth->request_error;
	if (bluetooth->request.socket < 0 && error == 0 && bluetooth->request_reason[0] == '\0') {
		error = EIO;
		(void)snprintf(bluetooth->request_reason, sizeof(bluetooth->request_reason), "%s", "lost");
	}

	/* The connection goes. */
	bt_close(&bluetooth->request);

	/* A daemon without POWER or connecting says so once. */
	same = strcmp(bluetooth->request_reason, "request");
	if (same == 0 && (bluetooth->request_kind == KL_BACKEND_BT_POWER_ON || bluetooth->request_kind == KL_BACKEND_BT_POWER_OFF))
		bluetooth->missing |= KL_BACKEND_BT_CAN_POWER;
	if (same == 0 && (bluetooth->request_kind == KL_BACKEND_BT_CONNECT || bluetooth->request_kind == KL_BACKEND_BT_DISCONNECT))
		bluetooth->missing |= KL_BACKEND_BT_CAN_CONNECT;

	/* The question it asked is over. */
	if (bluetooth->asked == BT_ASKED_REQUEST || bluetooth->question_id != 0U)
		bt_question_end(bluetooth, changed);

	/* The answer kept (the oldest is lost when they pile up). */
	if (bluetooth->result_count == BT_RESULTS_MAX) {
		bluetooth->result_count--;
		memmove(&bluetooth->results[0], &bluetooth->results[1], bluetooth->result_count * sizeof(bluetooth->results[0]));
	}

	/* This one, last. */
	result = &bluetooth->results[bluetooth->result_count];
	result->id = bluetooth->request_id;
	result->error = error;
	(void)snprintf(result->reason, sizeof(result->reason), "%s", bluetooth->request_reason);
	bluetooth->result_count++;
	*changed |= KL_BACKEND_BT_CHANGED_RESULT;

	/* Succeeded: no request, and the state read again now. */
	bluetooth->request_kind = 0U;
	bluetooth->due_ms = 0U;
}

/* Takes one line of the agent: its acceptance, a refusal (tried again later), a question, its end, or the role taken by another. */
static void
bt_agent_line(
	struct kl_backend_bluetooth *bluetooth,
	char *line,
	unsigned *changed)
{
	int taken;
	int same;

	/* A question, or a question that is over. */
	taken = bt_question_line(bluetooth, line, BT_ASKED_AGENT, changed);
	if (taken)
		return;

	/* The role held. */
	same = strcmp(line, "AGENT ok");
	if (same == 0) {
		bluetooth->agent_held = 1U;
		return;
	}

	/* Refused (another user's agent, or not permitted): tried again in a while. */
	same = strncmp(line, "ERROR ", 6U);
	if (same == 0) {
		bt_close(&bluetooth->agent);
		bluetooth->agent_held = 0U;
		bluetooth->agent_due_ms = bt_milliseconds() + BT_AGENT_RETRY_MS;
		return;
	}

	/*
	 * Another program of the user took the role (bt agent in a terminal):
	 * it is not taken back now (the two would take it from each other), but
	 * asked for again before a pairing of this program's.
	 */
	same = strcmp(line, "AGENT-END");
	if (same == 0) {
		bt_close(&bluetooth->agent);
		bluetooth->agent_held = 0U;
		bluetooth->agent_due_ms = UINT64_MAX;
		if (bluetooth->asked == BT_ASKED_AGENT)
			bt_question_end(bluetooth, changed);
		bluetooth->due_ms = 0U;
	}
}

/* Takes one line of a scan: its DONE (or ERROR) ends it; the devices are read by the reading. */
static void
bt_scan_line(
	struct kl_backend_bluetooth *bluetooth,
	char *line)
{
	uint64_t soon;
	int same;

	/* A refusal waits longer before the next scan. */
	same = strncmp(line, "ERROR ", 6U);
	if (same == 0) {
		bluetooth->scan_due_ms = bt_milliseconds() + BT_SCAN_FAILED_MS;
		return;
	}

	/* The end: the connection goes, the state is read now, the next scan soon. */
	same = strcmp(line, "DONE");
	if (same != 0)
		return;
	bt_close(&bluetooth->scan);
	bluetooth->due_ms = 0U;
	soon = bt_milliseconds() + BT_SCAN_AGAIN_MS;
	if (bluetooth->scan_due_ms < soon)
		bluetooth->scan_due_ms = soon;

	/* A request that waited for the scan goes now. */
	if (bluetooth->waiting) {
		bluetooth->waiting = 0U;
		(void)bt_send_request(bluetooth, bluetooth->waiting_line);
	}
}

/*
 * Takes a question line (CONFIRM, CONSENT, PASSKEY, each naming the device
 * and who started the pairing) or ASK-END on a connection; 1 when it was
 * one.
 */
static int
bt_question_line(
	struct kl_backend_bluetooth *bluetooth,
	const char *line,
	unsigned asked,
	unsigned *changed)
{
	unsigned long number;
	char *end;
	int same;

	/* The question asked is over. */
	same = strcmp(line, "ASK-END");
	if (same == 0) {
		bt_question_end(bluetooth, changed);
		return 1;
	}

	/* A number to compare (the other side shows it too). */
	same = strncmp(line, "CONFIRM ", 8U);
	if (same == 0) {
		number = strtoul(line + 8, &end, 10);
		if (end == line + 8 || number > 999999UL)
			return 1;
		bluetooth->asked = asked;
		bt_question_push(bluetooth, KL_BACKEND_BT_ASK_CONFIRM, (uint32_t)number, line, changed);
		return 1;
	}

	/* Agreeing to pair at all. */
	same = strncmp(line, "CONSENT", 7U);
	if (same == 0 && (line[7] == '\0' || line[7] == ' ')) {
		bluetooth->asked = asked;
		bt_question_push(bluetooth, KL_BACKEND_BT_ASK_CONSENT, 0U, line, changed);
		return 1;
	}

	/* A number to type on the device (nothing to answer). */
	same = strncmp(line, "PASSKEY ", 8U);
	if (same == 0) {
		number = strtoul(line + 8, &end, 10);
		if (end == line + 8 || number > 999999UL)
			return 1;
		bt_question_push(bluetooth, KL_BACKEND_BT_ASK_PASSKEY, (uint32_t)number, line, changed);
		return 1;
	}

	/* Not a question. */
	return 0;
}

/*
 * Keeps a new question, numbered: the device it names (its name when it
 * is known, else the address), and who started the pairing (the user's
 * name, and whether it is this program's user's own pairing).  The oldest
 * goes when they pile up.
 */
static void
bt_question_push(
	struct kl_backend_bluetooth *bluetooth,
	unsigned kind,
	uint32_t number,
	const char *line,
	unsigned *changed)
{
	struct kl_backend_bluetooth_question *question;
	struct passwd *account;
	char value[KL_BACKEND_BT_NAME_MAX];
	unsigned long uid;
	uid_t self;
	size_t index;
	int found;
	int same;

	/* Room. */
	if (bluetooth->question_count == BT_QUESTIONS_MAX) {
		bluetooth->question_count--;
		memmove(&bluetooth->questions[0], &bluetooth->questions[1], bluetooth->question_count * sizeof(bluetooth->questions[0]));
	}

	/* The question, numbered (the id stays the one asked now until its end). */
	question = &bluetooth->questions[bluetooth->question_count];
	memset(question, 0, sizeof(*question));
	question->id = bluetooth->next_question;
	bluetooth->next_question++;
	if (bluetooth->next_question == 0U)
		bluetooth->next_question = 1U;
	question->kind = kind;
	question->number = number;
	bluetooth->question_id = question->id;

	/* The device it names (the request's when an older daemon names none). */
	found = bt_field(line, "address=", question->address, sizeof(question->address));
	if (!found)
		(void)snprintf(question->address, sizeof(question->address), "%s", bluetooth->request_address);
	found = bt_field(line, "type=", value, sizeof(value));
	if (found)
		question->type = bt_type(value);
	memcpy(question->name, question->address, sizeof(question->address));
	for (index = 0; index < bluetooth->count; index++) {
		same = strcmp(bluetooth->devices[index].address, question->address);
		if (same == 0)
			(void)snprintf(question->name, sizeof(question->name), "%s", bluetooth->devices[index].name);
	}

	/* Who started it: this program's user, or another user named. */
	question->own = 1U;
	found = bt_field(line, "uid=", value, sizeof(value));
	if (found) {
		uid = strtoul(value, NULL, 10);
		self = getuid();
		if ((uid_t)uid != self)
			question->own = 0U;
		account = getpwuid((uid_t)uid);
		if (account != NULL)
			(void)snprintf(question->user, sizeof(question->user), "%s", account->pw_name);
		else
			(void)snprintf(question->user, sizeof(question->user), "%lu", uid);
	}

	/* Succeeded: kept. */
	bluetooth->question_count++;
	*changed |= KL_BACKEND_BT_CHANGED_QUESTION;
}

/* Ends the question asked now: nobody waits for its answer, and an END of its id is kept. */
static void
bt_question_end(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	struct kl_backend_bluetooth_question *question;
	uint32_t id;

	/* Only a question asked. */
	id = bluetooth->question_id;
	bluetooth->asked = BT_ASKED_NONE;
	bluetooth->question_id = 0U;
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
	*changed |= KL_BACKEND_BT_CHANGED_QUESTION;
}

/*
 * Keeps the devices this reading's DEVICES listed as seen now, and lists
 * again those seen within BT_SEEN_MS that a new scan has not found yet
 * (a scan starts its table from nothing).
 */
static void
bt_seen_merge(
	struct kl_backend_bluetooth *bluetooth,
	uint64_t now)
{
	struct kl_backend_bluetooth_device *device;
	struct bt_seen kept[KL_BACKEND_BT_DEVICES_MAX];
	size_t kept_count;
	size_t index;
	size_t other;
	int same;

	/* The devices read now that a scan saw (not paired, not connected: only DEVICES lines give rssi or a kind). */
	kept_count = 0;
	for (index = 0; index < bluetooth->next_count && kept_count < KL_BACKEND_BT_DEVICES_MAX; index++) {
		device = &bluetooth->next[index];
		if (device->paired || device->connected)
			continue;
		kept[kept_count].device = *device;
		kept[kept_count].seen_ms = now;
		kept_count++;
	}

	/* Those seen before, not too long ago, not seen now: kept, and listed again. */
	for (index = 0; index < bluetooth->seen_count && kept_count < KL_BACKEND_BT_DEVICES_MAX; index++) {
		/* Too old. */
		if (now - bluetooth->seen[index].seen_ms > BT_SEEN_MS)
			continue;

		/* Seen now already. */
		same = 1;
		for (other = 0; other < kept_count; other++) {
			same = strcmp(kept[other].device.address, bluetooth->seen[index].device.address);
			if (same == 0)
				break;
		}

		/* Seen now already: kept once. */
		if (same == 0)
			continue;

		/* Kept, and listed again unless the reading has it (paired or connected). */
		kept[kept_count] = bluetooth->seen[index];
		kept_count++;
		device = bt_device_find(bluetooth, bluetooth->seen[index].device.address, bluetooth->seen[index].device.type);
		if (device != NULL && !device->paired && !device->connected)
			*device = bluetooth->seen[index].device;
	}

	/* Succeeded: the table for the next reading. */
	memcpy(bluetooth->seen, kept, kept_count * sizeof(kept[0]));
	bluetooth->seen_count = kept_count;
}

/* Sends a request's line on a connection of its own; 0, or an errno value. */
static int
bt_send_request(
	struct kl_backend_bluetooth *bluetooth,
	const char *line)
{
	unsigned changed;
	int error;

	/* The connection and the line. */
	error = bt_connect(&bluetooth->request, line);
	if (error == 0)
		return 0;

	/* A request that cannot be sent ends at once, failed. */
	changed = 0;
	(void)snprintf(bluetooth->request_reason, sizeof(bluetooth->request_reason), "%s", "lost");
	bluetooth->request_error = ENOTCONN;
	bt_request_end(bluetooth, &changed);
	return error;
}

/* The daemon does not answer: unreachable, no device, the agent dropped; the questions asked are over. */
static void
bt_unreachable(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	/* The reading goes, tried again in a second. */
	bt_close(&bluetooth->reading);
	bluetooth->step = BT_STEP_NONE;
	bluetooth->due_ms = bt_milliseconds() + BT_RETRY_MS;

	/* What it could do is not known any more. */
	bluetooth->missing = 0U;

	/* Succeeded: an unreachable state with no device, told when it changed. */
	memset(&bluetooth->next_state, 0, sizeof(bluetooth->next_state));
	bluetooth->next_state.state = KL_BACKEND_BT_ABSENT;
	bluetooth->next_count = 0U;
	bt_commit(bluetooth, changed);
}

/* Reads the state's word (and why) into a state. */
static void
bt_state_word(
	struct kl_backend_bluetooth_state *state,
	const char *word)
{
	static const struct {
		const char *word;
		unsigned state;
	} words[] = {
		{ "ready", KL_BACKEND_BT_ON },
		{ "scanning", KL_BACKEND_BT_ON },
		{ "off", KL_BACKEND_BT_OFF },
		{ "starting", KL_BACKEND_BT_STARTING },
		{ "firmware-needed", KL_BACKEND_BT_FIRMWARE },
		{ "firmware-failed", KL_BACKEND_BT_FIRMWARE },
		{ "unsupported", KL_BACKEND_BT_UNSUPPORTED },
		{ "none", KL_BACKEND_BT_NONE },
		{ "lost", KL_BACKEND_BT_NONE }
	};
	size_t length;
	size_t index;
	size_t size;
	int same;

	/* The word alone (a reason may follow it); an unknown one is an error. */
	length = strcspn(word, " ");
	state->state = KL_BACKEND_BT_ERROR;

	/* The word's state. */
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++) {
		/* Only a word of the same length and letters. */
		size = strlen(words[index].word);
		if (size != length)
			continue;
		same = strncmp(word, words[index].word, length);
		if (same != 0)
			continue;
		state->state = words[index].state;
		break;
	}

	/* Scanning is on, and scans. */
	same = strncmp(word, "scanning", 8U);
	if (same == 0 && length == 8U)
		state->scanning = 1U;
}

/*
 * Takes a device's line of the reading (BOND, DEVICE or HID) into the
 * devices being read: a new device, or more about one already there.
 */
static void
bt_device_line(
	struct kl_backend_bluetooth *bluetooth,
	const char *line,
	unsigned source)
{
	struct kl_backend_bluetooth_device *device;
	char address[KL_BACKEND_BT_ADDRESS_MAX];
	char value[KL_BACKEND_BT_NAME_MAX * 4U];
	char name[KL_BACKEND_BT_NAME_MAX];
	unsigned long class_of_device;
	unsigned long appearance;
	unsigned type;
	int unnamed;
	int found;
	int valid;
	int same;

	/* Its address and type. */
	found = bt_field(line, "address=", address, sizeof(address));
	if (!found)
		return;
	valid = bt_address_valid(address);
	if (!valid)
		return;
	found = bt_field(line, "type=", value, sizeof(value));
	if (!found)
		return;
	type = bt_type(value);

	/* The device, found or new (none when the list is full). */
	device = bt_device_find(bluetooth, address, type);
	if (device == NULL)
		return;

	/* Its name, when the line has one and the device had none. */
	found = bt_field(line, "name=", value, sizeof(value));
	if (found) {
		bt_unescape(value, name, sizeof(name));
		unnamed = strcmp(device->name, device->address);
		if (name[0] != '\0' && unnamed == 0)
			(void)snprintf(device->name, sizeof(device->name), "%s", name);
	}

	/* A bond: paired, and whether by the legacy way. */
	if (source == BT_STEP_BONDS) {
		device->paired = 1U;
		found = bt_field(line, "legacy=", value, sizeof(value));
		same = strcmp(value, "1");
		if (found && same == 0)
			device->legacy = 1U;
	}

	/* A device seen: its signal and what it is. */
	if (source == BT_STEP_DEVICES) {
		found = bt_field(line, "rssi=", value, sizeof(value));
		if (found)
			device->rssi = atoi(value);
		class_of_device = 0;
		appearance = 0;
		found = bt_field(line, "class=", value, sizeof(value));
		if (found)
			class_of_device = strtoul(value, NULL, 16);
		found = bt_field(line, "appearance=", value, sizeof(value));
		if (found)
			appearance = strtoul(value, NULL, 16);
		if (device->kind == KL_BACKEND_BT_KIND_OTHER)
			device->kind = bt_kind(class_of_device, appearance);
	}

	/* A connected device: open is connected, with its battery when told. */
	if (source == BT_STEP_STATUS) {
		found = bt_field(line, "state=", value, sizeof(value));
		same = strcmp(value, "open");
		if (found && same == 0)
			device->connected = 1U;
		found = bt_field(line, "battery=", value, sizeof(value));
		same = strcmp(value, "-");
		if (found && same != 0)
			device->battery = atoi(value);
	}
}

/* Finds a device being read by its address and type, or adds it (NULL when the list is full). */
static struct kl_backend_bluetooth_device *
bt_device_find(
	struct kl_backend_bluetooth *bluetooth,
	const char *address,
	unsigned type)
{
	struct kl_backend_bluetooth_device *device;
	size_t index;
	int same;

	/* One already there. */
	for (index = 0; index < bluetooth->next_count; index++) {
		device = &bluetooth->next[index];
		same = strcmp(device->address, address);
		if (same == 0 && device->type == type)
			return device;
	}

	/* No room. */
	if (bluetooth->next_count == KL_BACKEND_BT_DEVICES_MAX)
		return NULL;

	/* Succeeded: a new one, named by its address until a name comes. */
	device = &bluetooth->next[bluetooth->next_count];
	bluetooth->next_count++;
	memset(device, 0, sizeof(*device));
	(void)snprintf(device->address, sizeof(device->address), "%s", address);
	(void)snprintf(device->name, sizeof(device->name), "%s", address);
	device->type = type;
	device->battery = -1;
	return device;
}

/*
 * Finds a field "name=value" of a line: the value up to the next space,
 * or between the quotes of name="...".  Returns 1 with the value (as
 * written, still escaped), 0 when the line has none.
 */
static int
bt_field(
	const char *line,
	const char *name,
	char *value,
	size_t size)
{
	const char *at;
	const char *end;
	size_t length;

	/* The field, at the start of the line or after a space. */
	value[0] = '\0';
	at = line;
	for (;;) {
		at = strstr(at, name);
		if (at == NULL)
			return 0;
		if (at == line || at[-1] == ' ')
			break;
		at++;
	}

	/* The value, after the name. */
	at += strlen(name);

	/* A quoted value to its closing quote (a quote inside is escaped), else to the next space. */
	if (*at == '"') {
		at++;
		end = strchr(at, '"');
		if (end == NULL)
			return 0;
	} else {
		end = at + strcspn(at, " ");
	}

	/* The value, cut to the room there is. */
	length = (size_t)(end - at);
	if (length >= size)
		length = size - 1U;
	memcpy(value, at, length);
	value[length] = '\0';

	/* Succeeded: found. */
	return 1;
}

/* Turns bluetoothd's \xNN back into bytes; a control byte becomes '?'. */
static void
bt_unescape(
	const char *text,
	char *output,
	size_t size)
{
	unsigned long value;
	size_t used;
	char digits[3];
	char *end;

	/* Each byte, or each escape. */
	used = 0;
	while (*text != '\0' && used + 1U < size) {
		/* An escape of two hexadecimal digits. */
		if (text[0] == '\\' && text[1] == 'x' && text[2] != '\0' && text[3] != '\0') {
			digits[0] = text[2];
			digits[1] = text[3];
			digits[2] = '\0';
			value = strtoul(digits, &end, 16);
			if (end == digits + 2) {
				if (value < 0x20UL || value == 0x7fUL)
					value = '?';
				output[used++] = (char)value;
				text += 4;
				continue;
			}
		}

		/* A byte as it is. */
		output[used++] = *text;
		text++;
	}

	/* Succeeded: ended. */
	output[used] = '\0';
}

/* Gives the type an address type's word names. */
static unsigned
bt_type(
	const char *word)
{
	int same;

	/* LE's public address. */
	same = strcmp(word, "le-public");
	if (same == 0)
		return KL_BACKEND_BT_LE_PUBLIC;

	/* LE's random address. */
	same = strcmp(word, "le-random");
	if (same == 0)
		return KL_BACKEND_BT_LE_RANDOM;

	/* BR/EDR. */
	return KL_BACKEND_BT_BREDR;
}

/* Gives the word of an address type, as the requests write it. */
static const char *
bt_type_name(
	unsigned type)
{
	/* LE's two. */
	if (type == KL_BACKEND_BT_LE_PUBLIC)
		return "le-public";
	if (type == KL_BACKEND_BT_LE_RANDOM)
		return "le-random";

	/* BR/EDR. */
	return "bredr";
}

/*
 * Tells what a device is from its class of device (BR/EDR: the major
 * class, and a peripheral's minor class) or its appearance (LE).
 */
static unsigned
bt_kind(
	unsigned long class_of_device,
	unsigned long appearance)
{
	unsigned long major;
	unsigned long minor;
	unsigned long category;

	/* LE's appearance: a keyboard, a mouse, a phone, a computer, audio. */
	if (appearance == 0x03c1UL)
		return KL_BACKEND_BT_KIND_KEYBOARD;
	if (appearance == 0x03c2UL || appearance == 0x03c5UL)
		return KL_BACKEND_BT_KIND_MOUSE;
	category = appearance >> 6;
	if (category == 0x01UL)
		return KL_BACKEND_BT_KIND_PHONE;
	if (category == 0x02UL)
		return KL_BACKEND_BT_KIND_COMPUTER;
	if (category == 0x21UL || category == 0x25UL)
		return KL_BACKEND_BT_KIND_AUDIO;

	/* BR/EDR's class: the major class and a peripheral's kind. */
	major = (class_of_device >> 8) & 0x1fUL;
	minor = (class_of_device >> 6) & 0x03UL;
	if (major == 0x05UL && (minor == 0x01UL || minor == 0x03UL))
		return KL_BACKEND_BT_KIND_KEYBOARD;
	if (major == 0x05UL && minor == 0x02UL)
		return KL_BACKEND_BT_KIND_MOUSE;
	if (major == 0x04UL)
		return KL_BACKEND_BT_KIND_AUDIO;
	if (major == 0x02UL)
		return KL_BACKEND_BT_KIND_PHONE;
	if (major == 0x01UL)
		return KL_BACKEND_BT_KIND_COMPUTER;

	/* Anything else. */
	return KL_BACKEND_BT_KIND_OTHER;
}

/* Gives the errno value of bluetoothd's reason for a refusal. */
static int
bt_error(
	const char *reason)
{
	static const struct {
		const char *reason;
		int error;
	} reasons[] = {
		{ "permission", EACCES },
		{ "busy", EBUSY },
		{ "off", ENETDOWN },
		{ "timeout", ETIMEDOUT },
		{ "rejected", ECONNREFUSED },
		{ "request", ENOTSUP },
		{ "not-ready", EAGAIN },
		{ "no-controller", EAGAIN }
	};
	size_t index;
	int same;

	/* The reasons the desktop tells apart. */
	for (index = 0; index < sizeof(reasons) / sizeof(reasons[0]); index++) {
		same = strcmp(reason, reasons[index].reason);
		if (same == 0)
			return reasons[index].error;
	}

	/* Any other. */
	return EIO;
}

/*
 * Makes the reading the state: the devices paired first (then by name),
 * and what changed told.
 */
static void
bt_commit(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	struct kl_backend_bluetooth_device swap;
	size_t index;
	size_t other;
	int differs;
	int later;

	/* The paired first, then by name (a few devices: a plain sort). */
	for (index = 0; index < bluetooth->next_count; index++) {
		for (other = index + 1U; other < bluetooth->next_count; other++) {
			/* Whether the other goes before this one. */
			later = 0;
			if (bluetooth->next[other].paired > bluetooth->next[index].paired) {
				later = 1;
			} else if (bluetooth->next[other].paired == bluetooth->next[index].paired) {
				differs = strcmp(bluetooth->next[other].name, bluetooth->next[index].name);
				if (differs < 0)
					later = 1;
			}

			/* Swapped when it does. */
			if (!later)
				continue;
			swap = bluetooth->next[index];
			bluetooth->next[index] = bluetooth->next[other];
			bluetooth->next[other] = swap;
		}
	}

	/* The state, told when it changed. */
	differs = memcmp(&bluetooth->state, &bluetooth->next_state, sizeof(bluetooth->state));
	if (differs != 0) {
		bluetooth->state = bluetooth->next_state;
		*changed |= KL_BACKEND_BT_CHANGED_STATE;
	}

	/* Succeeded: the devices, told when they changed. */
	differs = 0;
	if (bluetooth->count != bluetooth->next_count)
		differs = 1;
	if (!differs && bluetooth->count != 0U)
		differs = memcmp(bluetooth->devices, bluetooth->next, bluetooth->count * sizeof(bluetooth->devices[0]));
	if (differs != 0) {
		memcpy(bluetooth->devices, bluetooth->next, bluetooth->next_count * sizeof(bluetooth->devices[0]));
		bluetooth->count = bluetooth->next_count;
		*changed |= KL_BACKEND_BT_CHANGED_DEVICES;
	}
}

/* Tells whether an address is six pairs of hexadecimal digits with colons. */
static int
bt_address_valid(
	const char *address)
{
	size_t length;
	size_t index;
	char character;

	/* Seventeen characters. */
	length = strlen(address);
	if (length != 17U)
		return 0;

	/* Each a digit, or a colon every third. */
	for (index = 0; index < 17U; index++) {
		character = address[index];
		if (index % 3U == 2U) {
			if (character != ':')
				return 0;
			continue;
		}

		/* A hexadecimal digit elsewhere. */
		if ((character < '0' || character > '9') &&
		    (character < 'a' || character > 'f') &&
		    (character < 'A' || character > 'F'))
			return 0;
	}

	/* Succeeded: an address. */
	return 1;
}
