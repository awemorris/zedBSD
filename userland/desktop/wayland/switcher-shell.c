/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application switcher on the desktop (ws142-p005, the 2026-10-04 user
 * request, D1 and D2 of plan/ws142/phase001; switcher.c keeps its state).
 *
 * Alt+Tab, or a tap of three fingers on the touch pad, opens it on the
 * desktop's applications in the order of the bar's icons, with the current
 * one selected (BUG-209, the 2026-10-05 user decision).  While no window is docked the
 * selection shows as the bar's previews of that application under its icon
 * (apps-bar.c, as the pointer resting on the icon shows them); with a
 * docked window (D6), or for an application without room in the bar, it
 * shows in the middle of the output: the application's previews over a
 * row of the applications' icons, the selected one lit.  Never over a
 * fullscreen window, the login or lock screen, App Home or Wiseview.
 *
 * Tab and the right arrow (Shift+Tab and the left arrow back), another tap
 * of three fingers, and a swipe of two fingers across the pad (one swipe a
 * step, the way the fingers go, ws142-p009) move the selection one icon to
 * the right (or the left), around at the ends; a swipe down brings it.
 * Letting Alt go, Enter, and for the pad's switcher a tap or a click bring
 * the selected application's latest window to the top (back from
 * minimized); a click on a preview brings that window, on an icon of the
 * middle's row that application.  Esc, and a click elsewhere on the
 * keyboard's switcher, give it up.  A quick Alt+Tab (Alt let go soon,
 * before any step) leaves the switcher open, and letting Alt go brings
 * nothing from then on (BUG-209, the 2026-10-06 user instruction).  A button that acted on the switcher
 * keeps its release from the windows.
 */

#include "kwl.h"
#include "apps.h"
#include "apps-bar.h"
#include "glass.h"
#include "switcher.h"

#include <stdio.h>
#include <string.h>

/* The modifiers' bits the keys are read with (Shift, Control, Alt, Super). */
#define SWITCH_SHIFT		1U
#define SWITCH_CONTROL		4U
#define SWITCH_ALT		8U
#define SWITCH_SUPER		0x40U

/* The middle's icons: their place and mark (pixels), and the gap over them. */
#define CENTER_ICON		56
#define CENTER_MARK		48
#define CENTER_GAP		12

/* The switcher in the middle of the output: the selected application's previews and the row of icons. */
struct switch_center {
	struct apps_panel panel;
	struct apps_rect rect;
	struct apps_rect icons[KWL_APPS_MAX];
	int apps[KWL_APPS_MAX];
	unsigned icon_count;
	int selected;
};

static int view_and_selection(struct kwl_server *server, struct apps_view *view, int *found);
static void present(struct kwl_server *server);
static int center_build(struct kwl_server *server, const struct apps_view *view, struct switch_center *center);
static int bar_panel(struct kwl_server *server, const struct apps_view *view, struct apps_panel *panel);
static void bring_app(struct kwl_server *server, const struct apps_view *view, int found, const char *how);
static void close_switcher(struct kwl_server *server);
static void alt_released(struct kwl_server *server);
static const char *via_name(unsigned via);
static const char *placement_name(unsigned placement);

/*
 * Tells whether the switcher is on.
 */
int
kwl_switch_on(
	struct kwl_server *server)
{
	/* Off. */
	if (server->switcher.on == 0U)
		return 0;

	/* Succeeded: on. */
	return 1;
}

/*
 * Opens the switcher (Alt+Tab, a tap of three fingers).  Returns 1 when it opened.
 */
