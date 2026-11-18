/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar (WS155): Keiland's calendar.  The months one under another, a
 * week or a day, with the events of four calendars, a sidebar, a panel to
 * add an event by dragging its kind (an icon drawn in 3D, render3d.h and
 * scene.h) onto a date and to edit it, the application's one memo
 * (dragged onto a date, it is kept there as a memo), and the day chosen
 * with a small desk calendar in 3D that turns its page when the day
 * changes.  The events and memos are kept under ~/Documents/Calendar as
 * iCalendar files (store.c, plan/ws155/phase001/phase.md).
 *
 * The view (view.c) draws a frame with libkeiland's canvas and widgets and
 * knows nothing of the window, so that the host tests draw it into
 * pictures; the window (main.c) feeds it the input and the time.
 */

#ifndef CALENDAR_CALENDAR_H
#define CALENDAR_CALENDAR_H

#include "scene.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* Marks a parameter a function does not use. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/* The calendars, in the sidebar's order. */
enum cal_list {
	CAL_WORK,
	CAL_PERSONAL,
	CAL_FAMILY,
	CAL_STUDY,
	CAL_LISTS
};

/* A day: the year, the month (1 to 12) and the day of the month. */
struct cal_date {
	int year;
	int month;
	int day;
};

/*
 * One event or memo kept (store.c): whether it is a memo kept on a day,
 * its UID, its calendar (an event's), its day, whether it lasts the whole
 * day, its start and end (minutes of the day, for one with times), its
 * title, and a memo's words (allocated by the store, NULL for an event).
 */
struct cal_item {
	int memo;
	char uid[96];
	enum cal_list list;
	struct cal_date date;
	int all_day;
	int start;
	int end;
	char title[128];
	char *text;
};

/* The longest memo, with its NUL. */
#define CAL_MEMO_MAX		1024U

/* The actions of the menu, the keys and the buttons. */
#define CAL_ACTION_TODAY	1U
#define CAL_ACTION_PREVIOUS	2U
#define CAL_ACTION_NEXT		3U
#define CAL_ACTION_MOTION	4U
#define CAL_ACTION_QUIT		5U

/* What a drag of the memo is (a kind of event's is its index). */
#define CAL_DRAG_MEMO		4

/* The views of the middle: the months, a week, a day. */
#define CAL_MODE_MONTH		0
#define CAL_MODE_WEEK		1
#define CAL_MODE_DAY		2

/* The most cells remembered from a frame, and months shown. */
#define CAL_CELLS_MAX		512U
#define CAL_MONTHS		13

/* The most parts a click that chose another day draws again in its frame: the old day's cell and the card of the day chosen. */
#define CAL_REDRAW_MAX		2U

/* The colours the picture of the ground and the cards is made of (the window's ground, the cards' veils, shadow, ground and edge). */
#define CAL_GROUND_COLORS	7U

/* A cell of the frame drawn: where it is and its day (the drop of a drag finds it). */
struct cal_cell {
	struct kl_rect rect;
	struct cal_date date;
};

/*
 * The view's state.
 *
 * The days: today, the one chosen, the one the desk calendar shows, and
 * the first month of the scroll (six before today's).  The scroll of the
 * months, the month it is to go to at the next frame (0 for none), the
 * search field, and the calendars hidden (a bit each).
 *
 * The motion: whether it is reduced (pages and cells change at once),
 * when the view began, a page turning
 * (when it began, from which day to which), a cell sinking after a drop
 * (when and which), and a drag of a kind of event or of the memo (its
 * kind, CAL_DRAG_MEMO for the memo, -1 for none, and whether it has moved
 * off its card).
 *
 * The memo of the application (libkeiland's text area, ws090-p022; the
 * store keeps it).
 *
 * The view of the middle (CAL_MODE_*), and an event being edited: whether
 * the editor shows (in the panel), the store's index of the item (-1 for a
 * new event), the item as it is being edited (its day, calendar, whether
 * all day; a memo is only deleted), and the fields of its title and times.
 *
 * The cells of the last frame, where today's cell was last logged, the notice shown at the
 * bottom until a time (empty for none), whether the window stands on glass, whether
 * the program is to end, and whether a click in the last frame changed what was drawn
 * before it, so that the whole view is drawn again (the window clears it).
 *
 * A click that chose another day in the months: the parts to draw again in the
 * same frame (the window draws them and empties the list, ws210-p014), and the
 * card of the day chosen in the last frame (none: no width).
 *
 * The picture of the ground and the cards (made once for a size, the glass,
 * the cards and the colours, and copied into each frame), and whether it is
 * made for these.
 *
 * The 3D: the desk calendar's target and picture, what the last full frame
 * had under it (its area, whether it is kept, and whether the frame being
 * drawn met the desk calendar), whether the picture shows the desk calendar
 * still with the pages it has now (drawn again as it is), its mesh, the icons of
 * the kinds of event, and the pictures of the two pages with their days
 * and the accent they were drawn in (ws179-p002).
 */
