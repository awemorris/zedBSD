/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface of files: the frame of the window (the sidebar,
 * the content panel, the preview) and what the pointer does on it.  The
 * navigation (back, forward, home, the path, the search field, the view
 * and the preview buttons) is in the window's titlebar, which zdesktop
 * draws (ui-titlebar.c, titlebar.c).
 *
 * Each frame is drawn from the app's state, and while it is drawn every
 * clickable part records where it is (fm_ui_hit).  The pointer's input is
 * matched against the regions of the last frame, so a click lands on what
 * the user saw under the pointer.  The content panel is drawn by the view
 * of the place shown (ui-grid.c).
 */

#include "files.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The sidebar's places' ID among libkeiland's widgets (ui-widgets.c). */
#define UI_WIDGET_PLACE		1001U

/* The frame's measurements, in pixels: the tasks' place from the corner. */
#define UI_MARGIN		12

/* The margin round a region drawn again alone (its edge and shadow, BUG-221, BUG-226). */
#define UI_DAMAGE_MARGIN	6
#define UI_GAP			10

/*
 * The panels reach the window's edges, so that their outer edges line up
 * with the floating titlebar's and the titlebar's gap above them is
 * zdesktop's alone (ws090-p021); on glass they stand apart by zdesktop's
 * gap between the titlebar and the window (ws071-p017).
 */
#define UI_GLASS_GAP		8
#define UI_SIDEBAR_WIDTH	212
#define UI_PREVIEW_WIDTH	264
#define UI_PANEL_RADIUS		16.0f
#define UI_SIDEBAR_ROW		30
#define UI_SIDEBAR_HEADER	30

/* The text sizes of the frame. */
#define UI_TEXT_SIDEBAR		14U
#define UI_TEXT_HEADER		11U

/* A second click this soon after the first on the same region is a double click, in milliseconds. */
#define UI_DOUBLE_CLICK_MS	400U



static void ui_layout(struct fm_app *app);
static void ui_clear_rect(struct kl_canvas *canvas, const struct kl_rect *rect);
static void ui_draw_sidebar(struct fm_app *app, struct kl_canvas *canvas);
static int ui_place_current(struct fm_app *app, const struct fm_place *place);
static const char *ui_location_kind_name(unsigned kind);
static void ui_leave(struct fm_tab *tab);
static void ui_mark_cut(struct fm_tab *tab);
static void ui_select_paths(struct fm_app *app, struct fm_tab *tab);

/*
 * Sets up the file manager: the home folder, the sidebar, one tab showing
 * a start folder (the home dashboard when start is NULL).
 *
 * Returns 0, or ENOMEM.
 */
int
fm_app_init(
	struct fm_app *app,
	struct kl_text *text,
	const char *start)
{
	struct fm_location location;
	struct passwd *account;
	const char *home;
	char *comma;

	/* The defaults: icons by name, sidebar shown, preview hidden, hidden files hidden. */
	memset(app, 0, sizeof(*app));
	app->drag_wait_ms = -1;
	app->text = text;
	app->width = FM_WIDTH;
	app->height = FM_HEIGHT;
	app->view = FM_VIEW_ICONS;
	app->sort = FM_SORT_NAME;
	app->show_sidebar = 1;
	app->columns = FM_COLUMNS_DEFAULT;
	app->column_drag = -1;
	app->wall = time(NULL);
	app->focused = 1;
	app->dirty = 1;
	app->hover_index = -1;
	app->press_index = -1;
	app->drag_hit_index = -1;
	app->drag_place = -1;
	app->drop_part = -1;
	snprintf(app->wallpaper, sizeof(app->wallpaper), "%s", FM_WALLPAPER);
	app->click_index = -1;

	/* The list's column widths an earlier run kept (BUG-220). */
	fm_list_widths_load(app);

	/* The user's account, for the name and the home folder. */
	account = getpwuid(getuid());
	if (account != NULL && account->pw_name != NULL)
		snprintf(app->user, sizeof(app->user), "%s", account->pw_name);

	/*
	 * The name the Home page greets: a person's display name, the first
	 * part of the GECOS field, as the login screen shows it (ws035-p120).
	 * root's GECOS names its role ("System Administrator"), so root keeps
	 * its account name.
	 */
	if (account != NULL &&
	    account->pw_uid != 0 &&
	    account->pw_gecos != NULL &&
	    account->pw_gecos[0] != '\0' &&
	    account->pw_gecos[0] != ',') {
		snprintf(app->user, sizeof(app->user), "%s", account->pw_gecos);
		comma = strchr(app->user, ',');
		if (comma != NULL)
			*comma = '\0';
	}

	/* The home folder: HOME, else the account's, else the root. */
	home = getenv("HOME");
	if ((home == NULL || home[0] == '\0') && account != NULL)
		home = account->pw_dir;
	if (home == NULL || home[0] == '\0')
		home = "/";
	snprintf(app->home, sizeof(app->home), "%s", home);

	/* The sidebar. */
	fm_places_init(&app->places, app->home);

	/* The first tab. */
	app->tabs[0] = calloc(1, sizeof(*app->tabs[0]));
	if (app->tabs[0] == NULL)
		return ENOMEM;
	app->tabs[0]->cursor = -1;
	app->tabs[0]->anchor = -1;
	app->tab_count = 1;
	app->tab_index = 0;

	/* It shows the start folder, or Today (the dashboard, ws127-p011: Files starts on it). */
	memset(&location, 0, sizeof(location));
	location.kind = FM_LOCATION_TODAY;
	snprintf(location.path, sizeof(location.path), "%s", app->home);
	if (start != NULL) {
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", start);
	}

	/* The tab goes there. */
	fm_ui_go(app, &location);

	/* Succeeded: the app can draw its first frame. */
	return 0;
}

