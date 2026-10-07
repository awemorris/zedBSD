/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's input method (ws095-p004, plan/ws095/design.md sections 2
 * to 4): the compositor starts it, serves its protocols, and decides where each
 * key goes.
 *
 * The input method is one program, /usr/libexec/keiland-ime.  The compositor
 * starts it itself on a socket pair (the WAYLAND_SOCKET way), so its
 * connection is known without trusting a process ID, and only that
 * connection sees and binds the input method's globals: the input method
 * manager (input-method-unstable-v2), the virtual keyboard manager
 * (virtual-keyboard-unstable-v1) and the compositor's own status
 * (kl_ime_status_v1).  When it dies it is started again after a
 * second, three times a minute at most.
 *
 * Keys: Alt+Space asks the input method for its next language (and the
 * Japanese keyboard's 変換, 無変換, カタカナ/ひらがな, かな and 英数
 * choose one); the compositor takes these before anything else.  While a text
 * input is served, the language is not direct input and the input method
 * answers, a key goes to its keyboard grab instead of the application:
 * right after the compositor's own shortcuts, or before the window's menu and tab
 * keys while text is being composed.  The keys the input method does not
 * use come back on its virtual keyboard and go to the application.  A
 * release goes where its press went.  An input method that leaves a key
 * unanswered for half a second is passed by until it answers again, so a
 * hung input method cannot take the keyboard with it.  The modifiers the
 * application hears are always the keyboard's own; the virtual keyboard's
 * modifiers and keymap are taken and not used.
 *
 * The language belongs to the application with the keyboard (ws095-p016):
 * The compositor remembers each application's (by its windows' application ID,
 * else its connection) and the desktop's, chooses an application's again
 * when the keyboard comes back to it, and starts an application seen for
 * the first time with the desktop's.  A window, field or caret that moves
 * within one application changes nothing.
 */

#include "ime.h"
#include "compose.h"
#include "glass.h"
#include "extras.h"
#include "popup.h"
#include "keymap.h"
#include "menu.h"
#include "titlebar.h"
#include "desktop.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* The input method's program. */
#define IME_PROGRAM			KEILAND_LIBEXECDIR "/keiland-ime"

/* How long to wait before starting it again, and how often it may start in a window of time. */
#define IME_RESTART_MS			1000U
#define IME_START_WINDOW_MS		60000U

/* How long a key may go unanswered before the input method is passed by, and an answer noted as slow. */
#define IME_ANSWER_MS			500U
#define IME_SLOW_MS			100U

/* The requests of zwp_input_method_manager_v2. */
#define MANAGER_GET_INPUT_METHOD	0U
#define MANAGER_DESTROY			1U

/* The requests and events of zwp_input_method_v2. */
#define METHOD_COMMIT_STRING		0U
#define METHOD_SET_PREEDIT_STRING	1U
#define METHOD_DELETE_SURROUNDING_TEXT	2U
#define METHOD_COMMIT			3U
#define METHOD_GET_INPUT_POPUP_SURFACE	4U
#define METHOD_GRAB_KEYBOARD		5U
#define METHOD_DESTROY			6U
#define METHOD_ACTIVATE			0U
#define METHOD_DEACTIVATE		1U
#define METHOD_SURROUNDING_TEXT		2U
#define METHOD_TEXT_CHANGE_CAUSE	3U
#define METHOD_CONTENT_TYPE		4U
#define METHOD_DONE			5U
#define METHOD_UNAVAILABLE		6U

/* The request and event of zwp_input_popup_surface_v2. */
#define POPUP_DESTROY			0U
#define POPUP_TEXT_INPUT_RECTANGLE	0U

/* The request and events of zwp_input_method_keyboard_grab_v2. */
#define GRAB_RELEASE			0U
#define GRAB_KEYMAP			0U
#define GRAB_KEY			1U
#define GRAB_MODIFIERS			2U
#define GRAB_REPEAT_INFO		3U

/* The requests of zwp_virtual_keyboard_manager_v1 and zwp_virtual_keyboard_v1, and its error. */
#define KEYBOARDS_CREATE		0U
#define KEYBOARD_KEYMAP			0U
#define KEYBOARD_KEY			1U
#define KEYBOARD_MODIFIERS		2U
#define KEYBOARD_DESTROY		3U
#define KEYBOARD_ERROR_NO_KEYMAP	0U

/* The requests and events of kl_ime_status_manager_v1 and kl_ime_status_v1. */
#define STATUSES_DESTROY		0U
#define STATUSES_GET_STATUS		1U
#define STATUS_DESTROY			0U
#define STATUS_LANGUAGE			1U
#define STATUS_COMPOSING		2U
#define STATUS_PREDICTIONS		3U
#define STATUS_NEXT			0U
#define STATUS_SELECT			1U
#define STATUS_PREDICT			2U
#define STATUS_LEARN			3U

/* The version of kl_ime_status_v1 with the on-screen keyboard's predictions (ws166-p002). */
#define STATUS_VERSION_PREDICT		2U

/* The longest reading or word sent with predict and learn, with its NUL (a candidate is at most 160 bytes in the input method). */
#define IME_PREDICT_TEXT_MAX		256U

/* zwp_input_method_v2's error for a popup surface that has another role. */
#define METHOD_ERROR_ROLE		0U

/* The candidate window's gap below the cursor, and its shadow in the glass look. */
#define IME_POPUP_GAP			4
#define IME_POPUP_SHADOW		10.0f
#define IME_POPUP_RADIUS		10.0f

/* The indicator in the system bar: its width, the gap after it, its height, top and text's baseline, and how far past it a click still counts. */
#define IME_INDICATOR_WIDTH		26
#define IME_INDICATOR_GAP		12
#define IME_INDICATOR_HEIGHT		26
#define IME_INDICATOR_TOP		(KWL_GLASS_BAR_MIDDLE - IME_INDICATOR_HEIGHT / 2)
#define IME_INDICATOR_BASELINE		(KWL_GLASS_BAR_MIDDLE + 5)
#define IME_INDICATOR_SLOP		4

/* The keymap formats of wl_keyboard. */
#define IME_KEYMAP_NO_KEYMAP		0U
#define IME_KEYMAP_XKB_V1		1U

/* The evdev codes of the keys that change the language. */
#define IME_KEY_SPACE			57U
#define IME_KEY_HENKAN			92U
#define IME_KEY_KATAKANAHIRAGANA	93U
#define IME_KEY_MUHENKAN		94U
#define IME_KEY_HANGEUL			122U
#define IME_KEY_HANJA			123U

/* The modifier bits of the seat (input.c). */
#define IME_MODIFIER_CONTROL		0x04U
#define IME_MODIFIER_ALT		0x08U
#define IME_MODIFIER_META		0x40U

/*
 * The text input that stands for the compositor's own text field with the
 * keyboard (a title bar's search or path field, titlebar-shell.c;
 * BUG-177).  It has no object; its surface is the window and its state is
 * the field's, read again (into ime_field_text) each time the input method
 * is to be told it.  The input method serves it as any application's, and
 * what it makes goes to the field instead of a client.  It lives as long as
 * the compositor; its zero value is "no field".
 */
static struct kwl_text_input ime_field;
static char ime_field_text[KWL_TITLEBAR_TEXT_MAX + 1U];

/*
 * Which of the compositor's own fields ime_field stands for: a title bar's
 * (its window's surface), or App Home's search (ws090-p022: no surface,
 * its rectangle the output's).
 */
#define IME_FIELD_TITLEBAR		1U
#define IME_FIELD_HOME			2U
static unsigned ime_field_kind;

