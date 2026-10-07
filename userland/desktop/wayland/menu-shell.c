/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Menu as the compositor draws and operates it (WS070,
 * plan/ws070/design.md section 6).
 *
 * A window's top-level items are drawn after its title: in its floating
 * title bar, or in the system bar while it is docked.  Pressing one opens
 * its popup; while a popup is open ("menu mode") the compositor takes the pointer
 * and the keyboard: the pointer moves between the top-level items and the
 * rows, a submenu opens beside its row, a release or a click on a row
 * chooses it, and a press anywhere else closes the menu.  The keys move the
 * selection (up, down, left, right), choose (Enter, Space) and close (Esc,
 * F10).  F10 opens the focused window's first menu from the keyboard, and
 * the focused window's shortcuts choose their items directly.
 *
 * Hits are tested against the places the top-level items were last drawn
 * at, so that what the pointer is over is what the user sees.  A popup's
 * rows are made from the committed model each time they are drawn or
 * tested; the selection is kept as an item ID, so a commit that reorders
 * the rows keeps it, and one that removes the item drops it.
 */

#include "desktop.h"
#include "menu.h"
#include "popup.h"
#include "titlebar.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How many popups can be open (the model's tree is at most eight deep), and the rows of one. */
#define SHELL_DEPTH		8U
#define SHELL_ROWS		64U

/* The top-level items hit-tested in one frame, and the windows whose bar layout is logged. */
#define SHELL_HITS		128U
#define SHELL_LOGGED		32U

/* The "..." item that holds the top-level items the bar has no room for. */
#define SHELL_OVERFLOW		0xffffffffU

/*
 * The rows a titlebar's "..." holds for its hidden controls (WS070 p010):
 * how many one frame records for all windows, how many an open popup holds,
 * and the longest label kept of one.
 */
#define SHELL_EXTRAS		128U
#define SHELL_OPEN_EXTRAS	64U
#define SHELL_EXTRA_LABEL	64U

/* A top-level item: its padding on each side, the gap between two, and the gap after the title. */
#define ITEM_PADDING		10
#define ITEM_GAP		2
#define TITLE_GAP		18

/* How strong the faint line under a top-level menu item's label is, of the label's ink (BUG-219). */
#define ITEM_UNDERLINE		0.28f

/* A popup's rows, its padding, the gutter for check marks, its least width, corners and margin to the output's edge. */
#define ROW_HEIGHT		30
#define SEPARATOR_HEIGHT	11
#define POPUP_PADDING		6
#define POPUP_GUTTER		30
#define POPUP_MINIMUM		200
#define POPUP_RADIUS		10.0f
#define POPUP_MARGIN		8

/* The last evdev code the keysym tables cover (KEY_DELETE); the keys' codes are <uapi/input.h>'s. */
#define SHELL_KEY_LAST		111U

/* The keypad's Enter, which <uapi/input.h> does not name. */
#define SHELL_KEY_KPENTER	96U

/* The modifier bits of the compositor's wl_keyboard.modifiers. */
#define SEAT_SHIFT		0x01U
#define SEAT_CTRL		0x04U
#define SEAT_ALT		0x08U
#define SEAT_META		0x40U

/*
 * One top-level item as it was last drawn: the window, the item (or
 * SHELL_OVERFLOW), whether in the system bar, the first top-level item the
 * overflow holds, the rectangle the pointer hits (the bar's whole height),
 * and where a popup under it starts.
 */
struct shell_hit {
	struct kwl_object *surface;
	uint32_t item;
	unsigned docked;
	unsigned first_hidden;
	unsigned extra_start;
	unsigned extra_count;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int32_t popup_y;
};

/*
 * One open popup: the submenu whose children are its rows (SHELL_OVERFLOW
 * for the overflow's), the first hidden top-level item for the overflow,
 * where it was placed from (and, for a submenu, the parent popup's left
 * edge to flip to), where it is, and the selected row's item (0 for none).
 */
struct shell_popup {
	uint32_t parent;
	unsigned first_hidden;
	int32_t anchor_x;
	int32_t anchor_y;
	int32_t flip_x;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	uint32_t selected;
};

/*
 * A press on a top-level item that waits for its release to open it, or to
 * go far enough to move the window: the window (NULL for none), the item,
 * whether in the system bar, and where and by what it was pressed.
 */
struct shell_wait {
	struct kwl_object *surface;
	uint32_t item;
	unsigned docked;
	int32_t x;
	int32_t y;
	enum kwl_contact_source source;
};

/* The last bar layout logged for a window (a checksum), so a layout is logged once. */
struct shell_logged {
	struct kwl_object *surface;
	uint32_t checksum;
};

/*
 * The menus' state.
 *
 * surface is the window whose menu is open (NULL when none is), docked
 * whether it opened from the system bar, and popups[0..depth-1] the open
 * popups from the top-level one down.  pressing says the press that opened
 * or entered the menu is still down, so that its release on a row chooses
 * it.  eaten_key is a key whose press the compositor took, so that its release is
 * taken too.  The hits are rebuilt every frame (kwl_menu_frame).
 *
 * A titlebar's "..." (titlebar-shell.c) records the rows of its hidden
 * controls with its hit, in extras (rebuilt every frame too); the popup it
 * opens keeps its own copy in open_extras, whose rows come before the
 * window's menu.  A row's label is kept with it.
 *
 * context is the xdg_context_menu_v1 whose menu is open at a point of the
 * window (ws071-p009), NULL for the window's own menu: its model is the
 * context menu's, its choice and its end go to it, and it has no bar.
 *
 * desktop_top is the window that was on top when a context menu opened on
 * the desktop surface (ws094-p005; NULL for none, and for any other menu):
 * the desktop is never the window on top, so its menu stays open while
 * that window stays on top and closes when another comes up or maps.
 *
 * waiting is a press on a top-level item while no menu is open (ws099-p030;
 * its window is NULL when there is none): where it was pressed and by what,
 * and the item.  Its release where it was pressed opens the item's popup;
 * a press that goes far enough moves the window instead (shell.c).
 */
struct shell_menu {
	struct kwl_object *surface;
	struct kwl_object *context;
	struct kwl_object *desktop_top;
	unsigned docked;
	unsigned depth;
	struct shell_popup popups[SHELL_DEPTH];
	unsigned pressing;
	uint32_t eaten_key;
	unsigned hit_count;
	struct shell_hit hits[SHELL_HITS];
	struct shell_logged logged[SHELL_LOGGED];
	struct kwl_menu_item extras[SHELL_EXTRAS];
	char extra_labels[SHELL_EXTRAS][SHELL_EXTRA_LABEL];
	unsigned extra_count;
	struct kwl_menu_item open_extras[SHELL_OPEN_EXTRAS];
	char open_extra_labels[SHELL_OPEN_EXTRAS][SHELL_EXTRA_LABEL];
	unsigned open_extra_count;
	struct shell_wait waiting;
};

/*
 * The one compositor's menus.  The compositor runs one server per process, and
 * the menus live as long as it; the zero value is "no menu open, nothing
 * drawn yet".
 */
static struct shell_menu shell_menu;

/*
 * The model of a window without a menu whose "..." holds only hidden
 * controls: empty, so the popups have a model to read.  It never changes.
 */
static const struct kwl_menu_model shell_empty_model;

/* The line between the hidden controls' rows and the menu's in "...". */
static const struct kwl_menu_item shell_extra_line = {
	0xefffffffU, KWL_MENU_ROOT, KWL_MENU_SEPARATOR, 0U, 1U, 1U, 0U, 0U, 0U, 0U, (char *)"", NULL
};

/*
 * The XKB keysyms of the US layout's keys by evdev code, plain and with
 * Shift (the compositor has no keymap; clients are sent evdev codes).  0 is a key
 * with no keysym here.
 */
static const uint32_t shell_plain_keysyms[SHELL_KEY_LAST + 1U] = {
	0, 0xff1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0xff08, 0xff09,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0xff0d, 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' ', 0, 0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2,
	0xffc3, 0xffc4, 0xffc5, 0xffc6, 0xffc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0xffc8, 0xffc9, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0xff50, 0xff52, 0xff55, 0xff51, 0xff53, 0xff57, 0xff54, 0xff56, 0xff63, 0xffff
};

/* The same keys with Shift held: the symbols a US keyboard prints over the digits and punctuation. */
static const uint32_t shell_shifted_keysyms[SHELL_KEY_LAST + 1U] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '{', '}', 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, ':', '"', '~', 0, '|', 0, 0, 0, 0,
	0, 0, 0, '<', '>', '?', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void shell_open(struct kwl_server *server, const struct shell_hit *hit, unsigned keyboard);
static void shell_close_from(struct kwl_server *server, unsigned level, unsigned notify);
static void shell_activate(struct kwl_server *server, const struct kwl_menu_item *item, const char *via);
static unsigned shell_rows(const struct kwl_menu_model *model, const struct shell_popup *popup, const struct kwl_menu_item **rows);
static void shell_layout(struct kwl_server *server, const struct kwl_menu_model *model, unsigned level);
static const struct kwl_menu_item *shell_row_at(struct kwl_server *server, const struct kwl_menu_model *model, unsigned level, int32_t x, int32_t y, int32_t *row_y);
static int shell_popup_at(int32_t x, int32_t y);
static void shell_select_step(struct kwl_server *server, const struct kwl_menu_model *model, int step);
static void shell_select_letter(struct kwl_server *server, const struct kwl_menu_model *model, uint32_t keysym);
static void shell_open_child(struct kwl_server *server, const struct kwl_menu_model *model, unsigned level, const struct kwl_menu_item *item, int32_t row_y, unsigned keyboard);
static void shell_choose_row(struct kwl_server *server, const struct kwl_menu_model *model, const struct kwl_menu_item *row, int32_t row_y, unsigned level, const char *via);
static void shell_step_top(struct kwl_server *server, int step);
static void shell_menu_key(struct kwl_server *server, const struct kwl_menu_model *model, uint32_t key);
static unsigned shell_selectable(const struct kwl_menu_item *item);
static unsigned shell_usable(const struct kwl_menu_model *model, const struct kwl_menu_item *item);
static const struct shell_hit *shell_hit_at(struct kwl_server *server, int32_t x, int32_t y);
static void shell_wait_release(struct kwl_server *server);
static int shell_wait_motion(struct kwl_server *server);
static const struct shell_hit *shell_first_hit(struct kwl_object *surface);
static void shell_add_hit(struct kwl_object *surface, uint32_t item, unsigned docked, unsigned first_hidden, const struct kwl_menu_area *area, int32_t x, int32_t width);
static void shell_log_bar(struct kwl_object *surface, unsigned docked, const struct kwl_menu_area *area, uint32_t checksum);
static uint32_t shell_mix(uint32_t checksum, uint32_t value);
static void shell_log_popup(struct kwl_server *server, const struct kwl_menu_model *model, unsigned level);
static int shell_keysym(uint32_t key, uint32_t seat_modifiers, uint32_t *keysym, uint32_t *modifiers);
static const struct kwl_menu_item *shell_shortcut_item(const struct kwl_menu_model *model, uint32_t keysym, uint32_t modifiers);
static void shell_shortcut_text(const struct kwl_menu_item *item, char *text, size_t size);
static void shell_key_name(uint32_t keysym, char *text, size_t size);
static void shell_draw_popup(struct kwl_server *server, VkCommandBuffer command, const struct kwl_menu_model *model, unsigned level);
static void shell_draw_row(struct kwl_server *server, VkCommandBuffer command, const struct shell_popup *popup, const struct kwl_menu_item *row, int32_t row_y);
static const struct kwl_menu_model *shell_model(struct kwl_object *surface, struct kwl_object **place);
static const struct kwl_menu_item *shell_item(const struct kwl_menu_model *model, uint32_t id);
static struct kwl_object *shell_place(struct kwl_object *surface);

