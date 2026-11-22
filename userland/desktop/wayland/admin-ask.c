/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The admin actions in the compositor: kl_system_admin_v1's objects, the
 * relay of their requests to the session manager, and the dialog that
 * asks the person at the screen.
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
 * The dialog is drawn by the compositor alone, over everything it draws
 * (the input method's candidates are not drawn while it shows), with
 * every other display darkened, the compositor's own arrow for the cursor
 * and no direct scanout: what a client draws cannot cover it or pass for
 * it.  It says what the definition says (its title and message), which
 * window asked, and has Allow and Cancel, Cancel chosen first.  While it
 * shows, every input's entry gives the input to it and to no client: the
 * grabs, a drag and the fingers and pens down are let go and the clients
 * lose the keyboard and the pointer when it opens, and get them back when
 * it closes.  It takes no press and no key until ADMIN_ARM_MS after its
 * first frame has opened (a click meant for what was there before is not
 * an Allow), and a press counts only when it starts after that and ends
 * on the same button.  It is cancelled after ADMIN_DIALOG_MS, by Esc, by
 * the lock and by the power button.  A client whose request the person
 * cancelled is answered no without a dialog for ADMIN_AGAIN_MS, unless
 * the person pressed in its window within ADMIN_PRESSED_MS (they asked
 * again).  The plain look, which has no text to draw it with, shows no
 * dialog: its requests are refused as unsupported.
 */

#include "kwl.h"
#include "glass.h"
#include "data.h"
#include "extras.h"
#include "popup.h"
#include "tablet.h"
#include "touch.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <keiland/keiland.h>

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

/*
 * The dialog's darkening to open (milliseconds), how long after its first
 * frame has opened it takes no press or key, how long a client the person
 * said no to is answered no at once, and how recent a press in its window
 * lets it ask again.
 */
#define ADMIN_OPEN_MS		200U
#define ADMIN_ARM_MS		500U
#define ADMIN_AGAIN_MS		10000U
#define ADMIN_PRESSED_MS	2000U

/* How dark the dialog makes every display. */
#define ADMIN_DARK		0.55f

/* The card's size and corners, the buttons' size, gap and corners, the focus ring's width, and the text's inner margin. */
#define ADMIN_CARD_WIDTH	520
#define ADMIN_CARD_HEIGHT	300
#define ADMIN_CARD_RADIUS	24.0f
#define ADMIN_BUTTON_WIDTH	160
#define ADMIN_BUTTON_HEIGHT	40
#define ADMIN_BUTTON_GAP	16
#define ADMIN_BUTTON_RADIUS	14.0f
#define ADMIN_RING		3
#define ADMIN_MARGIN		32

/* The most lines the message wraps into, and the distance between two. */
#define ADMIN_LINES		4U
#define ADMIN_LINE_STEP		22

/* The choices: Allow and Cancel, a place on the card on neither, and a place outside it. */
#define ADMIN_ALLOW		0
#define ADMIN_CANCEL		1
#define ADMIN_NONE		(-1)
#define ADMIN_OUTSIDE		(-2)

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
 * The dialog of the request under way, shown while its stage is
 * ADMIN_SHOWN.
 *
 * open is set from the dialog's opening to its closing, and is what keeps
 * every input from the clients; opened_ms is when it opened (its
 * darkening's clock) and first_ms when its first frame was drawn (0
 * before), from which it is armed.  focus is the keys' choice (Cancel
 * first), pressed the button a press started on after it was armed
 * (ADMIN_NONE for none), and armed_drawn whether a frame showed it armed.
 * who is the window that asked, as the dialog names it.  The message's
 * lines are where its text wraps (bytes from its start and their
 * lengths), found once at the first frame; logged is set once the
 * dialog's places were logged.
 */
struct admin_view {
	unsigned open;
	uint64_t opened_ms;
	uint64_t first_ms;
	int focus;
	int pressed;
	unsigned armed_drawn;
	char who[KWL_TITLE_MAX];
	unsigned wrapped;
	unsigned line_count;
	size_t line_start[ADMIN_LINES];
	size_t line_length[ADMIN_LINES];
	unsigned logged;
};