int
kwl_switch_open(
	struct kwl_server *server,
	unsigned via)
{
	struct apps_view view;
	const char *selected;
	unsigned placement;
	int allowed;
	int collected;
	int error;

	/* Only where the switcher may show, with applications to switch between. */
	allowed = kwl_glass_switch_place(server, &placement);
	if (!allowed)
		return 0;
	collected = kwl_apps_view_collect(server, &view);
	if (!collected)
		return 0;
	error = kwl_switcher_open(&server->switcher, &view.apps, view.current, via, placement);
	if (error != 0)
		return 0;

	/* When it opened: a quick Alt+Tab is told from it; a swipe begins afresh. */
	server->switcher.opened_ms = kwl_milliseconds();
	kwl_swipe_end(&server->pad_swipe);

	/* The bar's own previews give way to it. */
	kwl_apps_bar_hide(server, "switch");

	/* Logged and shown. */
	selected = kwl_switcher_selected(&server->switcher);
	printf("KWL SWITCH open via=%s index=%u app=%s placement=%s count=%u at_ms=%llu\n", via_name(via), server->switcher.index, selected, placement_name(placement), server->switcher.count, (unsigned long long)kwl_milliseconds());
	present(server);
	return 1;
}

/*
 * Moves the selection.
 */
void
kwl_switch_step(
	struct kwl_server *server,
	int delta,
	const char *how)
{
	const char *selected;

	/* Only while on. */
	if (!server->switcher.on || delta == 0)
		return;

	/* The next one, logged and shown. */
	kwl_switcher_step(&server->switcher, delta);
	selected = kwl_switcher_selected(&server->switcher);
	printf("KWL SWITCH step index=%u app=%s via=%s at_ms=%llu\n", server->switcher.index, selected, how, (unsigned long long)kwl_milliseconds());
	present(server);
}

/*
 * Brings the selected application's latest window, and closes the switcher.
 */
void
kwl_switch_commit(
	struct kwl_server *server,
	const char *how)
{
	struct apps_view view;
	int found;
	int known;

	/* Only while on. */
	if (!server->switcher.on)
		return;

	/* The application, if it is still there. */
	known = view_and_selection(server, &view, &found);
	if (!known) {
		kwl_switch_cancel(server, "gone");
		return;
	}

	/* Brought. */
	bring_app(server, &view, found, how);
}

/*
 * Gives the switcher up, changing nothing.
 */
void
kwl_switch_cancel(
	struct kwl_server *server,
	const char *why)
{
	/* Only while on. */
	if (!server->switcher.on)
		return;

	/* Closed. */
	printf("KWL SWITCH cancel via=%s\n", why);
	close_switcher(server);
}

/*
 * Takes a key: Alt+Tab opens on the current application, or moves one icon
 * to the right (with Shift, to the left); while on, Tab and the arrows
 * move, Enter brings, Esc gives up, and letting Alt go brings the
 * keyboard's selection (not after a quick Alt+Tab, which leaves the
 * switcher open, BUG-209).  Returns 1 when the key is the switcher's (Alt's
 * own release goes on to the windows).
 */
