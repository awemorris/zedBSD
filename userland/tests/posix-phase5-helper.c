/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD posix phase5 helper userland behavior.
 */

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

static int create_objects(void);
static int objects_absent(void);
static int hold_file(const char *path);

/*
 * Runs the tests command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "ipc-create") == 0) {
		/* Obtains the create objects result. */
		function_result = create_objects();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "ipc-absent") == 0) {
		/* Obtains the objects absent result. */
		function_result = objects_absent();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 3 && strcmp(argv[1], "hold") == 0) {
		/* Obtains the hold file result. */
		function_result = hold_file(argv[2]);

		/* Returns the computed result. */
		return function_result;
	}

	fprintf(stderr,
		"usage: posix-phase5-helper ipc-create|ipc-absent|hold file\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the create objects operation. */
static int
create_objects(
	void)
{
	int message;
	int semaphore;
	int shared;

	message = msgget(MESSAGE_KEY, IPC_CREAT | IPC_EXCL | 0600);
	semaphore = semget(SEMAPHORE_KEY, 2, IPC_CREAT | IPC_EXCL | 0600);
	shared = shmget(SHARED_MEMORY_KEY, 4096, IPC_CREAT | IPC_EXCL | 0600);

	/* Handles the message condition. */
	if (message < 0 || semaphore < 0 || shared < 0) {
		fprintf(stderr, "phase5-helper: IPC creation: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the objects absent operation. */
static int
objects_absent(
	void)
{
	/* Handles the reported system error. */
	if (msgget(MESSAGE_KEY, 0) >= 0 || errno != ENOENT)
		return 1;

	/* Handles the reported system error. */
	if (semget(SEMAPHORE_KEY, 0, 0) >= 0 || errno != ENOENT)
		return 1;

	/* Handles the reported system error. */
	if (shmget(SHARED_MEMORY_KEY, 0, 0) >= 0 || errno != ENOENT)
		return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the hold file operation. */
static int
hold_file(
	const char *path)
{
	int descriptor;

	descriptor = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return 1;
	puts("zedBSD-POSIX-PHASE5-HOLDER-READY");
	(void)fflush(stdout);
	(void)sleep(20);
	(void)close(descriptor);

	/* Reports successful completion. */
	return 0;
}
