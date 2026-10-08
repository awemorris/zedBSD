/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The login screen (ws035-p095, plan/ws035/login-manager-design.md).
 *
 * The compositor --greeter draws it in place of the desktop and opens no Wayland
 * socket.  sessiond starts it as the unprivileged _greeter account, with
 * the display and the input devices given to that account, and answers on
 * the descriptor --auth-fd names:
 *
 *   READY                   GO: the display may be taken (handoff.c)
 *   STYLES name             STYLES password[ pin]: how the user may log in now
 *   AUTH name style         then the secret's line: OK, the user is in; FAIL reason
 *   POWER poweroff|reboot   OK (sent by libkeiland-backend's power, ws131-p005)
 *
 * After OK the screen says "Starting session..." and takes no input until
 * sessiond closes the descriptor, once the session is ready to take the
 * display (ws035-p101); the compositor then ends.  libkeiland-backend speaks
 * these lines (ws131-p006): this screen asks through
 * kl_backend_session_authenticate, kl_backend_session_unlock and
 * kl_backend_power_action, and the answers come back through handoff.c as
 * kwl_greeter_answer.
 *
 * The same screen is a session's lock (ws035-p102, kwl_lock): the session's
 * user only, no power buttons, and the secret goes to sessiond on the
 * session's descriptor (--control-fd) as UNLOCK style; OK unlocks, FAIL
 * (after sessiond's delay) asks again.
 *
 * The lock screen shows the clock and "Swipe up to unlock" first, without
 * the card (ws187-p002, lock-swipe.c): a swipe up from the lower part of
 * the output (a finger, or the pointer dragged), two fingers up on a touch
 * pad, or the wheel turned up opens a lock the session made itself (the
 * lid, sleep) within LOCK_GRACE_SECONDS of locking, and otherwise brings
 * the card; so does a key, which goes into the field.  The card goes
 * again after GREETER_CARD_IDLE_MS with nothing typed.  On the lock screen
 * the card offers its styles side by side under the field (ws187-p003):
 * Password, PIN and Security Key, each only when sessiond says the user
 * has it now; a press, or Tab, chooses one.
 *
 * The PIN (ws172-p002, docs/architecture/security.md "Login
 * authentication"): sessiond says which styles the user may use now
 * (STYLES); when the PIN is one the field takes the PIN first, and a link
 * under it switches between the PIN and the password.  Nothing is checked
 * here: /sbin/passkey checks both, and sessiond offers the PIN only after
 * the user's password since it started and turns it off after five wrong
 * ones.
 *
 * The screen is the blurred wallpaper with the time, large, and the date
 * above the middle (ws187-p001, lock-clock.c: sized by the output's
 * shorter side, clear of the card, on a portrait output too), a frosted
 * card in the middle with the users (the accounts with a uid
 * of 1000 or more and a login shell, or root when there are none), the
 * selected user's password field and the Log In button, and Restart and
 * Shut Down at the bottom right.  The password is shown as dots, sent to
 * sessiond and erased at once; it is never logged.
 *
 * Keys: the characters type into the password (US layout, Shift and Caps
 * Lock), Backspace erases, Esc clears, Enter logs in, Up, Down and Tab
 * choose the user.
 */

#include "language.h"
#include "glass.h"
#include "lock-clock.h"
#include "lock-swipe.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The users shown, at most, and the longest name and password. */
#define GREETER_USERS		8U
#define GREETER_NAME		32U
#define GREETER_PASSWORD	128U

/* The first uid of a person's account, and the first of the system's high ones. */
#define GREETER_UID_FIRST	1000U
#define GREETER_UID_LAST	59999U

/* The styles the lock screen's card may offer side by side (ws187-p003): the password, the PIN and a security key. */
#define GREETER_STYLES		3U

/* The card: its width, its corner, a user row's height, and the password field's height. */
#define GREETER_CARD_WIDTH	380
#define GREETER_CARD_RADIUS	22.0f
#define GREETER_ROW		44
#define GREETER_FIELD		44
#define GREETER_AVATAR		72

/* The line under the message that switches between the PIN and the password: its height. */
#define GREETER_LINK		28

/* A PIN's digits. */
#define GREETER_PIN_DIGITS	6U

/* The fewest characters of a security key's own PIN (CTAP 2.1). */
#define GREETER_KEY_PIN_MIN	4U

/* The power buttons' size, and their gap to the output's edge. */
#define GREETER_BUTTON_WIDTH	112
#define GREETER_BUTTON_HEIGHT	36
#define GREETER_MARGIN		24

/* The longest wait for the "Shutting down..." picture before the power request goes (ws099-p009). */
#define GREETER_POWER_MS	1000U

/* The dots of the power screen's spinner, and the time one turn takes. */
#define GREETER_SPINNER_DOTS	8U
#define GREETER_SPINNER_MS	1200U

/*
 * The pale glow behind the clock: how far it reaches past the wider line
 * across and past the two lines down, how far its soft edge spreads, and
 * its corner (pixels).
 */
#define GREETER_GLOW_ACROSS	32
#define GREETER_GLOW_DOWN	16
#define GREETER_GLOW_SPREAD	70
#define GREETER_GLOW_RADIUS	52.0f

/*
 * How long after a lock the session made itself a swipe alone opens it
 * (ws187-p002; the default proposed to the user on 2026-10-08, five
 * minutes, on the wall clock).
 */
#define LOCK_GRACE_SECONDS	300

/* How long the lock screen's card stays with nothing typed and no input before only the clock shows again. */
#define GREETER_CARD_IDLE_MS	30000U

/* The lock screen's "Swipe up to unlock": its baseline above the output's foot, and its opacity at rest (pixels). */
#define GREETER_HINT_FOOT	64
#define GREETER_HINT_OPACITY	0.80f

/* The modifiers' evdev codes: a key that only modifies brings no card (left and right Ctrl, Shift, Alt, Super, Caps Lock). */
#define GREETER_KEY_LEFTCTRL	29U
#define GREETER_KEY_LEFTSHIFT	42U
#define GREETER_KEY_RIGHTSHIFT	54U
#define GREETER_KEY_LEFTALT	56U
#define GREETER_KEY_CAPSLOCK	58U
#define GREETER_KEY_RIGHTCTRL	97U
#define GREETER_KEY_RIGHTALT	100U
#define GREETER_KEY_LEFTMETA	125U
#define GREETER_KEY_RIGHTMETA	126U

/* The Kei mark's square at the bottom left, in pixels. */
#define GREETER_BRAND_MARK	48

/* The evdev codes of the keys the screen takes apart from the characters. */
#define GREETER_KEY_ESC		1U
#define GREETER_KEY_BACKSPACE	14U
#define GREETER_KEY_TAB		15U
#define GREETER_KEY_ENTER	28U
#define GREETER_KEY_KPENTER	96U
#define GREETER_KEY_UP		103U
#define GREETER_KEY_DOWN	108U

/* How many evdev codes the character tables cover (up to the space bar). */
#define GREETER_KEYS		58U

/* The depressed Shift and the locked Caps Lock in the seat's modifier masks. */
#define GREETER_SHIFT		0x1U
#define GREETER_CAPS		0x2U

/* The pointer's left button. */
#define GREETER_BUTTON_LEFT	0x110U

/*
 * What a press on the screen hit: nothing, a user's row, the password
 * field, the Log In button, the PIN-or-password link, one of the lock
 * screen's styles, Restart or Shut Down.
 */
enum greeter_hit {
	GREETER_HIT_NONE,
	GREETER_HIT_USER,
	GREETER_HIT_FIELD,
	GREETER_HIT_LOGIN,
	GREETER_HIT_SWITCH,
	GREETER_HIT_STYLE,
	GREETER_HIT_RESTART,
	GREETER_HIT_POWEROFF
};

/*
 * One user the screen offers: the account's name and the name shown.
 */
struct greeter_user {
	char name[GREETER_NAME];
	char shown[GREETER_NAME];
};

/*
 * Where the parts of the screen are this frame, in output pixels
 * (x, y, width, height), laid out again on every frame and press; on the
 * lock screen the styles offered side by side in the link's line, with
 * their KL_BACKEND_STYLE_* and how many (none while only the password is).
 */
struct greeter_layout {
	int32_t card[4];
	int32_t avatar[4];
	int32_t rows[GREETER_USERS][4];
	int32_t field[4];
	int32_t login[4];
	int32_t link[4];
	int32_t styles[GREETER_STYLES][4];
	unsigned style_bits[GREETER_STYLES];
	unsigned style_count;
	int32_t restart[4];
	int32_t poweroff[4];
};

/*
 * The users read once at the start, the one selected, what has been typed
 * (erased as soon as it is sent), the line under the field (in the
 * language of when it was set, WS158), whether an answer is awaited.  The compositor runs one
 * greeter, so these live for the process.
 */
static struct greeter_user greeter_users[GREETER_USERS];
static unsigned greeter_user_count;
static unsigned greeter_selected;
static char greeter_password[GREETER_PASSWORD];
static unsigned greeter_password_length;
static char greeter_message[128];
static unsigned greeter_waiting;
static unsigned greeter_starting;

/*
 * Shut Down or Restart pressed (ws099-p009): the power request waiting to
 * go ("poweroff" or "reboot", empty when none), whether it has gone, and the
 * frame count and time when it was pressed.  The screen says "Shutting
 * down..." first, and the request goes to sessiond once that picture has
 * been shown (the frame count has moved on twice, or GREETER_POWER_MS has
 * passed), so the picture the display keeps at the end is that one.
 */
static char greeter_powering[16];
static unsigned greeter_power_sent;
static uint64_t greeter_power_frame;
static uint64_t greeter_power_ms;

/*
 * The styles (ws172-p002): those sessiond last said the selected user may
 * use now (KL_BACKEND_STYLE_* bits), the one the field takes, whether the
 * user chose it with the link, and whether STYLES is to be asked (wanted:
 * it waits for the answer under way) or is asked.
 */
static unsigned greeter_styles;
static unsigned greeter_style;
static unsigned greeter_style_chosen;
static unsigned greeter_styles_wanted;
static unsigned greeter_styles_asked;

/* Enter pressed while the session manager answered something else: the secret goes once it is free. */
static unsigned greeter_submit_pending;

/* A security key waits to be touched for the attempt under way (sessiond's TOUCH, ws172-p003). */
static unsigned greeter_touch;

/*
 * The lock screen's moves and grace (ws187-p002): whether the lock was the
 * user's choice (it always asks for the secret, kwl_lock_reason_manual),
 * when it locked on the wall clock, whether the card with the field shows
 * (a swipe outside the grace, or a key, brought it; 0 shows the clock and
 * the hint only), and the moves being followed.  Set by kwl_lock, cleared
 * by kwl_lock_release.
 */
static unsigned greeter_lock_manual;
static int64_t greeter_lock_at;
static unsigned greeter_lock_card;
static struct kwl_lock_swipe greeter_swipe;

/* The characters each key types, without and with Shift (US layout); 0 for none. */
static const char greeter_plain[GREETER_KEYS] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};
static const char greeter_shifted[GREETER_KEYS] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' '
};

static void greeter_read_users(void);
static void greeter_add_user(const char *name, const char *gecos);
static void greeter_layout(struct kwl_server *server, struct greeter_layout *layout);
static enum greeter_hit greeter_hit(struct kwl_server *server, const struct greeter_layout *layout, unsigned *user);
static int greeter_inside(const int32_t *rect, int32_t x, int32_t y);
static void greeter_draw_card(struct kwl_server *server, VkCommandBuffer command, const struct greeter_layout *layout);
static void greeter_draw_field(struct kwl_server *server, VkCommandBuffer command, const struct greeter_layout *layout);
static void greeter_draw_button(struct kwl_server *server, VkCommandBuffer command, const int32_t *rect, const char *label, int strong);
static void greeter_draw_clock(struct kwl_server *server, VkCommandBuffer command, const struct greeter_layout *layout);
static void greeter_draw_brand(struct kwl_server *server, VkCommandBuffer command);
static void greeter_draw_hint(struct kwl_server *server, VkCommandBuffer command);
static void greeter_lock_swiped(struct kwl_server *server, const char *via);
static void greeter_layout_styles(struct kwl_server *server, struct greeter_layout *layout);
static void greeter_draw_styles(struct kwl_server *server, VkCommandBuffer command, const struct greeter_layout *layout);
static void greeter_style_choose(struct kwl_server *server, unsigned style);
static int greeter_key_modifies(uint32_t key);
static void greeter_draw_centered(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t middle, int32_t baseline, const char *text, int32_t limit, const float *color);
static void greeter_select(struct kwl_server *server, unsigned user);
static void greeter_type(struct kwl_server *server, uint32_t key);
static void greeter_submit(struct kwl_server *server);
static void greeter_power(struct kwl_server *server, const char *what);
static void greeter_power_send(struct kwl_server *server);
static void greeter_draw_power(struct kwl_server *server, VkCommandBuffer command, const struct greeter_layout *layout);
static void greeter_answered(struct kwl_server *server, int error);
static void greeter_unlock(struct kwl_server *server);
static void greeter_refused(struct kwl_server *server);
static unsigned greeter_next_style(void);
static void greeter_key_refused(const char *reason);
static void greeter_styles_reset(void);
static void greeter_styles_ask(struct kwl_server *server);
static void greeter_styles_take(struct kwl_server *server);
static void greeter_style_switch(struct kwl_server *server);
static void greeter_erase(void);

/*
 * Prepares the login screen: the users, and the answers' descriptor.
 */
int
kwl_greeter_open(
	struct kwl_server *server)
{
	int flags;
	int error;

	/* The users shown; the first one's styles are asked once the screen runs. */
	greeter_read_users();
	greeter_selected = 0U;
	greeter_erase();
	greeter_message[0] = '\0';
	greeter_styles_reset();

	/* sessiond's answers are read without waiting for them. */
	flags = fcntl(server->auth_fd, F_GETFL);
	if (flags < 0) {
		printf("KWL GREETER auth-fd=%d errno=%d\n", server->auth_fd, errno);
		return EBADF;
	}

	/* The descriptor does not wait, and does not go to the programs the compositor starts. */
	error = fcntl(server->auth_fd, F_SETFL, flags | O_NONBLOCK);
	if (error != 0)
		return errno;
	(void)fcntl(server->auth_fd, F_SETFD, FD_CLOEXEC);

	/* Succeeded: the screen can be drawn. */
	printf("KWL GREETER open users=%u selected=%s\n", greeter_user_count, greeter_users[0].name);
	return 0;
}

/*
 * Locks a session (ws035-p102): the lock screen covers the desktop and
 * takes every key and button until the user's password unlocks it.
 * Returns 1 when locked, 0 when this compositor cannot be unlocked (no
 * sessiond to check the password) and so is not locked.
 */
int
kwl_lock(
	struct kwl_server *server,
	const char *reason)
{
	struct passwd *entry;
	int managed;

	/* Only a session a session manager started (it unlocks), and once. */
	if (server->greeter)
		return 0;
	managed = kl_backend_session_managed(server->backend);
	if (!managed)
		return 0;
	if (server->locked)
		return 1;

	/* The session's own user, and nothing typed. */
	greeter_user_count = 0U;
	entry = getpwuid(getuid());
	if (entry != NULL)
		greeter_add_user(entry->pw_name, entry->pw_gecos);
	if (greeter_user_count == 0U)
		greeter_add_user("?", NULL);
	greeter_selected = 0U;
	greeter_erase();
	greeter_message[0] = '\0';
	greeter_waiting = 0U;
	greeter_starting = 0U;

	/* Whether the user's PIN can unlock it: sessiond is asked. */
	greeter_styles_reset();

	/* Whether the user chose the lock, and when it locked: the grace a swipe alone has (ws187-p002); only the clock shows. */
	greeter_lock_manual = (unsigned)kwl_lock_reason_manual(reason);
	greeter_lock_at = (int64_t)time(NULL);
	greeter_lock_card = 0U;
	kwl_lock_swipe_reset(&greeter_swipe);

	/* The clipboard's history goes (clipboard.c). */
	kwl_clipboard_history_clear(server, "lock");

	/* A swap of arranged windows being dragged is given up (arrange-shell.c, ws177-p036). */
	kwl_arrange_swap_cancel(server, "lock");

	/* Succeeded: the lock screen shows. */
	server->locked = 1U;
	server->dirty = 1;
	printf("KWL LOCK locked reason=%s user=%s manual=%u\n", reason, greeter_users[0].name, greeter_lock_manual);
	return 1;
}

/*
 * Unlocks the session without the password (the lid opened soon after it
 * locked it, ws132-p008): what was typed is erased and the desktop shows.
 */
void
kwl_lock_release(
	struct kwl_server *server,
	const char *reason)
{
	/* Only a locked session. */
	if (!server->locked)
		return;

	/* Nothing typed stays, and an answer still on its way acts on nothing. */
	greeter_erase();
	greeter_message[0] = '\0';
	greeter_waiting = 0U;
	greeter_submit_pending = 0U;

	/* The next lock starts with the clock alone and no move followed. */
	greeter_lock_card = 0U;
	kwl_lock_swipe_reset(&greeter_swipe);

	/* Succeeded: the desktop shows; the idle time starts again. */
	server->locked = 0U;
	server->lock_input_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL LOCK unlocked reason=%s\n", reason);
}

/*
 * Acts on an answer of the session manager's to the login screen's or the
 * lock screen's request (libkeiland-backend reads it; handoff.c passes it
 * on).  request is the KL_BACKEND_SESSION_* it answers.
 */
void
kwl_greeter_answer(
	struct kwl_server *server,
	unsigned request,
	int error)
{
	/*
	 * The styles the user may use now: no longer asked, whatever shows;
	 * taken while the screen shows and no newer asking waits (another user
	 * was selected meanwhile).
	 */
	if (request == KL_BACKEND_SESSION_STYLES) {
		greeter_styles_asked = 0U;
		if (error != 0 || greeter_styles_wanted)
			return;
		if (server->greeter || server->locked)
			greeter_styles_take(server);
		return;
	}

	/* Only while the login screen or the lock screen shows. */
	if (!server->greeter && !server->locked)
		return;

	/* A security key waits to be touched: said until the answer (ws172-p003). */
	if (request == KL_BACKEND_SESSION_TOUCH) {
		if (greeter_waiting) {
			greeter_touch = 1U;
			server->dirty = 1;
			printf("KWL GREETER touch\n");
		}

		/* Not an answer: the attempt still waits. */
		return;
	}

	/* The same answers for both. */
	greeter_answered(server, error);
}

/*
 * Draws the login screen over the whole output.
 */
void
kwl_greeter_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct greeter_layout layout;
	struct glass_shape shape;

	/* Where everything goes. */
	greeter_layout(server, &layout);

	/*
	 * The wallpaper, blurred and washed towards a pale sky so the screen is
	 * as bright and airy as the boot screen; the words on it are dark slate
	 * (ws035-p109).
	 */
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_GLASS;
	shape.color[0] = 0.96f;
	shape.color[1] = 0.98f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.30f;
	shape.opaque = 1.0f;
	glass_shape_draw(server, command, &shape);

	/* The time and the date, above the card. */
	greeter_draw_clock(server, command, &layout);

	/* The Kei mark and word at the bottom left (ws035-p108). */
	greeter_draw_brand(server, command);

	/* Shutting down or restarting: only that, with a spinner, in the card. */
	if (greeter_powering[0] != '\0') {
		greeter_draw_power(server, command, &layout);
		return;
	}

	/* A lock screen before a swipe or a key: the hint instead of the card (ws187-p002). */
	if (server->locked && !greeter_lock_card) {
		greeter_draw_hint(server, command);
		return;
	}

	/* The card with the users, the password and Log In. */
	greeter_draw_card(server, command, &layout);

	/* The power buttons (not on a session's lock screen). */
	if (!server->locked) {
		greeter_draw_button(server, command, layout.restart, kl_tr("Restart"), 0);
		greeter_draw_button(server, command, layout.poweroff, kl_tr("Shut Down"), 0);
	}
}