/*
 * The admin actions' state, the event loop's: the compositor, the
 * request under way (stage ADMIN_IDLE for none) and its dialog, the
 * number the next request to the manager gets (it skips 0, which numbers
 * no request), the timer that wakes the loop when a stage's wait ends,
 * and the client the person last said no to and when (0 for none).
 */
static struct {
	struct kwl_server *server;
	struct admin_request current;
	struct admin_view view;
	uint32_t next_id;
	struct kwl_timer timer;
	uint64_t refused_client;
	uint64_t refused_ms;
} admin_state;

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
static int admin_said_no(const struct kwl_client *client);
static void admin_open(struct kwl_server *server);
static void admin_close(void);
static void admin_who(const struct admin_request *request, char *who, size_t size);
static void admin_answer(struct kwl_server *server, int choice, const char *via);
static int admin_armed(void);
static uint64_t admin_arm_ms(void);
static float admin_progress(void);
static void admin_card(const struct kwl_server *server, int32_t *card);
static void admin_button_rect(const int32_t *card, int choice, int32_t *rect);
static int admin_hit(const struct kwl_server *server, int32_t x, int32_t y);
static void admin_wrap(struct kwl_server *server, const char *text, int32_t limit);
static void admin_draw_button(struct kwl_server *server, struct kwl_list *command, const int32_t *rect, int choice, int lit, float opacity);
static void admin_draw_middle(struct kwl_server *server, struct kwl_list *command, enum glass_size size, int32_t middle, int32_t baseline, const char *text, const float *ink);

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
	int said_no;
	int valid;
	int error;

	/* The object goes; what it asked goes with it (kwl_admin_gone). */
	server = object->client->server;
	admin_state.server = server;
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

	/* A client the person just said no to hears no again, unless the person pressed in its window since. */
	said_no = admin_said_no(object->client);
	if (said_no) {
		admin_refuse_object(object, number, KL_SYSTEM_ADMIN_NO, "said-no");
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

		/* The darkening opens frame by frame, from before the first frame to the end of the opening. */
		if (admin_state.view.first_ms == 0U || now < admin_state.view.opened_ms + ADMIN_OPEN_MS)
			server->dirty = 1;

		/* The frame that shows the dialog armed is drawn when it is. */
		if (!admin_state.view.armed_drawn &&
		    admin_state.view.first_ms != 0U &&
		    now >= admin_arm_ms())
			server->dirty = 1;
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

/* Tells whether the dialog shows: while it does, no client has the keyboard, the pointer or any other input. */
int
kwl_admin_ask_showing(
	void)
{
	/* Open from its opening to its closing. */
	if (admin_state.view.open)
		return 1;
	return 0;
}

/*
 * Takes a button while the dialog shows: a press on a button once the
 * dialog is armed holds it, and its release there takes it; any other
 * button, press or release does nothing.  Returns 1 when the button was
 * the dialog's (every one while it shows and the screen is not locked).
 */
int
kwl_admin_ask_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int choice;
	int armed;
	int hit;

	/* Only while it shows; the lock screen has the buttons once the screen is locked. */
	if (!admin_state.view.open || server->locked)
		return 0;

	/* Only the left button (a finger's tap and a pen's touch are one) acts. */
	if (button != KWL_BUTTON_LEFT)
		return 1;
	hit = admin_hit(server, server->pointer_x, server->pointer_y);

	/* A release on the button its press started on takes it. */
	if (state == 0U) {
		choice = admin_state.view.pressed;
		admin_state.view.pressed = ADMIN_NONE;
		server->dirty = 1;
		if (choice >= 0 && hit == choice)
			admin_answer(server, choice, "press");
		return 1;
	}

	/* A press before the dialog is armed is no press of its. */
	armed = admin_armed();
	if (!armed)
		return 1;

	/* A press on a button holds it. */
	if (hit >= 0) {
		admin_state.view.pressed = hit;
		server->dirty = 1;
	}

	/* Succeeded: the button was the dialog's. */
	return 1;
}

