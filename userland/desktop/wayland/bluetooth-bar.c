/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bluetooth in the glass look's system bar (ws143-p006,
 * plan/ws143/phase006/phase.md section 5): the rune beside the network's
 * fan while the machine has a controller (a service that answers and a
 * controller it found), dark while it is on, pale while it is off or not
 * ready, with a dot at each side while a device is connected.
 *
 * A click on it opens the menu under it, the network menu's form
 * (network.c): the switch (when the service can turn the controller off),
 * the state when it is not on, the paired devices (connected ones say so;
 * a click connects or disconnects one when the service can), and
 * "Bluetooth Settings..." which starts Settings on its Bluetooth page.  A
 * click elsewhere, or Esc, closes it; the lock closes it.  While it is
 * open the state is read often (kwl_bluetooth_bar_watch).  A request that
 * failed says so on a line at the menu's end until the next one.
 *
 * All of it comes through bluetooth-shell.c, which has libkeiland-backend's
 * Bluetooth: nothing here speaks to the service.
 * Log: "KWL BT bar open", "KWL BT bar close via=V", "KWL BT bar act
 * row=N kind=K", "KWL BT menu ..." and "KWL BT row ..." (the rows' places,
 * when they change, for the tests that click them), "KWL BT icon ...".
 */

#include "kwl.h"
#include "glass.h"

#include "userland/desktop/paths.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The evdev code of Esc. */
#define BAR_KEY_ESC		1U

/* The menu's width, its padding, its rows' heights, and its corners. */
#define BAR_MENU_WIDTH		300
#define BAR_MENU_PADDING	8
#define BAR_ROW_HEIGHT		30
#define BAR_NOTE_HEIGHT		22
#define BAR_SEPARATOR		9
#define BAR_MENU_RADIUS		12.0f

/* The most paired devices listed, and the menu's most rows (the switch, the state, the devices, a note, the settings, a failure, separators). */
#define BAR_DEVICES_MAX		8U
#define BAR_ROWS_MAX		(BAR_DEVICES_MAX + 10U)

/* How long the switch keeps the position asked, for the state to agree. */
#define BAR_SWITCH_HOLD_MS	4000U

/* Settings on its Bluetooth page. */
#define BAR_SETTINGS_COMMAND	KEILAND_BINDIR "/settings bluetooth"

/* The kinds of row. */
enum bar_row_kind {
	BAR_ROW_SWITCH,
	BAR_ROW_NOTE,
	BAR_ROW_SEPARATOR,
	BAR_ROW_DEVICE,
	BAR_ROW_SETTINGS
};

/* One row of the open menu: its kind, its text, its place (from the menu's top), and for a device its index in the list. */
struct bar_row {
	enum bar_row_kind kind;
	char text[160];
	int32_t y;
	int32_t height;
	unsigned device;
};

/*
 * The view: the state and the paired devices as last read, whether the
 * menu is open, where and on which output's bar, its rows, the icon's
 * places, the switch's position asked and until when it holds, and the
 * last failure.
 */
static struct {
	struct kl_backend_bluetooth_state state;
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	size_t count;
	unsigned open;
	unsigned output;
	int32_t menu_x;
	int32_t menu_y;
	int32_t menu_height;
	struct bar_row rows[BAR_ROWS_MAX];
	unsigned row_count;
	uint64_t logged_layout;
	struct kwl_plane_places icons;
	int32_t icon_width;
	int32_t icon_height;
	unsigned icon_logged;
	unsigned switch_wanted;
	uint64_t switch_until;
	char failure[96];
} bar_view;

static void bar_read(void);
static int bar_shown(void);
static void bar_open(struct kwl_server *server);
static void bar_close(struct kwl_server *server, const char *via);
static void bar_layout(struct kwl_server *server);
static void bar_add_row(enum bar_row_kind kind, const char *text, int32_t height, unsigned device);
static void bar_act(struct kwl_server *server, const struct bar_row *row);
static const struct bar_row *bar_row_at(int32_t x, int32_t y);
static int bar_in_icon(unsigned slot, int32_t x, int32_t y);
static unsigned bar_switch_on(void);
static int bar_acts(const struct bar_row *row);
static const char *bar_state_text(void);
static void bar_log_layout(void);
static void bar_draw_row(struct kwl_server *server, VkCommandBuffer command, const struct bar_row *row, int32_t top, unsigned over);
static void bar_draw_switch(struct kwl_server *server, VkCommandBuffer command, int32_t right, int32_t middle, unsigned on);

