/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications of a desktop (ws142-p004; apps.h says what they are).
 *
 * kwl_apps_build gathers the windows into applications, puts them in the
 * desktop's bar order (those the order knows where it has them, new ones
 * after them by their opening) and writes that order back, so that an
 * application that has gone leaves it and one that comes is added on the
 * right.  kwl_apps_move is the drag of an icon.
 */

#include "apps.h"

#include <stdio.h>
#include <string.h>

static int find_key(const struct kwl_apps *apps, unsigned count, const char *key);
static void add_window(struct kwl_app *app, const struct kwl_apps_window *windows, unsigned index);
static unsigned order_place(const struct kwl_apps_order *order, const char *key);
static int comes_before(const struct kwl_app *app, unsigned place, const struct kwl_app *other, unsigned other_place);

/*
 * Writes an application's key: its application ID, or its client's for a window without one.
 */
void
kwl_apps_key(
	const char *app_id,
	uint64_t client,
	char *key,
	size_t size)
{
	/* The application ID when there is one. */
	if (app_id != NULL && app_id[0] != '\0') {
		(void)snprintf(key, size, "%s", app_id);
		return;
	}

	/* Otherwise the client's. */
	(void)snprintf(key, size, "client:%llu", (unsigned long long)client);
}

/*
 * Gathers windows into applications in the bar's order, and the most
 * recently used order; the order is the desktop's bar order, read and
 * written back.  Windows past KWL_APPS_MAX applications are left out,
 * and of an application's windows past KWL_APPS_WINDOWS those raised
 * earliest.
 */
void
kwl_apps_build(
	const struct kwl_apps_window *windows,
	unsigned count,
	struct kwl_apps_order *order,
	struct kwl_apps *apps)
{
	struct kwl_app moving;
	char key[KWL_APPS_KEY];
	unsigned places[KWL_APPS_MAX];
	unsigned place;
	unsigned index;
	unsigned at;
	int found;
	int before;

	/* Each window joins its application, or starts one. */
	apps->count = 0;
	for (index = 0; index < count; index++) {
		kwl_apps_key(windows[index].app_id, windows[index].client, key, sizeof(key));
		found = find_key(apps, apps->count, key);
		if (found < 0) {
			/* A new application, while there is room. */
			if (apps->count >= KWL_APPS_MAX)
				continue;
			found = (int)apps->count;
			memset(&apps->apps[found], 0, sizeof(apps->apps[found]));
			(void)snprintf(apps->apps[found].key, sizeof(apps->apps[found].key), "%s", key);
			apps->apps[found].open_order = windows[index].open_order;
			apps->apps[found].minimized = 1;
			apps->count++;
		}

		/* The window in its application. */
		add_window(&apps->apps[found], windows, index);
	}

	/* The bar's order: by the place in the desktop's order, those it does not know after, by their opening (an insertion sort). */
	for (index = 0; index < apps->count; index++) {
		moving = apps->apps[index];
		place = order_place(order, moving.key);
		at = index;
		while (at > 0) {
			/* Shifted right while the one before comes after it. */
			before = comes_before(&moving, place, &apps->apps[at - 1], places[at - 1]);
			if (!before)
				break;
			apps->apps[at] = apps->apps[at - 1];
			places[at] = places[at - 1];
			at--;
		}

		/* In its place. */
		apps->apps[at] = moving;
		places[at] = place;
	}

	/* The order written back: the applications there are now, from the left. */
	for (index = 0; index < apps->count; index++)
		(void)snprintf(order->keys[index], sizeof(order->keys[index]), "%s", apps->apps[index].key);
	order->count = apps->count;

	/* The most recently used order: by the latest raise. */
	for (index = 0; index < apps->count; index++) {
		at = index;
		while (at > 0 && apps->apps[apps->recent[at - 1]].map_order < apps->apps[index].map_order) {
			apps->recent[at] = apps->recent[at - 1];
			at--;
		}

		/* In its place. */
		apps->recent[at] = index;
	}
}

/*
 * Moves an application in a bar order from one place to another (the
 * others shift over).  Returns 0, or -1 for a place that is not there.
 */
int
kwl_apps_move(
	struct kwl_apps_order *order,
	unsigned from,
	unsigned to)
{
	char key[KWL_APPS_KEY];
	unsigned index;

	/* Both places in the order. */
	if (from >= order->count || to >= order->count)
		return -1;

	/* The key taken out, the others shifted, the key put in. */
	memcpy(key, order->keys[from], sizeof(key));
	if (from < to) {
		for (index = from; index < to; index++)
			memcpy(order->keys[index], order->keys[index + 1U], sizeof(key));
	} else {
		for (index = from; index > to; index--)
			memcpy(order->keys[index], order->keys[index - 1U], sizeof(key));
	}

	/* The key in its new place. */
	memcpy(order->keys[to], key, sizeof(key));

	/* Succeeded: moved. */
	return 0;
}

/*
 * Finds an application by its key: its index in the bar's order, or -1.
 */
int
kwl_apps_find(
	const struct kwl_apps *apps,
	const char *key)
{
	int found;

	/* Among all of them. */
	found = find_key(apps, apps->count, key);
	if (found < 0)
		return -1;

	/* Succeeded: its index. */
	return found;
}

/* Finds a key among the first applications: its index, or -1. */
static int
find_key(
	const struct kwl_apps *apps,
	unsigned count,
	const char *key)
{
	unsigned index;
	int same;

	/* Each application. */
	for (index = 0; index < count; index++) {
		same = strcmp(apps->apps[index].key, key);
		if (same == 0)
			return (int)index;
	}

	/* Succeeded: none. */
	return -1;
}

/* Adds a window to its application, among its windows by the latest raise first. */
static void
add_window(
	struct kwl_app *app,
	const struct kwl_apps_window *windows,
	unsigned index)
{
	unsigned at;

	/* The application's opening, latest raise and whether all are minimized. */
	if (windows[index].open_order < app->open_order)
		app->open_order = windows[index].open_order;
	if (windows[index].map_order > app->map_order)
		app->map_order = windows[index].map_order;
	if (!windows[index].minimized)
		app->minimized = 0;

	/* A full application keeps its latest raised: the new window only in place of an earlier one. */
	if (app->window_count >= KWL_APPS_WINDOWS) {
		if (windows[app->windows[app->window_count - 1U]].map_order >= windows[index].map_order)
			return;
		app->window_count--;
	}

	/* After those raised later. */
	at = app->window_count;
	while (at > 0 && windows[app->windows[at - 1]].map_order < windows[index].map_order) {
		app->windows[at] = app->windows[at - 1];
		at--;
	}

	/* In its place. */
	app->windows[at] = index;
	app->window_count++;
}

/* Gives a key's place in a bar order, or the order's length when it is not there. */
static unsigned
order_place(
	const struct kwl_apps_order *order,
	const char *key)
{
	unsigned index;
	int same;

	/* Each key of the order. */
	for (index = 0; index < order->count; index++) {
		same = strcmp(order->keys[index], key);
		if (same == 0)
			return index;
	}

	/* Succeeded: not there. */
	return order->count;
}

/* Tells whether an application comes before another in the bar: by its place, then (both new) by its opening. */
static int
comes_before(
	const struct kwl_app *app,
	unsigned place,
	const struct kwl_app *other,
	unsigned other_place)
{
	/* Different places decide. */
	if (place < other_place)
		return 1;
	if (place > other_place)
		return 0;

	/* Both new: the earlier opening first. */
	if (app->open_order < other->open_order)
		return 1;

	/* Succeeded: it does not. */
	return 0;
}
