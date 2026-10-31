/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The "HTML Format" clipboard format of Windows (CF_HTML, registered by
 * that name) for the Windows backend (cf-html.c): wrapping a fragment of
 * HTML for kl_window_copy, and unwrapping what another program put on the
 * clipboard.
 *
 * The format is UTF-8 text: a header of "Name:value" lines (Version,
 * StartHTML, EndHTML, StartFragment, EndFragment, optionally
 * StartSelection, EndSelection and SourceURL), then the HTML.  The offsets
 * count bytes from the start of the data.  Pure: no Windows header, no
 * system call; the tests run on the host.  Data from the clipboard is not
 * trusted: decoding never reads outside it and rejects offsets that do
 * not fit.
 */

#ifndef KEIUI_WIN32_CF_HTML_H
#define KEIUI_WIN32_CF_HTML_H

#include <stddef.h>

/*
 * The length of each offset the encoder writes: ten decimal digits with
 * leading zeros, so the header's length does not depend on the values.
 */
#define KEIUI_WIN32_CF_HTML_DIGITS	10U

/*
 * Wraps a fragment of HTML.
 *
 * fragment:   length bytes of UTF-8 HTML (what was selected); NULL only
 *             when length is 0.
 * source_url: the page's address for the SourceURL line, or NULL for none;
 *             a URL holding a CR or LF is EINVAL.
 * buffer:     size bytes; receives the data followed by a NUL (the
 *             clipboard's block holds the NUL; out_length does not count it).
 * out_length: receives the data's length; on ERANGE, the size needed
 *             without the NUL.
 * The data, exactly (each line ends in CR LF; NNNNNNNNNN is a ten-digit
 * offset):
 *   "Version:0.9\r\n"
 *   "StartHTML:NNNNNNNNNN\r\n" "EndHTML:NNNNNNNNNN\r\n"
 *   "StartFragment:NNNNNNNNNN\r\n" "EndFragment:NNNNNNNNNN\r\n"
 *   ["SourceURL:" url "\r\n"]
 *   "<html><body>\r\n<!--StartFragment-->" fragment "<!--EndFragment-->\r\n</body>\r\n</html>"
 * StartHTML is the offset of "<html>", EndHTML the data's length,
 * StartFragment the offset just after "<!--StartFragment-->" and
 * EndFragment that of "<!--EndFragment-->".
 * Returns 0, ERANGE (size cannot hold the data and its NUL; buffer then
 * holds an empty string when size is above 0), EINVAL (fragment NULL with
 * length above 0, buffer or out_length NULL, a bad URL, a total above
 * 9999999999 bytes).
 */
int keiui_win32_cf_html_encode(const char *fragment, size_t length, const char *source_url, char *buffer, size_t size, size_t *out_length);

/*
 * Finds the fragment (and the source URL) in CF_HTML data.
 *
 * data, length: the clipboard's bytes (the NUL at the end, if any, may be
 *               included in length or not).
 * fragment, fragment_length: receive where the fragment lies in data: from
 *               StartFragment to EndFragment when both are present and
 *               valid; otherwise from StartHTML to EndHTML (a
 *               StartHTML of -1, allowed by Version 1.0, means no HTML
 *               context: then StartFragment and EndFragment must be valid).
 * source_url, source_url_length: receive the SourceURL line's value
 *               (pointing into data), or NULL and 0 when absent; may be
 *               NULL to ignore.
 * Header rules: lines end in CR LF, LF or CR; names are matched exactly
 * (case-sensitive) before the first ':'; unknown names are skipped; the
 * header ends at the first line that does not have the form name ":"
 * value, or at the smallest offset given; an offset is decimal digits
 * (any number, leading zeros allowed) or "-1"; a value above length, or
 * an end before its start, is invalid.
 * Returns 0, EINVAL (data NULL with length above 0, fragment or
 * fragment_length NULL, no Version line, no usable pair of offsets, an
 * invalid offset).  On failure the outputs are NULL and 0.
 */
int keiui_win32_cf_html_decode(const char *data, size_t length, const char **fragment, size_t *fragment_length, const char **source_url, size_t *source_url_length);

#endif
