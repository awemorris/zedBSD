/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Users page's password and sign-in methods (WS200; the 2026-10-10
 * user request: a Change Password button that opens a wizard, and the
 * methods Password, PIN and Security Key turned on and off for the login
 * and locked screens).
 *
 * The password card's Change Password opens the popup (dialog.c): the
 * current password, then the new one twice, then Done.  The current
 * password is kept from its step to the change and wiped then, when the
 * popup closes and when it is left alone; the desktop changes it
 * (libkeiland's kl_system_account_set_password, on zedBSD passwd).  A
 * wrong current password goes back to its step, a new one refused stays
 * at the new one's.
 *
 * The Sign-in Methods card has a switch for each method.  A method not set
 * up (no PIN, no security key) is off and cannot be turned on here; the
 * last of the password and a key that is on cannot be turned off (the PIN
 * alone is no first sign-in after a start).  A switch opens the popup,
 * which asks the password (and, before the password is turned off, warns
 * that the console keeps it) and asks the desktop
 * (kl_system_account_set_methods, on zedBSD sessiond's SETMETHODS and
 * /sbin/passkey).  The console, su, sudo and SSH always take the
 * password.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The cards' controls: Change Password, and each method's switch. */
#define PASSWORD_CHANGE		400
#define PASSWORD_METHOD_FIRST	410

/* The steps of the popups (0 is the end). */
#define PASSWORD_STEP_DONE	0U
#define PASSWORD_STEP_CURRENT	1U
#define PASSWORD_STEP_NEW	2U
#define PASSWORD_STEP_WARNING	3U
#define PASSWORD_STEP_CONFIRM	4U

/* The fewest characters of a new password (the system's rule, said before it is asked). */
#define PASSWORD_MIN		8U

/* The methods, their rows and their switches' place. */
#define PASSWORD_METHODS	3
#define PASSWORD_METHOD_ROW	52
#define PASSWORD_TOGGLE_WIDTH	44

/* The text sizes of a row and of a line under it. */
#define PASSWORD_TEXT_ROW	15U
#define PASSWORD_TEXT_SUB	13U

/* The room of a refusal's word. */
#define PASSWORD_REASON		32U

/*
 * The methods in their order: the bit, the name, and what the row says
 * when the method is not set up.
 */
static const struct {
	unsigned bit;
	const char *name;
	const char *missing;
} password_methods[PASSWORD_METHODS] = {
	{ KL_SYSTEM_METHOD_PASSWORD, "Password", NULL },
	{ KL_SYSTEM_METHOD_PIN, "PIN (Software Security Key)", "Not set up: set a PIN on the Security Keys page." },
	{ KL_SYSTEM_METHOD_KEY, "Security Key", "No key registered: add one on the Security Keys page." },
};

static int password_available(const struct se_app *app);
static int password_methods_available(const struct se_app *app);
static int password_methods_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int password_set_up(const struct se_app *app, unsigned bit);
static unsigned password_methods_on(const struct se_app *app);
static int password_switch_enabled(const struct se_app *app, unsigned bit, unsigned on);
static void password_start(struct se_app *app, unsigned flow, unsigned method);
static void password_step(struct se_app *app, unsigned step);
static void password_act(struct se_app *app, unsigned action);
static int password_ready(const struct se_app *app);
static void password_next(struct se_app *app);
static void password_ask(struct se_app *app);
static void password_failed(struct se_app *app, uint32_t request, int error);
static void password_end_with(struct se_app *app, const char *title, const char *text);

/*
 * Draws the password card and the Sign-in Methods card from a top edge;
 * returns the edge below them.
 */
int
se_password_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int available;
	int enabled;
	int button;
	int height;
	int bottom;
	int y;

	/* The password card: what it does, and Change Password at the right. */
	available = password_available(app);
	height = 64 + 56;
	y = se_card_begin(app, canvas, x, top, width, height, "Password", "Change the password you log in with.");
	if (!available) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 26, "This desktop cannot change the password here.", PASSWORD_TEXT_SUB, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* The line and the button (grey while a change is asked or a popup is open). */
	(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 26, "The console, su, sudo and SSH take it too.", PASSWORD_TEXT_SUB, 0, width - 220, SE_COLOR_TEXT_SECONDARY);
	enabled = 1;
	if (app->password.asked || app->dialog.open)
		enabled = 0;
	button = se_button_width(app, "Change Password");
	(void)se_button_draw(app, canvas, x + width - 20 - button, y + 8, "Change Password", 1, enabled, PASSWORD_CHANGE);
	top += height;

	/* The sign-in methods under it, where the desktop has them. */
	bottom = password_methods_draw(app, canvas, x, top + 16, width);
	if (bottom == top + 16)
		return top;

	/* The edge below the cards. */
	return bottom;
}

