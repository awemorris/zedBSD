/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Users page's security keys card (ws172-p003;
 * docs/architecture/security.md, "The security key"): the FIDO2 keys
 * that log in and unlock on this machine's screen.
 *
 * The card lists the user's keys as the desktop tells them
 * (kl_system_account_keys: the session manager keeps them in /etc/passkey
 * on zedBSD), each with Remove, and has three fields: the current
 * password, a name for a new key, and the key's own PIN.  Add Security
 * Key asks the desktop to register the one key plugged in
 * (kl_system_account_add_key); while it waits for the user's touch the
 * card says so (KL_SYSTEM_CHANGED_TOUCH).  Remove asks with the password
 * alone (kl_system_account_remove_key).  The secrets are wiped as soon as
 * the change is asked; the answer comes as a line under the buttons.  A
 * desktop without the keys (KL_SYSTEM_HAS_KEYS) says so.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The card's controls: its three fields, Add Security Key, and each key's Remove. */
#define KEYS_FIELD_FIRST	300
#define KEYS_ADD		310
#define KEYS_REMOVE_FIRST	320

/* The fields: the current password, the key's name, the key's own PIN. */
#define KEYS_PASSWORD		0
#define KEYS_LABEL		1
#define KEYS_PIN		2

/* The fewest characters of a key's PIN (CTAP 2.1). */
#define KEYS_PIN_MIN		4U

/* A key's row, a field's row, the field's left edge in its row, and the line under the buttons. */
#define KEYS_KEY_ROW		44
#define KEYS_ROW		52
#define KEYS_FIELD_X		210
#define KEYS_MESSAGE_LINE	30

/* The text sizes of a row and of a message. */
#define KEYS_TEXT_ROW		15U
#define KEYS_TEXT_SUB		13U

/* The room of a refusal's word. */
#define KEYS_REASON		32U

/* The fields' labels, placeholders and kinds. */
static const char *const keys_labels[SE_KEY_FIELDS] = { "Current password", "Key name", "Key PIN" };
static const char *const keys_placeholders[SE_KEY_FIELDS] = { "Your password now", "YubiKey, for example", "The key's own PIN" };
static const unsigned keys_kinds[SE_KEY_FIELDS] = { SE_FIELD_SECRET, SE_FIELD_TEXT, SE_FIELD_SECRET };

static int keys_available(const struct se_app *app);
static int keys_add_ready(const struct se_app *app);
static void keys_ask(struct se_app *app, const char *ref);
static void keys_field_draw(struct se_app *app, struct kl_canvas *canvas, int index, int x, int y, int width);
static const char *keys_refusal(struct se_app *app, uint32_t request);

/*
 * Draws the security keys card from a top edge; returns the edge below it.
 */
