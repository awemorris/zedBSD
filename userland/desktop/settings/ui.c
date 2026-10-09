/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface of Settings (plan/ws089/design.md section 3): the list of
 * pages on the left and the page on the right, each a card floating on
 * the compositor's frosted glass; the history of pages that the titlebar's Back,
 * Forward, Home and breadcrumb walk; the pointer, the wheel, the keys, and a
 * finger's drag on the touch screen, which scrolls the pane it holds.
 *
 * A frame is drawn whole whenever something changed.  Drawing records the
 * clickable regions, and the next input is matched against them.
 */

#include "settings.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The panes reach the window's edges, so that their outer edges line up
 * with the floating titlebar's and the titlebar's gap above them is
 * the compositor's alone (ws090-p021); on glass they stand apart by the compositor's
 * gap between the titlebar and the window (the file manager's
 * measurements, ws071-p017).
 */
#define UI_GAP			10
#define UI_GLASS_GAP		8

/* The longest log line written (a longer one is cut short). */
#define UI_LOG_LINE_MAX		1024

/* The margin round a lit region drawn again alone (its edge and shadow, BUG-226). */
#define UI_DAMAGE_MARGIN	6

/* The list of pages' width, narrower in a narrow window, and the width that counts as narrow. */
#define UI_SIDEBAR_WIDTH	248
#define UI_SIDEBAR_NARROW	208
#define UI_NARROW_WINDOW	900

/* The panes' corners, and the list's rows and the lines between its groups. */
#define UI_PANEL_RADIUS		16.0f
#define UI_ROW_HEIGHT		34
#define UI_ROW_GAP		2
#define UI_GROUP_GAP		13

/* The page's margins inside its pane, and the space between its header and its cards. */
#define UI_PAGE_SIDE		32
#define UI_PAGE_TOP		28
#define UI_PAGE_BOTTOM		32
#define UI_PAGE_HEADER_GAP	20

/* The text size of the list. */
#define UI_TEXT_ROW		15U

/* How far a key's page step leaves of the page on screen, in pixels. */
#define UI_PAGE_OVERLAP		60

/* How far the arrows scroll the page, in pixels. */
#define UI_KEY_STEP		48

/* How far a finger moves up or down before its press is a scroll rather than a tap, in pixels. */
#define UI_TOUCH_SLOP		10

static void ui_layout(struct se_app *app);
static void ui_draw_sidebar(struct se_app *app, struct kl_canvas *canvas);
static void ui_reveal(struct se_app *app, const struct kl_rect *current);
static void ui_log_controls(struct se_app *app);
static void ui_draw_page(struct se_app *app, struct kl_canvas *canvas);
static int ui_hit_at(struct se_app *app, int x, int y, unsigned *kind, int *index);
static void ui_motion(struct se_app *app, const struct se_event *event);
static void ui_button(struct se_app *app, const struct se_event *event);
static void ui_click(struct se_app *app, unsigned kind, int index);
static void ui_touch_press(struct se_app *app, const struct se_event *event, unsigned kind);
static void ui_touch_move(struct se_app *app, const struct se_event *event);
static int ui_touch_release(struct se_app *app);
static const char *ui_touch_pane_word(unsigned pane);
static void ui_scroll(struct se_app *app, const struct se_event *event);
static void ui_scroll_stop(struct se_app *app, const struct se_event *event);
static int ui_scroll_by(struct se_app *app, int pane, int amount);
static void ui_kinetic_step(struct se_app *app, uint64_t now);
static int ui_kinetic_place(struct se_app *app, double position);
static int ui_pane_scroll(const struct se_app *app, int pane, int *limit, int *height);
static int ui_hit_rect(const struct se_app *app, unsigned kind, int index, struct kl_rect *rect);
static void ui_damage_add(struct se_app *app, const struct kl_rect *rect);
static void ui_clear_rect(struct kl_canvas *canvas, const struct kl_rect *rect);
static void ui_key(struct se_app *app, const struct se_event *event);
static void ui_step_page(struct se_app *app, int direction);
static void ui_scroll_page(struct se_app *app, int amount);
static void ui_back(struct se_app *app);
static void ui_forward(struct se_app *app);
static int ui_clamp(int value, int minimum, int maximum);

/*
 * Makes the interface ready, showing a page first.
 */
void
se_ui_init(
	struct se_app *app,
	struct kl_text *text,
	unsigned page)
{
	/* A page outside the table is Home. */
	if (page >= SE_PAGES)
		page = SE_PAGE_HOME;

	/* The fonts, the list shown, the focus, and nothing under the pointer. */
	app->text = text;
	app->show_sidebar = 1;
	app->focused = 1;
	app->hover_kind = SE_HIT_NONE;
	app->hover_index = -1;
	app->press_kind = SE_HIT_NONE;
	app->press_index = -1;

	/* The history starts at the first page, which the list shows. */
	app->page = page;
	app->reveal = 1;
	app->history[0] = page;
	app->history_count = 1;
	app->history_index = 0;

	/* The scroller a touch pad's fingers scroll the panes with (none: they scroll without flying), and the text fields' input (none: no field draws). */
	app->kinetic.scroller = kl_scroller_create();
	(void)se_fields_open(text);

	/* The first frame is due. */
	app->dirty = 1;
	se_log("PAGE %s", se_pages[page].word);
}

/*
 * Lets go of what the interface holds: the touch pad's scroller and the
 * text fields' input.
 */
void
se_ui_close(
	struct se_app *app)
{
	/* The scroller, when one was made. */
	if (app->kinetic.scroller != NULL)
		kl_scroller_destroy(app->kinetic.scroller);
	app->kinetic.scroller = NULL;
	app->kinetic.flying = 0;
	app->kinetic.holding = 0;

	/* The text fields' input. */
	se_fields_close();
}

/*
 * Carries out one input from the window.
 */
void
se_ui_event(
	struct se_app *app,
	const struct se_event *event)
{
	/* The time of the input, for whatever measures time. */
	app->now = event->time;

	/* Each kind of input. */
	switch (event->type) {
	case SE_EVENT_MOTION:
		se_fields_input(event);
		ui_motion(app, event);
		break;
	case SE_EVENT_BUTTON:
		/* The text fields see the press too (a click puts a field's caret). */
		se_fields_input(event);
		ui_button(app, event);
		break;
	case SE_EVENT_AXIS:
		ui_scroll(app, event);
		break;
	case SE_EVENT_AXIS_STOP:
		ui_scroll_stop(app, event);
		break;
	case SE_EVENT_LEAVE:
		/* Nothing is lit once the pointer is gone. */
		app->hover_kind = SE_HIT_NONE;
		app->hover_index = -1;
		app->dirty = 1;
		break;
	case SE_EVENT_KEY:
		ui_key(app, event);
		break;
	case SE_EVENT_TEXT:
	case SE_EVENT_TEXT_DELETE:
	case SE_EVENT_PREEDIT:
		/* An input method's text, for the field with the keyboard (widgets.c). */
		se_fields_input(event);
		app->dirty = 1;
		break;
	case SE_EVENT_FOCUS:
		/* The chosen page's row is drawn in the accent only while the window has the focus. */
		app->focused = event->focused;
		app->dirty = 1;
		break;
	case SE_EVENT_ACTION:
		se_ui_action(app, event->action);
		break;
	default:
		break;
	}
}

/*
 * Carries out an action of the menus or the keys.
 */
