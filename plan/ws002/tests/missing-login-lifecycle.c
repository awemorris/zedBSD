/* SPDX-License-Identifier: Zlib */
/* Test-only PID 1: exercise the actual getty/exec-failure/exit/wait lifecycle. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utmpx.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <zedbsd/system.h>

struct snapshot {
	struct system_resource_info resources;
	struct vm_statistics vm;
	uint32_t kernel_threads;
};

static int system_fd;
static int console_fd;

static void
sample(struct snapshot *out)
{
	struct process_info kernel;

	if (ioctl(system_fd, ZEDBSD_SYSTEM_GET_RESOURCES, &out->resources) != 0 ||
	    ioctl(system_fd, ZEDBSD_SYSTEM_GET_VMSTAT, &out->vm) != 0) {
		perror("LIFECYCLE snapshot");
		exit(2);
	}

	/* The first process after cursor -1 must be the kernel process. */
	memset(&kernel, 0, sizeof(kernel));
	kernel.pid = -1;
	if (ioctl(system_fd, ZEDBSD_SYSTEM_GET_PROCESS, &kernel) != 0)
		exit(2);
	if (kernel.pid != 0)
		exit(2);
	out->kernel_threads = kernel.threads;
}

static int
same(const struct snapshot *a, const struct snapshot *b)
{
	/* Compare owners, not the deliberately retained filesystem/page caches. */
	return a->resources.process == b->resources.process &&
	    a->resources.thread == b->resources.thread &&
	    a->resources.filedesc == b->resources.filedesc &&
	    a->resources.file == b->resources.file &&
	    a->resources.vmspace == b->resources.vmspace &&
	    a->vm.hal_tasks == b->vm.hal_tasks &&
	    a->vm.hal_task_stack_bytes == b->vm.hal_task_stack_bytes &&
	    a->vm.hal_spaces == b->vm.hal_spaces &&
	    a->kernel_threads == b->kernel_threads;
}

static void
report(unsigned cycle, const struct snapshot *s)
{
	printf("LIFECYCLE %u proc=%" PRIu64 " thread=%" PRIu64
	    " fd=%" PRIu64 " file=%" PRIu64 " vm=%" PRIu64
	    " task=%" PRIu64 " stack=%" PRIu64 " space=%" PRIu64 " object=%" PRIu64 "\n",
	    cycle, s->resources.process, s->resources.thread,
	    s->resources.filedesc, s->resources.file, s->resources.vmspace,
	    s->vm.hal_tasks, s->vm.hal_task_stack_bytes, s->vm.hal_spaces, s->resources.vm_object);
	fflush(stdout);
}

/* Allow one-shot bootstrap owners to retire before creating any child. */
static int
settle_bootstrap(struct snapshot *baseline)
{
	struct snapshot current;
	unsigned retry;
	unsigned stable;

	sample(baseline);
	printf("LIFECYCLE PRE-CHILD kernel threads=%" PRIu32 "\n",
	    baseline->kernel_threads);
	report(0, baseline);
	stable = 0;
	for (retry = 0; retry < 250; retry++) {
		if (usleep(20000) != 0)
			return -1;
		sample(&current);
		if (same(baseline, &current)) {
			stable++;
			if (stable == 25)
				return 0;
		} else {
			printf("LIFECYCLE PRE-CHILD TRANSITION kernel threads=%"
			    PRIu32 "\n", current.kernel_threads);
			report(0, &current);
			*baseline = current;
			stable = 0;
		}
	}
	printf("LIFECYCLE FAIL bootstrap did not settle\n");
	return -1;
}

/* Populate the getty read cache without creating any child/session. */
static void
warm_getty(void)
{
	char buffer[16384];
	int fd;
	ssize_t count;

	fd = open("/sbin/getty", O_RDONLY);
	if (fd < 0)
		exit(2);
	do {
		count = read(fd, buffer, sizeof(buffer));
	} while (count > 0 || (count < 0 && errno == EINTR));
	if (count < 0 || close(fd) != 0)
		exit(2);
}

int
main(void)
{
	struct snapshot before;
	struct snapshot after;
	pid_t child;
	pid_t waited;
	unsigned cycle;
	unsigned retry;
	int status;
	int recorded;
	struct utmpx *record;

	/* Keep one non-controlling console description throughout all samples. */
	if (getpid() != 1)
		return 2;
	console_fd = open("/dev/console", O_RDWR | O_NOCTTY);
	system_fd = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (console_fd < 0 || system_fd < 0)
		return 2;
	if (dup2(console_fd, 1) < 0 || dup2(console_fd, 2) < 0)
		return 2;
	(void)fcntl(console_fd, F_SETFD, FD_CLOEXEC);
	sample(&before);
	printf("LIFECYCLE BEFORE READ CACHE\n");
	report(0, &before);
	warm_getty();
	sample(&before);
	printf("LIFECYCLE AFTER GETTY READ CACHE\n");
	report(0, &before);
	{
		struct utmpx seed;
		memset(&seed, 0, sizeof(seed));
		seed.ut_type = LOGIN_PROCESS;
		memcpy(seed.ut_id, "cons", sizeof(seed.ut_id));
		strcpy(seed.ut_line, "console");
		if (pututxline(&seed) == NULL)
			return 2;
		endutxent();
		setutxent();
		while (getutxent() != NULL)
			;
		endutxent();
	}
	if (settle_bootstrap(&before) != 0)
		return 2;
	printf("LIFECYCLE BASELINE AFTER BOOTSTRAP AND CACHE\n");
	report(0, &before);

	for (cycle = 1; cycle <= 100; cycle++) {
		child = fork();
		if (child < 0)
			return 2;
		if (child == 0) {
			execl("/sbin/getty", "getty", "console", (char *)NULL);
			_exit(127);
		}
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 1) {
			printf("LIFECYCLE FAIL child status cycle=%u\n", cycle);
			return 2;
		}

		/* Getty records its PID only after successfully acquiring the terminal. */
		recorded = 0;
		setutxent();
		while ((record = getutxent()) != NULL) {
			if (record->ut_type == LOGIN_PROCESS && record->ut_pid == child &&
			    strncmp(record->ut_line, "console", sizeof(record->ut_line)) == 0)
				recorded = 1;
		}
		endutxent();
		if (!recorded) {
			printf("LIFECYCLE FAIL getty terminal acquisition cycle=%u\n", cycle);
			return 2;
		}

		/* Deferred task/VM retirement must finish within a bounded interval. */
		for (retry = 0; retry < 50; retry++) {
			sample(&after);
			if (same(&before, &after))
				break;
			usleep(20000);
		}
		report(cycle, &after);
		if (!same(&before, &after)) {
			printf("LIFECYCLE FAIL owners cycle=%u\n", cycle);
			return 2;
		}

	}
	printf("LIFECYCLE PASS 100 getty exec failures and owner recovery\n");
	fflush(stdout);
	for (;;)
		pause();
}