/*
 * Starts a frame: the top-level items are hit-tested where this frame draws them.
 */
void
kwl_menu_frame(
	struct kwl_server *server)
{
	/* The server is the one this file's state belongs to. */
	(void)server;

	/* The last frame's places, and the rows of the hidden controls, are forgotten. */
	shell_menu.hit_count = 0;
	shell_menu.extra_count = 0;
}

/*
 * Tells how wide a window's title may be drawn when its menu shares the
 * room after it: two fifths of the room with a menu, all of it without.
 */
int32_t
kwl_menu_title_limit(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t available)
{
	const struct kwl_menu_item *tops[1];
	const struct kwl_menu_model *model;
	struct kwl_object *place;
	unsigned count;

	/* The server is the one this file's state belongs to. */
	(void)server;

	/* A window without a menu keeps the whole room for its title. */
	model = kwl_menu_of_surface(surface, &place);
	if (model == NULL)
		return available;

	/* A menu with no visible top-level item takes no room either. */
	count = kwl_menu_children(model, KWL_MENU_ROOT, tops, 1U);
	if (count == 0U)
		return available;

	/* Succeeded: the title's share. */
	return available * 2 / 5;
}

/*
 * Draws a window's top-level items in the area after its title (in its
 * title bar, or in the system bar when docked), with the pointer's hover
 * and the open item marked, and records where they are for the pointer.
 */
void
kwl_menu_draw_bar(
	struct kwl_server *server,
	VkCommandBuffer command,
	struct kwl_object *surface,
	unsigned docked,
	const struct kwl_menu_area *area,
	const float *ink,
	float fade)
{
	static const float hover[4] = { 1.0f, 1.0f, 1.0f, 0.62f };
	const struct kwl_menu_item *tops[SHELL_ROWS];
	const struct kwl_menu_model *model;
	struct kwl_object *place;
	float colour[4];
	float pill[4];
	unsigned count;
	unsigned shown;
	unsigned index;
	unsigned recording;
	unsigned open;
	unsigned kept;
	uint32_t checksum;
	int32_t widths[SHELL_ROWS];
	float underline[4];
	int32_t more;
	int32_t x;
	int32_t pill_y;
	int32_t pill_height;
	int32_t baseline;

	/* The window's menu, and its top-level items that are not separators. */
	model = kwl_menu_of_surface(surface, &place);
	if (model == NULL)
		return;
	count = kwl_menu_children(model, KWL_MENU_ROOT, tops, SHELL_ROWS);
	shown = 0;
	for (index = 0; index < count; index++) {
		/* A separator has no place among the top-level items. */
		if (tops[index]->type == KWL_MENU_SEPARATOR)
			continue;
		tops[shown] = tops[index];
		widths[shown] = glass_text_width(server, SIZE_BAR, tops[index]->label) + 2 * ITEM_PADDING;
		shown++;
	}

	/* Only those are laid out. */
	count = shown;

	/* As many as fit; if not all do, the rest go under "..." at the end. */
	more = glass_text_width(server, SIZE_BAR, "...") + 2 * ITEM_PADDING;
	x = area->x;
	for (shown = 0; shown < count; shown++) {
		/* The last one needs no room for "..." after it. */
		if (shown + 1U == count && x + widths[shown] <= area->right)
			continue;

		/* Any other needs room for "..." after it too, or it and those after it go under "...". */
		if (x + widths[shown] + ITEM_GAP + more > area->right)
			break;

		/* The next one starts after it. */
		x += widths[shown] + ITEM_GAP;
	}

	/*
	 * Places are recorded only where they are seen as they are: not while
	 * the desktop layer is moved (App Home, the desktops' slide) or the
	 * window docks or fades.
	 */
	recording = 0;
	if (!server->layer_on &&
	    server->anim == NULL &&
	    fade >= 1.0f)
		recording = 1;

	/* The pills sit in the middle of the bar. */
	pill_height = 28;
	if (docked)
		pill_height = 26;
	pill_y = area->top + (area->height - pill_height) / 2;
	baseline = area->top + area->height / 2 + 5;

	/* Each item that fits, then "..." when some do not. */
	checksum = shell_mix(2166136261U, docked);
	x = area->x;
	for (index = 0; index <= shown; index++) {
		/* The overflow is drawn only when something is under it. */
		if (index == shown && shown == count)
			break;

		/* The overflow is as wide as "..." with its padding. */
		if (index == shown)
			widths[index] = more;

		/* Whether its popup is open. */
		open = 0;
		if (shell_menu.surface == surface &&
		    shell_menu.docked == docked &&
		    shell_menu.depth > 0U) {
			if (index < shown && shell_menu.popups[0].parent == tops[index]->id)
				open = 1;
			if (index == shown && shell_menu.popups[0].parent == SHELL_OVERFLOW)
				open = 1;
		}

		/* The open item is tinted; the one under the pointer, with no menu open, is lit. */
		if (open) {
			kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.20f * fade, pill);
			kept = kwl_accent_as_is(server);
			glass_draw_solid(server, command, (float)x, (float)pill_y, (float)widths[index], (float)pill_height, 7.0f, pill);
			kwl_accent_done(server, kept);
		} else if (shell_menu.surface == NULL &&
			   server->pointer_x >= x && server->pointer_x < x + widths[index] &&
			   server->pointer_y >= area->top && server->pointer_y < area->top + area->height) {
			memcpy(pill, hover, sizeof(pill));
			pill[3] *= fade;
			glass_draw_solid(server, command, (float)x, (float)pill_y, (float)widths[index], (float)pill_height, 7.0f, pill);
		}

		/* The label's ink: paler when the item cannot be chosen, and faded with the bar. */
		memcpy(colour, ink, sizeof(colour));
		if (index < shown && !tops[index]->enabled)
			colour[3] *= 0.4f;
		colour[3] *= fade;

		/*
		 * The item's label with a faint line under it, so that a menu reads as
		 * something to press, not as more of the title (BUG-219), or "..." for
		 * the overflow.
		 */
		if (index < shown) {
			glass_draw_text(server, command, SIZE_BAR, x + ITEM_PADDING, baseline, tops[index]->label, widths[index], colour);
			memcpy(underline, colour, sizeof(underline));
			underline[3] *= ITEM_UNDERLINE;
			glass_draw_solid(server, command, (float)(x + ITEM_PADDING), (float)(baseline + 4), (float)(widths[index] - 2 * ITEM_PADDING), 1.0f, 0.5f, underline);
		} else {
			glass_draw_text(server, command, SIZE_BAR, x + ITEM_PADDING, baseline, "...", widths[index], colour);
		}

		/* An item's place, for the pointer. */
		if (recording && index < shown)
			shell_add_hit(surface, tops[index]->id, docked, 0U, area, x, widths[index]);

		/* The overflow's, with the first top-level item it holds. */
		if (recording && index == shown)
			shell_add_hit(surface, SHELL_OVERFLOW, docked, shown, area, x, widths[index]);

		/* The layout's checksum, for the log: the item (none for the overflow), its offset and its width. */
		if (index < shown)
			checksum = shell_mix(checksum, tops[index]->id);
		checksum = shell_mix(checksum, (uint32_t)(x - area->origin));
		checksum = shell_mix(checksum, (uint32_t)widths[index]);

		/* The next item starts after it. */
		x += widths[index] + ITEM_GAP;
	}

	/* A new layout is logged for the tests that click the items. */
	if (recording)
		shell_log_bar(surface, docked, area, checksum);
}

/*
 * Draws the open popups over everything but the cursor, from the top-level
 * one down.
 */
void
kwl_menu_draw_popups(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	const struct kwl_menu_model *model;
	struct kwl_object *place;
	unsigned level;

	/* Nothing is open. */
	if (shell_menu.surface == NULL)
		return;

	/* The window's model, which may have changed since the popups opened. */
	model = shell_model(shell_menu.surface, &place);
	if (model == NULL)
		return;

	/* Each popup at its place for its current rows. */
	for (level = 0; level < shell_menu.depth; level++) {
		shell_layout(server, model, level);
		shell_draw_popup(server, command, model, level);
	}
}

/*
 * Handles a pointer button for the menus.  A press on a top-level item
 * waits: its release where it was pressed opens the item's popup (or
 * chooses an item with no children), and a press that goes far enough
 * moves the window instead (ws099-p030).  In menu mode the menus take
 * every button.  Returns 1 when the button was the menus'.
 */
