/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Users page's administration (ws089-p026; docs/architecture/
 * security.md, "Account administration"): what an administrator changes
 * of the people's accounts, through the desktop
 * (kl_system_account_administer; on zedBSD the compositor runs
 * account-admin).  Settings holds no privilege.
 *
 * The card shows under the list of users, for an administrator on a
 * desktop that administers the accounts (KL_SYSTEM_HAS_ADMINISTER).  A
 * click on a user of the list chooses it; the card's buttons start a
 * change: Add User, and for the user chosen (not oneself) Reset
 * Password, Remove, Make or Remove Administrator, Allow or Stop Wi-Fi.
 * The change's form has its fields (a new user's name, full name and
 * password; a new password), the administrator's own password, a switch
 * (the new user an administrator; the home folder removed with the user),
 * Cancel and the change's button.  The fields are wiped as soon as the
 * change is asked or cancelled; the answer comes as a line under the
 * buttons, in words for each refusal the tool gives, and the list is read
 * again after a change.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls of the card: the list's rows, the buttons of the changes, the fields, the switch, Cancel and the change. */
#define ADMIN_ROW_FIRST		100
#define ADMIN_START_FIRST	20
#define ADMIN_FIELD_FIRST	30
#define ADMIN_SWITCH		40
#define ADMIN_APPLY		41
#define ADMIN_CANCEL		42

/*
 * The most bytes account-admin takes of a name (ADMIN_NAME_MAX), a full
 * name (admin_display_valid) and a password (ACCOUNT_PASSWORD_MAX), which
 * the fields are limited to (ws177-p004).
 */
#define ADMIN_NAME_LIMIT	32U
#define ADMIN_FULL_NAME_LIMIT	64U
#define ADMIN_PASSWORD_LIMIT	256U

/* A field's row, the field's left edge in it, the buttons' row, and the line under them. */
#define ADMIN_ROW		52
#define ADMIN_FIELD_X		210
#define ADMIN_BUTTONS		48
#define ADMIN_MESSAGE_LINE	30

/* The text sizes of a row and of a message. */
#define ADMIN_TEXT_ROW		15U
#define ADMIN_TEXT_SUB		13U

/* The fields' labels and placeholders, and which of them are passwords. */
static const char *const admin_labels[SE_ADMIN_FIELDS] = { "User name", "Full name", "New password", "Your password" };
static const char *const admin_placeholders[SE_ADMIN_FIELDS] = { "Lower-case letters and digits", "The name shown", "At least 8 characters", "To confirm the change" };
static const int admin_secret[SE_ADMIN_FIELDS] = { 0, 0, 1, 1 };

/* The refusals' words and what Settings says for each. */
static const char *const admin_words[] = {
	"not-administrator", "bad-password", "no-such-user", "name-taken", "bad-name", "weak-password",
	"last-administrator", "self", "root", "busy", "home-exists", "bad-request"
};
static const char *const admin_sayings[] = {
	"Only an administrator can change the users.",
	"Your password is wrong.",
	"That user no longer exists.",
	"That user name is already taken.",
	"Use a user name of lower-case letters, digits, - and _, starting with a letter; no : in the full name.",
	"The new password is not accepted: use at least 8 characters.",
	"The computer must keep at least one administrator.",
	"You cannot do that to your own account here.",
	"System accounts cannot be changed here.",
	"That user is logged in or has programs running. Log them out first.",
	"A home folder of that name is already there. It must be moved away first.",
	"The request was not understood."
};

static const struct se_user_row *admin_chosen(const struct se_app *app);
static int admin_shown(enum se_admin_mode mode, int index);
static int admin_rows(enum se_admin_mode mode);
static int admin_ready(const struct se_app *app);
static void admin_start(struct se_app *app, enum se_admin_mode mode);
static void admin_apply(struct se_app *app);
static int admin_next_field(enum se_admin_mode mode, int from, int step);
static int admin_buttons_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width, int draw);
static void admin_field_draw(struct se_app *app, struct kl_canvas *canvas, int index, int x, int y, int width);
static const char *admin_title(const struct se_app *app);
static const char *admin_apply_label(const struct se_app *app);

