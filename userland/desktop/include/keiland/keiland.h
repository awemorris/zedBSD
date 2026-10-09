/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's library, with two jobs.
 *
 * It wraps the compositor's non-standard Wayland (xdg) extensions: a client of the
 * desktop (xserver, an application) uses standard Wayland and
 * Vulkan, and reaches anything only the compositor offers through this library,
 * never through a private protocol of its own.
 *
 * It is also an application's way to the desktop's system: the network, the
 * sound, the power and the devices (kl_system_*) and the desktop's settings
 * (kl_settings_*) are the compositor's, asked for through Keiland's system
 * extension.  The library itself speaks to no daemon and reads none of the
 * system's files (WS131 p011): the compositor's libkeiland-backend does, on
 * each operating system.
 *
 * Each feature adds its calls here when it arrives with its first user, so
 * that nothing is promised before it exists.  The first is the System Menu
 * (WS070): an application gives the compositor the meaning of its menus -- a tree
 * of numbered items with labels, states, actions and shortcuts -- and
 * the compositor draws them in the window's title bar, or in the system bar while
 * the window is docked, and tells the application what the user chose.
 *
 * It also holds what every program that follows a finger shares, so that
 * a finger feels the same everywhere: where a touch contact is at the time
 * a frame is drawn, and how fast it moved when it lifted (the touch motion,
 * WS081; its first user is the compositor itself), and what content a finger
 * scrolls does after the finger lets go, and what the fingers mean (the
 * scroller and the gestures, WS081 p005).
 */

#ifndef KEILAND_H
#define KEILAND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface version this header describes (2: the System Menu; 3: the recent files; 4: the titlebar; 5: the glass panels; 6: context menus; 7: drop targets in the titlebar; 8: the network; 9: the touch motion; 10: the scroller and the gestures; 11: the network's links, DNS and saved keys; 12: the file chooser (moved to the widgets with 16); 13: the desktop's preferences; 14: the desktop surface; 15: the sound output's volume; 16: the file chooser removed, now the widgets' kl_file_chooser; 17: kl_glass_set_blur; 18: the keyboard inset; 19: the editing operations; 20: the titlebar's sheet mode; 21: whether a sound service runs; 22: the network and the sound moved to kl_system_*, the old network and sound calls removed; 23: the machine's monitor, kl_system_monitor_*; 24: kl_system_network_set_scanning; 25: kl_titlebar_set_suggestions; 26: the application, kl_app_*, and the declarative menus, controls and glass of a window; 27: kl_system_account_set_password; 28: the removable volumes, kl_system_devices_mount; 29: the wired interfaces' configuration, kl_system_network_configure_wired; 30: Remote Login, kl_system_sharing_*; 31: the administration of the accounts, kl_system_account_administer; 32: one copy of a program and the activation, kl_instance_* and kl_activation_*; 33: a removable device's file system and size, kl_system_devices_info; 34: the lock screen's PIN, kl_system_account_set_pin; 35: the desktop's appearance, light or dark, kl_appearance_*; 36: what the user has enrolled, kl_system_account_enrolled; 37: the translations, kl_tr_*; 38: an input method's text for the focused widget, kl_ui_text and kl_ui_text_wanted; 39: a network link's speed, kl_network_link's link_mbps; 40: a touch pad's scrolling that flies on, kl_window_event's axis_source and KL_WINDOW_AXIS_STOP, kl_ui_axis, kl_scroll_axis and kl_axis_track; 41: the one inertia of every program, a touch pad's fingers on kl_scroller, kl_scroller_axis, _axis_stop and _axis_holding, kl_scroller_release's answer; 42: what a window shows, kl_window_set_content_type; 43: KL_WINDOW_AXIS_STOP is 18, apart from KL_WINDOW_ACTION; 44: a window's tabs, its selections' changes, drag and drop, kl_window_set_tabs and the others; 45: a window maximized, minimized, its screen's mode, its device, its frame's times and a breadcrumb's parts; 46: the desktop surface's role, drag and drop's actions and data, a drag over the titlebar's controls; 47: a window's input for its widgets and the text input of their fields, kl_ui_window_input and kl_ui_window_text, and the text area; 48: kl_slider_flags, kl_sidebar_place, kl_text_companions; 49: notifications, kl_system_notify, kl_system_notify_withdraw, kl_system_take_notify_event and kl_app_notify; 50: the recent list emptied and stopped, kl_recent_clear, kl_recent_set_keep and kl_recent_keep; 51: an application watches up to 64 descriptors, KL_APP_FDS_MAX; 52: the user's security keys, kl_system_account_add_key, kl_system_account_remove_key, kl_system_account_keys and kl_system_account_touched; 53: the views of many items, kl_list_header, kl_list_item, kl_list_cell, kl_grid_layout, kl_grid_cell, kl_grid_icon, kl_grid_item and kl_band; 54: the arrivals of mail, kl_system_mail_arrived, kl_system_mail_listen and kl_system_take_mail_event; 55: the phone, kl_system_phone_send, kl_system_phone_call and kl_system_take_phone_event; 56: the printers, kl_system_printers_* and kl_system_print_*; 57: the accent the user chose, kl_accent_get and kl_accent_values, the theme's accent_ink and accent_text; 58: the displays, kl_system_displays_*; 59: a display turned off, kl_system_displays_set_shown and KL_DISPLAY_OFF; 60: a button that is a picture alone, kl_icon_button, KL_ICON_DISCONNECT and KL_ICON_EJECT, and a button or a sidebar's place drawn without a kl_ui; 61: a scroll's own ends and rubber band, kl_scroll_set_bounds, the touch pad's times and velocity, kl_scroll_axis_at, kl_scroll_axis_stop_at and kl_scroll_axis_holding, and kl_scroll_fling's answer; 62: the part a change of the lit widget needs drawn again, kl_ui_take_damage, and kl_canvas_clear within the clip; 63: a frame shown by its changed part, kl_window_present_part; 64: a field's own limit, kl_field's limit and kl_field_set_limit; 65: the notification events a full ring lost, KL_NOTIFY_LOST, and whether a mail reader is allowed, kl_system_mail_allowed; 66: the recent list's stamp, kl_recent_stamp; 67: the text widgets' undo and redo, clipboard and words, Ctrl+Z, Ctrl+Y, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Left and Ctrl+Right, with kl_ui_window_text tying the input to its window; 68: what an input method reads around the caret, kl_window_text_context; 69: what Settings reads of the computer, kl_system_machine_*; 70: drag and drop of pictures between applications, KL_DROP_IMAGE, kl_window_start_drag_icon, kl_window_drag_fill, kl_drop_frame, kl_drop_caret and kl_ui_pointer_cancel; 71: the mounted file systems, kl_system_machine_mounts and KL_MACHINE_MOUNTS; 72: Bluetooth, kl_system_bluetooth_*; 73: the sound's playback streams, kl_audio_stream_*; 74: the fingers' selection of the fields and the text area with handles and a bar of editing buttons, kl_text_bar_*, kl_text_touch_select, kl_text_touch_hide_bar, kl_text_touch_toggle_bar, kl_text_touch's bar and KL_TEXT_TOUCH_BAR, kl_ui_set_text_bar; 75: a printer's name, IPP path or LPD queue changed, kl_system_printers_edit; 76: a network link that is a radio, kl_network_link's wireless; 77: a security key's own operations, kl_system_account_key_info, _key_info_get, _key_pin, _key_reset, _key_cancel, _replugged and _key_removed, KL_SYSTEM_HAS_KEY_OPS, KL_SYSTEM_CHANGED_KEYS and KL_SYSTEM_CHANGED_REPLUG; 78: the sign-in methods of the login and locked screens, kl_system_account_methods and _set_methods, KL_SYSTEM_HAS_METHODS; 79: the phone's messages synchronised, kl_system_phone_listen, _link, _watch_link, _link_set, _sync, _page_end, _send_text, _mark_read and kl_system_take_phone_item, KL_SYSTEM_HAS_PHONE_SYNC). */
#define KL_VERSION	79U

/*
 * Reports the interface version of the library that was loaded.
 *
 * A program built against this header may compare the result with
 * KL_VERSION to learn whether the library it runs with is older.
 */
unsigned kl_version(void);

/*
 * The System Menu.
 *
 * A service is one connection's way to the compositor's menus; a menu is one tree
 * of items; a window menu shows a menu on one xdg_toplevel.  One menu may be
 * shown on several windows (an application menu shared by its windows); the
 * choice comes back through the window menu it was made on.
 *
 * Items are named by numbers the application chooses (not 0; unique in
 * their menu).  KL_MENU_ROOT is the parent of the top-level items (in
 * a terminal: Shell, Edit, View, Session, Help); a submenu item is the
 * parent of the items under it.  Every change is made between
 * kl_menu_begin and kl_menu_commit, and the compositor shows the
 * changes of one commit together.
 *
 * The compositor owns the looks and the input.  A checkbox or radio item is not
 * checked by the compositor when it is chosen; the application sets its state in
 * the next transaction.  A shortcut is shown in the menu and the compositor
 * chooses the item when its keys are pressed in the focused window.
 *
 * Every call returns 0 or an errno value, and a refused call sends nothing:
 * EINVAL (a malformed argument, a change outside a transaction, a checked
 * state on an item that cannot be checked), EEXIST (an ID in use), ENOENT
 * (an ID that names no item), EBUSY (a transaction already open), E2BIG (a
 * menu or a label too large), ENOMEM.  The requests go out when the
 * application flushes its Wayland connection; the choices arrive when it
 * dispatches the queue its xdg_toplevel is on.
 */
struct wl_display;
struct wl_seat;
struct xdg_toplevel;
struct kl_menu_service;
struct kl_menu;
struct kl_window_menu;

/* The parent of the top-level items. */
#define KL_MENU_ROOT		0U

/* The kinds of item. */
#define KL_MENU_ITEM_NORMAL	0U
#define KL_MENU_ITEM_SEPARATOR	1U
#define KL_MENU_ITEM_CHECKBOX	2U
#define KL_MENU_ITEM_RADIO	3U
#define KL_MENU_ITEM_SUBMENU	4U

/* What an item means to the system (the compositor may give it an icon or a place of its own). */
#define KL_MENU_ROLE_NONE		0U
#define KL_MENU_ROLE_ABOUT	1U
#define KL_MENU_ROLE_PREFERENCES	2U
#define KL_MENU_ROLE_QUIT		3U
#define KL_MENU_ROLE_UNDO		4U
#define KL_MENU_ROLE_REDO		5U
#define KL_MENU_ROLE_CUT		6U
#define KL_MENU_ROLE_COPY		7U
#define KL_MENU_ROLE_PASTE	8U
#define KL_MENU_ROLE_DELETE	9U
#define KL_MENU_ROLE_SELECT_ALL	10U
#define KL_MENU_ROLE_NEW		11U
#define KL_MENU_ROLE_OPEN		12U
#define KL_MENU_ROLE_SAVE		13U
#define KL_MENU_ROLE_CLOSE	14U
#define KL_MENU_ROLE_FIND		15U
#define KL_MENU_ROLE_HELP		16U
#define KL_MENU_ROLE_FULLSCREEN	17U
#define KL_MENU_ROLE_ZOOM_IN	18U
#define KL_MENU_ROLE_ZOOM_OUT	19U

/* The modifiers of a shortcut, whose key is an XKB keysym ('c', '+', 0xffc8 for F11). */
#define KL_MENU_SHIFT		1U
#define KL_MENU_CTRL		2U
#define KL_MENU_ALT		4U
#define KL_MENU_SUPER		8U

/*
 * What a window menu tells the application.  Any member may be NULL.
 *
 * activated: the user chose an item (its ID and action), by the seat's
 * input of the serial.  opened, closed: the popup of a submenu (a top-level
 * item included) opened or closed; an application may update the menu in
 * answer, and the compositor redraws the open popup.
 */
struct kl_window_menu_listener {
	void (*activated)(void *data, struct kl_window_menu *window_menu, uint32_t item, uint32_t action, struct wl_seat *seat, uint32_t serial);
	void (*opened)(void *data, struct kl_window_menu *window_menu, uint32_t item);
	void (*closed)(void *data, struct kl_window_menu *window_menu, uint32_t item);
};

/*
 * Opens the connection's menu service.
 *
 * Returns NULL with errno ENOTSUP when the compositor has no System Menu;
 * the application then draws its own menus.
 */
struct kl_menu_service *kl_menu_service_open(struct wl_display *display);

/*
 * Closes a menu service; the menus and window menus made from it stay.
 */
void kl_menu_service_close(struct kl_menu_service *service);

/*
 * Makes an empty menu; NULL with errno set when it cannot.
 */
struct kl_menu *kl_menu_create(struct kl_menu_service *service);

/*
 * Destroys a menu; windows showing it show no menu.
 */
void kl_menu_destroy(struct kl_menu *menu);

/*
 * Starts a transaction: the changes that follow are shown together at the commit.
 */
int kl_menu_begin(struct kl_menu *menu);

/*
 * Ends a transaction, and the compositor shows its changes at once.
 */
int kl_menu_commit(struct kl_menu *menu);

/*
 * Adds an item as the last child of a parent.
 */
int kl_menu_append(struct kl_menu *menu, uint32_t id, uint32_t parent, unsigned type, const char *label, uint32_t action);

/*
 * Adds an item before one of a parent's children (0 appends).
 */
int kl_menu_insert(struct kl_menu *menu, uint32_t id, uint32_t parent, uint32_t before, unsigned type, const char *label, uint32_t action);

/*
 * Removes an item and everything under it.
 */
int kl_menu_remove(struct kl_menu *menu, uint32_t id);

/*
 * Sets an item's label (UTF-8, at most 255 bytes).
 */
int kl_menu_set_label(struct kl_menu *menu, uint32_t id, const char *label);

/*
 * Sets the action an item's choice reports.
 */
int kl_menu_set_action(struct kl_menu *menu, uint32_t id, uint32_t action);

/*
 * Sets whether an item can be chosen (it is shown pale when it cannot).
 */
int kl_menu_set_enabled(struct kl_menu *menu, uint32_t id, int enabled);

/*
 * Sets whether an item is shown at all.
 */
int kl_menu_set_visible(struct kl_menu *menu, uint32_t id, int visible);

/*
 * Sets whether a checkbox or radio item is checked.
 */
int kl_menu_set_checked(struct kl_menu *menu, uint32_t id, int checked);

/*
 * Sets an item's role (KL_MENU_ROLE_*).
 */
int kl_menu_set_role(struct kl_menu *menu, uint32_t id, unsigned role);

/*
 * Sets an item's icon by its icon-theme name ("" for none).
 */
int kl_menu_set_icon_name(struct kl_menu *menu, uint32_t id, const char *icon_name);

/*
 * Sets an item's shortcut: KL_MENU_* modifiers and an XKB keysym (0 removes it).
 */
int kl_menu_set_shortcut(struct kl_menu *menu, uint32_t id, unsigned modifiers, uint32_t keysym);

/*
 * Makes the place on a window that shows a menu; NULL with errno set when it cannot.
 */
struct kl_window_menu *kl_window_menu_create(struct kl_menu_service *service, struct xdg_toplevel *toplevel,
							  const struct kl_window_menu_listener *listener, void *data);

/*
 * Shows a menu on the window (NULL shows none).
 */
int kl_window_menu_set(struct kl_window_menu *window_menu, struct kl_menu *menu);

/*
 * Destroys a window's place for a menu; the window shows none.
 */
void kl_window_menu_destroy(struct kl_window_menu *window_menu);

/*
 * Context menus (ws071-p009): a menu's top-level items shown once as a
 * popup at a point of a surface, in answer to a press (its seat and
 * serial; the compositor opens only for the latest press).  The compositor owns the
 * looks and the input as for the menubar.  activated: the user chose an
 * item (its ID and action); done: the context menu closed, after a choice
 * or without one -- told once, last; the application destroys it then.
 * Either member may be NULL.
 */
struct wl_surface;
struct kl_context_menu;
struct kl_context_menu_listener {
	void (*activated)(void *data, struct kl_context_menu *context_menu, uint32_t item, uint32_t action, uint32_t serial);
	void (*done)(void *data, struct kl_context_menu *context_menu);
};

/*
 * Opens a menu as a context menu at (x, y) of a surface; NULL with errno
 * set: ENOTSUP for a compositor without context menus, ENOMEM, or EINVAL
 * when the listener cannot be installed.
 */
struct kl_context_menu *kl_menu_popup(struct kl_menu_service *service, struct kl_menu *menu, struct wl_surface *surface,
						  int32_t x, int32_t y, struct wl_seat *seat, uint32_t serial,
						  const struct kl_context_menu_listener *listener, void *data);

/*
 * Destroys a context menu; one still open closes without telling.
 */
void kl_context_menu_destroy(struct kl_context_menu *context_menu);

/*
 * The Titlebar Presentation (WS070 p008, plan/ws070/titlebar-design.md).
 *
 * The compositor draws a window's titlebar: its mark and title, a presentation,
 * and the window's buttons, in the floating titlebar or, while the window
 * is maximized, in the system bar.  The presentation is one of three
 * models the application gives: the menu (the System Menu above, the
 * default), controls (back, forward, a breadcrumb, a search field, a view
 * selector...), or tabs.  The application gives only what they mean;
 * the compositor decides how they look and where they go, and tells the
 * application what the user does with them.  Changes are made in
 * transactions, like a menu's.  Every call that returns an int returns 0
 * or an errno value.
 */
struct kl_titlebar;

/*
 * The presentation modes.  A sheet (KL_VERSION 20, ws090-p014) has no
 * titlebar of its own: the window hangs under its parent's titlebar
 * (xdg_toplevel_set_parent), in front of the parent, which takes no input
 * but its titlebar's while the sheet is open.  A compositor older than the
 * sheet refuses it (ENOTSUP) and the window stays a window of its own.
 */
#define KL_TITLEBAR_MENU		0U
#define KL_TITLEBAR_CONTROLS	1U
#define KL_TITLEBAR_TABS		2U
#define KL_TITLEBAR_SHEET		3U

/* The controls' roles, which decide how the compositor draws them. */
#define KL_CONTROL_BACK		1U
#define KL_CONTROL_FORWARD	2U
#define KL_CONTROL_HOME		3U
#define KL_CONTROL_UP		4U
#define KL_CONTROL_BREADCRUMB	5U
#define KL_CONTROL_SEARCH		6U
#define KL_CONTROL_VIEW_GRID	7U
#define KL_CONTROL_VIEW_LIST	8U
#define KL_CONTROL_VIEW_COLUMNS	9U
#define KL_CONTROL_SORT		10U
#define KL_CONTROL_FILTER		11U
#define KL_CONTROL_SIDEBAR	12U
#define KL_CONTROL_PREVIEW	13U
#define KL_CONTROL_PROGRESS	14U
#define KL_CONTROL_PRIMARY_ACTION	15U
#define KL_CONTROL_GENERIC	16U

/* The controls' priorities: the order they give way in when the room runs short. */
#define KL_PRIORITY_PRIMARY	0U
#define KL_PRIORITY_NORMAL	1U
#define KL_PRIORITY_SECONDARY	2U

/* A progress control's value that says the share done is not known. */
#define KL_PROGRESS_UNKNOWN	1001U

/* The tabs' flags, and the tab strip's options. */
#define KL_TAB_ACTIVE		1U
#define KL_TAB_ATTENTION		2U
#define KL_TAB_CLOSABLE		4U
#define KL_TABS_NEW_BUTTON	1U

/*
 * How a text control takes the keyboard (a search as a field with its text
 * selected; with _EDIT, ws177-p043, to go on editing it, the caret at its
 * end and nothing selected; a breadcrumb with _EDIT as a path to edit), and
 * how its editing ended.
 */
#define KL_FOCUS_FIELD		0U
#define KL_FOCUS_EDIT		1U
#define KL_TEXT_SUBMITTED		0U
#define KL_TEXT_CANCELLED		1U
#define KL_TEXT_LEFT		2U

/*
 * What the compositor tells the application about its titlebar: a control chosen
 * (detail is a breadcrumb's part, 0 otherwise), a text control's text as
 * it is typed and when its editing ends, a tab chosen or closed, the
 * new-tab button, and the overflow popup opening.  Any may be NULL.
 *
 * The compositor gives tabs the keyboard too, when the window's menu has no
 * shortcut for the key: Ctrl+Tab and Ctrl+PageDown activate the next tab,
 * Ctrl+Shift+Tab and Ctrl+PageUp the one before (tab_activated).  Closing
 * a tab and a new tab are the application's keys (its menu's shortcuts),
 * since a terminal's shell needs Ctrl+W and Ctrl+T.
 *
 * drop_target (KL_VERSION 7): while a drag and drop (wl_data_device)
 * is over a part of a breadcrumb in the titlebar, the compositor makes the
 * window's surface the drag's target (its data device hears enter, motion
 * and drop at the pointer's place, above the surface) and tells the part
 * here first (id and detail as for control_activated); id 0 says the drag
 * is over none of the controls now.  A drop then goes to that part's folder.
 */
struct kl_titlebar_listener {
	void (*control_activated)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t detail, struct wl_seat *seat, uint32_t serial);
	void (*text_changed)(void *data, struct kl_titlebar *titlebar, uint32_t id, const char *text);
	void (*text_done)(void *data, struct kl_titlebar *titlebar, uint32_t id, const char *text, unsigned how);
	void (*tab_activated)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t serial);
	void (*tab_close_requested)(void *data, struct kl_titlebar *titlebar, uint32_t id);
	void (*new_tab_requested)(void *data, struct kl_titlebar *titlebar, uint32_t serial);
	void (*overflow_menu_opened)(void *data, struct kl_titlebar *titlebar);
	void (*drop_target)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t detail);
};

/*
 * Gives a window its titlebar presentation, in menu mode until changed;
 * NULL with errno set (ENOTSUP for a compositor without it).
 */
struct kl_titlebar *kl_titlebar_create(struct wl_display *display, struct xdg_toplevel *toplevel,
						   const struct kl_titlebar_listener *listener, void *data);

/*
 * Takes the titlebar presentation away; the window shows its menu again.
 */
void kl_titlebar_destroy(struct kl_titlebar *titlebar);

/*
 * Starts a transaction; the changes until kl_titlebar_commit are shown together.
 */
int kl_titlebar_begin(struct kl_titlebar *titlebar);

/*
 * Ends a transaction; the compositor shows its changes at once.
 */
int kl_titlebar_commit(struct kl_titlebar *titlebar);

/*
 * Chooses the presentation (KL_TITLEBAR_*).
 */
int kl_titlebar_set_mode(struct kl_titlebar *titlebar, unsigned mode);

/*
 * Adds a control at the end: its ID (not 0), role, priority, the segmented group it joins (0 for none) and label.
 */
int kl_titlebar_add_control(struct kl_titlebar *titlebar, uint32_t id, unsigned role, unsigned priority, unsigned group, const char *label);

/*
 * Removes a control.
 */
int kl_titlebar_remove_control(struct kl_titlebar *titlebar, uint32_t id);

/*
 * Sets a control's label.
 */
int kl_titlebar_set_control_label(struct kl_titlebar *titlebar, uint32_t id, const char *label);

/*
 * Sets whether a control works now and whether it is checked.
 */
int kl_titlebar_set_control_state(struct kl_titlebar *titlebar, uint32_t id, int enabled, int checked);

/*
 * Sets a progress control's share done, in thousandths (KL_PROGRESS_UNKNOWN when not known).
 */
int kl_titlebar_set_control_value(struct kl_titlebar *titlebar, uint32_t id, unsigned value);

/*
 * Sets a search's or a breadcrumb's text and what it shows when empty.
 */
int kl_titlebar_set_control_text(struct kl_titlebar *titlebar, uint32_t id, const char *text, const char *placeholder);

/*
 * Sets a breadcrumb's parts, from the first (the outermost) to the last.
 */
int kl_titlebar_set_breadcrumb(struct kl_titlebar *titlebar, uint32_t id, const char *const *segments, size_t count);

/*
 * Adds a tab at the end, closable and not active.
 */
int kl_titlebar_add_tab(struct kl_titlebar *titlebar, uint32_t id, const char *title);

/*
 * Removes a tab.
 */
int kl_titlebar_remove_tab(struct kl_titlebar *titlebar, uint32_t id);

/*
 * Sets a tab's title and flags (KL_TAB_*).
 */
int kl_titlebar_set_tab(struct kl_titlebar *titlebar, uint32_t id, const char *title, unsigned flags);

/*
 * Sets the tab strip's options (KL_TABS_NEW_BUTTON).
 */
int kl_titlebar_set_tabs_options(struct kl_titlebar *titlebar, unsigned options);

/*
 * Gives the keyboard to a committed search or breadcrumb control (KL_FOCUS_*), outside a transaction.
 */
int kl_titlebar_focus_control(struct kl_titlebar *titlebar, uint32_t id, unsigned mode);

/* The most suggestions a field shows (kl_titlebar_set_suggestions). */
#define KL_TITLEBAR_SUGGESTIONS_MAX	12U

/*
 * Gives a committed search or breadcrumb field suggestions, outside a
 * transaction (ws127-p010): count pairs, each a label shown in a list
 * under the field and the text it puts in the field when chosen (Up, Down
 * and Enter, or a click or a tap; the field then reports the text as
 * changed, as typing does).  The list shows while the field has the
 * keyboard and goes when its text changes or its editing ends; count 0
 * takes it away.  Returns 0, ENOTSUP for a compositor without suggestions
 * (one older than KL_VERSION 25), ENOENT, EINVAL or E2BIG.
 */
int kl_titlebar_set_suggestions(struct kl_titlebar *titlebar, uint32_t id, const char *const *labels, const char *const *texts, size_t count);

/*
 * The recent files (WS071).
 *
 * One list of recently used files for all applications, newest first: a
 * file manager shows it as Recents, an application may offer it as "open
 * recent".  An application adds a file when it opens or saves one.  Every
 * call returns 0 or an errno value.
 */

/* The longest path and application name an entry holds, with the terminating NUL. */
#define KL_RECENT_PATH_MAX	4096U
#define KL_RECENT_NAME_MAX	64U

/* How many entries the list keeps (the oldest go first). */
#define KL_RECENT_KEPT		256U

/*
 * One entry of the recent list: the file's absolute path, the application
 * that used it (its app_id) and when, in seconds since the epoch.
 */
struct kl_recent_item {
	char path[KL_RECENT_PATH_MAX];
	char application[KL_RECENT_NAME_MAX];
	int64_t time;
};

/*
 * Adds a file (an absolute path) to the recent list, or makes it the
 * newest when it is listed.
 */
int kl_recent_add(const char *path, const char *application);

/*
 * Reads the recent list, newest first, into up to capacity items.
 */
int kl_recent_list(struct kl_recent_item *items, size_t capacity, size_t *count);

/*
 * Takes a file off the recent list.
 */
int kl_recent_remove(const char *path);

/*
 * Empties the recent list (KL_VERSION 50: Files' Recents, "Clear Recents").
 */
