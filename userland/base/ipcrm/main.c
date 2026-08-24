/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
parse_number(const char *text, long *value)
{
	char *end;

	errno = 0;
	*value = strtol(text, &end, 0);
	return errno == 0 && *text != '\0' && *end == '\0' && *value >= 0 ? 0
									  : -1;
}

static int
remove_object(enum object_kind kind, int id)
{
	switch (kind) {
	case OBJECT_MESSAGE:
		return msgctl(id, IPC_RMID, NULL);
	case OBJECT_SHARED_MEMORY:
		return shmctl(id, IPC_RMID, NULL);
	case OBJECT_SEMAPHORE:
		return semctl(id, 0, IPC_RMID);
	}
	errno = EINVAL;
	return -1;
}

static int
lookup_key(enum object_kind kind, key_t key)
{
	switch (kind) {
	case OBJECT_MESSAGE:
		return msgget(key, 0);
	case OBJECT_SHARED_MEMORY:
		return shmget(key, 0, 0);
	case OBJECT_SEMAPHORE:
		return semget(key, 0, 0);
	}
	errno = EINVAL;
	return -1;
}

static void
usage(void)
{
	fprintf(stderr, "usage: ipcrm [-q msgid] [-m shmid] [-s semid] "
			"[-Q msgkey] [-M shmkey] [-S semkey] ...\n");
}

int
main(int argc, char **argv)
{
	int option, failed = 0, operations = 0;

	while ((option = getopt(argc, argv, "q:m:s:Q:M:S:")) != -1) {
		enum object_kind kind;
		long number;
		int id;
		int by_key = option == 'Q' || option == 'M' || option == 'S';

		switch (option) {
		case 'q':
		case 'Q':
			kind = OBJECT_MESSAGE;
			break;
		case 'm':
		case 'M':
			kind = OBJECT_SHARED_MEMORY;
			break;
		case 's':
		case 'S':
			kind = OBJECT_SEMAPHORE;
			break;
		default:
			usage();
			return 2;
		}
		operations++;
		if (parse_number(optarg, &number) != 0) {
			fprintf(stderr, "ipcrm: invalid identifier: %s\n",
				optarg);
			failed = 1;
			continue;
		}
		id = by_key ? lookup_key(kind, (key_t)number) : (int)number;
		if (id < 0 || remove_object(kind, id) != 0) {
			fprintf(stderr, "ipcrm: %s: %s\n", optarg,
				strerror(errno));
			failed = 1;
		}
	}
	if (optind != argc || operations == 0) {
		usage();
		return 2;
	}
	return failed;
}