int
kwl_menu_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct kwl_menu_model *model;
	const struct kwl_menu_item *row;
	const struct shell_hit *hit;
	struct kwl_object *place;
	int32_t row_y;
	int level;

	/* With no menu open only a left press on a top-level item, and its release, are the menus'. */
	if (shell_menu.surface == NULL) {
		/* Another button goes on. */
		if (button != KWL_BUTTON_LEFT)
			return 0;

		/* The release of a waiting press opens its item's popup. */
		if (state == 0U) {
			if (shell_menu.waiting.surface == NULL)
				return 0;
			shell_wait_release(server);
			return 1;
		}

		/* A press off every top-level item goes on. */
		hit = shell_hit_at(server, server->pointer_x, server->pointer_y);
		if (hit == NULL)
			return 0;

		/* A floating window comes to the top. */
		if (!hit->docked)
			kwl_glass_raise(server, hit->surface);

		/* The press waits for its release, or to go far enough to move the window. */
		shell_menu.waiting.surface = hit->surface;
		shell_menu.waiting.item = hit->item;
		shell_menu.waiting.docked = hit->docked;
		shell_menu.waiting.x = server->pointer_x;
		shell_menu.waiting.y = server->pointer_y;
		shell_menu.waiting.source = server->shell_source;
		return 1;
	}

	/* The model of the open menu; a menu whose model went is closed. */
	model = shell_model(shell_menu.surface, &place);
	if (model == NULL) {
		shell_close_from(server, 0U, 0U);
		return 1;
	}

	/* A release chooses the row under it, when the press was the menus'. */
	if (state == 0U) {
		/* The release of a press the menus did not take is only swallowed. */
		if (!shell_menu.pressing)
			return 1;

		/* The press ends; a release off every popup chooses nothing. */
		shell_menu.pressing = 0;
		level = shell_popup_at(server->pointer_x, server->pointer_y);
		if (level < 0)
			return 1;

		/* The row under the release is chosen (the padding is no row). */
		row = shell_row_at(server, model, (unsigned)level, server->pointer_x, server->pointer_y, &row_y);
		if (row != NULL)
			shell_choose_row(server, model, row, row_y, (unsigned)level, "pointer");
		return 1;
	}

	/* A press on a top-level item of the open menu closes it, or opens that item's popup. */
	hit = shell_hit_at(server, server->pointer_x, server->pointer_y);
	if (hit != NULL &&
	    hit->surface == shell_menu.surface &&
	    hit->docked == shell_menu.docked) {
		/* The open item's own press closes its menu. */
		if (hit->item == shell_menu.popups[0].parent) {
			shell_close_from(server, 0U, 1U);
			return 1;
		}

		/* Another item's popup takes over. */
		shell_open(server, hit, 0);
		shell_menu.pressing = 1;
		return 1;
	}

	/* A press on a popup waits for its release to choose. */
	level = shell_popup_at(server->pointer_x, server->pointer_y);
	if (level >= 0) {
		shell_menu.pressing = 1;
		return 1;
	}

	/* A press anywhere else closes the menu and goes no further. */
	shell_close_from(server, 0U, 1U);

	/* Succeeded: the press was the menus'. */
	return 1;
}

/*
 * Follows the pointer in menu mode: another top-level item takes over, a
 * row is selected, and a submenu row opens its popup.  Out of menu mode a
 * waiting press on a top-level item that goes far enough moves its window
 * (ws099-p030).  Returns 1 when the motion is the menus', 0 otherwise.
 */
int
kwl_menu_motion(
	struct kwl_server *server)
{
	const struct kwl_menu_model *model;
	const struct kwl_menu_item *row;
	const struct shell_hit *hit;
	struct kwl_object *place;
	unsigned selectable;
	int32_t row_y;
	int level;
	int taken;

	/* Out of menu mode only a waiting press on a top-level item takes the pointer, until it moves its window. */
	if (shell_menu.surface == NULL) {
		taken = shell_wait_motion(server);
		return taken;
	}

	/* The open menu's model; while it has gone (the tick closes the menu) the motion is only taken. */
	model = shell_model(shell_menu.surface, &place);
	if (model == NULL)
		return 1;

	/* Another top-level item of the same bar opens instead. */
	hit = shell_hit_at(server, server->pointer_x, server->pointer_y);
	if (hit != NULL &&
	    hit->surface == shell_menu.surface &&
	    hit->docked == shell_menu.docked &&
	    hit->item != shell_menu.popups[0].parent) {
		shell_open(server, hit, 0);
		return 1;
	}

	/* Off every popup, the deepest one's selection goes (its submenus stay open). */
	level = shell_popup_at(server->pointer_x, server->pointer_y);
	if (level < 0) {
		shell_menu.popups[shell_menu.depth - 1U].selected = 0;
		return 1;
	}

	/* A row whose submenu is open already keeps it open; nothing changes. */
	row = shell_row_at(server, model, (unsigned)level, server->pointer_x, server->pointer_y, &row_y);
	if (row != NULL &&
	    shell_menu.depth > (unsigned)level + 1U &&
	    shell_menu.popups[level + 1].parent == row->id) {
		shell_menu.popups[level].selected = row->id;
		return 1;
	}

	/* Deeper popups than the row's own close, and its popup's selection goes. */
	shell_close_from(server, (unsigned)level + 1U, 1U);
	shell_menu.popups[level].selected = 0;

	/* On the padding nothing is selected. */
	if (row == NULL)
		return 1;

	/* Nor is a row that cannot be chosen. */
	selectable = shell_selectable(row);
	if (!selectable)
		return 1;

	/* The row under the pointer is selected. */
	shell_menu.popups[level].selected = row->id;

	/* A submenu's popup opens beside its row. */
	if (row->type == KWL_MENU_SUBMENU)
		shell_open_child(server, model, (unsigned)level, row, row_y, 0);

	/* Succeeded: the motion was the menus'. */
	return 1;
}

/*
 * Takes every key in menu mode and moves or chooses with it, and takes the
 * release of a key whose press the menus took.  Returns 1 when the key was
 * the menus'.
 */
int
kwl_menu_grab_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	const struct kwl_menu_model *model;
	struct kwl_object *place;

	/* The release of a key the menus took is theirs too. */
	if (state == 0U &&
	    shell_menu.eaten_key != 0U &&
	    key == shell_menu.eaten_key) {
		shell_menu.eaten_key = 0;
		return 1;
	}

	/* With no menu open, other keys go on. */
	if (shell_menu.surface == NULL)
		return 0;

	/* In menu mode every release is the menus'. */
	if (state == 0U)
		return 1;

	/* A press is acted on, and its release will be the menus' too. */
	shell_menu.eaten_key = key;

	/* The model of the open menu; a menu whose model went is closed. */
	model = shell_model(shell_menu.surface, &place);
	if (model == NULL) {
		shell_close_from(server, 0U, 0U);
		return 1;
	}

	/* The key moves, chooses or closes. */
	shell_menu_key(server, model, key);
	server->dirty = 1;

	/* Succeeded: the key was the menus'. */
	return 1;
}

/*
 * Opens the focused window's first menu with F10, and chooses the item
 * whose shortcut a key press is.  Called after the compositor's own shortcuts and
 * before the client.  Returns 1 when the key was the menus'.
 */
int
kwl_menu_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	const struct kwl_menu_model *model;
	const struct kwl_menu_item *item;
	const struct shell_hit *hit;
	struct kwl_object *place;
	uint32_t keysym;
	uint32_t modifiers;
	int known;

	/* Only a press, on a focused window. */
	if (state == 0U || server->focus == NULL)
		return 0;

	/* The window's menu; a window without one leaves its keys to the others. */
	model = kwl_menu_of_surface(server->focus, &place);
	if (model == NULL)
		return 0;

	/* F10 alone opens the first menu from the keyboard, where it was last drawn. */
	if (key == KEY_F10 && (server->modifiers & (SEAT_SHIFT | SEAT_CTRL | SEAT_ALT | SEAT_META)) == 0U) {
		/* A menu that was not drawn (the window is covered or hidden) cannot open. */
		hit = shell_first_hit(server->focus);
		if (hit == NULL)
			return 0;

		/* It opens with its first row selected, and F10's release is the menus'. */
		shell_open(server, hit, 1);
		shell_menu.eaten_key = key;
		return 1;
	}

	/* The key's symbol and modifiers; a key the table does not know is no shortcut. */
	known = shell_keysym(key, server->modifiers, &keysym, &modifiers);
	if (!known)
		return 0;

	/* The usable item whose shortcut it is; without one the key goes on. */
	item = shell_shortcut_item(model, keysym, modifiers);
	if (item == NULL)
		return 0;

	/* The item is chosen and the key's press and release go no further. */
	shell_menu.eaten_key = key;
	kwl_menu_send_activated(place, item, "shortcut");
	server->dirty = 1;

	/* Succeeded: the key was the menus'. */
	return 1;
}

/*
 * Closes the menu when what it belongs to has changed: its window is no
 * longer on top, docks, floats, goes fullscreen or away, App Home or
 * Wiseview opens, or an open submenu's item has gone; and drops a selection
 * that can no longer be chosen.
 */
void
kwl_menu_tick(
	struct kwl_server *server)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	const struct kwl_menu_model *model;
	const struct kwl_menu_item *item;
	struct kwl_object *surface;
	struct kwl_object *desktop;
	struct kwl_object *place;
	struct kwl_object *top;
	unsigned level;
	unsigned count;
	unsigned index;
	unsigned found;
	float home;

	/* Nothing is open. */
	surface = shell_menu.surface;
	if (surface == NULL)
		return;

	/* The window, its state and what covers it. */
	model = shell_model(surface, &place);
	home = kwl_home_progress(server);
	top = kwl_top_window(server);

	/*
	 * The desktop surface is never the window on top (desktop.c): its
	 * context menu stays open while the window on top is the one that was
	 * when it opened (ws094-p005).
	 */
	desktop = kwl_desktop_surface(server);
	if (surface == desktop && top == shell_menu.desktop_top)
		top = surface;

	/* The menu's window must still be the one on top, as it was, with nothing over it. */
	if (model == NULL ||
	    surface != top ||
	    surface->maximized != shell_menu.docked ||
	    surface->fullscreen ||
	    surface->minimized ||
	    home > 0.0f ||
	    server->wiseview > 0.0f ||
	    server->wiseview_gesture ||
	    server->wiseview_moving) {
		shell_close_from(server, 0U, 1U);
		return;
	}

	/* Each popup's submenu must still be a row of the popup above it (the top-level one, a visible submenu). */
	for (level = 0; level < shell_menu.depth; level++) {
		/* The overflow's rows are the top level's, and need no submenu. */
		if (shell_menu.popups[level].parent == SHELL_OVERFLOW)
			continue;

		/* Nor do a context menu's first popup's. */
		if (shell_menu.popups[level].parent == KWL_MENU_ROOT && shell_menu.context != NULL)
			continue;

		/* The popup's item must still be a visible, enabled submenu. */
		item = kwl_menu_item(model, shell_menu.popups[level].parent);
		found = 0;
		if (item != NULL &&
		    item->visible &&
		    item->enabled &&
		    item->type == KWL_MENU_SUBMENU)
			found = 1;

		/* A submenu popup's item is one of the rows above it. */
		if (found && level > 0U) {
			count = shell_rows(model, &shell_menu.popups[level - 1U], rows);
			found = 0;
			for (index = 0; index < count; index++) {
				/* The row is the submenu. */
				if (rows[index] == item)
					found = 1;
			}
		}

		/* This popup and those under it close. */
		if (!found) {
			shell_close_from(server, level, 1U);
			break;
		}
	}

	/* A selection that can no longer be chosen goes. */
	for (level = 0; level < shell_menu.depth; level++) {
		/* The selected row, still there and usable up to the top. */
		item = shell_item(model, shell_menu.popups[level].selected);
		found = 0;
		if (item != NULL)
			found = shell_usable(model, item);

		/* And still one that can be chosen. */
		if (found)
			found = shell_selectable(item);

		/* The row is gone, hidden or disabled. */
		if (!found)
			shell_menu.popups[level].selected = 0;
	}
}

