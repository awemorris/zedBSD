/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Security Keys page (ws199-p001 section 3; before it the Users
 * page's security keys card, ws172-p003; docs/architecture/security.md,
 * "The security key"): the Software Security Key (page-users-pin.c), and
 * the FIDO2 keys that log in and unlock on this machine's screen.
 *
 * The keys card lists the user's keys as the desktop tells them
 * (kl_system_account_keys: the session manager keeps them in /etc/passkey
 * on zedBSD), each with Remove, and has Add Key, and with the keys' own
 * operations (KL_SYSTEM_HAS_KEY_OPS) Change PIN and Reset Key.  Each opens
 * the popup (dialog.c) as a wizard:
 *
 *   Add Key     the account's password; the key there (what the desktop
 *               says of it, asked again when a key comes or goes); its
 *               name (the key's own, or one not yet used); a first PIN
 *               for a key without one; the key's PIN; the touch
 *               (kl_system_account_add_key).  Without the keys' own
 *               operations: the password, the name, the PIN, the touch.
 *   Remove      the password (kl_system_account_remove_key).
 *   Change PIN  the key there; its first PIN, or its PIN and a new one
 *               (kl_system_account_key_pin, no password: the key checks).
 *   Reset Key   a warning; the password; the key plugged in again; the
 *               touch (kl_system_account_key_reset).
 *
 * The password is kept from its step to the end of the wizard (an
 * addition whose key PIN was wrong asks only the PIN again), and wiped
 * then, when the popup closes and when it is left alone; a key's PIN goes
 * as it is sent (a first PIN set during an addition is kept for the
 * registration that follows).  A desktop without the keys
 * (KL_SYSTEM_HAS_KEYS) says so.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The card's controls: Add Key, Change PIN, Reset Key, and each key's Remove. */
#define KEYS_ADD		310
#define KEYS_CHANGE_PIN		311
#define KEYS_RESET		312
#define KEYS_REMOVE_FIRST	320

/* The wizards' steps (0 is the end). */
#define KEYS_STEP_DONE		0U
#define KEYS_STEP_PASSWORD	1U
#define KEYS_STEP_INSERT	2U
#define KEYS_STEP_NAME		3U
#define KEYS_STEP_SET_PIN	4U
#define KEYS_STEP_PIN		5U
#define KEYS_STEP_TOUCH		6U
#define KEYS_STEP_WARNING	7U
#define KEYS_STEP_REPLUG	8U
#define KEYS_STEP_CHANGE	9U

/* The fewest characters of a key's PIN (CTAP 2.1, when the key says none), the most bytes (CTAP: 63), and the retries shown. */
#define KEYS_PIN_MIN		4U
#define KEYS_PIN_MAX		63U
#define KEYS_RETRIES_SHOWN	3U

/* The name a new key gets when the key has none of its own, and how many numbered ones are tried after it. */
#define KEYS_NAME		"Security Key"
#define KEYS_NAME_TRIES		9U

/* A key's row, and the space between two cards. */
#define KEYS_KEY_ROW		44
#define KEYS_GAP		16

/* The text sizes of a row and of a message. */
#define KEYS_TEXT_ROW		15U
#define KEYS_TEXT_SUB		13U

/* The room of a refusal's word. */
#define KEYS_REASON		32U

/* What the user is asked to do with the key. */
#define KEYS_TOUCH_TEXT		"Touch your security key, or hold it to the reader."
#define KEYS_REPLUG_TEXT	"Unplug the key, then plug it back in (or take it away from the reader and hold it there again)."

static int keys_available(const struct se_app *app);
static int keys_ops(const struct se_app *app);
static int keys_card_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void keys_start(struct se_app *app, unsigned flow, const struct kl_system_key *key);
static void keys_step(struct se_app *app, unsigned step);
static void keys_place(const struct se_app *app, unsigned step, unsigned *place, unsigned *count);
static void keys_act(struct se_app *app, unsigned action);
static void keys_next(struct se_app *app);
static void keys_back(struct se_app *app);
static int keys_ready(const struct se_app *app);
static void keys_ask_info(struct se_app *app);
static void keys_info_result(struct se_app *app, int error);
static void keys_found(struct se_app *app);
static void keys_ask(struct se_app *app);
static void keys_result(struct se_app *app, int error);
static void keys_failed(struct se_app *app, int error);
static void keys_end_with(struct se_app *app, const char *title, const char *text);
static void keys_default_name(struct se_app *app, char *name, size_t size);
static int keys_name_valid(const char *name, size_t length);
static void keys_keep_name(struct se_app *app);
static size_t keys_code_points(const char *text);
static int keys_pin_ok(const struct se_app *app, unsigned first, unsigned again);

/*
 * Draws the Security Keys page's cards from a top edge; returns the edge
 * below them.
 */
int
se_keys_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int bottom;

	/* The Software Security Key, then the keys. */
	bottom = se_keys_soft_draw(app, canvas, x, top, width);
	bottom = keys_card_draw(app, canvas, x, bottom + KEYS_GAP, width);
	return bottom;
}

/*
 * Carries out a click on a control of the Security Keys page.
 */
