/* System V semaphore sets over zedBSD named semaphores. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_SEM_H
#define ZEDBSD_SYS_SEM_H
#include <sys/ipc.h>
#include <time.h>
struct semid_ds { struct ipc_perm sem_perm; time_t sem_otime,sem_ctime; unsigned long sem_nsems; };
struct sembuf { unsigned short sem_num; short sem_op; short sem_flg; };
#define SEM_UNDO 010000
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16
#define SETALL 17
int semget(key_t,int,int);
int semctl(int,int,int,...);
int semop(int,struct sembuf *,size_t);
#endif
