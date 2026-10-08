/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arrangement menu and the arrangement mode of the glass look (WS181,
 * the 2026-10-07 UAT).
 *
 * A tap or a click anywhere on the system bar's desktops' pill opens the
 * menu under it (the 2026-10-07 user decision D5): the seven layouts'
 * drawings, without words, two to a row in a large menu (the 2026-10-07
 * UAT, ws181-p006: an important choice should look it): side by side and
 * stacked; one on the left and one on the right; one on the top and one on
 * the bottom; the grid alone on the last row.  The menu acts on the desktop
 * shown; a desktop is switched by the swipe and the keys, not from the
 * pill.  Choosing a layout arranges the windows of the desktop shown --
 * up to the layout's limit, top first, the rest staying where they are --
 * each into the slot nearest to where it was (arrange.c), floating (the
 * docked mode ends quietly first), and they glide there.
 *
 * The desktop is then in the arrangement mode: a drag of an arranged
 * window's title swaps it with the window of the slot it is let go on (a
 * drag into the system bar docks it); a double click on a title docks it.
 * Docking a window, closing or minimizing an arranged one, a new window on
 * that desktop, or an arranged window resized or gone fullscreen ends the
 * mode, and the windows are plain floating windows where they are -- there
 * is nothing to undo (the user's "no steps to learn to leave it").
 *
 * The state is this file's: the menu, each desktop's arrangement and the
 * swap being dragged.  The windows' places are set through shell.c's
 * kwl_glass_* functions.
 */

#include "glass.h"
#include "arrange.h"
#include "desktop.h"
#include "extras.h"
#include "menu.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* How long a window glides to its slot, in milliseconds. */
#define ARRANGE_MS		180U

/*
 * How long past its time a movement still asks for frames (ms): enough
 * for the frame that draws its end, which a draw notes (settled, faded,
 * glide-end), and no more, so that a movement whose end is never drawn (a
 * window on a desktop not shown, under App Home) does not keep every frame
 * drawn (ws181-p009).
 */
#define ARRANGE_MOVING_GRACE_MS	100U

/*
 * How long a window asked for into an arrangement (the clock's Calendar,
 * ws181-p009) may take to map and still join it (ms): a program's start,
 * with room for a slow machine.
 */
#define ARRANGE_JOIN_MS		10000U

/*
 * The menu's cells (one a layout) in two columns and four rows, the gap
 * between them, its padding, its corner, and its gap under the bar: twice
 * as wide and five times as tall as the row of ws181-p005.
 */
#define ARRANGE_MENU_COLUMNS		2
#define ARRANGE_MENU_ROWS		4
#define ARRANGE_MENU_CELL_WIDTH		258
#define ARRANGE_MENU_CELL_HEIGHT	56
#define ARRANGE_MENU_CELL_GAP		8
#define ARRANGE_MENU_PAD		6
#define ARRANGE_MENU_RADIUS		18.0f
#define ARRANGE_MENU_GAP		6
#define ARRANGE_MENU_WIDTH		(2 * ARRANGE_MENU_PAD + ARRANGE_MENU_COLUMNS * ARRANGE_MENU_CELL_WIDTH + (ARRANGE_MENU_COLUMNS - 1) * ARRANGE_MENU_CELL_GAP)
#define ARRANGE_MENU_HEIGHT		(2 * ARRANGE_MENU_PAD + ARRANGE_MENU_ROWS * ARRANGE_MENU_CELL_HEIGHT + (ARRANGE_MENU_ROWS - 1) * ARRANGE_MENU_CELL_GAP)

/*
 * How the menu opens and closes (ws181-p007, the 2026-10-07 UAT): it grows
 * from the pill's size at the pill to its own over this many milliseconds,
 * eased out, its glass going from dense white to the windows' panels'
 * frosted glass (panels.c) as it grows, and fades out over this many; the
 * least scale it grows from, its opacity at the start, the white at the
 * start and at the end, the glass's rim, and the scale it fades out to.
 */
#define ARRANGE_MENU_OPEN_MS		180U
#define ARRANGE_MENU_CLOSE_MS		120U
#define ARRANGE_MENU_LEAST_SCALE	0.15f
#define ARRANGE_MENU_FIRST_OPACITY	0.5f
#define ARRANGE_MENU_DENSE		0.92f
#define ARRANGE_MENU_WHITE		0.34f
#define ARRANGE_MENU_RIM		0.70f
#define ARRANGE_MENU_FADED_SCALE	0.96f

/* The drawing of a layout in its cell (its slots scaled down from a five times larger area). */
#define ARRANGE_ICON_WIDTH	60
#define ARRANGE_ICON_HEIGHT	40
#define ARRANGE_ICON_SCALE	5

/* The keys the open menu takes. */
#define ARRANGE_KEY_ESC		1U
#define ARRANGE_KEY_ENTER	28U
#define ARRANGE_KEY_KP_ENTER	96U
#define ARRANGE_KEY_UP		103U
#define ARRANGE_KEY_LEFT	105U
#define ARRANGE_KEY_RIGHT	106U
#define ARRANGE_KEY_DOWN	108U

/* The menu's items: the layouts, in their order. */
#define ARRANGE_ITEMS		KWL_ARRANGE_LAYOUTS
#define ARRANGE_ITEM_NONE	(-1)

/*
 * One slot of an arrangement: its rectangle (the window's whole frame), the
 * window in it (NULL once it went), and the glide of the window into it
 * (the body's rectangle it started from and goes to, and when it started).
 */
struct arrange_slot {
	struct kwl_arrange_rect slot;
	struct kwl_object *window;
	int32_t from[4];
	int32_t to[4];
	uint64_t glide_ms;
};

/*
 * A desktop's arrangement: whether it is in the arrangement mode, its
 * layout, its slots, a window of it that went (kwl_arrange_forget, ended at
 * the next tick), and the newest window there when it was arranged (a
 * window mapped later ends the mode).
 */
struct arrange_desktop {
	unsigned on;
	unsigned layout;
	unsigned count;
	unsigned gone;
	uint64_t map_order;
	struct arrange_slot slots[KWL_ARRANGE_MAX];
};

/*
 * The menu: whether it is open, on which output (the one whose bar's
 * desktops' pill opened it, the system bar's or a head's, ws113-p015) and
 * where, the item a press is on (its release acts on it), the item the
 * keyboard selected, and whether a press on a pill waits for its release
 * to open the menu, and on which output's pill.  Its drawing (ws181-p007):
 * the pill's middle, its bar's top and its width it grows from, when it
 * opened (kwl_milliseconds' clock), whether its growing is over, when it
 * closed (0 once its fade is over or before it ever closed), and how far
 * it had grown then.
 */
struct arrange_menu {
	unsigned open;
	unsigned output;
	int32_t x;
	int32_t y;
	int32_t height;
	int pressed;
	int selected;
	unsigned pill_pressed;
	unsigned pill_output;
	unsigned swallow;
	int32_t pill_middle;
	int32_t pill_top;
	int32_t pill_width;
	uint64_t opened_ms;
	unsigned settled;
	uint64_t closed_ms;
	float closed_grown;
};

/*
 * How the menu is drawn this frame (ws181-p007): where its top left corner
 * is, its scale about it, the white of its glass, and its opacity.
 */
struct arrange_view {
	float x;
	float y;
	float scale;
	float white;
	float opacity;
};

/*
 * The swap being dragged: the arranged window following the pointer, its
 * slot, its desktop and output, and where the pointer holds it.
 */
struct arrange_swap {
	struct kwl_object *window;
	unsigned slot;
	unsigned desktop;
	unsigned output;
	int32_t dx;
	int32_t dy;
};

/*
 * A window asked for into the arrangement of the desktop shown (the
 * system bar's clock opening Calendar there, ws181-p009, the 2026-10-07
 * UAT): the desktop, its layout, and when it was asked.  The first window
 * mapped on that desktop within ARRANGE_JOIN_MS joins the arrangement
 * instead of ending it.
 */
struct arrange_join {
	unsigned on;
	unsigned desktop;
	unsigned output;
	unsigned layout;
	uint64_t asked_ms;
};

/*
 * What a desktop's arrangement was when its mode ended (ws177-p037, the
 * 2026-10-08 user decision (b)): whether there is one, its layout, how
 * many slots, and the window that was in each.
 */
struct arrange_memory {
	unsigned valid;
	unsigned layout;
	unsigned count;
	struct kwl_object *windows[KWL_ARRANGE_MAX];
};

/*
 * Each desktop's arrangement on each output (plane.h's slot, ws113-p015:
 * a display arranges its own windows), for the session.  A desktop out of
 * the arrangement mode has on 0; its slots are forgotten then.
 */
static struct arrange_desktop arrange_desktops[KWL_APPS_DESKTOPS][KWL_PLANE_SLOTS];

/* The menu, closed at the start; only one is open at a time. */
static struct arrange_menu arrange_menu;

/* The swap being dragged; window is NULL when none is. */
static struct arrange_swap arrange_swap;

/*
 * Each desktop's last arrangement on each output once its mode ended
 * (ws177-p037), for the session: choosing that layout there again puts the
 * same windows in the same slots.  A destroyed window's place is NULL
 * (kwl_arrange_forget), which no choice matches again.
 */
static struct arrange_memory arrange_memories[KWL_APPS_DESKTOPS][KWL_PLANE_SLOTS];

/*
 * The arrow whose press a keyboard swap took (ws177-p037), so its release
 * is taken too; 0 when none was.
 */
static uint32_t arrange_key_eaten;

/*
 * The window asked for into an arrangement; on is 0 when none is waited
 * for (set by kwl_arrange_join_prepare, used once by kwl_arrange_join_opened
 * or kwl_arrange_mapped).
 */
static struct arrange_join arrange_join;

static void arrange_menu_open(struct kwl_server *server, unsigned slot);
static void arrange_menu_close(struct kwl_server *server, const char *via);
static int arrange_menu_item_at(struct kwl_server *server, int32_t x, int32_t y);
static void arrange_menu_item_rect(int item, int32_t *x, int32_t *y, int32_t *width, int32_t *height);
static void arrange_menu_act(struct kwl_server *server, int item);
static void arrange_apply(struct kwl_server *server, unsigned output, unsigned layout);
static unsigned arrange_targets(struct kwl_server *server, unsigned output, struct kwl_object **windows, unsigned limit);
static unsigned arrange_output(const struct kwl_object *surface);
static int arrange_body(const struct kwl_object *surface, const struct kwl_arrange_rect *slot, int32_t body[4]);
static int arrange_misfit(struct kwl_object **windows, unsigned count, const struct kwl_arrange_rect *slots, const unsigned *order);
static int arrange_recall(unsigned desktop, unsigned output, unsigned layout, struct kwl_object **windows, unsigned count, unsigned *order);
static int arrange_neighbour(const struct arrange_desktop *arranged, unsigned from, uint32_t key);
static const char *arrange_key_name(uint32_t key);
static void arrange_glide_to(struct kwl_server *server, struct arrange_slot *slot, unsigned index);
static void arrange_end(struct kwl_server *server, unsigned desktop, unsigned output, const char *reason);
static int arrange_slot_of(unsigned desktop, unsigned output, const struct kwl_object *surface);
static int arrange_menu_view(struct kwl_server *server, struct arrange_view *view);
static float arrange_menu_grown(struct kwl_server *server);
static void arrange_menu_grow(struct kwl_server *server, float grown, struct arrange_view *view);
static void arrange_draw_icon(struct kwl_server *server, VkCommandBuffer command, unsigned layout, float x, float y, float scale, const float *ink, float alpha);
static void arrange_swap_end(struct kwl_server *server);
static int arrange_moving(void);

/*
 * Handles a pointer button for the arrangement menu: a release of a press
 * on a desktops' pill (the system bar's or a head's, ws113-p015) opens the
 * menu for that pill's output or closes it (the menu opens on the release,
 * as from the top band's tap); while it is open, a release on the item the
 * press was on acts on it, and a press outside closes it.  Returns 1 when
 * the button is the menu's.
 */
int
kwl_arrange_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int32_t pill_x;
	int32_t pill_top;
	int32_t pill_width;
	int on_pill;
	int item;

	/* Whether the pointer is on the desktops' pill of the output it is on (shell.c). */
	kwl_glass_desktops_pill(server, server->pointer_output, &pill_x, &pill_top, &pill_width);
	on_pill = 0;
	if (server->pointer_y >= pill_top &&
	    server->pointer_y < pill_top + KWL_GLASS_BAR &&
	    server->pointer_x >= pill_x &&
	    server->pointer_x < pill_x + pill_width)
		on_pill = 1;

	/* The release of a press on a pill toggles the menu, opened for that pill's output. */
	if (state == 0 && arrange_menu.pill_pressed) {
		arrange_menu.pill_pressed = 0;
		if (arrange_menu.open) {
			arrange_menu_close(server, "pill");
		} else {
			arrange_menu_open(server, arrange_menu.pill_output);
		}
		return 1;
	}

	/* The release of a press that closed the menu goes no further. */
	if (state == 0 && arrange_menu.swallow) {
		arrange_menu.swallow = 0;
		return 1;
	}

	/* With the menu closed, only a left press on the pill, held until its release. */
	if (!arrange_menu.open) {
		if (state == 0 ||
		    button != KWL_BUTTON_LEFT ||
		    !on_pill)
			return 0;
		arrange_menu.pill_pressed = 1;
		arrange_menu.pill_output = server->pointer_output;
		return 1;
	}

	/* While it is open, a release acts on the item its press was on, when it is still there. */
	if (state == 0) {
		item = arrange_menu_item_at(server, server->pointer_x, server->pointer_y);
		if (arrange_menu.pressed != ARRANGE_ITEM_NONE && item == arrange_menu.pressed)
			arrange_menu_act(server, item);
		arrange_menu.pressed = ARRANGE_ITEM_NONE;
		return 1;
	}

	/* A press on a pill closes it at its release. */
	if (on_pill) {
		arrange_menu.pill_pressed = 1;
		arrange_menu.pill_output = server->pointer_output;
		return 1;
	}

	/* A press outside the menu closes it and goes no further. */
	if (server->pointer_x < arrange_menu.x ||
	    server->pointer_x >= arrange_menu.x + ARRANGE_MENU_WIDTH ||
	    server->pointer_y < arrange_menu.y ||
	    server->pointer_y >= arrange_menu.y + arrange_menu.height) {
		arrange_menu_close(server, "outside");
		arrange_menu.swallow = 1;
		return 1;
	}

	/* A left press on an item waits for its release. */
	arrange_menu.pressed = ARRANGE_ITEM_NONE;
	if (button == KWL_BUTTON_LEFT)
		arrange_menu.pressed = arrange_menu_item_at(server, server->pointer_x, server->pointer_y);

	/* Succeeded: the press was the menu's. */
	return 1;
}

