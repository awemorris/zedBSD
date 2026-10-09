/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The control panel of the status pill (WS192, the 2026-10-09 user
 * request): a press or a tap anywhere on the pill at the right of a bar
 * opens a glass panel under it, at the top right of that output, whose rows
 * are large enough for a finger -- the Wi-Fi switch, Bluetooth's switch,
 * the sound's mute button and slider, the input method's language and the
 * battery.  On a tablet the pill's small icons were hard to hit one by one
 * (the user's report), so every press on the pill opens this panel (the
 * user's decision); the network's and Bluetooth's own menus open from the
 * panel's rows (their "›").
 *
 * A press on the pill again, a press outside the panel (which goes no
 * further) or Esc closes it.  The mouse and a finger reach it the same way:
 * a finger on the bar is followed as the pointer (touch.c, ROUTE_SHELL).
 *
 * The panel reads and acts through the bar's own widgets (network.c,
 * bluetooth-bar.c, volume.c, input-method.c); it keeps no state of theirs.
 * The bars tell it where the pill is as they draw it (draw_status in
 * shell.c), one place for each output.
 */

#include "glass.h"
#include "ime.h"
#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The evdev code of Esc. */
#define STATUS_KEY_ESC		1U

/* The panel's width, the margins around it, its padding, the gap between its rows and its corners. */
#define STATUS_WIDTH		360
#define STATUS_MARGIN		8
#define STATUS_DROP		6
#define STATUS_PADDING		16
#define STATUS_GAP		8
#define STATUS_RADIUS		20.0f

/* A row's height, the sound's (a title line over its controls), the battery's, and a row's corners. */
#define STATUS_ROW_HEIGHT	56
#define STATUS_SOUND_HEIGHT	96
#define STATUS_BATTERY_HEIGHT	44
#define STATUS_ROW_RADIUS	12.0f

/* The switch: its size, its knob, and the room for the "›" right of it. */
#define STATUS_SWITCH_WIDTH	52
#define STATUS_SWITCH_HEIGHT	32
#define STATUS_KNOB		26
#define STATUS_MORE_WIDTH	36

/* The sound's mute button, the slider's track and knob, and the title line over them. */
#define STATUS_MUTE_WIDTH	88
#define STATUS_MUTE_HEIGHT	44
#define STATUS_TRACK_HEIGHT	8
#define STATUS_SLIDER_KNOB	28
#define STATUS_TITLE_LINE	36

/* The most rows: Wi-Fi, Bluetooth, the sound, the input method and the battery. */
#define STATUS_ROWS_MAX		5U

/* What a row is. */
enum status_row_kind {
	STATUS_ROW_WIFI,
	STATUS_ROW_BLUETOOTH,
	STATUS_ROW_SOUND,
	STATUS_ROW_INPUT,
	STATUS_ROW_BATTERY
};

/*
 * One row of the open panel: its kind and its place in the plane.  The rows
 * are laid out when the panel opens and again when what shows changes
 * (Bluetooth's controller coming or going), never while a press is held.
 */
