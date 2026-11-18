/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dragging the selected items within the window (spec §14, design §10.4):
 * a press on an item that moves more than a few pixels drags the whole
 * selection.  Under the pointer a folder among the items, a place of the
 * sidebar or a tab is the target; the release moves the items there (on the
 * same device, else copies them), Ctrl copies, Ctrl+Shift makes links;
 * the Trash throws them away, and the Favorites'
 * title adds the dragged folders to the Favorites.  A favorite folder
 * dragged onto another one moves to its place in the list.  Esc gives up.
 *
 * Items dragged out of the window go on as a drag and drop of the compositor
 * (ws035-p084, dnd.c): their file names travel to another window (of this
 * program or another) or back to this one, where the drop comes in as the
 * drop events.  A drop coming in has a folder as its target: a folder among
 * the items, a folder of the sidebar, another tab's folder, a part of the
 * titlebar's path, or else the folder shown; its action (move or copy) is
 * the compositor's choice, made a copy across devices.
 */

#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* How far the pointer moves from the press before the items are dragged, in pixels. */
#define DRAG_START		6

/* The size of the dragged icon, and where it is from the pointer, in pixels. */
#define DRAG_ICON		52
#define DRAG_OFFSET_X		2
#define DRAG_OFFSET_Y		14

/*
 * Spring-loaded folders (ws127-p002, F-039): how long a drag rests on a
 * folder before it opens, in milliseconds.
 */
#define DRAG_SPRING_MS		750U

/*
 * The scroll at the content's edges under a drag: how near the top or the
 * bottom edge the pointer starts it, in pixels, and its speed there, in
 * pixels a millisecond, slow at the band's inner side and fast at the edge.
 */
#define DRAG_EDGE		44
#define DRAG_EDGE_SLOW		0.08
#define DRAG_EDGE_FAST		1.2

/* The radius of the badges on the dragged icon, in pixels. */
#define DRAG_BADGE		10

/* The badges' colors: the count's, a copy's and a link's. */
#define DRAG_COLOR_COUNT	FM_COLOR_ACCENT
#define DRAG_COLOR_COPY		KL_RGB(0x2fb45a)
#define DRAG_COLOR_LINK		KL_RGB(0x6b7585)
#define DRAG_COLOR_BADGE_TEXT	KL_RGB(0xffffff)
#define DRAG_COLOR_COUNT_TEXT	FM_COLOR_ACCENT_INK

static void drag_start(struct fm_app *app);
static void drag_start_place(struct fm_app *app);
static void drag_find(struct fm_app *app, int x, int y);
static void drag_find_place(struct fm_app *app, int x, int y);
static void drag_target(struct fm_app *app, unsigned target, unsigned kind, int index, const char *folder);
static int drag_favorite(const struct fm_app *app, int index);
static unsigned drag_operation(struct fm_app *app);
static const char *drag_first(struct fm_app *app);
static const char *drag_verb(unsigned operation);
static void drag_drop_folder(struct fm_app *app);
static void drag_drop_favorites(struct fm_app *app);
static void drag_drop_reorder(struct fm_app *app);
static void drag_end(struct fm_app *app);
static void drag_draw_target(struct fm_app *app, struct kl_canvas *canvas);
static void drag_draw_badges(struct fm_app *app, struct kl_canvas *canvas, float x, float y);
static void drag_draw_place(struct fm_app *app, struct kl_canvas *canvas);
static void drag_go_out(struct fm_app *app);
static void drop_find(struct fm_app *app);
static int drag_spring_tick(struct fm_app *app, uint64_t now);
static int drag_spring_tab(const struct fm_app *app);
static int drag_edge_tick(struct fm_app *app, uint64_t now);
static int drag_pointer(const struct fm_app *app, int *x, int *y);
static void drag_refind(struct fm_app *app);

/*
 * Follows the pointer while the left button holds an item: the drag starts
 * once the pointer is far enough from the press, and then its target is
 * the region under the pointer.  Returns nonzero while the items are being
 * dragged.
 */
int
fm_drag_motion(
	struct fm_app *app,
	int x,
	int y)
{
	int dx;
	int dy;

	/* A drag carried by the compositor is not the window's to follow. */
	if (app->drag != 0 && app->drag_outside != 0)
		return 1;

	/* Items dragged out of the window go on as the compositor's drag and drop. */
	if (app->drag != 0 && app->drag_place < 0 && (x < 0 || y < 0 || x >= app->width || y >= app->height)) {
		drag_go_out(app);
		return 1;
	}

	/* A drag in progress follows the pointer. */
	if (app->drag != 0) {
		if (app->drag_place >= 0) {
			drag_find_place(app, x, y);
		} else {
			drag_find(app, x, y);
		}

		/* A new frame shows it. */
		app->dirty = 1;
		return 1;
	}

	/* Not yet far enough from the press. */
	dx = x - app->press_x;
	dy = y - app->press_y;
	if (dx * dx + dy * dy <= DRAG_START * DRAG_START)
		return 0;

	/* Movement consumes a possible double click even if the drag cannot start. */
	app->press_open = 0;
	app->click_time = 0;

	/* A favorite is dragged within the sidebar from here on. */
	if (app->press_kind == FM_HIT_PLACE) {
		drag_start_place(app);
		if (app->drag == 0)
			return 0;
		drag_find_place(app, x, y);
		app->dirty = 1;
		return 1;
	}

	/* Otherwise the selection is. */
	drag_start(app);
	if (app->drag == 0)
		return 0;
	drag_find(app, x, y);
	app->dirty = 1;
	return 1;
}

