/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws145-p003: the host test of libkeiland-backend's printers
 * (userland/desktop/libkeiland-backend/print/print.c) with the host's
 * keiland-printd against mock-printers.py: printers added (an IPP one's
 * name asked of the daemon), a PDF printed to each, the jobs followed to
 * their end, the default moved, a printer removed, the settings file read
 * again by a second backend, and a job cancelled before it went.
 *
 *     host-print-backend PRINTD FOLDER IPP-PORT LPD-PORT DOCUMENT
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The checks that failed. */
static int failures;

int main(int argc, char **argv);
static void check(const char *name, int passed, const char *detail);
static int wait_job(struct kl_backend_print *print, uint32_t job, unsigned state, int seconds);
static int wait_name(struct kl_backend_print *print, uint32_t id, const char *name, int seconds);
static int take(struct kl_backend_print *print, uint32_t request, int *error);
static void pause_ms(unsigned ms);

/* Drives the printers and checks them. */
int
main(
	int argc,
	char **argv)
{
	struct kl_backend_print *print;
	struct kl_backend_print *second;
	struct kl_backend_printer printers[KL_BACKEND_PRINTERS_MAX];
	char config[512];
	char runtime[512];
	char detail[256];
	uint32_t request;
	uint32_t job;
	unsigned ipp_port;
	unsigned lpd_port;
	size_t count;
	int error;
	int fd;

	/* The arguments. */
	if (argc != 6) {
		fprintf(stderr, "usage: host-print-backend PRINTD FOLDER IPP-PORT LPD-PORT DOCUMENT\n");
		return 2;
	}
	ipp_port = (unsigned)atoi(argv[3]);
	lpd_port = (unsigned)atoi(argv[4]);
	(void)snprintf(config, sizeof(config), "%s/config/printers.conf", argv[2]);
	(void)snprintf(runtime, sizeof(runtime), "%s/runtime", argv[2]);

	/* The printers, none yet. */
	print = kl_backend_print_open(config, runtime, argv[1]);
	check("open", print != NULL && kl_backend_print_can(print), "the daemon can be run");
	if (print == NULL)
		return 1;

	/* An IPP printer: the first is the default; its name comes from the daemon. */
	(void)kl_backend_print_add(print, KL_BACKEND_PRINTER_IPP, "127.0.0.1", ipp_port, "", &request);
	check("add-ipp", take(print, request, &error) && error == 0, "result 0");
	count = kl_backend_print_printers(print, printers, KL_BACKEND_PRINTERS_MAX);
	check("ipp-default", count == 1U && printers[0].id == 1U && printers[0].is_default && strcmp(printers[0].path, "/ipp/print") == 0,
	    "printer 1 the default at /ipp/print");
	check("ipp-named", wait_name(print, 1U, "Mock Printer", 10), "NAMED gave Mock Printer");

	/* An LPD printer, its queue given. */
	(void)kl_backend_print_add(print, KL_BACKEND_PRINTER_LPD, "127.0.0.1", lpd_port, "raw", &request);
	check("add-lpd", take(print, request, &error) && error == 0, "result 0");
	(void)kl_backend_print_add(print, KL_BACKEND_PRINTER_LPD, "127.0.0.1", lpd_port, "", &request);
	check("add-twice", take(print, request, &error) && error == EINVAL, "the same printer refused");

	/* A PDF to the default (IPP), followed to its end. */
	fd = open(argv[5], O_RDONLY | O_CLOEXEC);
	(void)kl_backend_print_submit(print, 0U, "Backend one", fd, &request, &job);
	check("submit-ipp", take(print, request, &error) && error == 0 && job == 1U, "job 1 queued");
	check("ipp-done", wait_job(print, job, KL_BACKEND_PRINT_DONE, 30), "job 1 done");

	/* A PDF to the LPD printer. */
	fd = open(argv[5], O_RDONLY | O_CLOEXEC);
	(void)kl_backend_print_submit(print, 2U, "Backend two", fd, &request, &job);
	check("submit-lpd", take(print, request, &error) && error == 0 && job == 2U, "job 2 queued");
	check("lpd-done", wait_job(print, job, KL_BACKEND_PRINT_DONE, 30), "job 2 done");

	/* A printer not known, refused (the document closed). */
	fd = open(argv[5], O_RDONLY | O_CLOEXEC);
	(void)kl_backend_print_submit(print, 9U, "Nowhere", fd, &request, &job);
	check("submit-unknown", take(print, request, &error) && error == EINVAL && job == 0U, "printer 9 refused");

	/* The default moved, then the first printer removed: the settings file read by a second backend agrees. */
	(void)kl_backend_print_set_default(print, 2U, &request);
	check("set-default", take(print, request, &error) && error == 0, "result 0");
	(void)kl_backend_print_remove(print, 1U, &request);
	check("remove", take(print, request, &error) && error == 0, "result 0");
	second = kl_backend_print_open(config, runtime, argv[1]);
	count = kl_backend_print_printers(second, printers, KL_BACKEND_PRINTERS_MAX);
	(void)snprintf(detail, sizeof(detail), "count %lu id %u default %u path %s", (unsigned long)count, count > 0U ? printers[0].id : 0U,
	    count > 0U ? printers[0].is_default : 0U, count > 0U ? printers[0].path : "");
	check("reread", count == 1U && printers[0].id == 2U && printers[0].is_default && strcmp(printers[0].path, "raw") == 0, detail);

	/* The next number is not used again. */
	(void)kl_backend_print_add(second, KL_BACKEND_PRINTER_IPP, "127.0.0.1", ipp_port, "", &request);
	(void)take(second, request, &error);
	count = kl_backend_print_printers(second, printers, KL_BACKEND_PRINTERS_MAX);
	check("next-id", count == 2U && printers[1].id == 3U, "the new printer is 3");
	kl_backend_print_close(second);

	/* The end: the daemon is told to stop. */
	kl_backend_print_close(print);
	if (failures != 0) {
		printf("host-print-backend: FAIL %d\n", failures);
		return 1;
	}
	printf("host-print-backend: PASS\n");
	return 0;
}

