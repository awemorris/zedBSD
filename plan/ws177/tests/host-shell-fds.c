/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p019: the host test of the browser shell's network descriptors
 * (userland/desktop/browser/shell/window.c with fakes of libkeiland's
 * application): with more descriptors than the application watches (it
 * refuses past KL_APP_FDS_MAX, as libkeiland does), the ones it does not
 * watch are polled by the shell itself (a readable one gets POLLIN, a
 * closed one POLLHUP), the wait is cut short while there are any, and
 * the log says how many when that changes.
 *
 * Prints "PASS name" or "FAIL name [detail]"; exits with 1 when one failed.
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* How many descriptors the test gives (more than the application watches). */
#define TEST_FDS		80U

/* The checks that failed. */
static int test_failures;

/* What the fakes saw: the descriptors watched, and the last timeout asked for. */
static int test_watched[KL_APP_FDS_MAX];
static unsigned test_watched_count;
static int test_timeout;

int main(void);
static void test_check(const char *name, int passed, const char *detail);

/* libkeiland's fakes. */
int
kl_app_watch_fd(
	struct kl_app *app,
	int fd,
	unsigned events)
{
	unsigned index;

	(void)app;
	for (index = 0; index < test_watched_count; index++) {
		if (test_watched[index] == fd) {
			if (events == 0U) {
				test_watched[index] = test_watched[test_watched_count - 1U];
				test_watched_count--;
			}
			return 0;
		}
	}
	if (events == 0U)
		return 0;
	if (test_watched_count == KL_APP_FDS_MAX)
		return ENOSPC;
	test_watched[test_watched_count] = fd;
	test_watched_count++;
	return 0;
}

int
kl_app_dispatch(
	struct kl_app *app,
	int timeout_ms)
{
	(void)app;
	test_timeout = timeout_ms;
	return 0;
}

int
kl_app_take(
	struct kl_app *app,
	struct kl_app_event *event)
{
	(void)app;
	(void)event;
	return 0;
}

struct kl_app *kl_app_open(const struct kl_app_options *options) { (void)options; return NULL; }
void kl_app_close(struct kl_app *app) { (void)app; }
struct kl_window *kl_app_window_create(struct kl_app *app, const struct kl_window_options *options) { (void)app; (void)options; return NULL; }
void kl_window_close(struct kl_window *window) { (void)window; }
void kl_window_set_title(struct kl_window *window, const char *title) { (void)window; (void)title; }
void kl_window_size(const struct kl_window *window, uint32_t *width, uint32_t *height) { (void)window; *width = 0; *height = 0; }
void shell_titlebar_post(struct shell_titlebar *titlebar, const struct kl_window_event *event) { (void)titlebar; (void)event; }

/*
 * Gives the shell more descriptors than the application watches and
 * checks the answers.
 */
int
main(void)
{
	static struct shell_window window;
	struct pollfd fds[TEST_FDS];
	int pipes[TEST_FDS][2];
	unsigned index;
	int status;

	/* Pipes: the descriptors are their reading ends, waiting to read. */
	for (index = 0; index < TEST_FDS; index++) {
		if (pipe(pipes[index]) != 0)
			return 2;
		fds[index].fd = pipes[index][0];
		fds[index].events = POLLIN;
	}

	/* The last one readable, the one before it closed at its writing end. */
	if (write(pipes[TEST_FDS - 1U][1], "x", 1) != 1)
		return 2;
	(void)close(pipes[TEST_FDS - 2U][1]);

	/* A first round: the shell learns which it could not watch. */
	memset(&window, 0, sizeof(window));
	status = shell_window_dispatch(&window, -1, fds, TEST_FDS);
	test_check("dispatch", status == 0, "");
	test_check("watched-limit", test_watched_count == KL_APP_FDS_MAX, "");
	test_check("unwatched-readable", (fds[TEST_FDS - 1U].revents & POLLIN) != 0, "");
	test_check("unwatched-hangup", (fds[TEST_FDS - 2U].revents & POLLHUP) != 0, "");
	test_check("unwatched-quiet", fds[TEST_FDS - 3U].revents == 0, "");
	test_check("counted", window.unwatched_count == TEST_FDS - KL_APP_FDS_MAX, "");

	/* The next round waits briefly, as the unwatched ones need polling. */
	status = shell_window_dispatch(&window, -1, fds, TEST_FDS);
	test_check("short-wait", status == 0 && test_timeout >= 0 && test_timeout <= 10, "");

	/* With few descriptors again, the wait is the caller's. */
	status = shell_window_dispatch(&window, -1, fds, 4U);
	status = shell_window_dispatch(&window, -1, fds, 4U);
	test_check("all-watched", status == 0 && window.unwatched_count == 0U && test_timeout == -1, "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-shell-fds: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-shell-fds: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%s]\n", name, detail);
	test_failures++;
}