void
se_keys_press(
	struct se_app *app,
	int index)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	size_t count;
	int busy;
	int taken;
	int ops;

	/* The Software Security Key's. */
	taken = se_keys_soft_press(app, index);
	if (taken)
		return;

	/* One change at a time. */
	busy = app->keys.asked || app->keys.info_asked || app->dialog.open;
	if (busy)
		return;

	/* Add Key, while there is room for one more. */
	count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (index == KEYS_ADD) {
		if (count < KL_SYSTEM_KEYS_MAX)
			keys_start(app, SE_KEYS_FLOW_ADD, NULL);
		return;
	}

	/* Change PIN and Reset Key, with the keys' own operations. */
	ops = keys_ops(app);
	if (index == KEYS_CHANGE_PIN && ops) {
		keys_start(app, SE_KEYS_FLOW_KEY_PIN, NULL);
		return;
	}
	if (index == KEYS_RESET && ops) {
		keys_start(app, SE_KEYS_FLOW_RESET, NULL);
		return;
	}

	/* A key's Remove, when the key is still listed. */
	if (index < KEYS_REMOVE_FIRST || index >= KEYS_REMOVE_FIRST + (int)KL_SYSTEM_KEYS_MAX)
		return;
	if ((size_t)(index - KEYS_REMOVE_FIRST) >= count)
		return;
	keys_start(app, SE_KEYS_FLOW_REMOVE, &keys[index - KEYS_REMOVE_FIRST]);
}

/*
 * Takes the answer of a request the page asked (what the keys are, a
 * key's change or operation, or the PIN's).  Returns 1 when the request
 * was the page's.
 */
int
se_keys_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_keys *keys;

	/* What the keys there are. */
	keys = &app->keys;
	if (keys->info_asked && request == keys->info_request) {
		keys_info_result(app, error);
		app->dirty = 1;
		return 1;
	}

	/* Only the change the page asked. */
	if (!keys->asked || request != keys->request)
		return 0;

	/* The PIN's, or a key's. */
	if (keys->flow == SE_KEYS_FLOW_PIN_SET || keys->flow == SE_KEYS_FLOW_PIN_REMOVE) {
		se_keys_soft_result(app, error);
	} else {
		keys_result(app, error);
	}

	/* Taken. */
	app->dirty = 1;
	return 1;
}

/*
 * Takes the desktop's word that a key waits to be touched, when it is for
 * the page's change.
 */
void
se_keys_touched(
	struct se_app *app)
{
	struct se_keys *keys;
	uint32_t request;
	int taken;

	/* The touch's request, once. */
	keys = &app->keys;
	if (app->system == NULL)
		return;
	taken = kl_system_account_touched(app->system, &request);
	if (!taken || !keys->asked || request != keys->request)
		return;

	/* Said in the popup (with the keys' own operations it may be cancelled). */
	keys->touch = 1;
	if (app->dialog.open)
		se_dialog_busy(app, KEYS_TOUCH_TEXT, keys_ops(app));
	se_log("KEYS touch request=%u", request);
}

/*
 * Takes the desktop's word that a reset waits for the key to be plugged
 * in again (ws199-p001).
 */
void
se_keys_replugged(
	struct se_app *app)
{
	struct se_keys *keys;
	uint32_t request;
	int taken;

	/* The replug's request, once. */
	keys = &app->keys;
	if (app->system == NULL)
		return;
	taken = kl_system_account_replugged(app->system, &request);
	if (!taken || !keys->asked || request != keys->request)
		return;

	/* The step, busy and cancellable until the key is back and touched. */
	if (app->dialog.open && keys->flow == SE_KEYS_FLOW_RESET) {
		keys->step = KEYS_STEP_REPLUG;
		se_dialog_step(app, "Plug the key in again", 3U, 4U, "The key takes a reset only just after it is plugged in, so it is asked as soon as it comes back.", NULL, 0);
		se_dialog_busy(app, KEYS_REPLUG_TEXT, 1);
	}
	se_log("KEYS replug request=%u", request);
}

/*
 * A key came or went, or the screen was unlocked (ws199-p001): a wizard
 * that waits for the key asks again what is there.
 */
void
se_keys_changed(
	struct se_app *app)
{
	struct se_keys *keys;

	/* Only a wizard at its key's step, not asking already. */
	keys = &app->keys;
	se_log("KEYS changed step=%u", keys->step);
	if (!app->dialog.open || keys->step != KEYS_STEP_INSERT || keys->info_asked)
		return;
	keys_ask_info(app);
}

/* Ends the wizard under way: the popup closed, what was kept wiped (a change asked still gets its answer). */
void
se_keys_end(
	struct se_app *app)
{
	struct se_keys *keys;

	/* The password, a new PIN, and the popup. */
	keys = &app->keys;
	se_field_clear(&keys->password);
	se_field_clear(&keys->pin);
	se_dialog_close(app);

	/* No step; the change asked, if any, stays asked. */
	keys->step = KEYS_STEP_DONE;
	keys->touch = 0;
	keys->setting_pin = 0;
	if (!keys->asked)
		keys->flow = SE_KEYS_FLOW_NONE;
}

/* Wipes what the page keeps at the window's end. */
void
se_keys_close(
	struct se_app *app)
{
	/* The password, a new PIN and the popup's fields. */
	se_keys_end(app);
}

/* Keeps the password the popup's first field holds, for the step that sends it. */
void
se_keys_keep_password(
	struct se_app *app)
{
	/* Copied out of the popup, whose fields the next step wipes. */
	se_field_clear(&app->keys.password);
	kl_field_set(&app->keys.password, se_dialog_text(app, 0U));
}