struct status_row {
	enum status_row_kind kind;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

/*
 * The panel's view: whether it is open and on which output's bar, its
 * place and its rows, whether the sound's slider is being dragged, and the
 * pill's place on each output's bar (its left, its bar's top, its width,
 * and the right edge the panel lines up with, the clock pill's), filled as
 * the bars draw it.  Only the event loop's thread touches it.
 */
struct status_view {
	unsigned open;
	unsigned output;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	struct status_row rows[STATUS_ROWS_MAX];
	unsigned row_count;
	unsigned dragging;
	unsigned layout_key;
	struct kwl_plane_places islands;
	int32_t island_width[KWL_PLANE_SLOTS];
	int32_t island_right[KWL_PLANE_SLOTS];
};

/*
 * The one view of the panel.  It starts closed, placed on no output (all
 * zero), and lives as long as the compositor.
 */
static struct status_view status_view;

static void status_open(struct kwl_server *server, unsigned slot);
static void status_close(struct kwl_server *server, const char *via);
static void status_layout(struct kwl_server *server);
static unsigned status_shows(struct kwl_server *server);
static void status_add_row(enum status_row_kind kind, int32_t *top, int32_t height);
static int status_in_island(unsigned slot, int32_t x, int32_t y);
static int status_in_panel(int32_t x, int32_t y);
static const struct status_row *status_row_at(int32_t x, int32_t y);
static void status_act(struct kwl_server *server, const struct status_row *row, int32_t x, int32_t y);
static void status_switch_place(const struct status_row *row, int32_t *x, int32_t *y);
static void status_mute_place(const struct status_row *row, int32_t *x, int32_t *y);
static void status_slider_place(const struct status_row *row, int32_t *left, int32_t *width, int32_t *middle);
static unsigned status_slider_value(const struct status_row *row, int32_t x);
static const struct status_row *status_sound_row(void);
static void status_log_layout(void);
static void status_draw_row(struct kwl_server *server, VkCommandBuffer command, const struct status_row *row);
static void status_draw_switch_row(struct kwl_server *server, VkCommandBuffer command, const struct status_row *row, const char *name, const char *state, unsigned on, unsigned usable);
static void status_draw_sound(struct kwl_server *server, VkCommandBuffer command, const struct status_row *row);
static void status_draw_switch(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t y, unsigned on, float fade);

/*
 * Records where a bar's status pill is, as the bar draws it: its left, its
 * bar's top and its width, and the right edge the panel lines up with.
 */
void
kwl_status_panel_place(
	struct kwl_server *server,
	unsigned slot,
	int32_t x,
	int32_t top,
	int32_t width,
	int32_t right)
{
	/* Outside the outputs' slots there is no bar to remember. */
	if (slot >= KWL_PLANE_SLOTS)
		return;

	/* The pill's left and its bar's top, then its width and the edge to line up with. */
	kwl_plane_place(&status_view.islands, slot, x, top);
	status_view.island_width[slot] = width;
	status_view.island_right[slot] = right;

	/* An open panel follows what shows in it (a controller that came or went). */
	if (status_view.open && slot == status_view.output && !status_view.dragging)
		status_layout(server);
}

/*
 * Draws the open panel on the output whose bar opened it: its shadow, its
 * glass and its rows.
 */
void
kwl_status_panel_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct glass_shape shape;
	unsigned index;

	/* Only an open panel, on its output. */
	if (!status_view.open)
		return;
	if (status_view.output != server->view_output)
		return;

	/* The shadow, as the bar's other popups cast it. */
	glass_shape_init(&shape, (float)status_view.x, (float)status_view.y + 6.0f, (float)status_view.width, (float)status_view.height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = STATUS_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, as white as the volume's popup and the network's menu. */
	glass_shape_init(&shape, (float)status_view.x, (float)status_view.y, (float)status_view.width, (float)status_view.height);
	shape.mode = MODE_GLASS;
	shape.radius = STATUS_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* Each row. */
	for (index = 0U; index < status_view.row_count; index++)
		status_draw_row(server, command, &status_view.rows[index]);
}

/*
 * Handles a pointer button: with the panel closed a left press on a status
 * pill opens it; while it is open a press on the pill closes it, a press
 * outside closes it and goes no further, and a press on a row acts on it.
 * A press with Alt held is left to the pill's own icons (the network's
 * details, ws099-p032).  Returns 1 when the button was the panel's.
 */
int
kwl_status_panel_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct status_row *row;
	int inside;
	int other;
	int alt;

	/* With the panel closed, only a left press on a pill. */
	if (!status_view.open) {
		/* A release, or another button, goes on. */
		if (state == 0U || button != KWL_BUTTON_LEFT)
			return 0;

		/* A press off the pill of the bar the pointer is on goes on. */
		inside = status_in_island(server->pointer_output, server->pointer_x, server->pointer_y);
		if (!inside)
			return 0;

		/* Alt+click is the icons' own (the network's details). */
		alt = kwl_input_alt_held(server);
		if (alt)
			return 0;

		/* A menu the panel opened is still up: the press is the menu's, which closes it. */
		other = kwl_network_is_open();
		if (other)
			return 0;
		other = kwl_bluetooth_is_open();
		if (other)
			return 0;
		other = kwl_volume_is_open();
		if (other)
			return 0;

		/* The panel opens under that pill. */
		status_open(server, server->pointer_output);
		return 1;
	}

	/* A release ends a drag of the slider, with its sound; every release is the panel's. */
	if (state == 0U) {
		row = status_sound_row();
		if (status_view.dragging && row != NULL)
			kwl_volume_panel_slide(server, status_slider_value(row, server->pointer_x), 1U);
		status_view.dragging = 0U;
		return 1;
	}