/*
 * Handles a pointer button on the login screen: every button is the screen's.
 */
int
kwl_greeter_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct greeter_layout layout;
	enum greeter_hit hit;
	unsigned user;
	int swiped;

	/* A lock screen before its card: the left button's press and release may be a swipe up (ws187-p002). */
	if (server->locked && !greeter_lock_card) {
		if (button != GREETER_BUTTON_LEFT)
			return 1;

		/* A press is followed when it is in the lower part. */
		if (state != 0U) {
			(void)kwl_lock_swipe_press(&greeter_swipe, server->pointer_x, server->pointer_y, (int32_t)server->height);
			server->dirty = 1;
			return 1;
		}

		/* The release says whether it swiped. */
		swiped = kwl_lock_swipe_release(&greeter_swipe, (int32_t)server->height);
		server->dirty = 1;
		if (swiped)
			greeter_lock_swiped(server, "pointer");
		return 1;
	}

	/* Only the left button's press does anything, and nothing once the session is starting or the machine ending. */
	if (button != GREETER_BUTTON_LEFT || state == 0U || greeter_starting || greeter_powering[0] != '\0')
		return 1;

	/* What the press is on. */
	greeter_layout(server, &layout);
	user = 0U;
	hit = greeter_hit(server, &layout, &user);

	/* Acts on it. */
	switch (hit) {
	case GREETER_HIT_USER:
		greeter_select(server, user);
		break;
	case GREETER_HIT_LOGIN:
		greeter_submit(server);
		break;
	case GREETER_HIT_SWITCH:
		greeter_style_switch(server);
		break;
	case GREETER_HIT_STYLE:
		greeter_style_choose(server, layout.style_bits[user]);
		break;
	case GREETER_HIT_RESTART:
		if (!server->locked)
			greeter_power(server, "reboot");
		break;
	case GREETER_HIT_POWEROFF:
		if (!server->locked)
			greeter_power(server, "poweroff");
		break;
	default:
		break;
	}

	/* The press was the screen's. */
	server->dirty = 1;
	return 1;
}