/*
 * Carries out a click on a control of the password's or the methods'
 * cards.  Returns 1 when the control was theirs.
 */
int
se_password_press(
	struct se_app *app,
	int index)
{
	unsigned bit;
	unsigned on;
	int enabled;
	int busy;

	/* Neither card's. */
	if (index != PASSWORD_CHANGE && (index < PASSWORD_METHOD_FIRST || index >= PASSWORD_METHOD_FIRST + PASSWORD_METHODS))
		return 0;

	/* One change at a time. */
	busy = app->password.asked || app->keys.asked || app->dialog.open;
	if (busy)
		return 1;

	/* Change Password, where the desktop changes it. */
	if (index == PASSWORD_CHANGE) {
		enabled = password_available(app);
		if (enabled)
			password_start(app, SE_PASSWORD_FLOW_CHANGE, 0U);
		return 1;
	}

	/* A method's switch, when it may be flipped. */
	bit = password_methods[index - PASSWORD_METHOD_FIRST].bit;
	on = password_methods_on(app);
	enabled = password_switch_enabled(app, bit, on);
	if (enabled)
		password_start(app, SE_PASSWORD_FLOW_METHODS, bit);

	/* Taken. */
	return 1;
}

/*
 * Takes the answer of the change the popups asked.  Returns 1 when the
 * request was theirs.
 */
int
se_password_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_password *password;

	/* Only the change they asked. */
	password = &app->password;
	if (!password->asked || request != password->request)
		return 0;
	password->asked = 0;
	app->dirty = 1;
	if (password->flow == SE_PASSWORD_FLOW_CHANGE) {
		se_log("USERS result request=%u errno=%d", request, error);
	} else {
		se_log("USERS methods result request=%u errno=%d", request, error);
	}

	/* The popup closed meanwhile: nothing more to show. */
	if (!app->dialog.open) {
		se_field_clear(&password->current);
		password->flow = SE_PASSWORD_FLOW_NONE;
		return 1;
	}

	/* A failure, said at the step it goes back to. */
	if (error != 0) {
		password_failed(app, request, error);
		return 1;
	}

	/* The password changed. */
	if (password->flow == SE_PASSWORD_FLOW_CHANGE) {
		password_end_with(app, "Password", "Your password is changed. Use the new one from now on.");
		return 1;
	}

	/* The methods changed. */
	password_end_with(app, "Sign-in methods", "Changed. The console, su, sudo and SSH still take your password.");
	return 1;
}

/* Ends the popup under way: closed, the password kept wiped (a change asked still gets its answer). */
void
se_password_end(
	struct se_app *app)
{
	struct se_password *password;

	/* The password kept, and the popup when it is theirs. */
	password = &app->password;
	se_field_clear(&password->current);
	if (password->flow != SE_PASSWORD_FLOW_NONE && app->dialog.act == password_act)
		se_dialog_close(app);

	/* No step; the change asked, if any, stays asked. */
	password->step = PASSWORD_STEP_DONE;
	if (!password->asked)
		password->flow = SE_PASSWORD_FLOW_NONE;
}

