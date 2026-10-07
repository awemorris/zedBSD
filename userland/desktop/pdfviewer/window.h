/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of PDF Viewer that speak Wayland and the compositor's extensions:
 * the window (libkeiland's kl_window since ws090-p008: the toplevel, the
 * seat's input and the frames shown with Vulkan), the menus (menu.c) and
 * the titlebar's controls (titlebar.c).  The host tests build the rest of
 * the program without them.
 */

#ifndef PDFVIEWER_WINDOW_H
#define PDFVIEWER_WINDOW_H

#include "viewer.h"
#include "touch.h"

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <keiland/keiland.h>

/*
 * The window: libkeiland's window, which queues the input (the menus' and
 * the titlebar's actions among it, posted in the order they came) and
 * shows the frames.
 *
 * One lives for the whole run.
 */
struct pv_window {
	struct kl_window *kui;
};

/*
 * What the menus and the titlebar show of the viewer: whether a document
 * is open, the page and the count, the mode and the fit.
 */
struct pv_state {
	int has_document;
	size_t page;
	size_t count;
	int mode;
	int fit;
	int thumbnails;
	int has_selection;
};

/* The action of the titlebar's page control ("Page 3 of 10"), which does nothing but show its state. */
#define PV_ACTION_PAGE_INFO	98U

/*
 * The window's menus as given to libkeiland (menu.c, WS131 p017): the
 * window, whether the menu was given (not without the compositor's System
 * Menu), and the state the actions last showed and whether it was sent.
 */
struct pv_menu {
	struct pv_window *window;
	int shown_once;
	struct pv_state shown;
	int sent;
};

/*
 * The window's titlebar in the compositor (titlebar.c, WS131 p017): the window,
 * and whether its controls are shown (not without the compositor's
 * titlebar).  The controls' state is their actions' (menu.c).
 */
struct pv_titlebar {
	struct pv_window *window;
	int shown;
};

/* ws128-p004: the titlebar's find field gets the keyboard; its text's inputs go to the viewer. */
void pv_titlebar_focus_find(struct pv_titlebar *titlebar);
void pv_titlebar_input(struct pv_titlebar *titlebar, struct pv_app *app, const struct kl_window_event *input);

/* The menus (menu.c). */
int pv_menu_open(struct pv_menu *menu, struct pv_window *window, const struct pv_state *state);
void pv_menu_refresh(struct pv_menu *menu, const struct pv_state *state);
void pv_menu_close(struct pv_menu *menu);

/* The titlebar's controls (titlebar.c). */
int pv_titlebar_open(struct pv_titlebar *titlebar, struct pv_window *window, const struct pv_state *state);
void pv_titlebar_refresh(struct pv_titlebar *titlebar, const struct pv_state *state);
void pv_titlebar_close(struct pv_titlebar *titlebar);

#endif