/*
 * Handles a key on the login screen: every key is the screen's.
 */
int
kwl_greeter_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	int error;
	int held;

	int modifies;

	/* Releases do nothing, and nothing does once the session is starting or the machine ending. */
	if (state == 0U || greeter_starting || greeter_powering[0] != '\0')
		return 1;

	/* A key around a sleep is not the user's (the one that woke the machine, or typed unseen, sleep.c). */
	held = kwl_sleep_keys_held_now(server);
	if (held) {
		printf("KWL SLEEP ignore key\n");
		return 1;
	}

	/*
	 * A lock screen before its card: a key that does more than modify
	 * brings the card and goes on into it, so a keyboard alone can start
	 * typing (ws187-p002); a key never opens the lock without the secret.
	 */
	if (server->locked && !greeter_lock_card) {
		modifies = greeter_key_modifies(key);
		if (modifies)
			return 1;
		greeter_lock_card = 1U;
		kwl_lock_swipe_reset(&greeter_swipe);
		printf("KWL LOCK card via=key\n");
	}

	/* Routes the key by its code. */
	server->dirty = 1;
	switch (key) {
	case GREETER_KEY_ENTER:
	case GREETER_KEY_KPENTER:
		/* Enter logs in. */
		greeter_submit(server);
		return 1;
	case GREETER_KEY_BACKSPACE:
		/* Backspace erases the last character. */
		if (greeter_password_length > 0U) {
			greeter_password_length--;
			greeter_password[greeter_password_length] = '\0';
		}

		/* The key was the screen's. */
		return 1;
	case GREETER_KEY_ESC:
		/* Esc stops a security key's attempt (ws172-p003), or clears the password. */
		if (greeter_waiting && greeter_style == KL_BACKEND_STYLE_KEY) {
			error = kl_backend_session_cancel(server->backend);
			printf("KWL GREETER cancel error=%d\n", error);
			return 1;
		}

		/* Otherwise what was typed goes. */
		greeter_erase();
		return 1;
	case GREETER_KEY_UP:
		/* The user above. */
		if (greeter_selected > 0U)
			greeter_select(server, greeter_selected - 1U);
		return 1;
	case GREETER_KEY_TAB:
		/* On the lock screen (one user) Tab moves to the next style offered (ws187-p003). */
		if (server->locked) {
			greeter_style_switch(server);
			return 1;
		}

		/* On the login screen the user below, from the last back to the first. */
		greeter_select(server, (greeter_selected + 1U) % greeter_user_count);
		return 1;
	case GREETER_KEY_DOWN:
		/* The user below, from the last back to the first. */
		greeter_select(server, (greeter_selected + 1U) % greeter_user_count);
		return 1;
	default:
		break;
	}

	/* Any other key may type a character. */
	greeter_type(server, key);
	return 1;
}

/*
 * Follows the pointer pressed on a lock screen before its card: how far a
 * swipe has gone up (ws187-p002; the hint follows it).
 */
void
kwl_greeter_motion(
	struct kwl_server *server)
{
	/* Only a lock screen before its card follows a press. */
	if (!server->locked || greeter_lock_card)
		return;

	/* Nothing pressed in the lower part. */
	if (!greeter_swipe.pressing)
		return;

	/* The swipe so far, drawn at the next frame. */
	kwl_lock_swipe_motion(&greeter_swipe, server->pointer_x, server->pointer_y);
	server->dirty = 1;
}

/*
 * Takes the wheel's notches (positive down) on a lock screen before its
 * card: turned up two notches in a row, it is a swipe (ws187-p002).
 */
void
kwl_greeter_wheel(
	struct kwl_server *server,
	int32_t vertical)
{
	int swiped;

	/* Only a lock screen before its card. */
	if (!server->locked || greeter_lock_card)
		return;

	/* The notches, counted. */
	swiped = kwl_lock_swipe_wheel(&greeter_swipe, vertical, kwl_milliseconds());
	if (swiped)
		greeter_lock_swiped(server, "wheel");
}

/*
 * Takes two fingers' travel on a touch pad (micrometres, positive down,
 * the fingers' own way) on a lock screen before its card: far enough up,
 * it is a swipe (ws187-p002).
 */
void
kwl_greeter_pad_scroll(
	struct kwl_server *server,
	int64_t down_um)
{
	int swiped;

	/* Only a lock screen before its card. */
	if (!server->locked || greeter_lock_card)
		return;

	/* The travel, counted. */
	swiped = kwl_lock_swipe_pad(&greeter_swipe, down_um);
	if (swiped)
		greeter_lock_swiped(server, "pad");
}

/*
 * Takes the end of a touch on a touch pad on the lock screen: a gesture up
 * (two fingers from the bottom edge, three fingers up) with its travel
 * along its way, or 0 for any other end; far enough, it is a swipe
 * (ws187-p002).  The next touch counts afresh.
 */
void
kwl_greeter_pad_end(
	struct kwl_server *server,
	int64_t gesture_up_um)
{
	int swiped;

	/* Only a lock screen. */
	if (!server->locked)
		return;

	/* A gesture up far enough, before the card, is a swipe. */
	swiped = 0;
	if (!greeter_lock_card && gesture_up_um > 0)
		swiped = kwl_lock_swipe_pad_gesture(&greeter_swipe, gesture_up_um);

	/* The touch has ended either way. */
	kwl_lock_swipe_pad_end(&greeter_swipe);
	if (swiped)
		greeter_lock_swiped(server, "pad");
}

/*
 * Tells whether the login screen has logged in and waits for the session
 * to take the display (it shows "Starting session..." until it ends).
 */
int
kwl_greeter_starting(void)
{
	/* Starting once the login was accepted. */
	if (greeter_starting)
		return 1;

	/* Succeeded: not starting. */
	return 0;
}

/*
 * Shows a line under the password field of the login or lock screen (a
 * sleep's reason, ws052-p012), cut at a character's end to fit.
 */
