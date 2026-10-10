/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The phone on the wire (ws170-p004, plan/ws170/phase001/phase.md section
 * 3; ws197-p004a, plan/ws197/phase004/phase.md section 4):
 * kl_system_phone_v1, made by the system manager's get_phone (since its
 * version 16) for a client of the compositor's own user.
 *
 *   send(request, channel, to, text)  a message to a number
 *   call(request, channel, to)        a call to a number
 *
 * Each is answered by result(request, OK) when the backend took it, or
 * result(request, UNAVAILABLE) without one; the backend then tells
 * status(request, state).  Since version 27 the phone's messages: listen,
 * sync, send_text, mark_read, link_set and watch_link, answered by done,
 * and the items of a page or one that came, the page's end, the link and
 * the drops.  The messages go to the phone program only (its window's
 * app_id "phone"); link_set and watch_link are Settings' and asked of
 * anyone of the user.
 *
 * The backends are a table; the desktop's setting phone.backend chooses
 * one (0 none): loopback, for the tests and a demo (a message is sent and
 * delivered at once and comes back from the same number as "Echo:
 * <words>"; a call is not answered; a sync gives two messages), and
 * bluetooth, the user's paired phone through libkeiland-backend's
 * kl_backend_phone (bluetoothd's MAP on zedBSD).  That backend is open for
 * the desktop's life whatever the setting, so that Settings reads and sets
 * the phone's switch; the messages go through it only while the setting
 * is 2.  Nothing is kept on disk here: an item no phone program hears is
 * dropped (the program's next sync takes it), and a new message is told
 * as the compositor's own notification (ws197-p004c, plan/ws197/phase004/
 * phase.md section 7: the other side's name and the first line; the lock
 * screen shows "New message from <name>" alone; a click opens the phone
 * program at that conversation), unless the user turned the phone's
 * notifications off.  Numbers, names and words are never logged, only
 * their lengths.
 *
 * done, page_end, link and dropped are never lost: when the client reads
 * too little to take one (kwl_emit's ENOBUFS), it is owed, in order, and
 * sent again once there is room; meanwhile the items for that client are
 * dropped (a page's done then says NO_ROOM, a dropped is owed).
 *
 * Since version 28 (ws197-p005, plan/ws197/phase005/phase.md section 6.3)
 * a sync also reads the phone's contacts and its calls (the paired
 * phone's PBAP; the loopback has none), each item says what it is, and
 * each link is preceded by link_contacts: the contacts' state and whether
 * the phone's record is known, which tells the phone program when to
 * forget its copy of the phone's contacts.
 */

#include "kwl.h"
#include "notify.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Marks a parameter a function has to take but does not use (a backend's that ignores the number). */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The longest number and words a request of version 16 carries, with their NULs (keiland.h's KL_PHONE_NUMBER_MAX and _TEXT_MAX). */
#define PHONE_NUMBER_MAX	64U
#define PHONE_TEXT_MAX		1024U

/* What the loopback backend puts before the words it sends back. */
#define PHONE_ECHO		"Echo: "

/* The longest received event: the channel, two strings, the time's two words. */
#define PHONE_EVENT_MAX		(4U + 4U + PHONE_NUMBER_MAX + 4U + PHONE_TEXT_MAX + 8U + 8U)

/* The longest item event: nine words, five strings with their lengths, and the text's array with its length. */
#define PHONE_ITEM_MAX		(9U * 4U + 5U * 4U + KL_SYSTEM_PHONE_HANDLE_MAX + KL_SYSTEM_PHONE_KEY_MAX + KL_SYSTEM_PHONE_DATETIME_MAX + 2U * KL_SYSTEM_PHONE_PEER_MAX + 4U + KL_SYSTEM_PHONE_ITEM_TEXT_MAX + 16U)

/* The longest event owed: the link's nine words and two strings with their lengths (link_contacts, sent with it, is shorter). */
#define PHONE_OWED_BYTES	(9U * 4U + 4U + KL_SYSTEM_PHONE_ADDRESS_MAX + 4U + KL_SYSTEM_PHONE_WHY_MAX + 8U)

/* The program the messages go to (its window's app_id), its name on a notification, the setting that lets it notify, and how it is started at a conversation. */
#define PHONE_APP		"phone"
#define PHONE_APP_NAME		"Phone"
#define PHONE_NOTIFY_SETTING	"notify.allow.phone"
#define PHONE_COMMAND		KEILAND_BINDIR "/phone"

/* The most of a message's first line a notification is made from, with its NUL. */
#define PHONE_LINE_MAX		2048U

/* How many phone objects are followed, what one may be owed, how many requests wait for the backend, how many states wait for their text. */
#define PHONE_SLOTS		16U
#define PHONE_OWED_MAX		20U
#define PHONE_PENDING_MAX	48U
#define PHONE_HELD_MAX		8U

/* How long a text's state waits for the answer that names its number, and how long a sync waits its turn (milliseconds). */
#define PHONE_HOLD_MS		10000U
#define PHONE_SYNC_WAIT_MS	150000U

/* What is owed to a client. */
#define PHONE_OWED_PAGE_END	1U
#define PHONE_OWED_DONE		2U
#define PHONE_OWED_LINK		3U
#define PHONE_OWED_DROPPED	4U

/* What a request waiting for the backend is. */
#define PHONE_PENDING_SYNC	1U
#define PHONE_PENDING_SEND	2U
#define PHONE_PENDING_READ	3U
#define PHONE_PENDING_LINK	4U
#define PHONE_PENDING_V1_SEND	5U

/*
 * A page asked by a sync: what, the messages since a time (UNIX seconds),
 * at most limit a folder, from a cursor, count items.
 */
struct phone_page {
	unsigned what;
	int64_t since;
	unsigned limit;
	char cursor[KL_SYSTEM_PHONE_CURSOR_MAX];
	unsigned count;
};

/*
 * A backend: its name for the log; how it sends a message and makes a
 * call for a request of version 16 (it answers the result itself, then
 * tells the states and what comes); and how it carries a page, a text and
 * a message marked read of version 27 (each answered by done).  A NULL is
 * a request the backend does not carry (UNAVAILABLE).
 */
struct phone_backend {
	const char *name;
	void (*send)(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to, const char *text);
	void (*call)(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to);
	void (*sync)(struct kwl_object *object, uint32_t request, const struct phone_page *page);
	void (*send_text)(struct kwl_object *object, uint32_t request, const char *to, const uint8_t *text, size_t length);
	void (*mark_read)(struct kwl_object *object, uint32_t request, const char *handle);
};

/*
 * The link's state as the link event carries it, and the contacts' part
 * that link_contacts carries before it (ws197-p005): the contacts' state,
 * whether the phone's record is known (0 not known, 1 none, 2 one), and
 * why the contacts stopped.
 */
struct phone_link_event {
	uint32_t backend;
	uint32_t linked;
	uint32_t messages;
	uint32_t can_send;
	uint32_t notify;
	uint32_t owner;
	uint32_t enabled;
	uint32_t profiles;
	uint32_t present;
	char address[KL_SYSTEM_PHONE_ADDRESS_MAX];
	char why[KL_SYSTEM_PHONE_WHY_MAX];
	uint32_t contacts;
	uint32_t record;
	char contacts_why[KL_SYSTEM_PHONE_WHY_MAX];
};

/* One event owed to a client: its kind, the request and its code (a done), the page's end, or the link. */
struct phone_owed {
	unsigned kind;
	uint32_t request;
	uint32_t code;
	struct kl_backend_phone_result end;
	struct phone_link_event link;
};

/*
 * One phone object followed: whether it hears the messages (listen) or
 * the link alone (watch_link), what it is owed, and its sync waiting its
 * turn (the backend carries one sync at a time).  object is NULL for a
 * free slot.
 */
struct phone_slot {
	struct kwl_object *object;
	unsigned listening;
	unsigned watching;
	struct phone_owed owed[PHONE_OWED_MAX];
	size_t owed_count;
	unsigned sync_waiting;
	uint32_t sync_request;
	struct phone_page sync_page;
	uint64_t sync_since_ms;
};

/*
 * A request waiting for the backend: its kind, the object and request it
 * answers (the object NULL once it went), the backend's id, whether the
 * backend answered (a text then waits for its states, by bluetoothd's
 * number), whether an item of the page was lost on the way, and its age
 * among the others.
 */
struct phone_pending {
	unsigned used;
	unsigned kind;
	struct kwl_object *object;
	uint32_t request;
	uint32_t id;
	unsigned answered;
	uint32_t sent_request;
	unsigned spoiled;
	uint64_t order;
};

/* A text's state that came before the answer naming its number: kept a while. */
struct phone_held {
	unsigned used;
	uint32_t sent_request;
	unsigned state;
	uint64_t at_ms;
};

/*
 * The phone's state for the compositor's life: whether the backend was
 * opened (not on the login screen) and the backend, its state as last
 * read, the setting phone.backend as last seen (setting_known 0 before),
 * the objects followed, the requests waiting for the backend and the
 * texts' states held, whether a sync is at the backend, the requests'
 * ages, and the loopback's next key.  The event loop's thread alone uses
 * it.
 */
struct phone_shell_state {
	unsigned opened;
	struct kl_backend_phone *backend;
	struct kl_backend_phone_state state;
	unsigned setting_known;
	int setting;
	struct phone_slot slots[PHONE_SLOTS];
	struct phone_pending pending[PHONE_PENDING_MAX];
	struct phone_held held[PHONE_HELD_MAX];
	unsigned sync_running;
	uint64_t order;
	uint32_t loopback_key;
};

static void phone_loopback_send(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to, const char *text);
static void phone_loopback_call(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to);
static void phone_loopback_sync(struct kwl_object *object, uint32_t request, const struct phone_page *page);
static void phone_loopback_send_text(struct kwl_object *object, uint32_t request, const char *to, const uint8_t *text, size_t length);
static void phone_loopback_mark_read(struct kwl_object *object, uint32_t request, const char *handle);
static void phone_bluetooth_send(struct kwl_object *object, uint32_t request, uint32_t channel, const char *to, const char *text);
static void phone_bluetooth_sync(struct kwl_object *object, uint32_t request, const struct phone_page *page);
static void phone_bluetooth_send_text(struct kwl_object *object, uint32_t request, const char *to, const uint8_t *text, size_t length);
static void phone_bluetooth_mark_read(struct kwl_object *object, uint32_t request, const char *handle);

/* The backends by phone.backend's value (index 0 is none).  The table is constant for the compositor's life. */
static const struct phone_backend phone_backends[] = {
	{ "none", NULL, NULL, NULL, NULL, NULL },
	{ "loopback", phone_loopback_send, phone_loopback_call, phone_loopback_sync, phone_loopback_send_text, phone_loopback_mark_read },
	{ "bluetooth", phone_bluetooth_send, NULL, phone_bluetooth_sync, phone_bluetooth_send_text, phone_bluetooth_mark_read }
};

/* The phone's state (zero: nothing opened, nothing followed). */
static struct phone_shell_state phone_state;

static int phone_send(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_call(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_listen(struct kwl_object *object, const unsigned char *bytes, size_t size, unsigned watch);
static int phone_sync(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_send_text(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_mark_read(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_link_set(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int phone_setting(struct kwl_server *server);
static const struct phone_backend *phone_backend(struct kwl_server *server);
static int phone_is_phone_app(struct kwl_client *client);
static struct phone_slot *phone_slot_of(struct kwl_object *object, unsigned create);
static struct phone_pending *phone_pending_add(unsigned kind, struct kwl_object *object, uint32_t request, uint32_t id);
static struct phone_pending *phone_pending_find(uint32_t id);
static uint32_t phone_code(int error);
static void phone_received(struct kwl_server *server, uint32_t channel, const char *from, const char *text);
static void phone_status(struct kwl_object *object, uint32_t request, uint32_t state);
static void phone_result(struct kwl_object *object, uint32_t request, uint32_t applied);
static void phone_done(struct kwl_object *object, uint32_t request, uint32_t code);
static void phone_page_end(struct kwl_object *object, uint32_t request, const struct kl_backend_phone_result *end);
static void phone_dropped(struct kwl_object *object);
static void phone_owe(struct kwl_object *object, const struct phone_owed *owed);
static void phone_owe_room(struct phone_slot *slot);
static int phone_emit_owed(struct kwl_object *object, const struct phone_owed *owed);
static void phone_link_fill(unsigned refused, struct phone_link_event *link);
static void phone_link_tell(struct kwl_object *object, unsigned refused);
static void phone_links(void);
static int phone_item(struct kwl_object *object, uint32_t request, const struct kl_backend_phone_item *item);
static void phone_live(struct kwl_server *server, const struct kl_backend_phone_item *item);
static void phone_notify(struct kwl_server *server, const struct kl_backend_phone_item *item);
static int phone_shell_safe(const char *text);
static void phone_answers(void);
static void phone_answer(struct phone_pending *pending, const struct kl_backend_phone_result *result);
static void phone_items(struct kwl_server *server);
static void phone_sents(uint64_t now);
static int phone_sent_match(uint32_t sent_request, unsigned state);
static void phone_held_check(uint64_t now);
static void phone_sync_next(uint64_t now);
static void phone_unavailable(void);
static void phone_forget_texts(void);
static void phone_drops(void);
static void phone_owed_flush(void);
static int phone_string(const unsigned char *bytes, size_t size, size_t offset, size_t bound, const char **text, size_t *next);
static int phone_array(const unsigned char *bytes, size_t size, size_t offset, size_t bound, const uint8_t **data, size_t *length, size_t *next);
static size_t phone_put_string(unsigned char *payload, size_t offset, const char *text);
static size_t phone_put_word(unsigned char *payload, size_t offset, uint32_t word);
static size_t phone_put_array(unsigned char *payload, size_t offset, const void *data, size_t length);
static uint32_t phone_word(const unsigned char *bytes, size_t offset);

/*
 * Makes a phone object for a manager's get_phone (new id).  Returns 0, or
 * EPROTO for a malformed request.
 */
int
kwl_phone_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = phone_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_PHONE, manager->version);
	if (created == NULL)
		return EPROTO;

	/* The log the tests read. */
	printf("KWL PHONE object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);

	/* Succeeded: the object is the client's. */
	return 0;
}

/*
 * Carries out a request of a phone object.  Returns 0, or EPROTO for a
 * malformed request (or one of version 27 on an older object).
 */
int
kwl_phone_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The object goes (what it held goes with it, kwl_phone_gone). */
	if (opcode == KL_SYSTEM_PHONE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A message to send (version 16). */
	if (opcode == KL_SYSTEM_PHONE_SEND) {
		error = phone_send(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A call to make (version 16). */
	if (opcode == KL_SYSTEM_PHONE_CALL) {
		error = phone_call(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The messages' requests are of version 27. */
	if (object->version < KL_SYSTEM_SINCE_PHONE_SYNC)
		return EPROTO;

	/* Each of them. */
	switch (opcode) {
	case KL_SYSTEM_PHONE_LISTEN:
		error = phone_listen(object, bytes, size, 0U);
		break;
	case KL_SYSTEM_PHONE_WATCH_LINK:
		error = phone_listen(object, bytes, size, 1U);
		break;
	case KL_SYSTEM_PHONE_SYNC:
		error = phone_sync(object, bytes, size);
		break;
	case KL_SYSTEM_PHONE_SEND_TEXT:
		error = phone_send_text(object, bytes, size);
		break;
	case KL_SYSTEM_PHONE_MARK_READ:
		error = phone_mark_read(object, bytes, size);
		break;
	case KL_SYSTEM_PHONE_LINK_SET:
		error = phone_link_set(object, bytes, size);
		break;
	default:
		error = EPROTO;
		break;
	}

	/* A malformed one ends the client. */
	if (error != 0)
		return error;

	/* Succeeded: carried out or answered. */
	return 0;
}

/*
 * Looks after the phone once a pass: opens the backend on the desktop's
 * first tick, follows the setting, reads what came, hands the answers,
 * the items and the texts' states to their objects, starts a sync that
 * waited, and sends what the clients were owed.
 */
void
kwl_phone_tick(
	struct kwl_server *server)
{
	struct kl_backend_phone_state state;
	unsigned changed;
	uint64_t now;
	int setting;

	/* The login screen's compositor has no phone. */
	if (server->greeter)
		return;

	/* Opened once, its state read at once (a system without the phone says why). */
	if (!phone_state.opened) {
		phone_state.opened = 1U;
		phone_state.backend = kl_backend_phone_open();
		kl_backend_phone_get_state(phone_state.backend, &phone_state.state);
		printf("KWL PHONE open ok=%d\n", phone_state.backend != NULL);
	}

	/* The setting: the messages that went through the paired phone end when it changes away from it. */
	setting = phone_setting(server);
	if (!phone_state.setting_known || setting != phone_state.setting) {
		if (phone_state.setting_known && phone_state.setting == KL_SYSTEM_PHONE_BACKEND_BLUETOOTH)
			phone_unavailable();
		phone_state.setting_known = 1U;
		phone_state.setting = setting;
		printf("KWL PHONE backend=%d\n", setting);
		phone_links();
	}

	/* What came. */
	changed = 0U;
	if (phone_state.backend != NULL)
		(void)kl_backend_phone_update(phone_state.backend, &changed);

	/* The state: a backend that reached bluetoothd again forgets the texts' numbers (they start again). */
	if ((changed & KL_BACKEND_PHONE_CHANGED_STATE) != 0U) {
		kl_backend_phone_get_state(phone_state.backend, &state);
		if (state.reachable && !phone_state.state.reachable)
			phone_forget_texts();
		phone_state.state = state;
		printf("KWL PHONE state reachable=%u subscribed=%u linked=%u messages=%u why=%s contacts=%u contacts_why=%s record=%u/%u\n", state.reachable,
		    state.subscribed, state.linked, state.messages, state.why, state.contacts, state.contacts_why, state.record_known, state.have_record);
		phone_links();
	}

	/*
	 * A page's items before its answer (which ends the page), and the
	 * answers before the texts' states, which may name a number an answer
	 * gives.
	 */
	now = kwl_milliseconds();
	if ((changed & KL_BACKEND_PHONE_CHANGED_ITEM) != 0U)
		phone_items(server);
	if ((changed & KL_BACKEND_PHONE_CHANGED_RESULT) != 0U)
		phone_answers();
	if ((changed & KL_BACKEND_PHONE_CHANGED_SENT) != 0U)
		phone_sents(now);

	/* bluetoothd dropped an event: every phone program synchronises again. */
	if ((changed & KL_BACKEND_PHONE_CHANGED_DROPPED) != 0U)
		phone_drops();

	/* The texts' states held, a sync that waited, and what the clients were owed. */
	phone_held_check(now);
	phone_sync_next(now);
	phone_owed_flush();
}

/*
 * Lets go of what a phone object held (its hearing, what it was owed, its
 * sync waiting) as it goes; the requests it asked are answered to nobody.
 */
void
kwl_phone_gone(
	struct kwl_object *object)
{
	struct phone_slot *slot;
	unsigned index;

	/* Its requests waiting for the backend answer nobody. */
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		if (phone_state.pending[index].used && phone_state.pending[index].object == object)
			phone_state.pending[index].object = NULL;
	}

	/* Its slot, with nothing of it left. */
	slot = phone_slot_of(object, 0U);
	if (slot != NULL)
		memset(slot, 0, sizeof(*slot));
}

/*
 * Reads the phone's record and events again at once (after the phone was
 * paired or forgotten, ws197-p004 section 4.3).
 */
void
kwl_phone_refresh(
	void)
{
	/* Only an open backend. */
	if (phone_state.backend == NULL)
		return;

	/* Its record and events. */
	kl_backend_phone_refresh(phone_state.backend);
}

/*
 * Closes the backend at the compositor's end.
 */
void
kwl_phone_close(
	struct kwl_server *server)
{
	UNUSED_PARAMETER(server);

	/* Only an open one. */
	if (phone_state.backend == NULL)
		return;

	/* Closed, nothing of it kept. */
	kl_backend_phone_close(phone_state.backend);
	phone_state.backend = NULL;
}

/* Carries out send(request, channel, to, text) with the backend. */
static int
phone_send(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	const struct phone_backend *backend;
	const char *to;
	const char *text;
	uint32_t request;
	uint32_t channel;
	size_t offset;
	int error;

	/* The request's number and the channel. */
	if (size < 8U)
		return EPROTO;
	request = phone_word(bytes, 0U);
	channel = phone_word(bytes, 4U);

	/* The number. */
	error = phone_string(bytes, size, 8U, PHONE_NUMBER_MAX, &to, &offset);
	if (error != 0)
		return error;

	/* The words, the last argument. */
	error = phone_string(bytes, size, offset, PHONE_TEXT_MAX, &text, &offset);
	if (error != 0)
		return error;
	if (offset != size)
		return EPROTO;

	/* A channel of messages, and a number. */
	if (channel > 2U || to[0] == '\0') {
		phone_result(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Without a backend: unavailable. */
	backend = phone_backend(object->client->server);
	printf("KWL PHONE send client=%llu channel=%u to=%lu text=%lu backend=%s\n", (unsigned long long)object->client->number,
	    channel, (unsigned long)strlen(to), (unsigned long)strlen(text), backend->name);
	if (backend->send == NULL) {
		phone_result(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* The backend's, which answers the result itself. */
	backend->send(object, request, channel, to, text);
	return 0;
}

/* Carries out call(request, channel, to) with the backend. */
static int
phone_call(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	const struct phone_backend *backend;
	const char *to;
	uint32_t request;
	uint32_t channel;
	size_t offset;
	int error;

	/* The request's number and the channel. */
	if (size < 8U)
		return EPROTO;
	request = phone_word(bytes, 0U);
	channel = phone_word(bytes, 4U);

	/* The number, the last argument. */
	error = phone_string(bytes, size, 8U, PHONE_NUMBER_MAX, &to, &offset);
	if (error != 0)
		return error;
	if (offset != size)
		return EPROTO;

	/* A channel of calls, and a number. */
	if ((channel != 3U && channel != 4U) || to[0] == '\0') {
		phone_result(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Without a backend that calls: unavailable. */
	backend = phone_backend(object->client->server);
	printf("KWL PHONE call client=%llu channel=%u to=%lu backend=%s\n", (unsigned long long)object->client->number,
	    channel, (unsigned long)strlen(to), backend->name);
	if (backend->call == NULL) {
		phone_result(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* Taken, then the backend's. */
	phone_result(object, request, KL_SYSTEM_RESULT_OK);
	backend->call(object, request, channel, to);
	return 0;
}

/*
 * Carries out listen(on) (watch 0) or watch_link(on) (watch 1): the object
 * hears the messages or the link alone, and the link is told at once.
 */
static int
phone_listen(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size,
	unsigned watch)
{
	struct phone_slot *slot;
	unsigned on;
	int app;

	/* One word, 1 or 0. */
	if (size != 4U)
		return EPROTO;
	on = phone_word(bytes, 0U);
	if (on > 1U)
		return EPROTO;

	/* Its slot (none to stop for an object never followed, or no room). */
	slot = phone_slot_of(object, on);
	if (slot == NULL) {
		printf("KWL PHONE listen client=%llu on=%u kept=0\n", (unsigned long long)object->client->number, on);
		return 0;
	}

	/* The link alone: anyone of the user. */
	if (watch) {
		slot->watching = on;
		printf("KWL PHONE watch client=%llu on=%u\n", (unsigned long long)object->client->number, on);
		if (on)
			phone_link_tell(object, 0U);
		return 0;
	}

	/* The messages: the phone program only; another hears that it is not. */
	app = phone_is_phone_app(object->client);
	slot->listening = on;
	printf("KWL PHONE listen client=%llu on=%u app=%d\n", (unsigned long long)object->client->number, on, app);
	if (on)
		phone_link_tell(object, (unsigned)!app);

	/* Succeeded: heard or not. */
	return 0;
}

/* Carries out sync(request, what, since_high, since_low, limit, cursor, count). */
static int
phone_sync(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	const struct phone_backend *backend;
	struct phone_page page;
	struct phone_slot *slot;
	const char *cursor;
	uint32_t request;
	uint32_t since_high;
	uint32_t since_low;
	size_t offset;
	int error;
	int app;

	/* The request's number, what, the time and the limit. */
	if (size < 20U)
		return EPROTO;
	request = phone_word(bytes, 0U);
	memset(&page, 0, sizeof(page));
	page.what = phone_word(bytes, 4U);
	since_high = phone_word(bytes, 8U);
	since_low = phone_word(bytes, 12U);
	page.since = (int64_t)(((uint64_t)since_high << 32) | (uint64_t)since_low);
	page.limit = phone_word(bytes, 16U);

	/* The cursor, and the count, the last argument. */
	error = phone_string(bytes, size, 20U, KL_SYSTEM_PHONE_CURSOR_MAX, &cursor, &offset);
	if (error != 0)
		return error;
	if (offset + 4U != size)
		return EPROTO;
	page.count = phone_word(bytes, offset);
	(void)snprintf(page.cursor, sizeof(page.cursor), "%s", cursor);

	/* The phone program only. */
	app = phone_is_phone_app(object->client);
	if (!app) {
		phone_done(object, request, KL_SYSTEM_RESULT_REFUSED);
		return 0;
	}

	/* A page the backend takes. */
	if (page.what > KL_SYSTEM_PHONE_CALLS ||
	    page.since < 0 ||
	    page.limit > 500U ||
	    page.count == 0U ||
	    page.count > 32U) {
		phone_done(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* The contacts and the calls are asked of objects of version 28. */
	if (page.what != KL_SYSTEM_PHONE_MESSAGES && object->version < KL_SYSTEM_SINCE_PHONE_CONTACTS) {
		phone_done(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Without a backend that synchronises: unavailable. */
	backend = phone_backend(object->client->server);
	printf("KWL PHONE sync client=%llu request=%u count=%u limit=%u cursor=%lu backend=%s\n", (unsigned long long)object->client->number, request,
	    page.count, page.limit, (unsigned long)strlen(page.cursor), backend->name);
	if (backend->sync == NULL) {
		phone_done(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* One sync of an object waits at a time. */
	slot = phone_slot_of(object, 1U);
	if (slot == NULL || slot->sync_waiting) {
		phone_done(object, request, KL_SYSTEM_RESULT_BUSY);
		return 0;
	}

	/* The paired phone carries one sync at a time: this one waits its turn. */
	if (backend->sync == phone_bluetooth_sync && phone_state.sync_running) {
		slot->sync_waiting = 1U;
		slot->sync_request = request;
		slot->sync_page = page;
		slot->sync_since_ms = kwl_milliseconds();
		printf("KWL PHONE sync waits client=%llu request=%u\n", (unsigned long long)object->client->number, request);
		return 0;
	}

	/* Succeeded: the backend's. */
	backend->sync(object, request, &page);
	return 0;
}

/* Carries out send_text(request, channel, to, text). */
static int
phone_send_text(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	const struct phone_backend *backend;
	const uint8_t *text;
	const char *to;
	uint32_t request;
	uint32_t channel;
	size_t length;
	size_t offset;
	int error;
	int app;

	/* The request's number and the channel. */
	if (size < 8U)
		return EPROTO;
	request = phone_word(bytes, 0U);
	channel = phone_word(bytes, 4U);

	/* The number. */
	error = phone_string(bytes, size, 8U, KL_SYSTEM_PHONE_TO_MAX, &to, &offset);
	if (error != 0)
		return error;

	/* The text, the last argument. */
	error = phone_array(bytes, size, offset, KL_SYSTEM_PHONE_SEND_MAX, &text, &length, &offset);
	if (error != 0)
		return error;
	if (offset != size)
		return EPROTO;

	/* The phone program only. */
	app = phone_is_phone_app(object->client);
	if (!app) {
		phone_done(object, request, KL_SYSTEM_RESULT_REFUSED);
		return 0;
	}

	/* SMS only. */
	if (channel != KL_SYSTEM_PHONE_SMS) {
		phone_done(object, request, KL_SYSTEM_RESULT_UNSUPPORTED);
		return 0;
	}

	/* A number and a text. */
	if (to[0] == '\0' || length == 0U) {
		phone_done(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Without a backend that sends: unavailable. */
	backend = phone_backend(object->client->server);
	printf("KWL PHONE send_text client=%llu request=%u to=%lu text=%lu backend=%s\n", (unsigned long long)object->client->number, request,
	    (unsigned long)strlen(to), (unsigned long)length, backend->name);
	if (backend->send_text == NULL) {
		phone_done(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* Succeeded: the backend's. */
	backend->send_text(object, request, to, text, length);
	return 0;
}

/* Carries out mark_read(request, handle). */
static int
phone_mark_read(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	const struct phone_backend *backend;
	const char *handle;
	uint32_t request;
	size_t offset;
	int error;
	int app;

	/* The request's number. */
	if (size < 4U)
		return EPROTO;
	request = phone_word(bytes, 0U);

	/* The handle, the last argument. */
	error = phone_string(bytes, size, 4U, KL_SYSTEM_PHONE_HANDLE_MAX, &handle, &offset);
	if (error != 0)
		return error;
	if (offset != size)
		return EPROTO;

	/* The phone program only. */
	app = phone_is_phone_app(object->client);
	if (!app) {
		phone_done(object, request, KL_SYSTEM_RESULT_REFUSED);
		return 0;
	}

	/* Without a backend that marks: unavailable. */
	backend = phone_backend(object->client->server);
	printf("KWL PHONE mark_read client=%llu request=%u backend=%s\n", (unsigned long long)object->client->number, request, backend->name);
	if (backend->mark_read == NULL) {
		phone_done(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* Succeeded: the backend's. */
	backend->mark_read(object, request, handle);
	return 0;
}

/*
 * Carries out link_set(request, address, on, profiles): the phone's
 * switch, through the paired phone's backend whatever the setting.
 */
static int
phone_link_set(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct phone_pending *pending;
	const char *address;
	uint32_t request;
	uint32_t on;
	uint32_t profiles;
	uint32_t id;
	size_t offset;
	int error;

	/* The request's number. */
	if (size < 4U)
		return EPROTO;
	request = phone_word(bytes, 0U);

	/* The address, then the switch and the profiles, the last arguments. */
	error = phone_string(bytes, size, 4U, KL_SYSTEM_PHONE_ADDRESS_MAX, &address, &offset);
	if (error != 0)
		return error;
	if (offset + 8U != size)
		return EPROTO;
	on = phone_word(bytes, offset);
	profiles = phone_word(bytes, offset + 4U);
	printf("KWL PHONE link_set client=%llu request=%u on=%u profiles=%u\n", (unsigned long long)object->client->number, request, on, profiles);

	/* No backend (no memory). */
	if (phone_state.backend == NULL) {
		phone_done(object, request, KL_SYSTEM_RESULT_UNAVAILABLE);
		return 0;
	}

	/* Asked of the backend; a refusal is answered at once. */
	error = kl_backend_phone_link_set(phone_state.backend, address, on != 0U, profiles, &id);
	if (error != 0) {
		phone_done(object, request, phone_code(error));
		return 0;
	}

	/* Answered when the backend answers (busy without room to follow it). */
	pending = phone_pending_add(PHONE_PENDING_LINK, object, request, id);
	if (pending == NULL)
		phone_done(object, request, KL_SYSTEM_RESULT_BUSY);

	/* Succeeded: asked. */
	return 0;
}

/* The loopback backend's message: taken, sent, delivered, and back from the same number. */
static void
phone_loopback_send(
	struct kwl_object *object,
	uint32_t request,
	uint32_t channel,
	const char *to,
	const char *text)
{
	char echo[PHONE_TEXT_MAX];

	/* Taken, sent and delivered at once. */
	phone_result(object, request, KL_SYSTEM_RESULT_OK);
	phone_status(object, request, KL_SYSTEM_PHONE_SENT);
	phone_status(object, request, KL_SYSTEM_PHONE_DELIVERED);

	/* The answer from the same number, on the same channel. */
	(void)snprintf(echo, sizeof(echo), "%s%s", PHONE_ECHO, text);
	phone_received(object->client->server, channel, to, echo);
}

/* The loopback backend's call: nobody answers. */
static void
phone_loopback_call(
	struct kwl_object *object,
	uint32_t request,
	uint32_t channel,
	const char *to)
{
	UNUSED_PARAMETER(channel);
	UNUSED_PARAMETER(to);

	/* Not answered. */
	phone_status(object, request, KL_SYSTEM_PHONE_NO_ANSWER);
}

/* The loopback backend's page: two messages of the tests (one received, one sent back), and no more; no contacts and no calls. */
static void
phone_loopback_sync(
	struct kwl_object *object,
	uint32_t request,
	const struct phone_page *page)
{
	struct kl_backend_phone_item item;
	struct kl_backend_phone_result end;
	int64_t now;

	/* No contacts and no calls: an empty page. */
	memset(&end, 0, sizeof(end));
	if (page->what != KL_SYSTEM_PHONE_MESSAGES) {
		phone_page_end(object, request, &end);
		phone_done(object, request, KL_SYSTEM_RESULT_OK);
		return;
	}

	/* The message received a minute ago. */
	now = (int64_t)time(NULL);
	memset(&item, 0, sizeof(item));
	(void)snprintf(item.handle, sizeof(item.handle), "%s", "00000001.0000000000000001");
	(void)snprintf(item.key, sizeof(item.key), "%s", "0000000000000001");
	item.folder = KL_BACKEND_PHONE_FOLDER_INBOX;
	item.direction = KL_BACKEND_PHONE_DIRECTION_IN;
	item.time = now - 60;
	item.zone = KL_BACKEND_PHONE_ZONE_PHONE;
	(void)snprintf(item.peer, sizeof(item.peer), "%s", "+15550100");
	(void)snprintf(item.name, sizeof(item.name), "%s", "Loopback");
	item.text = "Hello from the loopback phone.";
	item.length = strlen(item.text);
	(void)phone_item(object, request, &item);

	/* The message sent back, read. */
	(void)snprintf(item.handle, sizeof(item.handle), "%s", "00000001.0000000000000002");
	(void)snprintf(item.key, sizeof(item.key), "%s", "0000000000000002");
	item.folder = KL_BACKEND_PHONE_FOLDER_SENT;
	item.direction = KL_BACKEND_PHONE_DIRECTION_OUT;
	item.time = now - 30;
	item.read = 1U;
	item.text = "Hello back.";
	item.length = strlen(item.text);
	(void)phone_item(object, request, &item);

	/* The page's end and the answer. */
	end.count = 2U;
	phone_page_end(object, request, &end);
	phone_done(object, request, KL_SYSTEM_RESULT_OK);
}

/* The loopback backend's text: in the outbox, sent, delivered, and back from the same number as a message that came. */
static void
phone_loopback_send_text(
	struct kwl_object *object,
	uint32_t request,
	const char *to,
	const uint8_t *text,
	size_t length)
{
	struct kl_backend_phone_item item;
	char echo[PHONE_TEXT_MAX];
	size_t kept;

	/* In the outbox, sent and delivered at once. */
	phone_done(object, request, KL_SYSTEM_RESULT_OK);
	phone_status(object, request, KL_SYSTEM_PHONE_SENT);
	phone_status(object, request, KL_SYSTEM_PHONE_DELIVERED);

	/* The words sent back, cut to the echo's room. */
	kept = length;
	if (kept > sizeof(echo) - sizeof(PHONE_ECHO))
		kept = sizeof(echo) - sizeof(PHONE_ECHO);
	(void)snprintf(echo, sizeof(echo), "%s%.*s", PHONE_ECHO, (int)kept, (const char *)text);

	/* As a message that came, with a key of its own. */
	phone_state.loopback_key++;
	memset(&item, 0, sizeof(item));
	(void)snprintf(item.handle, sizeof(item.handle), "00000001.%016x", 0x100U + phone_state.loopback_key);
	(void)snprintf(item.key, sizeof(item.key), "%016x", 0x100U + phone_state.loopback_key);
	item.folder = KL_BACKEND_PHONE_FOLDER_INBOX;
	item.direction = KL_BACKEND_PHONE_DIRECTION_IN;
	item.time = (int64_t)time(NULL);
	item.zone = KL_BACKEND_PHONE_ZONE_PHONE;
	(void)snprintf(item.peer, sizeof(item.peer), "%s", to);
	item.text = echo;
	item.length = strlen(echo);
	phone_live(object->client->server, &item);
}

/* The loopback backend's message marked read: done at once. */
static void
phone_loopback_mark_read(
	struct kwl_object *object,
	uint32_t request,
	const char *handle)
{
	UNUSED_PARAMETER(handle);

	/* Marked. */
	phone_done(object, request, KL_SYSTEM_RESULT_OK);
}

/* The paired phone's message of version 16: SMS only, from the phone program; its states follow. */
static void
phone_bluetooth_send(
	struct kwl_object *object,
	uint32_t request,
	uint32_t channel,
	const char *to,
	const char *text)
{
	struct phone_pending *pending;
	uint32_t id;
	int error;
	int app;

	/* SMS only. */
	if (channel != KL_SYSTEM_PHONE_SMS) {
		phone_result(object, request, KL_SYSTEM_RESULT_UNSUPPORTED);
		return;
	}

	/* The phone program only. */
	app = phone_is_phone_app(object->client);
	if (!app) {
		phone_result(object, request, KL_SYSTEM_RESULT_REFUSED);
		return;
	}

	/* Taken; a refusal of the backend is the message's failure. */
	phone_result(object, request, KL_SYSTEM_RESULT_OK);
	error = kl_backend_phone_send(phone_state.backend, to, (const uint8_t *)text, strlen(text), &id);
	if (error != 0) {
		phone_status(object, request, KL_SYSTEM_PHONE_FAILED);
		return;
	}

	/* Its states follow the backend's answer (a failure without room to follow it). */
	pending = phone_pending_add(PHONE_PENDING_V1_SEND, object, request, id);
	if (pending == NULL)
		phone_status(object, request, KL_SYSTEM_PHONE_FAILED);
}

/* The paired phone's page: one at a time at the backend, answered by its items, its end and done. */
static void
phone_bluetooth_sync(
	struct kwl_object *object,
	uint32_t request,
	const struct phone_page *page)
{
	struct phone_pending *pending;
	uint32_t id;
	int error;

	/* Asked of the backend; a refusal is answered at once. */
	error = kl_backend_phone_page(phone_state.backend, page->what, page->since, page->limit, page->cursor, page->count, &id);
	if (error != 0) {
		phone_done(object, request, phone_code(error));
		return;
	}

	/* Without room to follow it, given up. */
	pending = phone_pending_add(PHONE_PENDING_SYNC, object, request, id);
	if (pending == NULL) {
		phone_done(object, request, KL_SYSTEM_RESULT_BUSY);
		return;
	}

	/* Succeeded: the backend's sync until its answer. */
	phone_state.sync_running = 1U;
}

/* The paired phone's text: answered by done when the phone's outbox has it, then its states. */
static void
phone_bluetooth_send_text(
	struct kwl_object *object,
	uint32_t request,
	const char *to,
	const uint8_t *text,
	size_t length)
{
	struct phone_pending *pending;
	uint32_t id;
	int error;

	/* Asked of the backend; a refusal is answered at once. */
	error = kl_backend_phone_send(phone_state.backend, to, text, length, &id);
	if (error != 0) {
		phone_done(object, request, phone_code(error));
		return;
	}

	/* Answered when the backend answers (busy without room to follow it). */
	pending = phone_pending_add(PHONE_PENDING_SEND, object, request, id);
	if (pending == NULL)
		phone_done(object, request, KL_SYSTEM_RESULT_BUSY);
}

/* The paired phone's message marked read: answered when the backend answers. */
static void
phone_bluetooth_mark_read(
	struct kwl_object *object,
	uint32_t request,
	const char *handle)
{
	struct phone_pending *pending;
	uint32_t id;
	int error;

	/* Asked of the backend; a refusal is answered at once. */
	error = kl_backend_phone_read(phone_state.backend, handle, &id);
	if (error != 0) {
		phone_done(object, request, phone_code(error));
		return;
	}

	/* Answered when the backend answers (busy without room to follow it). */
	pending = phone_pending_add(PHONE_PENDING_READ, object, request, id);
	if (pending == NULL)
		phone_done(object, request, KL_SYSTEM_RESULT_BUSY);
}

/* Reads the desktop's setting phone.backend (0 for no settings or a value not known). */
static int
phone_setting(
	struct kwl_server *server)
{
	int number;
	int error;

	/* The setting. */
	error = kwl_settings_number(server, KL_SYSTEM_PHONE_SETTING, &number);
	if (error != 0)
		return KL_SYSTEM_PHONE_BACKEND_NONE;

	/* A value of the table. */
	if (number < 0 || (size_t)number >= sizeof(phone_backends) / sizeof(phone_backends[0]))
		return KL_SYSTEM_PHONE_BACKEND_NONE;

	/* Succeeded: its value. */
	return number;
}

/* Finds the backend the desktop's setting chooses (the paired phone's only with its backend open). */
static const struct phone_backend *
phone_backend(
	struct kwl_server *server)
{
	int setting;

	/* The setting. */
	setting = phone_setting(server);

	/* The paired phone needs its backend. */
	if (setting == KL_SYSTEM_PHONE_BACKEND_BLUETOOTH && phone_state.backend == NULL)
		return &phone_backends[KL_SYSTEM_PHONE_BACKEND_NONE];

	/* Its backend. */
	return &phone_backends[setting];
}

/* Tells whether a client is the phone program: one of its live surfaces has the phone program's application ID. */
static int
phone_is_phone_app(
	struct kwl_client *client)
{
	struct kwl_object *object;
	int differs;

	/* Each live surface of the client. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->kind != KWL_SURFACE || object->dead)
			continue;

		/* The phone program's window. */
		differs = strcmp(object->app_id, PHONE_APP);
		if (differs == 0)
			return 1;
	}

	/* Succeeded: not the phone program. */
	return 0;
}

/* Finds an object's slot, or makes one when create is 1 (NULL when none is free). */
static struct phone_slot *
phone_slot_of(
	struct kwl_object *object,
	unsigned create)
{
	struct phone_slot *free_slot;
	unsigned index;

	/* Its own, and the first free one. */
	free_slot = NULL;
	for (index = 0U; index < PHONE_SLOTS; index++) {
		if (phone_state.slots[index].object == object)
			return &phone_state.slots[index];
		if (phone_state.slots[index].object == NULL && free_slot == NULL)
			free_slot = &phone_state.slots[index];
	}

	/* None, and none to be made. */
	if (!create || free_slot == NULL)
		return NULL;

	/* Succeeded: a new one. */
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->object = object;
	return free_slot;
}

/*
 * Keeps a request waiting for the backend; with no room the oldest text
 * that waits only for its states goes.  Returns the entry, or NULL when
 * every one waits for an answer.
 */
static struct phone_pending *
phone_pending_add(
	unsigned kind,
	struct kwl_object *object,
	uint32_t request,
	uint32_t id)
{
	struct phone_pending *entry;
	struct phone_pending *candidate;
	unsigned index;

	/* A free entry, or else the oldest text answered. */
	entry = NULL;
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		candidate = &phone_state.pending[index];
		if (!candidate->used) {
			entry = candidate;
			break;
		}

		/* An older text that waits only for its states. */
		if (!candidate->answered)
			continue;
		if (entry == NULL || candidate->order < entry->order)
			entry = candidate;
	}

	/* No room. */
	if (entry == NULL)
		return NULL;

	/* Succeeded: kept. */
	memset(entry, 0, sizeof(*entry));
	entry->used = 1U;
	entry->kind = kind;
	entry->object = object;
	entry->request = request;
	entry->id = id;
	phone_state.order++;
	entry->order = phone_state.order;
	return entry;
}

/* Finds the request waiting for the backend's answer of an id (NULL when none). */
static struct phone_pending *
phone_pending_find(
	uint32_t id)
{
	struct phone_pending *candidate;
	unsigned index;

	/* Each one not answered yet. */
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		candidate = &phone_state.pending[index];
		if (!candidate->used || candidate->answered)
			continue;
		if (candidate->id == id)
			return candidate;
	}

	/* None. */
	return NULL;
}

/* Gives the result code of an errno value of the backend (ws197-p004 section 4). */
static uint32_t
phone_code(
	int error)
{
	/* Each errno value the backend gives. */
	switch (error) {
	case 0:
		return KL_SYSTEM_RESULT_OK;
	case EACCES:
		return KL_SYSTEM_RESULT_REFUSED;
	case ENOTSUP:
		return KL_SYSTEM_RESULT_UNSUPPORTED;
	case EBUSY:
		return KL_SYSTEM_RESULT_BUSY;
	case EINVAL:
		return KL_SYSTEM_RESULT_INVALID;
	case ENODEV:
		return KL_SYSTEM_RESULT_UNAVAILABLE;
	case ESTALE:
		return KL_SYSTEM_RESULT_STALE;
	case ECONNRESET:
		return KL_SYSTEM_RESULT_LOST;
	case ETIMEDOUT:
		return KL_SYSTEM_RESULT_TIMEOUT;
	case EMSGSIZE:
		return KL_SYSTEM_RESULT_TOO_LARGE;
	case ENOBUFS:
		return KL_SYSTEM_RESULT_NO_ROOM;
	case ENOTCONN:
		return KL_SYSTEM_RESULT_NOT_CONNECTED;
	case ENOENT:
		return KL_SYSTEM_RESULT_NO_KEY;
	default:
		break;
	}

	/* Anything else failed. */
	return KL_SYSTEM_RESULT_FAILED;
}

/*
 * Tells every phone object that a message came (version 16's received:
 * the loopback's echo; the paired phone's go to the phone program as
 * items instead).
 */
static void
phone_received(
	struct kwl_server *server,
	uint32_t channel,
	const char *from,
	const char *text)
{
	static unsigned char payload[PHONE_EVENT_MAX];
	struct kwl_client *client;
	struct kwl_object *object;
	uint64_t now;
	uint32_t words[2];
	size_t length;
	unsigned told;

	/* The event: the channel, the number, the words and the time in seconds (high and low words). */
	now = (uint64_t)time(NULL);
	memcpy(payload, &channel, 4U);
	length = phone_put_string(payload, 4U, from);
	length = phone_put_string(payload, length, text);
	words[0] = (uint32_t)(now >> 32);
	words[1] = (uint32_t)(now & 0xffffffffU);
	memcpy(payload + length, words, sizeof(words));
	length += sizeof(words);

	/* Each live phone object of each client that is not being ended. */
	told = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->dead || object->kind != KWL_SYSTEM_PHONE)
				continue;
			(void)kwl_emit(client, object->id, KL_SYSTEM_PHONE_EVENT_RECEIVED, payload, length);
			told++;
		}
	}

	/* The log the tests read: lengths only. */
	printf("KWL PHONE received channel=%u from=%lu text=%lu told=%u\n", channel, (unsigned long)strlen(from), (unsigned long)strlen(text), told);
}

/* Tells a request's state: status(request, state). */
static void
phone_status(
	struct kwl_object *object,
	uint32_t request,
	uint32_t state)
{
	uint32_t words[2];

	/* request, state. */
	words[0] = request;
	words[1] = state;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_STATUS, words, sizeof(words));
	printf("KWL PHONE status client=%llu request=%u state=%u\n", (unsigned long long)object->client->number, request, state);
}

/* Answers a request of version 16: result(request, applied, saved). */
static void
phone_result(
	struct kwl_object *object,
	uint32_t request,
	uint32_t applied)
{
	uint32_t words[3];

	/* request, applied, saved. */
	words[0] = request;
	words[1] = applied;
	words[2] = applied;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_RESULT, words, sizeof(words));
}

/* Answers a request of version 27: done(request, code), never lost. */
static void
phone_done(
	struct kwl_object *object,
	uint32_t request,
	uint32_t code)
{
	struct phone_owed owed;

	/* The done, owed when it cannot go now. */
	memset(&owed, 0, sizeof(owed));
	owed.kind = PHONE_OWED_DONE;
	owed.request = request;
	owed.code = code;
	phone_owe(object, &owed);
	printf("KWL PHONE done client=%llu request=%u code=%u\n", (unsigned long long)object->client->number, request, code);
}

/* Tells a sync's page's end: page_end(request, cursor, more, count, skipped, capped), never lost. */
static void
phone_page_end(
	struct kwl_object *object,
	uint32_t request,
	const struct kl_backend_phone_result *end)
{
	struct phone_owed owed;

	/* The page's end, owed when it cannot go now. */
	memset(&owed, 0, sizeof(owed));
	owed.kind = PHONE_OWED_PAGE_END;
	owed.request = request;
	owed.end = *end;
	phone_owe(object, &owed);
	printf("KWL PHONE page_end client=%llu request=%u count=%u more=%u capped=%u\n", (unsigned long long)object->client->number, request, end->count,
	    end->more, end->capped);
}

/* Tells that items were lost: dropped(), never lost. */
static void
phone_dropped(
	struct kwl_object *object)
{
	struct phone_owed owed;

	/* The drop, owed when it cannot go now. */
	memset(&owed, 0, sizeof(owed));
	owed.kind = PHONE_OWED_DROPPED;
	phone_owe(object, &owed);
	printf("KWL PHONE dropped client=%llu\n", (unsigned long long)object->client->number);
}

/*
 * Sends an event that is never lost, or owes it: while something is owed
 * it waits behind it, so that a later done never passes an earlier
 * page_end.  One drop owed is enough, and a link owed gives way to the
 * last.
 */
static void
phone_owe(
	struct kwl_object *object,
	const struct phone_owed *owed)
{
	struct phone_slot *slot;
	size_t index;
	int error;

	/* The new events go to objects of version 27 only, and none to a client being ended. */
	if (object == NULL || object->dead || object->client->fatal)
		return;
	if (object->version < KL_SYSTEM_SINCE_PHONE_SYNC)
		return;

	/* Nothing owed: it goes now, or is owed when the client reads too little. */
	slot = phone_slot_of(object, 0U);
	if (slot == NULL || slot->owed_count == 0U) {
		error = phone_emit_owed(object, owed);
		if (error != ENOBUFS)
			return;

		/* Owed: its slot. */
		slot = phone_slot_of(object, 1U);
		if (slot == NULL)
			return;
	}

	/* One drop owed is enough; a link owed gives way to the last. */
	for (index = 0U; index < slot->owed_count; index++) {
		if (owed->kind == PHONE_OWED_DROPPED && slot->owed[index].kind == PHONE_OWED_DROPPED)
			return;
		if (owed->kind == PHONE_OWED_LINK && slot->owed[index].kind == PHONE_OWED_LINK) {
			memmove(&slot->owed[index], &slot->owed[index + 1U], (slot->owed_count - index - 1U) * sizeof(slot->owed[0]));
			slot->owed_count--;
			break;
		}
	}

	/* Room, made when it is full. */
	if (slot->owed_count == PHONE_OWED_MAX)
		phone_owe_room(slot);

	/* Owed, after the others. */
	slot->owed[slot->owed_count] = *owed;
	slot->owed_count++;
	printf("KWL PHONE owed client=%llu kind=%u count=%lu\n", (unsigned long long)object->client->number, owed->kind, (unsigned long)slot->owed_count);
}

/*
 * Makes room among what an object is owed: the oldest page's end goes and
 * its done says NO_ROOM (the page is asked again); without one, the
 * oldest owed goes.
 */
static void
phone_owe_room(
	struct phone_slot *slot)
{
	uint32_t request;
	unsigned kind;
	size_t index;

	/* The oldest page's end, or the oldest owed. */
	for (index = 0U; index < slot->owed_count; index++) {
		if (slot->owed[index].kind == PHONE_OWED_PAGE_END)
			break;
	}

	/* No page's end: the oldest. */
	if (index == slot->owed_count)
		index = 0U;

	/* It goes. */
	request = slot->owed[index].request;
	kind = slot->owed[index].kind;
	memmove(&slot->owed[index], &slot->owed[index + 1U], (slot->owed_count - index - 1U) * sizeof(slot->owed[0]));
	slot->owed_count--;

	/* A page's end gone makes its page's done NO_ROOM. */
	if (kind != PHONE_OWED_PAGE_END)
		return;
	for (index = 0U; index < slot->owed_count; index++) {
		if (slot->owed[index].kind == PHONE_OWED_DONE && slot->owed[index].request == request)
			slot->owed[index].code = KL_SYSTEM_RESULT_NO_ROOM;
	}
}

/* Emits an event that is never lost.  Returns kwl_emit's answer (ENOBUFS when the client reads too little). */
static int
phone_emit_owed(
	struct kwl_object *object,
	const struct phone_owed *owed)
{
	unsigned char payload[PHONE_OWED_BYTES];
	size_t length;
	int error;

	/* Each kind. */
	error = 0;
	switch (owed->kind) {
	case PHONE_OWED_DONE:
		length = phone_put_word(payload, 0U, owed->request);
		length = phone_put_word(payload, length, owed->code);
		error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_DONE, payload, length);
		break;
	case PHONE_OWED_PAGE_END:
		length = phone_put_word(payload, 0U, owed->request);
		length = phone_put_string(payload, length, owed->end.cursor);
		length = phone_put_word(payload, length, owed->end.more);
		length = phone_put_word(payload, length, owed->end.count);
		length = phone_put_word(payload, length, owed->end.skipped);
		length = phone_put_word(payload, length, owed->end.capped);
		error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_PAGE_END, payload, length);
		break;
	case PHONE_OWED_LINK:
		/* The contacts' part first, to an object of version 28 (a link is sent again whole with it). */
		if (object->version >= KL_SYSTEM_SINCE_PHONE_CONTACTS) {
			length = phone_put_word(payload, 0U, owed->link.contacts);
			length = phone_put_word(payload, length, owed->link.record);
			length = phone_put_string(payload, length, owed->link.contacts_why);
			error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_LINK_CONTACTS, payload, length);
			if (error != 0)
				break;
		}

		/* The link. */
		length = phone_put_word(payload, 0U, owed->link.backend);
		length = phone_put_word(payload, length, owed->link.linked);
		length = phone_put_word(payload, length, owed->link.messages);
		length = phone_put_word(payload, length, owed->link.can_send);
		length = phone_put_word(payload, length, owed->link.notify);
		length = phone_put_word(payload, length, owed->link.owner);
		length = phone_put_word(payload, length, owed->link.enabled);
		length = phone_put_word(payload, length, owed->link.profiles);
		length = phone_put_word(payload, length, owed->link.present);
		length = phone_put_string(payload, length, owed->link.address);
		length = phone_put_string(payload, length, owed->link.why);
		error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_LINK, payload, length);
		break;
	case PHONE_OWED_DROPPED:
		error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_DROPPED, NULL, 0U);
		break;
	default:
		break;
	}

	/* Sent, or refused as kwl_emit said. */
	if (error != 0)
		return error;

	/* Succeeded: sent. */
	return 0;
}

/*
 * Fills the link's state as an object hears it: the setting's backend,
 * the paired phone's state (the loopback's is always ready, with no
 * contacts and no record known), or why it does not work; refused is 1
 * for a program that is not the phone program (why "not-phone").
 */
static void
phone_link_fill(
	unsigned refused,
	struct phone_link_event *link)
{
	const struct kl_backend_phone_state *state;

	/* The backend chosen. */
	memset(link, 0, sizeof(*link));
	link->backend = (uint32_t)phone_state.setting;

	/* Not the phone program: nothing but why. */
	if (refused) {
		(void)snprintf(link->why, sizeof(link->why), "%s", "not-phone");
		return;
	}

	/* The loopback: a phone always ready. */
	if (phone_state.setting == KL_SYSTEM_PHONE_BACKEND_LOOPBACK) {
		link->linked = 1U;
		link->messages = KL_BACKEND_PHONE_MESSAGES_READY;
		link->can_send = 1U;
		link->notify = 1U;
		link->owner = 1U;
		link->enabled = 1U;
		link->profiles = KL_BACKEND_PHONE_PROFILE_MESSAGES;
		link->present = 1U;
		return;
	}

	/* The paired phone's state as last read (Settings reads it whatever the setting). */
	state = &phone_state.state;
	link->linked = state->linked;
	link->messages = state->messages;
	link->can_send = state->can_send;
	link->notify = state->notify;
	link->owner = state->subscribed;
	link->enabled = state->enabled;
	link->profiles = state->profiles;
	link->present = state->present;
	(void)snprintf(link->address, sizeof(link->address), "%s", state->address);
	(void)snprintf(link->why, sizeof(link->why), "%s", state->why);

	/* The contacts. */
	link->contacts = state->contacts;
	(void)snprintf(link->contacts_why, sizeof(link->contacts_why), "%s", state->contacts_why);

	/*
	 * Whether the phone's record is known, whatever the setting (Stop
	 * using as phone sets it to none, and the phone program must still
	 * see that the record is off, ws197-p005 review-3 R1): not known while
	 * bluetoothd does not answer or before it said (R2), then none or one.
	 */
	link->record = 2U;
	if (!state->reachable) {
		link->record = 0U;
	} else if (!state->record_known) {
		link->record = 0U;
	} else if (!state->have_record) {
		link->record = 1U;
	}

	/* No backend chosen, and nothing else to say. */
	if (link->why[0] == '\0' && phone_state.setting == KL_SYSTEM_PHONE_BACKEND_NONE)
		(void)snprintf(link->why, sizeof(link->why), "%s", "no-backend");
}

/* Tells an object the link's state, never lost. */
static void
phone_link_tell(
	struct kwl_object *object,
	unsigned refused)
{
	struct phone_owed owed;

	/* The link, owed when it cannot go now. */
	memset(&owed, 0, sizeof(owed));
	owed.kind = PHONE_OWED_LINK;
	phone_link_fill(refused, &owed.link);
	phone_owe(object, &owed);
}

/* Tells every object that hears the link (listen or watch_link) its state. */
static void
phone_links(
	void)
{
	struct phone_slot *slot;
	unsigned refused;
	unsigned index;
	int app;

	/* Each object followed. */
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL)
			continue;
		if (!slot->listening && !slot->watching)
			continue;

		/* A listener that is not the phone program hears that it is not (a watcher hears the state). */
		refused = 0U;
		if (slot->listening && !slot->watching) {
			app = phone_is_phone_app(slot->object->client);
			if (!app)
				refused = 1U;
		}

		/* The state, or that it is refused. */
		phone_link_tell(slot->object, refused);
	}
}

/*
 * Emits an item to an object (a sync's request, or 0 for one that came):
 * item(request, what, handle, key, folder, direction, time_high,
 * time_low, zone, datetime, peer, name, flags, text).  Returns kwl_emit's
 * answer.
 */
static int
phone_item(
	struct kwl_object *object,
	uint32_t request,
	const struct kl_backend_phone_item *item)
{
	static unsigned char payload[PHONE_ITEM_MAX];
	uint32_t flags;
	size_t length;
	size_t text_length;
	int error;

	/* The new events go to objects of version 27 only, and none to a client being ended. */
	if (object->dead || object->client->fatal)
		return 0;
	if (object->version < KL_SYSTEM_SINCE_PHONE_SYNC)
		return 0;

	/* The flags. */
	flags = 0U;
	if (item->read)
		flags |= KL_SYSTEM_PHONE_ITEM_READ;
	if (item->partial)
		flags |= KL_SYSTEM_PHONE_ITEM_PARTIAL;
	if (item->truncated)
		flags |= KL_SYSTEM_PHONE_ITEM_TRUNCATED;

	/* The text, at most what a message has. */
	text_length = item->length;
	if (text_length > KL_SYSTEM_PHONE_ITEM_TEXT_MAX)
		text_length = KL_SYSTEM_PHONE_ITEM_TEXT_MAX;

	/* The words and the strings in their order. */
	length = phone_put_word(payload, 0U, request);
	length = phone_put_word(payload, length, item->what);
	length = phone_put_string(payload, length, item->handle);
	length = phone_put_string(payload, length, item->key);
	length = phone_put_word(payload, length, item->folder);
	length = phone_put_word(payload, length, item->direction);
	length = phone_put_word(payload, length, (uint32_t)((uint64_t)item->time >> 32));
	length = phone_put_word(payload, length, (uint32_t)((uint64_t)item->time & 0xffffffffU));
	length = phone_put_word(payload, length, item->zone);
	length = phone_put_string(payload, length, item->datetime);
	length = phone_put_string(payload, length, item->peer);
	length = phone_put_string(payload, length, item->name);
	length = phone_put_word(payload, length, flags);
	length = phone_put_array(payload, length, item->text, text_length);

	/* Sent, or refused as kwl_emit said. */
	error = kwl_emit(object->client, object->id, KL_SYSTEM_PHONE_EVENT_ITEM, payload, length);
	if (error != 0)
		return error;

	/* Succeeded: sent. */
	return 0;
}

/*
 * Hands a message that came to every phone program that listens; one that
 * read too little hears that it was dropped.  No program listening: nobody
 * keeps it (the next sync takes it), and a new one is told as a
 * notification.
 */
static void
phone_live(
	struct kwl_server *server,
	const struct kl_backend_phone_item *item)
{
	struct phone_slot *slot;
	unsigned index;
	unsigned told;
	int app;
	int error;

	/* Each phone program that listens. */
	told = 0U;
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL || !slot->listening)
			continue;
		app = phone_is_phone_app(slot->object->client);
		if (!app)
			continue;

		/* Behind what it is owed, dropped; else sent, dropped when it reads too little. */
		told++;
		if (slot->owed_count > 0U) {
			phone_dropped(slot->object);
			continue;
		}

		/* Sent, or dropped when the client reads too little. */
		error = phone_item(slot->object, 0U, item);
		if (error == ENOBUFS)
			phone_dropped(slot->object);
	}

	/* The log the tests read: lengths only. */
	printf("KWL PHONE live dir=%u text=%lu told=%u\n", item->direction, (unsigned long)item->length, told);

	/* No phone program to tell it: the compositor's own notification. */
	if (told == 0U)
		phone_notify(server, item);
}

/*
 * Tells a new message that came while no phone program listens (section
 * 7): a notification of the phone's, the other side's name (else its
 * number) and the first line, the lock screen's words "New message from
 * <name>", a click opening the phone program at the conversation; none
 * for one sent from the phone, one read there, or while the user turned
 * the phone's notifications off.
 */
static void
phone_notify(
	struct kwl_server *server,
	const struct kl_backend_phone_item *item)
{
	char line[PHONE_LINE_MAX];
	char title[KWL_NOTIFY_TITLE_MAX + 1U];
	char body[KWL_NOTIFY_BODY_MAX + 1U];
	char words[KWL_NOTIFY_TITLE_MAX * 2U];
	char lock_text[KWL_NOTIFY_TITLE_MAX + 1U];
	char command[KWL_NOTIFY_TITLE_MAX + 64U];
	const char *who;
	uint32_t id;
	size_t first;
	int allowed;
	int error;
	int safe;

	/* A message received, not read on the phone. */
	if (item->direction != KL_BACKEND_PHONE_DIRECTION_IN || item->read)
		return;

	/* The user's switch of the phone's notifications (on unless turned off). */
	error = kwl_settings_number(server, PHONE_NOTIFY_SETTING, &allowed);
	if (error == 0 && allowed == 0)
		return;

	/* The other side: its name on the phone, else its number. */
	who = item->peer;
	if (item->name[0] != '\0')
		who = item->name;
	(void)kwl_notify_clean(title, sizeof(title), who, 0);

	/* The first line of the words. */
	first = strcspn(item->text, "\n");
	if (first >= sizeof(line))
		first = sizeof(line) - 1U;
	memcpy(line, item->text, first);
	line[first] = '\0';
	(void)kwl_notify_clean(body, sizeof(body), line, 0);

	/* The lock screen's words, without the message's. */
	(void)snprintf(words, sizeof(words), "New message from %s", title);
	(void)kwl_notify_clean(lock_text, sizeof(lock_text), words, 0);

	/* A click opens the phone program at the conversation (the number only when the shell takes it as it is). */
	safe = phone_shell_safe(item->peer);
	(void)snprintf(command, sizeof(command), "%s", PHONE_COMMAND);
	if (safe)
		(void)snprintf(command, sizeof(command), "%s --peer '%s'", PHONE_COMMAND, item->peer);

	/* Posted. */
	id = kwl_notify_app_post(server, PHONE_APP_NAME, title, body, command, lock_text);
	printf("KWL PHONE notify id=%u title=%lu body=%lu peer=%d\n", id, (unsigned long)strlen(title), (unsigned long)strlen(body), safe);
}

/* Tells whether a number or sender's name goes in a command between single quotes as it is: 1 to 64 letters, digits and "+*#._ -". */
static int
phone_shell_safe(
	const char *text)
{
	size_t length;
	size_t span;

	/* Not empty, not long, and only those characters. */
	length = strlen(text);
	span = strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+*#._ -");
	if (length == 0U || length > 64U || span != length)
		return 0;

	/* Succeeded: safe. */
	return 1;
}

/* Hands the backend's answers to their objects. */
static void
phone_answers(
	void)
{
	struct kl_backend_phone_result result;
	struct phone_pending *pending;
	int taken;

	/* Each answer waiting. */
	for (;;) {
		taken = kl_backend_phone_take_result(phone_state.backend, &result);
		if (!taken)
			break;

		/* The request it answers (one given up meanwhile is not). */
		pending = phone_pending_find(result.id);
		if (pending == NULL)
			continue;
		phone_answer(pending, &result);
	}
}

/* Answers one request with the backend's answer. */
static void
phone_answer(
	struct phone_pending *pending,
	const struct kl_backend_phone_result *result)
{
	struct kwl_object *object;
	uint32_t code;

	/* Its object (NULL once it went) and the answer's code. */
	object = pending->object;
	code = phone_code(result->error);

	/* The kinds. */
	switch (pending->kind) {
	case PHONE_PENDING_SYNC:
		phone_state.sync_running = 0U;

		/* A page answered: its end, then done (NO_ROOM when one of its items was lost). */
		if (object != NULL && result->error == 0) {
			phone_page_end(object, pending->request, result);
			if (pending->spoiled)
				code = KL_SYSTEM_RESULT_NO_ROOM;
		}

		/* Its done, and the request is over. */
		if (object != NULL)
			phone_done(object, pending->request, code);
		pending->used = 0U;
		break;
	case PHONE_PENDING_SEND:
		if (object != NULL)
			phone_done(object, pending->request, code);

		/* A text in the outbox waits for its states by bluetoothd's number. */
		pending->used = 0U;
		if (result->error == 0 && object != NULL) {
			pending->used = 1U;
			pending->answered = 1U;
			pending->sent_request = result->sent_request;
		}

		/* Answered. */
		break;
	case PHONE_PENDING_V1_SEND:
		if (object != NULL && result->error != 0)
			phone_status(object, pending->request, KL_SYSTEM_PHONE_FAILED);

		/* A text in the outbox waits for its states by bluetoothd's number. */
		pending->used = 0U;
		if (result->error == 0 && object != NULL) {
			pending->used = 1U;
			pending->answered = 1U;
			pending->sent_request = result->sent_request;
		}

		/* Answered. */
		break;
	default:
		if (object != NULL)
			phone_done(object, pending->request, code);
		pending->used = 0U;
		break;
	}
}

/*
 * Hands the backend's items: a page's to the object of its sync (one that
 * reads too little, or is owed something, loses it and its page says
 * NO_ROOM), one that came to the phone programs listening (while the
 * setting is the paired phone).
 */
static void
phone_items(
	struct kwl_server *server)
{
	struct kl_backend_phone_item item;
	struct phone_pending *pending;
	struct phone_slot *slot;
	int taken;
	int error;

	/* Each item waiting. */
	for (;;) {
		taken = kl_backend_phone_take_item(phone_state.backend, &item);
		if (!taken)
			break;

		/* One that came by itself. */
		if (item.id == 0U) {
			if (phone_state.setting == KL_SYSTEM_PHONE_BACKEND_BLUETOOTH)
				phone_live(server, &item);
			continue;
		}

		/* A page's, for its sync's object. */
		pending = phone_pending_find(item.id);
		if (pending == NULL || pending->object == NULL)
			continue;

		/* Behind what the object is owed, lost. */
		slot = phone_slot_of(pending->object, 0U);
		if (slot != NULL && slot->owed_count > 0U) {
			pending->spoiled = 1U;
			continue;
		}

		/* Sent, or lost when the client reads too little. */
		error = phone_item(pending->object, pending->request, &item);
		if (error == ENOBUFS)
			pending->spoiled = 1U;
	}
}

/* Hands the texts' states to their objects; one that came before the answer naming its number is held a while. */
static void
phone_sents(
	uint64_t now)
{
	uint32_t sent_request;
	unsigned state;
	unsigned index;
	int taken;
	int matched;

	/* Each state waiting. */
	for (;;) {
		taken = kl_backend_phone_take_sent(phone_state.backend, &sent_request, &state);
		if (!taken)
			break;

		/* Its text's object. */
		matched = phone_sent_match(sent_request, state);
		if (matched)
			continue;

		/* Held a while (the oldest goes when all are held). */
		index = 0U;
		while (index < PHONE_HELD_MAX && phone_state.held[index].used)
			index++;
		if (index == PHONE_HELD_MAX) {
			memmove(&phone_state.held[0], &phone_state.held[1], (PHONE_HELD_MAX - 1U) * sizeof(phone_state.held[0]));
			index = PHONE_HELD_MAX - 1U;
		}

		/* The state, with when it came. */
		phone_state.held[index].used = 1U;
		phone_state.held[index].sent_request = sent_request;
		phone_state.held[index].state = state;
		phone_state.held[index].at_ms = now;
	}
}

/* Tells a text's state to its object by bluetoothd's number.  Returns 1 when a text has that number. */
static int
phone_sent_match(
	uint32_t sent_request,
	unsigned state)
{
	struct phone_pending *candidate;
	unsigned index;

	/* Each text answered. */
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		candidate = &phone_state.pending[index];
		if (!candidate->used || !candidate->answered)
			continue;
		if (candidate->sent_request != sent_request)
			continue;

		/* Its state (the object may have gone meanwhile). */
		if (candidate->object != NULL)
			phone_status(candidate->object, candidate->request, state);
		return 1;
	}

	/* No text has it (yet). */
	return 0;
}

/* Tells the held states whose text's number came; one held too long goes. */
static void
phone_held_check(
	uint64_t now)
{
	struct phone_held *held;
	unsigned index;
	int matched;

	/* Each held. */
	for (index = 0U; index < PHONE_HELD_MAX; index++) {
		held = &phone_state.held[index];
		if (!held->used)
			continue;

		/* Told, or given up after its time. */
		matched = phone_sent_match(held->sent_request, held->state);
		if (matched || now - held->at_ms >= PHONE_HOLD_MS)
			held->used = 0U;
	}
}

/*
 * Gives up a sync that waited too long (TIMEOUT), and starts the one that
 * waited longest when the paired phone carries none.
 */
static void
phone_sync_next(
	uint64_t now)
{
	struct phone_slot *slot;
	struct phone_slot *oldest;
	unsigned index;

	/* Each sync waiting. */
	oldest = NULL;
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL || !slot->sync_waiting)
			continue;

		/* Waited too long. */
		if (now - slot->sync_since_ms >= PHONE_SYNC_WAIT_MS) {
			slot->sync_waiting = 0U;
			phone_done(slot->object, slot->sync_request, KL_SYSTEM_RESULT_TIMEOUT);
			continue;
		}

		/* The one that waited longest. */
		if (oldest == NULL || slot->sync_since_ms < oldest->sync_since_ms)
			oldest = slot;
	}

	/* Its turn, when the paired phone carries none. */
	if (oldest == NULL || phone_state.sync_running)
		return;
	oldest->sync_waiting = 0U;
	phone_bluetooth_sync(oldest->object, oldest->sync_request, &oldest->sync_page);
}

/*
 * Ends the messages that went through the paired phone when the setting
 * changes away from it: every request waiting is answered UNAVAILABLE (a
 * text of version 16 FAILED), and the syncs waiting too.  The phone's
 * switch (link_set) goes on.
 */
static void
phone_unavailable(
	void)
{
	struct phone_pending *pending;
	struct phone_slot *slot;
	unsigned index;

	/* Each request waiting but the switch's. */
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		pending = &phone_state.pending[index];
		if (!pending->used || pending->kind == PHONE_PENDING_LINK)
			continue;

		/* Answered as unavailable, unless only its states were awaited. */
		if (!pending->answered && pending->object != NULL) {
			if (pending->kind == PHONE_PENDING_V1_SEND) {
				phone_status(pending->object, pending->request, KL_SYSTEM_PHONE_FAILED);
			} else {
				phone_done(pending->object, pending->request, KL_SYSTEM_RESULT_UNAVAILABLE);
			}
		}

		/* Not followed any more. */
		pending->used = 0U;
	}

	/* No sync at the backend is followed any more. */
	phone_state.sync_running = 0U;

	/* Each sync waiting. */
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL || !slot->sync_waiting)
			continue;
		slot->sync_waiting = 0U;
		phone_done(slot->object, slot->sync_request, KL_SYSTEM_RESULT_UNAVAILABLE);
	}
}

/* Forgets the texts waiting for their states (bluetoothd started again: its numbers start again). */
static void
phone_forget_texts(
	void)
{
	unsigned index;

	/* Each text answered, and each state held. */
	for (index = 0U; index < PHONE_PENDING_MAX; index++) {
		if (phone_state.pending[index].answered)
			phone_state.pending[index].used = 0U;
	}

	/* None held. */
	memset(phone_state.held, 0, sizeof(phone_state.held));
}

/* Tells every phone program that listens that items were dropped (it synchronises again). */
static void
phone_drops(
	void)
{
	struct phone_slot *slot;
	unsigned index;
	int app;

	/* Each phone program that listens. */
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL || !slot->listening)
			continue;
		app = phone_is_phone_app(slot->object->client);
		if (app)
			phone_dropped(slot->object);
	}
}

/* Sends what the objects were owed, in order, as far as each client takes it. */
static void
phone_owed_flush(
	void)
{
	struct phone_slot *slot;
	size_t sent;
	unsigned index;
	int error;

	/* Each object owed something. */
	for (index = 0U; index < PHONE_SLOTS; index++) {
		slot = &phone_state.slots[index];
		if (slot->object == NULL || slot->owed_count == 0U)
			continue;

		/* As many as go now (an event of another failure is not owed again). */
		sent = 0U;
		while (sent < slot->owed_count) {
			error = phone_emit_owed(slot->object, &slot->owed[sent]);
			if (error == ENOBUFS)
				break;
			sent++;
		}

		/* Those sent leave the list. */
		memmove(&slot->owed[0], &slot->owed[sent], (slot->owed_count - sent) * sizeof(slot->owed[0]));
		slot->owed_count -= sent;
	}
}

/*
 * Reads a string argument in place, at most bound bytes with its NUL;
 * *next is the offset after it.  Returns 0, or EPROTO.
 */
static int
phone_string(
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

	/* The length, with the NUL, within the request. */
	if (offset + 4U > size)
		return EPROTO;
	length = phone_word(bytes, offset);

	/* Within the bound. */
	if (length == 0U || length > bound)
		return EPROTO;

	/* The bytes, padded to a word, within the request. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;

	/* The text ends with its NUL. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;

	/* And has no other. */
	inner = memchr(bytes + offset + 4U, '\0', length - 1U);
	if (inner != NULL)
		return EPROTO;

	/* Succeeded: the text where it is, and where the next argument starts. */
	*text = (const char *)(bytes + offset + 4U);
	*next = offset + 4U + padded;
	return 0;
}

/*
 * Reads an array argument in place, at most bound bytes; *next is the
 * offset after it.  Returns 0, or EPROTO.
 */
static int
phone_array(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	size_t bound,
	const uint8_t **data,
	size_t *length,
	size_t *next)
{
	uint32_t count;
	size_t padded;

	/* The length within the request and the bound. */
	if (offset + 4U > size)
		return EPROTO;
	count = phone_word(bytes, offset);
	if (count > bound)
		return EPROTO;

	/* The bytes, padded to a word, within the request. */
	padded = ((size_t)count + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;

	/* Succeeded: the bytes where they are, and where the next argument starts. */
	*data = bytes + offset + 4U;
	*length = count;
	*next = offset + 4U + padded;
	return 0;
}

/* Writes a string argument and reports the offset after it. */
static size_t
phone_put_string(
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

	/* The offset after the string. */
	return offset + 4U + padded;
}

/* Writes a word argument and reports the offset after it. */
static size_t
phone_put_word(
	unsigned char *payload,
	size_t offset,
	uint32_t word)
{
	/* The word in the wire's native byte order. */
	memcpy(payload + offset, &word, sizeof(word));

	/* The offset after it. */
	return offset + sizeof(word);
}

/* Writes an array argument and reports the offset after it. */
static size_t
phone_put_array(
	unsigned char *payload,
	size_t offset,
	const void *data,
	size_t length)
{
	uint32_t count;
	size_t padded;

	/* The length, the bytes, and zeros to a four-byte boundary. */
	count = (uint32_t)length;
	padded = (length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &count, sizeof(count));
	memset(payload + offset + 4U, 0, padded);
	if (length > 0U)
		memcpy(payload + offset + 4U, data, length);

	/* The offset after the array. */
	return offset + 4U + padded;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
phone_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