/*
 * Forgets an object that is going: its window's places in the bar, and the
 * open menu when it is the window, its place, its toplevel or its model.
 * No event is sent; the object may be the one that would receive it.
 */
void
kwl_menu_forget(
	struct kwl_server *server,
	struct kwl_object *object)
{
	struct kwl_object *surface;
	struct kwl_object *place;
	struct kwl_object *toplevel;
	unsigned index;
	unsigned kept;
	unsigned mine;

	/* The window's hits and its logged layout go. */
	kept = 0;
	for (index = 0; index < shell_menu.hit_count; index++) {
		/* The window's own are dropped. */
		if (shell_menu.hits[index].surface == object)
			continue;

		/* The others are kept, in order. */
		shell_menu.hits[kept] = shell_menu.hits[index];
		kept++;
	}

	/* The table keeps the others, and the window's logged layout is forgotten. */
	shell_menu.hit_count = kept;
	if (shell_menu.waiting.surface == object)
		shell_menu.waiting.surface = NULL;
	for (index = 0; index < SHELL_LOGGED; index++) {
		/* The window's entry is free again. */
		if (shell_menu.logged[index].surface == object)
			shell_menu.logged[index].surface = NULL;
	}

	/* Nothing is open. */
	surface = shell_menu.surface;
	if (surface == NULL)
		return;

	/* A desktop's context menu is told done when the window that was on top goes (ws094-p005). */
	if (shell_menu.desktop_top != NULL && object == shell_menu.desktop_top) {
		shell_menu.desktop_top = NULL;
		shell_close_from(server, 0U, 1U);
		return;
	}

	/* A context menu that goes closes without being told; one whose menu goes is told done. */
	if (shell_menu.context != NULL && object == shell_menu.context) {
		shell_menu.context = NULL;
		shell_close_from(server, 0U, 0U);
		return;
	}

	/* A context menu whose menu goes is told done. */
	if (shell_menu.context != NULL && object == shell_menu.context->shown_menu) {
		shell_close_from(server, 0U, 1U);
		return;
	}

	/* The open menu's place, and its window's toplevel. */
	(void)kwl_menu_of_surface(surface, &place);
	toplevel = NULL;
	if (surface->role != NULL)
		toplevel = surface->role->top;

	/* The object is the open menu's when it is the window or its toplevel. */
	mine = 0;
	if (object == surface || object == toplevel)
		mine = 1;

	/* Or the menu's place, or the menu shown there. */
	if (place != NULL &&
	    (object == place || object == place->shown_menu))
		mine = 1;

	/* The menu closes without telling the window; a context menu of it is told done. */
	if (mine && shell_menu.context != NULL) {
		shell_close_from(server, 0U, 1U);
		return;
	}

	/* The window's own menu closes silently. */
	if (mine)
		shell_close_from(server, 0U, 0U);
}

/*
 * Opens a context menu (menu.c): its menu's top-level items as a popup at
 * a point of a window (surface coordinates), in place of any menu open.
 * Returns 0, or ENOENT when there is nothing to show (the caller tells the
 * context menu it is done).
 */
int
kwl_menu_open_context(
	struct kwl_server *server,
	struct kwl_object *context,
	struct kwl_object *surface,
	int32_t x,
	int32_t y)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	const struct kwl_menu_model *model;
	struct shell_popup *popup;
	struct kwl_object *desktop;
	int32_t left;
	int32_t top;
	unsigned count;

	/* A context menu without a committed menu has nothing to show. */
	if (context->shown_menu == NULL || context->shown_menu->menu_model == NULL)
		return ENOENT;

	/* Nor has one whose menu has no rows. */
	model = context->shown_menu->menu_model;
	count = kwl_menu_children(model, KWL_MENU_ROOT, rows, SHELL_ROWS);
	if (count == 0U)
		return ENOENT;

	/* Whatever was open closes, and no hidden controls' rows come with this popup. */
	shell_close_from(server, 0U, 1U);
	shell_menu.open_extra_count = 0;

	/* Where the window's body is on the output. */
	left = surface->x;
	top = surface->y;
	if (server->glass)
		(void)kwl_glass_body_origin(server, surface, &left, &top);

	/* The popup at the point, with the context menu's rows; on the desktop, the window on top then. */
	shell_menu.surface = surface;
	shell_menu.context = context;
	shell_menu.desktop_top = NULL;
	desktop = kwl_desktop_surface(server);
	if (surface == desktop)
		shell_menu.desktop_top = kwl_top_window(server);
	shell_menu.docked = surface->maximized;
	shell_menu.depth = 1;
	shell_menu.pressing = 0;
	popup = &shell_menu.popups[0];
	memset(popup, 0, sizeof(*popup));
	popup->parent = KWL_MENU_ROOT;
	popup->anchor_x = left + x;
	popup->anchor_y = top + y;
	popup->flip_x = left + x;
	shell_layout(server, model, 0U);

	/* The log gives the place and the rows. */
	printf("KWL MENU context client=%llu context=%u surface=%u x=%d y=%d rows=%u\n", (unsigned long long)context->client->number, context->id, surface->id, popup->x, popup->y, count);
	shell_log_popup(server, model, 0U);
	server->dirty = 1;

	/* Succeeded: the context menu is open. */
	return 0;
}

/*
 * Tells whether a menu (a window's, a titlebar's "..." or a context menu)
 * is open.
 */
int
kwl_menu_is_open(void)
{
	/* A menu open has its window. */
	if (shell_menu.surface != NULL)
		return 1;

	/* None. */
	return 0;
}

/*
 * Records a titlebar's "..." (titlebar-shell.c) as the menus' overflow at a
 * place of the area, holding rows for its hidden controls (their IDs and
 * labels) before the window's menu.
 */
void
kwl_menu_add_overflow(
	struct kwl_object *surface,
	unsigned docked,
	const struct kwl_menu_area *area,
	int32_t x,
	int32_t width,
	const uint32_t *ids,
	const char *const *labels,
	unsigned count)
{
	struct kwl_menu_item *extra;
	struct shell_hit *hit;
	unsigned start;
	unsigned index;

	/* The overflow's place, holding the menu from its first top-level item. */
	shell_add_hit(surface, SHELL_OVERFLOW, docked, 0U, area, x, width);

	/* No hit could be recorded at all. */
	if (shell_menu.hit_count == 0U)
		return;

	/* The last hit is the overflow's unless the table was full; the rows then have no hit to go with. */
	hit = &shell_menu.hits[shell_menu.hit_count - 1U];
	if (hit->surface != surface || hit->item != SHELL_OVERFLOW)
		return;

	/* The rows of the hidden controls, as many as the frame's table holds. */
	start = shell_menu.extra_count;
	for (index = 0; index < count && shell_menu.extra_count < SHELL_EXTRAS; index++) {
		/* A plain row, told apart from the menu's items by KWL_MENU_EXTRA_BASE, with a copy of its label. */
		extra = &shell_menu.extras[shell_menu.extra_count];
		memset(extra, 0, sizeof(*extra));
		extra->id = KWL_MENU_EXTRA_BASE | ids[index];
		extra->parent = KWL_MENU_ROOT;
		extra->type = KWL_MENU_NORMAL;
		extra->enabled = 1;
		extra->visible = 1;
		(void)snprintf(shell_menu.extra_labels[shell_menu.extra_count], SHELL_EXTRA_LABEL, "%s", labels[index]);
		extra->label = shell_menu.extra_labels[shell_menu.extra_count];
		shell_menu.extra_count++;
	}

	/* The hit names its rows. */
	hit->extra_start = start;
	hit->extra_count = shell_menu.extra_count - start;
}

/*
 * Finds the keysym a key types with the seat's modifiers on the US layout
 * (a letter with Shift is its capital); returns 1, or 0 for a key without
 * one.
 */
int
kwl_menu_keysym(
	uint32_t key,
	uint32_t seat_modifiers,
	uint32_t *keysym)
{
	uint32_t modifiers;
	int known;

	/* The menus' own table (a shortcut keeps a letter small, with Shift beside it). */
	known = shell_keysym(key, seat_modifiers, keysym, &modifiers);
	if (known == 0)
		return 0;

	/* A letter typed with Shift is its capital. */
	if ((modifiers & KWL_MENU_SHIFT) != 0U &&
	    *keysym >= 'a' &&
	    *keysym <= 'z')
		*keysym -= 'a' - 'A';

	/* Succeeded: the keysym. */
	return 1;
}

/*
 * Opens a top-level item's popup (closing any other), or chooses a
 * top-level item that has no children.  From the keyboard the first row
 * that can be chosen is selected.
 */
static void
shell_open(
	struct kwl_server *server,
	const struct shell_hit *hit,
	unsigned keyboard)
{
	const struct kwl_menu_model *model;
	const struct kwl_menu_item *item;
	struct shell_popup *popup;
	struct kwl_object *place;
	unsigned index;

	/* The window's model; a "..." holding only hidden controls reads the empty one. */
	model = kwl_menu_of_surface(hit->surface, &place);
	if (model == NULL && hit->extra_count > 0U)
		model = &shell_empty_model;

	/* A window with neither has nothing to open. */
	if (model == NULL)
		return;

	/* The top-level item (none for the overflow); one that went or is disabled does not open. */
	item = NULL;
	if (hit->item != SHELL_OVERFLOW) {
		item = kwl_menu_item(model, hit->item);
		if (item == NULL || !item->enabled)
			return;
	}

	/* Whatever was open closes, and a press waiting on a top-level item is over. */
	shell_close_from(server, 0U, 1U);
	shell_menu.waiting.surface = NULL;

	/* The rows of the hidden controls come with the popup, kept (with their labels) while it is open. */
	shell_menu.open_extra_count = 0;
	for (index = 0; index < hit->extra_count && index < SHELL_OPEN_EXTRAS; index++) {
		/* The row and its label, the row naming the copied label. */
		shell_menu.open_extras[index] = shell_menu.extras[hit->extra_start + index];
		memcpy(shell_menu.open_extra_labels[index], shell_menu.extra_labels[hit->extra_start + index], SHELL_EXTRA_LABEL);
		shell_menu.open_extras[index].label = shell_menu.open_extra_labels[index];
		shell_menu.open_extra_count++;
	}

	/* An item with no children is chosen at once. */
	if (item != NULL && item->type != KWL_MENU_SUBMENU) {
		shell_activate(server, item, "pointer");
		return;
	}

	/* The top-level popup, under the item. */
	shell_menu.surface = hit->surface;
	shell_menu.docked = hit->docked;
	shell_menu.depth = 1;
	popup = &shell_menu.popups[0];
	memset(popup, 0, sizeof(*popup));
	popup->parent = hit->item;
	popup->first_hidden = hit->first_hidden;
	popup->anchor_x = hit->x - 4;
	popup->anchor_y = hit->popup_y;
	shell_layout(server, model, 0U);

	/* The client hears that the submenu opened, or that a titlebar's "..." did (a chance to update its menu). */
	if (item != NULL) {
		kwl_menu_send_popup(place, item->id, 1U);
	} else {
		kwl_titlebar_overflow_opened(hit->surface);
	}

	/* The log gives the popup and its rows. */
	shell_log_popup(server, model, 0U);

	/* From the keyboard the first row that can be chosen is selected. */
	if (keyboard)
		shell_select_step(server, model, 1);

	/* The output shows the popup. */
	server->dirty = 1;
}

