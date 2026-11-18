/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_X11_XZED_H_
#define LIBC_X11_XZED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <X11/Xlib.h>
#include <stddef.h>

/*
 * Xzed-private string property used by desktop components.  The path
 * names an XPM file installed by the application package.
 */
#define XZED_ICON_PATH_ATOM ((Atom)0x5a000001U)

#define XZED_CURSOR_LEFT_PTR 68U
#define XZED_CURSOR_BOTTOM_LEFT 12U
#define XZED_CURSOR_BOTTOM_RIGHT 14U
#define XZED_CURSOR_HORIZONTAL 108U
#define XZED_CURSOR_VERTICAL 116U

int XzedSetIconPath(Display *, Window, const char *);
int XzedGetIconPath(Display *, Window, char **);
int XzedPutImageRGB24(Display *, Drawable, int, int, unsigned, unsigned,
    const unsigned char *, unsigned);
int XzedSetCursorShape(Display *, Window, unsigned);
int XzedSetInputMargins(Display *, Window, unsigned, unsigned, unsigned,
    unsigned);
int XzedMoveResizeWindowBuffered(Display *, Window, int, int, unsigned,
    unsigned);

/*
 * Sends an extension's request (GLX's, for libGL) and reads its reply:
 * the first 32 bytes into reply32, the rest into a malloc'd *extra.
 */
int XzedExtensionRequest(Display *, void *, size_t, unsigned char *,
    unsigned char **, size_t *);

#ifdef __cplusplus
}
#endif

#endif
