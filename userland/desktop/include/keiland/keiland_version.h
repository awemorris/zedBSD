/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface version of libkeiland and its history, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_VERSION_H
#define KEILAND_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The interface version the headers of <keiland/keiland.h> describe, and
 * what each version brought:
 *
 *   2: the System Menu
 *   3: the recent files
 *   4: the titlebar
 *   5: the glass panels
 *   6: context menus
 *   7: drop targets in the titlebar
 *   8: the network
 *   9: the touch motion
 *  10: the scroller and the gestures
 *  11: the network's links, DNS and saved keys
 *  12: the file chooser (moved to the widgets with 16)
 *  13: the desktop's preferences
 *  14: the desktop surface
 *  15: the sound output's volume
 *  16: the file chooser removed, now the widgets' kl_file_chooser
 *  17: kl_glass_set_blur
 *  18: the keyboard inset
 *  19: the editing operations
 *  20: the titlebar's sheet mode
 *  21: whether a sound service runs
 *  22: the network and the sound moved to kl_system_*, the old network and sound calls removed
 *  23: the machine's monitor, kl_system_monitor_*
 *  24: kl_system_network_set_scanning
 *  25: kl_titlebar_set_suggestions
 *  26: the application, kl_app_*, and the declarative menus, controls and glass of a window
 *  27: kl_system_account_set_password
 *  28: the removable volumes, kl_system_devices_mount
 *  29: the wired interfaces' configuration, kl_system_network_configure_wired
 *  30: Remote Login, kl_system_sharing_*
 *  31: the administration of the accounts, kl_system_account_administer
 *  32: one copy of a program and the activation, kl_instance_* and kl_activation_*
 *  33: a removable device's file system and size, kl_system_devices_info
 *  34: the lock screen's PIN, kl_system_account_set_pin
 *  35: the desktop's appearance, light or dark, kl_appearance_*
 *  36: what the user has enrolled, kl_system_account_enrolled
 *  37: the translations, kl_tr_*
 *  38: an input method's text for the focused widget, kl_ui_text and kl_ui_text_wanted
 *  39: a network link's speed, kl_network_link's link_mbps
 *  40: a touch pad's scrolling that flies on, kl_window_event's axis_source and
 *      KL_WINDOW_AXIS_STOP, kl_ui_axis, kl_scroll_axis and kl_axis_track
 *  41: the one inertia of every program, a touch pad's fingers on kl_scroller, kl_scroller_axis,
 *      _axis_stop and _axis_holding, kl_scroller_release's answer
 *  42: what a window shows, kl_window_set_content_type
 *  43: KL_WINDOW_AXIS_STOP is 18, apart from KL_WINDOW_ACTION
 *  44: a window's tabs, its selections' changes, drag and drop, kl_window_set_tabs and the others
 *  45: a window maximized, minimized, its screen's mode, its device, its frame's times and a
 *      breadcrumb's parts
 *  46: the desktop surface's role, drag and drop's actions and data, a drag over the titlebar's
 *      controls
 *  47: a window's input for its widgets and the text input of their fields, kl_ui_window_input and
 *      kl_ui_window_text, and the text area
 *  48: kl_slider_flags, kl_sidebar_place, kl_text_companions
 *  49: notifications, kl_system_notify, kl_system_notify_withdraw, kl_system_take_notify_event and
 *      kl_app_notify
 *  50: the recent list emptied and stopped, kl_recent_clear, kl_recent_set_keep and kl_recent_keep
 *  51: an application watches up to 64 descriptors, KL_APP_FDS_MAX
 *  52: the user's security keys, kl_system_account_add_key, kl_system_account_remove_key,
 *      kl_system_account_keys and kl_system_account_touched
 *  53: the views of many items, kl_list_header, kl_list_item, kl_list_cell, kl_grid_layout,
 *      kl_grid_cell, kl_grid_icon, kl_grid_item and kl_band
 *  54: the arrivals of mail, kl_system_mail_arrived, kl_system_mail_listen and
 *      kl_system_take_mail_event
 *  55: the phone, kl_system_phone_send, kl_system_phone_call and kl_system_take_phone_event
 *  56: the printers, kl_system_printers_* and kl_system_print_*
 *  57: the accent the user chose, kl_accent_get and kl_accent_values, the theme's accent_ink and
 *      accent_text
 *  58: the displays, kl_system_displays_*
 *  59: a display turned off, kl_system_displays_set_shown and KL_DISPLAY_OFF
 *  60: a button that is a picture alone, kl_icon_button, KL_ICON_DISCONNECT and KL_ICON_EJECT, and
 *      a button or a sidebar's place drawn without a kl_ui
 *  61: a scroll's own ends and rubber band, kl_scroll_set_bounds, the touch pad's times and
 *      velocity, kl_scroll_axis_at, kl_scroll_axis_stop_at and kl_scroll_axis_holding, and
 *      kl_scroll_fling's answer
 *  62: the part a change of the lit widget needs drawn again, kl_ui_take_damage, and
 *      kl_canvas_clear within the clip
 *  63: a frame shown by its changed part, kl_window_present_part
 *  64: a field's own limit, kl_field's limit and kl_field_set_limit
 *  65: the notification events a full ring lost, KL_NOTIFY_LOST, and whether a mail reader is
 *      allowed, kl_system_mail_allowed
 *  66: the recent list's stamp, kl_recent_stamp
 *  67: the text widgets' undo and redo, clipboard and words, Ctrl+Z, Ctrl+Y, Ctrl+C, Ctrl+X,
 *      Ctrl+V, Ctrl+Left and Ctrl+Right, with kl_ui_window_text tying the input to its window
 *  68: what an input method reads around the caret, kl_window_text_context
 *  69: what Settings reads of the computer, kl_system_machine_*
 *  70: drag and drop of pictures between applications, KL_DROP_IMAGE, kl_window_start_drag_icon,
 *      kl_window_drag_fill, kl_drop_frame, kl_drop_caret and kl_ui_pointer_cancel
 *  71: the mounted file systems, kl_system_machine_mounts and KL_MACHINE_MOUNTS
 *  72: Bluetooth, kl_system_bluetooth_*
 *  73: the sound's playback streams, kl_audio_stream_*
 *  74: the fingers' selection of the fields and the text area with handles and a bar of editing
 *      buttons, kl_text_bar_*, kl_text_touch_select, kl_text_touch_hide_bar,
 *      kl_text_touch_toggle_bar, kl_text_touch's bar and KL_TEXT_TOUCH_BAR, kl_ui_set_text_bar
 *  75: a printer's name, IPP path or LPD queue changed, kl_system_printers_edit
 *  76: a network link that is a radio, kl_network_link's wireless
 *  77: a security key's own operations, kl_system_account_key_info, _key_info_get, _key_pin,
 *      _key_reset, _key_cancel, _replugged and _key_removed, KL_SYSTEM_HAS_KEY_OPS,
 *      KL_SYSTEM_CHANGED_KEYS and KL_SYSTEM_CHANGED_REPLUG
 *  78: the sign-in methods of the login and locked screens, kl_system_account_methods and
 *      _set_methods, KL_SYSTEM_HAS_METHODS
 *  79: the phone's messages synchronised, kl_system_phone_listen, _link, _watch_link, _link_set,
 *      _sync, _page_end, _send_text, _mark_read and kl_system_take_phone_item,
 *      KL_SYSTEM_HAS_PHONE_SYNC, KL_BLUETOOTH_PAIR_PHONE, and a notification's words for the lock
 *      screen, kl_system_notify_lock and kl_app_notify_lock
 *  80: the phone's contacts and calls, KL_PHONE_CONTACTS and KL_PHONE_CALLS for
 *      kl_system_phone_sync, kl_phone_link's contacts, contacts_why and record,
 *      KL_SYSTEM_HAS_PHONE_CONTACTS
 *  81: path-based media library metadata/add/apply/watch and KL_FILE_CHOOSER_MEDIA
 *  82: caller-owned MIME FD for received phone media
 *  83: directory selection with KL_FILE_CHOOSER_FOLDER
 *  84: managed media removal by stable ID
 *  85: the media picker, kl_media_picker_*
 *  86: an MMS sent with its photos and videos, kl_system_phone_send_mms
 *  87: the paired phone's calls, kl_system_phone_calls, _hf and _control
 *  88: the media requests started without waiting, kl_system_media_take
 *  89: a widget that draws the same lit or held, kl_ui_hit_still
 *  90: the parts a frame draws, kl_ui_take_frame, kl_ui_damage, kl_ui_damage_whole and
 *      kl_window_present_parts
 *  91: a window's motions merged, kl_window_set_pacing and KL_PACING_MOTION
 *  92: a window's frames paced by the compositor's, KL_PACING_FRAME and kl_window_frame_wait
 *  93: a display's angle, kl_system_displays_rotation, _rotations and _set_rotation
 *  94: a breadcrumb always shown as a box, KL_BREADCRUMB_BOXED for kl_titlebar_set_control_value
 *      and kl_window_set_control_value
 *  95: a window whose content is opaque, kl_window_set_opaque, and the parts of the desktop surface
 *      drawn, kl_desktop_set_drawn
 *  96: the catalogs read on another thread, kl_tr_read, kl_tr_use and kl_tr_discard
 *  97: events posted from any thread, kl_app_post and KL_APP_POST, kl_window_post with data
 *      (kl_window_event's data) from any thread
 *  98: the programs an application starts, kl_process_* and KL_APP_PROCESS
 *  99: a copy's requests as the application's events, kl_instance_bind and KL_APP_INSTANCE
 * 100: a program started apart from the application, kl_launch
 * 101: the desktop's language's changes as the application's events, KL_APP_LANGUAGE (event.language)
 *      and kl_app_language
 * 102: no descriptor in the interface: kl_app_watch_fd, KL_APP_FD (and KL_APP_FD_*, KL_APP_FDS_MAX),
 *      kl_app_event.fd and kl_instance_fd are gone (kl_app_post, kl_process, kl_instance_bind instead)
 * 103: a program's threads, kl_thread_*, kl_mutex_*, kl_cond_*, kl_once and kl_thread_key_*
 *      (<keiland/keiland_thread.h>)
 * 104: a program's network, kl_address_*, kl_socket_* and kl_socket_poll with its wakers
 *      (<keiland/keiland_socket.h>)
 *
 * The file chooser of KL_VERSION 12 moved to the widgets with KL_VERSION 16:
 * kl_file_chooser_* of <keiland/keiland_file_chooser.h>, made of the
 * desktop's widgets.
 *
 * The desktop's preferences (KL_VERSION 13) are gone: the desktop's settings
 * are kl_settings_* of <keiland/keiland_settings.h>, and desktop.conf is the
 * compositor's own file.
 */
#define KL_VERSION 104U

/*
 * Reports the interface version of the library that was loaded.
 *
 * A program built against this header may compare the result with
 * KL_VERSION to learn whether the library it runs with is older.
 */
unsigned kl_version(void);

#ifdef __cplusplus
}
#endif

#endif