/*
 * Closes the popups from a level down (0 closes the menu), telling the
 * client of each submenu that closed when notify is set.
 */
static void
shell_close_from(
	struct kwl_server *server,
	unsigned level,
	unsigned notify)
{
	struct kwl_object *place;
	uint32_t parent;

	/* Nothing that deep is open. */
	if (level >= shell_menu.depth)
		return;

	/* The deepest first, as they were opened in reverse. */
	place = shell_place(shell_menu.surface);
	while (shell_menu.depth > level) {
		/* The deepest open popup goes. */
		shell_menu.depth--;
		parent = shell_menu.popups[shell_menu.depth].parent;

		/* The client hears that its submenu closed (the overflow is the compositor's own). */
		if (notify &&
		    place != NULL &&
		    parent != SHELL_OVERFLOW)
			kwl_menu_send_popup(place, parent, 0U);

		/* The log line the tests read. */
		printf("KWL MENU close client=%llu surface=%u item=%u depth=%u\n", (unsigned long long)shell_menu.surface->client->number, shell_menu.surface->id, parent, shell_menu.depth);
	}

	/* The whole menu closed: menu mode ends, and a context menu is told it is done. */
	if (shell_menu.depth == 0U) {
		/* A context menu is told it is done. */
		if (shell_menu.context != NULL && notify)
			kwl_menu_send_context_done(shell_menu.context);

		/* Menu mode ends: no context menu, no window, no press. */
		shell_menu.context = NULL;
		shell_menu.surface = NULL;
		shell_menu.pressing = 0;
	}

	/* The output is drawn without them. */
	server->dirty = 1;
}

/* Chooses an item: the menu closes and its client hears the choice. */
static void
shell_activate(
	struct kwl_server *server,
	const struct kwl_menu_item *item,
	const char *via)
{
	struct kwl_object *context;
	struct kwl_object *surface;
	struct kwl_object *place;
	uint32_t id;

	/* A context menu's choice goes to it; it closes (told done) after the choice. */
	context = shell_menu.context;
	if (context != NULL) {
		kwl_menu_send_context_activated(context, item, via);
		shell_close_from(server, 0U, 1U);
		return;
	}

	/* The window of the choice: the open menu's, or the one on top for a shortcut. */
	surface = shell_menu.surface;
	if (surface == NULL)
		surface = kwl_top_window(server);

	/* The place the choice goes to, found before the menu closes. */
	(void)kwl_menu_of_surface(surface, &place);

	/* A hidden control's row goes to the titlebar (its row is gone once the menu closes). */
	if ((item->id & KWL_MENU_EXTRA_BASE) == KWL_MENU_EXTRA_BASE && item->id != shell_extra_line.id) {
		id = item->id & ~KWL_MENU_EXTRA_BASE;
		shell_close_from(server, 0U, 1U);
		kwl_titlebar_overflow_chosen(server, surface, id);
		return;
	}

	/* The menu closes first (its submenus say so). */
	shell_close_from(server, 0U, 1U);

	/* Then the choice is sent to the window's menu. */
	if (place != NULL)
		kwl_menu_send_activated(place, item, via);
}

/* Lists a popup's rows: a submenu's visible children, or the overflow's top-level items. */
static unsigned
shell_rows(
	const struct kwl_menu_model *model,
	const struct shell_popup *popup,
	const struct kwl_menu_item **rows)
{
	const struct kwl_menu_item *tops[SHELL_ROWS];
	unsigned count;
	unsigned index;
	unsigned seen;
	unsigned kept;

	/* A submenu's rows are its children. */
	if (popup->parent != SHELL_OVERFLOW) {
		count = kwl_menu_children(model, popup->parent, rows, SHELL_ROWS);
		return count;
	}

	/* A titlebar's "..." holds its hidden controls first, a line after them when the menu follows. */
	kept = 0;
	for (index = 0; index < shell_menu.open_extra_count; index++) {
		rows[kept] = &shell_menu.open_extras[index];
		kept++;
	}

	/* The overflow holds the top-level items from the first the bar had no room for. */
	count = kwl_menu_children(model, KWL_MENU_ROOT, tops, SHELL_ROWS);

	/* A line between the hidden controls' rows and the menu's. */
	if (kept > 0U && count > 0U) {
		rows[kept] = &shell_extra_line;
		kept++;
	}

	/* The top-level items the bar counted. */
	seen = 0;
	for (index = 0; index < count; index++) {
		/* The bar skips separators, and so does the count of what it showed. */
		if (tops[index]->type == KWL_MENU_SEPARATOR)
			continue;

		/* One the bar had no room for is a row of the overflow. */
		if (seen >= popup->first_hidden) {
			rows[kept] = tops[index];
			kept++;
		}

		/* One more of those the bar counted. */
		seen++;
	}

	/* Succeeded: the hidden top-level items. */
	return kept;
}

/*
 * Works out a popup's size from its rows and its place from where it was
 * opened: under its top-level item, or beside its row (on the other side
 * when it does not fit), kept inside the output.
 */
static void
shell_layout(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	unsigned level)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	const struct kwl_plane_rect *output;
	struct shell_popup *popup;
	char shortcut[48];
	unsigned count;
	unsigned index;
	unsigned shown;
	unsigned slot;
	int32_t label;
	int32_t hint;
	int32_t width;
	int32_t height;
	int32_t right;
	int32_t bottom;

	/* The rows, and the widest of them. */
	popup = &shell_menu.popups[level];
	count = shell_rows(model, popup, rows);
	popup->width = POPUP_MINIMUM;
	height = 2 * POPUP_PADDING;
	for (index = 0; index < count; index++) {
		/* A separator is short and takes no width. */
		if (rows[index]->type == KWL_MENU_SEPARATOR) {
			height += SEPARATOR_HEIGHT;
			continue;
		}

		/* A row's height, and its width: the gutter, the label, a gap and the shortcut. */
		height += ROW_HEIGHT;
		shell_shortcut_text(rows[index], shortcut, sizeof(shortcut));
		label = glass_text_width(server, SIZE_BAR, rows[index]->label);
		hint = glass_text_width(server, SIZE_BAR, shortcut);
		width = POPUP_GUTTER + label + 40 + hint + 14;

		/* A submenu's row has its arrow too. */
		if (rows[index]->type == KWL_MENU_SUBMENU)
			width += 16;

		/* The popup is as wide as its widest row. */
		if (width > popup->width)
			popup->width = width;
	}

	/* Its height holds every row. */
	popup->height = height;

	/* The output it was opened on (a head's too, ws113-p015). */
	shown = kwl_outputs(server, outputs);
	slot = kwl_output_at(server, popup->anchor_x, popup->anchor_y);
	if (slot >= shown || outputs[slot].width == 0U)
		slot = KWL_PLANE_ANCHOR;
	output = &outputs[slot];

	/* Where it was opened from, pushed back from the right edge (a submenu flips to the left of its parent). */
	popup->x = popup->anchor_x;
	popup->y = popup->anchor_y;
	right = output->x + (int32_t)output->width - POPUP_MARGIN;
	if (popup->x + popup->width > right) {
		/* Back inside the output. */
		popup->x = right - popup->width;

		/* A submenu goes to the left of its parent instead. */
		if (level > 0U)
			popup->x = popup->flip_x - popup->width + 4;
	}

	/* Never past the left edge. */
	if (popup->x < output->x + POPUP_MARGIN)
		popup->x = output->x + POPUP_MARGIN;

	/* And from the bottom. */
	bottom = output->y + (int32_t)output->height - POPUP_MARGIN;
	if (popup->y + popup->height > bottom)
		popup->y = bottom - popup->height;

	/* Never over the bar (the system bar, or a head's). */
	if (popup->y < output->y + KWL_GLASS_BAR + 4)
		popup->y = output->y + KWL_GLASS_BAR + 4;
}

/* Finds the row of a popup at a point, and the row's top; NULL off its rows. */
static const struct kwl_menu_item *
shell_row_at(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	unsigned level,
	int32_t x,
	int32_t y,
	int32_t *row_y)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	struct shell_popup *popup;
	unsigned count;
	unsigned index;
	int32_t top;
	int32_t height;

	/* The popup where it is now. */
	shell_layout(server, model, level);
	popup = &shell_menu.popups[level];

	/* A point beside it is on none of its rows. */
	if (x < popup->x || x >= popup->x + popup->width)
		return NULL;

	/* The rows from the top. */
	count = shell_rows(model, popup, rows);
	top = popup->y + POPUP_PADDING;
	for (index = 0; index < count; index++) {
		/* The row's height: a separator is thinner. */
		height = ROW_HEIGHT;
		if (rows[index]->type == KWL_MENU_SEPARATOR)
			height = SEPARATOR_HEIGHT;

		/* The row the point is in. */
		if (y >= top && y < top + height) {
			*row_y = top;
			return rows[index];
		}

		/* The next row is under it. */
		top += height;
	}

	/* The padding, or below the rows. */
	return NULL;
}

/* Finds the deepest open popup at a point; -1 when the point is on none. */
static int
shell_popup_at(
	int32_t x,
	int32_t y)
{
	const struct shell_popup *popup;
	int level;

	/* The deepest popup is drawn over the others. */
	for (level = (int)shell_menu.depth - 1; level >= 0; level--) {
		/* A point beside the popup is not on it. */
		popup = &shell_menu.popups[level];
		if (x < popup->x || x >= popup->x + popup->width)
			continue;

		/* Nor is one above or below it. */
		if (y < popup->y || y >= popup->y + popup->height)
			continue;

		/* The point is on this popup. */
		return level;
	}

	/* None. */
	return -1;
}

