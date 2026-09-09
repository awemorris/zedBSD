#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned errors;
void command_error(const char *command, const char *operand)
{ (void)command; (void)operand; errors++; }
#define main mv_main
#include "../../../userland/base/mv/main.c"
#undef main

static int open_failure, flush_failure, close_failure, whole_failure;
static unsigned opened, flushed, closed;
static int test_open(const char *path, int flags, ...)
{ (void)path; assert(flags == (O_RDONLY | O_NONBLOCK)); opened++; if (open_failure) { errno = EACCES; return -1; } return 9; }
static int test_fsync(int fd)
{ assert(fd == 9); flushed++; if (flush_failure) { errno = EIO; return -1; } return 0; }
static int test_close(int fd)
{ assert(fd == 9); closed++; if (close_failure) { errno = ENOSPC; return -1; } return 0; }
static void test_sync(void) { if (whole_failure) errno = EIO; }
#define main sync_main
#define open test_open
#define fsync test_fsync
#define close test_close
#define sync test_sync
#include "../../../userland/base/sync/main.c"
#undef main
#undef open
#undef fsync
#undef close
#undef sync
static void create(const char *path, char byte)
{ int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0); assert(write(fd, &byte, 1) == 1); assert(close(fd) == 0); }
static void content(const char *path, char byte)
{ char got; int fd = open(path, O_RDONLY); assert(fd >= 0); assert(read(fd, &got, 1) == 1 && got == byte); assert(close(fd) == 0); }
int main(int argc, char **argv)
{
	char *strict[] = {"mv", "-T", "--update=none-fail", "--", "stage", "final", NULL};
	char *skip[] = {"mv", "-n", "-T", "stage", "final", NULL};
	char *replace[] = {"mv", "-T", "stage", "final", NULL};
	char *files[] = {"sync", "first", "second", NULL};
	char *all[] = {"sync", NULL};
	assert(argc == 2 && chdir(argv[1]) == 0);
	create("stage", 'a'); assert(mv_main(6, strict) == 0); content("final", 'a');
	create("stage", 'b'); assert(mv_main(6, strict) == 1);
	content("stage", 'b'); content("final", 'a');
	assert(mv_main(5, skip) == 0); content("stage", 'b');
	assert(mv_main(4, replace) == 0); content("final", 'b');
	assert(unlink("final") == 0 && mkdir("final", 0700) == 0);
	create("stage", 'c'); assert(mv_main(6, strict) == 1); content("stage", 'c');
	assert(rmdir("final") == 0); assert(symlink("stage", "final") == 0);
	assert(mv_main(6, strict) == 1); content("stage", 'c');
	assert(unlink("final") == 0 && unlink("stage") == 0);
	assert(sync_main(3, files) == 0 && opened == 2 && flushed == 2 && closed == 2);
	flush_failure = close_failure = 1;
	assert(sync_main(3, files) == 1 && closed == 4 && errno == EIO);
	flush_failure = 0;
	assert(sync_main(3, files) == 1 && errno == ENOSPC);
	close_failure = 0; open_failure = 1;
	assert(sync_main(3, files) == 1 && closed == 6 && errno == EACCES);
	whole_failure = 1; assert(sync_main(1, all) == 1);
	whole_failure = 0; assert(sync_main(1, all) == 0);
	puts("publication commands PASS no-clobber exact-path symlink flush/close/open errors");
	return 0;
}