/*
 * Takes a key while the dialog shows: once it is armed, Esc cancels, Tab
 * and the arrows move the keys' choice, Enter and Space take it; a
 * release, a key repeating and a key before it is armed do nothing.
 * Returns 1 when the key was the dialog's (every one while it shows and
 * the screen is not locked).
 */
int
kwl_admin_ask_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	int armed;

	/* Only while it shows; the lock screen has the keys once the screen is locked. */
	if (!admin_state.view.open || server->locked)
		return 0;

	/* Only a key pressed once the dialog is armed acts (one held from before it, or repeating, does not). */
	if (state != 1U)
		return 1;
	armed = admin_armed();
	if (!armed)
		return 1;

	/* Esc cancels. */
	if (key == KEY_ESC) {
		admin_answer(server, ADMIN_CANCEL, "escape");
		return 1;
	}

	/* Enter and Space take the keys' choice. */
	if (key == KEY_ENTER || key == KEY_KPENTER || key == KEY_SPACE) {
		admin_answer(server, admin_state.view.focus, "key");
		return 1;
	}

	/* Tab and the arrows move between the two. */
	if (key == KEY_TAB ||
	    key == KEY_LEFT ||
	    key == KEY_RIGHT ||
	    key == KEY_UP ||
	    key == KEY_DOWN) {
		admin_state.view.focus = ADMIN_CANCEL - admin_state.view.focus;
		server->dirty = 1;
	}

	/* Succeeded: the key was the dialog's. */
	return 1;
}

/*
 * Takes the pointer's motion while the dialog shows (the button under it
 * is drawn lit).  Returns 1 when it was the dialog's.
 */
int
kwl_admin_ask_motion(
	struct kwl_server *server)
{
	/* Only while it shows and the screen is not locked. */
	if (!admin_state.view.open || server->locked)
		return 0;

	/* Succeeded: the next frame lights what is under the pointer. */
	server->dirty = 1;
	return 1;
}

/*
 * Tells whether an input that only a client would hear (a wheel, a touch
 * pad's scroll and gestures, a pen's tablet events, a virtual keyboard's
 * keys) is to go nowhere: the dialog shows and the screen is not locked.
 * Returns 1 or 0.
 */
int
kwl_admin_ask_holds(
	struct kwl_server *server)
{
	/* The dialog's input, not the lock screen's. */
	if (!admin_state.view.open || server->locked)
		return 0;
	return 1;
}

/* Cancels the dialog for the power button: the person's own act comes first, and the power dialog shows after. */
void
kwl_admin_ask_power(
	struct kwl_server *server)
{
	/* Only a dialog that shows. */
	if (!admin_state.view.open)
		return;

	/* The request is cancelled; the client hears no. */
	kwl_admin_decide(server, 0U, "power");
}

/*
 * Draws the dialog over everything the compositor draws on its output but
 * the cursor: the darkened screen and the card with the definition's
 * title and message, the window that asked, and Allow and Cancel, both
 * faint until it is armed and the keys' choice ringed.  Nothing shows on
 * the lock or the login screen, or with the screen off.
 */
