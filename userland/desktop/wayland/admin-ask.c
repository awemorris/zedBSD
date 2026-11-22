/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The admin actions in the compositor: kl_system_admin_v1's objects, and
 * the relay of their requests to the session manager.
 *
 * An admin action is a program the system's definition names, run as
 * root by the session manager (sessiond) once the person at the screen
 * allowed it in the compositor's own dialog.  A client asks with run; the
 * compositor checks that the client's window is the one the person works
 * in (it has the keyboard, or gets it within ADMIN_FRONT_MS), and asks the
 * manager (libkeiland-backend's ADMIN ASK).  The manager answers:
 *
 *   SHOW      the dialog: the client hears asking, the dialog shows, and
 *             the person's answer goes back as ALLOW or CANCEL
 *   CONNECT   a permission kept for a program of the desktop: ALLOW with
 *             a connection at once
 *   STARTED   the program runs: its streams go to the client (one stream
 *             event each), then started
 *   REFUSED, FAILED   the client hears refused
 *
 * A program of the desktop (the installer) gets a connection the
 * compositor makes for it: a socket pair whose one end is a client of the
 * compositor's own making, marked as lent to a program run as root, and
 * whose other end goes to the manager with the ALLOW.  The program never
 * reaches the user's own socket.
 *
 * One request is under way at a time on the whole desktop, from the run
 * to its answer; another is refused busy, and so is one while another
 * dialog shows.  A request withdrawn, or whose object or client went, is
 * answered no more: a dialog shown for it closes and is cancelled, and
 * the streams of a program started for it are closed (the program sees
 * its input end and stops).  A client that asked tells the manager, when
 * it ends, to forget what it was allowed.  The screen's lock refuses a
 * request waiting for its window or its dialog.
 *
 * The dialog itself is not drawn yet: where it would show, the request is
 * refused as unsupported and cancelled, unless the test build gives the
 * place its own answer (admin_ask_test_show).
 */

#include "kwl.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* How long a request waits for its window to have the keyboard, and the dialog for the person's answer (milliseconds). */
#define ADMIN_FRONT_MS		5000U
#define ADMIN_DIALOG_MS		60000U

/* The longest string a run may carry, with its NUL; a longer one is malformed. */
#define ADMIN_WIRE_TEXT_MAX	1024U

/* The room for the desktop's locale as the manager reads it ("ll_CC.UTF-8"), with its NUL. */
#define ADMIN_LOCALE_SIZE	16U

/*
 * Where the request under way is (struct admin_request's stage): none,
 * waiting for its window to have the keyboard, waiting for the manager's
 * answer to its ASK, showing its dialog, waiting for the manager's answer
 * to its ALLOW.
 */
#define ADMIN_IDLE		0U
#define ADMIN_FRONT		1U
#define ADMIN_ASKED		2U
#define ADMIN_SHOWN		3U
#define ADMIN_ALLOWED		4U

/*
 * The request under way.
 *
 * object is the admin object that asked, NULL once the request was
 * withdrawn or the object went (its answer then goes to nobody); client
 * is its client's number, number the client's own number of the request
 * and surface the surface it named.  id is the compositor's number of the
 * request to the manager (0 before the ASK).  action, words and streams
 * are what it asked for; how, display, title and message what the
 * manager's SHOW said.  due_ms is when the wait of the stage ends
 * (kwl_milliseconds' clock).
 */
struct admin_request {
	unsigned stage;
	struct kwl_object *object;
	uint64_t client;
	uint32_t number;
	uint32_t surface;
	uint32_t id;
	char action[KL_SYSTEM_ADMIN_ACTION_MAX + 1U];
	char words[KL_SYSTEM_ADMIN_WORDS_MAX + 1U];
	unsigned streams;
	unsigned how;
	unsigned display;
	char title[KL_BACKEND_ADMIN_TEXT];
	char message[KL_BACKEND_ADMIN_TEXT];
	uint64_t due_ms;
};

/*
 * The admin actions' state, the event loop's: the request under way
 * (stage ADMIN_IDLE for none), the number the next request to the
 * manager gets (it skips 0, which numbers no request), and the timer that
 * wakes the loop when a stage's wait ends.
 */
static struct {
	struct admin_request current;
	uint32_t next_id;
	struct kwl_timer timer;
} admin_state;

/*
 * The test build's dialog.
 *
 * The test build defines it to stand for the dialog: it returns 1 when
 * the dialog shows (its answer comes later through kwl_admin_decide), or
 * 0 when it cannot.  It is weak so that the compositor links without it,
 * and where it is absent no dialog shows yet.
 */
extern int admin_ask_test_show(struct kwl_server *server, const char *title, const char *message, unsigned how, unsigned display) __attribute__((weak));

static int admin_decode(const unsigned char *bytes, size_t size, size_t *offset, char *text, size_t room);
static uint32_t admin_word(const unsigned char *bytes, size_t offset);
static int admin_action_valid(const char *action);
static int admin_words_valid(const char *words);
static int admin_front(struct kwl_server *server, const struct admin_request *request);
static void admin_ask(struct kwl_server *server);
static void admin_locale(const struct kwl_server *server, char *locale, size_t size);
static void admin_show(struct kwl_server *server, struct kl_backend_admin_answer *answer);
static void admin_allow(struct kwl_server *server);
static struct kwl_client *admin_lend(struct kwl_server *server, int descriptor);
static void admin_started(struct kl_backend_admin_answer *answer);
static void admin_refuse(uint32_t reason, const char *why);
static void admin_refuse_object(struct kwl_object *object, uint32_t number, uint32_t reason, const char *why);
static uint32_t admin_reason(unsigned reason);
static void admin_cancel(struct kwl_server *server, const char *why);
static void admin_abandon(struct kwl_server *server, const char *why);
static void admin_clear(void);
static void admin_wake(struct kwl_server *server);

/*
 * Makes an admin object for the manager's get_admin.  Returns 0, or
 * EPROTO.
 */
int
kwl_admin_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = admin_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_ADMIN, manager->version);
	if (created == NULL)
		return EPROTO;

	/* Succeeded: the object is the client's; it has no first state. */
	printf("KWL ADMIN object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);
	return 0;
}

