/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of the cpio archive's c_mode field and its magic number
 * (POSIX <cpio.h>).  The values are the standard's, in octal.
 */

#ifndef LIBC_CPIO_H
#define LIBC_CPIO_H

/* The permissions. */
#define C_IRUSR		0000400
#define C_IWUSR		0000200
#define C_IXUSR		0000100
#define C_IRGRP		0000040
#define C_IWGRP		0000020
#define C_IXGRP		0000010
#define C_IROTH		0000004
#define C_IWOTH		0000002
#define C_IXOTH		0000001

/* The set-user-ID, set-group-ID and restricted deletion bits. */
#define C_ISUID		0004000
#define C_ISGID		0002000
#define C_ISVTX		0001000

/* The types of file. */
#define C_ISDIR		0040000
#define C_ISFIFO	0010000
#define C_ISREG		0100000
#define C_ISBLK		0060000
#define C_ISCHR		0020000
#define C_ISCTG		0110000
#define C_ISLNK		0120000
#define C_ISSOCK	0140000

/* The magic number at the start of each header, as text. */
#define MAGIC		"070707"

#endif