/* Reports one check. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS or FAIL with what was expected. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}
	printf("FAIL %s: %s\n", name, detail);
	failures++;
}

/* Updates until a job reaches a state; 1 when it did. */
static int
wait_job(
	struct kl_backend_print *print,
	uint32_t job,
	unsigned state,
	int seconds)
{
	struct kl_backend_print_job jobs[KL_BACKEND_PRINT_JOBS_MAX];
	unsigned changed;
	size_t count;
	size_t index;
	int rounds;

	/* Each tenth of a second. */
	for (rounds = 0; rounds < seconds * 10; rounds++) {
		(void)kl_backend_print_update(print, &changed);
		count = kl_backend_print_jobs(print, jobs, KL_BACKEND_PRINT_JOBS_MAX);
		for (index = 0; index < count; index++) {
			if (jobs[index].job != job)
				continue;
			if (jobs[index].state == state)
				return 1;
			if (jobs[index].state >= KL_BACKEND_PRINT_DONE) {
				printf("  job %u ended in %u (%s)\n", job, jobs[index].state, jobs[index].detail);
				return 0;
			}
		}
		pause_ms(100);
	}
	return 0;
}

/* Updates until a printer has a name; 1 when it did. */
static int
wait_name(
	struct kl_backend_print *print,
	uint32_t id,
	const char *name,
	int seconds)
{
	struct kl_backend_printer printers[KL_BACKEND_PRINTERS_MAX];
	unsigned changed;
	size_t count;
	size_t index;
	int rounds;

	/* Each tenth of a second. */
	for (rounds = 0; rounds < seconds * 10; rounds++) {
		(void)kl_backend_print_update(print, &changed);
		count = kl_backend_print_printers(print, printers, KL_BACKEND_PRINTERS_MAX);
		for (index = 0; index < count; index++) {
			if (printers[index].id == id && strcmp(printers[index].name, name) == 0)
				return 1;
		}
		pause_ms(100);
	}
	return 0;
}

/* Takes the answer of a request (those before it are passed over); 1 with it. */
static int
take(
	struct kl_backend_print *print,
	uint32_t request,
	int *error)
{
	uint32_t answered;
	unsigned changed;
	unsigned saved;
	int tries;

	/* Each answer waiting; the settings' changes are answered by the writer thread (ws177-p024), so a few updates. */
	for (tries = 0; tries < 100; tries++) {
		(void)kl_backend_print_update(print, &changed);
		while (kl_backend_print_take_result(print, &answered, error, &saved)) {
			if (answered == request)
				return 1;
		}
		pause_ms(20);
	}
	return 0;
}

/* Sleeps a number of milliseconds. */
static void
pause_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The wait. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
