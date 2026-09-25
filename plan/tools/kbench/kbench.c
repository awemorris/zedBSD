/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A kernel microbenchmark for comparing kernel builds (LTO or not, -Os or
 * -O2).  Each test runs a fixed number of operations and prints nanoseconds
 * per operation, measured with CLOCK_MONOTONIC.
 *
 *	kbench [file]	(the file for the read and file-fault tests; default /bin/sh)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The size of the pages the fault tests touch. */
#define KBENCH_PAGE_SIZE 4096

static void bench_syscall(void);
static void bench_pipe(void);
static void bench_fork(void);
static void bench_exec(void);
static void bench_read(const char *file);
static void bench_anon_fault(void);
static void bench_file_fault(const char *file);
static long long now_ns(void);
static void report(const char *name, long long start, long long end, long count);

/*
 * Runs every test in turn.
 *
 * The file named by the argument (default /bin/sh) is the one the read and
 * file-fault tests use.
 */
int
main(
	int argc,
	char **argv)
{
	const char *file;

	/* Chooses the file the read and file-fault tests use. */
	file = "/bin/sh";
	if (argc > 1)
		file = argv[1];

	/* Runs the tests from the cheapest operation to the most expensive. */
	bench_syscall();
	bench_pipe();
	bench_fork();
	bench_exec();
	bench_read(file);
	bench_anon_fault();
	bench_file_fault(file);

	/* Succeeded: every test has printed its line. */
	return 0;
}

/* Times a trivial system call. */
static void
bench_syscall(
	void)
{
	long long start;
	long count;
	long index;

	/* Calls getppid() many times. */
	count = 200000;
	start = now_ns();
	for (index = 0; index < count; index++)
		(void)getppid();

	/* Prints the time per call. */
	report("syscall getppid", start, now_ns(), count);
}

/* Times a byte there and back through two pipes, between two processes. */
static void
bench_pipe(
	void)
{
	int pipe_ab[2];
	int pipe_ba[2];
	long long start;
	long count;
	long index;
	pid_t child;
	int wait_status;
	char byte;

	/* Opens a pipe each way for the round trips. */
	count = 20000;
	(void)pipe(pipe_ab);
	(void)pipe(pipe_ba);

	/* Starts the child that sends every byte back. */
	child = fork();
	if (child == 0) {
		/* Echoes each byte the parent sends. */
		for (index = 0; index < count; index++) {
			(void)read(pipe_ab[0], &byte, 1);
			(void)write(pipe_ba[1], &byte, 1);
		}

		/* Leaves once every byte is back. */
		_exit(0);
	}

	/* Sends each byte and waits for it to come back. */
	start = now_ns();
	for (index = 0; index < count; index++) {
		(void)write(pipe_ab[1], "x", 1);
		(void)read(pipe_ba[0], &byte, 1);
	}

	/* Prints the time per round trip. */
	report("pipe round trip", start, now_ns(), count);

	/* Reaps the child and closes the pipes. */
	(void)waitpid(child, &wait_status, 0);
	close(pipe_ab[0]);
	close(pipe_ab[1]);
	close(pipe_ba[0]);
	close(pipe_ba[1]);
}

/* Times a fork whose child exits at once, and the wait for it. */
static void
bench_fork(
	void)
{
	long long start;
	long count;
	long index;
	pid_t child;
	int wait_status;

	/* Forks and reaps a child that does nothing. */
	count = 2000;
	start = now_ns();
	for (index = 0; index < count; index++) {
		child = fork();
		if (child == 0)
			_exit(0);

		/* Reaps the child before the next fork. */
		(void)waitpid(child, &wait_status, 0);
	}

	/* Prints the time per child. */
	report("fork+exit+wait", start, now_ns(), count);
}

/* Times a fork, an exec of /bin/true, and the wait for it. */
static void
bench_exec(
	void)
{
	long long start;
	long count;
	long index;
	pid_t child;
	int wait_status;

	/* Forks a child that runs /bin/true, and reaps it. */
	count = 300;
	start = now_ns();
	for (index = 0; index < count; index++) {
		child = fork();
		if (child == 0) {
			execl("/bin/true", "true", (char *)NULL);
			_exit(127);
		}

		/* Reaps the child before the next fork. */
		(void)waitpid(child, &wait_status, 0);
	}

	/* Prints the time per child. */
	report("fork+exec true+wait", start, now_ns(), count);
}

/* Times reading a cached file whole, 64 KiB at a time. */
static void
bench_read(
	const char *file)
{
	static char buffer[65536];
	long long start;
	long count;
	long index;
	ssize_t length;
	int descriptor;

	/* Opens and reads the file to its end many times. */
	count = 200;
	start = now_ns();
	for (index = 0; index < count; index++) {
		descriptor = open(file, O_RDONLY);

		/* Reads until the end of the file. */
		length = read(descriptor, buffer, sizeof(buffer));
		while (length > 0)
			length = read(descriptor, buffer, sizeof(buffer));

		/* Closes the file before the next pass. */
		close(descriptor);
	}

	/* Prints the time per whole read. */
	report("read file whole", start, now_ns(), count);
}

/* Times faults on fresh anonymous pages (one write per page). */
static void
bench_anon_fault(
	void)
{
	char *memory;
	long long start;
	long count;
	long index;
	long pages;
	long page;

	/* Maps, touches and unmaps a fresh region several times. */
	pages = 8192;
	count = 4;
	start = now_ns();
	for (index = 0; index < count; index++) {
		memory = mmap(NULL, (size_t)pages * KBENCH_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

		/* Writes one byte on each page. */
		for (page = 0; page < pages; page++)
			memory[page * KBENCH_PAGE_SIZE] = 1;

		/* Unmaps the region before the next pass. */
		munmap(memory, (size_t)pages * KBENCH_PAGE_SIZE);
	}

	/* Prints the time per page. */
	report("anon page fault", start, now_ns(), count * pages);
}

/* Times faults on the pages of a file mapping (one read per page). */
static void
bench_file_fault(
	const char *file)
{
	struct stat status;
	char *memory;
	volatile char sink;
	long long start;
	long count;
	long index;
	long pages;
	long page;
	int descriptor;

	/* Finds how many whole pages the file has. */
	descriptor = open(file, O_RDONLY);
	fstat(descriptor, &status);
	pages = (long)(status.st_size / KBENCH_PAGE_SIZE);

	/* Maps, reads and unmaps the file several times. */
	count = 20;
	start = now_ns();
	for (index = 0; index < count; index++) {
		memory = mmap(NULL, (size_t)pages * KBENCH_PAGE_SIZE, PROT_READ, MAP_PRIVATE, descriptor, 0);

		/* Reads one byte on each page. */
		for (page = 0; page < pages; page++)
			sink = memory[page * KBENCH_PAGE_SIZE];

		/* Unmaps the file before the next pass. */
		munmap(memory, (size_t)pages * KBENCH_PAGE_SIZE);
	}

	/* Prints the time per page. */
	report("file page fault", start, now_ns(), count * pages);

	/* Closes the file. */
	close(descriptor);
	(void)sink;
}

/* Returns the monotonic clock in nanoseconds. */
static long long
now_ns(
	void)
{
	struct timespec time;

	/* Reads the clock. */
	clock_gettime(CLOCK_MONOTONIC, &time);

	/* Combines the seconds and nanoseconds. */
	return (long long)time.tv_sec * 1000000000LL + time.tv_nsec;
}

/* Prints nanoseconds per operation. */
static void
report(
	const char *name,
	long long start,
	long long end,
	long count)
{
	printf("%-22s %10lld ns/op\n", name, (end - start) / count);
}