/* Moves the deepest popup's selection to the next (step 1) or previous (-1) row that can be chosen, around the ends. */
static void
shell_select_step(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	int step)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	struct shell_popup *popup;
	unsigned count;
	unsigned tried;
	unsigned selectable;
	int at;
	int index;

	/* The deepest popup's rows; a popup without rows has nothing to select. */
	popup = &shell_menu.popups[shell_menu.depth - 1U];
	count = shell_rows(model, popup, rows);
	if (count == 0U)
		return;

	/* Where the selection is among them (-1 for nowhere). */
	at = -1;
	for (index = 0; index < (int)count; index++) {
		/* The selected row. */
		if (rows[index]->id == popup->selected)
			at = index;
	}

	/* From before the first (or after the last) when nothing is selected. */
	if (at < 0 && step < 0)
		at = (int)count;

	/* The next row in the direction that can be chosen, trying each row once. */
	selectable = 0;
	for (tried = 0; tried < count; tried++) {
		at = (at + step + (int)count) % (int)count;
		selectable = shell_selectable(rows[at]);
		if (selectable)
			break;
	}

	/* The selection moves there, when there is such a row. */
	if (selectable)
		popup->selected = rows[at]->id;

	/* The output shows the new selection. */
	server->dirty = 1;
}

/* Moves the deepest popup's selection to the next row whose label starts with a letter or digit. */
static void
shell_select_letter(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	uint32_t keysym)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	struct shell_popup *popup;
	unsigned count;
	unsigned tried;
	unsigned selectable;
	int at;
	int index;
	int first;

	/* The deepest popup's rows. */
	popup = &shell_menu.popups[shell_menu.depth - 1U];
	count = shell_rows(model, popup, rows);

	/* Where the selection is among them (-1 for nowhere). */
	at = -1;
	for (index = 0; index < (int)count; index++) {
		/* The selected row. */
		if (rows[index]->id == popup->selected)
			at = index;
	}

	/* The next row after the selection whose first letter, in lower case, is the key's. */
	for (tried = 0; tried < count; tried++) {
		/* The next row's first letter, in lower case. */
		at = (at + 1) % (int)count;
		first = (unsigned char)rows[at]->label[0];
		if (first >= 'A' && first <= 'Z')
			first = first - 'A' + 'a';

		/* A row starting with another letter is passed. */
		if ((uint32_t)first != keysym)
			continue;

		/* The first such row that can be chosen is selected. */
		selectable = shell_selectable(rows[at]);
		if (selectable) {
			popup->selected = rows[at]->id;
			break;
		}
	}

	/* The output shows the new selection. */
	server->dirty = 1;
}

/* Opens a submenu row's popup beside it, one level below the row's popup. */
static void
shell_open_child(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	unsigned level,
	const struct kwl_menu_item *item,
	int32_t row_y,
	unsigned keyboard)
{
	struct shell_popup *parent;
	struct shell_popup *popup;
	struct kwl_object *place;

	/* It is open already. */
	if (shell_menu.depth > level + 1U && shell_menu.popups[level + 1U].parent == item->id)
		return;

	/* No popup opens past the deepest level. */
	if (level + 1U >= SHELL_DEPTH)
		return;

	/* Deeper popups close. */
	shell_close_from(server, level + 1U, 1U);

	/* The new one goes beside the row (or, where it does not fit, to the left of the row's popup). */
	parent = &shell_menu.popups[level];
	popup = &shell_menu.popups[level + 1U];
	memset(popup, 0, sizeof(*popup));
	popup->parent = item->id;
	popup->anchor_x = parent->x + parent->width - 4;
	popup->anchor_y = row_y - POPUP_PADDING;
	popup->flip_x = parent->x;
	shell_menu.depth = level + 2U;
	shell_layout(server, model, level + 1U);

	/* The client hears that the submenu opened. */
	place = shell_place(shell_menu.surface);
	if (place != NULL)
		kwl_menu_send_popup(place, item->id, 1U);

	/* The log gives the popup and its rows. */
	shell_log_popup(server, model, level + 1U);

	/* From the keyboard the first row that can be chosen is selected. */
	if (keyboard)
		shell_select_step(server, model, 1);

	/* The output shows the popup. */
	server->dirty = 1;
}

/* Acts on a chosen row: a submenu opens, an item that can be chosen is activated. */
static void
shell_choose_row(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	const struct kwl_menu_item *row,
	int32_t row_y,
	unsigned level,
	const char *via)
{
	unsigned selectable;

	/* A separator or a disabled row does nothing, and the menu stays. */
	selectable = shell_selectable(row);
	if (!selectable)
		return;

	/* A submenu opens (from the keyboard, with its first row selected). */
	if (row->type == KWL_MENU_SUBMENU) {
		shell_open_child(server, model, level, row, row_y, 1);
		return;
	}

	/* Anything else is chosen. */
	shell_activate(server, row, via);
}

/* Opens the next (step 1) or previous (-1) top-level item of the open bar, around the ends. */
static void
shell_step_top(
	struct kwl_server *server,
	int step)
{
	const struct shell_hit *tops[SHELL_HITS];
	unsigned count;
	unsigned index;
	int at;

	/* A context menu has no bar to step along. */
	if (shell_menu.context != NULL)
		return;

	/* The open bar's top-level items, in order. */
	count = 0;
	at = 0;
	for (index = 0; index < shell_menu.hit_count; index++) {
		/* Only the open bar's items. */
		if (shell_menu.hits[index].surface != shell_menu.surface || shell_menu.hits[index].docked != shell_menu.docked)
			continue;

		/* The open item's place among them. */
		if (shell_menu.hits[index].item == shell_menu.popups[0].parent)
			at = (int)count;

		/* The item, in order. */
		tops[count] = &shell_menu.hits[index];
		count++;
	}

	/* A bar that was not drawn has no neighbour to open. */
	if (count == 0U)
		return;

	/* The neighbour, around the ends, opens from the keyboard. */
	at = (at + step + (int)count) % (int)count;
	shell_open(server, tops[at], 1);
}

/* Acts on a key pressed in menu mode. */
static void
shell_menu_key(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	uint32_t key)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	const struct kwl_menu_item *row;
	struct shell_popup *popup;
	unsigned level;
	unsigned count;
	unsigned index;
	uint32_t keysym;
	uint32_t modifiers;
	int32_t row_y;
	int known;

	/* The deepest popup, its selected row and where the row is. */
	level = shell_menu.depth - 1U;
	popup = &shell_menu.popups[level];
	shell_layout(server, model, level);
	count = shell_rows(model, popup, rows);
	row = NULL;
	row_y = popup->y + POPUP_PADDING;
	for (index = 0; index < count; index++) {
		/* The selected row, at the top reached so far. */
		if (rows[index]->id == popup->selected) {
			row = rows[index];
			break;
		}

		/* The next row starts under this one. */
		row_y += ROW_HEIGHT;
		if (rows[index]->type == KWL_MENU_SEPARATOR)
			row_y += SEPARATOR_HEIGHT - ROW_HEIGHT;
	}

	/* Each key's meaning. */
	switch (key) {
	case KEY_UP:
		shell_select_step(server, model, -1);
		break;
	case KEY_DOWN:
		shell_select_step(server, model, 1);
		break;
	case KEY_HOME:
		popup->selected = 0;
		shell_select_step(server, model, 1);
		break;
	case KEY_END:
		popup->selected = 0;
		shell_select_step(server, model, -1);
		break;
	case KEY_RIGHT:
		/* Into a selected submenu, otherwise to the next top-level menu. */
		if (row != NULL && row->type == KWL_MENU_SUBMENU) {
			shell_open_child(server, model, level, row, row_y, 1);
			break;
		}

		/* The next top-level menu opens. */
		shell_step_top(server, 1);
		break;
	case KEY_LEFT:
		/* Out of a submenu, otherwise to the previous top-level menu. */
		if (level > 0U) {
			shell_close_from(server, level, 1U);
			break;
		}

		/* The previous top-level menu opens. */
		shell_step_top(server, -1);
		break;
	case KEY_ENTER:
	case SHELL_KEY_KPENTER:
	case KEY_SPACE:
		/* The selected row is chosen. */
		if (row != NULL)
			shell_choose_row(server, model, row, row_y, level, "key");
		break;
	case KEY_ESC:
		/* One popup closes. */
		shell_close_from(server, level, 1U);
		break;
	case KEY_F10:
		/* The whole menu closes. */
		shell_close_from(server, 0U, 1U);
		break;
	default:
		/* A letter or a digit selects the next row starting with it. */
		known = shell_keysym(key, 0U, &keysym, &modifiers);
		if (known &&
		    ((keysym >= 'a' && keysym <= 'z') ||
		     (keysym >= '0' && keysym <= '9')))
			shell_select_letter(server, model, keysym);
		break;
	}
}

/* Tells whether a row can be selected and chosen: not a separator, and enabled. */
static unsigned
shell_selectable(
	const struct kwl_menu_item *item)
{
	/* A separator is never chosen. */
	if (item->type == KWL_MENU_SEPARATOR)
		return 0;

	/* A disabled item is shown but not chosen. */
	if (!item->enabled)
		return 0;

	/* Succeeded: the row can be chosen. */
	return 1;
}

/* Tells whether an item and all its submenus up to the top are visible and enabled. */
static unsigned
shell_usable(
	const struct kwl_menu_model *model,
	const struct kwl_menu_item *item)
{
	unsigned depth;

	/* Up the parents; the model is at most eight deep. */
	for (depth = 0; item != NULL && depth <= SHELL_DEPTH; depth++) {
		/* A hidden or disabled level makes the item unusable. */
		if (!item->visible || !item->enabled)
			return 0;

		/* The top is reached with every level usable. */
		if (item->parent == KWL_MENU_ROOT)
			return 1;

		/* Up to the submenu it is in. */
		item = kwl_menu_item(model, item->parent);
	}

	/* A broken chain is not usable. */
	return 0;
}

/*
 * Ends a waiting press on a top-level item with its release: where it was
 * pressed, its popup opens.  A press that went far enough without a motion
 * between (a finger's quick tap) opens nothing.
 */
static void
shell_wait_release(
	struct kwl_server *server)
{
	const struct shell_hit *hit;
	struct shell_wait waiting;
	int moved;

	/* The press ends. */
	waiting = shell_menu.waiting;
	shell_menu.waiting.surface = NULL;

	/* A release far from the press is no click. */
	moved = kwl_glass_press_moved(server, waiting.x, waiting.y, waiting.source);
	if (moved)
		return;