/* Gives the line for a key's refusal by its word (the desktop's, or a general one). */
const char *
se_keys_refusal(
	struct se_app *app,
	uint32_t request)
{
	static const struct {
		const char *word;
		const char *line;
	} lines[] = {
		{ "bad-secret", "The password is wrong." },
		{ "bad-key-pin", "The key's PIN is wrong." },
		{ "locked", "Too many wrong attempts. Wait, then try again." },
		{ "no-key", "No security key was found. Plug it in, or hold it to the reader." },
		{ "many-keys", "More than one key is there. Leave only the one key." },
		{ "key-locked", "The key's PIN is locked: too many wrong PINs. Reset the key to use it again." },
		{ "key-replug", "Too many wrong PINs for now. Unplug the key, plug it in again, and try again." },
		{ "no-pin", "The key has no PIN yet. Set one first." },
		{ "pin-set", "The key already has a PIN." },
		{ "pin-policy", "The key does not take that PIN." },
		{ "not-allowed", "The key came back too late for its reset. Try again, and touch it at once." },
		{ "timeout", "The key was not touched in time." },
		{ "canceled", "Cancelled." },
		{ "locked-account", "Your account is locked: it cannot have a security key." },
		{ "not-enrolled", "That key is no longer registered." },
		{ "device", "The security key did not answer." },
	};
	char reason[KEYS_REASON];
	size_t index;
	int refused;
	int same;

	/* The word the desktop gave. */
	refused = kl_system_account_refusal(app->system, request, reason, sizeof(reason));
	if (!refused)
		return "The security key could not be changed.";

	/* Its line. */
	for (index = 0U; index < sizeof(lines) / sizeof(lines[0]); index++) {
		same = strcmp(reason, lines[index].word);
		if (same == 0)
			return lines[index].line;
	}

	/* A word without one. */
	return "The security key could not be changed.";
}

/* Tells whether the desktop offers the keys (KL_SYSTEM_HAS_KEYS). */
static int
keys_available(
	const struct se_app *app)
{
	unsigned bits;

	/* No desktop system. */
	if (app->system == NULL)
		return 0;

	/* Its keys. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_KEYS) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Tells whether the desktop offers the keys' own operations (KL_SYSTEM_HAS_KEY_OPS, ws199-p001). */
static int
keys_ops(
	const struct se_app *app)
{
	unsigned bits;

	/* No desktop system. */
	if (app->system == NULL)
		return 0;

	/* Its keys' operations. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_KEY_OPS) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Draws the keys card from a top edge: how many, each with Remove, and the buttons; returns the edge below it. */
static int
keys_card_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	char state[SE_MESSAGE];
	size_t count;
	size_t index;
	int available;
	int enabled;
	int button;
	int right;
	int ops;
	int height;
	int y;

	/* Without the keys, a short card that says so. */
	available = keys_available(app);
	count = 0U;
	if (available)
		count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (count > KL_SYSTEM_KEYS_MAX)
		count = KL_SYSTEM_KEYS_MAX;
	height = 64 + 36 + (int)count * KEYS_KEY_ROW + 56;
	if (!available)
		height = 64 + 50;
	y = se_card_begin(app, canvas, x, top, width, height, "Security keys", "Sign in and unlock with a FIDO2 security key, plugged in or held to the reader.");
	if (!available) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 24, "This desktop cannot use security keys here.", KEYS_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* How many keys there are. */
	(void)snprintf(state, sizeof(state), "%s", "No security key is registered.");
	if (count == 1U)
		(void)snprintf(state, sizeof(state), "%s", "One security key is registered.");
	if (count > 1U)
		(void)snprintf(state, sizeof(state), "%lu security keys are registered.", (unsigned long)count);
	(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, state, KEYS_TEXT_SUB, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
	y += 36;

	/* Each key: its name, and Remove at the right. */
	right = x + width - 20;
	enabled = !app->keys.asked && !app->keys.info_asked && !app->dialog.open;
	button = se_button_width(app, "Remove");
	for (index = 0U; index < count; index++) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(KEYS_TEXT_ROW, y + 4, 36), keys[index].label, KEYS_TEXT_ROW, 0, width - 60 - button, SE_COLOR_TEXT);
		(void)se_button_draw(app, canvas, right - button, y + 6, "Remove", 0, enabled, KEYS_REMOVE_FIRST + (int)index);
		y += KEYS_KEY_ROW;
	}

	/* Add Key at the right, while there is room for one more (the session manager keeps KL_SYSTEM_KEYS_MAX). */
	button = se_button_width(app, "Add Key");
	right -= button;
	(void)se_button_draw(app, canvas, right, y + 8, "Add Key", 1, enabled && count < KL_SYSTEM_KEYS_MAX, KEYS_ADD);

	/* Change PIN and Reset Key left of it, with the keys' own operations (Linux and FreeBSD say they are not here). */
	ops = keys_ops(app);
	if (!ops) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 28, "A key's PIN and its reset are not available on this system.", KEYS_TEXT_SUB, 0, right - x - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}
	button = se_button_width(app, "Change PIN");
	right -= 8 + button;
	(void)se_button_draw(app, canvas, right, y + 8, "Change PIN", 0, enabled, KEYS_CHANGE_PIN);
	button = se_button_width(app, "Reset Key");
	right -= 8 + button;
	(void)se_button_draw(app, canvas, right, y + 8, "Reset Key", 0, enabled, KEYS_RESET);

	/* The edge below the card. */
	return top + height;
}