void
kwl_admin_ask_draw(
	struct kwl_server *server,
	struct kwl_list *command)
{
	static const float dark_ink[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	static const float soft_ink[4] = { 0.30f, 0.36f, 0.46f, 1.0f };
	const struct admin_request *current;
	const char *name;
	struct glass_shape shape;
	char line[KL_BACKEND_ADMIN_TEXT];
	char asked[KWL_TITLE_MAX + 64U];
	int32_t card[4];
	int32_t rect[4];
	int32_t middle;
	uint64_t now;
	unsigned index;
	float dark[4];
	float ink[4];
	float soft[4];
	float progress;
	float opacity;
	int armed;
	int hover;
	int choice;
	int lit;

	/* Only while it shows, and only over the desktop. */
	if (!admin_state.view.open)
		return;
	if (server->locked || server->greeter || server->screen_off)
		return;
	current = &admin_state.current;

	/* Its first frame starts its clock to be armed. */
	now = kwl_milliseconds();
	if (admin_state.view.first_ms == 0U)
		admin_state.view.first_ms = now;

	/* Drawn in the output's own places, over everything: nothing of a layer, a window's cover or a backdrop. */
	server->layer_on = 0;
	server->cover_count = 0U;
	server->draw_section = KWL_SECTION_OTHER;
	kwl_backdrop_reset(server);

	/* The whole screen darkened, as far as the dialog has opened. */
	progress = admin_progress();
	dark[0] = 0.0f;
	dark[1] = 0.0f;
	dark[2] = 0.0f;
	dark[3] = ADMIN_DARK * progress;
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, dark);

	/* The card's frosted glass in the middle. */
	admin_card(server, card);
	glass_shape_init(&shape, (float)card[0], (float)card[1], (float)card[2], (float)card[3]);
	shape.mode = MODE_GLASS;
	shape.radius = ADMIN_CARD_RADIUS;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.80f;
	shape.edge = 0.70f;
	shape.opacity = progress;
	glass_shape_draw(server, command, &shape);

	/* The message's lines, found once the glyphs can be measured. */
	if (!admin_state.view.wrapped)
		admin_wrap(server, current->message, ADMIN_CARD_WIDTH - 2 * ADMIN_MARGIN);

	/* The inks, fading in with the card. */
	memcpy(ink, dark_ink, sizeof(ink));
	memcpy(soft, soft_ink, sizeof(soft));
	ink[3] = progress;
	soft[3] = progress;
	middle = card[0] + card[2] / 2;

	/* What is asked, then the definition's title. */
	admin_draw_middle(server, command, SIZE_BAR, middle, card[1] + 36, kl_tr("Administrator permission"), soft);
	admin_draw_middle(server, command, SIZE_SIGN, middle, card[1] + 72, current->title, ink);

	/* The message, line by line. */
	for (index = 0U; index < admin_state.view.line_count; index++) {
		memcpy(line, current->message + admin_state.view.line_start[index], admin_state.view.line_length[index]);
		line[admin_state.view.line_length[index]] = '\0';
		admin_draw_middle(server, command, SIZE_BAR, middle, card[1] + 106 + (int32_t)index * ADMIN_LINE_STEP, line, ink);
	}

	/* The window that asked. */
	(void)kl_tr_format(asked, sizeof(asked), kl_tr("Asked by {1}"), admin_state.view.who, (const char *)NULL);
	admin_draw_middle(server, command, SIZE_BAR, middle, card[1] + 106 + (int32_t)ADMIN_LINES * ADMIN_LINE_STEP + 4, asked, soft);

	/* The buttons, faint until armed: the one under the pointer or pressed lit, the keys' choice ringed. */
	armed = admin_armed();
	opacity = progress;
	if (!armed)
		opacity = progress * 0.45f;
	hover = admin_hit(server, server->pointer_x, server->pointer_y);
	for (choice = ADMIN_ALLOW; choice <= ADMIN_CANCEL; choice++) {
		lit = 0;
		if (armed &&
		    (choice == hover ||
		     choice == admin_state.view.pressed))
			lit = 1;
		admin_button_rect(card, choice, rect);
		if (choice == admin_state.view.focus) {
			glass_draw_solid(
				server,
				command,
				(float)(rect[0] - ADMIN_RING),
				(float)(rect[1] - ADMIN_RING),
				(float)(rect[2] + 2 * ADMIN_RING),
				(float)(rect[3] + 2 * ADMIN_RING),
				ADMIN_BUTTON_RADIUS + (float)ADMIN_RING,
				ink);
		}
		admin_draw_button(server, command, rect, choice, lit, opacity);
	}

	/* A frame that shows it armed was drawn. */
	if (armed)
		admin_state.view.armed_drawn = 1U;

