/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * System V message queues over zedBSD kernel message queues.
 */

#ifndef LIBC_SYS_MSG_H
#define LIBC_SYS_MSG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ipc.h>
#include <time.h>

#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000

/* The count of messages on a queue, and the bytes a queue may hold. */
typedef size_t msgqnum_t;
typedef size_t msglen_t;

struct msqid_ds {
	struct ipc_perm msg_perm;
	msgqnum_t msg_qnum;
	msglen_t msg_qbytes;
	pid_t msg_lspid, msg_lrpid;
	time_t msg_stime, msg_rtime, msg_ctime;
};

int msgget(key_t,int);
int msgctl(int,int,struct msqid_ds *);
int msgsnd(int,const void *,size_t,int);
ssize_t msgrcv(int,void *,size_t,long,int);

#ifdef __cplusplus
}
#endif

#endif
