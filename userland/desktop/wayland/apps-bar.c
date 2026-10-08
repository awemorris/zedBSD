/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications' icons in the system bar, and their window previews
 * (ws142-p004, the 2026-10-04 user request and the decisions D2, D4 to D8
 * and D11 of plan/ws142/phase001).
 *
 * While no window is docked (D6: a docked window's title has the bar; a
 * fullscreen window hides the bar), the bar shows an icon for each
 * application with a window on that bar's display, on the desktop shown
 * (apps.c; ws113-p015, the 2026-10-08 user decision: each display's bar,
 * the system bar's and each head's, has its own windows' applications, an
 * application with windows on two displays is in both bars, and its
 * previews are of the windows on that display; the switcher has every
 * window).  The icons go from after the launcher's line to before the
 * desktops' line, in the desktop's own order (one for all the bars): the
 * order the applications were opened in, which dragging an icon changes (the
 * 2026-10-05 request; each desktop keeps its order).  The application of
 * the window on top has a short line under its icon (all the icons sit in
 * one pill, ws099-p034); one whose windows are all
 * minimized is drawn faint (D11).  When there are more applications than
 * room, the last place is "+N", which opens Wiseview.
 *
 * The pointer resting on an icon for HOVER_MS shows the application's
 * windows as previews (at most a quarter of the output's width and height
 * each, D4) in a glass panel under the bar; moving to another icon shows
 * its windows at once; leaving the icon and the panel for LEAVE_MS hides
 * them.  The previews show under the bar of the icon (on its output).
 * A click on the icon of an application with one window brings that
 * window to the top (back from minimized); with more it shows the previews
 * at once, and a second click hides them.  A click on a preview brings its
 * window; on its close button closes it.  Esc, and a press elsewhere, hide
 * the previews (the press goes on to what is under it).
 *
 * During a drag and drop (ws189-p002, plan/ws189/phase001/phase.md
 * section 3.3) the bar is spring-loaded: the drag resting SPRING_MS on an
 * application's icon brings its window forward (switched to as a click
 * would), or shows the previews of its windows, on one of which a rest of
 * SPRING_MS brings that window.  The icon lights up more as the rest goes
 * on.  The drag goes on; the window that came forward takes the drop when
 * the pointer goes onto it.
 */

#include "kwl.h"
#include "apps.h"
#include "apps-bar.h"
#include "desktop.h"
#include "glass.h"
#include "extras.h"
#include "keyboard.h"

#include <stdio.h>
#include <string.h>

/* Marks a parameter a function keeps for its callers but does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The pointer's rest on an icon before the previews show, and its absence before they go (milliseconds; D8). */
#define HOVER_MS		400U
#define LEAVE_MS		300U

/* How long a drag and drop rests on an icon or a preview before its window comes forward (milliseconds). */
#define SPRING_MS		700U

/* How far a press moves before it is the icon's drag (pixels). */
#define DRAG_START		8

/* The key of the "+N" place. */
#define MORE_KEY		"+"

/* What a slot of the bar is: an application, the "+N" place, or none. */
#define SLOT_NONE		(-1)
#define SLOT_MORE		(-2)

static int slot_at(const struct apps_view *view, int32_t x, int32_t y);
static void slot_rect(const struct apps_view *view, unsigned slot, struct apps_rect *rect);
static int panel_build(struct kwl_server *server, const struct apps_view *view, const char *key, struct apps_panel *panel);
static void log_bar(struct kwl_server *server, const struct apps_view *view);
static int view_collect_on(struct kwl_server *server, unsigned slot, int every, struct apps_view *view);
static void draw_more(struct kwl_server *server, VkCommandBuffer command, const struct apps_rect *rect, unsigned hidden, float light);
static void draw_light(struct kwl_server *server, VkCommandBuffer command, const struct apps_rect *rect, float strength);
static void spring_tick(struct kwl_server *server);

/*
 * Draws the applications' icons in an output's bar (draw_system_bar, or a
 * head's draw_head_bar, when no window is docked there).  Returns 1 when
 * it drew any.
 */
