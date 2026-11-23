/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the native thread (M) asks of the Java side of libkeiland, through JNI.
 *
 * Each function is called on M only, hands its work to the Java main thread and
 * returns at once without waiting for it.  It returns 0, or EIO when M has no
 * JNI environment, a class or method is missing, or Java raised an exception
 * (which is cleared), or ENOMEM.
 */

#ifndef KEILAND_ANDROID_BRIDGE_JNI_H
#define KEILAND_ANDROID_BRIDGE_JNI_H

#include <stddef.h>
#include <stdint.h>

/* Shows (1) or hides (0) the soft keyboard. */
int keiland_java_show_keyboard(int show);

/* Chooses letters only (0) or a language's input (1). */
int keiland_java_set_input_mode(int mode);

/* Tells the input method where the composing text is, in the window's pixels. */
int keiland_java_set_cursor_anchor(float x, float y, float width, float height);

/* Sets the task's title (UTF-8). */
int keiland_java_set_title(const char *title);

/* Puts length bytes of UTF-8 text on the clipboard; NULL or length 0 empties it. */
int keiland_java_clipboard_set(const char *text, size_t length);

/*
 * Shows a menu at a place.  Each of the count items has a label, the number of
 * its parent item (-1 for the top) and flags.  The answer comes back as a
 * KEILAND_BRIDGE_MENU event with this request number.
 */
int keiland_java_popup_menu(uint32_t request, int x, int y, const char *const *labels, const int32_t *parents, const int32_t *flags, size_t count);

/* Opens a web address (UTF-8) in the browser. */
int keiland_java_open_url(const char *url);

/* Closes the Activity and removes its task. */
int keiland_java_quit(void);

#endif /* KEILAND_ANDROID_BRIDGE_JNI_H */
