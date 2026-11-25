/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Remote Desktop page:
 *
 *   Remote Desktop  the switch of the system's RDP server (rdpd), on now and
 *                   at every start, which only root and the members of wheel
 *                   may turn; whether it runs, its port, and the address to
 *                   connect with.
 *   RDP User        the current session user's registration status in
 *                   /var/db/rdpd/ntlm.users, and a password form to register
 *                   or update the user's NTLM hash via rdpd.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The page's controls (hit indices). */
#define RDP_SWITCH		1
#define RDP_PASSWORD_FIELD	2
#define RDP_REGISTER_BUTTON	3

/* Layout constants. */
#define RDP_PAD			18
#define RDP_GAP			16
#define RDP_FIELD_X		180
#define RDP_TEXT_TITLE		15U
#define RDP_TEXT_SMALL		13U

static void rdp_connect_address(const struct se_app *app, unsigned port, char *text, size_t size);
static int rdp_state_same(const struct kl_rdp_state *old_state, const struct kl_rdp_state *new_state);
static void rdp_submit_user(struct se_app *app);

/*
 * Draws the Remote Desktop page's cards from a top edge; returns the edge
 * below them.
 */
int
se_remote_desktop_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_remote_desktop *rdp;
	const struct kl_rdp_state *state;
	struct kl_text_line line;
	struct kl_rect box;
	char port[16];
	char address[96];
	const char *status;
	const char *note;
	const char *user_name;
	const char *reg_status;
	const char *btn_label;
	kl_color ink;
	int capable;
	int enabled;
	int btn_w;
	int height;
	int y;

	rdp = &app->remote_desktop;
	state = &rdp->state;
	capable = (kl_system_capabilities(app->system) & KL_SYSTEM_HAS_SHARING) != 0U;

	/* Service card: toggle switch + status, port, connect address. */
	height = se_card_height(3, 1) + 56 + 18;
	y = se_card_begin(app, canvas, x, top, width, height, "Remote Desktop", "Other computers may connect to this desktop with RDP.");

	kl_text_metrics(app->text, RDP_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + RDP_PAD + 2, y + 10 + line.ascent, "Remote Desktop (RDP)", RDP_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	note = "Turned on, it also starts with the computer.";
	if (!capable)
		note = "Remote Desktop is set by this desktop's own tools.";
	else if (!state->available)
		note = "The RDP server is not installed on this computer.";
	else if (!state->allowed)
		note = "Only an administrator (a member of wheel) can change this.";
	(void)kl_text_draw_fit(app->text, canvas, x + RDP_PAD + 2, y + 32 + line.ascent, note, RDP_TEXT_SMALL, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
	enabled = capable && state->available && state->allowed && rdp->request == 0U;
	se_toggle_draw(app, canvas, x + width - RDP_PAD - 44, y + 16, state->enabled, enabled, RDP_SWITCH);
	y += 56;

	status = "Stopped";
	if (state->running)
		status = "Running";
	if (!state->available)
		status = "Not installed";
	if (rdp->request != 0U)
		status = "Changing...";
	(void)snprintf(port, sizeof(port), "%u", state->port);
	if (state->port == 0U)
		(void)snprintf(port, sizeof(port), "%s", "-");
	rdp_connect_address(app, state->port != 0U ? state->port : 3389U, address, sizeof(address));
	y = se_row_value(app, canvas, x, y, width, "Status", status, 0);
	y = se_row_value(app, canvas, x, y, width, "Port", port, 0);
	(void)se_row_value(app, canvas, x, y, width, "Connect to", address, 1);

	y = top + height;
	if (rdp->message[0] != '\0') {
		ink = SE_COLOR_TEXT_SECONDARY;
		if (rdp->message_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, rdp->message, RDP_TEXT_SMALL, 0, width, ink);
		y += 30;
	}

	/* User registration card: current user, NTLM status, password field, register button. */
	top = y + RDP_GAP;
	height = se_card_height(2, 1) + 56 + 18;
	y = se_card_begin(app, canvas, x, top, width, height, "RDP User Account", "Register your account password for NTLM / CredSSP login.");

	user_name = "Current user";
	if (app->users.name[0] != '\0')
		user_name = app->users.name;
	reg_status = state->user_registered ? "Registered" : "Not registered";
	y = se_row_value(app, canvas, x, y, width, "Account", user_name, 0);
	y = se_row_value(app, canvas, x, y, width, "NTLM status", reg_status, 0);

	(void)kl_text_draw_fit(app->text, canvas, x + RDP_PAD + 2, kl_text_center(RDP_TEXT_TITLE, y + 10, 36), "Password", RDP_TEXT_TITLE, 0, RDP_FIELD_X - 24, SE_COLOR_TEXT);
	btn_label = state->user_registered ? "Update" : "Register";
	btn_w = se_button_width(app, btn_label);
	box.x = x + RDP_FIELD_X;
	box.y = y + 10;
	box.width = width - RDP_FIELD_X - RDP_PAD - btn_w - 12;
	if (box.width < 120)
		box.width = 120;
	box.height = 36;
	se_ui_hit(app, &box, SE_HIT_CONTROL, RDP_PASSWORD_FIELD);
	(void)se_field_draw(app, canvas, &rdp->password, &box, "Account password", SE_FIELD_SECRET, rdp->focused);

	enabled = capable && rdp->user_request == 0U;
	(void)se_button_draw(app, canvas, x + width - RDP_PAD - btn_w, y + 10, btn_label, 1, enabled, RDP_REGISTER_BUTTON);

	y = top + height;
	if (rdp->user_message[0] != '\0') {
		ink = rdp->user_message_bad ? SE_COLOR_BAD : SE_COLOR_GOOD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, rdp->user_message, RDP_TEXT_SMALL, 0, width, ink);
		y += 30;
	}

	return y;
}