/* Starts a key's wizard: an addition, a removal of a key, a PIN's change, or a reset. */
static void
keys_start(
	struct se_app *app,
	unsigned flow,
	const struct kl_system_key *key)
{
	struct se_keys *keys;

	/* Nothing kept from before; the key a removal names. */
	se_keys_end(app);
	keys = &app->keys;
	keys->flow = flow;
	keys->name[0] = '\0';
	keys->ref[0] = '\0';
	keys->label[0] = '\0';
	keys->info_known = 0;
	keys->removed = 0U;
	if (key != NULL) {
		(void)snprintf(keys->ref, sizeof(keys->ref), "%s", key->ref);
		(void)snprintf(keys->label, sizeof(keys->label), "%s", key->label);
	}

	/* The popup, its owner this page, at the flow's first step. */
	se_dialog_open(app, keys_act, keys_ready);
	if (flow == SE_KEYS_FLOW_KEY_PIN) {
		keys_step(app, KEYS_STEP_INSERT);
	} else if (flow == SE_KEYS_FLOW_RESET) {
		keys_step(app, KEYS_STEP_WARNING);
	} else {
		keys_step(app, KEYS_STEP_PASSWORD);
	}
	se_log("KEYS key start flow=%u", flow);
}

/* Gives a step's place among the flow's steps (for "Step n of m"). */
static void
keys_place(
	const struct se_app *app,
	unsigned step,
	unsigned *place,
	unsigned *count)
{
	int ops;

	/* A removal has one step, a PIN's change two, a reset four. */
	ops = keys_ops(app);
	*place = 1U;
	*count = 1U;
	switch (app->keys.flow) {
	case SE_KEYS_FLOW_KEY_PIN:
		*count = 2U;
		if (step != KEYS_STEP_INSERT)
			*place = 2U;
		return;
	case SE_KEYS_FLOW_RESET:
		*count = 4U;
		if (step == KEYS_STEP_PASSWORD)
			*place = 2U;
		return;
	case SE_KEYS_FLOW_ADD:
		break;
	default:
		return;
	}

	/* An addition: the password, the key (with the keys' own operations), the name, its PIN, the touch. */
	*count = 4U + (unsigned)ops;
	switch (step) {
	case KEYS_STEP_INSERT:
		*place = 2U;
		break;
	case KEYS_STEP_NAME:
		*place = 2U + (unsigned)ops;
		break;
	case KEYS_STEP_SET_PIN:
	case KEYS_STEP_PIN:
		*place = 3U + (unsigned)ops;
		break;
	case KEYS_STEP_TOUCH:
		*place = 4U + (unsigned)ops;
		break;
	default:
		break;
	}
}

/* Shows a step of a key's wizard. */
static void
keys_step(
	struct se_app *app,
	unsigned step)
{
	struct se_keys *keys;
	char body[SE_MESSAGE * 2U];
	unsigned place;
	unsigned count;
	int ops;

	/* The step and its place. */
	keys = &app->keys;
	keys->step = step;
	keys_place(app, step, &place, &count);
	ops = keys_ops(app);

	/* Each step. */
	switch (step) {
	case KEYS_STEP_PASSWORD:
		se_field_clear(&keys->password);
		if (keys->flow == SE_KEYS_FLOW_REMOVE) {
			(void)snprintf(body, sizeof(body), "Remove \"%s\"? It will no longer sign in or unlock on this computer. Type your password to remove it.", keys->label);
			se_dialog_step(app, "Remove the security key", place, count, body, "Remove", 0);
		} else if (keys->flow == SE_KEYS_FLOW_RESET) {
			se_dialog_step(app, "Reset the security key", place, count, "Type your password. Then you will plug the key in again and touch it.", "Reset Key", 1);
		} else {
			se_dialog_step(app, "Add a security key", place, count, "Type your password to add a security key to your account.", "Next", 0);
		}
		se_dialog_field(app, "Password", "Your password now", SE_FIELD_SECRET, 0U);
		break;
	case KEYS_STEP_INSERT:
		se_dialog_step(app, "Your security key", place, count, "", NULL, keys->flow == SE_KEYS_FLOW_ADD);
		keys_ask_info(app);
		break;
	case KEYS_STEP_NAME:
		se_dialog_step(app, "Name the key", place, count, "Give the key a name you will know it by in this list.", "Next", 1);
		se_dialog_field(app, "Name", "YubiKey, for example", SE_FIELD_TEXT, KL_SYSTEM_KEY_LABEL_MAX);
		se_dialog_set_text(app, 0U, keys->name);
		break;
	case KEYS_STEP_SET_PIN:
		(void)snprintf(body, sizeof(body), "The key has no PIN yet. Choose one of %u to %u characters; the key asks it whenever it signs you in.", keys->info.min, KEYS_PIN_MAX);
		se_dialog_step(app, "Set the key's PIN", place, count, body, "Set PIN", keys->flow == SE_KEYS_FLOW_ADD);
		se_dialog_field(app, "New PIN", "The key's new PIN", SE_FIELD_SECRET, KEYS_PIN_MAX);
		se_dialog_field(app, "New PIN again", "The same again", SE_FIELD_SECRET, KEYS_PIN_MAX);
		break;
	case KEYS_STEP_CHANGE:
		(void)snprintf(body, sizeof(body), "Type the key's PIN now, then a new one of %u to %u characters.", keys->info.min, KEYS_PIN_MAX);
		se_dialog_step(app, "Change the key's PIN", place, count, body, "Change PIN", 0);
		se_dialog_field(app, "Key PIN", "The key's PIN now", SE_FIELD_SECRET, KEYS_PIN_MAX);
		se_dialog_field(app, "New PIN", "The key's new PIN", SE_FIELD_SECRET, KEYS_PIN_MAX);
		se_dialog_field(app, "New PIN again", "The same again", SE_FIELD_SECRET, KEYS_PIN_MAX);
		break;
	case KEYS_STEP_PIN:
		(void)snprintf(body, sizeof(body), "%s", "Type the key's own PIN.");
		if (!ops)
			(void)snprintf(body, sizeof(body), "%s", "Plug in the key, or hold it to the reader, and type the key's own PIN. A new key needs a PIN first: run fidoctl set-pin in Terminal.");
		if (keys->info_known && keys->info.retries != 0U && keys->info.retries <= KEYS_RETRIES_SHOWN)
			(void)snprintf(body, sizeof(body), "Type the key's own PIN. %u tries are left before the key locks.", keys->info.retries);
		se_dialog_step(app, "The key's PIN", place, count, body, "Add Key", 1);
		se_dialog_field(app, "Key PIN", "The key's own PIN", SE_FIELD_SECRET, KEYS_PIN_MAX);
		if (ops)
			se_dialog_link(app, "Forgot the PIN? Reset the key");
		break;
	case KEYS_STEP_WARNING:
		se_dialog_step(app, "Reset the security key", place, count,
		    "Resetting erases everything on the key: its PIN and every sign-in it holds, on this computer and on every other computer and website. This computer's sign-ins with it are removed too.",
		    "Continue", 0);
		break;
	default:
		break;
	}
}