void
kwl_greeter_say(
	struct kwl_server *server,
	const char *text)
{
	size_t length;

	/* The text, cut to the line's room without splitting a UTF-8 character. */
	length = strlen(text);
	if (length >= sizeof(greeter_message)) {
		length = sizeof(greeter_message) - 1U;
		while (length > 0U && ((unsigned char)text[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The line holds the text. */
	memcpy(greeter_message, text, length);
	greeter_message[length] = '\0';

	/* Drawn at the next frame. */
	server->dirty = 1;
}

/*
 * Redraws when the minute changes, and sends a power request once its
 * picture is shown (the answers come through kwl_greeter_answer).
 */
void
kwl_greeter_tick(
	struct kwl_server *server)
{
	uint64_t now_ms;
	int64_t minute;

	/* The clock shows a new minute. */
	minute = (int64_t)(time(NULL) / 60);
	if (minute != server->clock_minute) {
		server->clock_minute = minute;
		server->dirty = 1;
	}

	/*
	 * The styles of the selected user, asked once no other answer is
	 * awaited and the display has been handed over (sessiond reads a
	 * session's lines before its READY as nothing, and an answer to the
	 * login screen's would come during the wait for GO).
	 */
	if (greeter_styles_wanted && !greeter_waiting && !greeter_styles_asked && server->handed_over)
		greeter_styles_ask(server);

	/* A secret held back while another request was answered goes now. */
	if (greeter_submit_pending && !greeter_styles_asked && !greeter_styles_wanted)
		greeter_submit(server);

	/* The lock screen's card left alone with nothing typed goes, and the clock shows alone again (ws187-p002). */
	if (server->locked &&
	    greeter_lock_card &&
	    greeter_password_length == 0U &&
	    !greeter_waiting) {
		now_ms = kwl_milliseconds();
		if (now_ms - server->lock_input_ms >= GREETER_CARD_IDLE_MS) {
			greeter_lock_card = 0U;
			server->dirty = 1;
			printf("KWL LOCK card hidden\n");
		}
	}

	/* The lock screen has only its clock and its styles. */
	if (!server->greeter)
		return;

	/* Shutting down: the spinner turns, and the request goes once its picture is shown. */
	if (greeter_powering[0] != '\0') {
		server->dirty = 1;
		greeter_power_send(server);
	}
}

/* Reads the users offered: the people's accounts, or root when there are none. */
static void
greeter_read_users(
	void)
{
	struct passwd *entry;
	const char *shell;
	size_t length;
	int nologin;
	int match;

	/* Each account with a person's uid and a shell to log in to. */
	greeter_user_count = 0U;
	setpwent();
	for (;;) {
		entry = getpwent();
		if (entry == NULL)
			break;
		if (entry->pw_uid < GREETER_UID_FIRST || entry->pw_uid > GREETER_UID_LAST)
			continue;

		/* An account whose shell is nologin or false cannot log in. */
		shell = entry->pw_shell;
		length = strlen(shell);
		nologin = 0;
		if (length >= 7U) {
			match = strcmp(shell + length - 7U, "nologin");
			if (match == 0)
				nologin = 1;
		}

		/* The same for false. */
		if (length >= 5U) {
			match = strcmp(shell + length - 5U, "false");
			if (match == 0)
				nologin = 1;
		}

		/* Such an account is not offered. */
		if (nologin)
			continue;

		/* A person who can log in. */
		greeter_add_user(entry->pw_name, entry->pw_gecos);
	}

	/* The accounts are read. */
	endpwent();

	/* A machine without a person's account offers root. */
	if (greeter_user_count == 0U)
		greeter_add_user("root", "System Administrator");
}

/* Adds one user, shown by the first part of its GECOS field when it has one. */
static void
greeter_add_user(
	const char *name,
	const char *gecos)
{
	struct greeter_user *user;
	char *comma;

	/* The list holds what fits. */
	if (greeter_user_count >= GREETER_USERS)
		return;

	/* The name, and the name shown. */
	user = &greeter_users[greeter_user_count];
	snprintf(user->name, sizeof(user->name), "%s", name);
	snprintf(user->shown, sizeof(user->shown), "%s", name);
	if (gecos != NULL && gecos[0] != '\0' && gecos[0] != ',') {
		snprintf(user->shown, sizeof(user->shown), "%s", gecos);
		comma = strchr(user->shown, ',');
		if (comma != NULL)
			*comma = '\0';
	}

	/* One more in the list. */
	greeter_user_count++;
}

/* Lays the screen out for the output's size and the number of users. */
static void
greeter_layout(
	struct kwl_server *server,
	struct greeter_layout *layout)
{
	int32_t width;
	int32_t height;
	int32_t card_height;
	int32_t x;
	int32_t y;
	unsigned index;
	unsigned rows;

	/* The card: the avatar, the name, the other users' rows, the field and a line for messages. */
	width = (int32_t)server->width;
	height = (int32_t)server->height;
	rows = 0U;
	if (greeter_user_count > 1U)
		rows = greeter_user_count;
	card_height = 28 + GREETER_AVATAR + 48 + (int32_t)rows * GREETER_ROW + 12 + GREETER_FIELD + 44 + GREETER_LINK;
	layout->card[2] = GREETER_CARD_WIDTH;
	layout->card[3] = card_height;
	layout->card[0] = (width - GREETER_CARD_WIDTH) / 2;
	layout->card[1] = height / 2 - card_height / 2 + height / 16;
	x = layout->card[0];
	y = layout->card[1] + 28;

	/* The selected user's avatar, centred. */
	layout->avatar[0] = x + (GREETER_CARD_WIDTH - GREETER_AVATAR) / 2;
	layout->avatar[1] = y;
	layout->avatar[2] = GREETER_AVATAR;
	layout->avatar[3] = GREETER_AVATAR;
	y += GREETER_AVATAR + 48;

	/* One row a user when there is more than one. */
	for (index = 0U; index < GREETER_USERS; index++) {
		layout->rows[index][0] = x + 24;
		layout->rows[index][1] = y + (int32_t)index * GREETER_ROW;
		layout->rows[index][2] = GREETER_CARD_WIDTH - 48;
		layout->rows[index][3] = GREETER_ROW - 4;
	}

	/* Below the rows. */
	y += (int32_t)rows * GREETER_ROW + 12;

	/* The password field, with Log In at its right. */
	layout->field[0] = x + 24;
	layout->field[1] = y;
	layout->field[2] = GREETER_CARD_WIDTH - 48 - GREETER_FIELD - 8;
	layout->field[3] = GREETER_FIELD;
	layout->login[0] = layout->field[0] + layout->field[2] + 8;
	layout->login[1] = y;
	layout->login[2] = GREETER_FIELD;
	layout->login[3] = GREETER_FIELD;

	/* The link that switches between the PIN and the password, under the message's line. */
	layout->link[0] = x + 24;
	layout->link[1] = y + GREETER_FIELD + 40;
	layout->link[2] = GREETER_CARD_WIDTH - 48;
	layout->link[3] = GREETER_LINK;

	/* The lock screen's styles in the link's line. */
	greeter_layout_styles(server, layout);

	/* Restart and Shut Down at the bottom right. */
	layout->poweroff[0] = width - GREETER_MARGIN - GREETER_BUTTON_WIDTH;
	layout->poweroff[1] = height - GREETER_MARGIN - GREETER_BUTTON_HEIGHT;
	layout->poweroff[2] = GREETER_BUTTON_WIDTH;
	layout->poweroff[3] = GREETER_BUTTON_HEIGHT;
	layout->restart[0] = layout->poweroff[0] - 12 - GREETER_BUTTON_WIDTH;
	layout->restart[1] = layout->poweroff[1];
	layout->restart[2] = GREETER_BUTTON_WIDTH;
	layout->restart[3] = GREETER_BUTTON_HEIGHT;
}

/* Says what the pointer is on, and which user's row when it is one. */
static enum greeter_hit
greeter_hit(
	struct kwl_server *server,
	const struct greeter_layout *layout,
	unsigned *user)
{
	unsigned index;
	int inside;

	/* A user's row, when rows are shown. */
	for (index = 0U; greeter_user_count > 1U && index < greeter_user_count; index++) {
		inside = greeter_inside(layout->rows[index], server->pointer_x, server->pointer_y);
		if (inside) {
			*user = index;
			return GREETER_HIT_USER;
		}
	}

	/* The field, Log In and the power buttons. */
	inside = greeter_inside(layout->field, server->pointer_x, server->pointer_y);
	if (inside)
		return GREETER_HIT_FIELD;
	inside = greeter_inside(layout->login, server->pointer_x, server->pointer_y);
	if (inside)
		return GREETER_HIT_LOGIN;

	/* One of the lock screen's styles, where the login screen has its link (ws187-p003). */
	for (index = 0U; index < layout->style_count; index++) {
		inside = greeter_inside(layout->styles[index], server->pointer_x, server->pointer_y);
		if (inside) {
			*user = index;
			return GREETER_HIT_STYLE;
		}
	}

	/* A lock screen that offers its styles has no link, and no power buttons. */
	if (layout->style_count != 0U)
		return GREETER_HIT_NONE;

	/* The login screen's link, and the power buttons. */
	inside = greeter_inside(layout->link, server->pointer_x, server->pointer_y);
	if (inside && (greeter_styles & (KL_BACKEND_STYLE_PIN | KL_BACKEND_STYLE_KEY)) != 0U)
		return GREETER_HIT_SWITCH;
	inside = greeter_inside(layout->restart, server->pointer_x, server->pointer_y);
	if (inside)
		return GREETER_HIT_RESTART;
	inside = greeter_inside(layout->poweroff, server->pointer_x, server->pointer_y);
	if (inside)
		return GREETER_HIT_POWEROFF;

	/* Nothing. */
	return GREETER_HIT_NONE;
}

/* Reports whether a point is inside a rectangle (x, y, width, height). */
static int
greeter_inside(
	const int32_t *rect,
	int32_t x,
	int32_t y)
{
	/* Left of it or above it. */
	if (x < rect[0] || y < rect[1])
		return 0;

	/* Right of it or below it. */
	if (x >= rect[0] + rect[2] || y >= rect[1] + rect[3])
		return 0;

	/* Succeeded: the point is inside. */
	return 1;
}

/* Draws the frosted card: the avatar and name, the users' rows, the field and the message. */
static void
greeter_draw_card(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct greeter_layout *layout)
{
	static const float ink[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	static const float faint[4] = { 0.10f, 0.14f, 0.22f, 0.62f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float avatar[4] = { 0.26f, 0.48f, 0.86f, 1.0f };
	static const float chosen[4] = { 1.0f, 1.0f, 1.0f, 0.55f };
	static const float hover[4] = { 1.0f, 1.0f, 1.0f, 0.28f };
	static const float warning[4] = { 0.72f, 0.12f, 0.10f, 1.0f };
	struct glass_shape shape;
	const struct greeter_user *user;
	const float *color;
	const char *link;
	unsigned next;
	char letter[2];
	int32_t middle;
	int32_t baseline;
	unsigned index;
	int inside;

	/* The card's shadow. */
	glass_shape_init(&shape, (float)layout->card[0], (float)layout->card[1] + 10.0f, (float)layout->card[2], (float)layout->card[3]);
	shape.quad[0] -= 60.0f;
	shape.quad[1] -= 60.0f;
	shape.quad[2] += 120.0f;
	shape.quad[3] += 120.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = GREETER_CARD_RADIUS;
	shape.soft = 30.0f;
	shape.color[0] = 0.12f;
	shape.color[1] = 0.20f;
	shape.color[2] = 0.34f;
	shape.color[3] = 0.16f;
	glass_shape_draw(server, command, &shape);

	/* The frosted glass, whiter than the windows' so the dark text reads on it. */
	glass_shape_init(&shape, (float)layout->card[0], (float)layout->card[1], (float)layout->card[2], (float)layout->card[3]);
	shape.mode = MODE_GLASS;
	shape.radius = GREETER_CARD_RADIUS;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.62f;
	shape.edge = 0.70f;
	glass_shape_draw(server, command, &shape);

	/* The selected user's avatar: a circle with the first letter of the name. */
	user = &greeter_users[greeter_selected];
	glass_draw_solid(server, command, (float)layout->avatar[0], (float)layout->avatar[1], (float)GREETER_AVATAR, (float)GREETER_AVATAR, (float)GREETER_AVATAR / 2.0f, avatar);
	letter[0] = user->shown[0];
	if (letter[0] >= 'a' && letter[0] <= 'z')
		letter[0] = (char)(letter[0] - 'a' + 'A');
	letter[1] = '\0';
	middle = layout->avatar[0] + GREETER_AVATAR / 2;
	greeter_draw_centered(server, command, SIZE_ICON, middle, layout->avatar[1] + GREETER_AVATAR / 2 + 13, letter, GREETER_AVATAR, white);

	/* The name under it. */
	baseline = layout->avatar[1] + GREETER_AVATAR + 32;
	greeter_draw_centered(server, command, SIZE_SEARCH, middle, baseline, user->shown, GREETER_CARD_WIDTH - 32, ink);

	/* The users' rows, the selected one lit, when there is more than one. */
	for (index = 0U; greeter_user_count > 1U && index < greeter_user_count; index++) {
		inside = greeter_inside(layout->rows[index], server->pointer_x, server->pointer_y);
		if (index == greeter_selected) {
			glass_draw_solid(server, command, (float)layout->rows[index][0], (float)layout->rows[index][1], (float)layout->rows[index][2], (float)layout->rows[index][3], 10.0f, chosen);
		} else if (inside) {
			glass_draw_solid(server, command, (float)layout->rows[index][0], (float)layout->rows[index][1], (float)layout->rows[index][2], (float)layout->rows[index][3], 10.0f, hover);
		}

		/* The name shown, and the account's name at the right. */
		glass_draw_text(server, command, SIZE_TITLE, layout->rows[index][0] + 14, layout->rows[index][1] + 26, greeter_users[index].shown, layout->rows[index][2] - 28, ink);
		glass_draw_text(server, command, SIZE_BAR, layout->rows[index][0] + layout->rows[index][2] - 14 - glass_text_width(server, SIZE_BAR, greeter_users[index].name), layout->rows[index][1] + 26, greeter_users[index].name, 120, faint);
	}

	/* The password field and Log In. */
	greeter_draw_field(server, command, layout);

	/* The line under the field: a wrong password, or the wait for the answer. */
	baseline = layout->field[1] + GREETER_FIELD + 28;
	if (greeter_starting) {
		greeter_draw_centered(server, command, SIZE_TITLE, middle, baseline, kl_tr("Starting session..."), GREETER_CARD_WIDTH - 32, faint);
	} else if (greeter_waiting && greeter_touch) {
		greeter_draw_centered(server, command, SIZE_TITLE, middle, baseline, kl_tr("Touch your security key."), GREETER_CARD_WIDTH - 32, ink);
	} else if (greeter_waiting) {
		greeter_draw_centered(server, command, SIZE_TITLE, middle, baseline, kl_tr("Checking..."), GREETER_CARD_WIDTH - 32, faint);
	} else if (greeter_message[0] != '\0') {
		greeter_draw_centered(server, command, SIZE_TITLE, middle, baseline, greeter_message, GREETER_CARD_WIDTH - 32, warning);
	}

	/* The lock screen offers its styles side by side instead of the link (ws187-p003). */
	if (layout->style_count != 0U) {
		greeter_draw_styles(server, command, layout);
		return;
	}

	/* The link to the next style, while there is another than the password. */
	if ((greeter_styles & (KL_BACKEND_STYLE_PIN | KL_BACKEND_STYLE_KEY)) == 0U || greeter_starting)
		return;
	next = greeter_next_style();
	link = kl_tr("Use your password");
	if (next == KL_BACKEND_STYLE_PIN)
		link = kl_tr("Use your PIN");
	if (next == KL_BACKEND_STYLE_KEY)
		link = kl_tr("Use a security key");
	inside = greeter_inside(layout->link, server->pointer_x, server->pointer_y);
	color = faint;
	if (inside)
		color = ink;
	greeter_draw_centered(server, command, SIZE_BAR, middle, layout->link[1] + 20, link, GREETER_CARD_WIDTH - 32, color);
}

/* Draws the password field (dots for the characters, or its hint) and the Log In button. */
static void
greeter_draw_field(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct greeter_layout *layout)
{
	static const float field[4] = { 1.0f, 1.0f, 1.0f, 0.86f };
	static const float rim[4] = { 0.26f, 0.48f, 0.86f, 0.90f };
	static const float dot[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	static const float hint[4] = { 0.10f, 0.14f, 0.22f, 0.45f };
	const char *hint_text;
	float x;
	float y;
	unsigned index;
	unsigned shown;

	/* The field: a blue rim around a white box. */
	glass_draw_solid(server, command, (float)layout->field[0] - 2.0f, (float)layout->field[1] - 2.0f, (float)layout->field[2] + 4.0f, (float)layout->field[3] + 4.0f, 12.0f, rim);
	glass_draw_solid(server, command, (float)layout->field[0], (float)layout->field[1], (float)layout->field[2], (float)layout->field[3], 10.0f, field);

	/* The hint while it is empty: the style the field takes. */
	hint_text = kl_tr("Password");
	if (greeter_style == KL_BACKEND_STYLE_PIN)
		hint_text = kl_tr("PIN");
	if (greeter_style == KL_BACKEND_STYLE_KEY)
		hint_text = kl_tr("Security key PIN");
	if (greeter_password_length == 0U) {
		glass_draw_text(server, command, SIZE_TITLE, layout->field[0] + 16, layout->field[1] + 28, hint_text, layout->field[2] - 32, hint);
	}

	/* A dot a character, as many as fit. */
	shown = greeter_password_length;
	if (shown > (unsigned)((layout->field[2] - 32) / 16))
		shown = (unsigned)((layout->field[2] - 32) / 16);
	y = (float)layout->field[1] + (float)GREETER_FIELD / 2.0f - 5.0f;
	for (index = 0U; index < shown; index++) {
		x = (float)layout->field[0] + 16.0f + (float)index * 16.0f;
		glass_draw_solid(server, command, x, y, 10.0f, 10.0f, 5.0f, dot);
	}

	/* Log In: a right arrow (U+2192, from the glyph cache and its fallback font) on the blue button (ws035-p116). */
	greeter_draw_button(server, command, layout->login, "\xe2\x86\x92", 1);
}

/* Draws one button: frosted, or blue when it is the strong one; lit under the pointer. */
static void
greeter_draw_button(
	struct kwl_server *server,
	VkCommandBuffer command,
	const int32_t *rect,
	const char *label,
	int strong)
{
	static const float blue[4] = { 0.20f, 0.44f, 0.86f, 1.0f };
	static const float blue_lit[4] = { 0.28f, 0.54f, 0.95f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float slate[4] = { 0.17f, 0.23f, 0.31f, 1.0f };
	struct glass_shape shape;
	int inside;

	/* Whether the pointer is on it. */
	inside = greeter_inside(rect, server->pointer_x, server->pointer_y);

	/* The strong button is solid blue. */
	if (strong) {
		if (inside) {
			glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], 10.0f, blue_lit);
		} else {
			glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], 10.0f, blue);
		}

		/* Its label, white. */
		greeter_draw_centered(server, command, SIZE_SEARCH, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2 + 8, label, rect[2], white);
		return;
	}

	/* The others are light frosted glass with a slate label, whiter under the pointer. */
	glass_shape_init(&shape, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3]);
	shape.mode = MODE_GLASS;
	shape.radius = (float)rect[3] / 2.0f;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.55f;
	if (inside)
		shape.color[3] = 0.78f;
	shape.edge = 0.80f;
	glass_shape_draw(server, command, &shape);
	greeter_draw_centered(server, command, SIZE_TITLE, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2 + 5, label, rect[2] - 8, slate);
}

/*
 * Draws the Kei mark and the word Kei at the bottom left of the output, as
 * on the boot screen (the word in three letters, never a lone K).
 */
static void
greeter_draw_brand(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float slate[4] = { 0.17f, 0.23f, 0.31f, 0.95f };
	int32_t x;
	int32_t y;

	/* The mark's square, level with the power buttons' foot. */
	x = GREETER_MARGIN;
	y = (int32_t)server->height - GREETER_MARGIN - GREETER_BRAND_MARK;
	glass_draw_mark(server, command, x, y, GREETER_BRAND_MARK, GLASS_MARK_SPLASH, 1.0f);

	/* The word beside it, on the mark's lower part, in the boot screen's slate. */
	glass_draw_text(server, command, SIZE_ICON, x + GREETER_BRAND_MARK + 8, y + GREETER_BRAND_MARK - 12, "Kei", 200, slate);
}

/*
 * Draws the time, large, and the date under it, above the middle of the
 * output and clear of the card (ws187-p001, lock-clock.c).
 */
static void
greeter_draw_clock(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct greeter_layout *layout)
{
	static const float slate[4] = { 0.15f, 0.21f, 0.29f, 1.0f };
	static const float soft[4] = { 0.20f, 0.27f, 0.36f, 0.88f };
	struct kwl_lock_clock clock;
	struct glass_shape shape;
	char text[64];
	char date[64];
	struct tm local;
	time_t now;
	int32_t middle;
	int32_t time_width;
	int32_t date_width;
	int32_t glow_width;
	int large_error;

	/* The time and the date now. */
	now = time(NULL);
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&now, &local);
	(void)strftime(text, sizeof(text), "%H:%M", &local);
	kwl_language_date(&local, KWL_LANGUAGE_DATE_LONG, date, sizeof(date));

	/* Where the two lines go and how large the time is, for this output and this card. */
	middle = (int32_t)server->width / 2;
	kwl_lock_clock_layout((int32_t)server->width, (int32_t)server->height, layout->card[1], &clock);

	/* The time's digits at that size, or the atlas's clock size when they cannot be made. */
	large_error = glass_large_prepare(server, (unsigned)clock.pixels);
	time_width = glass_text_width(server, SIZE_CLOCK, text);
	if (large_error == 0)
		time_width = glass_large_text_width(server, text);
	date_width = glass_text_width(server, SIZE_SEARCH, date);

	/* The glow is as wide as the wider line. */
	glow_width = time_width;
	if (date_width > glow_width)
		glow_width = date_width;

	/*
	 * A soft pale glow behind the time and the date: a local scrim that
	 * keeps the slate words readable on a bright or busy wallpaper without
	 * darkening the screen (ws035-p109).
	 */
	glass_shape_init(
		&shape,
		(float)(middle - glow_width / 2 - GREETER_GLOW_ACROSS),
		(float)(clock.top - GREETER_GLOW_DOWN),
		(float)(glow_width + 2 * GREETER_GLOW_ACROSS),
		(float)(clock.bottom - clock.top + 2 * GREETER_GLOW_DOWN));
	shape.quad[0] -= (float)GREETER_GLOW_SPREAD;
	shape.quad[1] -= (float)GREETER_GLOW_SPREAD;
	shape.quad[2] += (float)(2 * GREETER_GLOW_SPREAD);
	shape.quad[3] += (float)(2 * GREETER_GLOW_SPREAD);
	shape.mode = MODE_SHADOW;
	shape.radius = GREETER_GLOW_RADIUS;
	shape.soft = 60.0f;
	shape.color[0] = 0.97f;
	shape.color[1] = 0.99f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.42f;
	glass_shape_draw(server, command, &shape);

	/* The time, centred on its baseline. */
	if (large_error == 0) {
		glass_draw_large_text(server, command, middle - time_width / 2, clock.time_baseline, text, slate);
	} else {
		greeter_draw_centered(server, command, SIZE_CLOCK, middle, clock.time_baseline, text, (int32_t)server->width, slate);
	}

	/* The date under it. */
	greeter_draw_centered(server, command, SIZE_SEARCH, middle, clock.date_baseline, date, (int32_t)server->width, soft);
}

/*
 * Draws the lock screen's "Swipe up to unlock" at the foot of the output
 * (ws187-p002): a press being swiped carries it up with the pointer, fading
 * as it nears the swipe's distance.
 */
static void
greeter_draw_hint(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	float color[4] = { 0.15f, 0.21f, 0.29f, GREETER_HINT_OPACITY };
	int32_t distance;
	int32_t rise;
	int32_t baseline;

	/* How far a press being swiped has gone up, at most the swipe's distance. */
	distance = kwl_lock_swipe_distance((int32_t)server->height);
	rise = 0;
	if (greeter_swipe.pressing && greeter_swipe.up > 0) {
		rise = greeter_swipe.up;
		if (rise > distance)
			rise = distance;
	}

	/* Carried up by the rise, and fading to a third of its opacity at the distance. */
	baseline = (int32_t)server->height - GREETER_HINT_FOOT - rise;
	color[3] = GREETER_HINT_OPACITY * (1.0f - 0.67f * (float)rise / (float)distance);

	/* The words, centred. */
	greeter_draw_centered(server, command, SIZE_SEARCH, (int32_t)server->width / 2, baseline, kl_tr("Swipe up to unlock"), (int32_t)server->width, color);
}

/*
 * Acts on a swipe on the lock screen (ws187-p002): a lock the session made
 * itself opens within its grace; otherwise the card asks for the secret.
 */
static void
greeter_lock_swiped(
	struct kwl_server *server,
	const char *via)
{
	int grace;

	/* Whether the lock is in its grace now. */
	grace = kwl_lock_grace(greeter_lock_manual, greeter_lock_at, (int64_t)time(NULL), LOCK_GRACE_SECONDS);
	printf("KWL LOCK swipe via=%s grace=%d manual=%u\n", via, grace, greeter_lock_manual);

	/* In its grace: open, without the secret. */
	if (grace) {
		kwl_lock_release(server, "swipe");
		return;
	}

	/* Otherwise the card, with the field taking the keys. */
	greeter_lock_card = 1U;
	kwl_lock_swipe_reset(&greeter_swipe);
	server->dirty = 1;
}

/* Tells whether a key only modifies the others (Ctrl, Shift, Alt, Super, Caps Lock). */
static int
greeter_key_modifies(
	uint32_t key)
{
	/* Each modifier's code. */
	switch (key) {
	case GREETER_KEY_LEFTCTRL:
	case GREETER_KEY_RIGHTCTRL:
	case GREETER_KEY_LEFTSHIFT:
	case GREETER_KEY_RIGHTSHIFT:
	case GREETER_KEY_LEFTALT:
	case GREETER_KEY_RIGHTALT:
	case GREETER_KEY_LEFTMETA:
	case GREETER_KEY_RIGHTMETA:
	case GREETER_KEY_CAPSLOCK:
		return 1;
	default:
		break;
	}

	/* Succeeded: a key that does more. */
	return 0;
}

/* Draws a line of text centred on a point of its baseline, no wider than limit. */
static void
greeter_draw_centered(
	struct kwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t middle,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	int32_t width;

	/* How wide it is, at most the limit (a longer text is cut in its middle). */
	width = glass_text_width(server, size, text);
	if (width > limit)
		width = limit;

	/* Half of it to the left of the middle. */
	glass_draw_text_middle(server, command, size, middle - width / 2, baseline, text, limit, color);
}

/* Selects a user, clearing what was typed for the one before. */
static void
greeter_select(
	struct kwl_server *server,
	unsigned user)
{
	/* A user that is not there. */
	if (user >= greeter_user_count)
		return;

	/* The new user starts with an empty password, and its styles are asked. */
	greeter_selected = user;
	greeter_erase();
	greeter_message[0] = '\0';
	greeter_styles_reset();
	server->dirty = 1;
	printf("KWL GREETER select user=%s\n", greeter_users[user].name);
}

/* Types a key's character into the password. */
static void
greeter_type(
	struct kwl_server *server,
	uint32_t key)
{
	char character;
	int shift;

	/* Keys beyond the table type nothing. */
	if (key >= GREETER_KEYS)
		return;

	/* Shift, and Caps Lock for the letters. */
	shift = 0;
	if ((server->modifiers & GREETER_SHIFT) != 0U)
		shift = 1;
	character = greeter_plain[key];
	if ((server->locked_modifiers & GREETER_CAPS) != 0U && character >= 'a' && character <= 'z')
		shift = !shift;
	if (shift)
		character = greeter_shifted[key];
	if (character == 0)
		return;

	/* While an answer is awaited, or the password is full, nothing is typed. */
	if (greeter_waiting || greeter_password_length + 1U >= sizeof(greeter_password))
		return;

	/* The PIN takes six digits only. */
	if (greeter_style == KL_BACKEND_STYLE_PIN) {
		if (character < '0' || character > '9')
			return;
		if (greeter_password_length >= GREETER_PIN_DIGITS)
			return;
	}

	/* The character. */
	greeter_password[greeter_password_length] = character;
	greeter_password_length++;
	greeter_password[greeter_password_length] = '\0';
	greeter_message[0] = '\0';
}

/* Sends the selected user's password to the session manager and erases it. */
static void
greeter_submit(
	struct kwl_server *server)
{
	int error;

	/* One question at a time; while the styles are asked the secret waits for their answer. */
	greeter_submit_pending = 0U;
	if (greeter_waiting)
		return;
	if (greeter_styles_asked || greeter_styles_wanted) {
		greeter_submit_pending = 1U;
		return;
	}

	/* A PIN is six digits; a shorter one is not sent (it would count as a wrong one). */
	if (greeter_style == KL_BACKEND_STYLE_PIN && greeter_password_length != GREETER_PIN_DIGITS) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("A PIN has six digits."));
		server->dirty = 1;
		return;
	}

	/* A security key's PIN has at least four characters (CTAP's least). */
	if (greeter_style == KL_BACKEND_STYLE_KEY && greeter_password_length < GREETER_KEY_PIN_MIN) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("A security key's PIN has at least four characters."));
		server->dirty = 1;
		return;
	}

	/* The request (unlock on a session's lock screen), through the backend. */
	greeter_waiting = 1;
	greeter_touch = 0U;
	greeter_message[0] = '\0';
	if (server->locked) {
		error = kl_backend_session_unlock(server->backend, greeter_style, greeter_password);
	} else {
		error = kl_backend_session_authenticate(server->backend, greeter_users[greeter_selected].name, greeter_style, greeter_password);
	}

	/* A session manager busy with another request: the secret stays typed and goes on a later tick. */
	if (error == EBUSY) {
		greeter_waiting = 0;
		greeter_submit_pending = 1U;
		return;
	}

	/* Nothing typed is kept once it is asked. */
	greeter_erase();
	if (error != 0) {
		printf("KWL GREETER send errno=%d\n", error);
		greeter_waiting = 0;
	}

	/* The screen shows the wait. */
	server->dirty = 1;
	printf("KWL GREETER auth user=%s style=%u\n", greeter_users[greeter_selected].name, greeter_style);
}