struct cal_view {
	struct cal_date today;
	struct cal_date selected;
	struct cal_date shown;
	struct cal_date first_month;
	struct kl_scroll scroll;
	int scroll_to;
	int scroll_glide;
	struct kl_field search;
	unsigned hidden;

	int reduce_motion;
	uint64_t started_us;
	int flipping;
	uint64_t flip_us;
	struct cal_date flip_to;
	int sinking;
	uint64_t sink_us;
	struct cal_date sink_date;
	int dragging;
	int drag_moved;
	double drag_from_x;
	double drag_from_y;

	struct kl_text_area memo;

	int mode;
	int editing;
	long edit_index;
	struct cal_item edit;
	struct kl_field edit_title;
	struct kl_field edit_start;
	struct kl_field edit_end;

	struct cal_cell cells[CAL_CELLS_MAX];
	size_t cell_count;
	int today_x;
	int today_y;
	char notice[128];
	uint64_t notice_until;
	int glass;
	int quit;
	int again;

	struct kl_rect redraw[CAL_REDRAW_MAX];
	size_t redraw_count;
	struct kl_rect day_card;

	struct kl_image ground;
	int ground_valid;
	int ground_glass;
	struct kl_rect ground_cards[3];
	kl_color ground_colors[CAL_GROUND_COLORS];

	struct r3_target target;
	struct kl_image picture;
	struct kl_image under;
	struct kl_rect desk_area;
	int desk_valid;
	int desk_seen;
	int picture_still;
	struct sc_mesh *mesh;
	struct kl_image icons[SC_ICONS];
	struct kl_image pages[SC_TEXTURES];
	struct cal_date page_dates[SC_TEXTURES];
	kl_color page_accents[SC_TEXTURES];
};

/* The days (date.c). */
int cal_days_in_month(int year, int month);
int cal_weekday(const struct cal_date *date);
void cal_add_months(struct cal_date *date, int months);
void cal_add_days(struct cal_date *date, int days);
int cal_same_day(const struct cal_date *a, const struct cal_date *b);
int cal_month_index(const struct cal_date *first, const struct cal_date *date);
const char *cal_month_name(int month);
const char *cal_weekday_name(int weekday);

/* The events and memos kept (store.c). */
int cal_store_open(const char *root);
void cal_store_close(void);
const struct cal_item *cal_items(size_t *count);
int cal_store_save_event(long index, const struct cal_item *item, long *kept);
int cal_store_delete(long index);
int cal_store_add_memo(const struct cal_date *date, const char *text);
const char *cal_store_memo(void);
int cal_store_set_memo(const char *text);
const char *cal_list_name(enum cal_list list);
kl_color cal_list_color(enum cal_list list);

/* The view (view.c). */
int cal_view_init(struct cal_view *view, const struct cal_date *today, uint64_t now_us);
void cal_view_release(struct cal_view *view);
void cal_view_action(struct cal_view *view, unsigned action, uint64_t now_us);
void cal_view_key(struct cal_view *view, uint32_t key, unsigned modifiers, uint64_t now_us);
void cal_view_draw(struct cal_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t cal_view_panels(const struct cal_view *view, int width, int height, struct kl_glass_panel *panels, size_t capacity);
int cal_view_wait(const struct cal_view *view, uint64_t now_us);
int cal_view_animating(const struct cal_view *view);
int cal_view_desk_only(const struct cal_view *view);
int cal_view_cell_at(const struct cal_view *view, double x, double y, struct kl_rect *rect);
void cal_view_draw_desk(struct cal_view *view, const struct kl_style *style, uint64_t now_us);

/* The log for the tests (main.c, and the host tests' own). */
void cal_log(const char *format, ...);

#endif
