/* Disposable QEMU media only: preserves one sector in the pre-partition gap.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/block.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "block-admin-probe:%d %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)

static struct zedbsd_block_info query(int fd)
{
	struct zedbsd_block_info info = {0};
	info.version = ZEDBSD_BLOCK_VERSION;
	info.struct_size = sizeof(info);
	REQUIRE(ioctl(fd, BLKGETINFO, &info) == 0);
	return info;
}

int main(int argc, char **argv)
{
	struct zedbsd_block_info info, changed;
	unsigned char original[512], modified[512], actual[512];
	int fd, other, copy, pipes[2], status;
	pid_t child;
	char ready;

	REQUIRE(argc == 4);
	fd = open(argv[2], O_RDWR | O_NOFOLLOW);
	REQUIRE(fd >= 0);
	info = query(fd);
	if (!strcmp(argv[1], "busy")) {
		REQUIRE(ioctl(fd, BLKRESERVE, &info) == -1 && errno == EBUSY);
		REQUIRE(close(fd) == 0);
		puts("block-admin busy PASS");
		return 0;
	}
	REQUIRE(!strcmp(argv[1], "exercise"));
	/* Guard the fixture layout: 512-byte sectors and first partition at 1 MiB. */
	REQUIRE(info.sector_size == 512 && !(info.flags & ZEDBSD_BLOCK_PARTITION));
	other = open(argv[3], O_RDONLY | O_NOFOLLOW); REQUIRE(other >= 0);
	changed = query(other); REQUIRE(changed.parent_offset == 2048);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == -1 && errno == EBUSY);
	REQUIRE(close(other) == 0);
	other = open(argv[2], O_RDONLY); REQUIRE(other >= 0);
	REQUIRE(ioctl(other, BLKRESERVE, &info) == -1 && errno == EBADF);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == -1 && errno == EBUSY);
	REQUIRE(close(other) == 0);
	changed = info; changed.device++;
	REQUIRE(ioctl(fd, BLKRESERVE, &changed) == -1 && errno == ESTALE);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == -1 && errno == EBUSY);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(open(argv[3], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(pread(fd, original, 512, 51200) == 512);
	memcpy(modified, original, 512); modified[13] ^= 0x5a;
	REQUIRE(pwrite(fd, modified, 512, 51200) == 512);
	REQUIRE(fsync(fd) == 0);
	REQUIRE(pread(fd, actual, 512, 51200) == 512 && !memcmp(actual, modified, 512));
	REQUIRE(pwrite(fd, original, 512, 51200) == 512);
	REQUIRE(fsync(fd) == 0);
	REQUIRE(ioctl(fd, BLKREREADPART, 0) == 0);
	REQUIRE(pread(fd, actual, 512, 51200) == 512 && !memcmp(actual, original, 512));
	copy = dup(fd); REQUIRE(copy >= 0);
	REQUIRE(close(fd) == 0);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(close(copy) == 0);
	other = open(argv[3], O_RDONLY); REQUIRE(other >= 0); REQUIRE(close(other) == 0);

	/* The child acquires independently, then dies without an explicit close. */
	REQUIRE(pipe(pipes) == 0);
	child = fork(); REQUIRE(child >= 0);
	if (child == 0) {
		close(pipes[0]);
		fd = open(argv[2], O_RDWR | O_NOFOLLOW); REQUIRE(fd >= 0);
		info = query(fd); REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
		REQUIRE(write(pipes[1], "R", 1) == 1);
		for (;;) pause();
	}
	close(pipes[1]);
	REQUIRE(read(pipes[0], &ready, 1) == 1 && ready == 'R');
	REQUIRE(close(pipes[0]) == 0);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(kill(child, SIGTERM) == 0);
	REQUIRE(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
	fd = open(argv[2], O_RDWR | O_NOFOLLOW); REQUIRE(fd >= 0);
	info = query(fd); REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
	REQUIRE(close(fd) == 0);
	puts("block-admin exercise PASS");
	return 0;
}