/*
 * Tells whether the Users page offers the administration: the desktop
 * administers the accounts, and the user Settings runs as is an
 * administrator.
 */
int
se_users_admin_available(
	const struct se_app *app)
{
	unsigned bits;

	/* A desktop that administers the accounts. */
	if (app->system == NULL)
		return 0;
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_ADMINISTER) == 0U)
		return 0;

	/* Not an administrator, as the desktop told of the user's own account (ws188-p002). */
	if (!app->users.self_admin)
		return 0;

	/* An administrator. */
	return 1;
}

/*
 * Draws the administration's card from a top edge; returns the edge below
 * it.
 */
int
se_users_admin_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_users *users;
	const char *switch_label;
	kl_color ink;
	int apply;
	int cancel;
	int enabled;
	int buttons;
	int shown;
	int height;
	int right;
	int index;
	int y;

	/* The card's height: the buttons, and the form of the change being made. */
	users = &app->users;
	buttons = admin_buttons_draw(app, canvas, x, 0, width, 0);
	height = 64 + buttons + ADMIN_MESSAGE_LINE;
	if (users->admin_mode != SE_ADMIN_NONE)
		height += 34 + admin_rows(users->admin_mode) * ADMIN_ROW + ADMIN_BUTTONS;

	/* The card and the buttons that start a change. */
	y = se_card_begin(app, canvas, x, top, width, height, "Manage users", "Choose a user in the list. Each change asks for your password.");
	y = admin_buttons_draw(app, canvas, x, y, width, 1);

	/* The form of the change being made: its title, its fields, its switch, Cancel and its button. */
	if (users->admin_mode != SE_ADMIN_NONE) {
		/* Its title. */
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, admin_title(app), ADMIN_TEXT_ROW, 1, width - 40, SE_COLOR_TEXT);
		y += 34;

		/* Its fields. */
		for (index = 0; index < SE_ADMIN_FIELDS; index++) {
			/* A field the change does not use. */
			shown = admin_shown(users->admin_mode, index);
			if (!shown)
				continue;

			/* One field. */
			admin_field_draw(app, canvas, index, x, y, width);
			y += ADMIN_ROW;
		}

		/* Its switch: the new user an administrator, or the home removed. */
		switch_label = NULL;
		if (users->admin_mode == SE_ADMIN_ADD)
			switch_label = "Administrator (can change the computer and its users)";
		else if (users->admin_mode == SE_ADMIN_REMOVE)
			switch_label = "Also delete the home folder and its files";
		if (switch_label != NULL) {
			(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(ADMIN_TEXT_ROW, y + 8, 36), switch_label, ADMIN_TEXT_ROW, 0, width - 120, SE_COLOR_TEXT);
			se_toggle_draw(app, canvas, x + width - 20 - 44, y + 14, users->admin_flag, 1, ADMIN_SWITCH);
			y += ADMIN_ROW;
		}

		/* Cancel and the change's button at the right. */
		right = x + width - 20;
		apply = se_button_width(app, admin_apply_label(app));
		cancel = se_button_width(app, "Cancel");
		enabled = admin_ready(app);
		(void)se_button_draw(app, canvas, right - apply, y + 8, admin_apply_label(app), 1, enabled, ADMIN_APPLY);
		(void)se_button_draw(app, canvas, right - apply - 8 - cancel, y + 8, "Cancel", 0, 1, ADMIN_CANCEL);
		y += ADMIN_BUTTONS;
	}

	/* The last answer: green when it was done, red when it was refused. */
	ink = SE_COLOR_GOOD;
	if (users->admin_bad)
		ink = SE_COLOR_BAD;
	if (users->admin_message[0] != '\0')
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, users->admin_message, ADMIN_TEXT_SUB, 0, width - 40, ink);

	/* The edge below the card. */
	return top + height;
}

