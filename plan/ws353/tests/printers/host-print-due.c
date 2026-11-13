/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the printers' only deadline
 * (userland/desktop/libkeiland-backend/print/print.c, kl_backend_print_due):
 * nothing waits for a time without a daemon, nor while a daemon keeps the
 * protocol; a daemon that breaks it (a line too long) is shut and its end
 * is due ten seconds later; an update before that keeps its socket, the
 * first update after it closes the socket; and once the daemons are gone
 * nothing waits for a time again.  The daemon is a shell script: the first
 * one sends a line too long and stays, the ones after end at once.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How long the backend gives a daemon shut for writing (ms), and the slack the test allows around it. */
#define TEST_SHUT_MS		10000U
#define TEST_SLACK_MS		1500U

/* The failures counted; the verdict is FAIL when any. */
static unsigned failures;

static void check(int condition, const char *what);
static uint64_t now_ms(void);
static int write_file(const char *path, const char *text, mode_t mode);
static void update_for(struct kl_backend_print *print, unsigned milliseconds);

int
main(void)
{
	struct kl_backend_print *print;
	const char *base;
	char folder[256];
	char config[320];
	char program[320];
	char document[320];
	char script[1024];
	uint32_t request;
	uint32_t job;
	uint64_t due;
	uint64_t asked;
	uint64_t left;
	unsigned changed;
	short events;
	int descriptor;
	int error;
	int fd;
	char *made;

	/* A folder of its own. */
	base = getenv("TEST_DIR");
	if (base == NULL)
		base = "/tmp";
	(void)snprintf(folder, sizeof(folder), "%s/print-due-XXXXXX", base);
	made = mkdtemp(folder);
	if (made == NULL) {
		perror("mkdtemp");
		return 1;
	}

	/* One LPD printer, the default, in the settings file. */
	(void)snprintf(config, sizeof(config), "%s/printers.conf", folder);
	error = write_file(config, "# Keiland printers\nnext-id 2\nprinter 1 lpd 127.0.0.1 515 lp Test\ndefault 1\n", 0644);
	check(error == 0, "the settings file made");

	/*
	 * The daemon: the first sends 2000 bytes without a line feed (the
	 * protocol broken) and stays 20 s; the ones after end at once.
	 */
	(void)snprintf(program, sizeof(program), "%s/fake-printd", folder);
	(void)snprintf(script, sizeof(script),
	    "#!/bin/sh\n"
	    "if [ -e '%s/started' ]; then exit 0; fi\n"
	    ": > '%s/started'\n"
	    "head -c 2000 /dev/zero | tr '\\000' x >&3\n"
	    "sleep 20\n",
	    folder, folder);
	error = write_file(program, script, 0755);
	check(error == 0, "the daemon's script made");

	/* A document (the daemon never reads it). */
	(void)snprintf(document, sizeof(document), "%s/document.pdf", folder);
	error = write_file(document, "%PDF-1.4\n", 0644);
	check(error == 0, "the document made");

	/* No daemon yet: nothing waits for a time. */
	print = kl_backend_print_open(config, folder, program);
	check(print != NULL, "open");
	if (print == NULL)
		return 1;
	kl_backend_print_host(print);
	due = kl_backend_print_due(print);
	check(due == UINT64_MAX, "no daemon: nothing is due");
	check(kl_backend_print_due(NULL) == UINT64_MAX, "no backend: nothing is due");

	/* A job starts the daemon; while it keeps the protocol nothing is due. */
	fd = open(document, O_RDONLY | O_CLOEXEC);
	check(fd >= 0, "the document opened");
	error = kl_backend_print_submit(print, 0U, "test", fd, &request, &job);
	check(error == 0 && job != 0U, "the job asked");
	descriptor = kl_backend_print_descriptor(print, &events);
	check(descriptor >= 0, "the daemon's connection is open");
	due = kl_backend_print_due(print);
	check(due == UINT64_MAX, "a daemon keeping the protocol: nothing is due");

	/* The line too long comes: the daemon is shut, its end due ten seconds on. */
	asked = now_ms();
	update_for(print, 3000U);
	due = kl_backend_print_due(print);
	printf("info due_in_ms=%lld\n", (long long)(due - asked));
	check(due != UINT64_MAX, "a daemon shut for writing: its end is due");
	check(due + TEST_SLACK_MS >= asked + TEST_SHUT_MS && due <= asked + TEST_SHUT_MS + TEST_SLACK_MS, "the end is due about ten seconds after the shut");

	/* Before the time the socket stays. */
	(void)kl_backend_print_update(print, &changed);
	descriptor = kl_backend_print_descriptor(print, &events);
	check(descriptor >= 0, "before its time the shut daemon's socket stays");

	/* At the time, the update closes it. */
	asked = now_ms();
	left = 0;
	if (due > asked)
		left = due - asked;
	usleep((useconds_t)(left * 1000U));
	update_for(print, 100U);
	asked = now_ms();
	printf("info updated_after_due_ms=%lld\n", (long long)(asked - due));
	check(asked >= due, "the time came");

	/* The daemons after it end at once; then nothing waits for a time. */
	update_for(print, 3000U);
	descriptor = kl_backend_print_descriptor(print, &events);
	due = kl_backend_print_due(print);
	check(descriptor < 0, "no daemon's connection is left");
	check(due == UINT64_MAX, "with the daemons gone nothing is due");
	kl_backend_print_close(print);

	/* The verdict. */
	if (failures != 0U) {
		printf("host-print-due: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-print-due: PASS\n");
	return 0;
}

/* Prints a check and counts it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* The line, and the count. */
	if (condition) {
		printf("ok   %s\n", what);
	} else {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* The monotonic clock in milliseconds, as the compositor reads it. */
static uint64_t
now_ms(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Writes a file with its mode.  Returns 0, or an errno value. */
static int
write_file(
	const char *path,
	const char *text,
	mode_t mode)
{
	size_t length;
	ssize_t written;
	int fd;

	/* The file. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return errno;

	/* Its text. */
	length = strlen(text);
	written = write(fd, text, length);
	(void)close(fd);
	if (written < 0 || (size_t)written != length)
		return EIO;

	/* Succeeded: written. */
	return 0;
}

/*
 * Runs the backend's update for a time as a host would: waits on the
 * daemon's connection (at most until its due time) and updates after each
 * wake.
 */
static void
update_for(
	struct kl_backend_print *print,
	unsigned milliseconds)
{
	struct pollfd waiter;
	unsigned changed;
	uint64_t until;
	uint64_t due;
	uint64_t now;
	int timeout;

	/* Each wake until the time is over. */
	until = now_ms() + milliseconds;
	for (;;) {
		now = now_ms();
		if (now >= until)
			break;

		/* The wait: the connection, and the due time when it is earlier. */
		timeout = (int)(until - now);
		due = kl_backend_print_due(print);
		if (due != UINT64_MAX && due < until) {
			timeout = 0;
			if (due > now)
				timeout = (int)(due - now);
		}

		/* The connection's events, or none without a daemon. */
		waiter.fd = kl_backend_print_descriptor(print, &waiter.events);
		waiter.revents = 0;
		(void)poll(&waiter, 1, timeout);

		/* What came, or the time. */
		(void)kl_backend_print_update(print, &changed);
	}
}