/*
 * Carries out a request of an admin object.  Returns 0, or EPROTO for a
 * malformed request (the client ends).
 */
int
kwl_admin_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *surface;
	struct admin_request *current;
	char action[ADMIN_WIRE_TEXT_MAX];
	char words[ADMIN_WIRE_TEXT_MAX];
	uint32_t number;
	uint32_t streams;
	uint32_t surface_id;
	size_t offset;
	int available;
	int showing;
	int valid;
	int error;

	/* The object goes; what it asked goes with it (kwl_admin_gone). */
	server = object->client->server;
	current = &admin_state.current;
	if (opcode == KL_SYSTEM_ADMIN_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* withdraw: the request under way, when it is this object's, is answered no more. */
	if (opcode == KL_SYSTEM_ADMIN_WITHDRAW) {
		if (size != 4U)
			return EPROTO;
		number = admin_word(bytes, 0U);
		if (current->stage != ADMIN_IDLE && current->object == object && current->number == number)
			admin_abandon(server, "withdraw");
		return 0;
	}

	/* Nothing else but run. */
	if (opcode != KL_SYSTEM_ADMIN_RUN)
		return EPROTO;

	/* run: the request's number, the action, the words, the streams and the surface. */
	if (size < 4U)
		return EPROTO;
	number = admin_word(bytes, 0U);
	offset = 4U;
	error = admin_decode(bytes, size, &offset, action, sizeof(action));
	if (error != 0)
		return error;
	error = admin_decode(bytes, size, &offset, words, sizeof(words));
	if (error != 0)
		return error;
	if (offset + 8U != size)
		return EPROTO;
	streams = admin_word(bytes, offset);
	surface_id = admin_word(bytes, offset + 4U);

	/* The streams are of the three, and the surface is a live surface of the client's. */
	if ((streams & ~KL_SYSTEM_ADMIN_STREAMS) != 0U)
		return EPROTO;
	surface = kwl_find(object->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE || surface->dead)
		return EPROTO;

	/* The client asked: its end makes the manager forget what it was allowed. */
	object->client->admin_asked = 1U;

	/* No manager runs actions here (the login screen, no session manager, another system). */
	available = kwl_admin_available(server);
	if (!available) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_UNSUPPORTED, "unsupported");
		return 0;
	}

	/* The locked screen takes no request. */
	if (server->locked) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_LOCKED, "locked");
		return 0;
	}

	/* An action's name and words the manager would read; others name no action. */
	valid = admin_action_valid(action);
	if (valid)
		valid = admin_words_valid(words);
	if (!valid) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_UNKNOWN, "unknown");
		return 0;
	}
	printf("KWL ADMIN ask client=%llu request=%u action=%s\n", (unsigned long long)object->client->number, number, action);

	/* One request at a time, and none while another dialog shows. */
	if (current->stage != ADMIN_IDLE) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_BUSY, "busy");
		return 0;
	}
	showing = kwl_bluetooth_ask_showing();
	if (!showing)
		showing = kwl_power_dialog_showing(server);
	if (showing) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_BUSY, "busy-dialog");
		return 0;
	}

	/* The request under way (the name and the words were found within their bounds): it waits for its window to have the keyboard. */
	memset(current, 0, sizeof(*current));
	current->stage = ADMIN_FRONT;
	current->object = object;
	current->client = object->client->number;
	current->number = number;
	current->surface = surface_id;
	memcpy(current->action, action, strlen(action) + 1U);
	memcpy(current->words, words, strlen(words) + 1U);
	current->streams = streams;
	current->due_ms = kwl_milliseconds() + ADMIN_FRONT_MS;

	/* Asked at once when its window has the keyboard already, else by the tick. */
	kwl_admin_tick(server);

	/* Succeeded: the request is under way, or answered. */
	return 0;
}

