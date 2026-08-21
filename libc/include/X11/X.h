#ifndef _X11_X_H_
#define _X11_X_H_
#include <stdint.h>
typedef uint32_t XID;
typedef XID Window;
typedef XID Drawable;
typedef XID Font;
typedef XID Pixmap;
typedef XID Colormap;
typedef XID Cursor;
typedef uint32_t Atom;
typedef uint32_t Time;
typedef uint32_t KeySym;
typedef uint8_t KeyCode;
typedef int Bool;
typedef unsigned long Mask;
#define False 0
#define True 1
#define None 0L
#define CurrentTime 0L
#define KeyPress 2
#define KeyRelease 3
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define Expose 12
#define KeyPressMask (1L<<0)
#define KeyReleaseMask (1L<<1)
#define ButtonPressMask (1L<<2)
#define ButtonReleaseMask (1L<<3)
#define PointerMotionMask (1L<<6)
#define ExposureMask (1L<<15)
#endif