/*
 * Frees the tabs and their listings.
 */
void
fm_app_release(
	struct fm_app *app)
{
	int index;

	/* The operations, stopped and let go, the widgets' input, the hero's pictures, the thumbnails and what the preview read. */
	fm_actions_release(app);
	fm_widgets_release(app);
	kl_image_release(&app->hero_source);
	kl_image_release(&app->hero);
	fm_thumb_release(app);
	fm_peek_release(&app->peek);
	fm_info_release(&app->info);

	/* Each tab's listing, then the tab. */
	for (index = 0; index < app->tab_count; index++) {
		fm_dir_free(&app->tabs[index]->listing);
		free(app->tabs[index]);
		app->tabs[index] = NULL;
	}

	/* No tab is left. */
	app->tab_count = 0;
}

/*
 * Handles one input of the window.
 */
void
fm_ui_event(
	struct fm_app *app,
	const struct fm_event *event)
{
	/* The time of the input, which double clicks are measured by. */
	app->now = event->time;
	app->modifiers = event->modifiers;

	/* Each kind of input. */
	switch (event->type) {
	case FM_EVENT_MOTION:
		fm_widgets_input(app, event);
		fm_input_motion(app, event);
		break;
	case FM_EVENT_BUTTON:
		/* libkeiland's widgets see the press too (the field of the name being changed places its caret; elsewhere the change ends). */
		fm_widgets_input(app, event);
		fm_input_motion(app, event);
		fm_input_button(app, event);
		break;
	case FM_EVENT_AXIS:
		fm_input_scroll(app, event->scroll);
		break;
	case FM_EVENT_LEAVE:
		fm_widgets_input(app, event);
		app->pointer_inside = 0;
		app->hover_kind = FM_HIT_NONE;
		app->hover_index = -1;
		app->dirty = 1;
		fm_scrollbar_leave(app);
		break;
	case FM_EVENT_KEY:
		fm_input_key(app, event);
		break;
	case FM_EVENT_FOCUS:
		app->focused = event->focused;
		app->dirty = 1;
		break;
	case FM_EVENT_ACTION:
		fm_ui_action(app, event->action);
		break;
	case FM_EVENT_DROP_ENTER:
	case FM_EVENT_DROP_MOTION:
	case FM_EVENT_DROP_LEAVE:
	case FM_EVENT_DROP:
	case FM_EVENT_DROP_PART:
	case FM_EVENT_DROP_ACTION:
	case FM_EVENT_DRAG_DONE:
		/* A drag and drop from zdesktop (ui-drag.c). */
		fm_drop_event(app, event);
		break;
	case FM_EVENT_TEXT:
	case FM_EVENT_TEXT_DELETE:
	case FM_EVENT_PREEDIT:
		/* An input method's text, for the name being changed (rename.c). */
		(void)fm_rename_input(app, event);
		break;
	default:
		break;
	}
}

/*
 * Lets time pass: reloads a folder that changed on disk (checked every
 * two seconds).
 */
void
fm_ui_tick(
	struct fm_app *app,
	uint64_t now)
{
	struct fm_tab *tab;
	struct fm_visit *visit;
	struct stat status;
	int made;
	int error;

	/* The time now, which the lists' dates and the messages are measured by. */
	app->now = now;
	app->wall = time(NULL);

	/* The tasks and the search move on. */
	(void)fm_actions_tick(app);
	fm_search_tick(app);

	/* The path's field suggests folders once its typing rests (ws127-p010). */
	fm_location_tick(app);

	/* The thumbnail asked for is made, and shown in a new frame. */
	made = fm_thumb_tick(app);
	if (made != 0)
		app->dirty = 1;

	/* The information's checksum moves on. */
	fm_info_tick(app);

	/* A drag springs a folder open or scrolls at an edge (ws127-p002). */
	(void)fm_drag_tick(app, now);

	/* A message that has run its time goes. */
	if (app->message[0] != '\0' && now >= app->message_until) {
		app->message[0] = '\0';
		app->dirty = 1;
	}

	/* The tab shown, checked no more often than every two seconds. */
	tab = fm_ui_tab(app);
	if (now < tab->checked_at + 2000U)
		return;
	tab->checked_at = now;

	/* Only a folder is watched. */
	visit = &tab->history[tab->history_index];
	if (visit->location.kind != FM_LOCATION_FOLDER)
		return;

	/* A folder whose modification time moved is read again. */
	error = stat(visit->location.path, &status);
	if (error != 0 || status.st_mtime == tab->listing.modified)
		return;

	/* Reads it again. */
	fm_ui_reload(app, tab);
	app->dirty = 1;
}

