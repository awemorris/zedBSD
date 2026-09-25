/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws034-p054: the libc fast paths for one thread must not lose a
 * cancellation, a fork child's thread identity or stdio locking.
 * Prints "CANCEL ok|FAIL <case>" and "CANCEL DONE n/m".
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed;
static int total;
static volatile int spinning;
static FILE *shared;

static void
check(int ok, const char *name)
{
	total++;
	if (ok)
		passed++;
	printf("CANCEL %s %s\n", ok ? "ok" : "FAIL", name);
	fflush(stdout);
}

static void *
spinner(void *argument)
{
	(void)argument;
	for (;;) {
		spinning = 1;
		pthread_testcancel();
	}
	return NULL;
}

static void *
writer(void *argument)
{
	int i;

	for (i = 0; i < 2000; i++)
		fprintf(shared, "%s %04d\n", (const char *)argument, i);
	return NULL;
}

int
main(void)
{
	pthread_t thread;
	pthread_t second;
	pthread_t parent_self;
	void *result;
	char line[64];
	int status;
	int lines;
	pid_t child;

	/* A fork child before any thread exists has its own identity. */
	parent_self = pthread_self();
	child = fork();
	if (child == 0)
		_exit(pthread_equal(pthread_self(), parent_self) ? 1 : 0);
	check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, "fork child pthread_self differs");

	/*
	 * A thread blocked in read() is not cancelled yet: the kernel wakes only
	 * cancelable waits (ws034-p056).  Not tested here.
	 */

	/* A thread polling pthread_testcancel() is cancelled. */
	check(pthread_create(&thread, NULL, spinner, NULL) == 0,
	    "create spinner");
	while (!spinning)
		usleep(1000);
	check(pthread_cancel(thread) == 0, "cancel spinner");
	check(pthread_join(thread, &result) == 0 &&
	    result == PTHREAD_CANCELED, "spinner canceled");

	/* Two threads writing one stream keep every line whole. */
	shared = tmpfile();
	check(shared != NULL, "tmpfile");
	pthread_create(&thread, NULL, writer, "aaaaaaaa");
	pthread_create(&second, NULL, writer, "bbbbbbbb");
	pthread_join(thread, NULL);
	pthread_join(second, NULL);
	rewind(shared);
	lines = 0;
	while (fgets(line, sizeof(line), shared) != NULL) {
		if (strlen(line) != 14 ||
		    (strncmp(line, "aaaaaaaa ", 9) != 0 &&
		     strncmp(line, "bbbbbbbb ", 9) != 0))
			break;
		lines++;
	}
	check(lines == 4000, "stdio lines whole under contention");

	printf("CANCEL DONE %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