static void ime_spawn(struct kwl_server *server, uint64_t now);
static struct kwl_client *ime_connect(struct kwl_server *server, int descriptor);
static void ime_lost(struct kwl_server *server);
static int ime_manager_request(struct kwl_object *manager, uint32_t opcode, const unsigned char *bytes, size_t size);
static int ime_method_request(struct kwl_object *method, uint32_t opcode, const unsigned char *bytes, size_t size);
static int ime_keyboard_request(struct kwl_object *keyboard, uint32_t opcode, const unsigned char *bytes, size_t size);
static int ime_status_request(struct kwl_object *status, uint32_t opcode, const unsigned char *bytes, size_t size);
static int ime_grab_keyboard(struct kwl_object *method, uint32_t id);
static int ime_popup(struct kwl_object *method, uint32_t id, uint32_t surface_id);
static int ime_popup_place(struct kwl_server *server, struct kwl_object *surface, int32_t *x, int32_t *y);
static void ime_popup_draw_one(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface);
static void ime_apply(struct kwl_server *server);
static void ime_activate(struct kwl_server *server, struct kwl_text_input *input);
static void ime_state(struct kwl_server *server, struct kwl_text_input *input);
static void ime_deactivate(struct kwl_server *server);
static void ime_rectangles(struct kwl_server *server);
static void ime_keyboard_key(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
static void ime_release_keyboard(struct kwl_server *server);
static void ime_send_key(struct kwl_server *server, uint32_t time, uint32_t key, uint32_t state);
static void ime_select(struct kwl_server *server, const char *id);
static int ime_is_direct(const struct kwl_ime *ime);
static void ime_emit(struct kwl_object *object, uint32_t opcode, const void *payload, size_t size);
static size_t ime_put_string(unsigned char *payload, size_t offset, const char *text);
static int ime_read_string(const unsigned char *bytes, size_t size, size_t offset, char **text, size_t *next);
static uint32_t ime_word(const unsigned char *bytes, size_t offset);
static void ime_set_text(char **field, const char *text);
static void ime_app_key(struct kwl_server *server, char *key, size_t size);
static struct kwl_ime_app *ime_app_find(struct kwl_ime *ime, const char *key);
static struct kwl_ime_app *ime_app_add(struct kwl_ime *ime, const char *key);
static void ime_app_remember(struct kwl_ime *ime);
static void ime_app_focus(struct kwl_server *server);
static int ime_client_has_app(const struct kwl_client *client, const char *app_id);
static void ime_app_forget(struct kwl_server *server, struct kwl_client *client);
static struct kwl_text_input *ime_current(struct kwl_server *server);
static void ime_deliver(struct kwl_server *server, struct kwl_text_input *input, const char *preedit, int32_t begin, int32_t end, const char *commit, uint32_t before, uint32_t after);

/*
 * Starts the system's input method, when its program is installed (not on
 * the login screen).
 */
void
kwl_ime_start(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	int installed;

	/* The login screen has no applications to type into. */
	if (server->greeter)
		return;

	/* Without the program there is no input method, and keys go to the applications. */
	installed = access(IME_PROGRAM, X_OK);
	if (installed != 0) {
		printf("KWL IME none program=%s errno=%d\n", IME_PROGRAM, errno);
		return;
	}

	/* The input method's state, for the compositor's lifetime. */
	ime = calloc(1, sizeof(*ime));
	if (ime == NULL) {
		printf("KWL IME none errno=%d\n", ENOMEM);
		return;
	}

	strcpy(ime->language, "direct");
	server->ime = ime;

	/* Starts the program. */
	ime_spawn(server, kwl_milliseconds());
}

/*
 * Looks after the input method once a pass: collects the process of one
 * that died, starts it again when due, passes by one that does not answer,
 * and follows the lock screen and App Home.
 */
void
kwl_ime_tick(
	struct kwl_server *server,
	uint64_t now)
{
	struct kwl_ime *ime;
	pid_t reaped;
	int status;
	uint64_t oldest;

	ime = server->ime;
	if (ime == NULL)
		return;

	/* A process that has ended is collected. */
	if (ime->pid > 0) {
		reaped = waitpid(ime->pid, &status, WNOHANG);
		if (reaped == ime->pid || (reaped < 0 && errno == ECHILD)) {
			printf("KWL IME exited pid=%ld\n", (long)ime->pid);
			ime->pid = 0;
		}
	}

	/* An input method that has gone is started again, unless it keeps dying (a change of method is no death). */
	if (ime->client == NULL && !ime->given_up && ime->restart_ms != 0U && now >= ime->restart_ms) {
		oldest = ime->starts[ime->start_index];
		if (!ime->replacing && oldest != 0U && now - oldest < IME_START_WINDOW_MS) {
			ime->given_up = 1;
			printf("KWL IME given-up starts=3 window_ms=%u\n", IME_START_WINDOW_MS);
		} else {
			ime_spawn(server, now);
		}
	}

	/* A key left unanswered too long: the input method is passed by until it answers. */
	if (ime->watching && now - ime->watch_ms >= IME_ANSWER_MS) {
		ime->watching = 0;
		ime->bypass = 1;
		printf("KWL IME bypass after_ms=%u\n", IME_ANSWER_MS);
	}

	/* The lock screen and App Home take the keys; the input method follows them. */
	kwl_ime_update(server, NULL);
}

/*
 * Carries out a request of one of the input method's interfaces.
 */
int
kwl_ime_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_ime *ime;
	uint64_t waited;
	int error;

	/* Any request of the input method is its answer: the watch ends, and a passed-by input method is heard again. */
	server = object->client->server;
	ime = server->ime;
	if (ime != NULL && object->client->ime) {
		/* A slow answer is noted, to tell a busy machine from a hung input method. */
		if (ime->watching) {
			waited = kwl_milliseconds() - ime->watch_ms;
			if (waited >= IME_SLOW_MS)
				printf("KWL IME slow-answer ms=%llu\n", (unsigned long long)waited);
		}

		ime->watching = 0;
		if (ime->bypass) {
			ime->bypass = 0;
			printf("KWL IME answering\n");
		}
	}

	/* Each interface has its own requests. */
	error = EPROTO;
	switch (object->kind) {
	case KWL_INPUT_METHOD_MANAGER:
	case KWL_VIRTUAL_KEYBOARD_MANAGER:
	case KWL_IME_STATUS_MANAGER:
		error = ime_manager_request(object, opcode, bytes, size);
		break;
	case KWL_INPUT_METHOD:
		error = ime_method_request(object, opcode, bytes, size);
		break;
	case KWL_INPUT_POPUP:
		/* A popup's only request is destroy. */
		if (opcode == POPUP_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			error = 0;
		}

		break;
	case KWL_KEYBOARD_GRAB:
		/* A grab's only request is release. */
		if (opcode == GRAB_RELEASE && size == 0U) {
			kwl_object_destroy(object);
			error = 0;
		}

		break;
	case KWL_VIRTUAL_KEYBOARD:
		error = ime_keyboard_request(object, opcode, bytes, size);
		break;
	case KWL_IME_STATUS:
		error = ime_status_request(object, opcode, bytes, size);
		break;
	default:
		break;
	}

	/* A malformed request fails the connection. */
	if (error != 0)
		return error;

	/* Succeeded: the request is carried out. */
	return 0;
}

/*
 * Forgets one of the input method's objects that goes.
 */
void
kwl_ime_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;
	struct kwl_ime *ime;
	unsigned i;

	server = object->client->server;
	ime = server->ime;

	/* A text input's record goes with it (text-input.c). */
	if (object->kind == KWL_TEXT_INPUT) {
		kwl_text_input_object_gone(object);
		return;
	}

	/* Nothing else is known without an input method. */
	if (ime == NULL)
		return;

	/* The input method's objects stop being named. */
	switch (object->kind) {
	case KWL_INPUT_METHOD:
		if (ime->method == object) {
			ime->method = NULL;
			ime->activated = 0;
			ime->active = NULL;
		}

		break;
	case KWL_KEYBOARD_GRAB:
		/* The keys held for the grab are forgotten; their releases go nowhere. */
		if (ime->grab == object) {
			ime->grab = NULL;
			for (i = 0; i < KWL_IME_KEYS; i++) {
				if (ime->route[i] == KWL_IME_ROUTE_GRAB)
					ime->route[i] = KWL_IME_ROUTE_NONE;
			}
		}

		break;
	case KWL_VIRTUAL_KEYBOARD:
		/* The keys it holds down are let go at the application. */
		if (ime->keyboard == object) {
			ime_release_keyboard(server);
			ime->keyboard = NULL;
			ime->keyboard_keymap = 0;
		}

		break;
	case KWL_IME_STATUS:
		if (ime->status == object)
			ime->status = NULL;
		break;
	case KWL_INPUT_POPUP:
		for (i = 0; i < sizeof(ime->popups) / sizeof(ime->popups[0]); i++) {
			if (ime->popups[i] == object)
				ime->popups[i] = NULL;
		}

		break;
	default:
		break;
	}
}

/*
 * Notes that a connection goes: when it is the input method's, the keys it
 * holds are let go, the application loses the preedit it was shown, and
 * the input method is started again later.
 */
void
kwl_ime_client_gone(
	struct kwl_client *client)
{
	struct kwl_server *server;

	/* An application's connection that ends takes its language with it when it was its last (ws095-p016). */
	server = client->server;
	if (server->ime == NULL)
		return;
	ime_app_forget(server, client);

	/* Only the input method's connection matters here. */
	if (server->ime->client != client)
		return;

	/* Everything the input method held goes. */
	ime_lost(server);
}

/*
 * Tells whether a global is shown to a connection: the input method's
 * globals only to the input method the compositor started.
 */
int
kwl_ime_global_visible(
	struct kwl_client *client,
	enum kwl_kind kind)
{
	/* The input method's globals are the input method's alone. */
	switch (kind) {
	case KWL_INPUT_METHOD_MANAGER:
	case KWL_VIRTUAL_KEYBOARD_MANAGER:
	case KWL_IME_STATUS_MANAGER:
		if (client->ime)
			return 1;
		return 0;
	default:
		break;
	}

	/* Every other global is everyone's. */
	return 1;
}

/*
 * Tells the input method's keyboard grab the keyboards' repeat again,
 * after the settings changed it (WS135), so that the keys it repeats
 * itself follow at once.
 */
void
kwl_ime_repeat_changed(
	struct kwl_server *server)
{
	int32_t repeat[2];

	/* No input method, or no grab yet: the grab hears it when it is made. */
	if (server->ime == NULL || server->ime->grab == NULL)
		return;

	/* Sends the grab the new rate and delay. */
	repeat[0] = server->repeat_rate;
	repeat[1] = server->repeat_delay_ms;
	ime_emit(server->ime->grab, GRAB_REPEAT_INFO, repeat, sizeof(repeat));
}

/*
 * Starts the input method again with the method the Languages page chose
 * (ime.method, WS154): the running program is asked to end (it saves what
 * its languages learned on the way out) and the start that follows uses
 * the new choice, without the wait or the count of a restart after a
 * death.  An input method that had given up is started again too.
 */
void
kwl_ime_method_changed(
	struct kwl_server *server)
{
	struct kwl_ime *ime;

	/* No input method, or it runs the method chosen already. */
	ime = server->ime;
	if (ime == NULL || (ime->client != NULL && ime->method_started == server->ime_method))
		return;

	/* The next start is the change's. */
	printf("KWL IME method=%d\n", server->ime_method);
	ime->replacing = 1;
	ime->given_up = 0;