int
kwl_apps_bar_draw(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned output)
{
	struct glass_bar_colours colours;
	struct glass_shape shape;
	struct kwl_apps_bar *state;
	struct apps_view view;
	struct apps_rect rect;
	struct kwl_object *surface;
	const struct kwl_app *app;
	unsigned slot;
	unsigned slots;
	int32_t x;
	int32_t middle;
	int32_t pill_x;
	int32_t pill_width;
	uint64_t rest;
	float alpha;
	float light;
	int mine;
	int same;
	int built;

	/* No icons where the bar has no room for them. */
	state = &server->apps_bar;
	built = kwl_apps_view_build_on(server, output, &view);
	if (!built)
		return 0;

	/* The bar's middle, and whether the wait, the previews or a press are about this bar's icons. */
	middle = view.top + KWL_GLASS_BAR / 2;
	mine = 0;
	if (state->output == output)
		mine = 1;

	/* The bar as it is now, in the log when it changed. */
	log_bar(server, &view);

	/* The bar's colours in the appearance shown (ws099-p034b). */
	kwl_glass_bar_colours(server, &colours);

	/* One pill behind all the icons shown and the "+N" place (ws099-p034). */
	slots = view.shown;
	if (view.hidden > 0U)
		slots++;
	pill_x = view.left + (ICON_WIDTH - ICON_MARK) / 2 - ICON_PILL_PAD;
	pill_width = (int32_t)slots * ICON_WIDTH - (ICON_WIDTH - ICON_MARK) + 2 * ICON_PILL_PAD;
	glass_draw_solid(server, command, (float)pill_x, (float)(middle - 17), (float)pill_width, 34.0f, 17.0f, colours.fill);
	glass_shape_init(&shape, (float)pill_x, (float)(middle - 17), (float)pill_width, 34.0f);
	shape.mode = MODE_RING;
	shape.radius = 17.0f;
	shape.soft = 1.0f;
	memcpy(shape.color, colours.edge, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);

	/* Each application's icon. */
	for (slot = 0; slot < view.shown; slot++) {
		app = &view.apps.apps[slot];
		slot_rect(&view, slot, &rect);

		/* Lit while the pointer rests on it or its previews show. */
		light = 0.0f;
		same = strcmp(app->key, state->key);
		if (mine && same == 0 && state->state == KWL_APPS_SHOWN)
			light = 1.0f;
		if (mine && same == 0 && state->state == KWL_APPS_ARMED)
			light = 0.6f;

		/* A drag and drop resting on it lights it more as the rest goes on (spring-loading). */
		same = strcmp(app->key, state->spring_key);
		if (mine && server->dnd_active && state->spring_key[0] != '\0' && same == 0) {
			rest = kwl_milliseconds() - state->spring_since_ms;
			light = 1.0f;
			if (rest < SPRING_MS)
				light = 0.5f + 0.5f * (float)rest / (float)SPRING_MS;
		}

		/* The icon being dragged follows the pointer. */
		same = strcmp(app->key, state->press_key);
		x = rect.x;
		if (mine && state->dragging && same == 0) {
			x = server->pointer_x - ICON_WIDTH / 2;
			light = 1.0f;
		}

		/* Its light. */
		rect.x = x;
		if (light > 0.0f)
			draw_light(server, command, &rect, light);

		/* Its mark, faint when all its windows are minimized (D11). */
		surface = view.surfaces[app->windows[0]];
		alpha = 1.0f;
		if (app->minimized)
			alpha = 0.45f;
		kwl_glass_draw_app_mark(server, command, surface, x + (ICON_WIDTH - ICON_MARK) / 2, middle, ICON_MARK, alpha);

		/* A short line under the application of the window on top (ws099-p034). */
		if ((int)slot == view.current)
			glass_draw_solid(server, command, (float)(x + ICON_WIDTH / 2 - 4), (float)(middle + 14), 8.0f, 2.5f, 1.25f, colours.ink);
	}

	/* The "+N" place for the applications without room. */
	if (view.hidden > 0U) {
		slot_rect(&view, view.shown, &rect);
		light = 0.0f;
		same = strcmp(state->press_key, MORE_KEY);
		if (mine && state->pressed && same == 0)
			light = 1.0f;
		draw_more(server, command, &rect, view.hidden, light);
	}

	/* Succeeded: drawn. */
	return 1;
}

/*
 * Draws the panel of previews, when it shows (over the windows, under the
 * menus), in the pass of the output whose bar it shows under.
 */
void
kwl_apps_bar_draw_popup(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float faint[4] = { 1.0f, 1.0f, 1.0f, 0.5f };
	struct kwl_apps_bar *state;
	struct apps_view view;
	struct apps_panel panel;
	struct glass_shape shape;
	unsigned index;
	int over;
	int built;

	/* Only while the previews show, on their output, and the bar still has its icons. */
	state = &server->apps_bar;
	if (state->state != KWL_APPS_SHOWN)
		return;
	if (state->output != server->view_output)
		return;
	built = kwl_apps_view_build_on(server, state->output, &view);
	if (built)
		built = panel_build(server, &view, state->key, &panel);
	if (!built)
		return;

	/* The panel: a shadow, then glass. */
	glass_shape_init(&shape, (float)panel.rect.x, (float)panel.rect.y + 6.0f, (float)panel.rect.width, (float)panel.rect.height);
	shape.quad[0] -= 32.0f;
	shape.quad[1] -= 32.0f;
	shape.quad[2] += 64.0f;
	shape.quad[3] += 64.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = 18.0f;
	shape.soft = 20.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.25f;
	glass_shape_draw(server, command, &shape);
	glass_shape_init(&shape, (float)panel.rect.x, (float)panel.rect.y, (float)panel.rect.width, (float)panel.rect.height);
	shape.mode = MODE_GLASS;
	shape.radius = 18.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.62f;
	shape.edge = 0.8f;
	glass_shape_draw(server, command, &shape);

	/* Each window's preview, the one under the pointer lit, a minimized one faint (D11). */
	over = kwl_apps_tile_at(panel.tiles, panel.count, server->pointer_x, server->pointer_y);
	for (index = 0; index < panel.count; index++) {
		kwl_glass_draw_preview(server, command, panel.surfaces[index], panel.tiles[index].x, panel.tiles[index].y, panel.tiles[index].width, panel.tiles[index].height, over == (int)index);
		if (panel.surfaces[index]->minimized)
			glass_draw_solid(server, command, (float)panel.tiles[index].x, (float)panel.tiles[index].y, (float)panel.tiles[index].width, (float)panel.tiles[index].height, 10.0f, faint);
	}
}

/*
 * Follows the pointer: the rest on an icon that shows its previews, the
 * move to another icon, the leaving of the icons and the panel, and an
 * icon's drag.  Returns 1 when the motion is the bar's (the drag, or over
 * the panel).
 */
int
kwl_apps_bar_motion(
	struct kwl_server *server)
{
	struct kwl_apps_bar *state;
	struct kwl_apps_order *order;
	struct apps_view view;
	struct apps_panel panel;
	unsigned output;
	int32_t dx;
	int target;
	int from;
	int slot;
	int same;
	int built;
	int in_panel;

	/*
	 * The bar followed: the one whose wait, previews or press there is, or
	 * with none the one the pointer is on (the system bar's or a head's,
	 * ws113-p015).
	 */
	state = &server->apps_bar;
	output = state->output;
	if (state->state == KWL_APPS_IDLE && !state->pressed)
		output = server->pointer_output;

	/* Without its icons, nothing shows or waits. */
	built = kwl_apps_view_build_on(server, output, &view);
	if (!built) {
		if (state->state != KWL_APPS_IDLE)
			kwl_apps_bar_hide(server, "away");
		state->pressed = 0;
		state->dragging = 0;
		return 0;
	}

