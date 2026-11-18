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
 * system's files: the compositor's libkeiland-backend does, on each operating
 * system.
 *
 * Each feature adds its calls here when it arrives with its first user, so
 * that nothing is promised before it exists.  The first is the System Menu:
 * an application gives the compositor the meaning of its menus -- a tree of
 * numbered items with labels, states, actions and shortcuts -- and the
 * compositor draws them in the window's title bar, or in the system bar while
 * the window is docked, and tells the application what the user chose.
 *
 * It also holds what every program that follows a finger shares, so that a
 * finger feels the same everywhere: where a touch contact is at the time a
 * frame is drawn, and how fast it moved when it lifted (the touch motion; its
 * first user is the compositor itself), and what content a finger scrolls
 * does after the finger lets go, and what the fingers mean (the scroller and
 * the gestures).
 */

#ifndef KEILAND_H
#define KEILAND_H

#include <keiland/keiland_version.h>
#include <keiland/keiland_menu.h>
#include <keiland/keiland_titlebar.h>
#include <keiland/keiland_recent.h>
#include <keiland/keiland_glass.h>
#include <keiland/keiland_motion.h>
#include <keiland/keiland_scroller.h>
#include <keiland/keiland_gesture.h>
#include <keiland/keiland_desktop.h>
#include <keiland/keiland_keyboard_inset.h>
#include <keiland/keiland_edit.h>
#include <keiland/keiland_settings.h>
#include <keiland/keiland_system.h>
#include <keiland/keiland_instance.h>
#include <keiland/keiland_appearance.h>
#include <keiland/keiland_tr.h>
#include <keiland/keiland_thread.h>
#include <keiland/keiland_socket.h>

/*
 * The desktop's shared widgets and controls: one library the desktop's
 * applications draw their parts with, so that a button, a list or a scroll
 * looks and feels the same in every one of them.
 *
 * The first layer is the drawing: a CPU canvas of premultiplied BGRA pixels,
 * the text drawn on it from TrueType fonts, the icons made of its shapes, and
 * the theme -- the colours and sizes of the Kei look.  The second is the
 * scroll, the input that finds which part of a frame a pointer, a wheel or a
 * finger meant, and the touch of a view of editable text.  The third is the
 * window: a Wayland toplevel whose input arrives as a queue of events, whose
 * CPU-drawn frames are shown through Vulkan or shared memory, and which holds
 * the clipboard and the primary selection.  The fourth is the widgets:
 * buttons, switches, sliders, text fields, lists, sidebars, cards and their
 * rows, dialogs, chips and progress bars, drawn in the Kei look of Files and
 * Settings, and the keyboard's focus among them.
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

#include <keiland/keiland_canvas.h>
#include <keiland/keiland_text.h>
#include <keiland/keiland_theme.h>
#include <keiland/keiland_icon.h>
#include <keiland/keiland_scroll.h>
#include <keiland/keiland_key.h>
#include <keiland/keiland_text_touch.h>
#include <keiland/keiland_ui.h>
#include <keiland/keiland_window.h>
#include <keiland/keiland_app.h>
#include <keiland/keiland_process.h>
#include <keiland/keiland_widgets.h>
#include <keiland/keiland_file_chooser.h>
#include <keiland/keiland_media_picker.h>
#include <keiland/keiland_audio.h>

#endif