/*
 * Ends a drag at the release of the button: the items go to the target, if
 * there is one.  Returns nonzero when a drag ended (the release then does
 * nothing else).
 */
int
fm_drag_release(
	struct fm_app *app)
{
	/* No drag: an ordinary release. */
	if (app->drag == 0)
		return 0;

	/* What the target does with the items. */
	switch (app->drag_target) {
	case FM_DRAG_FOLDER:
		drag_drop_folder(app);
		break;
	case FM_DRAG_TRASH:
		fm_log("DRAG drop operation=trash items=%lu", (unsigned long)app->drag_count);
		fm_action_trash(app);
		break;
	case FM_DRAG_FAVORITES:
		drag_drop_favorites(app);
		break;
	case FM_DRAG_REORDER:
		drag_drop_reorder(app);
		break;
	default:
		fm_log("DRAG drop operation=none");
		break;
	}

	/* The drag is over. */
	drag_end(app);
	return 1;
}

/*
 * Gives up a drag (Esc): nothing moves, and the press is over.
 */
void
fm_drag_cancel(
	struct fm_app *app)
{
	/* Only a drag in progress. */
	if (app->drag == 0)
		return;

	/* Said in the log, and ended. */
	fm_log("DRAG cancel");
	drag_end(app);
	app->pressing = 0;
}

/*
 * Draws a drag over the frame: the target lit, and the dragged items'
 * icon under the pointer with their count and what the drop will do.
 */
void
fm_drag_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_tab *tab;
	const struct fm_entry *entry;
	float x;
	float y;

	/* A drop coming in lights its target alone (the compositor draws what is carried). */
	if (app->drop_active != 0) {
		drag_draw_target(app, canvas);
		return;
	}

	/* Only while dragging within the window. */
	if (app->drag == 0 || app->drag_outside != 0)
		return;

	/* A favorite: its own look. */
	if (app->drag_place >= 0) {
		drag_draw_place(app, canvas);
		return;
	}

	/* The target. */
	drag_draw_target(app, canvas);

	/* The pressed item stands for them all (the first selected one if it went). */
	tab = fm_ui_tab(app);
	entry = NULL;
	if (app->press_index >= 0 && (size_t)app->press_index < tab->listing.count)
		entry = &tab->listing.entries[app->press_index];
	if (entry == NULL)
		return;

	/* Its icon below and right of the pointer (the target's name stays in sight), on a soft shadow. */
	x = (float)(app->pointer_x + DRAG_OFFSET_X);
	y = (float)(app->pointer_y + DRAG_OFFSET_Y);
	kl_canvas_shadow(canvas, x + 4.0f, y + 6.0f, DRAG_ICON - 8.0f, DRAG_ICON - 8.0f, 8.0f, 10.0f, FM_COLOR_SHADOW);
	fm_grid_entry_icon(app, canvas, entry, x, y, (float)DRAG_ICON);

	/* The count and the operation. */
	drag_draw_badges(app, canvas, x, y);
}

/*
 * Follows a drag and drop from the compositor: it comes over the window, moves,
 * is over a part of the titlebar's path, leaves, or is dropped; the action
 * the compositor chose; the end of the window's own drag that left it.
 */
void
fm_drop_event(
	struct fm_app *app,
	const struct fm_event *event)
{
	/* Each event. */
	switch (event->type) {
	case FM_EVENT_DROP_ENTER:
		/* A drag comes over the window: whose it is and what it carries. */
		app->drop_active = 1;
		app->drop_self = 0;
		if (event->pressed != 0 && app->drag_outside != 0)
			app->drop_self = 1;
		app->drop_files = event->focused;
		app->drop_x = event->x;
		app->drop_y = event->y;
		app->drag_target = FM_DRAG_NONE;
		app->drag_hit_kind = FM_HIT_NONE;
		app->drag_hit_index = -1;
		fm_log("DROP enter self=%d files=%d x=%d y=%d", app->drop_self, app->drop_files, event->x, event->y);
		drop_find(app);
		break;
	case FM_EVENT_DROP_MOTION:
		/* It moves over the window. */
		app->drop_x = event->x;
		app->drop_y = event->y;
		drop_find(app);
		break;
	case FM_EVENT_DROP_PART:
		/* It is over a part of the titlebar's path, or none. */
		app->drop_part = -1;
		if (event->action == FM_CONTROL_PATH)
			app->drop_part = (int)event->button;
		if (app->drop_active != 0)
			drop_find(app);
		break;
	case FM_EVENT_DROP_ACTION:
		/* The compositor's choice of move or copy. */
		app->drop_action = event->action;
		break;
	case FM_EVENT_DROP_LEAVE:
		/* It went elsewhere. */
		app->drop_active = 0;
		app->drag_target = FM_DRAG_NONE;
		app->drag_hit_kind = FM_HIT_NONE;
		app->drag_hit_index = -1;
		fm_log("DROP leave");
		break;
	case FM_EVENT_DROP:
		/* Dropped: a folder target's folder and operation go to the Wayland side, which fetches the names. */
		app->drop_active = 0;
		if (app->drag_target == FM_DRAG_FOLDER) {
			snprintf(app->drop_folder, sizeof(app->drop_folder), "%s", app->drag_folder);
			app->drop_operation = FM_TASK_MOVE;
			if (app->drop_action == FM_DND_COPY)
				app->drop_operation = FM_TASK_COPY;
			app->request = FM_REQUEST_DROP;
			fm_log("DROP drop self=%d destination=%s action=%u", app->drop_self, app->drop_folder, app->drop_action);

			/* "Ask": the choice in a context menu at the drop's place first. */
			if (app->drop_action == FM_DND_ASK) {
				app->request = FM_REQUEST_DROP_ASK;
				app->context_where = FM_CONTEXT_DROP;
				app->context_x = app->drop_x;
				app->context_y = app->drop_y;
				app->drop_asking = 1;
			}
		} else {
			fm_log("DROP drop target=none");
		}

		/* The window has no target any more. */
		app->drag_target = FM_DRAG_NONE;
		app->drag_hit_kind = FM_HIT_NONE;
		app->drag_hit_index = -1;
		break;
	case FM_EVENT_DRAG_DONE:
		/* The window's own drag that left it is over (dropped somewhere, or cancelled). */
		if (app->drag_outside != 0) {
			fm_log("DRAG out done dropped=%d", event->pressed);
			app->drag_outside = 0;
			drag_end(app);
			app->pressing = 0;
		}

		/* Nothing else to do. */
		break;
	default:
		break;
	}

	/* A new frame shows it. */
	app->dirty = 1;
}