	/* The switcher's previews stay as it shows them; the motion over them is the bar's. */
	if (server->switcher.on) {
		in_panel = 0;
		if (state->state == KWL_APPS_SHOWN) {
			built = panel_build(server, &view, state->key, &panel);
			if (built)
				in_panel = kwl_apps_inside(&panel.rect, server->pointer_x, server->pointer_y);
		}

		/* Elsewhere the motion goes on. */
		if (!in_panel)
			return 0;

		/* Over the previews the motion lights them. */
		server->dirty = 1;
		return 1;
	}

	/* A window's move or pull, or a desktop swipe, passing over the bar starts no wait. */
	if (server->drag != NULL ||
	    server->pull != NULL ||
	    server->desktop_press) {
		if (state->state == KWL_APPS_ARMED)
			kwl_apps_bar_hide(server, "move");
		return 0;
	}

	/* A pressed icon past DRAG_START is dragged: it takes the place under the pointer, the others make room. */
	if (state->pressed) {
		dx = server->pointer_x - state->press_x;
		same = strcmp(state->press_key, MORE_KEY);
		if (!state->dragging &&
		    same != 0 &&
		    (dx >= DRAG_START || dx <= -DRAG_START)) {
			state->dragging = 1;
			kwl_apps_bar_hide(server, "drag");
			printf("KWL APPS drag app=%s\n", state->press_key);
		}

		/* Its place follows the pointer. */
		if (state->dragging) {
			order = &state->orders[server->desktop];
			target = (server->pointer_x - view.left) / ICON_WIDTH;
			if (target < 0)
				target = 0;
			if (target >= (int)view.shown)
				target = (int)view.shown - 1;
			from = kwl_apps_find(&view.apps, state->press_key);
			if (from >= 0 &&
			    target >= 0 &&
			    from != target)
				(void)kwl_apps_move(order, (unsigned)from, (unsigned)target);
			server->dirty = 1;
		}

		/* The press's motion is the bar's. */
		return 1;
	}

	/* What is under the pointer: an icon, the panel. */
	slot = slot_at(&view, server->pointer_x, server->pointer_y);
	in_panel = 0;
	if (state->state == KWL_APPS_SHOWN) {
		built = panel_build(server, &view, state->key, &panel);
		if (built)
			in_panel = kwl_apps_inside(&panel.rect, server->pointer_x, server->pointer_y);
	}

	/* Resting on nothing yet: an application's icon starts the wait, on its bar. */
	if (state->state == KWL_APPS_IDLE) {
		if (slot >= 0) {
			state->output = output;
			state->state = KWL_APPS_ARMED;
			(void)snprintf(state->key, sizeof(state->key), "%s", view.apps.apps[slot].key);
			state->since_ms = kwl_milliseconds();
			server->dirty = 1;
		}

		/* The motion goes on. */
		return 0;
	}

	/* Waiting: another icon starts again, none stops. */
	if (state->state == KWL_APPS_ARMED) {
		if (slot < 0) {
			state->state = KWL_APPS_IDLE;
			state->key[0] = '\0';
			server->dirty = 1;
			return 0;
		}

		/* Another icon. */
		same = strcmp(view.apps.apps[slot].key, state->key);
		if (same != 0) {
			(void)snprintf(state->key, sizeof(state->key), "%s", view.apps.apps[slot].key);
			state->since_ms = kwl_milliseconds();
			server->dirty = 1;
		}

		/* The motion goes on. */
		return 0;
	}

	/* Shown: another icon shows its previews at once. */
	if (slot >= 0) {
		same = strcmp(view.apps.apps[slot].key, state->key);
		if (same != 0)
			kwl_apps_bar_show(server, &view, view.apps.apps[slot].key, KWL_APPS_VIA_HOVER);
		state->left = 0;
		return 0;
	}

	/* Over the panel: it stays, and the motion lights its previews. */
	if (in_panel) {
		state->left = 0;
		server->dirty = 1;
		return 1;
	}

	/* Away from both: the time to go starts. */
	if (!state->left) {
		state->left = 1;
		state->since_ms = kwl_milliseconds();
	}

	/* The motion goes on. */
	return 0;
}

/*
 * Takes a button: a press on an icon (its click or drag decided at the
 * release), on a preview (brings or closes its window), and the release of
 * such a press.  A press elsewhere hides the previews and goes on.
 * Returns 1 when the button is the bar's.
 */