	/* A press on a pill, on any output's bar, closes it. */
	inside = status_in_island(server->pointer_output, server->pointer_x, server->pointer_y);
	if (inside) {
		status_close(server, "island");
		return 1;
	}

	/* A press outside the panel closes it and goes no further. */
	inside = status_in_panel(server->pointer_x, server->pointer_y);
	if (!inside) {
		status_close(server, "outside");
		return 1;
	}

	/* A left press on a row acts on it. */
	row = status_row_at(server->pointer_x, server->pointer_y);
	if (row != NULL && button == KWL_BUTTON_LEFT)
		status_act(server, row, server->pointer_x, server->pointer_y);

	/* Succeeded: the press was the panel's. */
	return 1;
}

/*
 * Follows the pointer while the panel is open: a drag of the sound's slider
 * sets the volume.  Returns 1 when the motion was the panel's.
 */
int
kwl_status_panel_motion(
	struct kwl_server *server)
{
	const struct status_row *row;

	/* A closed panel does not follow the pointer. */
	if (!status_view.open)
		return 0;

	/* A drag sets the volume under the pointer, without its sound until it ends. */
	row = status_sound_row();
	if (status_view.dragging && row != NULL)
		kwl_volume_panel_slide(server, status_slider_value(row, server->pointer_x), 0U);

	/* Succeeded: the motion was the panel's. */
	return 1;
}

/*
 * Handles a key while the panel is open: Esc closes it, and the others are
 * the panel's too.  Returns 1 when the key was the panel's.
 */
int
kwl_status_panel_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* A closed panel takes no key. */
	if (!status_view.open)
		return 0;

	/* Esc, pressed, closes it. */
	if (key == STATUS_KEY_ESC && state != 0U)
		status_close(server, "key");

	/* Succeeded: the key was the panel's. */
	return 1;
}

/*
 * Tells whether the panel is open (the look is not still while it is, and
 * a press anywhere is its).
 */
int
kwl_status_panel_is_open(
	void)
{
	/* Open or not. */
	return (int)status_view.open;
}

/* Opens the panel under the pill of an output's bar, kept on that output. */
static void
status_open(
	struct kwl_server *server,
	unsigned slot)
{
	unsigned value;
	unsigned muted;
	int sound;

	/* Open on that output, nothing held. */
	status_view.open = 1U;
	status_view.output = slot;
	status_view.dragging = 0U;
	status_view.layout_key = 0U;
	status_layout(server);
	server->dirty = 1;

	/* Where it opened and whether its sound's controls act (none without a sound device), for the tests. */
	kwl_volume_panel_state(&value, &muted, &sound);
	printf("KWL STATUS panel open output=%u x=%d y=%d width=%d height=%d sound=%d\n", slot, status_view.x, status_view.y, status_view.width, status_view.height, sound);
	status_log_layout();
}

/* Closes the panel, a drag of the slider ending with its volume. */
static void
status_close(
	struct kwl_server *server,
	const char *via)
{
	const struct status_row *row;

	/* A drag in progress ends with its sound. */
	row = status_sound_row();
	if (status_view.dragging && row != NULL)
		kwl_volume_panel_slide(server, status_slider_value(row, server->pointer_x), 1U);

	/* Closed. */
	status_view.dragging = 0U;
	status_view.open = 0U;
	server->dirty = 1;
	printf("KWL STATUS panel close via=%s\n", via);
}

/*
 * Lays the panel out on its output: its right edge at the clock pill's,
 * under the bar, as wide as STATUS_WIDTH or the output allows, and the rows
 * that show from the top.  A change of what shows is logged again.
 */
