/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG path data: the 'd' attribute of a 'path' element read into a
 * vector path (SVG 2 §9.3).
 */

#ifndef KEILAND_BROWSER_SVG_PATHDATA_H
#define KEILAND_BROWSER_SVG_PATHDATA_H

#include "vector/vec-path.h"

/*
 * Parses path data into a path, appending to out.
 *
 * The grammar is SVG 2 §9.3.9's: the commands M m Z z L l H h V v C c S s
 * Q q T t A a, each followed by its argument sets, numbers as
 * svg_number_read reads them, separated by white space and/or one comma
 * where the grammar allows, a command letter repeatable implicitly by
 * further argument sets (§9.3.2: after M or m, further pairs are implicit
 * L or l commands), and the arc flags as the single characters 0 or 1
 * that need no separator after them (§9.3.9's flag).
 *
 * Meaning: §9.3.3 to §9.3.8.  Relative commands are relative to the
 * current point; after Z the current point is the subpath's first point
 * (§9.3.4); S and T reflect the previous command's second control point
 * when the previous command was C, c, S or s (for S) or Q, q, T or t (for
 * T), else the control point is the current point (§9.5.2); A goes
 * through vec_path_arc_to, which applies §9.5.1.
 *
 * Errors in the data (§9.5.4): the path keeps every complete argument set
 * before the first error, including the complete sets of the command in
 * which the error occurs ("M 10,10 L 20,20,30" keeps the line to 20,20:
 * §9.5.4's example), and drops the rest.  Data that does not start with M
 * or m (after white space) draws nothing.
 *
 * Input: text NUL-terminated UTF-8; out an initialized path (usually
 * empty).
 * Output: out with the commands appended; *complete set to 1 when the
 * whole text was valid, 0 when it stopped at an error.
 * Returns 0 (also when the data had an error: out then holds what came
 * before it), or the path's sticky error (ENOBUFS, ENOMEM, EINVAL from a
 * coordinate beyond VEC_COORD_MAX), in which case the caller draws
 * nothing of the path.
 */
int svg_path_data_parse(const char *text, struct vec_path *out, int *complete);

#endif