int
kwl_apps_bar_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state_value)
{
	struct kwl_apps_bar *state;
	struct apps_view view;
	struct apps_view shown;
	struct apps_panel panel;
	struct kwl_object *surface;
	const struct kwl_app *app;
	int found;
	int built;
	int shown_built;
	int slot;
	int tile;
	int same;
	int in_panel;

	/* The release of a press on an icon (of the bar it was pressed on): the drag's end, or the click. */
	state = &server->apps_bar;
	if (state_value == 0) {
		if (!state->pressed || button != KWL_BUTTON_LEFT)
			return 0;
		state->pressed = 0;
		built = kwl_apps_view_build_on(server, state->output, &view);

		/* A drag ends where it is. */
		if (state->dragging) {
			state->dragging = 0;
			found = -1;
			if (built)
				found = kwl_apps_find(&view.apps, state->press_key);
			printf("KWL APPS reorder app=%s place=%d desktop=%u\n", state->press_key, found, server->desktop + 1U);
			server->dirty = 1;
			return 1;
		}

		/* The "+N" place opens Wiseview. */
		same = strcmp(state->press_key, MORE_KEY);
		if (same == 0) {
			kwl_apps_bar_hide(server, "more");
			kwl_glass_open_wiseview(server, "apps");
			return 1;
		}

		/* An application that has gone does nothing. */
		found = -1;
		if (built)
			found = kwl_apps_find(&view.apps, state->press_key);
		if (found < 0)
			return 1;

		/* One window: it comes to the top. */
		app = &view.apps.apps[found];
		if (app->window_count == 1U) {
			kwl_apps_bar_hide(server, "raise");
			kwl_glass_switch_to(server, view.surfaces[app->windows[0]], "bar");
			return 1;
		}

		/* More: a second click on the shown icon hides its previews, otherwise they show at once. */
		same = strcmp(state->key, app->key);
		if (state->state == KWL_APPS_SHOWN &&
		    state->via == KWL_APPS_VIA_CLICK &&
		    same == 0) {
			kwl_apps_bar_hide(server, "click");
			return 1;
		}

		/* Shown by the click. */
		kwl_apps_bar_show(server, &view, app->key, KWL_APPS_VIA_CLICK);
		return 1;
	}

	/* The icons of the bar the pointer is on, and of the bar whose previews show (the same bar, or a head's, ws113-p015). */
	built = kwl_apps_view_build_on(server, server->pointer_output, &view);
	shown_built = 0;
	if (state->state == KWL_APPS_SHOWN)
		shown_built = kwl_apps_view_build_on(server, state->output, &shown);

	/* Without the icons the bars take nothing. */
	if (!built && !shown_built) {
		if (state->state != KWL_APPS_IDLE)
			kwl_apps_bar_hide(server, "away");
		return 0;
	}

	/* The previews take a press on themselves: a preview's close button closes its window, the preview brings it. */
	if (shown_built) {
		in_panel = 0;
		shown_built = panel_build(server, &shown, state->key, &panel);
		if (shown_built)
			in_panel = kwl_apps_inside(&panel.rect, server->pointer_x, server->pointer_y);
		if (in_panel) {
			tile = kwl_apps_tile_at(panel.tiles, panel.count, server->pointer_x, server->pointer_y);
			if (tile < 0 || button != KWL_BUTTON_LEFT)
				return 1;
			surface = panel.surfaces[tile];

			/* The close button at the preview's top right. */
			if (server->pointer_x >= panel.tiles[tile].x + panel.tiles[tile].width - 26 && server->pointer_y < panel.tiles[tile].y + 26) {
				(void)kwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
				printf("KWL APPS close-window surface=%u client=%llu\n", surface->id, (unsigned long long)surface->client->number);
				return 1;
			}

			/* The preview: its window comes to the top. */
			kwl_apps_bar_hide(server, "preview");
			kwl_glass_switch_to(server, surface, "preview");
			return 1;
		}
	}

	/* A left press on an icon or the "+N" place of the bar the pointer is on waits for its release. */
	slot = SLOT_NONE;
	if (built)
		slot = slot_at(&view, server->pointer_x, server->pointer_y);
	if (slot != SLOT_NONE && button == KWL_BUTTON_LEFT) {
		state->output = view.output;
		state->pressed = 1;
		state->dragging = 0;
		state->press_x = server->pointer_x;
		(void)snprintf(state->press_key, sizeof(state->press_key), "%s", MORE_KEY);
		if (slot >= 0)
			(void)snprintf(state->press_key, sizeof(state->press_key), "%s", view.apps.apps[slot].key);
		return 1;
	}

	/* A press elsewhere hides the previews, and goes on. */
	if (state->state != KWL_APPS_IDLE)
		kwl_apps_bar_hide(server, "press");
	return 0;
}

/*
 * Takes a key: Esc hides the previews.  Returns 1 when the key was the bar's.
 */
int
kwl_apps_bar_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state_value)
{
	/* Only Esc's press while the previews show or wait. */
	if (key != KWL_KEY_ESC ||
	    state_value == 0U ||
	    server->apps_bar.state == KWL_APPS_IDLE)
		return 0;

	/* A wait only stops (the key goes on); shown previews hide and take it. */
	if (server->apps_bar.state == KWL_APPS_ARMED) {
		kwl_apps_bar_hide(server, "escape");
		return 0;
	}

	/* Hidden. */
	kwl_apps_bar_hide(server, "escape");
	return 1;
}

/*
 * Lets time pass: a rest long enough shows the previews, an absence long enough hides them.
 */
void
kwl_apps_bar_tick(
	struct kwl_server *server)
{
	struct kwl_apps_bar *state;
	struct apps_view view;
	uint64_t now;
	int built;

	/* A drag and drop's rest on the icons or the previews (spring-loading). */
	state = &server->apps_bar;
	if (server->dnd_active)
		spring_tick(server);

	/* Nothing waits while idle. */
	if (state->state == KWL_APPS_IDLE)
		return;

	/* The icons of its bar gone (a window docked, Home, Wiseview, a fullscreen window, the head): nothing shows. */
	built = kwl_apps_view_build_on(server, state->output, &view);
	if (!built) {
		kwl_apps_bar_hide(server, "away");
		return;
	}

	/* The rest on an icon. */
	now = kwl_milliseconds();
	if (state->state == KWL_APPS_ARMED && now - state->since_ms >= HOVER_MS) {
		kwl_apps_bar_show(server, &view, state->key, KWL_APPS_VIA_HOVER);
		return;
	}

	/* The absence from the icons and the panel hides what the rest showed (a click's previews stay). */
	if (state->state == KWL_APPS_SHOWN &&
	    (state->via == KWL_APPS_VIA_HOVER || state->via == KWL_APPS_VIA_SPRING) &&
	    state->left &&
	    now - state->since_ms >= LEAVE_MS)
		kwl_apps_bar_hide(server, "leave");
}

/*
 * Follows a drag and drop over the bars (data.c, before it finds the
 * drag's target): the rest on an application's icon, or on a preview of
 * the previews spring-loading showed.  Returns 1 while the drag is on an
 * icon or those previews (the drag's mark then says nothing), 0 elsewhere.
 */
int
kwl_apps_bar_drag_motion(
	struct kwl_server *server)
{
	struct kwl_apps_bar *state;
	struct apps_view view;
	struct apps_panel panel;
	uint64_t now;
	int built;
	int slot;
	int tile;
	int same;
	int in_panel;

	/* The icons of the bar the pointer is on, and what is under it. */
	state = &server->apps_bar;
	now = kwl_milliseconds();
	slot = SLOT_NONE;
	built = kwl_apps_view_build_on(server, server->pointer_output, &view);
	if (built)
		slot = slot_at(&view, server->pointer_x, server->pointer_y);

