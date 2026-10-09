/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of the ustar archive's header block (POSIX <tar.h>): its magic
 * and version, its typeflag values and its mode bits, as the standard gives
 * them.
 */

#ifndef LIBC_TAR_H
#define LIBC_TAR_H

/* The magic field (with its trailing NUL) and the version field (without one). */
#define TMAGIC		"ustar"
#define TMAGLEN		6
#define TVERSION	"00"
#define TVERSLEN	2

/* The typeflag values. */
#define REGTYPE		'0'
#define AREGTYPE	'\0'
#define LNKTYPE		'1'
#define SYMTYPE		'2'
#define CHRTYPE		'3'
#define BLKTYPE		'4'
#define DIRTYPE		'5'
#define FIFOTYPE	'6'
#define CONTTYPE	'7'

/* The mode bits, in octal. */
#define TSUID		04000
#define TSGID		02000
#define TSVTX		01000
#define TUREAD		00400
#define TUWRITE		00200
#define TUEXEC		00100
#define TGREAD		00040
#define TGWRITE		00020
#define TGEXEC		00010
#define TOREAD		00004
#define TOWRITE		00002
#define TOEXEC		00001

#endif
