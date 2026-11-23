/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of the X server's Windows backend: X's UTF-8 to Windows'
 * UTF-16 and back (the clipboard and the titles), the line ends of each
 * side (LF in X, CR LF on the Windows clipboard), and the title of a
 * window with the remote host's name in front.
 *
 * Pure: no Windows header, no system call, no state; the tests run on the
 * host.  UTF-16 is held in uint16_t, which is Windows' wchar_t.
 */

#ifndef KEILAND_X11_WIN32_TEXT_H
#define KEILAND_X11_WIN32_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* The longest title given to a window, in UTF-16 units with its NUL. */
#define X11_WIN32_TITLE_MAX	512U

size_t x11_win32_utf8_to_utf16(const uint8_t *text, size_t size, int crlf, uint16_t *result, size_t capacity);
size_t x11_win32_utf16_to_utf8(const uint16_t *text, size_t count, int lf, uint8_t *result, size_t capacity);
void x11_win32_title(const char *host, const char *title, uint16_t *result, size_t capacity);

#endif