void
se_ui_action(
	struct se_app *app,
	uint32_t action)
{
	/* A page of the Go menu. */
	if (action >= SE_ACTION_PAGE_FIRST && action < SE_ACTION_PAGE_FIRST + SE_PAGES) {
		se_ui_go(app, action - SE_ACTION_PAGE_FIRST);
		return;
	}

	/* Each other action. */
	switch (action) {
	case SE_ACTION_CLOSE_WINDOW:
	case SE_ACTION_QUIT:
		app->request = SE_REQUEST_CLOSE;
		break;
	case SE_ACTION_BACK:
		ui_back(app);
		break;
	case SE_ACTION_FORWARD:
		ui_forward(app);
		break;
	case SE_ACTION_HOME:
		se_ui_go(app, SE_PAGE_HOME);
		break;
	case SE_ACTION_SHOW_SIDEBAR:
		/* The list of pages comes or goes; the page takes the room. */
		app->show_sidebar = !app->show_sidebar;
		app->dirty = 1;
		se_log("SIDEBAR shown=%d", app->show_sidebar);
		break;
	case SE_ACTION_MINIMIZE:
		app->request = SE_REQUEST_MINIMIZE;
		break;
	case SE_ACTION_ZOOM:
		app->request = SE_REQUEST_ZOOM;
		break;
	case SE_ACTION_ABOUT:
		se_ui_go(app, SE_PAGE_ABOUT);
		break;
	case SE_ACTION_FIND:
		se_search_focus(app);
		break;
	default:
		break;
	}
}

/*
 * Lets time pass: a page that shows the time since the machine started
 * (About) is drawn again when the minute changes.
 */
void
se_ui_tick(
	struct se_app *app,
	uint64_t now)
{
	uint64_t minute;

	/* A touch pad's scrolling that flies on moves (BUG-211). */
	ui_kinetic_step(app, now);

	/* The popup's busy ring and its idle time (ws199-p001). */
	se_dialog_tick(app, now);

	/* The minute now, and the time for whatever is drawn. */
	minute = now / 60000U;
	app->now = now;
	if (minute == app->minute)
		return;

	/* A new minute: About shows it. */
	app->minute = minute;
	if (app->page == SE_PAGE_ABOUT)
		app->dirty = 1;
}

/*
 * Shows a page: the history keeps the page being left, and anything
 * forward of it is dropped (as a browser does).  A search being shown
 * ends, so that the page is seen.
 */
void
se_ui_go(
	struct se_app *app,
	unsigned page)
{
	int index;

	/* A page outside the table changes nothing. */
	if (page >= SE_PAGES)
		return;

	/* A popup belongs to the page it opened on: going elsewhere cancels it (ws199-p001). */
	if (app->dialog.open != 0 && page != app->page)
		se_dialog_dismiss(app);

	/* The search's results give way to the page chosen, and so does the Welcome (left, not done: ws164-p002). */
	se_search_end(app);
	if (app->welcome != 0) {
		app->welcome = 0;
		app->show_sidebar = 1;
		se_log("WELCOME left page=%s", se_pages[page].word);
	}

	/*
	 * The page's controls are listed in the log again at the next frame,
	 * even when they are the same as the last page's (two pages without a
	 * control), so a test that asked for the page finds its LAYOUT line.
	 */
	app->logged_count = -1;
	app->dirty = 1;

	/* No swatch keeps the keyboard across pages (ws177-p004). */
	app->look.accent_focus = 0;

	/* The page shown stays, without a new step (said again for whoever asked it). */
	if (page == app->page) {
		se_log("PAGE %s", se_pages[page].word);
		return;
	}

	/* A full history drops its oldest step. */
	index = app->history_index + 1;
	if (index >= SE_HISTORY) {
		memmove(app->history, app->history + 1, sizeof(app->history[0]) * (SE_HISTORY - 1));
		index = SE_HISTORY - 1;
	}

	/* The page becomes the history's newest step. */
	app->history[index] = page;
	app->history_index = index;
	app->history_count = index + 1;

	/* It is shown from its top. */
	app->page = page;
	app->page_scroll = 0;
	app->reveal = 1;
	app->dirty = 1;
	se_log("PAGE %s", se_pages[page].word);
}

/*
 * Draws a frame of the window into a canvas the window's size, recording
 * its clickable regions.
 */
void
se_ui_draw(
	struct se_app *app,
	struct kl_canvas *canvas)
{
	struct kl_rect whole;
	int partial;

	/*
	 * Only the lit regions are drawn again when nothing else changed
	 * (BUG-226): every call draws, clipped to them, and the rest of the
	 * frame keeps the pixels of the last one.
	 */
	partial = 0;
	if (app->dirty == 0 && app->hover_pending != 0 && app->dialog.open == 0)
		partial = 1;
	app->hover_pending = 0;

	/*
	 * The frame is being brought up to date; drawing may ask for another
	 * (a scroll it corrected), which the main loop then draws at once.
	 */
	app->dirty = 0;

	/* The panes' places at this size, and no clickable region yet. */
	app->width = canvas->width;
	app->height = canvas->height;
	ui_layout(app);
	app->hit_count = 0;

	/* The window's ground: clear on glass (the desktop shows between the panes), else a quiet light gradient. */
	whole.x = 0;
	whole.y = 0;
	whole.width = canvas->width;
	whole.height = canvas->height;
	if (partial != 0)
		kl_canvas_clip_push(canvas, &app->hover_damage);
	if (app->glass != 0 && partial != 0) {
		ui_clear_rect(canvas, &app->hover_damage);
	} else if (app->glass != 0) {
		kl_canvas_clear(canvas);
	} else {
		kl_canvas_gradient(canvas, &whole, SE_COLOR_BACKGROUND_TOP, SE_COLOR_BACKGROUND_BOTTOM);
	}

	/* The list of pages, when shown, then the page; the text fields in their own frame of input (widgets.c, ws090-p007). */
	se_fields_begin(app->now * 1000U);
	if (app->show_sidebar != 0)
		ui_draw_sidebar(app, canvas);
	ui_draw_page(app, canvas);
	se_dialog_draw(app, canvas);
	(void)se_fields_end(app->now * 1000U);
	if (partial != 0)
		kl_canvas_clip_pop(canvas);

	/* The page's controls, in the log when they changed. */
	ui_log_controls(app);
}

/*
 * Lists the parts of the last frame that stand on the compositor's glass: the
 * list of pages and the page, into up to capacity panels.  Returns how
 * many there are.
 */
size_t
se_ui_panels(
	struct se_app *app,
	struct se_panel *panels,
	size_t capacity)
{
	const struct kl_rect *cards[2];
	size_t count;
	unsigned index;

	/* The two panes, in the order they are drawn. */
	cards[0] = &app->layout.sidebar;
	cards[1] = &app->layout.page;
	count = 0;

	/* Each pane that is shown (a hidden one has no size). */
	for (index = 0; index < 2U; index++) {
		if (cards[index]->width <= 0 || cards[index]->height <= 0)
			continue;
		if (count == capacity)
			break;
		panels[count].rect = *cards[index];
		panels[count].radius = (int)UI_PANEL_RADIUS;
		panels[count].kind = SE_PANEL_CARD;
		count++;
	}

	/* The panes shown. */
	return count;
}

/*
 * Records a clickable region of the frame being drawn; a region recorded
 * later wins where two overlap.
 */