/*
 * Looks after the request under way once a pass: a window that came to
 * the front, the lock, and the end of a wait.
 */
void
kwl_admin_tick(
	struct kwl_server *server)
{
	struct admin_request *current;
	uint64_t now;
	int front;

	/* Nothing under way. */
	current = &admin_state.current;
	if (current->stage == ADMIN_IDLE)
		return;

	/* The time the waits are measured against. */
	now = kwl_milliseconds();

	/* Waiting for the window: the lock refuses it, the front asks, the end of the wait refuses it. */
	if (current->stage == ADMIN_FRONT) {
		if (server->locked) {
			admin_refuse(KL_SYSTEM_ADMIN_LOCKED, "locked");
			return;
		}
		front = admin_front(server, current);
		if (front) {
			admin_ask(server);
			return;
		}
		if (now >= current->due_ms) {
			admin_refuse(KL_SYSTEM_ADMIN_NOT_FRONT, "not-front");
			return;
		}
	}

	/* The dialog: the lock and the login screen close it, and so does the end of its time. */
	if (current->stage == ADMIN_SHOWN) {
		if (server->locked || server->greeter) {
			admin_cancel(server, "locked");
			admin_refuse(KL_SYSTEM_ADMIN_LOCKED, "locked");
			return;
		}
		if (now >= current->due_ms) {
			admin_cancel(server, "timeout");
			admin_refuse(KL_SYSTEM_ADMIN_TIMEOUT, "timeout");
			return;
		}
	}

	/* The next pass comes when the wait ends. */
	admin_wake(server);
}

/*
 * Lets the request under way go when its object goes: it is answered no
 * more (objects.c calls it for every admin object's destroy, its client's
 * end included).
 */
void
kwl_admin_gone(
	struct kwl_object *object)
{
	struct admin_request *current;

	/* Only the object of the request under way. */
	current = &admin_state.current;
	if (current->stage == ADMIN_IDLE || current->object != object)
		return;

	/* The request is let go. */
	admin_abandon(object->client->server, "gone");
}

/*
 * Tells the manager that a client that asked for an action went, so that
 * what it was allowed goes (objects.c calls it for every client's end).
 */
void
kwl_admin_client_gone(
	struct kwl_client *client)
{
	int error;

	/* A client that never asked was allowed nothing. */
	if (!client->admin_asked)
		return;

	/* FORGET; it has no answer. */
	error = kl_backend_session_admin_forget(client->server->backend, client->number);
	printf("KWL ADMIN forget client=%llu error=%d\n", (unsigned long long)client->number, error);
}

/*
 * The backend's session_admin: the manager's answer to the request under
 * way.  The descriptors of a STARTED this keeps are set to -1; the backend
 * closes the others.
 */
void
kwl_admin_answer(
	void *data,
	struct kl_backend_admin_answer *answer)
{
	struct kwl_server *server;
	struct admin_request *current;