/*
 * Starts ending the machine (Shut Down or Restart): the screen says so from
 * the next frame, and greeter_power_send sends the request once that
 * picture has been shown.
 */
static void
greeter_power(
	struct kwl_server *server,
	const char *what)
{
	/* The request to send, and when it was asked for. */
	(void)snprintf(greeter_powering, sizeof(greeter_powering), "%s", what);
	greeter_power_sent = 0U;
	greeter_power_frame = server->frame;
	greeter_power_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL GREETER powering=%s frame=%llu\n", what, (unsigned long long)server->frame);
}

/*
 * Asks for the power action once the "Shutting down..." picture has been
 * shown (two frames on, or GREETER_POWER_MS).  The backend sends it to
 * sessiond (ws131-p005); sessiond's answer is read with the others.
 */
static void
greeter_power_send(
	struct kwl_server *server)
{
	uint64_t elapsed;
	unsigned action;
	int reboot;
	int error;

	/* Sent already. */
	if (greeter_power_sent)
		return;

	/* The picture is shown once two frames have gone since the press, or after the longest wait. */
	elapsed = kwl_milliseconds() - greeter_power_ms;
	if (server->frame < greeter_power_frame + 2U && elapsed < GREETER_POWER_MS)
		return;

	/* The action the button asked for. */
	action = KL_BACKEND_POWER_POWEROFF;
	reboot = strcmp(greeter_powering, "reboot");
	if (reboot == 0)
		action = KL_BACKEND_POWER_REBOOT;

