/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Users page (ws160-p002; the 2026-10-05 user request "パスワードは
 * SettingsのUsers画面でもGUI実装します。"): the account of the user Settings
 * runs as, and the change of its password.
 *
 * The account card shows the user's name, full name and home.  The list
 * card (ws089-p026) shows the people's accounts of the computer: those
 * with a user ID from 1000 and a shell to log in with, each with its full
 * name, whether it is an administrator and whether it is the user's own.
 * Both are read by the desktop (ws188-p002: libkeiland's
 * kl_system_machine_users through the compositor, machine.c); Settings
 * reads no account database itself.  The password card has three fields, the
 * current password, the new one and the new one again, shown as dots
 * unless Show is on; the field with the keyboard has the accent's edge,
 * Tab and a click move between them.  Change Password (and Enter) asks the
 * desktop (libkeiland's kl_system_account_set_password); Settings holds
 * no privilege and never runs the system's programs itself: the
 * compositor changes the password through libkeiland-backend (on zedBSD,
 * passwd).  The fields are wiped as soon as the change is asked, and the
 * answer comes as a line under the buttons: changed, the current password
 * wrong, the new one refused (at least 8 characters, not the old one), or
 * the desktop cannot change it.  A desktop without the account
 * (KL_SYSTEM_HAS_ACCOUNT) says so and offers no change.
 *
 * For an administrator, the administration's card (ws089-p026,
 * page-users-admin.c) stands under the list, whose rows are then chosen
 * with a click.  The PIN card (ws163-p003, page-users-pin.c) stands under
 * the password card.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls of the page: the three fields, Show, and Change Password. */
#define USERS_FIELD_FIRST	1
#define USERS_SHOW		10
#define USERS_CHANGE		11

/* The list's rows as controls (the administration chooses them, page-users-admin.c). */
#define USERS_ROW_FIRST		100

/* A field's row, the field's left edge in the row, and the line under the buttons. */
#define USERS_ROW		52
#define USERS_FIELD_X		210
#define USERS_MESSAGE_LINE	30

/* The text sizes of a row and of a message. */
#define USERS_TEXT_ROW		15U
#define USERS_TEXT_SUB		13U

/* The space between two cards. */
#define USERS_GAP		16

/* The fields' labels and placeholders. */
static const char *const users_labels[SE_USERS_FIELDS] = { "Current password", "New password", "New password again" };
static const char *const users_placeholders[SE_USERS_FIELDS] = { "Your password now", "At least 8 characters", "The same again" };

static int users_available(const struct se_app *app);
static void users_text(char *to, size_t size, const char *from, size_t room);
static int users_list_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int users_ready(const struct se_app *app);
static void users_change(struct se_app *app);
static void users_wipe(struct se_users *users);
static void users_field_draw(struct se_app *app, struct kl_canvas *canvas, int index, int x, int y, int width);

/*
 * Draws the Users page's cards from a top edge; returns the edge below
 * them.
 */
int
se_users_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_users *users;
	const char *reveal;
	kl_color ink;
	int administer;
	int available;
	int enabled;
	int differs;
	int change;
	int show;
	int right;
	int height;
	int bottom;
	int index;
	int y;

	/* The account and the list, asked of the desktop until they are known. */
	users = &app->users;
	se_users_load(app);

	/* The account card: the name, the full name and the home. */
	height = se_card_height(3, 1);
	y = se_card_begin(app, canvas, x, top, width, height, "Your account", NULL);
	y = se_row_value(app, canvas, x, y, width, "User name", users->name, 0);
	y = se_row_value(app, canvas, x, y, width, "Full name", users->full_name, 0);
	(void)se_row_value(app, canvas, x, y, width, "Home", users->home, 1);
	top += height + USERS_GAP;

	/* The list of the computer's users, and the administration's card for an administrator (ws089-p026). */
	top = users_list_draw(app, canvas, x, top, width) + USERS_GAP;
	administer = se_users_admin_available(app);
	if (administer)
		top = se_users_admin_draw(app, canvas, x, top, width) + USERS_GAP;

	/* The password card: the three fields, the buttons and the message. */
	available = users_available(app);
	height = 64 + SE_USERS_FIELDS * USERS_ROW + 60 + USERS_MESSAGE_LINE;
	if (!available)
		height = 64 + 50;
	y = se_card_begin(app, canvas, x, top, width, height, "Password", "Change the password you log in with.");
	if (!available) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 24, "This desktop cannot change the password here.", USERS_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		bottom = se_users_pin_draw(app, canvas, x, top + height + USERS_GAP, width);
		bottom = se_users_keys_draw(app, canvas, x, bottom + USERS_GAP, width);
		return bottom;
	}

	/* Each field. */
	for (index = 0; index < SE_USERS_FIELDS; index++) {
		users_field_draw(app, canvas, index, x, y, width);
		y += USERS_ROW;
	}

	/* The buttons at the right: Change Password, and Show or Hide left of it. */
	right = x + width - 20;
	reveal = "Show";
	if (users->shown)
		reveal = "Hide";
	change = se_button_width(app, "Change Password");
	show = se_button_width(app, reveal);
	enabled = users_ready(app);
	(void)se_button_draw(app, canvas, right - change, y + 12, "Change Password", 1, enabled, USERS_CHANGE);
	(void)se_button_draw(app, canvas, right - change - 8 - show, y + 12, reveal, 0, 1, USERS_SHOW);

	/* What the fields still need, or the last answer (green when it changed, red when it failed). */
	y += 60;
	ink = SE_COLOR_TEXT_SECONDARY;
	if (users->message[0] != '\0' && users->message_bad)
		ink = SE_COLOR_BAD;
	if (users->message[0] != '\0' && !users->message_bad)
		ink = SE_COLOR_GOOD;
	if (users->message[0] != '\0')
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, users->message, USERS_TEXT_SUB, 0, width - 40, ink);

	/* Without an answer to show, what the fields still need: the new password the same twice. */
	differs = 0;
	if (users->message[0] == '\0' && users->fields[1].length != 0 && users->fields[2].length != 0)
		differs = strcmp(users->fields[1].text, users->fields[2].text);
	if (differs != 0)
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, "The new password and its repeat differ.", USERS_TEXT_SUB, 0, width - 40, SE_COLOR_TEXT_SECONDARY);

	/* The PIN card under it (ws163-p003), and the security keys card (ws172-p003). */
	bottom = se_users_pin_draw(app, canvas, x, top + height + USERS_GAP, width);
	bottom = se_users_keys_draw(app, canvas, x, bottom + USERS_GAP, width);

	/* The edge below the cards. */
	return bottom;
}

