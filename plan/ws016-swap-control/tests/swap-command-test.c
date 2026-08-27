/* SWAP-T009/T010 production-linked command fixture. SPDX-License-Identifier:
 * Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define TEST_DESCRIPTOR 73
#define TEST_CALL_MAX 16
#define TEST_OUTPUT_MAX 4096

struct ioctl_call {
	int descriptor;
	unsigned long request;
	struct system_swap_control control;
};

static struct {
	int open_count;
	int close_count;
	int open_error;
	int open_flags;
	char open_path[64];
	int ioctl_count;
	int ioctl_errors[TEST_CALL_MAX];
	struct ioctl_call calls[TEST_CALL_MAX];
} mock;

int swapon_program_main(int, char **);
int swapoff_program_main(int, char **);

static void
fail(const char *test, const char *message)
{
	fprintf(stderr, "swap-command-test: %s: %s\n", test, message);
	exit(1);
}

static void
require(int condition, const char *test, const char *message)
{
	if (!condition)
		fail(test, message);
}

static void
reset_mock(void)
{
	memset(&mock, 0, sizeof(mock));
}

int
swap_test_open(const char *path, int flags, ...)
{
	mock.open_count++;
	mock.open_flags = flags;
	(void)snprintf(mock.open_path, sizeof(mock.open_path), "%s", path);
	if (mock.open_error != 0) {
		errno = mock.open_error;
		return -1;
	}
	return TEST_DESCRIPTOR;
}

int
swap_test_ioctl(int descriptor, unsigned long request, ...)
{
	struct system_swap_control *control;
	va_list ap;
	int index = mock.ioctl_count;

	if (index >= TEST_CALL_MAX) {
		errno = E2BIG;
		return -1;
	}
	va_start(ap, request);
	control = va_arg(ap, struct system_swap_control *);
	va_end(ap);
	mock.calls[index].descriptor = descriptor;
	mock.calls[index].request = request;
	mock.calls[index].control = *control;
	mock.ioctl_count++;
	if (mock.ioctl_errors[index] != 0) {
		errno = mock.ioctl_errors[index];
		return -1;
	}
	return 0;
}

int
swap_test_close(int descriptor)
{
	if (descriptor != TEST_DESCRIPTOR) {
		errno = EBADF;
		return -1;
	}
	mock.close_count++;
	return 0;
}

static int
invoke(int (*entry)(int, char **), int argc, char **argv, char *output,
	       size_t output_size)
{
	FILE *temporary;
	int saved, status;
	size_t length;

	temporary = tmpfile();
	if (temporary == NULL)
		fail("fixture", "tmpfile");
	saved = dup(STDERR_FILENO);
	if (saved < 0 || fflush(stderr) != 0 ||
	    dup2(fileno(temporary), STDERR_FILENO) < 0)
		fail("fixture", "redirect stderr");
	status = entry(argc, argv);
	if (fflush(stderr) != 0 || dup2(saved, STDERR_FILENO) < 0 ||
	    close(saved) != 0)
		fail("fixture", "restore stderr");
	if (fseek(temporary, 0, SEEK_SET) != 0)
		fail("fixture", "rewind stderr");
	length = fread(output, 1, output_size - 1, temporary);
	if (ferror(temporary) || !feof(temporary))
		fail("fixture", "read stderr");
	output[length] = '\0';
	if (fclose(temporary) != 0)
		fail("fixture", "close stderr capture");
	return status;
}

static void
require_open(const char *test)
{
	require(mock.open_count == 1, test, "open count");
	require(strcmp(mock.open_path, "/dev/system") == 0, test,
		"open path");
	require(mock.open_flags == O_RDWR, test, "open flags");
}

static void
require_call(const char *test, int index, unsigned long request,
	     const char *source)
{
	const struct system_swap_control *control = &mock.calls[index].control;
	unsigned int reserved;

	require(mock.calls[index].descriptor == TEST_DESCRIPTOR, test,
		"ioctl descriptor");
	require(mock.calls[index].request == request, test, "ioctl request");
	require(control->version == ZEDBSD_SYSTEM_SWAP_VERSION, test,
		"request version");
	require(control->struct_size == sizeof(*control), test,
		"request size");
	require(control->flags == 0 && control->reserved0 == 0, test,
		"request flags");
	require(strcmp(control->source, source) == 0, test, "request source");
	for (reserved = 0; reserved < 8; reserved++)
		require(control->reserved[reserved] == 0, test,
			"request reserved field");
}

static void
test_usage(const char *name, int (*entry)(int, char **))
{
	char output[TEST_OUTPUT_MAX], expected[128];
	char *none[] = {(char *)name, NULL};
	char *marker[] = {(char *)name, "--", NULL};
	char *unknown[] = {(char *)name, "valid", "-x", NULL};

	(void)snprintf(expected, sizeof(expected),
		       "usage: %s [--] SOURCE...\n", name);
	reset_mock();
	require(invoke(entry, 1, none, output, sizeof(output)) == 2, name,
		"no-operand status");
	require(strcmp(output, expected) == 0, name, "no-operand usage");
	require(mock.open_count == 0 && mock.ioctl_count == 0, name,
		"no-operand side effect");

	reset_mock();
	require(invoke(entry, 2, marker, output, sizeof(output)) == 2, name,
		"marker-only status");
	require(strcmp(output, expected) == 0, name, "marker-only usage");

	reset_mock();
	require(invoke(entry, 3, unknown, output, sizeof(output)) == 2, name,
		"unknown-option status");
	require(strcmp(output, expected) == 0, name, "unknown-option usage");
	require(mock.open_count == 0 && mock.ioctl_count == 0, name,
		"unknown-option side effect");
}

static void
test_success(const char *name, int (*entry)(int, char **),
	     unsigned long request)
{
	char output[TEST_OUTPUT_MAX];
	char *arguments[] = {(char *)name, "/dev/sda2", "UUID=0123",
			     "boot0:swapfile", NULL};

	reset_mock();
	require(invoke(entry, 4, arguments, output, sizeof(output)) == 0, name,
		"success status");
	require(output[0] == '\0', name, "success diagnostic");
	require_open(name);
	require(mock.ioctl_count == 3 && mock.close_count == 1, name,
		"success call counts");
	require_call(name, 0, request, "/dev/sda2");
	require_call(name, 1, request, "UUID=0123");
	require_call(name, 2, request, "boot0:swapfile");
}

static void
test_marker(const char *name, int (*entry)(int, char **),
	    unsigned long request)
{
	char output[TEST_OUTPUT_MAX];
	char *arguments[] = {(char *)name, "--", "-source", "--", NULL};

	reset_mock();
	require(invoke(entry, 4, arguments, output, sizeof(output)) == 0, name,
		"marker status");
	require(output[0] == '\0', name, "marker diagnostic");
	require(mock.ioctl_count == 2, name, "marker ioctl count");
	require_call(name, 0, request, "-source");
	require_call(name, 1, request, "--");
}

static void
test_continuation(const char *name, int (*entry)(int, char **),
		  unsigned long request)
{
	char output[TEST_OUTPUT_MAX], expected[TEST_OUTPUT_MAX];
	char *arguments[] = {(char *)name, "first", "second", "third", NULL};

	reset_mock();
	mock.ioctl_errors[0] = EBUSY;
	mock.ioctl_errors[2] = ENOENT;
	require(invoke(entry, 4, arguments, output, sizeof(output)) == 1, name,
		"aggregate failure status");
	(void)snprintf(expected, sizeof(expected), "%s: first: %s\n"
		       "%s: third: %s\n",
		       name, strerror(EBUSY), name, strerror(ENOENT));
	require(strcmp(output, expected) == 0, name, "failure diagnostics");
	require(mock.ioctl_count == 3 && mock.close_count == 1, name,
		"failure continuation");
	require_call(name, 0, request, "first");
	require_call(name, 1, request, "second");
	require_call(name, 2, request, "third");
}

static void
test_open_failure(const char *name, int (*entry)(int, char **))
{
	char output[TEST_OUTPUT_MAX], expected[TEST_OUTPUT_MAX];
	char *arguments[] = {(char *)name, "one", "two", NULL};

	reset_mock();
	mock.open_error = EACCES;
	require(invoke(entry, 3, arguments, output, sizeof(output)) == 1, name,
		"open failure status");
	(void)snprintf(expected, sizeof(expected), "%s: one: %s\n"
		       "%s: two: %s\n",
		       name, strerror(EACCES), name, strerror(EACCES));
	require(strcmp(output, expected) == 0, name,
		"open failure diagnostics");
	require_open(name);
	require(mock.ioctl_count == 0 && mock.close_count == 0, name,
		"open failure side effect");
}

static void
test_bounded_source(const char *name, int (*entry)(int, char **),
		    unsigned long request)
{
	char output[TEST_OUTPUT_MAX], expected[TEST_OUTPUT_MAX];
	char too_long[ZEDBSD_SYSTEM_SWAP_SOURCE_MAX + 1];
	char longest[ZEDBSD_SYSTEM_SWAP_SOURCE_MAX];
	char *arguments[] = {(char *)name, too_long, longest, "after", NULL};

	memset(too_long, 'x', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	memset(longest, 'y', sizeof(longest) - 1);
	longest[sizeof(longest) - 1] = '\0';
	reset_mock();
	require(invoke(entry, 4, arguments, output, sizeof(output)) == 1, name,
		"bounded source status");
	(void)snprintf(expected, sizeof(expected), "%s: %s: %s\n", name,
		       too_long, strerror(ENAMETOOLONG));
	require(strcmp(output, expected) == 0, name,
		"bounded source diagnostic");
	require(mock.ioctl_count == 2, name, "bounded source continuation");
	require_call(name, 0, request, longest);
	require_call(name, 1, request, "after");
}

static void
test_command(const char *name, int (*entry)(int, char **),
	     unsigned long request)
{
	test_usage(name, entry);
	test_success(name, entry, request);
	test_marker(name, entry, request);
	test_continuation(name, entry, request);
	test_open_failure(name, entry);
	test_bounded_source(name, entry, request);
}

int
main(void)
{
	test_command("swapon", swapon_program_main, ZEDBSD_SYSTEM_SWAP_ADD);
	test_command("swapoff", swapoff_program_main,
		     ZEDBSD_SYSTEM_SWAP_REMOVE);
	puts("SWAP-T009/T010 swap commands: PASS");
	return 0;
}