static void
status_layout(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	unsigned shows;
	int32_t island_x;
	int32_t top;
	int32_t right;
	int placed;

	/* The output, and the pill on its bar. */
	(void)kwl_output_rect(server, status_view.output, &output);
	placed = kwl_plane_placed(&status_view.islands, status_view.output, &island_x, &top);
	if (!placed) {
		island_x = output.x + (int32_t)output.width - STATUS_WIDTH - STATUS_MARGIN;
		top = output.y;
	}

	/* The width: as designed, or what the output leaves. */
	status_view.width = STATUS_WIDTH;
	if (status_view.width > (int32_t)output.width - 2 * STATUS_MARGIN)
		status_view.width = (int32_t)output.width - 2 * STATUS_MARGIN;

	/* The right edge lines up with the clock pill's, within the output. */
	right = output.x + (int32_t)output.width - STATUS_MARGIN;
	if (placed && status_view.island_right[status_view.output] > 0 && status_view.island_right[status_view.output] < right)
		right = status_view.island_right[status_view.output];
	status_view.x = right - status_view.width;
	if (status_view.x < output.x + STATUS_MARGIN)
		status_view.x = output.x + STATUS_MARGIN;
	status_view.y = top + KWL_GLASS_BAR + STATUS_DROP;

	/* The rows that show, from the top. */
	shows = status_shows(server);
	status_view.row_count = 0U;
	top = status_view.y + STATUS_PADDING;
	status_add_row(STATUS_ROW_WIFI, &top, STATUS_ROW_HEIGHT);
	if ((shows & 1U) != 0U)
		status_add_row(STATUS_ROW_BLUETOOTH, &top, STATUS_ROW_HEIGHT);
	status_add_row(STATUS_ROW_SOUND, &top, STATUS_SOUND_HEIGHT);
	if ((shows & 2U) != 0U)
		status_add_row(STATUS_ROW_INPUT, &top, STATUS_ROW_HEIGHT);
	if ((shows & 4U) != 0U)
		status_add_row(STATUS_ROW_BATTERY, &top, STATUS_BATTERY_HEIGHT);
	status_view.height = top - STATUS_GAP + STATUS_PADDING - status_view.y;

	/* What shows changed: the rows are logged again for the tests. */
	if (status_view.layout_key != (shows | 8U)) {
		if (status_view.layout_key != 0U)
			status_log_layout();
		status_view.layout_key = shows | 8U;
	}
}

/*
 * Tells which optional rows show, as bits: Bluetooth (1) while the bar has
 * its icon, the input method (2) while the bar has its language, and the
 * battery (4) on a machine that has one.
 */
static unsigned
status_shows(
	struct kwl_server *server)
{
	unsigned shows;
	int width;

	/* Bluetooth, while there is a controller. */
	shows = 0U;
	width = kwl_bluetooth_bar_width();
	if (width > 0)
		shows |= 1U;

	/* The input method, while it has told its language. */
	width = (int)kwl_ime_indicator_width(server);
	if (width > 0)
		shows |= 2U;

	/* The battery, on a machine with one. */
	if (server->power.percent >= 0)
		shows |= 4U;

	/* Succeeded: the rows that show. */
	return shows;
}

/* Adds a row of a height at a top, the next top after it and the gap. */
static void
status_add_row(
	enum status_row_kind kind,
	int32_t *top,
	int32_t height)
{
	struct status_row *row;

	/* No room. */
	if (status_view.row_count >= STATUS_ROWS_MAX)
		return;

	/* The row across the panel inside its padding. */
	row = &status_view.rows[status_view.row_count++];
	row->kind = kind;
	row->x = status_view.x + STATUS_PADDING;
	row->y = *top;
	row->width = status_view.width - 2 * STATUS_PADDING;
	row->height = height;

	/* The next row's top. */
	*top += height + STATUS_GAP;
}

/* Tells whether a point is on the status pill of an output's bar (the whole height of the bar, for a finger). */
static int
status_in_island(
	unsigned slot,
	int32_t x,
	int32_t y)
{
	int32_t left;
	int32_t top;
	int placed;

	/* The pill as last drawn on that output's bar. */
	if (slot >= KWL_PLANE_SLOTS)
		return 0;
	placed = kwl_plane_placed(&status_view.islands, slot, &left, &top);
	if (!placed)
		return 0;

	/* Within its width and the bar's height. */
	if (x < left || x >= left + status_view.island_width[slot])
		return 0;
	if (y < top || y >= top + KWL_GLASS_BAR)
		return 0;

	/* Succeeded: on the pill. */
	return 1;
}

/* Tells whether a point is on the open panel. */
static int
status_in_panel(
	int32_t x,
	int32_t y)
{
	/* Left and right of it. */
	if (x < status_view.x || x >= status_view.x + status_view.width)
		return 0;

	/* Above and below it. */
	if (y < status_view.y || y >= status_view.y + status_view.height)
		return 0;

	/* Succeeded: on the panel. */
	return 1;
}

