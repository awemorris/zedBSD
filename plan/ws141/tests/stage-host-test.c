/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the BCM2711 graphics driver's stage marks (ws141-p002).
 *
 * It links the driver's stage.c against stand-ins for the kernel calls it
 * makes, and checks that a mark is cut to 79 columns, that rpi4gpu.off=1 and
 * <family>.stop=<stage> are recognized as whole tokens only, and that a
 * missing command line lets every stage run.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The command line the stand-in handoff reports; NULL for none. */
static const char *test_command_line;

/* The last line the stand-in log received, and how many it received. */
static char test_last_line[512];
static unsigned test_line_count;

/* The number of failed checks. */
static unsigned test_failures;

void *kern_boot_handoff(const char *name);
int kern_boot_parameters_token_present(const char *text, const char *token);
void kern_usleep_range(unsigned min_us, unsigned max_us);
void kern_logf(const char *format, ...);

static void check(bool condition, const char *what);

/*
 * Supplies the platform handoff lookup for the current stage fixture.
 */
void *
kern_boot_handoff(
	const char *name)
{
	int comparison;

	/* Only the command line is asked for. */
	comparison = strcmp(name, "boot.command-line");
	if (comparison != 0)
		return NULL;

	/* Reports the test's line. */
	return (void *)test_command_line;
}

/*
 * Supplies whole-token command-line matching for stage admission.
 */
int
kern_boot_parameters_token_present(
	const char *text,
	const char *token)
{
	size_t length;
	const char *cursor;
	int comparison;

	/* No line has no tokens. */
	if (text == NULL)
		return 0;

	/* Compares every space-separated token whole. */
	length = strlen(token);
	cursor = text;
	while (*cursor != '\0') {
		/* Skips the separators. */
		while (*cursor == ' ')
			cursor++;

		/* Matches a token of the same length. */
		comparison = strncmp(cursor, token, length);
		if (comparison == 0) {
			if (cursor[length] == ' ' || cursor[length] == '\0')
				return 1;
		}

		/* Moves past this token. */
		while (*cursor != ' ' && *cursor != '\0')
			cursor++;
	}

	/* Not there. */
	return 0;
}

/*
 * Observes stage pauses without delaying the host fixture.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	(void)min_us;
	(void)max_us;
}

/*
 * Records the last stage diagnostic emitted by production code.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* Keeps the formatted line. */
	va_start(arguments, format);
	vsnprintf(test_last_line, sizeof(test_last_line), format, arguments);
	va_end(arguments);
	test_line_count++;
}

/*
 * Runs the checks and reports PASS or FAIL.
 */
int
main(
	void)
{
	char expected[128];
	bool allowed;
	bool off;

	/* A long mark is cut to 79 columns and keeps its newline. */
	bcm2711_stage_mark("rpi4gpu", "%s", "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
	check(strlen(test_last_line) == 80U, "a long mark is 79 columns and a newline");
	check(test_last_line[79] == '\n', "the cut mark ends with a newline");
	check(strncmp(test_last_line, "rpi4gpu: 0123", 13) == 0, "the mark starts with its family");

	/* A short mark is unchanged. */
	bcm2711_stage_mark("v3d", "V0 ok");
	check(strcmp(test_last_line, "v3d: V0 ok\n") == 0, "a short mark is unchanged");

	/* Without a command line the driver attaches and every stage runs. */
	test_command_line = NULL;
	off = bcm2711_stage_driver_off();
	check(!off, "no line: the driver attaches");
	allowed = bcm2711_stage_allowed("rpi4gpu", "P0");
	check(allowed, "no line: P0 runs");

	/* rpi4gpu.off=1 keeps the driver out; a longer token does not. */
	test_command_line = "console=serial0 rpi4gpu.off=1 kmsg=console";
	off = bcm2711_stage_driver_off();
	check(off, "rpi4gpu.off=1 keeps the driver out");
	test_command_line = "rpi4gpu.off=10";
	off = bcm2711_stage_driver_off();
	check(!off, "rpi4gpu.off=10 is not the off token");

	/* rpi4gpu.stop=N1 stops N1 only, and not V3D's stages. */
	test_command_line = "rpi4gpu.stop=N1 v3d.stop=V5";
	allowed = bcm2711_stage_allowed("rpi4gpu", "N1");
	check(!allowed, "rpi4gpu.stop=N1 stops N1");
	allowed = bcm2711_stage_allowed("rpi4gpu", "N0");
	check(allowed, "rpi4gpu.stop=N1 lets N0 run");
	allowed = bcm2711_stage_allowed("v3d", "V5");
	check(!allowed, "v3d.stop=V5 stops V5");
	allowed = bcm2711_stage_allowed("v3d", "V0");
	check(allowed, "v3d.stop=V5 lets V0 run");

	/* A stage name too long for the token is refused rather than cut. */
	allowed = bcm2711_stage_allowed("rpi4gpu", "a-stage-name-much-longer-than-any-real-stage-name");
	check(!allowed, "an over-long stage name is refused");

	/* The pause says what it waits for. */
	bcm2711_stage_pause("rpi4gpu", "N1");
	snprintf(expected, sizeof(expected), "rpi4gpu: N1 waits %u ms before writing\n", BCM2711_STAGE_PAUSE_MS);
	check(strcmp(test_last_line, expected) == 0, "the pause announces itself");

	/* Reports the verdict. */
	if (test_failures != 0) {
		printf("stage-host-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("stage-host-test: PASS\n");
	return 0;
}

/* Counts and shows one failed check. */
static void
check(
	bool condition,
	const char *what)
{
	/* A held check says nothing. */
	if (condition)
		return;

	/* Shows the failed check. */
	printf("FAIL: %s (last line: %s)\n", what, test_last_line);
	test_failures++;
}