/*
 * Takes the popup's action for a key's wizard: the step's button goes on
 * (or asks), Back goes back, Cancel closes (or stops what waits for the
 * key), the link resets the key, and idle wipes the password kept.
 */
static void
keys_act(
	struct se_app *app,
	unsigned action)
{
	struct se_keys *keys;
	int ops;

	/* Cancel: a key's wait is stopped (its answer ends the wizard); else nothing kept. */
	keys = &app->keys;
	ops = keys_ops(app);
	if (action == SE_DIALOG_CANCEL) {
		if (keys->asked && app->dialog.busy && ops) {
			(void)kl_system_account_key_cancel(app->system);
			se_log("KEYS cancel request=%u", keys->request);
			return;
		}
		se_keys_end(app);
		return;
	}

	/* Back: the step before. */
	if (action == SE_DIALOG_BACK) {
		keys_back(app);
		return;
	}

	/* The link: forgot the key's PIN, so a reset. */
	if (action == SE_DIALOG_LINK) {
		keys_start(app, SE_KEYS_FLOW_RESET, NULL);
		return;
	}

	/* Left alone: the password kept goes, and is asked again. */
	if (action == SE_DIALOG_IDLE) {
		if (keys->password.length != 0U && !keys->asked) {
			keys_step(app, KEYS_STEP_PASSWORD);
			se_dialog_error(app, "For your security, type your password again.");
		}
		return;
	}

	/* The step's button. */
	if (action == SE_DIALOG_PRIMARY)
		keys_next(app);
}

/* Goes on from the step shown (the step's button). */
static void
keys_next(
	struct se_app *app)
{
	struct se_keys *keys;
	int ops;

	/* Each step's next. */
	keys = &app->keys;
	ops = keys_ops(app);
	switch (keys->step) {
	case KEYS_STEP_DONE:
		/* The end's Done closes. */
		se_keys_end(app);
		return;
	case KEYS_STEP_INSERT:
		/* The key's step asks again. */
		keys_ask_info(app);
		return;
	case KEYS_STEP_WARNING:
		/* The warning read: the password. */
		keys_step(app, KEYS_STEP_PASSWORD);
		return;
	case KEYS_STEP_PASSWORD:
		/* Kept; a removal and a reset ask now, an addition goes to its key (or its name). */
		se_keys_keep_password(app);
		if (keys->flow != SE_KEYS_FLOW_ADD) {
			keys_ask(app);
			return;
		}
		if (ops) {
			keys_step(app, KEYS_STEP_INSERT);
			return;
		}
		keys_default_name(app, keys->name, sizeof(keys->name));
		keys_step(app, KEYS_STEP_NAME);
		return;
	case KEYS_STEP_NAME:
		/* Kept, then the key's PIN (a first one for a key without). */
		keys_keep_name(app);
		if (keys->info_known && !keys->info.pin) {
			keys_step(app, KEYS_STEP_SET_PIN);
			return;
		}
		keys_step(app, KEYS_STEP_PIN);
		return;
	default:
		/* A PIN's step asks. */
		keys_ask(app);
		return;
	}
}

/* Goes back a step (Back). */
static void
keys_back(
	struct se_app *app)
{
	struct se_keys *keys;
	int ops;

	/* Each step's step before. */
	keys = &app->keys;
	ops = keys_ops(app);
	switch (keys->step) {
	case KEYS_STEP_INSERT:
		/* The password. */
		keys_step(app, KEYS_STEP_PASSWORD);
		return;
	case KEYS_STEP_NAME:
		/* The key, or the password. */
		keys_keep_name(app);
		if (ops) {
			keys_step(app, KEYS_STEP_INSERT);
			return;
		}
		keys_step(app, KEYS_STEP_PASSWORD);
		return;
	case KEYS_STEP_SET_PIN:
	case KEYS_STEP_PIN:
		/* The key of a PIN's change, the name of an addition. */
		if (keys->flow == SE_KEYS_FLOW_KEY_PIN) {
			keys_step(app, KEYS_STEP_INSERT);
			return;
		}
		keys_step(app, KEYS_STEP_NAME);
		return;
	case KEYS_STEP_PASSWORD:
		/* A reset's warning. */
		if (keys->flow == SE_KEYS_FLOW_RESET)
			keys_step(app, KEYS_STEP_WARNING);
		return;
	default:
		return;
	}
}

