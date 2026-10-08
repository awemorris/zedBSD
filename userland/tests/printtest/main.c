/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * printtest (ws145-p003): a client of libkeiland's printers for the tests.
 *
 *   printtest [--timeout-s=N] list
 *   printtest [--timeout-s=N] add ipp|lpd HOST PORT [PATH]
 *   printtest [--timeout-s=N] default PRINTER | remove PRINTER | cancel JOB
 *   printtest [--timeout-s=N] edit PRINTER NAME [PATH]    (ws177-p025; "" keeps each)
 *   printtest [--timeout-s=N] print [--printer=N] FILE [TITLE]
 *
 * Each answer is a "PRINTTEST result" line; list prints the printers and
 * the jobs; print waits for its job to end (done, failed or cancelled)
 * and prints its states as they change.  Exits with 0 when the request was
 * taken (and a print's job was done), 1 otherwise.
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv);
static int test_command(struct wl_display *display, struct kl_system *system, int count, char **words, int seconds);
static void test_list(const struct kl_system *system);
static int test_wait(struct wl_display *display, struct kl_system *system, uint32_t request, int *error, int seconds);
static int test_follow(struct wl_display *display, struct kl_system *system, uint32_t request, int seconds);
static int test_round(struct wl_display *display, struct kl_system *system, int timeout_ms, unsigned *changed);
static long test_now_ms(void);

/* Carries out the command. */
int
main(
	int argc,
	char **argv)
{
	struct wl_display *display;
	struct kl_system *system;
	unsigned capabilities;
	int seconds;
	int status;
	int same;
	int arg;

	/* Each line as it is made, and the time out. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	seconds = 30;
	arg = 1;
	if (arg < argc) {
		same = strncmp(argv[arg], "--timeout-s=", 12U);
		if (same == 0) {
			seconds = atoi(argv[arg] + 12);
			arg++;
		}
	}

	/* A command. */
	if (arg >= argc) {
		fprintf(stderr, "usage: printtest [--timeout-s=N] list|add|default|remove|cancel|edit|print ...\n");
		return 2;
	}

	/* The compositor. */
	display = wl_display_connect(NULL);
	if (display == NULL) {
		printf("PRINTTEST failed step=connect errno=%d\n", errno);
		return 1;
	}

	/* Its printers. */
	system = kl_system_open(display);
	if (system == NULL) {
		printf("PRINTTEST failed step=open errno=%d\n", errno);
		return 1;
	}

	/* Whether it offers printers. */
	capabilities = kl_system_capabilities(system);
	printf("PRINTTEST open printers=%d\n", (capabilities & KL_SYSTEM_HAS_PRINTERS) != 0U);

	/* The command, then the end. */
	status = test_command(display, system, argc - arg, argv + arg, seconds);
	kl_system_close(system);
	wl_display_disconnect(display);
	return status;
}

/*
 * Carries out a command (its words); returns 0 when it was taken (and a
 * print's job was done), 1 otherwise.
 */
static int
test_command(
	struct wl_display *display,
	struct kl_system *system,
	int count,
	char **words,
	int seconds)
{
	const char *title;
	const char *path;
	uint32_t request;
	uint32_t printer;
	unsigned protocol;
	int error;
	int same;
	int ok;

	/* list: the printers and the jobs. */
	same = strcmp(words[0], "list");
	if (same == 0) {
		test_list(system);
		return 0;
	}

	/* print [--printer=N] FILE [TITLE]: the job followed to its end. */
	same = strcmp(words[0], "print");
	if (same == 0) {
		words++;
		count--;
		printer = 0;
		if (count > 0) {
			same = strncmp(words[0], "--printer=", 10U);
			if (same == 0) {
				printer = (uint32_t)atoi(words[0] + 10);
				words++;
				count--;
			}
		}

		/* A file to print. */
		if (count < 1)
			return 1;
		title = "printtest";
		if (count > 1)
			title = words[1];
		error = kl_system_printers_print(system, printer, words[0], title, &request);
		printf("PRINTTEST print asked error=%d\n", error);
		if (error != 0)
			return 1;
		ok = test_follow(display, system, request, seconds);
		if (!ok)
			return 1;
		return 0;
	}

	/* The other commands: asked, then answered. */
	error = EINVAL;
	same = strcmp(words[0], "add");
	if (same == 0 && count >= 4) {
		protocol = KL_PRINTER_IPP;
		same = strcmp(words[1], "lpd");
		if (same == 0)
			protocol = KL_PRINTER_LPD;
		path = "";
		if (count >= 5)
			path = words[4];
		error = kl_system_printers_add(system, protocol, words[2], (unsigned)atoi(words[3]), path, &request);
	}

	/* default, remove, cancel: a number. */
	same = strcmp(words[0], "default");
	if (same == 0 && count >= 2)
		error = kl_system_printers_set_default(system, (uint32_t)atoi(words[1]), &request);
	same = strcmp(words[0], "remove");
	if (same == 0 && count >= 2)
		error = kl_system_printers_remove(system, (uint32_t)atoi(words[1]), &request);
	same = strcmp(words[0], "cancel");
	if (same == 0 && count >= 2)
		error = kl_system_print_cancel(system, (uint32_t)atoi(words[1]), &request);

	/* edit: a number, a name and a path (each "" to keep it). */
	same = strcmp(words[0], "edit");
	if (same == 0 && count >= 3) {
		path = "";
		if (count >= 4)
			path = words[3];
		error = kl_system_printers_edit(system, (uint32_t)atoi(words[1]), words[2], path, &request);
	}

	/* The answer, and the lists after it. */
	if (error == 0) {
		ok = test_wait(display, system, request, &error, seconds);
		if (!ok)
			error = ETIMEDOUT;
	}

	/* The answer. */
	printf("PRINTTEST result error=%d\n", error);
	test_list(system);
	if (error != 0)
		return 1;
	return 0;
}