int kl_recent_clear(void);

/*
 * Chooses whether the recent list is kept (KL_VERSION 50: Settings'
 * Storage, "Keep recent items"): 0 empties it and kl_recent_add adds
 * nothing until 1 starts it again.  The choice is the user's, for every
 * application.
 */
int kl_recent_set_keep(int keep);

/*
 * Tells whether the recent list is kept (*keep is 1) or stopped (0).
 */
int kl_recent_keep(int *keep);

/*
 * Gives a stamp of the recent list (KL_VERSION 66, ws177-p008): a value
 * that changes whenever the list changes, is emptied, or is stopped or
 * started again, so a program showing the list ("Open Recent") reads it
 * again when the stamp is not the one it read it at (when its window gets
 * the keyboard, say).  Returns 0, or an errno value.
 */
int kl_recent_stamp(uint64_t *stamp);

/*
 * The glass panels (ws035-p083).
 *
 * A window whose Vulkan swapchain is see-through
 * (VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) names the parts of itself
 * that stand on the system's frosted glass: cards floating in the window.
 * The compositor draws the glass under them -- the desktop behind, blurred and
 * lightened, a bright rim, the card's shadow --
 * and the window's image over it by its alpha; between the panels the
 * desktop shows as it is.  The window says what its parts are, not how
 * the glass looks.
 *
 * The panels, in the surface's coordinates, take effect with the surface's
 * next commit (a Vulkan present), so they move with the frame drawn for
 * them.  Every call returns 0 or an errno value, and a refused call sends
 * nothing: EINVAL (an empty panel, a radius past the largest, an unknown
 * kind), E2BIG (too many panels).
 */
struct kl_glass;

/* The kind of panel (the only one so far): a card floating in the window. */
#define KL_GLASS_CARD		0U

/* The most panels a surface has, and the largest corner radius. */
#define KL_GLASS_PANELS_MAX	32U
#define KL_GLASS_RADIUS_MAX	64

/*
 * One panel: its rectangle in the surface's coordinates, the radius of
 * its corners and its kind.
 */
struct kl_glass_panel {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int32_t radius;
	unsigned kind;
};

/*
 * Gives a surface its glass, with no panels yet.  Returns NULL with errno
 * set: ENOTSUP for a compositor without glass, ENOMEM.
 */
struct kl_glass *kl_glass_create(struct wl_display *display, struct wl_surface *surface);

/*
 * Sets the surface's panels for its next commit (count 0: none).
 */
int kl_glass_set_panels(struct kl_glass *glass, const struct kl_glass_panel *panels, size_t count);

/*
 * Chooses whether the surface's glass (its panels and its title bar) shows
 * the windows under it blurred (enabled) or only the blurred wallpaper (the
 * default: the compositor draws nothing again for it), from the surface's
 * next commit (KL_VERSION 17).  Returns ENOTSUP when the compositor's
 * glass has no choice.
 */
int kl_glass_set_blur(struct kl_glass *glass, int enabled);

/*
 * Takes the glass away: the surface's next commit shows it without panels.
 */
void kl_glass_destroy(struct kl_glass *glass);

/*
 * The touch motion (WS081, plan/ws081/design.md sections 3 and 6).
 *
 * A cheap touch screen reports 30 to 60 times a second, unevenly, and with
 * noise.  Drawing the last report at each frame makes a scroll or a dragged
 * window judder: some frames get no report and stand still, the next jumps.
 * A motion instead fits a line and a parabola to the contact's last reports
 * and evaluates them a little behind the frame's time, so that the point
 * advances smoothly with the frames and is never extrapolated far past the
 * last report.
 *
 * A device (one touch screen in the compositor, one seat's touch in a client)
 * keeps what is learned across strokes: the report period, how late
 * reports arrive, the noise, and the mapping of the panel's Scan Time
 * (evdev MSC_TIMESTAMP) onto the host clock.  A motion is one contact's
 * stroke from touch-down to lift.  Neither is shared between threads.
 *
 * Times are CLOCK_MONOTONIC microseconds; positions are logical pixels.
 * The compositor's wl_touch times are the same clock's milliseconds (the low 32
 * bits), from the time the panel scanned the report when it has a Scan
 * Time, so a client can compare them with its own clock.  Calls that can
 * fail return 0 or an errno value.
 */

/* How far past the last report content that follows a finger may be extrapolated (12 ms). */
#define KL_MOTION_EXTRAPOLATION_CONTENT	12000U

/* How far past the last report the provisional tail of a drawn line may be extrapolated (16 ms). */
#define KL_MOTION_EXTRAPOLATION_INK	16000U

/* A device's timing and noise, learned across strokes (opaque). */
struct kl_motion_device;

/* One contact's stroke (opaque). */
struct kl_motion;

/*
 * Creates a device, knowing nothing of it yet.
 *
 * Returns NULL when memory is short.
 */
struct kl_motion_device *kl_motion_device_create(void);

/*
 * Destroys a device.  Its motions must be destroyed first.
 */
void kl_motion_device_destroy(struct kl_motion_device *device);

/*
 * Turns a report's host time and its panel's Scan Time into the time the
 * panel scanned it, on the host clock.
 *
 * host_us is the evdev time of the report; device_us is its MSC_TIMESTAMP
 * (microseconds, wrapping at 2^32, restarting at 0 after a pause).  The
 * result is never later than host_us and never goes back.  While the Scan
 * Time has not proved itself (it must advance at the host's rate, within
 * 10%, over half a second) the result is host_us.  A device without Scan
 * Time does not call this and uses the host time.
 */
int kl_motion_device_time(struct kl_motion_device *device, uint64_t host_us, uint32_t device_us,
    uint64_t *stamp_us);

/* Reports the device's measured report period in microseconds (0: not measured yet). */
uint32_t kl_motion_device_interval(const struct kl_motion_device *device);

/* Reports the device's measured noise in pixels (the default until five strokes have been seen). */
double kl_motion_device_noise(const struct kl_motion_device *device);

/*
 * Creates a motion for contacts of a device.
 *
 * Returns NULL for a missing device or when memory is short.
 */
struct kl_motion *kl_motion_create(struct kl_motion_device *device);

/* Destroys a motion. */
void kl_motion_destroy(struct kl_motion *motion);

/*
 * Starts a stroke: a finger touched.  Whatever the motion held is
 * forgotten, without teaching the device.
 */
void kl_motion_begin(struct kl_motion *motion);

/*
 * Adds one report of the stroke.
 *
 * stamp_us is when the finger was there (the evdev time, or the result of
 * kl_motion_device_time); arrival_us is when the caller read the
 * report, on the same clock.  A report older than the last one is refused
 * with EINVAL.  After a second without reports the older ones are dropped.
 */
int kl_motion_add(struct kl_motion *motion, uint64_t stamp_us, uint64_t arrival_us, double x, double y);

/*
 * Gives the point to draw at a frame.
 *
 * now_us is the time the frame is being drawn; extrapolation_us is how far
 * past the last report the point may be predicted
 * (KL_MOTION_EXTRAPOLATION_CONTENT or _INK).  Call it once a frame for
 * each contact: the time it evaluates the stroke at moves at most half a
 * millisecond a call.  Returns ENOENT before the first report.
 */
int kl_motion_point(struct kl_motion *motion, uint64_t now_us, uint32_t extrapolation_us, double *x,
    double *y);

/*
 * Gives the velocity (pixels a second) the finger had when it lifted.
 *
 * lift_us is the time of the lift.  The velocity is zero when the finger
 * had come to rest, and when its last report is older than the lift by
 * more than two periods (50 ms at least).  It is at most 8000 px/s.
 * Whether it starts a fling is the caller's decision.
 */
int kl_motion_velocity(struct kl_motion *motion, uint64_t lift_us, double *vx, double *vy);

/*
 * Ends a stroke: the finger lifted.  What the stroke measured (period,
 * delay, noise) is folded into the device.
 */
void kl_motion_end(struct kl_motion *motion);

/*
 * The scroller (WS081 p005, plan/ws081/design.md section 5): what content
 * that a finger scrolls does after the finger lets go, the same in every
 * program.
 *
 * A scroller holds a position on up to two axes within bounds.  A drag
 * moves it with the finger; past a bound the content resists (rubber
 * band).  Let go fast enough, it glides on and slows down by
 * dv/dt = -v/tau - mu sign(v) (tau 0.45 s, mu 300 px/s^2), which stops at a
 * finite time; past a bound it springs back (critically damped, 16/s).  A
 * press while it glides catches it (stops it, and the press should not
 * activate what is under it); a fling within 400 ms of a catch, the same
 * way within 30 degrees, adds the caught speed.  The position is a pure
 * function of the time while it glides, so frames may come unevenly.
 *
 * Positions are logical pixels of content offset (a larger position shows
 * content further right or down); a finger moving down by d moves the
 * position up by d.  Times are CLOCK_MONOTONIC microseconds.
 */
struct kl_scroller;

/* The slowest fling (px/s): a release slower than this only settles. */
#define KL_SCROLLER_FLING_MIN	300.0

/*
 * Creates a scroller at position 0 with bounds 0..0 on both axes (it
 * scrolls nowhere until kl_scroller_set_bounds).
 *
 * Returns NULL when memory is short.
 */
struct kl_scroller *kl_scroller_create(void);

/* Destroys a scroller. */
void kl_scroller_destroy(struct kl_scroller *scroller);

/*
 * Sets the bounds of the position on each axis and the viewport's size (the
 * rubber band's scale).  An axis whose maximum equals its minimum does not
 * scroll.  A position left outside the new bounds springs back.  Refuses a
 * maximum below its minimum or a viewport not above zero with EINVAL.
 */
int kl_scroller_set_bounds(struct kl_scroller *scroller, double minimum_x, double maximum_x, double minimum_y,
    double maximum_y, double viewport_width, double viewport_height);

/* Moves the position at once (clamped to the bounds), stopping any motion. */
void kl_scroller_set_position(struct kl_scroller *scroller, double x, double y);

/*
 * A finger touches: starts a drag from the current position.  Returns 1
 * when it caught moving content (the touch should not tap), otherwise 0.
 */
int kl_scroller_press(struct kl_scroller *scroller, uint64_t now_us);

/*
 * The finger has moved by dx, dy since the press (the total, not a step;
 * kl_gesture_drag_offset gives it resampled for the frame).  The first
 * time it has moved 8 px, a drag within 22.5 degrees of one axis locks the
 * other (on a scroller that scrolls both ways).
 */
void kl_scroller_drag(struct kl_scroller *scroller, double dx, double dy);

/*
 * The finger lifts with a velocity (px/s, as the finger moved;
 * kl_motion_velocity or the gesture's DRAG_END gives it): a fling
 * when it is fast enough, otherwise the content settles.  Returns 1 for a
 * fling (KL_VERSION 41), 0 when it settles.
 */
int kl_scroller_release(struct kl_scroller *scroller, uint64_t now_us, double vx, double vy);

/*
 * A touch pad's two fingers (KL_AXIS_SOURCE_FINGER, KL_VERSION 41,
 * ws090-p019) move the content by dx, dy: pixels as a wheel scrolls (down
 * and right positive), at the compositor's time event_us; now_us is the
 * time the steps use.  The first move since the fingers last lifted
 * presses the scroller (returns 1 when it caught moving content); each
 * drags it on and is kept for their velocity.
 */
int kl_scroller_axis(struct kl_scroller *scroller, double dx, double dy, uint64_t event_us, uint64_t now_us);

/*
 * The touch pad's fingers lift (the axis stop at event_us): the content
 * flies on from now_us at their velocity (none when they rested before
 * lifting) with the same deceleration as a finger's fling, or settles.
 * Gives the velocity (px/s, a wheel's way; either may be NULL) and returns
 * 1 for a fling.
 */
int kl_scroller_axis_stop(struct kl_scroller *scroller, uint64_t event_us, uint64_t now_us, double *vx, double *vy);

/* Tells whether a touch pad's fingers hold the content (since kl_scroller_axis, until they lift). */
int kl_scroller_axis_holding(const struct kl_scroller *scroller);

/* The touch was taken away (wl_touch.cancel): no fling; content past a bound springs back. */
void kl_scroller_cancel(struct kl_scroller *scroller, uint64_t now_us);

/*
 * Gives the position to draw at a time (past a bound, as the rubber band
 * shows it).  Returns 1 while the content moves by itself (keep drawing
 * frames), 0 when it rests or follows a finger.
 */
int kl_scroller_step(struct kl_scroller *scroller, uint64_t now_us, double *x, double *y);

/*
 * The gestures of a touch surface (WS081 p005, design section 5.6): what
 * the fingers of one wl_touch surface mean, so that a tap, a long press and
 * a drag are told apart the same way in every program.
 *
 * The program passes the surface's wl_touch events on (in surface-local
 * pixels, with the event's time and the time it read the event, both in
 * microseconds) and reads the gestures with kl_gesture_next, which
 * also keeps the time (a long press is found by the clock, so the program
 * calls it at least every frame while a finger is down).
 *
 * - TAP: a finger lifted within 8 px of where it touched, before 500 ms.
 *   DOUBLE_TAP follows a TAP within 300 ms and 16 px of the last TAP.
 * - LONG_PRESS: a finger held within 8 px for 500 ms (a context menu, or
 *   the start of a selection); the lift then taps nothing.
 * - DRAG_BEGIN: the fingers moved 8 px (after_long_press says whether a
 *   long press came first); while dragging, kl_gesture_drag_offset
 *   gives the fingers' centroid's movement since, resampled for a frame.
 *   DRAG_END: the last finger lifted, with its velocity (for a fling).
 * - Two fingers: the drag follows their centroid (a finger added or
 *   lifted does not make it jump), and kl_gesture_pinch gives the
 *   change of their distance since the second touched.
 * - CANCEL: kl_gesture_cancel was called (wl_touch.cancel): whatever
 *   was going on ends without its lift.
 */
struct kl_gesture;

#define KL_GESTURE_TAP		1U
#define KL_GESTURE_DOUBLE_TAP	2U
#define KL_GESTURE_LONG_PRESS	3U
#define KL_GESTURE_DRAG_BEGIN	4U
#define KL_GESTURE_DRAG_END	5U
#define KL_GESTURE_CANCEL		6U

/*
 * One gesture: its kind (KL_GESTURE_*), where (the touch's point for a
 * tap or a long press, the centroid where a drag began), the velocity of a
 * DRAG_END (px/s), the fingers down, and for a DRAG_BEGIN whether a long
 * press came first.
 */
struct kl_gesture_event {
	unsigned kind;
	double x;
	double y;
	double vx;
	double vy;
	unsigned fingers;
	int after_long_press;
};

/*
 * Creates the gestures of one surface, with a touch motion device of its
 * own (the seat's touch screen as the program sees it).
 *
 * Returns NULL when memory is short.
 */
struct kl_gesture *kl_gesture_create(void);

/* Destroys the gestures of a surface. */
void kl_gesture_destroy(struct kl_gesture *gesture);

/*
 * A finger touches (wl_touch.down): its id, the event's time and the time
 * the program read it, and its place.  Refuses a sixth finger, and an id
 * already down, with EBUSY and EEXIST.
 */
int kl_gesture_down(struct kl_gesture *gesture, int32_t id, uint64_t time_us, uint64_t arrival_us, double x,
    double y);

/* A finger moves (wl_touch.motion).  Refuses an id that is not down with ENOENT. */
int kl_gesture_motion(struct kl_gesture *gesture, int32_t id, uint64_t time_us, uint64_t arrival_us, double x,
    double y);

/* A finger lifts (wl_touch.up).  Refuses an id that is not down with ENOENT. */
int kl_gesture_up(struct kl_gesture *gesture, int32_t id, uint64_t time_us);

/* The compositor took the fingers (wl_touch.cancel). */
void kl_gesture_cancel(struct kl_gesture *gesture);

/*
 * Takes the next gesture, after judging the time now (a long press).
 * Returns 1 with a gesture in *event, 0 when there is none.
 */
int kl_gesture_next(struct kl_gesture *gesture, uint64_t now_us, struct kl_gesture_event *event);

/*
 * Gives how far the dragging fingers' centroid has moved since the drag
 * began, resampled for a frame drawn at now_us.  Returns ENOENT when no
 * drag is going on.
 */
int kl_gesture_drag_offset(struct kl_gesture *gesture, uint64_t now_us, double *dx, double *dy);

/*
 * Gives the ratio of two fingers' distance to their distance when the
 * second touched, and their centroid, for a frame drawn at now_us.
 * Returns ENOENT unless two fingers are down.
 */
int kl_gesture_pinch(struct kl_gesture *gesture, uint64_t now_us, double *scale, double *x, double *y);

/*
 * The file chooser of KL_VERSION 12 (ws092-p003) moved to the widgets
 * with KL_VERSION 16 (ws090-p006): kl_file_chooser_* below, made of
 * the desktop's widgets.
 */

/*
 * The desktop's preferences (ws089-p007, KL_VERSION 13) are gone: the
 * desktop's settings are kl_settings_* below (WS135), and desktop.conf is
 * the compositor's own file.
 */

/*
 * The desktop surface (KL_VERSION 14, ws094-p003).
 *
 * The compositor starts the program that shows the icons of ~/Desktop with a
 * token in its environment (KEILAND_DESKTOP_TOKEN); with the token, the
 * program's surface lies over the wallpaper and under every window, on
 * every virtual desktop, has no window of its own and hears the pointer,
 * the keyboard, the touch screen and drag and drop where no window is.
 * The program copies the token and takes it out of its environment before
 * it starts anything, so that no program it starts can take the role.
 */
struct kl_desktop;

/*
 * What the desktop surface hears: configure, the place on the output
 * (x, y) and the size the surface is to have; the program acknowledges it
 * (kl_desktop_ack) and draws at that size.
 */
