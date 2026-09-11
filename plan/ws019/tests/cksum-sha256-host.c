/* Actual cksum with short/interrupted/error read and close adapters. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static ssize_t test_read(int, void *, size_t);
static int test_close(int);
#define read test_read
#define close test_close
#define main cksum_main
#include "userland/base/cksum/main.c"
#undef main
#undef close
#undef read
void command_error(const char *command, const char *operand)
{ fprintf(stderr, "%s: %s: %s\n", command, operand ? operand : "stdin", strerror(errno)); }
static unsigned calls;
static ssize_t test_read(int fd, void *buffer, size_t size)
{
	calls++;
	if (getenv("READ_ERROR") && calls == 2) { errno = EIO; return -1; }
	if (getenv("SHORT_READ")) {
		if (calls == 1) { errno = EINTR; return -1; }
		if (size > 7) size = 7;
	}
	return read(fd, buffer, size);
}
static int test_close(int fd)
{
	int result = close(fd);
	if (getenv("CLOSE_ERROR")) { errno = EIO; return -1; }
	return result;
}
int main(int argc, char **argv)
{
	struct command_sha256_context context;
	uint8_t byte = 0;
	if (getenv("LENGTH_OVERFLOW")) {
		command_sha256_init(&context);
		context.length = UINT64_MAX / 8U;
		if (command_sha256_update(&context, &byte, 1) != EOVERFLOW) abort();
		if (context.length != UINT64_MAX / 8U) abort();
		if (command_sha256_update(&context, NULL, 1) != EINVAL) abort();
		return 0;
	}
	return cksum_main(argc, argv);
}