	/* The places, once, for the tests that press them. */
	if (!admin_state.view.logged) {
		admin_state.view.logged = 1U;
		printf("KWL ADMIN card x=%d y=%d width=%d height=%d\n", card[0], card[1], card[2], card[3]);
		for (choice = ADMIN_ALLOW; choice <= ADMIN_CANCEL; choice++) {
			admin_button_rect(card, choice, rect);
			name = "cancel";
			if (choice == ADMIN_ALLOW)
				name = "allow";
			printf("KWL ADMIN button=%s x=%d y=%d width=%d height=%d\n", name, rect[0], rect[1], rect[2], rect[3]);
		}
	}
}

/*
 * Darkens a head of the extended desktop while the dialog shows (heads.c
 * draws each head in its own pass): the dialog itself is on the output,
 * and no display shows a client's picture undimmed meanwhile.
 */
void
kwl_admin_ask_draw_head(
	struct kwl_server *server,
	struct kwl_list *command)
{
	float dark[4];

	/* Only while it shows over the desktop, in a head's pass. */
	if (!admin_state.view.open)
		return;
	if (server->locked || server->greeter || server->screen_off)
		return;
	if (server->view_width == 0U)
		return;

	/* The head's whole part of the plane, as dark as the output. */
	dark[0] = 0.0f;
	dark[1] = 0.0f;
	dark[2] = 0.0f;
	dark[3] = ADMIN_DARK * admin_progress();
	glass_draw_solid(server, command, (float)server->view_x, (float)server->view_y, (float)server->view_width, (float)server->view_height, 0.0f, dark);
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
	int showing;
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

	/* The plain look has no text to draw the dialog with: no dialog there. */
	if (!server->glass) {
		admin_cancel(server, "unsupported");
		admin_refuse(KL_SYSTEM_ADMIN_UNSUPPORTED, "no-dialog");
		return;
	}

	/* Another dialog that came meanwhile (the power button's, a pairing's) keeps the screen. */
	showing = kwl_bluetooth_ask_showing();
	if (!showing)
		showing = kwl_power_dialog_showing(server);
	if (showing) {
		admin_cancel(server, "busy");
		admin_refuse(KL_SYSTEM_ADMIN_BUSY, "busy-dialog");
		return;
	}

	/* What the dialog says, and whether its allow lends a connection. */
	current->how = answer->how;
	current->display = answer->display;
	(void)snprintf(current->title, sizeof(current->title), "%s", answer->title);
	(void)snprintf(current->message, sizeof(current->message), "%s", answer->message);

	/* The dialog shows: its time runs, it takes the input, and the client hears asking. */
	current->stage = ADMIN_SHOWN;
	current->due_ms = kwl_milliseconds() + ADMIN_DIALOG_MS;
	admin_open(server);
	printf("KWL ADMIN show id=%u how=consent display=%u who=%s\n", current->id, current->display, admin_state.view.who);
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

	/* Succeeded: the dialog, if one showed, goes, and STARTED or FAILED is awaited. */
	admin_close();
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
	printf("KWL ADMIN refused client=%llu request=%u reason=%u why=%s errno=%d\n", (unsigned long long)object->client->number, number, reason, why, error);
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
	/* The dialog, if one shows, goes with the request; then the state of none. */
	admin_close();
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
	uint64_t arm;

	/* Only the window's wait and the dialog's have an end here (the manager's is the backend's). */
	current = &admin_state.current;
	due = UINT64_MAX;
	if (current->stage == ADMIN_FRONT || current->stage == ADMIN_SHOWN)
		due = current->due_ms;

	/* A dialog drawn but not yet armed wakes the loop to draw it armed. */
	if (current->stage == ADMIN_SHOWN &&
	    admin_state.view.first_ms != 0U &&
	    !admin_state.view.armed_drawn) {
		arm = admin_arm_ms();
		if (arm < due)
			due = arm;
	}

	/* The pass at that time. */
	kwl_timer_wake_at(server, &admin_state.timer, "admin", due);
}

