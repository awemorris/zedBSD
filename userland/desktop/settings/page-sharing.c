/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Sharing page (ws089-p025):
 *
 *   Remote Login  the switch of the system's SSH server (sshd), on now and
 *                 at every start, which only root and the members of wheel
 *                 may turn; whether it runs, its port, the host key's
 *                 fingerprint, and what to type to reach this machine.
 *   Cloud storage what comes later (WS146, WS147).
 *
 * The desktop turns it through the session's manager (kl_system_sharing_*).
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The switch (hit index). */
#define SHARING_SWITCH		1

/* The card's margin, the space between cards, and the text sizes. */
#define SHARING_PAD		18
#define SHARING_GAP		16
#define SHARING_TEXT_TITLE	15U
#define SHARING_TEXT_SMALL	13U

static void sharing_address(const struct se_app *app, char *text, size_t size);

/*
 * Draws the Sharing page's cards from a top edge; returns the edge below
 * them.
 */
int
se_sharing_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_sharing *sharing;
	const struct kl_sharing_state *state;
	struct kl_text_line line;
	char port[16];
	char address[96];
	const char *status;
	const char *note;
	kl_color ink;
	int capable;
	int enabled;
	int height;
	int y;

	/* The card: the switch with its line, then the state's rows. */
	sharing = &app->sharing;
	state = &sharing->state;
	capable = (kl_system_capabilities(app->system) & KL_SYSTEM_HAS_SHARING) != 0U;
	height = se_card_height(4, 1) + 56 + 18;
	y = se_card_begin(app, canvas, x, top, width, height, "Remote Login", "Other computers may log in here with SSH.");

	/* The switch: on while it starts with the system; only for root and wheel. */
	kl_text_metrics(app->text, SHARING_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x + SHARING_PAD + 2, y + 10 + line.ascent, "Remote Login (SSH)", SHARING_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
	note = "Turned on, it also starts with the computer.";
	if (!capable)
		note = "Remote Login is set by this desktop's own tools.";
	else if (!state->available)
		note = "The SSH server is not installed on this computer.";
	else if (!state->allowed)
		note = "Only an administrator (a member of wheel) can change this.";
	(void)kl_text_draw_fit(app->text, canvas, x + SHARING_PAD + 2, y + 32 + line.ascent, note, SHARING_TEXT_SMALL, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
	enabled = capable && state->available && state->allowed && sharing->request == 0U;
	se_toggle_draw(app, canvas, x + width - SHARING_PAD - 44, y + 16, state->enabled, enabled, SHARING_SWITCH);
	y += 56;

	/* Whether it runs, its port, the key, and how to reach it. */
	status = "Stopped";
	if (state->running)
		status = "Running";
	if (!state->available)
		status = "Not installed";
	if (sharing->request != 0U)
		status = "Changing...";
	(void)snprintf(port, sizeof(port), "%u", state->port);
	if (state->port == 0U)
		(void)snprintf(port, sizeof(port), "%s", "-");
	sharing_address(app, address, sizeof(address));
	y = se_row_value(app, canvas, x, y, width, "Status", status, 0);
	y = se_row_value(app, canvas, x, y, width, "Port", port, 0);
	if (state->fingerprint[0] != '\0')
		y = se_row_value(app, canvas, x, y, width, "Host key", state->fingerprint, 0);
	else
		y = se_row_value(app, canvas, x, y, width, "Host key", "Made when the server first starts", 0);
	(void)se_row_value(app, canvas, x, y, width, "Log in with", address, 1);

	/* The last answer, under the card. */
	y = top + height;
	if (sharing->message[0] != '\0') {
		ink = SE_COLOR_TEXT_SECONDARY;
		if (sharing->message_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, sharing->message, SHARING_TEXT_SMALL, 0, width, ink);
		y += 30;
	}

	/* What comes later. */
	top = y + SHARING_GAP;
	y = se_card_begin(app, canvas, x, top, width, 64 + 40, "Cloud storage", NULL);
	(void)kl_text_draw_fit(app->text, canvas, x + SHARING_PAD + 2, y + 18, "OneDrive and Kei's own storage come in a later version of Kei.", SHARING_TEXT_SMALL, 0, width - 2 * SHARING_PAD, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the cards. */
	return top + 64 + 40;
}

/*
 * Carries out a click on the switch: Remote Login on or off.
 */
void
se_sharing_press(
	struct se_app *app,
	int index)
{
	struct se_sharing *sharing;
	unsigned on;
	int error;

	/* Only the switch, while nothing is awaited. */
	sharing = &app->sharing;
	if (index != SHARING_SWITCH || sharing->request != 0U || app->system == NULL)
		return;

	/* The other way, asked of the desktop. */
	on = !sharing->state.enabled;
	error = kl_system_sharing_set_ssh(app->system, on, &sharing->request);
	se_log("SHARING set ssh=%u error=%d", on, error);
	sharing->message[0] = '\0';
	if (error != 0) {
		sharing->request = 0U;
		(void)snprintf(sharing->message, sizeof(sharing->message), "%s", "This desktop cannot change Remote Login.");
		sharing->message_bad = 1;
	}

	/* The page shows it. */
	app->dirty = 1;
}

/*
 * Follows Remote Login's state: asked for when the page is first shown,
 * taken when the desktop tells it.
 */
void
se_sharing_poll(
	struct se_app *app)
{
	struct se_sharing *sharing;
	int error;

	/* Nothing without the desktop's system. */
	sharing = &app->sharing;
	if (app->system == NULL)
		return;

	/* Asked for once the page is shown. */
	if (app->page == SE_PAGE_SHARING && !sharing->asked) {
		sharing->asked = 1;
		error = kl_system_sharing_query(app->system, NULL);
		se_log("SHARING query error=%d", error);
	}

	/* A new state. */
	if ((app->system_changed & KL_SYSTEM_CHANGED_SHARING) != 0U) {
		kl_system_sharing_get_state(app->system, &sharing->state);
		se_log("SHARING state available=%u enabled=%u running=%u port=%u allowed=%u fingerprint=%s", sharing->state.available,
		    sharing->state.enabled, sharing->state.running, sharing->state.port, sharing->state.allowed, sharing->state.fingerprint);
		app->dirty = 1;
	}
}

/*
 * Takes the answer of a switch's request when it is the page's.  Returns 1
 * when it was.
 */
int
se_sharing_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_sharing *sharing;

	/* Only the request the page asked. */
	sharing = &app->sharing;
	if (sharing->request == 0U || request != sharing->request)
		return 0;
	sharing->request = 0U;
	se_log("SHARING result errno=%d", error);
	app->dirty = 1;

	/* What it says. */
	sharing->message_bad = 1;
	switch (error) {
	case 0:
		sharing->message[0] = '\0';
		sharing->message_bad = 0;
		break;
	case EPERM:
	case EACCES:
		(void)snprintf(sharing->message, sizeof(sharing->message), "%s", "Only an administrator (a member of wheel) can change Remote Login.");
		break;
	case EBUSY:
		(void)snprintf(sharing->message, sizeof(sharing->message), "%s", "Another change is under way. Try again in a moment.");
		break;
	case ENOTSUP:
		(void)snprintf(sharing->message, sizeof(sharing->message), "%s", "This desktop cannot change Remote Login.");
		break;
	default:
		(void)snprintf(sharing->message, sizeof(sharing->message), "%s", "Remote Login could not be changed.");
		break;
	}

	/* Succeeded: the answer was the page's. */
	return 1;
}

/* Says what to type to log in here: ssh, this user, and the IPv4 address of the network in use. */
static void
sharing_address(
	const struct se_app *app,
	char *text,
	size_t size)
{
	const struct se_network *network;
	const char *user;
	const char *address;
	size_t index;
	int differs;

	/* The user, as the desktop told the account (ws188-p002). */
	user = "you";
	if (app->users.name[0] != '\0')
		user = app->users.name;

	/* The address of the interface in use, else the first that has one (not the loopback). */
	network = &app->network;
	address = NULL;
	for (index = 0; index < network->link_count && address == NULL; index++) {
		differs = strcmp(network->links[index].name, network->state.interface);
		if (differs == 0 && network->links[index].address[0] != '\0')
			address = network->links[index].address;
	}

	/* Else the first with an address. */
	for (index = 0; index < network->link_count && address == NULL; index++) {
		if (!network->links[index].loopback && network->links[index].address[0] != '\0')
			address = network->links[index].address;
	}

	/* Without an address there is no way in yet. */
	if (address == NULL) {
		(void)snprintf(text, size, "%s", "Not connected to a network");
		return;
	}

	/* ssh user@address. */
	(void)snprintf(text, size, "ssh %s@%s", user, address);
}
