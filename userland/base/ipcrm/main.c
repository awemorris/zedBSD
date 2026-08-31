/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ipcrm userland command.
 */

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
static int parse_number(const char *text, long *value);
static int lookup_key(enum object_kind kind, key_t key);
static int remove_object(enum object_kind kind, int id);

/*
 * Runs the ipcrm command.
 */
int
main(
	int argc,
	char **argv)
{
	int by_key;
	enum object_kind kind;
	long number;
	int id;
	int option, failed, operations;

	failed = 0;
	operations = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "q:m:s:Q:M:S:")) != -1) {

				by_key = option == 'Q' || option == 'M' || option == 'S';

		/* Dispatch the selected command-line option. */
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

			/* Reports operation failure. */
			return 2;
		}
		operations++;

		/* Handles a failed parse number operation. */
		if (parse_number(optarg, &number) != 0) {
			fprintf(stderr, "ipcrm: invalid identifier: %s\n",
				optarg);
			failed = 1;
			continue;
		}
		id = by_key ? lookup_key(kind, (key_t)number) : (int)number;

		/* Handles a failed remove object operation. */
		if (id < 0 || remove_object(kind, id) != 0) {
			fprintf(stderr, "ipcrm: %s: %s\n", optarg,
				strerror(errno));
			failed = 1;
		}
	}

	/* Validates the command-line arguments. */
	if (optind != argc || operations == 0) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: ipcrm [-q msgid] [-m shmid] [-s semid] "
			"[-Q msgkey] [-M shmkey] [-S semkey] ...\n");
}

/* Supports the parse number operation. */
static int
parse_number(
	const char *text,
	long *value)
{
	char *end;

	errno = 0;
	*value = strtol(text, &end, 0);
	/* Returns the computed result. */
	return errno == 0 && *text != '\0' && *end == '\0' && *value >= 0 ? 0
									  : -1;
}

/* Supports the lookup key operation. */
static int
lookup_key(
	enum object_kind kind,
	key_t key)
{
	int function_result;

	/* Dispatch the selected syntax or record type. */
	switch (kind) {
	case OBJECT_MESSAGE:
		/* Obtains the msgget result. */
		function_result = msgget(key, 0);

		/* Returns the computed result. */
		return function_result;
	case OBJECT_SHARED_MEMORY:
		/* Obtains the shmget result. */
		function_result = shmget(key, 0, 0);

		/* Returns the computed result. */
		return function_result;
	case OBJECT_SEMAPHORE:
		/* Obtains the semget result. */
		function_result = semget(key, 0, 0);

		/* Returns the computed result. */
		return function_result;
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the remove object operation. */
static int
remove_object(
	enum object_kind kind,
	int id)
{
	int function_result;

	/* Dispatch the selected syntax or record type. */
	switch (kind) {
	case OBJECT_MESSAGE:
		/* Obtains the msgctl result. */
		function_result = msgctl(id, IPC_RMID, NULL);

		/* Returns the computed result. */
		return function_result;
	case OBJECT_SHARED_MEMORY:
		/* Obtains the shmctl result. */
		function_result = shmctl(id, IPC_RMID, NULL);

		/* Returns the computed result. */
		return function_result;
	case OBJECT_SEMAPHORE:
		/* Obtains the semctl result. */
		function_result = semctl(id, 0, IPC_RMID);

		/* Returns the computed result. */
		return function_result;
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}