	/* The item must still be where it was pressed, in the same bar. */
	hit = shell_hit_at(server, waiting.x, waiting.y);
	if (hit == NULL)
		return;
	if (hit->surface != waiting.surface || hit->item != waiting.item || hit->docked != waiting.docked)
		return;

	/* Succeeded: its menu opens (the next press chooses a row). */
	shell_open(server, hit, 0);
}

/*
 * Follows the pointer while a press on a top-level item waits: once it has
 * gone far enough, the window moves from the pressed point (shell.c) and the
 * press is no longer the menus'.  Returns 1 while the press still waits.
 */
static int
shell_wait_motion(
	struct kwl_server *server)
{
	struct shell_wait waiting;
	int moved;

	/* Nothing waits. */
	if (shell_menu.waiting.surface == NULL)
		return 0;

	/* A press whose release went elsewhere (a screen that opened over it) waits no more. */
	if ((server->buttons_down & 1U) == 0U) {
		shell_menu.waiting.surface = NULL;
		return 0;
	}

	/* Not far enough yet: the press keeps the pointer. */
	waiting = shell_menu.waiting;
	moved = kwl_glass_press_moved(server, waiting.x, waiting.y, waiting.source);
	if (!moved)
		return 1;

	/* The window moves instead, and its move follows this motion (shell.c). */
	shell_menu.waiting.surface = NULL;
	printf("KWL MENU press moves client=%llu surface=%u item=%u docked=%u\n", (unsigned long long)waiting.surface->client->number, waiting.surface->id, waiting.item, waiting.docked);
	kwl_glass_press_move(server, waiting.surface, waiting.docked, waiting.x, waiting.y);

	/* Succeeded: the motion goes on to the move. */
	return 0;
}

/*
 * Finds the top-level item at a point, where the last frame drew it; NULL
 * when there is none, or when another window covers the title bar there
 * (the system bar is over every window).
 */
static const struct shell_hit *
shell_hit_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	const struct shell_hit *hit;
	struct kwl_object *top;
	unsigned index;

	/* Drawn later is drawn over, so the last one wins. */
	for (index = shell_menu.hit_count; index > 0U; index--) {
		/* A point beside the item is not on it. */
		hit = &shell_menu.hits[index - 1U];
		if (x < hit->x || x >= hit->x + hit->width)
			continue;

		/* Nor is one above or below its bar. */
		if (y < hit->y || y >= hit->y + hit->height)
			continue;

		/* An item in a title bar counts only where its window is the one on top. */
		if (!hit->docked) {
			top = kwl_glass_window_at(server, x, y);
			if (top != hit->surface)
				return NULL;
		}

		/* Succeeded: the item under the point. */
		return hit;
	}

	/* None. */
	return NULL;
}

/* Finds a window's first top-level item as last drawn (in the system bar when it is docked). */
static const struct shell_hit *
shell_first_hit(
	struct kwl_object *surface)
{
	unsigned index;

	/* The first recorded for the window, in its present place. */
	for (index = 0; index < shell_menu.hit_count; index++) {
		/* The window's item, from the bar it is shown in. */
		if (shell_menu.hits[index].surface == surface && shell_menu.hits[index].docked == surface->maximized)
			return &shell_menu.hits[index];
	}

	/* The window's menu was not drawn. */
	return NULL;
}

/* Records where a top-level item was drawn: the bar's whole height over the item's width. */
static void
shell_add_hit(
	struct kwl_object *surface,
	uint32_t item,
	unsigned docked,
	unsigned first_hidden,
	const struct kwl_menu_area *area,
	int32_t x,
	int32_t width)
{
	struct shell_hit *hit;

	/* A full table drops the rest of the frame's items. */
	if (shell_menu.hit_count == SHELL_HITS)
		return;

	/* The item and its rectangle; a popup starts a little under the bar. */
	hit = &shell_menu.hits[shell_menu.hit_count];
	hit->surface = surface;
	hit->item = item;
	hit->docked = docked;
	hit->first_hidden = first_hidden;
	hit->x = x;
	hit->y = area->top;
	hit->width = width;
	hit->height = area->height;
	hit->popup_y = area->top + area->height + 6;

	/* One more is recorded this frame. */
	shell_menu.hit_count++;
}

/* Logs a window's top-level items when their layout differs from the last one logged for it. */
static void
shell_log_bar(
	struct kwl_object *surface,
	unsigned docked,
	const struct kwl_menu_area *area,
	uint32_t checksum)
{
	const struct shell_hit *hit;
	struct shell_logged *entry;
	const char *where;
	unsigned index;
	uint32_t item;

	/* The window's entry, or a free one (the first when the table is full). */
	entry = &shell_menu.logged[0];
	for (index = 0; index < SHELL_LOGGED; index++) {
		/* The window's own entry. */
		if (shell_menu.logged[index].surface == surface) {
			entry = &shell_menu.logged[index];
			break;
		}

		/* A free entry serves until the window's own is found. */
		if (shell_menu.logged[index].surface == NULL)
			entry = &shell_menu.logged[index];
	}

	/* The same layout was logged already. */
	if (entry->surface == surface && entry->checksum == checksum)
		return;

	/* The entry remembers this layout. */
	entry->surface = surface;
	entry->checksum = checksum;

	/* In the system bar, or in the window's own title bar. */
	where = "floating";
	if (docked)
		where = "docked";

	/* Each item of the window in this frame, from the bar's left edge (the overflow as item 0). */
	for (index = 0; index < shell_menu.hit_count; index++) {
		/* Only this window's items in this bar. */
		hit = &shell_menu.hits[index];
		if (hit->surface != surface || hit->docked != docked)
			continue;

		/* The overflow is logged as item 0. */
		item = hit->item;
		if (item == SHELL_OVERFLOW)
			item = 0U;

		/* The log line the tests read. */
		printf("KWL MENU bar client=%llu surface=%u where=%s item=%u offset=%d top=%d width=%d height=%d\n", (unsigned long long)surface->client->number, surface->id,
		       where, item, hit->x - area->origin, hit->y, hit->width, hit->height);
	}
}

/* Mixes a value into a checksum (FNV-1a over its four bytes). */
static uint32_t
shell_mix(
	uint32_t checksum,
	uint32_t value)
{
	unsigned index;

	/* Each byte, lowest first. */
	for (index = 0; index < 4U; index++) {
		checksum ^= (value >> (8U * index)) & 0xffU;
		checksum *= 16777619U;
	}

	/* Succeeded: the new checksum. */
	return checksum;
}

/* Logs a popup that opened, and its rows, for the tests that click them. */
static void
shell_log_popup(
	struct kwl_server *server,
	const struct kwl_menu_model *model,
	unsigned level)
{
	const struct kwl_menu_item *rows[SHELL_ROWS];
	const struct shell_popup *popup;
	unsigned count;
	unsigned index;
	uint32_t parent;
	int32_t top;
	int32_t height;

	/* The popup where it is now. */
	shell_layout(server, model, level);
	popup = &shell_menu.popups[level];

	/* Its item (the overflow's as item 0). */
	parent = popup->parent;
	if (parent == SHELL_OVERFLOW)
		parent = 0U;

	/* The log line the tests read. */
	printf("KWL MENU open client=%llu surface=%u item=%u depth=%u x=%d y=%d width=%d height=%d\n", (unsigned long long)shell_menu.surface->client->number, shell_menu.surface->id,
	       parent, level + 1U, popup->x, popup->y, popup->width, popup->height);

	/* Each row. */
	count = shell_rows(model, popup, rows);
	top = popup->y + POPUP_PADDING;
	for (index = 0; index < count; index++) {
		/* The row's height: a separator is thinner. */
		height = ROW_HEIGHT;
		if (rows[index]->type == KWL_MENU_SEPARATOR)
			height = SEPARATOR_HEIGHT;

		/* Its line, and the next row under it. */
		printf("KWL MENU row item=%u depth=%u y=%d height=%d\n", rows[index]->id, level + 1U, top, height);
		top += height;
	}
}

/*
 * Turns an evdev key and the seat's modifiers into a keysym and the
 * protocol's modifiers.  A letter keeps Shift as a modifier; a key whose
 * symbol Shift changes gives the changed symbol without Shift (Ctrl+Shift+=
 * is Ctrl++).  Returns 0 for a key the table does not know.
 */
static int
shell_keysym(
	uint32_t key,
	uint32_t seat_modifiers,
	uint32_t *keysym,
	uint32_t *modifiers)
{
	uint32_t plain;
	uint32_t shifted;

	/* A key past the table has no symbol here. */
	if (key > SHELL_KEY_LAST)
		return 0;

	/* The key's plain symbol (0 is none). */
	plain = shell_plain_keysyms[key];
	if (plain == 0U)
		return 0;

	/* The modifiers in the protocol's bits. */
	*modifiers = 0;
	if ((seat_modifiers & SEAT_SHIFT) != 0U)
		*modifiers |= KWL_MENU_SHIFT;
	if ((seat_modifiers & SEAT_CTRL) != 0U)
		*modifiers |= KWL_MENU_CTRL;
	if ((seat_modifiers & SEAT_ALT) != 0U)
		*modifiers |= KWL_MENU_ALT;
	if ((seat_modifiers & SEAT_META) != 0U)
		*modifiers |= KWL_MENU_SUPER;

	/* Shift changes the symbol of a digit or a punctuation key, and is then part of it. */
	*keysym = plain;
	shifted = shell_shifted_keysyms[key];
	if ((*modifiers & KWL_MENU_SHIFT) != 0U && shifted != 0U) {
		*keysym = shifted;
		*modifiers &= ~KWL_MENU_SHIFT;
	}

	/* Succeeded: the keysym and modifiers. */
	return 1;
}

/* Finds the usable item whose shortcut is a keysym with modifiers; NULL when none is. */
static const struct kwl_menu_item *
shell_shortcut_item(
	const struct kwl_menu_model *model,
	uint32_t keysym,
	uint32_t modifiers)
{
	const struct kwl_menu_item *item;
	uint32_t wanted;
	unsigned index;
	unsigned usable;

	/* Every item with a shortcut; a capital letter is the same key as its small one. */
	for (index = 0; index < model->count; index++) {
		/* An item without a shortcut, or with other modifiers, is passed. */
		item = &model->items[index];
		if (item->keysym == 0U || item->modifiers != modifiers)
			continue;

		/* Its key in lower case, as the key's symbol is. */
		wanted = item->keysym;
		if (wanted >= 'A' && wanted <= 'Z')
			wanted = wanted - 'A' + 'a';

		/* An item with another key is passed. */
		if (wanted != keysym)
			continue;

		/* A submenu or a separator is not chosen by a key. */
		if (item->type == KWL_MENU_SUBMENU || item->type == KWL_MENU_SEPARATOR)
			continue;

		/* Nor an item that cannot be chosen now (it, or a submenu above it, hidden or disabled). */
		usable = shell_usable(model, item);
		if (usable)
			return item;
	}

	/* No usable item has the shortcut. */
	return NULL;
}

