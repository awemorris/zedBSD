/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The view of PDF Viewer: how the pages are laid out, where the view is,
 * and what the keys, the pointer and the actions do to them.
 *
 * In the scroll mode the pages stand in one column, a gap apart, at one
 * scale (the widest page fits the width, or the tallest page fits the
 * window, or the user's zoom); the wheel, a drag and the keys move the
 * view along it.  In the page mode one page is shown at its own fitting
 * scale; a sideways drag moves it with the pointer and, let go far enough
 * or fast enough, turns to the next or the previous page, which slides in
 * (a swipe); the keys and the wheel turn pages too.
 *
 * ws079-p015: the sidebar of page thumbnails takes the left of the window
 * while it is shown; a click on a thumbnail shows its page, the wheel and a
 * drag scroll the column, and the column follows the page in view.  An
 * encrypted document the empty password does not open shows the password
 * card, which takes every key and click until the document opens or the
 * card is cancelled.
 *
 * ws128-p004: Find and the selection (find.c): Ctrl+F gives the titlebar's
 * find field the keyboard, F3 and Shift+F3 show the next and the previous
 * place found, a press on a page's words selects instead of dragging the
 * view, Ctrl+C copies the selection, Esc lets both go.
 *
 * ws081-p012: the touch screen's gestures (touch.c) use the places below
 * (pv_app_place_at, pv_app_show_place) to keep the point under two fingers
 * while they zoom, and end a swipe as the pointer's release does.
 *
 * Nothing here draws or speaks Wayland; draw.c draws what this lays out,
 * and main.c feeds it the window's input.
 */

#include "viewer.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The zoom steps and bounds, in pixels per point. */
#define VIEW_ZOOM_STEP		1.25
#define VIEW_ZOOM_MIN		0.1
#define VIEW_ZOOM_MAX		6.0

/* How far a key or a wheel notch scrolls, in pixels. */
#define VIEW_KEY_STEP		48.0

/* How far the pointer moves before a press is a drag, in pixels. */
#define VIEW_DRAG_START		6

/* The share of the width, and the speed (pixels a millisecond), that let go of a swipe turn the page. */
#define VIEW_SWIPE_SHARE	0.18
#define VIEW_SWIPE_SPEED	0.6

/* How long a page turn slides, and how long the page indicator stays, in milliseconds. */
#define VIEW_TURN_MS		220U
#define VIEW_INDICATOR_MS	1400U

/*
 * How long the window's size must stay still before the pages are drawn
 * again at their new scale, in milliseconds (BUG-259, the 2026-10-08 UAT:
 * while the window is resized the pages' rasters are stretched).
 */
#define VIEW_RESIZE_SETTLE_MS	200U

/* The wheel's travel that turns a page in the page mode, in pixels. */
#define VIEW_WHEEL_TURN		90.0

/* The longest a message stays by default, in milliseconds. */
#define VIEW_MESSAGE_MS		6000U

/* How many codes the key tables cover (up to the space bar). */
#define VIEW_KEY_TABLE_SIZE	58U

/* How many thumbnails past the sidebar's view are drawn ahead. */
#define VIEW_THUMBNAIL_AHEAD	2U

/*
 * The character each key types without shift, by evdev code; 0 for a key
 * that types none.  The US layout, as the compositor sends no keymap.
 */
static const char view_plain_keys[VIEW_KEY_TABLE_SIZE] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* The character each key types with shift, by evdev code. */
static const char view_shifted_keys[VIEW_KEY_TABLE_SIZE] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static size_t current_page(const struct pv_app *app);
static int toggle_key(const struct pv_event *event);
static void clamp_view(struct pv_app *app);
static void show_page(struct pv_app *app, size_t index);
static void start_turn(struct pv_app *app, int direction);
static void zoom_by(struct pv_app *app, double factor);
static void keep_anchor(struct pv_app *app, size_t page, double fraction);
static void handle_key(struct pv_app *app, const struct pv_event *event);
static void handle_button(struct pv_app *app, const struct pv_event *event);
static void handle_motion(struct pv_app *app, const struct pv_event *event);
static int image_at(struct pv_app *app, int x, int y);
static void handle_axis(struct pv_app *app, const struct pv_event *event);
static void open_chooser(struct pv_app *app);
static const char *reason_of(int error);
static double page_mode_top(const struct pv_app *app);
static int open_document(struct pv_app *app, const char *path, const char *password);
static void relayout(struct pv_app *app);
static int sidebar_takes(const struct pv_app *app, const struct pv_event *event, int sidebar);
static void sidebar_event(struct pv_app *app, const struct pv_event *event);
static int thumbnail_at(const struct pv_app *app, int y, size_t *index);
static void clamp_sidebar(struct pv_app *app);
static void reveal_thumbnail(struct pv_app *app, size_t index);
static void ask_password(struct pv_app *app, const char *path, int wrong);
static void password_event(struct pv_app *app, const struct pv_event *event);
static void password_key(struct pv_app *app, const struct pv_event *event);
static void submit_password(struct pv_app *app);
static void cancel_password(struct pv_app *app);
static void forget_password(struct pv_app *app);
static char key_character(uint32_t key, uint32_t modifiers);

/*
 * Starts the viewer with no document, at a window size.
 */
void
pv_app_init(
	struct pv_app *app,
	struct pv_text *text,
	int width,
	int height)
{
	/* Nothing open, the scroll mode fitting the width. */
	memset(app, 0, sizeof(*app));
	app->text = text;
	app->mode = PV_MODE_SCROLL;
	app->fit = PV_FIT_WIDTH;
	app->zoom = 1.0;
	app->window_width = width;
	app->width = width;
	app->height = height;
	app->now = pv_clock();
	app->find_page = (size_t)-1;
	app->dirty = 1;
}

/*
 * Frees the document.
 */
void
pv_app_release(
	struct pv_app *app)
{
	/* Closes what is open, forgets a password being typed, and words copied that were not taken. */
	pv_app_close_document(app);
	forget_password(app);
	free(app->copy_text);
	app->copy_text = NULL;
}

/*
 * Opens a PDF file in place of the one shown; a file that cannot be opened
 * leaves a message and no document, and an encrypted one the empty
 * password does not open shows the password card.
 *
 * Returns 0, or an errno value (PDF_EPASSWORD while the card asks).
 */
int
pv_app_open(
	struct pv_app *app,
	const char *path)
{
	int error;

	/* Opens it without a password. */
	error = open_document(app, path, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the document is shown. */
	return 0;
}

/*
 * Closes the document shown, if any.
 */
void
pv_app_close_document(
	struct pv_app *app)
{
	/* Nothing to close without a document. */
	if (!app->has_document)
		return;

	/* Closes it and forgets the view. */
	pv_document_close(&app->document);
	app->has_document = 0;
	app->page = 0;
	app->scroll_x = 0.0;
	app->scroll_y = 0.0;
	app->swipe = 0.0;
	app->turning = 0;
	app->pressed = 0;
	app->dragging = 0;
	app->thumbnail_pressed = 0;
	app->thumbnail_dragging = 0;
	app->thumbnail_scroll = 0.0;
	app->dirty = 1;

	/* Nothing selected or found (the pages' words went with them). */
	pv_find_clear(app);

	/* The pages take the whole window again: the sidebar needs a document. */
	app->width = app->window_width;
}

/*
 * Takes a new window size, keeping the page in view.  The pages are not
 * drawn again for it at once: until the size has stayed still for
 * VIEW_RESIZE_SETTLE_MS (pv_app_tick), the frame shows their rasters
 * stretched to the new scale (BUG-259).
 */
void
pv_app_resize(
	struct pv_app *app,
	int width,
	int height)
{
	size_t page;

	/* Remembers the page before the layout changes. */
	page = current_page(app);

	/* The new size, the pages beside the sidebar when it is shown, and the same page at its top. */
	app->window_width = width;
	app->height = height;
	app->width = width - pv_app_sidebar_width(app);
	if (app->mode == PV_MODE_SCROLL && app->has_document)
		app->scroll_y = pv_app_page_top(app, page) - PV_MARGIN;
	clamp_view(app);
	clamp_sidebar(app);
	app->dirty = 1;

	/* The resize goes on from now: the rasters wait until it settles. */
	app->resizing = 1;
	app->resized_at = app->now;
}

/*
 * Handles one input from the window: the password card takes every one
 * while it is shown, the sidebar the pointer's in it, and the pages the
 * rest in their own coordinates (right of the sidebar).
 */
void
pv_app_event(
	struct pv_app *app,
	const struct pv_event *event)
{
	struct pv_event local;
	int sidebar;
	int taken;

	/* Each key pressed or repeated is logged for the tests (BUG-111). */
	if (event->type == PV_EVENT_KEY && event->pressed)
		pv_log("KEY key=%u modifiers=%u repeat=%d", event->key, event->modifiers, event->repeat);

	/*
	 * A key held repeats only where more of the same makes sense (moving,
	 * zooming); a key that opens, closes, toggles or confirms does not, since
	 * a late release would do it again (BUG-111).
	 */
	if (event->type == PV_EVENT_KEY && event->repeat) {
		taken = toggle_key(event);
		if (taken)
			return;
	}

	/* The password card is in front of everything. */
	if (app->asking_password) {
		password_event(app, event);
		return;
	}

	/* The sidebar's pointer inputs. */
	sidebar = pv_app_sidebar_width(app);
	taken = sidebar_takes(app, event, sidebar);
	if (taken) {
		sidebar_event(app, event);
		return;
	}

	/* The pages' inputs, with the pointer where it is over them. */
	local = *event;
	local.x -= sidebar;

	/* Handles it by its kind (the selection takes the button and the motion first, ws128-p004). */
	switch (local.type) {
	case PV_EVENT_KEY:
		if (local.pressed)
			handle_key(app, &local);
		break;
	case PV_EVENT_BUTTON:
		taken = pv_select_button(app, &local);
		if (!taken)
			handle_button(app, &local);
		break;
	case PV_EVENT_MOTION:
		taken = pv_select_motion(app, &local);
		if (!taken)
			handle_motion(app, &local);
		break;
	case PV_EVENT_AXIS:
		handle_axis(app, &local);
		break;
	case PV_EVENT_LEAVE:
		break;
	case PV_EVENT_ACTION:
		pv_app_action(app, (enum pv_action)local.action);
		break;
	}
}

/*
 * Carries out an action of the menus, the titlebar or the keys.
 */
void
pv_app_action(
	struct pv_app *app,
	enum pv_action action)
{
	size_t page;

	/* The page the view is on, which most actions keep. */
	page = current_page(app);
	pv_log("ACTION %d page=%lu", (int)action, (unsigned long)page);

	/* Carries out the action. */
	switch (action) {
	case PV_ACTION_OPEN:
		open_chooser(app);
		break;
	case PV_ACTION_CLOSE:
		/* Closes the document, or the window when none is open. */
		if (app->has_document)
			pv_app_close_document(app);
		else
			app->want_close = 1;
		break;
	case PV_ACTION_QUIT:
		app->want_close = 1;
		break;
	case PV_ACTION_ANNOTATE:
		if (app->has_document)
			app->want_annotate = 1;
		break;
	case PV_ACTION_PRINT:
		if (app->has_document)
			app->want_print = 1;
		break;
	case PV_ACTION_MODE_SCROLL:
		app->mode = PV_MODE_SCROLL;
		if (app->fit == PV_FIT_PAGE)
			app->fit = PV_FIT_WIDTH;
		app->swipe = 0.0;
		app->turning = 0;
		show_page(app, page);
		break;
	case PV_ACTION_MODE_PAGE:
		app->mode = PV_MODE_PAGE;
		if (app->fit == PV_FIT_WIDTH)
			app->fit = PV_FIT_PAGE;
		show_page(app, page);
		break;
	case PV_ACTION_FIT_WIDTH:
		app->fit = PV_FIT_WIDTH;
		show_page(app, page);
		break;
	case PV_ACTION_FIT_PAGE:
		app->fit = PV_FIT_PAGE;
		show_page(app, page);
		break;
	case PV_ACTION_ZOOM_IN:
		zoom_by(app, VIEW_ZOOM_STEP);
		break;
	case PV_ACTION_ZOOM_OUT:
		zoom_by(app, 1.0 / VIEW_ZOOM_STEP);
		break;
	case PV_ACTION_ZOOM_RESET:
		/* The mode's own fit. */
		app->fit = PV_FIT_WIDTH;
		if (app->mode == PV_MODE_PAGE)
			app->fit = PV_FIT_PAGE;
		show_page(app, page);
		break;
	case PV_ACTION_PREVIOUS:
		/* The page mode turns; the scroll mode scrolls to the page before. */
		if (app->mode == PV_MODE_PAGE)
			start_turn(app, -1);
		else if (page > 0)
			show_page(app, page - 1);
		break;
	case PV_ACTION_NEXT:
		/* The page mode turns; the scroll mode scrolls to the page after. */
		if (app->mode == PV_MODE_PAGE)
			start_turn(app, 1);
		else
			show_page(app, page + 1);
		break;
	case PV_ACTION_FIRST:
		show_page(app, 0);
		break;
	case PV_ACTION_LAST:
		if (app->has_document)
			show_page(app, app->document.count - 1);
		break;
	case PV_ACTION_THUMBNAILS:
		/* The sidebar comes or goes; the pages are laid out again around the same page, which it then shows. */
		app->thumbnails = !app->thumbnails;
		app->thumbnail_followed = (size_t)-1;
		relayout(app);
		pv_log("THUMBNAILS shown=%d sidebar=%d", app->thumbnails, pv_app_sidebar_width(app));
		break;
	case PV_ACTION_FIND:
		/* The titlebar's find field takes the keyboard (main.c, ws128-p004). */
		app->want_find_focus = 1;
		break;
	case PV_ACTION_FIND_NEXT:
		pv_find_next(app, 1);
		break;
	case PV_ACTION_FIND_PREVIOUS:
		pv_find_next(app, -1);
		break;
	case PV_ACTION_COPY:
		pv_select_copy(app);
		break;
	case PV_ACTION_SELECT_ALL:
		pv_select_all(app);
		break;
	case PV_ACTION_NONE:
		break;
	}

	/* After any action the page indicator shows, and the frame is drawn again. */
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
}

/*
 * Moves time on: the page turn slides, and the indicator and the message
 * go when their time is up.
 *
 * Returns how many milliseconds until something is due (-1 for nothing).
 */
int
pv_app_tick(
	struct pv_app *app,
	uint64_t now)
{
	double progress;
	double eased;
	size_t page;
	int sidebar;
	int counting;
	int due;

	/* The time of the frame. */
	app->now = now;
	due = -1;

	/* Slides the page turn, easing out, and ends it on the new page. */
	if (app->turning) {
		progress = (double)(now - app->turn_start) / (double)VIEW_TURN_MS;
		if (progress >= 1.0) {
			app->turning = 0;
			app->swipe = 0.0;
			if (app->turn_direction > 0)
				app->page++;
			if (app->turn_direction < 0)
				app->page--;
			app->scroll_y = 0.0;
			app->scroll_x = 0.0;
			clamp_view(app);
			pv_log("PAGE shown=%lu", (unsigned long)app->page);
		} else {
			eased = 1.0 - (1.0 - progress) * (1.0 - progress) * (1.0 - progress);
			app->swipe = app->turn_from + (app->turn_to - app->turn_from) * eased;
			due = 16;
		}

		/* The frame moves with the turn. */
		app->dirty = 1;
	}

	/* A resize settled (BUG-259): the pages are drawn again at their new scale. */
	if (app->resizing && now >= app->resized_at + VIEW_RESIZE_SETTLE_MS) {
		app->resizing = 0;
		app->dirty = 1;
		pv_log("RESIZE settled width=%d height=%d", app->window_width, app->height);
	}

	/* Until then, its settling is due. */
	if (app->resizing) {
		if (due < 0 || (int)(app->resized_at + VIEW_RESIZE_SETTLE_MS - now) < due)
			due = (int)(app->resized_at + VIEW_RESIZE_SETTLE_MS - now);
	}

	/* The indicator goes when its time is up. */
	if (app->indicator_until != 0 && now >= app->indicator_until) {
		app->indicator_until = 0;
		app->dirty = 1;
	}

	/* Until then, the indicator's end is due. */
	if (app->indicator_until != 0) {
		if (due < 0 || (int)(app->indicator_until - now) < due)
			due = (int)(app->indicator_until - now);
	}

	/* The sidebar follows the page in view, once each time the page changes. */
	sidebar = pv_app_sidebar_width(app);
	if (app->has_document && sidebar > 0) {
		page = current_page(app);
		if (page != app->thumbnail_followed) {
			app->thumbnail_followed = page;
			reveal_thumbnail(app, page);
		}
	}

	/* So does the message. */
	if (app->message[0] != '\0' &&
	    app->message_until != 0 &&
	    now >= app->message_until) {
		app->message[0] = '\0';
		app->dirty = 1;
	}

	/* Until then, the message's end is due. */
	if (app->message[0] != '\0' && app->message_until != 0) {
		if (due < 0 || (int)(app->message_until - now) < due)
			due = (int)(app->message_until - now);
	}

	/* The places of Find are counted a few pages a tick (ws177-p041). */
	counting = pv_find_tick(app);
	if (counting >= 0 && (due < 0 || counting < due))
		due = counting;

	/* Reports the wait. */
	return due;
}

/*
 * Rasterizes one page the view will likely show next, while nothing else
 * is to be done: first a thumbnail of the sidebar's view (or just past it)
 * that is not drawn yet, then the pages after and before the page in view
 * (and the second after in the scroll mode).
 *
 * Returns 1 when a page was drawn (more may follow), 0 when all are ready.
 */
int
pv_app_prefetch(
	struct pv_app *app)
{
	const struct pv_page *shown;
	const struct pv_page *candidate;
	size_t candidates[3];
	size_t count;
	size_t index;
	size_t page;
	size_t first;
	size_t last;
	double scale;
	double difference;
	int sidebar;
	int error;

	/* Nothing while there is no document, or while the view moves (by the pointer or by touch) or the window is resized. */
	if (!app->has_document ||
	    app->turning ||
	    app->pressed ||
	    app->touching ||
	    app->zooming ||
	    app->resizing)
		return 0;

	/* The sidebar's thumbnails in view and just past it, one at a time. */
	sidebar = pv_app_sidebar_width(app);
	if (sidebar > 0 && !app->thumbnail_pressed) {
		pv_thumbnail_range(app, &first, &last);
		last += VIEW_THUMBNAIL_AHEAD;
		if (last >= app->document.count)
			last = app->document.count - 1;
		for (index = first; index <= last; index++) {
			if (app->document.pages[index].thumbnail != NULL)
				continue;
			error = pv_document_thumbnail(&app->document, index, &shown);
			if (error != 0)
				return 0;
			app->dirty = 1;
			return 1;
		}
	}

	/* The pages to have ready: after, then before, the page in view. */
	page = current_page(app);
	count = 0;
	if (page + 1 < app->document.count) {
		candidates[count] = page + 1;
		count++;
	}

	/* The page before. */
	if (page > 0) {
		candidates[count] = page - 1;
		count++;
	}

	/* In the scroll mode, the second after. */
	if (app->mode == PV_MODE_SCROLL && page + 2 < app->document.count) {
		candidates[count] = page + 2;
		count++;
	}

	/* Draws the first one without a raster at its scale. */
	for (index = 0; index < count; index++) {
		scale = pv_app_scale(app, candidates[index]);
		candidate = &app->document.pages[candidates[index]];
		if (candidate->raster != NULL) {
			difference = candidate->raster_scale - scale;
			if (difference < 1e-6 && difference > -1e-6)
				continue;
		}

		/* Draws it; one that cannot be drawn ends the prefetch. */
		error = pv_document_raster(&app->document, candidates[index], scale, &shown);
		if (error != 0)
			return 0;
		return 1;
	}

	/* Every page near the view is ready. */
	return 0;
}

/*
 * Reports the page the view is on (0 without a document).
 */
size_t
pv_app_current_page(
	const struct pv_app *app)
{
	size_t page;

	/* The page mode's page, or the scroll mode's across the middle. */
	page = current_page(app);

	/* Reports the page. */
	return page;
}

/*
 * Reports the scale a page is shown at, in pixels per point.
 */
double
pv_app_scale(
	const struct pv_app *app,
	size_t index)
{
	const struct pv_page *page;
	double width;
	double height;
	double room_width;
	double room_height;
	double scale;

	/* The user's zoom. */
	if (app->fit == PV_FIT_CUSTOM || !app->has_document)
		return app->zoom;

	/* The page fitted: the scroll mode fits the widest and tallest page, the page mode each page. */
	width = app->document.widest;
	height = app->document.tallest;
	if (app->mode == PV_MODE_PAGE && index < app->document.count) {
		page = &app->document.pages[index];
		width = page->width;
		height = page->height;
	}

	/* The room inside the margins, at least 16 pixels each way. */
	room_width = (double)app->width - 2.0 * PV_MARGIN;
	room_height = (double)app->height - 2.0 * PV_MARGIN;
	if (room_width < 16.0)
		room_width = 16.0;
	if (room_height < 16.0)
		room_height = 16.0;

	/* To the width, or to the whole page. */
	scale = room_width / width;
	if (app->fit == PV_FIT_PAGE && room_height / height < scale)
		scale = room_height / height;

	/* Keeps the scale within the zoom's bounds. */
	if (scale < VIEW_ZOOM_MIN)
		scale = VIEW_ZOOM_MIN;
	if (scale > VIEW_ZOOM_MAX)
		scale = VIEW_ZOOM_MAX;

	/* Reports the scale. */
	return scale;
}

/*
 * Reports how far the page mode's neighbour stands from the page shown,
 * centre to centre, in pixels: half of each page's width and two gaps.
 */
double
pv_app_neighbour_distance(
	const struct pv_app *app,
	size_t neighbour)
{
	double shown;
	double other;

	/* The two pages' widths at their scales. */
	shown = app->document.pages[app->page].width * pv_app_scale(app, app->page);
	other = app->document.pages[neighbour].width * pv_app_scale(app, neighbour);

	/* Reports the distance between their centres. */
	return (shown + other) / 2.0 + 2.0 * PV_GAP;
}

/*
 * Reports where a page's top is in the laid-out document, in pixels (the
 * scroll mode's column; the page mode's page is its only page).
 */
double
pv_app_page_top(
	const struct pv_app *app,
	size_t index)
{
	double top;
	double scale;
	size_t page;

	/* The page mode's one page. */
	if (app->mode == PV_MODE_PAGE) {
		top = page_mode_top(app);
		return top;
	}

	/* The pages above it, a gap apart, under the margin. */
	scale = pv_app_scale(app, 0);
	top = PV_MARGIN;
	for (page = 0; page < index && page < app->document.count; page++)
		top += app->document.pages[page].height * scale + PV_GAP;

	/* Reports the top. */
	return top;
}

/*
 * Reports the height of the laid-out document, in pixels.
 */
double
pv_app_content_height(
	const struct pv_app *app)
{
	double height;

	/* Nothing without a document. */
	if (!app->has_document)
		return 0.0;

	/* The page mode's page, with its margins. */
	if (app->mode == PV_MODE_PAGE) {
		height = app->document.pages[app->page].height * pv_app_scale(app, app->page) + 2.0 * PV_MARGIN;
		return height;
	}

	/* The column: the last page's bottom and the margin. */
	height = pv_app_page_top(app, app->document.count - 1);
	height += app->document.pages[app->document.count - 1].height * pv_app_scale(app, 0) + PV_MARGIN;

	/* Reports the height. */
	return height;
}

/*
 * Reports the width of the laid-out document, in pixels.
 */
double
pv_app_content_width(
	const struct pv_app *app)
{
	double width;

	/* Nothing without a document. */
	if (!app->has_document)
		return 0.0;

	/* The page mode's page, or the scroll mode's widest page, with the margins. */
	width = app->document.widest * pv_app_scale(app, 0);
	if (app->mode == PV_MODE_PAGE)
		width = app->document.pages[app->page].width * pv_app_scale(app, app->page);

	/* Reports the width. */
	return width + 2.0 * PV_MARGIN;
}

/*
 * Shows a message over the view for a while (0: until replaced).
 */
void
pv_app_message(
	struct pv_app *app,
	const char *message,
	uint64_t duration)
{
	/* Keeps the text and when it goes. */
	snprintf(app->message, sizeof(app->message), "%s", message);
	app->message_until = 0;
	if (duration != 0)
		app->message_until = app->now + duration;
	app->dirty = 1;
	pv_log("MESSAGE %s", message);
}

/*
 * Takes the file chooser's answer: the path chosen opens in place of the
 * document shown; NULL or an empty path says the chooser was cancelled.
 */
void
pv_app_chosen(
	struct pv_app *app,
	const char *path)
{
	/* The viewer no longer waits for the chooser. */
	app->choosing = 0;
	app->dirty = 1;

	/* Cancelled: the document shown stays. */
	if (path == NULL || path[0] == '\0') {
		pv_log("CHOOSER cancelled");
		return;
	}

	/* A file chosen is opened. */
	pv_log("CHOOSER chose path=%s", path);
	(void)pv_app_open(app, path);
}

/*
 * Reports the width of the sidebar of thumbnails: 0 unless it is asked
 * for, a document is open and the window leaves room for the pages.
 */
int
pv_app_sidebar_width(
	const struct pv_app *app)
{
	/* Not asked for, or nothing to show. */
	if (!app->thumbnails)
		return 0;
	if (!app->has_document)
		return 0;

	/* A window too narrow keeps its width for the pages. */
	if (app->window_width < PV_SIDEBAR_WIDTH + PV_SIDEBAR_ROOM)
		return 0;

	/* Reports the sidebar's width. */
	return PV_SIDEBAR_WIDTH;
}

/*
 * Reports the pages whose slots meet the sidebar's view, the first and the
 * last (last < first for a document without pages).
 */
void
pv_thumbnail_range(
	const struct pv_app *app,
	size_t *first,
	size_t *last)
{
	double top;
	double bottom;

	/* Nothing without pages. */
	*first = 1;
	*last = 0;
	if (!app->has_document || app->document.count == 0)
		return;

	/* The slots across the view's top and its bottom, within the document. */
	top = app->thumbnail_scroll - PV_THUMBNAIL_TOP;
	bottom = top + (double)app->height;
	if (top < 0.0)
		top = 0.0;
	*first = (size_t)(top / PV_THUMBNAIL_SLOT);
	*last = (size_t)(bottom / PV_THUMBNAIL_SLOT);
	if (*last >= app->document.count)
		*last = app->document.count - 1;
	if (*first > *last)
		*first = *last;
}

/*
 * Places a page's thumbnail in the window: the page's shape fitted in the
 * sidebar's box, centred across and standing on the box's bottom, the
 * box in the page's slot.
 */
void
pv_thumbnail_place(
	const struct pv_app *app,
	size_t index,
	int *x,
	int *y,
	int *width,
	int *height)
{
	const struct pv_page *page;
	double scale;
	double tall_scale;
	double slot_top;

	/* The page fitted in the box, as document.c draws its thumbnail. */
	page = &app->document.pages[index];
	scale = (double)PV_THUMBNAIL_WIDTH / page->width;
	tall_scale = (double)PV_THUMBNAIL_HEIGHT / page->height;
	if (tall_scale < scale)
		scale = tall_scale;
	*width = (int)ceil(page->width * scale - 1e-6);
	*height = (int)ceil(page->height * scale - 1e-6);

	/* The box in the slot, under the slot's top space; the thumbnail centred across it, on its bottom. */
	slot_top = PV_THUMBNAIL_TOP + (double)index * PV_THUMBNAIL_SLOT - app->thumbnail_scroll;
	*x = (PV_SIDEBAR_WIDTH - *width) / 2;
	*y = (int)floor(slot_top) + 8 + (PV_THUMBNAIL_HEIGHT - *height);
}

/*
 * Places the password card in the middle of the window, or of the part
 * of it the on-screen keyboard leaves (ws090-p008).
 */
void
pv_password_layout(
	const struct pv_app *app,
	int *x,
	int *y,
	int *width,
	int *height)
{
	int shown_width;
	int shown_height;

	/* The part of the window the on-screen keyboard leaves (all of it without the keyboard). */
	shown_width = app->window_width - app->keyboard_right;
	shown_height = app->height - app->keyboard_bottom;
	if (shown_width < PV_PASSWORD_WIDTH / 2)
		shown_width = app->window_width;
	if (shown_height < PV_PASSWORD_HEIGHT)
		shown_height = app->height;

	/* The card's size, within that part less a margin. */
	*width = PV_PASSWORD_WIDTH;
	if (*width > shown_width - 24)
		*width = shown_width - 24;
	*height = PV_PASSWORD_HEIGHT;

	/* In its middle. */
	*x = (shown_width - *width) / 2;
	*y = (shown_height - *height) / 2;
}

/*
 * Keeps the view within the laid-out document after it was moved from
 * outside (touch.c), and draws the frame again.
 */
void
pv_app_clamp(
	struct pv_app *app)
{
	/* Within the document, drawn again. */
	clamp_view(app);
	app->dirty = 1;
}

/*
 * Sets the user's zoom (within its bounds), without moving the view; the
 * caller places the view afterwards.
 */
void
pv_app_zoom_to(
	struct pv_app *app,
	double scale)
{
	/* Within the zoom's bounds. */
	if (scale < VIEW_ZOOM_MIN)
		scale = VIEW_ZOOM_MIN;
	if (scale > VIEW_ZOOM_MAX)
		scale = VIEW_ZOOM_MAX;

	/* The user's zoom from now on. */
	app->zoom = scale;
	app->fit = PV_FIT_CUSTOM;
	app->dirty = 1;
}

/*
 * Reports where a page's left edge is in the view, in pixels (the frame
 * draws it there, give or take the rounding to whole pixels): centred
 * when the pages fit across, otherwise moved by scroll_x.
 */
double
pv_app_page_left(
	const struct pv_app *app,
	size_t index)
{
	double width;
	double content_width;
	double left;

	/* The page's width, and the laid-out document's. */
	width = app->document.pages[index].width * pv_app_scale(app, index);
	content_width = pv_app_content_width(app);

	/* Centred across, or from the left of the view when the pages are wider. */
	left = ((double)app->width - width) / 2.0;
	if (app->mode == PV_MODE_SCROLL &&
	    content_width > (double)app->width)
		left = PV_MARGIN + (content_width - 2.0 * PV_MARGIN - width) / 2.0 - app->scroll_x;
	if (app->mode == PV_MODE_PAGE &&
	    width + 2.0 * PV_MARGIN > (double)app->width)
		left = PV_MARGIN - app->scroll_x;

	/* Reports the edge. */
	return left;
}

/*
 * Finds the place in the document under a point of the view (pixels from
 * the view's top left): the page whose slot holds it (the page mode's
 * page), and the point on that page in points.  Without a document the
 * place is page 0 at 0, 0.
 */
void
pv_app_place_at(
	const struct pv_app *app,
	double x,
	double y,
	struct pv_place *place)
{
	double down;
	double bottom;
	double scale;
	double top;
	double left;
	size_t page;

	/* Nothing to find without a document. */
	place->page = 0;
	place->x = 0.0;
	place->y = 0.0;
	if (!app->has_document)
		return;

	/* The page mode's page, or the scroll mode's page whose bottom (with its gap) is below the point. */
	page = app->page;
	if (app->mode == PV_MODE_SCROLL) {
		down = app->scroll_y + y;
		scale = pv_app_scale(app, 0);
		bottom = PV_MARGIN;
		for (page = 0; page + 1 < app->document.count; page++) {
			bottom += app->document.pages[page].height * scale + PV_GAP;
			if (bottom > down)
				break;
		}
	}

	/* The point on that page, in points. */
	scale = pv_app_scale(app, page);
	top = pv_app_page_top(app, page);
	left = pv_app_page_left(app, page);
	place->page = page;
	place->x = (x - left) / scale;
	place->y = (app->scroll_y + y - top) / scale;
}

/*
 * Moves the view so that a place in the document is under a point of the
 * view (as far as the document's ends allow; across, only when the pages
 * are wider than the view).
 */
void
pv_app_show_place(
	struct pv_app *app,
	const struct pv_place *place,
	double x,
	double y)
{
	double scale;
	double top;
	double left;
	double width;
	double content_width;

	/* Only a page the document has. */
	if (!app->has_document ||
	    place->page >= app->document.count)
		return;

	/* The page's scale, top and width in the layout now. */
	scale = pv_app_scale(app, place->page);
	top = pv_app_page_top(app, place->page);
	width = app->document.pages[place->page].width * scale;
	content_width = pv_app_content_width(app);

	/* Down: the place's height on the page under the point. */
	app->scroll_y = top + place->y * scale - y;

	/* Across: the page's left edge where the place lands under the point, when the pages scroll across. */
	left = x - place->x * scale;
	app->scroll_x = 0.0;
	if (app->mode == PV_MODE_SCROLL &&
	    content_width > (double)app->width)
		app->scroll_x = PV_MARGIN + (content_width - 2.0 * PV_MARGIN - width) / 2.0 - left;
	if (app->mode == PV_MODE_PAGE &&
	    width + 2.0 * PV_MARGIN > (double)app->width)
		app->scroll_x = PV_MARGIN - left;

	/* Within the document, drawn again. */
	clamp_view(app);
	app->dirty = 1;
}

/*
 * Shows a page (ws128-p004: the page of a place found): its top in the
 * scroll mode, the page itself in the page mode.
 */
void
pv_app_go_to(
	struct pv_app *app,
	size_t index)
{
	/* The view's own way of showing a page. */
	show_page(app, index);
}

/*
 * Ends a sideways drag of the page mode's page: let go far enough (a share
 * of the width) or fast enough (velocity, pixels a millisecond, as the
 * page moved), it turns to the neighbour it moved toward; otherwise, or
 * when it may not turn (the touch was cancelled), it slides back.
 */
void
pv_app_swipe_end(
	struct pv_app *app,
	double velocity,
	int may_turn)
{
	double share;
	int direction;

	/* How far the page moved, as a share of the width. */
	share = app->swipe / (double)app->width;

	/* The direction it turns, if any. */
	direction = 0;
	if (may_turn) {
		if (share < -VIEW_SWIPE_SHARE ||
		    velocity < -VIEW_SWIPE_SPEED)
			direction = 1;
		if (share > VIEW_SWIPE_SHARE ||
		    velocity > VIEW_SWIPE_SPEED)
			direction = -1;
	}

	/* Turns, or slides back. */
	pv_log("SWIPE offset=%.0f velocity=%.2f direction=%d", app->swipe, velocity, direction);
	start_turn(app, direction);
}

/*
 * Writes a log line on standard error: PDFVIEWER and the message.  The
 * tests wait for these lines.
 */
void
pv_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The prefix, the message and the end of the line, at once. */
	fputs("PDFVIEWER ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
}

/*
 * Reports a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
pv_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Finds the page the view is on: the page mode's page, or the one across the middle of the scroll mode's view. */
static size_t
current_page(
	const struct pv_app *app)
{
	double middle;
	double bottom;
	double scale;
	size_t page;

	/* No document is on page 0. */
	if (!app->has_document)
		return 0;

	/* The page mode's page. */
	if (app->mode == PV_MODE_PAGE)
		return app->page;

	/* The first page whose bottom (with its gap) is below the middle of the view. */
	middle = app->scroll_y + (double)app->height / 2.0;
	scale = pv_app_scale(app, 0);
	bottom = PV_MARGIN;
	for (page = 0; page < app->document.count; page++) {
		bottom += app->document.pages[page].height * scale + PV_GAP;
		if (bottom > middle)
			return page;
	}

	/* Below the last page. */
	return app->document.count - 1;
}

/* Keeps the view within the laid-out document. */
static void
clamp_view(
	struct pv_app *app)
{
	double largest;

	/* The page mode's page stays within the document. */
	if (app->has_document && app->page >= app->document.count)
		app->page = app->document.count - 1;

	/* The top of the view. */
	largest = pv_app_content_height(app) - (double)app->height;
	if (largest < 0.0)
		largest = 0.0;
	if (app->scroll_y > largest)
		app->scroll_y = largest;
	if (app->scroll_y < 0.0)
		app->scroll_y = 0.0;

	/* The left of the view. */
	largest = pv_app_content_width(app) - (double)app->width;
	if (largest < 0.0)
		largest = 0.0;
	if (app->scroll_x > largest)
		app->scroll_x = largest;
	if (app->scroll_x < 0.0)
		app->scroll_x = 0.0;
}

/* Shows a page: the scroll mode scrolls it to the top, the page mode shows it alone. */
static void
show_page(
	struct pv_app *app,
	size_t index)
{
	size_t current;

	/* Only a page the document has. */
	if (!app->has_document)
		return;
	if (index >= app->document.count)
		index = app->document.count - 1;

	/* Another page than the one shown: the turn's time starts (main.c logs when its frame is shown). */
	current = pv_app_current_page(app);
	if (index != current && app->turn_at == 0U)
		app->turn_at = pv_clock();

	/* Scrolls or switches to it. */
	if (app->mode == PV_MODE_SCROLL) {
		app->scroll_y = pv_app_page_top(app, index) - PV_MARGIN;
	} else {
		app->page = index;
		app->scroll_y = 0.0;
		app->swipe = 0.0;
		app->turning = 0;
	}

	/* The view stays within the document, and the indicator shows. */
	clamp_view(app);
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
	pv_log("PAGE shown=%lu", (unsigned long)index);
}

/*
 * Starts turning the page mode's page: +1 slides the next page in from the
 * right, -1 the previous one from the left, and 0 slides a dragged page
 * back; a turn past the first or the last page slides back.
 */
static void
start_turn(
	struct pv_app *app,
	int direction)
{
	double distance;

	/* Nothing turns without a document. */
	if (!app->has_document)
		return;

	/* A turn past either end slides back. */
	if (direction < 0 && app->page == 0)
		direction = 0;
	if (direction > 0 && app->page + 1 >= app->document.count)
		direction = 0;

	/* Slides from where the page is to where its neighbour stands, or back to rest. */
	distance = 0.0;
	if (direction > 0)
		distance = pv_app_neighbour_distance(app, app->page + 1);
	if (direction < 0)
		distance = pv_app_neighbour_distance(app, app->page - 1);
	app->turning = 1;
	if (direction != 0 && app->turn_at == 0U)
		app->turn_at = pv_clock();
	app->turn_from = app->swipe;
	app->turn_to = -(double)direction * distance;
	app->turn_direction = direction;
	app->turn_start = app->now;
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
	pv_log("TURN direction=%d from=%.0f", direction, app->turn_from);
}

/* Zooms by a factor, keeping the point in the middle of the view where it is. */
static void
zoom_by(
	struct pv_app *app,
	double factor)
{
	double scale;
	double middle;
	double top;
	double fraction;
	size_t page;

	/* Nothing to zoom without a document. */
	if (!app->has_document)
		return;

	/* Where the middle of the view is: which page, and how far down it. */
	page = current_page(app);
	scale = pv_app_scale(app, page);
	middle = app->scroll_y + (double)app->height / 2.0;
	top = pv_app_page_top(app, page);
	fraction = (middle - top) / (app->document.pages[page].height * scale);

	/* The new zoom, within its bounds. */
	scale *= factor;
	if (scale < VIEW_ZOOM_MIN)
		scale = VIEW_ZOOM_MIN;
	if (scale > VIEW_ZOOM_MAX)
		scale = VIEW_ZOOM_MAX;
	app->zoom = scale;
	app->fit = PV_FIT_CUSTOM;

	/* The same point in the middle again. */
	keep_anchor(app, page, fraction);
	pv_log("ZOOM scale=%.3f", scale);
}

/* Tells whether a key opens, closes, toggles or confirms something, so that its repeat is ignored. */
static int
toggle_key(
	const struct pv_event *event)
{
	/* With Control: open, close, quit, annotate, and the zoom's reset. */
	if ((event->modifiers & PV_MOD_CTRL) != 0) {
		switch (event->key) {
		case PV_KEY_O:
		case PV_KEY_W:
		case PV_KEY_Q:
		case PV_KEY_E:
		case PV_KEY_0:
		case PV_KEY_F:
		case PV_KEY_C:
			return 1;
		default:
			return 0;
		}
	}

	/* Alone: the thumbnails, the ends of the document, and the confirming and cancelling keys. */
	switch (event->key) {
	case PV_KEY_F9:
	case PV_KEY_HOME:
	case PV_KEY_END:
	case PV_KEY_ENTER:
	case PV_KEY_KP_ENTER:
	case PV_KEY_ESCAPE:
		return 1;
	default:
		break;
	}

	/* The rest may repeat. */
	return 0;
}

/* Scrolls so that a place (a share of a page's height) is in the middle of the view. */
static void
keep_anchor(
	struct pv_app *app,
	size_t page,
	double fraction)
{
	double scale;
	double top;
	double width;

	/* The page's top and scale in the new layout. */
	scale = pv_app_scale(app, page);
	top = pv_app_page_top(app, page);

	/* The place in the middle, and the view centred across. */
	app->scroll_y = top + fraction * app->document.pages[page].height * scale - (double)app->height / 2.0;
	width = pv_app_content_width(app);
	app->scroll_x = (width - (double)app->width) / 2.0;
	clamp_view(app);
	app->dirty = 1;
}

/* Handles a key press. */
static void
handle_key(
	struct pv_app *app,
	const struct pv_event *event)
{
	double page_height;
	double content_width;
	double content_height;
	int fits_across;
	int fits_down;

	/* The shortcuts with Control (the menus choose them first when the compositor has menus). */
	if ((event->modifiers & PV_MOD_CTRL) != 0) {
		switch (event->key) {
		case PV_KEY_O:
			pv_app_action(app, PV_ACTION_OPEN);
			break;
		case PV_KEY_W:
			pv_app_action(app, PV_ACTION_CLOSE);
			break;
		case PV_KEY_Q:
			pv_app_action(app, PV_ACTION_QUIT);
			break;
		case PV_KEY_E:
			pv_app_action(app, PV_ACTION_ANNOTATE);
			break;
		case PV_KEY_EQUAL:
		case PV_KEY_KP_PLUS:
			pv_app_action(app, PV_ACTION_ZOOM_IN);
			break;
		case PV_KEY_MINUS:
		case PV_KEY_KP_MINUS:
			pv_app_action(app, PV_ACTION_ZOOM_OUT);
			break;
		case PV_KEY_0:
			pv_app_action(app, PV_ACTION_ZOOM_RESET);
			break;
		case PV_KEY_F:
			pv_app_action(app, PV_ACTION_FIND);
			break;
		case PV_KEY_C:
			pv_app_action(app, PV_ACTION_COPY);
			break;
		case PV_KEY_A:
			pv_app_action(app, PV_ACTION_SELECT_ALL);
			break;
		default:
			break;
		}

		/* Control with any other key does nothing more. */
		return;
	}

	/*
	 * Whether the pages fit across and down: the arrows turn pages where
	 * there is nothing to scroll.
	 */
	fits_across = 0;
	content_width = pv_app_content_width(app);
	if (content_width <= (double)app->width)
		fits_across = 1;
	fits_down = 0;
	content_height = pv_app_content_height(app);
	if (content_height <= (double)app->height)
		fits_down = 1;

	/* The movement keys, by the mode. */
	page_height = (double)app->height - VIEW_KEY_STEP;
	switch (event->key) {
	case PV_KEY_PAGE_UP:
		/* The page mode turns back; the scroll mode scrolls up a screen. */
		if (app->mode == PV_MODE_PAGE)
			pv_app_action(app, PV_ACTION_PREVIOUS);
		else
			app->scroll_y -= page_height;
		break;
	case PV_KEY_PAGE_DOWN:
	case PV_KEY_SPACE:
		/* The page mode turns on; the scroll mode scrolls a screen, up with Shift. */
		if (app->mode == PV_MODE_PAGE)
			pv_app_action(app, PV_ACTION_NEXT);
		else if ((event->modifiers & PV_MOD_SHIFT) != 0)
			app->scroll_y -= page_height;
		else
			app->scroll_y += page_height;
		break;
	case PV_KEY_LEFT:
		/* Turns back, or scrolls left over pages wider than the window. */
		if (app->mode == PV_MODE_PAGE || fits_across)
			pv_app_action(app, PV_ACTION_PREVIOUS);
		else
			app->scroll_x -= VIEW_KEY_STEP;
		break;
	case PV_KEY_RIGHT:
		/* Turns on, or scrolls right over pages wider than the window. */
		if (app->mode == PV_MODE_PAGE || fits_across)
			pv_app_action(app, PV_ACTION_NEXT);
		else
			app->scroll_x += VIEW_KEY_STEP;
		break;
	case PV_KEY_UP:
		/* The page mode's page that fits turns back; anything else scrolls up. */
		if (app->mode == PV_MODE_PAGE && fits_down)
			pv_app_action(app, PV_ACTION_PREVIOUS);
		else
			app->scroll_y -= VIEW_KEY_STEP;
		break;
	case PV_KEY_DOWN:
		/* The page mode's page that fits turns on; anything else scrolls down. */
		if (app->mode == PV_MODE_PAGE && fits_down)
			pv_app_action(app, PV_ACTION_NEXT);
		else
			app->scroll_y += VIEW_KEY_STEP;
		break;
	case PV_KEY_HOME:
		pv_app_action(app, PV_ACTION_FIRST);
		break;
	case PV_KEY_END:
		pv_app_action(app, PV_ACTION_LAST);
		break;
	case PV_KEY_F9:
		pv_app_action(app, PV_ACTION_THUMBNAILS);
		break;
	case PV_KEY_F3:
		/* The next place found, the one before with Shift (ws128-p004). */
		if ((event->modifiers & PV_MOD_SHIFT) != 0)
			pv_app_action(app, PV_ACTION_FIND_PREVIOUS);
		else
			pv_app_action(app, PV_ACTION_FIND_NEXT);
		break;
	case PV_KEY_ESCAPE:
		/* The selection and the places found let go. */
		pv_find_clear(app);
		break;
	default:
		return;
	}

	/* The view stays within the document and is drawn again. */
	clamp_view(app);
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
}

/* Handles a pointer button: a press starts a drag or a swipe, a release ends it. */
static void
handle_button(
	struct pv_app *app,
	const struct pv_event *event)
{
	/* Only the left button. */
	if (event->button != PV_BUTTON_LEFT)
		return;

	/* A press starts a drag from where the view is. */
	if (event->pressed) {
		app->pressed = 1;
		app->dragging = 0;
		app->press_x = event->x;
		app->press_y = event->y;
		app->last_x = event->x;
		app->last_y = event->y;
		app->last_time = event->time;
		app->press_time = event->time;
		app->velocity_x = 0.0;
		app->press_scroll_x = app->scroll_x;
		app->press_scroll_y = app->scroll_y;
		return;
	}

	/* A release ends the press; a sideways drag of the page mode turns or slides back. */
	if (!app->pressed)
		return;
	app->pressed = 0;
	if (app->mode == PV_MODE_PAGE && app->dragging == 1)
		pv_app_swipe_end(app, app->velocity_x, 1);

	/* No drag goes on after a release. */
	app->dragging = 0;
}

/*
 * Handles the pointer's motion: a drag scrolls (the scroll mode, or the
 * page mode's page along its height), a sideways drag of the page mode
 * moves the page with the pointer.
 */
static void
handle_motion(
	struct pv_app *app,
	const struct pv_event *event)
{
	int moved_x;
	int moved_y;
	int distance_x;
	int distance_y;
	int found;
	uint64_t elapsed;

	/* Only a press drags. */
	if (!app->pressed || app->turning)
		return;
	moved_x = event->x - app->press_x;
	moved_y = event->y - app->press_y;

	/* The drag starts once the pointer has moved far enough; the page mode chooses sideways or along. */
	distance_x = moved_x;
	if (distance_x < 0)
		distance_x = -distance_x;
	distance_y = moved_y;
	if (distance_y < 0)
		distance_y = -distance_y;
	if (app->dragging == 0) {
		if (distance_x < VIEW_DRAG_START && distance_y < VIEW_DRAG_START)
			return;

		/* A press held still on an image first drags the image out of the window, not the view (ws189-p003). */
		if (event->time - app->press_time >= PV_DRAG_HOLD_MS) {
			found = image_at(app, app->press_x, app->press_y);
			if (found) {
				app->pressed = 0;
				app->drag_request = PV_DRAG_IMAGE;
				return;
			}
		}

		/* The view's drag. */
		app->dragging = 2;
		if (app->mode == PV_MODE_PAGE && distance_x >= distance_y)
			app->dragging = 1;
	}

	/* The speed across, for the swipe's release, smoothed over the last moves. */
	elapsed = event->time - app->last_time;
	if (elapsed > 0) {
		app->velocity_x = app->velocity_x * 0.4 + 0.6 * (double)(event->x - app->last_x) / (double)elapsed;
	}

	/* The pointer's last place and time. */
	app->last_x = event->x;
	app->last_y = event->y;
	app->last_time = event->time;

	/* A swipe follows the pointer across, resisting past the first and the last page. */
	if (app->dragging == 1) {
		app->swipe = (double)moved_x;
		if (app->page == 0 && app->swipe > 0.0)
			app->swipe /= 3.0;
		if (app->page + 1 >= app->document.count && app->swipe < 0.0)
			app->swipe /= 3.0;
		app->dirty = 1;
		return;
	}

	/* A drag moves the view with the pointer. */
	app->scroll_x = app->press_scroll_x - (double)moved_x;
	app->scroll_y = app->press_scroll_y - (double)moved_y;
	clamp_view(app);
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
}

/* Handles the wheel: Control zooms, the scroll mode scrolls, the page mode scrolls a tall page or turns. */
static void
handle_axis(
	struct pv_app *app,
	const struct pv_event *event)
{
	double largest;

	/* Control and the wheel zoom. */
	if ((event->modifiers & PV_MOD_CTRL) != 0) {
		if (event->scroll < 0)
			pv_app_action(app, PV_ACTION_ZOOM_IN);
		if (event->scroll > 0)
			pv_app_action(app, PV_ACTION_ZOOM_OUT);
		return;
	}

	/* The page mode turns at the page's ends. */
	if (app->mode == PV_MODE_PAGE && !app->turning) {
		largest = pv_app_content_height(app) - (double)app->height;
		if (largest < 0.0)
			largest = 0.0;
		if ((event->scroll > 0 &&
		     app->scroll_y >= largest) ||
		    (event->scroll < 0 &&
		     app->scroll_y <= 0.0)) {
			app->wheel += (double)event->scroll;
			if (app->wheel >= VIEW_WHEEL_TURN) {
				app->wheel = 0.0;
				pv_app_action(app, PV_ACTION_NEXT);
			} else if (app->wheel <= -VIEW_WHEEL_TURN) {
				app->wheel = 0.0;
				pv_app_action(app, PV_ACTION_PREVIOUS);
			}

			/* The wheel turned the page, or gathered toward a turn, and scrolls nothing. */
			return;
		}
	}

	/* Scrolls the view. */
	app->wheel = 0.0;
	app->scroll_y += (double)event->scroll;
	clamp_view(app);
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->dirty = 1;
}

/*
 * Asks for the file chooser (libkeiland's, which the window shows: main.c) at
 * the open document's folder, or the home folder.
 */
static void
open_chooser(
	struct pv_app *app)
{
	char folder[PV_PATH_MAX];
	const char *home;
	char *slash;

	/* The document's folder, the home folder, or the root. */
	folder[0] = '\0';
	if (app->has_document) {
		snprintf(folder, sizeof(folder), "%s", app->document.path);
		slash = strrchr(folder, '/');
		if (slash != NULL && slash != folder)
			*slash = '\0';
		if (slash == NULL)
			snprintf(folder, sizeof(folder), ".");
	}

	/* Without a document, the home folder, or the root. */
	if (folder[0] == '\0') {
		home = getenv("HOME");
		if (home == NULL || home[0] == '\0')
			home = "/";
		snprintf(folder, sizeof(folder), "%s", home);
	}

	/* The viewer waits for the chooser's answer (pv_app_chosen); the chooser starts in that folder. */
	snprintf(app->chooser_folder, sizeof(app->chooser_folder), "%s", folder);
	app->choosing = 1;
	app->dirty = 1;
	pv_log("CHOOSER open folder=%s", app->chooser_folder);
}

/* Says in words why a file could not be opened. */
static const char *
reason_of(
	int error)
{
	const char *words;

	/* The reasons libpdf and the system give. */
	if (error == ENOTSUP)
		return "it uses PDF features this version does not read yet";
	if (error == PDF_EFORMAT)
		return "the file is damaged or is not a PDF";
	if (error == ENOENT)
		return "there is no such file";
	if (error == EACCES)
		return "permission denied";
	if (error == ENOMEM)
		return "not enough memory";
	if (error == EFBIG)
		return "the file is too large";
	if (error == EINVAL)
		return "the document has no pages";

	/* Anything else, by the system's words. */
	words = strerror(error);
	return words;
}

/*
 * Opens a document with a password (NULL: none) in place of the one shown.
 * A file that cannot be opened leaves a message and no document; an
 * encrypted one the password does not open shows the password card, saying
 * so when a password was tried.
 *
 * Returns 0, or an errno value.
 */
static int
open_document(
	struct pv_app *app,
	const char *path,
	const char *password)
{
	char message[sizeof(app->message)];
	const char *name;
	const char *reason;
	int encrypted;
	int checked;
	int error;

	/* Closes the document shown. */
	pv_app_close_document(app);

	/* Opens the new one. */
	error = pv_document_open(&app->document, path, password);
	if (error != 0) {
		name = strrchr(path, '/');
		if (name == NULL) {
			name = path;
		} else {
			name++;
		}

		/* An encrypted document refused with EACCES needs a (another) password: the card asks for it. */
		encrypted = 0;
		if (error == PDF_EPASSWORD) {
			checked = pdf_document_encrypted(path, &encrypted);
			if (checked != 0)
				encrypted = 0;
		}

		/* The card asks for it. */
		if (encrypted) {
			pv_log("OPEN failed path=%s error=%d", path, error);
			ask_password(app, path, password != NULL);
			return error;
		}

		/* Anything else is told in words, and logged for the tests. */
		reason = reason_of(error);
		snprintf(message, sizeof(message), "Cannot open %s: %s.", name, reason);
		pv_app_message(app, message, VIEW_MESSAGE_MS * 2U);
		pv_log("OPEN failed path=%s error=%d", path, error);
		return error;
	}

	/* Starts at its first page, fitting the mode, the sidebar at its top. */
	app->has_document = 1;
	app->opened = 1;
	app->page = 0;
	app->scroll_x = 0.0;
	app->scroll_y = 0.0;
	app->swipe = 0.0;
	app->turning = 0;
	app->fit = PV_FIT_WIDTH;
	if (app->mode == PV_MODE_PAGE)
		app->fit = PV_FIT_PAGE;
	app->message[0] = '\0';
	app->indicator_until = app->now + VIEW_INDICATOR_MS;
	app->thumbnail_scroll = 0.0;
	app->thumbnail_followed = 0;
	app->width = app->window_width - pv_app_sidebar_width(app);
	clamp_view(app);
	app->dirty = 1;

	/* Succeeded: the document is shown. */
	pv_log("OPEN path=%s pages=%lu", path, (unsigned long)app->document.count);
	return 0;
}

/* Lays the pages out again beside the sidebar (or without it), keeping the page in view. */
static void
relayout(
	struct pv_app *app)
{
	size_t page;

	/* The page in view before the width changes. */
	page = current_page(app);

	/* The pages' width, and the same page at the top of the scroll mode's view. */
	app->width = app->window_width - pv_app_sidebar_width(app);
	if (app->mode == PV_MODE_SCROLL && app->has_document)
		app->scroll_y = pv_app_page_top(app, page) - PV_MARGIN;
	clamp_view(app);
	clamp_sidebar(app);
	app->dirty = 1;
}

/*
 * Tells whether a pointer input is the sidebar's: a press on it (not while
 * the pages are dragged), the moves and the release of that press, and the
 * wheel over it.
 */
static int
sidebar_takes(
	const struct pv_app *app,
	const struct pv_event *event,
	int sidebar)
{
	/* No sidebar takes nothing. */
	if (sidebar == 0)
		return 0;

	/* A press the sidebar took keeps its moves and its release. */
	if (app->thumbnail_pressed) {
		if (event->type == PV_EVENT_BUTTON)
			return 1;
		if (event->type == PV_EVENT_MOTION)
			return 1;
	}

	/* A drag of the pages keeps the pointer even over the sidebar. */
	if (app->pressed)
		return 0;

	/* A press or the wheel over the sidebar. */
	if (event->x >= sidebar)
		return 0;
	if (event->type == PV_EVENT_BUTTON && event->pressed)
		return 1;
	if (event->type == PV_EVENT_AXIS)
		return 1;

	/* Anything else goes to the pages. */
	return 0;
}

/*
 * Handles the sidebar's pointer input: a press and a release in place
 * shows the page of the thumbnail under it, a press moved along drags the
 * column, and the wheel scrolls it.
 */
static void
sidebar_event(
	struct pv_app *app,
	const struct pv_event *event)
{
	size_t index;
	int moved;
	int found;

	/* Handles it by its kind. */
	switch (event->type) {
	case PV_EVENT_BUTTON:
		/* Only the left button. */
		if (event->button != PV_BUTTON_LEFT)
			return;

		/* A press starts in place. */
		if (event->pressed) {
			app->thumbnail_pressed = 1;
			app->thumbnail_dragging = 0;
			app->thumbnail_press_y = event->y;
			app->thumbnail_press_scroll = app->thumbnail_scroll;
			return;
		}

		/* A release ends the press; one that did not drag chooses the thumbnail under it. */
		app->thumbnail_pressed = 0;
		if (app->thumbnail_dragging) {
			app->thumbnail_dragging = 0;
			return;
		}

		/* The thumbnail under the release, when there is one, shows its page. */
		found = thumbnail_at(app, event->y, &index);
		if (!found)
			return;
		pv_log("THUMBNAIL chose page=%lu", (unsigned long)index);
		show_page(app, index);
		break;
	case PV_EVENT_MOTION:
		/* The column moves with the pointer once it has moved far enough. */
		moved = event->y - app->thumbnail_press_y;
		if (!app->thumbnail_dragging) {
			if (moved < VIEW_DRAG_START && moved > -VIEW_DRAG_START)
				return;
			app->thumbnail_dragging = 1;
		}

		/* The column follows the pointer. */
		app->thumbnail_scroll = app->thumbnail_press_scroll - (double)moved;
		clamp_sidebar(app);
		app->dirty = 1;
		break;
	case PV_EVENT_AXIS:
		/* The wheel scrolls the column. */
		app->thumbnail_scroll += (double)event->scroll;
		clamp_sidebar(app);
		app->dirty = 1;
		break;
	case PV_EVENT_KEY:
	case PV_EVENT_LEAVE:
	case PV_EVENT_ACTION:
		break;
	}
}

/* Finds the page whose slot is at a height of the sidebar; 0 when none is. */
static int
thumbnail_at(
	const struct pv_app *app,
	int y,
	size_t *index)
{
	double place;

	/* The place in the column, under the space above the first slot. */
	place = (double)y + app->thumbnail_scroll - PV_THUMBNAIL_TOP;
	if (place < 0.0)
		return 0;

	/* The slot there, when the document has that page. */
	*index = (size_t)(place / PV_THUMBNAIL_SLOT);
	if (*index >= app->document.count)
		return 0;

	/* Found: the page of the slot. */
	return 1;
}

/* Keeps the sidebar's view within its column of thumbnails. */
static void
clamp_sidebar(
	struct pv_app *app)
{
	double largest;

	/* The column's height, with the space above and below, less the view's. */
	largest = 2.0 * PV_THUMBNAIL_TOP + (double)app->document.count * PV_THUMBNAIL_SLOT - (double)app->height;
	if (!app->has_document)
		largest = 0.0;
	if (largest < 0.0)
		largest = 0.0;

	/* The view's top within it. */
	if (app->thumbnail_scroll > largest)
		app->thumbnail_scroll = largest;
	if (app->thumbnail_scroll < 0.0)
		app->thumbnail_scroll = 0.0;
}

/* Scrolls the sidebar the least that shows a page's whole slot. */
static void
reveal_thumbnail(
	struct pv_app *app,
	size_t index)
{
	double top;
	double bottom;

	/* The slot's top and bottom in the column, with the space around it. */
	top = (double)index * PV_THUMBNAIL_SLOT;
	bottom = top + PV_THUMBNAIL_SLOT + 2.0 * PV_THUMBNAIL_TOP;

	/* Up to its top, or down to its bottom. */
	if (top < app->thumbnail_scroll)
		app->thumbnail_scroll = top;
	if (bottom > app->thumbnail_scroll + (double)app->height)
		app->thumbnail_scroll = bottom - (double)app->height;
	clamp_sidebar(app);
	app->dirty = 1;
}

/* Shows the password card for a document, empty, saying whether a password was just refused. */
static void
ask_password(
	struct pv_app *app,
	const char *path,
	int wrong)
{
	/* The document it is for, nothing typed. */
	snprintf(app->password_path, sizeof(app->password_path), "%s", path);
	forget_password(app);
	app->asking_password = 1;
	app->password_wrong = wrong;
	app->choosing = 0;
	app->dirty = 1;

	/* The log the tests read; never the password. */
	pv_log("PASSWORD asked path=%s wrong=%d", path, wrong);
}

/*
 * Handles an input while the password card is shown: the keys type, a
 * click on Open tries the password and one on Cancel closes the card.
 */
static void
password_event(
	struct pv_app *app,
	const struct pv_event *event)
{
	int x;
	int y;
	int width;
	int height;
	int open_x;
	int cancel_x;
	int buttons_y;

	/* A key pressed. */
	if (event->type == PV_EVENT_KEY) {
		if (event->pressed)
			password_key(app, event);
		return;
	}

	/* Only a press of the left button is a click. */
	if (event->type != PV_EVENT_BUTTON)
		return;
	if (event->button != PV_BUTTON_LEFT || !event->pressed)
		return;

	/* Where the buttons are: Open in the card's bottom right corner, Cancel before it. */
	pv_password_layout(app, &x, &y, &width, &height);
	open_x = x + width - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_WIDTH;
	cancel_x = open_x - 12 - PV_PASSWORD_BUTTON_WIDTH;
	buttons_y = y + height - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_HEIGHT;

	/* A click outside the buttons' row does nothing. */
	if (event->y < buttons_y || event->y >= buttons_y + PV_PASSWORD_BUTTON_HEIGHT)
		return;

	/* Open tries the password. */
	if (event->x >= open_x && event->x < open_x + PV_PASSWORD_BUTTON_WIDTH) {
		submit_password(app);
		return;
	}

	/* Cancel closes the card. */
	if (event->x >= cancel_x && event->x < cancel_x + PV_PASSWORD_BUTTON_WIDTH)
		cancel_password(app);
}

/* Handles a key on the password card: Enter tries, Escape cancels, Backspace erases, and the rest type. */
static void
password_key(
	struct pv_app *app,
	const struct pv_event *event)
{
	char character;

	/* Handles the keys that are commands. */
	switch (event->key) {
	case PV_KEY_ENTER:
	case PV_KEY_KP_ENTER:
		submit_password(app);
		return;
	case PV_KEY_ESCAPE:
		cancel_password(app);
		return;
	case PV_KEY_BACKSPACE:
		/* The last character goes, when there is one. */
		if (app->password_length == 0)
			return;
		app->password_length--;
		app->password[app->password_length] = '\0';
		app->dirty = 1;
		return;
	default:
		break;
	}

	/* A key that types a character adds it, within the longest password. */
	character = key_character(event->key, event->modifiers);
	if (character == 0)
		return;
	if (app->password_length >= PV_PASSWORD_MAX)
		return;
	app->password[app->password_length] = character;
	app->password_length++;
	app->password[app->password_length] = '\0';
	app->password_wrong = 0;
	app->dirty = 1;
}

/* Tries to open the card's document with the password typed; a refused one asks again. */
static void
submit_password(
	struct pv_app *app)
{
	char path[PV_PATH_MAX];
	char password[PV_PASSWORD_MAX + 1];
	int error;

	/* The document and the password, taken from the card, which the try may show again. */
	snprintf(path, sizeof(path), "%s", app->password_path);
	memcpy(password, app->password, sizeof(password));
	forget_password(app);
	app->asking_password = 0;
	pv_log("PASSWORD try path=%s", path);

	/* Opens it with the password; a refusal shows the card again, saying so. */
	error = open_document(app, path, password);
	memset(password, 0, sizeof(password));
	if (error != 0)
		return;

	/* Succeeded: the document is shown and the card is gone. */
	pv_log("PASSWORD accepted path=%s", path);
}

/* Closes the password card without opening its document. */
static void
cancel_password(
	struct pv_app *app)
{
	/* Nothing typed is kept. */
	forget_password(app);
	app->asking_password = 0;
	app->password_wrong = 0;
	app->dirty = 1;
	pv_log("PASSWORD cancelled path=%s", app->password_path);
}

/* Clears the password typed so far from memory. */
static void
forget_password(
	struct pv_app *app)
{
	/* Every byte of the buffer, not only the typed ones. */
	memset(app->password, 0, sizeof(app->password));
	app->password_length = 0;
}

/*
 * Reports the ASCII character a key types with the modifiers held (the US
 * layout), or 0 for a key that types none or a command key.
 */
static char
key_character(
	uint32_t key,
	uint32_t modifiers)
{
	/* A command key types nothing. */
	if ((modifiers & (PV_MOD_CTRL | PV_MOD_ALT | PV_MOD_SUPER)) != 0U)
		return 0;

	/* Keys past the tables type nothing. */
	if (key >= VIEW_KEY_TABLE_SIZE)
		return 0;

	/* The shifted character. */
	if ((modifiers & PV_MOD_SHIFT) != 0U)
		return view_shifted_keys[key];

	/* The plain character. */
	return view_plain_keys[key];
}

/* Reports the page mode's page top: centred when it is shorter than the window. */
static double
page_mode_top(
	const struct pv_app *app)
{
	double height;
	double top;

	/* The page's height at its scale. */
	if (!app->has_document)
		return PV_MARGIN;
	height = app->document.pages[app->page].height * pv_app_scale(app, app->page);

	/* Centred in the window, or under the margin when taller. */
	top = ((double)app->height - height) / 2.0;
	if (top < PV_MARGIN)
		top = PV_MARGIN;

	/* Reports the top. */
	return top;
}

/*
 * Finds the image of the page under a point of the pages' view (ws189-p003):
 * 1 with its page and corners kept for the drag (app->drag_page and
 * drag_quad, page points from the top left), 0 when no image is there.
 */
static int
image_at(
	struct pv_app *app,
	int x,
	int y)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct pv_place place;
	size_t index;
	int error;

	/* The page and its point. */
	pv_app_place_at(app, (double)x, (double)y, &place);
	if (place.page >= app->document.count)
		return 0;

	/* The page's objects, and the one at the point. */
	error = pdf_page_editor_open(app->document.document, place.page, &editor);
	if (error != 0)
		return 0;
	error = pdf_page_editor_hit(editor, place.x, place.y, &index);
	if (error == 0) {
		memset(&object, 0, sizeof(object));
		object.size = sizeof(object);
		error = pdf_page_editor_object(editor, index, &object);
	}

	/* The editor is done with; only an image is dragged. */
	pdf_page_editor_close(editor);
	if (error != 0 || object.kind != PDF_EDIT_IMAGE)
		return 0;

	/* Succeeded: the image's page and corners. */
	app->drag_page = place.page;
	memcpy(app->drag_quad, object.quad, sizeof(app->drag_quad));
	pv_log("DND image page=%lu object=%lu", (unsigned long)place.page, (unsigned long)index);
	return 1;
}