/* Finds the row under a point, or NULL in the gaps and the padding. */
static const struct status_row *
status_row_at(
	int32_t x,
	int32_t y)
{
	const struct status_row *row;
	unsigned index;

	/* Each row. */
	for (index = 0U; index < status_view.row_count; index++) {
		row = &status_view.rows[index];
		if (x < row->x || x >= row->x + row->width)
			continue;
		if (y < row->y || y >= row->y + row->height)
			continue;
		return row;
	}

	/* No row there. */
	return NULL;
}

/*
 * Acts on a press on a row: a switch turns its radio on or off, the rest of
 * Wi-Fi's or Bluetooth's row opens its menu (the panel closes), the mute
 * button switches mute, the slider sets the volume and starts a drag, the
 * input method's row asks for the next language.  The battery's row only
 * shows.
 */
static void
status_act(
	struct kwl_server *server,
	const struct status_row *row,
	int32_t x,
	int32_t y)
{
	unsigned value;
	unsigned muted;
	int32_t switch_x;
	int32_t switch_y;
	int32_t mute_x;
	int32_t mute_y;
	int sound;

	/* Each kind. */
	switch (row->kind) {
	case STATUS_ROW_WIFI:
	case STATUS_ROW_BLUETOOTH:
		/* The switch, a little larger than drawn for a finger. */
		status_switch_place(row, &switch_x, &switch_y);
		if (x >= switch_x - 8 &&
		    x < switch_x + STATUS_SWITCH_WIDTH + 8 &&
		    y >= row->y &&
		    y < row->y + row->height) {
			if (row->kind == STATUS_ROW_WIFI) {
				printf("KWL STATUS act name=wifi\n");
				kwl_network_panel_switch(server);
			} else {
				printf("KWL STATUS act name=bluetooth\n");
				kwl_bluetooth_panel_switch(server);
			}

			/* The panel shows the switch's new position at once. */
			server->dirty = 1;
			break;
		}

		/* The rest of the row opens the radio's own menu on this output; the panel goes. */
		status_close(server, "item");
		if (row->kind == STATUS_ROW_WIFI) {
			printf("KWL STATUS act name=wifi-more\n");
			kwl_network_panel_open(server, status_view.output);
		} else {
			printf("KWL STATUS act name=bluetooth-more\n");
			kwl_bluetooth_panel_open(server, status_view.output);
		}

		break;
	case STATUS_ROW_SOUND:
		/* Nothing without sound. */
		kwl_volume_panel_state(&value, &muted, &sound);
		if (!sound)
			break;

		/* The mute button switches mute. */
		status_mute_place(row, &mute_x, &mute_y);
		if (x >= mute_x &&
		    x < mute_x + STATUS_MUTE_WIDTH &&
		    y >= mute_y &&
		    y < mute_y + STATUS_MUTE_HEIGHT) {
			printf("KWL STATUS act name=mute\n");
			kwl_volume_panel_mute(server);
			break;
		}

		/* The controls' line right of it is the slider: the volume under the finger, then a drag. */
		if (x >= mute_x + STATUS_MUTE_WIDTH && y >= mute_y - 6) {
			status_view.dragging = 1U;
			printf("KWL STATUS act name=volume\n");
			kwl_volume_panel_slide(server, status_slider_value(row, x), 0U);
		}

		break;
	case STATUS_ROW_INPUT:
		/* The next language, as the bar's chip asks. */
		printf("KWL STATUS act name=input\n");
		kwl_ime_panel_next(server);
		break;
	default:
		break;
	}
}

/* Gives the switch's top left in a row: at the right, left of the "›", in the middle of the row. */
static void
status_switch_place(
	const struct status_row *row,
	int32_t *x,
	int32_t *y)
{
	/* Right, before the room of the "›", centred up and down. */
	*x = row->x + row->width - STATUS_MORE_WIDTH - STATUS_SWITCH_WIDTH;
	*y = row->y + (row->height - STATUS_SWITCH_HEIGHT) / 2;
}

/* Gives the mute button's top left in the sound's row: at the left of the controls' line under the title. */
static void
status_mute_place(
	const struct status_row *row,
	int32_t *x,
	int32_t *y)
{
	/* Inside the row's own padding, under the title line. */
	*x = row->x + 12;
	*y = row->y + STATUS_TITLE_LINE + (row->height - STATUS_TITLE_LINE - STATUS_MUTE_HEIGHT) / 2;
}