void
se_ui_hit(
	struct se_app *app,
	const struct kl_rect *rect,
	unsigned kind,
	int index)
{
	/* Regions past the table's size are not clickable (they do not happen in practice). */
	if (app->hit_count == SE_HITS)
		return;

	/* The region, after those drawn before it. */
	app->hits[app->hit_count].rect = *rect;
	app->hits[app->hit_count].kind = kind;
	app->hits[app->hit_count].index = index;
	app->hit_count++;
}

/*
 * Tells whether a region is lit: under the pointer, or pressed.
 */
int
se_ui_lit(
	const struct se_app *app,
	unsigned kind,
	int index)
{
	/* The region under the pointer. */
	if (app->hover_kind == kind && app->hover_index == index)
		return 1;

	/* The region a press is holding down. */
	if (app->press_kind == kind && app->press_index == index)
		return 1;

	/* Neither. */
	return 0;
}

/*
 * Works out what the titlebar shows: the history's steps, the breadcrumb
 * (Settings, then the page) and whether the list of pages is shown.
 */
void
se_ui_titlebar_state(
	struct se_app *app,
	struct se_titlebar_state *state)
{
	/* Nothing yet. */
	memset(state, 0, sizeof(*state));

	/* The history's steps. */
	state->can_back = 0;
	if (app->history_index > 0)
		state->can_back = 1;

	/* A search shown can always go back to its page. */
	if (app->search.active != 0)
		state->can_back = 1;
	state->can_forward = 0;
	if (app->history_index + 1 < app->history_count)
		state->can_forward = 1;

	/* The breadcrumb: Settings, and the search or the page unless it is Home. */
	(void)snprintf(state->parts[0], sizeof(state->parts[0]), "%s", kl_tr("Settings"));
	state->part_count = 1;
	if (app->search.active != 0) {
		(void)snprintf(state->parts[1], sizeof(state->parts[1]), "%s", kl_tr("Search"));
		state->part_count = 2;
	} else if (app->page != SE_PAGE_HOME) {
		(void)snprintf(state->parts[1], sizeof(state->parts[1]), "%s", kl_tr(se_pages[app->page].name));
		state->part_count = 2;
	}

	/* The search's query, and the last request for its field to have the keyboard. */
	(void)snprintf(state->query, sizeof(state->query), "%s", app->search.query);
	state->focus_serial = app->search.focus_serial;

	/* The list of pages. */
	state->sidebar = app->show_sidebar;
}

/*
 * Carries out what was done with the titlebar.
 */
void
se_ui_titlebar(
	struct se_app *app,
	const struct se_titlebar_event *event)
{
	/* The search's text as typed searches at once (the tables are small). */
	if (event->kind == SE_TITLEBAR_CHANGED && event->id == SE_CONTROL_SEARCH) {
		se_search_set(app, event->text);
		return;
	}

	/* The search's editing ended: Enter opens the chosen result, Esc ends the search, leaving (Tab) keeps it for the keys. */
	if (event->kind == SE_TITLEBAR_DONE && event->id == SE_CONTROL_SEARCH) {
		if (event->detail == KL_TEXT_SUBMITTED) {
			(void)se_search_open_chosen(app);
		} else if (event->detail == KL_TEXT_CANCELLED) {
			se_search_end(app);
		}

		/* Nothing else is done with the field's end. */
		return;
	}

	/* Otherwise only a control chosen does anything. */
	if (event->kind != SE_TITLEBAR_ACTIVATED)
		return;

	/* Each control. */
	switch (event->id) {
	case SE_CONTROL_BACK:
		ui_back(app);
		break;
	case SE_CONTROL_FORWARD:
		ui_forward(app);
		break;
	case SE_CONTROL_HOME:
		se_ui_go(app, SE_PAGE_HOME);
		break;
	case SE_CONTROL_PATH:
		/* The breadcrumb's first part is Home; the last is the page shown (or the search). */
		if (event->detail == 0U)
			se_ui_go(app, SE_PAGE_HOME);
		break;
	case SE_CONTROL_SEARCH:
		/* The compositor gives the field the keyboard itself; the typing arrives as text. */
		break;
	case SE_CONTROL_SIDEBAR:
		se_ui_action(app, SE_ACTION_SHOW_SIDEBAR);
		break;
	default:
		break;
	}
}

/*
 * Works out what the menus show: the history's steps, the list shown and
 * the page shown.
 */
void
se_ui_menu_state(
	struct se_app *app,
	struct se_menu_state *state)
{
	/* Nothing yet. */
	memset(state, 0, sizeof(*state));

	/* The history's steps, the list and the page. */
	if (app->history_index > 0)
		state->can_back = 1;
	if (app->history_index + 1 < app->history_count)
		state->can_forward = 1;
	state->sidebar = app->show_sidebar;
	state->page = app->page;
}

/*
 * Writes one line to standard error, prefixed ZSETTINGS (the tests read
 * these lines).
 *
 * The line is made whole first and written with one write: a second
 * Settings shares the session's log while it hands its page over, and
 * pieces written one by one were interleaved with the first one's
 * ("ZSETTINGS ZSETTINGS DONE ...", T1-320).
 */
void
se_log(
	const char *format,
	...)
{
	char line[UI_LOG_LINE_MAX];
	va_list arguments;
	size_t room;
	size_t length;
	int written;

	/* The prefix and the message (cut short when it is too long), leaving room for the end of the line. */
	(void)memcpy(line, "ZSETTINGS ", sizeof("ZSETTINGS ") - 1U);
	length = sizeof("ZSETTINGS ") - 1U;
	room = sizeof(line) - length - 1U;
	va_start(arguments, format);
	written = vsnprintf(&line[length], room, format, arguments);
	va_end(arguments);
	if (written > 0 && (size_t)written < room)
		length += (size_t)written;
	else if (written > 0)
		length += room - 1U;

	/* The end of the line, and the whole line at once. */
	line[length] = '\n';
	length++;
	(void)write(STDERR_FILENO, line, length);
}

/* Works out the panes' places at the window's size. */
static void
ui_layout(
	struct se_app *app)
{
	struct se_layout *layout;
	int margin;
	int gap;
	int left;
	int width;

	/* No margin around the panes (see UI_GLASS_GAP), and the gap between them: on glass, the titlebar's. */
	margin = 0;
	gap = UI_GAP;
	if (app->glass != 0)
		gap = UI_GLASS_GAP;

	/* The list on the left, when shown, narrower in a narrow window. */
	layout = &app->layout;
	left = margin;
	memset(&layout->sidebar, 0, sizeof(layout->sidebar));
	if (app->show_sidebar != 0) {
		width = UI_SIDEBAR_WIDTH;
		if (app->width < UI_NARROW_WINDOW)
			width = UI_SIDEBAR_NARROW;
		layout->sidebar.x = left;
		layout->sidebar.y = margin;
		layout->sidebar.width = width;
		layout->sidebar.height = app->height - 2 * margin;
		left += width + gap;
	}

	/* The page takes the rest. */
	layout->page.x = left;
	layout->page.y = margin;
	layout->page.width = app->width - margin - left;
	layout->page.height = app->height - 2 * margin;
}

/* Draws the list of pages by group, the page shown lit, scrolled when it is taller than its pane. */
static void
ui_draw_sidebar(
	struct se_app *app,
	struct kl_canvas *canvas)
{
	const struct kl_rect *panel;
	const struct se_page *page;
	struct kl_rect row;
	struct kl_rect current_row;
	kl_color ink;
	kl_color glyph;
	unsigned group;
	unsigned id;
	int current;
	int bold;
	int limit;
	int y;