/*
 * Carries out a click on a control of the administration.  Returns 1
 * when the control was the administration's.
 */
int
se_users_admin_press(
	struct se_app *app,
	int index)
{
	struct se_users *users;
	int reading;
	int ready;

	/*
	 * While the list is read again (ws188-p002), its rows and changes wait:
	 * a row chosen now could be another user's in the new list.
	 */
	users = &app->users;
	reading = se_machine_reading(app, KL_MACHINE_USERS);
	if (reading &&
	    index >= ADMIN_ROW_FIRST &&
	    index < ADMIN_ROW_FIRST + users->row_count)
		return 1;

	/* A user of the list chosen (a click on the chosen one lets it go), remembered by name. */
	if (index >= ADMIN_ROW_FIRST && index < ADMIN_ROW_FIRST + users->row_count) {
		if (users->selected == index - ADMIN_ROW_FIRST + 1) {
			users->selected = 0;
			users->selected_name[0] = '\0';
		} else {
			users->selected = index - ADMIN_ROW_FIRST + 1;
			(void)snprintf(users->selected_name, sizeof(users->selected_name), "%s", users->rows[users->selected - 1].name);
		}
		se_users_admin_wipe(users);
		users->admin_mode = SE_ADMIN_NONE;
		users->admin_message[0] = '\0';
		return 1;
	}

	/* A change started. */
	if (index > ADMIN_START_FIRST + SE_ADMIN_NONE && index <= ADMIN_START_FIRST + SE_ADMIN_NETWORK) {
		admin_start(app, (enum se_admin_mode)(index - ADMIN_START_FIRST));
		return 1;
	}

	/* A field takes the keyboard. */
	if (index >= ADMIN_FIELD_FIRST && index < ADMIN_FIELD_FIRST + SE_ADMIN_FIELDS) {
		users->admin_focus = index - ADMIN_FIELD_FIRST;
		users->keyboard = SE_USERS_KEYBOARD_ADMIN;
		return 1;
	}

	/* The switch. */
	if (index == ADMIN_SWITCH) {
		users->admin_flag = !users->admin_flag;
		return 1;
	}

	/* Cancel: the form goes, its fields wiped. */
	if (index == ADMIN_CANCEL) {
		se_users_admin_wipe(users);
		users->admin_mode = SE_ADMIN_NONE;
		users->keyboard = SE_USERS_KEYBOARD_PASSWORD;
		return 1;
	}

	/* The change, when the form is ready. */
	if (index == ADMIN_APPLY) {
		ready = admin_ready(app);
		if (ready)
			admin_apply(app);
		return 1;
	}

	/* Not the administration's. */
	return 0;
}

/*
 * Takes a key while the administration's fields have the keyboard: Tab
 * moves between them, Enter asks the change when the form is ready and
 * otherwise goes to the next field, Esc cancels, the others type.
 * Returns 1 when the key was used.
 */
int
se_users_admin_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_users *users;
	int ready;
	int used;

	/* Not the administration's keyboard. */
	users = &app->users;
	if (users->keyboard != SE_USERS_KEYBOARD_ADMIN || users->admin_mode == SE_ADMIN_NONE)
		return 0;

	/* Tab and Shift+Tab, through the fields the change uses. */
	if (event->key == SE_KEY_TAB) {
		if ((event->modifiers & SE_MOD_SHIFT) != 0U)
			users->admin_focus = admin_next_field(users->admin_mode, users->admin_focus, -1);
		else
			users->admin_focus = admin_next_field(users->admin_mode, users->admin_focus, 1);
		return 1;
	}

	/* Enter: the change, or the next field. */
	if (event->key == SE_KEY_ENTER) {
		ready = admin_ready(app);
		if (ready)
			admin_apply(app);
		else
			users->admin_focus = admin_next_field(users->admin_mode, users->admin_focus, 1);
		return 1;
	}

	/* Esc cancels. */
	if (event->key == SE_KEY_ESC) {
		(void)se_users_admin_press(app, ADMIN_CANCEL);
		return 1;
	}

	/* Anything else types into the field with the keyboard. */
	used = se_field_key(&users->admin_fields[users->admin_focus], event);
	if (used == 0)
		return 0;

	/* A new character takes the last answer away. */
	if (!users->admin_asked)
		users->admin_message[0] = '\0';

	/* The names' lengths for the tests (ws177-p004: the limits); a password's is not told. */
	if (users->admin_focus == SE_ADMIN_NAME || users->admin_focus == SE_ADMIN_FULL_NAME)
		se_log("USERS admin field=%d length=%lu", users->admin_focus, (unsigned long)users->admin_fields[users->admin_focus].length);

	/* Succeeded: the field took the key. */
	return 1;
}