/*
 * Handles a key while the menu is open: Left and Right move the selection
 * along the cells, Up and Down a row, Enter acts on it, Esc closes the
 * menu.  Returns 1 when the key is the
 * menu's (every key while it is open).
 */
int
kwl_arrange_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* A closed menu takes no key. */
	if (!arrange_menu.open)
		return 0;

	/* Only presses act; releases are the menu's too. */
	if (state == 0)
		return 1;

	/* Each key's work. */
	switch (key) {
	case ARRANGE_KEY_ESC:
		arrange_menu_close(server, "key");
		break;
	case ARRANGE_KEY_LEFT:
		arrange_menu.selected--;
		if (arrange_menu.selected < 0)
			arrange_menu.selected = (int)ARRANGE_ITEMS - 1;
		server->dirty = 1;
		break;
	case ARRANGE_KEY_RIGHT:
		arrange_menu.selected++;
		if (arrange_menu.selected >= (int)ARRANGE_ITEMS)
			arrange_menu.selected = 0;
		server->dirty = 1;
		break;
	case ARRANGE_KEY_UP:
		/* A row up; the first row stays. */
		if (arrange_menu.selected >= ARRANGE_MENU_COLUMNS)
			arrange_menu.selected -= ARRANGE_MENU_COLUMNS;
		server->dirty = 1;
		break;
	case ARRANGE_KEY_DOWN:
		/* A row down; the last row (the grid alone) takes either cell above it. */
		arrange_menu.selected += ARRANGE_MENU_COLUMNS;
		if (arrange_menu.selected >= (int)ARRANGE_ITEMS)
			arrange_menu.selected = (int)ARRANGE_ITEMS - 1;
		server->dirty = 1;
		break;
	case ARRANGE_KEY_ENTER:
	case ARRANGE_KEY_KP_ENTER:
		arrange_menu_act(server, arrange_menu.selected);
		break;
	default:
		break;
	}

	/* Succeeded: the key was the menu's. */
	return 1;
}

/*
 * Follows the pointer for the menu (the item under it is lit) and for a
 * swap being dragged (the window follows the pointer).  Returns 1 when the
 * motion is the arrangement's.
 */
int
kwl_arrange_motion(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	struct kwl_object *surface;
	int32_t lowest;
	int over;

	/*
	 * An open menu lights the item under the pointer, and the selection
	 * follows it: between two cells the item last pointed at stays lit,
	 * not the selection the menu opened with (ws181-p009, the first item
	 * flashed as the pointer crossed a gap).  Only a change is drawn.
	 */
	if (arrange_menu.open) {
		over = arrange_menu_item_at(server, server->pointer_x, server->pointer_y);
		if (over != ARRANGE_ITEM_NONE && over != arrange_menu.selected) {
			arrange_menu.selected = over;
			server->dirty = 1;
			printf("KWL ARRANGE menu lit item=%s\n", kwl_arrange_name((unsigned)over));
		}

		/* The motion was the menu's. */
		return 1;
	}

	/* Only a swap being dragged follows the pointer. */
	surface = arrange_swap.window;
	if (surface == NULL)
		return 0;

	/* The window follows the pointer, its title below its output's bar. */
	(void)kwl_output_rect(server, arrange_swap.output, &output);
	lowest = output.y + kwl_output_top(server, arrange_swap.output);
	surface->x = server->pointer_x - arrange_swap.dx;
	surface->y = server->pointer_y - arrange_swap.dy;
	if (surface->y < lowest)
		surface->y = lowest;
	server->dirty = 1;

	/* Succeeded: the motion is the swap's. */
	return 1;
}

/*
 * Starts a move of a window from one of its four ways (a press on its
 * title, a press on its title's menu or field that moved, the client's
 * own move request, a finger on its title): an arranged window's move is a
 * swap instead, which follows the pointer until it is let go.  Returns 1
 * when the swap started (the caller starts no move).
 */