	/* The pane: a light veil over the compositor's glass, or a whiter card with a bright edge over the window's ground. */
	panel = &app->layout.sidebar;
	if (app->glass != 0) {
		kl_canvas_round(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, SE_COLOR_GLASS_SIDEBAR);
	} else {
		kl_canvas_round(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, SE_COLOR_PANEL);
		kl_canvas_round_border(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, 1.0f, SE_COLOR_PANEL_EDGE);
	}

	/* The pane itself is a region the wheel scrolls; the rows over it are recorded after it. */
	se_ui_hit(app, panel, SE_HIT_SIDEBAR, 0);

	/* The rows stay inside the pane. */
	kl_canvas_clip_push(canvas, panel);

	/* Each page after Home, a line between two groups; the page shown's row is kept (none for Home). */
	memset(&current_row, 0, sizeof(current_row));
	group = se_pages[SE_PAGE_HOME + 1].group;
	y = panel->y + 10 - app->sidebar_scroll;
	for (id = SE_PAGE_HOME + 1; id < SE_PAGES; id++) {
		page = &se_pages[id];

		/* A new group starts after a thin line. */
		if (page->group != group) {
			group = page->group;
			y += UI_GROUP_GAP / 2;
			kl_canvas_line(canvas, (float)panel->x + 18.0f, (float)y + 0.5f, (float)(panel->x + panel->width) - 18.0f, (float)y + 0.5f, 1.0f, SE_COLOR_SEPARATOR);
			y += UI_GROUP_GAP - UI_GROUP_GAP / 2;
		}

		/* The row, lit when it is the page shown or under the pointer. */
		row.x = panel->x + 10;
		row.y = y;
		row.width = panel->width - 20;
		row.height = UI_ROW_HEIGHT;
		current = 0;
		if (app->page == id) {
			current = 1;
			current_row = row;
		}

		/* The row's ink, and its ground: the accent's for the page shown (grey without the focus), a shade under the pointer. */
		ink = SE_COLOR_TEXT;
		glyph = SE_COLOR_ICON;
		bold = 0;
		if (current != 0 && app->focused != 0) {
			kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, SE_COLOR_SELECTION);
			ink = SE_COLOR_ACCENT_TEXT;
			glyph = SE_COLOR_ACCENT;
			bold = 1;
		} else if (current != 0) {
			kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, SE_COLOR_SELECTION_INACTIVE);
			bold = 1;
		} else if (app->hover_kind == SE_HIT_PAGE_ROW && app->hover_index == (int)id) {
			kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, SE_COLOR_HOVER);
		}

		/* The picture and the name. */
		se_glyph_draw(canvas, page->glyph, (float)row.x + 10.0f, (float)row.y + 7.0f, 20.0f, glyph);
		(void)kl_text_draw_fit(app->text, canvas, row.x + 42, kl_text_center(UI_TEXT_ROW, row.y, row.height), kl_tr(page->name), UI_TEXT_ROW, bold, row.width - 50, ink);
		se_ui_hit(app, &row, SE_HIT_PAGE_ROW, (int)id);
		y += UI_ROW_HEIGHT + UI_ROW_GAP;
	}

	/* How tall the list is, for its scroll; a list that shrank below its scroll comes back up next frame. */
	kl_canvas_clip_pop(canvas);
	app->sidebar_extent = y + app->sidebar_scroll - panel->y + 8;
	limit = app->sidebar_extent - panel->height;
	if (limit < 0)
		limit = 0;
	if (app->sidebar_scroll > limit) {
		app->sidebar_scroll = limit;
		app->dirty = 1;
	}

	/* A page just shown is scrolled into sight. */
	if (app->reveal != 0)
		ui_reveal(app, &current_row);
}

/* Scrolls the list so that the page shown's row is in sight (at the next frame), once after the page changed. */
static void
ui_reveal(
	struct se_app *app,
	const struct kl_rect *current)
{
	const struct kl_rect *panel;
	int scroll;
	int limit;

	/* Only once for a change of page; Home has no row. */
	app->reveal = 0;
	if (current->height <= 0)
		return;

	/* A row above the pane's top comes down to it, one below its bottom comes up to it. */
	panel = &app->layout.sidebar;
	scroll = app->sidebar_scroll;
	if (current->y < panel->y + 8)
		scroll -= panel->y + 8 - current->y;
	if (current->y + current->height > panel->y + panel->height - 8)
		scroll += current->y + current->height - (panel->y + panel->height - 8);

	/* Within the list's extent. */
	limit = app->sidebar_extent - panel->height;
	scroll = ui_clamp(scroll, 0, limit);

	/* A change needs a frame. */
	if (scroll != app->sidebar_scroll) {
		app->sidebar_scroll = scroll;
		app->dirty = 1;
	}
}

/* Draws the page shown in its pane: its header and its cards, scrolled. */
static void
ui_draw_page(
	struct se_app *app,
	struct kl_canvas *canvas)
{
	const struct kl_rect *panel;
	const struct se_page *page;
	struct kl_rect content;
	int bottom;
	int limit;
	int x;
	int width;

	/* The pane: a light veil over the compositor's glass, or a whiter card with a bright edge over the window's ground. */
	panel = &app->layout.page;
	if (app->glass != 0) {
		kl_canvas_round(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, SE_COLOR_GLASS_PAGE);
	} else {
		kl_canvas_round(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, SE_COLOR_PANEL);
		kl_canvas_round_border(canvas, (float)panel->x, (float)panel->y, (float)panel->width, (float)panel->height, UI_PANEL_RADIUS, 1.0f, SE_COLOR_PANEL_EDGE);
	}

	/* The pane itself is a region the wheel scrolls; the page's controls are recorded after it. */
	se_ui_hit(app, panel, SE_HIT_PAGE, 0);

	/* The page stays inside the pane, in a column within its margins. */
	kl_canvas_clip_push(canvas, panel);
	x = panel->x + UI_PAGE_SIDE;
	width = panel->width - 2 * UI_PAGE_SIDE;
	page = &se_pages[app->page];

	/*
	 * The Welcome's step above its bar (ws164-p002); else the search's
	 * results in place of the page while a query is typed; else the header,
	 * then the page's own cards.
	 */
	if (app->welcome != 0) {
		content = *panel;
		content.height -= se_welcome_bar_height();
		kl_canvas_clip_push(canvas, &content);
		bottom = se_welcome_draw(app, canvas, x, panel->y + UI_PAGE_TOP - app->page_scroll, width);
		bottom += se_welcome_bar_height();
		kl_canvas_clip_pop(canvas);
		se_welcome_bar(app, canvas, panel);
	} else if (app->search.active != 0) {
		bottom = se_search_draw(app, canvas, x, panel->y + UI_PAGE_TOP - app->page_scroll, width);
	} else {
		bottom = se_page_header(app, canvas, page, x, panel->y + UI_PAGE_TOP - app->page_scroll, width);
		if (page->draw != NULL)
			bottom = page->draw(app, canvas, x, bottom + UI_PAGE_HEADER_GAP, width);
	}

	/* The pane's clip ends with the page. */
	kl_canvas_clip_pop(canvas);

