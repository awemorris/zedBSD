/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of m and n that initialize the CABAC context variables of
 * the software H.264 decoder (9.3.1.1), kept apart from the engine
 * (h264sw-cabac.h) because they are a large table of data.
 */

#ifndef LIBMEDIA_H264SW_CABAC_INIT_H
#define LIBMEDIA_H264SW_CABAC_INIT_H

#include "h264sw.h"
#include "h264sw-cabac.h"

/* The sets of initial values: I (and SI) slices, then cabac_init_idc 0, 1 and 2 of P and B slices. */
#define H264SW_CABAC_INIT_I 0U
#define H264SW_CABAC_INIT_SETS 4U

/*
 * The (m, n) pair of each context variable and each set (Tables 9-12 to
 * 9-24): h264sw_cabac_init_mn[ctxIdx][set][0] is m and [1] is n, for
 * ctxIdx 0..459 and set H264SW_CABAC_INIT_I or 1 + cabac_init_idc.
 *
 * A table that gives one column for all slice types (ctxIdx 0..10 and
 * 60..69, Tables 9-12 and 9-17) has the same pair in all four sets.  The
 * pairs Table 9-11 does not list for a slice type (ctxIdx 11..59 for I
 * slices) are (0, 0) and are never read; ctxIdx 276 (end_of_slice_flag)
 * is (0, 0) and is never initialized.  Every m and n of these tables
 * lies within -128..127 (n reaches 127 and -94), so int8_t holds them.
 */
extern const int8_t h264sw_cabac_init_mn[H264SW_CABAC_CONTEXTS][H264SW_CABAC_INIT_SETS][2];

#endif