int
kwl_arrange_move_start(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t x,
	int32_t y)
{
	unsigned output;
	int slot;

	UNUSED_PARAMETER(server);

	/* Only an arranged window of the desktop shown, and no swap already. */
	if (arrange_swap.window != NULL)
		return 0;
	output = arrange_output(surface);
	if (surface->desktop >= KWL_APPS_DESKTOPS || !arrange_desktops[surface->desktop][output].on)
		return 0;
	slot = arrange_slot_of(surface->desktop, output, surface);
	if (slot < 0)
		return 0;

	/* The swap holds the window where it was pressed. */
	arrange_swap.window = surface;
	arrange_swap.slot = (unsigned)slot;
	arrange_swap.desktop = surface->desktop;
	arrange_swap.output = output;
	arrange_swap.dx = x - surface->x;
	arrange_swap.dy = y - surface->y;
	arrange_desktops[surface->desktop][output].slots[slot].glide_ms = 0U;
	printf("KWL ARRANGE swap-start surface=%u slot=%d\n", surface->id, slot);

	/* Succeeded: the move is a swap. */
	return 1;
}

/*
 * Ends a swap at the button's release: let go on another slot, the two
 * windows trade slots; let go in the system bar, the window docks (which
 * ends the arrangement mode); anywhere else, it glides back to its slot.
 * Returns 1 when a swap ended (the release was the arrangement's).
 */
int
kwl_arrange_move_end(
	struct kwl_server *server)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct arrange_desktop *arranged;
	struct kwl_object *surface;
	struct kwl_object *other;
	unsigned index;
	unsigned target;
	unsigned count;
	int found;

	/* Only a swap being dragged. */
	surface = arrange_swap.window;
	if (surface == NULL)
		return 0;
	arrange_swap.window = NULL;
	arranged = &arrange_desktops[arrange_swap.desktop][arrange_swap.output];

	/* Let go in the bar of its output (a head's own, ws113-p015): docked, which ends the mode (kwl_arrange_end_all). */
	count = kwl_outputs(server, outputs);
	if (arrange_swap.output < count &&
	    server->pointer_output == arrange_swap.output &&
	    server->pointer_y < outputs[arrange_swap.output].y + KWL_GLASS_BAR) {
		kwl_glass_dock_window(server, surface, "arrange-drag");
		return 1;
	}

	/* The slot the pointer is in, other than the window's own. */
	found = 0;
	target = 0U;
	for (index = 0U; index < arranged->count; index++) {
		if (index == arrange_swap.slot)
			continue;
		if (server->pointer_x >= arranged->slots[index].slot.x &&
		    server->pointer_x < arranged->slots[index].slot.x + arranged->slots[index].slot.width &&
		    server->pointer_y >= arranged->slots[index].slot.y &&
		    server->pointer_y < arranged->slots[index].slot.y + arranged->slots[index].slot.height) {
			found = 1;
			target = index;
			break;
		}
	}

	/* Nowhere else: back to its own slot. */
	if (!found) {
		arrange_glide_to(server, &arranged->slots[arrange_swap.slot], arrange_swap.slot);
		printf("KWL ARRANGE swap-back surface=%u slot=%u\n", surface->id, arrange_swap.slot);
		return 1;
	}

	/* The two windows trade slots and glide there. */
	other = arranged->slots[target].window;
	arranged->slots[target].window = surface;
	arranged->slots[arrange_swap.slot].window = other;
	arrange_glide_to(server, &arranged->slots[target], target);
	if (other != NULL)
		arrange_glide_to(server, &arranged->slots[arrange_swap.slot], arrange_swap.slot);
	printf("KWL ARRANGE swap a=%u b=%u slots=%u,%u\n", surface->id, other != NULL ? other->id : 0U, arrange_swap.slot, target);

	/* Succeeded: the swap is done. */
	return 1;
}

/*
 * Gives the body of an arranged window gliding to its slot, while it
 * glides.  Returns 1 and sets body (x, y, width, height) when it does.
 */
int
kwl_arrange_glide(
	struct kwl_server *server,
	const struct kwl_object *surface,
	int32_t body[4])
{
	struct arrange_slot *slot;
	uint64_t elapsed;
	unsigned output;
	unsigned part;
	float t;
	int index;

	/* Only an arranged window. */
	output = arrange_output(surface);
	if (surface->desktop >= KWL_APPS_DESKTOPS || !arrange_desktops[surface->desktop][output].on)
		return 0;
	index = arrange_slot_of(surface->desktop, output, surface);
	if (index < 0)
		return 0;
	slot = &arrange_desktops[surface->desktop][output].slots[index];

	/* Only while it glides. */
	if (slot->glide_ms == 0U)
		return 0;

	/* A glide that has run its time is over: the window is drawn in its slot from now on (the tests wait for this line). */
	elapsed = kwl_milliseconds() - slot->glide_ms;
	if (elapsed >= ARRANGE_MS) {
		slot->glide_ms = 0U;
		printf("KWL ARRANGE glide-end surface=%u slot=%d\n", surface->id, index);
		return 0;
	}

	/* Eased out (1 - (1 - t)^3) from where it was to its slot. */
	t = (float)elapsed / (float)ARRANGE_MS;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	for (part = 0U; part < 4U; part++)
		body[part] = slot->from[part] + (int32_t)((float)(slot->to[part] - slot->from[part]) * t);
	server->dirty = 1;

	/* Succeeded: the window glides. */
	return 1;
}

/*
 * Follows the arrangements every frame: a desktop whose arranged window
 * went (destroyed, unmapped, minimized, fullscreen, on another desktop, or
 * resized by its frame) leaves the arrangement mode, its windows floating
 * where they are.
 */
void
kwl_arrange_tick(
	struct kwl_server *server)
{
	struct arrange_desktop *arranged;
	struct kwl_object *surface;
	int32_t body[4];
	unsigned desktop;
	unsigned output;
	unsigned index;
	const char *reason;
	int moving;

	/*
	 * The menu growing or fading and the windows gliding draw every frame
	 * until they are over.  The tick asks for the frames: one asked for
	 * while a frame is drawn is dropped when it is submitted (compose.c),
	 * and only input asked for the next one (BUG-246, the 5320's i915).
	 */
	moving = arrange_moving();
	if (moving)
		server->dirty = 1;

	/* Each desktop in the arrangement mode, on each output. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		for (output = 0U; output < KWL_PLANE_SLOTS; output++) {
			arranged = &arrange_desktops[desktop][output];
			if (!arranged->on)
				continue;

			/* A window that went ends it. */
			reason = NULL;
			if (arranged->gone)
				reason = "closed";

			/* Each arranged window, as it is now. */
			for (index = 0U; index < arranged->count; index++) {
				/* The first reason found is enough. */
				surface = arranged->slots[index].window;
				if (reason != NULL)
					break;
				if (surface == NULL)
					continue;
				if (surface->dead || !surface->mapped) {
					reason = "closed";
				} else if (surface->minimized) {
					reason = "minimized";
				} else if (surface->fullscreen) {
					reason = "fullscreen";
				} else if (surface->maximized) {
					reason = "dock";
				} else if (surface->desktop != desktop || surface->output != output) {
					reason = "moved";
				} else if (arrange_swap.window != surface && arranged->slots[index].glide_ms == 0U) {
					/* Not swapped nor gliding: its size is its slot's unless its frame resized it. */
					(void)arrange_body(surface, &arranged->slots[index].slot, body);
					if ((int32_t)surface->window_width != body[2] || (int32_t)surface->window_height != body[3])
						reason = "resized";
				}
			}

			/* The mode ends for the first reason found. */
			if (reason != NULL)
				arrange_end(server, desktop, output, reason);
		}
	}
}

/*
 * Forgets a destroyed window (objects.c through kwl_glass_forget): its slot
 * is emptied and its desktop's arrangement ends at the next tick; a swap
 * of it ends.
 */
void
kwl_arrange_forget(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	unsigned desktop;
	unsigned output;
	unsigned place;
	int index;

	UNUSED_PARAMETER(server);

	/* A swap of it is over. */
	if (arrange_swap.window == surface)
		arrange_swap.window = NULL;

	/* Its slot, on whichever desktop and output, and its place in what is remembered (no choice matches that again). */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		for (output = 0U; output < KWL_PLANE_SLOTS; output++) {
			for (place = 0U; place < arrange_memories[desktop][output].count; place++) {
				if (arrange_memories[desktop][output].windows[place] == surface)
					arrange_memories[desktop][output].windows[place] = NULL;
			}

			/* Its slot in an arrangement on now. */
			index = arrange_slot_of(desktop, output, surface);
			if (index < 0)
				continue;
			arrange_desktops[desktop][output].slots[index].window = NULL;
			arrange_desktops[desktop][output].gone = 1U;
		}
	}
}

/*
 * Ends the arrangement mode of every desktop of an output (its docked mode
 * starting: shell.c's layout_set, WS181 I3; each output its own,
 * ws113-p015), the menu and a swap there with it.
 */