	/* How tall the page is, for its scroll; a page that shrank below its scroll comes back up next frame. */
	app->page_extent = bottom + app->page_scroll - panel->y + UI_PAGE_BOTTOM;
	limit = app->page_extent - panel->height;
	if (limit < 0)
		limit = 0;
	if (app->page_scroll > limit) {
		app->page_scroll = limit;
		app->dirty = 1;
	}
}

/*
 * Finds the region of the last frame at a point: the latest recorded one
 * that holds it.  A region of the list or of the page counts only inside
 * its pane (a row scrolled out of sight is not clicked).  Returns 1 with
 * its kind and index, or 0 when there is none.
 */
static int
ui_hit_at(
	struct se_app *app,
	int x,
	int y,
	unsigned *kind,
	int *index)
{
	const struct se_hit *hit;
	const struct kl_rect *pane;
	int found;

	/* From the latest region back, so that the one drawn over wins. */
	for (found = app->hit_count - 1; found >= 0; found--) {
		hit = &app->hits[found];
		if (x < hit->rect.x ||
		    y < hit->rect.y ||
		    x >= hit->rect.x + hit->rect.width ||
		    y >= hit->rect.y + hit->rect.height)
			continue;

		/* The pane the region lies in: the list's rows in the list, the page's controls in the page. */
		pane = NULL;
		if (hit->kind == SE_HIT_PAGE_ROW)
			pane = &app->layout.sidebar;
		if (hit->kind == SE_HIT_TILE ||
		    hit->kind == SE_HIT_CONTROL ||
		    hit->kind == SE_HIT_RESULT)
			pane = &app->layout.page;

		/* A region scrolled out of its pane does not take the point. */
		if (pane != NULL) {
			if (x < pane->x ||
			    y < pane->y ||
			    x >= pane->x + pane->width ||
			    y >= pane->y + pane->height)
				continue;
		}

		/* The region. */
		*kind = hit->kind;
		*index = hit->index;
		return 1;
	}

	/* Nothing clickable there. */
	*kind = SE_HIT_NONE;
	*index = -1;
	return 0;
}

/* Lights the region under the pointer. */
static void
ui_motion(
	struct se_app *app,
	const struct se_event *event)
{
	const struct se_page *page;
	struct kl_rect before;
	struct kl_rect now;
	unsigned kind;
	int index;
	int found_before;
	int found_now;

	/* A finger held on a pane scrolls it once it has moved far enough. */
	if (app->touch.held != 0 && event->touch != 0) {
		ui_touch_move(app, event);
		return;
	}

	/* A press held on a page's control that drags (a slider) follows the pointer. */
	page = &se_pages[app->page];
	if (app->press_kind == SE_HIT_CONTROL &&
	    page->drag != NULL &&
	    app->search.active == 0 &&
	    app->dialog.open == 0) {
		app->drag_y = event->y;
		page->drag(app, app->press_index, event->x, SE_DRAG_MOVE);
		return;
	}

	/* The region under the pointer; only a row, a tile or a control is lit. */
	(void)ui_hit_at(app, event->x, event->y, &kind, &index);
	if (kind == SE_HIT_SIDEBAR || kind == SE_HIT_PAGE) {
		kind = SE_HIT_NONE;
		index = -1;
	}

	/* An unchanged lit region needs nothing. */
	if (kind == app->hover_kind && index == app->hover_index)
		return;

	/*
	 * A change of the lit region needs only the regions lit before and
	 * now drawn again (BUG-226), unless the whole frame is to be drawn
	 * anyway or one of them is not known.
	 */
	if (app->dirty == 0) {
		memset(&before, 0, sizeof(before));
		memset(&now, 0, sizeof(now));
		found_before = 1;
		if (app->hover_kind != SE_HIT_NONE)
			found_before = ui_hit_rect(app, app->hover_kind, app->hover_index, &before);
		found_now = 1;
		if (kind != SE_HIT_NONE)
			found_now = ui_hit_rect(app, kind, index, &now);
		if (found_before == 0 || found_now == 0) {
			app->dirty = 1;
		} else {
			if (app->hover_kind != SE_HIT_NONE)
				ui_damage_add(app, &before);
			if (kind != SE_HIT_NONE)
				ui_damage_add(app, &now);
		}
	}
	app->hover_kind = kind;
	app->hover_index = index;
}

/* Takes a press and a release of the left button: a click is a release over the region the press was on. */
static void
ui_button(
	struct se_app *app,
	const struct se_event *event)
{
	const struct se_page *page;
	unsigned kind;
	int index;
	int scrolled;

	/* Only the left button does anything. */
	if (event->button != SE_BUTTON_LEFT)
		return;

	/* The region under the pointer. */
	(void)ui_hit_at(app, event->x, event->y, &kind, &index);

	/* Under a popup a press holds its control down and a release over it clicks: no drag, no scroll (ws199-p001). */
	if (app->dialog.open != 0) {
		if (event->pressed != 0) {
			app->press_kind = kind;
			app->press_index = index;
		} else {
			if (kind == app->press_kind && index == app->press_index)
				ui_click(app, kind, index);
			app->press_kind = SE_HIT_NONE;
			app->press_index = -1;
		}

		/* Drawn again. */
		app->dirty = 1;
		return;
	}

	/* A press holds the region down; a page's control that drags starts its drag (a finger on it drags the slider, not the page). */
	page = &se_pages[app->page];
	if (event->pressed != 0) {
		app->press_kind = kind;
		app->press_index = index;
		app->dirty = 1;
		if (kind == SE_HIT_CONTROL &&
		    page->drag != NULL &&
		    app->search.active == 0) {
			app->drag_y = event->y;
			page->drag(app, index, event->x, SE_DRAG_START);
			return;
		}

		/* A finger held anywhere else may scroll its pane. */
		if (event->touch != 0)
			ui_touch_press(app, event, kind);
		return;
	}

	/* A finger lifted after it scrolled its pane: the scroll ends. */
	scrolled = ui_touch_release(app);

	/* The release ends a drag wherever it lands. */
	if (app->press_kind == SE_HIT_CONTROL &&
	    page->drag != NULL &&
	    app->search.active == 0) {
		app->drag_y = event->y;
		page->drag(app, app->press_index, event->x, SE_DRAG_END);
	}

	/* A release over the same region clicks it, unless the finger scrolled. */
	if (scrolled == 0 && kind == app->press_kind && index == app->press_index)
		ui_click(app, kind, index);

	/* Nothing is held down any more. */
	app->press_kind = SE_HIT_NONE;
	app->press_index = -1;
	app->dirty = 1;
}

/* Carries out a click on a region. */
static void
ui_click(
	struct se_app *app,
	unsigned kind,
	int index)
{
	const struct se_page *page;
	int taken;

	/* Under a popup only its own controls click (ws199-p001). */
	if (app->dialog.open != 0) {
		if (kind == SE_HIT_CONTROL)
			(void)se_dialog_press(app, index);
		return;
	}

	/* A row of the list and a tile of Home both open their page. */
	if (kind == SE_HIT_PAGE_ROW || kind == SE_HIT_TILE) {
		se_ui_go(app, (unsigned)index);
		return;
	}

	/* A result of the search opens its page. */
	if (kind == SE_HIT_RESULT) {
		se_search_press(app, index);
		return;
	}

	/* The Welcome's own controls (ws164-p002). */
	if (kind == SE_HIT_CONTROL && app->welcome != 0) {
		taken = se_welcome_press(app, index);
		if (taken)
			return;
	}

	/* A page's own control is the page's to carry out. */
	if (kind == SE_HIT_CONTROL) {
		page = &se_pages[app->page];
		if (page->press != NULL)
			page->press(app, index);
		app->dirty = 1;
	}
}

