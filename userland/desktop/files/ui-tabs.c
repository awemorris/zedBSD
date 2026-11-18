/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tabs of a window (spec §30, design §3).  A new tab opens beside the
 * one shown, at the place given; closing the last tab closes the window.
 * While the window has two tabs or more, a row of tabs runs across the top
 * of the content panel, which they switch: equal shares of its width, the
 * names centred, the shown tab lit with blue text, a short blue underline
 * and a brighter ground, the others plain text.  A tab's close button shows
 * on the shown tab and under the pointer.  The sidebar and the preview do
 * not move.
 */

#include "files.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The row's height (about 2.2 times the text's) and the room at its ends. */
#define TABS_HEIGHT		30
#define TABS_INSET		8

/* The text's size, and the room kept at both ends of a name (for the close button). */
#define TABS_TEXT		13U
#define TABS_PADDING		28

/* The close button's size and its distance from the tab's right end. */
#define TABS_CLOSE		16
#define TABS_CLOSE_RIGHT	8

/* The underline of the shown tab: its thickness, its width past the name's, and its gap above the row's foot. */
#define TABS_LINE		2
#define TABS_LINE_OVER		6
#define TABS_LINE_ABOVE		2

/* The corners of a lit tab's ground. */
#define TABS_RADIUS		8.0f

/*
 * The shown tab's ground and a tab's under the pointer: brighter than the
 * panel (white on glass, a faint blue-grey on the opaque white panel), and
 * the thin line under the row.
 */
#define TABS_COLOR_SHOWN_GLASS	KL_RGBA(0xffffff, 120)
#define TABS_COLOR_HOVER_GLASS	KL_RGBA(0xffffff, 60)
#define TABS_COLOR_SHOWN	KL_RGBA(FM_COLOR_ACCENT, 18)
#define TABS_COLOR_HOVER	KL_RGBA(0x5a6b85, 14)
#define TABS_COLOR_RULE		KL_RGBA(0x5a6b85, 40)

static void tabs_leave(struct fm_app *app);
static void tabs_shown(struct fm_app *app);
static void tabs_place(struct fm_app *app, const struct kl_rect *bar, int index, struct kl_rect *tab);
static void tabs_draw_one(struct fm_app *app, struct kl_canvas *canvas, int index, const struct kl_rect *tab);

/*
 * Opens a new tab beside the one shown, at a place, and shows it.  A
 * window already holding its most tabs says so and stays as it is.
 */
void
fm_tabs_new(
	struct fm_app *app,
	const struct fm_location *location)
{
	struct fm_tab *tab;
	int index;

	/* A window holds eight tabs at most. */
	if (app->tab_count == FM_TABS) {
		fm_ui_message(app, "A window holds at most 8 tabs.");
		return;
	}

	/* The new tab, with nothing selected yet. */
	tab = calloc(1, sizeof(*tab));
	if (tab == NULL) {
		fm_ui_message(app, "There is not enough memory for a new tab.");
		return;
	}

	/* Nothing has the cursor yet. */
	tab->cursor = -1;
	tab->anchor = -1;

	/* It goes right after the one shown, which the tabs after it make room for. */
	index = app->tab_index + 1;
	memmove(&app->tabs[index + 1], &app->tabs[index], sizeof(app->tabs[0]) * (size_t)(app->tab_count - index));
	app->tabs[index] = tab;
	app->tab_count++;

	/* The window leaves what it had open on the old tab, and the new tab goes to the place. */
	tabs_leave(app);
	app->tab_index = index;
	fm_ui_go(app, location);
	fm_log("TABS new index=%d count=%d", app->tab_index, app->tab_count);
}

/*
 * Opens a new tab at the place the shown tab is at.
 */
void
fm_tabs_duplicate(
	struct fm_app *app)
{
	struct fm_location location;
	struct fm_tab *tab;

	/* The place of the tab shown, copied before the tab list moves. */
	tab = fm_ui_tab(app);
	location = tab->history[tab->history_index].location;
	fm_tabs_new(app, &location);
}

/*
 * Closes a tab.  The last tab closes the window instead; otherwise the tab
 * to its right is shown (or the one to its left when it was the last).
 */
