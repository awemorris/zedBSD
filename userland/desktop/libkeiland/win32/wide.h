/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-16 and UTF-8 for the Windows backend (wide.c): Windows gives and
 * takes text as UTF-16 (window titles, WM_CHAR, the input method's
 * composition, the clipboard's CF_UNICODETEXT) and Keiland as UTF-8.
 *
 * Text from the system is not trusted to be well-formed: an unpaired
 * surrogate becomes U+FFFD rather than an error, so a broken string from
 * another program still shows.  The units are uint16_t (not wchar_t) so the
 * module is pure and its tests run on the host.  No Windows header, no
 * system call, no allocation.
 */

#ifndef KEIUI_WIN32_WIDE_H
#define KEIUI_WIN32_WIDE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The state of WM_CHAR's units: Windows sends a character above U+FFFF as
 * two WM_CHAR messages, the high surrogate first.  Zero-initialize it once
 * per window.
 */
struct keiui_win32_surrogate {
	uint16_t high;		/* a high surrogate waiting for its low one, or 0 */
};

/*
 * Converts UTF-16 to UTF-8, writing whole characters only.
 *
 * units:    count units; NULL only when count is 0.
 * count:    how many units to read; a NUL unit ends the text early.
 * buffer:   size bytes; receives UTF-8 and a NUL after it (always, when
 *           size is above 0).
 * consumed: when not NULL, receives how many units were converted (a
 *           character that did not fit is not consumed, nor are its units).
 * Unpaired surrogates become U+FFFD (EF BF BD).  When the text does not fit,
 * as many whole characters as fit are written (a character is never cut),
 * and the return says so.
 * Returns 0 (all converted), ERANGE (cut short: buffer full), EINVAL
 * (buffer NULL or size 0, units NULL with count above 0).
 */
int keiui_win32_utf8_from_utf16(const uint16_t *units, size_t count, char *buffer, size_t size, size_t *consumed);

/*
 * Converts UTF-8 to UTF-16.
 *
 * text:     length bytes (NUL not required, not read past length; a NUL
 *           byte inside ends the text early).
 * units:    capacity units; receives the UTF-16 and a NUL unit after it.
 * count:    receives the units written before the NUL; on ERANGE, the
 *           units the whole text needs (without the NUL), so the caller can
 *           allocate and call again.
 * Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF, truncated or
 * stray bytes) becomes U+FFFD, one per maximal invalid subpart (Unicode
 * 15.0 section 3.9, "U+FFFD Substitution of Maximal Subparts").
 * Returns 0, ERANGE (capacity cannot hold the text and its NUL; units then
 * holds an empty string when capacity is above 0), EINVAL (text NULL with
 * length above 0, units NULL, count NULL, capacity 0).
 */
int keiui_win32_utf16_from_utf8(const char *text, size_t length, uint16_t *units, size_t capacity, size_t *count);

/*
 * Takes one WM_CHAR unit and reports the character it completes.
 *
 * state:      the window's state.
 * unit:       the unit (wParam's low 16 bits).
 * code_point: receives the character when one is complete.
 * Returns 1 with code_point set when the unit completes a character (a
 * unit outside the surrogates, or a low surrogate after a high one); 0 when
 * it is a high surrogate now waiting (state keeps it).  A high surrogate
 * that replaces a waiting one, and a low one with none waiting, give
 * U+FFFD (returns 1; a replacing high one then waits).  EINVAL is not
 * possible: NULL arguments return 0 and change nothing.
 */
int keiui_win32_char_unit(struct keiui_win32_surrogate *state, uint16_t unit, uint32_t *code_point);

#endif