/* Holds a pane under a finger just pressed: a drag of the finger will scroll it (the list's rows scroll the list). */
static void
ui_touch_press(
	struct se_app *app,
	const struct se_event *event,
	unsigned kind)
{
	const struct kl_rect *page;
	struct se_touch_scroll *touch;

	/* The pane under the finger: the list for its rows and its ground, else the page when the finger is on it. */
	touch = &app->touch;
	page = &app->layout.page;
	if (kind == SE_HIT_PAGE_ROW || kind == SE_HIT_SIDEBAR) {
		touch->pane = SE_HIT_SIDEBAR;
		touch->start_scroll = app->sidebar_scroll;
	} else if (event->x >= page->x &&
		   event->x < page->x + page->width &&
		   event->y >= page->y &&
		   event->y < page->y + page->height) {
		touch->pane = SE_HIT_PAGE;
		touch->start_scroll = app->page_scroll;
	} else {
		/* Outside both panes a finger only clicks. */
		touch->held = 0;
		return;
	}

	/* The finger is held where it touched, not yet a scroll. */
	touch->held = 1;
	touch->scrolling = 0;
	touch->start_x = event->x;
	touch->start_y = event->y;
}

/* Follows a held finger: once it has moved far enough up or down, its pane scrolls with it and its press clicks nothing. */
static void
ui_touch_move(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_touch_scroll *touch;
	const struct kl_rect *pane;
	int moved_x;
	int moved_y;
	int distance_x;
	int distance_y;
	int limit;
	int scroll;

	/* How far the finger has moved since it touched. */
	touch = &app->touch;
	moved_x = event->x - touch->start_x;
	moved_y = event->y - touch->start_y;
	distance_x = abs(moved_x);
	distance_y = abs(moved_y);

	/* A small move, or one more sideways than up or down, is still a tap. */
	if (touch->scrolling == 0) {
		if (distance_y < UI_TOUCH_SLOP)
			return;
		if (distance_x > distance_y)
			return;

		/* From here the finger scrolls: the region it pressed is let go without a click. */
		touch->scrolling = 1;
		app->press_kind = SE_HIT_NONE;
		app->press_index = -1;
		app->hover_kind = SE_HIT_NONE;
		app->hover_index = -1;
		se_log("TOUCH scroll start pane=%s", ui_touch_pane_word(touch->pane));
	}

	/* The content follows the finger: a finger moving up shows what is below, within the pane's extent. */
	if (touch->pane == SE_HIT_SIDEBAR) {
		pane = &app->layout.sidebar;
		limit = app->sidebar_extent - pane->height;
		scroll = ui_clamp(touch->start_scroll - moved_y, 0, limit);
		if (scroll != app->sidebar_scroll) {
			app->sidebar_scroll = scroll;
			app->dirty = 1;
		}
	} else {
		pane = &app->layout.page;
		limit = app->page_extent - pane->height;
		scroll = ui_clamp(touch->start_scroll - moved_y, 0, limit);
		if (scroll != app->page_scroll) {
			app->page_scroll = scroll;
			app->dirty = 1;
		}
	}
}

/* Lets a held finger go.  Returns 1 when it had scrolled its pane (its release then clicks nothing), else 0. */
static int
ui_touch_release(
	struct se_app *app)
{
	struct se_touch_scroll *touch;
	int scrolled;

	/* No finger held, nothing scrolled. */
	touch = &app->touch;
	if (touch->held == 0)
		return 0;

	/* The finger is gone. */
	scrolled = touch->scrolling;
	touch->held = 0;
	touch->scrolling = 0;

	/* A scroll it made says where its pane stopped (the tests read it). */
	if (scrolled != 0 && touch->pane == SE_HIT_SIDEBAR) {
		se_log("TOUCH scroll end pane=list scroll=%d", app->sidebar_scroll);
	} else if (scrolled != 0) {
		se_log("TOUCH scroll end pane=page scroll=%d", app->page_scroll);
	}

	/* Reports whether the finger scrolled. */
	return scrolled;
}

/* Names a pane a finger scrolls, for the log. */
static const char *
ui_touch_pane_word(
	unsigned pane)
{
	/* The list of pages. */
	if (pane == SE_HIT_SIDEBAR)
		return "list";

	/* The page. */
	return "page";
}

/*
 * Scrolls the pane under the pointer by the wheel.  A touch pad's fingers
 * (BUG-211) hold the pane they began on until they lift: libkeiland's
 * scroller moves it with them and keeps their moves for the velocity
 * (ws090-p019); a wheel ends a flight.
 */
static void
ui_scroll(
	struct se_app *app,
	const struct se_event *event)
{
	const struct kl_rect *list;
	double x;
	double y;
	int pane;
	int scroll;
	int limit;
	int height;

	/* Nothing scrolls under a popup (ws199-p001). */
	if (app->dialog.open != 0)
		return;

	/* The list when the pointer is over it, else the page. */
	list = &app->layout.sidebar;
	pane = SE_KINETIC_PAGE;
	if (event->x >= list->x &&
	    event->x < list->x + list->width &&
	    event->y >= list->y &&
	    event->y < list->y + list->height)
		pane = SE_KINETIC_SIDEBAR;

	/* A wheel (or fingers without a scroller) moves the pane at once, and stops what flies. */
	app->kinetic.flying = 0;
	if (event->source != SE_SOURCE_FINGER || app->kinetic.scroller == NULL) {
		app->kinetic.holding = 0;
		(void)ui_scroll_by(app, pane, event->scroll);
		return;
	}

	/* The fingers' first move: the scroller takes the pane from where it is, within its ends. */
	if (app->kinetic.holding == 0) {
		app->kinetic.holding = 1;
		app->kinetic.pane = pane;
		scroll = ui_pane_scroll(app, pane, &limit, &height);
		(void)kl_scroller_set_bounds(app->kinetic.scroller, 0.0, 0.0, 0.0, (double)limit, 1.0, (double)height);
		kl_scroller_set_position(app->kinetic.scroller, 0.0, (double)scroll);
		se_log("KINETIC hold pane=%s", pane == SE_KINETIC_SIDEBAR ? "list" : "page");
	}

	/* The scroller follows the fingers (their times are the compositor's, its steps the clock's), and the pane follows it. */
	(void)kl_scroller_axis(app->kinetic.scroller, 0.0, (double)event->scroll, (uint64_t)event->axis_ms * 1000U, (uint64_t)event->time * 1000U);
	(void)kl_scroller_step(app->kinetic.scroller, (uint64_t)event->time * 1000U, &x, &y);
	(void)ui_kinetic_place(app, y);
}

/*
 * The touch pad's fingers lift (BUG-211): the pane they held flies on at
 * their velocity, as libkeiland's scroller throws it, unless they rested.
 */
