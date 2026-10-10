/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The phone on zedBSD (ws197-p004a, plan/ws197/phase004/phase.md section
 * 5): bluetoothd's PHONE lines on /run/bluetoothd.sock.
 *
 * Three connections, none waited for:
 *
 *   the events       PHONE SUBSCRIBE, then PHONE STATE, PHONE MESSAGE
 *                    with its text, PHONE SENT and PHONE DROPPED as they
 *                    come (the owner of the phone only);
 *   the pages        PHONE PAGE, one at a time, at most four waiting;
 *   the requests     PHONE SEND, READ, LINK and SHOW, one at a time, at
 *                    most thirty-two waiting (a text sent never waits
 *                    behind a page).
 *
 * bluetoothd answers a request up to its DONE (ERROR <word> before it when
 * it failed) and drops a line written while it answers, so the next
 * request of a connection is written after the DONE.  A request answered
 * by nothing in two minutes fails with ETIMEDOUT and its connection is
 * made again.  A daemon that is not running is not a failure: the state
 * is unreachable and the events are asked for again every second.
 *
 * When the events are refused (not the owner, or no phone), the record is
 * read with SHOW every thirty seconds, and the events asked for again as
 * soon as it says the phone is this user's.  Nothing is kept on disk and
 * no number, name or text is logged.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/base/bluetoothd/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* bluetoothd's socket (a host test builds with another path). */
#ifndef PHONE_SOCKET_PATH
#define PHONE_SOCKET_PATH	BTD_SOCKET
#endif

/* The monotonic clock in milliseconds (a host test builds with its own clock). */
#ifndef PHONE_NOW_MS
#define PHONE_NOW_MS		phone_milliseconds
#endif

/* The waits: before the events are asked for again, between readings of the record, after a busy daemon, for an answer. */
#define PHONE_RETRY_MS		1000U
#define PHONE_SHOW_MS		30000U
#define PHONE_BUSY_MS		10000U
#define PHONE_ANSWER_MS		120000U

/* The longest line read (bluetoothd writes 2047 bytes at most with the newline), and the longest request written. */
#define PHONE_INPUT_MAX		2048U
#define PHONE_LINE_MAX		192U

/* How many pages and other requests wait, and how many items, results and states of texts sent are kept. */
#define PHONE_PAGES_MAX		4U
#define PHONE_REQUESTS_MAX	32U
#define PHONE_ITEMS_MAX		64U
#define PHONE_RESULTS_MAX	48U
#define PHONE_SENTS_MAX		32U

/* What a request is. */
#define PHONE_KIND_PAGE		1U
#define PHONE_KIND_READ		2U
#define PHONE_KIND_SEND		3U
#define PHONE_KIND_LINK		4U
#define PHONE_KIND_SHOW		5U

/* Where the events stand: no connection, SUBSCRIBE asked, the events followed. */
#define PHONE_EVENTS_NONE	0U
#define PHONE_EVENTS_ASKED	1U
#define PHONE_EVENTS_FOLLOWED	2U

/* send's flag that keeps a broken connection from raising SIGPIPE, where there is one. */
#ifdef MSG_NOSIGNAL
#define PHONE_SEND_FLAGS	(MSG_DONTWAIT | MSG_NOSIGNAL)
#else
#define PHONE_SEND_FLAGS	MSG_DONTWAIT
#endif

/*
 * One request: its id (0 for SHOW, which this asks for itself), its kind,
 * its line, and a text sent's bytes after the line (malloc'd, NULL for the
 * others).
 */
struct phone_request {
	uint32_t id;
	unsigned kind;
	char line[PHONE_LINE_MAX];
	uint8_t *text;
	size_t length;
};

/*
 * One connection to bluetoothd: its socket (-1 while none), the bytes read
 * but not taken yet, the message whose text is being read (its item, the
 * text's room and how much has come), the bytes waiting to be written, and
 * for the pages and the requests: those waiting (the first is the one
 * written when one is answered), whether the first was written, when its
 * answer is due, and the answer gathered so far.
 */
struct phone_connection {
	int socket;
	char input[PHONE_INPUT_MAX];
	size_t used;

	/* The text of a message being read. */
	unsigned reading_text;
	struct kl_backend_phone_item text_item;
	char *text;
	size_t text_used;

	/* What waits to be written. */
	char *output;
	size_t output_length;
	size_t output_sent;

	/* The requests. */
	struct phone_request *requests;
	size_t capacity;
	size_t count;
	unsigned written;
	uint64_t due_ms;
	struct kl_backend_phone_result answer;
	unsigned show_line;

	/* bluetoothd said something no daemon says: the connection is made again. */
	unsigned broken;
};

/* An item kept until it is taken, with the text it owns. */
struct phone_kept_item {
	struct kl_backend_phone_item item;
	char *text;
};

/* A sent text's state kept until it is taken: bluetoothd's number and the state. */
struct phone_sent {
	uint32_t request;
	unsigned state;
};

/*
 * The following of the phone: the state as last read, the three
 * connections, where the events stand and when they are asked for again,
 * when the record is read again, the next id, and the items, results and
 * states of texts sent not taken yet (with the text of the item taken
 * last, freed by the next take).  It lives from kl_backend_phone_open to
 * kl_backend_phone_close.
 */
struct kl_backend_phone {
	struct kl_backend_phone_state state;

	/* The connections. */
	struct phone_connection events;
	struct phone_connection pages;
	struct phone_connection requests;
	struct phone_request page_room[PHONE_PAGES_MAX];
	struct phone_request request_room[PHONE_REQUESTS_MAX];

	/* The events and the record's readings. */
	unsigned events_stand;
	uint64_t events_due_ms;
	uint64_t show_due_ms;
	unsigned events_refused;
	unsigned events_busy;

	/* The ids. */
	uint32_t next_id;

	/* What waits to be taken. */
	struct phone_kept_item items[PHONE_ITEMS_MAX];
	size_t item_head;
	size_t item_count;
	char *taken_text;
	struct kl_backend_phone_result results[PHONE_RESULTS_MAX];
	size_t result_head;
	size_t result_count;
	struct phone_sent sents[PHONE_SENTS_MAX];
	size_t sent_head;
	size_t sent_count;

	/* What changed since the last update (bits), and whether something was dropped. */
	unsigned changed;
};