	/* The request, through the backend. */
	greeter_power_sent = 1U;
	error = kl_backend_power_action(server->backend, action);
	if (error != 0)
		printf("KWL GREETER send errno=%d\n", error);
	printf("KWL GREETER power=%s frames=%llu ms=%llu\n", greeter_powering, (unsigned long long)(server->frame - greeter_power_frame), (unsigned long long)elapsed);
}

/* Draws the card of a machine that is ending: "Shutting down..." or "Restarting...", and a turning ring of dots. */
static void
greeter_draw_power(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct greeter_layout *layout)
{
	static const float ink[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	struct glass_shape shape;
	const char *words;
	float dot[4];
	float angle;
	float phase;
	float cx;
	float cy;
	int32_t middle;
	unsigned index;
	int reboot;

	/* The card's frosted glass, as the login card's. */
	glass_shape_init(&shape, (float)layout->card[0], (float)layout->card[1], (float)layout->card[2], (float)layout->card[3]);
	shape.mode = MODE_GLASS;
	shape.radius = GREETER_CARD_RADIUS;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.62f;
	shape.edge = 0.70f;
	glass_shape_draw(server, command, &shape);

	/* The words, in the card's upper half. */
	words = kl_tr("Shutting down...");
	reboot = strcmp(greeter_powering, "reboot");
	if (reboot == 0)
		words = kl_tr("Restarting...");
	middle = layout->card[0] + layout->card[2] / 2;
	greeter_draw_centered(server, command, SIZE_SEARCH, middle, layout->card[1] + layout->card[3] * 2 / 5, words, GREETER_CARD_WIDTH - 32, ink);

	/* The spinner under them: dots on a ring, the brightest turning with the time. */
	cx = (float)middle;
	cy = (float)(layout->card[1] + layout->card[3] * 2 / 3);
	phase = (float)(kwl_milliseconds() % GREETER_SPINNER_MS) / (float)GREETER_SPINNER_MS;
	for (index = 0U; index < GREETER_SPINNER_DOTS; index++) {
		angle = 6.2831853f * (float)index / (float)GREETER_SPINNER_DOTS;
		dot[0] = 0.26f;
		dot[1] = 0.48f;
		dot[2] = 0.86f;
		dot[3] = 0.25f + 0.75f * fmodf(1.0f + (float)index / (float)GREETER_SPINNER_DOTS - phase, 1.0f);
		glass_draw_solid(server, command, cx + 22.0f * sinf(angle) - 5.0f, cy - 22.0f * cosf(angle) - 5.0f, 10.0f, 10.0f, 5.0f, dot);
	}
}

/*
 * Acts on one answer of the session manager's: 0 granted (OK), EACCES
 * refused (FAIL, with its word: kl_backend_session_reason), EBUSY another
 * request under way, EIO not done (ERROR), another a line not understood.
 */
static void
greeter_answered(
	struct kwl_server *server,
	int error)
{
	const char *answer;

	/* The screen is redrawn with the result (no touch is awaited any more); the answer as the manager said it. */
	server->dirty = 1;
	greeter_touch = 0U;
	answer = "?";
	if (error == 0) {
		answer = "OK";
	} else if (error == EACCES) {
		answer = "FAIL";
	} else if (error == EBUSY) {
		answer = "BUSY";
	} else if (error == EIO) {
		answer = "ERROR";
	}

	/* The log names the answer and its word. */
	printf("KWL GREETER answer=%s reason=%s\n", answer, kl_backend_session_reason(server->backend));

	/* Unlocked: the desktop shows again. */
	if (error == 0 && greeter_waiting && server->locked) {
		greeter_unlock(server);
		printf("KWL LOCK unlocked\n");
		return;
	}

	/* Logged in: the screen stays until sessiond closes the descriptor (the session is then ready). */
	if (error == 0 && greeter_waiting) {
		greeter_waiting = 0;
		greeter_starting = 1;
		printf("KWL GREETER starting\n");
		return;
	}

	/* A refusal says why; another failure says it failed. */
	if (error == EACCES) {
		greeter_refused(server);
	} else if (error == EBUSY) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("Busy. Try again."));
	} else if (error == EIO) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("The login failed."));
	}

	/* The next secret can be typed. */
	greeter_waiting = 0;
}