void
fm_tabs_close(
	struct fm_app *app,
	int index)
{
	struct fm_tab *tab;

	/* A tab that is not there. */
	if (index < 0 || index >= app->tab_count)
		return;

	/* The last tab closes the window. */
	if (app->tab_count == 1) {
		app->request = FM_REQUEST_CLOSE;
		return;
	}

	/* The tab and its listing go, and the tabs after it move down. */
	tab = app->tabs[index];
	fm_dir_free(&tab->listing);
	free(tab);
	memmove(&app->tabs[index], &app->tabs[index + 1], sizeof(app->tabs[0]) * (size_t)(app->tab_count - index - 1));
	app->tab_count--;
	app->tabs[app->tab_count] = NULL;

	/* The tab shown stays shown, one place to the left when it was after the closed one. */
	if (app->tab_index > index)
		app->tab_index--;

	/* The closed tab was shown: the one that took its place is (or the last). */
	if (app->tab_index == index) {
		if (app->tab_index == app->tab_count)
			app->tab_index--;
		tabs_shown(app);
	}

	/* The frame changes. */
	app->dirty = 1;
	fm_log("TABS close index=%d count=%d shown=%d", index, app->tab_count, app->tab_index);
}

/*
 * Shows a tab.
 */
void
fm_tabs_select(
	struct fm_app *app,
	int index)
{
	/* A tab that is not there, or the one already shown. */
	if (index < 0 || index >= app->tab_count || index == app->tab_index)
		return;

	/* The tab, read again. */
	app->tab_index = index;
	tabs_shown(app);
	app->dirty = 1;
	fm_log("TABS select index=%d count=%d", app->tab_index, app->tab_count);
}

/*
 * Shows the next tab (step 1) or the previous (step -1), going around at
 * the ends.
 */
void
fm_tabs_step(
	struct fm_app *app,
	int step)
{
	int index;

	/* One tab has nothing beside it. */
	if (app->tab_count < 2)
		return;

	/* The tab beside the one shown, around the ends. */
	index = (app->tab_index + step + app->tab_count) % app->tab_count;
	fm_tabs_select(app, index);
}

/*
 * Places the row of tabs at the top of the content panel, when the window
 * has two tabs or more, and returns how far the content's items move down
 * under it (the panel itself keeps its place and takes the row in).
 */
int
fm_tabs_layout(
	struct fm_app *app,
	int top,
	int left,
	int right)
{
	struct kl_rect *bar;

	/* No row for one tab. */
	bar = &app->layout.tabbar;
	memset(bar, 0, sizeof(*bar));
	if (app->tab_count < 2)
		return 0;

	/* The row across the panel's top. */
	bar->x = left;
	bar->y = top;
	bar->width = right - left;
	bar->height = TABS_HEIGHT;
	return TABS_HEIGHT;
}

/*
 * Draws the row of tabs, when there is one, over the top of the content
 * panel (drawn before it): a thin line under the row, then each tab.
 */
void
fm_tabs_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	const struct kl_rect *bar;
	struct kl_rect rule;
	struct kl_rect tab;
	int index;

	/* No row for one tab. */
	bar = &app->layout.tabbar;
	if (bar->width <= 0)
		return;

	/* The thin line between the row and the items, inside the panel's ends. */
	rule.x = bar->x + TABS_INSET;
	rule.y = bar->y + bar->height - 1;
	rule.width = bar->width - 2 * TABS_INSET;
	rule.height = 1;
	kl_canvas_fill(canvas, &rule, TABS_COLOR_RULE);

	/* Each tab, left to right. */
	for (index = 0; index < app->tab_count; index++) {
		tabs_place(app, bar, index, &tab);
		tabs_draw_one(app, canvas, index, &tab);
	}
}

/*
 * Makes a tab the one shown: whatever was open on the tab before closes
 * and the tab's place is read again, since it may have changed while the
 * tab was hidden.
 */
static void
tabs_shown(
	struct fm_app *app)
{
	/* What was open closes. */
	tabs_leave(app);

	/* The tab's place read again (a search tab's search starts again, another's stops). */
	fm_ui_reload(app, fm_ui_tab(app));
}

/*
 * Closes what was open on the tab being left: Quick Look, the information
 * and a name being changed; typing goes back to the items.
 */
static void
tabs_leave(
	struct fm_app *app)
{
	/* A name being changed keeps what was typed. */
	if (app->focus == FM_FOCUS_RENAME)
		fm_action_rename_end(app, 1);

	/* Quick Look and the information close, and typing goes to the items. */
	fm_look_close(app);
	fm_info_close(app);
	app->focus = FM_FOCUS_CONTENT;
	app->band = 0;
	app->typed_length = 0;
}

/* Places one tab of the row: the tabs share the row's width inside its ends, the last taking what is left over. */
static void
tabs_place(
	struct fm_app *app,
	const struct kl_rect *bar,
	int index,
	struct kl_rect *tab)
{
	int room;
	int left;
	int right;