/*
 * Takes the answer of the administration's change when it is the page's.
 * Returns 1 when the request was the page's.
 */
int
se_users_admin_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_users *users;
	char word[KL_ACCOUNT_REASON_SIZE];
	const char *saying;
	size_t i;
	int refused;
	int same;

	/* Only the change the page asked. */
	users = &app->users;
	if (!users->admin_asked || request != users->admin_request)
		return 0;
	users->admin_asked = 0;

	/* Done: the form goes and the list is read again. */
	if (error == 0) {
		(void)snprintf(users->admin_message, sizeof(users->admin_message), "Done.");
		users->admin_bad = 0;
		users->admin_mode = SE_ADMIN_NONE;
		users->keyboard = SE_USERS_KEYBOARD_PASSWORD;
		users->selected = 0;
		users->selected_name[0] = '\0';
		se_users_reload(app);
		app->dirty = 1;
		se_log("USERS admin result request=%u errno=0", request);
		return 1;
	}

	/* A refusal in words, or a failure. */
	saying = "The change could not be made.";
	if (error == ENOTSUP)
		saying = "This desktop cannot change the users.";
	else if (error == EBUSY)
		saying = "Another change is under way. Try again in a moment.";
	word[0] = '\0';
	refused = kl_system_account_refusal(app->system, request, word, sizeof(word));
	for (i = 0; refused && i < sizeof(admin_words) / sizeof(admin_words[0]); i++) {
		/* The tool's word. */
		same = strcmp(word, admin_words[i]);
		if (same == 0) {
			saying = admin_sayings[i];
			break;
		}
	}

	/* Shown under the buttons, and logged for the tests (without any password). */
	(void)snprintf(users->admin_message, sizeof(users->admin_message), "%s", saying);
	users->admin_bad = 1;
	app->dirty = 1;
	se_log("USERS admin result request=%u errno=%d reason=%s", request, error, word);

	/* Succeeded: the answer was the page's. */
	return 1;
}

/*
 * Wipes the administration's fields.
 */
void
se_users_admin_wipe(
	struct se_users *users)
{
	int index;

	/* Each field (se_field_clear overwrites its text). */
	for (index = 0; index < SE_ADMIN_FIELDS; index++)
		se_field_clear(&users->admin_fields[index]);

	/*
	 * Each field takes no more than account-admin does (ws177-p004): a
	 * name of 32 bytes, a full name of 64 and passwords of 256, so a text
	 * it would refuse cannot be typed.
	 */
	kl_field_set_limit(&users->admin_fields[SE_ADMIN_NAME], ADMIN_NAME_LIMIT);
	kl_field_set_limit(&users->admin_fields[SE_ADMIN_FULL_NAME], ADMIN_FULL_NAME_LIMIT);
	kl_field_set_limit(&users->admin_fields[SE_ADMIN_PASSWORD], ADMIN_PASSWORD_LIMIT);
	kl_field_set_limit(&users->admin_fields[SE_ADMIN_YOURS], ADMIN_PASSWORD_LIMIT);
}

/*
 * Reports the user chosen in the list, or NULL for none.
 */
static const struct se_user_row *
admin_chosen(
	const struct se_app *app)
{
	const struct se_users *users;

	/* None, or a row the list no longer has. */
	users = &app->users;
	if (users->selected <= 0 || users->selected > users->row_count)
		return NULL;

	/* The row. */
	return &users->rows[users->selected - 1];
}