	/* The running program ends; its connection's end starts the new one (ime_lost). */
	if (ime->pid > 0 && ime->client != NULL) {
		(void)kill(ime->pid, SIGTERM);
		return;
	}

	/* None runs: started at the next pass. */
	ime->restart_ms = kwl_milliseconds();
}

/*
 * Takes the keys that belong to the input method before the compositor's own:
 * the release of a key whose press went to the input method or was taken
 * here, and the keys that change the language.
 *
 * Returns nonzero when the key is taken.
 */
int
kwl_ime_key_early(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct kwl_ime *ime;
	uint32_t modifiers;
	int direct;

	ime = server->ime;
	if (ime == NULL || key >= KWL_IME_KEYS)
		return 0;

	/* A release goes where its press went. */
	if (state == 0U) {
		if (ime->route[key] == KWL_IME_ROUTE_TAKEN) {
			ime->route[key] = KWL_IME_ROUTE_NONE;
			return 1;
		}

		/* A key the input method heard pressed hears its release. */
		if (ime->route[key] == KWL_IME_ROUTE_GRAB) {
			ime->route[key] = KWL_IME_ROUTE_NONE;
			if (ime->grab != NULL)
				ime_send_key(server, time, key, state);
			return 1;
		}

		return 0;
	}

	/* The language keys need the input method's status. */
	if (ime->status == NULL)
		return 0;

	/* Alt+Space (without Ctrl or Super) asks for the next language (the user's choice, 2026-09-29). */
	modifiers = server->modifiers;
	if (key == IME_KEY_SPACE &&
	    (modifiers & IME_MODIFIER_ALT) != 0U &&
	    (modifiers & (IME_MODIFIER_CONTROL | IME_MODIFIER_META)) == 0U) {
		ime_emit(ime->status, STATUS_NEXT, NULL, 0);
		ime->route[key] = KWL_IME_ROUTE_TAKEN;
		return 1;
	}

	/* The Japanese keyboard's keys choose a language; 変換 is the input method's own while Japanese is chosen. */
	direct = ime_is_direct(ime);
	switch (key) {
	case IME_KEY_HENKAN:
		if (!direct)
			return 0;
		ime_select(server, "ja");
		break;
	case IME_KEY_KATAKANAHIRAGANA:
	case IME_KEY_HANGEUL:
		ime_select(server, "ja");
		break;
	case IME_KEY_MUHENKAN:
	case IME_KEY_HANJA:
		ime_select(server, "direct");
		break;
	default:
		return 0;
	}

	/* Succeeded: the language key is taken, and so is its release. */
	ime->route[key] = KWL_IME_ROUTE_TAKEN;
	return 1;
}

/*
 * Gives a key press to the input method's keyboard grab, when a text input
 * is served, the language is not direct input and the input method answers.
 * With composing_only, only while text is being composed (the place before
 * the window's menu and tab keys).
 *
 * Returns nonzero when the key is taken.
 */
int
kwl_ime_key_grab(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state,
	int composing_only)
{
	struct kwl_ime *ime;
	int direct;

	ime = server->ime;
	if (ime == NULL || key >= KWL_IME_KEYS)
		return 0;

	/* Releases were routed by kwl_ime_key_early. */
	if (state == 0U)
		return 0;

	/* Before the menu keys, only while composing. */
	if (composing_only && !ime->composing)
		return 0;

	/* The input method must hold the keyboard for a served text input, and answer. */
	if (ime->grab == NULL || !ime->activated || ime->bypass)
		return 0;

	/* Direct input passes the input method by (plan/ws095/design.md D5). */
	direct = ime_is_direct(ime);
	if (direct)
		return 0;

	/* The key goes to the grab, and the input method is watched until it answers. */
	ime_send_key(server, time, key, state);
	ime->route[key] = KWL_IME_ROUTE_GRAB;
	if (!ime->watching) {
		ime->watching = 1;
		ime->watch_ms = kwl_milliseconds();
	}

	/* Succeeded: the input method has the key. */
	return 1;
}

/*
 * Tells the input method's keyboard grab the modifiers held.
 */
void
kwl_ime_modifiers(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	uint32_t words[5];

	ime = server->ime;
	if (ime == NULL || ime->grab == NULL)
		return;

	/* The serial, the held and locked modifiers; nothing latched, group 0. */
	words[0] = kwl_next_serial(server);
	words[1] = server->modifiers;
	words[2] = 0;
	words[3] = server->locked_modifiers;
	words[4] = 0;
	ime_emit(ime->grab, GRAB_MODIFIERS, words, sizeof(words));
}

/*
 * Follows a change of the keyboard focus: the preedit the old text input
 * shows is committed to it (the words typed are not lost), its text inputs
 * hear leave, the new ones enter, and the input method is told.
 */
void
kwl_ime_focus(
	struct kwl_server *server,
	struct kwl_object *previous)
{
	struct kwl_ime *ime;

	/* The preedit shown is committed where it was typed. */
	ime = server->ime;
	if (ime != NULL && ime->active != NULL && ime->preedit_shown != NULL && ime->preedit_shown[0] != '\0')
		ime_deliver(server, ime->active, NULL, 0, 0, ime->preedit_shown, 0, 0);

	/* The keys the virtual keyboard held were the old window's; its leave lets them go. */
	if (ime != NULL)
		memset(ime->keyboard_down, 0, sizeof(ime->keyboard_down));

	/* The language of the application that has the keyboard now (ws095-p016). */
	if (ime != NULL)
		ime_app_focus(server);

	/* The text inputs hear leave and enter (text-input.c). */
	kwl_text_input_focus(server, previous);

	/* The input method follows. */
	kwl_ime_update(server, NULL);
}

/*
 * Brings the input method up to date with the text input to serve: it is
 * deactivated from one that is no longer served and activated for a new
 * one; a served text input that committed tells it its state again.
 */
void
kwl_ime_update(
	struct kwl_server *server,
	struct kwl_text_input *committed)
{
	struct kwl_ime *ime;
	struct kwl_text_input *current;

	ime = server->ime;
	if (ime == NULL || ime->method == NULL)
		return;

	/* The text input to serve now: the compositor's own field with the keyboard, else an application's. */
	current = ime_current(server);

	/* Another text input (or none): the old one is given up, and the new one taken. */
	if (current != ime->active || (current != NULL && !ime->activated)) {
		if (ime->activated)
			ime_deactivate(server);
		if (current != NULL)
			ime_activate(server, current);
		return;
	}

	/* The same text input committed new state. */
	if (current != NULL && committed == current)
		ime_state(server, current);
}

/*
 * Stops serving a text input that goes.
 */
void
kwl_ime_text_input_gone(
	struct kwl_server *server,
	struct kwl_text_input *input)
{
	struct kwl_ime *ime;

	/* Only the text input being served matters. */
	ime = server->ime;
	if (ime == NULL || ime->active != input)
		return;

	/* The input method is told it serves none. */
	ime_deactivate(server);
}

/*
 * Follows the compositor's own text field (titlebar-shell.c, BUG-177): its
 * editing began or ended, or its text or cursor changed.  The input
 * method is activated for it, told its new state, or deactivated.
 */
void
kwl_ime_field_changed(
	struct kwl_server *server)
{
	/* As a commit of the field's text input. */
	kwl_ime_update(server, &ime_field);

	/* The candidate window follows the field (its box is known once the field was drawn). */
	if (server->ime != NULL && server->ime->active == &ime_field)
		ime_rectangles(server);
}

/*
 * Gives a key press to the input method before the compositor's own text field
 * takes it, while the field is the text input served (the place of
 * kwl_ime_key_grab for an application's).  Returns nonzero when taken.
 */
int
kwl_ime_field_key(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	int taken;

	/* Only while the field is served. */
	if (server->ime == NULL || server->ime->active != &ime_field)
		return 0;

	/* The input method's grab, as for an application. */
	taken = kwl_ime_key_grab(server, time, key, state, 0);
	if (!taken)
		return 0;

	/* Succeeded: the input method has it. */
	return 1;
}

/*
 * Gives a key press to the input method before App Home takes it, while
 * Home's search is the text input served (ws090-p022).  Returns nonzero
 * when taken.
 */
int
kwl_ime_home_key(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	int taken;

	/* Only while Home's search is served. */
	if (ime_field_kind != IME_FIELD_HOME)
		return 0;

	/* As the compositor's other own field. */
	taken = kwl_ime_field_key(server, time, key, state);
	return taken;
}

/*
 * Notes a commit of a surface: a commit of the candidate window's surface
 * redraws the output (the window appears, changes or goes).
 */
void
kwl_ime_surface_commit(
	struct kwl_object *surface)
{
	struct kwl_ime *ime;
	unsigned i;

	/* Only the input method's surfaces can be its popups. */
	ime = surface->client->server->ime;
	if (ime == NULL)
		return;
	if (!surface->client->ime)
		return;

	/* A popup's surface redraws the output. */
	for (i = 0; i < sizeof(ime->popups) / sizeof(ime->popups[0]); i++) {
		if (ime->popups[i] == NULL)
			continue;

		/* The popup this surface is. */
		if (ime->popups[i]->surface == surface) {
			surface->client->server->dirty = 1;
			return;
		}
	}
}

/*
 * Draws the candidate windows over everything but the cursor, below the
 * served text input's cursor rectangle (above it where there is no room),
 * with the glass look's soft shadow.
 */
