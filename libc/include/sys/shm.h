/* System V shared memory over zedBSD POSIX shared memory. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_SHM_H
#define ZEDBSD_SYS_SHM_H
#include <sys/ipc.h>
#include <time.h>
struct shmid_ds { struct ipc_perm shm_perm; size_t shm_segsz; pid_t shm_lpid,shm_cpid; unsigned long shm_nattch; time_t shm_atime,shm_dtime,shm_ctime; };
#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHMLBA 4096
int shmget(key_t,size_t,int);
void *shmat(int,const void *,int);
int shmdt(const void *);
int shmctl(int,int,struct shmid_ds *);
#endif