/*
 * Tells whether a change uses a field.
 */
static int
admin_shown(
	enum se_admin_mode mode,
	int index)
{
	/* Each change's fields: all for an addition, a new password for a reset, the administrator's own for every one. */
	if (index == SE_ADMIN_YOURS)
		return 1;
	if (mode == SE_ADMIN_ADD)
		return 1;
	if (mode == SE_ADMIN_RESET && index == SE_ADMIN_PASSWORD)
		return 1;

	/* Not used. */
	return 0;
}

/*
 * Reports how many rows a change's form has: its fields, and its switch.
 */
static int
admin_rows(
	enum se_admin_mode mode)
{
	int rows;
	int index;
	int shown;

	/* The fields used. */
	rows = 0;
	for (index = 0; index < SE_ADMIN_FIELDS; index++) {
		/* One field. */
		shown = admin_shown(mode, index);
		if (shown)
			rows++;
	}

	/* The switch of an addition and of a removal. */
	if (mode == SE_ADMIN_ADD || mode == SE_ADMIN_REMOVE)
		rows++;

	/* The rows. */
	return rows;
}

/*
 * Tells whether the form is ready: every field the change uses typed, and
 * no change asked yet.
 */
static int
admin_ready(
	const struct se_app *app)
{
	const struct se_users *users;
	int reading;
	int index;
	int shown;

	/* One change at a time, of one being made. */
	users = &app->users;
	if (users->admin_asked || users->admin_mode == SE_ADMIN_NONE)
		return 0;

	/* Not while the list is read again (its rows may name other users then, ws188-p002). */
	reading = se_machine_reading(app, KL_MACHINE_USERS);
	if (reading)
		return 0;

	/* Each field it uses typed. */
	for (index = 0; index < SE_ADMIN_FIELDS; index++) {
		/* One field. */
		shown = admin_shown(users->admin_mode, index);
		if (shown && users->admin_fields[index].length == 0)
			return 0;
	}

	/* Succeeded: ready. */
	return 1;
}

/*
 * Starts a change: its form with empty fields, the keyboard in its first
 * field, the switch off (on for a group the user is not in).
 */
static void
admin_start(
	struct se_app *app,
	enum se_admin_mode mode)
{
	struct se_users *users;
	const struct se_user_row *row;

	/* A change of the chosen user needs one (not oneself). */
	users = &app->users;
	row = admin_chosen(app);
	if (mode != SE_ADMIN_ADD && (row == NULL || row->self))
		return;

	/* The form, empty. */
	se_users_admin_wipe(users);
	users->admin_mode = mode;
	users->admin_flag = 0;
	users->admin_message[0] = '\0';
	users->keyboard = SE_USERS_KEYBOARD_ADMIN;
	users->admin_focus = admin_next_field(mode, -1, 1);
	se_log("USERS admin start mode=%d", (int)mode);
}

/*
 * Asks the desktop for the change, and wipes the fields.
 */
static void
admin_apply(
	struct se_app *app)
{
	struct se_users *users;
	const struct se_user_row *row;
	char operation[KL_ACCOUNT_OPERATION_MAX + 1U];
	const char *kind;
	const char *verb;
	uint32_t request;
	int written;
	int error;

