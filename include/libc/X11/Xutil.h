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

#ifdef __cplusplus
}
#endif

#endif
