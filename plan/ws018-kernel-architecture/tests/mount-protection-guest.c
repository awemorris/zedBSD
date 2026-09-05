/* Real-syscall q077 probe; installed only in the disposable storage test image.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;

static int expect(const char *what, int result, int expected_errno)
{
	int actual_errno = errno;
	checks++;
	if ((expected_errno == 0 && result == 0) ||
	    (expected_errno != 0 && result == -1 && actual_errno == expected_errno))
		return 0;
	printf("mount-protection FAIL %s result=%d errno=%d expected=%d\n",
	    what, result, actual_errno, expected_errno);
	return 1;
}

#define OK(label, expr) do { errno = 0; if (expect(label, (expr), 0)) return 1; } while (0)
#define ERR(label, expr, code) do { errno = 0; if (expect(label, (expr), (code))) return 1; } while (0)

int main(int argc, char **argv)
{
	const char *target;
	struct stat before, after;
	char alias[96];
	int root, fd;
	if (argc != 3 || (strcmp(argv[1], "mounted") && strcmp(argv[1], "released")))
		return 2;
	target = argv[2];
	/* Bound all mutations to the two fixture-selected root-level targets. */
	if (strcmp(target, "/q076") && strcmp(target, "/q077-virtual"))
		return 2;
	if (!strcmp(argv[1], "released")) {
		OK("rename after unmount", rename(target, "/q077-moved"));
		OK("rmdir after unmount", rmdir("/q077-moved"));
		OK("recreate after unmount", mkdir(target, 0700));
		printf("mount-protection PASS released %u\n", checks);
		return 0;
	}
	OK("mounted stat", stat(target, &before));
	OK("source mkdir", mkdir("/q077-source", 0700));
	ERR("rmdir mounted", rmdir(target), EBUSY);
	ERR("rename mounted source", rename(target, "/q077-moved"), EBUSY);
	ERR("rename mounted target", rename("/q077-source", target), EBUSY);
	root = open("/", O_RDONLY | O_DIRECTORY);
	if (root < 0) {
		perror("mount-protection root fd");
		return 1;
	}
	ERR("unlinkat mounted", unlinkat(root, target + 1, AT_REMOVEDIR), EBUSY);
	ERR("renameat mounted source", renameat(root, target + 1, root, "q077-moved"), EBUSY);
	ERR("renameat mounted target", renameat(root, "q077-source", root, target + 1), EBUSY);
	OK("root fd close", close(root));
	OK("alias symlink", symlink("/", "/q077-root"));
	if (snprintf(alias, sizeof(alias), "/q077-root%s", target) >= (int)sizeof(alias))
		return 1;
	ERR("rmdir through symlink parent", rmdir(alias), EBUSY);
	OK("remove alias", unlink("/q077-root"));
	ERR("mkdir mounted collision", mkdir(target, 0700), EEXIST);
	ERR("symlink mounted collision", symlink("/", target), EEXIST);
	ERR("mkfifo mounted collision", mkfifo(target, 0600), EEXIST);
	errno = 0;
	fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (expect("exclusive create mounted collision", fd, EEXIST)) {
		if (fd >= 0)
			(void)close(fd);
		return 1;
	}
	OK("mounted stat after denied mutations", stat(target, &after));
	if (before.st_dev != after.st_dev || before.st_ino != after.st_ino || !S_ISDIR(after.st_mode)) {
		puts("mount-protection FAIL mounted identity changed");
		return 1;
	}
	OK("ordinary rename", rename("/q077-source", "/q077-moved"));
	OK("ordinary rmdir", rmdir("/q077-moved"));
	printf("mount-protection PASS mounted %u\n", checks);
	return 0;
}
