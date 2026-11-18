/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The phone on zedBSD (ws197-p004a, plan/ws197/phase004/phase.md section
 * 5): bluetoothd's PHONE lines on /run/bluetoothd.sock.  ws197-p005
 * (plan/ws197/phase005/phase.md section 6.2) adds the pages of the
 * phone's contacts and calls (PHONE CONTACT with its reduced vCard, PHONE
 * CALL-LOG with no text) and the contacts' state.
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
 *
 * The connections are a thread's of their own (daemon-link-zedbsd.h): it
 * connects (which may wait), writes and reads, and reads an MMS's body
 * from its file and writes a received one to a file of its own.  The event
 * loop's calls copy what the thread published, number the requests and
 * hand them over; none of them waits.  A request the thread cannot queue
 * is answered by a result with its errno value.
 */

#include "userland/base/bluetoothd/protocol.h"
#include "userland/desktop/libmms/mms.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* The slowest a phone's Bluetooth takes an MMS's body (bytes a second): a large body's answer is due that much later. */
#define PHONE_MMS_BYTES_PER_S	16384U

/* The longest line read (bluetoothd writes 2047 bytes at most with the newline), and the longest request written. */
#define PHONE_INPUT_MAX		2048U
#define PHONE_LINE_MAX		192U

/* How many pages and other requests wait, and how many items, results and states of texts sent are kept. */
#define PHONE_PAGES_MAX		4U
#define PHONE_REQUESTS_MAX	32U
#define PHONE_ITEMS_MAX		64U
#define PHONE_RESULTS_MAX	48U
#define PHONE_SENTS_MAX		32U

/* The most changes of calls kept until they are taken (ws197-p006). */
#define PHONE_CALLS_MAX		16U

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
	unsigned reading_mime;
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
 * What waits to be taken: the items (each with the text it owns and maybe
 * a MIME descriptor), the results, the states of texts sent and the
 * calls' changes, each a ring from its head.  One lives on the thread's
 * side, one published under the link's lock and one on the event loop's
 * side; phone_kept_move carries them on.
 */
struct phone_kept {
	struct phone_kept_item items[PHONE_ITEMS_MAX];
	size_t item_head;
	size_t item_count;
	struct kl_backend_phone_result results[PHONE_RESULTS_MAX];
	size_t result_head;
	size_t result_count;
	struct phone_sent sents[PHONE_SENTS_MAX];
	size_t sent_head;
	size_t sent_count;
	struct kl_backend_phone_call calls[PHONE_CALLS_MAX];
	size_t call_head;
	size_t call_count;
};

/*
 * The thread's following of the phone: the state as last read, the three
 * connections, where the events stand and when they are asked for again,
 * when the record is read again, what waits to be taken (not published
 * yet), the hands-free unit's state as last told, the number of the last
 * request the event loop handed over that was run, and what changed since
 * the last publication (KL_BACKEND_PHONE_CHANGED_* bits).
 *
 * Only the link's thread reads and writes it, from the link's start until
 * its stop; after the stop the record's close releases what it holds.
 */
struct phone_follow {
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

	/* What waits to be taken, and the hands-free unit's state (ws197-p006). */
	struct phone_kept kept;
	struct kl_backend_phone_hf hf;

	/* The event loop's number of the last request run. */
	uint32_t ops_done;

	/* What changed since the last publication (bits), and whether something was dropped. */
	unsigned changed;
};

/*
 * What the event loop sees of the following: the state, the hands-free
 * unit's state, what waits to be taken, how full the pages' and the
 * requests' connections are, whether an MMS waits, and the event loop's
 * number of the last request the thread ran.
 */
struct phone_view {
	struct kl_backend_phone_state state;
	struct kl_backend_phone_hf hf;
	struct phone_kept kept;
	size_t pages_count;
	size_t requests_count;
	unsigned mms_waiting;
	uint32_t ops_done;
};

/* What a request handed to the thread is: a line to queue, a refresh, or an MMS to read and queue. */
#define PHONE_OP_QUEUE		1U
#define PHONE_OP_REFRESH	2U
#define PHONE_OP_MMS		3U

/*
 * One request handed to the thread: its kind, the event loop's number of
 * it (serial), the request's id, kind and line, and a text's bytes
 * (malloc'd, NULL for none) or an MMS's file (a descriptor of its own, -1
 * for none) and length.
 */
struct phone_op {
	struct kl_daemon_link_op op;
	unsigned kind;
	uint32_t serial;
	uint32_t id;
	unsigned request;
	char line[PHONE_LINE_MAX];
	uint8_t *text;
	size_t length;
	int descriptor;
};

/*
 * The following of the phone: the link (its thread and lock), the
 * thread's own record, the view the thread published with the changes the
 * event loop has not taken (both under the link's lock), and the event
 * loop's side: the state and the hands-free unit's state the last update
 * took, what waits to be taken, the text and the MIME descriptor of the
 * item taken last (freed by the next take), the next request id, the
 * number of the last request handed over, the numbers of the pages and the
 * other requests handed over and not yet run (to know the connections'
 * room), and the number of an MMS handed over and not yet run (0: none).
 *
 * It lives from kl_backend_phone_open to kl_backend_phone_close.
 */
struct kl_backend_phone {
	struct kl_daemon_link link;
	struct phone_follow inner;
	struct phone_view published;
	unsigned published_changed;

	/* The event loop's side. */
	struct kl_backend_phone_state state;
	struct kl_backend_phone_hf hf;
	struct phone_view view;
	char *taken_text;
	int taken_mime;
	uint32_t next_id;
	uint32_t serial;
	uint32_t page_serials[PHONE_PAGES_MAX];
	size_t page_serial_count;
	uint32_t request_serials[PHONE_REQUESTS_MAX];
	size_t request_serial_count;
	uint32_t mms_serial;
};

static void phone_step(void *owner);
static size_t phone_poll_fill(void *owner, struct pollfd *descriptors, size_t capacity, int *timeout);
static int phone_publish(void *owner);

/* What bluetoothd's phone connections do on their link's thread. */
static const struct kl_daemon_link_ops phone_link_ops = {
	phone_step,
	phone_poll_fill,
	phone_publish,
};

