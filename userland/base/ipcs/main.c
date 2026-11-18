/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ipcs userland command.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

enum object_kind { OBJECT_MESSAGE, OBJECT_SHARED_MEMORY, OBJECT_SEMAPHORE };

static void usage(void);
static int list_objects(int messages, int shared_memory, int semaphores);
static int decode_name(const char *name, enum object_kind *kind, unsigned *id);
static int show_message(unsigned id);
static int show_shared_memory(unsigned id);
static int show_semaphore(unsigned id);

/*
 * Runs the ipcs command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int option, messages, shared_memory, semaphores;

	messages = 0;
	shared_memory = 0;
	semaphores = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "aqmsbcopt")) != -1) {
		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'a':
		case 'b':
		case 'c':
		case 'o':
		case 'p':
		case 't':
			break;
		case 'q':
			messages = 1;
			break;
		case 'm':
			shared_memory = 1;
			break;
		case 's':
			semaphores = 1;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind != argc) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the messages condition. */
	if (!messages && !shared_memory && !semaphores)
		messages = shared_memory = semaphores = 1;

	/* Obtains the list objects result. */
	function_result = list_objects(messages, shared_memory, semaphores);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: ipcs [-qms]\n");
}

/* Supports the list objects operation. */
static int
list_objects(
	int messages,
	int shared_memory,
	int semaphores)
{
	enum object_kind kind;
	unsigned id;
	int result;
	DIR *directory;
	struct dirent *entry;
	int failed;

	directory = opendir("/dev/shm");
	failed = 0;

	/* Handles the directory availability. */
	if (directory == NULL) {
		fprintf(stderr, "ipcs: /dev/shm: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the messages condition. */
	if (messages) {
		puts("Message Queues:\n       key       msqid        owner     "
		     "   group       bytes    messages");
	}

	/* Handles the shared memory condition. */
	if (shared_memory) {
		puts("Shared Memory:\n       key       shmid        owner      "
		     "  group       bytes     nattch");
	}

	/* Handles the semaphores condition. */
	if (semaphores) {
		puts("Semaphores:\n       key       semid        owner        "
		     "group      nsems");
	}

	/* Process each directory entry. */
	while ((entry = readdir(directory)) != NULL) {
		result = 0;

		/* Handles a failed decode name operation. */
		if (decode_name(entry->d_name, &kind, &id) != 0)
			continue;

		/* Handles the kind condition. */
		if (kind == OBJECT_MESSAGE && messages)
			result = show_message(id);
		else if (kind == OBJECT_SHARED_MEMORY && shared_memory)
			result = show_shared_memory(id);
		else if (kind == OBJECT_SEMAPHORE && semaphores)
			result = show_semaphore(id);

		/* Handles the reported system error. */
		if (result != 0 && errno != EACCES && errno != ENOENT)
			failed = 1;
	}

	/* Handles a failed closedir operation. */
	if (closedir(directory) != 0)
		failed = 1;

	/* Returns the computed result. */
	return failed;
}

/* Supports the decode name operation. */
static int
decode_name(
	const char *name,
	enum object_kind *kind,
	unsigned *id)
{
	char *end;
	unsigned long value;

	/* Selects the matching prefix. */
	if (!strncmp(name, "mq.sysv-msg-", 12)) {
		value = strtoul(name + 12, &end, 16);

		/* Checks the current endpoint. */
		if (end != name + 20 || *end != '\0')
			return -1;
		*kind = OBJECT_MESSAGE;
		*id = (unsigned)value;
		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching prefix. */
	if (!strncmp(name, "sysv-shm-", 9)) {
		value = strtoul(name + 9, &end, 16);

		/* Checks the current endpoint. */
		if (end != name + 17 || *end != '\0')
			return -1;
		*kind = OBJECT_SHARED_MEMORY;
		*id = (unsigned)value;
		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching prefix. */
	if (!strncmp(name, "sem.sysv-sem-", 13)) {
		value = strtoul(name + 13, &end, 16);

		/* Checks the current endpoint. */
		if (end != name + 21 || strcmp(end, "-0000") != 0)
			return -1;
		*kind = OBJECT_SEMAPHORE;
		*id = (unsigned)value;
		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the show message operation. */
static int
show_message(
	unsigned id)
{
	struct msqid_ds status;

	/* Handles a failed msgctl operation. */
	if (msgctl((int)id, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10zu %10zu\n",
	       (unsigned)status.msg_perm.key, id, (unsigned)status.msg_perm.uid,
	       (unsigned)status.msg_perm.gid, status.msg_qbytes,
	       status.msg_qnum);

	/* Reports successful completion. */
	return 0;
}

/* Supports the show shared memory operation. */
static int
show_shared_memory(
	unsigned id)
{
	struct shmid_ds status;

	/* Handles a failed shmctl operation. */
	if (shmctl((int)id, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10zu %10lu\n",
	       (unsigned)status.shm_perm.key, id, (unsigned)status.shm_perm.uid,
	       (unsigned)status.shm_perm.gid, status.shm_segsz,
	       status.shm_nattch);

	/* Reports successful completion. */
	return 0;
}

/* Supports the show semaphore operation. */
static int
show_semaphore(
	unsigned id)
{
	struct semid_ds status;

	/* Handles a failed semctl operation. */
	if (semctl((int)id, 0, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10lu\n", (unsigned)status.sem_perm.key,
	       id, (unsigned)status.sem_perm.uid, (unsigned)status.sem_perm.gid,
	       status.sem_nsems);

	/* Reports successful completion. */
	return 0;
}
