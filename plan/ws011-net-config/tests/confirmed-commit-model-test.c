/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/networkd/confirmed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fixture {
	char seen[512];
	size_t used;
	unsigned calls;
	unsigned fail_at;
};

static void fail(const char *);
static void expect(int, const char *);
static void write_program(const char *, const char *, mode_t);
static void write_bytes(const char *, const void *, size_t, mode_t);
static int validate_line(const char *, char *, size_t, void *);
static int execute_line(const char *, char *, size_t, void *);

int
main(
	void)
{
	struct networkd_confirmed state;
	struct fixture fixture;
	char path[128];
	char diagnostic[1024];
	uint32_t token;
	uint64_t now;
	int result;

	(void)snprintf(path, sizeof(path), "/tmp/net.rollback.%ld.model",
	    (long)getpid());
	(void)unlink(path);
	networkd_confirmed_init(&state);
	now = 1000000ULL;
	write_program(path, "V1 UP em0\nV1 DNS 10.0.0.1\n", 0600);
	expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) == 0,
	    "arm valid program");
	expect(token != 0U && networkd_confirmed_active(&state), "active token");
	expect(networkd_confirmed_check(&state, 0U) != 0 && errno == EBUSY,
	    "busy check");
	expect(networkd_confirmed_check(&state, token) == 0, "matching check");
	expect(networkd_confirmed_disarm(&state, token + 1U) != 0 && errno == ESTALE,
	    "stale disarm");
	expect(networkd_confirmed_poll_timeout(&state, now) == 60000,
	    "minute poll timeout");
	expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) != 0 &&
	    errno == EBUSY, "reject second pending transaction");

	/* Replacement after acceptance cannot change the retained program. */
	expect(unlink(path) == 0, "unlink accepted path");
	write_program(path, "V1 REPLACEMENT\n", 0600);
	memset(&fixture, 0, sizeof(fixture));
	result = networkd_confirmed_run_due(&state, now + 60000000ULL,
	    execute_line, &fixture, diagnostic, sizeof(diagnostic));
	expect(result == 1, "deadline rollback");
	expect(fixture.calls == 2U && strstr(fixture.seen, "V1 UP em0") != NULL &&
	    strstr(fixture.seen, "REPLACEMENT") == NULL,
	    "execute accepted descriptor");
	expect(access(path, F_OK) == 0, "preserve replacement inode");
	expect(unlink(path) == 0, "remove replacement fixture");

	/* Partial rollback continues through every valid line and then clears. */
	write_program(path, "V1 UP em0\nV1 FAIL em0\nV1 DOWN em0\n", 0600);
	expect(networkd_confirmed_arm(&state, path, getuid(), 2U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) == 0,
	    "arm partial program");
	memset(&fixture, 0, sizeof(fixture));
	fixture.fail_at = 2U;
	expect(networkd_confirmed_rollback(&state, execute_line, &fixture,
	    diagnostic, sizeof(diagnostic)) != 0 && fixture.calls == 3U,
	    "partial rollback continuation");
	expect(strstr(diagnostic, "rollback line 2") != NULL,
	    "bounded line diagnostic");
	expect(!networkd_confirmed_active(&state) && access(path, F_OK) != 0,
	    "partial rollback clears transaction");

	/* Unsafe and malformed files fail before ownership changes. */
	write_program(path, "V1 UP em0\n", 0644);
	expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) != 0,
	    "reject unsafe mode");
	expect(unlink(path) == 0, "remove mode fixture");
	write_program(path, "V1 UP em0", 0600);
	expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) != 0,
	    "reject missing newline");
	expect(unlink(path) == 0, "remove malformed fixture");
	expect(networkd_confirmed_arm(&state, "/tmp/not-a-rollback", getuid(),
	    1U, now, validate_line, NULL, &token, diagnostic,
	    sizeof(diagnostic)) != 0, "reject path namespace");
	expect(networkd_confirmed_arm(&state, path, getuid(), 0U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) != 0,
	    "reject zero timeout");

	/* Every rollback-program bound is enforced before arming. */
	{
		char over_count[(NETWORKD_ROLLBACK_OPERATION_MAX + 1U) * 9U + 1U];
		size_t used = 0U;
		unsigned index;

		for (index = 0U; index <= NETWORKD_ROLLBACK_OPERATION_MAX; index++) {
			memcpy(over_count + used, "V1 UP e\n", 8U);
			used += 8U;
		}
		over_count[used] = '\0';
		write_program(path, over_count, 0600);
		expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
		    validate_line, NULL, &token, diagnostic,
		    sizeof(diagnostic)) != 0, "reject over-count program");
		expect(unlink(path) == 0, "remove over-count fixture");
	}
	{
		char *over_line;

		over_line = malloc(NETWORKD_ROLLBACK_LINE_MAX + 3U);
		expect(over_line != NULL, "allocate over-line fixture");
		memset(over_line, 'A', NETWORKD_ROLLBACK_LINE_MAX + 1U);
		over_line[0] = 'V';
		over_line[1] = '1';
		over_line[2] = ' ';
		over_line[NETWORKD_ROLLBACK_LINE_MAX + 1U] = '\n';
		over_line[NETWORKD_ROLLBACK_LINE_MAX + 2U] = '\0';
		write_program(path, over_line, 0600);
		free(over_line);
		expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
		    validate_line, NULL, &token, diagnostic,
		    sizeof(diagnostic)) != 0, "reject over-line program");
		expect(unlink(path) == 0, "remove over-line fixture");
	}
	{
		char *over_total;

		over_total = malloc(NETWORKD_ROLLBACK_PROGRAM_MAX + 1U);
		expect(over_total != NULL, "allocate over-total fixture");
		memset(over_total, 'A', NETWORKD_ROLLBACK_PROGRAM_MAX + 1U);
		over_total[0] = 'V';
		over_total[1] = '1';
		over_total[2] = ' ';
		over_total[NETWORKD_ROLLBACK_PROGRAM_MAX] = '\n';
		write_bytes(path, over_total,
		    NETWORKD_ROLLBACK_PROGRAM_MAX + 1U, 0600);
		free(over_total);
		expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
		    validate_line, NULL, &token, diagnostic,
		    sizeof(diagnostic)) != 0, "reject over-total program");
		expect(unlink(path) == 0, "remove over-total fixture");
	}
	{
		static const char embedded_nul[] = "V1 UP\0em0\n";
		write_bytes(path, embedded_nul, sizeof(embedded_nul) - 1U, 0600);
		expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
		    validate_line, NULL, &token, diagnostic,
		    sizeof(diagnostic)) != 0, "reject embedded NUL");
		expect(unlink(path) == 0, "remove NUL fixture");
	}
	{
		char target[160];

		(void)snprintf(target, sizeof(target), "%s.target", path);
		write_program(target, "V1 UP em0\n", 0600);
		expect(symlink(target, path) == 0, "create symlink fixture");
		expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
		    validate_line, NULL, &token, diagnostic,
		    sizeof(diagnostic)) != 0, "reject symlink program");
		expect(unlink(path) == 0 && unlink(target) == 0,
		    "remove symlink fixture");
	}

	/* Daemon reset forgets its volatile generation and rejects the old token. */
	write_program(path, "V1 UP em0\n", 0600);
	expect(networkd_confirmed_arm(&state, path, getuid(), 1U, now,
	    validate_line, NULL, &token, diagnostic, sizeof(diagnostic)) == 0,
	    "arm restart fixture");
	networkd_confirmed_reset(&state);
	expect(!networkd_confirmed_active(&state) && access(path, F_OK) != 0 &&
	    networkd_confirmed_check(&state, token) != 0 && errno == ENOENT,
	    "restart forgets volatile transaction");
	networkd_confirmed_reset(&state);
	puts("WS011 confirmed transaction model: PASS");
	return 0;
}