void
kwl_arrange_end_all(
	struct kwl_server *server,
	unsigned output,
	const char *reason)
{
	unsigned desktop;

	/* The menu and a swap of the output. */
	if (arrange_swap.output == output)
		arrange_swap.window = NULL;
	if (arrange_menu.open && arrange_menu.output == output)
		arrange_menu_close(server, reason);

	/* Every arranged desktop of the output. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS && output < KWL_PLANE_SLOTS; desktop++) {
		if (arrange_desktops[desktop][output].on)
			arrange_end(server, desktop, output, reason);
	}
}

/*
 * Ends the arrangement mode of a desktop where a new window was mapped (a
 * window without a parent, newer than the arrangement; WS181 S6).
 */
void
kwl_arrange_mapped(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct arrange_desktop *arranged;
	unsigned output;
	uint64_t waited;

	/* Only a window without a parent on an arranged desktop of its output, newer than its arrangement. */
	if (surface->parent_window != NULL || surface->desktop >= KWL_APPS_DESKTOPS)
		return;
	output = arrange_output(surface);
	arranged = &arrange_desktops[surface->desktop][output];
	if (!arranged->on || surface->map_order <= arranged->map_order)
		return;

	/*
	 * The window asked for into this arrangement, on the desktop shown,
	 * in time: the layout arranges it with the others (ws181-p009).
	 */
	waited = kwl_milliseconds() - arrange_join.asked_ms;
	if (arrange_join.on &&
	    arrange_join.desktop == surface->desktop &&
	    arrange_join.output == output &&
	    surface->desktop == server->desktop &&
	    waited < ARRANGE_JOIN_MS) {
		arrange_join.on = 0U;
		printf("KWL ARRANGE join surface=%u via=mapped\n", surface->id);
		arrange_apply(server, output, arranged->layout);
		return;
	}

	/* The mode ends; the new window is placed as a new window is. */
	arrange_end(server, surface->desktop, output, "new");
}

/*
 * Notes that a window is about to be asked for into the arrangement of the
 * desktop shown (the system bar's clock opening Calendar, ws181-p009): its
 * window joins the arrangement when it maps, or when it runs already, at
 * kwl_arrange_join_opened.  A desktop out of the arrangement mode waits
 * for nothing.
 */
void
kwl_arrange_join_prepare(
	struct kwl_server *server)
{
	struct arrange_desktop *arranged;

	/* Nothing is waited for unless the desktop shown is arranged on the anchor (where new windows open). */
	arrange_join.on = 0U;
	if (server->desktop >= KWL_APPS_DESKTOPS)
		return;
	arranged = &arrange_desktops[server->desktop][KWL_PLANE_ANCHOR];
	if (!arranged->on)
		return;

	/* The desktop, its layout, and now. */
	arrange_join.on = 1U;
	arrange_join.desktop = server->desktop;
	arrange_join.output = KWL_PLANE_ANCHOR;
	arrange_join.layout = arranged->layout;
	arrange_join.asked_ms = kwl_milliseconds();
}

/*
 * Follows the request kwl_arrange_join_prepare noted, once it was made: a
 * failed one waits for nothing; a window that ran already (brought to the
 * front on the desktop shown, which may have ended the arrangement) joins
 * the layout now; a program started is waited for (kwl_arrange_mapped).
 */
void
kwl_arrange_join_opened(
	struct kwl_server *server,
	int error,
	int running)
{
	/* Nothing waited for. */
	if (!arrange_join.on)
		return;

	/* The request failed: nothing comes. */
	if (error != 0) {
		arrange_join.on = 0U;
		return;
	}

	/* A program started: its window is waited for. */
	if (!running)
		return;

	/* The window that ran already, on the desktop shown: the layout arranges it with the others. */
	arrange_join.on = 0U;
	if (server->desktop != arrange_join.desktop)
		return;
	printf("KWL ARRANGE join via=running\n");
	arrange_apply(server, arrange_join.output, arrange_join.layout);
}

/*
 * Ends the arrangement mode of a desktop a window comes to from another
 * one, and of the desktop it leaves when it was arranged there.
 */
void
kwl_arrange_moved(
	struct kwl_server *server,
	struct kwl_object *surface,
	unsigned from)
{
	unsigned output;
	int index;

	/* The desktop it left, when it was arranged there (on its output). */
	output = arrange_output(surface);
	if (from < KWL_APPS_DESKTOPS && arrange_desktops[from][output].on) {
		index = arrange_slot_of(from, output, surface);
		if (index >= 0)
			arrange_end(server, from, output, "moved");
	}

	/* The desktop it came to. */
	if (surface->desktop < KWL_APPS_DESKTOPS && arrange_desktops[surface->desktop][output].on)
		arrange_end(server, surface->desktop, output, "new");
}

/*
 * Arranges again every arranged desktop of an output whose size changed
 * (ws177-p035, a resolution change or the output moved to another
 * display; kwl_glass_output_resized): the same layout's slots in the new
 * work area, each window keeping its slot's place in the layout, gliding
 * there.  A desktop whose window no longer fits its slot leaves the
 * arrangement mode, its windows floating where they are.
 */
void
kwl_arrange_output_resized(
	struct kwl_server *server,
	unsigned output)
{
	struct arrange_desktop *arranged;
	struct kwl_arrange_rect area;
	struct kwl_arrange_rect slots[KWL_ARRANGE_MAX];
	int32_t body[4];
	unsigned desktop;
	unsigned made;
	unsigned index;
	int fits;
	int misfit;

	/* Only an output of the plane. */
	if (output >= KWL_PLANE_SLOTS)
		return;

	/* The output's new work area. */
	kwl_glass_work_area(server, output, &area);

	/* Each arranged desktop of the output. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		arranged = &arrange_desktops[desktop][output];
		if (!arranged->on)
			continue;

		/* The layout's slots for as many windows, in the new area. */
		made = kwl_arrange_slots(arranged->layout, arranged->count, &area, slots);

		/* Whether each window still fits its slot. */
		misfit = 0;
		for (index = 0U; index < made && index < arranged->count; index++) {
			if (arranged->slots[index].window == NULL)
				continue;
			fits = arrange_body(arranged->slots[index].window, &slots[index], body);
			if (!fits)
				misfit = 1;
		}

		/* One no longer fits: the mode ends, the windows where they are. */
		if (misfit || made != arranged->count) {
			arrange_end(server, desktop, output, "output");
			continue;
		}

		/* Each window to its slot's new place, gliding there. */
		for (index = 0U; index < made; index++) {
			arranged->slots[index].slot = slots[index];
			if (arranged->slots[index].window != NULL)
				arrange_glide_to(server, &arranged->slots[index], index);
		}

		/* The log names the new area. */
		printf("KWL ARRANGE output desktop=%u layout=%s windows=%u area=%d,%d,%d,%d\n", desktop + 1U, kwl_arrange_name(arranged->layout), made, area.x, area.y, area.width, area.height);
	}

	/* A swap being dragged on the output goes back to its slot's new place. */
	if (arrange_swap.window != NULL && arrange_swap.output == output)
		kwl_arrange_swap_cancel(server, "output");
}

/*
 * Closes the open arrangement menu first when a press lands on another
 * widget of its output's bar (ws177-p036: the volume's, the network's,
 * Bluetooth's icons), so that press opens that widget's own popup, not a
 * second popup over the menu.  The press goes on (returns 0); a press on
 * the desktops' pill, in the menu or off the bar is the menu's own
 * (kwl_arrange_button).
 */
int
kwl_arrange_bar_press(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int32_t pill_x;
	int32_t pill_top;
	int32_t pill_width;

	UNUSED_PARAMETER(button);

	/* Only a press while the menu is open, on the bar of the output it opened for. */
	if (!arrange_menu.open || state == 0)
		return 0;
	if (server->pointer_output != arrange_menu.output)
		return 0;
	kwl_glass_desktops_pill(server, arrange_menu.output, &pill_x, &pill_top, &pill_width);
	if (server->pointer_y < pill_top || server->pointer_y >= pill_top + KWL_GLASS_BAR)
		return 0;

	/* Not on the pill, which closes the menu at its release. */
	if (server->pointer_x >= pill_x && server->pointer_x < pill_x + pill_width)
		return 0;

	/* Succeeded: the menu closes, and the press goes on to the widget under it. */
	arrange_menu_close(server, "bar");
	return 0;
}

/*
 * Gives up a swap being dragged (ws177-p036: the desktop shown changes,
 * the session locks, the output's size changes): the window glides back
 * to its slot, and the release later is nobody's swap.
 */
void
kwl_arrange_swap_cancel(
	struct kwl_server *server,
	const char *reason)
{
	struct arrange_desktop *arranged;
	struct kwl_object *surface;

	/* Only a swap being dragged. */
	surface = arrange_swap.window;
	if (surface == NULL)
		return;
	arrange_swap.window = NULL;

	/* Back to its own slot, when the arrangement is still on. */
	arranged = &arrange_desktops[arrange_swap.desktop][arrange_swap.output];
	if (arranged->on &&
	    arrange_swap.slot < arranged->count &&
	    arranged->slots[arrange_swap.slot].window == surface)
		arrange_glide_to(server, &arranged->slots[arrange_swap.slot], arrange_swap.slot);
	printf("KWL ARRANGE swap-cancel surface=%u slot=%u reason=%s\n", surface->id, arrange_swap.slot, reason);
}

