/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the printers' settings file in a new home
 * (userland/desktop/libkeiland-backend/print/print.c): a home without
 * ~/.config gets ~/.config and ~/.config/keiland made, closed to others,
 * at the first change, and the change is saved (its lock file beside it);
 * a second backend on the same file reads the printer.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* The failures counted; the verdict is FAIL when any. */
static unsigned failures;

static void check(int condition, const char *what);
static int folder_closed(const char *path);

int
main(void)
{
	struct kl_backend_printer list[KL_BACKEND_PRINTERS_MAX];
	struct kl_backend_print *print;
	struct kl_backend_print *again;
	const char *base;
	char folder[256];
	char home[320];
	char dot_config[400];
	char keiland[480];
	char config[560];
	char lock[600];
	uint32_t request;
	uint32_t answered;
	unsigned changed;
	unsigned saved;
	size_t count;
	int error;
	int taken;
	int closed;
	char *made;

	/* A folder of its own, and a home in it without ~/.config. */
	base = getenv("TEST_DIR");
	if (base == NULL)
		base = "/tmp";
	(void)snprintf(folder, sizeof(folder), "%s/print-folders-XXXXXX", base);
	made = mkdtemp(folder);
	if (made == NULL) {
		perror("mkdtemp");
		return 1;
	}

	/* The home, and the paths under it. */
	(void)snprintf(home, sizeof(home), "%s/home", folder);
	error = mkdir(home, 0700);
	check(error == 0, "the home made, without .config");
	(void)snprintf(dot_config, sizeof(dot_config), "%s/.config", home);
	(void)snprintf(keiland, sizeof(keiland), "%s/keiland", dot_config);
	(void)snprintf(config, sizeof(config), "%s/printers.conf", keiland);
	(void)snprintf(lock, sizeof(lock), "%s.lock", config);

	/* A printer added and written as the compositor does (hosted). */
	print = kl_backend_print_open(config, folder, "/nonexistent/keiland-printd");
	check(print != NULL, "open");
	if (print == NULL)
		return 1;
	kl_backend_print_host(print);
	error = kl_backend_print_add(print, KL_BACKEND_PRINTER_LPD, "10.0.0.5", 515U, "lp", &request);
	check(error == 0, "add asked");
	kl_backend_print_write(print);
	(void)kl_backend_print_update(print, &changed);
	taken = kl_backend_print_take_result(print, &answered, &error, &saved);
	check(taken && answered == request && error == 0 && saved == 1U, "the result says applied and saved");

	/* The folders made, closed to others, and the files there. */
	closed = folder_closed(dot_config);
	check(closed, "~/.config made, closed to others");
	closed = folder_closed(keiland);
	check(closed, "~/.config/keiland made, closed to others");
	check(access(config, R_OK) == 0, "the settings file written");
	check(access(lock, F_OK) == 0, "its lock file beside it");
	kl_backend_print_close(print);

	/* Another backend on the file reads the printer. */
	again = kl_backend_print_open(config, folder, "/nonexistent/keiland-printd");
	check(again != NULL, "open again");
	if (again == NULL)
		return 1;
	count = kl_backend_print_printers(again, list, KL_BACKEND_PRINTERS_MAX);
	check(count == 1U, "the printer read from the file");
	kl_backend_print_close(again);

	/* The verdict. */
	if (failures != 0U) {
		printf("host-print-folders: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-print-folders: PASS\n");
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

/* Tells whether a path is a folder that others cannot enter. */
static int
folder_closed(
	const char *path)
{
	struct stat status;
	int directory;
	int error;

	/* What is there. */
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* A folder, with nothing for the group or the others. */
	directory = S_ISDIR(status.st_mode);
	if (!directory)
		return 0;
	if ((status.st_mode & 077) != 0)
		return 0;

	/* Succeeded: a closed folder. */
	return 1;
}