/*
 * Draws the window's frame onto a canvas of the window's size.
 */
void
fm_ui_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct kl_rect whole;
	int partial;

	/*
	 * Only the damage is drawn when nothing else changed (BUG-221,
	 * BUG-226): every call draws, clipped to it, and the rest of the frame
	 * keeps the pixels of the last one.
	 */
	partial = 0;
	if (app->dirty == 0 && app->damage_pending != 0)
		partial = 1;
	app->damage_pending = 0;

	/* What the frame changes, for its showing (BUG-221): the damage, or all of it. */
	app->frame_partial = partial;
	memset(&app->frame_part, 0, sizeof(app->frame_part));
	if (partial != 0)
		app->frame_part = app->damage;

	/* The panels' places at this size, and no clickable region yet. */
	app->width = canvas->width;
	app->height = canvas->height;
	ui_layout(app);
	app->hit_count = 0;

	/* The window's ground: clear on glass (the desktop shows between the panels), else a quiet light gradient. */
	whole.x = 0;
	whole.y = 0;
	whole.width = canvas->width;
	whole.height = canvas->height;
	if (partial != 0)
		kl_canvas_clip_push(canvas, &app->damage);
	if (app->glass != 0 && partial != 0) {
		ui_clear_rect(canvas, &app->damage);
	} else if (app->glass != 0) {
		kl_canvas_clear(canvas);
	} else {
		kl_canvas_gradient(canvas, &whole, FM_COLOR_BACKGROUND_TOP, FM_COLOR_BACKGROUND_BOTTOM);
	}

	/* libkeiland's widgets' frame (ui-widgets.c), then the sidebar, when shown. */
	(void)fm_widgets_begin(app);
	if (app->show_sidebar != 0)
		ui_draw_sidebar(app, canvas);

	/* The content's card, drawn by the view of the place, the row of tabs at its top, and the preview beside it when shown. */
	fm_grid_draw(app, canvas, &app->layout.content);
	fm_scrollbar_draw(app, canvas);
	fm_tabs_draw(app, canvas);
	if (app->show_preview != 0)
		fm_preview_draw(app, canvas, &app->layout.preview);

	/* The tasks' list at the top right when open (the titlebar's progress control opens it). */
	if (app->show_tasks != 0 && app->task_count > 0)
		fm_tasks_draw(app, canvas, app->width - UI_MARGIN - 8, UI_MARGIN + 6);

	/* Quick Look, the information or Help over all of it, and a question over that. */
	fm_look_draw(app, canvas);
	fm_info_draw(app, canvas);
	fm_help_draw(app, canvas);
	fm_overlay_draw(app, canvas);

	/* Items being dragged, over everything; the widgets' frame ends. */
	fm_drag_draw(app, canvas);
	fm_widgets_end(app);
	if (partial != 0)
		kl_canvas_clip_pop(canvas);

	/* The frame is up to date. */
	app->dirty = 0;
}

/*
 * Adds a region of the window, with UI_DAMAGE_MARGIN round it (an edge or
 * a shadow), to the part the next frame draws alone, and asks for that
 * frame (BUG-221, BUG-226).
 */
void
fm_ui_damage(
	struct fm_app *app,
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
	if (app->damage_pending == 0) {
		app->damage = grown;
		app->damage_pending = 1;
		return;
	}

	/* Another widens it to hold both. */
	right = app->damage.x + app->damage.width;
	if (grown.x + grown.width > right)
		right = grown.x + grown.width;
	bottom = app->damage.y + app->damage.height;
	if (grown.y + grown.height > bottom)
		bottom = grown.y + grown.height;
	if (grown.x < app->damage.x)
		app->damage.x = grown.x;
	if (grown.y < app->damage.y)
		app->damage.y = grown.y;
	app->damage.width = right - app->damage.x;
	app->damage.height = bottom - app->damage.y;
}

/*
 * Adds a rectangle, with a margin round it, to what a frame drawn by parts
 * changed (BUG-221): the box around them all.
 */
void
fm_ui_frame_part(
	struct fm_app *app,
	const struct kl_rect *rect,
	int margin)
{
	struct kl_rect grown;
	int right;
	int bottom;

	/* The rectangle and its margin; an empty one adds nothing. */
	if (rect->width <= 0 || rect->height <= 0)
		return;
	grown.x = rect->x - margin;
	grown.y = rect->y - margin;
	grown.width = rect->width + 2 * margin;
	grown.height = rect->height + 2 * margin;

	/* The first one is the part. */
	if (app->frame_part.width <= 0 || app->frame_part.height <= 0) {
		app->frame_part = grown;
		return;
	}

	/* Another widens it to hold both. */
	right = app->frame_part.x + app->frame_part.width;
	if (grown.x + grown.width > right)
		right = grown.x + grown.width;
	bottom = app->frame_part.y + app->frame_part.height;
	if (grown.y + grown.height > bottom)
		bottom = grown.y + grown.height;
	if (grown.x < app->frame_part.x)
		app->frame_part.x = grown.x;
	if (grown.y < app->frame_part.y)
		app->frame_part.y = grown.y;
	app->frame_part.width = right - app->frame_part.x;
	app->frame_part.height = bottom - app->frame_part.y;
}

