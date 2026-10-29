/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of m and n that initialize the CABAC context variables of the
 * software H.264 decoder (9.3.1.1).
 */

#include "h264sw-cabac-init.h"

#include <errno.h>
#include <stddef.h>

/*
 * The (m, n) pair of each context variable and each set (Tables 9-12 to
 * 9-24).
 *
 * The values are not entered yet; the block will name its table.
 */
const int8_t h264sw_cabac_init_mn[H264SW_CABAC_CONTEXTS][H264SW_CABAC_INIT_SETS][2] = { 0 };
