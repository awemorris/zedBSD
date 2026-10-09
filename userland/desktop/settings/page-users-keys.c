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
 * on zedBSD), each with Remove, and has Add Key.  Both open the popup
 * (dialog.c).  Adding is a wizard: the account's password, the key's name
 * (a name not yet used, which the user may change), the key's own PIN,
 * then the desktop is asked to register the one key plugged in or held to
 * the reader (kl_system_account_add_key) and the popup waits for the
 * user's touch (KL_SYSTEM_CHANGED_TOUCH says the key asks for it).
 * Removing asks the password alone (kl_system_account_remove_key).  The
 * password is kept from its step to the one that sends it, and wiped
 * then, when the popup closes and when it is left alone; the key's PIN
 * goes as it is sent.  A desktop without the keys (KL_SYSTEM_HAS_KEYS)
 * says so.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The card's controls: Add Key, and each key's Remove. */
#define KEYS_ADD		310
#define KEYS_REMOVE_FIRST	320

/* The steps of an addition: the password, the name, the key's PIN, and the touch; a removal has the password alone. */
#define KEYS_STEP_PASSWORD	1U
#define KEYS_STEP_NAME		2U
#define KEYS_STEP_PIN		3U
#define KEYS_STEP_TOUCH		4U
#define KEYS_STEPS		4U

/* The fewest characters of a key's PIN (CTAP 2.1), and the most bytes (CTAP: 63). */
#define KEYS_PIN_MIN		4U
#define KEYS_PIN_MAX		63U

/* The name a new key gets, and how many numbered ones are tried after it. */
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

static int keys_available(const struct se_app *app);
static int keys_card_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void keys_start(struct se_app *app, unsigned flow, const struct kl_system_key *key);
static void keys_step(struct se_app *app, unsigned step);
static void keys_act(struct se_app *app, unsigned action);
static int keys_ready(const struct se_app *app);
static void keys_ask(struct se_app *app);
static void keys_result(struct se_app *app, int error);
static void keys_default_name(struct se_app *app, char *name, size_t size);
static int keys_name_valid(const char *name, size_t length);
static void keys_keep_name(struct se_app *app);

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

	/* The Software Security Key's. */
	taken = se_keys_soft_press(app, index);
	if (taken)
		return;

	/* One change at a time. */
	busy = app->keys.asked || app->dialog.open;
	if (busy)
		return;

	/* Add Key, while there is room for one more. */
	count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (index == KEYS_ADD) {
		if (count < KL_SYSTEM_KEYS_MAX)
			keys_start(app, SE_KEYS_FLOW_ADD, NULL);
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
 * Takes the answer of a change the page asked (a key's, or the PIN's).
 * Returns 1 when the request was the page's.
 */
int
se_keys_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_keys *keys;

	/* Only the change the page asked. */
	keys = &app->keys;
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
 * the page's addition.
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

	/* Said in the popup. */
	keys->touch = 1;
	if (app->dialog.open)
		se_dialog_busy(app, "Touch your security key, or hold it to the reader.", 0);
	se_log("KEYS touch request=%u", request);
}

/* Ends the wizard under way: the popup closed, the password kept wiped (a change asked still gets its answer). */
void
se_keys_end(
	struct se_app *app)
{
	struct se_keys *keys;

	/* The password, and the popup. */
	keys = &app->keys;
	se_field_clear(&keys->password);
	se_dialog_close(app);

	/* No step; the change asked, if any, stays asked. */
	keys->step = 0U;
	keys->touch = 0;
	if (!keys->asked)
		keys->flow = SE_KEYS_FLOW_NONE;
}

/* Wipes what the page keeps at the window's end. */
void
se_keys_close(
	struct se_app *app)
{
	/* The password and the popup's fields. */
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
		{ "bad-secret", "The password or the key's PIN is wrong." },
		{ "locked", "Too many wrong attempts, or the key's PIN is locked. Wait, then try again." },
		{ "no-key", "No security key was found. Plug it in, or hold it to the reader." },
		{ "many-keys", "More than one key is there. Leave only the key to add." },
		{ "key-locked", "The security key is locked: too many wrong PINs." },
		{ "timeout", "The key was not touched in time." },
		{ "locked-account", "Your account is locked: it cannot have a security key." },
		{ "device", "The security key did not answer. It needs a PIN of its own." },
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

/* Draws the keys card from a top edge: how many, each with Remove, and Add Key; returns the edge below it. */
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
	enabled = !app->keys.asked && !app->dialog.open;
	button = se_button_width(app, "Remove");
	for (index = 0U; index < count; index++) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(KEYS_TEXT_ROW, y + 4, 36), keys[index].label, KEYS_TEXT_ROW, 0, width - 60 - button, SE_COLOR_TEXT);
		(void)se_button_draw(app, canvas, right - button, y + 6, "Remove", 0, enabled, KEYS_REMOVE_FIRST + (int)index);
		y += KEYS_KEY_ROW;
	}

	/* Add Key at the right, while there is room for one more (the session manager keeps KL_SYSTEM_KEYS_MAX). */
	button = se_button_width(app, "Add Key");
	(void)se_button_draw(app, canvas, right - button, y + 8, "Add Key", 1, enabled && count < KL_SYSTEM_KEYS_MAX, KEYS_ADD);

	/* The edge below the card. */
	return top + height;
}