	/* The previews spring-loading showed, and whether the pointer is over them. */
	in_panel = 0;
	if (state->state == KWL_APPS_SHOWN && state->via == KWL_APPS_VIA_SPRING) {
		built = kwl_apps_view_build_on(server, state->output, &view);
		if (built)
			built = panel_build(server, &view, state->key, &panel);
		if (built)
			in_panel = kwl_apps_inside(&panel.rect, server->pointer_x, server->pointer_y);
	}

	/* An application's icon: a new one starts the rest again. */
	if (slot >= 0) {
		built = kwl_apps_view_build_on(server, server->pointer_output, &view);
		same = strcmp(view.apps.apps[slot].key, state->spring_key);
		if (same != 0) {
			(void)snprintf(state->spring_key, sizeof(state->spring_key), "%s", view.apps.apps[slot].key);
			state->spring_since_ms = now;
			state->spring_done = 0;
			state->output = view.output;
			server->dirty = 1;
		}

		/* On an icon, no preview rests. */
		state->spring_tile = -1;
		state->left = 0;
		return 1;
	}

	/* The icon is left: its rest is over. */
	if (state->spring_key[0] != '\0') {
		state->spring_key[0] = '\0';
		state->spring_done = 0;
		server->dirty = 1;
	}

	/* A preview: a new one starts its rest. */
	if (in_panel) {
		tile = kwl_apps_tile_at(panel.tiles, panel.count, server->pointer_x, server->pointer_y);
		if (tile != state->spring_tile) {
			state->spring_tile = tile;
			state->spring_tile_since_ms = now;
		}

		/* The previews stay while the drag is over them. */
		state->left = 0;
		server->dirty = 1;
		return 1;
	}

	/* Away from both: the previews go after LEAVE_MS (kwl_apps_bar_tick). */
	state->spring_tile = -1;
	if (state->state == KWL_APPS_SHOWN && state->via == KWL_APPS_VIA_SPRING && !state->left) {
		state->left = 1;
		state->since_ms = now;
	}

	/* Succeeded: the drag is not on the bar. */
	return 0;
}

/*
 * Ends spring-loading with the drag and drop (dropped, given up): the rest
 * is forgotten and the previews it showed go.
 */
void
kwl_apps_bar_drag_end(
	struct kwl_server *server)
{
	struct kwl_apps_bar *state;

	/* The rests. */
	state = &server->apps_bar;
	state->spring_key[0] = '\0';
	state->spring_done = 0;
	state->spring_tile = -1;

	/* The previews spring-loading showed. */
	if (state->state == KWL_APPS_SHOWN && state->via == KWL_APPS_VIA_SPRING)
		kwl_apps_bar_hide(server, "drag-end");
}

/*
 * Gathers the system bar's applications and their icons' places
 * (kwl_apps_view_build_on, the anchor's).  Returns 0 when the bar has no
 * room for icons now, or no application.
 */
int
kwl_apps_view_build(
	struct kwl_server *server,
	struct apps_view *view)
{
	int built;

	/* The anchor's bar. */
	built = kwl_apps_view_build_on(server, KWL_PLANE_ANCHOR, view);
	if (!built)
		return 0;

	/* Succeeded: there are icons. */
	return 1;
}

/*
 * Gathers an output's bar's applications and their icons' places (the
 * system bar's, or a head's, ws113-p015).  Returns 0 when the bar has no
 * room for icons now, or no application.
 */
int
kwl_apps_view_build_on(
	struct kwl_server *server,
	unsigned slot,
	struct apps_view *view)
{
	unsigned slots;
	int32_t top;
	int room;
	int collected;

	/* Room in the bar. */
	room = kwl_glass_apps_room(server, slot, &view->left, &view->right, &top);
	if (!room)
		return 0;

	/* The applications. */
	collected = view_collect_on(server, slot, 0, view);
	if (!collected)
		return 0;
	view->top = top;

	/* As many icons as there is room for; the last place says how many more when they do not fit. */
	slots = 0;
	if (view->right > view->left)
		slots = (unsigned)((view->right - view->left) / ICON_WIDTH);
	if (slots == 0U)
		return 0;
	view->shown = view->apps.count;
	view->hidden = 0;
	if (view->apps.count > slots) {
		view->shown = slots - 1U;
		view->hidden = view->apps.count - view->shown;
	}

	/* The current application only when its icon is shown. */
	if (view->current >= (int)view->shown)
		view->current = -1;

	/* Succeeded: there are icons. */
	return 1;
}

/*
 * Gathers the applications of the desktop shown (in its bar order) and the
 * application of the window on top, without the icons' places, for the
 * switcher in the middle of the anchor: every window of the desktop, on
 * every display.  Returns 0 when there is none.
 */
int
kwl_apps_view_collect(
	struct kwl_server *server,
	struct apps_view *view)
{
	int collected;

	/* The anchor's. */
	collected = view_collect_on(server, KWL_PLANE_ANCHOR, 1, view);
	if (!collected)
		return 0;

	/* Succeeded: there are applications. */
	return 1;
}

/*
 * Gathers the applications of the desktop shown for an output: of the
 * windows on it for its bar (ws113-p015), or of every window (every, the
 * switcher's); the application of the window on top (the output's, or the
 * desktop's for every window); the output's rectangle (the previews' room)
 * and its bar's top.  Returns 0 when there is none.
 */