static void
ui_scroll_stop(
	struct se_app *app,
	const struct se_event *event)
{
	double vx;
	double vy;
	int flung;

	/* Only a pane the fingers hold. */
	if (app->kinetic.holding == 0)
		return;
	app->kinetic.holding = 0;

	/* The scroller throws it at their velocity, by the compositor's times of their moves; too slow a one throws nothing. */
	flung = kl_scroller_axis_stop(app->kinetic.scroller, (uint64_t)event->axis_ms * 1000U, (uint64_t)event->time * 1000U, &vx, &vy);
	if (flung == 0) {
		se_log("KINETIC none pane=%s velocity=%.0f", app->kinetic.pane == SE_KINETIC_SIDEBAR ? "list" : "page", vy);
		return;
	}

	/* The flight, which the ticks move on. */
	app->kinetic.flying = 1;
	app->dirty = 1;
	se_log("KINETIC start pane=%s velocity=%.0f", app->kinetic.pane == SE_KINETIC_SIDEBAR ? "list" : "page", vy);
}

/*
 * Scrolls a pane (SE_KINETIC_PAGE or SE_KINETIC_SIDEBAR) by an amount
 * within its ends; returns 1 when it moved.
 */
static int
ui_scroll_by(
	struct se_app *app,
	int pane,
	int amount)
{
	int before;
	int limit;

	/* The page. */
	if (pane != SE_KINETIC_SIDEBAR) {
		before = app->page_scroll;
		ui_scroll_page(app, amount);
		if (app->page_scroll == before)
			return 0;
		return 1;
	}

	/* The list. */
	before = app->sidebar_scroll;
	limit = app->sidebar_extent - app->layout.sidebar.height;
	app->sidebar_scroll = ui_clamp(app->sidebar_scroll + amount, 0, limit);
	if (app->sidebar_scroll == before)
		return 0;
	app->dirty = 1;

	/* Succeeded: the list moved. */
	return 1;
}

/*
 * Moves a flight on to a time (BUG-211): the pane where libkeiland's
 * scroller has it; the flight ends when the scroller rests or the pane
 * reaches an end.  Each step asks for a frame.
 */
static void
ui_kinetic_step(
	struct se_app *app,
	uint64_t now)
{
	double x;
	double y;
	int moving;
	int inside;

	/* Nothing flies. */
	if (app->kinetic.flying == 0)
		return;
	app->dirty = 1;

	/* The pane where the scroller has it now. */
	moving = kl_scroller_step(app->kinetic.scroller, now * 1000U, &x, &y);
	inside = ui_kinetic_place(app, y);

	/* An end reached, or at rest: the flight is over. */
	if (moving == 0 || inside == 0) {
		app->kinetic.flying = 0;
		se_log("KINETIC stop pane=%s", app->kinetic.pane == SE_KINETIC_SIDEBAR ? "list" : "page");
	}
}

/*
 * Puts the pane the fingers scroll where the scroller has it (rounded,
 * within its ends).  Returns 0 when the scroller went past an end (the
 * pane stays at it), 1 otherwise.
 */
static int
ui_kinetic_place(
	struct se_app *app,
	double position)
{
	int scroll;
	int wanted;
	int limit;
	int height;
	int inside;

	/* The pane's scroll now, and the one the scroller has, within the ends. */
	scroll = ui_pane_scroll(app, app->kinetic.pane, &limit, &height);
	wanted = (int)lround(position);
	inside = 1;
	if (wanted < 0 || wanted > limit) {
		wanted = ui_clamp(wanted, 0, limit);
		inside = 0;
	}

	/* The pane moved by the difference. */
	if (wanted != scroll)
		(void)ui_scroll_by(app, app->kinetic.pane, wanted - scroll);

	/* Succeeded: whether the scroller is within the ends. */
	return inside;
}

/* Gives a pane's scroll, the furthest it scrolls and its height (SE_KINETIC_PAGE or SE_KINETIC_SIDEBAR). */
static int
ui_pane_scroll(
	const struct se_app *app,
	int pane,
	int *limit,
	int *height)
{
	/* The list. */
	if (pane == SE_KINETIC_SIDEBAR) {
		*height = app->layout.sidebar.height;
		*limit = app->sidebar_extent - *height;
		if (*limit < 0)
			*limit = 0;
		return app->sidebar_scroll;
	}

	/* The page. */
	*height = app->layout.page.height;
	*limit = app->page_extent - *height;
	if (*limit < 0)
		*limit = 0;

	/* Succeeded: the page's scroll. */
	return app->page_scroll;
}

/* Carries out a key: the history, the pages in the list's order, the page's scroll, and closing. */
static void
ui_key(
	struct se_app *app,
	const struct se_event *event)
{
	const struct kl_rect *pane;
	const struct se_page *page;
	int used;

	/* Only a press does anything. */
	if (event->pressed == 0)
		return;

	/* A popup takes every key (ws199-p001). */
	used = se_dialog_key(app, event);
	if (used != 0) {
		app->dirty = 1;
		return;
	}

	/* Ctrl+F gives the keyboard to the titlebar's search (when the compositor's menus did not take it). */
	if ((event->modifiers & SE_MOD_CTRL) != 0U && event->key == SE_KEY_F) {
		se_search_focus(app);
		return;
	}

	/* While the search's results are shown, Up, Down, Enter and Esc are the search's. */
	if (app->search.active != 0) {
		/* Up chooses the result before (ws089-p012). */
		if (event->key == SE_KEY_UP) {
			se_search_step(app, -1);
			return;
		}

		/* Down chooses the result after. */
		if (event->key == SE_KEY_DOWN) {
			se_search_step(app, 1);
			return;
		}

		/* Enter opens the chosen result. */
		if (event->key == SE_KEY_ENTER) {
			(void)se_search_open_chosen(app);
			return;
		}

		/* Esc ends the search. */
		if (event->key == SE_KEY_ESC) {
			se_search_end(app);
			return;
		}
	}

	/* The page takes the key first (a text field that has the keyboard), unless the results hide it. */
	page = &se_pages[app->page];
	if (page->key != NULL && app->search.active == 0) {
		used = page->key(app, event);
		if (used != 0) {
			app->dirty = 1;
			return;
		}
	}

	/* The Welcome's keys: Enter, Esc, Alt with the arrows (ws177-p007). */
	if (app->welcome != 0) {
		used = se_welcome_key(app, event);
		if (used != 0) {
			app->dirty = 1;
			return;
		}
	}

	/* Alt with the arrows walks the history. */
	if ((event->modifiers & SE_MOD_ALT) != 0U) {
		if (event->key == SE_KEY_LEFT)
			ui_back(app);
		if (event->key == SE_KEY_RIGHT)
			ui_forward(app);
		return;
	}

	/* Ctrl+W and Ctrl+Q close the window (when the compositor's menus did not take them). */
	if ((event->modifiers & SE_MOD_CTRL) != 0U) {
		if (event->key == SE_KEY_W || event->key == SE_KEY_Q)
			app->request = SE_REQUEST_CLOSE;
		return;
	}

	/* Each key alone. */
	pane = &app->layout.page;
	switch (event->key) {
	case SE_KEY_UP:
		ui_step_page(app, -1);
		break;
	case SE_KEY_DOWN:
		ui_step_page(app, 1);
		break;
	case SE_KEY_PAGE_UP:
		ui_scroll_page(app, -(pane->height - UI_PAGE_OVERLAP));
		break;
	case SE_KEY_PAGE_DOWN:
		ui_scroll_page(app, pane->height - UI_PAGE_OVERLAP);
		break;
	case SE_KEY_HOME:
		ui_scroll_page(app, -app->page_extent);
		break;
	case SE_KEY_END:
		ui_scroll_page(app, app->page_extent);
		break;
	case SE_KEY_ESC:
		/* Esc goes back, as a browser does. */
		ui_back(app);
		break;
	default:
		break;
	}
}