/*
 * Swaps the focused arranged window with the one in the neighbouring slot
 * the arrow points to, from the keyboard alone (ws177-p037: Super with an
 * arrow, shell.c checks the modifiers; super is 1 when only Super is
 * held).  Both glide to their new slots.  While the desktop shown is
 * arranged and the focused window is in it, the key is the arrangement's
 * even with no slot that way; otherwise it goes on.  Returns 1 when the
 * key is the arrangement's (its release too).
 */
int
kwl_arrange_key_swap(
	struct kwl_server *server,
	struct kwl_object *surface,
	uint32_t key,
	uint32_t state,
	int super)
{
	struct arrange_desktop *arranged;
	struct kwl_object *other;
	unsigned output;
	uint32_t other_id;
	int from;
	int target;

	/* The release of the press taken goes no further. */
	if (state == 0U) {
		if (arrange_key_eaten == 0U || key != arrange_key_eaten)
			return 0;
		arrange_key_eaten = 0U;
		return 1;
	}

	/* Only an arrow with Super alone. */
	if (!super)
		return 0;
	if (key != ARRANGE_KEY_LEFT &&
	    key != ARRANGE_KEY_RIGHT &&
	    key != ARRANGE_KEY_UP &&
	    key != ARRANGE_KEY_DOWN)
		return 0;

	/* Only the focused window, arranged on the desktop shown. */
	if (surface == NULL ||
	    surface->desktop != server->desktop ||
	    surface->desktop >= KWL_APPS_DESKTOPS)
		return 0;
	output = arrange_output(surface);
	arranged = &arrange_desktops[surface->desktop][output];
	if (!arranged->on)
		return 0;
	from = arrange_slot_of(surface->desktop, output, surface);
	if (from < 0)
		return 0;
	arrange_key_eaten = key;

	/* The slot that way; none, and the key does nothing. */
	target = arrange_neighbour(arranged, (unsigned)from, key);
	if (target < 0) {
		printf("KWL ARRANGE key-swap none surface=%u slot=%d key=%s\n", surface->id, from, arrange_key_name(key));
		return 1;
	}

	/* A swap being dragged is given up first. */
	kwl_arrange_swap_cancel(server, "key");

	/* The two windows trade slots and glide there. */
	other = arranged->slots[target].window;
	arranged->slots[target].window = surface;
	arranged->slots[from].window = other;
	arrange_glide_to(server, &arranged->slots[target], (unsigned)target);
	other_id = 0U;
	if (other != NULL) {
		arrange_glide_to(server, &arranged->slots[from], (unsigned)from);
		other_id = other->id;
	}

	/* The log names both windows (b 0: the slot was empty). */
	printf("KWL ARRANGE key-swap a=%u b=%u slots=%d,%d key=%s\n", surface->id, other_id, from, target, arrange_key_name(key));

	/* Succeeded: the key swapped them. */
	return 1;
}

/*
 * Tells whether the arrangement menu shows on the output the pass draws:
 * open, or fading out after it closed (ws181-p007), on the output it
 * opened for (ws113-p015).  shell.c draws the scene under it blurred for
 * its glass while it does.
 */
int
kwl_arrange_showing(
	struct kwl_server *server)
{
	/* Only on the output it opened for. */
	if (arrange_menu.output != server->view_output)
		return 0;

	/* An open menu shows. */
	if (arrange_menu.open)
		return 1;

	/* A closed one shows while it fades. */
	if (arrange_menu.closed_ms != 0U)
		return 1;

	/* Succeeded: no menu shows. */
	return 0;
}

/*
 * Draws the arrangement menu, when it shows, under the desktops' pill: the
 * windows' panels' frosted glass (ws181-p007) with the seven layouts'
 * drawings in their cells, the one under the pointer or selected by the
 * keyboard lit in the accent.  Opening, it grows from the pill, dense white
 * becoming glass; closed, it fades.
 */
void
kwl_arrange_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	struct glass_shape shape;
	struct kwl_object *windows[KWL_ARRANGE_MAX];
	struct arrange_view view;
	float accent[4];
	float accent_ink[4];
	float colour[4];
	const float *ink;
	unsigned kept;
	unsigned item;
	unsigned count;
	int shows;
	int over;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	float left;
	float top;

	/* Only a menu that shows, on the output it opened for, where and how this frame draws it. */
	if (arrange_menu.output != server->view_output)
		return;
	shows = arrange_menu_view(server, &view);
	if (!shows)
		return;

	/* The shadow, as the power dialog's card's, scaled and faded with the menu. */
	glass_shape_init(&shape, view.x, view.y + 6.0f * view.scale, (float)ARRANGE_MENU_WIDTH * view.scale, (float)arrange_menu.height * view.scale);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = ARRANGE_MENU_RADIUS * view.scale;
	shape.soft = 18.0f * view.scale;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.20f;
	shape.opacity = view.opacity;
	glass_shape_draw(server, command, &shape);

	/* The frosted glass: the scene under it blurred (shell.c), whitened as the windows' panels. */
	glass_shape_init(&shape, view.x, view.y, (float)ARRANGE_MENU_WIDTH * view.scale, (float)arrange_menu.height * view.scale);
	shape.mode = MODE_GLASS;
	shape.radius = ARRANGE_MENU_RADIUS * view.scale;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = view.white;
	shape.edge = ARRANGE_MENU_RIM;
	shape.opacity = view.opacity;
	glass_shape_draw(server, command, &shape);

	/* The item under the pointer, or the keyboard's. */
	over = arrange_menu_item_at(server, server->pointer_x, server->pointer_y);
	if (over == ARRANGE_ITEM_NONE)
		over = arrange_menu.selected;

	/* How many windows the layouts would take (none: their drawings are faint). */
	count = arrange_targets(server, arrange_menu.output, windows, KWL_ARRANGE_MAX);

	/* The accent and its ink, for the lit item. */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, accent);
	kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, accent_ink);

	/* Each layout's cell, scaled about the menu's corner: its drawing in the middle. */
	for (item = 0U; item < ARRANGE_ITEMS; item++) {
		arrange_menu_item_rect((int)item, &x, &y, &width, &height);
		left = view.x + (float)(x - arrange_menu.x) * view.scale;
		top = view.y + (float)(y - arrange_menu.y) * view.scale;

		/* The lit cell is a rounded square of the accent with its ink, as they are. */
		ink = dark;
		kept = server->keep_colours;
		if ((int)item == over) {
			kept = kwl_accent_as_is(server);
			memcpy(colour, accent, sizeof(colour));
			colour[3] *= view.opacity;
			glass_draw_solid(server, command, left, top, (float)width * view.scale, (float)height * view.scale, 12.0f * view.scale, colour);
			ink = accent_ink;
		}

		/* The drawing, faint with no window to arrange. */
		if (count == 0U && (int)item != over)
			ink = soft;
		arrange_draw_icon(server, command, item,
				  left + (float)((width - ARRANGE_ICON_WIDTH) / 2) * view.scale,
				  top + (float)((height - ARRANGE_ICON_HEIGHT) / 2) * view.scale,
				  view.scale,
				  ink,
				  view.opacity);
		kwl_accent_done(server, kept);
	}
}

/*
 * Opens the menu for an output under its bar's desktops' pill, the
 * keyboard's selection on the first layout; the log names each item's
 * middle (the tests click them).
 */
static void
arrange_menu_open(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_plane_rect output;
	int32_t pill_x;
	int32_t pill_top;
	int32_t pill_width;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	unsigned item;

	/* Under the pill's middle, inside the output. */
	(void)kwl_output_rect(server, slot, &output);
	kwl_glass_desktops_pill(server, slot, &pill_x, &pill_top, &pill_width);
	arrange_menu.x = pill_x + pill_width / 2 - ARRANGE_MENU_WIDTH / 2;
	if (arrange_menu.x < output.x + 8)
		arrange_menu.x = output.x + 8;
	if (arrange_menu.x + ARRANGE_MENU_WIDTH > output.x + (int32_t)output.width - 8)
		arrange_menu.x = output.x + (int32_t)output.width - 8 - ARRANGE_MENU_WIDTH;
	arrange_menu.y = pill_top + KWL_GLASS_BAR + ARRANGE_MENU_GAP;
	arrange_menu.height = ARRANGE_MENU_HEIGHT;
	arrange_menu.open = 1;
	arrange_menu.output = slot;
	arrange_menu.pressed = ARRANGE_ITEM_NONE;
	arrange_menu.selected = 0;
	server->dirty = 1;

	/* It grows from the pill from now (ws181-p007); a fade of its last closing is over. */
	arrange_menu.pill_middle = pill_x + pill_width / 2;
	arrange_menu.pill_top = pill_top;
	arrange_menu.pill_width = pill_width;
	arrange_menu.opened_ms = kwl_milliseconds();
	arrange_menu.settled = 0U;
	arrange_menu.closed_ms = 0U;

	/* The log: the menu (a head's with its output), then each layout's middle. */
	if (slot == KWL_PLANE_ANCHOR) {
		printf("KWL ARRANGE menu open x=%d y=%d width=%d height=%d\n", arrange_menu.x, arrange_menu.y, ARRANGE_MENU_WIDTH, arrange_menu.height);
	} else {
		printf("KWL ARRANGE menu open x=%d y=%d width=%d height=%d output=%u\n", arrange_menu.x, arrange_menu.y, ARRANGE_MENU_WIDTH, arrange_menu.height, slot);
	}

	/* Each layout's middle. */
	for (item = 0U; item < ARRANGE_ITEMS; item++) {
		arrange_menu_item_rect((int)item, &x, &y, &width, &height);
		printf("KWL ARRANGE menu item=%s x=%d y=%d\n", kwl_arrange_name(item), x + width / 2, y + height / 2);
	}
}

