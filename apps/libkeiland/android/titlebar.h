/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state of a window's title bar strip on Android (titlebar.c): the menu
 * and the tabs the program gave, the strip's picture, and the touch that is
 * held on it.  A window of the application owns one; android.h puts it in
 * struct kl_window.
 */

#ifndef KEILAND_ANDROID_TITLEBAR_H
#define KEILAND_ANDROID_TITLEBAR_H

#include <keiland/keiland.h>
#include "apps/libkeiland/android/tabs.h"

#include <stddef.h>
#include <stdint.h>

/* How many tabs, menu items and action states a window keeps at most. */
#define ANDROID_TABS		64U
#define ANDROID_MENU_ITEMS	256U
#define ANDROID_ACTIONS		128U

/* The longest title of a tab, with its NUL. */
#define ANDROID_TAB_TITLE	256U

/* What a press of the strip holds (the parts of the strip, tabs and their close buttons by index). */
#define ANDROID_PART_NONE		0xFFFFFFFFU
#define ANDROID_PART_MENU		0xFFFFFFF0U
#define ANDROID_PART_NEW		0xFFFFFFF1U
#define ANDROID_PART_LIST		0xFFFFFFF2U
#define ANDROID_PART_TAB		0x10000U
#define ANDROID_PART_CLOSE		0x20000U

/* What a popup of the strip was opened for (what its answer is). */
#define ANDROID_POPUP_NONE	0U
#define ANDROID_POPUP_MENU	1U
#define ANDROID_POPUP_TAB	2U
#define ANDROID_POPUP_LIST	3U
#define ANDROID_POPUP_CONTEXT	4U

/* A long press on a tab opens its menu after this long, in milliseconds. */
#define ANDROID_LONG_PRESS_MS	500

/* What choosing an item of a popup does. */
#define ANDROID_ITEM_ACTION	0U
#define ANDROID_ITEM_TAB	1U
#define ANDROID_ITEM_CHOOSE	2U

/* The finger number a mouse's button holds the strip with. */
#define ANDROID_FINGER_MOUSE	(-2)

/* One tab. */
struct android_tab {
	uint32_t id;
	unsigned flags;
	char title[ANDROID_TAB_TITLE];
	struct android_tab_place place;
};

/* One item of a menu: a copy of struct kl_menu_entry (the label is the item's own). */
struct android_menu_item {
	uint32_t id;
	uint32_t parent;
	unsigned type;
	char *label;
	uint32_t action;
	unsigned modifiers;
	uint32_t keysym;
};

/* The state (KL_ACTION_*) of an action in the menu and the popups. */
struct android_action_state {
	uint32_t action;
	unsigned state;
};

/* What choosing one item of the open popup does: an action of the menu, a tab's request (KL_WINDOW_TAB_*), or choosing the tab of an ID. */
struct android_popup_item {
	uint32_t kind;
	uint32_t code;
	uint32_t id;
};

/*
 * The title bar's strip.  height is 0 while the window has neither menu nor
 * tabs; the strip's picture (width by height words) is drawn when anything
 * it shows changes, and kept for the native window to be painted from.
 */
struct android_titlebar {
	uint32_t height;
	uint32_t *pixels;
	uint32_t width;
	unsigned dpi;

	/* The strip's font. */
	struct kl_text text;
	int text_open;
	int text_tried;

	/* The menu, the actions' states, and the tabs. */
	struct android_menu_item *menu;
	size_t menu_count;
	struct android_action_state actions[ANDROID_ACTIONS];
	size_t action_count;
	struct android_tab *tabs;
	unsigned tab_count;
	unsigned tab_options;

	/* Where the menu's button, the new tab's button and the list's button are. */
	struct android_rect menu_place;
	struct android_rect new_place;
	struct android_tab_scroll scroll;

	/* The strip's scroll: the first tab shown, whether the active tab is to be brought into sight, the number of tabs shown and the active tab's ID last time. */
	unsigned first;
	int reveal;
	unsigned shown_last;
	uint32_t revealed;

	/* The press that is held: the part, the finger, when it began (microseconds) and whether its long press already came. */
	unsigned pressed;
	int32_t finger;
	uint64_t press_us;
	int long_done;
	int32_t press_x;
	int32_t press_y;
	int32_t swipe_x;

	/* The popup that is open: what it is for, its request's number, and what each of its items does (by position). */
	unsigned popup;
	uint32_t popup_request;
	struct android_popup_item *popup_items;
	size_t popup_count;
};

#endif /* KEILAND_ANDROID_TITLEBAR_H */