static void phone_op_run(void *owner, struct kl_daemon_link_op *op);
static void phone_op_drop(struct kl_daemon_link_op *op);
static void phone_refresh_run(struct phone_follow *phone);
static void phone_mms_run(struct phone_follow *phone, struct phone_op *request);
static int phone_submit(struct kl_backend_phone *phone, unsigned kind, unsigned request, const char *line, const uint8_t *text, size_t length, int descriptor, uint32_t *id);
static void phone_serials_settle(struct kl_backend_phone *phone);
static void phone_kept_move(struct phone_kept *to, struct phone_kept *from, unsigned *changed);
static void phone_kept_free(struct phone_kept *kept);
static void phone_follow_close(struct phone_follow *phone);
static uint64_t phone_milliseconds(void);
static void phone_connection_init(struct phone_connection *connection, struct phone_request *room, size_t capacity);
static int phone_connect(struct phone_connection *connection);
static void phone_close(struct phone_connection *connection);
static int phone_queue(struct phone_connection *connection, unsigned kind, const char *line, const uint8_t *text, size_t length, uint32_t id);
static void phone_request_drop(struct phone_connection *connection);
static void phone_fail_all(struct phone_follow *phone, struct phone_connection *connection, int error);
static void phone_start(struct phone_follow *phone, struct phone_connection *connection, uint64_t now);
static int phone_flush(struct phone_connection *connection);
static void phone_read(struct phone_follow *phone, struct phone_connection *connection);
static void phone_lost(struct phone_follow *phone, struct phone_connection *connection);
static int phone_take_input(struct phone_follow *phone, struct phone_connection *connection);
static void phone_text_take(struct phone_follow *phone, struct phone_connection *connection, size_t *taken);
static void phone_line(struct phone_follow *phone, struct phone_connection *connection, char *line);
static void phone_events_line(struct phone_follow *phone, char *line);
static void phone_answer_line(struct phone_follow *phone, struct phone_connection *connection, char *line);
static void phone_answer_done(struct phone_follow *phone, struct phone_connection *connection);
static int phone_item_line(struct phone_follow *phone, struct phone_connection *connection, const char *line, uint32_t id, unsigned what);
static void phone_message_fields(struct kl_backend_phone_item *item, const char *line);
static void phone_contact_fields(struct kl_backend_phone_item *item, const char *line);
static void phone_call_fields(struct kl_backend_phone_item *item, const char *line);
static unsigned phone_zone(const char *line);
static int phone_mime_take(struct phone_connection *connection);
static void phone_item_push(struct phone_follow *phone, struct phone_connection *connection);
static void phone_result_push(struct phone_follow *phone, const struct kl_backend_phone_result *result);
static void phone_sent_line(struct phone_follow *phone, const char *line);
static void phone_call_line(struct phone_follow *phone, const char *line);
static void phone_hf_line(struct phone_follow *phone, const char *line);
static int phone_field_int(const char *line, const char *name);
static void phone_state_line(struct phone_follow *phone, const char *line, unsigned whole);
static void phone_state_why(struct phone_follow *phone, const char *why);
static void phone_contacts_line(struct phone_follow *phone, const char *line);
static void phone_contacts_clear(struct phone_follow *phone);
static void phone_events_start(struct phone_follow *phone, uint64_t now);
static void phone_show_due(struct phone_follow *phone, uint64_t now);
static int phone_field(const char *line, const char *name, char *value, size_t size);
static unsigned phone_field_number(const char *line, const char *name);
static int phone_error(const char *word);
static unsigned phone_profiles(const char *text);
static int phone_number_valid(const char *to);
static int phone_word_valid(const char *word, size_t size);
static int phone_address_valid(const char *address);

/*
 * Starts following the phone; the link's thread asks for the events at
 * once.  Returns NULL only without memory (or without a thread).
 */
struct kl_backend_phone *
kl_backend_phone_open(
	void)
{
	struct kl_backend_phone *phone;
	int error;

	/* The record, with no connection. */
	phone = calloc(1, sizeof(*phone));
	if (phone == NULL)
		return NULL;

	/* The connections: the events take no requests, the pages four, the other requests thirty-two. */
	phone_connection_init(&phone->inner.events, NULL, 0U);
	phone_connection_init(&phone->inner.pages, phone->inner.page_room, PHONE_PAGES_MAX);
	phone_connection_init(&phone->inner.requests, phone->inner.request_room, PHONE_REQUESTS_MAX);

	/* Unreachable until the events are asked for; ids start at 1 (0 is a message that came by itself). */
	(void)snprintf(phone->inner.state.why, sizeof(phone->inner.state.why), "%s", "unreachable");
	phone->inner.hf.speaker = -1;
	phone->published.state = phone->inner.state;
	phone->published.hf = phone->inner.hf;
	phone->view.state = phone->inner.state;
	phone->view.hf = phone->inner.hf;
	phone->state = phone->inner.state;
	phone->hf = phone->inner.hf;
	phone->next_id = 1U;
	phone->taken_mime = -1;

	/* The thread that asks for the events, writes the requests and reads. */
	error = kl_daemon_link_start(&phone->link, "phone", &phone_link_ops, phone);
	if (error != 0) {
		free(phone);
		return NULL;
	}

	/* Succeeded: the thread asks for the events. */
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
	/* Nothing to close. */
	if (phone == NULL)
		return;

	/* The thread ends; what it held is this thread's to release. */
	kl_daemon_link_stop(&phone->link);
	phone_follow_close(&phone->inner);

	/* What was published and what waits to be taken, with the texts and descriptors they own. */
	phone_kept_free(&phone->published.kept);
	phone_kept_free(&phone->view.kept);

	/* Closes the descriptor whose previous take still borrowed it. */
	if (phone->taken_mime >= 0)
		close(phone->taken_mime);
	free(phone->taken_text);

	/* The record. */
	free(phone);
}

/*
 * Takes what the link's thread found since the last update: the state,
 * the hands-free unit's, the items, results, states of texts sent and
 * calls' changes.
 */