int
kwl_switch_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	int delta;
	int held;
	int opened;
	int on;

	/* Alt+Tab (not with Control or Super): opens, or moves on; Shift+Alt+Tab moves back. */
	if (key == KEY_TAB &&
	    (server->modifiers & SWITCH_ALT) != 0U &&
	    (server->modifiers & (SWITCH_CONTROL | SWITCH_SUPER)) == 0U) {
		/* Tab's release is the switcher's while it is on. */
		if (state == 0U) {
			on = kwl_switch_on(server);
			if (!on)
				return 0;
			return 1;
		}

		/* On with Shift, back. */
		delta = 1;
		if ((server->modifiers & SWITCH_SHIFT) != 0U)
			delta = -1;

		/* Off: it opens on the current application, with Shift or without (BUG-209). */
		if (!server->switcher.on) {
			opened = kwl_switch_open(server, KWL_SWITCHER_VIA_KEYS);
			if (!opened)
				return 0;
			return 1;
		}

		/* On: one icon to the right (with Shift, to the left). */
		kwl_switch_step(server, delta, "tab");
		return 1;
	}

	/* Off: nothing else is the switcher's. */
	if (!server->switcher.on)
		return 0;

	/*
	 * Alt let go: the keyboard's switcher brings its selection, unless a
	 * quick Alt+Tab leaves it open (BUG-209); the release goes on.
	 */
	if ((key == KEY_LEFTALT || key == KEY_RIGHTALT) &&
	    state == 0U) {
		held = kwl_input_alt_held(server);
		if (!held && server->switcher.via == KWL_SWITCHER_VIA_KEYS)
			alt_released(server);

		/* The release goes on to the windows. */
		return 0;
	}

	/* The arrows, Enter and Esc; their releases too. */
	if (key == KEY_RIGHT ||
	    key == KEY_LEFT ||
	    key == KEY_ENTER ||
	    key == KEY_ESC ||
	    key == KEY_TAB) {
		/* A release only stays from the windows. */
		if (state == 0U)
			return 1;

		/* Shift turns Tab back. */
		if (key == KEY_TAB && (server->modifiers & SWITCH_SHIFT) != 0U)
			key = KEY_LEFT;

		/* Each key's work. */
		switch (key) {
		case KEY_TAB:
		case KEY_RIGHT:
			kwl_switch_step(server, 1, "key");
			break;
		case KEY_LEFT:
			kwl_switch_step(server, -1, "key");
			break;
		case KEY_ENTER:
			kwl_switch_commit(server, "enter");
			break;
		default:
			kwl_switch_cancel(server, "escape");
			break;
		}

		/* The key was the switcher's. */
		return 1;
	}

	/* Succeeded: other keys go on. */
	return 0;
}

/*
 * Takes a button while the switcher is on: a preview brings its window, an
 * icon of the middle's row its application; elsewhere the pad's switcher
 * brings its selection (a tap or a click) and the keyboard's gives up.
 * The release of such a press is kept from the windows.  Returns 1 when
 * the button is the switcher's.
 */
int
kwl_switch_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct apps_view view;
	struct apps_panel panel;
	struct switch_center center;
	unsigned index;
	int built;
	int tile;
	int found;
	int hit;

	/* The release of a press the switcher took. */
	if (state == 0U) {
		if (server->switch_swallow == button && button != 0U) {
			server->switch_swallow = 0;
			return 1;
		}

		/* Another release is the switcher's while it is on. */
		if (server->switcher.on == 0U)
			return 0;
		return 1;
	}

	/* Off: not the switcher's. */
	if (!server->switcher.on)
		return 0;
	server->switch_swallow = button;

	/* The previews and icons where it shows. */
	found = -1;
	built = view_and_selection(server, &view, &found);
	tile = -1;
	if (built && server->switcher.placement == KWL_SWITCHER_BAR) {
		built = bar_panel(server, &view, &panel);
		if (built)
			tile = kwl_apps_tile_at(panel.tiles, panel.count, server->pointer_x, server->pointer_y);
	} else if (built) {
		built = center_build(server, &view, &center);
		if (built) {
			panel = center.panel;
			tile = kwl_apps_tile_at(panel.tiles, panel.count, server->pointer_x, server->pointer_y);
			for (index = 0; index < center.icon_count; index++) {
				hit = kwl_apps_inside(&center.icons[index], server->pointer_x, server->pointer_y);
				if (hit && center.apps[index] >= 0) {
					bring_app(server, &view, center.apps[index], "icon");
					return 1;
				}
			}
		}
	}

	/* A preview: its window. */
	if (built && tile >= 0) {
		printf("KWL SWITCH commit app=%s surface=%u via=preview client=%llu\n", view.apps.apps[found].key, panel.surfaces[tile]->id, (unsigned long long)panel.surfaces[tile]->client->number);
		close_switcher(server);
		kwl_glass_switch_to(server, panel.surfaces[tile], "switch");
		return 1;
	}

	/* Elsewhere: the pad's switcher brings its selection, the keyboard's gives up. */
	if (server->switcher.via == KWL_SWITCHER_VIA_PAD) {
		kwl_switch_commit(server, "pad");
		return 1;
	}

	/* The keyboard's gives up. */
	kwl_switch_cancel(server, "press");
	return 1;
}

