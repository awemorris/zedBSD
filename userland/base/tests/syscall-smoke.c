#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef ZEDBSD_USER_PAGE_SIZE
#define ZEDBSD_USER_PAGE_SIZE 4096U
#endif
#define TEST_PAGE_SIZE ((size_t)ZEDBSD_USER_PAGE_SIZE)

#ifdef ZEDBSD_USER_ABI_LP64
_Static_assert(sizeof(void *) == 8, "LP64 pointer ABI");
_Static_assert(sizeof(long) == 8, "LP64 long ABI");
_Static_assert(sizeof(off_t) == 8, "LP64 off_t ABI");
_Static_assert(sizeof(time_t) == 8, "LP64 time_t ABI");
_Static_assert(LONG_MAX > INT_MAX, "LP64 LONG_MAX");
_Static_assert(ULONG_MAX > UINT_MAX, "LP64 ULONG_MAX");
#else
_Static_assert(sizeof(long) == 4, "ILP32 long ABI");
_Static_assert(LONG_MAX == INT_MAX, "ILP32 LONG_MAX");
#endif

static volatile unsigned int caught_signals;
static volatile unsigned int nested_signal_depth;
static volatile unsigned int nested_signal_max_depth;
static volatile unsigned int nested_signal_error;
static pid_t nested_signal_pid;
static volatile unsigned int siginfo_error;
static volatile unsigned int siginfo_count;
static pid_t siginfo_pid;
static uintptr_t expected_fault_address;
static volatile unsigned int sigchld_count;
static volatile int sigchld_code;
static volatile int sigchld_status;
static volatile pid_t sigchld_pid;

static void
report_stage(unsigned int stage)
{
	char message[] = "POSIX_R1_STAGE:00\n";
	message[15] = (char)('0' + stage / 10U % 10U);
	message[16] = (char)('0' + stage % 10U);
	(void)write(1, message, sizeof(message) - 1U);
}

static void
report_checkpoint(const char *name)
{
	static const char prefix[] = "POSIX_R1_CHECK:";
	(void)write(1, prefix, sizeof(prefix) - 1U);
	(void)write(1, name, strlen(name));
	(void)write(1, "\n", 1);
}

static void
catch_signal(int signo)
{
	caught_signals |= 1U << (unsigned int)signo;
}

static void
catch_nested_signal(int signo)
{
	unsigned int depth = ++nested_signal_depth;

	if (signo != SIGUSR1 || depth > 8U)
		nested_signal_error = 1;
	if (depth > nested_signal_max_depth)
		nested_signal_max_depth = depth;
	if (depth < 8U && kill(nested_signal_pid, SIGUSR1) != 0)
		nested_signal_error = 1;
	if (nested_signal_depth != depth)
		nested_signal_error = 1;
	nested_signal_depth--;
}

static void
catch_siginfo(int signo, siginfo_t *info, void *opaque_context)
{
	ucontext_t *context = opaque_context;

	if (signo != SIGUSR1 || info == NULL || context == NULL ||
	    info->si_signo != SIGUSR1 || info->si_code != SI_USER ||
	    info->si_pid != siginfo_pid || context->uc_mcontext.mc_pc == 0 ||
	    context->uc_mcontext.mc_sp == 0)
		siginfo_error = 1;
	else
		context->uc_sigmask |= 1U << (SIGUSR2 - 1);
	siginfo_count++;
}

static void
catch_fault_siginfo(int signo, siginfo_t *info, void *opaque_context)
{
	ucontext_t *context = opaque_context;
	int valid = signo == SIGBUS && info != NULL && context != NULL &&
		    info->si_signo == SIGBUS && info->si_code == BUS_ADRERR &&
		    info->si_addr == expected_fault_address &&
		    context->uc_mcontext.mc_pc != 0 &&
		    context->uc_mcontext.mc_sp != 0;

	_exit(valid ? 0 : 1);
}