int
kl_backend_phone_update(
	struct kl_backend_phone *phone,
	unsigned *changed)
{
	/* A record and somewhere to say what changed. */
	if (phone == NULL || changed == NULL)
		return EINVAL;

	/* Copies the published view, moves what waits to be taken here, and takes the changes. */
	kl_daemon_link_lock(&phone->link);

	*changed = phone->published_changed;
	phone->published_changed = 0U;
	phone_kept_move(&phone->view.kept, &phone->published.kept, changed);
	phone->view.state = phone->published.state;
	phone->view.hf = phone->published.hf;
	phone->view.pages_count = phone->published.pages_count;
	phone->view.requests_count = phone->published.requests_count;
	phone->view.mms_waiting = phone->published.mms_waiting;
	phone->view.ops_done = phone->published.ops_done;

	kl_daemon_link_unlock(&phone->link);

	/* The event loop's copies, and the requests the thread has run since. */
	phone->state = phone->view.state;
	phone->hf = phone->view.hf;
	phone_serials_settle(phone);

	/* What waits is told again until it is taken. */
	if (phone->view.kept.item_count > 0U)
		*changed |= KL_BACKEND_PHONE_CHANGED_ITEM;
	if (phone->view.kept.result_count > 0U)
		*changed |= KL_BACKEND_PHONE_CHANGED_RESULT;
	if (phone->view.kept.sent_count > 0U)
		*changed |= KL_BACKEND_PHONE_CHANGED_SENT;

	/* Succeeded: what changed, told once. */
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

	/* Succeeded: the copy the last update took. */
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

	/* The messages, the contacts or the calls. */
	if (what > KL_BACKEND_PHONE_WHAT_CALLS)
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

	/*
	 * The line: since for the messages and the calls, a limit for the
	 * messages alone (bluetoothd refuses the others'), the cursor only
	 * when there is one.
	 */
	if (what == KL_BACKEND_PHONE_WHAT_CONTACTS && cursor[0] != '\0') {
		(void)snprintf(line, sizeof(line), "PHONE PAGE contacts cursor=%s count=%u\n", cursor, count);
	} else if (what == KL_BACKEND_PHONE_WHAT_CONTACTS) {
		(void)snprintf(line, sizeof(line), "PHONE PAGE contacts count=%u\n", count);
	} else if (what == KL_BACKEND_PHONE_WHAT_CALLS && cursor[0] != '\0') {
		(void)snprintf(line, sizeof(line), "PHONE PAGE calls since=%lld cursor=%s count=%u\n", (long long)since, cursor, count);
	} else if (what == KL_BACKEND_PHONE_WHAT_CALLS) {
		(void)snprintf(line, sizeof(line), "PHONE PAGE calls since=%lld count=%u\n", (long long)since, count);
	} else if (cursor[0] != '\0') {
		(void)snprintf(line, sizeof(line), "PHONE PAGE messages since=%lld limit=%u cursor=%s count=%u\n", (long long)since, limit, cursor, count);
	} else {
		(void)snprintf(line, sizeof(line), "PHONE PAGE messages since=%lld limit=%u count=%u\n", (long long)since, limit, count);
	}

	/* Waits its turn on the pages' connection. */
	error = phone_submit(phone, PHONE_OP_QUEUE, PHONE_KIND_PAGE, line, NULL, 0U, -1, id);
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
	error = phone_submit(phone, PHONE_OP_QUEUE, PHONE_KIND_READ, line, NULL, 0U, -1, id);
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
	error = phone_submit(phone, PHONE_OP_QUEUE, PHONE_KIND_SEND, line, text, length, -1, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Sends an MMS (ws197-p010): bluetoothd pushes its MIME body, read whole
 * by the link's thread from the caller's regular file, to the phone's
 * outbox as a bMessage of type MMS.  One MMS waits at a time.
 */
int
kl_backend_phone_send_mms(
	struct kl_backend_phone *phone,
	const char *to,
	int descriptor,
	uint32_t *id)
{
	struct stat status;
	char line[PHONE_LINE_MAX];
	size_t length;
	int valid;
	int regular;
	int own;
	int error;

	/* A record, a number, a descriptor and somewhere for the id. */
	if (phone == NULL || id == NULL || to == NULL || descriptor < 0)
		return EINVAL;
	valid = phone_number_valid(to);
	if (!valid)
		return EINVAL;

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* One MMS waits or is written at a time: its body may be megabytes. */
	if (phone->view.mms_waiting || phone->mms_serial != 0U)
		return EBUSY;

	/* A regular file (a pipe could block the thread for ever), of a length bluetoothd takes. */
	error = fstat(descriptor, &status);
	if (error != 0)
		return errno;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return EINVAL;
	if (status.st_size <= 0)
		return EINVAL;
	if ((uintmax_t)status.st_size > KL_BACKEND_PHONE_MIME_MAX)
		return EMSGSIZE;
	length = (size_t)status.st_size;

	/* The thread's own descriptor of the file, read from its start whatever its offset. */
	own = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
	if (own < 0)
		return errno;

	/* Waits its turn on the requests' connection, the line asking for an MMS of that length. */
	(void)snprintf(line, sizeof(line), "PHONE SEND to=\"%s\" type=mms length=%lu\n", to, (unsigned long)length);
	error = phone_submit(phone, PHONE_OP_MMS, PHONE_KIND_SEND, line, NULL, length, own, id);
	if (error != 0) {
		(void)close(own);
		return error;
	}

	/* Succeeded: asked. */
	return 0;
}

/*
 * Turns the phone's switch on with its profiles, or off keeping them
 * (bluetoothd's PHONE LINK); the record is read again once it is
 * answered.
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

	/*
	 * The line.  Off names no profiles: bluetoothd keeps the record's as
	 * they are, where an empty profiles= would clear them (ws197-p005
	 * review-3 minor 7).
	 */
	if (on) {
		(void)snprintf(line, sizeof(line), "PHONE LINK %s on profiles=%s\n", address, letters);
	} else {
		(void)snprintf(line, sizeof(line), "PHONE LINK %s off\n", address);
	}

	/* The line waits on the requests' connection. */
	error = phone_submit(phone, PHONE_OP_QUEUE, PHONE_KIND_LINK, line, NULL, 0U, -1, id);
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
	struct phone_kept *view;

	/* A record and somewhere for the item. */
	if (phone == NULL || item == NULL)
		return 0;
	view = &phone->view.kept;

	/* The text of the item taken last goes now. */
	free(phone->taken_text);
	phone->taken_text = NULL;
	if (phone->taken_mime >= 0)
		close(phone->taken_mime);
	phone->taken_mime = -1;

	/* None waits. */
	if (view->item_count == 0U)
		return 0;

	/* The oldest, whose text is now the one taken last. */
	kept = &view->items[view->item_head];
	*item = kept->item;
	item->text = kept->text;
	phone->taken_text = kept->text;
	kept->text = NULL;
	if (kept->item.has_mime)
		phone->taken_mime = kept->item.mime_descriptor;
	kept->item.has_mime = 0U;
	view->item_head = (view->item_head + 1U) % PHONE_ITEMS_MAX;
	view->item_count--;

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
	struct phone_kept *view;

	/* A record, somewhere for it, and one waiting. */
	if (phone == NULL || result == NULL)
		return 0;
	view = &phone->view.kept;
	if (view->result_count == 0U)
		return 0;

	/* The oldest. */
	*result = view->results[view->result_head];
	view->result_head = (view->result_head + 1U) % PHONE_RESULTS_MAX;
	view->result_count--;

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
	struct phone_kept *view;

	/* A record, somewhere for it, and one waiting. */
	if (phone == NULL || sent_request == NULL || state == NULL)
		return 0;
	view = &phone->view.kept;
	if (view->sent_count == 0U)
		return 0;

	/* The oldest. */
	*sent_request = view->sents[view->sent_head].request;
	*state = view->sents[view->sent_head].state;
	view->sent_head = (view->sent_head + 1U) % PHONE_SENTS_MAX;
	view->sent_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Asks something of the calls (ws197-p006): bluetoothd's PHONE DIAL,
 * ANSWER, HANGUP, HOLD, DTMF, VOLUME or VOICE, answered DONE or ERROR.
 */
int
kl_backend_phone_call_ask(
	struct kl_backend_phone *phone,
	unsigned ask,
	const char *argument,
	uint32_t *id)
{
	static const char *const words[] = { NULL, "DIAL", "ANSWER", "HANGUP", "HOLD", "DTMF", "VOLUME", "VOICE" };
	static const char *const keys[] = { NULL, "to", NULL, NULL, "action", "key", "speaker", "on" };
	char line[PHONE_LINE_MAX];
	size_t span;
	int error;

	/* A record, a request known, and somewhere for the id. */
	if (phone == NULL || id == NULL)
		return EINVAL;
	if (ask < KL_BACKEND_PHONE_ASK_DIAL || ask > KL_BACKEND_PHONE_ASK_VOICE)
		return EINVAL;

	/* The argument the request takes: short, of the letters a number, a key or a level has. */
	if (keys[ask] != NULL) {
		if (argument == NULL || argument[0] == '\0')
			return EINVAL;
		span = strspn(argument, "0123456789+*#ABCD");
		if (argument[span] != '\0' || span > 32U)
			return EINVAL;
	}

	/* bluetoothd answers. */
	if (!phone->state.reachable)
		return ENOTCONN;

	/* The line: the word, then the argument quoted. */
	if (keys[ask] != NULL)
		(void)snprintf(line, sizeof(line), "PHONE %s %s=\"%s\"\n", words[ask], keys[ask], argument);
	else
		(void)snprintf(line, sizeof(line), "PHONE %s\n", words[ask]);

	/* Waits its turn on the requests' connection. */
	error = phone_submit(phone, PHONE_OP_QUEUE, PHONE_KIND_READ, line, NULL, 0U, -1, id);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Takes the oldest change of a call (ws197-p006).
 */
int
kl_backend_phone_take_call(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_call *call)
{
	struct phone_kept *view;

	/* A record, somewhere for it, and one waiting. */
	if (phone == NULL || call == NULL)
		return 0;
	view = &phone->view.kept;
	if (view->call_count == 0U)
		return 0;

	/* The oldest. */
	*call = view->calls[view->call_head];
	view->call_head = (view->call_head + 1U) % PHONE_CALLS_MAX;
	view->call_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Copies the hands-free unit's state as last told (ws197-p006).
 */
void
kl_backend_phone_get_hf(
	const struct kl_backend_phone *phone,
	struct kl_backend_phone_hf *hf)
{
	/* The copy the last update took. */
	*hf = phone->hf;
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

	/* Hands the refresh to the thread (one lost without memory leaves the next reading to its time). */
	(void)phone_submit(phone, PHONE_OP_REFRESH, 0U, "", NULL, 0U, -1, NULL);
}

/* Steps the following on the link's thread: reads each connection, asks for the events and reads the record when due, and writes the next requests. */
static void
phone_step(
	void *owner)
{
	struct kl_backend_phone *record;
	struct phone_follow *phone;
	uint64_t now;

	/* The thread's own part of the record. */
	record = owner;
	phone = &record->inner;
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
}

/* Gives the connections to wait for (to write, too, while bytes wait), and the wait until what is due next. */
static size_t
phone_poll_fill(
	void *owner,
	struct pollfd *descriptors,
	size_t capacity,
	int *timeout)
{
	struct kl_backend_phone *record;
	struct phone_connection *connections[3];
	struct phone_connection *connection;
	struct phone_follow *phone;
	uint64_t due;
	uint64_t now;
	size_t count;
	size_t index;

	/* The thread's own part of the record. */
	record = owner;
	phone = &record->inner;
	now = PHONE_NOW_MS();

	/* What is due next: the events, the record's reading. */
	due = UINT64_MAX;
	if (phone->events.socket < 0)
		due = phone->events_due_ms;
	if (phone->events_stand != PHONE_EVENTS_FOLLOWED && phone->state.reachable && phone->show_due_ms < due)
		due = phone->show_due_ms;

	/* Each connection: an answer's time, a request to write, and its descriptor. */
	connections[0] = &phone->events;
	connections[1] = &phone->pages;
	connections[2] = &phone->requests;
	count = 0U;
	for (index = 0U; index < 3U; index++) {
		connection = connections[index];

		/* An answer overdue is given up at its time; a request waiting is written at once. */
		if (connection->written && connection->due_ms < due)
			due = connection->due_ms;
		if (!connection->written && connection->count > 0U)
			due = now;

		/* No connection, or no room for it. */
		if (connection->socket < 0 || count >= capacity)
			continue;

		/* Its lines, and the room to write the bytes that wait. */
		descriptors[count].fd = connection->socket;
		descriptors[count].events = POLLIN;
		if (connection->output != NULL && connection->output_sent < connection->output_length)
			descriptors[count].events |= POLLOUT;
		descriptors[count].revents = 0;
		count++;
	}

	/* Lowers the wait to what is due. */
	if (due <= now) {
		*timeout = 0;
	} else if (due - now < (uint64_t)*timeout) {
		*timeout = (int)(due - now);
	}

	/* Succeeded: the descriptors to wait for. */
	return count;
}

/* Publishes the state, what waits to be taken and the connections' room to the event loop's side (under the link's lock); 1 when something changed. */
static int
phone_publish(
	void *owner)
{
	struct kl_backend_phone *record;
	struct phone_follow *phone;
	struct phone_view *published;
	unsigned mms_waiting;
	size_t index;

	/* The thread's own part of the record, and the view it publishes. */
	record = owner;
	phone = &record->inner;
	published = &record->published;

	/* Whether an MMS waits or is written (its body is longer than any text). */
	mms_waiting = 0U;
	for (index = 0U; index < phone->requests.count; index++) {
		if (phone->requests.requests[index].length > KL_BACKEND_PHONE_SEND_MAX)
			mms_waiting = 1U;
	}

	/* Nothing new: no wake. */
	if (phone->changed == 0U &&
	    phone->ops_done == published->ops_done &&
	    phone->pages.count == published->pages_count &&
	    phone->requests.count == published->requests_count &&
	    mms_waiting == published->mms_waiting)
		return 0;

	/* The state, the hands-free unit's, the connections' room and the requests run. */
	published->state = phone->state;
	published->hf = phone->hf;
	published->pages_count = phone->pages.count;
	published->requests_count = phone->requests.count;
	published->mms_waiting = mms_waiting;
	published->ops_done = phone->ops_done;

	/* What waits to be taken, after what is not taken yet, and the changes. */
	record->published_changed |= phone->changed;
	phone->changed = 0U;
	phone_kept_move(&published->kept, &phone->kept, &record->published_changed);

	/* Succeeded: the event loop has something to take. */
	return 1;
}

/* Runs a request on the link's thread: queues its line, reads and queues an MMS, or refreshes; one that cannot be queued is answered. */
static void
phone_op_run(
	void *owner,
	struct kl_daemon_link_op *op)
{
	struct kl_backend_phone_result result;
	struct kl_backend_phone *record;
	struct phone_connection *connection;
	struct phone_follow *phone;
	struct phone_op *request;
	int error;

	/* The thread's own part of the record, and the request. */
	record = owner;
	phone = &record->inner;
	request = (struct phone_op *)op;

	/* A refresh: the record and the events again. */
	if (request->kind == PHONE_OP_REFRESH) {
		phone_refresh_run(phone);
	} else if (request->kind == PHONE_OP_MMS) {
		/* An MMS: its body read from its file, then queued. */
		phone_mms_run(phone, request);
	} else {
		/* A line (and a text) on its connection; one that finds no room is answered at once. */
		connection = &phone->requests;
		if (request->request == PHONE_KIND_PAGE)
			connection = &phone->pages;
		error = phone_queue(connection, request->request, request->line, request->text, request->length, request->id);
		if (error != 0) {
			memset(&result, 0, sizeof(result));
			result.id = request->id;
			result.error = error;
			phone_result_push(phone, &result);
		}
	}

	/* The request is run; what it owned goes. */
	phone->ops_done = request->serial;
	phone_op_drop(op);
}

/* Reads the record at the next step and asks for the events again unless they are followed (bluetoothd closes them for an owner no longer). */
static void
phone_refresh_run(
	struct phone_follow *phone)
{
	/* The record at the next step. */
	phone->show_due_ms = 0U;

	/* The events again, unless they are followed. */
	if (phone->events_stand != PHONE_EVENTS_FOLLOWED) {
		phone_close(&phone->events);
		phone->events_stand = PHONE_EVENTS_NONE;
		phone->events_due_ms = 0U;
	}
}

/* Releases what a request handed to the thread owns: a text's bytes, an MMS's file. */
static void
phone_op_drop(
	struct kl_daemon_link_op *op)
{
	struct phone_op *request;

	/* The request. */
	request = (struct phone_op *)op;

	/* Its text and its file. */
	free(request->text);
	request->text = NULL;
	if (request->descriptor >= 0)
		(void)close(request->descriptor);
	request->descriptor = -1;
}

/* Reads an MMS's body whole from its file on the link's thread and queues it after its line; one that cannot be is answered. */
static void
phone_mms_run(
	struct phone_follow *phone,
	struct phone_op *request)
{
	struct kl_backend_phone_result result;
	struct phone_request *queued;
	uint8_t *mime;
	size_t index;
	size_t done;
	ssize_t got;
	int error;

	/* One MMS waits or is written at a time. */
	error = 0;
	for (index = 0U; index < phone->requests.count; index++) {
		if (phone->requests.requests[index].length > KL_BACKEND_PHONE_SEND_MAX)
			error = EBUSY;
	}

	/* The body's own copy, read from the file's start whatever its offset. */
	mime = NULL;
	if (error == 0) {
		mime = malloc(request->length);
		if (mime == NULL)
			error = ENOMEM;
	}

	/* Reads the file by position until all of its bytes are in. */
	done = 0U;
	while (error == 0 && done < request->length) {
		got = pread(request->descriptor, mime + done, request->length - done, (off_t)done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			error = EIO;
			break;
		}

		/* Read so far. */
		done += (size_t)got;
	}

	/* Waits its turn on the requests' connection, the line asking for an MMS of that length. */
	if (error == 0)
		error = phone_queue(&phone->requests, PHONE_KIND_SEND, request->line, NULL, 0U, request->id);

	/* One that could not be read or queued is answered at once. */
	if (error != 0) {
		free(mime);
		memset(&result, 0, sizeof(result));
		result.id = request->id;
		result.error = error;
		phone_result_push(phone, &result);
		return;
	}

	/* The request queued last owns the body, sent after its line (not copied again). */
	queued = &phone->requests.requests[phone->requests.count - 1U];
	queued->text = mime;
	queued->length = request->length;
}

/*
 * Numbers a request and hands it to the link's thread (with a copy of its
 * text, and owning the MMS's descriptor once it succeeds); 0, EBUSY when
 * its connection's waiting ones are full, or ENOMEM.
 */
static int
phone_submit(
	struct kl_backend_phone *phone,
	unsigned kind,
	unsigned request,
	const char *line,
	const uint8_t *text,
	size_t length,
	int descriptor,
	uint32_t *id)
{
	struct phone_op *asked;
	size_t waiting;

	/* The requests the thread has run since the last look. */
	phone_serials_settle(phone);

	/* Room among a connection's waiting ones, counting those handed over and not run yet. */
	if (kind != PHONE_OP_REFRESH && request == PHONE_KIND_PAGE) {
		waiting = phone->view.pages_count + phone->page_serial_count;
		if (waiting >= PHONE_PAGES_MAX)
			return EBUSY;
	} else if (kind != PHONE_OP_REFRESH) {
		waiting = phone->view.requests_count + phone->request_serial_count;
		if (waiting >= PHONE_REQUESTS_MAX)
			return EBUSY;
	}

	/* The request, the link's from the submit on. */
	asked = calloc(1U, sizeof(*asked));
	if (asked == NULL)
		return ENOMEM;
	asked->descriptor = -1;

	/* A text's own copy. */
	if (text != NULL && length > 0U) {
		asked->text = malloc(length);
		if (asked->text == NULL) {
			free(asked);
			return ENOMEM;
		}

		/* Copies the text the request carries. */
		memcpy(asked->text, text, length);
	}

	/* The event loop's number of it (never 0, the number of none run). */
	phone->serial++;
	if (phone->serial == 0U)
		phone->serial = 1U;

	/* Its id (a refresh has none; the others never 0). */
	if (kind != PHONE_OP_REFRESH) {
		asked->id = phone->next_id;
		phone->next_id++;
		if (phone->next_id == 0U)
			phone->next_id = 1U;
	}

	/* What it is, and where it waits until the thread has run it. */
	asked->op.run = phone_op_run;
	asked->op.drop = phone_op_drop;
	asked->kind = kind;
	asked->serial = phone->serial;
	asked->request = request;
	(void)snprintf(asked->line, sizeof(asked->line), "%s", line);
	asked->length = length;
	asked->descriptor = descriptor;
	if (kind != PHONE_OP_REFRESH && request == PHONE_KIND_PAGE) {
		phone->page_serials[phone->page_serial_count] = phone->serial;
		phone->page_serial_count++;
	} else if (kind != PHONE_OP_REFRESH) {
		phone->request_serials[phone->request_serial_count] = phone->serial;
		phone->request_serial_count++;
	}

	/* Remembers which request is the MMS one. */
	if (kind == PHONE_OP_MMS)
		phone->mms_serial = phone->serial;

	/* Gives the caller the request's number. */
	if (id != NULL)
		*id = asked->id;

	/* Hands the request to the thread. */
	kl_daemon_link_submit(&phone->link, &asked->op);

	/* Succeeded: asked. */
	return 0;
}

/* Forgets the numbers of the requests handed over that the thread has run, as the last update saw. */
static void
phone_serials_settle(
	struct kl_backend_phone *phone)
{
	size_t kept;
	size_t index;

	/* The pages not run yet (the numbers grow, so the run ones are the first). */
	kept = 0U;
	for (index = 0U; index < phone->page_serial_count; index++) {
		if (phone->page_serials[index] <= phone->view.ops_done)
			continue;
		phone->page_serials[kept] = phone->page_serials[index];
		kept++;
	}

	/* Keeps only the pages not run yet. */
	phone->page_serial_count = kept;

	/* The other requests not run yet. */
	kept = 0U;
	for (index = 0U; index < phone->request_serial_count; index++) {
		if (phone->request_serials[index] <= phone->view.ops_done)
			continue;
		phone->request_serials[kept] = phone->request_serials[index];
		kept++;
	}

	/* Keeps only the requests not run yet. */
	phone->request_serial_count = kept;

	/* An MMS run is the thread's to count from now on. */
	if (phone->mms_serial != 0U && phone->mms_serial <= phone->view.ops_done)
		phone->mms_serial = 0U;
}

/* Moves what waits to be taken from one side to the next, after what is there; the oldest of a full ring goes (an item's with its text, CHANGED_DROPPED). */
static void
phone_kept_move(
	struct phone_kept *to,
	struct phone_kept *from,
	unsigned *changed)
{
	struct phone_kept_item *dropped;
	size_t slot;

	/* The items, the oldest first. */
	while (from->item_count > 0U) {
		if (to->item_count == PHONE_ITEMS_MAX) {
			dropped = &to->items[to->item_head];
			if (dropped->item.has_mime)
				close(dropped->item.mime_descriptor);
			free(dropped->text);
			dropped->text = NULL;
			to->item_head = (to->item_head + 1U) % PHONE_ITEMS_MAX;
			to->item_count--;
			*changed |= KL_BACKEND_PHONE_CHANGED_DROPPED;
		}

		/* Moves the oldest item of the source onto the end of the ring, and hands its text over. */
		slot = (to->item_head + to->item_count) % PHONE_ITEMS_MAX;
		to->items[slot] = from->items[from->item_head];
		from->items[from->item_head].text = NULL;
		from->items[from->item_head].item.has_mime = 0U;
		to->item_count++;
		from->item_head = (from->item_head + 1U) % PHONE_ITEMS_MAX;
		from->item_count--;
	}

	/* The results. */
	while (from->result_count > 0U) {
		if (to->result_count == PHONE_RESULTS_MAX) {
			to->result_head = (to->result_head + 1U) % PHONE_RESULTS_MAX;
			to->result_count--;
		}

		/* Moves the oldest result of the source onto the end of the ring. */
		slot = (to->result_head + to->result_count) % PHONE_RESULTS_MAX;
		to->results[slot] = from->results[from->result_head];
		to->result_count++;
		from->result_head = (from->result_head + 1U) % PHONE_RESULTS_MAX;
		from->result_count--;
	}

	/* The states of texts sent. */
	while (from->sent_count > 0U) {
		if (to->sent_count == PHONE_SENTS_MAX) {
			to->sent_head = (to->sent_head + 1U) % PHONE_SENTS_MAX;
			to->sent_count--;
		}

		/* Moves the oldest sent record of the source onto the end of the ring. */
		slot = (to->sent_head + to->sent_count) % PHONE_SENTS_MAX;
		to->sents[slot] = from->sents[from->sent_head];
		to->sent_count++;
		from->sent_head = (from->sent_head + 1U) % PHONE_SENTS_MAX;
		from->sent_count--;
	}

	/* The calls' changes. */
	while (from->call_count > 0U) {
		if (to->call_count == PHONE_CALLS_MAX) {
			to->call_head = (to->call_head + 1U) % PHONE_CALLS_MAX;
			to->call_count--;
		}

		/* Moves the oldest call of the source onto the end of the ring. */
		slot = (to->call_head + to->call_count) % PHONE_CALLS_MAX;
		to->calls[slot] = from->calls[from->call_head];
		to->call_count++;
		from->call_head = (from->call_head + 1U) % PHONE_CALLS_MAX;
		from->call_count--;
	}
}

/* Releases the texts and the MIME descriptors of the items a side holds. */
static void
phone_kept_free(
	struct phone_kept *kept)
{
	struct phone_kept_item *item;
	size_t index;

	/* Each item not taken. */
	for (index = 0U; index < kept->item_count; index++) {
		item = &kept->items[(kept->item_head + index) % PHONE_ITEMS_MAX];
		free(item->text);
		item->text = NULL;
		if (item->item.has_mime)
			close(item->item.mime_descriptor);
		item->item.has_mime = 0U;
	}

	/* Empties the ring, whose items are all released. */
	kept->item_count = 0U;
}

/* Closes the thread's connections and drops what they and the thread's side hold (after the thread ended). */
static void
phone_follow_close(
	struct phone_follow *phone)
{
	/* Each connection with what it holds. */
	phone_close(&phone->events);
	phone_close(&phone->pages);
	phone_close(&phone->requests);

	/* The texts of the requests waiting. */
	while (phone->pages.count > 0U)
		phone_request_drop(&phone->pages);
	while (phone->requests.count > 0U)
		phone_request_drop(&phone->requests);

	/* The items not published. */
	phone_kept_free(&phone->kept);
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
 * Puts a request at the end of a connection's waiting ones, with the id
 * the event loop gave it (0 for SHOW, which this asks for itself).
 * Returns 0, ENOMEM, or EBUSY when they are full.
 */
static int
phone_queue(
	struct phone_connection *connection,
	unsigned kind,
	const char *line,
	const uint8_t *text,
	size_t length,
	uint32_t id)
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

	/* The kind, the line and the id. */
	request->kind = kind;
	(void)snprintf(request->line, sizeof(request->line), "%s", line);
	request->id = id;

	/* Succeeded: waiting. */
	connection->count++;
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
	struct phone_follow *phone,
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
	struct phone_follow *phone,
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

	/* Written: its answer is due in two minutes (and an MMS's body's time over the radio), gathered from nothing. */
	connection->written = 1U;
	connection->due_ms = now + PHONE_ANSWER_MS;
	if (request->length > KL_BACKEND_PHONE_SEND_MAX)
		connection->due_ms += (uint64_t)request->length * 1000U / PHONE_MMS_BYTES_PER_S;
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
	struct phone_follow *phone,
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
	struct phone_follow *phone,
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

		/* The hands-free unit is not known any more: off until the events tell it again (with its calls). */
		memset(&phone->hf, 0, sizeof(phone->hf));
		phone->hf.speaker = -1;
		phone->changed |= KL_BACKEND_PHONE_CHANGED_HF;

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
	struct phone_follow *phone,
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
	struct phone_follow *phone,
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
	struct phone_follow *phone,
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
	struct phone_follow *phone,
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
		(void)phone_item_line(phone, &phone->events, line, 0U, KL_BACKEND_PHONE_WHAT_MESSAGES);
		return;
	}

	/* A sent text's state. */
	same = strncmp(line, "PHONE SENT ", 11U);
	if (same == 0) {
		phone_sent_line(phone, line);
		return;
	}

	/* A call's change (ws197-p006). */
	same = strncmp(line, "PHONE CALL ", 11U);
	if (same == 0) {
		phone_call_line(phone, line);
		return;
	}

	/* The hands-free unit's state (ws197-p006). */
	same = strncmp(line, "PHONE HF ", 9U);
	if (same == 0) {
		phone_hf_line(phone, line);
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
 * Takes PHONE CALL index=N state=W dir=in|out number="..." name="..."
 * (ws197-p006): a change of a call, kept for the caller (the oldest goes
 * when the ring is full).
 */
static void
phone_call_line(
	struct phone_follow *phone,
	const char *line)
{
	static const char *const states[] = { "", "incoming", "dialing", "alerting", "active", "held", "waiting", "ended" };
	struct kl_backend_phone_call *call;
	char value[16];
	unsigned state;
	unsigned index;
	size_t slot;
	int same;

	/* The state's word. */
	(void)phone_field(line, "state=", value, sizeof(value));
	state = 0U;
	for (index = KL_BACKEND_PHONE_CALL_INCOMING; index <= KL_BACKEND_PHONE_CALL_ENDED; index++) {
		same = strcmp(value, states[index]);
		if (same == 0)
			state = index;
	}

	/* Another word is not a call's. */
	if (state == 0U)
		return;

	/* No room: the oldest goes. */
	if (phone->kept.call_count == PHONE_CALLS_MAX) {
		phone->kept.call_head = (phone->kept.call_head + 1U) % PHONE_CALLS_MAX;
		phone->kept.call_count--;
	}

	/* The change for the caller. */
	slot = (phone->kept.call_head + phone->kept.call_count) % PHONE_CALLS_MAX;
	call = &phone->kept.calls[slot];
	memset(call, 0, sizeof(*call));
	call->index = phone_field_number(line, "index=");
	call->state = state;
	(void)phone_field(line, "dir=", value, sizeof(value));
	same = strcmp(value, "out");
	if (same == 0)
		call->outgoing = 1U;
	(void)phone_field(line, "number=", call->number, sizeof(call->number));
	(void)phone_field(line, "name=", call->name, sizeof(call->name));
	phone->kept.call_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_CALL;
}

/*
 * Takes PHONE HF state=W why=W service=N signal=N battery=N roam=N
 * speaker=N voice=N hold=N (ws197-p006): the hands-free unit's state.
 */
static void
phone_hf_line(
	struct phone_follow *phone,
	const char *line)
{
	char value[16];
	unsigned number;
	int same;

	/* The state's word (off for another). */
	(void)phone_field(line, "state=", value, sizeof(value));
	phone->hf.state = KL_BACKEND_PHONE_HF_OFF;
	same = strcmp(value, "connecting");
	if (same == 0)
		phone->hf.state = KL_BACKEND_PHONE_HF_CONNECTING;
	same = strcmp(value, "ready");
	if (same == 0)
		phone->hf.state = KL_BACKEND_PHONE_HF_READY;
	same = strcmp(value, "failed");
	if (same == 0)
		phone->hf.state = KL_BACKEND_PHONE_HF_FAILED;

	/* The values. */
	phone->hf.service = phone_field_int(line, "service=");
	phone->hf.signal = phone_field_int(line, "signal=");
	phone->hf.battery = phone_field_int(line, "battery=");
	phone->hf.roam = phone_field_int(line, "roam=");
	phone->hf.speaker = phone_field_int(line, "speaker=");
	number = phone_field_number(line, "voice=");
	phone->hf.voice = 0U;
	if (number != 0U)
		phone->hf.voice = 1U;
	number = phone_field_number(line, "hold=");
	phone->hf.hold = 0U;
	if (number != 0U)
		phone->hf.hold = 1U;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_HF;
}

/* Reads a field as a signed decimal number (0 when it is not one). */
static int
phone_field_int(
	const char *line,
	const char *name)
{
	char value[16];
	char *end;
	long number;

	/* The field. */
	(void)phone_field(line, name, value, sizeof(value));
	number = strtol(value, &end, 10);
	if (end == value || *end != '\0')
		return 0;

	/* Succeeded: the number. */
	return (int)number;
}

/*
 * Takes a line of an answer to a page or a request: the items (messages,
 * contacts and calls), the page's end, the text's number, the record
 * (SHOW), the failure and the DONE.
 */
static void
phone_answer_line(
	struct phone_follow *phone,
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
		(void)phone_item_line(phone, connection, line, answer->id, KL_BACKEND_PHONE_WHAT_MESSAGES);
		return;
	}

	/* One contact of a page (its reduced vCard follows). */
	same = strncmp(line, "PHONE CONTACT ", 14U);
	if (same == 0) {
		(void)phone_item_line(phone, connection, line, answer->id, KL_BACKEND_PHONE_WHAT_CONTACTS);
		return;
	}

	/* One call of a page (length=0: nothing follows). */
	same = strncmp(line, "PHONE CALL-LOG ", 15U);
	if (same == 0) {
		(void)phone_item_line(phone, connection, line, answer->id, KL_BACKEND_PHONE_WHAT_CALLS);
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
	struct phone_follow *phone,
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
			phone_contacts_clear(phone);
			phone_state_why(phone, "no-record");
		}

		/*
		 * Whether there is a record is known once SHOW answered without
		 * a failure (ws197-p005 review-3 R2): until then a record not
		 * seen is not read as none, which would forget the phone's copy.
		 */
		if (connection->answer.error == 0)
			phone->state.record_known = 1U;

		/* Answered: the next may be written. */
		phone_request_drop(connection);
		return;
	}

	/* The result for the caller. */
	phone_result_push(phone, &connection->answer);

	/* The switch changed: the record and the events at once. */
	if (kind == PHONE_KIND_LINK && connection->answer.error == 0)
		phone_refresh_run(phone);

	/* The next may be written. */
	phone_request_drop(connection);
}

/*
 * Takes a PHONE MESSAGE, PHONE CONTACT or PHONE CALL-LOG line (what says
 * which): the item's fields, then its text of length bytes is read before
 * the next line.  Returns 0, or EPROTO (no length, or a text longer than
 * bluetoothd sends) or ENOMEM, the connection then being made again.
 */
static int
phone_item_line(
	struct phone_follow *phone,
	struct phone_connection *connection,
	const char *line,
	uint32_t id,
	unsigned what)
{
	struct kl_backend_phone_item *item;
	char value[KL_BACKEND_PHONE_PEER_MAX];
	unsigned long length;
	unsigned long maximum;
	char *end;
	int found;
	int same;

	/* The item, numbered by its page (0 for one that came by itself). */
	item = &connection->text_item;
	memset(item, 0, sizeof(*item));
	item->what = what;
	item->id = id;

	/* The fields of its kind. */
	if (what == KL_BACKEND_PHONE_WHAT_CONTACTS) {
		phone_contact_fields(item, line);
	} else if (what == KL_BACKEND_PHONE_WHAT_CALLS) {
		phone_call_fields(item, line);
	} else {
		phone_message_fields(item, line);
	}

	/* Distinguishes raw MIME from the ordinary UTF-8 body before selecting its bound. */
	connection->reading_mime = 0U;
	found = phone_field(line, "format=", value, sizeof(value));
	if (found && what == KL_BACKEND_PHONE_WHAT_MESSAGES) {
		same = strcmp(value, "mime");
		if (same != 0) {
			connection->broken = 1U;
			return EPROTO;
		}

		/* The complete body is MIME rather than a displayable UTF-8 string. */
		connection->reading_mime = 1U;
	}

	/* The text's length. */
	found = phone_field(line, "length=", value, sizeof(value));
	if (!found) {
		connection->broken = 1U;
		return EPROTO;
	}

	/* Its number. */
	length = strtoul(value, &end, 10);

	/* Chooses the MIME bound only after its explicit format was validated. */
	maximum = KL_BACKEND_PHONE_TEXT_MAX;
	if (connection->reading_mime)
		maximum = KL_BACKEND_PHONE_MIME_MAX;

	/* A whole number, within what bluetoothd sends. */
	if (end == value ||
	    *end != '\0' ||
	    length > maximum) {
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

/* Reads a PHONE MESSAGE line's fields into its item. */
static void
phone_message_fields(
	struct kl_backend_phone_item *item,
	const char *line)
{
	char value[KL_BACKEND_PHONE_PEER_MAX];
	int same;

	/* Its handle and key. */
	(void)phone_field(line, "handle=", item->handle, sizeof(item->handle));
	(void)phone_field(line, "key=", item->key, sizeof(item->key));

	/* The folder and the direction. */
	(void)phone_field(line, "folder=", value, sizeof(value));
	same = strcmp(value, "sent");
	item->folder = KL_BACKEND_PHONE_FOLDER_INBOX;
	if (same == 0)
		item->folder = KL_BACKEND_PHONE_FOLDER_SENT;

	/* Carries the MMS text type through the existing message-only folder flag. */
	(void)phone_field(line, "type=", value, sizeof(value));
	same = strcmp(value, "mms");
	if (same == 0)
		item->folder |= KL_BACKEND_PHONE_FOLDER_MMS;

	/* Reads the direction independently from the message type. */
	(void)phone_field(line, "dir=", value, sizeof(value));
	same = strcmp(value, "out");
	item->direction = KL_BACKEND_PHONE_DIRECTION_IN;
	if (same == 0)
		item->direction = KL_BACKEND_PHONE_DIRECTION_OUT;

	/* The time in UNIX seconds, and where it came from. */
	(void)phone_field(line, "time=", value, sizeof(value));
	item->time = (int64_t)strtoll(value, NULL, 10);
	item->zone = phone_zone(line);

	/* The phone's datetime, the other side's number and name. */
	(void)phone_field(line, "datetime=", item->datetime, sizeof(item->datetime));
	(void)phone_field(line, "peer=", item->peer, sizeof(item->peer));
	(void)phone_field(line, "name=", item->name, sizeof(item->name));

	/* The flags. */
	item->read = phone_field_number(line, "read=");
	item->partial = phone_field_number(line, "partial=");
	item->truncated = phone_field_number(line, "truncated=");
}

/* Reads a PHONE CONTACT line's fields into its item: the key, how many numbers (folder), the first number and the name. */
static void
phone_contact_fields(
	struct kl_backend_phone_item *item,
	const char *line)
{
	/* Its key and how many numbers it has. */
	(void)phone_field(line, "key=", item->key, sizeof(item->key));
	item->folder = phone_field_number(line, "tels=");

	/* Its first number and its name. */
	(void)phone_field(line, "peer=", item->peer, sizeof(item->peer));
	(void)phone_field(line, "name=", item->name, sizeof(item->name));
}

/*
 * Reads a PHONE CALL-LOG line's fields into its item: the key, the kind
 * (folder) and whether it was dialed (direction), the time and where it
 * came from (none: the time the line was made, partial), the phone's
 * datetime, the number and the name.
 */
static void
phone_call_fields(
	struct kl_backend_phone_item *item,
	const char *line)
{
	char value[KL_BACKEND_PHONE_PEER_MAX];
	int same;

	/* Its key. */
	(void)phone_field(line, "key=", item->key, sizeof(item->key));

	/* The kind: a call received unless the phone said dialed or missed. */
	(void)phone_field(line, "kind=", value, sizeof(value));
	item->folder = KL_BACKEND_PHONE_CALL_RECEIVED;
	item->direction = KL_BACKEND_PHONE_DIRECTION_IN;
	same = strcmp(value, "dialed");
	if (same == 0) {
		item->folder = KL_BACKEND_PHONE_CALL_DIALED;
		item->direction = KL_BACKEND_PHONE_DIRECTION_OUT;
	}

	/* A call missed is one received that nobody answered. */
	same = strcmp(value, "missed");
	if (same == 0)
		item->folder = KL_BACKEND_PHONE_CALL_MISSED;

	/* The time in UNIX seconds, and where it came from. */
	(void)phone_field(line, "time=", value, sizeof(value));
	item->time = (int64_t)strtoll(value, NULL, 10);
	item->zone = phone_zone(line);

	/* A time the phone did not give is the time the line was made. */
	item->partial = phone_field_number(line, "partial=");
	if (item->zone == KL_BACKEND_PHONE_ZONE_RECEIVED)
		item->partial = 1U;

	/* The phone's datetime, the number and the name. */
	(void)phone_field(line, "datetime=", item->datetime, sizeof(item->datetime));
	(void)phone_field(line, "peer=", item->peer, sizeof(item->peer));
	(void)phone_field(line, "name=", item->name, sizeof(item->name));
}

/*
 * Reads where a line's time came from: zone=phone, mse or local; any
 * other word (received, a call's none) or none is the time it came.
 */
static unsigned
phone_zone(
	const char *line)
{
	char value[16];
	int same;

	/* The word. */
	(void)phone_field(line, "zone=", value, sizeof(value));

	/* The phone's own time. */
	same = strcmp(value, "phone");
	if (same == 0)
		return KL_BACKEND_PHONE_ZONE_PHONE;

	/* The phone's zone (MAP's MSE). */
	same = strcmp(value, "mse");
	if (same == 0)
		return KL_BACKEND_PHONE_ZONE_MSE;

	/* This computer's zone. */
	same = strcmp(value, "local");
	if (same == 0)
		return KL_BACKEND_PHONE_ZONE_LOCAL;

	/* Succeeded: when it came. */
	return KL_BACKEND_PHONE_ZONE_RECEIVED;
}

/* Converts an input MIME body to its caption and an independently readable anonymous file. */
static int
phone_mime_take(
	struct phone_connection *connection)
{
	struct mms_document document;
	FILE *stream;
	char *caption;
	size_t written;
	int descriptor;
	int error;
	int closed;

	/* Validates media and text before exporting any partial MIME. */
	error = mms_parse((const uint8_t *)connection->text, connection->text_item.length, &document);
	if (error != 0)
		return error;

	/* Reserves the ordinary item caption independently of its large body. */
	caption = malloc(document.text_length + 1U);
	if (caption == NULL) {
		mms_release(&document);
		return ENOMEM;
	}

	/* Creates an unlinked stream that survives only while an owner holds its descriptor. */
	stream = tmpfile();
	if (stream == NULL) {
		error = errno;
		free(caption);
		mms_release(&document);
		return error;
	}

	/* Completes the file before publishing the descriptor. */
	written = fwrite(connection->text, 1U, connection->text_item.length, stream);
	error = 0;
	if (written != connection->text_item.length)
		error = EIO;
	if (error == 0) {
		error = fflush(stream);
		if (error != 0)
			error = EIO;
	}

	/* Rewinds the shared file description before duplicating it for the backend. */
	descriptor = -1;
	if (error == 0) {
		error = fseek(stream, 0L, SEEK_SET);
		if (error != 0)
			error = EIO;
	}

	/* Transfers a descriptor whose lifetime is independent of stdio. */
	if (error == 0) {
		descriptor = dup(fileno(stream));
		if (descriptor < 0)
			error = errno;
	}

	/* Keeps every failure path free of both temporary bytes and descriptors. */
	closed = fclose(stream);
	if (closed != 0 && error == 0)
		error = EIO;
	if (error != 0) {
		if (descriptor >= 0)
			close(descriptor);
		free(caption);
		mms_release(&document);
		return error;
	}

	/* Exposes the ordinary caption with its original MIME body beside it. */
	memcpy(caption, document.text, document.text_length + 1U);
	free(connection->text);
	connection->text = caption;
	connection->text_item.length = document.text_length;
	connection->text_item.truncated = (unsigned)document.truncated;
	connection->text_item.has_mime = 1U;
	connection->text_item.mime_descriptor = descriptor;
	mms_release(&document);

	/* Succeeded: the next item can transport media without a large wire array. */
	return 0;
}

/* Keeps a connection's whole message for the caller; with no room the oldest goes and a drop is told. */
static void
phone_item_push(
	struct phone_follow *phone,
	struct phone_connection *connection)
{
	struct phone_kept_item *kept;
	size_t slot;
	int error;

	/* Converts MIME to a caption plus an anonymous descriptor before publishing it. */
	if (connection->reading_mime) {
		error = phone_mime_take(connection);
		if (error != 0) {
			connection->broken = 1U;
			return;
		}
	}

	/* No room: the oldest goes, and the caller synchronises again. */
	if (phone->kept.item_count == PHONE_ITEMS_MAX) {
		if (phone->kept.items[phone->kept.item_head].item.has_mime)
			close(phone->kept.items[phone->kept.item_head].item.mime_descriptor);
		free(phone->kept.items[phone->kept.item_head].text);
		phone->kept.items[phone->kept.item_head].text = NULL;
		phone->kept.item_head = (phone->kept.item_head + 1U) % PHONE_ITEMS_MAX;
		phone->kept.item_count--;
		phone->changed |= KL_BACKEND_PHONE_CHANGED_DROPPED;
	}

	/* The item with the text it now owns. */
	slot = (phone->kept.item_head + phone->kept.item_count) % PHONE_ITEMS_MAX;
	kept = &phone->kept.items[slot];
	kept->item = connection->text_item;
	kept->item.text = NULL;
	kept->text = connection->text;
	phone->kept.item_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_ITEM;

	/* The connection reads lines again. */
	connection->text = NULL;
	connection->text_used = 0U;
	connection->reading_text = 0U;
	connection->reading_mime = 0U;
	connection->text_item.has_mime = 0U;
}

/* Keeps a result for the caller; with no room the oldest goes (the caller's own watch ends its wait). */
static void
phone_result_push(
	struct phone_follow *phone,
	const struct kl_backend_phone_result *result)
{
	size_t slot;

	/* No room: the oldest goes. */
	if (phone->kept.result_count == PHONE_RESULTS_MAX) {
		phone->kept.result_head = (phone->kept.result_head + 1U) % PHONE_RESULTS_MAX;
		phone->kept.result_count--;
	}

	/* The result. */
	slot = (phone->kept.result_head + phone->kept.result_count) % PHONE_RESULTS_MAX;
	phone->kept.results[slot] = *result;
	phone->kept.result_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_RESULT;
}

/* Takes a PHONE SENT line of the events: bluetoothd's number and the state (pushed is the answer's own, not here). */
static void
phone_sent_line(
	struct phone_follow *phone,
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
	if (phone->kept.sent_count == PHONE_SENTS_MAX) {
		phone->kept.sent_head = (phone->kept.sent_head + 1U) % PHONE_SENTS_MAX;
		phone->kept.sent_count--;
	}

	/* The state for the caller. */
	slot = (phone->kept.sent_head + phone->kept.sent_count) % PHONE_SENTS_MAX;
	phone->kept.sents[slot].request = phone_field_number(line, "request=");
	phone->kept.sents[slot].state = state;
	phone->kept.sent_count++;
	phone->changed |= KL_BACKEND_PHONE_CHANGED_SENT;
}

/*
 * Takes the record's fields ("address=..." of PHONE STATE, or of SHOW's
 * PHONE line); whole is 1 for the events (the owner sees every field).  A
 * record of this user's while the events are refused asks for them now.
 */
static void
phone_state_line(
	struct phone_follow *phone,
	const char *line,
	unsigned whole)
{
	struct kl_backend_phone_state *state;
	char value[KL_BACKEND_BT_REASON_MAX];
	unsigned mine;
	int same;
	int found;

	/* A record, at its address; whether there is one is known now. */
	state = &phone->state;
	state->have_record = 1U;
	state->record_known = 1U;
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
		phone_contacts_clear(phone);
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

	/* The contacts (ws197-p005). */
	phone_contacts_line(phone, line);

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
	struct phone_follow *phone,
	const char *why)
{
	/* The word, and a change for the caller. */
	(void)snprintf(phone->state.why, sizeof(phone->state.why), "%s", why);
	phone->changed |= KL_BACKEND_PHONE_CHANGED_STATE;
}

/* Reads the contacts' state and why they stopped from a line ("contacts=off|connecting|ready|failed", "contacts_why=..."). */
static void
phone_contacts_line(
	struct phone_follow *phone,
	const char *line)
{
	struct kl_backend_phone_state *state;
	char value[KL_BACKEND_BT_REASON_MAX];
	int same;
	int found;

	/* The state: off unless the line says otherwise (a bluetoothd before ws197-p005 says nothing). */
	state = &phone->state;
	(void)phone_field(line, "contacts=", value, sizeof(value));
	state->contacts = KL_BACKEND_PHONE_MESSAGES_OFF;
	same = strcmp(value, "connecting");
	if (same == 0)
		state->contacts = KL_BACKEND_PHONE_MESSAGES_CONNECTING;
	same = strcmp(value, "ready");
	if (same == 0)
		state->contacts = KL_BACKEND_PHONE_MESSAGES_READY;
	same = strcmp(value, "failed");
	if (same == 0)
		state->contacts = KL_BACKEND_PHONE_MESSAGES_FAILED;

	/* Why they stopped, or nothing. */
	found = phone_field(line, "contacts_why=", value, sizeof(value));
	if (!found)
		value[0] = '\0';
	(void)snprintf(state->contacts_why, sizeof(state->contacts_why), "%s", value);
}

/* Forgets the contacts' state (no record, not the owner's). */
static void
phone_contacts_clear(
	struct phone_follow *phone)
{
	/* Off, and no why. */
	phone->state.contacts = KL_BACKEND_PHONE_MESSAGES_OFF;
	phone->state.contacts_why[0] = '\0';
}

/* Asks bluetoothd for the events; without it the state is unreachable and asked again in a second. */
static void
phone_events_start(
	struct phone_follow *phone,
	uint64_t now)
{
	int error;

	/* The connection; bluetoothd not there is unreachable. */
	error = phone_connect(&phone->events);
	if (error != 0) {
		/* Everything forgotten, the contacts and whether there is a record too. */
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
	struct phone_follow *phone,
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
	(void)phone_queue(&phone->requests, PHONE_KIND_SHOW, "PHONE SHOW\n", NULL, 0U, 0U);
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