/*
 * Takes a swipe of two fingers on the pad while the switcher is on (one
 * swipe a step, swipe.c, ws142-p009, BUG-215): to the right the next
 * application, to the left the one before, down brings the selection.
 * Returns 1 when it took the swipe (the switcher is on).
 */
int
kwl_switch_pad_swipe(
	struct kwl_server *server,
	unsigned direction)
{
	/* Only while on. */
	if (!server->switcher.on)
		return 0;

	/* Each swipe's work; up, or no decision yet, does nothing. */
	if (direction == KWL_SWIPE_RIGHT) {
		kwl_switch_step(server, 1, "pad");
	} else if (direction == KWL_SWIPE_LEFT) {
		kwl_switch_step(server, -1, "pad");
	} else if (direction == KWL_SWIPE_DOWN) {
		kwl_switch_commit(server, "pad-swipe");
	}

	/* Succeeded: the swipe is the switcher's. */
	return 1;
}

/*
 * Gives the switcher up when it may no longer show (App Home, Wiseview, a fullscreen window, the lock).
 */
void
kwl_switch_tick(
	struct kwl_server *server)
{
	unsigned placement;
	int allowed;

	/* Only while on. */
	if (!server->switcher.on)
		return;

	/* Away. */
	allowed = kwl_glass_switch_place(server, &placement);
	if (!allowed)
		kwl_switch_cancel(server, "away");
}

/*
 * Draws the switcher in the middle of the output, when it shows there.
 */
void
kwl_switch_draw(
	struct kwl_server *server,
	struct kwl_list *command)
{
	static const float faint[4] = { 1.0f, 1.0f, 1.0f, 0.5f };
	struct apps_view view;
	struct switch_center center;
	struct glass_shape shape;
	struct kwl_object *surface;
	unsigned index;
	unsigned kept;
	float lit[4];
	float alpha;
	int found;
	int built;
	int over;

	/* Only on, in the middle. */
	if (!server->switcher.on || server->switcher.placement != KWL_SWITCHER_CENTER)
		return;
	built = view_and_selection(server, &view, &found);
	if (built)
		built = center_build(server, &view, &center);
	if (!built)
		return;

	/* The panel: a shadow, then glass. */
	glass_shape_init(&shape, (float)center.rect.x, (float)center.rect.y + 6.0f, (float)center.rect.width, (float)center.rect.height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = 22.0f;
	shape.soft = 24.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.28f;
	glass_shape_draw(server, command, &shape);
	glass_shape_init(&shape, (float)center.rect.x, (float)center.rect.y, (float)center.rect.width, (float)center.rect.height);
	shape.mode = MODE_GLASS;
	shape.radius = 22.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.66f;
	shape.edge = 0.8f;
	glass_shape_draw(server, command, &shape);

	/* The selected application's previews, the one under the pointer lit, a minimized one faint. */
	over = kwl_apps_tile_at(center.panel.tiles, center.panel.count, server->pointer_x, server->pointer_y);
	for (index = 0; index < center.panel.count; index++) {
		kwl_glass_draw_preview(server, command, center.panel.surfaces[index], center.panel.tiles[index].x, center.panel.tiles[index].y, center.panel.tiles[index].width, center.panel.tiles[index].height, over == (int)index);
		if (center.panel.surfaces[index]->minimized)
			glass_draw_solid(server, command, (float)center.panel.tiles[index].x, (float)center.panel.tiles[index].y, (float)center.panel.tiles[index].width, (float)center.panel.tiles[index].height, 10.0f, faint);
	}

	/* The row of icons, the selected one lit in the accent the user chose (as it is), those all minimized faint. */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.30f, lit);
	for (index = 0; index < center.icon_count; index++) {
		if (center.apps[index] < 0)
			continue;
		if ((int)index == center.selected) {
			kept = kwl_accent_as_is(server);
			glass_draw_solid(server, command, (float)center.icons[index].x, (float)center.icons[index].y, (float)CENTER_ICON, (float)CENTER_ICON, 14.0f, lit);
			kwl_accent_done(server, kept);
		}
		surface = view.surfaces[view.apps.apps[center.apps[index]].windows[0]];
		alpha = 1.0f;
		if (view.apps.apps[center.apps[index]].minimized)
			alpha = 0.45f;
		kwl_glass_draw_app_mark(server, command, surface, center.icons[index].x + (CENTER_ICON - CENTER_MARK) / 2, center.icons[index].y + CENTER_ICON / 2, CENTER_MARK, alpha);
	}
}