/* Closes the menu. */
static void
arrange_menu_close(
	struct kwl_server *server,
	const char *via)
{
	/* It fades from as far as it had grown (ws181-p007). */
	if (arrange_menu.open) {
		arrange_menu.closed_grown = arrange_menu_grown(server);
		arrange_menu.closed_ms = kwl_milliseconds();
	}

	/* Closed, nothing pressed. */
	arrange_menu.open = 0;
	arrange_menu.pressed = ARRANGE_ITEM_NONE;
	server->dirty = 1;
	printf("KWL ARRANGE menu close via=%s\n", via);
}

/*
 * Gives how the menu is drawn this frame (ws181-p007): growing for
 * ARRANGE_MENU_OPEN_MS after it opened, then still; fading for
 * ARRANGE_MENU_CLOSE_MS after it closed, from as far as it had grown,
 * shrinking a little towards its top's middle.  A frame of either asks for
 * the next.  Returns 1 when the menu shows, 0 when it does not.
 */
static int
arrange_menu_view(
	struct kwl_server *server,
	struct arrange_view *view)
{
	uint64_t elapsed;
	float grown;
	float faded;
	float scale;
	float width;

	/* An open menu, growing or grown. */
	if (arrange_menu.open) {
		grown = arrange_menu_grown(server);
		arrange_menu_grow(server, grown, view);
		return 1;
	}

	/* A closed one that has faded, or never opened, does not show. */
	if (arrange_menu.closed_ms == 0U)
		return 0;

	/* A fade that has run its time is over. */
	elapsed = kwl_milliseconds() - arrange_menu.closed_ms;
	if (elapsed >= ARRANGE_MENU_CLOSE_MS) {
		arrange_menu.closed_ms = 0U;
		return 0;
	}

	/* Fading from as far as it had grown, a little smaller about its top's middle. */
	arrange_menu_grow(server, arrange_menu.closed_grown, view);
	faded = (float)elapsed / (float)ARRANGE_MENU_CLOSE_MS;
	scale = 1.0f - (1.0f - ARRANGE_MENU_FADED_SCALE) * faded;
	width = (float)ARRANGE_MENU_WIDTH * view->scale;
	view->x += width * (1.0f - scale) * 0.5f;
	view->scale *= scale;
	view->opacity *= 1.0f - faded;
	server->dirty = 1;

	/* Succeeded: the menu fades. */
	return 1;
}

/*
 * Gives how far the open menu has grown, eased out (1 - (1 - t)^3), from 0
 * when it opened to 1; a frame while it grows asks for the next, and the
 * first frame grown logs it (the tests take their picture after it).
 */
static float
arrange_menu_grown(
	struct kwl_server *server)
{
	uint64_t elapsed;
	float t;

	/* Grown: still. */
	if (arrange_menu.settled)
		return 1.0f;

	/* Grown now, once. */
	elapsed = kwl_milliseconds() - arrange_menu.opened_ms;
	if (elapsed >= ARRANGE_MENU_OPEN_MS) {
		arrange_menu.settled = 1U;
		printf("KWL ARRANGE menu settled ms=%llu\n", (unsigned long long)elapsed);
		return 1.0f;
	}

	/* Still growing: the next frame too. */
	server->dirty = 1;
	t = (float)elapsed / (float)ARRANGE_MENU_OPEN_MS;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

	/* Succeeded: how far it has grown. */
	return t;
}

/*
 * Gives the menu's view as far as it has grown (0 to 1): from the pill's
 * width at the pill's middle on the bar, dense white and half faded, to
 * its own place and size on the windows' panels' frosted glass (white
 * through, with the panels made solid, BUG-214).
 */
static void
arrange_menu_grow(
	struct kwl_server *server,
	float grown,
	struct arrange_view *view)
{
	float first_scale;
	float first_x;
	float first_y;

	/* Where it grows from: the pill's width, its middle, its bar's middle. */
	first_scale = (float)arrange_menu.pill_width / (float)ARRANGE_MENU_WIDTH;
	if (first_scale < ARRANGE_MENU_LEAST_SCALE)
		first_scale = ARRANGE_MENU_LEAST_SCALE;
	if (first_scale > 1.0f)
		first_scale = 1.0f;
	first_x = (float)arrange_menu.pill_middle - (float)ARRANGE_MENU_WIDTH * first_scale * 0.5f;
	first_y = (float)arrange_menu.pill_top + (float)KWL_GLASS_BAR * 0.5f - (float)arrange_menu.height * first_scale * 0.5f;

	/* As far as it has grown towards its own place, size, glass and opacity. */
	view->scale = first_scale + (1.0f - first_scale) * grown;
	view->x = first_x + ((float)arrange_menu.x - first_x) * grown;
	view->y = first_y + ((float)arrange_menu.y - first_y) * grown;
	view->white = ARRANGE_MENU_DENSE + (ARRANGE_MENU_WHITE - ARRANGE_MENU_DENSE) * grown;
	view->opacity = ARRANGE_MENU_FIRST_OPACITY + (1.0f - ARRANGE_MENU_FIRST_OPACITY) * grown;

	/* The panels made solid make the menu solid white too. */
	if (server->panels_opaque != 0U)
		view->white = 1.0f;
}

/* Gives the menu's item at a point, or ARRANGE_ITEM_NONE. */
static int
arrange_menu_item_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	unsigned item;
	int32_t left;
	int32_t top;
	int32_t width;
	int32_t height;

	UNUSED_PARAMETER(server);

	/* Each item's rectangle. */
	for (item = 0U; item < ARRANGE_ITEMS; item++) {
		arrange_menu_item_rect((int)item, &left, &top, &width, &height);
		if (x >= left && x < left + width && y >= top && y < top + height)
			return (int)item;
	}

	/* Succeeded: no item there. */
	return ARRANGE_ITEM_NONE;
}

/* Gives a layout's cell in the menu: two to a row, the grid alone across the last row. */
static void
arrange_menu_item_rect(
	int item,
	int32_t *x,
	int32_t *y,
	int32_t *width,
	int32_t *height)
{
	int column;
	int row;

	/* The cell's column and row, inside the padding. */
	column = item % ARRANGE_MENU_COLUMNS;
	row = item / ARRANGE_MENU_COLUMNS;
	*x = arrange_menu.x + ARRANGE_MENU_PAD + column * (ARRANGE_MENU_CELL_WIDTH + ARRANGE_MENU_CELL_GAP);
	*y = arrange_menu.y + ARRANGE_MENU_PAD + row * (ARRANGE_MENU_CELL_HEIGHT + ARRANGE_MENU_CELL_GAP);
	*width = ARRANGE_MENU_CELL_WIDTH;
	*height = ARRANGE_MENU_CELL_HEIGHT;

	/* The grid, alone on its row, takes the row's whole width. */
	if (item == (int)KWL_ARRANGE_GRID)
		*width = ARRANGE_MENU_COLUMNS * ARRANGE_MENU_CELL_WIDTH + (ARRANGE_MENU_COLUMNS - 1) * ARRANGE_MENU_CELL_GAP;
}

/* Acts on a menu's item: the layout arranges the windows of the desktop shown; the menu closes. */
static void
arrange_menu_act(
	struct kwl_server *server,
	int item)
{
	/* No item. */
	if (item == ARRANGE_ITEM_NONE)
		return;

	/* The menu closes first. */
	arrange_menu_close(server, "choice");

	/* Succeeded: the layout arranges the windows of the menu's output. */
	arrange_apply(server, arrange_menu.output, (unsigned)item);
}

/*
 * Arranges the windows of the desktop shown in a layout (WS181 §4.4): the
 * docked mode ends quietly first (every window floating, no animation of
 * its own), each window goes to the slot nearest to where it floats
 * (arrange.c), told its size, and glides there from where it was drawn;
 * the desktop is in the arrangement mode.
 */
