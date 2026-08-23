/* System V IPC common definitions. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_IPC_H
#define ZEDBSD_SYS_IPC_H
#include <sys/types.h>
typedef int key_t;
#define IPC_PRIVATE ((key_t)0)
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
struct ipc_perm {
	uid_t uid, gid, cuid, cgid;
	mode_t mode;
	unsigned short seq;
	key_t key;
};
key_t ftok(const char *, int);
#endif