/* Tells whether the popup's step may go on: the password typed, a name that is one, PINs that fit. */
static int
keys_ready(
	const struct se_app *app)
{
	size_t length;
	size_t points;
	int fits;

	/* The step's first field. */
	length = se_dialog_length(app, 0U);
	switch (app->keys.step) {
	case KEYS_STEP_PASSWORD:
		return length != 0U;
	case KEYS_STEP_NAME:
		return keys_name_valid(se_dialog_text(app, 0U), length);
	case KEYS_STEP_SET_PIN:
		return keys_pin_ok(app, 0U, 1U);
	case KEYS_STEP_CHANGE:
		fits = keys_pin_ok(app, 1U, 2U);
		return length != 0U && fits;
	case KEYS_STEP_PIN:
		points = keys_code_points(se_dialog_text(app, 0U));
		return points >= KEYS_PIN_MIN && length <= KEYS_PIN_MAX;
	default:
		return 1;
	}
}

/* Asks the desktop what the keys there are (the step busy until it answers). */
static void
keys_ask_info(
	struct se_app *app)
{
	struct se_keys *keys;
	uint32_t request;
	int error;

	/* Asked. */
	keys = &app->keys;
	error = kl_system_account_key_info(app->system, &request);
	if (error != 0) {
		se_dialog_step(app, "Your security key", 0U, 0U, "", "Check Again", 0);
		se_dialog_error(app, "The desktop cannot tell what key is there.");
		se_log("KEYS info errno=%d", error);
		return;
	}

	/* Busy until the answer. */
	keys->info_asked = 1;
	keys->info_request = request;
	se_dialog_busy(app, "Looking for your security key...", 0);
	se_log("KEYS info request=%u", request);
}

/* Takes what the keys there are: one goes on, none or several are said and waited on. */
static void
keys_info_result(
	struct se_app *app,
	int error)
{
	struct se_keys *keys;
	struct kl_system_key_info info;
	unsigned place;
	unsigned count;
	int back;
	int known;

	/* No longer asked; only a wizard still at the key's step goes on. */
	keys = &app->keys;
	keys->info_asked = 0;
	known = kl_system_account_key_info_get(app->system, &info);
	se_log("KEYS info result errno=%d count=%u pin=%u", error, info.count, info.pin);
	if (!app->dialog.open || keys->step != KEYS_STEP_INSERT)
		return;

	/* Not told: said, and asked again with Check Again. */
	keys_place(app, KEYS_STEP_INSERT, &place, &count);
	back = keys->flow == SE_KEYS_FLOW_ADD;
	if (error != 0 || !known) {
		se_dialog_step(app, "Your security key", place, count, "", "Check Again", back);
		se_dialog_error(app, "The desktop could not tell what key is there. Try again.");
		return;
	}

	/* None: plugged in or held to the reader, waited for (a key that comes asks again). */
	if (info.count == 0U) {
		se_dialog_step(app, "Your security key", place, count, "Plug in your security key, or hold it to the reader.", "Check Again", back);
		return;
	}

	/* Several: one at a time. */
	if (info.count > 1U) {
		se_dialog_step(app, "Your security key", place, count, "More than one key is there. Leave only the one key.", "Check Again", back);
		return;
	}

	/* One: on with it. */
	keys->info = info;
	if (keys->info.min < KEYS_PIN_MIN)
		keys->info.min = KEYS_PIN_MIN;
	keys->info_known = 1;
	keys_found(app);
}

/* Goes on with the one key found: an addition to its name, a PIN's change to its PIN. */
static void
keys_found(
	struct se_app *app)
{
	struct se_keys *keys;

	/* A PIN's change: a first PIN, or the PIN and a new one. */
	keys = &app->keys;
	if (keys->flow == SE_KEYS_FLOW_KEY_PIN) {
		if (!keys->info.pin) {
			keys_step(app, KEYS_STEP_SET_PIN);
			return;
		}
		keys_step(app, KEYS_STEP_CHANGE);
		return;
	}

	/* An addition: the key's own name when it can be one, else a free one. */
	keys_default_name(app, keys->name, sizeof(keys->name));
	keys_step(app, KEYS_STEP_NAME);
}

/*
 * Asks the desktop for the step's change: a removal, a reset (the password
 * kept), a first PIN or a PIN's change (no password), or an addition (the
 * password kept, the name, the PIN typed or just set).
 */
static void
keys_ask(
	struct se_app *app)
{
	struct se_keys *keys;
	char pin[KEYS_PIN_MAX + 1U];
	char fresh[KEYS_PIN_MAX + 1U];
	const char *busy;
	uint32_t request;
	int cancellable;
	int error;

	/* What is asked, the secrets out of the popup before it is busy. */
	keys = &app->keys;
	pin[0] = '\0';
	fresh[0] = '\0';
	cancellable = keys_ops(app);
	busy = "Checking the password...";
	keys->setting_pin = 0;
	switch (keys->step) {
	case KEYS_STEP_SET_PIN:
		(void)snprintf(fresh, sizeof(fresh), "%s", se_dialog_text(app, 0U));
		error = kl_system_account_key_pin(app->system, NULL, fresh, &request);
		keys->setting_pin = 1;
		busy = "Setting the key's PIN...";
		if (keys->flow == SE_KEYS_FLOW_ADD) {
			se_field_clear(&keys->pin);
			kl_field_set(&keys->pin, fresh);
		}
		break;
	case KEYS_STEP_CHANGE:
		(void)snprintf(pin, sizeof(pin), "%s", se_dialog_text(app, 0U));
		(void)snprintf(fresh, sizeof(fresh), "%s", se_dialog_text(app, 1U));
		error = kl_system_account_key_pin(app->system, pin, fresh, &request);
		busy = "Changing the key's PIN...";
		break;
	case KEYS_STEP_PIN:
		(void)snprintf(pin, sizeof(pin), "%s", se_dialog_text(app, 0U));
		error = kl_system_account_add_key(app->system, keys->password.text, keys->name, pin, &request);
		busy = "Checking the password and the key...";
		break;
	default:
		if (keys->flow == SE_KEYS_FLOW_REMOVE) {
			error = kl_system_account_remove_key(app->system, keys->password.text, keys->ref, &request);
		} else {
			error = kl_system_account_key_reset(app->system, keys->password.text, &request);
		}
		se_field_clear(&keys->password);
		cancellable = 0;
		break;
	}
	memset(pin, 0, sizeof(pin));
	memset(fresh, 0, sizeof(fresh));

