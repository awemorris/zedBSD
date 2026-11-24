/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tab-only backend of the X server: shows every top-level window and
 * the root window in mikuterm tabs without creating native OS windows.
 */

#ifndef MIKUTERM_X11_TAB_ONLY_H
#define MIKUTERM_X11_TAB_ONLY_H

#include "x11-link.h"

#include "userland/desktop/xserver/x11backend.h"
#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/* The tabs and overlays managed by one tab-only X server. */
struct x11_tab_only;

/*
 * Callbacks for sending frames to mikuterm and querying how many bytes
 * are still queued on the link.
 */
struct x11_tab_only_link {
	int (*frame)(void *context, unsigned type, uint32_t id, const void *body, uint32_t size);
	size_t (*waiting)(void *context);
	void *context;
};

/* Backend operations table for x11server_options.backend. */
extern const struct x11_backend_ops x11_tab_only_ops;

int x11_tab_only_create(const struct x11_tab_only_link *link, unsigned scale, struct x11_tab_only **result);
void x11_tab_only_attach(struct x11_tab_only *tabs, struct x11server *server);
void x11_tab_only_destroy(struct x11_tab_only *tabs);
int x11_tab_only_take(struct x11_tab_only *tabs, const struct x11_link_frame *frame, uint32_t now);
void x11_tab_only_send(struct x11_tab_only *tabs, uint32_t now);

#endif