	/* The row's room, and the tab's ends in it. */
	room = bar->width - 2 * TABS_INSET;
	left = bar->x + TABS_INSET + room * index / app->tab_count;
	right = bar->x + TABS_INSET + room * (index + 1) / app->tab_count;

	/* The tab, the height of the row less its line. */
	tab->x = left;
	tab->y = bar->y + 3;
	tab->width = right - left;
	tab->height = bar->height - 5;
}

/*
 * Draws one tab: its ground when it is shown or under the pointer, the
 * name of its place centred (blue with a short blue underline for the
 * shown tab), and its close button on the shown tab and under the pointer.
 */
static void
tabs_draw_one(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int index,
	const struct kl_rect *tab)
{
	const struct fm_location *location;
	const struct fm_tab *shown;
	struct kl_rect close;
	struct kl_rect line;
	char name[FM_NAME_MAX];
	kl_color ground;
	kl_color ink;
	int text_width;
	int baseline;
	int hovered;
	int current;
	int left;

	/* Whether the tab is the one shown, and whether it or its close button is under the pointer. */
	current = 0;
	if (index == app->tab_index)
		current = 1;
	hovered = 0;
	if (app->hover_kind == FM_HIT_TAB && app->hover_index == index)
		hovered = 1;
	if (app->hover_kind == FM_HIT_TAB_CLOSE && app->hover_index == index)
		hovered = 1;

	/* The ground: brighter for the shown tab, a little for one under the pointer, none for the rest. */
	ground = 0;
	ink = FM_COLOR_TEXT_SECONDARY;
	if (current != 0) {
		ground = TABS_COLOR_SHOWN;
		if (app->glass != 0)
			ground = TABS_COLOR_SHOWN_GLASS;
		ink = FM_COLOR_ACCENT_TEXT;
	} else if (hovered != 0) {
		ground = TABS_COLOR_HOVER;
		if (app->glass != 0)
			ground = TABS_COLOR_HOVER_GLASS;
		ink = FM_COLOR_TEXT;
	}

	/* The ground, when the tab has one. */
	if (ground != 0)
		kl_canvas_round(canvas, (float)tab->x + 2.0f, (float)tab->y, (float)tab->width - 4.0f, (float)tab->height, TABS_RADIUS, ground);

	/* The whole tab is clickable as the tab. */
	fm_ui_hit(app, tab, FM_HIT_TAB, index);

	/* The name of the tab's place, cut to fit between the ends' room, centred. */
	shown = app->tabs[index];
	location = &shown->history[shown->history_index].location;
	(void)kl_text_fit(app->text, kl_tr(fm_location_name(location, app->home)), TABS_TEXT, current, tab->width - 2 * TABS_PADDING, name, sizeof(name));
	text_width = kl_text_width(app->text, name, strlen(name), TABS_TEXT, current);
	left = tab->x + (tab->width - text_width) / 2;
	baseline = kl_text_center(TABS_TEXT, tab->y, tab->height);
	(void)kl_text_draw(app->text, canvas, left, baseline, name, strlen(name), TABS_TEXT, current, ink);

	/* The shown tab's short blue underline, a little wider than its name, at the row's foot. */
	if (current != 0) {
		line.x = left - TABS_LINE_OVER;
		line.y = tab->y + tab->height + TABS_LINE_ABOVE - TABS_LINE;
		line.width = text_width + 2 * TABS_LINE_OVER;
		line.height = TABS_LINE;
		kl_canvas_round(canvas, (float)line.x, (float)line.y, (float)line.width, (float)line.height, 1.0f, FM_COLOR_ACCENT);
	}

	/* The close button at the right end, clickable always, shown on the shown tab and under the pointer. */
	close.x = tab->x + tab->width - TABS_CLOSE_RIGHT - TABS_CLOSE;
	close.y = tab->y + (tab->height - TABS_CLOSE) / 2;
	close.width = TABS_CLOSE;
	close.height = TABS_CLOSE;
	fm_ui_hit(app, &close, FM_HIT_TAB_CLOSE, index);
	if (current == 0 && hovered == 0)
		return;

	/* libkeiland's quiet round button, lit under the pointer. */
	fm_icon_button(app, canvas, FM_WIDGET_TAB_CLOSE, index, &close, KL_ICON_CLOSE, TABS_CLOSE - 8, KL_BUTTON_ROUND | KL_BUTTON_QUIET);
}