	/* Not asked: said at once. */
	if (error != 0) {
		se_dialog_error(app, "The security key could not be changed.");
		se_log("KEYS key errno=%d", error);
		return;
	}

	/* An addition's last step shows the touch to come. */
	if (keys->step == KEYS_STEP_PIN) {
		keys->step = KEYS_STEP_TOUCH;
		se_dialog_step(app, "Touch the key", 0U, 0U, "When the key asks, touch it (its light blinks). A key held to the reader is touched by being there.", "Add Key", 0);
	}

	/* Busy until the answer (the touch's or the replug's word may come first). */
	keys->asked = 1;
	keys->request = request;
	keys->touch = 0;
	se_dialog_busy(app, busy, cancellable && keys->step == KEYS_STEP_TOUCH);
	se_log("KEYS key request=%u flow=%u step=%u", request, keys->flow, keys->step);
}

/* Takes the answer of a key's change: the next part, the end, or what went wrong. */
static void
keys_result(
	struct se_app *app,
	int error)
{
	struct se_keys *keys;
	char text[SE_MESSAGE];
	const char *plural;

	/* No longer asked; the popup may have been closed meanwhile. */
	keys = &app->keys;
	keys->asked = 0;
	keys->touch = 0;
	se_log("KEYS key result request=%u errno=%d flow=%u step=%u", keys->request, error, keys->flow, keys->step);
	if (!app->dialog.open) {
		se_field_clear(&keys->pin);
		return;
	}

	/* A failure. */
	if (error != 0) {
		keys_failed(app, error);
		return;
	}

	/* An addition's first PIN set: the registration with it, at once. */
	if (keys->flow == SE_KEYS_FLOW_ADD && keys->setting_pin) {
		keys->setting_pin = 0;
		keys->step = KEYS_STEP_PIN;
		se_dialog_step(app, "The key's PIN", 0U, 0U, "", "Add Key", 0);
		se_dialog_field(app, "Key PIN", "", SE_FIELD_SECRET, KEYS_PIN_MAX);
		se_dialog_set_text(app, 0U, keys->pin.text);
		se_field_clear(&keys->pin);
		keys_ask(app);
		return;
	}

	/* A PIN's change. */
	if (keys->flow == SE_KEYS_FLOW_KEY_PIN) {
		keys_end_with(app, "Security key", "The key's PIN is set. The key asks it whenever it signs you in.");
		return;
	}

	/* A reset, with the registrations that went. */
	if (keys->flow == SE_KEYS_FLOW_RESET) {
		keys->removed = kl_system_account_key_removed(app->system);
		plural = "s";
		if (keys->removed == 1U)
			plural = "";
		(void)snprintf(text, sizeof(text), "The key is reset. %u sign-in registration%s on this computer went with it. Add it again to sign in with it.", keys->removed, plural);
		keys_end_with(app, "Security key reset", text);
		return;
	}

	/* A removal, or an addition. */
	if (keys->flow == SE_KEYS_FLOW_REMOVE) {
		keys_end_with(app, "Security key", "The security key is removed.");
		return;
	}
	keys_end_with(app, "Security key", "The security key is added. The login and locked screens take it from now on.");
}

/* Says what went wrong, at the step it sends the user back to. */
static void
keys_failed(
	struct se_app *app,
	int error)
{
	struct se_keys *keys;
	const char *message;
	char reason[KEYS_REASON];
	int refused;
	int same;

	/* The line. */
	keys = &app->keys;
	switch (error) {
	case EPERM:
		message = se_keys_refusal(app, keys->request);
		break;
	case EINVAL:
		message = "The key's name or PIN is not accepted.";
		break;
	case ENOTSUP:
		message = "This desktop cannot use security keys.";
		break;
	case EBUSY:
		message = "Another check is under way, or the screen was locked. Try again in a moment.";
		break;
	default:
		message = "The security key could not be changed.";
		break;
	}

	/* Its word, for where to go back. */
	reason[0] = '\0';
	refused = 0;
	if (error == EPERM)
		refused = kl_system_account_refusal(app->system, keys->request, reason, sizeof(reason));
	se_field_clear(&keys->pin);
	keys->setting_pin = 0;

	/* A wrong key PIN in an addition: the PIN again (the password is kept). */
	same = refused && strcmp(reason, "bad-key-pin") == 0;
	if (same && keys->flow == SE_KEYS_FLOW_ADD && keys->password.length != 0U) {
		keys_step(app, KEYS_STEP_PIN);
		se_dialog_error(app, message);
		return;
	}

	/* A PIN's change: its PINs again. */
	if (keys->flow == SE_KEYS_FLOW_KEY_PIN && keys->info_known) {
		if (keys->info.pin) {
			keys_step(app, KEYS_STEP_CHANGE);
		} else {
			keys_step(app, KEYS_STEP_SET_PIN);
		}
		se_dialog_error(app, message);
		return;
	}

	/* A reset that may have reset the key: said so. */
	same = refused && strcmp(reason, "timeout") == 0;
	if (same && keys->flow == SE_KEYS_FLOW_RESET) {
		keys_end_with(app, "Security key", "The key may or may not have been reset. If it no longer signs in, add it again.");
		return;
	}

	/* Anything else: the first step (the key's, or the password's), with it said. */
	if (keys->flow == SE_KEYS_FLOW_KEY_PIN) {
		keys_step(app, KEYS_STEP_INSERT);
	} else {
		keys_step(app, KEYS_STEP_PASSWORD);
	}
	se_dialog_error(app, message);
}