static uint64_t phone_milliseconds(void);
static void phone_connection_init(struct phone_connection *connection, struct phone_request *room, size_t capacity);
static int phone_connect(struct phone_connection *connection);
static void phone_close(struct phone_connection *connection);
static int phone_queue(struct kl_backend_phone *phone, struct phone_connection *connection, unsigned kind, const char *line, const uint8_t *text, size_t length, uint32_t *id);
static void phone_request_drop(struct phone_connection *connection);
static void phone_fail_all(struct kl_backend_phone *phone, struct phone_connection *connection, int error);
static void phone_start(struct kl_backend_phone *phone, struct phone_connection *connection, uint64_t now);
static int phone_flush(struct phone_connection *connection);
static void phone_read(struct kl_backend_phone *phone, struct phone_connection *connection);
static void phone_lost(struct kl_backend_phone *phone, struct phone_connection *connection);
static int phone_take_input(struct kl_backend_phone *phone, struct phone_connection *connection);
static void phone_text_take(struct kl_backend_phone *phone, struct phone_connection *connection, size_t *taken);
static void phone_line(struct kl_backend_phone *phone, struct phone_connection *connection, char *line);
static void phone_events_line(struct kl_backend_phone *phone, char *line);
static void phone_answer_line(struct kl_backend_phone *phone, struct phone_connection *connection, char *line);
static void phone_answer_done(struct kl_backend_phone *phone, struct phone_connection *connection);
static int phone_message_line(struct kl_backend_phone *phone, struct phone_connection *connection, const char *line, uint32_t id);
static void phone_item_push(struct kl_backend_phone *phone, struct phone_connection *connection);
static void phone_result_push(struct kl_backend_phone *phone, const struct kl_backend_phone_result *result);
static void phone_sent_line(struct kl_backend_phone *phone, const char *line);
static void phone_state_line(struct kl_backend_phone *phone, const char *line, unsigned whole);
static void phone_state_why(struct kl_backend_phone *phone, const char *why);
static void phone_events_start(struct kl_backend_phone *phone, uint64_t now);
static void phone_show_due(struct kl_backend_phone *phone, uint64_t now);
static int phone_field(const char *line, const char *name, char *value, size_t size);
static unsigned phone_field_number(const char *line, const char *name);
static int phone_error(const char *word);
static unsigned phone_profiles(const char *text);
static int phone_number_valid(const char *to);
static int phone_word_valid(const char *word, size_t size);
static int phone_address_valid(const char *address);

/*
 * Starts following the phone; bluetoothd not running yet is reached by a
 * later update.  Returns NULL only without memory.
 */
struct kl_backend_phone *
kl_backend_phone_open(
	void)
{
	struct kl_backend_phone *phone;

	/* The record, with no connection. */
	phone = calloc(1, sizeof(*phone));
	if (phone == NULL)
		return NULL;

	/* The connections: the events take no requests, the pages four, the other requests thirty-two. */
	phone_connection_init(&phone->events, NULL, 0U);
	phone_connection_init(&phone->pages, phone->page_room, PHONE_PAGES_MAX);
	phone_connection_init(&phone->requests, phone->request_room, PHONE_REQUESTS_MAX);

	/* Unreachable until the events are asked for, which the first update does; ids start at 1 (0 is a message that came by itself). */
	(void)snprintf(phone->state.why, sizeof(phone->state.why), "%s", "unreachable");
	phone->next_id = 1U;

	/* Succeeded: the first update asks for the events. */
	return phone;
}

/*
 * Stops following: every connection closes, and what waits is dropped
 * (the caller answers the requests it was waiting for).
 */
void
kl_backend_phone_close(
	struct kl_backend_phone *phone)
{
	size_t index;

	/* Nothing to close. */
	if (phone == NULL)
		return;

	/* Each connection with what it holds. */
	phone_close(&phone->events);
	phone_close(&phone->pages);
	phone_close(&phone->requests);

	/* The texts of the requests waiting. */
	while (phone->pages.count > 0U)
		phone_request_drop(&phone->pages);
	while (phone->requests.count > 0U)
		phone_request_drop(&phone->requests);

	/* The texts of the items not taken, and of the one taken last. */
	for (index = 0U; index < phone->item_count; index++)
		free(phone->items[(phone->item_head + index) % PHONE_ITEMS_MAX].text);
	free(phone->taken_text);

	/* The record. */
	free(phone);
}

/*
 * Reads what bluetoothd has sent on each connection, gives up an answer
 * overdue, asks for the events and reads the record when due, and writes
 * the next requests.
 */
int
kl_backend_phone_update(
	struct kl_backend_phone *phone,
	unsigned *changed)
{
	uint64_t now;

	/* A record and somewhere to say what changed. */
	if (phone == NULL || changed == NULL)
		return EINVAL;
	now = PHONE_NOW_MS();

	/* What has come on each connection. */
	phone_read(phone, &phone->events);
	phone_read(phone, &phone->pages);
	phone_read(phone, &phone->requests);

	/* The events, when they are due. */
	if (phone->events.socket < 0 && now >= phone->events_due_ms)
		phone_events_start(phone, now);

	/* The record, while the events are refused. */
	if (phone->events_stand != PHONE_EVENTS_FOLLOWED &&
	    phone->state.reachable &&
	    now >= phone->show_due_ms)
		phone_show_due(phone, now);

	/* The next pages and requests, and an answer that did not come in time. */
	phone_start(phone, &phone->pages, now);
	phone_start(phone, &phone->requests, now);

	/* What waits is told again until it is taken. */
	if (phone->item_count > 0U)
		phone->changed |= KL_BACKEND_PHONE_CHANGED_ITEM;
	if (phone->result_count > 0U)
		phone->changed |= KL_BACKEND_PHONE_CHANGED_RESULT;
	if (phone->sent_count > 0U)
		phone->changed |= KL_BACKEND_PHONE_CHANGED_SENT;

	/* Succeeded: what changed, told once. */
	*changed = phone->changed;
	phone->changed = 0U;
	return 0;
}

/*
 * Copies the state as last read.
 */
void
kl_backend_phone_get_state(
	const struct kl_backend_phone *phone,
	struct kl_backend_phone_state *state)
{
	/* Nothing followed: unreachable. */
	memset(state, 0, sizeof(*state));
	if (phone == NULL)
		return;

	/* Succeeded: the copy. */
	*state = phone->state;
}

