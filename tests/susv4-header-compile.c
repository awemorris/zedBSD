/* SUSv4/XSI public-header compile fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <ctype.h>
#include <dirent.h>
#include <fmtmsg.h>
#include <ftw.h>
#include <grp.h>
#include <libgen.h>
#include <math.h>
#include <ndbm.h>
#include <pthread.h>
#include <pwd.h>
#include <search.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <ulimit.h>
#include <unistd.h>
#include <utmpx.h>
#include <wchar.h>

#if _XOPEN_VERSION != 700
#error "SUSv4 requires _XOPEN_VERSION 700"
#endif
#if _XOPEN_UNIX != 1
#error "SUSv4 XSI profile must advertise _XOPEN_UNIX"
#endif

static int visit(const char *path, const struct stat *status, int flag)
{
	(void)path;
	(void)status;
	return flag == FTW_F ? 0 : 0;
}

int main(void)
{
	DBM *database = NULL;
	ENTRY entry = { NULL, NULL };
	struct msqid_ds message_status;
	struct semid_ds semaphore_status;
	struct shmid_ds shared_status;
	struct sembuf operation = { 0, 0, IPC_NOWAIT };
	struct itimerval timer;

	(void)database;
	(void)entry;
	(void)message_status;
	(void)semaphore_status;
	(void)shared_status;
	(void)operation;
	(void)timer;
	(void)visit;
	return 0;
}