void
kwl_ime_popup_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct kwl_ime *ime;
	unsigned i;

	/* Only while a text input is served. */
	ime = server->ime;
	if (ime == NULL)
		return;
	if (!ime->activated)
		return;
	if (ime->active == NULL)
		return;

	/* Each popup. */
	for (i = 0; i < sizeof(ime->popups) / sizeof(ime->popups[0]); i++) {
		if (ime->popups[i] == NULL)
			continue;

		ime_popup_draw_one(server, command, ime->popups[i]->surface);
	}
}

/*
 * Gives the width the indicator takes in the system bar: none without an
 * input method that has told its language.
 */
int32_t
kwl_ime_indicator_width(
	struct kwl_server *server)
{
	struct kwl_ime *ime;

	/* No input method, or none that has told its language. */
	ime = server->ime;
	if (ime == NULL)
		return 0;
	if (ime->client == NULL)
		return 0;
	if (ime->label[0] == '\0')
		return 0;

	/* Its chip and a gap. */
	return IME_INDICATOR_WIDTH + IME_INDICATOR_GAP;
}

/*
 * Draws the indicator of the language chosen ("A", "あ") at x in a bar
 * whose top is at top: bright while a text input is served, pale
 * otherwise.  The bar is the system bar, or a head's on the output the
 * pass draws (ws113-p015).
 */
void
kwl_ime_indicator_draw(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t top,
	const float *ink)
{
	struct kwl_ime *ime;
	float back[4];
	float color[4];
	int32_t width;

	/* Nothing without an input method. */
	ime = server->ime;
	if (ime == NULL)
		return;

	/* Nothing while it has told no language (and a click finds nothing). */
	width = kwl_ime_indicator_width(server);
	if (width == 0) {
		ime->indicator_shown = 0;
		return;
	}

	/* Where a click on this output's bar chooses the next language; the system bar's is logged when it moves (a test reads it). */
	kwl_plane_place(&ime->indicators, server->view_output, x, top);
	if (server->view_output == KWL_PLANE_ANCHOR) {
		if (!ime->indicator_shown || ime->indicator_x != x)
			printf("KWL IME indicator x=%d label=%s\n", (int)x, ime->label);
		ime->indicator_x = x;
	}

	/* It shows, and a click can find it. */
	ime->indicator_shown = 1;

	/* The ink, pale while no text input is served. */
	memcpy(color, ink, sizeof(color));
	if (!ime->activated)
		color[3] *= 0.45f;

	/* A round chip behind the label (ws099-p034). */
	memcpy(back, ink, sizeof(back));
	back[3] = 0.16f;
	glass_draw_solid(server, command, (float)x, (float)(top + IME_INDICATOR_TOP), (float)IME_INDICATOR_WIDTH, (float)IME_INDICATOR_HEIGHT, (float)IME_INDICATOR_HEIGHT * 0.5f, back);

	/* The label, centred. */
	width = glass_text_width(server, SIZE_BAR, ime->label);
	glass_draw_text(server, command, SIZE_BAR, x + (IME_INDICATOR_WIDTH - width) / 2, top + IME_INDICATOR_BASELINE, ime->label, IME_INDICATOR_WIDTH,
			color);
}

/*
 * Takes a press on the indicator: the input method chooses the next
 * language.  Its release is taken too.
 *
 * Returns nonzero when the button is taken.
 */
int
kwl_ime_indicator_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct kwl_ime *ime;
	int32_t x;
	int32_t y;
	int32_t left;
	int32_t top;
	int placed;

	/* Only the left button on a shown indicator of an input method that hears its status. */
	ime = server->ime;
	if (ime == NULL)
		return 0;
	if (!ime->indicator_shown)
		return 0;
	if (ime->status == NULL)
		return 0;
	if (button != KWL_BUTTON_LEFT)
		return 0;

	/* The chip as drawn on the bar of the output the pointer is on (the system bar's or a head's, ws113-p015). */
	placed = kwl_plane_placed(&ime->indicators, server->pointer_output, &left, &top);
	if (!placed)
		return 0;

	/* The pointer must be on the chip (a little larger than the drawing), in the bar. */
	x = server->pointer_x;
	y = server->pointer_y;
	if (x < left - IME_INDICATOR_SLOP)
		return 0;
	if (x >= left + IME_INDICATOR_WIDTH + IME_INDICATOR_SLOP)
		return 0;
	if (y < top || y >= top + KWL_GLASS_BAR)
		return 0;

	/* A press asks for the next language. */
	if (state != 0U) {
		ime_emit(ime->status, STATUS_NEXT, NULL, 0);
		printf("KWL IME indicator next\n");
	}

	/* Succeeded: the press (or its release) is the indicator's. */
	return 1;
}

/*
 * Starts the input method's program on a socket pair and makes its
 * connection.
 */
static void
ime_spawn(
	struct kwl_server *server,
	uint64_t now)
{
	struct kwl_ime *ime;
	struct kwl_client *client;
	const char *method;
	char variable[32];
	int pair[2];
	int descriptor;
	int error;
	pid_t child;

	ime = server->ime;
	ime->restart_ms = 0;

	/* The input method chosen, as the program's argument (WS154). */
	method = "--method=ja";
	if (server->ime_method == 0)
		method = "--method=none";
	else if (server->ime_method == 2)
		method = "--method=skk";

	/* The pair: the compositor keeps one end, the input method gets the other. */
	error = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
	if (error != 0) {
		printf("KWL IME spawn-failed step=socketpair errno=%d\n", errno);
		ime->restart_ms = now + IME_RESTART_MS;
		return;
	}

	/* The program runs in a process of its own. */
	child = fork();
	if (child < 0) {
		printf("KWL IME spawn-failed step=fork errno=%d\n", errno);
		close(pair[0]);
		close(pair[1]);
		ime->restart_ms = now + IME_RESTART_MS;
		return;
	}

	/* The child: its own session, only its end of the pair, named by WAYLAND_SOCKET. */
	if (child == 0) {
		(void)setsid();
		for (descriptor = 3; descriptor < 1024; descriptor++) {
			if (descriptor != pair[1])
				(void)close(descriptor);
		}

		(void)fcntl(pair[1], F_SETFD, 0);
		snprintf(variable, sizeof(variable), "%d", pair[1]);
		(void)setenv("WAYLAND_SOCKET", variable, 1);
		(void)unsetenv("WAYLAND_DISPLAY");
		(void)execl(IME_PROGRAM, IME_PROGRAM, method, (char *)NULL);
		_exit(127);
	}

	/* The parent keeps its end as a new connection. */
	close(pair[1]);
	client = ime_connect(server, pair[0]);
	if (client == NULL) {
		(void)kill(child, SIGTERM);
		ime->pid = child;
		ime->restart_ms = now + IME_RESTART_MS;
		return;
	}

	/* The start is counted in the window of the last three, unless it follows a change of method. */
	if (ime->replacing) {
		ime->replacing = 0;
	} else {
		ime->starts[ime->start_index] = now;
		ime->start_index = (ime->start_index + 1U) % 3U;
	}

	/* Succeeded: the input method runs on its connection. */
	ime->pid = child;
	ime->client = client;
	ime->method_started = server->ime_method;
	printf("KWL IME started pid=%ld client=%llu %s\n", (long)child, (unsigned long long)client->number, method);
}

/*
 * Makes the input method's end of the pair a connection, as accept_client
 * does for the socket's (main.c).
 */
static struct kwl_client *
ime_connect(
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
		printf("KWL IME spawn-failed step=nonblock errno=%d\n", errno);
		close(descriptor);
		return NULL;
	}

	/* The connection's state. */
	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(descriptor);
		return NULL;
	}

	/* It is the input method's: only it sees the input method's globals. */
	client->fd = descriptor;
	client->server = server;
	client->number = ++server->client_serial;
	client->connected_ms = kwl_milliseconds();
	client->ime = 1;
	client->next = server->clients;
	server->clients = client;

	/* The display, the connection's first object. */
	display = kwl_create(client, 1, KWL_DISPLAY, 1);
	if (display == NULL) {
		kwl_client_destroy(client);
		return NULL;
	}

	/* Succeeded: the connection is served from the next pass. */
	printf("KWL CLIENT client=%llu fd=%d ime=1\n", (unsigned long long)client->number, descriptor);
	return client;
}

/*
 * Lets go of everything a gone input method held: the keys its virtual
 * keyboard pressed, the preedit the application shows, the keys it heard
 * pressed; it is started again later.
 */
static void
ime_lost(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	unsigned i;

	ime = server->ime;
	printf("KWL IME lost pid=%ld\n", (long)ime->pid);

	/* The keys its virtual keyboard holds are let go at the application. */
	ime_release_keyboard(server);

	/* The application's preedit goes. */
	if (ime->active != NULL && ime->preedit_shown != NULL && ime->preedit_shown[0] != '\0')
		ime_deliver(server, ime->active, NULL, 0, 0, NULL, 0, 0);

	/* The keys it heard pressed: their releases go nowhere. */
	for (i = 0; i < KWL_IME_KEYS; i++) {
		if (ime->route[i] == KWL_IME_ROUTE_GRAB)
			ime->route[i] = KWL_IME_ROUTE_NONE;
	}

	/* Its objects and state are forgotten. */
	ime->method = NULL;
	ime->grab = NULL;
	ime->keyboard = NULL;
	ime->keyboard_keymap = 0;
	ime->status = NULL;
	memset(ime->popups, 0, sizeof(ime->popups));
	ime->active = NULL;
	ime->activated = 0;
	ime->composing = 0;
	ime->watching = 0;
	ime->bypass = 0;
	strcpy(ime->language, "direct");
	ime_set_text(&ime->pending_commit, NULL);
	ime_set_text(&ime->pending_preedit, NULL);
	ime_set_text(&ime->preedit_shown, NULL);
	ime->client = NULL;

	/* It is started again after a moment (at once after a change of method); the indicator and the candidate window go meanwhile. */
	ime->label[0] = '\0';
	server->dirty = 1;
	ime->restart_ms = kwl_milliseconds() + IME_RESTART_MS;
	if (ime->replacing)
		ime->restart_ms = kwl_milliseconds();
}