struct kl_desktop_listener {
	void (*configure)(void *data, struct kl_desktop *desktop, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
};

/*
 * Gives a surface the desktop's role with the token.  Returns NULL with
 * errno set: EINVAL without a token, ENOTSUP for a compositor without the
 * desktop, ENOMEM.  A token the compositor does not know ends the
 * connection.
 */
struct kl_desktop *kl_desktop_create(struct wl_display *display, struct wl_surface *surface, const char *token, const struct kl_desktop_listener *listener, void *data);

/*
 * Acknowledges a configure: the next commit is drawn for it.
 */
void kl_desktop_ack(struct kl_desktop *desktop, uint32_t serial);

/*
 * Gives the desktop's role up.
 */
void kl_desktop_destroy(struct kl_desktop *desktop);

/*
 * The keyboard inset (KL_VERSION 18, ws102-p015).
 *
 * A window hears how much of it the on-screen keyboard covers, in the
 * window's pixels from its right edge (the flick panel's column) and from
 * its bottom edge (the QWERTY row), when the keyboard opens, closes or
 * changes the window's size or place (before that configure).  The window
 * may then keep what matters -- the caret -- where it can be seen.  Both
 * are 0 when the keyboard has closed or does not cover the window.
 */
struct kl_keyboard_inset;
struct xdg_toplevel;

/* Why the inset changed: no keyboard, the right column's, the bottom row's. */
#define KL_KEYBOARD_INSET_NONE	0U
#define KL_KEYBOARD_INSET_RIGHT	1U
#define KL_KEYBOARD_INSET_BOTTOM	2U

/* The application's callback: the covered widths from the right and bottom edges, and the reason. */
typedef void (*kl_keyboard_inset_fn)(void *data, int32_t right, int32_t bottom, uint32_t reason);

/*
 * Asks for a window's keyboard inset; callback runs on the application's
 * default queue.  Returns NULL with errno set: ENOTSUP for a compositor
 * without it (nothing more to do), EINVAL, ENOMEM.
 */
struct kl_keyboard_inset *kl_keyboard_inset_create(struct wl_display *display, struct xdg_toplevel *toplevel, kl_keyboard_inset_fn callback, void *data);

/*
 * Stops hearing the keyboard.
 */
void kl_keyboard_inset_destroy(struct kl_keyboard_inset *inset);

/*
 * The editing operations (KL_VERSION 19, ws102-p017).
 *
 * A window says which editing operations it carries out and its state --
 * whether it has a selection, something to paste, something to undo or to
 * redo, whether a selection is being made -- and hears the operations the
 * on-screen keyboard's buttons ask for (the buttons are grey when the
 * state rules one out).  A window without it is sent the keys instead
 * (Ctrl+C, X, V, Z, Y, A).
 */
struct kl_edit;

/* The operations, as the callback hears them; a window's operations are the bits 1 << operation. */
#define KL_EDIT_COPY		0U
#define KL_EDIT_CUT		1U
#define KL_EDIT_PASTE		2U
#define KL_EDIT_UNDO		3U
#define KL_EDIT_REDO		4U
#define KL_EDIT_SELECT_ALL		5U
#define KL_EDIT_SELECT_BEGIN	6U
#define KL_EDIT_SELECT_END		7U

/* The state's bits. */
#define KL_EDIT_HAS_SELECTION	1U
#define KL_EDIT_CAN_PASTE		2U
#define KL_EDIT_CAN_UNDO		4U
#define KL_EDIT_CAN_REDO		8U
#define KL_EDIT_SELECTING		16U

/* The application's callback: the operation asked for. */
typedef void (*kl_edit_fn)(void *data, uint32_t operation);

/*
 * Asks for a window's edit object; callback runs on the application's
 * default queue.  Returns NULL with errno set: ENOTSUP for a compositor
 * without it, EINVAL, ENOMEM.
 */
struct kl_edit *kl_edit_create(struct wl_display *display, struct xdg_toplevel *toplevel, kl_edit_fn callback, void *data);

/*
 * Says the operations the window carries out (bits 1 << KL_EDIT_*) and
 * its state (KL_EDIT_HAS_SELECTION ...); an unchanged pair is not sent.
 */
void kl_edit_set_state(struct kl_edit *edit, uint32_t operations, uint32_t state);

/*
 * Stops taking operations.
 */
void kl_edit_destroy(struct kl_edit *edit);

/*
 * The desktop's settings (WS135, plan/ws135/design.md section 3): every
 * application reads, changes and watches them here, and opens no settings
 * file itself.  Each key is resolved where it lives: the compositor's
 * (the wallpaper, the windows' opacity, the pointer, the keyboards'
 * repeat, the sound) through Keiland's system extension, an application's
 * own (terminal.*) in its file under ~/.config/keiland.  The application
 * cannot tell the two apart.
 *
 * Changes are watched, not polled: the compositor tells every client each
 * change, and kl_settings_dispatch, called after the display's events are
 * read, runs the watches.  One thread uses one kl_settings.
 */

/* The longest key and value, with their NUL. */
#define KL_SETTINGS_KEY_MAX	64U
#define KL_SETTINGS_VALUE_MAX	256U

/* A value the user did not choose: its resolver's default. */
#define KL_SETTINGS_DEFAULT	0x1U

/*
 * One application's view of the desktop's settings: the compositor's
 * values it was told, its own file's, its watches and its requests not
 * answered yet.  It lives from kl_settings_open to kl_settings_close.
 */
struct kl_settings;

/*
 * Called from kl_settings_dispatch for a key whose value changed; value is
 * NULL when the key has none now (not reported yet, or the compositor
 * went).
 */
typedef void (*kl_settings_watch_fn)(void *data, const char *key, const char *value, unsigned flags);

/*
 * Opens the settings on a display (app names the application's own
 * settings, "terminal" for terminal.*; NULL for none).  It waits once for
 * the compositor's settings.  Returns NULL with errno ENOMEM; without
 * Keiland's extension it opens all the same and the compositor's keys
 * answer ENOTSUP.
 */
struct kl_settings *kl_settings_open(struct wl_display *display, const char *app);

/*
 * Closes the settings.
 */
void kl_settings_close(struct kl_settings *settings);

/*
 * Copies a key's value and its flags (flags may be NULL).  Returns 0,
 * ENOENT for a key the desktop does not have, ENOTSUP for a compositor's
 * key without the compositor's extension, EAGAIN while it is not reported
 * yet (the sound before audiod), or ERANGE when it does not fit.
 */
int kl_settings_get(const struct kl_settings *settings, const char *key, char *value, size_t size, unsigned *flags);

/*
 * Reports a key's value as a whole number; fallback when it is not known
 * or not a number.
 */
int kl_settings_get_int(const struct kl_settings *settings, const char *key, int fallback);

/*
 * Asks for a key to take a value; request (may be NULL) names the answer
 * kl_settings_take_result gives.  The value comes back as a change.
 * Returns 0 when asked, ENOENT, ENOTSUP, EPERM (a key only reported),
 * EINVAL (a value outside the key's type or range), or an errno value of
 * the application's file.
 */
int kl_settings_set(struct kl_settings *settings, const char *key, const char *value, uint32_t *request);

/*
 * Asks for a key to take a whole number, as kl_settings_set.
 */
int kl_settings_set_int(struct kl_settings *settings, const char *key, int value, uint32_t *request);

/*
 * Asks for a key to go back to its default, as kl_settings_set.
 */
int kl_settings_reset(struct kl_settings *settings, const char *key, uint32_t *request);

/*
 * Watches the keys that start with prefix ("" for every key); *watch (may
 * be NULL) names the watch for kl_settings_unwatch.  Returns 0, EINVAL or
 * ENOMEM.
 */
int kl_settings_watch(struct kl_settings *settings, const char *prefix, kl_settings_watch_fn fn, void *data, unsigned *watch);

/*
 * Stops a watch (also from within a watch's callback).
 */
void kl_settings_unwatch(struct kl_settings *settings, unsigned watch);

/*
 * Takes the compositor's events the display has read, then runs the
 * watches of the keys that changed.  It never waits.  Returns 0, or EPIPE
 * once the compositor went (its keys answer ENOTSUP from then on).
 */
int kl_settings_dispatch(struct kl_settings *settings);

/*
 * Takes one finished request: 1 with its number and its error (0, EPERM,
 * ENOTSUP, EBUSY, EINVAL, ENODEV, EIO), 0 when none is finished.
 */
int kl_settings_take_result(struct kl_settings *settings, uint32_t *request, int *error);

/*
 * The desktop's system for applications (WS131 p010, plan/ws131/design.md
 * section 4.4): the network, the sound, the power and the removable
 * devices, as the compositor holds them through Keiland's system
 * extension.  An application asks the compositor and never reaches a
 * daemon or the operating system itself.
 *
 * The state is the compositor's, told as one state at a time: an
 * application sees the state before a change or after it, never half of
 * it.  A request is answered once, by a result kl_system_take_result
 * gives; a change it makes comes as a new state.  One network request is
 * outstanding at a time, the system bar's included; another is answered
 * EBUSY.  One thread uses one kl_system.
 */

/* The longest SSID, interface name and dotted IPv4 address, with their NULs. */
#define KL_NETWORK_SSID_MAX	33U
#define KL_NETWORK_NAME_MAX	16U
#define KL_NETWORK_ADDRESS_MAX	16U

/* The most networks a scan, interfaces, DNS servers and saved networks a kl_system keeps. */
#define KL_NETWORK_SCAN_MAX	24U
#define KL_NETWORK_LINKS_MAX	16U
#define KL_NETWORK_DNS_MAX	4U
#define KL_NETWORK_SAVED_MAX	24U

/* A WPA key's length. */
#define KL_NETWORK_KEY_MIN	8U
#define KL_NETWORK_KEY_MAX	63U

/* What carries the connection. */
#define KL_NETWORK_NONE		0U
#define KL_NETWORK_WIRED	1U
#define KL_NETWORK_WIFI		2U

/* The Wi-Fi's state. */
#define KL_WIFI_ABSENT		0U	/* no radio */
#define KL_WIFI_OFF		1U
#define KL_WIFI_SEARCHING	2U
#define KL_WIFI_CONNECTING	3U
#define KL_WIFI_CONNECTED	4U
#define KL_WIFI_DISCONNECTED	5U	/* on, and left unconnected by the user */

/* The network's requests (kl_system_network_request). */
#define KL_NETWORK_SCAN		1U
#define KL_NETWORK_JOIN		2U	/* names the network; its key must be saved */
#define KL_NETWORK_DISCONNECT	3U
#define KL_NETWORK_WIFI_ON	4U
#define KL_NETWORK_WIFI_OFF	5U

/* The power's actions, their bits in the state's actions, and where the power comes from. */
#define KL_POWER_POWEROFF	1U
#define KL_POWER_REBOOT		2U
#define KL_POWER_SUSPEND	3U
#define KL_POWER_ACTION_BIT(action)	(1U << (action))
#define KL_POWER_SOURCE_UNKNOWN	0U
#define KL_POWER_SOURCE_AC	1U
#define KL_POWER_SOURCE_BATTERY	2U

/* The longest device ID, name and location, with their NULs, and the most devices kept. */
#define KL_DEVICE_TEXT_MAX	64U
#define KL_DEVICES_MAX		16U

/* What the compositor offers (kl_system_capabilities). */
#define KL_SYSTEM_HAS_NETWORK	0x2U
#define KL_SYSTEM_HAS_AUDIO	0x4U
#define KL_SYSTEM_HAS_POWER	0x8U
#define KL_SYSTEM_HAS_DEVICES	0x10U
#define KL_SYSTEM_HAS_MONITOR	0x20U	/* kl_system_monitor_open (WS134 p012) */
#define KL_SYSTEM_HAS_ACCOUNT	0x40U	/* kl_system_account_set_password (KL_VERSION 27, ws160-p002) */
#define KL_SYSTEM_HAS_SHARING	0x80U	/* kl_system_sharing_* (KL_VERSION 30, ws089-p025) */
#define KL_SYSTEM_HAS_ADMINISTER	0x100U	/* kl_system_account_administer (KL_VERSION 31, ws089-p026) */
#define KL_SYSTEM_HAS_PIN	0x200U	/* kl_system_account_set_pin (KL_VERSION 34, ws163-p003) */
#define KL_SYSTEM_HAS_NOTIFY	0x400U	/* kl_system_notify (KL_VERSION 49, ws156-p002) */
#define KL_SYSTEM_HAS_KEYS	0x800U	/* kl_system_account_add_key and the keys (KL_VERSION 52, ws172-p003) */
#define KL_SYSTEM_HAS_MAIL	0x1000U	/* kl_system_mail_arrived and kl_system_mail_listen (KL_VERSION 54, ws169-p002) */
#define KL_SYSTEM_HAS_PHONE	0x2000U	/* kl_system_phone_send and kl_system_phone_call (KL_VERSION 55, ws170-p004) */
#define KL_SYSTEM_HAS_PRINTERS	0x4000U	/* kl_system_printers_* and kl_system_print_* (KL_VERSION 56, ws145-p003) */
#define KL_SYSTEM_HAS_DISPLAYS	0x8000U	/* kl_system_displays_* (KL_VERSION 58, ws113-p005) */
#define KL_SYSTEM_HAS_MACHINE	0x10000U	/* kl_system_machine_* (KL_VERSION 69, ws188-p002) */
#define KL_SYSTEM_HAS_BLUETOOTH	0x20000U	/* kl_system_bluetooth_* (KL_VERSION 72, ws143-p006) */
#define KL_SYSTEM_HAS_KEY_OPS	0x40000U	/* kl_system_account_key_info, _key_pin, _key_reset (KL_VERSION 77, ws199-p001) */
#define KL_SYSTEM_HAS_METHODS	0x80000U	/* kl_system_account_methods and _set_methods (KL_VERSION 78, WS200) */
#define KL_SYSTEM_HAS_PHONE_SYNC	0x100000U	/* kl_system_phone_listen, _link, _watch_link, _link_set, _sync, _page_end, _send_text, _mark_read, kl_system_take_phone_item (KL_VERSION 79, ws197-p004a) */

/* What a kl_system_dispatch found changed. */
#define KL_SYSTEM_CHANGED_NETWORK	0x1U	/* the network's state */
#define KL_SYSTEM_CHANGED_SCAN		0x2U	/* the networks of the scan */
#define KL_SYSTEM_CHANGED_DETAILS	0x4U	/* the interfaces, DNS servers and saved networks asked for */
#define KL_SYSTEM_CHANGED_AUDIO		0x8U
#define KL_SYSTEM_CHANGED_POWER		0x10U
#define KL_SYSTEM_CHANGED_DEVICES	0x20U
#define KL_SYSTEM_CHANGED_RESULT	0x40U	/* a request was answered */
#define KL_SYSTEM_CHANGED_SHARING	0x80U	/* Remote Login's state (KL_VERSION 30) */
#define KL_SYSTEM_CHANGED_ENROLLED	0x100U	/* the user's PIN and security keys (KL_VERSION 36) */
#define KL_SYSTEM_CHANGED_NOTIFY	0x200U	/* a notification's event (KL_VERSION 49) */
#define KL_SYSTEM_CHANGED_TOUCH	0x400U	/* a security key waits to be touched for an addition (KL_VERSION 52) */
#define KL_SYSTEM_CHANGED_MAIL	0x800U	/* a message arrived for a listener (KL_VERSION 54) */
#define KL_SYSTEM_CHANGED_PHONE	0x1000U	/* a phone's message came, or a message's or call's state (KL_VERSION 55) */
#define KL_SYSTEM_CHANGED_PRINTERS	0x2000U	/* the printers or the print jobs (KL_VERSION 56) */
#define KL_SYSTEM_CHANGED_DISPLAYS	0x4000U	/* the displays, their mode or a light (KL_VERSION 58) */
#define KL_SYSTEM_CHANGED_MACHINE	0x8000U	/* an answer of kl_system_machine_query (KL_VERSION 69) */
#define KL_SYSTEM_CHANGED_BLUETOOTH	0x10000U	/* Bluetooth's state or its devices (KL_VERSION 72) */
#define KL_SYSTEM_CHANGED_KEYS		0x20000U	/* a security key came or went, or the screen was unlocked (KL_VERSION 77) */
#define KL_SYSTEM_CHANGED_REPLUG	0x40000U	/* a key's reset waits for the key to be plugged in again (KL_VERSION 77) */

/*
 * The network: whether the daemon is reached, whether the machine is
 * connected and through what and which interface, the wired interface up
 * with an address (empty when none), and the Wi-Fi's state, interface and
 * the network it is on or joining.
 */
struct kl_network_state {
	unsigned reachable;
	unsigned connected;
	unsigned kind;
	char interface[KL_NETWORK_NAME_MAX];
	char wired[KL_NETWORK_NAME_MAX];
	unsigned wifi;
	char wifi_interface[KL_NETWORK_NAME_MAX];
	char ssid[KL_NETWORK_SSID_MAX];
};

/* One network a scan found: its SSID, its signal in dBm, and whether it asks for a key. */
struct kl_network_ap {
	char ssid[KL_NETWORK_SSID_MAX];
	int rssi;
	unsigned secured;
};

/*
 * One interface: its name, whether it is up, has its link and is the
 * loopback, its IPv4 address and netmask (empty when none), its hardware
 * address as text, its MTU, and the bytes it has received and sent; for a
 * wired one (KL_VERSION 29) how it is configured (KL_WIRED_*, unknown for
 * any other or an older compositor) and the router it was given;
 * (KL_VERSION 39) its link's speed in Mb/s, 0 while it is not known (or
 * from an older compositor); and (KL_VERSION 76) whether it is a radio
 * (Wi-Fi), as the system tells it (0 from an older compositor).
 */
struct kl_network_link {
	char name[KL_NETWORK_NAME_MAX];
	unsigned up;
	unsigned running;
	unsigned loopback;
	char address[KL_NETWORK_ADDRESS_MAX];
	char netmask[KL_NETWORK_ADDRESS_MAX];
	char hardware[18];
	unsigned mtu;
	uint64_t received_bytes;
	uint64_t sent_bytes;
	unsigned wired_mode;
	char router[KL_NETWORK_ADDRESS_MAX];
	unsigned link_mbps;
	unsigned wireless;
};

/* How a wired interface is configured (KL_VERSION 29). */
#define KL_WIRED_UNKNOWN	0U
#define KL_WIRED_DHCP		1U
#define KL_WIRED_STATIC		2U

/*
 * A wired interface's configuration asked for (KL_VERSION 29): its name,
 * DHCP or a static IPv4 address (KL_WIRED_*), the address, netmask and
 * router of a static one (empty otherwise; the router may be empty), and
 * up to two DNS servers (empty: the servers DHCP gives).
 */
struct kl_network_wired_config {
	char interface[KL_NETWORK_NAME_MAX];
	unsigned mode;
	char address[KL_NETWORK_ADDRESS_MAX];
	char netmask[KL_NETWORK_ADDRESS_MAX];
	char router[KL_NETWORK_ADDRESS_MAX];
	char dns[2][KL_NETWORK_ADDRESS_MAX];
};

/*
 * The sound output: whether the sound service is reached and has a
 * device, the device's rate and channels, the volume of each channel
 * (0 to 100) and whether it is muted.
 */
struct kl_audio_state {
	unsigned reachable;
	unsigned device;
	unsigned rate;
	unsigned channels;
	unsigned left;
	unsigned right;
	unsigned muted;
};

/*
 * The power: where it comes from, the battery's charge in percent (-1 when
 * unknown), whether it charges, and the KL_POWER_ACTION_BIT of each action
 * the user may take now.
 */
struct kl_power_state {
	unsigned source;
	int percent;
	unsigned charging;
	unsigned actions;
};

/*
 * One removable device (KL_VERSION 28, ws132-p004: a volume volumed lists):
 * its ID (the disk's name), kind (KL_DEVICE_STORAGE), state (the
 * KL_DEVICE_* bits: mounted; new, inserted and never mounted since), name
 * (the label, or the disk's name) and where it is mounted ("" when not).
 */
#define KL_DEVICE_STORAGE	1U
#define KL_DEVICE_MOUNTED	0x1U
#define KL_DEVICE_NEW		0x2U

struct kl_device {
	char id[KL_DEVICE_TEXT_MAX];
	unsigned kind;
	unsigned state;
	char name[KL_DEVICE_TEXT_MAX];
	char location[KL_DEVICE_TEXT_MAX];
};

/*
 * A removable device's file system ("fat", "ufs"; "" when the compositor
 * did not tell) and size in bytes (0 when not told), KL_VERSION 33
 * (ws132-p009): what a mount's confirmation shows.
 */
#define KL_DEVICE_FS_MAX	8U

struct kl_device_info {
	char fs[KL_DEVICE_FS_MAX];
	uint64_t bytes;
};

/*
 * One application's view of the system: the state the compositor told,
 * and the requests not answered yet.  It lives from kl_system_open to
 * kl_system_close.
 */
struct kl_system;

/*
 * Opens the system on a display and waits once for its first state.
 * Returns NULL with errno ENOTSUP without Keiland's system extension (not
 * Keiland, or another user's compositor), EPIPE when the compositor went,
 * or ENOMEM.
 */
struct kl_system *kl_system_open(struct wl_display *display);

/*
 * Closes the system.
 */
void kl_system_close(struct kl_system *system);

/*
 * Takes the compositor's events the display has read; *changed (may be
 * NULL) has the KL_SYSTEM_CHANGED_* bits of what changed since the last
 * dispatch.  It never waits.  Returns 0, or EPIPE once the compositor went.
 */
int kl_system_dispatch(struct kl_system *system, unsigned *changed);

/*
 * Reports the KL_SYSTEM_HAS_* bits of what the compositor offers.
 */
unsigned kl_system_capabilities(const struct kl_system *system);

/*
 * Takes one answered request: 1 with its number and its error (0, EPERM,
 * ENOTSUP, EBUSY, EINVAL, ENODEV, EIO; a join's and a save_key's own
 * ENOENT: no key saved, EACCES: the network refused the key, ENETUNREACH:
 * the network is out of reach), 0 when none is answered.
 */
int kl_system_take_result(struct kl_system *system, uint32_t *request, int *error);

/*
 * Copies the network's state.
 */
void kl_system_network_get_state(const struct kl_system *system, struct kl_network_state *state);

/*
 * Copies up to capacity networks of the last scan, the strongest first,
 * and returns how many were copied.
 */
size_t kl_system_network_get_scan(const struct kl_system *system, struct kl_network_ap *aps, size_t capacity);

/*
 * Asks for a network request (KL_NETWORK_*; a join names the SSID, the
 * others take NULL); request (may be NULL) names its answer.  Returns 0
 * when asked, ENOTSUP, or EINVAL.
 */
int kl_system_network_request(struct kl_system *system, unsigned what, const char *ssid, uint32_t *request);

/*
 * Asks for a Wi-Fi network's key to be saved in the user's store and the
 * network joined, answered once it is joined or it failed.  Returns 0 when
 * asked, ENOTSUP, or EINVAL (an SSID or key outside its bounds).  The key
 * is not kept.
 */
int kl_system_network_save_key(struct kl_system *system, const char *ssid, const char *key, uint32_t *request);

/*
 * Asks for the network's details: the interfaces, the DNS servers and the
 * saved networks come (KL_SYSTEM_CHANGED_DETAILS) before the answer.  It
 * is no request of the network daemon's and is asked alongside one.
 * Returns 0 when asked, or ENOTSUP.
 */
int kl_system_network_query_details(struct kl_system *system, uint32_t *request);

/*
 * Asks the compositor to keep the radios scanning (on 1) while the
 * application shows the networks around, and no longer (on 0) when it
 * stops showing them (ws089-p021).  It is no request and has no answer: a
 * new scan comes as KL_SYSTEM_CHANGED_SCAN.  The compositor counts every
 * application that asked (and its own Wi-Fi menu), and an application that
 * closes its system or ends asks no longer.  One asking holds a minute: an
 * application that shows the networks longer asks again (every 30 seconds,
 * say), so that one that hangs does not keep the radios scanning.  Returns
 * 0, or ENOTSUP when
 * the compositor does not know it (one older than KL_VERSION 24).
 */
int kl_system_network_set_scanning(struct kl_system *system, unsigned on);

/*
 * Asks for a wired interface to be configured (KL_VERSION 29, ws089-p022):
 * DHCP or a static IPv4 address, with its router and DNS servers; the
 * network daemon keeps it for the next start too.  request (may be NULL)
 * names its answer, which comes once it is applied or refused.  Returns 0
 * when asked, ENOTSUP (an older compositor), or EINVAL (a field that is
 * too long, or a mode that is neither).
 */
int kl_system_network_configure_wired(struct kl_system *system, const struct kl_network_wired_config *config, uint32_t *request);

/*
 * Remote Login (KL_VERSION 30, ws089-p025): whether the system has it,
 * whether it starts with the system and runs now, its port, whether this
 * user may change it (root or a member of wheel), whether the state was
 * ever read, and the host key's fingerprint ("SHA256:...", or empty).
 */
#define KL_SHARING_FINGERPRINT_MAX	64U
struct kl_sharing_state {
	unsigned available;
	unsigned enabled;
	unsigned running;
	unsigned port;
	unsigned allowed;
	char fingerprint[KL_SHARING_FINGERPRINT_MAX];
};

/* Copies Remote Login's state (all zero without KL_SYSTEM_HAS_SHARING). */
void kl_system_sharing_get_state(const struct kl_system *system, struct kl_sharing_state *state);

/*
 * Turns Remote Login on (on 1) or off (0), now and at every start; the
 * answer names request and comes after the new state.  Returns 0 when
 * asked, or ENOTSUP without KL_SYSTEM_HAS_SHARING.  The answer is EPERM
 * for a user who may not.
 */
int kl_system_sharing_set_ssh(struct kl_system *system, unsigned on, uint32_t *request);

/* Reads Remote Login's state again (KL_SYSTEM_CHANGED_SHARING follows).  Returns 0 or ENOTSUP. */
int kl_system_sharing_query(struct kl_system *system, uint32_t *request);

/*
 * KL_VERSION 49 (ws156-p002, plan/ws156/phase001/phase.md section 2): a
 * notification shown at the bottom of the screen and kept in the desktop's
 * log.  app is the name shown (NULL: the window's application ID), title
 * and body its words (at most 64, 128 and 512 bytes of UTF-8), replaces an
 * earlier notification's number to give it new words (0 for a new one),
 * flags KL_NOTIFY_URGENT (shown over a fullscreen window) and
 * KL_NOTIFY_ACTION (a click of its body is told).  kl_system_notify asks;
 * the answer comes as a notification event: KL_NOTIFY_POSTED with its
 * number for the request, or the request's result (KL_SYSTEM_CHANGED_RESULT:
 * EINVAL for words too long, EBUSY when the application has 32 already).
 * Later events: KL_NOTIFY_ACTIVATED (its body clicked) and KL_NOTIFY_CLOSED
 * with why (KL_NOTIFY_DISMISSED, _EXPIRED out of the log, _CLEARED, _WITHDRAWN).
 * kl_system_notify_withdraw takes one back.  Returns 0 when asked, ENOTSUP
 * without KL_SYSTEM_HAS_NOTIFY, or EINVAL.  KL_VERSION 65 (ws177-p005):
 * when the events come faster than they are taken and the library's ring
 * of 32 drops the oldest, the next take gives a KL_NOTIFY_LOST first, its
 * id the number dropped; a post faster than the desktop's rate (10 a
 * second) is refused as EBUSY, and words that are not UTF-8 or hold
 * control characters are shown mended (U+FFFD, a space).
 */
#define KL_NOTIFY_URGENT	0x1U
#define KL_NOTIFY_ACTION	0x2U
#define KL_NOTIFY_POSTED	1U
#define KL_NOTIFY_ACTIVATED	2U
#define KL_NOTIFY_CLOSED	3U
#define KL_NOTIFY_LOST		4U
#define KL_NOTIFY_DISMISSED	1U
#define KL_NOTIFY_EXPIRED	2U
#define KL_NOTIFY_CLEARED	3U
#define KL_NOTIFY_WITHDRAWN	4U
struct kl_notification {
	const char *app;
	const char *title;
	const char *body;
	uint32_t replaces;
	unsigned flags;
};
struct kl_notify_event {
	unsigned kind;
	uint32_t request;
	uint32_t id;
	unsigned reason;
};
int kl_system_notify(struct kl_system *system, const struct kl_notification *notification, uint32_t *request);
int kl_system_notify_withdraw(struct kl_system *system, uint32_t id, uint32_t *request);
int kl_system_take_notify_event(struct kl_system *system, struct kl_notify_event *event);

/*
 * KL_VERSION 54 (ws169-p002, plan/ws169/phase001/phase.md section 1): the
 * arrivals of mail.  The mail program tells the compositor of a new
 * message with kl_system_mail_arrived: the account's name, the sender, the
 * subject and a sign-in code found in it (empty or NULL for none; the body
 * is never sent).  A reader (the browser) asks with kl_system_mail_listen
 * under its name; the compositor takes only a name its settings know
 * (mail.codes.<app>, EINVAL as the request's result otherwise) and tells
 * a message to the reader only while the user has that setting on.  Each
 * message told is a KL_SYSTEM_CHANGED_MAIL and one kl_system_take_mail_event.
 * The strings are cut to KL_MAIL_TEXT_MAX and KL_MAIL_CODE_MAX bytes with
 * their NUL.  Returns 0 when asked, ENOTSUP without KL_SYSTEM_HAS_MAIL, or
 * EINVAL.
 */
#define KL_MAIL_TEXT_MAX	256U
#define KL_MAIL_CODE_MAX	16U
struct kl_mail_arrival {
	const char *account;
	const char *from;
	const char *subject;
	const char *code;
};
struct kl_mail_event {
	char from[KL_MAIL_TEXT_MAX];
	char subject[KL_MAIL_TEXT_MAX];
	char code[KL_MAIL_CODE_MAX];
};
int kl_system_mail_arrived(struct kl_system *system, const struct kl_mail_arrival *arrival, uint32_t *request);
int kl_system_mail_listen(struct kl_system *system, const char *app, uint32_t *request);
int kl_system_take_mail_event(struct kl_system *system, struct kl_mail_event *event);

/*
 * KL_VERSION 65 (ws177-p005): whether the user lets this reader hear the
 * arrivals now, told after its listen is taken and whenever the setting
 * changes (each a KL_SYSTEM_CHANGED_MAIL): 1 allowed, 0 not, -1 not told
 * yet.  Since then only the mail program (its windows' app_id "mailer")
 * is heard by kl_system_mail_arrived; another program's is refused
 * (EINVAL as the request's result).
 */
int kl_system_mail_allowed(const struct kl_system *system);

/*
 * KL_VERSION 55 (ws170-p004, plan/ws170/phase001/phase.md section 3): the
 * phone.  kl_system_phone_send sends a message on a channel (KL_PHONE_SMS,
 * _MMS, _RCS) to a number, kl_system_phone_call calls one (KL_PHONE_LINE,
 * _VOIP); the compositor's backend, which the desktop's setting
 * phone.backend chooses, carries them out.  The request's result is
 * ENODEV without a backend.  What follows comes as phone events, each a
 * KL_SYSTEM_CHANGED_PHONE: KL_PHONE_STATUS with the request and its state
 * (KL_PHONE_SENT, _DELIVERED, _FAILED, _ANSWERED, _NO_ANSWER), and
 * KL_PHONE_RECEIVED with a message that came (to every application of the
 * user that has the phone).  Numbers are cut to KL_PHONE_NUMBER_MAX and
 * words to KL_PHONE_TEXT_MAX bytes with their NUL.  Returns 0 when asked,
 * ENOTSUP without KL_SYSTEM_HAS_PHONE, or EINVAL.
 */
#define KL_PHONE_SMS		0U
#define KL_PHONE_MMS		1U
#define KL_PHONE_RCS		2U
#define KL_PHONE_LINE		3U
#define KL_PHONE_VOIP		4U
#define KL_PHONE_RECEIVED	1U
#define KL_PHONE_STATUS		2U
#define KL_PHONE_SENT		1U
#define KL_PHONE_DELIVERED	2U
#define KL_PHONE_FAILED		3U
#define KL_PHONE_ANSWERED	4U
#define KL_PHONE_NO_ANSWER	5U
#define KL_PHONE_NUMBER_MAX	64U
#define KL_PHONE_TEXT_MAX	1024U
struct kl_phone_event {
	unsigned kind;
	uint32_t request;
	unsigned state;
	unsigned channel;
	char from[KL_PHONE_NUMBER_MAX];
	char text[KL_PHONE_TEXT_MAX];
	uint64_t time;
};
int kl_system_phone_send(struct kl_system *system, unsigned channel, const char *to, const char *text, uint32_t *request);
int kl_system_phone_call(struct kl_system *system, unsigned channel, const char *to, uint32_t *request);
int kl_system_take_phone_event(struct kl_system *system, struct kl_phone_event *event);

/*
 * KL_VERSION 79 (ws197-p004a, plan/ws197/phase004/phase.md section 3): the
 * paired phone's messages (Bluetooth's MAP), for the phone program (its
 * windows' app_id "phone") only; the phone program keeps them, nothing
 * else does.
 *
 * kl_system_phone_listen(1) makes this program hear what comes: items (a
 * message that came by itself, request 0), the link's changes and the
 * drops; the link's state is told once at once (why "not-phone" for
 * another program).  kl_system_phone_sync asks a page of the
 * synchronisation (the messages since a time, at most limit a folder (0:
 * all), from a cursor ("" at the start), count items (1 to 32); one at a
 * time a program): its items come numbered by the request, then its end
 * (kl_system_phone_page_end: where the next starts, whether more follow,
 * how many items came, were skipped, whether a folder's limit stopped it),
 * then its result.  kl_system_phone_send_text sends a text (1 to
 * KL_PHONE_SEND_MAX bytes, no NUL) to a number (the separators "-", " ",
 * "(", ")" and "." are taken out; then 1 to 32 of the digits, '+', '*' and
 * '#'): its result 0 says the phone's outbox has it, and KL_PHONE_STATUS
 * events say it was sent, delivered or failed.  kl_system_phone_mark_read
 * marks a message read on the phone by the handle of its item.
 * kl_system_phone_watch_link and kl_system_phone_link_set are Settings':
 * the link's changes alone, and the phone's switch and its profiles
 * (KL_PHONE_PROFILE_*).
 *
 * Every request is answered once by its result (kl_system_take_result):
 * 0, ENODEV without a backend, ENOTSUP, ENOTCONN (the phone's messages
 * are not connected), EACCES (not the phone's owner, or not the phone
 * program), EINVAL, EBUSY, ESTALE (a cursor or a handle of an earlier
 * session), ECONNRESET (lost on the way: a text may or may not have gone),
 * ETIMEDOUT, EMSGSIZE, ENOENT (not on the phone), ENOBUFS (this program
 * read too little and items were lost) or EIO.  A sync not answered in 180
 * seconds lets the next one start.
 *
 * Each item, link change and drop is a KL_SYSTEM_CHANGED_PHONE; the phone
 * events tell KL_PHONE_ITEMS (items wait: take them all with
 * kl_system_take_phone_item until it gives 0, also after any
 * KL_SYSTEM_CHANGED_PHONE), KL_PHONE_LINK_CHANGED (kl_system_phone_link
 * copies the state) and KL_PHONE_DROPPED (items or events were lost:
 * synchronise again).  An item's text lives in the library until the next
 * kl_system_take_phone_item.  The structures are passed with their size,
 * so that a later version adds fields at their end.
 */
#define KL_PHONE_MESSAGES		0U
#define KL_PHONE_ITEMS			10U
#define KL_PHONE_LINK_CHANGED		11U
#define KL_PHONE_DROPPED		12U
#define KL_PHONE_SEND_MAX		8192U
#define KL_PHONE_ITEM_TEXT_MAX		16384U
#define KL_PHONE_HANDLE_MAX		32U
#define KL_PHONE_KEY_MAX		20U
#define KL_PHONE_CURSOR_MAX		64U
#define KL_PHONE_PEER_MAX		132U
#define KL_PHONE_DATETIME_MAX		24U
#define KL_PHONE_ADDRESS_MAX		18U
#define KL_PHONE_WHY_MAX		32U
#define KL_PHONE_PROFILE_MESSAGES	1U
#define KL_PHONE_PROFILE_CONTACTS	2U
#define KL_PHONE_PROFILE_CALLS		4U

/*
 * One message: the sync's request (0 for one that came by itself), what
 * (KL_PHONE_MESSAGES), its handle for kl_system_phone_mark_read (good for
 * the phone's session only: not to be kept), its key (16 hexadecimal
 * digits, the same across sessions, or "-"), its folder (0 inbox, 1 sent)
 * and direction (0 in, 1 out), its time in UNIX seconds and where that
 * came from (0 the phone, 1 the phone's zone, 2 this computer's zone, 3
 * when it came), the phone's datetime as written, the other side's number
 * and name (empty when none), whether it is read, has no key, or was cut
 * to KL_PHONE_ITEM_TEXT_MAX bytes, and its text (UTF-8, ended by a NUL)
 * and length.
 */
struct kl_phone_item {
	uint32_t request;
	unsigned what;
	char handle[KL_PHONE_HANDLE_MAX];
	char key[KL_PHONE_KEY_MAX];
	unsigned folder;
	unsigned direction;
	int64_t time;
	unsigned zone;
	char datetime[KL_PHONE_DATETIME_MAX];
	char peer[KL_PHONE_PEER_MAX];
	char name[KL_PHONE_PEER_MAX];
	unsigned read;
	unsigned partial;
	unsigned truncated;
	const char *text;
	size_t length;
};

/*
 * The phone link's state: the backend (0 none, 1 loopback, 2 bluetooth),
 * whether the phone is linked, its messages (0 off, 1 connecting, 2 ready,
 * 3 failed), whether a text can be sent and new messages are told, whether
 * this user owns the phone, the phone's switch and its profiles
 * (KL_PHONE_PROFILE_*), whether the owner is at the seat, the phone's
 * address, and why it does not work (a word, or empty).
 */
struct kl_phone_link {
	unsigned backend;
	unsigned linked;
	unsigned messages;
	unsigned can_send;
	unsigned notify;
	unsigned owner;
	unsigned enabled;
	unsigned profiles;
	unsigned present;
	char address[KL_PHONE_ADDRESS_MAX];
	char why[KL_PHONE_WHY_MAX];
};

/* The phone's messages' calls, as told above (KL_SYSTEM_HAS_PHONE_SYNC). */
int kl_system_phone_listen(struct kl_system *system, unsigned on);
int kl_system_phone_link(const struct kl_system *system, struct kl_phone_link *link, size_t size);
int kl_system_phone_sync(struct kl_system *system, unsigned what, int64_t since, unsigned limit, const char *cursor, unsigned count, uint32_t *request);
int kl_system_take_phone_item(struct kl_system *system, struct kl_phone_item *item, size_t size);
int kl_system_phone_page_end(const struct kl_system *system, uint32_t request, char *cursor, size_t cursor_size, unsigned *more, unsigned *count, unsigned *skipped, unsigned *capped);
int kl_system_phone_send_text(struct kl_system *system, unsigned channel, const char *to, const char *text, size_t length, uint32_t *request);
int kl_system_phone_mark_read(struct kl_system *system, const char *handle, uint32_t *request);
int kl_system_phone_watch_link(struct kl_system *system, unsigned on);
int kl_system_phone_link_set(struct kl_system *system, const char *address, unsigned on, unsigned profiles, uint32_t *request);

/*
 * KL_VERSION 56 (ws145-p003, plan/ws145/design.md section 2): the printers.
 * The user's printers (an address, a port and a protocol, IPP or LPD, with
 * the IPP path or the LPD queue, "" for the usual one) are kept by the
 * compositor; kl_system_printers_get and kl_system_print_jobs_get copy the
 * printers and the jobs (the jobs not ended and the last ones ended), and
 * a change of either is KL_SYSTEM_CHANGED_PRINTERS.  kl_system_printers_print
 * prints a PDF file (its path; the library opens it and sends its
 * descriptor) on a printer (0: the default) under a title; the job's
 * number is kl_system_print_job_of's for the request once its result came.
 * Each request is answered as a result: 0, EINVAL (a printer or a job not
 * known, the same printer twice, a file that is not a PDF), EBUSY, ENOTSUP
 * or EIO.  The calls return 0 when asked, ENOTSUP without
 * KL_SYSTEM_HAS_PRINTERS, or EINVAL; print also the file's errno, EFBIG
 * for an empty file or one over 256 MiB.
 */
#define KL_PRINTER_IPP		1U
#define KL_PRINTER_LPD		2U
#define KL_PRINTER_DEFAULT	0x1U
#define KL_PRINTERS_MAX		16U
#define KL_PRINT_JOBS_MAX	32U
#define KL_PRINTER_HOST_MAX	64U
#define KL_PRINTER_PATH_MAX	64U
#define KL_PRINTER_NAME_MAX	128U
#define KL_PRINT_TITLE_MAX	128U
#define KL_PRINT_DETAIL_MAX	32U
#define KL_PRINT_QUEUED		1U
#define KL_PRINT_SENDING	2U
#define KL_PRINT_WAITING	3U
#define KL_PRINT_DONE		4U
#define KL_PRINT_FAILED		5U
#define KL_PRINT_CANCELLED	6U
struct kl_printer {
	uint32_t id;
	unsigned protocol;
	char host[KL_PRINTER_HOST_MAX];
	unsigned port;
	char path[KL_PRINTER_PATH_MAX];
	char name[KL_PRINTER_NAME_MAX];
	unsigned flags;
};
struct kl_print_job {
	uint32_t job;
	uint32_t printer;
	unsigned state;
	char title[KL_PRINT_TITLE_MAX];
	char detail[KL_PRINT_DETAIL_MAX];
};
size_t kl_system_printers_get(const struct kl_system *system, struct kl_printer *printers, size_t capacity);
size_t kl_system_print_jobs_get(const struct kl_system *system, struct kl_print_job *jobs, size_t capacity);
int kl_system_printers_add(struct kl_system *system, unsigned protocol, const char *host, unsigned port, const char *path, uint32_t *request);
int kl_system_printers_remove(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_set_default(struct kl_system *system, uint32_t printer, uint32_t *request);
int kl_system_printers_print(struct kl_system *system, uint32_t printer, const char *path, const char *title, uint32_t *request);
int kl_system_print_job_of(const struct kl_system *system, uint32_t request, uint32_t *job);
int kl_system_print_cancel(struct kl_system *system, uint32_t job, uint32_t *request);

/*
 * KL_VERSION 75 (ws177-p025): changes a printer's name and its IPP path or
 * LPD queue ("" or NULL keeps each).  The name is made one line as a
 * print's title is (control characters become spaces, cut to 127 bytes);
 * the path has no space.  The answer is a result; ENOTSUP from a desktop
 * older than the printers' edit (kl_system_manager_v1 version 24).
 */
int kl_system_printers_edit(struct kl_system *system, uint32_t printer, const char *name, const char *path, uint32_t *request);

/*
 * KL_VERSION 58 (ws113-p005, plan/ws113/phase005/phase.md): the displays.
 * The compositor shows every display connected in one of two modes, all
 * extended (each its own part of one plane) or all mirrored.
 * kl_system_displays_get copies the displays (each one's key, a label for
 * people, its place in the plane, its mode, KL_DISPLAY_* flags and the
 * light of a built-in panel in percent), kl_system_displays_mode tells the
 * mode; a change of either (a display plugged or unplugged, a choice
 * applied by anyone, a light changed, by the light keys too) is
 * KL_SYSTEM_CHANGED_DISPLAYS.  kl_system_displays_apply chooses the mode
 * and, for the extended mode, the places of the displays named (by their
 * keys; the others keep theirs); kl_system_displays_set_brightness sets
 * the light of a built-in panel (0 to 100).  Both are answered as a result
 * (kl_system_take_result): 0, ESTALE when the displays changed after the
 * copy the choice was made from (copy them again), EINVAL for places that
 * overlap, are not joined by an edge or are out of range, EPERM outside an
 * active session (the login screen, a lock), ENOTSUP for a display without
 * a light the compositor sets, or EIO.  The calls return 0 when asked,
 * ENOTSUP without KL_SYSTEM_HAS_DISPLAYS, or EINVAL.
 *
 * KL_VERSION 59 (ws113-p014): kl_system_displays_set_shown turns a
 * display off (shown 0), or on again, in the extended mode; the
 * compositor keeps the choice, and an anchor turned off gives the desktop
 * to a display on.  A display turned off carries KL_DISPLAY_OFF (in the
 * mirror too, which shows it still).  It is answered 0, EINVAL for the last
 * display on or in the mirror, ENODEV for a key not connected, EPERM
 * outside an active session, or EIO; the call returns ENOTSUP from a
 * compositor before it.
 */
#define KL_DISPLAYS_MAX			8U
#define KL_DISPLAY_KEY_MAX		64U
#define KL_DISPLAY_LABEL_MAX		64U
#define KL_DISPLAYS_EXTENDED		0U
#define KL_DISPLAYS_MIRROR		1U
#define KL_DISPLAY_INTERNAL		0x1U	/* the machine's own panel */
#define KL_DISPLAY_ANCHOR		0x2U	/* the desktop's display: the bar and the windows are on it */
#define KL_DISPLAY_SHOWN		0x4U	/* shown now */
#define KL_DISPLAY_BACKLIGHT		0x8U	/* its light can be set */
#define KL_DISPLAY_LIMITED		0x10U	/* held back: the machine shows no more displays at once */
#define KL_DISPLAY_OFF			0x20U	/* turned off by the user in the extended mode (KL_VERSION 59) */
struct kl_display {
	char key[KL_DISPLAY_KEY_MAX];
	char label[KL_DISPLAY_LABEL_MAX];
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
	unsigned flags;
	unsigned brightness;
};
struct kl_display_place {
	const char *key;
	int32_t x;
	int32_t y;
};
size_t kl_system_displays_get(const struct kl_system *system, struct kl_display *displays, size_t capacity);
unsigned kl_system_displays_mode(const struct kl_system *system);
int kl_system_displays_apply(struct kl_system *system, unsigned mode, const struct kl_display_place *places, size_t count, uint32_t *request);
int kl_system_displays_set_brightness(struct kl_system *system, const char *key, unsigned percent, uint32_t *request);
int kl_system_displays_set_shown(struct kl_system *system, const char *key, unsigned shown, uint32_t *request);

/*
 * What Settings reads of the computer (KL_VERSION 69, ws188-p002): the
 * system's names (About), the file systems' sizes (Storage), the people's
 * accounts and the program's own (Users), and the login screen's language
 * (Languages).  The compositor reads them when asked, on a thread of its
 * own, and answers the parts asked.
 *
 * kl_system_machine_query asks for the KL_MACHINE_* parts; it returns 0
 * when asked (a request number, as every request), ENOTSUP without
 * KL_SYSTEM_HAS_MACHINE, or EINVAL.  The answer is a result
 * (kl_system_take_result): 0 when the parts came, EBUSY when too many
 * queries wait, ENODEV when the program's queue had no room for the whole
 * answer, EINVAL or EIO; a part that came is a KL_SYSTEM_CHANGED_MACHINE
 * before the result, and its serial (kl_system_machine_serial) grows by
 * one, so that a program copies only the parts that changed.  A part never
 * answered is not known (kl_system_machine_known): kl_system_machine_about
 * and kl_system_machine_login_language then answer ENOENT, and the lists
 * copy none.  The lists are whole: each answer replaces the last.  A user
 * is a person's account (KL_MACHINE_USER_PERSON: a user ID from 1000 and a
 * shell to log in with), the program's own (KL_MACHINE_USER_SELF, the only
 * one with its home), or both, with whether it is an administrator and may
 * control Wi-Fi.  The login language is "en", "ja", or "" when it is not
 * set.
 */
#define KL_MACHINE_ABOUT		0x1U
#define KL_MACHINE_FILESYSTEMS		0x2U
#define KL_MACHINE_USERS		0x4U
#define KL_MACHINE_LOGIN_LANGUAGE	0x8U
#define KL_MACHINE_MOUNTS		0x10U	/* KL_VERSION 71, ws188-p004: ENOTSUP from a compositor without it */
#define KL_MACHINE_USER_PERSON		0x1U
#define KL_MACHINE_USER_SELF		0x2U
#define KL_MACHINE_USER_ADMIN		0x4U
#define KL_MACHINE_USER_NETWORK		0x8U
#define KL_MACHINE_FILESYSTEMS_MAX	8U
#define KL_MACHINE_USERS_MAX		64U
#define KL_MACHINE_MOUNTS_MAX		64U

/* The system's names (an empty one could not be read) and how many processors are online (0 not known). */
struct kl_machine_about {
	char system[128];
	char kernel[160];
	char architecture[32];
	char processor[64];
	char host[64];
	unsigned cpus;
};

/* One file system: where it is mounted and its sizes in bytes. */
struct kl_machine_filesystem {
	char path[64];
	uint64_t total;
	uint64_t available;
	uint64_t used;
};

/* One account: its name, its full name, its home (the program's own only) and its KL_MACHINE_USER_* flags. */
struct kl_machine_user {
	char name[64];
	char full_name[128];
	char home[256];
	unsigned flags;
};

/* One mounted file system a user may keep files on (KL_VERSION 71): where it is mounted and its type. */
struct kl_machine_mount {
	char path[256];
	char type[32];
};

int kl_system_machine_query(struct kl_system *system, unsigned what, uint32_t *request);
unsigned kl_system_machine_known(const struct kl_system *system);
uint32_t kl_system_machine_serial(const struct kl_system *system, unsigned part);
int kl_system_machine_about(const struct kl_system *system, struct kl_machine_about *about);
size_t kl_system_machine_filesystems(const struct kl_system *system, struct kl_machine_filesystem *list, size_t capacity);
size_t kl_system_machine_users(const struct kl_system *system, struct kl_machine_user *list, size_t capacity);
int kl_system_machine_login_language(const struct kl_system *system, char *code, size_t size);
size_t kl_system_machine_mounts(const struct kl_system *system, struct kl_machine_mount *list, size_t capacity);

/*
 * KL_VERSION 72 (ws143-p006, plan/ws143/phase006/phase.md section 6):
 * Bluetooth, which the compositor keeps (a pairing's questions are the
 * compositor's own window's, never an application's).
 * kl_system_bluetooth_state copies the state: whether the service answers,
 * the controller's state (KL_BLUETOOTH_ABSENT ... KL_BLUETOOTH_ERROR), the
 * KL_BLUETOOTH_SCANNING, _PAIRING, _POWERED (the user's switch) and
 * _ANSWERS (this desktop answers the pairings' questions; 0 while another
 * program of the user does) flags, what the service can do
 * (KL_BLUETOOTH_CAN_*), and the controller's address and name.
 * kl_system_bluetooth_devices copies the devices (the paired first, then
 * those a scan saw): address, type, name, kind (for an icon),
 * KL_BLUETOOTH_PAIRED, _LEGACY and _CONNECTED, battery in percent (-1
 * unknown) and signal in dBm (0 unknown).  Either changing is
 * KL_SYSTEM_CHANGED_BLUETOOTH.
 * kl_system_bluetooth_watch(1) has the state read often while it is shown
 * (0 when it is not); kl_system_bluetooth_scan(1) looks for the devices
 * around, and holds for a minute: ask again while they are shown, 0 when
 * they are not.  kl_system_bluetooth_power turns the controller on or off
 * (the service remembers it); kl_system_bluetooth_device pairs, forgets,
 * connects or disconnects a device (KL_BLUETOOTH_PAIR ... _DISCONNECT).
 * They are answered as a result (kl_system_take_result): 0, EPERM (not
 * allowed), EBUSY (another request or a pairing runs), EINVAL, ENOTSUP
 * (the service cannot), ENODEV (no service, or the controller is off),
 * ENETUNREACH (the device did not answer), EACCES (the device or the
 * person refused) or EIO.  The calls return 0 when asked, ENOTSUP without
 * KL_SYSTEM_HAS_BLUETOOTH, or EINVAL.
 */
#define KL_BLUETOOTH_ABSENT		0U	/* no service to ask */
#define KL_BLUETOOTH_NONE		1U	/* no controller */
#define KL_BLUETOOTH_STARTING		2U
#define KL_BLUETOOTH_OFF		3U	/* turned off by the user */
#define KL_BLUETOOTH_ON			4U
#define KL_BLUETOOTH_FIRMWARE		5U	/* its firmware is missing or did not load */
#define KL_BLUETOOTH_UNSUPPORTED	6U
#define KL_BLUETOOTH_ERROR		7U
#define KL_BLUETOOTH_SCANNING		0x1U
#define KL_BLUETOOTH_PAIRING		0x2U
#define KL_BLUETOOTH_POWERED		0x4U
#define KL_BLUETOOTH_ANSWERS		0x8U
#define KL_BLUETOOTH_CAN_POWER		0x1U
#define KL_BLUETOOTH_CAN_CONNECT	0x2U
#define KL_BLUETOOTH_BREDR		0U
#define KL_BLUETOOTH_LE_PUBLIC		1U
#define KL_BLUETOOTH_LE_RANDOM		2U
#define KL_BLUETOOTH_KIND_OTHER		0U
#define KL_BLUETOOTH_KIND_KEYBOARD	1U
#define KL_BLUETOOTH_KIND_MOUSE		2U
#define KL_BLUETOOTH_KIND_AUDIO		3U
#define KL_BLUETOOTH_KIND_PHONE		4U
#define KL_BLUETOOTH_KIND_COMPUTER	5U
#define KL_BLUETOOTH_PAIRED		0x1U
#define KL_BLUETOOTH_LEGACY		0x2U
#define KL_BLUETOOTH_CONNECTED		0x4U
#define KL_BLUETOOTH_PAIR		1U
#define KL_BLUETOOTH_FORGET		2U
#define KL_BLUETOOTH_CONNECT		3U
#define KL_BLUETOOTH_DISCONNECT		4U
#define KL_BLUETOOTH_ADDRESS_MAX	18U
#define KL_BLUETOOTH_NAME_MAX		64U
#define KL_BLUETOOTH_DEVICES_MAX	32U
struct kl_bluetooth_state {
	unsigned reachable;
	unsigned state;
	unsigned flags;
	unsigned features;
	char address[KL_BLUETOOTH_ADDRESS_MAX];
	char name[KL_BLUETOOTH_NAME_MAX];
};
struct kl_bluetooth_device {
	char address[KL_BLUETOOTH_ADDRESS_MAX];
	unsigned type;
	char name[KL_BLUETOOTH_NAME_MAX];
	unsigned kind;
	unsigned flags;
	int battery;
	int rssi;
};
int kl_system_bluetooth_state(const struct kl_system *system, struct kl_bluetooth_state *state);
size_t kl_system_bluetooth_devices(const struct kl_system *system, struct kl_bluetooth_device *devices, size_t capacity);
int kl_system_bluetooth_watch(struct kl_system *system, unsigned on);
int kl_system_bluetooth_scan(struct kl_system *system, unsigned on);
int kl_system_bluetooth_power(struct kl_system *system, unsigned on, uint32_t *request);
int kl_system_bluetooth_device(struct kl_system *system, unsigned action, const char *address, unsigned type, uint32_t *request);

/*
 * Copy up to capacity of the details last asked for and return how many
 * were copied.
 */
size_t kl_system_network_get_links(const struct kl_system *system, struct kl_network_link *links, size_t capacity);
size_t kl_system_network_get_dns(const struct kl_system *system, char (*servers)[KL_NETWORK_ADDRESS_MAX], size_t capacity);
size_t kl_system_network_get_saved(const struct kl_system *system, char (*ssids)[KL_NETWORK_SSID_MAX], size_t capacity);

/*
 * Copies the sound output's state.
 */
void kl_system_audio_get_state(const struct kl_system *system, struct kl_audio_state *state);

/*
 * Asks for each channel's volume (0 to 100) and the mute.  Returns 0 when
 * asked, ENOTSUP, or EINVAL.
 */
int kl_system_audio_set_volume(struct kl_system *system, unsigned left, unsigned right, unsigned muted, uint32_t *request);

/*
 * Asks for the short feedback sound at the device volume.  Returns 0 when
 * asked, or ENOTSUP.
 */
int kl_system_audio_feedback(struct kl_system *system, uint32_t *request);

/*
 * Copies the power's state.
 */
void kl_system_power_get_state(const struct kl_system *system, struct kl_power_state *state);

/*
 * Asks for a power action (KL_POWER_*); one not in the state's actions is
 * answered ENOTSUP.  Returns 0 when asked, ENOTSUP, or EINVAL.
 */
int kl_system_power_action(struct kl_system *system, unsigned action, uint32_t *request);

/*
 * Copies up to capacity removable devices and returns how many were
 * copied.
 */
size_t kl_system_devices_get(const struct kl_system *system, struct kl_device *devices, size_t capacity);

/*
 * Copies a listed device's file system and size: 1 when the device is in
 * the list (its info is empty from a compositor older than version 9), 0
 * when it is not.
 */
int kl_system_devices_info(const struct kl_system *system, const char *id, struct kl_device_info *info);

/*
 * Asks for a removable device to be ejected (unmounted; a device that is not
 * mounted may simply be taken out), answered by a result: 0, EBUSY while a
 * program uses it (kl_system_devices_busy_program names it), EACCES for a
 * user who is not the session's.  Returns 0 when asked, ENOTSUP, or EINVAL.
 */
int kl_system_devices_eject(struct kl_system *system, const char *id, uint32_t *request);

/*
 * Asks for a removable device to be mounted under /media (KL_VERSION 28,
 * ws132-p004), answered by a result: 0 (its location then comes with the
 * devices' change), EACCES, or another errno value.  Returns 0 when asked,
 * ENOTSUP (a compositor without it), or EINVAL.
 */
int kl_system_devices_mount(struct kl_system *system, const char *id, uint32_t *request);

/*
 * Copies the program that keeps a device from being ejected, for a request
 * answered EBUSY (KL_VERSION 28).  Returns 1 with it, 0 without one.
 */
int kl_system_devices_busy_program(const struct kl_system *system, uint32_t request, char *program, size_t size);

/*
 * Asks for the password of the user the desktop runs as to be changed
 * from current to fresh (KL_VERSION 27, ws160-p002), answered by a result
 * (kl_system_take_result): 0, EPERM when current is wrong, EINVAL when
 * fresh breaks the system's rules (on zedBSD: at least 8 characters, not
 * the old one), ENOTSUP where the desktop cannot change it, EBUSY while
 * another change is under way, EIO when it failed.
 * Returns 0 when asked, ENOTSUP without KL_SYSTEM_HAS_ACCOUNT, or EINVAL
 * (an empty password, or one longer than 256 bytes).  The caller wipes
 * its copies; the library keeps none.
 */
int kl_system_account_set_password(struct kl_system *system, const char *current, const char *fresh, uint32_t *request);

/* The longest operation kl_system_account_administer takes (without its NUL), and the room of a refusal's word (with it). */
#define KL_ACCOUNT_OPERATION_MAX	1024U
#define KL_ACCOUNT_REASON_SIZE		32U

/*
 * Asks for an administrator's change of the people's accounts (KL_VERSION
 * 31, ws089-p026; docs/architecture/security.md): password is the
 * caller's own, operation the request's lines after it, each ended by a
 * line end ("add\nNAME\nDISPLAY\nPASSWORD\nadmin|user\n", "remove\nNAME\n
 * keep-home|remove-home\n", "reset-password\nNAME\nPASSWORD\n",
 * "group-add\nNAME\nwheel|network\n", "group-remove\n..."), answered by
 * a result (kl_system_take_result): 0, EPERM when the caller is not an
 * administrator or the password is wrong, EINVAL for another refusal,
 * ENOTSUP, EBUSY while another change is under way, EIO when it failed.
 * A refusal's word comes first (kl_system_account_refusal).  Returns 0
 * when asked, ENOTSUP without KL_SYSTEM_HAS_ADMINISTER, or EINVAL.  The
 * caller wipes its copies; the library keeps none.
 */
int kl_system_account_administer(struct kl_system *system, const char *password, const char *operation, uint32_t *request);

/*
 * Asks for the six-digit PIN of the user the desktop runs as to be set to
 * pin, or removed when pin is empty (KL_VERSION 34, ws163-p003; since
 * ws172-p002 the session manager keeps it, in /etc/passkey on zedBSD),
 * answered by a result (kl_system_take_result): 0, EPERM when current (the
 * user's password) is wrong (the refusal's word, as
 * kl_system_account_refusal gives it, says why: bad-secret, locked, ...),
 * EINVAL when pin is not six digits, ENOTSUP without a session manager,
 * EBUSY while another change is under way, EIO when it failed.  Returns 0
 * when asked, ENOTSUP without KL_SYSTEM_HAS_PIN, or EINVAL.  The caller
 * wipes its copies; the library keeps none.
 */
int kl_system_account_set_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request);

/*
 * Gives whether the user has a PIN (pin 1) and how many security keys are
 * registered (KL_VERSION 36, ws172-p002), as the compositor last told
 * (KL_SYSTEM_CHANGED_ENROLLED says it changed).  Returns 1 when known, 0
 * before the compositor told it (or one that cannot: both 0).
 */
int kl_system_account_enrolled(const struct kl_system *system, unsigned *pin, unsigned *keys);

/*
 * The user's security keys (KL_VERSION 52, ws172-p003; with
 * KL_SYSTEM_HAS_KEYS).  A key is its reference (what remove_key takes) and
 * its label.  kl_system_account_keys copies at most capacity of the keys
 * the compositor last told (with each enrolled, KL_SYSTEM_CHANGED_ENROLLED)
 * and returns how many there are.
 *
 * kl_system_account_add_key registers the key plugged in: its label (1 to
 * KL_SYSTEM_KEY_LABEL_MAX bytes, no colon) and the key's own PIN, checked
 * by the user's password; while the key waits to be touched
 * KL_SYSTEM_CHANGED_TOUCH comes and kl_system_account_touched gives the
 * request (once).  kl_system_account_remove_key removes one by its
 * reference.  Both are answered as kl_system_account_set_pin (a refusal's
 * word through kl_system_account_refusal: bad-secret, no-key, many-keys,
 * key-locked, timeout, ...).  Neither secret is kept.  They return 0,
 * ENOTSUP without KL_SYSTEM_HAS_KEYS, or EINVAL.
 */
#define KL_SYSTEM_KEY_REF_MAX	16U
#define KL_SYSTEM_KEY_LABEL_MAX	32U
#define KL_SYSTEM_KEYS_MAX	5U
struct kl_system_key {
	char ref[KL_SYSTEM_KEY_REF_MAX + 1U];
	char label[KL_SYSTEM_KEY_LABEL_MAX + 1U];
};
size_t kl_system_account_keys(const struct kl_system *system, struct kl_system_key *keys, size_t capacity);
int kl_system_account_add_key(struct kl_system *system, const char *password, const char *label, const char *pin, uint32_t *request);
int kl_system_account_remove_key(struct kl_system *system, const char *password, const char *ref, uint32_t *request);
int kl_system_account_touched(struct kl_system *system, uint32_t *request);

/*
 * The keys' own operations (KL_VERSION 77, ws199-p001; with
 * KL_SYSTEM_HAS_KEY_OPS).  kl_system_account_key_info asks what the keys
 * there are; its result's answer comes with kl_system_account_key_info_get
 * (count, and for one key its name, whether it has a PIN, its PIN's retries
 * and its PIN's fewest characters).  kl_system_account_key_pin sets the one
 * key's first PIN (current NULL) or changes it; the key checks the PINs.
 * kl_system_account_key_reset resets the key the user plugs in again,
 * checked by the user's password: KL_SYSTEM_CHANGED_REPLUG comes while the
 * key is to be plugged in again (kl_system_account_replugged gives the
 * request, once), KL_SYSTEM_CHANGED_TOUCH while it is to be touched, and
 * after the result kl_system_account_key_removed gives how many of this
 * machine's registrations went.  The results are as
 * kl_system_account_set_pin's (the refusal's word: bad-secret, bad-key-pin,
 * key-locked, key-replug, no-pin, pin-set, pin-policy, not-allowed, no-key,
 * many-keys, timeout, canceled, device).  kl_system_account_key_cancel
 * stops the operation under way.  KL_SYSTEM_CHANGED_KEYS says a key came
 * or went (or the screen was unlocked): what is there may be asked again.
 * The requests return 0, ENOTSUP without KL_SYSTEM_HAS_KEY_OPS, or EINVAL.
 */
#define KL_SYSTEM_KEY_NAME_MAX	63U
struct kl_system_key_info {
	unsigned count;
	char name[KL_SYSTEM_KEY_NAME_MAX + 1U];
	unsigned pin;
	unsigned retries;
	unsigned min;
};
int kl_system_account_key_info(struct kl_system *system, uint32_t *request);
int kl_system_account_key_info_get(const struct kl_system *system, struct kl_system_key_info *info);
int kl_system_account_key_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request);
int kl_system_account_key_reset(struct kl_system *system, const char *password, uint32_t *request);
int kl_system_account_key_cancel(struct kl_system *system);
int kl_system_account_replugged(struct kl_system *system, uint32_t *request);
unsigned kl_system_account_key_removed(const struct kl_system *system);

