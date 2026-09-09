/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <sys/statvfs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct statvfs observation;
static int observations;
static int fixture_statvfs(const char *path, struct statvfs *stats);
#define main tested_df_main
#define statvfs(path, stats) fixture_statvfs(path, stats)
#include "userland/base/df/main.c"
#undef statvfs
#undef main

static int
fixture_statvfs(const char *path, struct statvfs *stats)
{
	observations++;
	if (!strcmp(path, "/missing")) {
		errno = ENOENT;
		return -1;
	}
	*stats = observation;
	return 0;
}

static void
check(int truth)
{
	if (!truth)
		abort();
}

int
main(void)
{
	unsigned long long seed, blocks, fragment, unit, actual, used, avail;
	__uint128_t expected;
	unsigned i, percent;
	int error, saved, full, before;
	FILE *capture;
	char text[2048];
	size_t length;
	char *defaults[] = { "df", NULL };
	char *portable[] = { "df", "-Pk", "--", "/good", NULL };
	char *multiple[] = { "df", "/missing", "/good", NULL };
	char *invalid[] = { "df", "-unknown", NULL };

	/* A wider independent arithmetic oracle covers valid and overflowing ratios. */
	seed = 0x197519L;
	for (i = 0; i < 20000; i++) {
		seed = seed * 6364136223846793005ULL + 1;
		blocks = seed;
		seed = seed * 6364136223846793005ULL + 1;
		fragment = seed;
		if (i % 3 == 0)
			fragment %= 8193;
		unit = 512;
		if (i & 1)
			unit = 1024;
		expected = (__uint128_t)blocks * fragment / unit;
		actual = 73;
		error = units(blocks, fragment, unit, &actual);
		if (expected > ULLONG_MAX) {
			check(error == EOVERFLOW && actual == 73);
		} else {
			check(error == 0 && actual == expected);
		}
		used = blocks;
		avail = (ULLONG_MAX - used) / 3;
		expected = ((__uint128_t)used * 100 + used + avail - 1) /
		    ((__uint128_t)used + avail);
		percent = capacity(used, avail);
		check(percent == expected);
	}
	check(capacity(0, 0) == 0);
	check(capacity(0, ULLONG_MAX) == 0);
	check(capacity(ULLONG_MAX, 0) == 100);
	check(capacity(1, 2) == 34);
	check(units(ULLONG_MAX, 512, 512, &actual) == 0 && actual == ULLONG_MAX);
	check(units(ULLONG_MAX, 1024, 512, &actual) == EOVERFLOW);
	check(units(0, ULLONG_MAX, 1024, &actual) == 0 && actual == 0);
	check(units(1, 511, 512, &actual) == 0 && actual == 0);

	/* Exercise the actual CLI and its final buffered write. */
	saved = dup(STDOUT_FILENO);
	capture = tmpfile();
	check(saved >= 0 && capture != NULL);
	check(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	memset(&observation, 0, sizeof(observation));
	observation.f_frsize = 4096;
	observation.f_blocks = 100;
	observation.f_bfree = 60;
	observation.f_bavail = 50;
	check(tested_df_main(1, defaults) == 0 && defaults[1] == NULL);
	check(tested_df_main(4, portable) == 0);
	check(tested_df_main(3, multiple) == 1);
	before = observations;
	check(tested_df_main(2, invalid) == 2 && observations == before);
	observation.f_bavail = 61;
	check(tested_df_main(1, defaults) == 1);
	observation.f_bavail = 0;
	observation.f_bfree = 101;
	check(tested_df_main(1, defaults) == 1);
	observation.f_bfree = 0;
	observation.f_frsize = 0;
	check(tested_df_main(1, defaults) == 1);
	observation.f_frsize = 4096;
	observation.f_blocks = ULLONG_MAX;
	check(tested_df_main(1, defaults) == 1);
	observation.f_frsize = 512;
	observation.f_bfree = ULLONG_MAX;
	observation.f_bavail = ULLONG_MAX;
	check(tested_df_main(1, defaults) == 0);
	rewind(capture);
	length = fread(text, 1, sizeof(text) - 1, capture);
	text[length] = '\0';
	check(strstr(text, "1024-blocks") != NULL && strstr(text, "512-blocks") != NULL);
	check(strstr(text, "45% /good") != NULL);
	full = open("/dev/full", O_WRONLY);
	check(full >= 0 && dup2(full, STDOUT_FILENO) >= 0);
	check(tested_df_main(1, defaults) == 1);
	clearerr(stdout);
	check(dup2(saved, STDOUT_FILENO) >= 0);
	close(saved);
	close(full);
	fclose(capture);
	puts("df capacity PASS 20000 independent arithmetic cases and CLI/error boundaries");
	return 0;
}