/*
 * Tells whether a client is to hear no at once: the person said no to its
 * request less than ADMIN_AGAIN_MS ago, and has not pressed in its window
 * in the last ADMIN_PRESSED_MS.  Returns 1 or 0.
 */
static int
admin_said_no(
	const struct kwl_client *client)
{
	uint64_t now;

	/* Only the client the person said no to. */
	if (admin_state.refused_client == 0U || admin_state.refused_client != client->number)
		return 0;

	/* Only for a while after. */
	now = kwl_milliseconds();
	if (now - admin_state.refused_ms >= ADMIN_AGAIN_MS)
		return 0;

	/* A press of the person's in its window just now asked again. */
	if (client->pressed_ms != 0U && now - client->pressed_ms < ADMIN_PRESSED_MS)
		return 0;

	/* Succeeded: no, at once. */
	return 1;
}

/*
 * Opens the dialog of the request under way: what holds the input lets go
 * of it (while the clients still hear the releases), then the clients lose
 * the keyboard and the pointer to the dialog.
 */
static void
admin_open(
	struct kwl_server *server)
{
	struct admin_view *view;

	/* A popup's grab, a drag and drop, a window's move or resize, the fingers and the pens let go. */
	kwl_popup_dismiss_grab(server);
	if (server->dnd_active)
		kwl_data_drag_cancel(server);
	kwl_seat_grabs_end(server);
	kwl_touch_cancel(server, "admin");
	kwl_tablet_cancel(server);

	/* The dialog from now, Cancel chosen, naming the window that asked. */
	view = &admin_state.view;
	memset(view, 0, sizeof(*view));
	view->open = 1U;
	view->opened_ms = kwl_milliseconds();
	view->focus = ADMIN_CANCEL;
	view->pressed = ADMIN_NONE;
	admin_who(&admin_state.current, view->who, sizeof(view->who));

	/* No client has the keyboard or the pointer while it shows (seat.c), and the cursor is the compositor's arrow. */
	kwl_seat_focus(server);
	kwl_seat_pointer_update(server);
	kwl_cursor_frame(server, 0U);
	server->dirty = 1;
}

/* Closes the dialog, if it shows: the window on top gets the keyboard back, and the pointer comes back with its next motion. */
static void
admin_close(
	void)
{
	struct kwl_server *server;

	/* Only a dialog that shows. */
	if (!admin_state.view.open)
		return;

	/* The dialog goes. */
	memset(&admin_state.view, 0, sizeof(admin_state.view));
	admin_state.view.pressed = ADMIN_NONE;
	printf("KWL ADMIN closed id=%u\n", admin_state.current.id);

	/* The keyboard's focus as it would be without it. */
	server = admin_state.server;
	kwl_seat_focus(server);
	server->dirty = 1;
}

/* Writes the name the dialog gives the window that asked: its title, or else its application's ID. */
static void
admin_who(
	const struct admin_request *request,
	char *who,
	size_t size)
{
	struct kwl_object *surface;

	/* Nothing known. */
	(void)snprintf(who, size, "?");
	if (request->object == NULL)
		return;

	/* The surface it named. */
	surface = kwl_find(request->object->client, request->surface);
	if (surface == NULL || surface->kind != KWL_SURFACE || surface->dead)
		return;

	/* Its title, else its application's ID. */
	if (surface->title[0] != '\0') {
		(void)snprintf(who, size, "%s", surface->title);
	} else if (surface->app_id[0] != '\0') {
		(void)snprintf(who, size, "%s", surface->app_id);
	}
}

/*
 * Answers the dialog by the person's choice: Allow sends the ALLOW;
 * Cancel says no, and the client is answered no at once for a while.
 */
static void
admin_answer(
	struct kwl_server *server,
	int choice,
	const char *via)
{
	/* Allowed. */
	if (choice == ADMIN_ALLOW) {
		kwl_admin_decide(server, 1U, via);
		return;
	}

	/* No: remembered for the client's next request. */
	admin_state.refused_client = admin_state.current.client;
	admin_state.refused_ms = kwl_milliseconds();
	kwl_admin_decide(server, 0U, via);
}