/* Tells whether the desktop changes the password (KL_SYSTEM_HAS_ACCOUNT). */
static int
password_available(
	const struct se_app *app)
{
	unsigned bits;

	/* No desktop system. */
	if (app->system == NULL)
		return 0;

	/* Its account. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_ACCOUNT) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Tells whether the desktop has the sign-in methods (KL_SYSTEM_HAS_METHODS). */
static int
password_methods_available(
	const struct se_app *app)
{
	unsigned bits;

	/* No desktop system. */
	if (app->system == NULL)
		return 0;

	/* Its methods. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_METHODS) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Draws the Sign-in Methods card (none where the desktop has no methods); returns the edge below it. */
static int
password_methods_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const char *line;
	kl_color ink;
	unsigned on;
	unsigned bit;
	unsigned others;
	int available;
	int enabled;
	int set_up;
	int height;
	int index;
	int y;

	/* Only where the desktop has them. */
	available = password_methods_available(app);
	if (!available)
		return top;

	/* The card. */
	on = password_methods_on(app);
	height = 64 + PASSWORD_METHODS * PASSWORD_METHOD_ROW + 20;
	y = se_card_begin(app, canvas, x, top, width, height, "Sign-in Methods", "What the login and locked screens take. The console, su, sudo and SSH always take your password.");
	y += 12;

	/* Each method: its name, what is missing, and its switch at the right. */
	for (index = 0; index < PASSWORD_METHODS; index++) {
		bit = password_methods[index].bit;
		set_up = password_set_up(app, bit);
		enabled = password_switch_enabled(app, bit, on);
		ink = SE_COLOR_TEXT;
		if (!set_up)
			ink = SE_COLOR_TEXT_SECONDARY;
		(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(PASSWORD_TEXT_ROW, y + 6, 24), password_methods[index].name, PASSWORD_TEXT_ROW, 0, width - 120, ink);

		/* The line under the name: what is missing, or why it stays on. */
		line = NULL;
		if (!set_up)
			line = password_methods[index].missing;
		others = on & ~bit & (KL_SYSTEM_METHOD_PASSWORD | KL_SYSTEM_METHOD_KEY);
		if (set_up && (on & bit) != 0U && bit != KL_SYSTEM_METHOD_PIN && others == 0U)
			line = "The password or a security key must stay on.";
		if (line != NULL)
			(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 44, line, PASSWORD_TEXT_SUB, 0, width - 120, SE_COLOR_TEXT_SECONDARY);

		/* The switch. */
		se_toggle_draw(app, canvas, x + width - 20 - PASSWORD_TOGGLE_WIDTH, y + 8, (on & bit) != 0U, enabled, PASSWORD_METHOD_FIRST + index);
		y += PASSWORD_METHOD_ROW;
	}

	/* The edge below the card. */
	return top + height;
}

/* Tells whether the user has set a method up: the password always, the PIN and a key when the desktop says so. */
static int
password_set_up(
	const struct se_app *app,
	unsigned bit)
{
	unsigned pin;
	unsigned keys;
	int known;

	/* The password. */
	if (bit == KL_SYSTEM_METHOD_PASSWORD)
		return 1;

	/* What is enrolled, as the desktop last told. */
	known = kl_system_account_enrolled(app->system, &pin, &keys);
	if (!known)
		return 0;

	/* The PIN, or a key. */
	if (bit == KL_SYSTEM_METHOD_PIN)
		return pin != 0U;
	return keys != 0U;
}

/* Gives the methods shown on: those turned on that the user has set up. */
static unsigned
password_methods_on(
	const struct se_app *app)
{
	unsigned methods;
	unsigned on;
	int index;
	int set_up;

	/* As the desktop last told (every method until it did). */
	(void)kl_system_account_methods(app->system, &methods);

	/* Each method set up and turned on. */
	on = 0U;
	for (index = 0; index < PASSWORD_METHODS; index++) {
		set_up = password_set_up(app, password_methods[index].bit);
		if (set_up && (methods & password_methods[index].bit) != 0U)
			on |= password_methods[index].bit;
	}

	/* The methods on. */
	return on;
}

/*
 * Tells whether a method's switch may be flipped: the method set up, no
 * change under way, and, to turn the password or a key off, the other of
 * them on.
 */
static int
password_switch_enabled(
	const struct se_app *app,
	unsigned bit,
	unsigned on)
{
	unsigned others;
	int set_up;

	/* Nothing while a change is under way or a popup is open. */
	if (app->password.asked || app->dialog.open)
		return 0;

	/* Only a method set up. */
	set_up = password_set_up(app, bit);
	if (!set_up)
		return 0;

	/* Turned on, or the PIN turned off: always. */
	if ((on & bit) == 0U || bit == KL_SYSTEM_METHOD_PIN)
		return 1;

	/* The password or a key turned off: the other of them must stay on. */
	others = on & ~bit & (KL_SYSTEM_METHOD_PASSWORD | KL_SYSTEM_METHOD_KEY);
	if (others == 0U)
		return 0;

	/* Allowed. */
	return 1;
}

/* Opens a popup at its first step: the password's change, or a method turned on or off. */
static void
password_start(
	struct se_app *app,
	unsigned flow,
	unsigned method)
{
	struct se_password *password;
	unsigned stored;

	/* Nothing kept from before; the methods asked for, the method flipped among those stored. */
	se_password_end(app);
	password = &app->password;
	password->flow = flow;
	password->method = method;
	(void)kl_system_account_methods(app->system, &stored);
	password->methods = stored ^ method;
	se_log("USERS start flow=%u method=%u", flow, method);

	/* The popup, its owner this page. */
	se_dialog_open(app, password_act, password_ready);

	/* The password turned off is warned of first; the others start with a password. */
	if (flow == SE_PASSWORD_FLOW_METHODS && method == KL_SYSTEM_METHOD_PASSWORD && (password->methods & method) == 0U) {
		password_step(app, PASSWORD_STEP_WARNING);
	} else if (flow == SE_PASSWORD_FLOW_METHODS) {
		password_step(app, PASSWORD_STEP_CONFIRM);
	} else {
		password_step(app, PASSWORD_STEP_CURRENT);
	}
}

/* Shows a step of a popup. */
static void
password_step(
	struct se_app *app,
	unsigned step)
{
	struct se_password *password;
	char body[SE_MESSAGE * 2U];
	const char *name;
	unsigned place;
	unsigned count;
	int index;
	int turning_on;

	/* The step, and the method's name. */
	password = &app->password;
	password->step = step;
	name = "";
	for (index = 0; index < PASSWORD_METHODS; index++) {
		if (password_methods[index].bit == password->method)
			name = password_methods[index].name;
	}

	/* The password turned off has two steps; the others one. */
	count = 1U;
	place = 1U;
	if (password->method == KL_SYSTEM_METHOD_PASSWORD && (password->methods & KL_SYSTEM_METHOD_PASSWORD) == 0U)
		count = 2U;
	if (step == PASSWORD_STEP_CONFIRM && count == 2U)
		place = 2U;

	/* Each step. */
	switch (step) {
	case PASSWORD_STEP_CURRENT:
		se_field_clear(&password->current);
		se_dialog_step(app, "Change your password", 1U, 2U, "Type the password you use now.", "Next", 0);
		se_dialog_field(app, "Current password", "Your password now", SE_FIELD_SECRET, 0U);
		break;
	case PASSWORD_STEP_NEW:
		(void)snprintf(body, sizeof(body), "Choose a new password of at least %u characters, not the one you use now. Type it twice.", PASSWORD_MIN);
		se_dialog_step(app, "Change your password", 2U, 2U, body, "Change Password", 1);
		se_dialog_field(app, "New password", "At least 8 characters", SE_FIELD_SECRET, 0U);
		se_dialog_field(app, "New password again", "The same again", SE_FIELD_SECRET, 0U);
		break;
	case PASSWORD_STEP_WARNING:
		se_dialog_step(app, "Sign-in methods", place, count,
		    "The login and locked screens will no longer take your password: only your security key (and your PIN after it). If you lose the key, sign in on the console with your password and turn the password on again here.",
		    "Continue", 0);
		break;
	case PASSWORD_STEP_CONFIRM:
		turning_on = (password->methods & password->method) != 0U;
		if (turning_on) {
			(void)snprintf(body, sizeof(body), "The login and locked screens will take your %s. Type your password to change it.", name);
		} else {
			(void)snprintf(body, sizeof(body), "The login and locked screens will no longer take your %s. Type your password to change it.", name);
		}

		/* The password's field. */
		se_dialog_step(app, "Sign-in methods", place, count, body, "Change", count == 2U);
		se_dialog_field(app, "Password", "Your password now", SE_FIELD_SECRET, 0U);
		break;
	default:
		break;
	}
}

/*
 * Takes the popup's action: the step's button goes on (or asks), Back goes
 * back, Cancel closes, and idle wipes the password kept.
 */
static void
password_act(
	struct se_app *app,
	unsigned action)
{
	struct se_password *password;

	/* Cancel: nothing kept (a change asked still gets its answer). */
	password = &app->password;
	if (action == SE_DIALOG_CANCEL) {
		se_password_end(app);
		return;
	}

	/* Back: the current password's step, or the warning. */
	if (action == SE_DIALOG_BACK) {
		if (password->step == PASSWORD_STEP_NEW) {
			password_step(app, PASSWORD_STEP_CURRENT);
		} else if (password->step == PASSWORD_STEP_CONFIRM) {
			password_step(app, PASSWORD_STEP_WARNING);
		}

		/* Done. */
		return;
	}

	/* Left alone: the password kept goes, and is asked again. */
	if (action == SE_DIALOG_IDLE) {
		if (password->current.length != 0U && !password->asked) {
			password_step(app, PASSWORD_STEP_CURRENT);
			se_dialog_error(app, "For your security, type your password again.");
		}

		/* Done. */
		return;
	}

	/* The step's button. */
	if (action == SE_DIALOG_PRIMARY)
		password_next(app);
}

/* Tells whether the step's button may be pressed: its fields typed. */
static int
password_ready(
	const struct se_app *app)
{
	size_t first;
	size_t second;

	/* The step's fields. */
	first = se_dialog_length(app, 0U);
	second = se_dialog_length(app, 1U);
	switch (app->password.step) {
	case PASSWORD_STEP_CURRENT:
	case PASSWORD_STEP_CONFIRM:
		return first != 0U;
	case PASSWORD_STEP_NEW:
		if (first == 0U || second == 0U)
			return 0;
		return 1;
	default:
		return 1;
	}
}

/* Goes on from the step shown (the step's button). */
static void
password_next(
	struct se_app *app)
{
	struct se_password *password;
	const char *fresh;
	const char *repeat;
	size_t length;
	int same;

	/* Each step's next. */
	password = &app->password;
	switch (password->step) {
	case PASSWORD_STEP_DONE:
		/* The end's Done closes. */
		se_password_end(app);
		return;
	case PASSWORD_STEP_CURRENT:
		/* Kept, then the new one. */
		se_field_clear(&password->current);
		kl_field_set(&password->current, se_dialog_text(app, 0U));
		password_step(app, PASSWORD_STEP_NEW);
		return;
	case PASSWORD_STEP_WARNING:
		/* The warning read: the password. */
		password_step(app, PASSWORD_STEP_CONFIRM);
		return;
	case PASSWORD_STEP_NEW:
		break;
	default:
		/* The methods' password: asked. */
		password_ask(app);
		return;
	}

	/* The new password twice the same. */
	fresh = se_dialog_text(app, 0U);
	repeat = se_dialog_text(app, 1U);
	same = strcmp(fresh, repeat);
	if (same != 0) {
		se_dialog_error(app, "The new password and its repeat differ.");
		return;
	}

	/* Long enough. */
	length = strlen(fresh);
	if (length < PASSWORD_MIN) {
		se_dialog_error(app, "Use at least 8 characters.");
		return;
	}

	/* Not the one used now. */
	same = strcmp(fresh, password->current.text);
	if (same == 0) {
		se_dialog_error(app, "Choose a password other than the one you use now.");
		return;
	}

	/* Asked. */
	password_ask(app);
}

/* Asks the desktop for the change of the step shown, busy until it answers. */
static void
password_ask(
	struct se_app *app)
{
	struct se_password *password;
	const char *typed;
	const char *busy;
	uint32_t request;
	unsigned first;
	int error;

	/* The password's change (the current one kept, the new one typed), or the methods' (the password typed). */
	password = &app->password;
	typed = se_dialog_text(app, 0U);
	if (password->flow == SE_PASSWORD_FLOW_CHANGE) {
		error = kl_system_account_set_password(app->system, password->current.text, typed, &request);
		busy = "Changing the password...";
	} else {
		error = kl_system_account_set_methods(app->system, typed, password->methods, &request);
		busy = "Changing how you sign in...";
	}

	/* The current password kept goes either way (the popup's fields go with its next step). */
	se_field_clear(&password->current);

	/* Not asked: said at once, back at the step that asks the password. */
	if (error != 0) {
		first = PASSWORD_STEP_CONFIRM;
		if (password->flow == SE_PASSWORD_FLOW_CHANGE)
			first = PASSWORD_STEP_CURRENT;
		password_step(app, first);
		se_dialog_error(app, "The desktop could not ask for the change.");
		se_log("USERS ask flow=%u errno=%d", password->flow, error);
		return;
	}

	/* Busy until the answer (it cannot be cancelled). */
	password->asked = 1;
	password->request = request;
	se_dialog_busy(app, busy, 0);
	if (password->flow == SE_PASSWORD_FLOW_CHANGE) {
		se_log("USERS change request=%u", request);
	} else {
		se_log("USERS methods request=%u methods=%u", request, password->methods);
	}
}

/* Says what went wrong, at the step it sends the user back to. */
static void
password_failed(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_password *password;
	char reason[PASSWORD_REASON];
	int refused;
	int same;

	/* The password's change: the current one wrong, the new one refused, or another failure. */
	password = &app->password;
	if (password->flow == SE_PASSWORD_FLOW_CHANGE) {
		if (error == EPERM) {
			password_step(app, PASSWORD_STEP_CURRENT);
			se_dialog_error(app, "The current password is wrong.");
		} else if (error == EINVAL) {
			password_step(app, PASSWORD_STEP_CURRENT);
			se_dialog_error(app, "The new password is not accepted: use at least 8 characters, not the old password. Type your password again.");
		} else if (error == EBUSY) {
			password_step(app, PASSWORD_STEP_CURRENT);
			se_dialog_error(app, "Another change of the password is under way. Try again in a moment.");
		} else {
			password_end_with(app, "Password", "The password could not be changed.");
		}

		/* Said. */
		return;
	}

	/* The methods' change: its word says why. */
	reason[0] = '\0';
	refused = kl_system_account_refusal(app->system, request, reason, sizeof(reason));
	same = refused && strcmp(reason, "bad-secret") == 0;
	if (same) {
		password_step(app, PASSWORD_STEP_CONFIRM);
		se_dialog_error(app, "The password is wrong.");
		return;
	}

	/* Too many wrong passwords. */
	same = refused && strcmp(reason, "locked") == 0;
	if (same) {
		password_step(app, PASSWORD_STEP_CONFIRM);
		se_dialog_error(app, "Too many wrong attempts. Wait, then try again.");
		return;
	}

	/* A key the account no longer has. */
	same = refused && strcmp(reason, "not-enrolled") == 0;
	if (same) {
		password_end_with(app, "Sign-in methods", "That needs a security key registered on this computer.");
		return;
	}

	/* Busy, or anything else. */
	if (error == EBUSY) {
		password_step(app, PASSWORD_STEP_CONFIRM);
		se_dialog_error(app, "Another check is under way, or the screen was locked. Try again in a moment.");
		return;
	}

	/* Anything else. */
	password_end_with(app, "Sign-in methods", "The sign-in methods could not be changed.");
}

/* Shows the popup's end with its text and Done. */
static void
password_end_with(
	struct se_app *app,
	const char *title,
	const char *text)
{
	/* What was kept goes; the end. */
	se_field_clear(&app->password.current);
	app->password.step = PASSWORD_STEP_DONE;
	se_dialog_step(app, title, 0U, 0U, text, "Done", 0);
	se_dialog_final(app);
}