/*
 * Carries out a click on a control of the Users page.
 */
void
se_users_press(
	struct se_app *app,
	int index)
{
	struct se_users *users;
	int ready;
	int taken;

	/* The administration's controls, and the PIN card's. */
	users = &app->users;
	taken = se_users_admin_press(app, index);
	if (taken)
		return;
	taken = se_users_pin_press(app, index);
	if (taken)
		return;
	taken = se_users_keys_press(app, index);
	if (taken)
		return;

	/* A field takes the keyboard (from the administration's or the PIN card's fields). */
	if (index >= USERS_FIELD_FIRST && index < USERS_FIELD_FIRST + SE_USERS_FIELDS) {
		users->focus = index - USERS_FIELD_FIRST;
		users->keyboard = SE_USERS_KEYBOARD_PASSWORD;
		return;
	}

	/* Show and Hide. */
	if (index == USERS_SHOW) {
		users->shown = !users->shown;
		return;
	}

	/* Change Password, when the fields are ready. */
	if (index == USERS_CHANGE) {
		ready = users_ready(app);
		if (ready)
			users_change(app);
	}
}

/*
 * Takes a key on the Users page: Tab (and Shift+Tab) moves between the
 * fields, Enter asks the change when the fields are ready and otherwise
 * goes to the next field, Esc empties the fields (or, when they are empty,
 * is the window's), and the others type.  Returns 1 when the key was used.
 */