/*
 * Tells whether the bar has the Bluetooth icon now (1) or not (0): the
 * service answers and has found a controller.
 */
int
kwl_bluetooth_bar_width(
	void)
{
	/* As last read. */
	bar_read();
	return bar_shown();
}

/*
 * Draws the icon in its 20-pixel square from x, and keeps where a click on
 * it opens the menu on this output's bar.
 */
void
kwl_bluetooth_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t top,
	const float *ink)
{
	float colour[4];
	float accent[4];
	int32_t middle;
	unsigned connected;
	size_t index;
	int shown;

	/* Only with a controller. */
	shown = bar_shown();
	if (!shown)
		return;

	/* Where a click on this output's bar opens the menu (a little larger than the drawing). */
	kwl_plane_place(&bar_view.icons, server->view_output, x - 5, top);
	bar_view.icon_width = 30;
	bar_view.icon_height = KWL_GLASS_BAR - 6;
	if (!bar_view.icon_logged && server->view_output == KWL_PLANE_ANCHOR) {
		bar_view.icon_logged = 1U;
		printf("KWL BT icon x=%d y=%d width=%d height=%d\n", x - 5, top + 3, bar_view.icon_width, bar_view.icon_height);
	}

	/* While the menu is open, a pale back of the accent behind the icon it opened from. */
	middle = top + KWL_GLASS_BAR_MIDDLE;
	if (bar_view.open && bar_view.output == server->view_output) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.28f, accent);
		glass_draw_solid(server, command, (float)(x - 5), (float)(middle - 14), (float)bar_view.icon_width, 28.0f, 7.0f, accent);
	}

	/* The rune: dark while on, pale otherwise. */
	memcpy(colour, ink, sizeof(colour));
	if (bar_view.state.state != KL_BACKEND_BT_ON)
		colour[3] *= 0.35f;
	glass_draw_icon(server, command, GLASS_ICON_BLUETOOTH, x, middle - 10, 20U, colour);

	/* A dot at each side while a device is connected. */
	connected = 0U;
	for (index = 0; index < bar_view.count; index++) {
		if (bar_view.devices[index].connected)
			connected = 1U;
	}

	/* Drawn while on. */
	if (connected && bar_view.state.state == KL_BACKEND_BT_ON) {
		glass_draw_solid(server, command, (float)(x + 2), (float)(middle - 1), 3.0f, 3.0f, 1.5f, ink);
		glass_draw_solid(server, command, (float)(x + 16), (float)(middle - 1), 3.0f, 3.0f, 1.5f, ink);
	}
}