	/* Only the request under way asked, and only in a stage that waits for the manager. */
	server = data;
	current = &admin_state.current;
	printf("KWL ADMIN answer id=%u kind=%u\n", answer->id, answer->kind);
	if (current->stage != ADMIN_ASKED && current->stage != ADMIN_ALLOWED) {
		/* A dialog or a connection asked for nothing under way is cancelled. */
		if (answer->kind == KL_BACKEND_ADMIN_SHOW || answer->kind == KL_BACKEND_ADMIN_CONNECT)
			(void)kl_backend_session_admin_cancel(server->backend, answer->id);
		return;
	}
	if (answer->id != current->id) {
		/* The same, for another request than the one under way. */
		if (answer->kind == KL_BACKEND_ADMIN_SHOW || answer->kind == KL_BACKEND_ADMIN_CONNECT)
			(void)kl_backend_session_admin_cancel(server->backend, answer->id);
		return;
	}

	/* Each answer. */
	switch (answer->kind) {
	case KL_BACKEND_ADMIN_SHOW:
		admin_show(server, answer);
		break;
	case KL_BACKEND_ADMIN_CONNECT:
		/* A permission kept: the connection goes without the dialog, to a request still wanted. */
		if (current->object == NULL) {
			(void)kl_backend_session_admin_cancel(server->backend, current->id);
			admin_clear();
			break;
		}
		current->display = 1U;
		admin_allow(server);
		break;
	case KL_BACKEND_ADMIN_STARTED:
		admin_started(answer);
		break;
	case KL_BACKEND_ADMIN_REFUSED:
		admin_refuse(admin_reason(answer->reason), "manager");
		break;
	default:
		/* FAILED: not started, not answered in time, or the manager went. */
		printf("KWL ADMIN failed id=%u errno=%d\n", answer->id, answer->error);
		admin_refuse(KL_SYSTEM_ADMIN_FAILED, "failed");
		break;
	}
}

/*
 * Takes the person's answer to the dialog: allow 1 sends the ALLOW, 0
 * refuses the request (no) and cancels it with the manager.  via says what
 * answered, for the log.
 */
void
kwl_admin_decide(
	struct kwl_server *server,
	unsigned allow,
	const char *via)
{
	struct admin_request *current;

	/* Only a dialog showing takes an answer. */
	current = &admin_state.current;
	if (current->stage != ADMIN_SHOWN)
		return;
	printf("KWL ADMIN decide id=%u allow=%u via=%s\n", current->id, allow, via);

	/* Allowed: the ALLOW, with a connection for a program of the desktop. */
	if (allow) {
		admin_allow(server);
		return;
	}

	/* Not allowed: the manager forgets it, and the client hears no. */
	admin_cancel(server, via);
	admin_refuse(KL_SYSTEM_ADMIN_NO, via);
}

/*
 * Tells whether admin actions may be asked here: a session (not the login
 * screen) whose manager listens.  Returns 1 or 0.
 */
int
kwl_admin_available(
	struct kwl_server *server)
{
	int managed;

	/* The login screen runs no action. */
	if (server->greeter)
		return 0;

	/* A session manager that listens runs them. */
	managed = kl_backend_session_managed(server->backend);
	if (!managed)
		return 0;

	/* Succeeded: actions may be asked. */
	return 1;
}

/*
 * Reads one string argument of a request at *offset into text (room
 * bytes with its NUL), and moves *offset past it.  Returns 0, or EPROTO
 * for a string malformed or longer than the room.
 */
static int
admin_decode(
	const unsigned char *bytes,
	size_t size,
	size_t *offset,
	char *text,
	size_t room)
{
	const void *inner;
	uint32_t length;
	size_t padded;

	/* The length, with the NUL, within the request and the room. */
	if (*offset + 4U > size)
		return EPROTO;
	length = admin_word(bytes, *offset);
	if (length == 0U || length > room)
		return EPROTO;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (*offset + 4U + padded > size)
		return EPROTO;

	/* The text ends with its NUL and has no other. */
	if (bytes[*offset + 4U + length - 1U] != '\0')
		return EPROTO;
	inner = memchr(bytes + *offset + 4U, '\0', length - 1U);
	if (inner != NULL)
		return EPROTO;

	/* Succeeded: the text, and the next argument's place. */
	memcpy(text, bytes + *offset + 4U, length);
	*offset += 4U + padded;
	return 0;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
admin_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}

