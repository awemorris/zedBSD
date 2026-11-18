/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A Xilinx 7-series bitstream as the Zynq's configuration DMA takes it.
 *
 * A .bit file is a header (the design's name, the part, the date and the
 * time) followed by the configuration data, whose 32-bit words are big
 * endian; a .bin file is the data alone, in either byte order.  The DMA
 * reads little-endian words, so the data it is given has the bytes of
 * each word reversed: the sync word 0xAA995566 lies in memory as the bytes
 * 66 55 99 AA.  bitstream_prepare() takes either file and gives that.
 */

#ifndef USERLAND_BASE_FPGALOAD_BITSTREAM_H
#define USERLAND_BASE_FPGALOAD_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

/* How far into the data the sync word is looked for, as the kernel looks. */
#define BITSTREAM_SYNC_SEARCH	4096U

/*
 * Finds the configuration data of a .bit or .bin file and puts it in the
 * DMA's order in place: *data and *size are left naming it.  Returns 0, or
 * EINVAL when the file is neither (a malformed header, no sync word, a
 * length that is not whole words).
 */
int bitstream_prepare(uint8_t **data, size_t *size);

#endif