/*
 * Carries out a request of one of the input method's globals.
 */
static int
ime_manager_request(
	struct kwl_object *manager,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_ime *ime;
	struct kwl_object *object;
	uint32_t id;

	server = manager->client->server;
	ime = server->ime;

	/* Each global has its own requests. */
	switch (manager->kind) {
	case KWL_INPUT_METHOD_MANAGER:
		/* destroy leaves what it made. */
		if (opcode == MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(manager);
			return 0;
		}

		/* get_input_method: the seat and the new ID. */
		if (opcode != MANAGER_GET_INPUT_METHOD || size != 8U)
			return EPROTO;

		id = ime_word(bytes, 4);
		object = kwl_create(manager->client, id, KWL_INPUT_METHOD, manager->version);
		if (object == NULL)
			return EPROTO;

		/* A second input method on the seat is unavailable from the start. */
		if (ime == NULL || ime->method != NULL) {
			ime_emit(object, METHOD_UNAVAILABLE, NULL, 0);
			return 0;
		}

		/* The input method; a text input served now activates it at once. */
		ime->method = object;
		kwl_ime_update(server, NULL);
		return 0;
	case KWL_VIRTUAL_KEYBOARD_MANAGER:
		/* create_virtual_keyboard: the seat and the new ID. */
		if (opcode != KEYBOARDS_CREATE || size != 8U)
			return EPROTO;

		id = ime_word(bytes, 4);
		object = kwl_create(manager->client, id, KWL_VIRTUAL_KEYBOARD, manager->version);
		if (object == NULL)
			return EPROTO;

		/* The newest virtual keyboard is the one whose keys go to the applications. */
		if (ime != NULL)
			ime->keyboard = object;
		return 0;
	case KWL_IME_STATUS_MANAGER:
		/* destroy leaves the status. */
		if (opcode == STATUSES_DESTROY && size == 0U) {
			kwl_object_destroy(manager);
			return 0;
		}

		/* get_status: the new ID. */
		if (opcode != STATUSES_GET_STATUS || size != 4U)
			return EPROTO;

		id = ime_word(bytes, 0);
		object = kwl_create(manager->client, id, KWL_IME_STATUS, manager->version);
		if (object == NULL)
			return EPROTO;

		if (ime != NULL)
			ime->status = object;
		return 0;
	default:
		break;
	}

	/* No other global is the input method's. */
	return EPROTO;
}

/*
 * Carries out a request of zwp_input_method_v2.
 */
static int
ime_method_request(
	struct kwl_object *method,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_ime *ime;
	char *text;
	size_t next;
	int error;

	server = method->client->server;
	ime = server->ime;

	/* destroy retires the object. */
	if (opcode == METHOD_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(method);
		return 0;
	}

	/* An unavailable input method's requests are taken and change nothing. */
	if (ime == NULL || ime->method != method)
		return 0;

	/* Each request sets pending state or makes an object; commit applies. */
	switch (opcode) {
	case METHOD_COMMIT_STRING:
		/* The text to commit. */
		error = ime_read_string(bytes, size, 0, &text, &next);
		if (error != 0 || next != size)
			return EPROTO;
		free(ime->pending_commit);
		ime->pending_commit = text;
		return 0;
	case METHOD_SET_PREEDIT_STRING:
		/* The text being composed and its cursor. */
		error = ime_read_string(bytes, size, 0, &text, &next);
		if (error != 0 || next + 8U != size)
			return EPROTO;
		free(ime->pending_preedit);
		ime->pending_preedit = text;
		ime->pending_begin = (int32_t)ime_word(bytes, next);
		ime->pending_end = (int32_t)ime_word(bytes, next + 4U);
		return 0;
	case METHOD_DELETE_SURROUNDING_TEXT:
		/* The text to delete around the cursor. */
		if (size != 8U)
			return EPROTO;
		ime->pending_before = ime_word(bytes, 0);
		ime->pending_after = ime_word(bytes, 4);
		return 0;
	case METHOD_COMMIT:
		/* What was set applies, whatever the serial (plan/ws095/design.md section 3.2). */
		if (size != 4U)
			return EPROTO;
		ime_apply(server);
		return 0;
	case METHOD_GET_INPUT_POPUP_SURFACE:
		/* The popup's new ID and its surface. */
		if (size != 8U)
			return EPROTO;
		error = ime_popup(method, ime_word(bytes, 0), ime_word(bytes, 4));
		return error;
	case METHOD_GRAB_KEYBOARD:
		/* The grab's new ID. */
		if (size != 4U)
			return EPROTO;
		error = ime_grab_keyboard(method, ime_word(bytes, 0));
		return error;
	default:
		break;
	}

	/* No other request exists. */
	return EPROTO;
}

/*
 * Carries out a request of zwp_virtual_keyboard_v1.
 */
static int
ime_keyboard_request(
	struct kwl_object *keyboard,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_ime *ime;
	int descriptor;
	int error;

	server = keyboard->client->server;
	ime = server->ime;

	/* Each request of the virtual keyboard. */
	switch (opcode) {
	case KEYBOARD_KEYMAP:
		/* The keymap is taken and not used: the applications keep the compositor's (keymap.c). */
		if (size != 8U)
			return EPROTO;
		descriptor = kwl_take_fd(keyboard->client);
		if (descriptor < 0)
			return EPROTO;
		close(descriptor);
		if (ime != NULL && ime->keyboard == keyboard)
			ime->keyboard_keymap = 1;
		return 0;
	case KEYBOARD_KEY:
		/* A key needs the keymap first (the protocol's no_keymap error). */
		if (size != 12U)
			return EPROTO;
		if (ime == NULL || ime->keyboard != keyboard)
			return 0;
		if (!ime->keyboard_keymap) {
			error = kwl_error_code(keyboard->client, keyboard->id, KEYBOARD_ERROR_NO_KEYMAP, "no keymap was set");
			return error;
		}

		/* The key goes to the application. */
		ime_keyboard_key(server, ime_word(bytes, 0), ime_word(bytes, 4), ime_word(bytes, 8));
		return 0;
	case KEYBOARD_MODIFIERS:
		/* The applications hear the keyboard's own modifiers; these are taken and not used. */
		if (size != 16U)
			return EPROTO;
		if (ime != NULL && ime->keyboard == keyboard && !ime->keyboard_keymap) {
			error = kwl_error_code(keyboard->client, keyboard->id, KEYBOARD_ERROR_NO_KEYMAP, "no keymap was set");
			return error;
		}

		return 0;
	case KEYBOARD_DESTROY:
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(keyboard);
		return 0;
	default:
		break;
	}

	/* No other request exists. */
	return EPROTO;
}

/*
 * Carries out a request of kl_ime_status_v1.
 */
static int
ime_status_request(
	struct kwl_object *status,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_ime *ime;
	char *id;
	char *label;
	size_t next;
	size_t end;
	int error;

	ime = status->client->server->ime;

	/* Each request of the status. */
	switch (opcode) {
	case STATUS_DESTROY:
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(status);
		return 0;
	case STATUS_LANGUAGE:
		/* The language chosen now and its label. */
		error = ime_read_string(bytes, size, 0, &id, &next);
		if (error != 0)
			return EPROTO;
		error = ime_read_string(bytes, size, next, &label, &end);
		if (error != 0 || end != size) {
			free(id);
			return EPROTO;
		}

		/* The language decides whether keys pass the input method by; its label is the indicator's. */
		if (ime != NULL && ime->status == status) {
			snprintf(ime->language, sizeof(ime->language), "%s", id);
			snprintf(ime->label, sizeof(ime->label), "%s", label);
			status->client->server->dirty = 1;
			printf("KWL IME language=%s\n", ime->language);

			/* It is the language of the application (or the desktop) that has the keyboard (ws095-p016). */
			ime_app_remember(ime);
		}

		free(id);
		free(label);
		return 0;
	case STATUS_COMPOSING:
		/* Whether text is being composed. */
		if (size != 4U)
			return EPROTO;
		if (ime != NULL && ime->status == status)
			ime->composing = ime_word(bytes, 0) != 0U;
		return 0;
	case STATUS_PREDICTIONS:
		/* The answer to a predict: its serial and the words (version 2, ws166-p002). */
		if (status->version < STATUS_VERSION_PREDICT || size < 8U)
			return EPROTO;
		error = ime_read_string(bytes, size, 4U, &id, &end);
		if (error != 0 || end != size)
			return EPROTO;

		/* The on-screen keyboard shows them when they answer its latest reading. */
		if (ime != NULL && ime->status == status)
			kwl_keyboard_predictions(status->client->server, ime_word(bytes, 0), id);
		free(id);
		return 0;
	default:
		break;
	}

	/* No other request exists. */
	return EPROTO;
}

/*
 * Makes the input method's keyboard grab, and tells it the keymap, the
 * repeat and the modifiers.
 */
