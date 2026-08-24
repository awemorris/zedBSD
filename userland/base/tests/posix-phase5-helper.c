/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

#define MESSAGE_KEY ((key_t)0x1234)
#define SEMAPHORE_KEY ((key_t)0x2345)
#define SHARED_MEMORY_KEY ((key_t)0x3456)

static int
create_objects(void)
{
	int message = msgget(MESSAGE_KEY, IPC_CREAT | IPC_EXCL | 0600);
	int semaphore = semget(SEMAPHORE_KEY, 2, IPC_CREAT | IPC_EXCL | 0600);
	int shared =
	    shmget(SHARED_MEMORY_KEY, 4096, IPC_CREAT | IPC_EXCL | 0600);

	if (message < 0 || semaphore < 0 || shared < 0) {
		fprintf(stderr, "phase5-helper: IPC creation: %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int
objects_absent(void)
{
	if (msgget(MESSAGE_KEY, 0) >= 0 || errno != ENOENT)
		return 1;
	if (semget(SEMAPHORE_KEY, 0, 0) >= 0 || errno != ENOENT)
		return 1;
	if (shmget(SHARED_MEMORY_KEY, 0, 0) >= 0 || errno != ENOENT)
		return 1;
	return 0;
}

static int
hold_file(const char *path)
{
	int descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return 1;
	puts("zedBSD-POSIX-PHASE5-HOLDER-READY");
	(void)fflush(stdout);
	(void)sleep(20);
	(void)close(descriptor);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "ipc-create") == 0)
		return create_objects();
	if (argc == 2 && strcmp(argv[1], "ipc-absent") == 0)
		return objects_absent();
	if (argc == 3 && strcmp(argv[1], "hold") == 0)
		return hold_file(argv[2]);
	fprintf(stderr,
		"usage: posix-phase5-helper ipc-create|ipc-absent|hold file\n");
	return 2;
}