/*
 * Tells whether a drop coming in has a target the window takes (a folder).
 */
int
fm_drop_accepts(
	const struct fm_app *app)
{
	/* Only a folder, while a drop is over the window. */
	if (app->drop_active == 0 || app->drag_target != FM_DRAG_FOLDER)
		return 0;

	/* It is taken. */
	return 1;
}

/*
 * Carries out a drop made: the dropped paths move (or are copied) into its
 * folder; a move across devices is a copy.
 */
void
fm_drop_perform(
	struct fm_app *app,
	char *const *paths,
	size_t count)
{
	struct stat target;
	struct stat source;
	unsigned operation;
	int error;

	/* Nothing dropped. */
	if (count == 0)
		return;

	/* A move to another device copies (as a drag within the window does). */
	operation = app->drop_operation;
	if (operation == FM_TASK_MOVE) {
		error = stat(app->drop_folder, &target);
		if (error == 0)
			error = lstat(paths[0], &source);
		if (error == 0 && target.st_dev != source.st_dev)
			operation = FM_TASK_COPY;
	}

	/* Logged, and the task started. */
	fm_log("DROP operation=%s items=%lu destination=%s first=%s", drag_verb(operation), (unsigned long)count, app->drop_folder, paths[0]);
	(void)fm_action_transfer(app, operation, paths, count, app->drop_folder);
}


/*
 * Moves a drag on with time, at the time now of fm_ui_tick (ws127-p002,
 * F-039): a folder the drag rested on for DRAG_SPRING_MS opens
 * (spring-loaded), another tab it rested on comes to the front (ws127-p006),
 * and the pointer near the content's top or bottom edge
 * scrolls it, faster nearer the edge.  Works for the window's own drag and
 * for a drop coming in.  Returns how many milliseconds to the next step
 * (the loop wakes then), or -1 when nothing waits; the same is kept in
 * app->drag_wait_ms for fm_ui_wait.
 */
int
fm_drag_tick(
	struct fm_app *app,
	uint64_t now)
{
	int spring;
	int edge;

	/* Only while something is dragged over the window. */
	app->drag_wait_ms = -1;
	if ((app->drag == 0 || app->drag_outside != 0 || app->drag_place >= 0) && app->drop_active == 0) {
		app->spring_ms = 0U;
		app->edge_ms = 0U;
		return -1;
	}

	/* The two steps on one clock; the sooner one sets the wait. */
	edge = drag_edge_tick(app, now);
	spring = drag_spring_tick(app, now);
	app->drag_wait_ms = spring;
	if (edge >= 0 && (spring < 0 || edge < spring))
		app->drag_wait_ms = edge;

	/* Reports the wait to the next step. */
	return app->drag_wait_ms;
}

/* Starts dragging the selection, when there is one. */
static void
drag_start(
	struct fm_app *app)
{
	struct fm_tab *tab;
	const char *shown;
	uint64_t bytes;
	size_t count;
	int error;

	/* Only a selection is dragged. */
	tab = fm_ui_tab(app);
	count = fm_select_count(tab, &bytes);
	if (count == 0)
		return;

	/* The drag, with no target yet; the selection stays as it is. */
	app->drag = 1;
	app->drag_count = count;
	app->drag_place = -1;
	app->drag_target = FM_DRAG_NONE;
	app->drag_hit_kind = FM_HIT_NONE;
	app->drag_hit_index = -1;
	app->drag_folder[0] = '\0';
	app->press_deferred = 0;
	app->band = 0;

	/* The paths, kept for a drop after a folder sprang open. */
	error = fm_selected_paths(app, &app->drag_paths, &app->drag_path_count);
	if (error != 0) {
		app->drag_paths = NULL;
		app->drag_path_count = 0;
	}

	/* The folder they came from, whose items are no target. */
	app->drag_source[0] = '\0';
	shown = fm_current_folder(app);
	if (shown != NULL)
		snprintf(app->drag_source, sizeof(app->drag_source), "%s", shown);
	fm_log("DRAG start items=%lu", (unsigned long)count);
}

/* Starts dragging a favorite folder of the sidebar (the others stay). */
static void
drag_start_place(
	struct fm_app *app)
{
	int favorite;

	/* Only a favorite folder. */
	favorite = drag_favorite(app, app->press_index);
	if (favorite == 0)
		return;

	/* The drag, with no target yet; the press no longer goes to the place. */
	app->drag = 1;
	app->drag_count = 0;
	app->drag_place = app->press_index;
	app->drag_target = FM_DRAG_NONE;
	app->drag_hit_kind = FM_HIT_NONE;
	app->drag_hit_index = -1;
	app->drag_folder[0] = '\0';
	app->press_deferred = 0;
	fm_log("DRAG start place=%d path=%s", app->press_index, app->places.items[app->press_index].location.path);
}

