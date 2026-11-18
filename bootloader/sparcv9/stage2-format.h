/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * On-disk zedBSD SPARC V9 second-stage header.
 * Multi-byte fields are big-endian.
 */

#ifndef BOOTLOADER_SPARCV9_STAGE2_FORMAT_H
#define BOOTLOADER_SPARCV9_STAGE2_FORMAT_H

/* The header's identity and size. */
#define SPARCV9_STAGE2_MAGIC 0x5a533932U
#define SPARCV9_STAGE2_VERSION 1U
#define SPARCV9_STAGE2_HEADER_SIZE 32U

/* Where the header and the payload are on the disk. */
#define SPARCV9_STAGE2_HEADER_LBA 32U
#define SPARCV9_STAGE2_PAYLOAD_LBA 33U

/* The window the payload is loaded into. */
#define SPARCV9_STAGE2_LOAD_ADDRESS 0x00100000UL
#define SPARCV9_STAGE2_LOAD_LIMIT 0x00400000UL

#endif