/*
 * Finds the rectangle of a region of the last frame by its kind and
 * index; returns 1 when it is there.
 */
int
fm_ui_hit_rect(
	const struct fm_app *app,
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
 * Lists the parts of the last frame that stand on zdesktop's glass: the
 * sidebar, the content's card (with its tabs) and the preview, into up to
 * capacity panels.  Returns how many there are.
 */
size_t
fm_ui_panels(
	struct fm_app *app,
	struct fm_panel *panels,
	size_t capacity)
{
	const struct kl_rect *cards[3];
	size_t count;
	unsigned index;

	/* The three cards of the layout, in the order they are drawn. */
	cards[0] = &app->layout.sidebar;
	cards[1] = &app->layout.card;
	cards[2] = &app->layout.preview;
	count = 0;

	/* Each card that is shown (a hidden one has no size). */
	for (index = 0; index < 3U; index++) {
		if (cards[index]->width <= 0 || cards[index]->height <= 0)
			continue;
		if (count == capacity)
			break;
		panels[count].rect = *cards[index];
		panels[count].radius = (int)UI_PANEL_RADIUS;
		panels[count].kind = FM_PANEL_CARD;
		count++;
	}

	/* The cards. */
	return count;
}

/*
 * Records a clickable region of the frame being drawn.
 */
void
fm_ui_hit(
	struct fm_app *app,
	const struct kl_rect *rect,
	unsigned kind,
	int index)
{
	/* Regions past the table's size are not clickable (they do not happen in practice). */
	if (app->hit_count == FM_HITS)
		return;

	/* The region, after those drawn before it (the later wins where they overlap). */
	app->hits[app->hit_count].rect = *rect;
	app->hits[app->hit_count].kind = kind;
	app->hits[app->hit_count].index = index;
	app->hit_count++;
}

/*
 * Returns the tab shown.
 */
struct fm_tab *
fm_ui_tab(
	struct fm_app *app)
{
	/* The current tab. */
	return app->tabs[app->tab_index];
}

/*
 * Goes to a place in the tab shown: the history keeps where the tab was,
 * and anything forward of it is dropped (as a browser does).
 */
void
fm_ui_go(
	struct fm_app *app,
	const struct fm_location *location)
{
	struct fm_tab *tab;
	struct fm_visit *visit;
	int home_folder;
	int root_folder;
	int index;

	/* The place being left keeps its scroll and its cursor. */
	tab = fm_ui_tab(app);
	if (tab->history_count > 0)
		ui_leave(tab);

	/* A full history drops its oldest step. */
	index = tab->history_index + 1;
	if (tab->history_count == 0)
		index = 0;
	if (index == FM_HISTORY) {
		memmove(&tab->history[0], &tab->history[1], sizeof(tab->history[0]) * (FM_HISTORY - 1));
		index = FM_HISTORY - 1;
	}

	/* The new step, and nothing forward of it. */
	visit = &tab->history[index];
	memset(visit, 0, sizeof(*visit));
	visit->location = *location;
	tab->history_index = index;
	tab->history_count = index + 1;

	/* A folder opened is kept among the recent folders (the home folder and the root are not worth it). */
	home_folder = strcmp(location->path, app->home);
	root_folder = strcmp(location->path, "/");
	if (location->kind == FM_LOCATION_FOLDER && home_folder != 0 && root_folder != 0)
		fm_home_folder_opened(location->path);

	/* The place's items, from the top, with nothing selected. */
	tab->scroll = 0;
	fm_dir_free(&tab->listing);
	fm_ui_reload(app, tab);
	app->focus = FM_FOCUS_CONTENT;
	app->band = 0;
	app->dirty = 1;
}

/*
 * Shows a short message in the status pill for a few seconds.
 */
void
fm_ui_message(
	struct fm_app *app,
	const char *message)
{
	/* The message, until three seconds from now. */
	snprintf(app->message, sizeof(app->message), "%s", message);
	app->message_until = app->now + 3000U;
	app->dirty = 1;
	fm_log("MESSAGE %s", message);
}

/*
 * Reports how long the main loop may sleep before the file manager has
 * work again: 0 while a thumbnail, an operation or a search is waiting to
 * move on, a short while when a search is about to start, and -1 when
 * nothing waits (only input wakes it).
 */
int
fm_ui_wait(
	struct fm_app *app)
{
	int busy;

	/* A thumbnail asked for, an operation, a search walking or a checksum: no sleep. */
	if (app->thumb_wanted[0] != '\0')
		return 0;

	/* A thumbnail's child running (ws168-p004): looked at again soon. */
	busy = fm_thumb_busy();
	if (busy != 0)
		return 20;
	if (app->task_count > 0)
		return 0;
	if (app->search.active != 0)
		return 0;
	if (app->info_open != 0 && app->info.checksum_state == FM_CHECKSUM_RUNNING)
		return 0;

	/* A search typed a moment ago starts soon. */
	if (app->search_typed_at != 0U)
		return 20;

	/* The path's field typed in a moment ago suggests folders soon (ws127-p010). */
	if (app->location_typed_at != 0U)
		return 50;

	/* A drag waits for a folder to spring open, or scrolls at an edge (ws127-p002). */
	if (app->drag_wait_ms >= 0 && (app->drag != 0 || app->drop_active != 0))
		return app->drag_wait_ms;

	/* A new removable device blinks: frames soon (ws132-p005). */
	busy = fm_devices_blinking(app);
	if (busy != 0) {
		app->dirty = 1;
		return 33;
	}

	/* The overlay scroll bar waits to fade, or fades: a frame soon. */
	busy = fm_scrollbar_busy(app);
	if (busy != 0) {
		app->dirty = 1;
		return 16;
	}

	/* Nothing waits. */
	return -1;
}

/*
 * Writes one line of the file manager's log on standard error: "ZFILES "
 * and the message.  The tests wait for these lines.
 */
void
fm_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The prefix, the message and the end of the line, at once. */
	fputs("ZFILES ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
}

/*
 * Fills the parts of the path shown in the titlebar and returns how many there are.
 */
int
fm_ui_crumbs(
	struct fm_app *app,
	struct fm_crumb *crumbs,
	int capacity)
{
	struct fm_tab *tab;
	const struct fm_location *location;
	const char *path;
	const char *part;
	const char *end;
	size_t home_length;
	size_t length;
	size_t done;
	int prefix;
	int count;
	int inside;

	/* A place that is not a folder is a single part. */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;
	if (location->kind != FM_LOCATION_FOLDER) {
		snprintf(crumbs[0].label, sizeof(crumbs[0].label), "%s", kl_tr(fm_location_name(location, app->home)));
		crumbs[0].location = *location;
		return 1;
	}

	/* A folder under the home folder starts from Home, any other from Computer. */
	path = location->path;
	home_length = strlen(app->home);
	inside = 0;
	prefix = strncmp(path, app->home, home_length);
	if (prefix == 0 &&
	    home_length > 1U &&
	    (path[home_length] == '/' ||
	     path[home_length] == '\0'))
		inside = 1;
	count = 1;
	crumbs[0].location.kind = FM_LOCATION_FOLDER;
	if (inside != 0) {
		snprintf(crumbs[0].label, sizeof(crumbs[0].label), "%s", kl_tr("Home"));
		snprintf(crumbs[0].location.path, sizeof(crumbs[0].location.path), "%s", app->home);
		done = home_length;
	} else {
		snprintf(crumbs[0].label, sizeof(crumbs[0].label), "%s", kl_tr("Computer"));
		snprintf(crumbs[0].location.path, sizeof(crumbs[0].location.path), "/");
		done = 0;
	}

	/* Each further part of the path, leading to the path up to it. */
	part = path + done;
	while (*part != '\0' && count < capacity) {
		/* Slashes between parts. */
		while (*part == '/')
			part++;
		if (*part == '\0')
			break;

		/* The part runs to the next slash. */
		end = strchr(part, '/');
		if (end == NULL)
			end = part + strlen(part);
		length = (size_t)(end - part);

		/* Its label and the path up to it. */
		if (length >= sizeof(crumbs[count].label))
			length = sizeof(crumbs[count].label) - 1U;
		memcpy(crumbs[count].label, part, length);
		crumbs[count].label[length] = '\0';
		crumbs[count].location.kind = FM_LOCATION_FOLDER;
		length = (size_t)(end - path);
		if (length >= sizeof(crumbs[count].location.path))
			length = sizeof(crumbs[count].location.path) - 1U;
		memcpy(crumbs[count].location.path, path, length);
		crumbs[count].location.path[length] = '\0';
		count++;
		part = end;
	}

	/* Reports how many parts there are. */
	return count;
}

/*
 * Reads the items of the place a tab shows, sorted, and logs the place.
 *
 * The items selected before (and the cursor) stay selected when they are
 * still there, so a folder read again after a change keeps its selection;
 * a place come back to by the history gets its cursor back.
 */
void
fm_ui_reload(
	struct fm_app *app,
	struct fm_tab *tab)
{
	const struct fm_location *location;
	struct fm_visit *visit;
	const char *path;
	char **kept;
	char *cursor_name;
	size_t kept_count;
	size_t index;
	int found;

	/* The place, and the folder its items come from. */
	visit = &tab->history[tab->history_index];
	location = &visit->location;
	path = NULL;
	if (location->kind == FM_LOCATION_FOLDER)
		path = location->path;

	/* The names of the selected items and of the cursor's, taken from the old listing. */
	kept_count = 0;
	kept = NULL;
	cursor_name = NULL;
	if (tab->listing.count != 0U)
		kept = calloc(tab->listing.count, sizeof(kept[0]));
	for (index = 0; kept != NULL && index < tab->listing.count; index++) {
		if (tab->listing.entries[index].selected == 0)
			continue;
		kept[kept_count] = tab->listing.entries[index].name;
		tab->listing.entries[index].name = NULL;
		kept_count++;
	}

	/* The cursor's name, taken the same way. */
	if (tab->cursor >= 0 &&
	    (size_t)tab->cursor < tab->listing.count &&
	    tab->listing.entries[tab->cursor].name != NULL) {
		cursor_name = tab->listing.entries[tab->cursor].name;
		tab->listing.entries[tab->cursor].name = NULL;
	}

	/* A folder's items, the trash's, or the items of a place that is not one folder. */
	fm_dir_free(&tab->listing);
	if (location->kind != FM_LOCATION_SEARCH)
		fm_search_stop(&app->search);
	if (path != NULL) {
		(void)fm_dir_read(&tab->listing, path, app->show_hidden);
	} else if (location->kind == FM_LOCATION_TRASH) {
		(void)fm_dir_read_trash(&tab->listing);
	} else if (location->kind == FM_LOCATION_TODAY) {
		fm_home_gather(app);
	} else {
		fm_search_load(app, tab);
	}

	/* The items in the window's order (the recent files stay newest first), the cut ones marked. */
	if (location->kind != FM_LOCATION_RECENTS)
		fm_dir_sort(&tab->listing, app->sort, app->sort_reverse);
	ui_mark_cut(tab);

	/* Nothing has the cursor yet, and the folder was just checked. */
	tab->cursor = -1;
	tab->anchor = -1;
	tab->checked_at = app->now;

	/* The kept names selected again where they are still there. */
	for (index = 0; index < kept_count; index++) {
		found = fm_select_find(tab, kept[index]);
		if (found >= 0)
			tab->listing.entries[found].selected = 1;
		free(kept[index]);
	}

	/* The names are not needed any more. */
	free(kept);

	/* The cursor on its item again. */
	if (cursor_name != NULL) {
		found = fm_select_find(tab, cursor_name);
		tab->cursor = found;
		tab->anchor = found;
		free(cursor_name);
	}

	/* A place come back to gets the cursor it was left with. */
	if (kept_count == 0U && tab->cursor < 0 && visit->cursor[0] != '\0') {
		found = fm_select_find(tab, visit->cursor);
		fm_select_only(tab, found);
	}

	/* What a finished operation made is selected instead, the first one with the cursor. */
	ui_select_paths(app, tab);

	/* The log line the tests wait for. */
	fm_log("LOCATION kind=%s path=%s items=%lu error=%d", ui_location_kind_name(location->kind), location->path, (unsigned long)tab->listing.count, tab->listing.error);
}

/*
 * Goes one step back in the tab's history.
 */
void
fm_ui_back(
	struct fm_app *app)
{
	struct fm_tab *tab;

	/* The first step has nothing behind it. */
	tab = fm_ui_tab(app);
	if (tab->history_index == 0)
		return;

	/* The step before, at the scroll it was left with. */
	ui_leave(tab);
	tab->history_index--;
	fm_dir_free(&tab->listing);
	fm_ui_reload(app, tab);
	tab->scroll = tab->history[tab->history_index].scroll;
	app->dirty = 1;
}

/*
 * Goes one step forward in the tab's history.
 */
void
fm_ui_forward(
	struct fm_app *app)
{
	struct fm_tab *tab;

	/* The last step has nothing after it. */
	tab = fm_ui_tab(app);
	if (tab->history_index + 1 >= tab->history_count)
		return;

	/* The step after, at the scroll it was left with. */
	ui_leave(tab);
	tab->history_index++;
	fm_dir_free(&tab->listing);
	fm_ui_reload(app, tab);
	tab->scroll = tab->history[tab->history_index].scroll;
	app->dirty = 1;
}

/*
 * Opens an item of the listing: a folder in the tab, a file with its
 * default application.
 */
void
fm_ui_open(
	struct fm_app *app,
	int index)
{
	struct fm_location location;
	struct fm_tab *tab;
	struct fm_entry *entry;

	/* The item. */
	tab = fm_ui_tab(app);
	if (index < 0 || (size_t)index >= tab->listing.count)
		return;
	entry = &tab->listing.entries[index];

	/* A folder opens in this tab. */
	if (entry->folder != 0) {
		memset(&location, 0, sizeof(location));
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", entry->path);
		fm_ui_go(app, &location);
		return;
	}

	/* A file opens with its default way. */
	fm_open_entry(app, index, 0);
}

/* Places the sidebar, the content and the preview for the window's size. */
static void
ui_layout(
	struct fm_app *app)
{
	struct fm_layout *layout;
	int margin;
	int gap;
	int top;
	int left;
	int right;
	int row;

	/* No margin around the panels (see UI_GLASS_GAP), and the gap between them: on glass, the titlebar's. */
	margin = 0;
	gap = UI_GAP;
	if (app->glass != 0)
		gap = UI_GLASS_GAP;

	/* The panels start at the top margin (the titlebar is zdesktop's, above the window). */
	layout = &app->layout;
	top = margin;
	left = margin;
	right = app->width - margin;

	/* The sidebar on the left, when shown. */
	memset(&layout->sidebar, 0, sizeof(layout->sidebar));
	if (app->show_sidebar != 0) {
		layout->sidebar.x = left;
		layout->sidebar.y = top;
		layout->sidebar.width = UI_SIDEBAR_WIDTH;
		layout->sidebar.height = app->height - top - margin;
		left += UI_SIDEBAR_WIDTH + gap;
	}

	/* The preview on the right, when shown. */
	memset(&layout->preview, 0, sizeof(layout->preview));
	if (app->show_preview != 0) {
		layout->preview.width = UI_PREVIEW_WIDTH;
		layout->preview.x = right - UI_PREVIEW_WIDTH;
		layout->preview.y = top;
		layout->preview.height = app->height - top - margin;
		right -= UI_PREVIEW_WIDTH + gap;
	}

	/* The content's card between the sidebar and the preview. */
	layout->card.x = left;
	layout->card.y = top;
	layout->card.width = right - left;
	layout->card.height = app->height - top - margin;

	/* The content in the card, under the row of tabs while the window has two tabs or more (ui-tabs.c). */
	layout->content = layout->card;
	row = fm_tabs_layout(app, top, left, right);
	layout->content.y += row;
	layout->content.height -= row;
}

/* Draws the sidebar: Favorites, Devices and Locations, the place shown lit. */
static void
ui_draw_sidebar(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	static const char *const titles[] = { "Favorites", "Locations", "Devices" };
	const struct fm_device *device;
	const char *title;
	struct kl_style style;
	struct kl_rect eject;
	float bright;
	const struct kl_rect *panel;
	const struct fm_place *place;
	struct kl_rect row;
	struct kl_rect remove;
	unsigned section;
	unsigned flags;
	int current;
	int removable;
	int hovered;
	int index;
	int y;

	/* The panel: libkeiland's sidebar panel (a light veil on glass, else with a bright edge, ws090-p023). */
	fm_style(app, canvas, &style);
	panel = &app->layout.sidebar;
	kl_panel(&style, panel, 1);

	/* The rows stay inside the panel. */
	kl_canvas_clip_push(canvas, panel);

	/* Each place under its section's title, the list scrolled when it is taller than the panel. */
	section = FM_PLACES;
	y = panel->y + 8 - app->sidebar_scroll;
	for (index = 0; index < app->places.count; index++) {
		place = &app->places.items[index];

		/* A new section starts with its title. */
		if (place->section != section) {
			section = place->section;
			if (index > 0)
				y += 8;
			title = kl_tr(titles[section]);
			(void)kl_sidebar_section(&style, panel->x + 8, y, panel->width - 16, title);
			row.x = panel->x + 8;
			row.y = y;
			row.width = panel->width - 16;
			row.height = UI_SIDEBAR_HEADER;
			fm_ui_hit(app, &row, FM_HIT_SECTION, (int)section);
			y += UI_SIDEBAR_HEADER;
		}

		/* The row's place, the place shown, and its device. */
		row.x = panel->x + 8;
		row.y = y;
		row.width = panel->width - 16;
		row.height = UI_SIDEBAR_ROW;
		current = ui_place_current(app, place);
		device = NULL;
		if (place->device > 0 && place->device <= app->places.device_count)
			device = &app->places.devices[place->device - 1];

		/* A new removable device blinks: its row lit and dimmed three times (ws132-p005). */
		bright = 1.0f;
		if (device != NULL)
			bright = fm_devices_blink(app, device);
		if (bright < 1.0f)
			kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 9.0f, KL_RGBA(FM_COLOR_ACCENT, (uint32_t)((1.0f - bright) * 110.0f)));

		/* libkeiland's place: faint when it is not there, quiet for a device not mounted until a double click mounts it. */
		flags = 0U;
		if (device != NULL && !device->mounted)
			flags = KL_PLACE_QUIET;
		if (place->missing != 0)
			flags = KL_PLACE_FAINT;
		(void)kl_sidebar_place(app->ui, &style, UI_WIDGET_PLACE, (uint32_t)index, &row, (enum kl_icon)place->icon, kl_tr(place->label), current, flags);
		fm_ui_hit(app, &row, FM_HIT_PLACE, index);

		/* A favorite folder under the pointer offers a small button that takes it off the sidebar. */
		removable = 0;
		removable = fm_place_is_favorite_folder(place);
		hovered = 0;
		if (app->hover_kind == FM_HIT_PLACE && app->hover_index == index)
			hovered = 1;
		if (app->hover_kind == FM_HIT_BUTTON && app->hover_index == FM_BUTTON_REMOVE_PLACE + index)
			hovered = 1;
		if (removable != 0 && hovered != 0) {
			remove.x = row.x + row.width - 26;
			remove.y = row.y + 5;
			remove.width = 20;
			remove.height = 20;
			fm_icon_button(app, canvas, FM_WIDGET_ICON, FM_BUTTON_REMOVE_PLACE + index, &remove, KL_ICON_CLOSE, 14, KL_BUTTON_ROUND | KL_BUTTON_QUIET);
			fm_ui_hit(app, &remove, FM_HIT_BUTTON, FM_BUTTON_REMOVE_PLACE + index);
		}

		/* A device's row is logged once after the list changed (the tests click it). */
		if (device != NULL && !app->device_rows_logged)
			fm_log("DEVICE row id=%s x=%d y=%d width=%d height=%d", device->id, row.x, row.y, row.width, row.height);

		/* A mounted device has an eject button at its right. */
		if (device != NULL && device->mounted) {
			eject.x = row.x + row.width - 26;
			eject.y = row.y + 5;
			eject.width = 20;
			eject.height = 20;
			fm_icon_button(app, canvas, FM_WIDGET_ICON, FM_BUTTON_EJECT_PLACE + index, &eject, KL_ICON_EJECT, 20, KL_BUTTON_ROUND | KL_BUTTON_QUIET);
			fm_ui_hit(app, &eject, FM_HIT_BUTTON, FM_BUTTON_EJECT_PLACE + index);
		}

		/* The next row. */
		y += UI_SIDEBAR_ROW;
	}

	/* The devices' rows are logged. */
	app->device_rows_logged = 1;

	/* How tall the list is, which the scrolling is kept within. */
	app->layout.sidebar_height = y + app->sidebar_scroll - panel->y + 8;

	/* The panel's clip ends. */
	kl_canvas_clip_pop(canvas);
}

