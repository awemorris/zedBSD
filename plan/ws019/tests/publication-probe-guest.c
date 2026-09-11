/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Target syscall and namespace acceptance on the selected disposable mount. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "publication FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); exit(1); } } while (0)
static int directory;
static void create(const char *name, const char *data)
{
	int fd = openat(directory, name, O_CREAT | O_EXCL | O_WRONLY, 0755);
	REQUIRE(fd >= 0);
	REQUIRE(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
	REQUIRE(fsync(fd) == 0); REQUIRE(close(fd) == 0);
}
static void contents(const char *name, const char *data)
{
	char buffer[32];
	int fd = openat(directory, name, O_RDONLY);
	ssize_t size;
	REQUIRE(fd >= 0); size = read(fd, buffer, sizeof(buffer));
	REQUIRE(size == (ssize_t)strlen(data));
	REQUIRE(memcmp(buffer, data, (size_t)size) == 0); REQUIRE(close(fd) == 0);
}
struct job { const char *source; int result, error; };
static void *publish(void *argument)
{
	struct job *job = argument;
	job->result = renameat2(directory, job->source, directory, "winner", RENAME_NOREPLACE);
	job->error = errno;
	return NULL;
}
static void command_race(const char *path)
{
	char left[1024], right[1024], target[1024];
	int gate[2], status[2];
	pid_t children[2];
	unsigned i;
	char byte;

	REQUIRE(snprintf(left, sizeof(left), "%s/left", path) < (int)sizeof(left));
	REQUIRE(snprintf(right, sizeof(right), "%s/right", path) < (int)sizeof(right));
	REQUIRE(snprintf(target, sizeof(target), "%s/winner", path) < (int)sizeof(target));
	create("left", "left"); create("right", "right");
	REQUIRE(pipe(gate) == 0);
	for (i = 0; i < 2; i++) {
		children[i] = fork(); REQUIRE(children[i] >= 0);
		if (children[i] == 0) {
			close(gate[1]);
			if (read(gate[0], &byte, 1) != 1) _exit(3);
			close(gate[0]); close(directory);
			execl("/bin/mv", "mv", "-T", "--update=none-fail", "--",
			    i == 0 ? left : right, target, (char *)NULL);
			_exit(4);
		}
	}
	REQUIRE(close(gate[0]) == 0);
	REQUIRE(write(gate[1], "go", 2) == 2); REQUIRE(close(gate[1]) == 0);
	for (i = 0; i < 2; i++) {
		REQUIRE(waitpid(children[i], &status[i], 0) == children[i]);
		REQUIRE(WIFEXITED(status[i])); status[i] = WEXITSTATUS(status[i]);
	}
	REQUIRE((status[0] == 0 && status[1] == 1) || (status[1] == 0 && status[0] == 1));
	contents("winner", status[0] == 0 ? "left" : "right");
	REQUIRE(unlinkat(directory, status[0] == 0 ? "right" : "left", 0) == 0);
	REQUIRE(unlinkat(directory, "winner", 0) == 0);
}
int main(int argc, char **argv)
{
	struct job jobs[2];
	pthread_t threads[2];
	struct stat st;
	unsigned n;
	int fd;
	REQUIRE(argc == 3);
	umask(0);
	directory = open(argv[1], O_RDONLY | O_DIRECTORY);
	REQUIRE(directory >= 0);
	if (!strcmp(argv[2], "verify")) {
		contents("persist", "durable publication\n");
		REQUIRE(close(directory) == 0);
		puts("publication persisted PASS"); return 0;
	}
	create("first", "first"); create("second", "second");
	REQUIRE(renameat2(directory, "first", directory, "second", RENAME_NOREPLACE) == -1 && errno == EEXIST);
	REQUIRE(renameat2(directory, "first", directory, "first", RENAME_NOREPLACE) == -1 && errno == EEXIST);
	REQUIRE(renameat2(directory, "first", directory, "unused", 2) == -1 && errno == EINVAL);
	REQUIRE(renameat2(directory, "first", directory, "first", 0) == 0);
	contents("first", "first"); contents("second", "second");
	if (!strcmp(argv[2], "fat")) {
		REQUIRE(renameat2(directory, "first", directory, "SECOND", RENAME_NOREPLACE) == -1 && errno == EEXIST);
	} else if (!strcmp(argv[2], "ufs")) {
		REQUIRE(linkat(directory, "second", directory, "alias", 0) == 0);
		REQUIRE(renameat2(directory, "second", directory, "alias", RENAME_NOREPLACE) == -1 && errno == EEXIST);
		REQUIRE(unlinkat(directory, "alias", 0) == 0);
	}
	REQUIRE(mkdirat(directory, "dir", 0755) == 0);
	REQUIRE(renameat2(directory, "first", directory, "dir", RENAME_NOREPLACE) == -1 && errno == EEXIST);
	REQUIRE(unlinkat(directory, "dir", AT_REMOVEDIR) == 0);
	REQUIRE(renameat(directory, "first", directory, "second") == 0);
	contents("second", "first");
	REQUIRE(unlinkat(directory, "second", 0) == 0);
	REQUIRE(renameat2(-1, "absent", directory, "x", RENAME_NOREPLACE) == -1 && errno == EBADF);
	for (n = 0; n < 20; n++) {
		create("left", "left"); create("right", "right");
		jobs[0].source = "left"; jobs[1].source = "right";
		REQUIRE(pthread_create(&threads[0], NULL, publish, &jobs[0]) == 0);
		REQUIRE(pthread_create(&threads[1], NULL, publish, &jobs[1]) == 0);
		REQUIRE(pthread_join(threads[0], NULL) == 0);
		REQUIRE(pthread_join(threads[1], NULL) == 0);
		REQUIRE((jobs[0].result == 0 && jobs[1].result == -1 && jobs[1].error == EEXIST) ||
		    (jobs[1].result == 0 && jobs[0].result == -1 && jobs[0].error == EEXIST));
		contents("winner", jobs[0].result == 0 ? "left" : "right");
		REQUIRE(unlinkat(directory, jobs[0].result == 0 ? "right" : "left", 0) == 0);
		REQUIRE(unlinkat(directory, "winner", 0) == 0);
	}
	command_race(argv[1]);
	create("staged", "durable publication\n");
	REQUIRE(renameat2(directory, "staged", directory, "persist", RENAME_NOREPLACE) == 0);
	REQUIRE(fsync(directory) == 0);
	fd = openat(directory, "persist", O_RDONLY); REQUIRE(fd >= 0);
	REQUIRE(fstat(fd, &st) == 0 && st.st_size == 20); REQUIRE(close(fd) == 0);
	REQUIRE(close(directory) == 0);
	puts("publication target PASS races=20 competing-mv legacy-rename noreplace directory-fsync");
	return 0;
}
