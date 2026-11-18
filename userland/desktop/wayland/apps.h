/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications of a desktop (apps.c, ws142-p004): the windows of the
 * desktop shown, gathered into applications for the system bar's icons
 * (apps-bar.c) and the switcher.
 *
 * Windows with the same application ID are one application (two processes
 * of one program too); a window without one is an application of its own
 * client (D7).  The bar keeps an order of its own for each desktop: the
 * order the applications were opened in, new ones on the right, which the
 * user may change by dragging an icon (the 2026-10-05 request); the
 * switcher goes by the most recent use (D2).  Each application's windows
 * are the most recently raised first.
 *
 * It knows nothing of the server: the caller describes the windows, and
 * keeps each desktop's order between calls.  So the host tests run it
 * alone.
 */

#ifndef KWL_APPS_H
#define KWL_APPS_H

#include <stddef.h>
#include <stdint.h>

/* The most applications of a desktop, windows of an application, and bytes of an application's key. */
#define KWL_APPS_MAX		32U
#define KWL_APPS_WINDOWS	16U
#define KWL_APPS_KEY		72U

/* One window as the caller describes it. */
struct kwl_apps_window {
	/* Its application ID ("" for none) and its client's number. */
	const char *app_id;
	uint64_t client;
	/* When it was first shown, and when it was last raised (larger is later). */
	uint64_t open_order;
	uint64_t map_order;
	/* Whether it is minimized. */
	unsigned minimized;
};

/* One application. */
struct kwl_app {
	/* Its key: the application ID, or "client:N" for a window without one. */
	char key[KWL_APPS_KEY];
	/* Its windows (indexes into the caller's windows), the most recently raised first. */
	unsigned windows[KWL_APPS_WINDOWS];
	unsigned window_count;
	/* Its first window's opening, and its latest raise. */
	uint64_t open_order;
	uint64_t map_order;
	/* Whether every one of its windows is minimized. */
	unsigned minimized;
};

/* The applications of a desktop. */
struct kwl_apps {
	/* In the bar's order. */
	struct kwl_app apps[KWL_APPS_MAX];
	unsigned count;
	/* Indexes into apps, the most recently used first (the switcher's order). */
	unsigned recent[KWL_APPS_MAX];
};

/* A desktop's bar order, kept by the caller: the applications' keys from the left. */
struct kwl_apps_order {
	char keys[KWL_APPS_MAX][KWL_APPS_KEY];
	unsigned count;
};

void kwl_apps_key(const char *app_id, uint64_t client, char *key, size_t size);
void kwl_apps_build(const struct kwl_apps_window *windows, unsigned count, struct kwl_apps_order *order, struct kwl_apps *apps);
int kwl_apps_move(struct kwl_apps_order *order, unsigned from, unsigned to);
int kwl_apps_find(const struct kwl_apps *apps, const char *key);

#endif
