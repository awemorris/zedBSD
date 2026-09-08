/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * System V IPC common definitions.
 */

#ifndef LIBC_SYS_IPC_H
#define LIBC_SYS_IPC_H

#include <sys/types.h>

#define IPC_PRIVATE ((key_t)0)
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2

typedef int key_t;

struct ipc_perm {
	uid_t uid, gid, cuid, cgid;
	mode_t mode;
	unsigned short seq;
	key_t key;
};

key_t ftok(const char *, int);

#endif