static int
ime_grab_keyboard(
	struct kwl_object *method,
	uint32_t id)
{
	struct kwl_server *server;
	struct kwl_object *grab;
	uint32_t words[2];
	int32_t repeat[2];
	uint32_t size;
	int descriptor;
	int error;

	server = method->client->server;

	/* The grab. */
	grab = kwl_create(method->client, id, KWL_KEYBOARD_GRAB, method->version);
	if (grab == NULL)
		return EPROTO;

	server->ime->grab = grab;

	/* The keymap the keys are in: the compositor's (keymap.c), or none. */
	words[0] = IME_KEYMAP_XKB_V1;
	descriptor = kwl_keymap_descriptor(&size);
	words[1] = size;
	if (descriptor < 0) {
		descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			return errno;
		words[0] = IME_KEYMAP_NO_KEYMAP;
		words[1] = 0;
	}

	error = kwl_emit_fd(grab->client, grab->id, GRAB_KEYMAP, words, sizeof(words), descriptor);
	if (error != 0)
		return error;

	/* The repeat the applications are told, then the modifiers held. */
	repeat[0] = server->repeat_rate;
	repeat[1] = server->repeat_delay_ms;
	ime_emit(grab, GRAB_REPEAT_INFO, repeat, sizeof(repeat));
	kwl_ime_modifiers(server);

	/* Succeeded: keys can go to the input method. */
	return 0;
}

/*
 * Makes an input popup surface (the candidate window); it hears the
 * served text input's rectangle.  Drawing it is ws095-p005's.
 */
static int
ime_popup(
	struct kwl_object *method,
	uint32_t id,
	uint32_t surface_id)
{
	struct kwl_ime *ime;
	struct kwl_object *popup;
	struct kwl_object *surface;
	unsigned i;
	int error;

	/* The surface must be the input method's own, live and a surface. */
	surface = kwl_find(method->client, surface_id);
	if (surface == NULL ||
	    surface->dead ||
	    surface->kind != KWL_SURFACE)
		return EPROTO;

	/* It must have no role yet. */
	if (surface->role != NULL ||
	    surface->cursor_role ||
	    surface->sub_role != NULL) {
		error = kwl_error_code(method->client, method->id, METHOD_ERROR_ROLE, "the surface has another role");
		return error;
	}

	/* The popup, which names its surface. */
	popup = kwl_create(method->client, id, KWL_INPUT_POPUP, method->version);
	if (popup == NULL)
		return EPROTO;

	popup->surface = surface;

	/* It takes a free slot; more popups than slots are not told the rectangle. */
	ime = method->client->server->ime;
	for (i = 0; i < sizeof(ime->popups) / sizeof(ime->popups[0]); i++) {
		if (ime->popups[i] == NULL) {
			ime->popups[i] = popup;
			break;
		}
	}

	/* A popup made while a text input is served hears where it is. */
	if (ime->activated)
		ime_rectangles(method->client->server);

	/* Succeeded: the popup exists. */
	return 0;
}

/*
 * Applies what the input method set at its commit: the served text input
 * hears it; the pending state is emptied.
 */
static void
ime_apply(
	struct kwl_server *server)
{
	struct kwl_ime *ime;

	ime = server->ime;

	/* The served text input hears the preedit, the commit and the deletion together. */
	if (ime->active != NULL && ime->activated) {
		ime_deliver(server, ime->active, ime->pending_preedit, ime->pending_begin, ime->pending_end,
			    ime->pending_commit, ime->pending_before, ime->pending_after);
		ime_set_text(&ime->preedit_shown, ime->pending_preedit);
	}

	/* The pending state starts again from nothing. */
	ime_set_text(&ime->pending_commit, NULL);
	ime_set_text(&ime->pending_preedit, NULL);
	ime->pending_begin = 0;
	ime->pending_end = 0;
	ime->pending_before = 0;
	ime->pending_after = 0;
}

/*
 * Activates the input method for a text input and tells it the state.
 */
static void
ime_activate(
	struct kwl_server *server,
	struct kwl_text_input *input)
{
	struct kwl_ime *ime;

	ime = server->ime;

	/* activate, then the state, then done. */
	ime->active = input;
	ime->activated = 1;
	ime_set_text(&ime->preedit_shown, NULL);
	ime_emit(ime->method, METHOD_ACTIVATE, NULL, 0);
	ime_state(server, input);
	ime_rectangles(server);
	server->dirty = 1;

	/* Logged: the compositor's own field has no object of its own (Home's search no window either). */
	if (input == &ime_field && ime_field_kind == IME_FIELD_HOME) {
		printf("KWL IME activate field home\n");
		return;
	}
	if (input == &ime_field) {
		printf("KWL IME activate field client=%llu surface=%u\n", (unsigned long long)input->surface->client->number, input->surface->id);
		return;
	}

	/* An application's text input. */
	printf("KWL IME activate client=%llu\n", (unsigned long long)input->object->client->number);
}

/*
 * Tells the input method a served text input's state, and done.
 */
static void
ime_state(
	struct kwl_server *server,
	struct kwl_text_input *input)
{
	unsigned char payload[KWL_IME_TEXT_MAX + 16U];
	struct kwl_ime *ime;
	uint32_t words[2];
	size_t offset;

	ime = server->ime;

	/* The surrounding text, when the text input gave it. */
	if (input->text != NULL) {
		offset = ime_put_string(payload, 0, input->text);
		words[0] = (uint32_t)input->cursor;
		words[1] = (uint32_t)input->anchor;
		memcpy(payload + offset, words, sizeof(words));
		ime_emit(ime->method, METHOD_SURROUNDING_TEXT, payload, offset + sizeof(words));
	}

	/* What changed the text, and the content type. */
	words[0] = input->cause;
	ime_emit(ime->method, METHOD_TEXT_CHANGE_CAUSE, words, 4U);
	words[0] = input->hint;
	words[1] = input->purpose;
	ime_emit(ime->method, METHOD_CONTENT_TYPE, words, sizeof(words));

	/* done applies them; the input method's commits name the number of dones. */
	ime_emit(ime->method, METHOD_DONE, NULL, 0);
	ime->done_count++;
}

/*
 * Deactivates the input method: no text input is served.
 */
static void
ime_deactivate(
	struct kwl_server *server)
{
	struct kwl_ime *ime;

	ime = server->ime;

	/* deactivate and done; the input method drops what it composes. */
	if (ime->method != NULL) {
		ime_emit(ime->method, METHOD_DEACTIVATE, NULL, 0);
		ime_emit(ime->method, METHOD_DONE, NULL, 0);
		ime->done_count++;
	}

	ime->active = NULL;
	ime->activated = 0;
	ime_set_text(&ime->preedit_shown, NULL);
	server->dirty = 1;
	printf("KWL IME deactivate\n");
}

/*
 * Tells the input method's popups the served text input's cursor rectangle.
 */
static void
ime_rectangles(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	unsigned i;

	ime = server->ime;
	if (ime->active == NULL)
		return;

	/* Each popup hears the rectangle (placing the popup is ws095-p005's). */
	for (i = 0; i < sizeof(ime->popups) / sizeof(ime->popups[0]); i++) {
		if (ime->popups[i] != NULL)
			ime_emit(ime->popups[i], POPUP_TEXT_INPUT_RECTANGLE, ime->active->rectangle, sizeof(ime->active->rectangle));
	}
}

/*
 * Draws one candidate window: its surface's image by the cursor, blended
 * (its corners are transparent), with the glass look's soft shadow.
 */
static void
ime_popup_draw_one(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface)
{
	const struct kwl_import *image;
	struct kwl_import alpha;
	struct glass_shape shape;
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;
	int placed;

	/* A live surface with an image (an empty one hides the window). */
	if (surface == NULL)
		return;
	if (surface->dead)
		return;

	image = kwl_compose_surface_image(surface);
	if (image == NULL)
		return;

	/* Its place by the cursor. */
	placed = ime_popup_place(server, surface, &x, &y);
	if (!placed)
		return;

	/* Its size, the image's when the surface has none of its own. */
	kwl_surface_size(surface, &width, &height);
	if (width == 0U || height == 0U) {
		width = image->width;
		height = image->height;
	}

	/* The glass look gives it a soft shadow. */
	if (server->glass) {
		glass_shape_init(&shape, (float)x, (float)y + 3.0f, (float)width, (float)height);
		shape.quad[0] -= 2.0f * IME_POPUP_SHADOW;
		shape.quad[1] -= 2.0f * IME_POPUP_SHADOW;
		shape.quad[2] += 4.0f * IME_POPUP_SHADOW;
		shape.quad[3] += 4.0f * IME_POPUP_SHADOW;
		shape.mode = MODE_SHADOW;
		shape.radius = IME_POPUP_RADIUS;
		shape.soft = IME_POPUP_SHADOW;
		shape.color[0] = 0.05f;
		shape.color[1] = 0.08f;
		shape.color[2] = 0.16f;
		shape.color[3] = 0.28f;
		glass_shape_draw(server, command, &shape);
	}

	/* The image, blended. */
	alpha = *image;
	alpha.draw = KWL_DRAW_ALPHA;
	kwl_compose_surface_quad(server, command, surface, &alpha, x, y);
}

/*
 * Places a candidate window below the served text input's cursor rectangle,
 * or above it where it would pass the output's bottom, and within the
 * output's left and right edges.
 *
 * Returns zero when the served text input has no surface on the output.
 */