/*
 * Whether the user's security key asks its PIN to sign in (key_pin 1) and
 * its touch to unlock (key_touch 1), as the compositor last told with each
 * enrolled (KL_VERSION 77, ws199-p001): 1 when known.  Set with
 * kl_system_account_set_key_options, checked by the user's password
 * (no touch only without the PIN), answered as kl_system_account_set_pin
 * (the refusal's word: bad-secret, not-enrolled, ...).  Returns 0,
 * ENOTSUP without KL_SYSTEM_HAS_KEY_OPS, or EINVAL.
 */
int kl_system_account_key_options(const struct kl_system *system, unsigned *key_pin, unsigned *key_touch);
int kl_system_account_set_key_options(struct kl_system *system, const char *password, unsigned key_pin, unsigned key_touch, uint32_t *request);

/*
 * The methods the login and locked screens take for the user (KL_VERSION
 * 78, WS200; with KL_SYSTEM_HAS_METHODS): KL_SYSTEM_METHOD_* bits, as the
 * compositor last told with each enrolled (every method until told; 1 when
 * known).  A method the user has not set up (no PIN, no key) is not taken
 * whatever its bit; the password is taken too when nothing else is a first
 * sign-in.  The console, su, sudo and SSH always take the password.
 * kl_system_account_set_methods sets them, checked by the user's password,
 * the password or a key among them, answered as kl_system_account_set_pin
 * (the refusal's word: bad-secret, not-enrolled, bad-request, ...).
 * Returns 0, ENOTSUP without KL_SYSTEM_HAS_METHODS, or EINVAL.
 */