/* Gives the slider's track: its left, its width and its middle, right of the mute button. */
static void
status_slider_place(
	const struct status_row *row,
	int32_t *left,
	int32_t *width,
	int32_t *middle)
{
	int32_t mute_x;
	int32_t mute_y;

	/* Right of the mute button to the row's right padding, in the middle of the button's height. */
	status_mute_place(row, &mute_x, &mute_y);
	*left = mute_x + STATUS_MUTE_WIDTH + 16;
	*width = row->x + row->width - 16 - *left;
	*middle = mute_y + STATUS_MUTE_HEIGHT / 2;
}

/* Gives the volume (0 to 100) the slider means at a point across it. */
static unsigned
status_slider_value(
	const struct status_row *row,
	int32_t x)
{
	int32_t left;
	int32_t width;
	int32_t middle;
	int32_t value;

	/* No row, no change from nothing. */
	if (row == NULL)
		return 0U;

	/* The knob's middle travels the track less its own width. */
	status_slider_place(row, &left, &width, &middle);
	if (width <= STATUS_SLIDER_KNOB)
		return 0U;
	value = (x - left - STATUS_SLIDER_KNOB / 2) * 100 / (width - STATUS_SLIDER_KNOB);
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;

	/* Succeeded: the volume there. */
	return (unsigned)value;
}

/* Finds the sound's row of the open panel. */
static const struct status_row *
status_sound_row(
	void)
{
	unsigned index;

	/* Each row. */
	for (index = 0U; index < status_view.row_count; index++) {
		if (status_view.rows[index].kind == STATUS_ROW_SOUND)
			return &status_view.rows[index];
	}

	/* None (never, while open). */
	return NULL;
}

/* Logs the rows' and the controls' places, for the tests that press them. */
static void
status_log_layout(
	void)
{
	static const char *const names[] = { "wifi", "bluetooth", "sound", "input", "battery" };
	const struct status_row *row;
	unsigned index;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t middle;

	/* Each row, and its switch, mute button or slider. */
	for (index = 0U; index < status_view.row_count; index++) {
		row = &status_view.rows[index];
		printf("KWL STATUS item name=%s x=%d y=%d width=%d height=%d\n", names[row->kind], row->x, row->y, row->width, row->height);

		/* A radio's switch. */
		if (row->kind == STATUS_ROW_WIFI || row->kind == STATUS_ROW_BLUETOOTH) {
			status_switch_place(row, &x, &y);
			printf("KWL STATUS item name=%s-switch x=%d y=%d width=%d height=%d\n", names[row->kind], x, y, STATUS_SWITCH_WIDTH, STATUS_SWITCH_HEIGHT);
		}

		/* The sound's mute button and slider. */
		if (row->kind == STATUS_ROW_SOUND) {
			status_mute_place(row, &x, &y);
			printf("KWL STATUS item name=mute x=%d y=%d width=%d height=%d\n", x, y, STATUS_MUTE_WIDTH, STATUS_MUTE_HEIGHT);
			status_slider_place(row, &x, &width, &middle);
			printf("KWL STATUS item name=volume x=%d y=%d width=%d height=%d\n", x, middle - STATUS_SLIDER_KNOB / 2, width, STATUS_SLIDER_KNOB);
		}
	}
}