static int
ime_popup_place(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t *x,
	int32_t *y)
{
	struct kwl_text_input *input;
	struct kwl_object *window;
	uint32_t width;
	uint32_t height;
	int32_t origin_x;
	int32_t origin_y;
	int32_t below;
	int32_t above;

	/* The text input's surface, where the rectangle is (Home's search has none: the rectangle is the output's). */
	input = server->ime->active;
	window = input->surface;
	origin_x = 0;
	origin_y = 0;
	if (input != &ime_field || ime_field_kind != IME_FIELD_HOME) {
		if (window == NULL || window->dead)
			return 0;
		origin_x = window->x;
		origin_y = window->y;
	}

	/* The popup's size. */
	kwl_surface_size(surface, &width, &height);

	/* Below the rectangle, at its left. */
	*x = origin_x + input->rectangle[0];
	below = origin_y + input->rectangle[1] + input->rectangle[3] + IME_POPUP_GAP;
	above = origin_y + input->rectangle[1] - IME_POPUP_GAP - (int32_t)height;
	*y = below;
	if (below + (int32_t)height > (int32_t)server->height && above >= 0)
		*y = above;

	/* Within the output's edges. */
	if (*x + (int32_t)width > (int32_t)server->width)
		*x = (int32_t)server->width - (int32_t)width;
	if (*x < 0)
		*x = 0;

	/* Succeeded: the place. */
	return 1;
}

/*
 * Gives a key of the virtual keyboard to the application.  While nothing
 * is being composed, a press may be the window's menu or tab key first.
 */
static void
ime_keyboard_key(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct kwl_ime *ime;
	int taken;

	ime = server->ime;

	/* A key past the remembered range goes as it is. */
	if (key >= KWL_IME_KEYS) {
		kwl_seat_key_deliver(server, time, key, state);
		return;
	}

	/* Home's search served: every key is Home's while it is open (home.c, ws090-p022). */
	if (ime->active == &ime_field && ime_field_kind == IME_FIELD_HOME) {
		taken = kwl_home_key(server, key, state);
		if (taken)
			return;
	}

	/* The compositor's own field served: its keys are the field's (titlebar-shell.c), the rest the application's. */
	if (ime->active == &ime_field) {
		taken = kwl_titlebar_key(server, key, state);
		if (taken)
			return;
	}

	/* A release of a key the menu took goes nowhere. */
	if (state == 0U && ime->keyboard_taken[key]) {
		ime->keyboard_taken[key] = 0;
		return;
	}

	/* A press may be the window's menu or tab key, when nothing is being composed. */
	if (state != 0U && !ime->composing) {
		taken = kwl_menu_key(server, key, state);
		if (!taken)
			taken = kwl_titlebar_tab_key(server, key, state);
		if (taken) {
			ime->keyboard_taken[key] = 1;
			return;
		}
	}

	/* The application hears it, and a held key is remembered. */
	kwl_seat_key_deliver(server, time, key, state);
	ime->keyboard_down[key] = 0;
	if (state != 0U)
		ime->keyboard_down[key] = 1;
}

/*
 * Lets go, at the application, of every key the virtual keyboard holds.
 */
static void
ime_release_keyboard(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	uint32_t key;

	ime = server->ime;

	/* Each held key hears its release. */
	for (key = 0; key < KWL_IME_KEYS; key++) {
		if (!ime->keyboard_down[key])
			continue;

		ime->keyboard_down[key] = 0;
		kwl_seat_key_deliver(server, kwl_milliseconds() & 0xffffffffU, key, 0);
	}

	memset(ime->keyboard_taken, 0, sizeof(ime->keyboard_taken));
}

/*
 * Sends the keyboard grab a key.
 */
static void
ime_send_key(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	uint32_t words[4];

	/* A serial, the time, the evdev code and the state. */
	words[0] = kwl_next_serial(server);
	words[1] = time;
	words[2] = key;
	words[3] = state;
	ime_emit(server->ime->grab, GRAB_KEY, words, sizeof(words));
}

/*
 * Tells the input method to choose a language.
 */
static void
ime_select(
	struct kwl_server *server,
	const char *id)
{
	unsigned char payload[64];
	size_t size;

	/* select carries the language's ID. */
	size = ime_put_string(payload, 0, id);
	ime_emit(server->ime->status, STATUS_SELECT, payload, size);
}

/*
 * Asks the input method for the words a reading of the on-screen keyboard
 * starts (kl_ime_status_v1.predict, version 2; ws166-p002); the
 * answer comes as a predictions request with the same serial.  Returns 0
 * when asked, ENOTSUP without an input method that predicts.
 */
int
kwl_ime_predict(
	struct kwl_server *server,
	uint32_t serial,
	const char *reading)
{
	unsigned char payload[4U + 4U + IME_PREDICT_TEXT_MAX + 4U];
	size_t length;
	size_t size;

	/* An input method of version 2. */
	if (server->ime == NULL || server->ime->status == NULL || server->ime->status->version < STATUS_VERSION_PREDICT)
		return ENOTSUP;

	/* A reading too long is not predicted. */
	length = strlen(reading);
	if (length >= IME_PREDICT_TEXT_MAX)
		return EINVAL;

	/* predict carries the serial and the reading. */
	memcpy(payload, &serial, 4U);
	size = ime_put_string(payload, 4U, reading);
	ime_emit(server->ime->status, STATUS_PREDICT, payload, size);

	/* Succeeded. */
	return 0;
}

/*
 * Tells the input method the word chosen for a reading of the on-screen
 * keyboard, which it learns as if it had converted it (kl_ime_status_v1.learn,
 * version 2; ws166-p002).
 */
void
kwl_ime_learn(
	struct kwl_server *server,
	const char *reading,
	const char *word)
{
	unsigned char payload[2U * (4U + IME_PREDICT_TEXT_MAX + 4U)];
	size_t reading_length;
	size_t word_length;
	size_t size;

	/* An input method of version 2, and texts that fit. */
	if (server->ime == NULL || server->ime->status == NULL || server->ime->status->version < STATUS_VERSION_PREDICT)
		return;
	reading_length = strlen(reading);
	word_length = strlen(word);
	if (reading_length >= IME_PREDICT_TEXT_MAX || word_length >= IME_PREDICT_TEXT_MAX)
		return;

	/* learn carries the reading and the word. */
	size = ime_put_string(payload, 0, reading);
	size = ime_put_string(payload, size, word);
	ime_emit(server->ime->status, STATUS_LEARN, payload, size);
}

/*
 * Tells whether the language chosen is direct input.
 */
static int
ime_is_direct(
	const struct kwl_ime *ime)
{
	int order;

	/* The input method names direct input "direct". */
	order = strcmp(ime->language, "direct");
	if (order == 0)
		return 1;

	/* Any other language composes. */
	return 0;
}

/*
 * Queues an event, failing the client whose queue refuses it.
 */
static void
ime_emit(
	struct kwl_object *object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	int error;

	/* No object, a dead one, or a failed client hears nothing. */
	if (object == NULL || object->dead || object->client->fatal)
		return;

	/* A client that stopped reading loses its connection. */
	error = kwl_emit(object->client, object->id, opcode, payload, size);
	if (error != 0) {
		object->client->fatal = 1;
		object->client->fatal_time = kwl_milliseconds();
	}
}

/*
 * Writes a string argument and gives the offset after it.
 */
static size_t
ime_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, and zeros to a four-byte boundary. */
	length = (uint32_t)strlen(text) + 1U;
	if (length > KWL_IME_TEXT_MAX)
		length = KWL_IME_TEXT_MAX;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, 4);
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length - 1U);

	/* The offset after the string. */
	return offset + 4U + padded;
}

/*
 * Reads a string argument into an allocated copy; *next is the offset after it.
 *
 * Returns 0, EPROTO for a malformed string, or ENOMEM.
 */
static int
ime_read_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	char **text,
	size_t *next)
{
	uint32_t length;
	size_t padded;
	char *copy;

	/* The length, with the NUL. */
	if (offset + 4U > size)
		return EPROTO;

	length = ime_word(bytes, offset);
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (length == 0U || length > KWL_IME_TEXT_MAX || offset + 4U + padded > size)
		return EPROTO;

	/* The text must end with its NUL. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;

	/* The copy. */
	copy = malloc(length);
	if (copy == NULL)
		return ENOMEM;

	memcpy(copy, bytes + offset + 4U, length);

	/* Succeeded: the text and where the next argument starts. */
	*text = copy;
	*next = offset + 4U + padded;
	return 0;
}

/*
 * Reads a 32-bit word of a request.
 */
static uint32_t
ime_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The wire's native byte order. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}

/*
 * Replaces an allocated text with a copy of another (NULL for none).
 */
