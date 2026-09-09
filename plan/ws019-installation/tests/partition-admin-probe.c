/* Disposable QEMU partitions only; restore every byte written in the target.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/block.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "partition-admin-probe:%d %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)

static struct zedbsd_block_info query(int fd)
{
	struct zedbsd_block_info info = {0};
	info.version = ZEDBSD_BLOCK_VERSION; info.struct_size = sizeof(info);
	REQUIRE(ioctl(fd, BLKGETINFO, &info) == 0);
	return info;
}

int main(int argc, char **argv)
{
	struct zedbsd_block_info info, parent_info;
	unsigned char original[512], modified[512], actual[512];
	char path[256], text[80], ready;
	int fd, parent, sibling, pipes[2], status, copy;
	pid_t child;
	off_t offset;
	struct stat diagnostic;

	REQUIRE(argc == 5 && (!strcmp(argv[4], "ro") || !strcmp(argv[4], "rw")));
	if (!strcmp(argv[4], "rw")) {
		REQUIRE(snprintf(path, sizeof(path), "%s/baseline-proof", argv[3]) < (int)sizeof(path));
		status = stat(path, &diagnostic);
		printf("baseline path=%s stat=%d errno=%d mode=%o\n", path, status, errno,
		    status == 0 ? (unsigned)diagnostic.st_mode : 0);
		sibling = open(path, O_RDWR | O_CREAT | O_EXCL, 0755);
		printf("baseline create fd=%d errno=%d\n", sibling, errno);
		REQUIRE(sibling >= 0);
		REQUIRE(close(sibling) == 0);
		REQUIRE(unlink(path) == 0);
	}
	fd = open(argv[1], O_RDWR | O_NOFOLLOW); REQUIRE(fd >= 0);
	info = query(fd);
	REQUIRE(info.sector_size == 512 && (info.flags & ZEDBSD_BLOCK_PARTITION) && info.sector_count > 1);
	parent = open(argv[2], O_RDONLY); REQUIRE(parent >= 0);
	parent_info = query(parent); REQUIRE(info.parent_device == parent_info.device);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == -1 && errno == EBUSY);
	REQUIRE(close(parent) == 0);
	REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
	REQUIRE(open(argv[1], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(ioctl(fd, BLKREREADPART, 0) == -1 && errno == EINVAL);

	/* File operations on the already-mounted disjoint sibling remain usable. */
	REQUIRE(snprintf(path, sizeof(path), "%s/sentinel.txt", argv[3]) < (int)sizeof(path));
	sibling = open(path, O_RDONLY); REQUIRE(sibling >= 0);
	REQUIRE(read(sibling, text, sizeof(text)) > 0); REQUIRE(close(sibling) == 0);
	if (!strcmp(argv[4], "rw")) {
		REQUIRE(snprintf(path, sizeof(path), "%s/admin-proof", argv[3]) < (int)sizeof(path));
		status = stat(path, &diagnostic);
		printf("reserved path=%s stat=%d errno=%d mode=%o\n", path, status, errno,
		    status == 0 ? (unsigned)diagnostic.st_mode : 0);
		/* This plain FAT mount presents fixed 0755 permissions. */
		sibling = open(path, O_RDWR | O_CREAT | O_TRUNC, 0755); REQUIRE(sibling >= 0);
		REQUIRE(write(sibling, "disjoint sibling write PASS\n", 28) == 28);
		REQUIRE(fsync(sibling) == 0);
		REQUIRE(pread(sibling, text, 28, 0) == 28 && !memcmp(text, "disjoint sibling write PASS\n", 28));
		REQUIRE(close(sibling) == 0);
	}
	for (unsigned edge = 0; edge < 2; edge++) {
		offset = edge ? (off_t)((info.sector_count - 1) * 512) : 0;
		REQUIRE(pread(fd, original, 512, offset) == 512);
		memcpy(modified, original, 512); modified[13] ^= 0x69;
		REQUIRE(pwrite(fd, modified, 512, offset) == 512);
		REQUIRE(fsync(fd) == 0);
		REQUIRE(pread(fd, actual, 512, offset) == 512 && !memcmp(actual, modified, 512));
		REQUIRE(pwrite(fd, original, 512, offset) == 512);
		REQUIRE(fsync(fd) == 0);
	}
	REQUIRE(pwrite(fd, modified, 1, (off_t)(info.sector_count * 512)) == -1);
	copy = dup(fd); REQUIRE(copy >= 0); REQUIRE(close(fd) == 0);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(close(copy) == 0);

	REQUIRE(pipe(pipes) == 0);
	child = fork(); REQUIRE(child >= 0);
	if (child == 0) {
		close(pipes[0]);
		fd = open(argv[1], O_RDWR); REQUIRE(fd >= 0);
		info = query(fd); REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
		REQUIRE(write(pipes[1], "R", 1) == 1);
		for (;;) pause();
	}
	close(pipes[1]);
	REQUIRE(read(pipes[0], &ready, 1) == 1 && ready == 'R'); close(pipes[0]);
	REQUIRE(open(argv[2], O_RDONLY) == -1 && errno == EBUSY);
	REQUIRE(kill(child, SIGTERM) == 0);
	REQUIRE(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
	fd = open(argv[1], O_RDWR); REQUIRE(fd >= 0);
	info = query(fd); REQUIRE(ioctl(fd, BLKRESERVE, &info) == 0);
	REQUIRE(close(fd) == 0);
	printf("partition-admin %s PASS\n", argv[4]);
	return 0;
}
