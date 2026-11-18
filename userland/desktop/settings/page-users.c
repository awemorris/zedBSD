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
 * reads no account database itself.  The password card's Change Password
 * and the Sign-in Methods card open popups (WS200, page-users-password.c);
 * Settings holds no privilege and never runs the system's programs
 * itself: the compositor changes the password through libkeiland-backend
 * (on zedBSD, passwd).
 *
 * For an administrator, the administration's card (ws089-p026,
 * page-users-admin.c) stands under the list, whose rows are then chosen
 * with a click.  Under the password card, a card leads to the Security
 * Keys page (ws199-p001), which has the PIN and the security keys.
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The control of the page that leads to the Security Keys page (the password's are page-users-password.c's). */
#define USERS_KEYS		12

/* The list's rows as controls (the administration chooses them, page-users-admin.c). */
#define USERS_ROW_FIRST		100

/* The text size of a card's line. */
#define USERS_TEXT_SUB		13U

/* The space between two cards. */
#define USERS_GAP		16

static void users_text(char *to, size_t size, const char *from, size_t room);
static int users_list_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int users_keys_draw(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);

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
	int administer;
	int height;
	int bottom;
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

	/* The password and the sign-in methods (WS200), then the way to the PIN and the security keys (ws199-p001). */
	top = se_password_draw(app, canvas, x, top, width) + USERS_GAP;
	bottom = users_keys_draw(app, canvas, x, top, width);

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
	int taken;

	/* The administration's controls, then the password's and the methods' (WS200). */
	taken = se_users_admin_press(app, index);
	if (taken)
		return;
	taken = se_password_press(app, index);
	if (taken)
		return;

	/* The Security Keys page. */
	if (index == USERS_KEYS)
		se_ui_go(app, SE_PAGE_SECURITY_KEYS);
}

/*
 * Takes a key on the Users page: the administration's fields are the only
 * ones of the page (the password's are in the popup, which takes the keys
 * while it is open).  Returns 1 when the key was used.
 */
int
se_users_key(
	struct se_app *app,
	const struct se_event *event)
{
	int used;

	/* The administration's fields, when they have the keyboard. */
	used = se_users_admin_key(app, event);
	if (used)
		return 1;

	/* Nothing else of the page types. */
	return 0;
}

/*
 * Takes the answer of a change the Users page asked (the administration's,
 * the Security Keys page's, the password's or the methods').  Returns 1
 * when the request was the page's.
 */
int
se_users_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	int taken;

	/* The administration's change, and the Security Keys page's (ws199-p001). */
	taken = se_users_admin_result(app, request, error);
	if (taken)
		return 1;
	taken = se_keys_result(app, request, error);
	if (taken)
		return 1;

	/* The password's or the methods' (WS200). */
	taken = se_password_result(app, request, error);
	if (taken)
		return 1;

	/* Not the page's. */
	return 0;
}

/* Draws the card that leads to the Security Keys page (ws199-p001); returns the edge below it. */
static int
users_keys_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int button;
	int height;
	int y;

	/* The card: what the page has, and its button at the right. */
	height = 64 + 56;
	y = se_card_begin(app, canvas, x, top, width, height, "PIN and security keys", "Sign in and unlock with a PIN or a FIDO2 security key.");
	(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 26, "They are set on the Security Keys page.", USERS_TEXT_SUB, 0, width - 200, SE_COLOR_TEXT_SECONDARY);
	button = se_button_width(app, "Security Keys");
	(void)se_button_draw(app, canvas, x + width - 20 - button, y + 8, "Security Keys", 0, 1, USERS_KEYS);

	/* The edge below the card. */
	return top + height;
}

/* Wipes what the page keeps at the window's end. */
void
se_users_close(
	struct se_app *app)
{
	/* The password kept, the administration's fields, and the Security Keys page's. */
	se_password_end(app);
	se_users_admin_wipe(&app->users);
	se_keys_close(app);
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