/* Tells whether an action's name is one the manager reads: a lowercase letter or digit, then those and '-', at most 32. */
static int
admin_action_valid(
	const char *action)
{
	size_t index;
	char byte;

	/* Each byte, the first not a '-'. */
	for (index = 0U; action[index] != '\0'; index++) {
		byte = action[index];
		if (index >= KL_SYSTEM_ADMIN_ACTION_MAX)
			return 0;
		if (byte >= 'a' && byte <= 'z')
			continue;
		if (byte >= '0' && byte <= '9')
			continue;
		if (byte == '-' && index != 0U)
			continue;

		/* A byte no name has. */
		return 0;
	}

	/* An empty name is none. */
	if (index == 0U)
		return 0;

	/* Succeeded: a name. */
	return 1;
}

/*
 * Tells whether the words are ones the manager reads: at most 8 words of
 * 1 to 64 bytes ([A-Za-z0-9._/:=@+,-]) joined by one space, at most 256
 * bytes (empty for none).
 */
static int
admin_words_valid(
	const char *words)
{
	const char *punctuation;
	size_t index;
	size_t word_length;
	unsigned count;
	char byte;

	/* No words. */
	if (words[0] == '\0')
		return 1;

	/* Each byte: a word's, or the one space between two words. */
	word_length = 0U;
	count = 1U;
	for (index = 0U; words[index] != '\0'; index++) {
		byte = words[index];
		if (index >= KL_SYSTEM_ADMIN_WORDS_MAX)
			return 0;

		/* A space ends a word that is not empty, and starts the next. */
		if (byte == ' ') {
			if (word_length == 0U)
				return 0;
			word_length = 0U;
			count++;
			if (count > KL_SYSTEM_ADMIN_WORD_COUNT)
				return 0;
			continue;
		}

		/* A letter, a digit or the punctuation a path or an option has, within a word's length. */
		punctuation = strchr("._/:=@+,-", byte);
		if ((byte < 'a' || byte > 'z') &&
		    (byte < 'A' || byte > 'Z') &&
		    (byte < '0' || byte > '9') &&
		    punctuation == NULL)
			return 0;
		word_length++;
		if (word_length > KL_SYSTEM_ADMIN_WORD_MAX)
			return 0;
	}

	/* The last word is not empty (no space at the end). */
	if (word_length == 0U)
		return 0;

	/* Succeeded: words the manager reads. */
	return 1;
}

/*
 * Tells whether the request's window is the one the person works in: its
 * surface is still there, its client has the keyboard, and the screen is
 * not locked.  Returns 1 or 0.
 */
static int
admin_front(
	struct kwl_server *server,
	const struct admin_request *request)
{
	struct kwl_object *surface;

	/* A request whose object went has no window. */
	if (request->object == NULL)
		return 0;

	/* The surface it named, still a live surface. */
	surface = kwl_find(request->object->client, request->surface);
	if (surface == NULL || surface->kind != KWL_SURFACE || surface->dead)
		return 0;

	/* The keyboard's surface is one of the client's (its window, or a popup of it). */
	if (server->focus == NULL || server->focus->dead)
		return 0;
	if (server->focus->client != request->object->client)
		return 0;

	/* Succeeded: the client's window has the keyboard. */
	return 1;
}

/* Asks the manager for the request under way (ADMIN ASK); a request it cannot take is refused. */
static void
admin_ask(
	struct kwl_server *server)
{
	struct admin_request *current;
	char locale[ADMIN_LOCALE_SIZE];
	uint32_t reason;
	int error;

	/* The request's number to the manager, never 0. */
	current = &admin_state.current;
	admin_state.next_id++;
	if (admin_state.next_id == 0U)
		admin_state.next_id = 1U;
	current->id = admin_state.next_id;

	/* The request, in the desktop's locale. */
	admin_locale(server, locale, sizeof(locale));
	error = kl_backend_session_admin_ask(server->backend, current->id, current->client, locale, current->streams, current->action, current->words);
	if (error != 0) {
		printf("KWL ADMIN ask-failed id=%u errno=%d\n", current->id, error);
		reason = KL_SYSTEM_ADMIN_FAILED;
		if (error == EBUSY)
			reason = KL_SYSTEM_ADMIN_BUSY;
		else if (error == ENOTSUP)
			reason = KL_SYSTEM_ADMIN_UNSUPPORTED;
		admin_refuse(reason, "ask");
		return;
	}

	/* Succeeded: the manager's answer is awaited (the backend gives up on it in time). */
	current->stage = ADMIN_ASKED;
	printf("KWL ADMIN asked id=%u client=%llu action=%s streams=%x\n", current->id, (unsigned long long)current->client, current->action, current->streams);
}

