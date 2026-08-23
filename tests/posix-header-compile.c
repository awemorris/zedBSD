/* POSIX R2 public-header compile fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dirent.h>
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/quota.h>
#include <sys/snapshot.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/process.h>

_Static_assert(ARG_MAX == ZEDBSD_ARG_MAX,
    "libc and kernel exec byte limits must agree");
_Static_assert(ZEDBSD_EXEC_VECTOR_MAX ==
    ZEDBSD_ARG_MAX / sizeof(uintptr_t),
    "exec vector capacity must follow the active ABI pointer width");

static void *thread_entry(void *argument) { return argument; }

int main(void)
{
	pthread_t thread = 0;
	struct utsname name;
	struct pollfd event = { -1, 0, 0 };
	struct flock lock = { F_UNLCK, SEEK_SET, 0, 0, 0 };
	struct rlimit limit = { 0, 0 };
	struct quota_control quota = {
		.size = sizeof(quota), .version = ZEDBSD_QUOTA_VERSION
	};
	struct snapshot_control snapshot = {
		.size = sizeof(snapshot), .version = ZEDBSD_SNAPSHOT_VERSION
	};
	posix_spawnattr_t attributes;
	(void)thread_entry;
	(void)thread;
	(void)name;
	(void)event;
	(void)lock;
	(void)limit;
	(void)quota;
	(void)snapshot;
	(void)attributes;
	return 0;
}
