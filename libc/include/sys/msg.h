/* System V message queues over zedBSD kernel message queues. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_MSG_H
#define ZEDBSD_SYS_MSG_H
#include <sys/ipc.h>
#include <time.h>
struct msqid_ds { struct ipc_perm msg_perm; size_t msg_qnum,msg_qbytes; pid_t msg_lspid,msg_lrpid; time_t msg_stime,msg_rtime,msg_ctime; };
#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000
int msgget(key_t,int);
int msgctl(int,int,struct msqid_ds *);
int msgsnd(int,const void *,size_t,int);
ssize_t msgrcv(int,void *,size_t,long,int);
#endif