int
se_users_keys_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	struct se_users *users;
	char state[SE_MESSAGE];
	const char *hint;
	kl_color ink;
	size_t count;
	size_t index;
	int available;
	int enabled;
	int button;
	int right;
	int height;
	int y;

	/* Without the keys, a short card that says so. */
	users = &app->users;
	available = keys_available(app);
	count = 0U;
	if (available)
		count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (count > KL_SYSTEM_KEYS_MAX)
		count = KL_SYSTEM_KEYS_MAX;
	height = 64 + 36 + (int)count * KEYS_KEY_ROW + SE_KEY_FIELDS * KEYS_ROW + 60 + KEYS_MESSAGE_LINE;
	if (!available)
		height = 64 + 50;
	y = se_card_begin(app, canvas, x, top, width, height, "Security keys", "Log in and unlock with a FIDO2 security key.");
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

	/* Each key: its name, and Remove at the right while the password is typed. */
	right = x + width - 20;
	enabled = !users->key_asked && users->key_fields[KEYS_PASSWORD].length != 0;
	button = se_button_width(app, "Remove");
	for (index = 0U; index < count; index++) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(KEYS_TEXT_ROW, y + 4, 36), keys[index].label, KEYS_TEXT_ROW, 0, width - 60 - button, SE_COLOR_TEXT);
		(void)se_button_draw(app, canvas, right - button, y + 6, "Remove", 0, enabled, KEYS_REMOVE_FIRST + (int)index);
		y += KEYS_KEY_ROW;
	}

	/* Each field. */
	for (index = 0U; index < SE_KEY_FIELDS; index++) {
		keys_field_draw(app, canvas, (int)index, x, y, width);
		y += KEYS_ROW;
	}

	/* Add Security Key at the right. */
	button = se_button_width(app, "Add Security Key");
	enabled = keys_add_ready(app);
	(void)se_button_draw(app, canvas, right - button, y + 12, "Add Security Key", 1, enabled, KEYS_ADD);

	/* The wait for the touch, or the last answer (green when it was done, red when it failed). */
	y += 60;
	ink = SE_COLOR_GOOD;
	if (users->key_bad)
		ink = SE_COLOR_BAD;
	if (users->key_touch)
		ink = SE_COLOR_TEXT;
	if (users->key_message[0] != '\0')
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, users->key_message, KEYS_TEXT_SUB, 0, width - 40, ink);

	/*
	 * With no answer to show and Add not ready, what Add needs: a new key
	 * (a YubiKey as it comes) has no PIN, and without one it cannot be
	 * added here, which the disabled button alone did not say (BUG-279).
	 */
	hint = NULL;
	if (users->key_message[0] == '\0' && !enabled) {
		hint = "Type your password and a name for the key, then its PIN.";
		if (users->key_fields[KEYS_PIN].length < KEYS_PIN_MIN)
			hint = "Type the key's PIN. A new key needs one first: run fidoctl set-pin in Terminal.";
	}
	if (hint != NULL)
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, hint, KEYS_TEXT_SUB, 0, width - 40, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card. */
	return top + height;
}

/*
 * Carries out a click on a control of the keys card.  Returns 1 when the
 * control was the card's.
 */
int
se_users_keys_press(
	struct se_app *app,
	int index)
{
	struct kl_system_key keys[KL_SYSTEM_KEYS_MAX];
	struct se_users *users;
	size_t count;
	int ready;

	/* A field takes the keyboard. */
	users = &app->users;
	if (index >= KEYS_FIELD_FIRST && index < KEYS_FIELD_FIRST + SE_KEY_FIELDS) {
		users->key_focus = index - KEYS_FIELD_FIRST;
		users->keyboard = SE_USERS_KEYBOARD_KEYS;
		return 1;
	}

	/* Add Security Key, when the fields are ready. */
	if (index == KEYS_ADD) {
		ready = keys_add_ready(app);
		if (ready)
			keys_ask(app, NULL);
		return 1;
	}

	/* Not the card's. */
	if (index < KEYS_REMOVE_FIRST || index >= KEYS_REMOVE_FIRST + (int)KL_SYSTEM_KEYS_MAX)
		return 0;

	/* A key's Remove, when the password is typed and the key is still listed. */
	count = kl_system_account_keys(app->system, keys, KL_SYSTEM_KEYS_MAX);
	if (users->key_asked || users->key_fields[KEYS_PASSWORD].length == 0 || (size_t)(index - KEYS_REMOVE_FIRST) >= count)
		return 1;
	keys_ask(app, keys[index - KEYS_REMOVE_FIRST].ref);
	return 1;
}

/*
 * Takes a key while the keys card's fields have the keyboard: Tab moves
 * between them, Enter asks the addition when the fields are ready and
 * otherwise goes to the next field, Esc empties the fields (or gives the
 * keyboard back to the password card), the others type.  Returns 1 when
 * the key was used.
 */