/* Starts a key's wizard at the password: an addition, or the removal of a key. */
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
	if (key != NULL) {
		(void)snprintf(keys->ref, sizeof(keys->ref), "%s", key->ref);
		(void)snprintf(keys->label, sizeof(keys->label), "%s", key->label);
	}

	/* An addition's name to start from. */
	if (flow == SE_KEYS_FLOW_ADD)
		keys_default_name(app, keys->name, sizeof(keys->name));

	/* The popup, its owner this page. */
	se_dialog_open(app, keys_act, keys_ready);
	keys_step(app, KEYS_STEP_PASSWORD);
	se_log("KEYS key start remove=%d", flow == SE_KEYS_FLOW_REMOVE);
}

/* Shows a step of a key's wizard. */
static void
keys_step(
	struct se_app *app,
	unsigned step)
{
	struct se_keys *keys;
	char body[SE_MESSAGE];

	/* The step. */
	keys = &app->keys;
	keys->step = step;

	/* A removal: the password alone. */
	if (keys->flow == SE_KEYS_FLOW_REMOVE) {
		se_field_clear(&keys->password);
		(void)snprintf(body, sizeof(body), "Remove \"%s\"? It will no longer sign in or unlock on this computer. Type your password to remove it.", keys->label);
		se_dialog_step(app, "Remove the security key", 1U, 1U, body, "Remove", 0);
		se_dialog_field(app, "Password", "Your password now", SE_FIELD_SECRET, 0U);
		return;
	}

	/* An addition's steps. */
	switch (step) {
	case KEYS_STEP_PASSWORD:
		se_field_clear(&keys->password);
		se_dialog_step(app, "Add a security key", KEYS_STEP_PASSWORD, KEYS_STEPS, "Type your password to add a security key to your account.", "Next", 0);
		se_dialog_field(app, "Password", "Your password now", SE_FIELD_SECRET, 0U);
		break;
	case KEYS_STEP_NAME:
		se_dialog_step(app, "Name the key", KEYS_STEP_NAME, KEYS_STEPS, "Give the key a name you will know it by in this list.", "Next", 1);
		se_dialog_field(app, "Name", "YubiKey, for example", SE_FIELD_TEXT, KL_SYSTEM_KEY_LABEL_MAX);
		se_dialog_set_text(app, 0U, keys->name);
		break;
	default:
		se_dialog_step(app, "The key's PIN", KEYS_STEP_PIN, KEYS_STEPS,
		    "Plug in the key, or hold it to the reader, and type the key's own PIN. A new key needs a PIN first: run fidoctl set-pin in Terminal.", "Add Key", 1);
		se_dialog_field(app, "Key PIN", "The key's own PIN", SE_FIELD_SECRET, KEYS_PIN_MAX);
		break;
	}
}

/*
 * Takes the popup's action for a key's wizard: the step's button goes on
 * (or asks), Back goes back, Cancel closes, and idle wipes the password
 * kept and asks it again.
 */
static void
keys_act(
	struct se_app *app,
	unsigned action)
{
	struct se_keys *keys;

	/* Cancel (and Close after the end): nothing kept. */
	keys = &app->keys;
	if (action == SE_DIALOG_CANCEL) {
		se_keys_end(app);
		return;
	}

	/* Back: the step before (the name kept). */
	if (action == SE_DIALOG_BACK) {
		if (keys->step == KEYS_STEP_NAME)
			keys_keep_name(app);

		/* The step before, if any. */
		if (keys->step > KEYS_STEP_PASSWORD)
			keys_step(app, keys->step - 1U);
		return;
	}

	/* Left alone: the password kept goes, and is asked again. */
	if (action == SE_DIALOG_IDLE) {
		if (keys->step > KEYS_STEP_PASSWORD && !keys->asked) {
			keys_step(app, KEYS_STEP_PASSWORD);
			se_dialog_error(app, "For your security, type your password again.");
		}

		/* Nothing else. */
		return;
	}

	/* Only the step's button from here. */
	if (action != SE_DIALOG_PRIMARY)
		return;

	/* The end's Done closes. */
	if (keys->step == 0U) {
		se_keys_end(app);
		return;
	}

	/* The password: kept, then the name (or, for a removal, asked). */
	if (keys->step == KEYS_STEP_PASSWORD) {
		se_keys_keep_password(app);
		if (keys->flow == SE_KEYS_FLOW_REMOVE) {
			keys_ask(app);
			return;
		}

		/* An addition's name. */
		keys_step(app, KEYS_STEP_NAME);
		return;
	}

	/* The name: kept, then the key's PIN. */
	if (keys->step == KEYS_STEP_NAME) {
		keys_keep_name(app);
		keys_step(app, KEYS_STEP_PIN);
		return;
	}

	/* The key's PIN: asked. */
	keys_ask(app);
}