/*
 * Writes the desktop's locale as the manager reads it: the language's
 * locale ("ja_JP.UTF-8"), a language with its region and the encoding, or
 * "" when there is none to say.
 */
static void
admin_locale(
	const struct kwl_server *server,
	char *locale,
	size_t size)
{
	static const struct {
		const char *language;
		const char *locale;
	} locales[] = {
		{ "en", "en_US.UTF-8" },
		{ "ja", "ja_JP.UTF-8" },
	};
	const char *region;
	size_t length;
	size_t index;
	int same;

	/* A language the table knows. */
	locale[0] = '\0';
	for (index = 0U; index < sizeof(locales) / sizeof(locales[0]); index++) {
		same = strcmp(server->language_asked, locales[index].language);
		if (same == 0) {
			(void)snprintf(locale, size, "%s", locales[index].locale);
			return;
		}
	}

	/* A language with its region ("ll_CC") that fits takes the encoding; the backend refuses any other form. */
	region = strchr(server->language_asked, '_');
	length = strlen(server->language_asked);
	if (region != NULL && length + sizeof(".UTF-8") <= size) {
		memcpy(locale, server->language_asked, length);
		memcpy(locale + length, ".UTF-8", sizeof(".UTF-8"));
	}
}

/*
 * Shows the dialog the manager asked for: the client hears asking, and
 * the person's answer comes through kwl_admin_decide.  A request no longer
 * wanted, a way of confirming not made yet, and a dialog that cannot show
 * are cancelled and refused.
 */
static void
admin_show(
	struct kwl_server *server,
	struct kl_backend_admin_answer *answer)
{
	struct admin_request *current;
	uint32_t payload[1];
	int shown;
	int error;

	/* A request withdrawn meanwhile: the manager forgets it. */
	current = &admin_state.current;
	if (current->object == NULL) {
		(void)kl_backend_session_admin_cancel(server->backend, current->id);
		admin_clear();
		return;
	}

	/* Only the person's consent is asked yet: a password is a way not made. */
	if (answer->how != KL_BACKEND_ADMIN_CONSENT) {
		admin_cancel(server, "method");
		admin_refuse(KL_SYSTEM_ADMIN_METHOD, "method");
		return;
	}

	/* What the dialog says, and whether its allow lends a connection. */
	current->how = answer->how;
	current->display = answer->display;
	(void)snprintf(current->title, sizeof(current->title), "%s", answer->title);
	(void)snprintf(current->message, sizeof(current->message), "%s", answer->message);

	/* The dialog's place: shown by the test build's stand-in, or not yet. */
	shown = 0;
	if (admin_ask_test_show != NULL)
		shown = admin_ask_test_show(server, current->title, current->message, current->how, current->display);
	if (!shown) {
		admin_cancel(server, "unsupported");
		admin_refuse(KL_SYSTEM_ADMIN_UNSUPPORTED, "no-dialog");
		return;
	}

	/* The dialog shows: its time runs, and the client hears asking. */
	current->stage = ADMIN_SHOWN;
	current->due_ms = kwl_milliseconds() + ADMIN_DIALOG_MS;
	printf("KWL ADMIN show id=%u how=consent display=%u\n", current->id, current->display);
	payload[0] = current->number;
	error = kwl_emit(current->object->client, current->object->id, KL_SYSTEM_ADMIN_EVENT_ASKING, payload, sizeof(payload));
	if (error != 0)
		printf("KWL ADMIN emit-failed event=asking errno=%d\n", error);
	admin_wake(server);
}

/*
 * Sends the ALLOW of the request under way, with a connection made for a
 * program of the desktop; a request that cannot be allowed is cancelled
 * and refused.
 */
static void
admin_allow(
	struct kwl_server *server)
{
	struct admin_request *current;
	struct kwl_client *lent;
	int pair[2];
	int connection;
	int error;

	/* A program of the desktop gets a connection: one end the compositor's client, the other the program's. */
	current = &admin_state.current;
	connection = -1;
	if (current->display) {
		error = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
		if (error != 0) {
			printf("KWL ADMIN lend-failed step=socketpair errno=%d\n", errno);
			admin_cancel(server, "lend");
			admin_refuse(KL_SYSTEM_ADMIN_FAILED, "lend");
			return;
		}
		lent = admin_lend(server, pair[0]);
		if (lent == NULL) {
			(void)close(pair[1]);
			admin_cancel(server, "lend");
			admin_refuse(KL_SYSTEM_ADMIN_FAILED, "lend");
			return;
		}
		connection = pair[1];
		printf("KWL ADMIN lent id=%u client=%llu\n", current->id, (unsigned long long)lent->number);
	}