/*
 * Says why sessiond refused the secret, and asks the styles again after a
 * wrong PIN (the fifth turns the PIN off).
 */
static void
greeter_refused(
	struct kwl_server *server)
{
	const char *reason;
	int same;

	/* The PIN is off now (five wrong ones, or not offered since the start): the password. */
	reason = kl_backend_session_reason(server->backend);
	same = strcmp(reason, "pin-off");
	if (same == 0) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("Use your password."));
		greeter_styles &= ~KL_BACKEND_STYLE_PIN;
		greeter_style = KL_BACKEND_STYLE_PASSWORD;
		return;
	}

	/* The account cannot log in. */
	same = strcmp(reason, "locked");
	if (same == 0) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("This account is locked."));
		return;
	}

	/* The check itself failed (passkey is missing or broke): not the user's mistake. */
	same = strcmp(reason, "internal");
	if (same == 0) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("The login failed."));
		return;
	}

	/* The check took too long. */
	same = strcmp(reason, "timeout");
	if (same == 0) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("That took too long. Try again."));
		return;
	}

	/* A security key's refusals (ws172-p003). */
	if (greeter_style == KL_BACKEND_STYLE_KEY) {
		greeter_key_refused(reason);
		return;
	}

	/* A wrong PIN: said, and whether the PIN is still offered is asked. */
	if (greeter_style == KL_BACKEND_STYLE_PIN) {
		snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("Wrong PIN. Try again."));
		greeter_styles_wanted = 1U;
		return;
	}

	/* A wrong password. */
	snprintf(greeter_message, sizeof(greeter_message), "%s", kl_tr("Wrong password. Try again."));
}

/* Takes the lock screen away: the desktop shows, and the idle time starts again. */
static void
greeter_unlock(
	struct kwl_server *server)
{
	/* No answer is awaited any more. */
	greeter_waiting = 0;
	server->locked = 0U;
	server->lock_input_ms = kwl_milliseconds();
	kwl_lid_unlocked(&server->lid);
}

