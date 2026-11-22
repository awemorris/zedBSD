/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the X server's Windows backend: which physical key a Windows
 * keyboard message names, as a Linux evdev code.
 *
 * The X server's keycodes are the evdev codes plus 8 (the convention of
 * every X server on Linux, so the remote X clients see the keycodes they
 * expect).  A Windows key message (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN,
 * WM_SYSKEYUP) names the key by its scan code of scan code set 1 (lParam
 * bits 16 to 23) and the extended bit (lParam bit 24, the 0xE0 prefix),
 * which say where the key is on the keyboard whatever the layout.
 *
 * Pure: no Windows header, no system call, no state; the tests run on the
 * host.
 */

#ifndef KEILAND_X11_WIN32_SCANCODE_H
#define KEILAND_X11_WIN32_SCANCODE_H

#include <stdint.h>

/* The two virtual-key codes the mapping reads (<winuser.h> values). */
#define X11_WIN32_VK_PAUSE	0x13U
#define X11_WIN32_VK_NUMLOCK	0x90U

/* The largest evdev code an X keycode can carry (255 - 8). */
#define X11_WIN32_EVDEV_MAX	247U

uint32_t x11_win32_scan_to_evdev(unsigned scan, int extended, unsigned virtual_key);
int x11_win32_evdev_to_scan(uint32_t code, unsigned *scan, int *extended);

#endif
