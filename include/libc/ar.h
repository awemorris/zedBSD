/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The archive format.
 *
 * An archive is the magic string below followed by members, each one a
 * header of printable fields and then its bytes, padded to an even length.
 * The fields are written as text and are not terminated: a reader takes
 * each one to its width and stops at the first trailing space.
 *
 * The same format is what this system's own archiver reads and writes;
 * what is here is the description under the names software elsewhere uses
 * for it.
 */

#ifndef LIBC_AR_H
#define LIBC_AR_H

/* What an archive begins with, and how long that is. */
#define ARMAG		"!<arch>\n"
#define SARMAG		8

/* What every member header ends with, and how long that is. */
#define ARFMAG		"`\n"

struct ar_hdr {
	char ar_name[16];	/* the member's name, space padded */
	char ar_date[12];	/* seconds since the epoch, as decimal */
	char ar_uid[6];		/* the owner, as decimal */
	char ar_gid[6];		/* the group, as decimal */
	char ar_mode[8];	/* the mode, as octal */
	char ar_size[10];	/* the member's length, as decimal */
	char ar_fmag[2];	/* ARFMAG */
};

#endif