/* Finds the target under a point: a folder, the Trash or the Favorites' title; logged when it changes. */
static void
drag_find(
	struct fm_app *app,
	int x,
	int y)
{
	const struct fm_location *location;
	const struct fm_entry *entry;
	const struct fm_place *place;
	struct fm_tab *tab;
	const char *shown;
	unsigned target;
	unsigned kind;
	int index;
	int same;
	size_t entry_index;
	char folder[FM_PATH_MAX];

	/* The region under the point, and no target yet. */
	(void)fm_input_hit_at(app, x, y, &kind, &index);
	tab = fm_ui_tab(app);
	target = FM_DRAG_NONE;
	folder[0] = '\0';

	/* What each region stands for. */
	switch (kind) {
	case FM_HIT_ITEM:
		/* A folder among the items that is not itself dragged. */
		if (index < 0 || (size_t)index >= tab->listing.count)
			break;
		entry = &tab->listing.entries[index];
		if (entry->folder != 0 && entry->selected == 0) {
			target = FM_DRAG_FOLDER;
			snprintf(folder, sizeof(folder), "%s", entry->path);
		}

		break;
	case FM_HIT_PLACE:
		/* A place of the sidebar: a folder that is there, or the Trash. */
		if (index < 0 || index >= app->places.count)
			break;
		place = &app->places.items[index];
		if (place->missing != 0)
			break;
		if (place->location.kind == FM_LOCATION_FOLDER) {
			target = FM_DRAG_FOLDER;
			snprintf(folder, sizeof(folder), "%s", place->location.path);
		} else if (place->location.kind == FM_LOCATION_TRASH) {
			if (tab->history[tab->history_index].location.kind != FM_LOCATION_TRASH)
				target = FM_DRAG_TRASH;
		}

		break;
	case FM_HIT_TAB:
	case FM_HIT_TAB_CLOSE:
		/* Another tab's folder. */
		if (index < 0 || index >= app->tab_count || index == app->tab_index)
			break;
		location = &app->tabs[index]->history[app->tabs[index]->history_index].location;
		if (location->kind == FM_LOCATION_FOLDER) {
			target = FM_DRAG_FOLDER;
			snprintf(folder, sizeof(folder), "%s", location->path);
		}

		break;
	case FM_HIT_SECTION:
		/* The Favorites' title takes the dragged folders (the other items are left out). */
		if (index != FM_SECTION_FAVORITES)
			break;
		for (entry_index = 0; entry_index < tab->listing.count; entry_index++) {
			entry = &tab->listing.entries[entry_index];
			if (entry->selected != 0 && entry->folder != 0) {
				target = FM_DRAG_FAVORITES;
				break;
			}
		}

		break;
	default:
		break;
	}

	/*
	 * The content's empty part of a folder that sprang open takes the
	 * items (the folder shown is another than the one they came from).
	 */
	shown = fm_current_folder(app);
	if (target == FM_DRAG_NONE &&
	    (kind == FM_HIT_CONTENT || kind == FM_HIT_NONE) &&
	    app->drag != 0 &&
	    shown != NULL &&
	    app->drag_source[0] != '\0') {
		same = strcmp(app->drag_source, shown);
		if (same != 0 &&
		    x >= app->layout.content.x && x < app->layout.content.x + app->layout.content.width &&
		    y >= app->layout.content.y && y < app->layout.content.y + app->layout.content.height) {
			target = FM_DRAG_FOLDER;
			snprintf(folder, sizeof(folder), "%s", shown);
		}
	}

	/* The folder the items are in already is no target (the one they came from, when a folder sprang open). */
	if (app->drag != 0 && app->drag_source[0] != '\0')
		shown = app->drag_source;
	if (target == FM_DRAG_FOLDER && shown != NULL) {
		same = strcmp(folder, shown);
		if (same == 0)
			target = FM_DRAG_NONE;
	}

	/* The target. */
	drag_target(app, target, kind, index, folder);
}

/* Finds the target of a dragged favorite under a point: another favorite folder. */
static void
drag_find_place(
	struct fm_app *app,
	int x,
	int y)
{
	unsigned target;
	unsigned kind;
	int index;
	int favorite;

	/* The region under the point. */
	(void)fm_input_hit_at(app, x, y, &kind, &index);

	/* Another favorite folder is the target. */
	target = FM_DRAG_NONE;
	if (kind == FM_HIT_PLACE && index != app->drag_place) {
		favorite = drag_favorite(app, index);
		if (favorite != 0)
			target = FM_DRAG_REORDER;
	}

	/* The target. */
	drag_target(app, target, kind, index, "");
}

/* Keeps a new target and logs it; an unchanged one is left alone. */
static void
drag_target(
	struct fm_app *app,
	unsigned target,
	unsigned kind,
	int index,
	const char *folder)
{
	int spring_tab;

	/* Unchanged: nothing more to do. */
	if (target == app->drag_target && kind == app->drag_hit_kind && index == app->drag_hit_index)
		return;

	/* The new target. */
	app->drag_target = target;
	app->drag_hit_kind = kind;
	app->drag_hit_index = index;
	snprintf(app->drag_folder, sizeof(app->drag_folder), "%s", folder);

	/* A folder among the items or of the sidebar springs open if the drag rests on it. */
	app->spring_ms = 0U;
	if (target == FM_DRAG_FOLDER && (kind == FM_HIT_ITEM || kind == FM_HIT_PLACE))
		app->spring_ms = app->now;

	/* Another tab, whatever it shows, comes to the front if the drag rests on it (ws127-p006). */
	spring_tab = drag_spring_tab(app);
	if (spring_tab >= 0)
		app->spring_ms = app->now;

	/* Logged by its kind. */
	switch (target) {
	case FM_DRAG_FOLDER:
		fm_log("DRAG target kind=folder path=%s", folder);
		break;
	case FM_DRAG_TRASH:
		fm_log("DRAG target kind=trash");
		break;
	case FM_DRAG_FAVORITES:
		fm_log("DRAG target kind=favorites");
		break;
	case FM_DRAG_REORDER:
		fm_log("DRAG target kind=place place=%d", index);
		break;
	default:
		fm_log("DRAG target kind=none");
		break;
	}
}