#define KL_SYSTEM_METHOD_PASSWORD	0x1U
#define KL_SYSTEM_METHOD_PIN		0x2U
#define KL_SYSTEM_METHOD_KEY		0x4U
#define KL_SYSTEM_METHODS_ALL		0x7U
int kl_system_account_methods(const struct kl_system *system, unsigned *methods);
int kl_system_account_set_methods(struct kl_system *system, const char *password, unsigned methods, uint32_t *request);

/*
 * Copies the word of a refused kl_system_account_administer request
 * (not-administrator, bad-password, no-such-user, name-taken, bad-name,
 * weak-password, last-administrator, self, root, busy, home-exists,
 * bad-request, failed).  Returns 1 with it, 0 without one.
 */
int kl_system_account_refusal(const struct kl_system *system, uint32_t request, char *reason, size_t size);

/*
 * The machine's monitor (WS134 p012, plan/ws134/design.md section 1.3):
 * what the System Monitor shows, sampled by the compositor and made into
 * rates here.  A monitor is opened on a kl_system and its samples come
 * with kl_system_dispatch; kl_system_monitor_take gives the newest frame:
 * the rates over the time since the sample before it (the CPUs' shares,
 * bytes and operations a second, the disks' mean latency, the GPUs'
 * shares) and the present values.  The first frame comes with the second
 * sample.  A device that came again, or a counter that went back, has no
 * rate for that frame.  Close every monitor before its kl_system.
 */

/* The most CPUs, GPUs, disks and links a monitor follows. */
#define KL_MONITOR_CPU_MAX	256U
#define KL_MONITOR_GPU_MAX	4U
#define KL_MONITOR_DISK_MAX	8U
#define KL_MONITOR_LINK_MAX	16U

/* What a frame has (struct kl_monitor_frame's valid). */
#define KL_MONITOR_FRAME_CPU		0x001U
#define KL_MONITOR_FRAME_MEMORY		0x002U
#define KL_MONITOR_FRAME_SWAP		0x004U
#define KL_MONITOR_FRAME_LINKS		0x008U
#define KL_MONITOR_FRAME_DISKS		0x010U
#define KL_MONITOR_FRAME_GPU_BUSY	0x020U
#define KL_MONITOR_FRAME_GPU_MEMORY	0x040U
#define KL_MONITOR_FRAME_GPU_FREQ	0x080U
#define KL_MONITOR_FRAME_TEMPERATURE	0x100U
#define KL_MONITOR_FRAME_POWER		0x200U

/* A disk's kind (struct kl_monitor_info's disk kind). */
#define KL_MONITOR_KIND_OTHER	1U
#define KL_MONITOR_KIND_NVME	2U
#define KL_MONITOR_KIND_USB	3U
#define KL_MONITOR_KIND_UAS	4U
#define KL_MONITOR_KIND_IDE	5U
#define KL_MONITOR_KIND_SDMMC	6U
#define KL_MONITOR_KIND_SCSI	7U

/* One GPU of the info: its id, name and driver. */
struct kl_monitor_gpu_info {
	uint64_t id;
	char name[48];
	char driver[16];
};

/* One disk of the info: its id, name and kind. */
struct kl_monitor_disk_info {
	uint64_t id;
	char name[32];
	unsigned kind;
};

/* One link of the info: its id and name. */
struct kl_monitor_link_info {
	uint64_t id;
	char name[16];
};

/* What does not change from frame to frame: the CPUs, the machine's name, the devices. */
struct kl_monitor_info {
	unsigned cpu_count;
	char host[64];
	unsigned gpu_count;
	unsigned disk_count;
	unsigned link_count;
	struct kl_monitor_gpu_info gpu[KL_MONITOR_GPU_MAX];
	struct kl_monitor_disk_info disk[KL_MONITOR_DISK_MAX];
	struct kl_monitor_link_info link[KL_MONITOR_LINK_MAX];
};

/* One link's bytes a second, received and sent, and whether it is up. */
struct kl_monitor_link_rate {
	uint64_t id;
	double rx_rate;
	double tx_rate;
	unsigned up;
};

/* One disk's bytes and operations a second, its mean latency in milliseconds, and its busy share (0 to 1). */
struct kl_monitor_disk_rate {
	uint64_t id;
	double read_rate;
	double write_rate;
	double read_ops;
	double write_ops;
	double latency_ms;
	double busy;
};

/* One GPU's busy share (0 to 1), its memory, frequencies, temperature and power. */
struct kl_monitor_gpu_rate {
	uint64_t id;
	double busy;
	uint64_t memory_used;
	uint64_t memory_total;
	unsigned cur_mhz;
	unsigned max_mhz;
	int milli_celsius;
	unsigned milli_watts;
};

/*
 * One frame: when (CLOCK_MONOTONIC, ns) and over how many seconds, what it
 * has, the CPUs' busy shares (0 to 1, all and each), the memory in bytes,
 * the links' and the disks' rates (each, and their sums; the disks'
 * latency is the mean over every operation), the GPUs, and the CPU's
 * temperature.
 */
struct kl_monitor_frame {
	uint64_t time_ns;
	double seconds;
	unsigned valid;
	double cpu;
	unsigned cpu_count;
	double cpu_core[KL_MONITOR_CPU_MAX];
	uint64_t memory_total;
	uint64_t memory_free;
	uint64_t memory_cache;
	uint64_t memory_reclaimable;
	uint64_t swap_total;
	uint64_t swap_used;
	unsigned link_count;
	struct kl_monitor_link_rate link[KL_MONITOR_LINK_MAX];
	double rx_rate;
	double tx_rate;
	unsigned disk_count;
	struct kl_monitor_disk_rate disk[KL_MONITOR_DISK_MAX];
	double read_rate;
	double write_rate;
	double disk_latency_ms;
	unsigned gpu_count;
	struct kl_monitor_gpu_rate gpu[KL_MONITOR_GPU_MAX];
	int cpu_milli_celsius;
};

/* A monitor on a kl_system. */
struct kl_system_monitor;

/*
 * Opens a monitor sampled every period_ms (250 to 10000; 0 is 1000).
 * Returns NULL with errno ENOTSUP when the compositor offers none, or
 * ENOMEM.
 */
struct kl_system_monitor *kl_system_monitor_open(struct kl_system *system, unsigned period_ms);

/*
 * Copies the newest frame into *frame: 1 when one came since the last
 * take, 0 when none did (*frame is left as it was).
 */
int kl_system_monitor_take(struct kl_system_monitor *monitor, struct kl_monitor_frame *frame);

/*
 * The info as last told (all zero before the first), and how many times it
 * changed (a new number means the devices changed).
 */
const struct kl_monitor_info *kl_system_monitor_info(const struct kl_system_monitor *monitor, unsigned *changes);

/*
 * Closes a monitor.
 */
void kl_system_monitor_close(struct kl_system_monitor *monitor);

/*
 * One copy of a program, and the activation (ws089-p016).
 *
 * A program that keeps one window (Settings) runs once per user: a second
 * start hands its request -- a line of text the program gives meaning to,
 * such as a page to open -- to the copy that runs, with an activation
 * token, and ends.  The copy that runs takes the request, carries it out
 * and brings its window to the front with the token (kl_activate).  The
 * copies find each other through a socket in the user's runtime directory
 * (XDG_RUNTIME_DIR), which must be the user's own and closed to others.
 *
 * An activation token is a compositor's permission to bring a window to
 * the front (xdg_activation_v1).  A program started by the desktop has one
 * in XDG_ACTIVATION_TOKEN; a program may ask the compositor for one to
 * hand to a program it starts (kl_activation_token).
 */

/* The longest request (its NUL counted), and the longest activation token. */
#define KL_INSTANCE_REQUEST_MAX		256U
#define KL_ACTIVATION_TOKEN_MAX		256U

/* The copy of a program that runs: its socket. */
struct kl_instance;

/*
 * Makes this program the one copy of the name (lowercase letters, digits
 * and '-'), or hands request to the copy that runs.  Returns 0 with
 * *instance set when this is the one copy (it listens for later starts),
 * 0 with *instance NULL when the request was handed over (the program
 * ends), or an error (the program runs on its own, as without the call):
 * EINVAL for a bad name or request, ENOTSUP without a private runtime
 * directory.  The token handed over is one asked of the compositor (which
 * grants it to a program that has just started), or XDG_ACTIVATION_TOKEN's
 * when none can be asked for; the variable is removed either way.
 */
int kl_instance_open(const char *name, const char *request, struct kl_instance **instance);

/*
 * The descriptor that becomes readable when a later start hands a request
 * over (for the program's poll), or -1 for NULL.
 */
int kl_instance_fd(const struct kl_instance *instance);

/*
 * Takes one request handed over: 1 with the request and the token (an
 * empty string when there is none) copied, 0 when none waits.
 */
int kl_instance_take(struct kl_instance *instance, char *request, size_t request_size, char *token, size_t token_size);

/*
 * Stops being the one copy: the socket goes.
 */
void kl_instance_close(struct kl_instance *instance);

/*
 * Asks the compositor for an activation token for a program to be started
 * (app_id, NULL for none), from a surface the user works in (NULL for
 * none).  Returns 0 with the token copied, ENOTSUP when the compositor has
 * no activation, or another error.
 */
int kl_activation_token(struct wl_display *display, struct wl_surface *surface, const char *app_id, char *token, size_t size);

/*
 * Brings a surface's window to the front with an activation token (the
 * compositor decides whether the token allows it).  Returns 0 when it was
 * asked, ENOTSUP when the compositor has no activation.
 */
int kl_activate(struct wl_display *display, struct wl_surface *surface, const char *token);

/*
 * The desktop's appearance (KL_VERSION 35, ws089-p017): light or dark, as
 * the user chose in Settings.  The compositor tells it when the program
 * asks and again whenever it changes (kl_theme_v1); a program draws
 * in it and draws again when it changes.  kl_theme_default gives the
 * theme of the appearance the program was told last (the same pointer,
 * its contents changed), and an application of kl_app_open is told with
 * a KL_APP_THEME event.  A compositor without the appearance is light.
 */
#define KL_APPEARANCE_LIGHT	0U
#define KL_APPEARANCE_DARK	1U

/* The appearance watched on one connection. */
struct kl_appearance;

/* Called with the appearance (KL_APPEARANCE_*) when it or the accent changes (KL_VERSION 57), within the dispatch of the display's default queue. */
typedef void (*kl_appearance_fn)(void *data, unsigned appearance);

/*
 * Watches the appearance on a display: learns it now (with a roundtrip
 * of the library's own queue, which runs none of the program's events)
 * and calls changed (NULL for none) whenever it changes.  Returns 0 with
 * *appearance set, ENOTSUP when the compositor has no appearance (the
 * program is light), ENOMEM or EPROTO.
 */
int kl_appearance_open(struct wl_display *display, kl_appearance_fn changed, void *data, struct kl_appearance **appearance);

/*
 * The appearance (KL_APPEARANCE_*) a watch was told last; for NULL, the
 * one the program was told last on any watch (light before any).
 */
unsigned kl_appearance_get(const struct kl_appearance *appearance);

/*
 * Stops watching (NULL does nothing).
 */
void kl_appearance_close(struct kl_appearance *appearance);

/*
 * The translations of the user interface's text (WS158).  The English
 * text in the source is the key: kl_tr("Open") gives the text of the
 * language chosen, or "Open" itself when the catalog has none.  The
 * catalogs are UTF-8 text files, KEILAND_DATADIR/keiland/locale/LANGUAGE/
 * DOMAIN.tr, one for each program (its domain) and one the desktop's
 * programs share ("keiland").  The language is the desktop's setting
 * ui.language (0 English, 1 Japanese), which kl_tr_follow watches, so
 * that a program changes its language while it runs.  One thread uses
 * them; a text they give lives until the language or the domain changes.
 */

/* The domain the desktop's programs share, looked in after a program's own. */
#define KL_TR_SHARED_DOMAIN	"keiland"

/* The longest name of a language or a domain. */
#define KL_TR_NAME_MAX		31U

/*
 * Called by kl_tr_follow after the language changed and its catalogs were
 * read: the program draws its text again.  language is "en", "ja", ...
 */
typedef void (*kl_tr_changed_fn)(void *data, const char *language);

/*
 * Reads a program's catalogs (domain and KL_TR_SHARED_DOMAIN) in a
 * language from the installed directory.  English needs none.  A catalog
 * that is not there leaves its texts in English.  Returns 0, EINVAL for a
 * name that is not lower-case letters, digits, '-' and '_', or ENOMEM; the
 * texts are English after a failure.
 */
int kl_tr_open(const char *domain, const char *language);

/*
 * Reads them from another directory, which holds LANGUAGE/DOMAIN.tr (a
 * program installed elsewhere, or a test).  Returns as kl_tr_open.
 */
int kl_tr_open_directory(const char *directory, const char *domain, const char *language);

/*
 * Forgets the catalogs: every text is English again.
 */
void kl_tr_close(void);

/*
 * Reports the language read ("en" before any).
 */
const char *kl_tr_language(void);

/*
 * Reports the language a value of ui.language names ("en" for 0, "ja"
 * for 1), or NULL for a value no language has.
 */
const char *kl_tr_language_code(int setting);

/*
 * Gives the text of the language for an English text.
 */
const char *kl_tr(const char *english);

/*
 * Gives the text for an English text in a context, for an English word
 * that is translated in more than one way ("Open" a verb or an adjective).
 */
const char *kl_trc(const char *context, const char *english);

/*
 * Gives the text for a number of things: the singular or the plural in
 * English, and the form the language takes for count.  The number itself
 * goes in with kl_tr_format.
 */
const char *kl_trn(const char *singular, const char *plural, unsigned long count);

/*
 * Writes a text of the language with its places filled: {1} to {9} are
 * the strings after pattern, in order, which a NULL ends; {{ is a brace.
 * The places can come in any order, so that a language puts the words
 * where its grammar wants them.  out always ends with a NUL.  Returns 0,
 * ERANGE when the text was cut to fit, or EINVAL for a place without its
 * string.
 */
int kl_tr_format(char *out, size_t size, const char *pattern, ...);

/*
 * Follows the desktop's language: reads the program's catalogs in the
 * language of ui.language now, and again whenever it changes, calling
 * changed (may be NULL) after each change.  The watch runs from
 * kl_settings_dispatch, and ends with the settings.  Returns 0, EINVAL or
 * ENOMEM.
 */
int kl_tr_follow(struct kl_settings *settings, const char *domain, kl_tr_changed_fn changed, void *data);

/*
 * The desktop's shared widgets and controls (WS090, plan/ws090/design.md;
 * libkeiui until WS131 p012, a header of their own until WS131 p023):
 * one library the desktop's applications draw their parts with, so that a
 * button, a list or a scroll looks and feels the same in every one of them.
 *
 * The first layer (libkeiui's version 1) is the drawing: a CPU canvas of
 * premultiplied BGRA pixels, the text drawn on it from TrueType fonts, the
 * icons made of its shapes, and the theme -- the colours and sizes of the
 * Kei look.  It began as the file manager's drawing surface (Files'
 * canvas.c, text.c and icons.c) and Settings' line pictures, moved here
 * unchanged so that an application moved onto the library draws the same
 * pixels as before.  The second (libkeiui's version 2) is the scroll, the input
 * that finds which part of a frame a pointer, a wheel or a finger meant,
 * and the touch of a view of editable text.  The third (libkeiui's version 3) is
 * the window: a Wayland toplevel whose input arrives as a queue of
 * events, whose CPU-drawn frames are shown through Vulkan or shared
 * memory, and which holds the clipboard and the primary selection.  The
 * fourth (libkeiui's version 4) is the widgets: buttons, switches, sliders, text
 * fields, lists, sidebars, cards and their rows, dialogs, chips and
 * progress bars, drawn in the Kei look of Files and Settings, and the
 * keyboard's focus among them.
 *
 * Times are CLOCK_MONOTONIC microseconds throughout (the clock of
 * libkeiland's touch motion, scroller and gestures).
 *
 * Nothing in this layer knows about Wayland or Vulkan.  A window's frame
 * is drawn into a canvas and handed to its presenter, and host tests draw
 * into a canvas of their own and write it out as a picture.  Every call
 * is made from one thread.  The library's name is internal and never
 * appears in what a user reads.
 */

/* How deep the clip rectangles nest. */
#define KL_CANVAS_CLIPS		16

/* The most corners a polygon may have. */
#define KL_POLYGON_POINTS	96

/*
 * How many fonts the text draws from: the main one, a fallback, and the
 * colour emoji font (KL_TEXT_EMOJI, opened the first time a character
 * neither of the others has is drawn; version 9).
 */
#define KL_TEXT_FACES		3
#define KL_TEXT_EMOJI		"/usr/share/fonts/keiland-emoji.ttf"

/* A color as 0xAARRGGBB, not premultiplied. */
typedef uint32_t kl_color;

/* An opaque color from 0xRRGGBB. */
#define KL_RGB(value)		((kl_color)(0xff000000U | (uint32_t)(value)))

/* A color from 0xRRGGBB and an alpha from 0 to 255. */
#define KL_RGBA(value, alpha)	((kl_color)(((uint32_t)(alpha) << 24) | ((uint32_t)(value) & 0xffffffU)))

/*
 * A rectangle of whole pixels.
 *
 * It is a plain value: a layout computes one, the drawing and the hit test
 * both read it.
 */
struct kl_rect {
	int x;
	int y;
	int width;
	int height;
};

/*
 * A picture in memory, premultiplied BGRA (0xAARRGGBB words).
 *
 * The pixels belong to whoever made the image: kl_image_create allocates
 * them and kl_image_release frees them.
 */
struct kl_image {
	uint32_t *pixels;
	int width;
	int height;
	size_t stride;
};

/*
 * A surface to draw on.
 *
 * The pixels are the caller's; the canvas adds the clip rectangles and the
 * scratch row the polygon filler accumulates coverage in, which live as
 * long as the canvas.
 */
struct kl_canvas {
	/* The pixels, the words in a row and the size. */
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;

	/* The clip in force, and the ones it replaced (innermost last). */
	struct kl_rect clip;
	struct kl_rect clips[KL_CANVAS_CLIPS];
	int clip_depth;

	/* One row of polygon coverage, a float per pixel and one more. */
	float *coverage;
};

/*
 * One glyph drawn at one size, kept for the next time.
 *
 * key is zero for an empty slot; the bitmap is the glyph's coverage, or
 * pixels its colour (premultiplied 0xAARRGGBB, a colour emoji, version 9)
 * with bitmap NULL.
 */
struct kl_glyph {
	uint32_t key;
	int width;
	int height;
	int left;
	int top;
	int advance;
	uint8_t *bitmap;
	uint32_t *pixels;
};

/*
 * One font file: its bytes (kept for the face) and the face.
 */
struct kl_text_face {
	void *data;
	size_t size;
	struct truetype_face *face;
	unsigned pixels;
};

/*
 * The text of the window: the fonts and every glyph drawn so far.
 *
 * One lives for the whole program.  The cache is emptied when it fills up,
 * which only costs drawing the glyphs again.  emoji_tried says the emoji
 * font (faces[2]) was looked for (version 9).
 */
struct kl_text {
	struct kl_text_face faces[KL_TEXT_FACES];
	int face_count;
	int emoji_tried;
	struct kl_glyph *cache;
	unsigned cache_size;
	unsigned cache_used;
	uint8_t *scratch;
	size_t scratch_size;
};

/*
 * The vertical measurements of text at one size, in pixels.
 */
struct kl_text_line {
	int ascent;
	int descent;
	int height;
};

/*
 * The icons drawn with lines (sidebar, toolbar) or filled shapes (items).
 */
enum kl_icon {
	KL_ICON_HOME,
	KL_ICON_DESKTOP,
	KL_ICON_DOCUMENTS,
	KL_ICON_DOWNLOADS,
	KL_ICON_PICTURES,
	KL_ICON_MUSIC,
	KL_ICON_MOVIES,
	KL_ICON_FOLDER_LINE,
	KL_ICON_RECENTS,
	KL_ICON_TRASH,
	KL_ICON_COMPUTER,
	KL_ICON_VOLUME,
	KL_ICON_BACK,
	KL_ICON_FORWARD,
	KL_ICON_SEARCH,
	KL_ICON_GRID,
	KL_ICON_LIST,
	KL_ICON_PREVIEW,
	KL_ICON_CHEVRON,
	KL_ICON_CLOSE,
	KL_ICON_PLUS,
	KL_ICON_UP,
	KL_ICON_DOWN,

	/* The line pictures (Settings' pages), from KL_ICON_TILES on, in the order Settings numbered them. */
	KL_ICON_TILES,
	KL_ICON_WIFI,
	KL_ICON_ETHERNET,
	KL_ICON_BLUETOOTH,
	KL_ICON_SHIELD,
	KL_ICON_GLOBE,
	KL_ICON_PALETTE,
	KL_ICON_PICTURE,
	KL_ICON_BELL,
	KL_ICON_SPEAKER,
	KL_ICON_MONITOR,
	KL_ICON_DISK,
	KL_ICON_BATTERY,
	KL_ICON_KEYBOARD,
	KL_ICON_MOUSE,
	KL_ICON_TOUCHPAD,
	KL_ICON_PRINTER,
	KL_ICON_SHARE,
	KL_ICON_PEOPLE,
	KL_ICON_EYE,
	KL_ICON_LOCK,
	KL_ICON_PERSON,
	KL_ICON_REFRESH,
	KL_ICON_INFO,
	KL_ICON_DISCLOSURE,

	/* Files' sidebar's Today, a calendar's page (KL_VERSION 47, ws090-p009). */
	KL_ICON_TODAY,

	/*
	 * KL_VERSION 60 (ws090-p023): a cross in a circle, leaving a network
	 * (Settings' line picture), and a device's eject, a block over a bar
	 * (Files' sidebar), filled.
	 */
	KL_ICON_DISCONNECT,
	KL_ICON_EJECT
};

/* The canvas (canvas.c). */
int kl_canvas_init(struct kl_canvas *canvas, uint32_t *pixels, size_t stride, int width, int height);
void kl_canvas_release(struct kl_canvas *canvas);
void kl_canvas_clip_push(struct kl_canvas *canvas, const struct kl_rect *rect);
void kl_canvas_clip_pop(struct kl_canvas *canvas);
void kl_canvas_clear(struct kl_canvas *canvas);
void kl_canvas_fill(struct kl_canvas *canvas, const struct kl_rect *rect, kl_color color);
void kl_canvas_gradient(struct kl_canvas *canvas, const struct kl_rect *rect, kl_color top, kl_color bottom);
void kl_canvas_round(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, kl_color color);
void kl_canvas_round_gradient(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, kl_color top, kl_color bottom);
void kl_canvas_round_border(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, float thickness, kl_color color);
void kl_canvas_shadow(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, float softness, kl_color color);
void kl_canvas_circle(struct kl_canvas *canvas, float cx, float cy, float radius, kl_color color);
void kl_canvas_ring(struct kl_canvas *canvas, float cx, float cy, float radius, float thickness, float fraction, kl_color color);
void kl_canvas_polygon(struct kl_canvas *canvas, const float *points, int count, kl_color color);
void kl_canvas_line(struct kl_canvas *canvas, float x0, float y0, float x1, float y1, float thickness, kl_color color);
void kl_canvas_mask(struct kl_canvas *canvas, int x, int y, const uint8_t *mask, int width, int height, size_t stride, kl_color color);
void kl_canvas_image(struct kl_canvas *canvas, const struct kl_image *image, float x, float y, float width, float height, float radius, float opacity);
int kl_image_create(struct kl_image *image, int width, int height);
void kl_image_release(struct kl_image *image);
void kl_image_scale(const struct kl_image *source, struct kl_image *target);
kl_color kl_color_mix(kl_color from, kl_color to, float amount);