/*
 * Carries out a click on a control of the Remote Desktop page.
 */
void
se_remote_desktop_press(
	struct se_app *app,
	int index)
{
	struct se_remote_desktop *rdp;
	unsigned on;
	int error;

	rdp = &app->remote_desktop;
	if (app->system == NULL)
		return;

	if (index == RDP_SWITCH) {
		if (rdp->request != 0U)
			return;
		on = !rdp->state.enabled;
		error = kl_system_sharing_set_rdp(app->system, on, &rdp->request);
		se_log("RDP set=%u error=%d", on, error);
		rdp->message[0] = '\0';
		if (error != 0) {
			rdp->request = 0U;
			(void)snprintf(rdp->message, sizeof(rdp->message), "%s", "This desktop cannot change Remote Desktop.");
			rdp->message_bad = 1;
		}
		app->dirty = 1;
		return;
	}

	if (index == RDP_PASSWORD_FIELD) {
		rdp->focused = 1;
		app->dirty = 1;
		return;
	}

	if (index == RDP_REGISTER_BUTTON) {
		rdp_submit_user(app);
	}
}

/*
 * Handles keyboard input for the password field on the Remote Desktop page.
 */
int
se_remote_desktop_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_remote_desktop *rdp;
	int used;

	rdp = &app->remote_desktop;
	if (!rdp->focused)
		return 0;

	if (event->key == SE_KEY_ENTER) {
		rdp_submit_user(app);
		return 1;
	}
	if (event->key == SE_KEY_ESC) {
		rdp->focused = 0;
		app->dirty = 1;
		return 1;
	}
	if (rdp->user_request != 0U)
		return 1;

	used = se_field_key(&rdp->password, event);
	if (used == 0)
		return 0;
	if (rdp->user_message_bad)
		rdp->user_message[0] = '\0';
	app->dirty = 1;
	return 1;
}

/*
 * Follows Remote Desktop's state: asked when the page is first shown,
 * updated when the desktop tells it.
 */
void
se_remote_desktop_poll(
	struct se_app *app)
{
	struct se_remote_desktop *rdp;
	struct kl_rdp_state state;
	int same;
	int error;

	rdp = &app->remote_desktop;
	if (app->system == NULL)
		return;

	if (app->page == SE_PAGE_REMOTE_DESKTOP && !rdp->asked) {
		rdp->asked = 1;
		error = kl_system_sharing_query_rdp(app->system, NULL);
		se_log("RDP query error=%d", error);
	}

	if ((app->system_changed & KL_SYSTEM_CHANGED_RDP) != 0U) {
		kl_system_sharing_get_rdp_state(app->system, &state);
		same = rdp_state_same(&rdp->state, &state);
		rdp->state = state;
		se_log("RDP state available=%u enabled=%u running=%u port=%u allowed=%u user_registered=%u",
		    rdp->state.available, rdp->state.enabled, rdp->state.running,
		    rdp->state.port, rdp->state.allowed, rdp->state.user_registered);
		if (same == 0 && app->page == SE_PAGE_REMOTE_DESKTOP)
			app->dirty = 1;
	}
}

/*
 * Takes the answer of a Remote Desktop request when it is this page's.
 * Returns 1 when consumed.
 */
