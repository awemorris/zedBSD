#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile unsigned int caught_signals;

static void
report_stage(unsigned int stage)
{
	char message[] = "POSIX_R1_STAGE:00\n";
	message[15] = (char)('0' + stage / 10U % 10U);
	message[16] = (char)('0' + stage % 10U);
	(void)write(1, message, sizeof(message) - 1U);
}

static void
catch_signal(int signo)
{
	caught_signals |= 1U << (unsigned int)signo;
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
		    (ssize_t)(sizeof(child_message) - 1U) ? 23 : 24;
	allocation = malloc(256U * 1024U);
	if (allocation == NULL)
		return 2;
	for (offset = 0; offset < 256U * 1024U; offset += 4096U)
		allocation[offset] = (unsigned char)(offset >> 12);
	if (allocation[63U * 4096U] != 63U) {
		free(allocation);
		return 3;
	}
	free(allocation);
	report_stage(1);
	if (pipe(descriptors) != 0)
		return 4;
	if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(descriptors[0], F_GETFD) != FD_CLOEXEC)
		return 5;
	child = fork();
	if (child < 0)
		return 6;
	if (child == 0) {
		char *child_argv[] = { "/init.elf", "fork-child", NULL };
		(void)close(descriptors[0]);
		if (dup2(descriptors[1], 1) != 1)
			_exit(25);
		(void)close(descriptors[1]);
		execve(child_argv[0], child_argv, envp);
		_exit(26);
	}
	(void)close(descriptors[1]);
	if (read(descriptors[0], received, sizeof(child_message) - 1U) !=
	    (ssize_t)(sizeof(child_message) - 1U) ||
	    memcmp(received, child_message, sizeof(child_message) - 1U) != 0)
		return 7;
	if (close(descriptors[0]) != 0 || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 23)
		return 8;
	report_stage(2);
	if (signal(SIGUSR1, catch_signal) == SIG_ERR ||
	    signal(SIGUSR2, catch_signal) == SIG_ERR)
		return 9;
	if (kill(getpid(), SIGUSR1) != 0 ||
	    (caught_signals & (1U << SIGUSR1)) == 0)
		return 10;
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
		struct timespec delay = { 0, 20000000 };
		(void)write(1, "POSIX_R1_CHILD_SLEEP\n", 21);
		(void)nanosleep(&delay, NULL);
		(void)write(1, "POSIX_R1_CHILD_WAKE\n", 20);
		_exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
	}
	{
		struct timespec delay = { 1, 0 };
		struct timespec remaining = { 0, 0 };
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
	report_stage(7);
	{
		unsigned char *mapping = mmap(NULL, 3U * 4096U,
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return 16;
		mapping[0] = 1;
		mapping[4096U] = 2;
		mapping[8192U] = 3;
		if (mprotect(mapping + 4096U, 4096U, PROT_READ) != 0 ||
		    msync(mapping, 4096U, MS_SYNC) != 0 ||
		    munmap(mapping + 4096U, 4096U) != 0 ||
		    munmap(mapping, 4096U) != 0 ||
		    munmap(mapping + 8192U, 4096U) != 0)
			return 17;
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
			unsigned char *shared1 = mmap(NULL, 4096,
			    PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
			unsigned char *shared2 = mmap(NULL, 4096,
			    PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
			unsigned char *private_map;
			if (shared1 == MAP_FAILED || shared2 == MAP_FAILED)
				return 21;
			shared1[0] = 'm';
			if (shared2[0] != 'm')
				return 21;
			child = fork();
			if (child < 0)
				return 21;
			if (child == 0) {
				shared2[0] = 'c';
				_exit(msync(shared2, 4096, MS_SYNC) == 0 ? 0 : 1);
			}
			if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
			    WEXITSTATUS(status) != 0 || shared1[0] != 'c' ||
			    msync(shared1, 4096, MS_SYNC) != 0 ||
			    pread(file, &persisted, 1, 0) != 1 || persisted != 'c' ||
			    munmap(shared1, 4096) != 0 || munmap(shared2, 4096) != 0)
				return 21;
			private_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE, file, 0);
			if (private_map == MAP_FAILED)
				return 21;
			private_map[0] = 'p';
			child = fork();
			if (child < 0)
				return 21;
			if (child == 0) {
				private_map[0] = 'q';
				_exit(private_map[0] == 'q' ? 0 : 1);
			}
			if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
			    WEXITSTATUS(status) != 0 || private_map[0] != 'p')
				return 21;
			persisted = 0;
			if (msync(private_map, 4096, MS_SYNC) != 0 ||
			    pread(file, &persisted, 1, 0) != 1 || persisted != 'c' ||
			    munmap(private_map, 4096) != 0)
				return 21;
		}
		{
			struct timespec times[2] = {
				{ 1767225600, 123456789 },
				{ 1767225602, 987654321 },
			};
			struct stat attributes;
			if (futimens(file, times) != 0 || fstat(file, &attributes) != 0 ||
			    attributes.st_atime != times[0].tv_sec ||
			    attributes.st_mtime != times[1].tv_sec ||
			    fchmod(file, 0555) != 0 || fstat(file, &attributes) != 0 ||
			    (attributes.st_mode & 0777U) != 0555U ||
			    fchown(file, getuid(), getgid()) != 0)
				return 21;
			errno = 0;
			if (fchmod(file, 0644) != -1 || errno != EOPNOTSUPP)
				return 21;
		}
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
		(ssize_t)(sizeof(message) - 1U) ? 37 : 1;
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