/* The text (text.c). */
int kl_text_open(struct kl_text *text, const char *primary, const char *fallback);
/* KL_VERSION 48 (ws090-p023): the companions' files for fonts opened from now on (a host test with the tree's fonts); NULL: the installed ones. */
void kl_text_companions(const char *bold, const char *mono);
void kl_text_close(struct kl_text *text);
void kl_text_metrics(struct kl_text *text, unsigned pixels, struct kl_text_line *line);
int kl_text_center(unsigned pixels, int top, int height);
int kl_text_width(struct kl_text *text, const char *string, size_t length, unsigned pixels, int bold);
int kl_text_draw(struct kl_text *text, struct kl_canvas *canvas, int x, int baseline, const char *string, size_t length, unsigned pixels, int bold, kl_color color);
int kl_text_draw_fit(struct kl_text *text, struct kl_canvas *canvas, int x, int baseline, const char *string, unsigned pixels, int bold, int width, kl_color color);
size_t kl_text_fit(struct kl_text *text, const char *string, unsigned pixels, int bold, int width, char *out, size_t size);
size_t kl_text_break(struct kl_text *text, const char *string, unsigned pixels, int bold, int width);
uint32_t kl_utf8_next(const char *string, size_t length, size_t *index);

/*
 * The theme: the colours and sizes of the Kei look, which every widget
 * draws with (the file manager's values, plan/ws071/spec.md), in the
 * light or the dark appearance with the accent the user chose (KL_VERSION
 * 57); an application reads it and does not change it.
 */
struct kl_theme {
	/* The window's ground (a vertical gradient) and the cards on it. */
	kl_color ground_top;
	kl_color ground_bottom;
	kl_color panel;
	kl_color panel_edge;
	kl_color shadow;

	/* A sidebar and a content card standing on glass, and on the plain ground. */
	kl_color glass_sidebar;
	kl_color glass_content;
	kl_color sidebar;

	/* Text: the main ink, the secondary and the faint, and an icon's ink. */
	kl_color text;
	kl_color text_secondary;
	kl_color text_faint;
	kl_color icon;

	/* The accent, a selection with and without the keyboard, the pointer's hover, a separator, a folder and danger. */
	kl_color accent;
	kl_color selection;
	kl_color selection_inactive;
	kl_color hover;
	kl_color separator;
	kl_color folder;
	kl_color danger;

	/* A card's and a control's corner radius, a list row's height, and the text sizes of body, secondary and title text. */
	float card_radius;
	float control_radius;
	int row_height;
	unsigned text_body;
	unsigned text_small;
	unsigned text_title;

	/*
	 * libkeiui's version 4 (Settings' values, plan/ws089): a card within a page
	 * and its edge, the line between a card's rows, a control's ground and
	 * edge, a switch's track when off, good and bad news, a control's
	 * height, and a switch's size.
	 */
	kl_color card;
	kl_color card_edge;
	kl_color row_separator;
	kl_color control;
	kl_color control_edge;
	kl_color track;
	kl_color good;
	kl_color bad;
	int control_height;
	int switch_width;
	int switch_height;

	/*
	 * KL_VERSION 57 (ws179-p001): the ink of text and marks drawn on the
	 * accent (white or nearly black, whichever reads), and the accent as
	 * the colour of text on the window's ground.
	 */
	kl_color accent_ink;
	kl_color accent_text;
};

/* The theme (theme.c). */
const struct kl_theme *kl_theme_default(void);

/*
 * One of two colours by the desktop's appearance the program was told last
 * (KL_VERSION 35, ws089-p017): light in the light appearance, dark in the
 * dark one -- for a program's colours of its own beside the theme's.
 */
kl_color kl_theme_choose(kl_color light, kl_color dark);

/*
 * The accent the user chose in Settings (KL_VERSION 57, ws179-p001): one of
 * eight colours, told with the appearance (kl_theme_v1 version 2); the
 * theme's accent, selection, accent_ink and accent_text follow it, and a
 * watch's callback is called when it changes.  A compositor that does not
 * tell it leaves blue.
 */
#define KL_ACCENT_BLUE		0U
#define KL_ACCENT_PURPLE	1U
#define KL_ACCENT_PINK		2U
#define KL_ACCENT_RED		3U
#define KL_ACCENT_ORANGE	4U
#define KL_ACCENT_YELLOW	5U
#define KL_ACCENT_GREEN		6U
#define KL_ACCENT_GRAPHITE	7U
#define KL_ACCENTS		8U

/* An accent's colours in one appearance: the accent, the ink on it, the accent as text, and the selection's ground. */
struct kl_accent {
	kl_color accent;
	kl_color ink;
	kl_color text;
	kl_color selection;
};

/* The accent (KL_ACCENT_*) the program was told last (blue before any). */
unsigned kl_accent_get(void);

/* An accent's colours in an appearance (KL_APPEARANCE_*), for a program that shows the choices. */
void kl_accent_values(unsigned accent, unsigned appearance, struct kl_accent *values);

/* The icons (icons.c and icons-line.c). */
void kl_icon_draw(struct kl_canvas *canvas, enum kl_icon icon, float x, float y, float size, kl_color color);
void kl_icon_folder(struct kl_canvas *canvas, float x, float y, float size, kl_color tint);
void kl_icon_file(struct kl_canvas *canvas, struct kl_text *text, float x, float y, float size, kl_color band, const char *label);
void kl_icon_tag(struct kl_canvas *canvas, float cx, float cy, float radius, kl_color color);

/*
 * The scroll (scroll.c, libkeiui's version 2): the state of one part of a window
 * whose content is larger than the part, which an application keeps and
 * draws its content at (x, y) of.
 *
 * The wheel and the keys glide the content to where they send it (the
 * distance left shrinks by e every KL_SCROLL_GLIDE_US); a finger drags it
 * and lets it fly on with libkeiland's scroller (the inertia and the
 * rubber band past an end are the scroller's).  Outside a finger's hold
 * the position stays within 0..content-viewport on each axis that
 * scrolls.  The scroll bars show while the content moves and fade out
 * over KL_SCROLL_FADE_US after it stops.
 *
 * A program whose ends are not 0..content-viewport (Terminal's scrollback
 * runs to a negative position, KL_VERSION 61) gives them and the rubber
 * band's size with kl_scroll_set_bounds; kl_scroll_set_size goes back to
 * the ends of the sizes.  A program that moves its view by other means
 * (keys, new output) hands the place over with kl_scroll_move_to without a
 * glide, and the next finger drags on from there.
 *
 * The fields are read by the application (x and y above all); they are
 * written only through the calls.  Nothing here draws but
 * kl_scroll_draw_bars, so a program that draws its own content with
 * Vulkan (Terminal, Notes) uses the same scroll without the canvas.
 */
struct kl_scroller;

/* The axes a scroll moves along. */
#define KL_SCROLL_X		1U
#define KL_SCROLL_Y		2U

/* How quickly a glide closes on its target (the time constant), and how long the bars take to fade. */
#define KL_SCROLL_GLIDE_US	70000U
#define KL_SCROLL_FADE_US	1000000U

/*
 * The track of a touch pad's two-finger scrolling (KL_VERSION 40, BUG-211):
 * its last moves and their times, from which the velocity is worked out
 * when the fingers lift, so that the content flies on.  The samples older
 * than KL_AXIS_TRACK_WINDOW_US at the lift do not count, and fingers that
 * rested longer than KL_AXIS_TRACK_REST_US before lifting throw nothing.
 * It is plain data a caller keeps; kl_scroller keeps one for a touch pad's
 * fingers (KL_VERSION 41), so a program needs none of its own.
 */
#define KL_AXIS_TRACK_SAMPLES	16U
#define KL_AXIS_TRACK_WINDOW_US	100000U
#define KL_AXIS_TRACK_REST_US	60000U

struct kl_axis_track {
	unsigned count;
	unsigned next;
	double dx[KL_AXIS_TRACK_SAMPLES];
	double dy[KL_AXIS_TRACK_SAMPLES];
	uint64_t us[KL_AXIS_TRACK_SAMPLES];
};

void kl_axis_track_reset(struct kl_axis_track *track);
void kl_axis_track_add(struct kl_axis_track *track, double dx, double dy, uint64_t now_us);
void kl_axis_track_velocity(const struct kl_axis_track *track, uint64_t now_us, double *vx, double *vy);

struct kl_scroll {
	/* The axes it moves along, the position drawn, and the sizes of the content and of the part that shows it. */
	unsigned axes;
	double x;
	double y;
	double content_width;
	double content_height;
	double viewport_width;
	double viewport_height;

	/* A glide of the wheel or the keys: where it started, where it goes, and when it started. */
	int gliding;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	uint64_t glide_us;

	/* The finger's scroller, whether it owns the content (a finger holds it or it flies on), and whether the finger has lifted. */
	struct kl_scroller *scroller;
	int touched;
	int released;

	/* When the content last moved (for the bars), 0 before it ever moved. */
	uint64_t moved_us;

	/*
	 * The ends kl_scroll_set_bounds gave (bounded is 1 from then until
	 * kl_scroll_set_size), in place of 0..content-viewport, and the size
	 * the rubber band past them is measured against (KL_VERSION 61).
	 */
	int bounded;
	double minimum_x;
	double maximum_x;
	double minimum_y;
	double maximum_y;
	double band_width;
	double band_height;
};

int kl_scroll_init(struct kl_scroll *scroll, unsigned axes);
void kl_scroll_release(struct kl_scroll *scroll);
void kl_scroll_set_size(struct kl_scroll *scroll, double content_width, double content_height, double viewport_width, double viewport_height);
int kl_scroll_set_bounds(struct kl_scroll *scroll, double minimum_x, double maximum_x, double minimum_y, double maximum_y, double band_width, double band_height);
void kl_scroll_wheel(struct kl_scroll *scroll, double dx, double dy, uint64_t now_us);
void kl_scroll_move_to(struct kl_scroll *scroll, double x, double y, int glide, uint64_t now_us);
void kl_scroll_reveal(struct kl_scroll *scroll, const struct kl_rect *rect, uint64_t now_us);
int kl_scroll_key(struct kl_scroll *scroll, uint32_t key, unsigned modifiers, double line, uint64_t now_us);
int kl_scroll_press(struct kl_scroll *scroll, uint64_t now_us);
void kl_scroll_drag(struct kl_scroll *scroll, double dx, double dy);
int kl_scroll_fling(struct kl_scroll *scroll, double vx, double vy, uint64_t now_us);
void kl_scroll_cancel(struct kl_scroll *scroll, uint64_t now_us);
void kl_scroll_axis(struct kl_scroll *scroll, double dx, double dy, unsigned source, uint64_t now_us);
int kl_scroll_axis_stop(struct kl_scroll *scroll, uint64_t now_us);
int kl_scroll_axis_at(struct kl_scroll *scroll, double dx, double dy, unsigned source, uint64_t event_us, uint64_t now_us);
int kl_scroll_axis_stop_at(struct kl_scroll *scroll, uint64_t event_us, uint64_t now_us, double *vx, double *vy);
int kl_scroll_axis_holding(const struct kl_scroll *scroll);
int kl_scroll_step(struct kl_scroll *scroll, uint64_t now_us);
double kl_scroll_limit_x(const struct kl_scroll *scroll);
double kl_scroll_limit_y(const struct kl_scroll *scroll);
int kl_scroll_draw_bars(const struct kl_scroll *scroll, struct kl_canvas *canvas, const struct kl_rect *viewport, const struct kl_theme *theme, uint64_t now_us);

/*
 * The overlay scroll bar (scroll-bar.c, libkeiui's version 12, ws127-p002): the
 * vertical bar of a view, drawn over its content's right edge the way
 * macOS draws one (the user's choice of 2026-10-02).  It comes out thin
 * while the content moves, grows thick (with a faint track) while the
 * pointer is near the edge or drags it, and fades a while after the last
 * of these.  A press on the thumb drags the content; a press on the track
 * moves it a page towards the press.
 *
 * The state knows nothing of the window or of drawing: the application
 * tells it what happened (its sizes are in the content's pixels, offset
 * is how far the content is scrolled), asks for the shape to draw with its
 * own canvas (kl_scroll_bar_draw draws it on a kl_canvas), and draws
 * again while kl_scroll_bar_busy says the bar still changes.  The times
 * are microseconds of one clock.
 */
#define KL_SCROLL_BAR_THIN	6
#define KL_SCROLL_BAR_THICK	11
#define KL_SCROLL_BAR_REACH	16
#define KL_SCROLL_BAR_GAP	2
#define KL_SCROLL_BAR_MIN	28
#define KL_SCROLL_BAR_SHOW_US	1000000U
#define KL_SCROLL_BAR_FADE_US	400000U

/*
 * The state of one overlay bar.  active_us is when the content last moved
 * or the pointer last came near or dragged (0 before any); near says the
 * pointer is over the bar's band; dragging and grab (where in the thumb the
 * drag holds it) belong to a press on the thumb.  Zeroed, it is a bar that
 * has not shown yet.
 */
struct kl_scroll_bar {
	uint64_t active_us;
	int near;
	int dragging;
	double grab;
};

/*
 * What to draw now: the track (shown while the bar is thick) and the
 * thumb, in the window's pixels, and the strength of the ink (0 to 1).
 */
struct kl_scroll_bar_shape {
	double track_x;
	double track_y;
	double track_width;
	double track_height;
	double thumb_x;
	double thumb_y;
	double thumb_width;
	double thumb_height;
	double alpha;
	int thick;
};