	/*
	 * The ALLOW; the manager keeps its own copy of the connection, so the
	 * compositor's goes (the lent client ends by itself when no program
	 * comes to the other end).
	 */
	error = kl_backend_session_admin_allow(server->backend, current->id, connection);
	if (connection >= 0)
		(void)close(connection);
	if (error != 0) {
		printf("KWL ADMIN allow-failed id=%u errno=%d\n", current->id, error);
		admin_cancel(server, "allow");
		admin_refuse(KL_SYSTEM_ADMIN_FAILED, "allow");
		return;
	}

	/* Succeeded: STARTED or FAILED is awaited. */
	current->stage = ADMIN_ALLOWED;
	printf("KWL ADMIN allowed id=%u display=%u\n", current->id, current->display);
}

/*
 * Makes a client of the compositor's end of a connection lent to a
 * program run as root.  Returns the client, or NULL (the descriptor is
 * closed then).
 */
static struct kwl_client *
admin_lend(
	struct kwl_server *server,
	int descriptor)
{
	struct kwl_client *client;
	struct kwl_object *display;
	int flags;

	/* The connection is read without waiting, like every other. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags >= 0)
		flags = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (flags < 0) {
		printf("KWL ADMIN lend-failed step=nonblock errno=%d\n", errno);
		(void)close(descriptor);
		return NULL;
	}

	/* The connection's state. */
	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		(void)close(descriptor);
		return NULL;
	}

	/* It is lent to a program run as root: its windows may say so. */
	client->fd = descriptor;
	client->server = server;
	client->number = ++server->client_serial;
	client->connected_ms = kwl_milliseconds();
	client->admin_lent = 1U;
	client->next = server->clients;
	server->clients = client;

	/* The display, the connection's first object. */
	display = kwl_create(client, 1, KWL_DISPLAY, 1);
	if (display == NULL) {
		kwl_client_destroy(client);
		return NULL;
	}

	/* Succeeded: the connection is served from the next pass. */
	printf("KWL CLIENT client=%llu fd=%d admin=1\n", (unsigned long long)client->number, descriptor);
	return client;
}

/*
 * Passes a started program's streams to the client that asked (one stream
 * event each, then started); a request no longer wanted leaves them to the
 * backend, which closes them.
 */
static void
admin_started(
	struct kl_backend_admin_answer *answer)
{
	struct admin_request *current;
	struct kwl_object *object;
	uint32_t payload[2];
	unsigned bit;
	unsigned index;
	int failed;
	int error;

	/* Nobody wants them: the program sees its input end and stops. */
	current = &admin_state.current;
	object = current->object;
	if (object == NULL) {
		printf("KWL ADMIN started id=%u unwanted\n", current->id);
		admin_clear();
		return;
	}

	/* Each stream in the order of the bits; an event that cannot be queued still takes its descriptor. */
	failed = 0;
	index = 0U;
	for (bit = KL_SYSTEM_ADMIN_INPUT; bit <= KL_SYSTEM_ADMIN_STATUS; bit <<= 1) {
		if ((answer->bits & bit) == 0U || index >= answer->count)
			continue;
		payload[0] = current->number;
		payload[1] = bit;
		error = kwl_emit_fd(object->client, object->id, KL_SYSTEM_ADMIN_EVENT_STREAM, payload, sizeof(payload), answer->descriptors[index]);
		answer->descriptors[index] = -1;
		index++;
		if (error != 0)
			failed = error;
	}

	/* started, after the streams. */
	payload[0] = current->number;
	error = kwl_emit(object->client, object->id, KL_SYSTEM_ADMIN_EVENT_STARTED, payload, sizeof(payload[0]));
	if (error != 0)
		failed = error;
	printf("KWL ADMIN started id=%u client=%llu streams=%x errno=%d\n", current->id, (unsigned long long)current->client, answer->bits, failed);

	/* A client that could not take them reads too little: it is ended, as for every event. */
	if (failed != 0)
		kwl_client_fail(object->client);

	/* Succeeded: the request is done. */
	admin_clear();
}

