/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_X11_XUTIL_H
#define LIBC_X11_XUTIL_H

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A visual as GLX and XGetVisualInfo describe it (Xzed has TrueColor, 24 bits). */
typedef struct {
	Visual *visual;
	VisualID visualid;
	int screen;
	int depth;
	int class;
	unsigned long red_mask;
	unsigned long green_mask;
	unsigned long blue_mask;
	int colormap_size;
	int bits_per_rgb;
} XVisualInfo;

#define TrueColor 4

/* A window's WM_CLASS: its instance's name and its class (ws035-p092; the desktop's application ID). */
typedef struct {
	char *res_name;
	char *res_class;
} XClassHint;

/* Sets a window's WM_CLASS. */
int XSetClassHint(Display *, Window, XClassHint *);

#ifdef __cplusplus
}
#endif

#endif