/* Tells whether the dialog takes presses and keys: ADMIN_ARM_MS after its first frame has opened.  Returns 1 or 0. */
static int
admin_armed(
	void)
{
	uint64_t now;

	/* Not before the time. */
	now = kwl_milliseconds();
	if (now < admin_arm_ms())
		return 0;
	return 1;
}

/* Gives when the dialog is armed: ADMIN_ARM_MS after the end of its first frame's opening, never before that frame. */
static uint64_t
admin_arm_ms(
	void)
{
	/* No frame yet. */
	if (admin_state.view.first_ms == 0U)
		return UINT64_MAX;

	/* From the first frame, the opening and the wait. */
	return admin_state.view.first_ms + ADMIN_OPEN_MS + ADMIN_ARM_MS;
}

/* Tells how far the dialog's darkening has opened (0 to 1). */
static float
admin_progress(
	void)
{
	uint64_t elapsed;
	float t;

	/* The time since it opened, over the opening's. */
	elapsed = kwl_milliseconds() - admin_state.view.opened_ms;
	t = (float)elapsed / (float)ADMIN_OPEN_MS;
	if (t > 1.0f)
		t = 1.0f;
	return t;
}

/* Gives the card's rectangle (x, y, width, height), in the middle of the output. */
static void
admin_card(
	const struct kwl_server *server,
	int32_t *card)
{
	/* In the middle. */
	card[0] = ((int32_t)server->width - ADMIN_CARD_WIDTH) / 2;
	card[1] = ((int32_t)server->height - ADMIN_CARD_HEIGHT) / 2;
	card[2] = ADMIN_CARD_WIDTH;
	card[3] = ADMIN_CARD_HEIGHT;
}

/* Gives a button's rectangle: Allow at the left and Cancel at the right, under the words. */
static void
admin_button_rect(
	const int32_t *card,
	int choice,
	int32_t *rect)
{
	int32_t middle;

	/* At the bottom of the card. */
	middle = card[0] + card[2] / 2;
	rect[1] = card[1] + card[3] - 24 - ADMIN_BUTTON_HEIGHT;
	rect[2] = ADMIN_BUTTON_WIDTH;
	rect[3] = ADMIN_BUTTON_HEIGHT;

	/* The two side by side. */
	rect[0] = middle - ADMIN_BUTTON_GAP / 2 - ADMIN_BUTTON_WIDTH;
	if (choice == ADMIN_CANCEL)
		rect[0] = middle + ADMIN_BUTTON_GAP / 2;
}

/* Tells which button a point is on (ADMIN_ALLOW, ADMIN_CANCEL), or ADMIN_NONE on the card, or ADMIN_OUTSIDE. */
static int
admin_hit(
	const struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	int32_t card[4];
	int32_t rect[4];
	int choice;

	/* Each button. */
	admin_card(server, card);
	for (choice = ADMIN_ALLOW; choice <= ADMIN_CANCEL; choice++) {
		admin_button_rect(card, choice, rect);
		if (x >= rect[0] && x < rect[0] + rect[2] && y >= rect[1] && y < rect[1] + rect[3])
			return choice;
	}

	/* The card, or outside it. */
	if (x >= card[0] && x < card[0] + card[2] && y >= card[1] && y < card[1] + card[3])
		return ADMIN_NONE;
	return ADMIN_OUTSIDE;
}

/*
 * Finds where the message wraps to fit limit pixels: at its last space
 * that fits, else at the last character that fits (a text without spaces,
 * such as Japanese), at most ADMIN_LINES lines (the last one takes the
 * rest and is cut where it is drawn).
 */
