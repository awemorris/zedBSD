/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
decode_name(const char *name, enum object_kind *kind, unsigned *id)
{
	char *end;
	unsigned long value;

	if (!strncmp(name, "mq.sysv-msg-", 12)) {
		value = strtoul(name + 12, &end, 16);
		if (end != name + 20 || *end != '\0')
			return -1;
		*kind = OBJECT_MESSAGE;
		*id = (unsigned)value;
		return 0;
	}
	if (!strncmp(name, "sysv-shm-", 9)) {
		value = strtoul(name + 9, &end, 16);
		if (end != name + 17 || *end != '\0')
			return -1;
		*kind = OBJECT_SHARED_MEMORY;
		*id = (unsigned)value;
		return 0;
	}
	if (!strncmp(name, "sem.sysv-sem-", 13)) {
		value = strtoul(name + 13, &end, 16);
		if (end != name + 21 || strcmp(end, "-0000") != 0)
			return -1;
		*kind = OBJECT_SEMAPHORE;
		*id = (unsigned)value;
		return 0;
	}
	return -1;
}

static int
show_message(unsigned id)
{
	struct msqid_ds status;
	if (msgctl((int)id, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10zu %10zu\n",
	       (unsigned)status.msg_perm.key, id, (unsigned)status.msg_perm.uid,
	       (unsigned)status.msg_perm.gid, status.msg_qbytes,
	       status.msg_qnum);
	return 0;
}

static int
show_shared_memory(unsigned id)
{
	struct shmid_ds status;
	if (shmctl((int)id, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10zu %10lu\n",
	       (unsigned)status.shm_perm.key, id, (unsigned)status.shm_perm.uid,
	       (unsigned)status.shm_perm.gid, status.shm_segsz,
	       status.shm_nattch);
	return 0;
}

static int
show_semaphore(unsigned id)
{
	struct semid_ds status;
	if (semctl((int)id, 0, IPC_STAT, &status) != 0)
		return -1;
	printf("0x%08x %10u %10u %10u %10lu\n", (unsigned)status.sem_perm.key,
	       id, (unsigned)status.sem_perm.uid, (unsigned)status.sem_perm.gid,
	       status.sem_nsems);
	return 0;
}

static int
list_objects(int messages, int shared_memory, int semaphores)
{
	DIR *directory = opendir("/dev/shm");
	struct dirent *entry;
	int failed = 0;

	if (directory == NULL) {
		fprintf(stderr, "ipcs: /dev/shm: %s\n", strerror(errno));
		return 1;
	}
	if (messages)
		puts("Message Queues:\n       key       msqid        owner     "
		     "   group       bytes    messages");
	if (shared_memory)
		puts("Shared Memory:\n       key       shmid        owner      "
		     "  group       bytes     nattch");
	if (semaphores)
		puts("Semaphores:\n       key       semid        owner        "
		     "group      nsems");
	while ((entry = readdir(directory)) != NULL) {
		enum object_kind kind;
		unsigned id;
		int result = 0;
		if (decode_name(entry->d_name, &kind, &id) != 0)
			continue;
		if (kind == OBJECT_MESSAGE && messages)
			result = show_message(id);
		else if (kind == OBJECT_SHARED_MEMORY && shared_memory)
			result = show_shared_memory(id);
		else if (kind == OBJECT_SEMAPHORE && semaphores)
			result = show_semaphore(id);
		if (result != 0 && errno != EACCES && errno != ENOENT)
			failed = 1;
	}
	if (closedir(directory) != 0)
		failed = 1;
	return failed;
}

static void
usage(void)
{
	fprintf(stderr, "usage: ipcs [-qms]\n");
}

int
main(int argc, char **argv)
{
	int option, messages = 0, shared_memory = 0, semaphores = 0;

	while ((option = getopt(argc, argv, "aqmsbcopt")) != -1) {
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
			return 2;
		}
	}
	if (optind != argc) {
		usage();
		return 2;
	}
	if (!messages && !shared_memory && !semaphores)
		messages = shared_memory = semaphores = 1;
	return list_objects(messages, shared_memory, semaphores);
}
