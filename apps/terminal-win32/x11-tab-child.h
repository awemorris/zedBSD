/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server process's side of the X windows shown in mikuterm's tabs
 * (a Host's X11WindowTabify): a backend of the X server's core that
 * wraps the Windows backend.
 *
 * Each top-level X window that no other window owns and that has no shape
 * (and the root window of a rooted server) is shown in a tab of mikuterm
 * instead of a window of this process: its pixels go to mikuterm over the
 * link (x11-link.h), changed parts only, as fast as the link takes them,
 * and the keys, text, pointer, size and closing the user gives the tab
 * come back and go to the core as the Windows backend's would.  Every
 * other window (a dialog, a menu, a tooltip, a shaped window, and one more
 * than the tabs a link has) is a window of the Windows backend as it is
 * without tabs, and so is a window mikuterm had no tab for.
 *
 * The tab's X window is kept where its picture is on the screen (the
 * pointer's place on the screen less its place on the window, taken when
 * the pointer comes in or a button goes down), so that the menus it opens
 * as windows of their own appear over the tab where the program means
 * them.  A tab the user closed asks the window to close; a window that is
 * still there a moment later (its program asked the user first, who kept
 * it) opens in a tab again.
 */

#ifndef MIKUTERM_X11_TAB_CHILD_H
#define MIKUTERM_X11_TAB_CHILD_H

#include "x11-link.h"

#include "userland/desktop/xserver/win32/backend.h"
#include "userland/desktop/xserver/x11backend.h"
#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/* The tabs of one X server, and the Windows backend they wrap. */
struct x11_tab_child;

/*
 * How the tabs reach mikuterm: frame queues one frame of the link (0, or
 * an errno value), and waiting tells how many bytes are queued and not
 * yet sent, both with the context.
 */
struct x11_tab_child_link {
	int (*frame)(void *context, unsigned type, uint32_t id, const void *body, uint32_t size);
	size_t (*waiting)(void *context);
	void *context;
};

/* The backend's functions, for x11server_options.backend (the context is the struct x11_tab_child). */
extern const struct x11_backend_ops x11_tab_child_ops;

int x11_tab_child_create(struct x11_win32 *inner, const struct x11_tab_child_link *link, struct x11_tab_child **result);
void x11_tab_child_attach(struct x11_tab_child *tabs, struct x11server *server);
void x11_tab_child_destroy(struct x11_tab_child *tabs);
int x11_tab_child_take(struct x11_tab_child *tabs, const struct x11_link_frame *frame, uint32_t now);
void x11_tab_child_send(struct x11_tab_child *tabs, uint32_t now);

#endif