/* Writes an item's shortcut as it is shown ("Ctrl+Shift+C"), or an empty string for none. */
static void
shell_shortcut_text(
	const struct kwl_menu_item *item,
	char *text,
	size_t size)
{
	char name[16];

	/* The text starts empty, and an item without a shortcut keeps it so. */
	text[0] = '\0';
	if (item->keysym == 0U)
		return;

	/* The modifiers in a fixed order: Ctrl, Alt, Shift, Super. */
	if ((item->modifiers & KWL_MENU_CTRL) != 0U)
		(void)strncat(text, "Ctrl+", size - strlen(text) - 1U);
	if ((item->modifiers & KWL_MENU_ALT) != 0U)
		(void)strncat(text, "Alt+", size - strlen(text) - 1U);
	if ((item->modifiers & KWL_MENU_SHIFT) != 0U)
		(void)strncat(text, "Shift+", size - strlen(text) - 1U);
	if ((item->modifiers & KWL_MENU_SUPER) != 0U)
		(void)strncat(text, "Super+", size - strlen(text) - 1U);

	/* Then the key's name. */
	shell_key_name(item->keysym, name, sizeof(name));
	(void)strncat(text, name, size - strlen(text) - 1U);
}

/* Writes the name a keysym is shown by: a letter in capitals, a symbol itself, F1..F12, or a key's name. */
static void
shell_key_name(
	uint32_t keysym,
	char *text,
	size_t size)
{
	/* The function keys. */
	if (keysym >= 0xffbeU && keysym <= 0xffc9U) {
		(void)snprintf(text, size, "F%u", keysym - 0xffbeU + 1U);
		return;
	}

	/* A letter in capitals. */
	if (keysym >= 'a' && keysym <= 'z') {
		(void)snprintf(text, size, "%c", (int)(keysym - 'a' + 'A'));
		return;
	}

	/* A space by its name. */
	if (keysym == ' ') {
		(void)snprintf(text, size, "Space");
		return;
	}

	/* Any other printable symbol as itself. */
	if (keysym > ' ' && keysym < 0x7fU) {
		(void)snprintf(text, size, "%c", (int)keysym);
		return;
	}

	/* The named keys. */
	switch (keysym) {
	case 0xff0dU:
		(void)snprintf(text, size, "Enter");
		break;
	case 0xff1bU:
		(void)snprintf(text, size, "Esc");
		break;
	case 0xff09U:
		(void)snprintf(text, size, "Tab");
		break;
	case 0xff08U:
		(void)snprintf(text, size, "Backspace");
		break;
	case 0xffffU:
		(void)snprintf(text, size, "Del");
		break;
	case 0xff63U:
		(void)snprintf(text, size, "Ins");
		break;
	case 0xff50U:
		(void)snprintf(text, size, "Home");
		break;
	case 0xff57U:
		(void)snprintf(text, size, "End");
		break;
	case 0xff55U:
		(void)snprintf(text, size, "PgUp");
		break;
	case 0xff56U:
		(void)snprintf(text, size, "PgDn");
		break;
	case 0xff51U:
		(void)snprintf(text, size, "Left");
		break;
	case 0xff52U:
		(void)snprintf(text, size, "Up");
		break;
	case 0xff53U:
		(void)snprintf(text, size, "Right");
		break;
	case 0xff54U:
		(void)snprintf(text, size, "Down");
		break;
	default:
		/* A key without a name here. */
		(void)snprintf(text, size, "?");
		break;
	}
}

/* Draws one popup: its shadow, its glass and its rows. */
static void
shell_draw_popup(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_menu_model *model,
	unsigned level)
{
	static const float line[4] = { 0.12f, 0.16f, 0.24f, 0.16f };
	const struct kwl_menu_item *rows[SHELL_ROWS];
	const struct shell_popup *popup;
	struct glass_shape shape;
	unsigned count;
	unsigned index;
	int32_t top;

	/* The shadow under it. */
	popup = &shell_menu.popups[level];
	glass_shape_init(&shape, (float)popup->x, (float)popup->y + 6.0f, (float)popup->width, (float)popup->height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = POPUP_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, whiter than a title bar so the rows read well over anything. */
	glass_shape_init(&shape, (float)popup->x, (float)popup->y, (float)popup->width, (float)popup->height);
	shape.mode = MODE_GLASS;
	shape.radius = POPUP_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* The rows from the top: a separator is a thin line. */
	count = shell_rows(model, popup, rows);
	top = popup->y + POPUP_PADDING;
	for (index = 0; index < count; index++) {
		/* A separator is a thin line across the popup. */
		if (rows[index]->type == KWL_MENU_SEPARATOR) {
			glass_draw_solid(server, command, (float)(popup->x + 12), (float)(top + SEPARATOR_HEIGHT / 2), (float)(popup->width - 24), 1.0f, 0.0f, line);
			top += SEPARATOR_HEIGHT;
			continue;
		}

		/* Any other row. */
		shell_draw_row(server, command, popup, rows[index], top);
		top += ROW_HEIGHT;
	}
}

/*
 * Draws one row: the selection's blue band, the check mark or radio dot in
 * the gutter, the label, the shortcut at the right and a submenu's arrow.
 */
static void
shell_draw_row(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct shell_popup *popup,
	const struct kwl_menu_item *row,
	int32_t row_y)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	char shortcut[48];
	float blue[4];
	float ink[4];
	float hint[4];
	unsigned kept;
	int32_t middle;
	int32_t baseline;
	int32_t right;
	int32_t width;
	int present;

	/* The row's middle and the text's baseline. */
	middle = row_y + ROW_HEIGHT / 2;
	baseline = middle + 5;
	right = popup->x + popup->width - 14;

	/*
	 * The selected row is a band of the accent the user chose with its ink,
	 * the row drawn as it is (ws179-p001); a disabled row is pale.
	 */
	memcpy(ink, dark, sizeof(ink));
	memcpy(hint, soft, sizeof(hint));
	kept = server->keep_colours;
	if (row->id == popup->selected) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, blue);
		kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, ink);
		kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 0.85f, hint);
		kept = kwl_accent_as_is(server);
		glass_draw_solid(server, command, (float)(popup->x + 5), (float)(row_y + 1), (float)(popup->width - 10), (float)(ROW_HEIGHT - 2), 6.0f, blue);
	} else if (!row->enabled) {
		ink[3] = 0.35f;
		hint[3] = 0.35f;
	}

	/* A checked checkbox has a check mark in the gutter (a small square without the glyph). */
	if (row->type == KWL_MENU_CHECKBOX && row->checked) {
		present = glass_glyph_advance(server, SIZE_BAR, GLASS_CHECK_GLYPH);
		if (present > 0)
			glass_draw_glyph(server, command, SIZE_BAR, GLASS_CHECK_GLYPH, popup->x + 12, baseline, ink);
		else
			glass_draw_solid(server, command, (float)(popup->x + 13), (float)(middle - 4), 8.0f, 8.0f, 2.0f, ink);
	}

	/* A checked radio item has a dot. */
	if (row->type == KWL_MENU_RADIO && row->checked)
		glass_draw_solid(server, command, (float)(popup->x + 13), (float)(middle - 4), 8.0f, 8.0f, 4.0f, ink);

	/* The label after the gutter. */
	glass_draw_text(server, command, SIZE_BAR, popup->x + POPUP_GUTTER, baseline, row->label, popup->width - POPUP_GUTTER - 14, ink);

	/* A submenu's arrow at the right edge ("›", or ">" without the glyph). */
	if (row->type == KWL_MENU_SUBMENU) {
		present = glass_glyph_advance(server, SIZE_BAR, GLASS_ARROW_GLYPH);
		if (present > 0)
			glass_draw_glyph(server, command, SIZE_BAR, GLASS_ARROW_GLYPH, right - present, baseline, hint);
		else
			glass_draw_text(server, command, SIZE_BAR, right - 8, baseline, ">", 16, hint);

		/* The shortcut goes before the arrow. */
		right -= 16;
	}

	/* The shortcut, right-aligned before the arrow's place. */
	shell_shortcut_text(row, shortcut, sizeof(shortcut));
	if (shortcut[0] != '\0') {
		width = glass_text_width(server, SIZE_BAR, shortcut);
		glass_draw_text(server, command, SIZE_BAR, right - width, baseline, shortcut, width + 8, hint);
	}

	/* The colours' mapping as it was before the row. */
	kwl_accent_done(server, kept);
}

/* Finds the model of a window's open menu: its own, or the empty one while "..." holds only hidden controls. */
static const struct kwl_menu_model *
shell_model(
	struct kwl_object *surface,
	struct kwl_object **place)
{
	const struct kwl_menu_model *model;
	const struct kwl_object *menu;

	/* An open context menu's model, with no place (its choice goes to the context menu). */
	if (surface != NULL &&
	    surface == shell_menu.surface &&
	    shell_menu.context != NULL) {
		*place = NULL;
		menu = shell_menu.context->shown_menu;
		if (menu == NULL || menu->dead)
			return NULL;

		/* Succeeded: the context menu's model. */
		return menu->menu_model;
	}

	/* The window's menu. */
	model = kwl_menu_of_surface(surface, place);
	if (model != NULL)
		return model;

	/* A "..." open with hidden controls and no menu reads the empty model. */
	if (surface != NULL &&
	    surface == shell_menu.surface &&
	    shell_menu.open_extra_count > 0U)
		return &shell_empty_model;

	/* The window has no menu. */
	return NULL;
}

/* Finds a row by its ID: a hidden control's row of the open "...", or an item of the model. */
static const struct kwl_menu_item *
shell_item(
	const struct kwl_menu_model *model,
	uint32_t id)
{
	const struct kwl_menu_item *item;
	unsigned index;

	/* The open "..."'s rows of the hidden controls. */
	for (index = 0; index < shell_menu.open_extra_count; index++) {
		if (shell_menu.open_extras[index].id == id)
			return &shell_menu.open_extras[index];
	}

	/* The model's items. */
	item = kwl_menu_item(model, id);
	return item;
}

/* Finds where a window's menu events go: its menu's place, or none while a context menu is open. */
static struct kwl_object *
shell_place(
	struct kwl_object *surface)
{
	struct kwl_object *place;

	/* A context menu has no place. */
	if (shell_menu.context != NULL)
		return NULL;

	/* The window's menu's place. */
	(void)kwl_menu_of_surface(surface, &place);
	return place;
}