/* Draws the open menu under the icon: its shadow, its glass and its rows, the row under the pointer lit. */
void
kwl_bluetooth_draw_menu(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct glass_shape shape;
	const struct bar_row *over;
	unsigned index;

	/* Only an open menu, on the output whose bar opened it. */
	if (!bar_view.open || bar_view.output != server->view_output)
		return;

	/* The rows for the state last read. */
	bar_layout(server);

	/* The shadow. */
	glass_shape_init(&shape, (float)bar_view.menu_x, (float)bar_view.menu_y + 6.0f, (float)BAR_MENU_WIDTH, (float)bar_view.menu_height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = BAR_MENU_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, as white as the network's menu. */
	glass_shape_init(&shape, (float)bar_view.menu_x, (float)bar_view.menu_y, (float)BAR_MENU_WIDTH, (float)bar_view.menu_height);
	shape.mode = MODE_GLASS;
	shape.radius = BAR_MENU_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* Each row, the one under the pointer lit when it acts. */
	over = bar_row_at(server->pointer_x, server->pointer_y);
	for (index = 0; index < bar_view.row_count; index++)
		bar_draw_row(server, command, &bar_view.rows[index], bar_view.menu_y + bar_view.rows[index].y, over == &bar_view.rows[index]);
}

/*
 * Handles a pointer button: a press on the icon opens or closes the menu;
 * while it is open, a press on a row acts on it and a press elsewhere
 * closes it.  Returns 1 when the button was Bluetooth's.
 */
int
kwl_bluetooth_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct bar_row *row;
	int inside;

	/* With the menu closed, only a left press on the icon. */
	if (!bar_view.open) {
		if (state == 0U || button != KWL_BUTTON_LEFT)
			return 0;
		inside = bar_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
		if (!inside)
			return 0;
		bar_view.output = server->pointer_output;
		bar_open(server);
		return 1;
	}

	/* While it is open, releases are the menu's. */
	if (state == 0U)
		return 1;

	/* A press on the icon, on any output's bar, closes it. */
	inside = bar_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
	if (inside) {
		bar_close(server, "icon");
		return 1;
	}

	/* A press outside the menu closes it and goes no further. */
	if (server->pointer_x < bar_view.menu_x ||
	    server->pointer_x >= bar_view.menu_x + BAR_MENU_WIDTH ||
	    server->pointer_y < bar_view.menu_y ||
	    server->pointer_y >= bar_view.menu_y + bar_view.menu_height) {
		bar_close(server, "outside");
		return 1;
	}

	/* A left press on a row that acts. */
	row = bar_row_at(server->pointer_x, server->pointer_y);
	if (row != NULL && button == KWL_BUTTON_LEFT)
		bar_act(server, row);
	return 1;
}

/* Handles a key while the menu is open: Esc closes it, and the others are the menu's too.  Returns 1 when the key was Bluetooth's. */
int
kwl_bluetooth_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* A closed menu takes no key. */
	if (!bar_view.open)
		return 0;

	/* Esc, pressed, closes it. */
	if (key == BAR_KEY_ESC && state != 0U)
		bar_close(server, "key");
	return 1;
}

/* Follows the pointer while the menu is open (the row under it is lit).  Returns 1 when the motion was the menu's. */
int
kwl_bluetooth_motion(
	struct kwl_server *server)
{
	/* A closed menu does not follow the pointer. */
	if (!bar_view.open)
		return 0;
	server->dirty = 1;
	return 1;
}

/* Tells whether the menu is open. */
int
kwl_bluetooth_is_open(
	void)
{
	/* Open. */
	if (bar_view.open)
		return 1;
	return 0;
}

/*
 * Looks after the bar once a pass: the answer to the bar's request (a
 * failure kept for the menu), the switch let go once the state agrees,
 * and the menu closed by the lock or by the controller going.
 */
void
kwl_bluetooth_bar_tick(
	struct kwl_server *server)
{
	int answered;
	int shown;
	int error;

	/* The answer to the bar's last request. */
	error = 0;
	answered = kwl_bluetooth_bar_answer(&error);
	if (answered) {
		bar_view.failure[0] = '\0';
		if (error != 0) {
			(void)snprintf(bar_view.failure, sizeof(bar_view.failure), "%s", kl_tr("The request failed."));
			bar_view.switch_until = 0U;
		}

		/* Logged, and drawn again. */
		printf("KWL BT bar answer error=%d\n", error);
		server->dirty = 1;
	}

	/* Only an open menu from here. */
	if (!bar_view.open)
		return;

	/* The lock, the login screen, or no controller any more: the menu goes. */
	bar_read();
	shown = bar_shown();
	if (server->locked || server->greeter || !shown)
		bar_close(server, "state");
}

/* Reads the state and the devices as bluetooth-shell.c last had them. */
static void
bar_read(
	void)
{
	size_t count;

	/* The state and the devices that fit. */
	count = kwl_bluetooth_view(&bar_view.state, bar_view.devices, KL_BACKEND_BT_DEVICES_MAX);
	if (count > KL_BACKEND_BT_DEVICES_MAX)
		count = KL_BACKEND_BT_DEVICES_MAX;
	bar_view.count = count;
}

