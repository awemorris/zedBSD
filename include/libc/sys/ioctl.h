/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_IOCTL_H
#define LIBC_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <features.h>
#include <stddef.h>

/*
 * The terminal requests, and the shapes they are asked with.
 *
 * They are defined with the terminal interface, which is where they belong,
 * but portable software asks for them here: <sys/ioctl.h> is where every
 * Unix has kept them, and a program wanting the size of its window includes
 * this and expects TIOCGWINSZ and struct winsize to be in it.  Nothing new
 * is declared by this; it is the same definitions reached by the name such
 * software already looks under.
 */
#include <uapi/termios.h>

#include <uapi/ioctl.h>

#if __ZEDBSD_LEGACY_VISIBLE
int ioctl(int descriptor, unsigned long request, ...);
#endif

#ifdef __cplusplus
}
#endif

#endif