/* Draws one row: a light card under it and what it shows. */
static void
status_draw_row(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct status_row *row)
{
	static const float card[4] = { 1.0f, 1.0f, 1.0f, 0.55f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	char name[64];
	char state[160];
	char percent[48];
	unsigned on;
	unsigned usable;
	int32_t width;

	/* The card the row's controls stand on. */
	glass_draw_solid(server, command, (float)row->x, (float)row->y, (float)row->width, (float)row->height, STATUS_ROW_RADIUS, card);

	/* What the row shows. */
	switch (row->kind) {
	case STATUS_ROW_WIFI:
		/* The network: Wi-Fi's switch and its state, or the wired connection's state without a switch. */
		kwl_network_panel_state(&usable, &on, state, sizeof(state));
		(void)snprintf(name, sizeof(name), "%s", kl_tr("Wi-Fi"));
		if (!usable)
			(void)snprintf(name, sizeof(name), "%s", kl_tr("Network"));
		status_draw_switch_row(server, command, row, name, state, on, usable);
		break;
	case STATUS_ROW_BLUETOOTH:
		/* Bluetooth's switch and its state. */
		kwl_bluetooth_panel_state(&usable, &on, state, sizeof(state));
		status_draw_switch_row(server, command, row, kl_tr("Bluetooth"), state, on, usable);
		break;
	case STATUS_ROW_SOUND:
		/* The sound's title, mute button and slider. */
		status_draw_sound(server, command, row);
		break;
	case STATUS_ROW_INPUT:
		/* The input method: its name, the language chosen and what a press does. */
		glass_draw_text(server, command, SIZE_TITLE, row->x + 14, row->y + 24, kl_tr("Input"), row->width - 80, dark);
		glass_draw_text(server, command, SIZE_BAR, row->x + 14, row->y + 44, kl_tr("Tap to switch the language"), row->width - 80, soft);
		state[0] = '\0';
		(void)kwl_ime_panel_label(server, state, sizeof(state));
		width = glass_text_width(server, SIZE_TITLE, state);
		glass_draw_text(server, command, SIZE_TITLE, row->x + row->width - 24 - width, row->y + 36, state, 60, dark);
		break;
	case STATUS_ROW_BATTERY:
		/* The battery's charge, and whether it charges. */
		(void)snprintf(percent, sizeof(percent), "%d%%", server->power.percent);
		glass_draw_text(server, command, SIZE_TITLE, row->x + 14, row->y + 28, kl_tr("Battery"), row->width - 120, dark);
		if (server->power.charging != 0U)
			(void)snprintf(percent, sizeof(percent), "%d%% %s", server->power.percent, kl_tr("Charging"));
		width = glass_text_width(server, SIZE_BAR, percent);
		glass_draw_text(server, command, SIZE_BAR, row->x + row->width - 14 - width, row->y + 28, percent, row->width / 2, soft);
		break;
	default:
		break;
	}
}

/* Draws a radio's row: its name and state at the left, its switch and the "›" at the right. */
static void
status_draw_switch_row(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct status_row *row,
	const char *name,
	const char *state,
	unsigned on,
	unsigned usable)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	int32_t limit;
	int32_t x;
	int32_t y;

	/* The name and the state, short of the switch. */
	limit = row->width - STATUS_MORE_WIDTH - STATUS_SWITCH_WIDTH - 28;
	glass_draw_text(server, command, SIZE_TITLE, row->x + 14, row->y + 24, name, limit, dark);
	glass_draw_text(server, command, SIZE_BAR, row->x + 14, row->y + 44, state, limit, soft);

	/* The switch, faded when the radio cannot be switched. */
	status_switch_place(row, &x, &y);
	if (usable) {
		status_draw_switch(server, command, x, y, on, 1.0f);
	} else {
		status_draw_switch(server, command, x, y, on, 0.35f);
	}

	/* The "›" that opens the radio's own menu. */
	glass_draw_text(server, command, SIZE_TITLE, row->x + row->width - STATUS_MORE_WIDTH + 12, row->y + row->height / 2 + 8, "\xe2\x80\xba", STATUS_MORE_WIDTH, dark);
}