/* Tells whether a place is a favorite folder (Home and the other sections are not). */
static int
drag_favorite(
	const struct fm_app *app,
	int index)
{
	const struct fm_place *place;
	int favorite;

	/* A place that is there. */
	if (index < 0 || index >= app->places.count)
		return 0;

	/* One of the user's favorite folders (Today and Home are fixed). */
	place = &app->places.items[index];
	favorite = fm_place_is_favorite_folder(place);
	if (favorite == 0)
		return 0;

	/* A favorite folder. */
	return 1;
}

/*
 * Works out what a drop on a folder does: Ctrl+Shift links, Ctrl copies,
 * and otherwise the items move within their device and are copied to
 * another one.  Returns an FM_TASK_* kind.
 */
static unsigned
drag_operation(
	struct fm_app *app)
{
	struct stat target;
	struct stat source;
	const char *first;
	int error;

	/* The keys held decide first. */
	if ((app->modifiers & (FM_MOD_CTRL | FM_MOD_SHIFT)) == (FM_MOD_CTRL | FM_MOD_SHIFT))
		return FM_TASK_LINK;
	if ((app->modifiers & FM_MOD_CTRL) != 0U)
		return FM_TASK_COPY;

	/* The target folder's device. */
	error = stat(app->drag_folder, &target);
	if (error != 0)
		return FM_TASK_MOVE;

	/* The first item's device. */
	first = drag_first(app);
	if (first == NULL)
		return FM_TASK_MOVE;
	error = lstat(first, &source);
	if (error != 0)
		return FM_TASK_MOVE;

	/* Another device: a copy. */
	if (target.st_dev != source.st_dev)
		return FM_TASK_COPY;

	/* The same device: a move. */
	return FM_TASK_MOVE;
}

/* Returns the first selected item's path, or NULL. */
static const char *
drag_first(
	struct fm_app *app)
{
	struct fm_tab *tab;
	size_t index;

	/* The first selected entry. */
	tab = fm_ui_tab(app);
	for (index = 0; index < tab->listing.count; index++) {
		if (tab->listing.entries[index].selected != 0)
			return tab->listing.entries[index].path;
	}

	/* None is selected. */
	return NULL;
}

/* Returns the log's word for a drop's operation. */
static const char *
drag_verb(
	unsigned operation)
{
	/* The three a folder takes. */
	if (operation == FM_TASK_LINK)
		return "link";
	if (operation == FM_TASK_COPY)
		return "copy";

	/* A move. */
	return "move";
}

/* Drops the items on a folder: a move, copy or link task into it. */
static void
drag_drop_folder(
	struct fm_app *app)
{
	unsigned operation;
	char **paths;
	size_t count;
	int error;

	/* The operation, and the paths the drag started with (the selection's, before any folder sprang open). */
	operation = drag_operation(app);
	if (app->drag_paths != NULL && app->drag_path_count > 0) {
		fm_log("DRAG drop operation=%s items=%lu destination=%s", drag_verb(operation), (unsigned long)app->drag_path_count, app->drag_folder);
		(void)fm_action_transfer(app, operation, app->drag_paths, app->drag_path_count, app->drag_folder);
		return;
	}

	/* Otherwise the selection's paths. */
	error = fm_selected_paths(app, &paths, &count);
	if (error != 0 || count == 0) {
		fm_paths_free(paths, count);
		return;
	}

	/* Logged, and the task started. */
	fm_log("DRAG drop operation=%s items=%lu destination=%s", drag_verb(operation), (unsigned long)count, app->drag_folder);
	(void)fm_action_transfer(app, operation, paths, count, app->drag_folder);
	fm_paths_free(paths, count);
}

/* Drops folders on the Favorites' title: each dragged folder that is not there yet is added. */
static void
drag_drop_favorites(
	struct fm_app *app)
{
	const struct fm_entry *entry;
	struct fm_tab *tab;
	size_t index;
	int added;
	int error;
	char message[64];

	/* Each dragged folder. */
	tab = fm_ui_tab(app);
	added = 0;
	for (index = 0; index < tab->listing.count; index++) {
		entry = &tab->listing.entries[index];
		if (entry->selected == 0 || entry->folder == 0)
			continue;
		error = fm_places_add_favorite(&app->places, entry->path);
		if (error != 0)
			continue;

		/* The sidebar with it (the next folder is added after it). */
		fm_places_init(&app->places, app->home);
		fm_log("FAVORITE add path=%s", entry->path);
		added++;
	}

	/* Said and logged. */
	fm_log("DRAG drop operation=favorites added=%d", added);
	if (added == 0) {
		fm_ui_message(app, "It is already in the sidebar");
		return;
	}

	/* How many were added. */
	snprintf(message, sizeof(message), "Added %d to the sidebar", added);
	fm_ui_message(app, message);
}