int
se_users_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_users *users;
	int available;
	int ready;
	int used;

	/* The administration's fields, or the PIN card's, when they have the keyboard. */
	users = &app->users;
	used = se_users_admin_key(app, event);
	if (used)
		return 1;
	used = se_users_pin_key(app, event);
	if (used)
		return 1;
	used = se_users_keys_key(app, event);
	if (used)
		return 1;

	/* Without the account there is nothing to type. */
	available = users_available(app);
	if (!available)
		return 0;

	/* Tab and Shift+Tab. */
	if (event->key == SE_KEY_TAB) {
		if ((event->modifiers & SE_MOD_SHIFT) != 0U) {
			users->focus = (users->focus + SE_USERS_FIELDS - 1) % SE_USERS_FIELDS;
		} else {
			users->focus = (users->focus + 1) % SE_USERS_FIELDS;
		}

		/* Taken. */
		return 1;
	}

	/* Enter: the change, or the next field. */
	if (event->key == SE_KEY_ENTER) {
		ready = users_ready(app);
		if (ready) {
			users_change(app);
		} else {
			users->focus = (users->focus + 1) % SE_USERS_FIELDS;
		}

		/* Taken. */
		return 1;
	}

	/* Esc empties the fields; with nothing typed it is the window's (back). */
	if (event->key == SE_KEY_ESC) {
		if (users->fields[0].length == 0 && users->fields[1].length == 0 && users->fields[2].length == 0)
			return 0;
		users_wipe(users);
		users->message[0] = '\0';
		return 1;
	}

	/* Anything else types into the field with the keyboard (or is not the page's). */
	used = se_field_key(&users->fields[users->focus], event);
	if (used == 0)
		return 0;

	/* A new character takes the last answer away. */
	if (!users->asked)
		users->message[0] = '\0';

	/* Succeeded: the field took the key. */
	return 1;
}

/*
 * Takes the answer of the password change when it is the Users page's.
 * Returns 1 when the request was the page's.
 */
int
se_users_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_users *users;
	const char *message;
	int bad;
	int taken;

	/* The administration's change, and the PIN's. */
	taken = se_users_admin_result(app, request, error);
	if (taken)
		return 1;
	taken = se_users_pin_result(app, request, error);
	if (taken)
		return 1;
	taken = se_users_keys_result(app, request, error);
	if (taken)
		return 1;

	/* Only the change the page asked. */
	users = &app->users;
	if (!users->asked || request != users->request)
		return 0;
	users->asked = 0;

	/* What the answer says. */
	bad = 1;
	switch (error) {
	case 0:
		message = "Your password is changed. Use the new one from now on.";
		bad = 0;
		break;
	case EPERM:
		message = "The current password is wrong.";
		users->focus = 0;
		break;
	case EINVAL:
		message = "The new password is not accepted: use at least 8 characters, not the old password.";
		users->focus = 1;
		break;
	case ENOTSUP:
		message = "This desktop cannot change the password.";
		break;
	case EBUSY:
		message = "Another change of the password is under way. Try again in a moment.";
		break;
	default:
		message = "The password could not be changed.";
		break;
	}

	/* Shown under the buttons, and logged for the tests (without any password). */
	(void)snprintf(users->message, sizeof(users->message), "%s", message);
	users->message_bad = bad;
	app->dirty = 1;
	se_log("USERS result request=%u errno=%d", request, error);

	/* Succeeded: the answer was the page's. */
	return 1;
}

/* Tells whether the desktop offers the account (KL_SYSTEM_HAS_ACCOUNT). */
static int
users_available(
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

	/* Succeeded: offered. */
	return 1;
}

/* Wipes the password fields at the window's end. */
void
se_users_close(
	struct se_app *app)
{
	/* The three fields, the administration's, the PIN card's and the keys card's. */
	users_wipe(&app->users);
	se_users_admin_wipe(&app->users);
	se_users_pin_wipe(&app->users);
	se_users_keys_wipe(&app->users);
}

/*
 * Asks for the user's account and the list of users until they are known,
 * for a page that needs to know whether the user is an administrator
 * before the Users page was shown (the Languages page's system language,
 * ws158-p004).
 */
void
se_users_load(
	struct se_app *app)
{
	/* Wanted until the desktop's answer is copied (machine.c). */
	se_machine_want(app, KL_MACHINE_USERS);
}

/*
 * Asks for the list again at once, after a change of the accounts was
 * made (the administration's, ws089-p026).
 */
void
se_users_reload(
	struct se_app *app)
{
	/* A new reading, whatever reading waits. */
	(void)se_machine_ask_now(app, KL_MACHINE_USERS | KL_MACHINE_LOGIN_LANGUAGE);
}

/*
 * Copies the users of the desktop's last answer: the own account's name,
 * full name and home and whether it is an administrator, and the people's
 * accounts for the list.  The administration's choice follows its user by
 * name (a list read again may put the rows in another order), and is let
 * go when the user is gone.
 */
void
se_users_copy(
	struct se_app *app)
{
	static struct kl_machine_user list[KL_MACHINE_USERS_MAX];
	struct se_users *users;
	struct se_user_row *row;
	size_t count;
	size_t index;
	int differs;

