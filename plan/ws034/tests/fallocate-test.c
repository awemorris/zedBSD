/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * ws034-p053: mkostemp, mkostemps, posix_fallocate, and fstat of a pipe and
 * a socket on the guest.
 * Prints "FALLOC ok|FAIL <case>" and "FALLOC DONE n/m".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed, total;

static void
check(int ok, const char *name)
{
	total++;
	passed += ok != 0;
	printf("FALLOC %s %s (errno=%d)\n", ok ? "ok" : "FAIL", name, errno);
}

int
main(void)
{
	char plain[] = "/tmp/fallocXXXXXX";
	char suffixed[] = "/tmp/fallocXXXXXX.txt";
	char bad[] = "/tmp/fallocXXXX";
	struct stat status;
	int pipe_fd[2];
	int fd, fd2;

	fd = mkostemp(plain, O_CLOEXEC);
	check(fd >= 0 && strncmp(plain, "/tmp/falloc", 11) == 0 &&
	    strcmp(plain + 11, "XXXXXX") != 0, "mkostemp opens a new name");
	check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "mkostemp O_CLOEXEC");
	fd2 = mkostemps(suffixed, 4, 0);
	check(fd2 >= 0 && strcmp(suffixed + strlen(suffixed) - 4, ".txt") == 0 &&
	    (fcntl(fd2, F_GETFD) & FD_CLOEXEC) == 0, "mkostemps keeps the suffix");
	errno = 0;
	check(mkostemp(bad, 0) == -1 && errno == EINVAL, "short template EINVAL");
	errno = 0;
	check(mkostemp(plain, O_TRUNC) == -1 && errno == EINVAL, "bad flag EINVAL");

	check(posix_fallocate(fd, 0, 100000) == 0, "fallocate 100000");
	check(fstat(fd, &status) == 0 && status.st_size == 100000, "size grew");
	check(status.st_blocks * 512 >= 100000 - 4096, "blocks allocated");
	check(posix_fallocate(fd, 10, 100) == 0 && fstat(fd, &status) == 0 &&
	    status.st_size == 100000, "inside range leaves size");
	check(posix_fallocate(fd, 200000, 1) == 0 && fstat(fd, &status) == 0 &&
	    status.st_size == 200001, "beyond end extends to offset+len");
	check(posix_fallocate(fd, -1, 10) == EINVAL, "negative offset EINVAL");
	check(posix_fallocate(fd, 0, 0) == EINVAL, "zero length EINVAL");
	check(posix_fallocate(-1, 0, 10) == EBADF, "bad fd EBADF");
	check(pipe(pipe_fd) == 0 && posix_fallocate(pipe_fd[0], 0, 10) == ESPIPE,
	    "pipe ESPIPE");

	/* fstat of a pipe and a socket, which have no inode. */
	check(fstat(pipe_fd[0], &status) == 0 && S_ISFIFO(status.st_mode),
	    "fstat pipe S_IFIFO");
	fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
	check(fd2 >= 0 && fstat(fd2, &status) == 0 && S_ISSOCK(status.st_mode),
	    "fstat socket S_IFSOCK");

	unlink(plain);
	unlink(suffixed);
	printf("FALLOC DONE %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