/* Starts the styles again for a new user or screen: the password until sessiond says more. */
static void
greeter_styles_reset(
	void)
{
	greeter_styles = KL_BACKEND_STYLE_PASSWORD;
	greeter_style = KL_BACKEND_STYLE_PASSWORD;
	greeter_style_chosen = 0U;
	greeter_styles_wanted = 1U;
	greeter_submit_pending = 0U;
}

/* Asks sessiond the selected user's styles (the session's own on the lock screen). */
static void
greeter_styles_ask(
	struct kwl_server *server)
{
	const char *user;
	int error;

	/* The login screen names the user; the lock screen's is the session's. */
	user = NULL;
	if (!server->locked)
		user = greeter_users[greeter_selected].name;
	error = kl_backend_session_styles(server->backend, user);

	/* A busy manager is asked again on a later tick; any other failure leaves the password alone. */
	if (error == EBUSY)
		return;
	greeter_styles_wanted = 0U;
	if (error == 0)
		greeter_styles_asked = 1U;
}

/*
 * Takes sessiond's styles: the PIN is offered (and taken first unless the
 * user chose the password) or not (the password then).
 */
static void
greeter_styles_take(
	struct kwl_server *server)
{
	struct greeter_layout layout;
	unsigned styles;
	unsigned index;

	/* The styles, the password always among them. */
	styles = kl_backend_session_styles_get(server->backend);
	greeter_styles = styles | KL_BACKEND_STYLE_PASSWORD;
	server->dirty = 1;
	printf("KWL GREETER styles=%u\n", greeter_styles);

	/* Where the link to the next style is, for the tests' pointer (ws172-p003). */
	greeter_layout(server, &layout);
	printf("KWL GREETER link x=%d y=%d width=%d height=%d\n", layout.link[0], layout.link[1], layout.link[2], layout.link[3]);

	/* Where the lock screen's styles are, for the tests' pointer too (ws187-p003). */
	for (index = 0U; index < layout.style_count; index++)
		printf("KWL GREETER style-at style=%u x=%d y=%d width=%d height=%d\n", layout.style_bits[index], layout.styles[index][0], layout.styles[index][1], layout.styles[index][2], layout.styles[index][3]);

	/* A style no longer offered gives way to the password, and what was typed for it goes. */
	if (greeter_style != KL_BACKEND_STYLE_PASSWORD && (greeter_styles & greeter_style) == 0U) {
		greeter_erase();
		greeter_style = KL_BACKEND_STYLE_PASSWORD;
	}

	/* No PIN: the password (or the key the user chose). */
	if ((greeter_styles & KL_BACKEND_STYLE_PIN) == 0U)
		return;

	/* The PIN first, while nothing is typed and the user did not choose the password. */
	if (!greeter_style_chosen && greeter_password_length == 0U)
		greeter_style = KL_BACKEND_STYLE_PIN;
}

/* Switches the field to the next style the user has: the password, the PIN, a security key (the link under it). */
static void
greeter_style_switch(
	struct kwl_server *server)
{
	/* Only while another style than the password is offered and no answer is awaited. */
	if ((greeter_styles & (KL_BACKEND_STYLE_PIN | KL_BACKEND_STYLE_KEY)) == 0U || greeter_waiting)
		return;

	/* The next style, with nothing typed. */
	greeter_style = greeter_next_style();

	/* The user chose it; nothing typed for the other stays. */
	greeter_style_chosen = 1U;
	greeter_erase();
	greeter_message[0] = '\0';
	server->dirty = 1;
	printf("KWL GREETER style=%u\n", greeter_style);
}

/*
 * Lays out the lock screen's styles side by side in the link's line
 * (ws187-p003): those sessiond offers now, in the order password, PIN,
 * security key, each an equal part of the line; none on the login screen
 * or while only the password is offered.
 */
static void
greeter_layout_styles(
	struct kwl_server *server,
	struct greeter_layout *layout)
{
	static const unsigned order[GREETER_STYLES] = {
		KL_BACKEND_STYLE_PASSWORD,
		KL_BACKEND_STYLE_PIN,
		KL_BACKEND_STYLE_KEY
	};
	int32_t part;
	unsigned index;
	unsigned count;

	/* None until a lock screen offers more than the password. */
	layout->style_count = 0U;
	if (!server->locked)
		return;
	if ((greeter_styles & (KL_BACKEND_STYLE_PIN | KL_BACKEND_STYLE_KEY)) == 0U)
		return;

	/* The styles offered, in their order. */
	count = 0U;
	for (index = 0U; index < GREETER_STYLES; index++) {
		if ((greeter_styles & order[index]) != 0U) {
			layout->style_bits[count] = order[index];
			count++;
		}
	}

	/* Each an equal part of the link's line, a small gap between them. */
	part = layout->link[2] / (int32_t)count;
	for (index = 0U; index < count; index++) {
		layout->styles[index][0] = layout->link[0] + (int32_t)index * part + 2;
		layout->styles[index][1] = layout->link[1];
		layout->styles[index][2] = part - 4;
		layout->styles[index][3] = layout->link[3];
	}

	/* Succeeded: the styles are laid out. */
	layout->style_count = count;
}

/*
 * Draws the lock screen's styles side by side (ws187-p003): the field's
 * style lit, the one under the pointer lighter, the others' names faint.
 */
static void
greeter_draw_styles(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct greeter_layout *layout)
{
	static const float ink[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	static const float faint[4] = { 0.10f, 0.14f, 0.22f, 0.62f };
	static const float chosen[4] = { 1.0f, 1.0f, 1.0f, 0.70f };
	static const float hover[4] = { 1.0f, 1.0f, 1.0f, 0.32f };
	const int32_t *rect;
	const float *color;
	const char *label;
	unsigned index;
	int inside;

	/* Each style offered. */
	for (index = 0U; index < layout->style_count; index++) {
		/* Its pill: lit when the field takes it, lighter under the pointer. */
		rect = layout->styles[index];
		inside = greeter_inside(rect, server->pointer_x, server->pointer_y);
		color = faint;
		if (layout->style_bits[index] == greeter_style) {
			glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], (float)rect[3] / 2.0f, chosen);
			color = ink;
		} else if (inside) {
			glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], (float)rect[3] / 2.0f, hover);
			color = ink;
		}

		/* Its name. */
		label = kl_tr("Password");
		if (layout->style_bits[index] == KL_BACKEND_STYLE_PIN)
			label = kl_tr("PIN");
		if (layout->style_bits[index] == KL_BACKEND_STYLE_KEY)
			label = kl_tr("Security Key");
		greeter_draw_centered(server, command, SIZE_BAR, rect[0] + rect[2] / 2, rect[1] + 19, label, rect[2] - 8, color);
	}
}

/*
 * Makes the field take a style the lock screen offers, chosen by a press
 * (ws187-p003); nothing typed for the other stays.
 */
static void
greeter_style_choose(
	struct kwl_server *server,
	unsigned style)
{
	/* Not while an answer is awaited, nor for the style the field takes already. */
	if (greeter_waiting)
		return;
	if (style == greeter_style)
		return;

	/* The style, chosen by the user, with nothing typed. */
	greeter_style = style;
	greeter_style_chosen = 1U;
	greeter_erase();
	greeter_message[0] = '\0';
	server->dirty = 1;
	printf("KWL GREETER style=%u via=choice\n", greeter_style);
}

/* Erases what has been typed. */
static void
greeter_erase(
	void)
{
	/* Every byte, not only the characters typed; nothing waits to be sent. */
	memset(greeter_password, 0, sizeof(greeter_password));
	greeter_password_length = 0U;
	greeter_submit_pending = 0U;
}

/*
 * Gives the style after the field's in the order password, PIN, security
 * key, among those offered (the password always is).
 */
static unsigned
greeter_next_style(void)
{
	/* After the password: the PIN, else a key. */
	if (greeter_style == KL_BACKEND_STYLE_PASSWORD) {
		if ((greeter_styles & KL_BACKEND_STYLE_PIN) != 0U)
			return KL_BACKEND_STYLE_PIN;
		if ((greeter_styles & KL_BACKEND_STYLE_KEY) != 0U)
			return KL_BACKEND_STYLE_KEY;
		return KL_BACKEND_STYLE_PASSWORD;
	}

	/* After the PIN: a key, else the password. */
	if (greeter_style == KL_BACKEND_STYLE_PIN && (greeter_styles & KL_BACKEND_STYLE_KEY) != 0U)
		return KL_BACKEND_STYLE_KEY;

	/* After a key, back to the password. */
	return KL_BACKEND_STYLE_PASSWORD;
}

/* Says why a security key's attempt was refused (ws172-p003). */
static void
greeter_key_refused(
	const char *reason)
{
	const char *said;
	int same;

	/* No registered key plugged in. */
	said = kl_tr("Wrong security key PIN. Try again.");
	same = strcmp(reason, "no-key");
	if (same == 0)
		said = kl_tr("No registered security key is plugged in.");

	/* The key locked itself (too many wrong PINs). */
	same = strcmp(reason, "key-locked");
	if (same == 0)
		said = kl_tr("The security key is locked.");

	/* The key did not answer as it should. */
	same = strcmp(reason, "device");
	if (same == 0)
		said = kl_tr("The security key did not answer.");

	/* A key that may have been copied. */
	same = strcmp(reason, "cloned");
	if (same == 0)
		said = kl_tr("This security key cannot be used.");

	/* The line under the field. */
	snprintf(greeter_message, sizeof(greeter_message), "%s", said);
}