void kl_scroll_bar_moved(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_hover(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double x, double y, uint64_t now_us);
int kl_scroll_bar_leave(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_shape(const struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double offset, uint64_t now_us, struct kl_scroll_bar_shape *shape);
int kl_scroll_bar_press(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double offset, double x, double y, uint64_t now_us, double *new_offset);
int kl_scroll_bar_drag(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double y, uint64_t now_us, double *new_offset);
int kl_scroll_bar_release(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_busy(const struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_draw(const struct kl_scroll_bar *bar, struct kl_canvas *canvas, const struct kl_rect *viewport, double content, double offset, uint64_t now_us);

/*
 * The keys (input.c, libkeiui's version 2).  The compositor forwards evdev key codes
 * with no keymap; the library carries the US layout, as the desktop's
 * programs do, until an input method arrives (WS095).
 */
#define KL_KEY_ESC		1U
#define KL_KEY_BACKSPACE	14U
#define KL_KEY_TAB		15U
#define KL_KEY_ENTER		28U
#define KL_KEY_SPACE		57U
#define KL_KEY_KPENTER	96U
#define KL_KEY_HOME		102U
#define KL_KEY_UP		103U
#define KL_KEY_PAGEUP		104U
#define KL_KEY_LEFT		105U
#define KL_KEY_RIGHT		106U
#define KL_KEY_END		107U
#define KL_KEY_DOWN		108U
#define KL_KEY_PAGEDOWN	109U
#define KL_KEY_DELETE		111U

/* The modifiers held. */
#define KL_MOD_SHIFT		1U
#define KL_MOD_CTRL		2U
#define KL_MOD_ALT		4U
#define KL_MOD_SUPER		8U

uint32_t kl_key_character(uint32_t key, unsigned modifiers);

/*
 * The touch of a view of editable text (text-touch.c, libkeiui's version 2,
 * plan/ws090/design.md section 6.1): in a text editor's body and a text
 * field, one finger's drag selects and two fingers scroll.
 *
 * A tap puts the caret, a double tap selects a word, one finger's drag
 * selects from where it touched (the content scrolls by itself while the
 * finger is near the view's edge), and a long press asks for the context
 * menu.  A selection made by touch shows a handle at each end; dragging a
 * handle moves that end.  Two fingers are the scroll's (kl_ui gives them
 * to it).
 *
 * The view gives three answers in its content's coordinates (the scroll
 * already undone): the text position nearest a point, the caret's
 * rectangle at a position, and the word around a position.  Positions are
 * whatever the view counts in (byte offsets in Text Editor).
 */
struct kl_text_view {
	size_t (*position_at)(void *data, double x, double y);
	void (*caret_rect)(void *data, size_t position, struct kl_rect *rect);
	void (*word_at)(void *data, size_t position, size_t *start, size_t *end);
};

/* What the fingers changed, for kl_text_touch_take. */
#define KL_TEXT_TOUCH_SELECTION	1U
#define KL_TEXT_TOUCH_MENU		2U
#define KL_TEXT_TOUCH_BAR		4U	/* KL_VERSION 74: the bar came or went */

/* The handles: their drawn diameter and the diameter a finger finds them within. */
#define KL_TEXT_HANDLE		12
#define KL_TEXT_HANDLE_REACH	44

/* How near the view's edge a selecting finger scrolls the content, and how fast at the edge (pixels a second). */
#define KL_TEXT_EDGE		24
#define KL_TEXT_EDGE_SPEED	1200.0

/* Which end of the selection a handle drag moves. */
#define KL_TEXT_HANDLE_NONE	0
#define KL_TEXT_HANDLE_ANCHOR	1
#define KL_TEXT_HANDLE_CARET	2

struct kl_text_touch {
	/* The view's answers and their data. */
	const struct kl_text_view *view;
	void *data;

	/* The selection: from the anchor to the caret (equal: only a caret). */
	size_t anchor;
	size_t caret;

	/*
	 * Whether a finger is selecting, which handle it holds, whether the
	 * handles show, the finger (content coordinates), and how far the finger
	 * holding a handle is from the middle of its end's caret (subtracted, so
	 * that the end follows the caret's line, not the knob's below it).
	 */
	int selecting;
	int handle;
	int handles;
	double finger_x;
	double finger_y;
	double grip_x;
	double grip_y;

	/* What changed since kl_text_touch_take, and where the context menu was asked for (window coordinates). */
	unsigned changes;
	double menu_x;
	double menu_y;

	/*
	 * KL_VERSION 74 (ws190-p002): whether the bar of editing buttons
	 * (kl_text_bar_*) shows over the selection.  It is the struct's last
	 * field: a program built with an older header has a touch without it
	 * and must not give that touch to kl_ui_text_region of a library of 74
	 * or later.
	 */
	int bar;
};

void kl_text_touch_init(struct kl_text_touch *touch, const struct kl_text_view *view, void *data);
void kl_text_touch_set_selection(struct kl_text_touch *touch, size_t anchor, size_t caret);
void kl_text_touch_tap(struct kl_text_touch *touch, double x, double y, int twice);
void kl_text_touch_long_press(struct kl_text_touch *touch, double window_x, double window_y);
void kl_text_touch_drag_begin(struct kl_text_touch *touch, double x, double y);
void kl_text_touch_drag(struct kl_text_touch *touch, double x, double y);
void kl_text_touch_drag_end(struct kl_text_touch *touch);
int kl_text_touch_edge(const struct kl_text_touch *touch, const struct kl_scroll *scroll, double *vx, double *vy);
unsigned kl_text_touch_take(struct kl_text_touch *touch);
void kl_text_touch_select(struct kl_text_touch *touch, size_t anchor, size_t caret);
void kl_text_touch_hide_bar(struct kl_text_touch *touch);
void kl_text_touch_toggle_bar(struct kl_text_touch *touch);
void kl_text_touch_draw_handles(const struct kl_text_touch *touch, struct kl_canvas *canvas, double origin_x, double origin_y, const struct kl_theme *theme);

/*
 * KL_VERSION 74 (ws190-p002, plan/ws190/phase001/phase.md section 2.2):
 * the bar of editing buttons over a selection the fingers made -- Cut,
 * Copy, Paste and Select All, those that apply.  kl_text_bar_buttons
 * chooses the buttons from what the text is (KL_TEXT_BAR_SELECTED ...),
 * kl_text_bar_layout places the bar above the selection (below it when
 * there is no room, over it when neither fits) within bounds, kl_text_bar_hit
 * records its buttons for the input of a frame and reports the one clicked
 * since the last frame, and kl_text_bar_draw draws it.  The library's
 * fields and text area show it themselves; a text view of a program's own
 * (Text Editor) uses these calls.  Ids from 0xfffffff0 up are the
 * library's own and are not used by programs.
 */
#define KL_TEXT_BAR_CUT		1U
#define KL_TEXT_BAR_COPY	2U
#define KL_TEXT_BAR_PASTE	4U
#define KL_TEXT_BAR_SELECT_ALL	8U
#define KL_TEXT_BAR_BUTTONS	4U

/* What the bar is chosen from (bits for kl_text_bar_buttons). */
#define KL_TEXT_BAR_SELECTED	1U	/* the selection is not empty */
#define KL_TEXT_BAR_WHOLE	2U	/* the selection is the whole text */
#define KL_TEXT_BAR_EMPTY	4U	/* the text is empty */
#define KL_TEXT_BAR_SECRET	8U	/* a secret field's: nothing is copied out of it */
#define KL_TEXT_BAR_READ_ONLY	16U	/* nothing is cut out of it or pasted into it */
#define KL_TEXT_BAR_CLIPBOARD	32U	/* a window's clipboard is there */
#define KL_TEXT_BAR_CAN_PASTE	64U	/* the clipboard has text to paste */

/* The bar's height, the gap between it and the selection, and a button's margin on each side of its label. */
#define KL_TEXT_BAR_HEIGHT	36
#define KL_TEXT_BAR_GAP		8
#define KL_TEXT_BAR_MARGIN	12

/*
 * A bar laid out: the buttons it shows (KL_TEXT_BAR_* bits), its rectangle
 * in the window, and its cells from the left, each a button's kind and
 * rectangle.  count is 0 for no bar.
 */
struct kl_ui;
struct kl_style;

struct kl_text_bar {
	unsigned buttons;
	struct kl_rect rect;
	size_t count;
	unsigned kinds[KL_TEXT_BAR_BUTTONS];
	struct kl_rect cells[KL_TEXT_BAR_BUTTONS];
};

unsigned kl_text_bar_buttons(unsigned facts);
int kl_text_bar_layout(struct kl_text_bar *bar, struct kl_text *text, unsigned buttons, const struct kl_rect *selection, const struct kl_rect *visible, const struct kl_rect *bounds);
unsigned kl_text_bar_hit(struct kl_ui *ui, uint32_t id, const struct kl_text_bar *bar, unsigned *held);
void kl_text_bar_draw(const struct kl_text_bar *bar, const struct kl_style *style, unsigned held);

/*
 * The input of a window (ui.c, libkeiui's version 2, design section 4): which
 * part of a frame a pointer, the wheel or a finger meant.
 *
 * While a frame is drawn, each part that takes input is recorded: a
 * widget (kl_ui_hit, by an id the application chooses and an index), a
 * scroll's viewport (kl_ui_scroll_region) and a view of editable text
 * (kl_ui_text_region).  Input that arrives before the next frame is
 * resolved against the parts of the frame last drawn, the latest recorded
 * first (on top): a press and a release on the same widget click it (twice
 * within 400 ms: a double click), the wheel goes to the scroll under the
 * pointer, and a finger goes by libkeiland's gestures: a tap to the widget
 * it touched (else to the text view there), a drag to the scroll or text
 * view it touched (a widget such as a list's row inside a scroll does not
 * take a drag; the scroll does).  In a text view one finger's drag selects
 * and two fingers' drag scrolls (kl_text_touch).  Input that meets no
 * part is kept for the application (kl_ui_take; a drag's distance from
 * kl_ui_drag_offset).  Each input call returns 1 when the window must
 * draw again.
 *
 * A scroll or a text touch given to kl_ui_scroll_region or
 * kl_ui_text_region must live until the next frame is drawn (the input
 * in between reaches it).
 */
struct kl_ui;

/* What a widget's record reports of the input (bits). */
#define KL_HIT_HOT		1U	/* the pointer is over it */
#define KL_HIT_ACTIVE		2U	/* a press on it is held */
#define KL_HIT_CLICKED	4U	/* pressed and released on it since the last frame */
#define KL_HIT_DOUBLE		8U	/* the click was the second of a double click or tap */
#define KL_HIT_FOCUSED	16U	/* it has the keyboard's focus (a widget that takes the keyboard) */
#define KL_HIT_TOUCHED	32U	/* libkeiui's version 5: the click was a finger's tap */

/* The input no part took. */
#define KL_EVENT_PRESS		1U
#define KL_EVENT_RELEASE	2U
#define KL_EVENT_WHEEL		3U
#define KL_EVENT_TAP		4U
#define KL_EVENT_DOUBLE_TAP	5U
#define KL_EVENT_LONG_PRESS	6U
#define KL_EVENT_DRAG_BEGIN	7U
#define KL_EVENT_DRAG_END	8U

/*
 * One input no part took: its kind, where (window coordinates), the
 * wheel's distance or a drag's velocity, the fingers down, and the id of
 * the region it happened over (a long press over a text view: that view's
 * id, 0 over none).
 */
struct kl_event {
	unsigned kind;
	double x;
	double y;
	double dx;
	double dy;
	unsigned fingers;
	uint32_t region;
	uint32_t code;
	unsigned modifiers;
};

struct kl_window_event;

struct kl_ui *kl_ui_create(void);
void kl_ui_destroy(struct kl_ui *ui);
int kl_ui_pointer_motion(struct kl_ui *ui, double x, double y);
int kl_ui_pointer_leave(struct kl_ui *ui);
int kl_ui_pointer_cancel(struct kl_ui *ui);
int kl_ui_take_damage(struct kl_ui *ui, struct kl_rect *rect);
int kl_ui_pointer_button(struct kl_ui *ui, int pressed, uint64_t now_us);
int kl_ui_wheel(struct kl_ui *ui, double dx, double dy, uint64_t now_us);
/*
 * Takes a window's scrolling (KL_WINDOW_AXIS, KL_WINDOW_AXIS_STOP): 0 when
 * nothing took it, 1 when a scroll did, KL_UI_AXIS_FLUNG (KL_VERSION 43)
 * when the fingers' lift threw the content (it flies on).
 */
#define KL_UI_AXIS_FLUNG	2
int kl_ui_axis(struct kl_ui *ui, const struct kl_window_event *event);
int kl_ui_touch_down(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us, double x, double y);
int kl_ui_touch_motion(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us, double x, double y);
int kl_ui_touch_up(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us);
int kl_ui_touch_cancel(struct kl_ui *ui, uint64_t now_us);
void kl_ui_begin(struct kl_ui *ui, uint64_t now_us);
unsigned kl_ui_hit(struct kl_ui *ui, uint32_t id, uint32_t index, const struct kl_rect *rect);
void kl_ui_scroll_region(struct kl_ui *ui, uint32_t id, const struct kl_rect *rect, struct kl_scroll *scroll);
void kl_ui_text_region(struct kl_ui *ui, uint32_t id, const struct kl_rect *rect, struct kl_scroll *scroll, struct kl_text_touch *touch);

/*
 * KL_VERSION 74 (ws190-p002): the fingers' selection of the fields and the
 * text areas.  A finger's double tap on a focused field's text selects the
 * word there with a handle at each end and the bar of editing buttons
 * (kl_text_bar_*); a finger on a handle drags that end, and the bar's
 * buttons cut, copy, paste and select all through the window's clipboard
 * (kl_ui_window_text ties it).  kl_ui_end draws the handles and the bar on
 * the canvas the field was drawn on, within the clip in force then, over
 * everything drawn before it, so a program calls it while that canvas is
 * still the frame's.  kl_ui_set_text_bar turns the selection off (enabled
 * 0: a double tap selects the whole text as a click's does) or gives where
 * the bar may stand (bounds in window coordinates; NULL: the whole canvas).
 */
void kl_ui_set_text_bar(struct kl_ui *ui, const struct kl_rect *bounds, int enabled);
int kl_ui_end(struct kl_ui *ui, uint64_t now_us);
int kl_ui_take(struct kl_ui *ui, struct kl_event *event);
int kl_ui_drag_offset(struct kl_ui *ui, uint64_t now_us, double *dx, double *dy);

/*
 * The window (window.c, present.c, present-shm.c, clipboard.c,
 * primary.c; libkeiui's version 3, plan/ws090/design.md section 5): an
 * xdg-shell toplevel of its own connection, its seat's input, the frames
 * the application draws on the CPU, and the clipboard and the primary
 * selection.
 *
 * The input arrives as events in a queue the application takes after
 * each kl_window_dispatch, in the order they came: the pointer, the
 * wheel, the keys (a held key repeats: the application calls
 * kl_window_repeat after the dispatch, so that a release read in the
 * same dispatch stops it first, BUG-111), the keyboard's focus, the
 * fingers (their times turned into CLOCK_MONOTONIC microseconds), a new
 * size and the request to close.  An application posts its own inputs
 * heard through other objects during a dispatch (a System Menu's shortcut,
 * a titlebar's control) with kl_window_post, so that they keep their
 * place among the keys (typed text, then Ctrl+S).  Input on the program's other surfaces
 * (a file chooser's window) is not the window's and never queued.
 *
 * A frame is ordinary memory of premultiplied 0xAARRGGBB words the size
 * kl_window_present_resize reported.  KL_PRESENT_VULKAN shows it through
 * a Vulkan swapchain (see-through when the compositor offers it, the way
 * the compositor's glass needs), KL_PRESENT_SHM through wl_shm buffers (for a
 * small window of a library, or where Vulkan is missing), and
 * KL_PRESENT_NONE leaves the surface to the application's own Vulkan.
 * The menus, the titlebar's controls and the glass panels stay the
 * application's (libkeiland), on the objects the accessors give.
 */
struct kl_window;
struct wl_display;
struct wl_surface;
struct wl_seat;
struct xdg_toplevel;

/* How the frames are shown. */
#define KL_PRESENT_VULKAN	0U
#define KL_PRESENT_SHM		1U
#define KL_PRESENT_NONE	2U

/* The kinds of input. */
#define KL_WINDOW_MOTION	1U
#define KL_WINDOW_LEAVE	2U
#define KL_WINDOW_BUTTON	3U
#define KL_WINDOW_AXIS		4U
#define KL_WINDOW_KEY		5U
#define KL_WINDOW_FOCUS	6U
#define KL_WINDOW_TOUCH_DOWN	7U
#define KL_WINDOW_TOUCH_MOTION	8U
#define KL_WINDOW_TOUCH_UP	9U
#define KL_WINDOW_TOUCH_CANCEL	10U
#define KL_WINDOW_RESIZE	11U
#define KL_WINDOW_CLOSE	12U
#define KL_WINDOW_POST		13U

/*
 * libkeiui's version 6: the text an input method or the compositor's on-screen keyboard
 * sends through the text input (text-input-unstable-v3), while the window
 * asks for it (kl_window_text_input): text to insert at the caret in place
 * of the selection (text), the text being composed to show at the caret
 * until it is committed or replaced (text, empty when it goes; begin and end
 * are its cursor's byte offsets, -1 when hidden), and bytes to delete
 * before and after the caret first (before, after).  They come in the order
 * of the protocol's done: delete, commit, preedit.
 */
#define KL_WINDOW_TEXT_COMMIT	14U
#define KL_WINDOW_TEXT_PREEDIT	15U
#define KL_WINDOW_TEXT_DELETE	16U

/*
 * KL_VERSION 40 (BUG-211): what a KL_WINDOW_AXIS came from (axis_source:
 * a wheel, a touch pad's fingers, or something continuous), and the end of
 * the fingers' scrolling, KL_WINDOW_AXIS_STOP, after which the content may
 * fly on (kl_ui_axis, kl_scroll_axis_stop).  It is 18 (KL_VERSION 43):
 * KL_VERSION 40 gave it 17, the number KL_WINDOW_ACTION has had since
 * KL_VERSION 26, so that a lift of the fingers came to an application of
 * kl_app as an action.
 */
#define KL_WINDOW_AXIS_STOP	18U
#define KL_AXIS_SOURCE_WHEEL		0U
#define KL_AXIS_SOURCE_FINGER		1U
#define KL_AXIS_SOURCE_CONTINUOUS	2U

/* The longest text one input carries, with its NUL (a longer one is cut at a character's start). */
#define KL_WINDOW_TEXT_MAX	256U

/* The evdev codes of the pointer's buttons. */
#define KL_BUTTON_LEFT		0x110U
#define KL_BUTTON_RIGHT	0x111U
#define KL_BUTTON_MIDDLE	0x112U

/*
 * What a window is made with.  Any pointer may be NULL: display (the
 * WAYLAND_DISPLAY one), title and application (the app_id).  width and
 * height are the size asked for until the compositor gives one.
 * fullscreen (libkeiui's version 11) asks for the full screen before the window
 * is first configured, so that its first configure is the full screen's.
 * role and token (KL_VERSION 46) make the desktop's surface instead.
 */
struct kl_window_options {
	const char *display;
	const char *title;
	const char *application;
	uint32_t width;
	uint32_t height;
	unsigned present;
	int fullscreen;
	unsigned role;
	const char *token;
};

/*
 * KL_VERSION 46: a window's role (kl_window_options' role): a toplevel
 * window, or the desktop's surface under every window (Files' desktop,
 * with the token the compositor gave its program in token).  A desktop
 * surface has no title, menus, titlebar, full screen or maximized state;
 * the compositor gives its size.
 */
#define KL_WINDOW_ROLE_TOPLEVEL	0U
#define KL_WINDOW_ROLE_DESKTOP	1U

/*
 * One input: its kind (KL_WINDOW_*), where the pointer or the finger is
 * (surface pixels), a button's or a key's code and whether it is pressed
 * (a focus: 1 when it came), whether a key is a repeat, the modifiers held
 * (KL_MOD_*), the wheel's distance in pixels, a finger's id and the time
 * it happened (a finger's, and since libkeiui's version 11 the pointer's motions and
 * buttons, from the compositor's time; otherwise when it was read), when it
 * was read, and its serial (a press's, for a popup or
 * a selection); for the text input's (libkeiui's version 6), its text, the
 * composed text's cursor, and the bytes to delete around the caret; for a
 * pen tablet's (KL_VERSION 44), its tool (KL_TABLET_*), its barrel buttons
 * held (KL_TABLET_BUTTON_*), its pressure (0 to 1, -1 for a tool without
 * it) and its tilt (degrees).
 */
struct kl_window_event {
	unsigned kind;
	double x;
	double y;
	uint32_t code;
	int pressed;
	int repeated;
	unsigned modifiers;
	double dx;
	double dy;
	int32_t id;
	uint64_t time_us;
	uint64_t arrival_us;
	uint32_t serial;
	char text[KL_WINDOW_TEXT_MAX];
	int32_t begin;
	int32_t end;
	uint32_t before;
	uint32_t after;
	unsigned axis_source;
	unsigned tool;
	unsigned buttons;
	double pressure;
	double tilt_x;
	double tilt_y;
};

struct kl_window *kl_window_open(const struct kl_window_options *options);
void kl_window_close(struct kl_window *window);
int kl_window_dispatch(struct kl_window *window, int timeout_ms);
int kl_window_take(struct kl_window *window, struct kl_window_event *event);
void kl_window_post(struct kl_window *window, uint32_t code);
int kl_window_repeat(struct kl_window *window, uint64_t now_us);
int kl_window_repeat_wait(const struct kl_window *window, uint64_t now_us);
void kl_window_set_title(struct kl_window *window, const char *title);
void kl_window_size(const struct kl_window *window, uint32_t *width, uint32_t *height);
int kl_window_present_resize(struct kl_window *window, uint32_t *width, uint32_t *height);
int kl_window_present(struct kl_window *window, const uint32_t *pixels, size_t stride);
int kl_window_present_part(struct kl_window *window, const uint32_t *pixels, size_t stride, const struct kl_rect *part);
int kl_window_see_through(const struct kl_window *window);
struct wl_display *kl_window_display(const struct kl_window *window);
struct wl_surface *kl_window_surface(const struct kl_window *window);
struct xdg_toplevel *kl_window_toplevel(const struct kl_window *window);
uint32_t kl_window_serial(const struct kl_window *window);
uint32_t kl_window_press_serial(const struct kl_window *window);
void kl_window_set_serial(struct kl_window *window, uint32_t serial);
struct wl_seat *kl_window_seat(const struct kl_window *window);
void kl_window_copy(struct kl_window *window, const char *text, size_t length);
size_t kl_window_paste(struct kl_window *window, char *text, size_t size);
int kl_window_can_paste(const struct kl_window *window);
void kl_window_text_input(struct kl_window *window, int enabled);
void kl_window_text_cursor(struct kl_window *window, int x, int y, int width, int height);

/*
 * KL_VERSION 68 (ws177-p019): what an input method reads around the caret
 * while the window asks for text: the field's text (UTF-8, NULL when the
 * application does not tell it; a longer one than KL_TEXT_SURROUNDING_MAX
 * bytes less one is cut around the caret), the caret's and the selection's
 * other end's byte offsets in it, and the field's hints (KL_TEXT_HINT_*)
 * and purpose (KL_TEXT_PURPOSE_*), text-input-v3's numbers.  Sent when it
 * changes, and each time the text input is enabled.
 */
#define KL_TEXT_SURROUNDING_MAX		4000U
#define KL_TEXT_PURPOSE_NORMAL		0U
#define KL_TEXT_PURPOSE_DIGITS		2U
#define KL_TEXT_PURPOSE_NUMBER		3U
#define KL_TEXT_PURPOSE_PHONE		4U
#define KL_TEXT_PURPOSE_URL		5U
#define KL_TEXT_PURPOSE_EMAIL		6U
#define KL_TEXT_HINT_MULTILINE		0x200U
void kl_window_text_context(struct kl_window *window, const char *text, size_t cursor, size_t anchor, unsigned hint, unsigned purpose);
void kl_window_select(struct kl_window *window, const char *text, size_t length);

/*
 * libkeiui's version 7 (ws102-p015, plan/ws102/design.md section 2.8): the
 * on-screen keyboard's inset.  The compositor tells a window how much of it the
 * keyboard covers, in the window's pixels from its right edge (the flick
 * panel's column) and from its bottom edge (the QWERTY row), when the
 * keyboard opens, closes or changes the window (before that configure);
 * both are 0 when it has closed or does not cover the window.  With a
 * compositor that does not tell, nothing happens.
 *
 * By default the caret is kept in sight: when the keyboard comes or
 * changes, the next kl_ui_end moves the scroll of the frame's text view
 * (kl_ui_text_region; the one with the keyboard's focus, else the last
 * recorded) so that the caret's line is in the middle of the part of the
 * view the keyboard leaves, as far as the scroll goes (not past the text's
 * start or end; a view that does not scroll stays).  The application may
 * hear the inset first: its callback returns 1 when it took care of it
 * itself (the default is skipped), 0 to keep the default.  The reasons are
 * KL_KEYBOARD_INSET_* of <keiland/keiland.h> (one definition since WS131 p014).
 */
typedef int (*kl_window_keyboard_inset_fn)(void *data, int right, int bottom, unsigned reason);
void kl_window_on_keyboard_inset(struct kl_window *window, kl_window_keyboard_inset_fn callback, void *data);
void kl_window_keyboard_inset(const struct kl_window *window, int *right, int *bottom);

/*
 * libkeiui's version 8 (ws102-p017, plan/ws102/design.md section 2.10): the
 * editing operations the on-screen keyboard's buttons ask for.  A window
 * tells the compositor it carries all of them out and its state, and hears them.
 * By default each becomes the keys it stands for, queued as the window's
 * own key inputs (copy Ctrl+C, cut Ctrl+X, paste Ctrl+V, undo Ctrl+Z, redo
 * Ctrl+Shift+Z, select all Ctrl+A), and select_begin and select_end start
 * and end a selection: while it is made, the keys that move the caret
 * (the arrows, Home, End, Page Up and Page Down) come with Shift, and a
 * copy or a cut ends it.  An application that knows its state tells it
 * (kl_window_edit_state: KL_EDIT_HAS_SELECTION ...); otherwise a
 * selection, undo and redo are taken to be there and paste follows the
 * clipboard.  The application may hear an operation first: its callback
 * returns 1 when it carried it out itself (the default is skipped).  The
 * operations and the state's bits are KL_EDIT_* of <keiland/keiland.h> (one
 * definition since WS131 p014).
 */
typedef int (*kl_window_edit_fn)(void *data, unsigned operation);
void kl_window_on_edit(struct kl_window *window, kl_window_edit_fn callback, void *data);
void kl_window_edit_state(struct kl_window *window, unsigned state);
int kl_window_selecting(const struct kl_window *window);
size_t kl_window_paste_primary(struct kl_window *window, char *text, size_t size);

/*
 * libkeiui's version 10 (ws090-p008, Image Viewer): the full screen.  An
 * application asks the compositor for it (or out of it), and learns from
 * the configure whether the window is fullscreen.
 */
void kl_window_set_fullscreen(struct kl_window *window, int fullscreen);
int kl_window_fullscreen(const struct kl_window *window);

/*
 * KL_VERSION 42 (ws122-p005b): what the window shows, told to the
 * compositor (wp_content_type_v1) from the window's next frame: nothing in
 * particular, a photo, a video or a game.  A fullscreen video or game may
 * then be shown without composing (the compositor's game mode).  Without
 * the compositor's protocol nothing is told (ENOTSUP).
 */
#define KL_CONTENT_NONE		0U
#define KL_CONTENT_PHOTO	1U
#define KL_CONTENT_VIDEO	2U
#define KL_CONTENT_GAME		3U
int kl_window_set_content_type(struct kl_window *window, unsigned type);

/*
 * libkeiui's version 11 (ws090-p011, Terminal): an application that waits for
 * other descriptors too (a terminal's shells) waits for them with the
 * compositor, at most KL_WINDOW_FDS_MAX of them; ready[i] says fds[i] has
 * something to read or has hung up.
 */
#define KL_WINDOW_FDS_MAX	16U
int kl_window_dispatch_fds(struct kl_window *window, const int *fds, unsigned count, int timeout_ms, int *ready);
uint64_t kl_clock_us(void);

/*
 * KL_VERSION 26 (WS131 p015, plan/ws131/design.md section 6): the
 * application.  One kl_app is one connection to the compositor: its
 * globals are learnt with one roundtrip when it opens, and its windows,
 * menus, titlebars and glass bind what they need from that one registry
 * without searching again.  Its windows' input, the actions chosen in their
 * menus and controls, and the descriptors it watches arrive as one queue of
 * kl_app_event values, in the order they happened: the application waits
 * with kl_app_dispatch and takes them with kl_app_take (a window of an
 * application has no queue of its own; kl_window_take finds nothing).  A
 * held key repeats by itself within kl_app_dispatch, after the events read
 * with it (a release read in the same dispatch stops it first, BUG-111).
 * The desktop's system (network, sound, power, kl_system) is the
 * application's too.  A window made by kl_window_open is still a
 * connection of its own.
 *
 * Every call is made from the one thread that opened the application.
 */
struct kl_app;
struct kl_system;
struct kl_glass_panel;

/* The most descriptors an application watches (16 before KL_VERSION 51, WS131 p025: the browser's network). */
#define KL_APP_FDS_MAX		64U

/* What a watched descriptor is waited for, and what it became (a hang-up or an error is always told). */
#define KL_APP_FD_READ		1U
#define KL_APP_FD_WRITE	2U
#define KL_APP_FD_HANGUP	4U

/* The kinds of an application's event. */
#define KL_APP_WINDOW		1U
#define KL_APP_FD		2U
#define KL_APP_THEME		3U

/*
 * What an application is opened with.  Either pointer may be NULL:
 * display (the WAYLAND_DISPLAY one) and application (the app_id its
 * windows get when their options name none).
 */
struct kl_app_options {
	const char *display;
	const char *application;
};

/*
 * One event of an application: its kind; for KL_APP_WINDOW the window and
 * its input (as kl_window_take gave it; KL_WINDOW_ACTION for an action
 * chosen), for KL_APP_FD the descriptor and what it became (KL_APP_FD_*);
 * KL_APP_THEME (KL_VERSION 35) says the desktop's appearance changed and
 * kl_theme_default's colours with it, for the application to draw its
 * windows again (kl_appearance_get(NULL) tells which).
 */
struct kl_app_event {
	unsigned kind;
	struct kl_window *window;
	struct kl_window_event input;
	int fd;
	unsigned ready;
};

/*
 * Opens an application: connects, learns the globals and binds what every
 * window shares.  Returns NULL with errno set: a connection's error,
 * EOPNOTSUPP (no compositor or shell), EPROTO, ENOMEM.
 */
struct kl_app *kl_app_open(const struct kl_app_options *options);

/*
 * Closes the windows still open, the system, and the connection.
 */
void kl_app_close(struct kl_app *app);

/*
 * Waits up to a timeout (milliseconds, -1 for ever; no wait while events
 * are queued or a repeat is due) for the compositor or a watched
 * descriptor, and queues what happened.  Returns 0, or -1 when the
 * connection is broken.
 */
int kl_app_dispatch(struct kl_app *app, int timeout_ms);

/*
 * Watches a descriptor for KL_APP_FD_READ and KL_APP_FD_WRITE (0 stops
 * watching it): while it is ready, each dispatch queues a KL_APP_FD event.
 * Returns 0, EINVAL, or ENOSPC past KL_APP_FDS_MAX.
 */
int kl_app_watch_fd(struct kl_app *app, int fd, unsigned events);

/*
 * Takes the oldest event.  Returns 1 with it in *event, 0 when none is queued.
 */
int kl_app_take(struct kl_app *app, struct kl_app_event *event);

/*
 * The application's system, opened the first time it is asked for; NULL
 * with errno set when the compositor has none (kl_system_open).
 */
struct kl_system *kl_app_system(struct kl_app *app);

/*
 * KL_VERSION 49 (ws156-p002): posts a notification of the application (its
 * application ID as the name shown), without waiting for its number.
 * Returns 0 when asked, ENOTSUP when the compositor takes none, or EINVAL.
 */
int kl_app_notify(struct kl_app *app, const char *title, const char *body);

/* The application's connection, for libkeiland's other objects. */
struct wl_display *kl_app_display(const struct kl_app *app);

/*
 * Makes a window of the application (options->display is not used; a
 * NULL application takes the application's).  It is configured when this
 * returns, as kl_window_open's is.  Returns NULL with errno set as
 * kl_window_open does.
 */
struct kl_window *kl_app_window_create(struct kl_app *app, const struct kl_window_options *options);

/*
 * The declarative menus, controls and glass of a window (KL_VERSION 26).
 * The application gives each as a table, as often as it likes; the
 * library compares it with the one shown and sends only what changed.
 * An item or a control chosen is queued among the window's inputs as a
 * KL_WINDOW_ACTION input: its action in code, the item's or control's ID
 * in id, and a breadcrumb's part in begin (0 otherwise).  An action's
 * state applies to every item and control of that action, in the menu,
 * the controls and later popups.  Without the compositor's System Menu,
 * Titlebar Presentation or glass, the calls return ENOTSUP and nothing is
 * shown; the window works as before.  Each returns 0 or an errno value.
 */
#define KL_WINDOW_ACTION	17U

/* An action's state (bits; 0 is enabled, unchecked and shown). */
#define KL_ACTION_DISABLED	1U
#define KL_ACTION_CHECKED	2U
#define KL_ACTION_HIDDEN	4U

/*
 * One menu item: its ID and its parent's (KL_MENU_ROOT for a top-level
 * item), its type (KL_MENU_ITEM_*), label, action, role (KL_MENU_ROLE_*)
 * and shortcut (KL_MENU_* modifiers and an XKB keysym, 0 for none) -- the
 * fields of libkeiland's menu, in one row.
 */
struct kl_menu_entry {
	uint32_t id;
	uint32_t parent;
	unsigned type;
	const char *label;
	uint32_t action;
	unsigned role;
	unsigned modifiers;
	uint32_t keysym;
};

/*
 * One titlebar control: its ID, role (KL_CONTROL_*), priority
 * (KL_PRIORITY_*), group, label, and the action its choice queues.
 */
struct kl_control_entry {
	uint32_t id;
	unsigned role;
	unsigned priority;
	unsigned group;
	const char *label;
	uint32_t action;
};

/* The window's menu in the compositor's System Menu (count 0 takes it away). */
int kl_window_set_menu(struct kl_window *window, const struct kl_menu_entry *entries, size_t count);

/* The window's titlebar controls (count 0 gives the titlebar back to the menu). */
int kl_window_set_controls(struct kl_window *window, const struct kl_control_entry *entries, size_t count);

/* The state of an action (KL_ACTION_* bits) in the window's menu, controls and popups. */
int kl_window_set_action_state(struct kl_window *window, uint32_t action, unsigned state);

/* A context menu of top-level items at (x, y) of the window, for its last press. */
int kl_window_popup_menu(struct kl_window *window, const struct kl_menu_entry *entries, size_t count, int x, int y);

/* The window's glass panels, from its next frame (count 0 takes them away). */
int kl_window_set_glass(struct kl_window *window, const struct kl_glass_panel *panels, size_t count);

/*
 * KL_VERSION 43 (WS131 p016): the text of a control that takes text (a
 * KL_CONTROL_SEARCH field).  As it is typed, a KL_WINDOW_CONTROL_TEXT
 * input brings it (the control's ID in id, the text in text); when its
 * editing ends, a KL_WINDOW_CONTROL_DONE input brings the text and how it
 * ended in code (KL_TEXT_SUBMITTED and the others).  A choice of a menu's
 * item or a control also stands as the window's last input for the
 * clipboard (its serial).
 */
#define KL_WINDOW_CONTROL_TEXT	19U
#define KL_WINDOW_CONTROL_DONE	20U

/* Sets a control's text and its placeholder (either may be NULL to leave it), after kl_window_set_controls. */
int kl_window_set_control_text(struct kl_window *window, uint32_t id, const char *text, const char *placeholder);

/* Gives a control's field the keyboard (the find field when Find is chosen). */
int kl_window_focus_control(struct kl_window *window, uint32_t id);

/*
 * KL_VERSION 44 (WS131 p018): the tabs of a window's titlebar, the
 * selections' changes, drag and drop, as Terminal had them of its own.
 *
 * Tabs: the titlebar shows the table given (count 0 takes them away, and
 * the titlebar shows the controls again, or the menu): each tab's ID (not
 * 0), title and KL_TAB_* flags of <keiland/keiland.h>, with KL_TABS_* options.  A
 * tab chosen, its close button or the new tab's button comes as a
 * KL_WINDOW_TAB input: KL_WINDOW_TAB_* in code, the tab's ID in id (0 for
 * a new one).
 *
 * The selections: a KL_WINDOW_SELECTION input says the clipboard or the
 * primary selection changed (KL_SELECTION_* in code), and whether it has
 * text (pressed).  kl_window_selection_own tells whether the window's own
 * text is the selection (a paste then takes it directly).
 *
 * Drops: a window takes the drags of the types it accepts
 * (KL_DROP_TEXT, KL_DROP_URIS: a "text/uri-list", which comes as it is),
 * as a copy.  A drag over it comes as KL_WINDOW_DROP_ENTER (the types it
 * has that the window takes in code, where it is in x and y) and
 * KL_WINDOW_DROP_LEAVE; dropped, as KL_WINDOW_DROP (the type it is read
 * as in code: the file names when it has them), and the window then takes
 * it with kl_window_take_drop (which finishes the drop).  A drop of the
 * window's own drag is taken directly.
 *
 * A drag of text out of the window starts with kl_window_drag_text from a
 * press (its serial); its end comes as KL_WINDOW_DRAG_DONE (code 1 when it
 * was dropped, 0 when not).
 */
#define KL_WINDOW_SELECTION	21U
#define KL_WINDOW_DROP_ENTER	22U
#define KL_WINDOW_DROP_LEAVE	23U
#define KL_WINDOW_DROP		24U
#define KL_WINDOW_DRAG_DONE	25U
#define KL_WINDOW_TAB		26U

/* Which selection changed. */
#define KL_SELECTION_CLIPBOARD	1U
#define KL_SELECTION_PRIMARY	2U

/* The types a drop is taken as (bits; KL_DROP_IMAGE, "image/png", KL_VERSION 70). */
#define KL_DROP_TEXT		1U
#define KL_DROP_URIS		2U
#define KL_DROP_IMAGE		4U

/* What a tab's input asks. */
#define KL_WINDOW_TAB_CHOSEN	1U
#define KL_WINDOW_TAB_CLOSE	2U
#define KL_WINDOW_TAB_NEW	3U

/* One tab: its ID (not 0), its title and its KL_TAB_* flags. */
struct kl_tab_entry {
	uint32_t id;
	const char *title;
	unsigned flags;
};

/*
 * A pen tablet: a window whose application takes it
 * (kl_window_accept_tablet) hears the pen as KL_WINDOW_TABLET_* inputs
 * with its pressure and tilt (a contact's start, moves and end, a move
 * over the window without touching it, and leaving it); any other window
 * hears the pen as the pointer.  And a held key's repeat may be turned off
 * for a window (kl_window_set_repeat).
 */
#define KL_WINDOW_TABLET_DOWN	27U
#define KL_WINDOW_TABLET_MOTION	28U
#define KL_WINDOW_TABLET_UP	29U
#define KL_WINDOW_TABLET_HOVER	30U
#define KL_WINDOW_TABLET_LEAVE	31U

/* A tablet's tool, and its barrel buttons (bits). */
#define KL_TABLET_PEN		0U
#define KL_TABLET_ERASER	1U
#define KL_TABLET_BUTTON_STYLUS	1U
#define KL_TABLET_BUTTON_STYLUS2	2U

int kl_window_accept_tablet(struct kl_window *window);

/*
 * KL_VERSION 45 (WS131 p019, Settings' own window moved here): whether the
 * window is maximized, maximizing or bringing it back and minimizing it,
 * the first screen's current mode (its refresh in millihertz; ENOENT while
 * unknown), the Vulkan device that shows the frames and the last frame's
 * times, a breadcrumb control's parts (one chosen comes as the control's
 * KL_WINDOW_ACTION with the part in begin), and the glass blurring what is
 * under the window.
 */
struct kl_present_times {
	unsigned copy_ms;
	unsigned acquire_ms;
	unsigned present_ms;
	unsigned wait_ms;
};

int kl_window_maximized(const struct kl_window *window);
void kl_window_set_maximized(struct kl_window *window, int maximized);
void kl_window_minimize(struct kl_window *window);
int kl_window_output_mode(const struct kl_window *window, int32_t *width, int32_t *height, int32_t *refresh);
const char *kl_window_device_name(const struct kl_window *window);
void kl_window_present_times(const struct kl_window *window, struct kl_present_times *times);
int kl_window_set_control_parts(struct kl_window *window, uint32_t id, const char *const *parts, size_t count);
int kl_window_set_glass_blur(struct kl_window *window, int enabled);

/*
 * KL_VERSION 46 (WS131 p020, Files' drag and drop and its desktop moved
 * here): the drag over a window as it moves (KL_WINDOW_DROP_MOTION) and the
 * compositor's choice of action for it (KL_WINDOW_DROP_ACTION, in code);
 * the application answers which actions it takes and the one it prefers
 * (kl_window_answer_drop; until it answers, a drag of a type it accepts is
 * taken as a copy), reads what was dropped (kl_window_receive_drop, in
 * memory the caller frees) and finishes the drop with the action carried
 * out, or gives it up with 0 (kl_window_finish_drop).  A drag out of the
 * window offers the data of a few types with the actions it allows
 * (kl_window_start_drag); its KL_WINDOW_DRAG_DONE has the compositor's
 * choice of action in begin.  A drag over a control of the titlebar comes
 * as KL_WINDOW_CONTROL_DROP (the control's ID in id, the part in begin,
 * id 0 when it left the controls).  KL_DROP_ENTER's pressed says the drag
 * is the window's own.
 */
#define KL_WINDOW_DROP_MOTION	32U
#define KL_WINDOW_DROP_ACTION	33U
#define KL_WINDOW_CONTROL_DROP	34U

/* The context menu (kl_window_popup_menu) closed, chosen from or not (KL_VERSION 46). */
#define KL_WINDOW_POPUP_DONE	35U

/* The actions of drag and drop (bits, as wl_data_device_manager's). */
#define KL_DND_COPY		1U
#define KL_DND_MOVE		2U
#define KL_DND_ASK		4U

/* One type a drag offers, and its data. */
struct kl_drag_data {
	const char *type;
	const void *data;
	size_t length;
};

void kl_window_answer_drop(struct kl_window *window, unsigned actions, unsigned preferred);
int kl_window_receive_drop(struct kl_window *window, char **data, size_t *length, unsigned *type);
void kl_window_finish_drop(struct kl_window *window, unsigned action);
int kl_window_start_drag(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions, uint32_t serial);

/* A control's value (a progress's share), a text control's suggestions, and the keyboard given to a control as KL_FOCUS_FIELD or _EDIT (KL_VERSION 46). */
int kl_window_set_control_value(struct kl_window *window, uint32_t id, unsigned value);
int kl_window_set_control_suggestions(struct kl_window *window, uint32_t id, const char *const *labels, const char *const *texts, size_t count);
int kl_window_focus_control_mode(struct kl_window *window, uint32_t id, unsigned mode);

/* Where the desktop's surface is on the screen (KL_VERSION 46). */
int kl_window_desktop_place(const struct kl_window *window, int32_t *x, int32_t *y);

/*
 * KL_VERSION 46: a window whose keys mean something else (a terminal's
 * Ctrl+C) takes the keyboard's editing buttons as the keys the compositor
 * chooses for it (a terminal's Ctrl+Shift+C and V), not as operations:
 * its edit object goes, and kl_window_on_edit's callback hears nothing.
 */
void kl_window_edit_by_keys(struct kl_window *window);
int kl_window_set_repeat(struct kl_window *window, int enabled);
int kl_window_set_tabs(struct kl_window *window, const struct kl_tab_entry *tabs, size_t count, unsigned options);
int kl_window_selection_own(const struct kl_window *window, unsigned which);
int kl_window_accept_drops(struct kl_window *window, unsigned types);
size_t kl_window_take_drop(struct kl_window *window, char *text, size_t size, unsigned *type);
int kl_window_drag_text(struct kl_window *window, const char *text, size_t length, uint32_t serial);

/*
 * KL_VERSION 70 (ws189-p002, plan/ws189/phase001/phase.md section 2): drag
 * and drop of pictures between applications.
 *
 * A window that accepts KL_DROP_IMAGE takes a drag of "image/png"; a drag
 * with several of the types a window accepts is read as its file names
 * first, then its picture, then its text.  A drop is read up to 1 MiB of
 * file names, 64 MiB of a picture and 16 MiB of text (E2BIG past it).
 *
 * kl_window_answer_drop answers for the place the drag is over: an
 * application may answer at each KL_WINDOW_DROP_ENTER and _MOTION (only a
 * changed answer is sent), and actions 0 says the drop is not taken there,
 * which the compositor shows on the drag.
 *
 * A drag out of the window may carry a picture under the pointer
 * (kl_window_start_drag_icon): premultiplied pixels, shown at most
 * KL_DRAG_ICON_MAX pixels on its longer side and a little see-through,
 * with the point hot_x, hot_y of them under the pointer.  Without shared
 * memory the drag goes without its picture.  A type whose data must be
 * made (a picture encoded) may start with NULL data and be filled with
 * kl_window_drag_fill before the program dispatches again: the drag
 * starts while the button is held, and its data is asked for only after
 * a drop.  A drop of a drag of another window of the same program is read
 * directly, as the window's own is.
 *
 * kl_ui_pointer_cancel forgets the press a widget holds when the press
 * became a drag (the compositor keeps its release).
 *
 * A window lights where a drop would land in the same way as every other
 * application: kl_drop_frame around a place (a picture's frame, a file's
 * cell), kl_drop_caret at the point where text would go, both in the
 * theme's accent.  An application that draws its own pixels uses the same
 * sizes (KL_DROP_RING, KL_DROP_CARET) and the fill's alpha
 * (KL_DROP_FILL_ALPHA of 255).
 */
#define KL_DRAG_ICON_MAX	160
#define KL_DROP_RING		2
#define KL_DROP_CARET		2
#define KL_DROP_FILL_ALPHA	31U

/* A drag's picture: premultiplied 0xAARRGGBB pixels without padding, and the point of them under the pointer. */
struct kl_drag_icon {
	const uint32_t *pixels;
	int width;
	int height;
	int hot_x;
	int hot_y;
};

int kl_window_start_drag_icon(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions, uint32_t serial, const struct kl_drag_icon *icon);
int kl_window_drag_fill(struct kl_window *window, const char *type, const void *data, size_t length);
void kl_drop_frame(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float width, float height, float radius);
void kl_drop_caret(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float height);

/*
 * A Vulkan surface over a window shown with KL_PRESENT_NONE, for an
 * application drawing with its own Vulkan instance (which enabled
 * VK_KHR_wayland_surface).  Declared for a program that included the
 * Vulkan header first.  Returns 0, EINVAL, or EIO when Vulkan refused.
 */
#if defined(VK_VERSION_1_0)
int kl_window_vulkan_surface(struct kl_window *window, VkInstance instance, VkSurfaceKHR *surface);
#endif

/*
 * The widgets (widgets.c, field.c, list.c, cards.c; libkeiui's version 4,
 * plan/ws090/design.md section 3): each is drawn by one call during a
 * frame, which also records where it is for the input and reports what
 * the input did to it since the last frame (the immediate way of section
 * 2).  What a widget remembers between frames -- a field's text, a list's
 * selection and scroll -- is the application's, in a small struct it
 * keeps.  A widget draws with a style: the canvas of the frame, the text,
 * the theme, and whether the window stands on glass.
 *
 * The keyboard's focus is on one widget at a time (by id and index).  A
 * click or a tap on a widget that takes the keyboard gives it the focus;
 * Tab and Shift+Tab move it through those widgets in the order they were
 * drawn.  The keys a focused widget does not take, and every key while no
 * widget has the focus, are the application's (KL_EVENT_KEY).
 */
struct kl_style {
	struct kl_canvas *canvas;
	struct kl_text *text;
	const struct kl_theme *theme;
	int glass;
};

/* A key no widget took (an event's kind; kl_event's code and modifiers name it). */
#define KL_EVENT_KEY		9U

/* A button's look and state (bits). */
#define KL_BUTTON_PRIMARY	1U
#define KL_BUTTON_DANGER	2U
#define KL_BUTTON_DISABLED	4U

/* KL_VERSION 60 (ws090-p023): a picture's button that is a circle, and one whose ground shows only under the pointer. */
#define KL_BUTTON_ROUND	8U
#define KL_BUTTON_QUIET	16U

/* What a text field reports (bits). */
#define KL_FIELD_CHANGED	1U
#define KL_FIELD_SUBMITTED	2U
#define KL_FIELD_CANCELLED	4U

/* What a list reports (bits). */
#define KL_LIST_SELECTED	1U
#define KL_LIST_ACTIVATED	2U
#define KL_LIST_TOUCHED	4U	/* libkeiui's version 5: the row was chosen by a finger's tap */

/* The longest text a field holds, with its NUL. */
#define KL_FIELD_MAX		512U

/*
 * A one-line text field's state: its UTF-8 text, the caret and the other
 * end of the selection (byte offsets on character boundaries), how far the
 * text is scrolled across, whether its characters are shown as dots, and
 * (KL_VERSION 47) whether it takes no input method though its characters
 * show (an address, a key shown as typed; a secret field takes none
 * either), and (KL_VERSION 64, ws177-p004) the most bytes its text may
 * hold: what the receiver of the text takes, so a longer text cannot be
 * typed (0, as a zeroed field starts, is the field's room, KL_FIELD_MAX
 * less one).
 */
struct kl_field {
	char text[KL_FIELD_MAX];
	size_t length;
	size_t caret;
	size_t anchor;
	int scroll;
	int secret;
	int plain;
	size_t limit;
};

/*
 * KL_VERSION 47 (ws090-p022): a text area's state, text of several lines
 * (kl_text_area): its UTF-8 text, the caret and the other end of the
 * selection (byte offsets on character boundaries), how far it is scrolled
 * down, and the place across Up and Down keep (-1 for none).
 */
#define KL_TEXT_AREA_MAX	8192U
struct kl_text_area {
	char text[KL_TEXT_AREA_MAX];
	size_t length;
	size_t caret;
	size_t anchor;
	int scroll;
	int goal_x;
};

/*
 * A list's state: how many items it has, the one selected (-1 for none),
 * and its scroll.
 */
struct kl_list {
	size_t count;
	long selected;
	struct kl_scroll scroll;
};

/* The keyboard's focus, and where the pointer is for a widget that follows it. */
int kl_ui_key(struct kl_ui *ui, uint32_t key, int pressed, unsigned modifiers);
void kl_ui_set_focus(struct kl_ui *ui, uint32_t id, uint32_t index);
void kl_ui_clear_focus(struct kl_ui *ui);
int kl_ui_has_focus(const struct kl_ui *ui, uint32_t id, uint32_t index);
void kl_ui_pointer(const struct kl_ui *ui, double *x, double *y);

/*
 * KL_VERSION 38 (BUG-203): the text an input method or the on-screen
 * keyboard sends, for the widget with the focus.  The application gives
 * each KL_WINDOW_TEXT_* input of its window to kl_ui_text: a text field
 * puts a commit in place of its selection, deletes the bytes around its
 * caret, and shows the text being composed at its caret.  After each frame
 * kl_ui_text_wanted tells whether the focused widget takes text, with its
 * caret's rectangle in the window, for kl_window_text_input and
 * kl_window_text_cursor.
 */
int kl_ui_text(struct kl_ui *ui, const struct kl_window_event *event);
int kl_ui_text_wanted(const struct kl_ui *ui, struct kl_rect *caret);

/*
 * KL_VERSION 47 (ws090-p022): the wiring every program with widgets on a
 * window needs, so that each field takes an input method's text.
 * kl_ui_window_input gives one input of the window to the widgets (the
 * pointer, the main button, the wheel, the keys, the fingers and the text
 * input's KL_WINDOW_TEXT_*) and returns 1 when it was theirs, 0 for another
 * kind (the program's).  kl_ui_window_text, after each frame, asks for the
 * window's text input while the focused widget takes text and tells where
 * its caret is (kl_ui_text_wanted, kl_window_text_input and
 * kl_window_text_cursor); a secret field takes none.
 */
int kl_ui_window_input(struct kl_ui *ui, const struct kl_window_event *event);
void kl_ui_window_text(struct kl_ui *ui, struct kl_window *window);

/* The widgets, each drawn and asked by one call during a frame. */
int kl_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, const char *label, unsigned flags);
int kl_button_width(const struct kl_style *style, const char *label);

/*
 * KL_VERSION 60 (ws090-p023): a button that is a picture alone, one of
 * several under an id (index), filling its rectangle: the control's
 * rounded square within its edge, or a circle (KL_BUTTON_ROUND); a quiet
 * one (KL_BUTTON_QUIET) shows its ground only under the pointer or while
 * held.  The icon, pixels square, is in the middle.  KL_BUTTON_DISABLED
 * fades it.  Reports 1 when it was pressed since the last frame.
 *
 * kl_button, kl_icon_button and kl_sidebar_place also draw with a NULL
 * ui (an application whose kl_ui could not be made): unlit, and never
 * pressed.
 */
int kl_icon_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, int pixels, unsigned flags);
int kl_switch(struct kl_ui *ui, const struct kl_style *style, uint32_t id, int x, int y, int *on, unsigned flags);
int kl_slider(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, double minimum, double maximum, double step, double *value);
/* KL_VERSION 48 (ws090-p023): a slider with flags (KL_BUTTON_DISABLED: faded, no input). */
int kl_slider_flags(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, double minimum, double maximum, double step, double *value, unsigned flags);
void kl_field_set(struct kl_field *field, const char *text);
void kl_field_set_limit(struct kl_field *field, size_t limit);
unsigned kl_field(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_field *field, const char *placeholder);
void kl_text_area_set(struct kl_text_area *area, const char *text);
unsigned kl_text_area(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_text_area *area, const char *placeholder);
int kl_list_init(struct kl_list *list);
void kl_list_release(struct kl_list *list);
unsigned kl_list_begin(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_list *list, size_t count, size_t *first, size_t *last);
unsigned kl_list_row(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_list *list, size_t index, struct kl_rect *row, kl_color *ink);
void kl_list_end(struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *rect, struct kl_list *list);

/*
 * KL_VERSION 53 (ws090-p024): the parts of the views of many items, in
 * Files' look -- a list of several columns under a header whose titles sort
 * it, and a grid of icons with their names -- where many items are
 * selected at once and a rubber band picks them.  The application keeps the
 * selection and the hits, lays out its columns, and draws each icon; these
 * draw the rest.  kl_list_header draws the header in its rectangle (each
 * column's title at its x and width, KL_COLUMN_* flags), kl_list_item a
 * row's ground for its state (KL_ITEM_* bits) with the inks of its text,
 * kl_list_cell a cell's text; kl_grid_layout measures a grid (as many
 * columns as fit, centred), kl_grid_cell places a cell, kl_grid_icon its
 * icon, and kl_grid_item draws a cell's ground and its name (returning the
 * name's last baseline); kl_band draws a rubber band.
 */
#define KL_ITEM_SELECTED	1U	/* the item is selected */
#define KL_ITEM_FOCUSED	2U	/* the view has the keyboard: a selection in the accent */
#define KL_ITEM_HOVER		4U	/* the pointer is over the item */

#define KL_COLUMN_SORTED	1U	/* the list is sorted by the column */
#define KL_COLUMN_REVERSED	2U	/* ... the other way */
#define KL_COLUMN_HOVER	4U	/* the pointer is over its title */

#define KL_CELL_RIGHT		1U	/* a cell's text at the column's right (a size) */

/* The longest second line of a grid item's name, with its NUL. */
#define KL_GRID_NAME_MAX	256U

struct kl_column {
	const char *title;
	int x;
	int width;
	unsigned flags;
};

struct kl_grid {
	int columns;
	int left;
	int top;
	int cell_width;
	int cell_height;
	size_t rows;
	int content_height;
};

void kl_list_header(const struct kl_style *style, const struct kl_rect *rect, const struct kl_column *columns, size_t count);
void kl_list_item(const struct kl_style *style, const struct kl_rect *row, unsigned state, kl_color *ink, kl_color *faint);
void kl_list_cell(const struct kl_style *style, int x, int width, int baseline, const char *text, unsigned pixels, unsigned flags, kl_color ink);
void kl_grid_layout(const struct kl_rect *rect, int cell_width, int cell_height, size_t count, struct kl_grid *grid);
void kl_grid_cell(const struct kl_grid *grid, size_t index, int scroll, struct kl_rect *cell);
void kl_grid_icon(const struct kl_rect *cell, int icon, struct kl_rect *place);
int kl_grid_item(const struct kl_style *style, const struct kl_rect *cell, int icon, const char *name, unsigned pixels, unsigned state);
void kl_band(const struct kl_style *style, const struct kl_rect *rect);
int kl_sidebar_section(const struct kl_style *style, int x, int y, int width, const char *title);
int kl_sidebar_item(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, const char *label, int current);
/* KL_VERSION 48 (ws090-p023): a sidebar's place in the faint ink (not there) or the quiet one (not ready). */
#define KL_PLACE_FAINT		1U
#define KL_PLACE_QUIET		2U
int kl_sidebar_place(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, const char *label, int current, unsigned flags);
void kl_panel(const struct kl_style *style, const struct kl_rect *rect, int sidebar);
int kl_card(const struct kl_style *style, const struct kl_rect *rect, const char *title, const char *subtitle);
int kl_row(const struct kl_style *style, int x, int y, int width, const char *label, const char *value, int last);
int kl_header(const struct kl_style *style, int x, int y, int width, const char *title, const char *summary);
int kl_dialog(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *area, const char *title, const char *body, const char *const *labels, int count);
void kl_chip(const struct kl_style *style, int centre_x, int bottom, const char *message);
void kl_progress(const struct kl_style *style, const struct kl_rect *rect, double fraction, uint64_t now_us);

/*
 * The file chooser (libkeiui's version 5; libkeiland's own file chooser of
 * KL_VERSION 12, moved here by ws090-p006 and made of the widgets):
 * the Open and Save As window every application shares.  It shows the
 * folders and files of a folder, the sidebar's places (Recent, Home and
 * its usual folders, Computer), the filter chosen, and in Save mode takes
 * a name and asks before a file is replaced.  The answer comes once,
 * through the listener, while the application dispatches its default
 * Wayland queue; the application then destroys the chooser.  The
 * application keeps running meanwhile, and should take no input of its
 * own until the answer comes.
 *
 * The chooser is a window of its own on the application's connection, with
 * its own wl_seat objects.  Wayland sends a client's pointer, keyboard and
 * touch events to all of its objects of a seat, so an application ignores
 * the enter, key and touch events of surfaces that are not its own (as
 * kl_window does).
 */
struct kl_file_chooser;

/* What the chooser asks for: an existing file to open, or a folder and a name to save as. */
#define KL_FILE_CHOOSER_OPEN		0U
#define KL_FILE_CHOOSER_SAVE		1U

/* How it ended: a path was chosen, or the user cancelled. */
#define KL_FILE_CHOOSER_CHOSEN		0U
#define KL_FILE_CHOOSER_CANCELLED	1U

/* The most filters one chooser offers. */
#define KL_FILE_CHOOSER_FILTERS_MAX	16U

/*
 * One filter: the label it is shown by, and the file name extensions it
 * shows, separated by spaces and without their dots ("txt md c h"),
 * compared without regard to case.  NULL or empty extensions show every
 * file.  Folders are always shown.
 */
struct kl_file_filter {
	const char *label;
	const char *extensions;
};

/*
 * What a chooser starts with.  Any pointer may be NULL.
 *
 * mode: KL_FILE_CHOOSER_OPEN or _SAVE.  title: the window's title ("Open"
 * or "Save As" when NULL).  application: the app_id the window gets, so
 * that compositor shows it as the application's.  folder: where it starts
 * (the home folder when NULL or not a folder).  name: the name Save starts
 * with, selected up to its extension.  filters, filter_count and filter:
 * the filters offered (at most KL_FILE_CHOOSER_FILTERS_MAX) and the one
 * chosen first; without filters every file is shown.  font and
 * fallback_font: the interface's font and the one for characters it lacks
 * (the system's when NULL).
 */
struct kl_file_chooser_options {
	unsigned mode;
	const char *title;
	const char *application;
	const char *folder;
	const char *name;
	const struct kl_file_filter *filters;
	size_t filter_count;
	size_t filter;
	const char *font;
	const char *fallback_font;
};

/*
 * What a chooser tells the application, once and last: how it ended
 * (KL_FILE_CHOOSER_*), the absolute path chosen (empty when cancelled),
 * and the filter chosen last.  In Save mode the user has already agreed to
 * replace a file that exists.  The chooser's window is closed by then; the
 * application destroys the chooser, from the callback or later.
 */
struct kl_file_chooser_listener {
	void (*done)(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
};

/*
 * Opens a file chooser over an application's window (parent may be NULL)
 * on the application's connection.
 *
 * Returns NULL with errno set: EINVAL (an unknown mode, too many filters,
 * a filter number past them, no listener), ENOTSUP (a compositor without
 * wl_shm or xdg_wm_base), an errno value of opening the font, ENOMEM.
 */
struct kl_file_chooser *kl_file_chooser_open(struct wl_display *display, struct xdg_toplevel *parent, const struct kl_file_chooser_options *options, const struct kl_file_chooser_listener *listener, void *data);

/*
 * Closes a chooser; one still open closes without telling.
 */
void kl_file_chooser_destroy(struct kl_file_chooser *chooser);

/*
 * The sound's playback streams (KL_VERSION 73, WS191).
 *
 * A stream plays frames a program writes: it is opened with a format, and
 * the frames go into a ring of shared memory the desktop gives it, so the
 * sound never passes the compositor.  Each stream has its own connection
 * to the compositor (from WAYLAND_DISPLAY), so it needs no window and may
 * be used from any thread: one thread writes (kl_audio_stream_write), any
 * thread may read the positions, and the controls and
 * kl_audio_stream_dispatch take the stream's lock.  A stream whose sound
 * service went (its lost event, or its connection's end) keeps working as
 * a silent sink: the frames written are taken at the stream's rate while
 * it runs, so a player's clock and its writing go on; its controls then
 * answer EPIPE (and still take effect on the sink).
 *
 * Adding a field to struct kl_audio_format changes the interface (a new
 * KL_VERSION and a call that takes the larger struct).
 */
#define KL_AUDIO_FORMAT_S16_LE	1U
#define KL_AUDIO_FORMAT_S32_LE	2U
#define KL_AUDIO_FORMAT_F32_LE	3U

/* What kl_audio_stream_dispatch found. */
#define KL_AUDIO_EVENT_DRAINED	0x1U	/* a drain asked for is complete; the stream is stopped */
#define KL_AUDIO_EVENT_UNDERRUN	0x2U	/* the device found the ring empty while running */
#define KL_AUDIO_EVENT_LOST	0x4U	/* the sound service went: the stream is a silent sink from now */

/* What a stream plays: interleaved little-endian frames. */
struct kl_audio_format {
	unsigned format;	/* KL_AUDIO_FORMAT_* */
	unsigned channels;	/* 1 or 2 */
	unsigned rate;		/* frames a second, 8000 to 192000 */
	unsigned buffer_frames;	/* the ring, 0 for the desktop's choice */
	unsigned period_frames;	/* 0 for the desktop's choice */
};

struct kl_audio_stream;

/*
 * Opens a stream, stopped, and waits (at most two seconds) until the
 * desktop made it.  Returns 0, or ENOTSUP (no Keiland, no sound streams,
 * WAYLAND_SOCKET set or WAYLAND_DISPLAY unset), ENODEV (no sound device),
 * EAGAIN (the sound service is not there now), EINVAL, EMFILE (too many
 * streams), ENOMEM, EPROTO (a ring not as asked for), EPIPE (the
 * compositor went) or ETIMEDOUT.
 */
int kl_audio_stream_open(const struct kl_audio_format *format, struct kl_audio_stream **stream);

/*
 * Closes a stream.  No other thread may use it any more.
 */
void kl_audio_stream_close(struct kl_audio_stream *stream);

/*
 * The controls, each waiting (at most two seconds) for the desktop's
 * answer: start (the device reads the ring from now), stop (a pause; what
 * is written stays), flush (drops what is written and not read; a running
 * stream keeps running, a drain ends) and drain (plays what is written,
 * then stops; KL_AUDIO_EVENT_DRAINED tells the end).  Each returns 0,
 * EPIPE (the stream is lost: the sink took the control), EBUSY, EAGAIN,
 * EIO or ETIMEDOUT.
 */
int kl_audio_stream_start(struct kl_audio_stream *stream);
int kl_audio_stream_stop(struct kl_audio_stream *stream);
int kl_audio_stream_flush(struct kl_audio_stream *stream);
int kl_audio_stream_drain(struct kl_audio_stream *stream);

/*
 * Writes up to count frames into the ring, as many as there is room for,
 * and returns how many were written (one writing thread only).
 */
size_t kl_audio_stream_write(struct kl_audio_stream *stream, const void *frames, size_t count);

/*
 * The positions, in frames since the stream was opened: written by the
 * program, consumed (taken from the ring by the device side; what is
 * written and not consumed is what the ring holds), and the position heard
 * (the clock of a player; time_ns, when not NULL, has the CLOCK_MONOTONIC
 * time it was reckoned at).  Any thread may read them.
 */
uint64_t kl_audio_stream_written(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_consumed(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_position(const struct kl_audio_stream *stream, int64_t *time_ns);

/*
 * The ring's frames.
 */
unsigned kl_audio_stream_capacity(const struct kl_audio_stream *stream);

/*
 * Takes what the desktop told without waiting, and gives the
 * KL_AUDIO_EVENT_* bits that came since the last call.  Returns 0 (also
 * when another thread holds the stream: nothing is taken then), or EPIPE
 * once the stream is lost.
 */
int kl_audio_stream_dispatch(struct kl_audio_stream *stream, unsigned *events);

#ifdef __cplusplus
}
#endif

#endif
