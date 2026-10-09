/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Security Keys page's Software Security Key (ws199-p001 section 3.1;
 * before it the Users page's PIN card, ws163-p003, ws172-p002): the
 * six-digit PIN that logs in and unlocks on this machine's screen, a key
 * with no device.
 *
 * The card says whether a PIN is set, as the desktop tells it
 * (kl_system_account_enrolled: the session manager keeps the PIN, in
 * /etc/passkey on zedBSD, which Settings cannot read), and has Set Up PIN
 * (Change PIN when one is set) and Remove.  Each opens the popup
 * (dialog.c): the current password first, then for a new PIN the six
 * digits twice; the desktop is asked with the password and the PIN
 * (libkeiland's kl_system_account_set_pin, an empty PIN to remove it),
 * and the session manager checks the password.  The password is kept from
 * its step to the one that sends it, and wiped then, when the popup closes
 * and when it is left alone.  A desktop without the PIN
 * (KL_SYSTEM_HAS_PIN) says so and offers no change.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The card's controls: Set Up PIN (or Change PIN), and Remove. */
#define SOFT_APPLY		210
#define SOFT_REMOVE		211

/* The digits of a PIN. */
#define SOFT_DIGITS		6U

/* The steps of a new PIN (the password, the PIN), and of a removal (the password). */
#define SOFT_STEP_PASSWORD	1U
#define SOFT_STEP_PIN		2U
#define SOFT_STEPS		2U

/* The text sizes of a row. */
#define SOFT_TEXT_SUB		13U
#define SOFT_TEXT_ROW		15U

/* The room of a refusal's word. */
#define SOFT_REASON		32U

static int soft_available(const struct se_app *app);
static int soft_is_set(const struct se_app *app, int *set);
static void soft_start(struct se_app *app, unsigned flow);
static void soft_step(struct se_app *app, unsigned step);
static void soft_ask(struct se_app *app);

/*
 * Draws the Software Security Key card from a top edge; returns the edge
 * below it.
 */
int
se_keys_soft_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *apply;
	const char *state;
	int available;
	int busy;
	int set;
	int known;
	int apply_width;
	int remove_width;
	int right;
	int height;
	int y;

	/* Without the PIN, a short card that says so. */
	available = soft_available(app);
	height = 64 + 36 + 56;
	if (!available)
		height = 64 + 50;
	y = se_card_begin(app, canvas, x, top, width, height, "Software Security Key", "A six-digit PIN that signs in and unlocks on this computer's screen.");
	if (!available) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 24, "This desktop cannot set a PIN here.", SOFT_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* Whether a PIN is set now, once the desktop has told it. */
	known = soft_is_set(app, &set);
	state = "Looking for your PIN...";
	if (known && !set)
		state = "No PIN is set. The login and locked screens take your password.";
	if (known && set)
		state = "A PIN is set. After a restart, sign in once with your password.";
	(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, state, SOFT_TEXT_SUB, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
	y += 36;

	/* The buttons at the right: Set Up PIN (Change PIN), and Remove left of it when one is set; none while a change is under way. */
	right = x + width - 20;
	apply = "Set Up PIN";
	if (set)
		apply = "Change PIN";
	busy = app->keys.asked || app->dialog.open;
	apply_width = se_button_width(app, apply);
	(void)se_button_draw(app, canvas, right - apply_width, y + 8, apply, 1, known && !busy, SOFT_APPLY);
	if (set) {
		remove_width = se_button_width(app, "Remove");
		(void)se_button_draw(app, canvas, right - apply_width - 8 - remove_width, y + 8, "Remove", 0, !busy, SOFT_REMOVE);
	}

	/* The edge below the card. */
	return top + height;
}

/*
 * Carries out a click on a control of the card.  Returns 1 when the
 * control was the card's.
 */
int
se_keys_soft_press(
	struct se_app *app,
	int index)
{
	int set;
	int known;

	/* Not the card's. */
	if (index != SOFT_APPLY && index != SOFT_REMOVE)
		return 0;

	/* One change at a time, once the desktop has told whether a PIN is set. */
	known = soft_is_set(app, &set);
	if (!known || app->keys.asked || app->dialog.open)
		return 1;

	/* Set Up PIN, Change PIN or Remove. */
	if (index == SOFT_APPLY) {
		soft_start(app, SE_KEYS_FLOW_PIN_SET);
	} else if (set) {
		soft_start(app, SE_KEYS_FLOW_PIN_REMOVE);
	}

	/* Taken. */
	return 1;
}

/*
 * Takes the popup's action for the PIN's wizards: the step's button goes
 * on (or asks), Back goes back, Cancel closes, and idle wipes the
 * password kept and asks it again.
 */
void
se_keys_soft_act(
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

	/* Back: the password again. */
	if (action == SE_DIALOG_BACK) {
		soft_step(app, SOFT_STEP_PASSWORD);
		return;
	}

	/* Left alone: the password kept goes, and is asked again. */
	if (action == SE_DIALOG_IDLE) {
		if (keys->step > SOFT_STEP_PASSWORD && !keys->asked) {
			soft_step(app, SOFT_STEP_PASSWORD);
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

	/* The password: kept, then the PIN (or, for a removal, asked). */
	if (keys->step == SOFT_STEP_PASSWORD) {
		se_keys_keep_password(app);
		if (keys->flow == SE_KEYS_FLOW_PIN_REMOVE) {
			soft_ask(app);
			return;
		}

		/* A new PIN's digits. */
		soft_step(app, SOFT_STEP_PIN);
		return;
	}

	/* The PIN: asked. */
	soft_ask(app);
}

/* Tells whether the popup's step may go on: the password typed, or the new PIN whole and twice the same. */
int
se_keys_soft_ready(
	const struct se_app *app)
{
	const char *pin;
	const char *again;
	size_t length;
	int same;

	/* The end's Done. */
	if (app->keys.step == 0U)
		return 1;

	/* The password typed. */
	length = se_dialog_length(app, 0U);
	if (app->keys.step == SOFT_STEP_PASSWORD)
		return length != 0U;

	/* The new PIN of six digits, twice the same. */
	if (length != SOFT_DIGITS)
		return 0;
	pin = se_dialog_text(app, 0U);
	again = se_dialog_text(app, 1U);
	same = strcmp(pin, again);
	if (same != 0)
		return 0;

	/* Ready. */
	return 1;
}

/*
 * Takes the answer of a PIN's change: the end, or the password's step
 * again with what went wrong.
 */
void
se_keys_soft_result(
	struct se_app *app,
	int error)
{
	struct se_keys *keys;
	const char *message;
	const char *done;
	char reason[SOFT_REASON];
	int refused;
	int locked;
	int removing;

	/* No longer asked; the popup may have been closed meanwhile. */
	keys = &app->keys;
	keys->asked = 0;
	removing = keys->flow == SE_KEYS_FLOW_PIN_REMOVE;
	se_log("KEYS pin result request=%u errno=%d remove=%d", keys->request, error, removing);
	if (!app->dialog.open)
		return;

	/* Done: the end. */
	if (error == 0) {
		done = "The PIN is set. The login and locked screens take it from now on.";
		if (removing)
			done = "The PIN is removed. The login and locked screens take your password.";
		keys->step = 0U;
		se_dialog_step(app, "Software Security Key", 0U, 0U, done, "Done", 0);
		return;
	}

	/* What went wrong. */
	switch (error) {
	case EPERM:
		/* The refusal's word, when the desktop gave one: a locked account, or a wrong password. */
		refused = kl_system_account_refusal(app->system, keys->request, reason, sizeof(reason));
		locked = 1;
		if (refused)
			locked = strcmp(reason, "locked-account");
		message = "The password is wrong.";
		if (locked == 0)
			message = "Your account is locked: it cannot have a PIN.";
		break;
	case EINVAL:
		message = "The PIN is not accepted: use six digits.";
		break;
	case ENOTSUP:
		message = "This desktop cannot set a PIN.";
		break;
	case EBUSY:
		message = "Another check is under way. Try again in a moment.";
		break;
	default:
		message = "The PIN could not be changed.";
		break;
	}

	/* The password again, with it said. */
	soft_step(app, SOFT_STEP_PASSWORD);
	se_dialog_error(app, message);
}

/* Tells whether the desktop offers the PIN (KL_SYSTEM_HAS_PIN). */
static int
soft_available(
	const struct se_app *app)
{
	unsigned bits;

	/* No desktop system. */
	if (app->system == NULL)
		return 0;

	/* Its PIN. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_PIN) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Tells whether the desktop has told whether a PIN is set (1), and sets *set to whether one is. */
static int
soft_is_set(
	const struct se_app *app,
	int *set)
{
	unsigned pin;
	unsigned keys;
	int known;

	/* Not known before the desktop told it. */
	*set = 0;
	if (app->system == NULL)
		return 0;
	known = kl_system_account_enrolled(app->system, &pin, &keys);
	if (!known)
		return 0;

	/* Succeeded: known, and whether a PIN is set. */
	if (pin != 0U)
		*set = 1;
	return 1;
}

/* Starts a PIN's wizard at the password. */
static void
soft_start(
	struct se_app *app,
	unsigned flow)
{
	/* The popup, its owner this card. */
	se_keys_end(app);
	app->keys.flow = flow;
	se_dialog_open(app, se_keys_soft_act, se_keys_soft_ready);
	soft_step(app, SOFT_STEP_PASSWORD);
	se_log("KEYS pin start remove=%d", flow == SE_KEYS_FLOW_PIN_REMOVE);
}

/* Shows a step of the PIN's wizard. */
static void
soft_step(
	struct se_app *app,
	unsigned step)
{
	struct se_keys *keys;
	unsigned steps;

	/* A removal has the password alone. */
	keys = &app->keys;
	keys->step = step;
	steps = SOFT_STEPS;
	if (keys->flow == SE_KEYS_FLOW_PIN_REMOVE)
		steps = 1U;

	/* The password: wiped when it is asked again. */
	if (step == SOFT_STEP_PASSWORD) {
		se_field_clear(&keys->password);
		if (keys->flow == SE_KEYS_FLOW_PIN_REMOVE) {
			se_dialog_step(app, "Remove the PIN", 1U, steps, "Type your password to remove the PIN. The login and locked screens will take your password.", "Remove", 0);
		} else {
			se_dialog_step(app, "Software Security Key", 1U, steps, "Type your password to set the PIN.", "Next", 0);
		}

		/* Its field. */
		se_dialog_field(app, "Password", "Your password now", SE_FIELD_SECRET, 0U);
		return;
	}

	/* The new PIN, twice. */
	se_dialog_step(app, "Software Security Key", 2U, steps, "Choose six digits. After a restart, sign in once with your password before the PIN works.", "Set PIN", 1);
	se_dialog_field(app, "New PIN", "Six digits", SE_FIELD_SECRET, 0U);
	se_dialog_digits(app, 0U, SOFT_DIGITS);
	se_dialog_field(app, "New PIN again", "The same again", SE_FIELD_SECRET, 0U);
	se_dialog_digits(app, 1U, SOFT_DIGITS);
}

/* Asks the desktop to set the PIN (or to remove it) with the password kept, and wipes both. */
static void
soft_ask(
	struct se_app *app)
{
	struct se_keys *keys;
	char pin[SOFT_DIGITS + 1U];
	uint32_t request;
	int error;

	/* The PIN typed (none for a removal), out of the popup before it is busy. */
	keys = &app->keys;
	pin[0] = '\0';
	if (keys->flow == SE_KEYS_FLOW_PIN_SET)
		(void)snprintf(pin, sizeof(pin), "%s", se_dialog_text(app, 0U));

	/* Asked; the password and the PIN leave with the next flush. */
	error = kl_system_account_set_pin(app->system, keys->password.text, pin, &request);
	se_field_clear(&keys->password);
	memset(pin, 0, sizeof(pin));

	/* Not asked: said at once, at the password. */
	if (error != 0) {
		soft_step(app, SOFT_STEP_PASSWORD);
		se_dialog_error(app, "The PIN could not be changed.");
		se_log("KEYS pin errno=%d", error);
		return;
	}

	/* Succeeded: busy until the answer. */
	keys->asked = 1;
	keys->request = request;
	se_dialog_busy(app, "Checking the password...", 0);
	se_log("KEYS pin request=%u remove=%d", request, keys->flow == SE_KEYS_FLOW_PIN_REMOVE);
}
