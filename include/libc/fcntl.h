/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/fcntl.h>

/*
 * Reads and writes go through one cache, so a read already sees every write
 * that completed before it.  O_RSYNC therefore asks for nothing beyond what
 * O_SYNC gives the writes, and shares its bit as it does on Linux.
 */
#define O_RSYNC	O_SYNC

/*
 * O_TTY_INIT is zero: a terminal has no parameters outside the standard
 * termios ones, so every first open already leaves it in the conforming state
 * the flag asks for.
 */
#define O_TTY_INIT	0

/*
 * The fcntl() commands that name the owner of a file's signals as a pair of a
 * kind and an identifier.  The kernel does not know these numbers: fcntl()
 * turns them into F_GETOWN and F_SETOWN, whose owner is a process identifier
 * or a negated process group identifier.  They follow the kernel's last
 * command number, and a call that reaches the kernel without fcntl() is
 * refused with EINVAL.
 */
#define F_SETOWN_EX	15
#define F_GETOWN_EX	16

/* The kinds of owner in a struct f_owner_ex. */
#define F_OWNER_PID	1
#define F_OWNER_PGRP	2

/* The advice posix_fadvise() takes about how a range of a file will be read. */
#define POSIX_FADV_NORMAL	0
#define POSIX_FADV_RANDOM	1
#define POSIX_FADV_SEQUENTIAL	2
#define POSIX_FADV_WILLNEED	3
#define POSIX_FADV_DONTNEED	4
#define POSIX_FADV_NOREUSE	5

int faccessat(int, const char *, int, int);

#include <sys/types.h>
struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};

/*
 * The owner of a file's signals as F_GETOWN_EX reports it and F_SETOWN_EX
 * takes it: a process or a process group, and its identifier.
 */
struct f_owner_ex {
	int type;
	pid_t pid;
};

int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);

/* Gives the bytes [offset, offset + length) of a file storage now. */
int posix_fallocate(int, off_t, off_t);

/* Takes advice about how the bytes [offset, offset + length) of a file will be read. */
int posix_fadvise(int, off_t, off_t, int);

#ifdef __cplusplus
}
#endif

#endif
