/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The byte order reversals of the GNU library's <byteswap.h>, which is not
 * in POSIX: bswap_16, bswap_32 and bswap_64 reverse the bytes of a 16-,
 * 32- and 64-bit unsigned value.  The compiler provides each as a single
 * instruction where the machine has one.  <endian.h> has the conversions
 * to and from a named byte order, which portable programs prefer.
 */

#ifndef LIBC_BYTESWAP_H
#define LIBC_BYTESWAP_H

#define bswap_16(value)	__builtin_bswap16(value)
#define bswap_32(value)	__builtin_bswap32(value)
#define bswap_64(value)	__builtin_bswap64(value)

#endif