/* Answers the request under way with refused (when it is still wanted) and lets it go. */
static void
admin_refuse(
	uint32_t reason,
	const char *why)
{
	struct admin_request *current;

	/* The client hears it when its object is still there. */
	current = &admin_state.current;
	if (current->object != NULL)
		admin_refuse_object(current->object, current->number, reason, why);
	admin_clear();
}

/* Sends refused for one request of an object. */
static void
admin_refuse_object(
	struct kwl_object *object,
	uint32_t number,
	uint32_t reason,
	const char *why)
{
	uint32_t payload[2];
	int error;

	/* The request's number and why. */
	payload[0] = number;
	payload[1] = reason;
	error = kwl_emit(object->client, object->id, KL_SYSTEM_ADMIN_EVENT_REFUSED, payload, sizeof(payload));
	printf("KWL ADMIN refused client=%llu request=%u reason=%u why=%s errno=%d\n",
	    (unsigned long long)object->client->number, number, reason, why, error);
}

/* Gives a manager's REFUSED reason as the client's (KL_SYSTEM_ADMIN_*). */
static uint32_t
admin_reason(
	unsigned reason)
{
	/* Each of the manager's words. */
	switch (reason) {
	case KL_BACKEND_ADMIN_UNKNOWN:
	case KL_BACKEND_ADMIN_INVALID:
		/* No such action, or words or streams it does not list. */
		return KL_SYSTEM_ADMIN_UNKNOWN;
	case KL_BACKEND_ADMIN_NOT_ALLOWED:
		return KL_SYSTEM_ADMIN_NOT_ALLOWED;
	case KL_BACKEND_ADMIN_METHOD:
		return KL_SYSTEM_ADMIN_METHOD;
	case KL_BACKEND_ADMIN_BUSY:
		return KL_SYSTEM_ADMIN_BUSY;
	case KL_BACKEND_ADMIN_LOCKED:
		return KL_SYSTEM_ADMIN_LOCKED;
	default:
		break;
	}

	/* A word not known here: the action did not run. */
	return KL_SYSTEM_ADMIN_FAILED;
}

/* Tells the manager the request under way is not allowed (ADMIN CANCEL), once it was asked. */
static void
admin_cancel(
	struct kwl_server *server,
	const char *why)
{
	struct admin_request *current;
	int error;

	/* A request not asked yet is nothing to the manager. */
	current = &admin_state.current;
	if (current->id == 0U)
		return;

	/* CANCEL; it has no answer. */
	error = kl_backend_session_admin_cancel(server->backend, current->id);
	printf("KWL ADMIN cancel id=%u why=%s errno=%d\n", current->id, why, error);
}

/*
 * Lets the request under way go without an answer to its client (it was
 * withdrawn, or its object went): one waiting for its window is simply
 * forgotten, a dialog showing is cancelled, and one waiting for the
 * manager's answer is answered to nobody (the answer is cleaned up when
 * it comes).
 */
static void
admin_abandon(
	struct kwl_server *server,
	const char *why)
{
	struct admin_request *current;

	/* Each stage. */
	current = &admin_state.current;
	printf("KWL ADMIN abandon id=%u stage=%u why=%s\n", current->id, current->stage, why);
	switch (current->stage) {
	case ADMIN_FRONT:
		admin_clear();
		break;
	case ADMIN_SHOWN:
		admin_cancel(server, why);
		admin_clear();
		break;
	case ADMIN_ASKED:
	case ADMIN_ALLOWED:
		/* The manager's answer still comes; it goes to nobody. */
		current->object = NULL;
		break;
	default:
		break;
	}
}

/* Ends the request under way: nothing is under way after it. */
static void
admin_clear(
	void)
{
	/* The state of none. */
	memset(&admin_state.current, 0, sizeof(admin_state.current));
	admin_state.current.stage = ADMIN_IDLE;
}

/* Arms the timer for the end of the wait of the request under way (or disarms it). */
static void
admin_wake(
	struct kwl_server *server)
{
	struct admin_request *current;
	uint64_t due;

	/* Only the window's wait and the dialog's have an end here (the manager's is the backend's). */
	current = &admin_state.current;
	due = UINT64_MAX;
	if (current->stage == ADMIN_FRONT || current->stage == ADMIN_SHOWN)
		due = current->due_ms;

	/* The pass at that time. */
	kwl_timer_wake_at(server, &admin_state.timer, "admin", due);
}
