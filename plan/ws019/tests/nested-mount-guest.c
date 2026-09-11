/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check(int valid, const char *operation);

static void
check(int valid, const char *operation)
{
	if (!valid) {
		fprintf(stderr, "nested mount: %s errno=%d\n", operation, errno);
		exit(1);
	}
}

int
main(void)
{
	int fd, status;
	pid_t child;

	check(mkdir("/run/q154", 0700) == 0, "workspace");
	check(mkdir("/run/q154/target", 0700) == 0, "target");
	check(symlink("target", "/run/q154/link") == 0, "symlink");
	check(mount("tmpfs", "/run/q154/missing", 0, NULL) < 0 && errno == ENOENT, "missing target");
	check(mount("tmpfs", "/run/q154/link", 0, NULL) == 0, "symlink mount");
	check(mount("tmpfs", "/run/q154/target", 0, NULL) < 0 && errno == EBUSY, "duplicate mount");
	check(rmdir("/run/q154/target") < 0 && errno == EBUSY, "covered directory");
	fd = open("/run/q154/target/held", O_CREAT | O_RDWR, 0600);
	check(fd >= 0, "held file");
	check(unlink("/run/q154/target/held") == 0, "unlink while open");
	check(unmount("/run/q154/target", 0) < 0 && errno == EBUSY, "open file busy");
	check(close(fd) == 0, "close held file");
	fd = open("/run/q154/target/populated", O_CREAT | O_RDWR, 0600);
	check(fd >= 0, "create persistent tmpfs entry");
	check(write(fd, "populated", 9) == 9, "populate tmpfs data");
	check(close(fd) == 0, "close populated file");
	check(mkdir("/run/q154/target/child", 0700) == 0, "nested target");
	check(mount("tmpfs", "/run/q154/target/child", 0, NULL) == 0, "child mount");
	check(unmount("/run/q154/target", 0) < 0 && errno == EBUSY, "child busy");
	check(unmount("/run/q154/link/child", 0) == 0, "child alias unmount");
	check(chdir("/run/q154") == 0, "relative cwd");
	check(unmount("link", 0) == 0, "relative alias unmount");
	check(unmount("target", 0) < 0 && errno == EINVAL, "ordinary directory");
	check(mount("unknown", "target", 0, NULL) < 0, "failed mount");
	check(mount("tmpfs", "target", MNT_RDONLY, NULL) == 0, "retry read-only");
	check(open("target/new", O_CREAT | O_RDWR, 0600) < 0 && errno == EROFS, "read-only enforcement");
	check(unmount("target", 0) == 0, "retry teardown");
	child = fork();
	check(child >= 0, "fork unprivileged caller");
	if (child == 0) {
		check(setuid(1000) == 0, "drop child privilege");
		check(mount("tmpfs", "target", 0, NULL) < 0 && errno == EPERM, "unprivileged mount");
		check(unmount("target", 0) < 0 && errno == EPERM, "unprivileged unmount");
		_exit(0);
	}
	check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status");
	check(chdir("/") == 0, "restore cwd");
	check(unlink("/run/q154/link") == 0, "remove link");
	check(rmdir("/run/q154/target") == 0 && rmdir("/run/q154") == 0, "workspace cleanup");
	puts("native nested mount PASS paths busy rollback readonly privilege populated-tmpfs cleanup");
	return 0;
}