int
se_remote_desktop_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_remote_desktop *rdp;

	rdp = &app->remote_desktop;
	if (rdp->request != 0U && request == rdp->request) {
		rdp->request = 0U;
		se_log("RDP service result errno=%d", error);
		app->dirty = 1;
		rdp->message_bad = 1;
		switch (error) {
		case 0:
			rdp->message[0] = '\0';
			rdp->message_bad = 0;
			break;
		case EPERM:
		case EACCES:
			(void)snprintf(rdp->message, sizeof(rdp->message), "%s", "Only an administrator (a member of wheel) can change Remote Desktop.");
			break;
		case EBUSY:
			(void)snprintf(rdp->message, sizeof(rdp->message), "%s", "Another change is under way. Try again in a moment.");
			break;
		case ENOTSUP:
			(void)snprintf(rdp->message, sizeof(rdp->message), "%s", "This desktop cannot change Remote Desktop.");
			break;
		default:
			(void)snprintf(rdp->message, sizeof(rdp->message), "%s", "Remote Desktop could not be changed.");
			break;
		}
		return 1;
	}

	if (rdp->user_request != 0U && request == rdp->user_request) {
		rdp->user_request = 0U;
		se_field_clear(&rdp->password);
		se_log("RDP user result errno=%d", error);
		app->dirty = 1;
		switch (error) {
		case 0:
			(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "User registered for Remote Desktop.");
			rdp->user_message_bad = 0;
			break;
		case EACCES:
			(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "Incorrect password.");
			rdp->user_message_bad = 1;
			break;
		case EBUSY:
			(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "Another change is under way. Try again in a moment.");
			rdp->user_message_bad = 1;
			break;
		case ENOTSUP:
			(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "This desktop cannot register Remote Desktop users.");
			rdp->user_message_bad = 1;
			break;
		default:
			(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "Could not register user for Remote Desktop.");
			rdp->user_message_bad = 1;
			break;
		}
		return 1;
	}

	return 0;
}

/* Wipes the password field when closing. */
void
se_remote_desktop_close(
	struct se_app *app)
{
	se_field_clear(&app->remote_desktop.password);
}

/* Submits the password to register/update the session user in ntlm.users. */
static void
rdp_submit_user(
	struct se_app *app)
{
	struct se_remote_desktop *rdp;
	int error;

	rdp = &app->remote_desktop;
	if (rdp->user_request != 0U || app->system == NULL)
		return;
	if (rdp->password.length == 0U || rdp->password.text[0] == '\0') {
		(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "Enter your account password.");
		rdp->user_message_bad = 1;
		app->dirty = 1;
		return;
	}

	error = kl_system_sharing_add_rdp_user(app->system, rdp->password.text, &rdp->user_request);
	se_log("RDP add_user error=%d", error);
	if (error != 0) {
		rdp->user_request = 0U;
		se_field_clear(&rdp->password);
		(void)snprintf(rdp->user_message, sizeof(rdp->user_message), "%s", "This desktop cannot register Remote Desktop users.");
		rdp->user_message_bad = 1;
	} else {
		rdp->user_message[0] = '\0';
		rdp->user_message_bad = 0;
	}
	app->dirty = 1;
}

/* Formats the RDP connection address (IPv4:port). */
static void
rdp_connect_address(
	const struct se_app *app,
	unsigned port,
	char *text,
	size_t size)
{
	const struct se_network *network;
	const char *address;
	size_t index;
	int differs;

	network = &app->network;
	address = NULL;
	for (index = 0; index < network->link_count && address == NULL; index++) {
		differs = strcmp(network->links[index].name, network->state.interface);
		if (differs == 0 && network->links[index].address[0] != '\0')
			address = network->links[index].address;
	}
	for (index = 0; index < network->link_count && address == NULL; index++) {
		if (!network->links[index].loopback && network->links[index].address[0] != '\0')
			address = network->links[index].address;
	}
	if (address == NULL) {
		(void)snprintf(text, size, "%s", "Not connected to a network");
		return;
	}
	if (port == 3389U)
		(void)snprintf(text, size, "%s", address);
	else
		(void)snprintf(text, size, "%s:%u", address, port);
}

/* Tells whether two Remote Desktop states are identical. */
static int
rdp_state_same(
	const struct kl_rdp_state *old_state,
	const struct kl_rdp_state *new_state)
{
	if (old_state->available != new_state->available)
		return 0;
	if (old_state->enabled != new_state->enabled)
		return 0;
	if (old_state->running != new_state->running)
		return 0;
	if (old_state->port != new_state->port)
		return 0;
	if (old_state->allowed != new_state->allowed)
		return 0;
	if (old_state->user_registered != new_state->user_registered)
		return 0;
	return 1;
}