/* Gathers the desktop's applications and finds the selected one.  Returns 0 when it is gone. */
static int
view_and_selection(
	struct kwl_server *server,
	struct apps_view *view,
	int *found)
{
	const char *selected;
	int collected;

	/* The applications now. */
	*found = -1;
	collected = kwl_apps_view_collect(server, view);
	selected = kwl_switcher_selected(&server->switcher);
	if (!collected || selected == NULL)
		return 0;

	/* The selected one among them. */
	*found = kwl_apps_find(&view->apps, selected);
	if (*found < 0)
		return 0;

	/* Succeeded: found. */
	return 1;
}

/*
 * Shows the selection: at its icon in the bar (the bar's previews) when
 * the switcher shows there and the application has an icon; otherwise in
 * the middle of the output.
 */
static void
present(
	struct kwl_server *server)
{
	struct apps_view view;
	const char *selected;
	unsigned placement;
	int built;
	int found;
	int allowed;

	/* Where it may show now. */
	server->dirty = 1;
	allowed = kwl_glass_switch_place(server, &placement);
	selected = kwl_switcher_selected(&server->switcher);
	if (!allowed || selected == NULL)
		return;

	/* At the icon, when the application has one. */
	if (placement == KWL_SWITCHER_BAR) {
		built = kwl_apps_view_build(server, &view);
		found = -1;
		if (built)
			found = kwl_apps_find(&view.apps, selected);
		if (found >= 0 && found < (int)view.shown) {
			server->switcher.placement = KWL_SWITCHER_BAR;
			kwl_apps_bar_show(server, &view, selected, KWL_APPS_VIA_SWITCH);
			return;
		}
	}

	/* Otherwise in the middle. */
	server->switcher.placement = KWL_SWITCHER_CENTER;
	kwl_apps_bar_hide(server, "switch");
	printf("KWL SWITCH center app=%s\n", selected);
}

/*
 * Lays out the switcher in the middle of the output: the selected
 * application's previews over a row of the applications' icons (in the
 * switcher's order), centred.  Returns 0 when the selection is gone.
 */
static int
center_build(
	struct kwl_server *server,
	const struct apps_view *view,
	struct switch_center *center)
{
	const char *selected;
	int32_t icons_width;
	int32_t width;
	int32_t row_x;
	unsigned index;
	int found;

	/* The selected application's previews. */
	selected = kwl_switcher_selected(&server->switcher);
	if (selected == NULL)
		return 0;
	found = kwl_apps_find(&view->apps, selected);
	if (found < 0)
		return 0;
	kwl_apps_tiles_layout(server, view, (unsigned)found, &center->panel);

	/* The icons in the switcher's order (an application gone since keeps its place, empty). */
	center->icon_count = server->switcher.count;
	center->selected = (int)server->switcher.index;
	for (index = 0; index < center->icon_count; index++)
		center->apps[index] = kwl_apps_find(&view->apps, server->switcher.keys[index]);

	/* The panel's size and place in the middle. */
	icons_width = (int32_t)center->icon_count * CENTER_ICON;
	width = center->panel.rect.width;
	if (icons_width > width)
		width = icons_width;
	center->rect.width = width + 2 * PANEL_PAD;
	center->rect.height = PANEL_PAD + center->panel.rect.height + CENTER_GAP + CENTER_ICON + PANEL_PAD;
	center->rect.x = ((int32_t)server->width - center->rect.width) / 2;
	center->rect.y = ((int32_t)server->height - center->rect.height) / 2;
	if (center->rect.y < KWL_GLASS_BAR + PANEL_DROP)
		center->rect.y = KWL_GLASS_BAR + PANEL_DROP;