static void
fail(
	const char *message)
{
	fprintf(stderr, "confirmed-commit-model-test: %s\n", message);
	exit(1);
}

static void
expect(
	int condition,
	const char *message)
{
	if (!condition)
		fail(message);
}

static void
write_program(
	const char *path,
	const char *content,
	mode_t mode)
{
	write_bytes(path, content, strlen(content), mode);
}

static void
write_bytes(
	const char *path,
	const void *content,
	size_t length,
	mode_t mode)
{
	int descriptor;

	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
	if (descriptor < 0)
		fail("create program");
	if (write(descriptor, content, length) != (ssize_t)length ||
	    close(descriptor) != 0)
		fail("write program");
}

static int
validate_line(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	(void)context;
	if (strncmp(line, "V1 ", 3U) != 0) {
		(void)snprintf(diagnostic, capacity, "invalid test line");
		return -1;
	}
	return 0;
}

static int
execute_line(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	struct fixture *fixture = context;
	int count;

	fixture->calls++;
	count = snprintf(fixture->seen + fixture->used,
	    sizeof(fixture->seen) - fixture->used, "%s\n", line);
	if (count > 0 && (size_t)count < sizeof(fixture->seen) - fixture->used)
		fixture->used += (size_t)count;
	if (fixture->fail_at == fixture->calls) {
		(void)snprintf(diagnostic, capacity, "synthetic failure");
		errno = EIO;
		return -1;
	}
	return 0;
}