static void
admin_wrap(
	struct kwl_server *server,
	const char *text,
	int32_t limit)
{
	struct admin_view *view;
	char piece[KL_BACKEND_ADMIN_TEXT];
	size_t length;
	size_t start;
	size_t end;
	size_t next;
	size_t space;
	int32_t width;

	/* No line yet. */
	view = &admin_state.view;
	view->wrapped = 1U;
	view->line_count = 0U;
	length = strnlen(text, sizeof(piece) - 1U);

	/* Each line from where the last one ended. */
	start = 0U;
	while (start < length && view->line_count < ADMIN_LINES) {
		/* A line starts after the spaces that ended the last. */
		while (start < length && text[start] == ' ')
			start++;
		if (start >= length)
			break;

		/* The last line takes the rest. */
		if (view->line_count == ADMIN_LINES - 1U) {
			view->line_start[view->line_count] = start;
			view->line_length[view->line_count] = length - start;
			view->line_count++;
			break;
		}

		/* The line grows a character at a time while it fits, noting its last space. */
		end = start;
		space = 0U;
		while (end < length) {
			next = end + 1U;
			while (next < length && ((unsigned char)text[next] & 0xC0U) == 0x80U)
				next++;
			memcpy(piece, text + start, next - start);
			piece[next - start] = '\0';
			width = glass_text_width(server, SIZE_BAR, piece);
			if (width > limit)
				break;
			if (text[end] == ' ')
				space = end;
			end = next;
		}

		/* Cut inside the text: at its last space when there is one. */
		if (end < length && space > start)
			end = space;

		/* A character wider than the line still takes a line of its own. */
		if (end == start) {
			end = start + 1U;
			while (end < length && ((unsigned char)text[end] & 0xC0U) == 0x80U)
				end++;
		}

		/* The line. */
		view->line_start[view->line_count] = start;
		view->line_length[view->line_count] = end - start;
		view->line_count++;
		start = end;
	}
}

/* Draws a button: Allow in the accent with its ink, Cancel white glass with dark words; lit a little brighter. */
static void
admin_draw_button(
	struct kwl_server *server,
	struct kwl_list *command,
	const int32_t *rect,
	int choice,
	int lit,
	float opacity)
{
	float fill[4];
	float ink[4];
	const char *text;
	int32_t width;
	unsigned kept;

	/* Allow: the accent the user chose, as it is; Cancel: white with dark words. */
	kept = server->keep_colours;
	if (choice == ADMIN_ALLOW) {
		kwl_accent_colour(server, 0, KWL_ACCENT_FILL, 0.92f * opacity, fill);
		kwl_accent_colour(server, 0, KWL_ACCENT_INK, opacity, ink);
		kept = kwl_accent_as_is(server);
		text = kl_tr("Allow");
	} else {
		fill[0] = 1.0f;
		fill[1] = 1.0f;
		fill[2] = 1.0f;
		fill[3] = 0.85f * opacity;
		ink[0] = 0.10f;
		ink[1] = 0.14f;
		ink[2] = 0.22f;
		ink[3] = opacity;
		text = kl_tr("Cancel");
	}

	/* Lit: more opaque. */
	if (lit)
		fill[3] = fill[3] + (opacity - fill[3]) * 0.45f;

	/* The button and its words in its middle. */
	glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], ADMIN_BUTTON_RADIUS, fill);
	width = glass_text_width(server, SIZE_SEARCH, text);
	glass_draw_text(server, command, SIZE_SEARCH, rect[0] + (rect[2] - width) / 2, rect[1] + rect[3] / 2 + 7, text, rect[2] - 24, ink);
	kwl_accent_done(server, kept);
}

/* Draws a line of text centred on middle, cut to the card's inside. */
static void
admin_draw_middle(
	struct kwl_server *server,
	struct kwl_list *command,
	enum glass_size size,
	int32_t middle,
	int32_t baseline,
	const char *text,
	const float *ink)
{
	int32_t width;
	int32_t limit;

	/* Its width, no wider than the card's inside. */
	limit = ADMIN_CARD_WIDTH - 2 * ADMIN_MARGIN;
	width = glass_text_width(server, size, text);
	if (width > limit)
		width = limit;
	glass_draw_text(server, command, size, middle - width / 2, baseline, text, limit, ink);
}