/* Tells whether the icon shows: the service answers and has a controller. */
static int
bar_shown(
	void)
{
	/* No service, or no controller. */
	if (!bar_view.state.reachable)
		return 0;
	if (bar_view.state.state == KL_BACKEND_BT_ABSENT || bar_view.state.state == KL_BACKEND_BT_NONE)
		return 0;
	return 1;
}

/* Opens the menu, watching the state while it is open. */
static void
bar_open(
	struct kwl_server *server)
{
	/* Open, a failure of before forgotten. */
	bar_view.open = 1U;
	bar_view.logged_layout = 0U;
	bar_view.failure[0] = '\0';
	kwl_bluetooth_bar_watch(1U);
	server->dirty = 1;
	printf("KWL BT bar open\n");
}

/* Closes the menu. */
static void
bar_close(
	struct kwl_server *server,
	const char *via)
{
	/* Closed, no longer watching. */
	bar_view.open = 0U;
	kwl_bluetooth_bar_watch(0U);
	server->dirty = 1;
	printf("KWL BT bar close via=%s\n", via);
}

/*
 * Lays out the rows from the state last read, under the icon of the bar
 * it opened from: the switch, the state, the paired devices, the
 * settings and a failure.
 */
static void
bar_layout(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	unsigned listed;
	unsigned hidden;
	size_t index;
	int32_t icon_x;
	int32_t top;
	int placed;
	char number[16];
	char text[160];

	/* The state as last read. */
	bar_read();
	bar_view.row_count = 0U;
	bar_view.menu_height = BAR_MENU_PADDING;

	/* The switch when the service can turn the controller off, and the state when it is not on. */
	if ((bar_view.state.features & KL_BACKEND_BT_CAN_POWER) != 0U)
		bar_add_row(BAR_ROW_SWITCH, "Bluetooth", BAR_ROW_HEIGHT, 0U);
	if (bar_view.state.state != KL_BACKEND_BT_ON)
		bar_add_row(BAR_ROW_NOTE, bar_state_text(), BAR_NOTE_HEIGHT, 0U);
	bar_add_row(BAR_ROW_SEPARATOR, "", BAR_SEPARATOR, 0U);

	/* The paired devices, as many as the menu lists. */
	listed = 0U;
	hidden = 0U;
	for (index = 0; index < bar_view.count; index++) {
		if (!bar_view.devices[index].paired)
			continue;
		if (listed >= BAR_DEVICES_MAX) {
			hidden++;
			continue;
		}

		/* Its row. */
		bar_add_row(BAR_ROW_DEVICE, bar_view.devices[index].name, BAR_ROW_HEIGHT, (unsigned)index);
		listed++;
	}

	/* None, or more than the menu lists. */
	if (listed == 0U)
		bar_add_row(BAR_ROW_NOTE, kl_tr("No paired devices"), BAR_NOTE_HEIGHT, 0U);
	if (hidden > 0U) {
		(void)snprintf(number, sizeof(number), "%u", hidden);
		(void)kl_tr_format(text, sizeof(text), kl_tr("{1} more in Settings"), number, (const char *)NULL);
		bar_add_row(BAR_ROW_NOTE, text, BAR_NOTE_HEIGHT, 0U);
	}

	/* The settings, and a failure. */
	bar_add_row(BAR_ROW_SEPARATOR, "", BAR_SEPARATOR, 0U);
	bar_add_row(BAR_ROW_SETTINGS, kl_tr("Bluetooth Settings..."), BAR_ROW_HEIGHT, 0U);
	if (bar_view.failure[0] != '\0')
		bar_add_row(BAR_ROW_NOTE, bar_view.failure, BAR_NOTE_HEIGHT, 0U);
	bar_view.menu_height += BAR_MENU_PADDING;

	/* Under the icon, inside its output. */
	(void)kwl_output_rect(server, bar_view.output, &output);
	placed = kwl_plane_placed(&bar_view.icons, bar_view.output, &icon_x, &top);
	if (!placed) {
		icon_x = output.x;
		top = output.y;
	}

	/* Its right edge a little in from the icon's, inside the output. */
	bar_view.menu_x = icon_x + bar_view.icon_width - BAR_MENU_WIDTH + 60;
	if (bar_view.menu_x + BAR_MENU_WIDTH > output.x + (int32_t)output.width - 8)
		bar_view.menu_x = output.x + (int32_t)output.width - 8 - BAR_MENU_WIDTH;
	if (bar_view.menu_x < output.x + 8)
		bar_view.menu_x = output.x + 8;
	bar_view.menu_y = top + KWL_GLASS_BAR + 6;

	/* A new layout is logged for the tests that click the rows. */
	bar_log_layout();
}

