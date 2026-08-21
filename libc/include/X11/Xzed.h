#ifndef _X11_XZED_H_
#define _X11_XZED_H_

#include <X11/Xlib.h>

/* Xzed-private string property used by desktop components.  The path names
 * an XPM file installed by the application package. */
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

#endif