static void
arrange_apply(
	struct kwl_server *server,
	unsigned output,
	unsigned layout)
{
	struct kwl_object *windows[KWL_ARRANGE_MAX];
	struct kwl_arrange_rect area;
	struct kwl_arrange_rect slots[KWL_ARRANGE_MAX];
	struct arrange_desktop *arranged;
	int32_t from[KWL_ARRANGE_MAX][4];
	int32_t centres[KWL_ARRANGE_MAX][2];
	int32_t body[4];
	unsigned order[KWL_ARRANGE_MAX];
	unsigned count;
	unsigned made;
	unsigned index;
	unsigned slot;
	int misfit;
	int recalled;

	/* The windows to arrange, top first, up to the layout's limit. */
	count = arrange_targets(server, output, windows, kwl_arrange_limit(layout));
	if (count == 0U) {
		printf("KWL ARRANGE apply layout=%s desktop=%u windows=0\n", kwl_arrange_name(layout), server->desktop + 1U);
		return;
	}

	/* Where each is drawn now, the glides' start. */
	for (index = 0U; index < count; index++)
		kwl_glass_body(server, windows[index], from[index]);

	/* Every window floating, quietly (the docked mode ends without an animation). */
	kwl_glass_leave_quiet(server, output, "arrange");

	/* Each window's centre where it floats now (its body as drawn, the size its client drew). */
	for (index = 0U; index < count; index++) {
		kwl_glass_body(server, windows[index], body);
		centres[index][0] = body[0] + body[2] / 2;
		centres[index][1] = body[1] + body[3] / 2;
	}

	/*
	 * The slots in the work area, and which window goes to which.  A
	 * window whose smallest size is larger than its slot leaves the
	 * arrangement, staying where it floats as one past the layout's limit
	 * does, and the others are arranged again without it (ws177-p035).
	 */
	kwl_glass_work_area(server, output, &area);
	made = kwl_arrange_slots(layout, count, &area, slots);
	kwl_arrange_assign(centres, made, slots, order);

	/*
	 * The same windows arranged in the same layout before on this desktop
	 * go back to the slots they had (ws177-p037, the 2026-10-08 user
	 * decision (b)), not to the nearest ones.
	 */
	recalled = arrange_recall(server->desktop, output, layout, windows, made, order);
	if (recalled)
		printf("KWL ARRANGE recall desktop=%u layout=%s windows=%u\n", server->desktop + 1U, kwl_arrange_name(layout), made);

	/* A window too large for its slot. */
	misfit = arrange_misfit(windows, made, slots, order);
	while (misfit >= 0) {
		printf("KWL ARRANGE too-large surface=%u min=%dx%d slot=%dx%d\n", windows[misfit]->id, windows[misfit]->min_width, windows[misfit]->min_height, slots[order[misfit]].width, slots[order[misfit]].height);

		/* The window out, the ones below it up a place. */
		for (index = (unsigned)misfit; index + 1U < count; index++) {
			windows[index] = windows[index + 1U];
			memcpy(from[index], from[index + 1U], sizeof(from[index]));
			memcpy(centres[index], centres[index + 1U], sizeof(centres[index]));
		}

		/* One window fewer. */
		count--;

		/* None left: nothing is arranged. */
		if (count == 0U) {
			printf("KWL ARRANGE apply layout=%s desktop=%u windows=0\n", kwl_arrange_name(layout), server->desktop + 1U);
			return;
		}

		/* The others, arranged again (to the slots they had, when they had them). */
		made = kwl_arrange_slots(layout, count, &area, slots);
		kwl_arrange_assign(centres, made, slots, order);
		recalled = arrange_recall(server->desktop, output, layout, windows, made, order);
		if (recalled)
			printf("KWL ARRANGE recall desktop=%u layout=%s windows=%u\n", server->desktop + 1U, kwl_arrange_name(layout), made);
		misfit = arrange_misfit(windows, made, slots, order);
	}

	/* The desktop's arrangement. */
	arranged = &arrange_desktops[server->desktop][output];
	memset(arranged, 0, sizeof(*arranged));
	arranged->on = 1U;
	arranged->layout = layout;
	arranged->count = made;
	arranged->map_order = server->map_order;
	for (slot = 0U; slot < made; slot++)
		arranged->slots[slot].slot = slots[slot];

	/* Each window into its slot, gliding from where it was drawn, the arranged ones on top in the slots' order. */
	/* The windows into their slots, the arranged ones raised in the slots' order. */
	for (index = made; index > 0U; index--) {
		slot = order[index - 1U];
		arranged->slots[slot].window = windows[index - 1U];
		memcpy(arranged->slots[slot].from, from[index - 1U], sizeof(arranged->slots[slot].from));
		kwl_glass_raise(server, windows[index - 1U]);
	}
	for (slot = 0U; slot < made; slot++) {
		(void)arrange_body(arranged->slots[slot].window, &slots[slot], body);
		kwl_glass_place_body(server, arranged->slots[slot].window, body[0], body[1], body[2], body[3]);
		memcpy(arranged->slots[slot].to, body, sizeof(arranged->slots[slot].to));
		arranged->slots[slot].glide_ms = kwl_milliseconds();
	}

	/* The log, whole on one line after the windows' own lines (the tests read it). */
	printf("KWL ARRANGE apply layout=%s desktop=%u windows=%u ", kwl_arrange_name(layout), server->desktop + 1U, made);
	if (output != KWL_PLANE_ANCHOR)
		printf("output=%u ", output);
	printf("slots=");
	for (slot = 0U; slot < made; slot++)
		printf("%s%u@%d,%d,%d,%d", slot == 0U ? "" : ";", arranged->slots[slot].window->id, slots[slot].x, slots[slot].y, slots[slot].width, slots[slot].height);
	printf("\n");
	server->dirty = 1;
}

/*
 * Finds the windows to arrange on the desktop shown, top first, up to a
 * limit: mapped, not minimized, not fullscreen, without a parent, not the
 * desktop's icons (in the docked mode the windows hidden by docking too).
 * Returns how many.
 */
static unsigned
arrange_targets(
	struct kwl_server *server,
	unsigned output,
	struct kwl_object **windows,
	unsigned limit)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	unsigned count;
	unsigned index;
	unsigned place;
	int desktop_surface;

	/* Each window of the desktop shown, kept in order from the top. */
	count = 0U;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only live mapped windows of the desktop shown, without a parent, shown. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role ||
			    surface->parent_window != NULL ||
			    surface->desktop != server->desktop ||
			    surface->output != output ||
			    surface->minimized ||
			    surface->fullscreen)
				continue;
			desktop_surface = kwl_desktop_is(surface);
			if (desktop_surface)
				continue;

			/* Its place in the order from the top. */
			place = count;
			while (place > 0U && windows[place - 1U]->map_order < surface->map_order)
				place--;
			if (place >= limit)
				continue;

			/* Kept there, the ones below moved down (the last past the limit is dropped). */
			if (count < limit)
				count++;
			for (index = count - 1U; index > place; index--)
				windows[index] = windows[index - 1U];
			windows[place] = surface;
		}
	}

	/* Succeeded: the windows. */
	return count;
}

/*
 * Gives a window's body in a slot: under its title bar for a window whose
 * title the compositor draws, the whole slot for one that draws its own;
 * a window of limited size at its own size in the middle of that
 * (arrange.c, ws177-p035).  Returns 1 when the window fits in the slot, 0
 * when its smallest size is larger.
 */
static int
arrange_body(
	const struct kwl_object *surface,
	const struct kwl_arrange_rect *slot,
	int32_t body[4])
{
	struct kwl_arrange_rect room;
	struct kwl_arrange_rect placed;
	int32_t limits[4];
	int decorated;
	int fits;

	/* The whole slot. */
	room = *slot;

	/* The compositor's title bar above the body. */
	decorated = kwl_decoration_server(surface);
	if (decorated) {
		room.y += KWL_GLASS_TITLE + KWL_GLASS_GAP;
		room.height -= KWL_GLASS_TITLE + KWL_GLASS_GAP;
	}

	/* The window's own smallest and largest sizes, in the middle of the room. */
	limits[0] = surface->min_width;
	limits[1] = surface->min_height;
	limits[2] = surface->max_width;
	limits[3] = surface->max_height;
	fits = kwl_arrange_fit(&room, limits, &placed);
	body[0] = placed.x;
	body[1] = placed.y;
	body[2] = placed.width;
	body[3] = placed.height;

	/* Too large for the slot. */
	if (!fits)
		return 0;

	/* Succeeded: the body fits in the slot. */
	return 1;
}

/*
 * Finds a window whose smallest size does not fit the slot it is given
 * (order: each window's slot), the lowest of them (the last one of
 * windows, top first).  Returns its index, or -1 when every window fits.
 */
static int
arrange_misfit(
	struct kwl_object **windows,
	unsigned count,
	const struct kwl_arrange_rect *slots,
	const unsigned *order)
{
	int32_t body[4];
	unsigned index;
	int misfit;
	int fits;

	/* Each window in its slot; the last that does not fit is kept. */
	misfit = -1;
	for (index = 0U; index < count; index++) {
		fits = arrange_body(windows[index], &slots[order[index]], body);
		if (!fits)
			misfit = (int)index;
	}

	/* Succeeded: the lowest window too large, or none. */
	return misfit;
}

/* Glides a slot's window from where it is drawn now into the slot, and tells it the slot's size. */
static void
arrange_glide_to(
	struct kwl_server *server,
	struct arrange_slot *slot,
	unsigned index)
{
	int32_t body[4];

	UNUSED_PARAMETER(index);

	/* From where it is drawn now. */
	kwl_glass_body(server, slot->window, slot->from);

	/* To its slot, told its size. */
	(void)arrange_body(slot->window, &slot->slot, body);
	kwl_glass_place_body(server, slot->window, body[0], body[1], body[2], body[3]);
	memcpy(slot->to, body, sizeof(slot->to));
	slot->glide_ms = kwl_milliseconds();
	server->dirty = 1;
}