static int
view_collect_on(
	struct kwl_server *server,
	unsigned slot,
	int every,
	struct apps_view *view)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *parent;
	struct kwl_object *top;
	struct kwl_apps_window *window;
	unsigned index;
	unsigned app;
	int desktop_surface;

	/* The output and its bar's top. */
	(void)kwl_output_rect(server, slot, &view->area);
	view->output = slot;
	view->top = view->area.y;

	/* A desktop that keeps an order. */
	view->shown = 0;
	view->hidden = 0;
	view->current = -1;
	view->window_count = 0;
	view->apps.count = 0;
	if (server->desktop >= KWL_APPS_DESKTOPS)
		return 0;

	/* The windows of the desktop shown, as Wiseview has them (a sheet comes with its parent). */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a window with an image, of the desktop shown. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->role->top == NULL ||
			    surface->cursor_role ||
			    surface->current == NULL ||
			    surface->desktop != server->desktop)
				continue;
			parent = kwl_sheet_parent(surface);
			if (parent != NULL || view->window_count >= VIEW_WINDOWS)
				continue;

			/* A bar has the windows on its display alone. */
			if (!every && surface->output != slot)
				continue;

			/* The desktop's icons are no application (desktop.c). */
			desktop_surface = kwl_desktop_is(surface);
			if (desktop_surface)
				continue;

			/* Described for apps.c. */
			window = &view->described[view->window_count];
			window->app_id = surface->app_id;
			window->client = client->number;
			window->open_order = surface->open_order;
			window->map_order = surface->map_order;
			window->minimized = surface->minimized;
			view->surfaces[view->window_count] = surface;
			view->window_count++;
		}
	}

	/* The applications, in the desktop's order. */
	kwl_apps_build(view->described, view->window_count, &server->apps_bar.orders[server->desktop], &view->apps);
	if (view->apps.count == 0U)
		return 0;

	/* The application of the window on top: of the desktop for every window, of the display for its bar. */
	top = kwl_top_window(server);
	if (!every)
		top = kwl_output_top_window(server, slot);
	parent = kwl_sheet_parent(top);
	if (parent != NULL)
		top = parent;
	for (app = 0; app < view->apps.count && top != NULL; app++) {
		for (index = 0; index < view->apps.apps[app].window_count; index++) {
			if (view->surfaces[view->apps.apps[app].windows[index]] == top)
				view->current = (int)app;
		}
	}

	/* Succeeded: there are applications. */
	return 1;
}

/* Tells which slot of the bar is under a point: an application's index, SLOT_MORE, or SLOT_NONE. */
static int
slot_at(
	const struct apps_view *view,
	int32_t x,
	int32_t y)
{
	int32_t slot;

	/* Only in the bar, in the icons' span. */
	if (y < view->top ||
	    y >= view->top + KWL_GLASS_BAR ||
	    x < view->left)
		return SLOT_NONE;
	slot = (x - view->left) / ICON_WIDTH;

	/* An application's icon, or the "+N" place after them. */
	if (slot < (int32_t)view->shown)
		return (int)slot;
	if (slot == (int32_t)view->shown && view->hidden > 0U)
		return SLOT_MORE;

	/* Succeeded: nothing there. */
	return SLOT_NONE;
}

/* Gives a slot's place in the bar. */
static void
slot_rect(
	const struct apps_view *view,
	unsigned slot,
	struct apps_rect *rect)
{
	/* One after the other from the left of the span, the bar's height. */
	rect->x = view->left + (int32_t)slot * ICON_WIDTH;
	rect->y = view->top;
	rect->width = ICON_WIDTH;
	rect->height = KWL_GLASS_BAR;
}

/*
 * Lays out the previews of an application under its icon, for the switcher (switcher-shell.c).  Returns 0 when it has no icon.
 */
int
kwl_apps_bar_panel(
	struct kwl_server *server,
	const struct apps_view *view,
	const char *key,
	struct apps_panel *panel)
{
	int built;

	/* As the bar's own. */
	built = panel_build(server, view, key, panel);
	if (!built)
		return 0;

	/* Succeeded: laid out. */
	return 1;
}

/* Lays out the previews of an application under its icon (kwl_apps_tiles_layout).  Returns 0 when the application has no icon. */
static int
panel_build(
	struct kwl_server *server,
	const struct apps_view *view,
	const char *key,
	struct apps_panel *panel)
{
	struct apps_rect icon;
	int32_t center;
	unsigned index;
	int found;

	/* The application, among those with an icon. */
	found = kwl_apps_find(&view->apps, key);
	if (found < 0 || found >= (int)view->shown)
		return 0;
	kwl_apps_tiles_layout(server, view, (unsigned)found, panel);

	/* The panel under the icon, inside the output. */
	slot_rect(view, (unsigned)found, &icon);
	center = icon.x + icon.width / 2;
	panel->rect.width += 2 * PANEL_PAD;
	panel->rect.height += PANEL_PAD;
	panel->rect.x = center - panel->rect.width / 2;
	if (panel->rect.x + panel->rect.width > view->area.x + (int32_t)view->area.width - 16)
		panel->rect.x = view->area.x + (int32_t)view->area.width - 16 - panel->rect.width;
	if (panel->rect.x < view->area.x + 16)
		panel->rect.x = view->area.x + 16;
	panel->rect.y = view->top + KWL_GLASS_BAR + PANEL_DROP;

	/* The previews moved into the panel. */
	for (index = 0; index < panel->count; index++) {
		panel->tiles[index].x += panel->rect.x + PANEL_PAD;
		panel->tiles[index].y += panel->rect.y + PANEL_PAD;
	}

	/* Succeeded: laid out. */
	return 1;
}

/*
 * Lays out an application's windows as previews from 0, 0: each at most a
 * quarter of the view's output's width and height (D4), keeping its shape; all
 * of them smaller together (to half) when one row is wider than the
 * output, then in more rows; under each row the room for the previews'
 * labels.  The panel's rectangle gets the size they take (no padding, no
 * place).
 */
void
kwl_apps_tiles_layout(
	struct kwl_server *server,
	const struct apps_view *view,
	unsigned found,
	struct apps_panel *panel)
{
	const struct kwl_app *app;
	uint32_t width;
	uint32_t height;
	int32_t sizes[KWL_APPS_WINDOWS][2];
	int32_t available;
	int32_t total;
	int32_t row_x;
	int32_t row_y;
	int32_t row_height;
	int32_t widest;
	float scale;
	float shrink;
	unsigned index;

	UNUSED_PARAMETER(server);

	/* The application and its windows. */
	app = &view->apps.apps[found];
	panel->app = (int)found;
	panel->count = app->window_count;

