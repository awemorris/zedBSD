/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Focused SUSv4/XSI regression executable for a dedicated test image.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned failures;

static void check(int condition, const char *name);
static void test_message_queue(void);
static void test_semaphore(void);
static void test_shared_memory(void);

/*
 * Runs the tests command.
 */
int
main(
	void)
{
	struct rusage usage;
	struct itimerval timer = {{0, 0}, {0, 0}}, current;
	char path[256];
	int priority;

	check(_XOPEN_VERSION == 700 && _XOPEN_UNIX == 1,
	      "compile-time XSI advertisement");
	check(sysconf(_SC_XOPEN_VERSION) == 700 && sysconf(_SC_XOPEN_UNIX) == 1,
	      "runtime XSI advertisement");
	errno = 0;
	priority = getpriority(PRIO_PROCESS, 0);
	check(priority >= -20 && priority <= 20 && errno == 0, "getpriority");
	check(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage");
	check(setitimer(ITIMER_REAL, &timer, NULL) == 0 &&
		  getitimer(ITIMER_REAL, &current) == 0,
	      "interval timer UAPI");
	check(realpath("/", path) == path && strcmp(path, "/") == 0,
	      "realpath");

	test_message_queue();
	test_semaphore();
	test_shared_memory();
	sync();
	printf("SUSV4 RESULT: %s (%u failures)\n",
	       failures == 0 ? "PASS" : "FAIL", failures);

	/* Returns the computed result. */
	return failures == 0 ? 0 : 1;
}

/* Supports the check operation. */
static void
check(
	int condition,
	const char *name)
{
	/* Handles the condition condition. */
	if (condition) {
		printf("SUSV4 PASS: %s\n", name);
	} else {
		printf("SUSV4 FAIL: %s (errno=%d)\n", name, errno);
		failures++;
	}
}

/* Supports the test message queue operation. */
static void
test_message_queue(
	void)
{
	struct message {
		long type;
		char text[16];
	} sent = {7, "zedBSD"}, received;
	int id = msgget(IPC_PRIVATE, 0600);

	check(id >= 0, "msgget IPC_PRIVATE");

	/* Handles the id condition. */
	if (id < 0)
		return;
	check(msgsnd(id, &sent, sizeof(sent.text), 0) == 0, "msgsnd");
	memset(&received, 0, sizeof(received));
	check(msgrcv(id, &received, sizeof(received.text), 7, IPC_NOWAIT) ==
		      (ssize_t)sizeof(received.text) &&
		  received.type == 7 && strcmp(received.text, "zedBSD") == 0,
	      "msgrcv type selection");
	check(msgctl(id, IPC_RMID, NULL) == 0, "msgctl IPC_RMID");
}

/* Supports the test semaphore operation. */
static void
test_semaphore(
	void)
{
	struct sembuf operation = {0, -1, 0};
	int id;

	id = semget(IPC_PRIVATE, 1, 0600);

	check(id >= 0, "semget IPC_PRIVATE");

	/* Handles the id condition. */
	if (id < 0)
		return;
	check(semctl(id, 0, SETVAL, 2) == 0, "semctl SETVAL");
	check(semop(id, &operation, 1) == 0, "semop decrement");
	check(semctl(id, 0, GETVAL) == 1, "semctl GETVAL");
	check(semctl(id, 0, IPC_RMID) == 0, "semctl IPC_RMID");
}

/* Supports the test shared memory operation. */
static void
test_shared_memory(
	void)
{
	struct shmid_ds status;
	char *memory;
	int id;

	id = shmget(IPC_PRIVATE, 4096, 0600);

	check(id >= 0, "shmget IPC_PRIVATE");

	/* Handles the id condition. */
	if (id < 0)
		return;
	memory = shmat(id, NULL, 0);
	check(memory != (void *)-1, "shmat");

	/* Handles the memory condition. */
	if (memory != (void *)-1) {
		strcpy(memory, "shared");
		check(strcmp(memory, "shared") == 0, "shared-memory access");
		check(shmdt(memory) == 0, "shmdt");
	}
	check(shmctl(id, IPC_STAT, &status) == 0 && status.shm_segsz == 4096,
	      "shmctl IPC_STAT");
	check(shmctl(id, IPC_RMID, NULL) == 0, "shmctl IPC_RMID");
}