static void
ime_set_text(
	char **field,
	const char *text)
{
	size_t length;
	char *copy;

	/* The old text goes. */
	free(*field);
	*field = NULL;
	if (text == NULL)
		return;

	/* The copy; without memory the field stays empty. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return;

	memcpy(copy, text, length);
	*field = copy;
}

/*
 * Writes the key of whose language is chosen now (ws095-p016): empty for
 * the desktop (no window has the keyboard, or the desktop surface has it),
 * "app:" and the application ID of the window with the keyboard, or
 * "client:" and its connection's number when it names no application.
 */
static void
ime_app_key(
	struct kwl_server *server,
	char *key,
	size_t size)
{
	struct kwl_object *focus;
	int desktop;

	/* No window with the keyboard, or the desktop's surface: the desktop. */
	key[0] = '\0';
	focus = server->focus;
	if (focus == NULL)
		return;
	desktop = kwl_desktop_is(focus);
	if (desktop != 0)
		return;

	/* The window's application, by its ID when it has one. */
	if (focus->app_id[0] != '\0') {
		(void)snprintf(key, size, "app:%s", focus->app_id);
		return;
	}

	/* Otherwise its connection. */
	(void)snprintf(key, size, "client:%llu", (unsigned long long)focus->client->number);
}

/* Finds an application's entry by its key; NULL when none is kept. */
static struct kwl_ime_app *
ime_app_find(
	struct kwl_ime *ime,
	const char *key)
{
	unsigned index;
	int differs;

	/* Each entry in use. */
	for (index = 0; index < KWL_IME_APPS; index++) {
		if (ime->apps[index].key[0] == '\0')
			continue;
		differs = strcmp(ime->apps[index].key, key);
		if (differs == 0)
			return &ime->apps[index];
	}

	/* Not kept. */
	return NULL;
}

/*
 * Makes an entry for an application, in a free place or, when every place
 * is taken, in place of the first (the table is small and an application
 * that lost its entry only starts again with the desktop's language).
 */
static struct kwl_ime_app *
ime_app_add(
	struct kwl_ime *ime,
	const char *key)
{
	struct kwl_ime_app *app;
	unsigned index;

	/* A free place, else the first one. */
	app = &ime->apps[0];
	for (index = 0; index < KWL_IME_APPS; index++) {
		if (ime->apps[index].key[0] == '\0') {
			app = &ime->apps[index];
			break;
		}
	}

	/* The entry, with no language yet. */
	memset(app, 0, sizeof(*app));
	(void)snprintf(app->key, sizeof(app->key), "%s", key);

	/* Succeeded: the entry is the application's. */
	return app;
}

/* Keeps the language chosen now as the one of whose it is: the application with the keyboard, or the desktop. */
static void
ime_app_remember(
	struct kwl_ime *ime)
{
	struct kwl_ime_app *app;

	/* The desktop's. */
	if (ime->focus_key[0] == '\0') {
		(void)snprintf(ime->desktop_language, sizeof(ime->desktop_language), "%s", ime->language);
		ime->desktop_known = 1;
		return;
	}

	/* An application's (an entry made when it has none yet). */
	app = ime_app_find(ime, ime->focus_key);
	if (app == NULL)
		app = ime_app_add(ime, ime->focus_key);
	(void)snprintf(app->language, sizeof(app->language), "%s", ime->language);
}

/*
 * Follows the keyboard to another application (or the desktop): the
 * language it had is chosen again; an application seen for the first time
 * starts with the desktop's (or, before the desktop ever had the keyboard,
 * keeps the language chosen now).  The same application -- another of its
 * windows, fields or carets -- changes nothing.
 */
static void
ime_app_focus(
	struct kwl_server *server)
{
	struct kwl_ime *ime;
	struct kwl_ime_app *app;
	char key[KWL_IME_APP_KEY];
	const char *wanted;
	const char *why;
	int same;

	/* Whose the keyboard is now; the same application as before keeps its language. */
	ime = server->ime;
	ime_app_key(server, key, sizeof(key));
	same = strcmp(key, ime->focus_key);
	if (same == 0)
		return;

	/* The language chosen until now stays with whose it was. */
	ime_app_remember(ime);
	(void)snprintf(ime->focus_key, sizeof(ime->focus_key), "%s", key);

	/* The language to choose: the desktop's, an application's own, or the desktop's for a new one. */
	wanted = ime->language;
	why = "kept";
	if (key[0] == '\0') {
		if (ime->desktop_known) {
			wanted = ime->desktop_language;
			why = "desktop";
		}
	} else {
		app = ime_app_find(ime, key);
		if (app != NULL && app->language[0] != '\0') {
			wanted = app->language;
			why = "remembered";
		} else {
			if (ime->desktop_known) {
				wanted = ime->desktop_language;
				why = "inherited";
			}
			app = ime_app_add(ime, key);
			(void)snprintf(app->language, sizeof(app->language), "%s", wanted);
		}
	}
	printf("KWL IME app key=%s language=%s from=%s\n", key[0] != '\0' ? key : "desktop", wanted, why);

	/* Chosen only when it differs from the input method's now (the status then tells the new one). */
	same = strcmp(wanted, ime->language);
	if (same == 0 || ime->status == NULL)
		return;
	ime_select(server, wanted);
}

/* Tells whether a connection has a window of an application ID. */
static int
ime_client_has_app(
	const struct kwl_client *client,
	const char *app_id)
{
	const struct kwl_object *object;
	int differs;

	/* Each live window (surface) of the connection. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->kind != KWL_SURFACE || object->dead)
			continue;
		differs = strcmp(object->app_id, app_id);
		if (differs == 0)
			return 1;
	}

	/* None names the application. */
	return 0;
}

/*
 * Forgets the languages of the applications whose last connection ends: a
 * connection's own entry, and an application ID's when no other connection
 * has a window of it.  The next start of the application begins with the
 * desktop's language again.  When the application with the keyboard ends,
 * the keyboard is the desktop's: its language is chosen.
 */
static void
ime_app_forget(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct kwl_ime *ime;
	struct kwl_ime_app *focused;
	struct kwl_client *other;
	char key[KWL_IME_APP_KEY];
	const char *app_id;
	const char *wanted;
	const char *why;
	unsigned index;
	int differs;
	int kept;
	int has;

	/* The connection's own key. */
	ime = server->ime;
	(void)snprintf(key, sizeof(key), "client:%llu", (unsigned long long)client->number);

	/* Each entry: the connection's own goes; an application's goes when this was its last connection. */
	for (index = 0; index < KWL_IME_APPS; index++) {
		if (ime->apps[index].key[0] == '\0')
			continue;
		differs = strcmp(ime->apps[index].key, key);
		if (differs == 0) {
			memset(&ime->apps[index], 0, sizeof(ime->apps[index]));
			continue;
		}

		/* An application ID this connection has a window of. */
		differs = strncmp(ime->apps[index].key, "app:", 4);
		if (differs != 0)
			continue;
		app_id = ime->apps[index].key + 4;
		has = ime_client_has_app(client, app_id);
		if (has == 0)
			continue;

		/* Another connection with a window of it keeps it. */
		kept = 0;
		for (other = server->clients; other != NULL; other = other->next) {
			if (other == client)
				continue;
			has = ime_client_has_app(other, app_id);
			if (has != 0)
				kept = 1;
		}
		if (kept == 0)
			memset(&ime->apps[index], 0, sizeof(ime->apps[index]));
	}

	/* Nothing more unless the application with the keyboard has ended. */
	if (ime->focus_key[0] == '\0')
		return;
	focused = ime_app_find(ime, ime->focus_key);
	if (focused != NULL)
		return;

	/*
	 * Its window goes without a new focus (seat.c clears it): the keyboard
	 * is the desktop's, and so is the language (T1-102: Alt+Space on the
	 * desktop then chooses the desktop's, which the next new application
	 * inherits).
	 */
	ime->focus_key[0] = '\0';
	wanted = ime->language;
	why = "kept";
	if (ime->desktop_known) {
		wanted = ime->desktop_language;
		why = "desktop";
	}

	/* The log line the tests read. */
	printf("KWL IME app key=desktop language=%s from=%s\n", wanted, why);

	/* Chosen only when it differs from the input method's now. */
	differs = strcmp(wanted, ime->language);
	if (differs == 0 || ime->status == NULL)
		return;
	ime_select(server, wanted);
}

/*
 * Gives the text input to serve: the compositor's own text field with the
 * keyboard (its state read again), else the application's
 * (text-input.c).
 */
static struct kwl_text_input *
ime_current(
	struct kwl_server *server)
{
	struct kwl_text_input *input;
	struct kwl_object *surface;
	int known;

	/* App Home's search, while Home is open (ws090-p022). */
	known = kwl_home_field_state(server, ime_field_text, sizeof(ime_field_text), &ime_field.cursor, &ime_field.anchor, ime_field.rectangle);
	if (known) {
		ime_field_kind = IME_FIELD_HOME;
		ime_field.surface = NULL;
		ime_field.text = ime_field_text;
		ime_field.enabled = 1;
		ime_field.cause = 0;
		ime_field.hint = 0;
		ime_field.purpose = 0;
		return &ime_field;
	}

	/* The compositor's own field, when one has the keyboard. */
	surface = kwl_titlebar_field_surface(server);
	if (surface != NULL) {
		known = kwl_titlebar_field_state(server, ime_field_text, sizeof(ime_field_text), &ime_field.cursor, &ime_field.anchor, ime_field.rectangle);
		if (known) {
			ime_field_kind = IME_FIELD_TITLEBAR;
			ime_field.surface = surface;
			ime_field.text = ime_field_text;
			ime_field.enabled = 1;
			ime_field.cause = 0;
			ime_field.hint = 0;
			ime_field.purpose = 0;
			return &ime_field;
		}
	}

	/* Otherwise an application's. */
	input = kwl_text_input_current(server);

	/* Succeeded: the one, or none. */
	return input;
}

/*
 * Delivers what the input method made: to the compositor's own field
 * (titlebar-shell.c), or to an application's text input (text-input.c).
 */
static void
ime_deliver(
	struct kwl_server *server,
	struct kwl_text_input *input,
	const char *preedit,
	int32_t begin,
	int32_t end,
	const char *commit,
	uint32_t before,
	uint32_t after)
{
	/* App Home's search (ws090-p022). */
	if (input == &ime_field && ime_field_kind == IME_FIELD_HOME) {
		kwl_home_field_input(server, preedit, commit, before);
		return;
	}

	/* The compositor's own field. */
	if (input == &ime_field) {
		kwl_titlebar_field_input(server, preedit, commit, before, after);
		return;
	}

	/* An application's text input. */
	kwl_text_input_deliver(input, preedit, begin, end, commit, before, after);
}