/* Drops a favorite on another: it moves to that one's place in the list. */
static void
drag_drop_reorder(
	struct fm_app *app)
{
	int moved;
	int error;

	/* The list written in the new order, and the sidebar filled again. */
	moved = app->drag_place;
	error = fm_places_move_favorite(&app->places, moved, app->drag_hit_index);
	fm_log("DRAG drop operation=reorder place=%d to=%d error=%d", moved, app->drag_hit_index, error);
	if (error != 0) {
		fm_ui_message(app, "Couldn't change the sidebar");
		return;
	}

	/* The sidebar in its new order. */
	fm_places_init(&app->places, app->home);
}

/* Ends a drag: no target, no ghost. */
static void
drag_end(
	struct fm_app *app)
{
	/* Nothing is dragged any more. */
	app->drag = 0;
	app->drag_count = 0;
	app->drag_place = -1;
	app->drag_target = FM_DRAG_NONE;
	app->drag_hit_kind = FM_HIT_NONE;
	app->drag_hit_index = -1;
	app->drag_folder[0] = '\0';
	app->press_deferred = 0;
	app->dirty = 1;

	/* The kept paths, the source, the spring and the edge's scroll go with the drag. */
	fm_paths_free(app->drag_paths, app->drag_path_count);
	app->drag_paths = NULL;
	app->drag_path_count = 0;
	app->drag_source[0] = '\0';
	app->spring_ms = 0U;
	app->edge_ms = 0U;
}

/* Lights the target's region: a tint and a blue edge. */
static void
drag_draw_target(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	const struct kl_rect *rect;
	int hit;

	/* Only a target. */
	if (app->drag_target == FM_DRAG_NONE)
		return;

	/* A drop into the folder shown lights the content's edge. */
	if (app->drop_active != 0 && app->drag_hit_kind == FM_HIT_NONE && app->drag_hit_index == -1) {
		rect = &app->layout.content;
		kl_canvas_round_border(canvas, (float)rect->x + 2.0f, (float)rect->y + 2.0f, (float)rect->width - 4.0f, (float)rect->height - 4.0f, 16.0f, 2.0f, FM_COLOR_ACCENT);
		return;
	}

	/* The last region of the frame that is the target's (the last drawn is the one under the pointer). */
	rect = NULL;
	for (hit = app->hit_count - 1; hit >= 0; hit--) {
		if (app->hits[hit].kind == app->drag_hit_kind && app->hits[hit].index == app->drag_hit_index) {
			rect = &app->hits[hit].rect;
			break;
		}
	}

	/* None was drawn (it scrolled away). */
	if (rect == NULL)
		return;

	/* The tint and the edge. */
	kl_canvas_round(canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, 8.0f, FM_COLOR_SELECTION);
	kl_canvas_round_border(canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, 8.0f, 2.0f, FM_COLOR_ACCENT);
}

/* Draws the dragged icon's badges: how many items (more than one), and a plus for a copy or an arrow for a link. */
static void
drag_draw_badges(
	struct fm_app *app,
	struct kl_canvas *canvas,
	float x,
	float y)
{
	unsigned operation;
	char count[24];
	float cx;
	float cy;
	int width;
	int baseline;

	/* The count at the top right. */
	if (app->drag_count > 1U) {
		cx = x + (float)DRAG_ICON - 2.0f;
		cy = y + 2.0f;
		snprintf(count, sizeof(count), "%lu", (unsigned long)app->drag_count);
		kl_canvas_circle(canvas, cx, cy, (float)DRAG_BADGE, DRAG_COLOR_COUNT);
		width = kl_text_width(app->text, count, strlen(count), 11U, 1);
		baseline = kl_text_center(11U, (int)cy - DRAG_BADGE, 2 * DRAG_BADGE);
		(void)kl_text_draw(app->text, canvas, (int)cx - width / 2, baseline, count, strlen(count), 11U, 1, DRAG_COLOR_COUNT_TEXT);
	}

	/* A folder target's operation; a move has no badge. */
	if (app->drag_target != FM_DRAG_FOLDER)
		return;
	operation = drag_operation(app);
	if (operation == FM_TASK_MOVE)
		return;

	/* A plus for a copy, an arrow for a link, at the bottom right. */
	cx = x + (float)DRAG_ICON - 2.0f;
	cy = y + (float)DRAG_ICON - 2.0f;
	if (operation == FM_TASK_COPY) {
		kl_canvas_circle(canvas, cx, cy, (float)DRAG_BADGE, DRAG_COLOR_COPY);
		kl_canvas_line(canvas, cx - 5.0f, cy, cx + 5.0f, cy, 2.0f, DRAG_COLOR_BADGE_TEXT);
		kl_canvas_line(canvas, cx, cy - 5.0f, cx, cy + 5.0f, 2.0f, DRAG_COLOR_BADGE_TEXT);
	} else {
		kl_canvas_circle(canvas, cx, cy, (float)DRAG_BADGE, DRAG_COLOR_LINK);
		kl_canvas_line(canvas, cx - 4.0f, cy + 4.0f, cx + 4.0f, cy - 4.0f, 2.0f, DRAG_COLOR_BADGE_TEXT);
		kl_canvas_line(canvas, cx - 1.0f, cy - 4.0f, cx + 4.0f, cy - 4.0f, 2.0f, DRAG_COLOR_BADGE_TEXT);
		kl_canvas_line(canvas, cx + 4.0f, cy - 4.0f, cx + 4.0f, cy + 1.0f, 2.0f, DRAG_COLOR_BADGE_TEXT);
	}
}