/* Shows the wizard's end with its text and Done. */
static void
keys_end_with(
	struct se_app *app,
	const char *title,
	const char *text)
{
	/* What was kept goes; the end. */
	se_field_clear(&app->keys.password);
	se_field_clear(&app->keys.pin);
	app->keys.step = KEYS_STEP_DONE;
	se_dialog_step(app, title, 0U, 0U, text, "Done", 0);
	se_dialog_final(app);
}

/*
 * Makes a new key's name: the key's own (as the desktop said it, made a
 * name), else "Security Key", or the first "Security Key N" not yet listed.
 */
static void
keys_default_name(
	struct se_app *app,
	char *name,
	size_t size)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	char own[KL_SYSTEM_KEY_LABEL_MAX + 1U];
	size_t count;
	size_t index;
	size_t length;
	unsigned number;
	int same;
	int used;

	/* The keys listed now. */
	count = 0U;
	if (app->system != NULL)
		count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (count > KL_SYSTEM_KEYS_MAX)
		count = KL_SYSTEM_KEYS_MAX;

	/* The key's own name, without a colon and cut to a label's length. */
	own[0] = '\0';
	if (app->keys.info_known)
		(void)snprintf(own, sizeof(own), "%.*s", (int)(sizeof(own) - 1U), app->keys.info.name);
	length = strlen(own);
	for (index = 0U; index < length; index++) {
		if (own[index] == ':')
			own[index] = ' ';
	}

	/* The own name, then the plain one, then the numbered ones, until one is free (the last tried otherwise). */
	for (number = 0U; number <= KEYS_NAME_TRIES; number++) {
		if (number == 0U && own[0] == '\0')
			continue;
		(void)snprintf(name, size, "%s", own);
		if (number == 1U)
			(void)snprintf(name, size, "%s", KEYS_NAME);
		if (number > 1U)
			(void)snprintf(name, size, "%s %u", KEYS_NAME, number);
		used = 0;
		for (index = 0U; index < count && !used; index++) {
			same = strcmp(keys[index].label, name);
			if (same == 0)
				used = 1;
		}

		/* A free name. */
		if (!used)
			return;
	}
}

/* Keeps the name the popup's field holds, without the spaces before and after it (the session manager drops those). */
static void
keys_keep_name(
	struct se_app *app)
{
	const char *name;
	size_t start;
	size_t end;

	/* The text, its spaces skipped at both ends. */
	name = se_dialog_text(app, 0U);
	start = 0U;
	while (name[start] == ' ')
		start++;
	end = strlen(name);
	while (end > start && name[end - 1U] == ' ')
		end--;

	/* Kept. */
	(void)snprintf(app->keys.name, sizeof(app->keys.name), "%.*s", (int)(end - start), name + start);
}

/* Tells whether a key's name may be kept: 1 to 32 bytes, not spaces alone, no colon and no control character. */
static int
keys_name_valid(
	const char *name,
	size_t length)
{
	size_t index;
	unsigned char character;
	int visible;

	/* Its length. */
	if (length == 0U || length > KL_SYSTEM_KEY_LABEL_MAX)
		return 0;

	/* Each byte; one that is not a space makes it a name. */
	visible = 0;
	for (index = 0U; index < length; index++) {
		character = (unsigned char)name[index];
		if (character == ':' || character < 0x20U || character == 0x7fU)
			return 0;
		if (character != ' ')
			visible = 1;
	}

	/* A name, when it has more than spaces. */
	return visible;
}

/* Counts a UTF-8 text's code points (the bytes that do not continue one). */
static size_t
keys_code_points(
	const char *text)
{
	size_t count;
	size_t index;

	/* Each byte that starts a code point. */
	count = 0U;
	for (index = 0U; text[index] != '\0'; index++) {
		if (((unsigned char)text[index] & 0xc0U) != 0x80U)
			count++;
	}

	/* The count. */
	return count;
}

/* Tells whether the popup's new PIN (field first) and its repeat (field again) fit the key: its fewest characters, at most 63 bytes, the same twice. */
static int
keys_pin_ok(
	const struct se_app *app,
	unsigned first,
	unsigned again)
{
	const char *pin;
	const char *repeat;
	unsigned minimum;
	size_t points;
	size_t length;
	int same;

	/* Long enough, not too long. */
	pin = se_dialog_text(app, first);
	repeat = se_dialog_text(app, again);
	minimum = app->keys.info.min;
	if (minimum < KEYS_PIN_MIN)
		minimum = KEYS_PIN_MIN;
	points = keys_code_points(pin);
	length = se_dialog_length(app, first);
	if (points < minimum || length > KEYS_PIN_MAX)
		return 0;

	/* The same twice. */
	same = strcmp(pin, repeat);
	return same == 0;
}
