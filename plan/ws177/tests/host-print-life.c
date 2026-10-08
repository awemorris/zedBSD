/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p023: the host test of the printers' daemon's life in
 * libkeiland-backend (print.c) against a stand-in daemon (fake-printd.py):
 * a job sent again after the daemon ended before accepting it, twice at
 * most; the rest after three ends in ten seconds; FATAL; a descriptor sent
 * back (shut, then the job sent again); a cancel answered REJECTED or by
 * the daemon's end (cancelled, not sent again).
 *
 *     host-print-life FAKE-PRINTD-PY FOLDER DOCUMENT
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The checks that failed, the stand-in's script and the document printed. */
static int failures;
static const char *fake_script;
static const char *document;

int main(int argc, char **argv);
static void check(const char *name, int passed, const char *detail);
static struct kl_backend_print *scenario(const char *folder, const char *name, const char *modes, char *dir, size_t size);
static uint32_t submit(struct kl_backend_print *print);
static int wait_job(struct kl_backend_print *print, uint32_t job, unsigned state, const char *detail, int seconds);
static int starts(const char *dir);
static int logged(const char *dir, const char *text, int seconds);
static void pause_ms(unsigned ms);
static int took(struct kl_backend_print *print, uint32_t request);
static long now_ms(void);

/* Runs the scenarios. */
int
main(
	int argc,
	char **argv)
{
	struct kl_backend_print *print;
	char dir[512];
	char lock_path[600];
	char text[128];
	uint32_t request;
	uint32_t job;
	unsigned changed;
	long took_ms;
	long started;
	int answered;
	int tries;
	int held;
	int ok;

	/* The arguments. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-print-life FAKE-PRINTD-PY FOLDER DOCUMENT\n");
		return 2;
	}
	fake_script = argv[1];
	document = argv[3];

	/* 1. The daemon ends twice before accepting: the job is sent again and printed by the third. */
	print = scenario(argv[2], "resend", "crash\ncrash\nnormal\n", dir, sizeof(dir));
	job = submit(print);
	ok = wait_job(print, job, KL_BACKEND_PRINT_DONE, NULL, 15);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("resend-twice", ok && starts(dir) == 3, text);
	kl_backend_print_close(print);

	/* 2. It ends every time: failed after two sends again; three ends rest it, the next job fails at once. */
	print = scenario(argv[2], "limit", "crash\n", dir, sizeof(dir));
	job = submit(print);
	ok = wait_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon", 15);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("resend-limit", ok && starts(dir) == 3, text);
	job = submit(print);
	ok = wait_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon", 3);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("rest-after-three-ends", ok && starts(dir) == 3, text);
	kl_backend_print_close(print);

	/* 3. FATAL: the job fails and the daemon is not started again for now. */
	print = scenario(argv[2], "fatal", "fatal\nnormal\n", dir, sizeof(dir));
	job = submit(print);
	ok = wait_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon", 10);
	job = submit(print);
	ok = ok && wait_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon", 3);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("fatal-rest", ok && starts(dir) == 1, text);
	kl_backend_print_close(print);

	/* 4. A descriptor sent back: the daemon is shut, ends, and the job goes to a new one. */
	print = scenario(argv[2], "fd", "fd\nnormal\n", dir, sizeof(dir));
	job = submit(print);
	ok = wait_job(print, job, KL_BACKEND_PRINT_DONE, NULL, 15);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("fd-back-shut-and-resend", ok && starts(dir) == 2, text);
	kl_backend_print_close(print);

	/* 5. A cancel the daemon answers REJECTED: cancelled. */
	print = scenario(argv[2], "reject", "hold\n", dir, sizeof(dir));
	job = submit(print);
	ok = logged(dir, "JOB ", 10);
	(void)kl_backend_print_cancel(print, job, &request);
	ok = ok && wait_job(print, job, KL_BACKEND_PRINT_CANCELLED, NULL, 10);
	check("cancel-rejected", ok, "cancelled after REJECTED");
	kl_backend_print_close(print);

	/* 6. A cancel the daemon answers by ending: cancelled, not sent again. */
	print = scenario(argv[2], "end", "hold-end\nnormal\n", dir, sizeof(dir));
	job = submit(print);
	ok = logged(dir, "JOB ", 10);
	(void)kl_backend_print_cancel(print, job, &request);
	ok = ok && wait_job(print, job, KL_BACKEND_PRINT_CANCELLED, NULL, 10);
	pause_ms(500);
	(void)snprintf(text, sizeof(text), "starts %d", starts(dir));
	check("cancel-daemon-end", ok && starts(dir) == 1, text);
	kl_backend_print_close(print);

	/*
	 * 7. Another session holds the settings file's lock (ws177-p024): an add
	 * does not stop the caller's thread; it is made and answered once the
	 * lock goes.
	 */
	print = scenario(argv[2], "locked", "normal\n", dir, sizeof(dir));
	(void)snprintf(lock_path, sizeof(lock_path), "%s/printers.conf.lock", dir);
	held = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	(void)flock(held, LOCK_EX);
	started = now_ms();
	(void)kl_backend_print_add(print, KL_BACKEND_PRINTER_LPD, "127.0.0.2", 515U, "", &request);
	answered = 0;
	for (tries = 0; tries < 10; tries++) {
		(void)kl_backend_print_update(print, &changed);
		answered = answered || took(print, request);
	}
	took_ms = now_ms() - started;
	(void)close(held);
	for (tries = 0; tries < 100 && !answered; tries++) {
		(void)kl_backend_print_update(print, &changed);
		answered = took(print, request);
		pause_ms(20);
	}
	(void)snprintf(text, sizeof(text), "%ld ms while locked, answered %d", took_ms, answered);
	check("locked-file-does-not-block", took_ms < 200 && answered, text);
	kl_backend_print_close(print);

	/* The outcome. */
	printf("host-print-life: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

/* Prints a check's outcome. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS, or FAIL with why. */
	if (passed) {
		printf("PASS %s\n", name);
	} else {
		printf("FAIL %s: %s\n", name, detail);
		failures++;
	}
	fflush(stdout);
}

/*
 * Makes a scenario's folder (the stand-in's modes, its wrapper, a runtime
 * directory) and a backend with one LPD printer.
 */
static struct kl_backend_print *
scenario(
	const char *folder,
	const char *name,
	const char *modes,
	char *dir,
	size_t size)
{
	struct kl_backend_print *print;
	char path[1024];
	char config[1024];
	char runtime[1024];
	struct kl_backend_printer printers[KL_BACKEND_PRINTERS_MAX];
	uint32_t request;
	unsigned changed;
	size_t count;
	FILE *file;
	int tries;

	/* The folder and the modes. */
	(void)snprintf(dir, size, "%s/%s", folder, name);
	(void)mkdir(dir, 0700);
	(void)snprintf(path, sizeof(path), "%s/modes", dir);
	file = fopen(path, "w");
	if (file != NULL) {
		fputs(modes, file);
		fclose(file);
	}

	/* The wrapper the backend starts. */
	(void)snprintf(path, sizeof(path), "%s/fake-printd", dir);
	file = fopen(path, "w");
	if (file != NULL) {
		fprintf(file, "#!/bin/sh\nexec python3 %s %s\n", fake_script, dir);
		fclose(file);
	}
	(void)chmod(path, 0700);

	/* The runtime directory and the backend with its printer. */
	(void)snprintf(runtime, sizeof(runtime), "%s/runtime", dir);
	(void)mkdir(runtime, 0700);
	(void)snprintf(config, sizeof(config), "%s/printers.conf", dir);
	print = kl_backend_print_open(config, runtime, path);
	if (print == NULL) {
		fprintf(stderr, "no backend\n");
		exit(1);
	}
	(void)kl_backend_print_add(print, KL_BACKEND_PRINTER_LPD, "127.0.0.1", 515U, "", &request);

	/* The printer is in the table once the writer thread made the change (ws177-p024). */
	for (tries = 0; tries < 100; tries++) {
		(void)kl_backend_print_update(print, &changed);
		count = kl_backend_print_printers(print, printers, KL_BACKEND_PRINTERS_MAX);
		if (count == 1U)
			break;
		pause_ms(20);
	}
	return print;
}

/* Prints the document on the default printer; the job's number. */
static uint32_t
submit(
	struct kl_backend_print *print)
{
	uint32_t request;
	uint32_t job;
	int fd;

	/* The document's descriptor, the backend's from here. */
	fd = open(document, O_RDONLY | O_CLOEXEC);
	job = 0;
	(void)kl_backend_print_submit(print, 0U, "Life", fd, &request, &job);
	return job;
}

/* Waits for a job to reach a state (and a detail, when one is given). */
static int
wait_job(
	struct kl_backend_print *print,
	uint32_t job,
	unsigned state,
	const char *detail,
	int seconds)
{
	struct kl_backend_print_job jobs[KL_BACKEND_PRINT_JOBS_MAX];
	unsigned changed;
	size_t count;
	size_t index;
	int tries;

	/* A look every 50 ms. */
	for (tries = 0; tries < seconds * 20; tries++) {
		(void)kl_backend_print_update(print, &changed);
		count = kl_backend_print_jobs(print, jobs, KL_BACKEND_PRINT_JOBS_MAX);
		for (index = 0; index < count; index++) {
			if (jobs[index].job != job || jobs[index].state != state)
				continue;
			if (detail == NULL || strcmp(jobs[index].detail, detail) == 0)
				return 1;
		}
		pause_ms(50);
	}

	/* Not in time. */
	return 0;
}

/* Counts the stand-in's starts. */
static int
starts(
	const char *dir)
{
	char path[1024];
	FILE *file;
	int count;
	int c;

	/* One x a start. */
	(void)snprintf(path, sizeof(path), "%s/starts", dir);
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	count = 0;
	for (;;) {
		c = fgetc(file);
		if (c == EOF)
			break;
		if (c == 'x')
			count++;
	}
	fclose(file);
	return count;
}

/* Waits for a text in the stand-in's log. */
static int
logged(
	const char *dir,
	const char *text,
	int seconds)
{
	char path[1024];
	char line[512];
	FILE *file;
	int tries;

	/* A look every 50 ms. */
	(void)snprintf(path, sizeof(path), "%s/log", dir);
	for (tries = 0; tries < seconds * 20; tries++) {
		file = fopen(path, "r");
		if (file != NULL) {
			while (fgets(line, sizeof(line), file) != NULL) {
				if (strstr(line, text) != NULL) {
					fclose(file);
					return 1;
				}
			}
			fclose(file);
		}
		pause_ms(50);
	}

	/* Not in time. */
	return 0;
}

/* Sleeps a number of milliseconds. */
static void
pause_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The time. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Tells whether a request's answer came (and takes it). */
static int
took(
	struct kl_backend_print *print,
	uint32_t request)
{
	uint32_t answered;
	unsigned saved;
	int error;

	/* Each answer waiting. */
	while (kl_backend_print_take_result(print, &answered, &error, &saved)) {
		if (answered == request)
			return 1;
	}

	/* Not yet. */
	return 0;
}

/* The monotonic clock in milliseconds. */
static long
now_ms(void)
{
	struct timespec now;

	/* The clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}