/* Tells whether a sidebar place is the place shown. */
static int
ui_place_current(
	struct fm_app *app,
	const struct fm_place *place)
{
	const struct fm_location *location;
	struct fm_tab *tab;
	int match;

	/* The place shown. */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;

	/* Places of other kinds differ. */
	if (location->kind != place->location.kind)
		return 0;

	/* Today and the recent files are one place each. */
	if (location->kind == FM_LOCATION_TODAY || location->kind == FM_LOCATION_RECENTS || location->kind == FM_LOCATION_TRASH)
		return 1;

	/* Others are the same place when their paths are. */
	match = strcmp(location->path, place->location.path);
	if (match != 0)
		return 0;

	/* The same place. */
	return 1;
}

/* Names a kind of place for the log. */
static const char *
ui_location_kind_name(
	unsigned kind)
{
	/* Each kind's word. */
	switch (kind) {
	case FM_LOCATION_TODAY:
		return "today";
	case FM_LOCATION_FOLDER:
		return "folder";
	case FM_LOCATION_RECENTS:
		return "recents";
	case FM_LOCATION_TRASH:
		return "trash";
	case FM_LOCATION_SEARCH:
		return "search";
	default:
		break;
	}

	/* A kind this program does not know. */
	return "unknown";
}

/* Keeps, in the tab's current step, its scroll and the name of the item with the cursor. */
static void
ui_leave(
	struct fm_tab *tab)
{
	struct fm_visit *visit;