/*
 * Asks a page of the synchronisation on the pages' connection.
 */
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
	char line[PHONE_LINE_MAX];
	int valid;
	int error;

	/* A record and somewhere for the id. */
	if (phone == NULL || id == NULL || cursor == NULL)
		return EINVAL;

	/* The messages only (the contacts and the calls come later). */
	if (what != KL_BACKEND_PHONE_WHAT_MESSAGES)
		return EINVAL;

	/* A time, a limit a folder and a count bluetoothd takes. */
	if (since < 0 || limit > KL_BACKEND_PHONE_LIMIT_MAX)
		return EINVAL;
	if (count == 0U || count > KL_BACKEND_PHONE_PAGE_MAX)
		return EINVAL;

	/* A cursor of one word (bluetoothd's own, or empty at the start). */
	valid = phone_word_valid(cursor, KL_BACKEND_PHONE_CURSOR_MAX);
	if (cursor[0] != '\0' && !valid)
		return EINVAL;

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* The line: the cursor only when there is one. */
	if (cursor[0] != '\0') {
		(void)snprintf(line, sizeof(line), "PHONE PAGE messages since=%lld limit=%u cursor=%s count=%u\n", (long long)since, limit, cursor, count);
	} else {
		(void)snprintf(line, sizeof(line), "PHONE PAGE messages since=%lld limit=%u count=%u\n", (long long)since, limit, count);
	}

	/* Waits its turn on the pages' connection. */
	error = phone_queue(phone, &phone->pages, PHONE_KIND_PAGE, line, NULL, 0U, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Marks a message read on the phone by its handle.
 */
int
kl_backend_phone_read(
	struct kl_backend_phone *phone,
	const char *handle,
	uint32_t *id)
{
	char line[PHONE_LINE_MAX];
	int valid;
	int error;

	/* A record, a handle of one word and somewhere for the id. */
	if (phone == NULL || id == NULL || handle == NULL)
		return EINVAL;
	valid = phone_word_valid(handle, KL_BACKEND_PHONE_HANDLE_MAX);
	if (!valid)
		return EINVAL;

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* Waits its turn on the requests' connection. */
	(void)snprintf(line, sizeof(line), "PHONE READ handle=%s\n", handle);
	error = phone_queue(phone, &phone->requests, PHONE_KIND_READ, line, NULL, 0U, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Sends a text to a number; bluetoothd pushes it to the phone's outbox.
 */
int
kl_backend_phone_send(
	struct kl_backend_phone *phone,
	const char *to,
	const uint8_t *text,
	size_t length,
	uint32_t *id)
{
	char line[PHONE_LINE_MAX];
	const void *nul;
	int valid;
	int error;

	/* A record, a number and somewhere for the id. */
	if (phone == NULL || id == NULL || to == NULL || text == NULL)
		return EINVAL;
	valid = phone_number_valid(to);
	if (!valid)
		return EINVAL;

	/* A text bluetoothd takes: not empty, not too long, no NUL (it would end the text on the phone). */
	if (length == 0U || length > KL_BACKEND_PHONE_SEND_MAX)
		return EINVAL;
	nul = memchr(text, '\0', length);
	if (nul != NULL)
		return EINVAL;

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* Waits its turn on the requests' connection, its text after the line. */
	(void)snprintf(line, sizeof(line), "PHONE SEND to=\"%s\" length=%lu\n", to, (unsigned long)length);
	error = phone_queue(phone, &phone->requests, PHONE_KIND_SEND, line, text, length, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Turns the phone's switch and its profiles on or off (bluetoothd's PHONE
 * LINK); the record is read again once it is answered.
 */
int
kl_backend_phone_link_set(
	struct kl_backend_phone *phone,
	const char *address,
	unsigned on,
	unsigned profiles,
	uint32_t *id)
{
	char line[PHONE_LINE_MAX];
	char letters[8];
	size_t used;
	int valid;
	int error;

	/* A record, an address and somewhere for the id. */
	if (phone == NULL || id == NULL || address == NULL)
		return EINVAL;
	valid = phone_address_valid(address);
	if (!valid)
		return EINVAL;

	/* Profiles bluetoothd knows. */
	if ((profiles & ~(KL_BACKEND_PHONE_PROFILE_MESSAGES | KL_BACKEND_PHONE_PROFILE_CONTACTS | KL_BACKEND_PHONE_PROFILE_CALLS)) != 0U)
		return EINVAL;

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* The profiles as the letters m, c and h with commas (none: empty). */
	used = 0U;
	letters[0] = '\0';
	if ((profiles & KL_BACKEND_PHONE_PROFILE_MESSAGES) != 0U)
		used += (size_t)snprintf(letters + used, sizeof(letters) - used, "%s", "m,");
	if ((profiles & KL_BACKEND_PHONE_PROFILE_CONTACTS) != 0U)
		used += (size_t)snprintf(letters + used, sizeof(letters) - used, "%s", "c,");
	if ((profiles & KL_BACKEND_PHONE_PROFILE_CALLS) != 0U)
		used += (size_t)snprintf(letters + used, sizeof(letters) - used, "%s", "h,");

	/* The last comma goes. */
	if (used > 0U)
		letters[used - 1U] = '\0';

	/* Waits its turn on the requests' connection. */
	if (on) {
		(void)snprintf(line, sizeof(line), "PHONE LINK %s on profiles=%s\n", address, letters);
	} else {
		(void)snprintf(line, sizeof(line), "PHONE LINK %s off profiles=%s\n", address, letters);
	}

	/* The line waits on the requests' connection. */
	error = phone_queue(phone, &phone->requests, PHONE_KIND_LINK, line, NULL, 0U, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Takes the oldest item; its text lives until the next take.
 */
int
kl_backend_phone_take_item(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_item *item)
{
	struct phone_kept_item *kept;

	/* A record and somewhere for the item. */
	if (phone == NULL || item == NULL)
		return 0;

	/* The text of the item taken last goes now. */
	free(phone->taken_text);
	phone->taken_text = NULL;

	/* None waits. */
	if (phone->item_count == 0U)
		return 0;

	/* The oldest, whose text is now the one taken last. */
	kept = &phone->items[phone->item_head];
	*item = kept->item;
	item->text = kept->text;
	phone->taken_text = kept->text;
	kept->text = NULL;
	phone->item_head = (phone->item_head + 1U) % PHONE_ITEMS_MAX;
	phone->item_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Takes the oldest result.
 */
int
kl_backend_phone_take_result(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_result *result)
{
	/* A record, somewhere for it, and one waiting. */
	if (phone == NULL || result == NULL)
		return 0;
	if (phone->result_count == 0U)
		return 0;

	/* The oldest. */
	*result = phone->results[phone->result_head];
	phone->result_head = (phone->result_head + 1U) % PHONE_RESULTS_MAX;
	phone->result_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Takes the oldest state of a sent text that came on the events.
 */
int
kl_backend_phone_take_sent(
	struct kl_backend_phone *phone,
	uint32_t *sent_request,
	unsigned *state)
{
	/* A record, somewhere for it, and one waiting. */
	if (phone == NULL || sent_request == NULL || state == NULL)
		return 0;
	if (phone->sent_count == 0U)
		return 0;

	/* The oldest. */
	*sent_request = phone->sents[phone->sent_head].request;
	*state = phone->sents[phone->sent_head].state;
	phone->sent_head = (phone->sent_head + 1U) % PHONE_SENTS_MAX;
	phone->sent_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Reads the record and asks for the events again at once (a pairing of
 * the phone or a forget may have made this user the owner or not).
 */
void
kl_backend_phone_refresh(
	struct kl_backend_phone *phone)
{
	/* Nothing followed. */
	if (phone == NULL)
		return;

	/* The record at the next update. */
	phone->show_due_ms = 0U;

	/* The events again, unless they are followed (bluetoothd closes them for an owner no longer). */
	if (phone->events_stand != PHONE_EVENTS_FOLLOWED) {
		phone_close(&phone->events);
		phone->events_stand = PHONE_EVENTS_NONE;
		phone->events_due_ms = 0U;
	}
}

/* Gives the monotonic clock in milliseconds. */
static uint64_t
phone_milliseconds(
	void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Prepares a connection with no socket and the room for its requests. */
static void
phone_connection_init(
	struct phone_connection *connection,
	struct phone_request *room,
	size_t capacity)
{
	/* Nothing open, nothing waiting. */
	memset(connection, 0, sizeof(*connection));
	connection->socket = -1;
	connection->requests = room;
	connection->capacity = capacity;
}

/* Connects to bluetoothd; 0, or an errno value with no connection. */
static int
phone_connect(
	struct phone_connection *connection)
{
	struct sockaddr_un address;
	int descriptor;
	int status;
	int error;

	/* The socket. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0) {
		error = errno;
		return error;
	}

	/* bluetoothd's path. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", PHONE_SOCKET_PATH);

	/* The connection. */
	status = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Succeeded: connected, nothing read or written yet. */
	connection->socket = descriptor;
	connection->used = 0U;
	return 0;
}

/*
 * Closes a connection (none is fine) and drops what it was reading and
 * writing; its requests stay (the first one written again).
 */
static void
phone_close(
	struct phone_connection *connection)
{
	/* The socket. */
	if (connection->socket >= 0)
		(void)close(connection->socket);
	connection->socket = -1;
	connection->used = 0U;

	/* The text being read. */
	free(connection->text);
	connection->text = NULL;
	connection->text_used = 0U;
	connection->reading_text = 0U;

	/* The bytes waiting to be written. */
	free(connection->output);
	connection->output = NULL;
	connection->output_length = 0U;
	connection->output_sent = 0U;

	/* No request written on it any more, and nothing wrong said. */
	connection->written = 0U;
	connection->broken = 0U;
}

/*
 * Puts a request at the end of a connection's waiting ones and numbers it.
 * Returns 0, ENOMEM, or EBUSY when they are full.
 */
static int
phone_queue(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	unsigned kind,
	const char *line,
	const uint8_t *text,
	size_t length,
	uint32_t *id)
{
	struct phone_request *request;

	/* Room among the waiting ones. */
	if (connection->count >= connection->capacity)
		return EBUSY;
	request = &connection->requests[connection->count];
	memset(request, 0, sizeof(*request));

	/* A text's own copy, sent after the line. */
	if (length > 0U) {
		request->text = malloc(length);
		if (request->text == NULL)
			return ENOMEM;
		memcpy(request->text, text, length);
		request->length = length;
	}

	/* The kind and the line. */
	request->kind = kind;
	(void)snprintf(request->line, sizeof(request->line), "%s", line);

	/* Its id (SHOW, asked by this itself, has 0; the others never 0). */
	if (kind != PHONE_KIND_SHOW) {
		request->id = phone->next_id;
		phone->next_id++;
		if (phone->next_id == 0U)
			phone->next_id = 1U;
	}

	/* Succeeded: waiting. */
	connection->count++;
	if (id != NULL)
		*id = request->id;
	return 0;
}

/* Takes the first request off a connection's waiting ones with its text. */
static void
phone_request_drop(
	struct phone_connection *connection)
{
	/* Nothing waits. */
	if (connection->count == 0U)
		return;

	/* Its text, then the others move up. */
	free(connection->requests[0].text);
	memmove(&connection->requests[0], &connection->requests[1], (connection->count - 1U) * sizeof(connection->requests[0]));
	connection->count--;
	connection->written = 0U;
}

/* Answers every request of a connection with an error (SHOW, this one's own, is dropped silently). */
static void
phone_fail_all(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	int error)
{
	struct kl_backend_phone_result result;

	/* Each waiting one, the first first. */
	while (connection->count > 0U) {
		if (connection->requests[0].kind != PHONE_KIND_SHOW) {
			memset(&result, 0, sizeof(result));
			result.id = connection->requests[0].id;
			result.error = error;
			phone_result_push(phone, &result);
		}

		/* Off the waiting ones. */
		phone_request_drop(connection);
	}
}

/*
 * Moves a connection of requests on: an answer overdue fails with
 * ETIMEDOUT and the connection is made again; with none written, the first
 * waiting is written (after connecting when needed); what waits to be
 * written goes as far as the socket takes it.
 */
static void
phone_start(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	uint64_t now)
{
	struct kl_backend_phone_result result;
	struct phone_request *request;
	size_t line_length;
	int error;

	/* An answer that did not come in time: given up, the connection made again for the next. */
	if (connection->written && now >= connection->due_ms) {
		if (connection->requests[0].kind != PHONE_KIND_SHOW) {
			memset(&result, 0, sizeof(result));
			result.id = connection->requests[0].id;
			result.error = ETIMEDOUT;
			phone_result_push(phone, &result);
		}

		/* The connection is made again for the next. */
		phone_close(connection);
		phone_request_drop(connection);
	}

	/* Nothing to write while one is answered, or while none waits. */
	if (connection->written || connection->count == 0U) {
		error = phone_flush(connection);
		if (error != 0)
			phone_lost(phone, connection);
		return;
	}

	/* A connection for it; without bluetoothd every waiting one fails. */
	if (connection->socket < 0) {
		error = phone_connect(connection);
		if (error != 0) {
			phone_fail_all(phone, connection, ENOTCONN);
			return;
		}
	}

	/* The line and its text, to be written as the socket takes them. */
	request = &connection->requests[0];
	line_length = strlen(request->line);
	connection->output = malloc(line_length + request->length);
	if (connection->output == NULL) {
		phone_fail_all(phone, connection, ENOMEM);
		return;
	}

	/* The line, then the text. */
	memcpy(connection->output, request->line, line_length);
	if (request->length > 0U)
		memcpy(connection->output + line_length, request->text, request->length);
	connection->output_length = line_length + request->length;
	connection->output_sent = 0U;

	/* Written: its answer is due in two minutes, gathered from nothing. */
	connection->written = 1U;
	connection->due_ms = now + PHONE_ANSWER_MS;
	memset(&connection->answer, 0, sizeof(connection->answer));
	connection->answer.id = request->id;
	connection->show_line = 0U;

	/* As much as the socket takes now. */
	error = phone_flush(connection);
	if (error != 0)
		phone_lost(phone, connection);
}

/* Writes what waits as far as the socket takes it; 0, or EPIPE when the connection broke. */
static int
phone_flush(
	struct phone_connection *connection)
{
	ssize_t sent;
	int error;

	/* Each part the socket takes. */
	while (connection->output != NULL && connection->output_sent < connection->output_length) {
		sent = send(connection->socket, connection->output + connection->output_sent, connection->output_length - connection->output_sent, PHONE_SEND_FLAGS);

		/* The socket is full for now. */
		if (sent < 0) {
			error = errno;
			if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
				return 0;
			return EPIPE;
		}

		/* Written. */
		connection->output_sent += (size_t)sent;
	}

	/* All written: the room goes. */
	if (connection->output != NULL && connection->output_sent == connection->output_length) {
		free(connection->output);
		connection->output = NULL;
		connection->output_length = 0U;
		connection->output_sent = 0U;
	}

	/* Succeeded: written as far as it could be. */
	return 0;
}

/*
 * Reads what has come on a connection and gives it to its conversation;
 * a connection that ended or broke ends it.
 */
static void
phone_read(
	struct kl_backend_phone *phone,
	struct phone_connection *connection)
{
	ssize_t count;
	int error;
	int broken;

	/* Every byte there now. */
	while (connection->socket >= 0) {
		count = recv(connection->socket, connection->input + connection->used, sizeof(connection->input) - connection->used, MSG_DONTWAIT);

		/* Nothing more for now. */
		if (count < 0) {
			error = errno;
			if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
				break;
		}

		/* The end of the connection: its conversation ends. */
		if (count <= 0) {
			phone_lost(phone, connection);
			break;
		}

		/* The lines and the texts in it; a conversation that broke stops. */
		connection->used += (size_t)count;
		broken = phone_take_input(phone, connection);
		if (broken) {
			phone_lost(phone, connection);
			break;
		}
	}
}

/*
 * Ends a conversation whose connection ended or broke: the events are
 * asked for again in a second; the requests waiting fail (ECONNRESET), the
 * connection made again for the next.
 */
static void
phone_lost(
	struct kl_backend_phone *phone,
	struct phone_connection *connection)
{
	uint64_t now;

	/* Closed. */
	phone_close(connection);

	/* The events: not followed any more, asked for again in a second. */
	if (connection == &phone->events) {
		now = PHONE_NOW_MS();
		if (phone->events_stand == PHONE_EVENTS_FOLLOWED) {
			phone->state.subscribed = 0U;
			phone->changed |= KL_BACKEND_PHONE_CHANGED_STATE;
		}

		/* Asked for again in a second. */
		phone->events_stand = PHONE_EVENTS_NONE;
		phone->events_due_ms = now + PHONE_RETRY_MS;
		return;
	}

	/* The pages or the requests: every one of them is answered with the loss. */
	phone_fail_all(phone, connection, ECONNRESET);
}

/*
 * Takes the lines and the texts in what a connection has read.  Returns 1
 * when bluetoothd said something no daemon says (the connection is made
 * again), 0 otherwise.
 */
static int
phone_take_input(
	struct kl_backend_phone *phone,
	struct phone_connection *connection)
{
	size_t taken;
	size_t length;
	char *end;

	/* Each text and each whole line, as long as the connection stays and makes sense. */
	taken = 0U;
	while (connection->socket >= 0 && !connection->broken && taken < connection->used) {
		/* A message's text takes the bytes first. */
		if (connection->reading_text) {
			phone_text_take(phone, connection, &taken);
			continue;
		}

		/* A whole line, or the rest waits for more. */
		end = memchr(connection->input + taken, '\n', connection->used - taken);
		if (end == NULL)
			break;
		*end = '\0';
		length = (size_t)(end - (connection->input + taken)) + 1U;
		phone_line(phone, connection, connection->input + taken);
		taken += length;
	}

	/* A connection that said something no daemon says. */
	if (connection->broken)
		return 1;

	/* A connection the line closed has nothing left to keep. */
	if (connection->socket < 0)
		return 0;

	/* The rest moves to the front. */
	memmove(connection->input, connection->input + taken, connection->used - taken);
	connection->used -= taken;

	/* A line longer than any bluetoothd writes. */
	if (connection->used == sizeof(connection->input))
		return 1;

	/* Succeeded: taken as far as it goes. */
	return 0;
}

/* Takes the bytes of a message's text that have come; a whole text makes its item. */
static void
phone_text_take(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	size_t *taken)
{
	size_t wanted;
	size_t there;

	/* As much of the text as there is. */
	wanted = connection->text_item.length - connection->text_used;
	there = connection->used - *taken;
	if (there > wanted)
		there = wanted;
	memcpy(connection->text + connection->text_used, connection->input + *taken, there);
	connection->text_used += there;
	*taken += there;

	/* The whole text: the item is kept for the caller. */
	if (connection->text_used == connection->text_item.length) {
		connection->text[connection->text_used] = '\0';
		phone_item_push(phone, connection);
	}
}

/* Gives a line to its connection's conversation. */
static void
phone_line(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	char *line)
{
	/* The events. */
	if (connection == &phone->events) {
		phone_events_line(phone, line);
		return;
	}

	/* An answer to a page or a request. */
	phone_answer_line(phone, connection, line);
}

/*
 * Takes a line of the events: SUBSCRIBE's answer (ERROR, DONE), then the
 * state, the messages, the states of texts sent and the drops.
 */
static void
phone_events_line(
	struct kl_backend_phone *phone,
	char *line)
{
	uint64_t now;
	int same;

	/* SUBSCRIBE refused: not the owner (or no phone), busy, or a daemon without PHONE. */
	same = strncmp(line, "ERROR ", 6U);
	if (same == 0) {
		same = strcmp(line + 6, "busy");
		if (same == 0) {
			phone->events_busy = 1U;
		} else {
			phone->events_refused = 1U;
		}

		/* The DONE that follows settles it. */
		return;
	}

	/* SUBSCRIBE answered. */
	same = strcmp(line, "DONE");
	if (same == 0 && phone->events_stand == PHONE_EVENTS_ASKED) {
		now = PHONE_NOW_MS();

		/* Busy: asked again in ten seconds. */
		if (phone->events_busy) {
			phone_close(&phone->events);
			phone->events_stand = PHONE_EVENTS_NONE;
			phone->events_due_ms = now + PHONE_BUSY_MS;
			return;
		}

		/* Refused: the record is read now and every thirty seconds; asked again when it says the phone is this user's. */
		if (phone->events_refused) {
			phone_close(&phone->events);
			phone->events_stand = PHONE_EVENTS_NONE;
			phone->events_due_ms = UINT64_MAX;
			phone->show_due_ms = 0U;
			phone->state.subscribed = 0U;
			phone->changed |= KL_BACKEND_PHONE_CHANGED_STATE;
			return;
		}

		/* Followed: the state comes next. */
		phone->events_stand = PHONE_EVENTS_FOLLOWED;
		return;
	}

	/* Nothing else counts before SUBSCRIBE is answered. */
	if (phone->events_stand != PHONE_EVENTS_FOLLOWED)
		return;

	/* The state. */
	same = strncmp(line, "PHONE STATE ", 12U);
	if (same == 0) {
		phone->state.subscribed = 1U;
		phone_state_line(phone, line + 12, 1U);
		return;
	}

	/* A message that came by itself (its text follows). */
	same = strncmp(line, "PHONE MESSAGE ", 14U);
	if (same == 0) {
		(void)phone_message_line(phone, &phone->events, line, 0U);
		return;
	}

	/* A sent text's state. */
	same = strncmp(line, "PHONE SENT ", 11U);
	if (same == 0) {
		phone_sent_line(phone, line);
		return;
	}

	/* bluetoothd dropped an event: the caller synchronises again. */
	same = strcmp(line, "PHONE DROPPED");
	if (same == 0) {
		phone->changed |= KL_BACKEND_PHONE_CHANGED_DROPPED;
		return;
	}

	/* PHONE MESSAGE-GONE and anything newer are not followed here. */
}

/*
 * Takes a line of an answer to a page or a request: the items, the page's
 * end, the text's number, the record (SHOW), the failure and the DONE.
 */
static void
phone_answer_line(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	char *line)
{
	struct kl_backend_phone_result *answer;
	char value[KL_BACKEND_PHONE_CURSOR_MAX];
	int same;

	/* A line nobody asked for is not taken. */
	if (!connection->written)
		return;
	answer = &connection->answer;

	/* The end of the answer. */
	same = strcmp(line, "DONE");
	if (same == 0) {
		phone_answer_done(phone, connection);
		return;
	}

	/* A failure, in bluetoothd's word. */
	same = strncmp(line, "ERROR ", 6U);
	if (same == 0) {
		answer->error = phone_error(line + 6);
		return;
	}

	/* One message of a page (its text follows). */
	same = strncmp(line, "PHONE MESSAGE ", 14U);
	if (same == 0) {
		(void)phone_message_line(phone, connection, line, answer->id);
		return;
	}

	/* The page's end: where the next starts, whether more follow, the counts. */
	same = strncmp(line, "PHONE PAGE-END ", 15U);
	if (same == 0) {
		(void)phone_field(line, "cursor=", value, sizeof(value));
		(void)snprintf(answer->cursor, sizeof(answer->cursor), "%s", value);
		answer->more = phone_field_number(line, "more=");
		answer->count = phone_field_number(line, "count=");
		answer->skipped = phone_field_number(line, "skipped=");
		answer->capped = phone_field_number(line, "capped=");
		return;
	}

	/* A text pushed to the phone's outbox: bluetoothd's number for its later states. */
	same = strncmp(line, "PHONE SENT ", 11U);
	if (same == 0) {
		answer->sent_request = phone_field_number(line, "request=");
		return;
	}

	/* The record as SHOW tells it. */
	same = strncmp(line, "PHONE address=", 14U);
	if (same == 0 && connection->requests[0].kind == PHONE_KIND_SHOW) {
		connection->show_line = 1U;
		phone_state_line(phone, line + 6, 0U);
		return;
	}
}

/*
 * Ends the answer to the first request of a connection: its result, or
 * for SHOW the record's state; then the next can be written.
 */
static void
phone_answer_done(
	struct kl_backend_phone *phone,
	struct phone_connection *connection)
{
	struct phone_request *request;
	unsigned kind;

	/* The request answered. */
	request = &connection->requests[0];
	kind = request->kind;

	/* SHOW: no record line (none, or a daemon without PHONE) means no phone. */
	if (kind == PHONE_KIND_SHOW) {
		if (!connection->show_line) {
			phone->state.have_record = 0U;
			phone->state.linked = 0U;
			phone->state.messages = KL_BACKEND_PHONE_MESSAGES_OFF;
			phone->state.address[0] = '\0';
			phone_state_why(phone, "no-record");
		}

		/* Answered: the next may be written. */
		phone_request_drop(connection);
		return;
	}

	/* The result for the caller. */
	phone_result_push(phone, &connection->answer);

	/* The switch changed: the record and the events at once. */
	if (kind == PHONE_KIND_LINK && connection->answer.error == 0)
		kl_backend_phone_refresh(phone);

	/* The next may be written. */
	phone_request_drop(connection);
}

/*
 * Takes a PHONE MESSAGE line: the item's fields, then its text of length
 * bytes is read before the next line.  Returns 0, or EPROTO (no length,
 * or a text longer than bluetoothd sends) or ENOMEM, the connection then
 * being made again.
 */
static int
phone_message_line(
	struct kl_backend_phone *phone,
	struct phone_connection *connection,
	const char *line,
	uint32_t id)
{
	struct kl_backend_phone_item *item;
	char value[KL_BACKEND_PHONE_PEER_MAX];
	unsigned long length;
	char *end;
	int same;
	int found;

	/* The item, numbered by its page (0 for one that came by itself). */
	item = &connection->text_item;
	memset(item, 0, sizeof(*item));
	item->id = id;

	/* Its handle and key. */
	(void)phone_field(line, "handle=", item->handle, sizeof(item->handle));
	(void)phone_field(line, "key=", item->key, sizeof(item->key));

	/* The folder and the direction. */
	(void)phone_field(line, "folder=", value, sizeof(value));
	same = strcmp(value, "sent");
	item->folder = KL_BACKEND_PHONE_FOLDER_INBOX;
	if (same == 0)
		item->folder = KL_BACKEND_PHONE_FOLDER_SENT;
	(void)phone_field(line, "dir=", value, sizeof(value));
	same = strcmp(value, "out");
	item->direction = KL_BACKEND_PHONE_DIRECTION_IN;
	if (same == 0)
		item->direction = KL_BACKEND_PHONE_DIRECTION_OUT;

	/* The time in UNIX seconds. */
	(void)phone_field(line, "time=", value, sizeof(value));
	item->time = (int64_t)strtoll(value, NULL, 10);

	/* Where the time came from. */
	(void)phone_field(line, "zone=", value, sizeof(value));
	item->zone = KL_BACKEND_PHONE_ZONE_RECEIVED;
	same = strcmp(value, "phone");
	if (same == 0)
		item->zone = KL_BACKEND_PHONE_ZONE_PHONE;
	same = strcmp(value, "mse");
	if (same == 0)
		item->zone = KL_BACKEND_PHONE_ZONE_MSE;
	same = strcmp(value, "local");
	if (same == 0)
		item->zone = KL_BACKEND_PHONE_ZONE_LOCAL;

	/* The phone's datetime, the other side's number and name. */
	(void)phone_field(line, "datetime=", item->datetime, sizeof(item->datetime));
	(void)phone_field(line, "peer=", item->peer, sizeof(item->peer));
	(void)phone_field(line, "name=", item->name, sizeof(item->name));

	/* The flags. */
	item->read = phone_field_number(line, "read=");
	item->partial = phone_field_number(line, "partial=");
	item->truncated = phone_field_number(line, "truncated=");

	/* The text's length. */
	found = phone_field(line, "length=", value, sizeof(value));
	if (!found) {
		connection->broken = 1U;
		return EPROTO;
	}

	/* Its number. */
	length = strtoul(value, &end, 10);

	/* A whole number, within what bluetoothd sends. */
	if (end == value ||
	    *end != '\0' ||
	    length > KL_BACKEND_PHONE_TEXT_MAX) {
		connection->broken = 1U;
		return EPROTO;
	}

	/* The room for the text and its NUL. */
	connection->text = malloc((size_t)length + 1U);
	if (connection->text == NULL) {
		connection->broken = 1U;
		return ENOMEM;
	}

	/* Nothing of the text read yet. */
	item->length = (size_t)length;
	connection->text_used = 0U;

	/* An empty text is whole at once; another is read next. */
	if (length == 0U) {
		connection->text[0] = '\0';
		phone_item_push(phone, connection);
		return 0;
	}

	/* Succeeded: the text is read before the next line. */
	connection->reading_text = 1U;
	return 0;
}

/* Keeps a connection's whole message for the caller; with no room the oldest goes and a drop is told. */
static void
phone_item_push(
	struct kl_backend_phone *phone,
	struct phone_connection *connection)
{
	struct phone_kept_item *kept;
	size_t slot;

	/* No room: the oldest goes, and the caller synchronises again. */
	if (phone->item_count == PHONE_ITEMS_MAX) {
		free(phone->items[phone->item_head].text);
		phone->items[phone->item_head].text = NULL;
		phone->item_head = (phone->item_head + 1U) % PHONE_ITEMS_MAX;
		phone->item_count--;
		phone->changed |= KL_BACKEND_PHONE_CHANGED_DROPPED;
	}

	/* The item with the text it now owns. */
	slot = (phone->item_head + phone->item_count) % PHONE_ITEMS_MAX;
	kept = &phone->items[slot];
	kept->item = connection->text_item;
	kept->item.text = NULL;
	kept->text = connection->text;
	phone->item_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_ITEM;

	/* The connection reads lines again. */
	connection->text = NULL;
	connection->text_used = 0U;
	connection->reading_text = 0U;
}

/* Keeps a result for the caller; with no room the oldest goes (the caller's own watch ends its wait). */
static void
phone_result_push(
	struct kl_backend_phone *phone,
	const struct kl_backend_phone_result *result)
{
	size_t slot;

	/* No room: the oldest goes. */
	if (phone->result_count == PHONE_RESULTS_MAX) {
		phone->result_head = (phone->result_head + 1U) % PHONE_RESULTS_MAX;
		phone->result_count--;
	}

	/* The result. */
	slot = (phone->result_head + phone->result_count) % PHONE_RESULTS_MAX;
	phone->results[slot] = *result;
	phone->result_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_RESULT;
}

/* Takes a PHONE SENT line of the events: bluetoothd's number and the state (pushed is the answer's own, not here). */
static void
phone_sent_line(
	struct kl_backend_phone *phone,
	const char *line)
{
	char value[16];
	unsigned state;
	size_t slot;
	int same;

	/* The state: sent, delivered or failed. */
	(void)phone_field(line, "state=", value, sizeof(value));
	state = 0U;
	same = strcmp(value, "sent");
	if (same == 0)
		state = KL_BACKEND_PHONE_SENT;
	same = strcmp(value, "delivered");
	if (same == 0)
		state = KL_BACKEND_PHONE_DELIVERED;
	same = strcmp(value, "failed");
	if (same == 0)
		state = KL_BACKEND_PHONE_FAILED;

	/* Another word is not a state of a text sent. */
	if (state == 0U)
		return;

	/* No room: the oldest goes (the app's rule of an hour settles it). */
	if (phone->sent_count == PHONE_SENTS_MAX) {
		phone->sent_head = (phone->sent_head + 1U) % PHONE_SENTS_MAX;
		phone->sent_count--;
	}

	/* The state for the caller. */
	slot = (phone->sent_head + phone->sent_count) % PHONE_SENTS_MAX;
	phone->sents[slot].request = phone_field_number(line, "request=");
	phone->sents[slot].state = state;
	phone->sent_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_SENT;
}

/*
 * Takes the record's fields ("address=..." of PHONE STATE, or of SHOW's
 * PHONE line); whole is 1 for the events (the owner sees every field).  A
 * record of this user's while the events are refused asks for them now.
 */
static void
phone_state_line(
	struct kl_backend_phone *phone,
	const char *line,
	unsigned whole)
{
	struct kl_backend_phone_state *state;
	char value[KL_BACKEND_BT_REASON_MAX];
	unsigned mine;
	int same;
	int found;

	/* A record, at its address. */
	state = &phone->state;
	state->have_record = 1U;
	(void)phone_field(line, "address=", state->address, sizeof(state->address));
	state->enabled = phone_field_number(line, "enabled=");

	/* Seen in part (not the owner): the switch only, and why nothing more. */
	mine = phone_field_number(line, "mine=");
	if (!whole && !mine) {
		state->linked = 0U;
		state->messages = KL_BACKEND_PHONE_MESSAGES_OFF;
		state->can_send = 0U;
		state->notify = 0U;
		state->present = 0U;
		state->profiles = 0U;
		phone_state_why(phone, "not-owner");
		return;
	}

	/* The profiles and whether the owner is at the seat. */
	(void)phone_field(line, "profiles=", value, sizeof(value));
	state->profiles = phone_profiles(value);
	state->present = phone_field_number(line, "present=");

	/* The phone link. */
	(void)phone_field(line, "link=", value, sizeof(value));
	same = strcmp(value, "ready");
	state->linked = 0U;
	if (same == 0)
		state->linked = 1U;

	/* The messages. */
	(void)phone_field(line, "messages=", value, sizeof(value));
	state->messages = KL_BACKEND_PHONE_MESSAGES_OFF;
	same = strcmp(value, "connecting");
	if (same == 0)
		state->messages = KL_BACKEND_PHONE_MESSAGES_CONNECTING;
	same = strcmp(value, "ready");
	if (same == 0)
		state->messages = KL_BACKEND_PHONE_MESSAGES_READY;
	same = strcmp(value, "failed");
	if (same == 0)
		state->messages = KL_BACKEND_PHONE_MESSAGES_FAILED;
	state->can_send = phone_field_number(line, "send=");
	state->notify = phone_field_number(line, "notify=");

	/* Why the link or the messages stopped, or nothing. */
	found = phone_field(line, "why=", value, sizeof(value));
	if (!found)
		value[0] = '\0';
	phone_state_why(phone, value);

	/* The owner's record while the events are refused: they are asked for now. */
	if (!whole && phone->events_stand == PHONE_EVENTS_NONE && phone->events.socket < 0)
		phone->events_due_ms = 0U;
}

/* Sets why the phone does not work (empty: it works as far as told) and tells the state changed. */
static void
phone_state_why(
	struct kl_backend_phone *phone,
	const char *why)
{
	/* The word, and a change for the caller. */
	(void)snprintf(phone->state.why, sizeof(phone->state.why), "%s", why);
	phone->changed |= KL_BACKEND_PHONE_CHANGED_STATE;
}

/* Asks bluetoothd for the events; without it the state is unreachable and asked again in a second. */
static void
phone_events_start(
	struct kl_backend_phone *phone,
	uint64_t now)
{
	int error;

	/* The connection; bluetoothd not there is unreachable. */
	error = phone_connect(&phone->events);
	if (error != 0) {
		if (phone->state.reachable) {
			memset(&phone->state, 0, sizeof(phone->state));
			phone_state_why(phone, "unreachable");
		}

		/* Asked again in a second. */
		phone->events_due_ms = now + PHONE_RETRY_MS;
		return;
	}

	/* bluetoothd answers: reachable, the record read too while the events are not followed. */
	if (!phone->state.reachable) {
		phone->state.reachable = 1U;
		phone->show_due_ms = 0U;
		phone->changed |= KL_BACKEND_PHONE_CHANGED_STATE;
	}

	/* SUBSCRIBE, answered by DONE (or ERROR and DONE). */
	phone->events_refused = 0U;
	phone->events_busy = 0U;
	phone->events.output = malloc(sizeof("PHONE SUBSCRIBE\n") - 1U);
	if (phone->events.output == NULL) {
		phone_close(&phone->events);
		phone->events_due_ms = now + PHONE_RETRY_MS;
		return;
	}

	/* The line, waiting to be written. */
	memcpy(phone->events.output, "PHONE SUBSCRIBE\n", sizeof("PHONE SUBSCRIBE\n") - 1U);
	phone->events.output_length = sizeof("PHONE SUBSCRIBE\n") - 1U;
	phone->events.output_sent = 0U;
	phone->events_stand = PHONE_EVENTS_ASKED;

	/* Written as far as the socket takes it. */
	error = phone_flush(&phone->events);
	if (error != 0)
		phone_lost(phone, &phone->events);
}

/* Reads the record with SHOW (once, not again while one waits), and again in thirty seconds. */
static void
phone_show_due(
	struct kl_backend_phone *phone,
	uint64_t now)
{
	size_t index;

	/* The next reading. */
	phone->show_due_ms = now + PHONE_SHOW_MS;

	/* One SHOW waiting is enough. */
	for (index = 0U; index < phone->requests.count; index++) {
		if (phone->requests.requests[index].kind == PHONE_KIND_SHOW)
			return;
	}

	/* SHOW, after the requests waiting. */
	(void)phone_queue(phone, &phone->requests, PHONE_KIND_SHOW, "PHONE SHOW\n", NULL, 0U, NULL);
}

/*
 * Finds a field "name=value" of a line: the value up to the next space,
 * or between the quotes of name="..." with bluetoothd's escapes (\", \\
 * and \xHH, a control byte becoming '?') undone.  Returns 1 with the value
 * cut to size, 0 when the line has none (the value is then empty).
 */
static int
phone_field(
	const char *line,
	const char *name,
	char *value,
	size_t size)
{
	const char *at;
	unsigned long byte;
	size_t length;
	size_t used;
	char digits[3];
	char *end;
	int quoted;
	int start;
	int same;

	/*
	 * The field, at the start of the line or after a space, never inside
	 * a quoted value: a name or a text of the phone's could otherwise pose
	 * as a field (" length=5" in a contact's name, ws197-p005 review M1).
	 */
	value[0] = '\0';
	length = strlen(name);
	at = line;
	quoted = 0;
	start = 1;
	while (*at != '\0') {
		/* A field's start outside the quotes: the one asked for. */
		if (!quoted && start) {
			same = strncmp(at, name, length);
			if (same == 0)
				break;
		}

		/* Inside a field from here on. */
		start = 0;

		/* An escape inside the quotes takes its next character along. */
		if (quoted && *at == '\\' && at[1] != '\0') {
			at += 2;
			continue;
		}

		/* The quotes open and close; a space outside them starts a field. */
		if (*at == '"') {
			quoted = !quoted;
		} else if (!quoted && *at == ' ') {
			start = 1;
		}

		/* The next character. */
		at++;
	}

	/* None. */
	if (*at == '\0')
		return 0;

	/* The value, after the name. */
	at += strlen(name);

	/* A word, up to the next space. */
	used = 0U;
	if (*at != '"') {
		while (*at != '\0' && *at != ' ') {
			if (used + 1U < size) {
				value[used] = *at;
				used++;
			}

			/* The next character. */
			at++;
		}

		/* Succeeded: the word. */
		value[used] = '\0';
		return 1;
	}

	/* A quoted value, up to its closing quote. */
	at++;
	while (*at != '\0' && *at != '"') {
		byte = (unsigned char)*at;
		at++;

		/* An escape: a quote or a backslash, or two hexadecimal digits. */
		if (byte == '\\' &&
		    (*at == '"' || *at == '\\')) {
			byte = (unsigned char)*at;
			at++;
		} else if (byte == '\\' &&
			   at[0] == 'x' &&
			   at[1] != '\0' &&
			   at[2] != '\0') {
			digits[0] = at[1];
			digits[1] = at[2];
			digits[2] = '\0';
			byte = strtoul(digits, &end, 16);
			if (end != digits + 2)
				byte = '?';
			if (byte < 0x20UL || byte == 0x7fUL)
				byte = '?';
			at += 3;
		}

		/* The byte, while there is room. */
		if (used + 1U < size) {
			value[used] = (char)byte;
			used++;
		}
	}

	/* Succeeded: the value. */
	value[used] = '\0';
	return 1;
}

/* Reads a field as an unsigned number (0 when the line has none or it is not one). */
static unsigned
phone_field_number(
	const char *line,
	const char *name)
{
	char value[24];
	unsigned long number;
	char *end;
	int found;

	/* The field. */
	found = phone_field(line, name, value, sizeof(value));
	if (!found)
		return 0U;

	/* Its number, whole. */
	number = strtoul(value, &end, 10);
	if (end == value || *end != '\0')
		return 0U;

	/* Succeeded: the number. */
	return (unsigned)number;
}

/* Gives the errno value of bluetoothd's word of a failure (section 3.5). */
static int
phone_error(
	const char *word)
{
	static const struct {
		const char *word;
		int error;
	} words[] = {
		{ "not-ready", ENOTCONN },
		{ "no-send", ENOTSUP },
		{ "request", ENOTSUP },
		{ "permission", EACCES },
		{ "owned", EACCES },
		{ "phone-seat", EACCES },
		{ "not-phone", EACCES },
		{ "argument", EINVAL },
		{ "number", EINVAL },
		{ "length", EINVAL },
		{ "busy", EBUSY },
		{ "stale-cursor", ESTALE },
		{ "stale", ESTALE },
		{ "lost", ECONNRESET },
		{ "slow", ETIMEDOUT },
		{ "timeout", ETIMEDOUT },
		{ "too-large", EMSGSIZE },
		{ "not-found", ENOENT }
	};
	size_t index;
	int same;

	/* A word of the table. */
	for (index = 0U; index < sizeof(words) / sizeof(words[0]); index++) {
		same = strcmp(word, words[index].word);
		if (same == 0)
			return words[index].error;
	}

	/* refused, unavailable and any other: the phone or bluetoothd failed. */
	return EIO;
}

/* Reads the profiles "m,c,h" (each letter when on, "-" or empty for none). */
static unsigned
phone_profiles(
	const char *text)
{
	unsigned profiles;

	/* Each letter. */
	profiles = 0U;
	for (; *text != '\0'; text++) {
		if (*text == 'm') {
			profiles |= KL_BACKEND_PHONE_PROFILE_MESSAGES;
		} else if (*text == 'c') {
			profiles |= KL_BACKEND_PHONE_PROFILE_CONTACTS;
		} else if (*text == 'h') {
			profiles |= KL_BACKEND_PHONE_PROFILE_CALLS;
		}
	}

	/* Succeeded: the bits. */
	return profiles;
}

/* Tells whether a number is one bluetoothd sends to: 1 to 32 of the digits, '+', '*' and '#'. */
static int
phone_number_valid(
	const char *to)
{
	size_t length;
	size_t span;

	/* Its length. */
	length = strlen(to);
	if (length == 0U || length >= KL_BACKEND_PHONE_NUMBER_MAX)
		return 0;

	/* Only the dialling characters. */
	span = strspn(to, "0123456789+*#");
	if (span != length)
		return 0;

	/* Succeeded: a number. */
	return 1;
}

/* Tells whether a value is one word a line can carry: printable, no space or quote, shorter than size. */
static int
phone_word_valid(
	const char *word,
	size_t size)
{
	size_t length;
	size_t index;
	unsigned char byte;

	/* Not empty and within its room. */
	length = strlen(word);
	if (length == 0U || length >= size)
		return 0;

	/* Each byte printable, and none that parts or quotes. */
	for (index = 0U; index < length; index++) {
		byte = (unsigned char)word[index];
		if (byte <= 0x20U || byte >= 0x7fU)
			return 0;
		if (byte == '"' || byte == '\\')
			return 0;
	}

	/* Succeeded: a word. */
	return 1;
}

/* Tells whether a text is an address "AA:BB:CC:DD:EE:FF". */
static int
phone_address_valid(
	const char *address)
{
	size_t length;
	size_t index;
	int hexadecimal;

	/* Seventeen characters. */
	length = strlen(address);
	if (length != 17U)
		return 0;

	/* Two hexadecimal digits, then a colon, six times. */
	for (index = 0U; index < length; index++) {
		if (index % 3U == 2U) {
			if (address[index] != ':')
				return 0;
			continue;
		}

		/* A digit. */
		hexadecimal = 0;
		if (address[index] >= '0' && address[index] <= '9')
			hexadecimal = 1;
		if (address[index] >= 'A' && address[index] <= 'F')
			hexadecimal = 1;
		if (address[index] >= 'a' && address[index] <= 'f')
			hexadecimal = 1;
		if (!hexadecimal)
			return 0;
	}

	/* Succeeded: an address. */
	return 1;
}
