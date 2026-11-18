/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the character set conversion tables of dd.
 */

#ifndef USERLAND_BASE_DD_TABLES_H
#define USERLAND_BASE_DD_TABLES_H

extern const unsigned char dd_ascii_to_ebcdic[256];
extern const unsigned char dd_ascii_to_ibm[256];
extern const unsigned char dd_ebcdic_to_ascii[256];

#endif