	/* The previews in its upper part, centred. */
	for (index = 0; index < center->panel.count; index++) {
		center->panel.tiles[index].x += center->rect.x + (center->rect.width - center->panel.rect.width) / 2;
		center->panel.tiles[index].y += center->rect.y + PANEL_PAD;
	}

	/* The icons under them, centred. */
	row_x = center->rect.x + (center->rect.width - icons_width) / 2;
	for (index = 0; index < center->icon_count; index++) {
		center->icons[index].x = row_x + (int32_t)index * CENTER_ICON;
		center->icons[index].y = center->rect.y + PANEL_PAD + center->panel.rect.height + CENTER_GAP;
		center->icons[index].width = CENTER_ICON;
		center->icons[index].height = CENTER_ICON;
	}

	/* Succeeded: laid out. */
	return 1;
}

/* Gives the bar's previews of the selection, when it shows there.  Returns 0 otherwise. */
static int
bar_panel(
	struct kwl_server *server,
	const struct apps_view *view,
	struct apps_panel *panel)
{
	struct apps_view bar;
	const char *selected;
	int built;

	/* The bar's own view, with the icons' places. */
	(void)view;
	built = kwl_apps_view_build(server, &bar);
	if (!built)
		return 0;

	/* Its panel of the selection. */
	selected = kwl_switcher_selected(&server->switcher);
	built = kwl_apps_bar_panel(server, &bar, selected, panel);
	if (!built)
		return 0;

	/* Succeeded: laid out. */
	return 1;
}

/* Brings an application's latest window, and closes the switcher. */
static void
bring_app(
	struct kwl_server *server,
	const struct apps_view *view,
	int found,
	const char *how)
{
	struct kwl_object *surface;

	/* Its latest raised window. */
	surface = view->surfaces[view->apps.apps[found].windows[0]];
	printf("KWL SWITCH commit app=%s surface=%u via=%s at_ms=%llu client=%llu\n", view->apps.apps[found].key, surface->id, how, (unsigned long long)kwl_milliseconds(), (unsigned long long)surface->client->number);

	/* Closed, then brought. */
	close_switcher(server);
	kwl_glass_switch_to(server, surface, "switch");
}

/*
 * Brings the keyboard's selection as Alt is let go, or leaves the switcher
 * open after a quick Alt+Tab (switcher.c says when).
 */
static void
alt_released(
	struct kwl_server *server)
{
	uint64_t now;
	unsigned sticky;
	int brings;

	/* What letting Alt go does now. */
	now = kwl_milliseconds();
	sticky = server->switcher.sticky;
	brings = kwl_switcher_alt_released(&server->switcher, now);
	if (brings) {
		kwl_switch_commit(server, "alt");
		return;
	}

	/* Left open by this release (the first after a quick Alt+Tab); the log says so once. */
	if (!sticky && server->switcher.sticky)
		printf("KWL SWITCH stay via=quick-alt index=%u app=%s held_ms=%llu\n", server->switcher.index, kwl_switcher_selected(&server->switcher), (unsigned long long)(now - server->switcher.opened_ms));
}

/* Closes the switcher and its previews. */
static void
close_switcher(
	struct kwl_server *server)
{
	/* Off. */
	kwl_switcher_close(&server->switcher);
	kwl_apps_bar_hide(server, "switch");
	server->dirty = 1;
}

/* Names how the switcher was opened. */
static const char *
via_name(
	unsigned via)
{
	/* The keys or the pad. */
	if (via == KWL_SWITCHER_VIA_PAD)
		return "pad";
	return "keys";
}

/* Names where the switcher shows. */
static const char *
placement_name(
	unsigned placement)
{
	/* The bar or the middle. */
	if (placement == KWL_SWITCHER_CENTER)
		return "center";
	return "bar";
}
