/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's view (WS155 p000; calendar.h).
 *
 * Three panes in Files' style: on the compositor's glass they are cards apart,
 * reaching the window's edges, with no ground between (the desktop shows
 * through); on an opaque window, white cards on a quiet blue ground.  At
 * the left the sidebar -- the places (Month View, Today, Search,
 * Settings), the calendars with their colors to show or hide, and a small
 * card of encouragement; in the middle the bar of the view (back and
 * forward a month, Today, Month / Week / Day, the search) over the months
 * one under another, Sunday's column a faint red and Saturday's a faint
 * blue, today in the accent, each day's events as small pills of their
 * calendar's color and its memos as outlined pills with a note; at the
 * right the panel to add an event -- a card for each kind with its icon in
 * 3D, to drag onto a date -- over the application's one memo (dragged by
 * its header onto a date, a copy is kept there as a memo) and the day
 * chosen: a small desk calendar in 3D, the date, and its events and memos
 * in full.
 *
 * The motion is little and calm: the small desk calendar's page turns
 * over when the day chosen changes, and the cell something is dropped on
 * sinks for a moment.  Nothing moves by itself; with the motion reduced,
 * every change is at once.
 */

#include "calendar.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pi. */
#define CAL_PI			3.14159265f

/*
 * The cards: they reach the window's edges, so that they line up with
 * the floating titlebar (ws090-p021); the gap between them (on glass,
 * Files' own: 8 pixels), their corner, the sidebar's and the panel's widths.
 */
#define CAL_GAP			12
#define CAL_GLASS_GAP		8
#define CAL_CARD_RADIUS		16.0f
#define CAL_SIDEBAR		212
#define CAL_PANEL		300

/* The widths under which the panel, and then the sidebar, are not shown. */
#define CAL_NO_PANEL		1020
#define CAL_NO_SIDEBAR		760

/* The months: the bar over them, the days' row, a month's heading, the margin, the least height of a cell. */
#define CAL_TOPBAR		60
#define CAL_WEEKDAYS		30
#define CAL_HEADING		52
#define CAL_GRID_PAD		12
#define CAL_CELL_MIN		84

/* An event's pill: its height and the space between. */
#define CAL_PILL		18
#define CAL_PILL_GAP		3

/* The panel's cards of the kinds of event, and the size of their icons. */
#define CAL_KIND_HEIGHT		108
#define CAL_ICON		56

/* The 3D target's factor over the picture, and the desk calendar's distance from the camera. */
#define CAL_SUPERSAMPLE		2
#define CAL_DESK_DISTANCE	6.4f

/* The motion's times: a page's turn, a cell's sink, a frame while something moves. */
#define CAL_FLIP_US		560000U
#define CAL_SINK_US		300000U
#define CAL_MOVING_MS		16

/* The memo: its header's and its words' heights, and the small desk calendar of the day chosen. */
#define CAL_MEMO_HEADER		30
#define CAL_MEMO_HEIGHT		150
#define CAL_MEMO_LEAST		60
#define CAL_DAY_MIN		(CAL_DAY_DESK_HEIGHT + 44)
#define CAL_DAY_DESK_WIDTH	76
#define CAL_DAY_DESK_HEIGHT	88

/* How far a press on a kind's card moves before it is a drag, and how long a notice shows. */
#define CAL_DRAG_START		6.0
#define CAL_NOTICE_US		4000000U

/* A page's picture. */
#define CAL_PAGE_WIDTH		240
#define CAL_PAGE_HEIGHT		272

/* The most events a cell lists. */
#define CAL_DAY_EVENTS		16U

/* The widgets' ids. */
#define CAL_ID_PREVIOUS		1U
#define CAL_ID_NEXT		2U
#define CAL_ID_TODAY		3U
#define CAL_ID_SEGMENT		4U
#define CAL_ID_SEARCH		5U
#define CAL_ID_MORE		6U
#define CAL_ID_PLACE		7U
#define CAL_ID_LIST		8U
#define CAL_ID_ADD_LIST		9U
#define CAL_ID_GRID		10U
#define CAL_ID_CELL		11U
#define CAL_ID_KIND		12U
#define CAL_ID_CUSTOM		13U
#define CAL_ID_MEMO_GRIP	14U
#define CAL_ID_MEMO_TEXT	15U
#define CAL_ID_DAY_ITEM		16U
#define CAL_ID_EDIT_TITLE	17U
#define CAL_ID_EDIT_START	18U
#define CAL_ID_EDIT_END		19U
#define CAL_ID_EDIT_ALL_DAY	20U
#define CAL_ID_EDIT_LIST	21U
#define CAL_ID_EDIT_SAVE	22U
#define CAL_ID_EDIT_DELETE	23U
#define CAL_ID_EDIT_CANCEL	24U
#define CAL_ID_LIST_ITEM	25U

/* The colors: the ground, the cards, Sunday's and Saturday's, the text of their numbers. */
#define CAL_COLOR_GROUND_TOP	kl_theme_choose(KL_RGB(0xeef4fd), KL_RGB(0x1b1f26))
#define CAL_COLOR_GROUND_BOTTOM	kl_theme_choose(KL_RGB(0xdde8f8), KL_RGB(0x16191f))
#define CAL_COLOR_CARD		kl_theme_choose(KL_RGBA(0xffffff, 238), KL_RGBA(0x262b34, 238))
#define CAL_COLOR_KIND_GLASS	kl_theme_choose(KL_RGBA(0xffffff, 110), KL_RGBA(0x2a2f38, 110))
#define CAL_COLOR_MEMO		KL_RGB(0xf59e0b)
#define CAL_COLOR_MEMO_EDGE	KL_RGBA(0x64748b, 120)
#define CAL_COLOR_SUNDAY	KL_RGBA(0xe5484d, 12)
#define CAL_COLOR_SATURDAY	KL_RGBA(0x2f7cf6U, 12)
#define CAL_COLOR_SUNDAY_TEXT	KL_RGB(0xd2434a)
#define CAL_COLOR_SATURDAY_TEXT	KL_RGB(0x2b6fd6)
#define CAL_COLOR_WHITE		KL_RGB(0xffffff)
#define CAL_COLOR_SURFACE	kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x23272f))

/*
 * One item of a day: an event or a memo kept there (its words), its time
 * as words (empty for all day), its calendar, and its index in the store.
 */
struct view_entry {
	const char *title;
	char time[16];
	enum cal_list list;
	const char *memo;
	long index;
	int start;
	int all_day;
};

/* Where the parts of a frame are. */
struct view_layout {
	struct kl_rect sidebar;
	struct kl_rect main;
	struct kl_rect panel;
	struct kl_rect topbar;
	struct kl_rect weekdays;
	struct kl_rect grid;
};

/* The places of the sidebar. */
static const char *const view_places[] = { "Month View", "Today", "Search", "Settings" };
static const enum kl_icon view_place_icons[] = { KL_ICON_GRID, KL_ICON_RECENTS, KL_ICON_SEARCH, KL_ICON_TILES };

/* The kinds of event of the panel, in the icons' order, with what each holds and its calendar. */
static const char *const view_kinds[] = { "Work", "Personal", "Study", "Family" };
static const char *const view_kind_lines[] = { "Meeting, Task, Deadline", "Health, Hobby, Errand", "Class, Exam, Reading", "Birthday, School, Trip" };
static const enum cal_list view_kind_lists[] = { CAL_WORK, CAL_PERSONAL, CAL_STUDY, CAL_FAMILY };

/* The titles of a new event, by calendar. */
static const char *const view_new_titles[CAL_LISTS] = { "New Work Event", "New Personal Event", "New Family Event", "New Study Event" };

/* The views of the bar. */
static const char *const view_segments[] = { "Month", "Week", "Day" };