/* Adds a row under the last. */
static void
bar_add_row(
	enum bar_row_kind kind,
	const char *text,
	int32_t height,
	unsigned device)
{
	struct bar_row *row;

	/* No room. */
	if (bar_view.row_count >= BAR_ROWS_MAX)
		return;

	/* The row. */
	row = &bar_view.rows[bar_view.row_count++];
	row->kind = kind;
	(void)snprintf(row->text, sizeof(row->text), "%s", text);
	row->y = bar_view.menu_height;
	row->height = height;
	row->device = device;
	bar_view.menu_height += height;
}

/*
 * Acts on a row: the switch turns the controller on or off, a device is
 * connected or disconnected, the settings start Settings on its page.
 */
static void
bar_act(
	struct kwl_server *server,
	const struct bar_row *row)
{
	const struct kl_backend_bluetooth_device *device;
	unsigned request;
	unsigned on;
	pid_t child;
	int error;
	int acts;

	/* Only a row that acts. */
	acts = bar_acts(row);
	if (!acts)
		return;
	printf("KWL BT bar act row=%u kind=%d\n", (unsigned)(row - bar_view.rows), (int)row->kind);

	/* Settings, on its Bluetooth page; the menu goes. */
	if (row->kind == BAR_ROW_SETTINGS) {
		child = kwl_spawn(server, BAR_SETTINGS_COMMAND);
		printf("KWL BT bar settings pid=%ld\n", (long)child);
		bar_close(server, "settings");
		return;
	}

	/* The switch: the other position, held until the state agrees. */
	error = 0;
	if (row->kind == BAR_ROW_SWITCH) {
		on = !bar_switch_on();
		request = KL_BACKEND_BT_POWER_OFF;
		if (on)
			request = KL_BACKEND_BT_POWER_ON;
		error = kwl_bluetooth_bar_request(request, NULL, 0U);
		if (error == 0) {
			bar_view.switch_wanted = on;
			bar_view.switch_until = kwl_milliseconds() + BAR_SWITCH_HOLD_MS;
		}
	}

	/* A device: connected or disconnected. */
	if (row->kind == BAR_ROW_DEVICE) {
		device = &bar_view.devices[row->device];
		request = KL_BACKEND_BT_CONNECT;
		if (device->connected)
			request = KL_BACKEND_BT_DISCONNECT;
		error = kwl_bluetooth_bar_request(request, device->address, device->type);
	}

	/* A request that could not be sent says so. */
	bar_view.failure[0] = '\0';
	if (error == EBUSY)
		(void)snprintf(bar_view.failure, sizeof(bar_view.failure), "%s", kl_tr("Bluetooth is busy. Try again."));
	else if (error != 0)
		(void)snprintf(bar_view.failure, sizeof(bar_view.failure), "%s", kl_tr("The request failed."));
	server->dirty = 1;
}

/* Gives the row a point is on, when it acts. */
static const struct bar_row *
bar_row_at(
	int32_t x,
	int32_t y)
{
	const struct bar_row *row;
	unsigned index;
	int32_t top;
	int acts;

	/* Outside the menu's width, nothing. */
	if (!bar_view.open)
		return NULL;
	if (x < bar_view.menu_x || x >= bar_view.menu_x + BAR_MENU_WIDTH)
		return NULL;

	/* The row the point is in. */
	for (index = 0; index < bar_view.row_count; index++) {
		row = &bar_view.rows[index];
		top = bar_view.menu_y + row->y;
		if (y < top || y >= top + row->height)
			continue;
		acts = bar_acts(row);
		if (acts)
			return row;
		return NULL;
	}

	/* The padding. */
	return NULL;
}