static void
catch_sigchld_info(int signo, siginfo_t *info, void *opaque_context)
{
	(void)opaque_context;
	if (signo == SIGCHLD && info != NULL) {
		sigchld_code = info->si_code;
		sigchld_status = info->si_status;
		sigchld_pid = info->si_pid;
	}
	sigchld_count++;
}

static int
run_test(int argc, char **argv, char **envp)
{
	const char message[] = "BOOT_USER_SYSCALL_OK\n";
	const char child_message[] = "FORK_EXEC_PIPE_OK\n";
	unsigned char *allocation;
	char received[sizeof(child_message)];
	int descriptors[2], status;
	pid_t child;
	size_t offset;
	(void)envp;
	if (argc > 1 && strcmp(argv[1], "fork-child") == 0)
		return write(1, child_message, sizeof(child_message) - 1U) ==
			       (ssize_t)(sizeof(child_message) - 1U)
			   ? 23
			   : 24;
	allocation = malloc(256U * 1024U);
	if (allocation == NULL)
		return 2;
#ifdef ZEDBSD_USER_ABI_LP64
	if ((uintptr_t)&message <= UINT32_MAX)
		return 201;
#endif
	for (offset = 0; offset < 256U * 1024U; offset += 4096U)
		allocation[offset] = (unsigned char)(offset >> 12);
	if (allocation[63U * 4096U] != 63U) {
		free(allocation);
		return 3;
	}
	free(allocation);
	report_stage(1);
	report_checkpoint("pipe-fork-exec-begin");
	if (pipe(descriptors) != 0)
		return 4;
	if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(descriptors[0], F_GETFD) != FD_CLOEXEC)
		return 5;
	child = fork();
	if (child < 0)
		return 6;
	if (child == 0) {
		char *child_argv[] = {"/init.elf", "fork-child", NULL};
		(void)close(descriptors[0]);
		if (dup2(descriptors[1], 1) != 1)
			_exit(25);
		(void)close(descriptors[1]);
		execve(child_argv[0], child_argv, envp);
		_exit(26);
	}
	report_checkpoint("pipe-fork-exec-parent-read");
	(void)close(descriptors[1]);
	if (read(descriptors[0], received, sizeof(child_message) - 1U) !=
		(ssize_t)(sizeof(child_message) - 1U) ||
	    memcmp(received, child_message, sizeof(child_message) - 1U) != 0)
		return 7;
	report_checkpoint("pipe-fork-exec-parent-wait");
	if (close(descriptors[0]) != 0 || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 23)
		return 8;
	report_checkpoint("pipe-fork-exec-done");
	/* Only the console character device has terminal capability. */
	if (!isatty(1))
		return 251;
	{
		int system_fd = open("/dev/system", O_RDONLY);
		if (system_fd < 0)
			return 252;
		errno = 0;
		if (isatty(system_fd) != 0 || errno != ENOTTY ||
		    close(system_fd) != 0)
			return 253;
	}
	report_checkpoint("terminal-done");
	/* A bad status pointer must not consume the zombie. */
	child = fork();
	if (child < 0)
		return 239;
	if (child == 0)
		_exit(42);
	errno = 0;
	if (waitpid(child, (int *)(uintptr_t)1, 0) != -1 || errno != EFAULT ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 42)
		return 240;
	report_checkpoint("bad-wait-done");
	/* Invalid read and readv output must not advance or consume input. */
	{
		char byte = 0;
		int image = open("/init.elf", O_RDONLY);
		struct iovec vector[2];
		if (image < 0)
			return 241;
		errno = 0;
		if (read(image, (void *)(uintptr_t)1, 1) != -1 ||
		    errno != EFAULT || read(image, &byte, 1) != 1 ||
		    byte != 0x7f || lseek(image, 0, SEEK_SET) != 0)
			return 242;
		vector[0].iov_base = &byte;
		vector[0].iov_len = 1;
		vector[1].iov_base = (void *)(uintptr_t)1;
		vector[1].iov_len = 1;
		errno = 0;
		if (readv(image, vector, 2) != -1 || errno != EFAULT ||
		    read(image, &byte, 1) != 1 || byte != 0x7f ||
		    close(image) != 0)
			return 243;
	}
	report_checkpoint("bad-read-done");
	if (pipe(descriptors) != 0 || write(descriptors[1], "p", 1) != 1)
		return 244;
	errno = 0;
	if (read(descriptors[0], (void *)(uintptr_t)1, 1) != -1 ||
	    errno != EFAULT || read(descriptors[0], received, 1) != 1 ||
	    received[0] != 'p' || close(descriptors[0]) != 0 ||
	    close(descriptors[1]) != 0)
		return 245;
	report_checkpoint("pipe-efault-done");
	/* Orphans are reparented to init (PID 1), which eventually reaps them.
	 */
	if (pipe(descriptors) != 0)
		return 246;
	child = fork();
	if (child < 0)
		return 247;
	if (child == 0) {
		pid_t grandchild = fork();
		if (grandchild < 0)
			_exit(1);
		if (grandchild == 0) {
			struct timespec delay = {0, 20000000};
			char value;
			(void)close(descriptors[0]);
			(void)write(1, "SYSCALL_ORPHAN_SLEEP\n",
				    sizeof("SYSCALL_ORPHAN_SLEEP\n") - 1U);
			(void)nanosleep(&delay, NULL);
			(void)write(1, "SYSCALL_ORPHAN_WAKE\n",
				    sizeof("SYSCALL_ORPHAN_WAKE\n") - 1U);
			value = getppid() == 1 ? 'o' : 'x';
			(void)write(descriptors[1], &value, 1);
			_exit(value == 'o' ? 0 : 1);
		}
		_exit(0);
	}
	(void)close(descriptors[1]);
	report_checkpoint("orphan-parent-wait");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    read(descriptors[0], received, 1) != 1 || received[0] != 'o' ||
	    close(descriptors[0]) != 0)
		return 248;
	report_checkpoint("orphan-done");
	/* Exercise the child-exit/wait registration edge repeatedly. */
	for (offset = 0; offset < 64; offset++) {
		child = fork();
		if (child < 0)
			return 249;
		if (child == 0)
			_exit(0);
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
			return 250;
		if ((offset & 7U) == 7U)
			report_checkpoint("wait-race-progress");
	}
	report_checkpoint("wait-race-done");
	report_stage(2);
	if (signal(SIGUSR1, catch_signal) == SIG_ERR ||
	    signal(SIGUSR2, catch_signal) == SIG_ERR)
		return 9;
	if (kill(getpid(), SIGUSR1) != 0 ||
	    (caught_signals & (1U << SIGUSR1)) == 0)
		return 10;
	/* A signal may interrupt a handler; kernel and HAL frames must be LIFO.
	 */
	{
		struct sigaction nested;

		memset(&nested, 0, sizeof(nested));
		nested.sa_handler = (uint64_t)(uintptr_t)catch_nested_signal;
		nested.sa_flags = SA_NODEFER;
		nested_signal_pid = getpid();
		nested_signal_depth = 0;
		nested_signal_max_depth = 0;
		nested_signal_error = 0;
		if (sigaction(SIGUSR1, &nested, NULL) != 0 ||
		    kill(nested_signal_pid, SIGUSR1) != 0 ||
		    nested_signal_depth != 0 || nested_signal_max_depth != 8U ||
		    nested_signal_error != 0 ||
		    signal(SIGUSR1, catch_signal) == SIG_ERR)
			return 258;
	}
	/* SA_SIGINFO carries a fixed-width record and an editable signal mask.
	 */
	{
		struct sigaction action;
		sigset_t current, unblock = 1U << (SIGUSR2 - 1);

		memset(&action, 0, sizeof(action));
		action.sa_handler = (uint64_t)(uintptr_t)catch_siginfo;
		action.sa_flags = SA_SIGINFO;
		siginfo_pid = getpid();
		siginfo_error = 0;
		siginfo_count = 0;
		if (sigaction(SIGUSR1, &action, NULL) != 0 ||
		    kill(siginfo_pid, SIGUSR1) != 0 || siginfo_count != 1U ||
		    siginfo_error != 0 ||
		    sigprocmask(SIG_SETMASK, NULL, &current) != 0 ||
		    (current & unblock) == 0 ||
		    sigprocmask(SIG_UNBLOCK, &unblock, NULL) != 0 ||
		    signal(SIGUSR1, catch_signal) == SIG_ERR)
			return 260;
	}
	/* SIGCHLD identifies the child and reports its unencoded exit status.
	 */
	{
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_handler = (uint64_t)(uintptr_t)catch_sigchld_info;
		action.sa_flags = SA_SIGINFO | SA_RESTART;
		sigchld_count = 0;
		sigchld_pid = -1;
		sigchld_code = 0;
		sigchld_status = -1;
		if (sigaction(SIGCHLD, &action, NULL) != 0)
			return 262;
		child = fork();
		if (child < 0)
			return 262;
		if (child == 0)
			_exit(47);
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 47)
			return 263;
		if (sigchld_count != 1U)
			return 264;
		if (sigchld_pid != child)
			return 265;
		if (sigchld_code != CLD_EXITED)
			return 266;
		if (sigchld_status != 47)
			return 267;
		memset(&action, 0, sizeof(action));
		action.sa_handler = SIG_DFL;
		if (sigaction(SIGCHLD, &action, NULL) != 0)
			return 262;
	}
	report_stage(3);
	{
		sigset_t blocked = 1U << (SIGUSR2 - 1);
		sigset_t pending = 0;
		if (sigprocmask(SIG_BLOCK, &blocked, NULL) != 0 ||
		    kill(getpid(), SIGUSR2) != 0 || sigpending(&pending) != 0 ||
		    (pending & blocked) == 0 ||
		    (caught_signals & (1U << SIGUSR2)) != 0)
			return 11;
		if (sigprocmask(SIG_UNBLOCK, &blocked, NULL) != 0 ||
		    (caught_signals & (1U << SIGUSR2)) == 0)
			return 12;
	}
	report_stage(4);
	caught_signals &= ~(1U << SIGUSR1);
	child = fork();
	if (child < 0)
		return 13;
	if (child == 0) {
		struct timespec delay = {0, 20000000};
		(void)write(1, "POSIX_R1_CHILD_SLEEP\n", 21);
		(void)nanosleep(&delay, NULL);
		(void)write(1, "POSIX_R1_CHILD_WAKE\n", 20);
		_exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
	}
	{
		struct timespec delay = {1, 0};
		struct timespec remaining = {0, 0};
		(void)write(1, "POSIX_R1_PARENT_SLEEP\n", 22);
		errno = 0;
		if (nanosleep(&delay, &remaining) != -1 || errno != EINTR ||
		    remaining.tv_sec < 0 ||
		    (caught_signals & (1U << SIGUSR1)) == 0)
			return 14;
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return 15;
	report_stage(5);
	{
		sigset_t block = 1U << (SIGUSR1 - 1);
		sigset_t empty = 0, current = 0;
		caught_signals &= ~(1U << SIGUSR1);
		if (sigprocmask(SIG_BLOCK, &block, NULL) != 0)
			return 15;
		child = fork();
		if (child < 0)
			return 15;
		if (child == 0)
			_exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
		errno = 0;
		if (sigsuspend(&empty) != -1 || errno != EINTR ||
		    (caught_signals & (1U << SIGUSR1)) == 0 ||
		    sigprocmask(SIG_SETMASK, NULL, &current) != 0 ||
		    (current & block) == 0 ||
		    sigprocmask(SIG_UNBLOCK, &block, NULL) != 0 ||
		    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0)
			return 15;
	}
	report_stage(6);
	child = fork();
	if (child < 0)
		return 15;
	if (child == 0) {
		(void)kill(getpid(), SIGSTOP);
		_exit(0);
	}
	if (waitpid(child, &status, WUNTRACED) != child ||
	    !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP ||
	    kill(child, SIGCONT) != 0 ||
	    waitpid(child, &status, WCONTINUED) != child ||
	    !WIFCONTINUED(status) || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 15;
	/* An orphaned stopped process group receives SIGHUP followed by
	 * SIGCONT. */
	if (pipe(descriptors) != 0)
		return 259;
	child = fork();
	if (child < 0)
		return 259;
	if (child == 0) {
		pid_t member = fork();
		if (member < 0)
			_exit(1);
		if (member == 0) {
			char result;
			(void)close(descriptors[0]);
			caught_signals = 0;
			if (setpgid(0, 0) != 0 ||
			    signal(SIGHUP, catch_signal) == SIG_ERR)
				_exit(2);
			(void)kill(getpid(), SIGSTOP);
			result =
			    (caught_signals & (1U << SIGHUP)) != 0 ? 'h' : 'x';
			(void)write(descriptors[1], &result, 1);
			_exit(result == 'h' ? 0 : 3);
		}
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		if (waitpid(member, &status, WUNTRACED) != member ||
		    !WIFSTOPPED(status))
			_exit(4);
		/* Exiting reparents member and makes its process group
		 * orphaned. */
		_exit(0);
	}
	(void)close(descriptors[1]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    read(descriptors[0], received, 1) != 1 || received[0] != 'h' ||
	    close(descriptors[0]) != 0)
		return 259;
	report_stage(7);
	{
		unsigned char *mapping =
		    mmap(NULL, 3U * TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return 16;
#ifdef ZEDBSD_USER_ABI_LP64
		if ((uintptr_t)mapping <= UINT32_MAX)
			return 202;
#endif
		mapping[0] = 1;
		mapping[TEST_PAGE_SIZE] = 2;
		mapping[2U * TEST_PAGE_SIZE] = 3;
		if (mprotect(mapping + TEST_PAGE_SIZE, TEST_PAGE_SIZE,
			     PROT_READ) != 0 ||
		    msync(mapping, TEST_PAGE_SIZE, MS_SYNC) != 0 ||
		    munmap(mapping + TEST_PAGE_SIZE, TEST_PAGE_SIZE) != 0 ||
		    munmap(mapping, TEST_PAGE_SIZE) != 0 ||
		    munmap(mapping + 2U * TEST_PAGE_SIZE, TEST_PAGE_SIZE) != 0)
			return 17;
	}
	/* Normal file mappings fault with SIGBUS on a page wholly past EOF. */
	{
		struct stat image_status;
		int image = open("/init.elf", O_RDONLY);
		off_t past_eof;
		unsigned char *mapping;
		if (image < 0 || fstat(image, &image_status) != 0)
			return 254;
		past_eof = (image_status.st_size + TEST_PAGE_SIZE - 1) &
			   ~(off_t)(TEST_PAGE_SIZE - 1);
		mapping = mmap(NULL, TEST_PAGE_SIZE, PROT_READ, MAP_PRIVATE,
			       image, past_eof);
		if (mapping == MAP_FAILED || close(image) != 0)
			return 255;
		child = fork();
		if (child < 0)
			return 256;
		if (child == 0) {
			volatile unsigned char value = mapping[0];
			(void)value;
			_exit(1);
		}
		if (waitpid(child, &status, 0) != child ||
		    !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS ||
		    munmap(mapping, TEST_PAGE_SIZE) != 0)
			return 257;
	}
	/* A synchronous VM fault reports BUS_ADRERR and the exact address. */
	{
		struct stat image_status;
		struct sigaction action;
		int image = open("/init.elf", O_RDONLY);
		off_t past_eof;
		unsigned char *mapping;
		if (image < 0 || fstat(image, &image_status) != 0)
			return 261;
		past_eof = (image_status.st_size + TEST_PAGE_SIZE - 1) &
			   ~(off_t)(TEST_PAGE_SIZE - 1);
		mapping = mmap(NULL, TEST_PAGE_SIZE, PROT_READ, MAP_PRIVATE,
			       image, past_eof);
		if (mapping == MAP_FAILED || close(image) != 0)
			return 261;
		child = fork();
		if (child < 0)
			return 261;
		if (child == 0) {
			volatile unsigned char value;
			memset(&action, 0, sizeof(action));
			action.sa_handler =
			    (uint64_t)(uintptr_t)catch_fault_siginfo;
			action.sa_flags = SA_SIGINFO;
			expected_fault_address = (uintptr_t)mapping;
			if (sigaction(SIGBUS, &action, NULL) != 0)
				_exit(2);
			value = mapping[0];
			(void)value;
			_exit(3);
		}
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0 ||
		    munmap(mapping, TEST_PAGE_SIZE) != 0)
			return 261;
	}
	report_stage(8);
	{
		struct stat status;
		int root = open("/", O_RDONLY | O_DIRECTORY);
		int image;
		if (root < 0)
			return 18;
		image = openat(root, "init.elf", O_RDONLY);
		if (image < 0 || fstatat(root, "init.elf", &status, 0) != 0 ||
		    !S_ISREG(status.st_mode) || status.st_size <= 0 ||
		    status.st_blksize != 512 || close(image) != 0 ||
		    close(root) != 0)
			return 19;
	}
	report_stage(9);
	{
		char byte = 0;
		int file, root, directory;
		root = open("/", O_RDONLY | O_DIRECTORY);
		if (root < 0 || mkdirat(root, "r1", 0777) != 0)
			return 20;
		directory = openat(root, "r1", O_RDONLY | O_DIRECTORY);
		file = openat(directory, "a", O_CREAT | O_RDWR | O_EXCL, 0666);
		if (file < 0 || write(file, "z", 1) != 1)
			return 21;
		{
			char persisted = 0;
			int read_only = openat(directory, "a", O_RDONLY);
			unsigned char *read_only_shared;
			unsigned char *shared1;
			unsigned char *shared2;
			unsigned char *private_map;
			if (read_only < 0)
				return 21;
			read_only_shared = mmap(NULL, TEST_PAGE_SIZE, PROT_READ,
						MAP_SHARED, read_only, 0);
			shared1 =
			    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
				 MAP_SHARED, file, 0);
			shared2 =
			    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
				 MAP_SHARED, file, 0);
			if (close(read_only) != 0 ||
			    read_only_shared == MAP_FAILED ||
			    shared1 == MAP_FAILED || shared2 == MAP_FAILED)
				return 21;
			shared1[0] = 'm';
			if (shared2[0] != 'm' || read_only_shared[0] != 'm')
				return 21;
			child = fork();
			if (child < 0)
				return 21;
			if (child == 0) {
				shared2[0] = 'c';
				_exit(msync(shared2, TEST_PAGE_SIZE, MS_SYNC) ==
					      0
					  ? 0
					  : 1);
			}
			if (waitpid(child, &status, 0) != child ||
			    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
				return 211;
			if (shared1[0] != 'c')
				return 215;
			if (msync(shared1, TEST_PAGE_SIZE, MS_SYNC) != 0)
				return 216;
			if (pread(file, &persisted, 1, 0) != 1)
				return 217;
			if (persisted != 'c')
				return 218;
			if (munmap(read_only_shared, TEST_PAGE_SIZE) != 0 ||
			    munmap(shared1, TEST_PAGE_SIZE) != 0 ||
			    munmap(shared2, TEST_PAGE_SIZE) != 0)
				return 219;
			private_map =
			    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE, file, 0);
			if (private_map == MAP_FAILED)
				return 212;
			private_map[0] = 'p';
			child = fork();
			if (child < 0)
				return 21;
			if (child == 0) {
				private_map[0] = 'q';
				_exit(private_map[0] == 'q' ? 0 : 1);
			}
			if (waitpid(child, &status, 0) != child ||
			    !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
			    private_map[0] != 'p')
				return 213;
			persisted = 0;
			if (msync(private_map, TEST_PAGE_SIZE, MS_SYNC) != 0 ||
			    pread(file, &persisted, 1, 0) != 1 ||
			    persisted != 'c' ||
			    munmap(private_map, TEST_PAGE_SIZE) != 0)
				return 214;
		}
		{
			struct timespec times[2] = {
#ifdef ZEDBSD_USER_ABI_LP64
			    {(time_t)2208988800LL, 123456789},
			    {(time_t)2208988802LL, 987654321},
#else
			    {1767225600, 123456789},
			    {1767225602, 987654321},
#endif
			};
			struct stat attributes;
			if (futimens(file, times) != 0)
				return 231;
			if (fstat(file, &attributes) != 0)
				return 232;
			if (attributes.st_atime != times[0].tv_sec ||
			    attributes.st_mtime != times[1].tv_sec)
				return 233;
			if (fchmod(file, 0555) != 0)
				return 234;
			if (fstat(file, &attributes) != 0)
				return 235;
			if ((attributes.st_mode & 0777U) != 0555U)
				return 236;
			if (fchown(file, getuid(), getgid()) != 0)
				return 237;
			errno = 0;
			if (fchmod(file, 0644) != -1 || errno != EOPNOTSUPP)
				return 238;
		}
#ifdef ZEDBSD_USER_ABI_LP64
		if (lseek(file, (off_t)UINT32_MAX + 1, SEEK_SET) !=
		    (off_t)UINT32_MAX + 1)
			return 229;
		errno = 0;
		if (write(file, "x", 1) != -1 || errno != EFBIG ||
		    lseek(file, 0, SEEK_SET) != 0)
			return 230;
#endif
		if (renameat(directory, "a", directory, "b") != 0 ||
		    close(file) != 0)
			return 21;
		errno = 0;
		if (linkat(directory, "b", directory, "hard", 0) != -1 ||
		    errno != EOPNOTSUPP)
			return 21;
		errno = 0;
		if (symlinkat("b", directory, "soft") != -1 ||
		    errno != EOPNOTSUPP)
			return 21;
		file = openat(directory, "b", O_RDWR);
		if (file < 0)
			return 221;
		if (unlinkat(directory, "b", 0) != 0)
			return 222;
		if (lseek(file, 0, SEEK_SET) != 0)
			return 226;
		if (read(file, &byte, 1) != 1)
			return 227;
		if (byte != 'c')
			return 228;
		if (close(file) != 0 || close(directory) != 0)
			return 224;
		if (unlinkat(root, "r1", AT_REMOVEDIR) != 0 || close(root) != 0)
			return 225;
	}
	report_stage(10);
	return write(1, message, strlen(message)) ==
		       (ssize_t)(sizeof(message) - 1U)
		   ? 37
		   : 1;
}

int
main(int argc, char **argv, char **envp)
{
	int result;

	if (argc <= 1)
		(void)write(1, "POSIX_R1_BEGIN\n", 15);
	result = run_test(argc, argv, envp);
	if (argc <= 1 && result != 37) {
		char failure[] = "POSIX_R1_FAIL:000\n";
		unsigned int value = result < 0 ? 999U : (unsigned int)result;
		failure[14] = (char)('0' + value / 100U % 10U);
		failure[15] = (char)('0' + value / 10U % 10U);
		failure[16] = (char)('0' + value % 10U);
		(void)write(1, failure, sizeof(failure) - 1U);
	}
	return result;
}