	/* Each window's size at most a quarter of the output's width and height. */
	total = 0;
	for (index = 0; index < panel->count; index++) {
		panel->surfaces[index] = view->surfaces[app->windows[index]];
		kwl_surface_size(panel->surfaces[index], &width, &height);
		if (width == 0U || height == 0U) {
			width = 640U;
			height = 400U;
		}

		/* The quarter's scale, never larger than the window. */
		scale = (float)view->area.width / 4.0f / (float)width;
		if ((float)view->area.height / 4.0f / (float)height < scale)
			scale = (float)view->area.height / 4.0f / (float)height;
		if (scale > 1.0f)
			scale = 1.0f;
		sizes[index][0] = (int32_t)((float)width * scale);
		sizes[index][1] = (int32_t)((float)height * scale);
		total += sizes[index][0] + PREVIEW_GAP;
	}

	/* All smaller together when one row is wider than the room, to half at most. */
	available = (int32_t)view->area.width - 32 - 2 * PANEL_PAD;
	total -= PREVIEW_GAP;
	if (total > available) {
		shrink = (float)available / (float)total;
		if (shrink < 0.5f)
			shrink = 0.5f;
		for (index = 0; index < panel->count; index++) {
			sizes[index][0] = (int32_t)((float)sizes[index][0] * shrink);
			sizes[index][1] = (int32_t)((float)sizes[index][1] * shrink);
		}
	}

	/* Left to right, a new row when the room runs out. */
	row_x = 0;
	row_y = 0;
	row_height = 0;
	widest = 0;
	for (index = 0; index < panel->count; index++) {
		if (row_x > 0 && row_x + sizes[index][0] > available) {
			row_y += row_height + PREVIEW_LABEL;
			row_x = 0;
			row_height = 0;
		}

		/* The preview in its row. */
		panel->tiles[index].x = row_x;
		panel->tiles[index].y = row_y;
		panel->tiles[index].width = sizes[index][0];
		panel->tiles[index].height = sizes[index][1];
		row_x += sizes[index][0] + PREVIEW_GAP;
		if (row_x - PREVIEW_GAP > widest)
			widest = row_x - PREVIEW_GAP;
		if (sizes[index][1] > row_height)
			row_height = sizes[index][1];
	}

	/* The size they take, with the last row's labels. */
	panel->rect.x = 0;
	panel->rect.y = 0;
	panel->rect.width = widest;
	panel->rect.height = row_y + row_height + PREVIEW_LABEL;
}

/*
 * Tells which preview is under a point, or -1.
 */
int
kwl_apps_tile_at(
	const struct apps_rect *tiles,
	unsigned count,
	int32_t x,
	int32_t y)
{
	unsigned index;
	int hit;

	/* Each preview. */
	for (index = 0; index < count; index++) {
		hit = kwl_apps_inside(&tiles[index], x, y);
		if (hit)
			return (int)index;
	}

	/* Succeeded: none. */
	return -1;
}

/*
 * Tells whether a point is inside a rectangle.
 */
int
kwl_apps_inside(
	const struct apps_rect *rect,
	int32_t x,
	int32_t y)
{
	/* Within both ranges. */
	if (x < rect->x || x >= rect->x + rect->width)
		return 0;
	if (y < rect->y || y >= rect->y + rect->height)
		return 0;
	return 1;
}

/*
 * Shows an application's previews, and says where they are.
 */
void
kwl_apps_bar_show(
	struct kwl_server *server,
	const struct apps_view *view,
	const char *key,
	unsigned via)
{
	struct kwl_apps_bar *state;
	struct apps_panel panel;
	const char *how;
	unsigned index;
	int built;

	/* The application's panel, if it has an icon. */
	state = &server->apps_bar;
	built = panel_build(server, view, key, &panel);
	if (!built) {
		kwl_apps_bar_hide(server, "gone");
		return;
	}

	/* Shown, under the bar of the view's output. */
	state->output = view->output;
	state->state = KWL_APPS_SHOWN;
	state->via = via;
	state->left = 0;
	(void)snprintf(state->key, sizeof(state->key), "%s", key);
	server->dirty = 1;

	/* Logged with each preview's place. */
	how = "hover";
	if (via == KWL_APPS_VIA_CLICK)
		how = "click";
	if (via == KWL_APPS_VIA_SWITCH)
		how = "switch";
	if (via == KWL_APPS_VIA_SPRING)
		how = "spring";
	printf("KWL APPS preview app=%s windows=%u via=%s at_ms=%llu\n", key, panel.count, how, (unsigned long long)kwl_milliseconds());
	for (index = 0; index < panel.count; index++)
		printf("KWL APPS preview window surface=%u x=%d y=%d width=%d height=%d client=%llu\n", panel.surfaces[index]->id, panel.tiles[index].x, panel.tiles[index].y, panel.tiles[index].width, panel.tiles[index].height, (unsigned long long)panel.surfaces[index]->client->number);
}

/*
 * Hides the previews (or stops the wait for them).
 */
void
kwl_apps_bar_hide(
	struct kwl_server *server,
	const char *why)
{
	struct kwl_apps_bar *state;

	/* Logged only when they showed. */
	state = &server->apps_bar;
	if (state->state == KWL_APPS_SHOWN)
		printf("KWL APPS preview close via=%s\n", why);

	/* Idle. */
	state->state = KWL_APPS_IDLE;
	state->key[0] = '\0';
	state->left = 0;
	server->dirty = 1;
}

