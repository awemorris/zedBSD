/*
 * KA-T040: exec preparation behavior fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/cred.h>
#include <kern/exec.h>
#include <kern/mount.h>
#include <zedbsd/process.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned checks;
static unsigned allocations;
static unsigned frees;
static int fail_allocation;
static void *last_allocation;
static size_t last_allocation_size;

#define CHECK(expression)                                                     \
	do {                                                                   \
		checks++;                                                      \
		if (!(expression)) {                                          \
			fprintf(stderr, "KA-T040: check failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                    \
			exit(1);                                               \
		}                                                              \
	} while (0)

void *
kern_calloc(size_t count, size_t size)
{
	void *allocation;

	if (fail_allocation) {
		fail_allocation = 0;
		return NULL;
	}
	allocation = calloc(count, size);
	if (allocation != NULL) {
		allocations++;
		last_allocation = allocation;
		last_allocation_size = count * size;
	}
	return allocation;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		frees++;
	free(pointer);
}

static int
allocation_contains(const void *pointer)
{
	uintptr_t base = (uintptr_t)last_allocation;
	uintptr_t value = (uintptr_t)pointer;

	return last_allocation != NULL && value >= base &&
	    value < base + last_allocation_size;
}

static void
check_shebang_result(const struct exec_shebang *result,
	const char *interpreter, const char *argument, int has_argument)
{
	CHECK(strcmp(result->interpreter, interpreter) == 0);
	CHECK(result->has_optional_argument == (unsigned)has_argument);
	CHECK(strcmp(result->optional_argument,
	    has_argument ? argument : "") == 0);
}

static void
test_shebang(void)
{
	struct exec_shebang result;
	static const unsigned char nul_line[] = {
	    '#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\0', 'x', '\n'};
	static const unsigned char control_line[] = {
	    '#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', 1, '\n'};
	char maximum_line[EXEC_SHEBANG_LINE_MAX + 1U];
	char overlong_line[EXEC_SHEBANG_LINE_MAX + 2U];
	const char *line;
	int parsed;

	memset(&result, 0xa5, sizeof(result));
	line = "ELF";
	parsed = exec_shebang_parse(line, strlen(line), 1, &result);
	CHECK(parsed == 0);
	check_shebang_result(&result, "", "", 0);

	memset(&result, 0xa5, sizeof(result));
	parsed = exec_shebang_parse(NULL, 0, 1, &result);
	CHECK(parsed == 0);
	check_shebang_result(&result, "", "", 0);
	CHECK(exec_shebang_parse(NULL, 1, 1, &result) == -EINVAL);
	CHECK(exec_shebang_parse("#!/bin/sh\n", 10, 1, NULL) == -EINVAL);

	line = "#!/bin/sh\n";
	parsed = exec_shebang_parse(line, strlen(line), 0, &result);
	CHECK(parsed == 1);
	check_shebang_result(&result, "/bin/sh", "", 0);

	line = "#! \t/bin/interpreter\t  one  two \t\r\n";
	parsed = exec_shebang_parse(line, strlen(line), 0, &result);
	CHECK(parsed == 1);
	check_shebang_result(&result, "/bin/interpreter", "one  two", 1);

	line = "#!/bin/sh -eu with spaces";
	parsed = exec_shebang_parse(line, strlen(line), 1, &result);
	CHECK(parsed == 1);
	check_shebang_result(&result, "/bin/sh", "-eu with spaces", 1);
	CHECK(exec_shebang_parse(line, strlen(line), 0, &result) == -E2BIG);

	line = "#!  \t\n";
	CHECK(exec_shebang_parse(line, strlen(line), 1, &result) == -ENOEXEC);
	line = "#!bin/sh\n";
	CHECK(exec_shebang_parse(line, strlen(line), 1, &result) == -ENOEXEC);
	line = "#!/bin/\rsh\n";
	CHECK(exec_shebang_parse(line, strlen(line), 1, &result) == -ENOEXEC);
	line = "#!/bin/sh bad\rarg\n";
	CHECK(exec_shebang_parse(line, strlen(line), 1, &result) == -ENOEXEC);
	CHECK(exec_shebang_parse(nul_line, sizeof(nul_line), 1, &result) ==
	    -ENOEXEC);
	CHECK(exec_shebang_parse(control_line, sizeof(control_line), 1, &result) ==
	    -ENOEXEC);

	memset(maximum_line, 'a', sizeof(maximum_line));
	memcpy(maximum_line, "#!/bin/x ", 9U);
	maximum_line[EXEC_SHEBANG_LINE_MAX] = '\n';
	parsed = exec_shebang_parse(maximum_line, sizeof(maximum_line), 0,
	    &result);
	CHECK(parsed == 1);
	CHECK(strcmp(result.interpreter, "/bin/x") == 0);
	CHECK(result.has_optional_argument == 1U);
	CHECK(strlen(result.optional_argument) ==
	    EXEC_SHEBANG_LINE_MAX - 9U);

	memset(overlong_line, 'a', sizeof(overlong_line));
	memcpy(overlong_line, "#!/bin/x ", 9U);
	overlong_line[EXEC_SHEBANG_LINE_MAX + 1U] = '\n';
	CHECK(exec_shebang_parse(overlong_line, sizeof(overlong_line), 0,
	    &result) == -E2BIG);
}

static void
check_vector(char **vector, const char *const expected[], size_t count)
{
	size_t index;

	CHECK(vector == last_allocation);
	for (index = 0; index < count; index++) {
		CHECK(vector[index] != NULL);
		CHECK(allocation_contains(vector[index]));
		CHECK(strcmp(vector[index], expected[index]) == 0);
	}
	CHECK(vector[count] == NULL);
}

static void
test_script_argv(void)
{
	struct exec_shebang shebang;
	char *old_argv[] = {"old-name", "first", "second", NULL};
	char *minimal_argv[] = {"old-name", NULL};
	const char *with_argument[] = {
	    "/bin/sh", "-eu with spaces", "/tmp/script", "first", "second"};
	const char *without_argument[] = {"/bin/sh", "/tmp/script"};
	char **vector;
	char **sentinel = (char **)(uintptr_t)1U;
	char *large;
	char **full_vector;
	size_t index;
	unsigned before_allocations;
	unsigned before_frees;
	int error;

	memset(&shebang, 0, sizeof(shebang));
	strcpy(shebang.interpreter, "/bin/sh");
	strcpy(shebang.optional_argument, "-eu with spaces");
	shebang.has_optional_argument = 1;
	before_allocations = allocations;
	before_frees = frees;
	error = exec_script_argv_build(old_argv, &shebang, "/tmp/script",
	    &vector);
	CHECK(error == 0);
	CHECK(allocations == before_allocations + 1U);
	check_vector(vector, with_argument,
	    sizeof(with_argument) / sizeof(with_argument[0]));
	exec_script_argv_free(vector);
	CHECK(frees == before_frees + 1U);

	shebang.has_optional_argument = 0;
	shebang.optional_argument[0] = '\0';
	error = exec_script_argv_build(minimal_argv, &shebang, "/tmp/script",
	    &vector);
	CHECK(error == 0);
	check_vector(vector, without_argument,
	    sizeof(without_argument) / sizeof(without_argument[0]));
	exec_script_argv_free(vector);

	CHECK(exec_script_argv_build(NULL, &shebang, "/tmp/script", &vector) ==
	    EINVAL);
	{
		char *empty_argv[] = {NULL};
		CHECK(exec_script_argv_build(empty_argv, &shebang,
		    "/tmp/script", &vector) == EINVAL);
	}
	CHECK(exec_script_argv_build(minimal_argv, NULL, "/tmp/script",
	    &vector) == EINVAL);
	{
		struct exec_shebang empty_shebang;
		memset(&empty_shebang, 0, sizeof(empty_shebang));
		CHECK(exec_script_argv_build(minimal_argv, &empty_shebang,
		    "/tmp/script", &vector) == EINVAL);
	}
	CHECK(exec_script_argv_build(minimal_argv, &shebang, NULL, &vector) ==
	    EINVAL);
	CHECK(exec_script_argv_build(minimal_argv, &shebang, "", &vector) ==
	    EINVAL);
	CHECK(exec_script_argv_build(minimal_argv, &shebang, "/tmp/script",
	    NULL) == EINVAL);

	full_vector = calloc(ZEDBSD_EXEC_VECTOR_MAX, sizeof(*full_vector));
	CHECK(full_vector != NULL);
	for (index = 0; index < ZEDBSD_EXEC_VECTOR_MAX; index++)
		full_vector[index] = "x";
	CHECK(exec_script_argv_build(full_vector, &shebang, "/tmp/script",
	    &vector) == E2BIG);
	free(full_vector);

	large = malloc(ZEDBSD_ARG_MAX + 1U);
	CHECK(large != NULL);
	memset(large, 'x', ZEDBSD_ARG_MAX);
	large[ZEDBSD_ARG_MAX] = '\0';
	{
		char *large_argv[] = {"old-name", large, NULL};
		CHECK(exec_script_argv_build(large_argv, &shebang,
		    "/tmp/script", &vector) == E2BIG);
	}
	free(large);

	before_allocations = allocations;
	fail_allocation = 1;
	vector = sentinel;
	CHECK(exec_script_argv_build(minimal_argv, &shebang, "/tmp/script",
	    &vector) == ENOMEM);
	CHECK(vector == sentinel);
	CHECK(allocations == before_allocations);

	before_frees = frees;
	exec_script_argv_free(NULL);
	CHECK(frees == before_frees);
}

static struct ucred
credential(unsigned ruid, unsigned euid, unsigned rgid, unsigned egid)
{
	struct ucred value;

	memset(&value, 0, sizeof(value));
	value.ruid = ruid;
	value.euid = euid;
	value.suid = 999U;
	value.rgid = rgid;
	value.egid = egid;
	value.sgid = 999U;
	return value;
}

static void
test_credentials(void)
{
	struct stat status;
	struct ucred value;
	unsigned secure;

	memset(&status, 0, sizeof(status));
	status.st_mode = S_ISUID | S_ISGID;
	status.st_uid = 0;
	status.st_gid = 10;

	value = credential(100, 100, 200, 200);
	secure = 0;
	exec_credential_prepare(&value, &status, 0, 0, &secure);
	CHECK(value.euid == 0 && value.egid == 10);
	CHECK(value.suid == 0 && value.sgid == 10);
	CHECK(secure == 1U);

	value = credential(100, 100, 200, 200);
	secure = 9;
	exec_credential_prepare(&value, &status, 0, 1, &secure);
	CHECK(value.euid == 100 && value.egid == 200);
	CHECK(value.suid == 100 && value.sgid == 200);
	CHECK(secure == 0U);

	value = credential(100, 100, 200, 200);
	secure = 9;
	exec_credential_prepare(&value, &status, MOUNT_NOSUID, 0, &secure);
	CHECK(value.euid == 100 && value.egid == 200);
	CHECK(value.suid == 100 && value.sgid == 200);
	CHECK(secure == 0U);

	value = credential(100, 100, 200, 200);
	secure = 0;
	exec_credential_prepare(&value, &status, MOUNT_READ_ONLY, 0, &secure);
	CHECK(value.euid == 0 && value.egid == 10);
	CHECK(value.suid == 0 && value.sgid == 10);
	CHECK(secure == 1U);

	value = credential(100, 101, 200, 201);
	secure = 0;
	exec_credential_prepare(&value, NULL, 0, 0, &secure);
	CHECK(value.euid == 101 && value.egid == 201);
	CHECK(value.suid == 101 && value.sgid == 201);
	CHECK(secure == 1U);

	value = credential(100, 100, 200, 200);
	exec_credential_prepare(&value, &status, 0, 0, NULL);
	CHECK(value.euid == 0 && value.egid == 10);
	CHECK(value.suid == 0 && value.sgid == 10);

	secure = 77;
	exec_credential_prepare(NULL, &status, 0, 0, &secure);
	CHECK(secure == 77U);
}

int
main(void)
{
	test_shebang();
	test_script_argv();
	test_credentials();
	printf("KA-T040: exec preparation baseline: PASS (%u checks)\n", checks);
	return 0;
}
