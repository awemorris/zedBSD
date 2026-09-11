/* Test-only observer: production sh collapses status and resets it each line.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int status;
	pid_t child, waited;
	if (argc < 2)
		return 125;
	child = fork();
	if (child < 0)
		return 125;
	if (child == 0) {
		execvp(argv[1], argv + 1);
		perror("storage-exit exec");
		_exit(127);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child || !WIFEXITED(status)) {
		puts("storage-result-abnormal");
		return 125;
	}
	printf("storage-result-%d\n", WEXITSTATUS(status));
	return 0;
}