/* Logs the bar's applications and their icons' places when they changed. */
static void
log_bar(
	struct kwl_server *server,
	const struct apps_view *view)
{
	struct kwl_apps_bar *state;
	struct apps_rect rect;
	char line[sizeof(server->apps_bar.logged[0])];
	const char *separator;
	size_t used;
	unsigned index;
	int same;
	int length;

	/* The line: the count, and the applications from the left. */
	state = &server->apps_bar;
	length = snprintf(line, sizeof(line), "count=%u hidden=%u desktop=%u apps=", view->apps.count, view->hidden, server->desktop + 1U);
	used = 0;
	if (length > 0)
		used = (size_t)length;
	for (index = 0; index < view->shown && used < sizeof(line); index++) {
		separator = ",";
		if (index == 0U)
			separator = "";
		length = snprintf(line + used, sizeof(line) - used, "%s%s", separator, view->apps.apps[index].key);
		if (length > 0)
			used += (size_t)length;
	}

	/* Unchanged on this bar: nothing to say. */
	same = strcmp(line, state->logged[view->output]);
	if (same == 0)
		return;
	(void)snprintf(state->logged[view->output], sizeof(state->logged[view->output]), "%s", line);

	/* The bar, then each icon; a head's lines name its output. */
	if (view->output == KWL_PLANE_ANCHOR) {
		printf("KWL APPS bar %s\n", line);
	} else {
		printf("KWL APPS bar %s output=%u\n", line, view->output);
	}

	/* Each icon's place. */
	for (index = 0; index < view->shown; index++) {
		slot_rect(view, index, &rect);
		if (view->output == KWL_PLANE_ANCHOR) {
			printf("KWL APPS icon app=%s x=%d y=%d width=%d height=%d windows=%u\n", view->apps.apps[index].key, rect.x, rect.y, rect.width, rect.height, view->apps.apps[index].window_count);
		} else {
			printf("KWL APPS icon app=%s x=%d y=%d width=%d height=%d windows=%u output=%u\n", view->apps.apps[index].key, rect.x, rect.y, rect.width, rect.height, view->apps.apps[index].window_count, view->output);
		}
	}
}

/* Draws the "+N" place: how many applications have no room. */
static void
draw_more(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct apps_rect *rect,
	unsigned hidden,
	float light)
{
	struct glass_bar_colours colours;
	char text[16];
	int32_t width;

	/* A light rounded square the shape of the tiles (the bar's lit colour), lit while pressed. */
	kwl_glass_bar_colours(server, &colours);
	if (light > 0.0f)
		draw_light(server, command, rect, light);
	glass_draw_solid(server, command, (float)(rect->x + (ICON_WIDTH - ICON_MARK) / 2), (float)(rect->y + KWL_GLASS_BAR / 2 - ICON_MARK / 2), (float)ICON_MARK, (float)ICON_MARK, (float)ICON_MARK * GLASS_ICON_TILE_RADIUS, colours.lit);

	/* The count in its middle. */
	(void)snprintf(text, sizeof(text), "+%u", hidden);
	width = glass_text_width(server, SIZE_BAR, text);
	glass_draw_text(server, command, SIZE_BAR, rect->x + ICON_WIDTH / 2 - width / 2, rect->y + KWL_GLASS_BAR / 2 + 5, text, ICON_WIDTH, colours.ink);
}

/* Draws the light behind an icon the pointer rests on, or whose previews show. */
static void
draw_light(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct apps_rect *rect,
	float strength)
{
	struct glass_bar_colours colours;
	float colour[4];

	/* A soft rounded square of the bar's lit colour behind the mark, the shape of the tiles, inside the pill. */
	kwl_glass_bar_colours(server, &colours);
	memcpy(colour, colours.lit, sizeof(colour));
	colour[3] = colours.lit[3] * strength;
	glass_draw_solid(server, command, (float)(rect->x + 2), (float)(rect->y + KWL_GLASS_BAR / 2 - ICON_WIDTH / 2 + 2), (float)(ICON_WIDTH - 4), (float)(ICON_WIDTH - 4), (float)(ICON_WIDTH - 4) * GLASS_ICON_TILE_RADIUS, colour);
}

/*
 * Lets a drag and drop's rest bring a window forward: SPRING_MS on an
 * application's icon switches to its one window, or shows the previews of
 * its windows; SPRING_MS on one of those previews switches to that window.
 */
static void
spring_tick(
	struct kwl_server *server)
{
	struct kwl_apps_bar *state;
	struct kwl_object *surface;
	struct apps_view view;
	struct apps_panel panel;
	const struct kwl_app *app;
	uint64_t now;
	int found;
	int built;

	/* A rest on an icon not answered yet lights it more at each frame. */
	state = &server->apps_bar;
	now = kwl_milliseconds();
	if (state->spring_key[0] != '\0' && !state->spring_done)
		server->dirty = 1;

	/* A rest on an icon long enough, not answered yet. */
	if (state->spring_key[0] != '\0' &&
	    !state->spring_done &&
	    now - state->spring_since_ms >= SPRING_MS) {
		state->spring_done = 1;
		built = kwl_apps_view_build_on(server, state->output, &view);
		found = -1;
		if (built)
			found = kwl_apps_find(&view.apps, state->spring_key);
		if (found < 0)
			return;

		/* One window: it comes forward. */
		app = &view.apps.apps[found];
		if (app->window_count == 1U) {
			surface = view.surfaces[app->windows[0]];
			kwl_apps_bar_hide(server, "spring");
			kwl_glass_switch_to(server, surface, "spring");
			printf("KWL APPS spring app=%s surface=%u\n", app->key, surface->id);
			return;
		}

		/* More: their previews show. */
		kwl_apps_bar_show(server, &view, app->key, KWL_APPS_VIA_SPRING);
		printf("KWL APPS spring app=%s previews=%u\n", app->key, app->window_count);
		return;
	}

	/* A rest on a preview long enough: its window comes forward. */
	if (state->state != KWL_APPS_SHOWN || state->via != KWL_APPS_VIA_SPRING)
		return;
	if (state->spring_tile < 0 || now - state->spring_tile_since_ms < SPRING_MS)
		return;
	built = kwl_apps_view_build_on(server, state->output, &view);
	if (built)
		built = panel_build(server, &view, state->key, &panel);
	if (!built || state->spring_tile >= (int)panel.count)
		return;

	/* Succeeded: the window, with the previews gone. */
	surface = panel.surfaces[state->spring_tile];
	state->spring_tile = -1;
	printf("KWL APPS spring app=%s surface=%u\n", state->key, surface->id);
	kwl_apps_bar_hide(server, "spring");
	kwl_glass_switch_to(server, surface, "spring");
}