/* Shows the page before or after the one shown, in the list's order (Home goes to the first). */
static void
ui_step_page(
	struct se_app *app,
	int direction)
{
	unsigned page;

	/* From Home, down is the first page and up stays. */
	if (app->page == SE_PAGE_HOME) {
		if (direction > 0)
			se_ui_go(app, SE_PAGE_HOME + 1);
		return;
	}

	/* The neighbour in the list; the ends stay. */
	page = app->page;
	if (direction < 0 && page > SE_PAGE_HOME + 1)
		page--;
	if (direction > 0 && page + 1 < SE_PAGES)
		page++;
	se_ui_go(app, page);
}

/* Scrolls the page by an amount of pixels, within its extent. */
static void
ui_scroll_page(
	struct se_app *app,
	int amount)
{
	int limit;
	int scroll;

	/* The furthest the page scrolls is its extent less its pane. */
	limit = app->page_extent - app->layout.page.height;
	scroll = ui_clamp(app->page_scroll + amount, 0, limit);

	/* A change needs a frame. */
	if (scroll != app->page_scroll) {
		app->page_scroll = scroll;
		app->dirty = 1;
	}
}

/* Shows the page before in the history. */
static void
ui_back(
	struct se_app *app)
{
	/* A search shown goes back to the page it covers. */
	if (app->search.active != 0) {
		se_search_end(app);
		return;
	}

	/* The first step has nothing before it. */
	if (app->history_index <= 0)
		return;

	/* The step before, from its top. */
	app->history_index--;
	app->page = app->history[app->history_index];
	app->page_scroll = 0;
	app->reveal = 1;
	app->dirty = 1;
	se_log("PAGE %s back", se_pages[app->page].word);
}

/* Shows the page after in the history. */
static void
ui_forward(
	struct se_app *app)
{
	/* The newest step has nothing after it. */
	if (app->history_index + 1 >= app->history_count)
		return;

	/* A search shown gives way to the step after. */
	se_search_end(app);

	/* The step after, from its top. */
	app->history_index++;
	app->page = app->history[app->history_index];
	app->page_scroll = 0;
	app->reveal = 1;
	app->dirty = 1;
	se_log("PAGE %s forward", se_pages[app->page].word);
}

/* Keeps a value between a minimum and a maximum (a maximum below the minimum counts as the minimum). */
static int
ui_clamp(
	int value,
	int minimum,
	int maximum)
{
	/* A maximum below the minimum leaves only the minimum. */
	if (maximum < minimum)
		maximum = minimum;

	/* Below the range. */
	if (value < minimum)
		return minimum;

	/* Above the range. */
	if (value > maximum)
		return maximum;

	/* Within it. */
	return value;
}

/*
 * Lists the page's controls in the log when they differ from the last
 * list ("ZSETTINGS CONTROL index=N x= y= width= height=", window
 * coordinates; a search's result is a RESULT line), so that a test finds
 * a control it is to click.
 */
static void
ui_log_controls(
	struct se_app *app)
{
	const struct se_hit *hit;
	int count;
	int index;
	int same;
	int differs;

	/* The controls of this frame, gathered at the front of the list kept. */
	count = 0;
	same = 1;
	for (index = 0; index < app->hit_count; index++) {
		hit = &app->hits[index];
		if (hit->kind != SE_HIT_CONTROL && hit->kind != SE_HIT_RESULT)
			continue;

		/* A control past the logged ones, or that differs from the one logged at its place. */
		if (count >= app->logged_count) {
			same = 0;
		} else {
			differs = memcmp(&app->logged[count], hit, sizeof(*hit));
			if (differs != 0)
				same = 0;
		}

		/* It is kept for the next frame. */
		app->logged[count] = *hit;
		count++;
	}

	/* Nothing moved, came or went. */
	if (same != 0 && count == app->logged_count)
		return;

	/* The new list. */
	app->logged_count = count;
	se_log("LAYOUT page=%s controls=%d", se_pages[app->page].word, count);
	for (index = 0; index < count; index++) {
		hit = &app->logged[index];
		if (hit->kind == SE_HIT_RESULT) {
			se_log("RESULT index=%d x=%d y=%d width=%d height=%d", hit->index, hit->rect.x, hit->rect.y, hit->rect.width, hit->rect.height);
		} else {
			se_log("CONTROL index=%d x=%d y=%d width=%d height=%d", hit->index, hit->rect.x, hit->rect.y, hit->rect.width, hit->rect.height);
		}
	}
}

/* Finds the rectangle of a region of the last frame by its kind and index; returns 1 when it is there. */
static int
ui_hit_rect(
	const struct se_app *app,
	unsigned kind,
	int index,
	struct kl_rect *rect)
{
	int found;

	/* From the latest region back, as the pointer finds them. */
	for (found = app->hit_count - 1; found >= 0; found--) {
		if (app->hits[found].kind != kind || app->hits[found].index != index)
			continue;
		*rect = app->hits[found].rect;
		return 1;
	}

	/* Not in the last frame. */
	return 0;
}

/*
 * Adds a region, with UI_DAMAGE_MARGIN round it (a lit row's edge and
 * shadow), to the part of the window the next frame draws, and asks for
 * that frame (BUG-226).
 */
static void
ui_damage_add(
	struct se_app *app,
	const struct kl_rect *rect)
{
	struct kl_rect grown;
	int right;
	int bottom;

	/* The region and its margin. */
	grown.x = rect->x - UI_DAMAGE_MARGIN;
	grown.y = rect->y - UI_DAMAGE_MARGIN;
	grown.width = rect->width + 2 * UI_DAMAGE_MARGIN;
	grown.height = rect->height + 2 * UI_DAMAGE_MARGIN;

	/* The first region is the part. */
	if (app->hover_pending == 0) {
		app->hover_damage = grown;
		app->hover_pending = 1;
		return;
	}

	/* Another widens it to hold both. */
	right = app->hover_damage.x + app->hover_damage.width;
	if (grown.x + grown.width > right)
		right = grown.x + grown.width;
	bottom = app->hover_damage.y + app->hover_damage.height;
	if (grown.y + grown.height > bottom)
		bottom = grown.y + grown.height;
	if (grown.x < app->hover_damage.x)
		app->hover_damage.x = grown.x;
	if (grown.y < app->hover_damage.y)
		app->hover_damage.y = grown.y;
	app->hover_damage.width = right - app->hover_damage.x;
	app->hover_damage.height = bottom - app->hover_damage.y;
}

/* Makes a rectangle of the canvas transparent (the glass shows through), within the canvas. */
static void
ui_clear_rect(
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	uint32_t *row;
	int left;
	int top;
	int right;
	int bottom;
	int y;

	/* The rectangle within the canvas. */
	left = rect->x;
	top = rect->y;
	right = rect->x + rect->width;
	bottom = rect->y + rect->height;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > canvas->width)
		right = canvas->width;
	if (bottom > canvas->height)
		bottom = canvas->height;
	if (left >= right || top >= bottom)
		return;

	/* Each of its rows, transparent black (premultiplied). */
	for (y = top; y < bottom; y++) {
		row = canvas->pixels + (size_t)y * canvas->stride;
		memset(row + left, 0, sizeof(row[0]) * (size_t)(right - left));
	}
}