/* Tells whether a row acts: the switch, the settings, and a device when the service connects them. */
static int
bar_acts(
	const struct bar_row *row)
{
	/* Each kind. */
	if (row->kind == BAR_ROW_SWITCH || row->kind == BAR_ROW_SETTINGS)
		return 1;
	if (row->kind == BAR_ROW_DEVICE && (bar_view.state.features & KL_BACKEND_BT_CAN_CONNECT) != 0U && bar_view.state.state == KL_BACKEND_BT_ON)
		return 1;
	return 0;
}

/* Tells whether a point is on the icon as last drawn on an output's bar. */
static int
bar_in_icon(
	unsigned slot,
	int32_t x,
	int32_t y)
{
	int32_t icon_x;
	int32_t top;
	int placed;
	int shown;

	/* Not shown, or never drawn on that output's bar. */
	shown = bar_shown();
	if (!shown)
		return 0;
	placed = kwl_plane_placed(&bar_view.icons, slot, &icon_x, &top);
	if (!placed)
		return 0;

	/* Its rectangle, a little in from the bar's top and bottom. */
	if (x < icon_x || x >= icon_x + bar_view.icon_width)
		return 0;
	if (y < top + 3 || y >= top + 3 + bar_view.icon_height)
		return 0;
	return 1;
}

/* Tells the switch's position: the one asked while it holds, else the user's switch as the service has it. */
static unsigned
bar_switch_on(
	void)
{
	uint64_t now;

	/* Asked a moment ago, and the state does not agree yet. */
	now = kwl_milliseconds();
	if (now < bar_view.switch_until && bar_view.state.power != bar_view.switch_wanted)
		return bar_view.switch_wanted;
	if (bar_view.state.power)
		return 1U;
	return 0U;
}

/* Gives the words for a state that is not on. */
static const char *
bar_state_text(
	void)
{
	/* Each state. */
	switch (bar_view.state.state) {
	case KL_BACKEND_BT_OFF:
		return kl_tr("Bluetooth is off");
	case KL_BACKEND_BT_STARTING:
		return kl_tr("Starting...");
	case KL_BACKEND_BT_FIRMWARE:
		return kl_tr("The adapter's firmware is missing");
	case KL_BACKEND_BT_UNSUPPORTED:
		return kl_tr("This adapter is not supported");
	default:
		return kl_tr("Bluetooth is not available");
	}
}

/* Logs the rows' places when they change, for the tests that click them. */
static void
bar_log_layout(
	void)
{
	const struct bar_row *row;
	uint64_t checksum;
	unsigned index;
	size_t at;

	/* A checksum of the rows' kinds, places and texts, and of the menu's place. */
	checksum = 1469598103934665603ULL;
	for (index = 0; index < bar_view.row_count; index++) {
		row = &bar_view.rows[index];
		checksum = (checksum ^ (uint64_t)row->kind) * 1099511628211ULL;
		checksum = (checksum ^ (uint64_t)(uint32_t)row->y) * 1099511628211ULL;
		for (at = 0; row->text[at] != '\0'; at++)
			checksum = (checksum ^ (unsigned char)row->text[at]) * 1099511628211ULL;
	}

	/* And the menu's place. */
	checksum = (checksum ^ (uint64_t)(uint32_t)bar_view.menu_x) * 1099511628211ULL;

	/* Only a layout not logged yet. */
	if (checksum == bar_view.logged_layout)
		return;
	bar_view.logged_layout = checksum;

	/* Each row, with its rectangle. */
	printf("KWL BT menu x=%d y=%d width=%d height=%d rows=%u\n", bar_view.menu_x, bar_view.menu_y, BAR_MENU_WIDTH, bar_view.menu_height, bar_view.row_count);
	for (index = 0; index < bar_view.row_count; index++) {
		row = &bar_view.rows[index];
		printf("KWL BT row index=%u kind=%d x=%d y=%d width=%d height=%d text=%s\n", index, (int)row->kind,
		    bar_view.menu_x, bar_view.menu_y + row->y, BAR_MENU_WIDTH, row->height, row->text);
	}
}