/* Tells whether the popup's step may go on: the password typed, a name that is one, a PIN long enough. */
static int
keys_ready(
	const struct se_app *app)
{
	size_t length;

	/* The end's Done. */
	if (app->keys.step == 0U)
		return 1;

	/* The step's field. */
	length = se_dialog_length(app, 0U);
	switch (app->keys.step) {
	case KEYS_STEP_PASSWORD:
		return length != 0U;
	case KEYS_STEP_NAME:
		return keys_name_valid(se_dialog_text(app, 0U), length);
	default:
		return length >= KEYS_PIN_MIN && length <= KEYS_PIN_MAX;
	}
}

/* Asks the desktop to add the key (with the password, the name and the PIN) or to remove one (with the password), and wipes the secrets. */
static void
keys_ask(
	struct se_app *app)
{
	struct se_keys *keys;
	char pin[KEYS_PIN_MAX + 1U];
	uint32_t request;
	int error;

	/* Asked; the secrets leave with the next flush. */
	keys = &app->keys;
	if (keys->flow == SE_KEYS_FLOW_REMOVE) {
		error = kl_system_account_remove_key(app->system, keys->password.text, keys->ref, &request);
	} else {
		(void)snprintf(pin, sizeof(pin), "%s", se_dialog_text(app, 0U));
		error = kl_system_account_add_key(app->system, keys->password.text, keys->name, pin, &request);
		memset(pin, 0, sizeof(pin));
	}

	/* The password goes either way. */
	se_field_clear(&keys->password);

	/* Not asked: said at once, at the password. */
	if (error != 0) {
		keys_step(app, KEYS_STEP_PASSWORD);
		se_dialog_error(app, "The security key could not be changed.");
		se_log("KEYS key errno=%d", error);
		return;
	}

	/* Succeeded: busy until the answer (the touch's word may come first). */
	keys->asked = 1;
	keys->request = request;
	keys->touch = 0;
	if (keys->flow == SE_KEYS_FLOW_REMOVE) {
		se_dialog_busy(app, "Checking the password...", 0);
	} else {
		keys->step = KEYS_STEP_TOUCH;
		se_dialog_step(app, "Touch the key", KEYS_STEP_TOUCH, KEYS_STEPS, "When the key asks, touch it (its light blinks). A key held to the reader is touched by being there.", "Add Key", 1);
		se_dialog_busy(app, "Checking the password and the key...", 0);
	}

	/* Logged for the tests (without a secret). */
	se_log("KEYS key request=%u remove=%d", request, keys->flow == SE_KEYS_FLOW_REMOVE);
}

/* Takes the answer of a key's change: the end, or the password's step again with what went wrong. */
static void
keys_result(
	struct se_app *app,
	int error)
{
	struct se_keys *keys;
	const char *message;
	const char *done;
	int removing;

	/* No longer asked; the popup may have been closed meanwhile. */
	keys = &app->keys;
	keys->asked = 0;
	keys->touch = 0;
	removing = keys->flow == SE_KEYS_FLOW_REMOVE;
	se_log("KEYS key result request=%u errno=%d remove=%d", keys->request, error, removing);
	if (!app->dialog.open)
		return;

	/* Done: the end. */
	if (error == 0) {
		done = "The security key is added. The login and locked screens take it from now on.";
		if (removing)
			done = "The security key is removed.";
		keys->step = 0U;
		se_dialog_step(app, "Security key", 0U, 0U, done, "Done", 0);
		return;
	}

	/* What went wrong. */
	switch (error) {
	case EPERM:
		message = se_keys_refusal(app, keys->request);
		break;
	case EINVAL:
		message = "The key's name is not accepted: at most 32 characters, no colon.";
		break;
	case ENOTSUP:
		message = "This desktop cannot use security keys.";
		break;
	case EBUSY:
		message = "Another check is under way. Try again in a moment.";
		break;
	default:
		message = "The security key could not be changed.";
		break;
	}

	/* The password again (the name kept), with it said. */
	keys_step(app, KEYS_STEP_PASSWORD);
	se_dialog_error(app, message);
}

/* Makes a new key's name: "Security Key", or the first "Security Key N" not yet listed. */
static void
keys_default_name(
	struct se_app *app,
	char *name,
	size_t size)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	size_t count;
	size_t index;
	unsigned number;
	int same;
	int used;

	/* The keys listed now. */
	count = 0U;
	if (app->system != NULL)
		count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (count > KL_SYSTEM_KEYS_MAX)
		count = KL_SYSTEM_KEYS_MAX;

	/* The plain name, then the numbered ones, until one is free (the last tried otherwise). */
	for (number = 1U; number <= KEYS_NAME_TRIES; number++) {
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