/*
 * Draws a dragged favorite: a line where it will go (above the target when
 * it moves up, below when it moves down) and its name in a small pill
 * under the pointer.
 */
static void
drag_draw_place(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	const struct fm_place *place;
	const struct kl_rect *rect;
	float line;
	int width;
	int hit;
	int x;
	int y;

	/* The target's row, the last drawn. */
	rect = NULL;
	if (app->drag_target == FM_DRAG_REORDER) {
		for (hit = app->hit_count - 1; hit >= 0; hit--) {
			if (app->hits[hit].kind == FM_HIT_PLACE && app->hits[hit].index == app->drag_hit_index) {
				rect = &app->hits[hit].rect;
				break;
			}
		}
	}

	/* The line where the favorite will go. */
	if (rect != NULL) {
		line = (float)rect->y;
		if (app->drag_hit_index > app->drag_place)
			line = (float)(rect->y + rect->height);
		kl_canvas_round(canvas, (float)rect->x + 4.0f, line - 1.5f, (float)rect->width - 8.0f, 3.0f, 1.5f, FM_COLOR_ACCENT);
		kl_canvas_circle(canvas, (float)rect->x + 4.0f, line, 4.0f, FM_COLOR_ACCENT);
	}

	/* The favorite's name in a pill beside the pointer. */
	place = &app->places.items[app->drag_place];
	width = kl_text_width(app->text, place->label, strlen(place->label), 13U, 0) + 24;
	x = app->pointer_x + DRAG_OFFSET_X + 8;
	y = app->pointer_y + DRAG_OFFSET_Y;
	kl_canvas_shadow(canvas, (float)x, (float)y + 2.0f, (float)width, 26.0f, 13.0f, 8.0f, FM_COLOR_SHADOW);
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, 26.0f, 13.0f, KL_RGBA(0xffffff, 235));
	(void)kl_text_draw(app->text, canvas, x + 12, kl_text_center(13U, y, 26), place->label, strlen(place->label), 13U, 0, FM_COLOR_TEXT);
}

/*
 * Hands the dragged items to the compositor when the pointer leaves the window:
 * the window's own target goes, and the Wayland side starts the drag and
 * drop (FM_REQUEST_DRAG_OUT).
 */
static void
drag_go_out(
	struct fm_app *app)
{
	/* No target of the window's any more. */
	app->drag_target = FM_DRAG_NONE;
	app->drag_hit_kind = FM_HIT_NONE;
	app->drag_hit_index = -1;

	/* The compositor carries the items from here on. */
	app->drag_outside = 1;
	app->request = FM_REQUEST_DRAG_OUT;
	app->dirty = 1;
	fm_log("DRAG out items=%lu", (unsigned long)app->drag_count);
}

/*
 * Finds the target of a drop coming in, where it is now: the part of the
 * titlebar's path it is over, or a folder under it (as for a drag within
 * the window), or else the folder shown (for another window's items).
 * Only folders are targets; a changed target is answered to the compositor.
 */
static void
drop_find(
	struct fm_app *app)
{
	static struct fm_crumb crumbs[FM_CRUMBS];
	const struct kl_rect *content;
	const char *shown;
	unsigned previous;
	char folder[FM_PATH_MAX];
	int count;
	int same;

	/* The target before, to tell a change. */
	previous = app->drag_target;
	snprintf(folder, sizeof(folder), "%s", app->drag_folder);

	/* A drag without file names has no target here. */
	if (app->drop_self == 0 && app->drop_files == 0) {
		drag_target(app, FM_DRAG_NONE, FM_HIT_NONE, -1, "");
	} else if (app->drop_part >= 0) {
		/* A part of the path: its folder (the folder shown is no target for the window's own items). */
		count = fm_ui_crumbs(app, crumbs, FM_CRUMBS);
		drag_target(app, FM_DRAG_NONE, FM_HIT_NONE, -1, "");
		if (app->drop_part < count && crumbs[app->drop_part].location.kind == FM_LOCATION_FOLDER) {
			shown = fm_current_folder(app);
			same = 1;
			if (shown != NULL)
				same = strcmp(shown, crumbs[app->drop_part].location.path);
			if (app->drop_self == 0 || same != 0)
				drag_target(app, FM_DRAG_FOLDER, FM_HIT_NONE, -2 - app->drop_part, crumbs[app->drop_part].location.path);
		}
	} else {
		/* Under the pointer as for a drag within the window; only a folder counts. */
		drag_find(app, app->drop_x, app->drop_y);
		if (app->drag_target != FM_DRAG_FOLDER)
			drag_target(app, FM_DRAG_NONE, FM_HIT_NONE, -1, "");

		/* Another window's items can go into the folder shown, anywhere on the content. */
		shown = fm_current_folder(app);
		content = &app->layout.content;
		if (app->drop_self == 0 && app->drag_target == FM_DRAG_NONE && shown != NULL &&
		    app->drop_x >= content->x && app->drop_x < content->x + content->width &&
		    app->drop_y >= content->y && app->drop_y < content->y + content->height)
			drag_target(app, FM_DRAG_FOLDER, FM_HIT_NONE, -1, shown);
	}

	/* A changed target is answered. */
	same = strcmp(folder, app->drag_folder);
	if (previous != app->drag_target || same != 0)
		app->drop_answer = 1;
	app->dirty = 1;
}