/* Prints the printers and the jobs. */
static void
test_list(
	const struct kl_system *system)
{
	struct kl_printer printers[KL_PRINTERS_MAX];
	struct kl_print_job jobs[KL_PRINT_JOBS_MAX];
	size_t count;
	size_t index;

	/* Each printer. */
	count = kl_system_printers_get(system, printers, KL_PRINTERS_MAX);
	for (index = 0; index < count; index++) {
		printf("PRINTTEST printer id=%u protocol=%u host=%s port=%u path=%s default=%u name=%s\n", printers[index].id, printers[index].protocol,
		    printers[index].host, printers[index].port, printers[index].path, printers[index].flags & KL_PRINTER_DEFAULT, printers[index].name);
	}

	/* Each job. */
	count = kl_system_print_jobs_get(system, jobs, KL_PRINT_JOBS_MAX);
	for (index = 0; index < count; index++)
		printf("PRINTTEST job=%u printer=%u state=%u detail=%s\n", jobs[index].job, jobs[index].printer, jobs[index].state, jobs[index].detail);
}

/* Waits for a request's answer; 1 with it. */
static int
test_wait(
	struct wl_display *display,
	struct kl_system *system,
	uint32_t request,
	int *error,
	int seconds)
{
	uint32_t answered;
	unsigned changed;
	long until;
	long now;
	int status;
	int taken;

	/* Until the answer or the time. */
	until = test_now_ms() + (long)seconds * 1000L;
	for (;;) {
		now = test_now_ms();
		if (now >= until)
			return 0;
		status = test_round(display, system, 200, &changed);
		if (status != 0)
			return 0;

		/* The answers that came. */
		for (;;) {
			taken = kl_system_take_result(system, &answered, error);
			if (!taken)
				break;
			if (answered == request)
				return 1;
		}
	}
}

/* Follows a print: its result, its job, the job's states to its end; 1 when it was done. */
static int
test_follow(
	struct wl_display *display,
	struct kl_system *system,
	uint32_t request,
	int seconds)
{
	struct kl_print_job jobs[KL_PRINT_JOBS_MAX];
	unsigned last;
	unsigned changed;
	uint32_t job;
	size_t count;
	size_t index;
	long until;
	long now;
	int status;
	int found;
	int error;
	int ok;

	/* The result. */
	ok = test_wait(display, system, request, &error, seconds);
	if (!ok)
		error = ETIMEDOUT;
	printf("PRINTTEST result error=%d\n", error);
	if (error != 0)
		return 0;

	/* The job. */
	found = kl_system_print_job_of(system, request, &job);
	if (!found) {
		printf("PRINTTEST failed step=job\n");
		return 0;
	}

	/* %u=Its states from here. */
	printf("PRINTTEST queued job=%u\n", job);

	/* Its states until it ends. */
	last = 0;
	until = test_now_ms() + (long)seconds * 1000L;
	for (;;) {
		now = test_now_ms();
		if (now >= until)
			break;
		status = test_round(display, system, 200, &changed);
		if (status != 0)
			return 0;
		count = kl_system_print_jobs_get(system, jobs, KL_PRINT_JOBS_MAX);
		for (index = 0; index < count; index++) {
			if (jobs[index].job != job || jobs[index].state == last)
				continue;
			last = jobs[index].state;
			printf("PRINTTEST job=%u state=%u detail=%s\n", job, last, jobs[index].detail);
			if (last >= KL_PRINT_DONE) {
				printf("PRINTTEST done job=%u state=%u\n", job, last);
				return last == KL_PRINT_DONE;
			}
		}
	}

	/* timeout=Not ended in time. */
	printf("PRINTTEST failed step=timeout\n");
	return 0;
}

/* One round of the display: its events read and the system dispatched; nonzero when the display went. */
static int
test_round(
	struct wl_display *display,
	struct kl_system *system,
	int timeout_ms,
	unsigned *changed)
{
	struct pollfd descriptor;
	int status;

	/* The events read before, until the display may be read. */
	for (;;) {
		status = wl_display_prepare_read(display);
		if (status == 0)
			break;
		(void)wl_display_dispatch_pending(display);
	}

	/* What is queued sent, and the display's events waited for. */
	(void)wl_display_flush(display);
	descriptor.fd = wl_display_get_fd(display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1, timeout_ms);
	if (status > 0)
		status = wl_display_read_events(display);
	else
		wl_display_cancel_read(display);
	if (status < 0)
		return -1;

	/* The application's queue, then the system's. */
	(void)wl_display_dispatch_pending(display);
	status = kl_system_dispatch(system, changed);
	return status;
}

/* The monotonic clock in milliseconds. */
static long
test_now_ms(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}