int
se_users_keys_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_users *users;
	int empty;
	int ready;
	int used;

	/* Not the card's keyboard. */
	users = &app->users;
	if (users->keyboard != SE_USERS_KEYBOARD_KEYS)
		return 0;

	/* Tab and Shift+Tab. */
	if (event->key == SE_KEY_TAB) {
		if ((event->modifiers & SE_MOD_SHIFT) != 0U) {
			users->key_focus = (users->key_focus + SE_KEY_FIELDS - 1) % SE_KEY_FIELDS;
		} else {
			users->key_focus = (users->key_focus + 1) % SE_KEY_FIELDS;
		}

		/* Taken. */
		return 1;
	}

	/* Enter: the addition, or the next field. */
	if (event->key == SE_KEY_ENTER) {
		ready = keys_add_ready(app);
		if (ready) {
			keys_ask(app, NULL);
		} else {
			users->key_focus = (users->key_focus + 1) % SE_KEY_FIELDS;
		}

		/* Taken. */
		return 1;
	}

	/* Esc empties the fields; with nothing typed the keyboard goes back to the password card. */
	if (event->key == SE_KEY_ESC) {
		empty = users->key_fields[KEYS_PASSWORD].length == 0 && users->key_fields[KEYS_LABEL].length == 0 &&
		    users->key_fields[KEYS_PIN].length == 0;
		if (empty)
			users->keyboard = SE_USERS_KEYBOARD_PASSWORD;
		se_users_keys_wipe(users);
		users->key_message[0] = '\0';
		return 1;
	}

	/* Anything else types into the field with the keyboard. */
	used = se_field_key(&users->key_fields[users->key_focus], event);
	if (used == 0)
		return 0;

	/* A new character takes the last answer away. */
	if (!users->key_asked)
		users->key_message[0] = '\0';

	/* Succeeded: the field took the key. */
	return 1;
}

/*
 * Takes the answer of a key's change when it is the card's.  Returns 1
 * when the request was the card's.
 */
int
se_users_keys_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_users *users;
	const char *message;
	int bad;

	/* Only the change the card asked. */
	users = &app->users;
	if (!users->key_asked || request != users->key_request)
		return 0;
	users->key_asked = 0;
	users->key_touch = 0;

	/* What the answer says. */
	bad = 1;
	switch (error) {
	case 0:
		message = "The security key is registered. The login and locked screens take it from now on.";
		if (users->key_removing)
			message = "The security key is removed.";
		bad = 0;
		break;
	case EPERM:
		/* The refusal's word, when the desktop gave one. */
		message = keys_refusal(app, request);
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

	/* Shown under the button, and logged for the tests (without a secret). */
	(void)snprintf(users->key_message, sizeof(users->key_message), "%s", message);
	users->key_bad = bad;
	app->dirty = 1;
	se_log("USERS key result request=%u errno=%d remove=%d", request, error, users->key_removing);

	/* Succeeded: the answer was the card's. */
	return 1;
}

/*
 * Takes the desktop's word that a key waits to be touched, when it is for
 * the card's addition.
 */
void
se_users_keys_touched(
	struct se_app *app)
{
	struct se_users *users;
	uint32_t request;
	int taken;

	/* The touch's request, once. */
	users = &app->users;
	if (app->system == NULL)
		return;
	taken = kl_system_account_touched(app->system, &request);
	if (!taken || !users->key_asked || request != users->key_request)
		return;

	/* Said under the button. */
	users->key_touch = 1;
	(void)snprintf(users->key_message, sizeof(users->key_message), "%s", "Touch your security key.");
	users->key_bad = 0;
	app->dirty = 1;
	se_log("USERS key touch request=%u", request);
}

/*
 * Wipes the keys card's fields.
 */
void
se_users_keys_wipe(
	struct se_users *users)
{
	int index;

