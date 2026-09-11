#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned close_calls;
static int copy_failure, close_failure;
void command_error(const char *command, const char *path) { (void)command; (void)path; }
int command_copy_fd(int input, int output)
{
	char buffer[64]; ssize_t size;
	while ((size = read(input, buffer, sizeof(buffer))) > 0) {
		if (copy_failure) {
			assert(write(output, buffer, 1) == 1); errno = ENOSPC; return -1;
		}
		assert(write(output, buffer, (size_t)size) == size);
	}
	return size < 0 ? -1 : 0;
}
static int tracked_close(int fd)
{
	int result = close(fd); close_calls++;
	if (close_failure) { errno = EIO; return -1; }
	return result;
}
#define main cp_main
#define close tracked_close
#include "../../../userland/base/cp/main.c"
#undef close
#undef main
static void create(const char *name, const char *text, mode_t mode)
{
	int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, mode); assert(fd >= 0);
	assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text)); assert(close(fd) == 0);
	assert(chmod(name, mode) == 0);
}
static void contents(const char *name, const char *text)
{
	char buffer[64]; ssize_t size; int fd = open(name, O_RDONLY); assert(fd >= 0);
	size = read(fd, buffer, sizeof(buffer)); assert(size == (ssize_t)strlen(text));
	assert(!memcmp(buffer, text, (size_t)size)); assert(close(fd) == 0);
}
int main(int argc, char **argv)
{
	char *ordinary[] = {"cp", "source", "output", NULL};
	char *strict[] = {"cp", "-T", "--update=none-fail", "source", "output", NULL};
	char *attributes[] = {"cp", "-T", "--update=none-fail", "--attributes-only", "--preserve=mode", "source", "output", NULL};
	char *preserve[] = {"cp", "--preserve=mode", "--attributes-only", "source", "output", NULL};
	struct stat st; unsigned before; mode_t mask;
	assert(argc == 2 && chdir(argv[1]) == 0);
	create("source", "source-data", 0755);
	umask(077); assert(cp_main(7, attributes) == 0);
	assert(stat("output", &st) == 0 && st.st_size == 0 && (st.st_mode & 0777) == 0755);
	mask = umask(022); assert(mask == 077);
	assert(cp_main(5, strict) == 1); contents("output", "");
	assert(unlink("output") == 0); assert(cp_main(3, ordinary) == 0);
	contents("output", "source-data");
	assert(chmod("source", 0700) == 0); assert(cp_main(5, preserve) == 0);
	contents("output", "source-data"); assert(stat("output", &st) == 0 && (st.st_mode & 0777) == 0700);
	assert(unlink("output") == 0 && link("source", "output") == 0);
	assert(cp_main(3, ordinary) == 1); contents("source", "source-data");
	assert(unlink("output") == 0 && symlink("source", "output") == 0);
	assert(cp_main(5, strict) == 1); contents("source", "source-data");
	assert(cp_main(3, ordinary) == 1); contents("source", "source-data");
	assert(unlink("output") == 0);
	copy_failure = close_failure = 1; before = close_calls;
	assert(cp_main(5, strict) == 1 && errno == ENOSPC);
	assert(close_calls == before + 2); contents("output", "s");
	copy_failure = close_failure = 0;
	assert(unlink("output") == 0); before = close_calls;
	close_failure = 1; assert(cp_main(5, strict) == 1 && errno == EIO);
	assert(close_calls == before + 2); close_failure = 0;
	assert(unlink("output") == 0 && unlink("source") == 0);
	puts("staging cp PASS exclusive aliases attributes mode close/error ownership");
	return 0;
}