	/* The operation's lines (kl_system_account_administer). */
	users = &app->users;
	row = admin_chosen(app);
	operation[0] = '\0';
	written = 0;
	switch (users->admin_mode) {
	case SE_ADMIN_ADD:
		/* The name, the full name, the password, and whether an administrator. */
		kind = "user";
		if (users->admin_flag)
			kind = "admin";
		written = snprintf(operation, sizeof(operation), "add\n%s\n%s\n%s\n%s\n", users->admin_fields[SE_ADMIN_NAME].text,
				   users->admin_fields[SE_ADMIN_FULL_NAME].text, users->admin_fields[SE_ADMIN_PASSWORD].text, kind);
		break;
	case SE_ADMIN_RESET:
		written = snprintf(operation, sizeof(operation), "reset-password\n%s\n%s\n", row->name, users->admin_fields[SE_ADMIN_PASSWORD].text);
		break;
	case SE_ADMIN_REMOVE:
		/* The home kept unless the switch is on. */
		kind = "keep-home";
		if (users->admin_flag)
			kind = "remove-home";
		written = snprintf(operation, sizeof(operation), "remove\n%s\n%s\n", row->name, kind);
		break;
	case SE_ADMIN_WHEEL:
	case SE_ADMIN_NETWORK:
		/* In the group or out of it, the other way from now. */
		kind = "wheel";
		verb = "group-add";
		if (users->admin_mode == SE_ADMIN_WHEEL && row->admin)
			verb = "group-remove";
		if (users->admin_mode == SE_ADMIN_NETWORK) {
			kind = "network";
			if (row->network)
				verb = "group-remove";
		}

		/* The lines. */
		written = snprintf(operation, sizeof(operation), "%s\n%s\n%s\n", verb, row->name, kind);
		break;
	default:
		return;
	}

	/* Fields too long for one operation (libkeiland's fields hold more than account-admin takes) are said so. */
	if (written < 0 || (size_t)written >= sizeof(operation)) {
		memset(operation, 0, sizeof(operation));
		(void)snprintf(users->admin_message, sizeof(users->admin_message), "The names or the password are too long.");
		users->admin_bad = 1;
		return;
	}

	/* Asked; the passwords leave with the next flush, and no copy stays. */
	error = kl_system_account_administer(app->system, users->admin_fields[SE_ADMIN_YOURS].text, operation, &request);
	memset(operation, 0, sizeof(operation));
	se_users_admin_wipe(users);
	users->admin_focus = admin_next_field(users->admin_mode, -1, 1);

	/* Not asked: said at once. */
	if (error != 0) {
		(void)snprintf(users->admin_message, sizeof(users->admin_message), "The change could not be asked (%s).", strerror(error));
		users->admin_bad = 1;
		se_log("USERS admin errno=%d", error);
		return;
	}

	/* Succeeded: the answer comes as a result. */
	users->admin_asked = 1;
	users->admin_request = request;
	(void)snprintf(users->admin_message, sizeof(users->admin_message), "Changing...");
	users->admin_bad = 0;
	se_log("USERS admin request=%u mode=%d", request, (int)users->admin_mode);
}

/*
 * Reports the next field (step 1) or the one before (step -1) a change
 * uses, from a field (-1 for the start).
 */
static int
admin_next_field(
	enum se_admin_mode mode,
	int from,
	int step)
{
	int index;
	int tries;
	int shown;

	/* Round the fields until one the change uses. */
	index = from;
	for (tries = 0; tries < SE_ADMIN_FIELDS; tries++) {
		/* The next, round. */
		index = (index + step + SE_ADMIN_FIELDS) % SE_ADMIN_FIELDS;
		shown = admin_shown(mode, index);
		if (shown)
			return index;
	}

	/* The administrator's own, which every change uses. */
	return SE_ADMIN_YOURS;
}

/*
 * Lays out (and draws, when asked) the buttons that start a change, from a
 * top edge, wrapping to a new line when the card is full.  Returns the
 * edge below them when drawn, or their height when not.
 */