/* Draws the sound's row: the title and the volume over the mute button and the slider. */
static void
status_draw_sound(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct status_row *row)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	static const float track[4] = { 0.62f, 0.66f, 0.72f, 0.55f };
	static const float button[4] = { 0.62f, 0.66f, 0.72f, 0.35f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float edge[4] = { 0.12f, 0.16f, 0.24f, 0.25f };
	char text[48];
	float fill[4];
	float fade;
	unsigned value;
	unsigned muted;
	unsigned kept;
	int32_t mute_x;
	int32_t mute_y;
	int32_t left;
	int32_t width;
	int32_t middle;
	int32_t knob;
	int sound;

	/* The title, and at the right the volume (Muted, or the percent) or why there is no sound. */
	kwl_volume_panel_state(&value, &muted, &sound);
	glass_draw_text(server, command, SIZE_TITLE, row->x + 14, row->y + 26, kl_tr("Sound"), row->width / 2, dark);
	(void)snprintf(text, sizeof(text), "%u%%", value);
	if (muted)
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Muted"));
	if (!sound)
		(void)snprintf(text, sizeof(text), "%s", kl_tr("No sound"));
	glass_draw_text(server, command, SIZE_BAR, row->x + row->width - 14 - glass_text_width(server, SIZE_BAR, text), row->y + 26, text, row->width / 2, soft);

	/* The controls are pale and do nothing without sound. */
	fade = 1.0f;
	if (!sound)
		fade = 0.35f;

	/* The mute button: filled with the accent while muted. */
	status_mute_place(row, &mute_x, &mute_y);
	kept = server->keep_colours;
	memcpy(fill, button, sizeof(fill));
	if (muted) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, fade, fill);
		kept = kwl_accent_as_is(server);
	}

	/* The button, the accent's colours given back, and its word. */
	glass_draw_solid(server, command, (float)mute_x, (float)mute_y, (float)STATUS_MUTE_WIDTH, (float)STATUS_MUTE_HEIGHT, (float)STATUS_MUTE_HEIGHT / 2.0f, fill);
	kwl_accent_done(server, kept);
	glass_draw_text_middle(server, command, SIZE_BAR, mute_x + STATUS_MUTE_WIDTH / 2, mute_y + STATUS_MUTE_HEIGHT / 2 + 6, kl_tr("Mute"), STATUS_MUTE_WIDTH, dark);

	/* The slider: the track, its filled part and the knob at the volume. */
	status_slider_place(row, &left, &width, &middle);
	glass_draw_solid(server, command, (float)left, (float)(middle - STATUS_TRACK_HEIGHT / 2), (float)width, (float)STATUS_TRACK_HEIGHT, (float)STATUS_TRACK_HEIGHT / 2.0f, track);
	knob = left + (int32_t)((unsigned)(width - STATUS_SLIDER_KNOB) * value / 100U);
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, fade, fill);
	kept = kwl_accent_as_is(server);
	glass_draw_solid(server, command, (float)left, (float)(middle - STATUS_TRACK_HEIGHT / 2), (float)(knob - left + STATUS_SLIDER_KNOB / 2), (float)STATUS_TRACK_HEIGHT, (float)STATUS_TRACK_HEIGHT / 2.0f, fill);
	kwl_accent_done(server, kept);
	glass_draw_solid(server, command, (float)knob - 1.0f, (float)(middle - STATUS_SLIDER_KNOB / 2) - 1.0f, (float)STATUS_SLIDER_KNOB + 2.0f, (float)STATUS_SLIDER_KNOB + 2.0f, (float)STATUS_SLIDER_KNOB / 2.0f + 1.0f, edge);
	glass_draw_solid(server, command, (float)knob, (float)(middle - STATUS_SLIDER_KNOB / 2), (float)STATUS_SLIDER_KNOB, (float)STATUS_SLIDER_KNOB, (float)STATUS_SLIDER_KNOB / 2.0f, white);
}

/* Draws a switch with its top left at x, y: on or off, faded when it cannot be used. */
static void
status_draw_switch(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t y,
	unsigned on,
	float fade)
{
	float pill[4] = { 0.62f, 0.66f, 0.72f, 1.0f };
	float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	int32_t inset;
	unsigned kept;

	/* The pill: the accent the user chose when on, drawn as it is with its knob (ws179-p001). */
	kept = server->keep_colours;
	if (on) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, pill);
		kept = kwl_accent_as_is(server);
	}

	/* Faded when it cannot be used, and drawn. */
	pill[3] *= fade;
	white[3] *= fade;
	glass_draw_solid(server, command, (float)x, (float)y, (float)STATUS_SWITCH_WIDTH, (float)STATUS_SWITCH_HEIGHT, (float)STATUS_SWITCH_HEIGHT / 2.0f, pill);

	/* The knob, right when on. */
	inset = (STATUS_SWITCH_HEIGHT - STATUS_KNOB) / 2;
	if (on) {
		glass_draw_solid(server, command, (float)(x + STATUS_SWITCH_WIDTH - inset - STATUS_KNOB), (float)(y + inset), (float)STATUS_KNOB, (float)STATUS_KNOB, (float)STATUS_KNOB / 2.0f, white);
	} else {
		glass_draw_solid(server, command, (float)(x + inset), (float)(y + inset), (float)STATUS_KNOB, (float)STATUS_KNOB, (float)STATUS_KNOB / 2.0f, white);
	}

	/* The accent's colours given back. */
	kwl_accent_done(server, kept);
}