/* Draws one row: a separator, a note, the switch, a device (connected ones say so) or the settings, lit when the pointer is on it. */
static void
bar_draw_row(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct bar_row *row,
	int32_t top,
	unsigned over)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	static const float line[4] = { 0.12f, 0.16f, 0.24f, 0.16f };
	const struct kl_backend_bluetooth_device *device;
	const char *status;
	float accent[4];
	float ink[4];
	int32_t left;
	int32_t right;
	int32_t middle;
	int32_t baseline;
	int32_t width;
	unsigned kept;

	/* The row's edges, its middle and the text's baseline. */
	left = bar_view.menu_x;
	right = bar_view.menu_x + BAR_MENU_WIDTH - 14;
	middle = top + row->height / 2;
	baseline = middle + 5;

	/* A separator is a thin line. */
	if (row->kind == BAR_ROW_SEPARATOR) {
		glass_draw_solid(server, command, (float)(left + 12), (float)(top + row->height / 2), (float)(BAR_MENU_WIDTH - 24), 1.0f, 0.0f, line);
		return;
	}

	/* A note is soft text. */
	if (row->kind == BAR_ROW_NOTE) {
		glass_draw_text(server, command, SIZE_BAR, left + 14, baseline, row->text, BAR_MENU_WIDTH - 28, soft);
		return;
	}

	/* The lit row is a band of the accent with its ink, as they are. */
	memcpy(ink, dark, sizeof(ink));
	kept = server->keep_colours;
	if (over) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, accent);
		kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, ink);
		kept = kwl_accent_as_is(server);
		glass_draw_solid(server, command, (float)(left + 5), (float)(top + 1), (float)(BAR_MENU_WIDTH - 10), (float)(row->height - 2), 6.0f, accent);
	}

	/* The switch's row: its label, then the switch at the right. */
	if (row->kind == BAR_ROW_SWITCH) {
		glass_draw_text(server, command, SIZE_TITLE, left + 14, baseline + 1, row->text, 160, ink);
		kwl_accent_done(server, kept);
		bar_draw_switch(server, command, right, middle, bar_switch_on());
		return;
	}

	/* A device: its name, and at the right whether it is connected (the action it would take when lit). */
	if (row->kind == BAR_ROW_DEVICE) {
		device = &bar_view.devices[row->device];
		status = "";
		if (device->connected)
			status = kl_tr("Connected");
		if (over && device->connected)
			status = kl_tr("Disconnect");
		if (over && !device->connected)
			status = kl_tr("Connect");
		width = glass_text_width(server, SIZE_BAR, status);
		glass_draw_text(server, command, SIZE_BAR, left + 14, baseline, row->text, BAR_MENU_WIDTH - 40 - width, ink);
		if (over)
			glass_draw_text(server, command, SIZE_BAR, right - width, baseline, status, width + 2, ink);
		kwl_accent_done(server, kept);
		if (!over)
			glass_draw_text(server, command, SIZE_BAR, right - width, baseline, status, width + 2, soft);
		return;
	}

	/* The settings: plain text in the row's ink. */
	glass_draw_text(server, command, SIZE_BAR, left + 14, baseline, row->text, BAR_MENU_WIDTH - 28, ink);
	kwl_accent_done(server, kept);
}

/* Draws the switch ending at right: a pill, of the accent when on, with its knob. */
static void
bar_draw_switch(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t right,
	int32_t middle,
	unsigned on)
{
	static const float grey[4] = { 0.62f, 0.66f, 0.72f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float accent[4];
	unsigned kept;
	int32_t x;

	/* Off: a grey pill with its knob at the left. */
	x = right - 36;
	if (!on) {
		glass_draw_solid(server, command, (float)x, (float)(middle - 10), 36.0f, 20.0f, 10.0f, grey);
		glass_draw_solid(server, command, (float)(x + 2), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
		return;
	}

	/* On: a pill of the accent with its knob at the right, as they are. */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, accent);
	kept = kwl_accent_as_is(server);
	glass_draw_solid(server, command, (float)x, (float)(middle - 10), 36.0f, 20.0f, 10.0f, accent);
	glass_draw_solid(server, command, (float)(x + 18), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
	kwl_accent_done(server, kept);
}