	/* The accounts of the last answer. */
	users = &app->users;
	count = kl_system_machine_users(app->system, list, KL_MACHINE_USERS_MAX);
	users->read = 1;

	/* The own account, when the answer has it. */
	users->name[0] = '\0';
	users->full_name[0] = '\0';
	users->home[0] = '\0';
	users->self_admin = 0;
	for (index = 0; index < count; index++) {
		if ((list[index].flags & KL_MACHINE_USER_SELF) == 0U)
			continue;

		/* Its names, its home, and whether it administers. */
		users_text(users->name, sizeof(users->name), list[index].name, sizeof(list[index].name));
		users_text(users->full_name, sizeof(users->full_name), list[index].full_name, sizeof(list[index].full_name));
		users_text(users->home, sizeof(users->home), list[index].home, sizeof(list[index].home));
		if ((list[index].flags & KL_MACHINE_USER_ADMIN) != 0U)
			users->self_admin = 1;
		se_log("USERS account name=%s", users->name);
	}

	/* The people's accounts, as far as the list has room. */
	users->row_count = 0;
	for (index = 0; index < count; index++) {
		if ((list[index].flags & KL_MACHINE_USER_PERSON) == 0U)
			continue;
		if (users->row_count == SE_USERS_LIST_MAX)
			break;

		/* Its name, its full name, and what it is. */
		row = &users->rows[users->row_count];
		memset(row, 0, sizeof(*row));
		users_text(row->name, sizeof(row->name), list[index].name, sizeof(list[index].name));
		users_text(row->full_name, sizeof(row->full_name), list[index].full_name, sizeof(list[index].full_name));
		if ((list[index].flags & KL_MACHINE_USER_ADMIN) != 0U)
			row->admin = 1;
		if ((list[index].flags & KL_MACHINE_USER_NETWORK) != 0U)
			row->network = 1;
		if ((list[index].flags & KL_MACHINE_USER_SELF) != 0U)
			row->self = 1;
		users->row_count++;
	}

	/* The administration's choice, found again by its name (none chosen: nothing to find). */
	users->selected = 0;
	if (users->selected_name[0] == '\0') {
		se_log("USERS list count=%d", users->row_count);
		return;
	}
	for (index = 0; index < (size_t)users->row_count; index++) {
		/* The chosen user's row. */
		differs = strcmp(users->rows[index].name, users->selected_name);
		if (differs == 0)
			users->selected = (int)index + 1;
	}

	/* A chosen user who is gone is chosen no longer, nor is a change of it made. */
	if (users->selected == 0) {
		users->selected_name[0] = '\0';
		if (users->admin_mode != SE_ADMIN_NONE && users->admin_mode != SE_ADMIN_ADD) {
			se_users_admin_wipe(users);
			users->admin_mode = SE_ADMIN_NONE;
		}
	}

	/* The count in the log. */
	se_log("USERS list count=%d", users->row_count);
}

/*
 * Draws the list card: one row a user, its name and what it is.  Returns
 * the edge below the card.
 */
static int
users_list_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_users *users;
	const struct se_user_row *row;
	struct kl_rect box;
	char line[256];
	const char *role;
	const char *wifi;
	const char *own;
	const char *shown;
	int administer;
	int last;
	int rows;
	int height;
	int y;
	int i;

	/* One row a user, one at least (saying there is none); chosen with a click by an administrator. */
	users = &app->users;
	administer = se_users_admin_available(app);
	rows = users->row_count;
	if (rows == 0)
		rows = 1;

	/* The card. */
	height = se_card_height(rows, 1);
	y = se_card_begin(app, canvas, x, top, width, height, "Users on this computer", NULL);
	if (users->row_count == 0) {
		(void)se_row_value(app, canvas, x, y, width, "None", "No other accounts to show.", 1);
		return top + height;
	}

	/* Each user: the full name, whether an administrator, and whether it is you. */
	for (i = 0; i < users->row_count; i++) {
		/* The words of the row. */
		row = &users->rows[i];
		role = "Standard";
		if (row->admin)
			role = "Administrator";

		/* Whether it may control Wi-Fi, and whether it is the user's own. */
		wifi = "";
		if (row->network)
			wifi = " \xc2\xb7 Wi-Fi";
		own = "";
		if (row->self)
			own = " \xc2\xb7 You";

		/* The full name, or the name when there is none. */
		shown = row->full_name;
		if (row->full_name[0] == '\0')
			shown = row->name;

		/* The row, the last without the line under it. */
		last = 0;
		if (i + 1 == users->row_count)
			last = 1;

		/* Drawn; for an administrator a click chooses it, and the chosen one has the selection's ground. */
		(void)snprintf(line, sizeof(line), "%s \xc2\xb7 %s%s%s", shown, role, wifi, own);
		box.x = x + 8;
		box.y = y;
		box.width = width - 16;
		box.height = se_row_value(app, canvas, x, y, width, row->name, line, last) - y;
		if (administer)
			se_ui_hit(app, &box, SE_HIT_CONTROL, USERS_ROW_FIRST + i);
		if (administer && users->selected == i + 1)
			kl_canvas_round(canvas, (float)box.x, (float)box.y + 2.0f, (float)box.width, (float)box.height - 4.0f, 8.0f, SE_COLOR_SELECTION);
		y += box.height;
	}

	/* The edge below the card. */
	return top + height;
}