static int
admin_buttons_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width,
	int draw)
{
	const struct se_user_row *row;
	const char *labels[5];
	enum se_admin_mode modes[5];
	int count;
	int left;
	int line;
	int button;
	int i;

	/* Add User; for another user chosen, the changes of that user. */
	row = admin_chosen(app);
	count = 0;
	labels[count] = "Add User...";
	modes[count] = SE_ADMIN_ADD;
	count++;
	if (row != NULL && !row->self) {
		/* Its password and its removal. */
		labels[count] = "Reset Password...";
		modes[count] = SE_ADMIN_RESET;
		count++;
		labels[count] = "Remove...";
		modes[count] = SE_ADMIN_REMOVE;
		count++;

		/* Administrator, the other way from now. */
		labels[count] = "Make Administrator";
		if (row->admin)
			labels[count] = "Remove Administrator";
		modes[count] = SE_ADMIN_WHEEL;
		count++;

		/* Wi-Fi, the other way from now. */
		labels[count] = "Allow Wi-Fi Control";
		if (row->network)
			labels[count] = "Stop Wi-Fi Control";
		modes[count] = SE_ADMIN_NETWORK;
		count++;
	}

	/* Each button, left to right, wrapping. */
	left = x + 20;
	line = 0;
	for (i = 0; i < count; i++) {
		/* A new line when it does not fit. */
		button = se_button_width(app, labels[i]);
		if (left + button > x + width - 20 && left > x + 20) {
			left = x + 20;
			line++;
		}

		/* Drawn, the change being made in the accent. */
		if (draw)
			(void)se_button_draw(app, canvas, left, y + 4 + line * ADMIN_BUTTONS, labels[i], app->users.admin_mode == modes[i], 1, ADMIN_START_FIRST + (int)modes[i]);
		left += button + 8;
	}

	/* Their height, or the edge below them. */
	if (!draw)
		return (line + 1) * ADMIN_BUTTONS;
	return y + (line + 1) * ADMIN_BUTTONS;
}

/*
 * Draws one field's row: its label at the left, the field at the right,
 * dots for a password, the placeholder when empty, and the cursor in the
 * field with the keyboard.
 */
static void
admin_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int index,
	int x,
	int y,
	int width)
{
	struct se_users *users;
	struct kl_rect box;
	unsigned kind;
	int focused;

	/* The label. */
	users = &app->users;
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(ADMIN_TEXT_ROW, y + 8, 36), admin_labels[index], ADMIN_TEXT_ROW, 0, ADMIN_FIELD_X - 30, SE_COLOR_TEXT);

	/* The field's place, and whether it has the keyboard. */
	box.x = x + ADMIN_FIELD_X;
	box.y = y + 8;
	box.width = width - ADMIN_FIELD_X - 20;
	box.height = 36;
	focused = 0;
	if (users->keyboard == SE_USERS_KEYBOARD_ADMIN && users->admin_focus == index)
		focused = 1;

	/* A click on it gives it the keyboard. */
	se_ui_hit(app, &box, SE_HIT_CONTROL, ADMIN_FIELD_FIRST + index);

	/* libkeiland's field: a password as dots, the login name plain, the full name with an input method (ws090-p007). */
	kind = SE_FIELD_TEXT;
	if (index == SE_ADMIN_NAME)
		kind = SE_FIELD_PLAIN;
	if (admin_secret[index])
		kind = SE_FIELD_SECRET;
	(void)se_field_draw(app, canvas, &users->admin_fields[index], &box, admin_placeholders[index], kind, focused);
}

/*
 * Reports the title of the change being made.
 */
static const char *
admin_title(
	const struct se_app *app)
{
	/* Each change's. */
	switch (app->users.admin_mode) {
	case SE_ADMIN_ADD:
		return "Add a user";
	case SE_ADMIN_RESET:
		return "Set a new password for the chosen user";
	case SE_ADMIN_REMOVE:
		return "Remove the chosen user";
	case SE_ADMIN_WHEEL:
		return "Change whether the chosen user is an administrator";
	case SE_ADMIN_NETWORK:
		return "Change whether the chosen user may control Wi-Fi";
	default:
		break;
	}

	/* None. */
	return "";
}

/*
 * Reports the words of the change's button.
 */
static const char *
admin_apply_label(
	const struct se_app *app)
{
	/* Each change's. */
	switch (app->users.admin_mode) {
	case SE_ADMIN_ADD:
		return "Add User";
	case SE_ADMIN_RESET:
		return "Set Password";
	case SE_ADMIN_REMOVE:
		return "Remove User";
	default:
		break;
	}

	/* The group changes. */
	return "Change";
}