	/* The scroll. */
	visit = &tab->history[tab->history_index];
	visit->scroll = tab->scroll;

	/* The cursor's item by name (the items may be listed in another order when the tab comes back). */
	visit->cursor[0] = '\0';
	if (tab->cursor >= 0 && (size_t)tab->cursor < tab->listing.count)
		snprintf(visit->cursor, sizeof(visit->cursor), "%s", tab->listing.entries[tab->cursor].name);
}

/* Marks the items that are cut on the clipboard (they are drawn faded until the paste). */
static void
ui_mark_cut(
	struct fm_tab *tab)
{
	char **paths;
	unsigned mode;
	size_t count;
	size_t index;
	size_t cut;
	int error;
	int match;

	/* The clipboard; only a cut marks anything. */
	error = fm_clip_get(&mode, &paths, &count);
	if (error != 0 || mode != FM_CLIP_CUT) {
		fm_paths_free(paths, count);
		return;
	}

	/* Each item whose path is on it. */
	for (index = 0; index < tab->listing.count; index++) {
		for (cut = 0; cut < count; cut++) {
			match = strcmp(tab->listing.entries[index].path, paths[cut]);
			if (match == 0)
				tab->listing.entries[index].cut = 1;
		}
	}

	/* The clipboard's paths are not needed any more. */
	fm_paths_free(paths, count);
}

/* Selects the items of the paths a finished operation left (and drops them). */
static void
ui_select_paths(
	struct fm_app *app,
	struct fm_tab *tab)
{
	size_t index;
	size_t wanted;
	int first;
	int match;

	/* Nothing waits to be selected. */
	if (app->select_count == 0U)
		return;

	/* The items of those paths, and nothing else. */
	first = -1;
	fm_select_none(tab);
	for (index = 0; index < tab->listing.count; index++) {
		for (wanted = 0; wanted < app->select_count; wanted++) {
			match = strcmp(tab->listing.entries[index].path, app->select_paths[wanted]);
			if (match != 0)
				continue;
			tab->listing.entries[index].selected = 1;
			if (first < 0)
				first = (int)index;
		}
	}

	/* The first has the cursor. */
	tab->cursor = first;
	tab->anchor = first;

	/* The paths were used. */
	fm_paths_free(app->select_paths, app->select_count);
	app->select_paths = NULL;
	app->select_count = 0;
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