/* Tells whether the fields are ready for the change: all typed, the new one twice the same, and none asked yet. */
static int
users_ready(
	const struct se_app *app)
{
	const struct se_users *users;
	int same;

	/* One change at a time. */
	users = &app->users;
	if (users->asked)
		return 0;

	/* All three typed. */
	if (users->fields[0].length == 0 || users->fields[1].length == 0 || users->fields[2].length == 0)
		return 0;

	/* The new one twice the same. */
	same = strcmp(users->fields[1].text, users->fields[2].text);
	if (same != 0)
		return 0;

	/* Succeeded: ready. */
	return 1;
}

/* Asks the desktop for the change, and wipes the fields. */
static void
users_change(
	struct se_app *app)
{
	struct se_users *users;
	uint32_t request;
	int error;

	/* Asked; the passwords leave with the next flush. */
	users = &app->users;
	error = kl_system_account_set_password(app->system, users->fields[0].text, users->fields[1].text, &request);
	users_wipe(users);
	users->focus = 0;

	/* Not asked: said at once. */
	if (error != 0) {
		(void)snprintf(users->message, sizeof(users->message), "The password could not be changed (%s).", strerror(error));
		users->message_bad = 1;
		se_log("USERS change errno=%d", error);
		return;
	}

	/* Succeeded: the answer comes as a result. */
	users->asked = 1;
	users->request = request;
	(void)snprintf(users->message, sizeof(users->message), "Changing the password...");
	users->message_bad = 0;
	se_log("USERS change request=%u", request);
}

/* Wipes the three fields. */
static void
users_wipe(
	struct se_users *users)
{
	int index;

	/* Each field (se_field_clear overwrites its text). */
	for (index = 0; index < SE_USERS_FIELDS; index++)
		se_field_clear(&users->fields[index]);
}

/* Draws one field's row: its label at the left, the field at the right, dots or the text, and the cursor in the field with the keyboard. */
static void
users_field_draw(
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
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(USERS_TEXT_ROW, y + 8, 36), users_labels[index], USERS_TEXT_ROW, 0, USERS_FIELD_X - 30, SE_COLOR_TEXT);

	/* The field's place, and whether it has the keyboard. */
	box.x = x + USERS_FIELD_X;
	box.y = y + 8;
	box.width = width - USERS_FIELD_X - 20;
	box.height = 36;
	focused = 0;
	if (users->keyboard == SE_USERS_KEYBOARD_PASSWORD && users->focus == index)
		focused = 1;

	/* A click on it gives it the keyboard. */
	se_ui_hit(app, &box, SE_HIT_CONTROL, USERS_FIELD_FIRST + index);

	/* libkeiland's field: the passwords as dots, or plain when shown; never an input method (ws090-p007). */
	kind = SE_FIELD_SECRET;
	if (users->shown)
		kind = SE_FIELD_PLAIN;
	(void)se_field_draw(app, canvas, &users->fields[index], &box, users_placeholders[index], kind, focused);
}

/*
 * Copies a text of the desktop's answer (its room of room bytes, ended
 * within it) into a field of size bytes, cut to fit.
 */
static void
users_text(
	char *to,
	size_t size,
	const char *from,
	size_t room)
{
	size_t length;

	/* The text's length within its room, and as much as fits. */
	length = strnlen(from, room);
	if (length >= size)
		length = size - 1U;

	/* The bytes, ended. */
	memcpy(to, from, length);
	to[length] = '\0';
}