/* Opens a folder the drag rested on long enough; returns the milliseconds left, or -1 when none waits. */
static int
drag_spring_tick(
	struct fm_app *app,
	uint64_t now)
{
	struct fm_location location;
	uint64_t rested;
	int spring_tab;

	/* Nothing rests on a target that springs. */
	if (app->spring_ms == 0U)
		return -1;

	/* Another tab springs whatever it shows; anything else only as a folder (ws127-p006). */
	spring_tab = drag_spring_tab(app);
	if (spring_tab < 0) {
		if (app->drag_target != FM_DRAG_FOLDER || app->drag_folder[0] == '\0')
			return -1;
	}

	/* Not long enough yet: the time left. */
	rested = now - app->spring_ms;
	if (now < app->spring_ms)
		rested = 0U;
	if (rested < (uint64_t)DRAG_SPRING_MS)
		return (int)((uint64_t)DRAG_SPRING_MS - rested);

	/* A tab comes to the front, and the drag goes on over what it shows. */
	if (spring_tab >= 0) {
		fm_log("DRAG spring tab=%d", spring_tab);
		app->spring_ms = 0U;
		fm_tabs_select(app, spring_tab);
		drag_refind(app);
		app->dirty = 1;
		return 16;
	}

	/* The folder opens in the tab under the drag, which goes on over it. */
	memset(&location, 0, sizeof(location));
	location.kind = FM_LOCATION_FOLDER;
	snprintf(location.path, sizeof(location.path), "%s", app->drag_folder);
	fm_log("DRAG spring path=%s", location.path);
	app->spring_ms = 0U;
	fm_ui_go(app, &location);
	drag_refind(app);
	app->dirty = 1;

	/* Succeeded: the next step at once (the new target may spring in turn). */
	return 16;
}

/* Reports the tab other than the one shown that the drag's target lies on, or -1 (ws127-p006). */
static int
drag_spring_tab(
	const struct fm_app *app)
{
	/* Only a tab's region (its body or its close button). */
	if (app->drag_hit_kind != FM_HIT_TAB && app->drag_hit_kind != FM_HIT_TAB_CLOSE)
		return -1;

	/* A tab that is there. */
	if (app->drag_hit_index < 0 || app->drag_hit_index >= app->tab_count)
		return -1;

	/* The tab shown has nothing to bring to the front. */
	if (app->drag_hit_index == app->tab_index)
		return -1;

	/* Succeeded: that tab springs. */
	return app->drag_hit_index;
}

/* Scrolls the content when the drag is near its top or bottom edge; returns 16 while it does, -1 otherwise. */
static int
drag_edge_tick(
	struct fm_app *app,
	uint64_t now)
{
	const struct kl_rect *content;
	struct fm_tab *tab;
	uint64_t elapsed;
	double nearness;
	double depth;
	double speed;
	int amount;
	int inside;
	int before;
	int x;
	int y;

	/* The pointer, inside the content's width and height. */
	content = &app->layout.content;
	inside = drag_pointer(app, &x, &y);
	if (inside == 0 ||
	    x < content->x || x >= content->x + content->width ||
	    y < content->y || y >= content->y + content->height) {
		app->edge_ms = 0U;
		return -1;
	}

	/* How near an edge: 0 at the band's inner side, 1 at the edge; nothing outside the bands. */
	nearness = 0.0;
	if (y < content->y + DRAG_EDGE)
		nearness = -(double)(content->y + DRAG_EDGE - y) / (double)DRAG_EDGE;
	else if (y >= content->y + content->height - DRAG_EDGE)
		nearness = (double)(y - (content->y + content->height - DRAG_EDGE)) / (double)DRAG_EDGE;
	if (nearness == 0.0) {
		app->edge_ms = 0U;
		return -1;
	}

	/* The time since the last step (the first step only starts the clock). */
	if (app->edge_ms == 0U || now < app->edge_ms) {
		app->edge_ms = now;
		return 16;
	}

	/* The step's time, at most 50 ms (a stalled loop does not jump). */
	elapsed = now - app->edge_ms;
	if (elapsed > 50U)
		elapsed = 50U;
	app->edge_ms = now;

	/* The speed grows towards the edge; up for the top, down for the bottom. */
	depth = nearness;
	if (depth < 0.0)
		depth = -depth;
	speed = DRAG_EDGE_SLOW + (DRAG_EDGE_FAST - DRAG_EDGE_SLOW) * depth;
	amount = (int)(speed * (double)elapsed + 0.5);
	if (amount < 1)
		amount = 1;
	if (nearness < 0.0)
		amount = -amount;

	/* The content scrolls, and the target under the pointer is found again. */
	tab = fm_ui_tab(app);
	before = tab->scroll;
	fm_input_scroll(app, amount);
	tab = fm_ui_tab(app);
	if (tab->scroll != before)
		drag_refind(app);

	/* Succeeded: the next step soon. */
	return 16;
}

/* Gives where the drag is: the pointer for the window's own drag, the drop's place for one coming in. */
static int
drag_pointer(
	const struct fm_app *app,
	int *x,
	int *y)
{
	/* A drop coming in is where the compositor last said. */
	if (app->drop_active != 0) {
		*x = app->drop_x;
		*y = app->drop_y;
		return 1;
	}

	/* The window's own drag is under the pointer. */
	if (app->drag != 0 && app->pointer_inside != 0) {
		*x = app->pointer_x;
		*y = app->pointer_y;
		return 1;
	}

	/* No place. */
	return 0;
}

/* Finds the drag's target again after the content moved under it. */
static void
drag_refind(
	struct fm_app *app)
{
	/* A drop coming in has its own rules; the window's drag follows the pointer. */
	if (app->drop_active != 0) {
		drop_find(app);
		return;
	}

	/* The window's own drag finds its target under the pointer. */
	if (app->drag != 0)
		drag_find(app, app->pointer_x, app->pointer_y);
}