	/* Each field (se_field_clear overwrites its text). */
	for (index = 0; index < SE_KEY_FIELDS; index++)
		se_field_clear(&users->key_fields[index]);
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

/* Tells whether the fields are ready to add a key: the password, a name and a PIN of four or more, and none asked. */
static int
keys_add_ready(
	const struct se_app *app)
{
	const struct se_users *users;
	const char *colon;

	/* One change at a time. */
	users = &app->users;
	if (users->key_asked)
		return 0;

	/* The password, a name without a colon of at most 32 bytes, and the key's PIN. */
	if (users->key_fields[KEYS_PASSWORD].length == 0)
		return 0;
	if (users->key_fields[KEYS_LABEL].length == 0 || users->key_fields[KEYS_LABEL].length > KL_SYSTEM_KEY_LABEL_MAX)
		return 0;
	colon = strchr(users->key_fields[KEYS_LABEL].text, ':');
	if (colon != NULL)
		return 0;
	if (users->key_fields[KEYS_PIN].length < KEYS_PIN_MIN)
		return 0;

	/* Ready. */
	return 1;
}

/* Asks the desktop to add the key plugged in (ref NULL) or to remove the key of a reference, and wipes the secrets. */
static void
keys_ask(
	struct se_app *app,
	const char *ref)
{
	struct se_users *users;
	uint32_t request;
	int error;

	/* Asked; the secrets leave with the next flush. */
	users = &app->users;
	if (ref == NULL) {
		error = kl_system_account_add_key(app->system, users->key_fields[KEYS_PASSWORD].text, users->key_fields[KEYS_LABEL].text,
		    users->key_fields[KEYS_PIN].text, &request);
	} else {
		error = kl_system_account_remove_key(app->system, users->key_fields[KEYS_PASSWORD].text, ref, &request);
	}

	/* The secrets go at once. */
	se_users_keys_wipe(users);
	users->key_focus = KEYS_PASSWORD;

	/* Not asked: said at once. */
	if (error != 0) {
		(void)snprintf(users->key_message, sizeof(users->key_message), "The security key could not be changed (%s).", strerror(error));
		users->key_bad = 1;
		se_log("USERS key errno=%d", error);
		return;
	}

	/* Succeeded: the answer comes as a result. */
	users->key_asked = 1;
	users->key_removing = ref != NULL;
	users->key_touch = 0;
	users->key_request = request;
	(void)snprintf(users->key_message, sizeof(users->key_message), "%s", "Checking...");
	users->key_bad = 0;
	se_log("USERS key request=%u remove=%d", request, users->key_removing);
}

/* Draws one field's row: its label at the left, the field at the right, and the cursor in the field with the keyboard. */
static void
keys_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int index,
	int x,
	int y,
	int width)
{
	struct se_users *users;
	struct kl_rect box;
	int focused;

	/* The label. */
	users = &app->users;
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(KEYS_TEXT_ROW, y + 8, 36), keys_labels[index], KEYS_TEXT_ROW, 0, KEYS_FIELD_X - 30, SE_COLOR_TEXT);

	/* The field's place, and whether it has the keyboard. */
	box.x = x + KEYS_FIELD_X;
	box.y = y + 8;
	box.width = width - KEYS_FIELD_X - 20;
	box.height = 36;
	focused = 0;
	if (users->keyboard == SE_USERS_KEYBOARD_KEYS && users->key_focus == index)
		focused = 1;

	/* A click on it gives it the keyboard. */
	se_ui_hit(app, &box, SE_HIT_CONTROL, KEYS_FIELD_FIRST + index);

	/* libkeiland's field: the password and the PIN as dots. */
	(void)se_field_draw(app, canvas, &users->key_fields[index], &box, keys_placeholders[index], keys_kinds[index], focused);
}

/* Gives the line for a refusal by its word (a wrong password, no key, two keys, a locked key, no touch). */
static const char *
keys_refusal(
	struct se_app *app,
	uint32_t request)
{
	static const struct {
		const char *word;
		const char *line;
	} lines[] = {
		{ "bad-secret", "The password or the key's PIN is wrong." },
		{ "no-key", "No security key is plugged in, or it is not this account's." },
		{ "many-keys", "Plug in only the key to register." },
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