/* Ends a desktop's arrangement mode on an output: its windows are plain floating windows where they are (a head's line names it). */
static void
arrange_end(
	struct kwl_server *server,
	unsigned desktop,
	unsigned output,
	const char *reason)
{
	struct arrange_desktop *arranged;
	struct arrange_memory *memory;
	unsigned index;

	/* A swap on it is over. */
	if (arrange_swap.window != NULL && arrange_swap.desktop == desktop && arrange_swap.output == output)
		arrange_swap_end(server);

	/* Its layout and which window was in which slot are remembered (ws177-p037). */
	arranged = &arrange_desktops[desktop][output];
	memory = &arrange_memories[desktop][output];
	memset(memory, 0, sizeof(*memory));
	memory->valid = 1U;
	memory->layout = arranged->layout;
	memory->count = arranged->count;
	for (index = 0U; index < arranged->count; index++)
		memory->windows[index] = arranged->slots[index].window;

	/* Nothing else of the arrangement is kept. */
	memset(arranged, 0, sizeof(*arranged));
	server->dirty = 1;
	if (output == KWL_PLANE_ANCHOR) {
		printf("KWL ARRANGE end desktop=%u reason=%s\n", desktop + 1U, reason);
	} else {
		printf("KWL ARRANGE end desktop=%u reason=%s output=%u\n", desktop + 1U, reason, output);
	}
}

/* Gives a window's slot in a desktop's arrangement on an output, or -1. */
static int
arrange_slot_of(
	unsigned desktop,
	unsigned output,
	const struct kwl_object *surface)
{
	unsigned index;

	/* Each slot of the desktop. */
	for (index = 0U; index < arrange_desktops[desktop][output].count; index++) {
		if (arrange_desktops[desktop][output].slots[index].window == surface)
			return (int)index;
	}

	/* Succeeded: it is not arranged there. */
	return -1;
}

/* Gives the output a window is on (plane.h's slot), the anchor for one out of range. */
static unsigned
arrange_output(
	const struct kwl_object *surface)
{
	/* A slot of the plane, else the anchor. */
	if (surface->output >= KWL_PLANE_SLOTS)
		return KWL_PLANE_ANCHOR;
	return surface->output;
}

/* Draws a layout's small drawing: its slots for its usual number of windows, scaled down (and by scale, the menu growing), filled in the ink. */
static void
arrange_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned layout,
	float x,
	float y,
	float scale,
	const float *ink,
	float alpha)
{
	struct kwl_arrange_rect area;
	struct kwl_arrange_rect slots[KWL_ARRANGE_MAX];
	float colour[4];
	unsigned count;
	unsigned made;
	unsigned index;

	/* Three windows, four in the grid, in an area five times the drawing's size. */
	count = 3U;
	if (layout == KWL_ARRANGE_GRID)
		count = 4U;
	area.x = -KWL_ARRANGE_MARGIN;
	area.y = -KWL_ARRANGE_MARGIN;
	area.width = ARRANGE_ICON_WIDTH * ARRANGE_ICON_SCALE + 2 * KWL_ARRANGE_MARGIN;
	area.height = ARRANGE_ICON_HEIGHT * ARRANGE_ICON_SCALE + 2 * KWL_ARRANGE_MARGIN;
	made = kwl_arrange_slots(layout, count, &area, slots);

	/* Each slot, scaled down, filled lightly with the ink. */
	memcpy(colour, ink, sizeof(colour));
	colour[3] = 0.55f * alpha;
	for (index = 0U; index < made; index++) {
		glass_draw_solid(server, command,
				 x + (float)slots[index].x / (float)ARRANGE_ICON_SCALE * scale,
				 y + (float)slots[index].y / (float)ARRANGE_ICON_SCALE * scale,
				 ((float)slots[index].width / (float)ARRANGE_ICON_SCALE - 2.0f) * scale,
				 ((float)slots[index].height / (float)ARRANGE_ICON_SCALE - 2.0f) * scale,
				 3.0f * scale,
				 colour);
	}
}

/* Tells whether something of the arrangement moves: the menu growing or fading, or a window gliding to its slot. */
static int
arrange_moving(void)
{
	const struct arrange_desktop *arranged;
	const struct arrange_slot *slot;
	unsigned desktop;
	unsigned output;
	unsigned index;
	uint64_t now;

	/* The time the movements are measured against. */
	now = kwl_milliseconds();

	/* The open menu still growing, until its time and the frame after it. */
	if (arrange_menu.open &&
	    !arrange_menu.settled &&
	    now - arrange_menu.opened_ms < ARRANGE_MENU_OPEN_MS + ARRANGE_MOVING_GRACE_MS)
		return 1;

	/* The closed menu still fading, likewise. */
	if (!arrange_menu.open &&
	    arrange_menu.closed_ms != 0U &&
	    now - arrange_menu.closed_ms < ARRANGE_MENU_CLOSE_MS + ARRANGE_MOVING_GRACE_MS)
		return 1;

	/* A window of any desktop and output in the arrangement mode on its way to its slot, likewise. */
	for (desktop = 0U; desktop < KWL_APPS_DESKTOPS; desktop++) {
		for (output = 0U; output < KWL_PLANE_SLOTS; output++) {
			arranged = &arrange_desktops[desktop][output];
			if (!arranged->on)
				continue;
			for (index = 0U; index < arranged->count; index++) {
				slot = &arranged->slots[index];
				if (slot->glide_ms != 0U && now - slot->glide_ms < ARRANGE_MS + ARRANGE_MOVING_GRACE_MS)
					return 1;
			}
		}
	}

	/* Succeeded: nothing moves. */
	return 0;
}

/* Ends a swap being dragged without a release (the arrangement ended): the window stays where it is. */
static void
arrange_swap_end(
	struct kwl_server *server)
{
	/* No swap any more. */
	arrange_swap.window = NULL;
	server->dirty = 1;
}

/*
 * Finds the order of slots the windows had when this layout arranged them
 * on this desktop before (ws177-p037): the arrangement on now, when it is
 * of that layout, else the one remembered when its mode ended.  Only the
 * same windows, as many, recall it.  Returns 1 and sets order (each
 * window's slot) when they do.
 */
static int
arrange_recall(
	unsigned desktop,
	unsigned output,
	unsigned layout,
	struct kwl_object **windows,
	unsigned count,
	unsigned *order)
{
	struct arrange_memory now;
	const struct arrange_memory *memory;
	const struct arrange_desktop *arranged;
	unsigned recalled[KWL_ARRANGE_MAX];
	unsigned index;
	unsigned slot;
	int found;

	/* The arrangement on now, of the same layout, is what is recalled. */
	arranged = &arrange_desktops[desktop][output];
	memory = &arrange_memories[desktop][output];
	if (arranged->on && arranged->layout == layout) {
		memset(&now, 0, sizeof(now));
		now.valid = 1U;
		now.layout = arranged->layout;
		now.count = arranged->count;
		for (index = 0U; index < arranged->count; index++)
			now.windows[index] = arranged->slots[index].window;
		memory = &now;
	}

	/* Only the same layout with as many windows. */
	if (!memory->valid ||
	    memory->layout != layout ||
	    memory->count != count)
		return 0;

	/* Each window's slot, which every window must have had. */
	for (index = 0U; index < count; index++) {
		found = 0;
		for (slot = 0U; slot < memory->count; slot++) {
			if (memory->windows[slot] == windows[index] && windows[index] != NULL) {
				recalled[index] = slot;
				found = 1;
				break;
			}
		}

		/* A window that was not there: nothing is recalled. */
		if (!found)
			return 0;
	}

	/* Succeeded: the windows go back to their slots. */
	memcpy(order, recalled, sizeof(recalled[0]) * count);
	return 1;
}

/*
 * Finds the slot an arrow points to from a slot: of the slots whose middle
 * is further that way, the nearest, counting a step across as twice a
 * step along.  Returns its index, or -1 when there is none that way.
 */
static int
arrange_neighbour(
	const struct arrange_desktop *arranged,
	unsigned from,
	uint32_t key)
{
	const struct kwl_arrange_rect *slot;
	int64_t best_score;
	int64_t score;
	int32_t from_x;
	int32_t from_y;
	int32_t dx;
	int32_t dy;
	int32_t along;
	int32_t across;
	unsigned index;
	int best;

	/* The middle of the slot it starts from. */
	slot = &arranged->slots[from].slot;
	from_x = slot->x + slot->width / 2;
	from_y = slot->y + slot->height / 2;

	/* Each other slot's middle, along the arrow and across it. */
	best = -1;
	best_score = 0;
	for (index = 0U; index < arranged->count; index++) {
		if (index == from)
			continue;
		slot = &arranged->slots[index].slot;
		dx = slot->x + slot->width / 2 - from_x;
		dy = slot->y + slot->height / 2 - from_y;

		/* How far along the arrow, and across it. */
		if (key == ARRANGE_KEY_LEFT) {
			along = -dx;
			across = dy;
		} else if (key == ARRANGE_KEY_RIGHT) {
			along = dx;
			across = dy;
		} else if (key == ARRANGE_KEY_UP) {
			along = -dy;
			across = dx;
		} else {
			along = dy;
			across = dx;
		}

		/* Across either way counts the same. */
		if (across < 0)
			across = -across;

		/* Only further that way; the nearest is kept. */
		if (along <= 0)
			continue;
		score = (int64_t)along + 2 * (int64_t)across;
		if (best < 0 || score < best_score) {
			best = (int)index;
			best_score = score;
		}
	}

	/* Succeeded: the slot that way, or none. */
	return best;
}

/* Names an arrow for the log. */
static const char *
arrange_key_name(
	uint32_t key)
{
	/* Each arrow's name. */
	switch (key) {
	case ARRANGE_KEY_LEFT:
		return "left";
	case ARRANGE_KEY_RIGHT:
		return "right";
	case ARRANGE_KEY_UP:
		return "up";
	default:
		break;
	}

	/* Succeeded: the last arrow is down. */
	return "down";
}