static void view_layout(const struct cal_view *view, int width, int height, struct view_layout *layout);
static void view_card(const struct cal_view *view, const struct kl_style *style, const struct kl_rect *rect, kl_color veil);
static void view_sidebar(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_topbar(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_weekdays(const struct kl_style *style, const struct kl_rect *area);
static void view_grid(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_cell(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *cell, const struct kl_rect *band, const struct cal_date *date, int inside, uint64_t now_us);
static void view_panel(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_desk(struct cal_view *view, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_page(struct cal_view *view, const struct kl_style *style, int index, const struct cal_date *date);
static void view_keep_under(struct cal_view *view, const struct kl_style *style, const struct kl_rect *area);
static void view_let_go(struct cal_view *view, int kind, double x, double y, uint64_t now_us);
static unsigned view_drag_source(struct cal_view *view, struct kl_ui *ui, uint32_t id, int kind, const struct kl_rect *rect, uint64_t now_us);
static int view_memo(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, int top, int height, uint64_t now_us);
static void view_chosen_day(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *card, uint64_t now_us);
static void view_editor(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_edit(struct cal_view *view, long index, uint64_t now_us);
static void view_edit_save(struct cal_view *view, uint64_t now_us);
static void view_edit_delete(struct cal_view *view, uint64_t now_us);
static int view_parse_minute(const char *text, int *minute);
static void view_copy_title(char *to, size_t size, const char *from);
static void view_list(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static int view_kind_cards(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_memo_drop(struct cal_view *view, const struct cal_date *date, uint64_t now_us);
static void view_note_icon(struct kl_canvas *canvas, float x, float y, float size);
static int view_words(const struct kl_style *style, const char *text, int x, int y, int width, unsigned pixels, kl_color color, int draw);
static void view_ghost(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style);
static size_t view_day(const struct cal_view *view, const struct cal_date *date, struct view_entry *entries, size_t size);
static int view_matches(const struct cal_view *view, const char *title);
static int view_contains(const char *text, const char *part);
static void view_select(struct cal_view *view, const struct cal_date *date, uint64_t now_us);
static void view_show(struct cal_view *view, const struct cal_date *date, uint64_t now_us);
static void view_drop(struct cal_view *view, enum cal_list list, const struct cal_date *date, uint64_t now_us);
static void view_notice(struct cal_view *view, const char *message, uint64_t now_us);
static int view_month_top(const struct cal_view *view, int index, int cell_height);
static int view_month_rows(const struct cal_view *view, int index, struct cal_date *first);
static int view_cell_height(const struct kl_rect *grid);
static void view_centred(const struct kl_style *style, int cx, int baseline, const char *text, unsigned pixels, int bold, kl_color color);
static float view_ease(float t);

/*
 * Makes the view's state for a day as today: the day chosen and shown is
 * today, the months go six before and after it, the icons are drawn.
 */
int
cal_view_init(
	struct cal_view *view,
	const struct cal_date *today,
	uint64_t now_us)
{
	struct r3_target target;
	struct r3_matrix turn;
	struct r3_matrix tilt;
	struct r3_matrix place;
	int error;
	int i;

	/* The days. */
	memset(view, 0, sizeof(view[0]));
	view->today = *today;
	view->selected = *today;
	view->shown = *today;
	view->first_month = *today;
	view->first_month.day = 1;
	cal_add_months(&view->first_month, -(CAL_MONTHS / 2));
	view->scroll_to = CAL_MONTHS / 2 + 1;
	view->scroll_glide = 0;
	view->dragging = -1;
	view->started_us = now_us;
	view->edit_index = -1;
	kl_text_area_set(&view->memo, cal_store_memo());

	/* The months' scroll, down only. */
	error = kl_scroll_init(&view->scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* The mesh the 3D things are made in. */
	view->mesh = malloc(sizeof(view->mesh[0]));
	if (view->mesh == NULL) {
		kl_scroll_release(&view->scroll);
		return ENOMEM;
	}

	/* The icons of the kinds, drawn once in 3D, twice as large and averaged down. */
	error = r3_target_init(&target, CAL_ICON * CAL_SUPERSAMPLE, CAL_ICON * CAL_SUPERSAMPLE);
	if (error != 0) {
		cal_view_release(view);
		return error;
	}

	/* Each kind's icon. */
	for (i = 0; i < SC_ICONS; i++) {
		/* The icon's picture. */
		error = kl_image_create(&view->icons[i], CAL_ICON, CAL_ICON);
		if (error != 0)
			break;

		/* Turned a little to show its sides, and lit from the upper left. */
		r3_target_clear(&target);
		target.focal = (float)target.width * 1.9f;
		r3_rotate_y(&turn, -0.5f);
		r3_rotate_x(&tilt, 0.32f);
		r3_translate(&place, 0.0f, 0.0f, 4.2f);
		r3_combine(&tilt, &turn, &turn);
		r3_combine(&place, &turn, &turn);
		sc_mesh_clear(view->mesh);
		sc_icon(view->mesh, (enum sc_icon)i);
		sc_draw(&target, view->mesh, &turn, NULL);
		r3_resolve(&target, CAL_SUPERSAMPLE, &view->icons[i]);
	}

	/* The target goes; a picture not made fails the view. */
	r3_target_release(&target);
	if (error != 0) {
		cal_view_release(view);
		return error;
	}

	/* Succeeded: the first frame draws the rest. */
	cal_log("READY today=%04d-%02d-%02d", today->year, today->month, today->day);
	return 0;
}

/*
 * Frees what the view's state holds.
 */
void
cal_view_release(
	struct cal_view *view)
{
	int i;

	/* The pictures. */
	for (i = 0; i < SC_ICONS; i++)
		kl_image_release(&view->icons[i]);
	for (i = 0; i < SC_TEXTURES; i++)
		kl_image_release(&view->pages[i]);
	kl_image_release(&view->picture);
	kl_image_release(&view->under);

	/* The 3D target, the mesh and the scroll. */
	r3_target_release(&view->target);
	free(view->mesh);
	view->mesh = NULL;
	kl_scroll_release(&view->scroll);
}

/*
 * Carries out an action of the menu, a key or a button.
 */
void
cal_view_action(
	struct cal_view *view,
	unsigned action,
	uint64_t now_us)
{
	struct cal_date date;

	/* Each action. */
	switch (action) {
	case CAL_ACTION_TODAY:
		/* Today chosen, its month brought into view. */
		view_select(view, &view->today, now_us);
		view->scroll_to = CAL_MONTHS / 2 + 1;
		view->scroll_glide = 1;
		break;
	case CAL_ACTION_PREVIOUS:
	case CAL_ACTION_NEXT:
		/* The day chosen a month back or on, its month brought into view. */
		date = view->selected;
		if (action == CAL_ACTION_PREVIOUS)
			cal_add_months(&date, -1);
		else
			cal_add_months(&date, 1);
		view_select(view, &date, now_us);
		view->scroll_to = cal_month_index(&view->first_month, &date) + 1;
		view->scroll_glide = 1;
		break;
	case CAL_ACTION_MOTION:
		/* The motion reduced or brought back (a page turning ends at once). */
		if (view->reduce_motion)
			view->reduce_motion = 0;
		else
			view->reduce_motion = 1;
		if (view->flipping) {
			view->flipping = 0;
			view->shown = view->flip_to;
		}

		/* A cell sinking comes back at once. */
		view->sinking = 0;
		cal_log("MOTION reduced=%d", view->reduce_motion);
		break;
	case CAL_ACTION_QUIT:
		view->quit = 1;
		break;
	default:
		break;
	}
}

/*
 * Takes a key no widget took: the arrows move the day chosen by a day or a
 * week, Page Up and Page Down by a month, T goes to today.
 */
void
cal_view_key(
	struct cal_view *view,
	uint32_t key,
	unsigned modifiers,
	uint64_t now_us)
{
	struct cal_date date;
	int days;
	int index;

	/* The keys with a modifier are the menu's. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return;

	/* Each key. */
	days = 0;
	switch (key) {
	case KL_KEY_LEFT:
		days = -1;
		break;
	case KL_KEY_RIGHT:
		days = 1;
		break;
	case KL_KEY_UP:
		days = -7;
		break;
	case KL_KEY_DOWN:
		days = 7;
		break;
	case KL_KEY_PAGEUP:
		cal_view_action(view, CAL_ACTION_PREVIOUS, now_us);
		return;
	case KL_KEY_PAGEDOWN:
		cal_view_action(view, CAL_ACTION_NEXT, now_us);
		return;
	case 20U:
		/* T. */
		cal_view_action(view, CAL_ACTION_TODAY, now_us);
		return;
	default:
		return;
	}

	/* The day moved, within the months shown. */
	date = view->selected;
	cal_add_days(&date, days);
	index = cal_month_index(&view->first_month, &date);
	if (index < 0 || index >= CAL_MONTHS)
		return;

	/* Chosen; a new month comes into view. */
	if (date.month != view->selected.month) {
		view->scroll_to = index + 1;
		view->scroll_glide = 1;
	}

	/* The day chosen. */
	view_select(view, &date, now_us);
}

/*
 * Draws a frame of the view in a window of a size, between the caller's
 * kl_ui_begin and kl_ui_end, and takes what the input did to its widgets.
 */
void
cal_view_draw(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_rect whole;

	/* A page that has turned shows its new day. */
	if (view->flipping && now_us - view->flip_us >= CAL_FLIP_US) {
		view->flipping = 0;
		view->shown = view->flip_to;
	}

	/* A cell that has sunk comes back. */
	if (view->sinking && now_us - view->sink_us >= CAL_SINK_US)
		view->sinking = 0;

	/* The ground: clear on glass, else a quiet blue. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (view->glass) {
		kl_canvas_clear(style->canvas);
	} else {
		kl_canvas_gradient(style->canvas, &whole, CAL_COLOR_GROUND_TOP, CAL_COLOR_GROUND_BOTTOM);
	}

	/* The sidebar's card and what is on it, when there is room for it. */
	view_layout(view, width, height, &layout);
	if (layout.sidebar.width > 0) {
		view_card(view, style, &layout.sidebar, style->theme->glass_sidebar);
		view_sidebar(view, ui, style, &layout.sidebar, now_us);
	}

	/*
	 * The panel, when there is room for it, before the months: a drop it
	 * takes (on the cells of the frame shown) is in the months drawn next.
	 */
	view->desk_valid = 0;
	if (layout.panel.width > 0) {
		view_card(view, style, &layout.panel, style->theme->glass_content);
		view_panel(view, ui, style, &layout.panel, now_us);
	}

	/* The months' card: its bar, the days of the week, the months. */
	view_card(view, style, &layout.main, style->theme->glass_content);
	view_topbar(view, ui, style, &layout.topbar, now_us);
	if (view->mode == CAL_MODE_MONTH) {
		view_weekdays(style, &layout.weekdays);
		view_grid(view, ui, style, &layout.grid, now_us);
	} else {
		view->cell_count = 0;
		view_list(view, ui, style, &layout.grid, now_us);
	}

	/* A kind of event being dragged, over everything. */
	view_ghost(view, ui, style);

	/* The notice over the bottom of the months while it shows. */
	if (view->notice[0] != '\0' && now_us < view->notice_until)
		kl_chip(style, layout.main.x + layout.main.width / 2, layout.main.y + layout.main.height - 16, view->notice);
}

/*
 * Lists the parts of the view that stand on the compositor's glass (its cards)
 * for a window of a size, into up to capacity panels; returns how many.
 */
size_t
cal_view_panels(
	const struct cal_view *view,
	int width,
	int height,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	struct view_layout layout;
	const struct kl_rect *cards[3];
	size_t count;
	size_t i;

	/* The three cards, as the frame draws them. */
	view_layout(view, width, height, &layout);
	cards[0] = &layout.sidebar;
	cards[1] = &layout.main;
	cards[2] = &layout.panel;
	count = 0;
	for (i = 0; i < 3U && count < capacity; i++) {
		/* A card not shown has no panel. */
		if (cards[i]->width <= 0 || cards[i]->height <= 0)
			continue;

		/* The card's panel. */
		memset(&panels[count], 0, sizeof(panels[count]));
		panels[count].x = cards[i]->x;
		panels[count].y = cards[i]->y;
		panels[count].width = cards[i]->width;
		panels[count].height = cards[i]->height;
		panels[count].radius = (int32_t)CAL_CARD_RADIUS;
		panels[count].kind = KL_GLASS_CARD;
		count++;
	}

	/* The panels listed. */
	return count;
}

/*
 * Reports how long the window may wait for input (ms) before the next
 * frame is due by itself, or -1 for no time.
 */
int
cal_view_wait(
	const struct cal_view *view,
	uint64_t now_us)
{
	uint64_t left;

	/* A page turning, a cell sinking or a drag: every frame. */
	if (view->flipping || view->sinking || view->dragging >= 0)
		return CAL_MOVING_MS;

	/* A notice, until it goes. */
	if (view->notice[0] != '\0' && now_us < view->notice_until) {
		left = view->notice_until - now_us;
		return (int)(left / 1000U) + 1;
	}

	/* Nothing moves. */
	return -1;
}

/*
 * Reports whether the next frame may be the desk calendar's alone: only it
 * moves (it turns a page), and the last full frame kept what is under it.
 */
int
cal_view_desk_only(
	const struct cal_view *view)
{
	/* A cell sinking or a drag needs the whole frame. */
	if (view->sinking || view->dragging >= 0)
		return 0;

	/* The last frame kept what is under the desk calendar. */
	if (!view->desk_valid)
		return 0;

	/* Only the desk calendar moves. */
	return 1;
}

/*
 * Draws a frame of the desk calendar alone over what the last full frame
 * kept under it (the rest of the canvas is as that frame left it); no
 * widget is drawn, so the caller does not begin or end a frame of input.
 */
void
cal_view_draw_desk(
	struct cal_view *view,
	const struct kl_style *style,
	uint64_t now_us)
{
	size_t row;
	size_t width;
	uint32_t *line;

	/* A page that has turned shows its new day. */
	if (view->flipping && now_us - view->flip_us >= CAL_FLIP_US) {
		view->flipping = 0;
		view->shown = view->flip_to;
	}

	/* What was under it, back on the canvas. */
	width = (size_t)view->desk_area.width;
	for (row = 0; row < (size_t)view->desk_area.height; row++) {
		line = style->canvas->pixels + ((size_t)view->desk_area.y + row) * style->canvas->stride + (size_t)view->desk_area.x;
		memcpy(line, view->under.pixels + row * view->under.stride, width * sizeof(line[0]));
	}

	/* The desk calendar over it (which keeps it again). */
	view_desk(view, style, &view->desk_area, now_us);
}

/*
 * Lays out the cards in a window of a size: the sidebar, the months and
 * the panel side by side (the panel, then the sidebar, left out of a
 * narrow window), and the bar, the days' row and the months in the
 * middle one.
 */
static void
view_layout(
	const struct cal_view *view,
	int width,
	int height,
	struct view_layout *layout)
{
	int margin;
	int gap;
	int left;
	int right;

	/* No margin (the cards reach the window's edges), and the gap: Files' on glass. */
	memset(layout, 0, sizeof(layout[0]));
	margin = 0;
	gap = CAL_GAP;
	if (view->glass)
		gap = CAL_GLASS_GAP;

	/* The sidebar, when the window is wide enough. */
	left = margin;
	if (width >= CAL_NO_SIDEBAR) {
		layout->sidebar.x = margin;
		layout->sidebar.y = margin;
		layout->sidebar.width = CAL_SIDEBAR;
		layout->sidebar.height = height - 2 * margin;
		left = margin + CAL_SIDEBAR + gap;
	}

	/* The panel, when the window is wider still. */
	right = width - margin;
	if (width >= CAL_NO_PANEL) {
		layout->panel.x = width - margin - CAL_PANEL;
		layout->panel.y = margin;
		layout->panel.width = CAL_PANEL;
		layout->panel.height = height - 2 * margin;
		right = layout->panel.x - gap;
	}

	/* The months' card between them, with its bar, its days' row and the months. */
	layout->main.x = left;
	layout->main.y = margin;
	layout->main.width = right - left;
	layout->main.height = height - 2 * margin;
	layout->topbar = layout->main;
	layout->topbar.height = CAL_TOPBAR;
	layout->weekdays = layout->main;
	layout->weekdays.y = layout->main.y + CAL_TOPBAR;
	layout->weekdays.height = CAL_WEEKDAYS;
	layout->grid = layout->main;
	layout->grid.y = layout->weekdays.y + CAL_WEEKDAYS;
	layout->grid.height = layout->main.height - CAL_TOPBAR - CAL_WEEKDAYS - 4;
}

/*
 * Draws a card's ground: on glass a light veil (the compositor's glass is under
 * it, as under Files' panels), else nearly white with a soft shadow and an
 * edge.
 */
static void
view_card(
	const struct cal_view *view,
	const struct kl_style *style,
	const struct kl_rect *rect,
	kl_color veil)
{
	/* On glass, the veil alone. */
	if (view->glass) {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CAL_CARD_RADIUS, veil);
		return;
	}

	/* Opaque: the shadow, the card and its edge. */
	kl_canvas_shadow(style->canvas, (float)rect->x, (float)rect->y + 2.0f, (float)rect->width, (float)rect->height, CAL_CARD_RADIUS, 10.0f, style->theme->shadow);
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CAL_CARD_RADIUS, CAL_COLOR_CARD);
	kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, CAL_CARD_RADIUS, 1.0f, style->theme->panel_edge);
}

/*
 * Draws the sidebar: the application's mark and name, the places, the
 * calendars to show or hide, and the card of encouragement at the bottom.
 */
static void
view_sidebar(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect row;
	struct kl_rect tip;
	const char *line;
	kl_color color;
	unsigned hit;
	size_t length;
	int clicked;
	int current;
	int shown;
	int x;
	int y;
	int i;

	/* The mark: a small page with a red band, and the name. */
	x = area->x + 16;
	y = area->y + 16;
	kl_canvas_round(style->canvas, (float)x, (float)y, 32.0f, 32.0f, 8.0f, style->theme->accent);
	kl_canvas_round(style->canvas, (float)x + 6.0f, (float)y + 7.0f, 20.0f, 19.0f, 4.0f, CAL_COLOR_WHITE);
	kl_canvas_round(style->canvas, (float)x + 6.0f, (float)y + 7.0f, 20.0f, 6.0f, 3.0f, KL_RGB(0xe5484d));
	(void)kl_text_draw(style->text, style->canvas, x + 42, y + 22, "Calendar", strlen("Calendar"), 17U, 1, style->theme->text);

	/* The places. */
	y = area->y + 64;
	for (i = 0; i < 4; i++) {
		/* One place; Month View is the one shown. */
		row.x = area->x + 8;
		row.y = y;
		row.width = area->width - 16;
		row.height = 34;
		current = 0;
		if (i == 0)
			current = 1;
		clicked = kl_sidebar_item(ui, style, CAL_ID_PLACE, (uint32_t)i, &row, view_place_icons[i], view_places[i], current);
		y += 36;

		/* Not clicked: nothing to do. */
		if (!clicked)
			continue;

		/* Today goes to today, Search to the search field, Settings is not in the mock. */
		if (i == 1)
			cal_view_action(view, CAL_ACTION_TODAY, now_us);
		else if (i == 2)
			kl_ui_set_focus(ui, CAL_ID_SEARCH, 0U);
		else if (i == 3)
			view_notice(view, "Calendar's settings are not in this version yet.", now_us);
	}

	/* The calendars: a box of its color each, filled while shown; a click shows or hides it. */
	row.x = area->x + 16;
	row.y = y + 10;
	row.width = area->width - 32;
	row.height = 1;
	kl_canvas_fill(style->canvas, &row, style->theme->row_separator);
	y = kl_sidebar_section(style, area->x + 8, y + 14, area->width - 16, "My Calendars");
	for (i = 0; i < CAL_LISTS; i++) {
		/* The row and its input. */
		row.x = area->x + 8;
		row.y = y;
		row.width = area->width - 16;
		row.height = 32;
		hit = kl_ui_hit(ui, CAL_ID_LIST, (uint32_t)i, &row);
		shown = 1;
		if ((view->hidden & (1U << i)) != 0U)
			shown = 0;

		/* A click shows or hides it. */
		if ((hit & KL_HIT_CLICKED) != 0U) {
			view->hidden ^= 1U << i;
			shown = 0;
			if ((view->hidden & (1U << i)) == 0U)
				shown = 1;
			cal_log("LIST %s shown=%d", cal_list_name((enum cal_list)i), shown);
		}

		/* The ground under the pointer. */
		if ((hit & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 9.0f, style->theme->hover);

		/* The box: filled with a check while shown, an outline while hidden. */
		color = cal_list_color((enum cal_list)i);
		if (shown) {
			kl_canvas_round(style->canvas, (float)row.x + 10.0f, (float)row.y + 8.0f, 16.0f, 16.0f, 4.0f, color);
			kl_canvas_line(style->canvas, (float)row.x + 13.5f, (float)row.y + 16.0f, (float)row.x + 17.0f, (float)row.y + 19.5f, 2.0f, CAL_COLOR_WHITE);
			kl_canvas_line(style->canvas, (float)row.x + 17.0f, (float)row.y + 19.5f, (float)row.x + 22.5f, (float)row.y + 12.5f, 2.0f, CAL_COLOR_WHITE);
		} else {
			kl_canvas_round_border(style->canvas, (float)row.x + 10.0f, (float)row.y + 8.0f, 16.0f, 16.0f, 4.0f, 1.5f, color);
		}

		/* Its name. */
		(void)kl_text_draw(style->text, style->canvas, row.x + 38, row.y + 21, cal_list_name((enum cal_list)i), strlen(cal_list_name((enum cal_list)i)), 14U, 0, style->theme->text);
		y += 34;
	}

	/* Add Calendar: not in the mock. */
	row.x = area->x + 8;
	row.y = y;
	row.width = area->width - 16;
	row.height = 32;
	hit = kl_ui_hit(ui, CAL_ID_ADD_LIST, 0U, &row);
	if ((hit & KL_HIT_CLICKED) != 0U)
		view_notice(view, "Adding a calendar is not in this version yet.", now_us);

	/* The row: the ground under the pointer, a plus and the words. */
	if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 9.0f, style->theme->hover);
	kl_icon_draw(style->canvas, KL_ICON_PLUS, (float)row.x + 9.0f, (float)row.y + 7.0f, 18.0f, style->theme->text_secondary);
	(void)kl_text_draw(style->text, style->canvas, row.x + 38, row.y + 21, "Add Calendar", strlen("Add Calendar"), 14U, 0, style->theme->text_secondary);

	/* The card of encouragement at the bottom, when there is room. */
	tip.x = area->x + 12;
	tip.width = area->width - 24;
	tip.height = 122;
	tip.y = area->y + area->height - 12 - tip.height;
	if (tip.y < y + 44)
		return;
	kl_canvas_round(style->canvas, (float)tip.x, (float)tip.y, (float)tip.width, (float)tip.height, 14.0f, KL_RGBA(style->theme->accent, 22));
	kl_canvas_image(style->canvas, &view->icons[SC_ICON_STUDY], (float)tip.x + 8.0f, (float)tip.y + 6.0f, 40.0f, 40.0f, 0.0f, 1.0f);
	(void)kl_text_draw_fit(style->text, style->canvas, tip.x + 12, tip.y + 66, "A more organized you", 13U, 1, tip.width - 24, style->theme->text);
	line = "Plan today for a brighter tomorrow.";
	length = kl_text_break(style->text, line, 12U, 0, tip.width - 24);
	(void)kl_text_draw(style->text, style->canvas, tip.x + 12, tip.y + 88, line, length, 12U, 0, style->theme->text_secondary);
	(void)kl_text_draw_fit(style->text, style->canvas, tip.x + 12, tip.y + 106, line + length, 12U, 0, tip.width - 24, style->theme->text_secondary);
}

/*
 * Draws the bar over the months: back and forward a month and Today at
 * the left, Month / Week / Day in the middle, the search and the menu at
 * the right.
 */
static void
view_topbar(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect button;
	struct kl_rect segment;
	struct kl_rect field;
	enum kl_icon chevron;
	kl_color ink;
	unsigned hits[3];
	unsigned hit;
	int pressed;
	int middle;
	int chosen;
	int i;

	/* Back and forward: a chevron in a soft circle each. */
	middle = area->y + area->height / 2;
	for (i = 0; i < 2; i++) {
		/* The button and its input. */
		button.x = area->x + 16 + i * 38;
		button.y = middle - 16;
		button.width = 32;
		button.height = 32;
		hit = kl_ui_hit(ui, CAL_ID_PREVIOUS + (uint32_t)i, 0U, &button);
		if ((hit & KL_HIT_CLICKED) != 0U)
			cal_view_action(view, CAL_ACTION_PREVIOUS + (unsigned)i, now_us);

		/* Drawn, darker under the pointer. */
		if ((hit & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
			kl_canvas_circle(style->canvas, (float)button.x + 16.0f, (float)button.y + 16.0f, 16.0f, style->theme->hover);

		/* Its chevron, back or forward. */
		chevron = KL_ICON_FORWARD;
		if (i == 0)
			chevron = KL_ICON_BACK;
		kl_icon_draw(style->canvas, chevron, (float)button.x + 7.0f, (float)button.y + 7.0f, 18.0f, style->theme->icon);
	}

	/* Today. */
	button.x = area->x + 16 + 80;
	button.y = middle - 16;
	button.width = 72;
	button.height = 32;
	pressed = kl_button(ui, style, CAL_ID_TODAY, &button, "Today", 0U);
	if (pressed)
		cal_view_action(view, CAL_ACTION_TODAY, now_us);

	/* Month / Week / Day in the middle. */
	segment.width = 3 * 62 + 6;
	segment.height = 32;
	segment.x = area->x + (area->width - segment.width) / 2;
	segment.y = middle - 16;
	kl_canvas_round(style->canvas, (float)segment.x, (float)segment.y, (float)segment.width, (float)segment.height, 10.0f, KL_RGBA(0x8a96aa, 30));
	for (i = 0; i < 3; i++) {
		/* One segment's input first, so that a click shows its view in this frame. */
		button.x = segment.x + 3 + i * 62;
		button.y = segment.y + 3;
		button.width = 62;
		button.height = 26;
		hits[i] = kl_ui_hit(ui, CAL_ID_SEGMENT, (uint32_t)i, &button);
		if ((hits[i] & KL_HIT_CLICKED) != 0U) {
			view->mode = i;
			cal_log("MODE %s", view_segments[i]);
		}
	}

	/* Then each drawn. */
	for (i = 0; i < 3; i++) {
		/* One segment. */
		button.x = segment.x + 3 + i * 62;
		button.y = segment.y + 3;
		button.width = 62;
		button.height = 26;
		hit = hits[i];

		/* The one chosen in the accent and bold, the others plain. */
		ink = style->theme->text;
		chosen = 0;
		if (i == view->mode) {
			kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->accent);
			ink = style->theme->accent_ink;
			chosen = 1;
		} else if ((hit & KL_HIT_HOT) != 0U) {
			kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->hover);
		}

		/* Its name. */
		view_centred(style, button.x + button.width / 2, button.y + 18, view_segments[i], 13U, chosen, ink);
	}

	/* The menu at the right, and the search before it. */
	button.x = area->x + area->width - 16 - 32;
	button.y = middle - 16;
	button.width = 32;
	button.height = 32;
	hit = kl_ui_hit(ui, CAL_ID_MORE, 0U, &button);
	if ((hit & KL_HIT_CLICKED) != 0U)
		view_notice(view, "The menu is not in this version yet.", now_us);

	/* The menu's three dots, on a soft circle under the pointer. */
	if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_circle(style->canvas, (float)button.x + 16.0f, (float)button.y + 16.0f, 16.0f, style->theme->hover);
	for (i = 0; i < 3; i++)
		kl_canvas_circle(style->canvas, (float)button.x + 9.0f + 7.0f * (float)i, (float)button.y + 16.0f, 1.8f, style->theme->icon);

	/* The search before it, when it does not reach the views. */
	field.width = 180;
	field.height = 32;
	field.x = button.x - 8 - field.width;
	field.y = middle - 16;
	if (field.x > segment.x + segment.width + 12)
		(void)kl_field(ui, style, CAL_ID_SEARCH, &field, &view->search, "Search events...");

}

/*
 * Draws the days of the week over the months' columns, Sunday in red and
 * Saturday in blue.
 */
static void
view_weekdays(
	const struct kl_style *style,
	const struct kl_rect *area)
{
	static const char *const names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	kl_color ink;
	int column;
	int i;

	/* Each day's name in the middle of its column. */
	column = (area->width - 2 * CAL_GRID_PAD) / 7;
	for (i = 0; i < 7; i++) {
		/* Its color. */
		ink = style->theme->text_secondary;
		if (i == 0)
			ink = CAL_COLOR_SUNDAY_TEXT;
		else if (i == 6)
			ink = CAL_COLOR_SATURDAY_TEXT;
		view_centred(style, area->x + CAL_GRID_PAD + column * i + column / 2, area->y + 20, names[i], 12U, 1, ink);
	}
}

/*
 * Draws the months one under another in their band, scrolled: each its
 * heading and its weeks of cells.
 */
static void
view_grid(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct cal_date first;
	struct cal_date date;
	struct kl_rect cell;
	struct kl_rect line;
	char heading[64];
	int cell_height;
	int column;
	int content;
	int top;
	int rows;
	int lead;
	int length;
	int inside;
	int month;
	int row;
	int i;

	/* The cells' size, and the months' height. */
	cell_height = view_cell_height(area);
	column = (area->width - 2 * CAL_GRID_PAD) / 7;
	content = view_month_top(view, CAL_MONTHS, cell_height) + CAL_GRID_PAD;
	kl_scroll_set_size(&view->scroll, (double)area->width, (double)content, (double)area->width, (double)area->height);

	/* A month to go to (counted from 1): its heading at the top. */
	if (view->scroll_to > 0) {
		top = view_month_top(view, view->scroll_to - 1, cell_height);
		kl_scroll_move_to(&view->scroll, 0.0, (double)top, view->scroll_glide && !view->reduce_motion, now_us);
		view->scroll_to = 0;
	}

	/* The viewport takes the wheel and a finger's drag; its cells are found again. */
	kl_ui_scroll_region(ui, CAL_ID_GRID, area, &view->scroll);
	kl_canvas_clip_push(style->canvas, area);
	view->cell_count = 0;

	/* Each month whose block shows. */
	for (month = 0; month < CAL_MONTHS; month++) {
		/* Where it is, and whether it shows. */
		top = area->y + view_month_top(view, month, cell_height) - (int)view->scroll.y;
		rows = view_month_rows(view, month, &first);
		if (top > area->y + area->height || top + CAL_HEADING + rows * cell_height < area->y)
			continue;

		/* Its heading: the month bold, the year lighter. */
		(void)snprintf(heading, sizeof(heading), "%s", cal_month_name(first.month));
		(void)kl_text_draw(style->text, style->canvas, area->x + CAL_GRID_PAD + 4, top + 36, heading, strlen(heading), 22U, 1, style->theme->text);
		(void)snprintf(heading, sizeof(heading), "%d", first.year);
		(void)kl_text_draw(style->text, style->canvas, area->x + CAL_GRID_PAD + 12 + kl_text_width(style->text, cal_month_name(first.month), strlen(cal_month_name(first.month)), 22U, 1), top + 36, heading, strlen(heading), 22U, 0, style->theme->text_faint);

		/* Its weeks: the first starts on the Sunday before the 1st. */
		lead = cal_weekday(&first);
		length = cal_days_in_month(first.year, first.month);
		for (row = 0; row < rows; row++) {
			for (i = 0; i < 7; i++) {
				/* The cell and its day (a day of the month before or after is grey). */
				cell.x = area->x + CAL_GRID_PAD + column * i;
				cell.y = top + CAL_HEADING + row * cell_height;
				cell.width = column;
				cell.height = cell_height;
				date = first;
				cal_add_days(&date, row * 7 + i - lead);
				inside = 0;
				if (row * 7 + i - lead >= 0 && row * 7 + i - lead < length)
					inside = 1;

				/* A cell out of the band is not drawn. */
				if (cell.y > area->y + area->height || cell.y + cell.height < area->y)
					continue;

				/* The cell. */
				view_cell(view, ui, style, &cell, area, &date, inside, now_us);
			}
		}

		/* The lines between the weeks and the edge of the month. */
		for (row = 0; row <= rows; row++) {
			line.x = area->x + CAL_GRID_PAD;
			line.y = top + CAL_HEADING + row * cell_height;
			line.width = column * 7;
			line.height = 1;
			kl_canvas_fill(style->canvas, &line, style->theme->row_separator);
		}
	}

	/* The clip goes, and the bar shows while the months move. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->scroll, style->canvas, area, style->theme, now_us);
}

/*
 * Draws one cell: its ground (Sunday's and Saturday's, today's, sinking
 * after a drop), its number, its events as pills, the ring of the day
 * chosen or of a drop's target; a day of the month takes clicks.
 */
static void
view_cell(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *cell,
	const struct kl_rect *band,
	const struct cal_date *date,
	int inside,
	uint64_t now_us)
{
	struct view_entry entries[CAL_DAY_EVENTS];
	struct kl_rect ground;
	struct kl_rect seen;
	char number[16];
	char more[32];
	kl_color ink;
	kl_color color;
	kl_color tint;
	int sinking;
	int chosen;
	double px;
	double py;
	float depth;
	unsigned hit;
	size_t count;
	size_t fits;
	size_t i;
	int weekday;
	int today;
	int matched;
	int y;

	/* The weekend's grounds. */
	weekday = cal_weekday(date);
	ground = *cell;
	if (weekday == 0)
		kl_canvas_fill(style->canvas, &ground, CAL_COLOR_SUNDAY);
	else if (weekday == 6)
		kl_canvas_fill(style->canvas, &ground, CAL_COLOR_SATURDAY);

	/* A day of the month before or after: its number, grey. */
	(void)snprintf(number, sizeof(number), "%d", date->day);
	if (!inside) {
		(void)kl_text_draw(style->text, style->canvas, cell->x + 10, cell->y + 21, number, strlen(number), 13U, 0, style->theme->text_faint);
		return;
	}

	/* The part of it within the months' band, which alone takes input. */
	seen = *cell;
	if (seen.y < band->y) {
		seen.height -= band->y - seen.y;
		seen.y = band->y;
	}

	/* Cut at the band's bottom too. */
	if (seen.y + seen.height > band->y + band->height)
		seen.height = band->y + band->height - seen.y;

	/* Its input, and where a drop finds it. */
	hit = 0;
	if (seen.height > 0)
		hit = kl_ui_hit(ui, CAL_ID_CELL, (uint32_t)(date->year * 10000 + date->month * 100 + date->day), &seen);
	if ((hit & KL_HIT_CLICKED) != 0U) {
		kl_ui_clear_focus(ui);
		view_select(view, date, now_us);
	}

	/* Where a drop finds it. */
	if (view->cell_count < CAL_CELLS_MAX && seen.height > 0) {
		view->cells[view->cell_count].rect = seen;
		view->cells[view->cell_count].date = *date;
		view->cell_count++;
	}

	/* Today's cell, logged when it moves (the tests drop on it). */
	today = cal_same_day(date, &view->today);
	if (today && seen.height > 0 && (seen.x != view->today_x || seen.y != view->today_y)) {
		view->today_x = seen.x;
		view->today_y = seen.y;
		cal_log("CELL today x=%d y=%d width=%d height=%d", seen.x, seen.y, seen.width, seen.height);
	}

	/* Sinking after a drop: pressed in a little, and shaded, for a moment. */
	sinking = 0;
	if (view->sinking)
		sinking = cal_same_day(date, &view->sink_date);
	if (sinking) {
		depth = sinf(CAL_PI * (float)(now_us - view->sink_us) / (float)CAL_SINK_US);
		ground.x += (int)(3.0f * depth);
		ground.y += (int)(3.0f * depth);
		ground.width -= (int)(6.0f * depth);
		ground.height -= (int)(6.0f * depth);
		kl_canvas_round(style->canvas, (float)ground.x, (float)ground.y, (float)ground.width, (float)ground.height, 8.0f, KL_RGBA(style->theme->accent, (unsigned)(60.0f * depth)));
	}

	/* The number's color: red on a Sunday, blue on a Saturday. */
	ink = style->theme->text;
	if (weekday == 0)
		ink = CAL_COLOR_SUNDAY_TEXT;
	else if (weekday == 6)
		ink = CAL_COLOR_SATURDAY_TEXT;

	/* Today: the cell in the accent's tint, its number white in an accent circle. */
	today = cal_same_day(date, &view->today);
	if (today) {
		kl_canvas_round(style->canvas, (float)ground.x + 2.0f, (float)ground.y + 2.0f, (float)ground.width - 4.0f, (float)ground.height - 4.0f, 8.0f, KL_RGBA(style->theme->accent, 30));
		kl_canvas_circle(style->canvas, (float)cell->x + 17.0f, (float)ground.y + 16.0f, 12.0f, style->theme->accent);
		ink = style->theme->accent_ink;
	}

	/* The number. */
	if (today) {
		view_centred(style, cell->x + 17, ground.y + 21, number, 13U, 1, ink);
	} else {
		(void)kl_text_draw(style->text, style->canvas, cell->x + 10, ground.y + 21, number, strlen(number), 13U, 0, ink);
	}

	/* The events as pills, as many as fit; "+N more" for the rest. */
	count = view_day(view, date, entries, CAL_DAY_EVENTS);
	fits = (size_t)((cell->height - 34) / (CAL_PILL + CAL_PILL_GAP));
	if (count > fits && fits > 0U)
		fits--;
	y = ground.y + 30;
	for (i = 0; i < count && i < fits; i++) {
		/* A memo: an outlined pill with its note, apart from the events. */
		if (entries[i].memo != NULL) {
			kl_canvas_round(style->canvas, (float)cell->x + 5.0f, (float)y, (float)cell->width - 10.0f, (float)CAL_PILL, 6.0f, kl_theme_choose(KL_RGBA(0xffffff, 200), KL_RGBA(0x2a2f38, 200)));
			kl_canvas_round_border(style->canvas, (float)cell->x + 5.0f, (float)y, (float)cell->width - 10.0f, (float)CAL_PILL, 6.0f, 1.0f, CAL_COLOR_MEMO_EDGE);
			view_note_icon(style->canvas, (float)cell->x + 8.0f, (float)y + 3.0f, 12.0f);
			(void)kl_text_draw_fit(style->text, style->canvas, cell->x + 24, y + 13, entries[i].title, 11U, 0, cell->width - 33, style->theme->text);
			y += CAL_PILL + CAL_PILL_GAP;
			continue;
		}

		/* Its colors: its calendar's tint and the text's, fainter when the search does not match it. */
		color = cal_list_color(entries[i].list);
		matched = view_matches(view, entries[i].title);
		tint = (color & 0xffffffU) | 0x30000000U;
		ink = style->theme->text;
		if (!matched) {
			tint = (color & 0xffffffU) | 0x12000000U;
			ink = style->theme->text_faint;
		}

		/* The pill, a dot of its color, the title. */
		kl_canvas_round(style->canvas, (float)cell->x + 5.0f, (float)y, (float)cell->width - 10.0f, (float)CAL_PILL, 6.0f, tint);
		kl_canvas_circle(style->canvas, (float)cell->x + 12.0f, (float)y + 9.0f, 3.0f, color);
		(void)kl_text_draw_fit(style->text, style->canvas, cell->x + 19, y + 13, entries[i].title, 11U, 0, cell->width - 28, ink);
		y += CAL_PILL + CAL_PILL_GAP;
	}

	/* The rest, counted. */
	if (count > i) {
		(void)snprintf(more, sizeof(more), "+%zu more", count - i);
		(void)kl_text_draw(style->text, style->canvas, cell->x + 10, y + 12, more, strlen(more), 11U, 0, style->theme->text_secondary);
	}

	/* The day chosen: a ring of the accent. */
	chosen = cal_same_day(date, &view->selected);
	if (chosen)
		kl_canvas_round_border(style->canvas, (float)ground.x + 2.0f, (float)ground.y + 2.0f, (float)ground.width - 4.0f, (float)ground.height - 4.0f, 8.0f, 2.0f, style->theme->accent);

	/* The target of a drag: a ring and a tint. */
	if (view->dragging >= 0 && view->drag_moved) {
		/* The cell under the pointer. */
		kl_ui_pointer(ui, &px, &py);
		if (px >= cell->x &&
		    px < cell->x + cell->width &&
		    py >= cell->y &&
		    py < cell->y + cell->height) {
			kl_canvas_round(style->canvas, (float)cell->x + 2.0f, (float)cell->y + 2.0f, (float)cell->width - 4.0f, (float)cell->height - 4.0f, 8.0f, KL_RGBA(style->theme->accent, 24));
			kl_canvas_round_border(style->canvas, (float)cell->x + 2.0f, (float)cell->y + 2.0f, (float)cell->width - 4.0f, (float)cell->height - 4.0f, 8.0f, 2.0f, KL_RGBA(style->theme->accent, 160));
		}
	}
}

/*
 * Draws the panel to add an event: its title and what to do, a card for
 * each kind (dragged onto a date, it adds an event), Custom, and the desk
 * calendar in 3D with its words.
 */
static void
view_panel(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect day;
	struct kl_rect editor;
	int memo_height;
	int bottom;
	int top;

	/* An event being edited: the editor in place of the kinds. */
	if (view->editing) {
		editor.x = area->x + 14;
		editor.y = area->y + 14;
		editor.width = area->width - 28;
		editor.height = 92 + 2 * (CAL_KIND_HEIGHT + 8) + 52 - 14;
		view_editor(view, ui, style, &editor, now_us);
		bottom = editor.y + editor.height;
	}

	/* Otherwise the kinds of event to drag. */
	if (!view->editing)
		bottom = view_kind_cards(view, ui, style, area, now_us);

	/*
	 * The memo below, as tall as it may be while the day chosen keeps the
	 * room it needs under it (a short window makes the memo shorter, down
	 * to a few lines), then the day chosen while there is room.
	 */
	top = bottom + 14;
	memo_height = area->y + area->height - 14 - CAL_DAY_MIN - 12 - (top + CAL_MEMO_HEADER + 4);
	if (memo_height > CAL_MEMO_HEIGHT)
		memo_height = CAL_MEMO_HEIGHT;
	else if (memo_height < CAL_MEMO_LEAST)
		memo_height = CAL_MEMO_LEAST;
	top = view_memo(view, ui, style, area, top, memo_height, now_us);
	day.x = area->x + 14;
	day.y = top + 12;
	day.width = area->width - 28;
	day.height = area->y + area->height - 14 - day.y;
	if (day.height >= CAL_DAY_DESK_HEIGHT + 8)
		view_chosen_day(view, ui, style, &day, now_us);
}

/* Draws the title, the kinds of event to drag onto a date and Custom; returns the bottom of what it drew. */
static int
view_kind_cards(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect card;
	const char *line;
	kl_color ground;
	unsigned hit;
	size_t length;
	int card_width;
	int top;
	int i;

	/* The title and what to do. */
	(void)kl_text_draw(style->text, style->canvas, area->x + 18, area->y + 36, "Add Event", strlen("Add Event"), 18U, 1, style->theme->text);
	line = "Drag an icon to a date on the calendar to create a new event.";
	length = kl_text_break(style->text, line, 12U, 0, area->width - 36);
	(void)kl_text_draw(style->text, style->canvas, area->x + 18, area->y + 58, line, length, 12U, 0, style->theme->text_secondary);
	(void)kl_text_draw_fit(style->text, style->canvas, area->x + 18, area->y + 75, line + length, 12U, 0, area->width - 36, style->theme->text_secondary);

	/* The kinds' cards, two by two. */
	card_width = (area->width - 2 * 14 - 8) / 2;
	top = area->y + 92;
	for (i = 0; i < SC_ICONS; i++) {
		/* The card and its input: a press held and moved is a drag. */
		card.x = area->x + 14 + (i % 2) * (card_width + 8);
		card.y = top + (i / 2) * (CAL_KIND_HEIGHT + 8);
		card.width = card_width;
		card.height = CAL_KIND_HEIGHT;
		hit = view_drag_source(view, ui, CAL_ID_KIND, i, &card, now_us);

		/* Drawn: white (a lighter veil on glass), tinted under the pointer, its icon, its kind and what it holds. */
		ground = CAL_COLOR_SURFACE;
		if (view->glass)
			ground = CAL_COLOR_KIND_GLASS;
		kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, ground);
		if ((hit & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
			kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, KL_RGBA(style->theme->accent, 16));
		kl_canvas_round_border(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, 1.0f, style->theme->panel_edge);
		kl_canvas_image(style->canvas, &view->icons[i], (float)(card.x + (card.width - CAL_ICON) / 2), (float)card.y + 6.0f, (float)CAL_ICON, (float)CAL_ICON, 0.0f, 1.0f);
		view_centred(style, card.x + card.width / 2, card.y + 78, view_kinds[i], 13U, 1, style->theme->text);
		view_centred(style, card.x + card.width / 2, card.y + 96, view_kind_lines[i], 10U, 0, style->theme->text_secondary);
	}

	/* Custom: not in the mock. */
	card.x = area->x + 14;
	card.y = top + 2 * (CAL_KIND_HEIGHT + 8);
	card.width = area->width - 28;
	card.height = 52;
	hit = kl_ui_hit(ui, CAL_ID_CUSTOM, 0U, &card);
	if ((hit & KL_HIT_CLICKED) != 0U)
		view_notice(view, "Custom kinds are not in this version yet.", now_us);
	ground = CAL_COLOR_SURFACE;
	if (view->glass)
		ground = CAL_COLOR_KIND_GLASS;
	kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, ground);
	if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, KL_RGBA(style->theme->accent, 16));
	kl_canvas_round_border(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, 1.0f, style->theme->panel_edge);
	kl_canvas_circle(style->canvas, (float)card.x + 28.0f, (float)card.y + 26.0f, 16.0f, KL_RGBA(style->theme->accent, 30));
	kl_icon_draw(style->canvas, KL_ICON_PLUS, (float)card.x + 19.0f, (float)card.y + 17.0f, 18.0f, style->theme->accent);
	(void)kl_text_draw(style->text, style->canvas, card.x + 54, card.y + 23, "Custom", strlen("Custom"), 13U, 1, style->theme->text);
	(void)kl_text_draw(style->text, style->canvas, card.x + 54, card.y + 40, "Create your own", strlen("Create your own"), 11U, 0, style->theme->text_secondary);

	/* Succeeded: the bottom of the Custom card. */
	return card.y + card.height;
}

/*
 * Draws the small desk calendar of the day chosen in 3D in an area: still,
 * its page turning over when the day changes, a soft shadow under.
 */
static void
view_desk(
	struct cal_view *view,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct r3_texture textures[SC_TEXTURES];
	struct r3_matrix turn;
	struct r3_matrix tilt;
	struct r3_matrix place;
	struct cal_date next;
	float flip;
	float opacity;
	float progress;
	int error;
	int i;

	/* The target and the picture at the area's size (made again when it changes). */
	if (view->picture.width != area->width || view->picture.height != area->height) {
		kl_image_release(&view->picture);
		r3_target_release(&view->target);
		error = kl_image_create(&view->picture, area->width, area->height);
		if (error != 0)
			return;
		error = r3_target_init(&view->target, area->width * CAL_SUPERSAMPLE, area->height * CAL_SUPERSAMPLE);
		if (error != 0) {
			kl_image_release(&view->picture);
			return;
		}
	}

	/* The pages' pictures: the day shown, and under it the day it turns to. */
	next = view->shown;
	if (view->flipping)
		next = view->flip_to;
	view_page(view, style, SC_TEXTURE_PAGE, &view->shown);
	view_page(view, style, SC_TEXTURE_NEXT, &next);
	for (i = 0; i < SC_TEXTURES; i++) {
		textures[i].pixels = view->pages[i].pixels;
		textures[i].width = view->pages[i].width;
		textures[i].height = view->pages[i].height;
		textures[i].stride = view->pages[i].stride;
	}

	/* The page turning: over the top, eased, fading out from a third of the way. */
	flip = 0.0f;
	opacity = 1.0f;
	if (view->flipping) {
		progress = (float)(now_us - view->flip_us) / (float)CAL_FLIP_US;
		flip = 0.85f * CAL_PI * view_ease(progress);
		if (progress > 0.3f)
			opacity = 1.0f - view_ease((progress - 0.3f) / 0.6f);
	}

	/* The camera: the desk calendar a little turned and tilted, before it. */
	view->target.focal = (float)view->target.height * 2.3f;
	view->target.cx = (float)view->target.width * 0.5f;
	view->target.cy = (float)view->target.height * 0.5f;
	r3_rotate_y(&turn, -0.3f);
	r3_rotate_x(&tilt, 0.18f);
	r3_translate(&place, 0.0f, 0.0f, CAL_DESK_DISTANCE);
	r3_combine(&tilt, &turn, &turn);
	r3_combine(&place, &turn, &turn);

	/* The desk calendar. */
	r3_target_clear(&view->target);
	sc_mesh_clear(view->mesh);
	sc_desk_calendar(view->mesh, flip, opacity);
	sc_draw(&view->target, view->mesh, &turn, textures);

	/* What is under it, kept for the frames of the desk alone, and the shadow on the desk. */
	view_keep_under(view, style, area);
	kl_canvas_shadow(style->canvas, (float)area->x + (float)area->width * 0.2f, (float)area->y + (float)area->height * 0.8f, (float)area->width * 0.6f, 5.0f, 3.0f, 6.0f, KL_RGBA(0x1f3a66, 40));

	/* The picture over it. */
	r3_resolve(&view->target, CAL_SUPERSAMPLE, &view->picture);
	kl_canvas_image(style->canvas, &view->picture, (float)area->x, (float)area->y, (float)area->width, (float)area->height, 0.0f, 1.0f);
}

/*
 * Keeps what is on the canvas under the desk calendar's area, for the
 * frames of the desk alone; the area must lie within the canvas.
 */
static void
view_keep_under(
	struct cal_view *view,
	const struct kl_style *style,
	const struct kl_rect *area)
{
	const uint32_t *line;
	size_t row;
	int error;

	/* Within the canvas, or not kept. */
	if (area->x < 0 ||
	    area->y < 0 ||
	    area->x + area->width > style->canvas->width ||
	    area->y + area->height > style->canvas->height)
		return;

	/* A picture of the area's size (made again when it changes). */
	if (view->under.width != area->width || view->under.height != area->height) {
		kl_image_release(&view->under);
		error = kl_image_create(&view->under, area->width, area->height);
		if (error != 0)
			return;
	}

	/* The canvas's rows under it. */
	for (row = 0; row < (size_t)area->height; row++) {
		line = style->canvas->pixels + ((size_t)area->y + row) * style->canvas->stride + (size_t)area->x;
		memcpy(view->under.pixels + row * view->under.stride, line, (size_t)area->width * sizeof(line[0]));
	}

	/* Kept: a frame of the desk alone may follow. */
	view->desk_area = *area;
	view->desk_valid = 1;
}

/*
 * Draws a page's picture for a day, unless it shows that day already: a
 * band with the month (red on a Sunday), the day's number large, and the
 * day of the week.
 */
static void
view_page(
	struct cal_view *view,
	const struct kl_style *style,
	int index,
	const struct cal_date *date)
{
	struct kl_style page_style;
	struct kl_canvas canvas;
	struct kl_image *image;
	struct kl_rect rect;
	char text[32];
	kl_color band;
	kl_color band_ink;
	kl_color ink;
	int weekday;
	int error;
	int same;
	size_t i;

	/* The picture, made once. */
	image = &view->pages[index];
	if (image->pixels == NULL) {
		error = kl_image_create(image, CAL_PAGE_WIDTH, CAL_PAGE_HEIGHT);
		if (error != 0)
			return;
		view->page_dates[index].year = 0;
	}

	/* Showing the day already, in the accent the user chose now (ws179-p002). */
	same = cal_same_day(&view->page_dates[index], date);
	if (same && view->page_accents[index] == style->theme->accent)
		return;

	/* A canvas on it. */
	error = kl_canvas_init(&canvas, image->pixels, image->stride, image->width, image->height);
	if (error != 0)
		return;

	/* The colors: the band in the accent the user chose with its ink (red with white on a Sunday), the number red on a Sunday and blue on a Saturday. */
	weekday = cal_weekday(date);
	band = style->theme->accent;
	band_ink = style->theme->accent_ink;
	ink = style->theme->text;
	if (weekday == 0) {
		band = KL_RGB(0xe5484d);
		band_ink = CAL_COLOR_WHITE;
		ink = CAL_COLOR_SUNDAY_TEXT;
	} else if (weekday == 6) {
		ink = CAL_COLOR_SATURDAY_TEXT;
	}

	/* The paper, and the band at its top. */
	rect.x = 0;
	rect.y = 0;
	rect.width = image->width;
	rect.height = image->height;
	kl_canvas_fill(&canvas, &rect, CAL_COLOR_WHITE);
	rect.height = 64;
	kl_canvas_fill(&canvas, &rect, band);

	/* The month in capitals and the year on the band. */
	(void)snprintf(text, sizeof(text), "%s %d", cal_month_name(date->month), date->year);
	for (i = 0; text[i] != '\0'; i++) {
		/* One letter in capitals. */
		if (text[i] >= 'a' && text[i] <= 'z')
			text[i] = (char)(text[i] - 'a' + 'A');
	}

	/* The words, centred, through a style on the page's canvas. */
	page_style = *style;
	page_style.canvas = &canvas;
	view_centred(&page_style, image->width / 2, 42, text, 20U, 1, band_ink);
	(void)snprintf(text, sizeof(text), "%d", date->day);
	view_centred(&page_style, image->width / 2, 196, text, 118U, 1, ink);
	view_centred(&page_style, image->width / 2, 240, cal_weekday_name(weekday), 22U, 0, style->theme->text_secondary);

	/* The page's lower edge, a faint line. */
	rect.y = image->height - 2;
	rect.height = 2;
	kl_canvas_fill(&canvas, &rect, KL_RGB(0xe3e8f0));

	/* Done: it shows the day. */
	kl_canvas_release(&canvas);
	view->page_dates[index] = *date;
	view->page_accents[index] = style->theme->accent;
}

/*
 * Carries out the end of a press on a kind's card at a point: a drag let
 * go on a day adds an event there, a press that did not move says what to
 * do, a drag let go elsewhere does nothing.
 */
static void
view_let_go(
	struct cal_view *view,
	int kind,
	double x,
	double y,
	uint64_t now_us)
{
	const struct kl_rect *rect;
	size_t i;

	/* A click: what to do. */
	if (!view->drag_moved && kind == CAL_DRAG_MEMO) {
		view_notice(view, "Drag the memo onto a date to keep it there.", now_us);
		return;
	} else if (!view->drag_moved) {
		view_notice(view, "Drag the card onto a date to add an event.", now_us);
		return;
	}

	/* The day under the point, among the cells of the frame. */
	for (i = 0; i < view->cell_count; i++) {
		/* A cell holding the point: the memo kept there, or an event added. */
		rect = &view->cells[i].rect;
		if (x >= rect->x &&
		    x < rect->x + rect->width &&
		    y >= rect->y &&
		    y < rect->y + rect->height) {
			if (kind == CAL_DRAG_MEMO)
				view_memo_drop(view, &view->cells[i].date, now_us);
			else
				view_drop(view, view_kind_lists[kind], &view->cells[i].date, now_us);
			return;
		}
	}
}

/*
 * Records a card that may be dragged onto a date (a kind of event, or the
 * memo) and follows its drag: a press held and moved is a drag, its end
 * is carried out where it is let go.  Returns what the input did to it
 * (KL_HIT_* bits).
 */
static unsigned
view_drag_source(
	struct cal_view *view,
	struct kl_ui *ui,
	uint32_t id,
	int kind,
	const struct kl_rect *rect,
	uint64_t now_us)
{
	double px;
	double py;
	double distance;
	unsigned hit;

	/* The card, and a press on it starting a drag (none other under way). */
	hit = kl_ui_hit(ui, id, (uint32_t)kind, rect);
	kl_ui_pointer(ui, &px, &py);
	if ((hit & KL_HIT_ACTIVE) != 0U && view->dragging < 0) {
		view->dragging = kind;
		view->drag_moved = 0;
		view->drag_from_x = px;
		view->drag_from_y = py;
	}

	/* Held and moved far enough from where it was pressed: a drag. */
	distance = fabs(px - view->drag_from_x) + fabs(py - view->drag_from_y);
	if (view->dragging == kind && (hit & KL_HIT_ACTIVE) != 0U && distance > CAL_DRAG_START)
		view->drag_moved = 1;

	/* Let go: carried out where it is. */
	if (view->dragging == kind && (hit & KL_HIT_ACTIVE) == 0U) {
		view_let_go(view, kind, px, py, now_us);
		view->dragging = -1;
	}

	/* What the input did to it. */
	return hit;
}

/*
 * Draws the application's memo from a top edge in the panel: its header
 * (a note, "Memo", and a grip to drag it by onto a date) and its words,
 * which take the keyboard when clicked, in a height.  Returns the edge
 * below it.
 */
static int
view_memo(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	int top,
	int height,
	uint64_t now_us)
{
	struct kl_rect header;
	struct kl_rect text;
	unsigned changes;
	unsigned hit;
	int error;
	int i;

	/* The header, which drags the memo: a note, the title, the grip and a word of what to do. */
	header.x = area->x + 14;
	header.y = top;
	header.width = area->width - 28;
	header.height = CAL_MEMO_HEADER;
	hit = view_drag_source(view, ui, CAL_ID_MEMO_GRIP, CAL_DRAG_MEMO, &header, now_us);
	if ((hit & (KL_HIT_HOT | KL_HIT_ACTIVE)) != 0U)
		kl_canvas_round(style->canvas, (float)header.x, (float)header.y, (float)header.width, (float)header.height, 8.0f, style->theme->hover);
	view_note_icon(style->canvas, (float)header.x + 6.0f, (float)header.y + 6.0f, 18.0f);
	(void)kl_text_draw(style->text, style->canvas, header.x + 32, header.y + 20, "Memo", strlen("Memo"), 15U, 1, style->theme->text);
	(void)kl_text_draw(style->text, style->canvas, header.x + header.width - 112, header.y + 19, "Drag to a date", strlen("Drag to a date"), 11U, 0, style->theme->text_secondary);
	for (i = 0; i < 6; i++)
		kl_canvas_circle(style->canvas, (float)(header.x + header.width - 14 + (i % 2) * 5), (float)(header.y + 10 + (i / 2) * 5), 1.4f, style->theme->text_faint);

	/* The words, in libkeiland's text area (an input method's text too, ws090-p022). */
	text.x = area->x + 14;
	text.y = top + CAL_MEMO_HEADER + 4;
	text.width = area->width - 28;
	text.height = height;
	changes = kl_text_area(ui, style, CAL_ID_MEMO_TEXT, &text, &view->memo, "Write a memo...");

	/* A change is kept at once (one small file). */
	if ((changes & KL_FIELD_CHANGED) != 0U) {
		error = cal_store_set_memo(view->memo.text);
		if (error != 0)
			cal_log("MEMO save-failed error=%d", error);
	}

	/* The edge below it. */
	return text.y + text.height;
}

/*
 * Draws the day chosen on a card: its small desk calendar in 3D (its page
 * turning when the day changes), its date, and its events and memos (a
 * memo's words in full).
 */
static void
view_chosen_day(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *card,
	uint64_t now_us)
{
	struct view_entry entries[CAL_DAY_EVENTS];
	struct kl_rect desk;
	struct kl_rect row;
	unsigned hit;
	int top;
	const char *plural;
	char line[160];
	size_t count;
	size_t i;
	int weekday;
	int y;

	/* The card, a faint veil. */
	kl_canvas_round(style->canvas, (float)card->x, (float)card->y, (float)card->width, (float)card->height, 14.0f, KL_RGBA(style->theme->accent, 14));

	/* The small desk calendar at the left of its header. */
	desk.x = card->x + 6;
	desk.y = card->y + 4;
	desk.width = CAL_DAY_DESK_WIDTH;
	desk.height = CAL_DAY_DESK_HEIGHT;
	view_desk(view, style, &desk, now_us);

	/* The day's date and how many things it holds. */
	weekday = cal_weekday(&view->selected);
	count = view_day(view, &view->selected, entries, CAL_DAY_EVENTS);
	(void)kl_text_draw_fit(style->text, style->canvas, card->x + 90, card->y + 34, cal_weekday_name(weekday), 15U, 1, card->width - 100, style->theme->text);
	(void)snprintf(line, sizeof(line), "%s %d, %d", cal_month_name(view->selected.month), view->selected.day, view->selected.year);
	(void)kl_text_draw_fit(style->text, style->canvas, card->x + 90, card->y + 54, line, 12U, 0, card->width - 100, style->theme->text_secondary);
	plural = "s";
	if (count == 1U)
		plural = "";
	(void)snprintf(line, sizeof(line), "%zu item%s", count, plural);
	(void)kl_text_draw_fit(style->text, style->canvas, card->x + 90, card->y + 72, line, 11U, 0, card->width - 100, style->theme->text_faint);

	/* Its items under the header, as far as the card goes. */
	kl_canvas_clip_push(style->canvas, card);
	y = card->y + CAL_DAY_DESK_HEIGHT + 14;
	for (i = 0; i < count && y < card->y + card->height; i++) {
		/* A memo: its note and its words in full. */
		top = y;
		if (entries[i].memo != NULL) {
			view_note_icon(style->canvas, (float)card->x + 12.0f, (float)y - 12.0f, 14.0f);
			y += view_words(style, entries[i].memo, card->x + 32, y - 15, card->width - 44, 12U, style->theme->text, 1) + 4;
		} else {
			/* An event: its calendar's dot, its time and its title. */
			kl_canvas_circle(style->canvas, (float)card->x + 18.0f, (float)y - 4.0f, 4.0f, cal_list_color(entries[i].list));
			if (entries[i].time[0] != '\0')
				(void)snprintf(line, sizeof(line), "%s  %s", entries[i].time, entries[i].title);
			else
				(void)snprintf(line, sizeof(line), "All day  %s", entries[i].title);
			(void)kl_text_draw_fit(style->text, style->canvas, card->x + 32, y, line, 12U, 0, card->width - 44, style->theme->text);
			y += 20;
		}

		/* The row opens the item in the editor. */
		row.x = card->x + 6;
		row.y = top - 16;
		row.width = card->width - 12;
		row.height = y - top;
		hit = kl_ui_hit(ui, CAL_ID_DAY_ITEM, (uint32_t)entries[i].index, &row);
		if ((hit & KL_HIT_CLICKED) != 0U)
			view_edit(view, entries[i].index, now_us);
	}

	/* The clip goes; a day with nothing says so. */
	kl_canvas_clip_pop(style->canvas);
	if (count == 0U)
		(void)kl_text_draw(style->text, style->canvas, card->x + 14, card->y + CAL_DAY_DESK_HEIGHT + 14, "Nothing on this day.", strlen("Nothing on this day."), 12U, 0, style->theme->text_faint);
}

/*
 * Keeps the application's memo on a day (while the program runs): its
 * first line its title; the cell sinks a moment and the day is chosen.
 */
static void
view_memo_drop(
	struct cal_view *view,
	const struct cal_date *date,
	uint64_t now_us)
{
	char message[128];
	int error;

	/* An empty memo keeps nothing. */
	if (view->memo.length == 0U) {
		view_notice(view, "Write something in the memo first.", now_us);
		return;
	}

	/* A copy of the memo kept on the day (its first line its title). */
	error = cal_store_add_memo(date, view->memo.text);
	if (error != 0) {
		view_notice(view, "The memo could not be kept.", now_us);
		return;
	}

	/* The log the tests read. */
	cal_log("MEMO date=%04d-%02d-%02d length=%zu", date->year, date->month, date->day, view->memo.length);

	/* The cell sinks (not with the motion reduced). */
	if (!view->reduce_motion) {
		view->sinking = 1;
		view->sink_us = now_us;
		view->sink_date = *date;
	}

	/* The day chosen, and a word of what was kept. */
	view_select(view, date, now_us);
	(void)snprintf(message, sizeof(message), "Kept the memo on %s %d.", cal_month_name(date->month), date->day);
	view_notice(view, message, now_us);
}

/*
 * Draws a note in a square box of a size at (x, y): an amber sheet with
 * its corner folded and two lines.
 */
static void
view_note_icon(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size)
{
	float fold[6];

	/* The sheet, its folded corner and its lines. */
	kl_canvas_round(canvas, x, y, size, size, size * 0.18f, CAL_COLOR_MEMO);
	fold[0] = x + size * 0.62f;
	fold[1] = y + size;
	fold[2] = x + size;
	fold[3] = y + size * 0.62f;
	fold[4] = x + size * 0.62f;
	fold[5] = y + size * 0.62f;
	kl_canvas_polygon(canvas, fold, 3, KL_RGB(0xfcd34d));
	kl_canvas_line(canvas, x + size * 0.22f, y + size * 0.32f, x + size * 0.78f, y + size * 0.32f, size * 0.09f, KL_RGBA(0xffffff, 230));
	kl_canvas_line(canvas, x + size * 0.22f, y + size * 0.52f, x + size * 0.52f, y + size * 0.52f, size * 0.09f, KL_RGBA(0xffffff, 230));
}

/*
 * Lays out (and draws, when asked) words in a width: each line of the
 * text broken into pieces that fit.  Returns the height they take, or
 * with draw -1 the width of their last piece (where a caret goes).
 */
static int
view_words(
	const struct kl_style *style,
	const char *text,
	int x,
	int y,
	int width,
	unsigned pixels,
	kl_color color,
	int draw)
{
	struct kl_text_line metrics;
	size_t length;
	size_t end;
	size_t shown;
	size_t at;
	int line_height;
	int height;
	int last;

	/* The line's height. */
	kl_text_metrics(style->text, pixels, &metrics);
	line_height = metrics.height + 4;
	height = 0;
	last = 0;
	at = 0;
	length = strlen(text);

	/* Each line of the text (a line break ends one; an empty one is a gap). */
	while (at <= length && height < 4000) {
		/* Where the text's line ends. */
		end = at;
		while (end < length && text[end] != '\n')
			end++;

		/* Its pieces that fit the width (an empty line is one empty piece). */
		do {
			/* One piece: what fits the width, of what is left of the line. */
			shown = end - at;
			if (shown > 0U)
				shown = kl_text_break(style->text, text + at, pixels, 0, width);

			/* No further than the line's end, and a byte at least while some is left. */
			if (shown > end - at)
				shown = end - at;
			else if (shown == 0U && end > at)
				shown = 1;

			/* Drawn on its line. */
			if (draw == 1)
				(void)kl_text_draw(style->text, style->canvas, x, y + height + metrics.ascent, text + at, shown, pixels, 0, color);
			last = kl_text_width(style->text, text + at, shown, pixels, 0);
			height += line_height;
			at += shown;
		} while (at < end);

		/* Past the line break. */
		at = end + 1U;
	}

	/* The width of the last piece, for a caret. */
	if (draw == -1)
		return last;

	/* The height. */
	return height;
}

/*
 * Draws the kind being dragged at the pointer: its icon and name on a
 * small card, a little see-through.
 */
static void
view_ghost(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style)
{
	double px;
	double py;
	float x;
	float y;

	/* Only a drag that has moved. */
	if (view->dragging < 0 || !view->drag_moved)
		return;

	/* The card under the pointer's corner. */
	kl_ui_pointer(ui, &px, &py);
	x = (float)px - 20.0f;
	y = (float)py - 20.0f;
	kl_canvas_shadow(style->canvas, x, y + 3.0f, 132.0f, 44.0f, 12.0f, 10.0f, style->theme->shadow);
	kl_canvas_round(style->canvas, x, y, 132.0f, 44.0f, 12.0f, kl_theme_choose(KL_RGBA(0xffffff, 230), KL_RGBA(0x2a2f38, 230)));

	/* The memo: its note and "Memo". */
	if (view->dragging == CAL_DRAG_MEMO) {
		view_note_icon(style->canvas, x + 10.0f, y + 10.0f, 24.0f);
		(void)kl_text_draw(style->text, style->canvas, (int)x + 46, (int)y + 27, "Memo", strlen("Memo"), 13U, 1, style->theme->text);
		return;
	}

	/* A kind: its icon and its name. */
	kl_canvas_image(style->canvas, &view->icons[view->dragging], x + 4.0f, y + 4.0f, 36.0f, 36.0f, 0.0f, 1.0f);
	(void)kl_text_draw(style->text, style->canvas, (int)x + 46, (int)y + 27, view_kinds[view->dragging], strlen(view_kinds[view->dragging]), 13U, 1, style->theme->text);
}

/*
 * Lists the items of a day, the events of hidden calendars left out: the
 * events all day first, then by their start, then the memos kept there.
 * Returns how many.
 */
static size_t
view_day(
	const struct cal_view *view,
	const struct cal_date *date,
	struct view_entry *entries,
	size_t size)
{
	const struct cal_item *items;
	struct view_entry moved;
	size_t count;
	size_t found;
	size_t at;
	size_t i;
	int same;
	int later;

	/* The events of the day, of calendars shown. */
	items = cal_items(&count);
	found = 0;
	for (i = 0; i < count && found < size; i++) {
		if (items[i].memo)
			continue;
		same = cal_same_day(&items[i].date, date);
		if (!same || (view->hidden & (1U << items[i].list)) != 0U)
			continue;

		/* The entry, its time as words. */
		memset(&entries[found], 0, sizeof(entries[found]));
		entries[found].title = items[i].title;
		entries[found].list = items[i].list;
		entries[found].index = (long)i;
		entries[found].all_day = items[i].all_day;
		entries[found].start = items[i].start;
		if (!items[i].all_day)
			(void)snprintf(entries[found].time, sizeof(entries[found].time), "%02d:%02d", items[i].start / 60, items[i].start % 60);

		/* Its place: after those all day and those that start before it. */
		moved = entries[found];
		at = found;
		while (at > 0U) {
			later = 0;
			if (entries[at - 1U].all_day == 0 && moved.all_day)
				later = 1;
			else if (entries[at - 1U].all_day == moved.all_day && entries[at - 1U].start > moved.start)
				later = 1;
			if (!later)
				break;
			entries[at] = entries[at - 1U];
			at--;
		}

		/* Its place. */
		entries[at] = moved;
		found++;
	}

	/* The memos kept on it. */
	for (i = 0; i < count && found < size; i++) {
		if (!items[i].memo)
			continue;
		same = cal_same_day(&items[i].date, date);
		if (!same)
			continue;
		memset(&entries[found], 0, sizeof(entries[found]));
		entries[found].title = items[i].title;
		entries[found].list = CAL_WORK;
		entries[found].memo = items[i].title;
		if (items[i].text != NULL)
			entries[found].memo = items[i].text;
		entries[found].index = (long)i;
		found++;
	}

	/* The number found. */
	return found;
}

/*
 * Reports whether an event's title matches the search (all do while it is
 * empty).
 */
static int
view_matches(
	const struct cal_view *view,
	const char *title)
{
	int held;

	/* An empty search matches everything. */
	if (view->search.length == 0U)
		return 1;

	/* The words in the title. */
	held = view_contains(title, view->search.text);
	return held;
}

/*
 * Reports whether a text holds a part, the case of ASCII letters ignored.
 */
static int
view_contains(
	const char *text,
	const char *part)
{
	size_t length;
	size_t i;
	size_t j;
	int a;
	int b;

	/* Each place the part could start. */
	length = strlen(part);
	for (i = 0; text[i] != '\0' || length == 0U; i++) {
		/* The bytes from there, matched one by one in lower case. */
		for (j = 0; j < length; j++) {
			a = (unsigned char)text[i + j];
			b = (unsigned char)part[j];
			if (a >= 'A' && a <= 'Z')
				a += 'a' - 'A';
			if (b >= 'A' && b <= 'Z')
				b += 'a' - 'A';
			if (a != b)
				break;
		}

		/* Every byte of the part matched. */
		if (j == length)
			return 1;
	}

	/* Not held. */
	return 0;
}

/*
 * Chooses a day: the desk calendar turns to it.
 */
static void
view_select(
	struct cal_view *view,
	const struct cal_date *date,
	uint64_t now_us)
{
	/* The day, and the page. */
	view->selected = *date;
	cal_log("SELECT date=%04d-%02d-%02d", date->year, date->month, date->day);
	view_show(view, date, now_us);
}

/*
 * Turns the desk calendar's page to a day: at once when the motion is
 * reduced, else over the top (a page still turning ends first).
 */
static void
view_show(
	struct cal_view *view,
	const struct cal_date *date,
	uint64_t now_us)
{
	int same;

	/* A page still turning ends where it was going. */
	if (view->flipping) {
		view->flipping = 0;
		view->shown = view->flip_to;
	}

	/* The day shown already. */
	same = cal_same_day(&view->shown, date);
	if (same)
		return;

	/* With the motion reduced, at once. */
	cal_log("FLIP from=%04d-%02d-%02d to=%04d-%02d-%02d reduced=%d",
		view->shown.year,
		view->shown.month,
		view->shown.day,
		date->year,
		date->month,
		date->day,
		view->reduce_motion);
	if (view->reduce_motion) {
		view->shown = *date;
		return;
	}

	/* The page turns from now. */
	view->flipping = 1;
	view->flip_us = now_us;
	view->flip_to = *date;
}

/*
 * Starts a new event of a calendar on a day: the cell sinks a moment, the
 * day is chosen, and the editor opens with a title and the hour from 9.
 */
static void
view_drop(
	struct cal_view *view,
	enum cal_list list,
	const struct cal_date *date,
	uint64_t now_us)
{
	/* The log the tests read. */
	cal_log("DROP list=%s date=%04d-%02d-%02d", cal_list_name(list), date->year, date->month, date->day);

	/* The cell sinks (not with the motion reduced). */
	if (!view->reduce_motion) {
		view->sinking = 1;
		view->sink_us = now_us;
		view->sink_date = *date;
	}

	/* The day chosen. */
	view_select(view, date, now_us);

	/* The new event in the editor. */
	memset(&view->edit, 0, sizeof(view->edit));
	view->edit.list = list;
	view->edit.date = *date;
	view->edit.start = 9 * 60;
	view->edit.end = 10 * 60;
	view->edit_index = -1;
	view->editing = 1;
	kl_field_set(&view->edit_title, view_new_titles[list]);
	kl_field_set(&view->edit_start, "09:00");
	kl_field_set(&view->edit_end, "10:00");
	cal_log("EDIT new list=%s", cal_list_name(list));
}

/*
 * Opens a kept item in the editor: an event's title, times and calendar;
 * a memo kept on a day can only be deleted.
 */
static void
view_edit(
	struct cal_view *view,
	long index,
	uint64_t now_us)
{
	const struct cal_item *items;
	char text[16];
	size_t count;

	UNUSED_PARAMETER(now_us);

	/* An item that is there. */
	items = cal_items(&count);
	if (index < 0 || (size_t)index >= count)
		return;

	/* Its fields. */
	view->edit = items[index];
	view->edit.text = NULL;
	view->edit_index = index;
	view->editing = 1;
	kl_field_set(&view->edit_title, items[index].title);
	(void)snprintf(text, sizeof(text), "%02d:%02d", items[index].start / 60, items[index].start % 60);
	kl_field_set(&view->edit_start, text);
	(void)snprintf(text, sizeof(text), "%02d:%02d", items[index].end / 60, items[index].end % 60);
	kl_field_set(&view->edit_end, text);
	cal_log("EDIT open index=%ld memo=%d", index, items[index].memo);
}

/*
 * Draws the editor: its title, the event's title, whether all day, its
 * start and end, its calendar, and Save, Delete and Cancel (a memo kept
 * on a day: its words, Delete and Cancel).
 */
static void
view_editor(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect field;
	struct kl_rect button;
	const char *heading;
	char line[96];
	unsigned hit;
	int clicked;
	int on;
	int x;
	int y;
	int i;

	/* The title of the editor and the day. */
	x = area->x + 4;
	y = area->y + 22;
	heading = "Edit Event";
	if (view->edit.memo)
		heading = "Memo";
	else if (view->edit_index < 0)
		heading = "New Event";
	(void)kl_text_draw(style->text, style->canvas, x, y, heading, strlen(heading), 18U, 1, style->theme->text);
	(void)snprintf(line, sizeof(line), "%s %d %s %d", cal_weekday_name(cal_weekday(&view->edit.date)), view->edit.date.day, cal_month_name(view->edit.date.month), view->edit.date.year);
	(void)kl_text_draw_fit(style->text, style->canvas, x, y + 20, line, 12U, 0, area->width - 8, style->theme->text_secondary);
	y += 36;

	/* A memo: its title only. */
	if (view->edit.memo) {
		(void)kl_text_draw_fit(style->text, style->canvas, x, y + 20, view->edit.title, 13U, 0, area->width - 8, style->theme->text);
		y += 40;
	} else {
		/* The title. */
		field.x = x;
		field.y = y;
		field.width = area->width - 8;
		field.height = 32;
		(void)kl_field(ui, style, CAL_ID_EDIT_TITLE, &field, &view->edit_title, "Title");
		y += 42;

		/* All day. */
		(void)kl_text_draw(style->text, style->canvas, x, y + 18, "All day", strlen("All day"), 13U, 0, style->theme->text);
		on = view->edit.all_day;
		clicked = kl_switch(ui, style, CAL_ID_EDIT_ALL_DAY, x + area->width - 60, y, &on, 0U);
		if (clicked)
			view->edit.all_day = on;
		y += 36;

		/* The start and the end, while not all day. */
		if (!view->edit.all_day) {
			(void)kl_text_draw(style->text, style->canvas, x, y + 21, "From", strlen("From"), 13U, 0, style->theme->text_secondary);
			field.x = x + 44;
			field.y = y;
			field.width = (area->width - 8 - 44 - 36) / 2;
			field.height = 32;
			(void)kl_field(ui, style, CAL_ID_EDIT_START, &field, &view->edit_start, "09:00");
			(void)kl_text_draw(style->text, style->canvas, field.x + field.width + 10, y + 21, "to", strlen("to"), 13U, 0, style->theme->text_secondary);
			field.x += field.width + 36;
			(void)kl_field(ui, style, CAL_ID_EDIT_END, &field, &view->edit_end, "10:00");
		}

		/* The calendars under the times. */
		y += 42;

		/* The calendar: a dot and name for each, the one chosen ringed. */
		for (i = 0; i < CAL_LISTS; i++) {
			button.x = x + (i % 2) * ((area->width - 8) / 2);
			button.y = y + (i / 2) * 30;
			button.width = (area->width - 8) / 2 - 4;
			button.height = 26;
			hit = kl_ui_hit(ui, CAL_ID_EDIT_LIST, (uint32_t)i, &button);
			if ((hit & KL_HIT_CLICKED) != 0U)
				view->edit.list = (enum cal_list)i;
			if ((enum cal_list)i == view->edit.list)
				kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->selection);
			else if ((hit & KL_HIT_HOT) != 0U)
				kl_canvas_round(style->canvas, (float)button.x, (float)button.y, (float)button.width, (float)button.height, 8.0f, style->theme->hover);
			kl_canvas_circle(style->canvas, (float)button.x + 14.0f, (float)button.y + 13.0f, 5.0f, cal_list_color((enum cal_list)i));
			(void)kl_text_draw(style->text, style->canvas, button.x + 26, button.y + 18, cal_list_name((enum cal_list)i), strlen(cal_list_name((enum cal_list)i)), 12U, 0, style->theme->text);
		}

		/* The buttons under them. */
		y += 66;
	}

	/* Save (an event), Delete (one kept), Cancel. */
	button.width = 72;
	button.height = 32;
	button.y = y;
	button.x = area->x + area->width - 4 - button.width;
	if (!view->edit.memo) {
		clicked = kl_button(ui, style, CAL_ID_EDIT_SAVE, &button, "Save", KL_BUTTON_PRIMARY);
		if (clicked)
			view_edit_save(view, now_us);
		button.x -= button.width + 6;
	}

	/* Cancel closes the editor. */
	clicked = kl_button(ui, style, CAL_ID_EDIT_CANCEL, &button, "Cancel", 0U);
	if (clicked) {
		view->editing = 0;
		cal_log("EDIT cancel");
	}

	/* Delete, for one kept. */
	if (view->edit_index >= 0) {
		button.x -= button.width + 6;
		clicked = kl_button(ui, style, CAL_ID_EDIT_DELETE, &button, "Delete", 0U);
		if (clicked)
			view_edit_delete(view, now_us);
	}
}

/* Keeps the event of the editor (its times read from their fields) and closes the editor. */
static void
view_edit_save(
	struct cal_view *view,
	uint64_t now_us)
{
	struct cal_item item;
	long kept;
	int start;
	int end;
	int error;

	/* Its title and times. */
	item = view->edit;
	view_copy_title(item.title, sizeof(item.title), view->edit_title.text);
	if (item.title[0] == '\0')
		(void)snprintf(item.title, sizeof(item.title), "%s", view_new_titles[item.list]);
	if (!item.all_day) {
		error = view_parse_minute(view->edit_start.text, &start);
		if (error == 0)
			error = view_parse_minute(view->edit_end.text, &end);
		if (error != 0) {
			view_notice(view, "Write the times as 09:30.", now_us);
			return;
		}

		/* An end before the start is the start. */
		if (end < start)
			end = start;
		item.start = start;
		item.end = end;
	}

	/* Kept. */
	error = cal_store_save_event(view->edit_index, &item, &kept);
	cal_log("EVENT saved index=%ld list=%s date=%04d-%02d-%02d all_day=%d start=%d end=%d error=%d", kept, cal_list_name(item.list), item.date.year, item.date.month, item.date.day, item.all_day, item.start, item.end, error);
	if (error != 0) {
		view_notice(view, "The event could not be kept.", now_us);
		return;
	}

	/* The editor closes. */
	view->editing = 0;
	view_notice(view, "Event saved.", now_us);
}

/* Deletes the item of the editor and closes the editor. */
static void
view_edit_delete(
	struct cal_view *view,
	uint64_t now_us)
{
	int error;

	/* Removed. */
	error = cal_store_delete(view->edit_index);
	cal_log("EVENT deleted index=%ld error=%d", view->edit_index, error);
	if (error != 0) {
		view_notice(view, "It could not be deleted.", now_us);
		return;
	}

	/* The editor closes. */
	view->editing = 0;
	view->edit_index = -1;
	view_notice(view, "Deleted.", now_us);
}

/* Copies a title into a room, cut before a whole UTF-8 character that does not fit. */
static void
view_copy_title(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* As much as fits with the NUL, not into the middle of a character. */
	length = strlen(from);
	if (length >= size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The bytes and the NUL. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Reads a time "H:MM" or "HH:MM" as minutes of the day; EINVAL for anything else. */
static int
view_parse_minute(
	const char *text,
	int *minute)
{
	int hour;
	int minutes;
	int read;

	/* The hour and the minutes. */
	read = sscanf(text, "%d:%d", &hour, &minutes);
	if (read != 2)
		return EINVAL;
	if (hour < 0 || hour > 23 || minutes < 0 || minutes > 59)
		return EINVAL;

	/* Succeeded: the minutes of the day. */
	*minute = hour * 60 + minutes;
	return 0;
}

/*
 * Draws the week of the day chosen (seven columns, the items of each day
 * under its name) or the day chosen (its items with their times), in the
 * months' place; an item opens in the editor.
 */
static void
view_list(
	struct cal_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct view_entry entries[CAL_DAY_EVENTS];
	struct cal_date day;
	struct kl_rect column;
	struct kl_rect row;
	char line[192];
	kl_color ink;
	unsigned hit;
	size_t count;
	size_t i;
	int today;
	int days;
	int weekday;
	int width;
	int d;
	int y;

	/* The days shown: the week of the day chosen from its Sunday, or the day. */
	day = view->selected;
	days = 1;
	if (view->mode == CAL_MODE_WEEK) {
		weekday = cal_weekday(&day);
		cal_add_days(&day, -weekday);
		days = 7;
	}

	/* A column for each. */
	width = area->width / days;

	/* Each day's column. */
	for (d = 0; d < days; d++) {
		column.x = area->x + d * width;
		column.y = area->y;
		column.width = width;
		column.height = area->height;

		/* Its name and date, today in the accent. */
		(void)snprintf(line, sizeof(line), "%s %d", cal_weekday_name(cal_weekday(&day)), day.day);
		if (days == 1)
			(void)snprintf(line, sizeof(line), "%s, %s %d", cal_weekday_name(cal_weekday(&day)), cal_month_name(day.month), day.day);
		ink = style->theme->text;
		today = cal_same_day(&day, &view->today);
		if (today)
			ink = style->theme->accent;
		(void)kl_text_draw_fit(style->text, style->canvas, column.x + 10, column.y + 24, line, 13U, 1, column.width - 16, ink);
		if (d > 0) {
			row.x = column.x;
			row.y = column.y + 8;
			row.width = 1;
			row.height = column.height - 16;
			kl_canvas_fill(style->canvas, &row, style->theme->row_separator);
		}

		/* Its items: the calendar's color, the time, the title (a memo's note). */
		count = view_day(view, &day, entries, CAL_DAY_EVENTS);
		y = column.y + 40;
		for (i = 0; i < count && y + 30 < column.y + column.height; i++) {
			row.x = column.x + 6;
			row.y = y;
			row.width = column.width - 12;
			row.height = 30;
			hit = kl_ui_hit(ui, CAL_ID_LIST_ITEM, (uint32_t)entries[i].index, &row);
			if ((hit & KL_HIT_CLICKED) != 0U)
				view_edit(view, entries[i].index, now_us);
			if (entries[i].memo != NULL) {
				kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 8.0f, CAL_COLOR_SURFACE);
				view_note_icon(style->canvas, (float)row.x + 4.0f, (float)row.y + 7.0f, 16.0f);
				(void)snprintf(line, sizeof(line), "%s", entries[i].title);
			} else {
				kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 8.0f, KL_RGBA(cal_list_color(entries[i].list), 48));
				kl_canvas_circle(style->canvas, (float)row.x + 12.0f, (float)row.y + 15.0f, 4.0f, cal_list_color(entries[i].list));
				if (entries[i].time[0] != '\0')
					(void)snprintf(line, sizeof(line), "%s %s", entries[i].time, entries[i].title);
				else
					(void)snprintf(line, sizeof(line), "%s", entries[i].title);
			}

			/* Its line, and the next row under it. */
			(void)kl_text_draw_fit(style->text, style->canvas, row.x + 24, row.y + 20, line, 12U, 0, row.width - 30, style->theme->text);
			y += 34;
		}

		/* A day without items. */
		if (count == 0U && days == 1)
			(void)kl_text_draw(style->text, style->canvas, column.x + 10, column.y + 60, "Nothing on this day.", strlen("Nothing on this day."), 12U, 0, style->theme->text_faint);
		cal_add_days(&day, 1);
	}
}

/*
 * Shows a notice for a while.
 */
static void
view_notice(
	struct cal_view *view,
	const char *message,
	uint64_t now_us)
{
	/* The words and until when. */
	(void)snprintf(view->notice, sizeof(view->notice), "%s", message);
	view->notice_until = now_us + CAL_NOTICE_US;
}

/*
 * Reports where a month's block starts in the months' content (the end of
 * the last one for CAL_MONTHS).
 */
static int
view_month_top(
	const struct cal_view *view,
	int index,
	int cell_height)
{
	struct cal_date first;
	int top;
	int i;

	/* The blocks before it. */
	top = 0;
	for (i = 0; i < index && i < CAL_MONTHS; i++)
		top += CAL_HEADING + view_month_rows(view, i, &first) * cell_height;

	/* Its top. */
	return top;
}

/*
 * Reports how many weeks a month of the scroll spans, and its first day.
 */
static int
view_month_rows(
	const struct cal_view *view,
	int index,
	struct cal_date *first)
{
	int lead;
	int length;

	/* The month's 1st, the days before it in its first week, and its length. */
	*first = view->first_month;
	cal_add_months(first, index);
	lead = cal_weekday(first);
	length = cal_days_in_month(first->year, first->month);

	/* The weeks. */
	return (lead + length + 6) / 7;
}

/*
 * Reports a cell's height: six weeks and a heading fill the months' band,
 * not under CAL_CELL_MIN.
 */
static int
view_cell_height(
	const struct kl_rect *grid)
{
	int height;

	/* A sixth of the band below a heading. */
	height = (grid->height - CAL_HEADING) / 6;
	if (height < CAL_CELL_MIN)
		height = CAL_CELL_MIN;

	/* The height. */
	return height;
}

/*
 * Draws a line of text centred on a point across.
 */
static void
view_centred(
	const struct kl_style *style,
	int cx,
	int baseline,
	const char *text,
	unsigned pixels,
	int bold,
	kl_color color)
{
	int width;

	/* Half its width to the left. */
	width = kl_text_width(style->text, text, strlen(text), pixels, bold);
	(void)kl_text_draw(style->text, style->canvas, cx - width / 2, baseline, text, strlen(text), pixels, bold, color);
}

/*
 * Reports an ease in and out (a smooth step) of a time from 0 to 1.
 */
static float
view_ease(
	float t)
{
	/* Within 0 and 1. */
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	/* The cubic's value. */
	return t * t * (3.0f - 2.0f * t);
}
