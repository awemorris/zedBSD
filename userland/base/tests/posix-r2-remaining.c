/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <aio.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int
failure(const char *name)
{
	char digits[12];
	unsigned value = (unsigned)errno, count = 0;
	(void)write(2, "POSIX_R2R_FAIL: ", 17);
	(void)write(2, name, strlen(name));
	(void)write(2, " errno=", 7);
	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && count < sizeof(digits));
	while (count != 0)
		(void)write(2, &digits[--count], 1);
	(void)write(2, "\n", 1);
	return -1;
}

static void
marker(const char *text)
{
	(void)write(1, text, strlen(text));
}

static int
test_tmpfs(void)
{
	struct stat status;
	char value[8];
	int fd;
	(void)unlink("/tmp/r2r-link");
	(void)unlink("/tmp/r2r-renamed");
	(void)unlink("/tmp/r2r-symlink");
	(void)unlink("/tmp/r2r-file");
	fd = open("/tmp/r2r-file", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || lseek(fd, 8192, SEEK_SET) != 8192 ||
	    write(fd, "x", 1) != 1 || fstat(fd, &status) != 0 ||
	    status.st_size != 8193 || close(fd) != 0 ||
	    link("/tmp/r2r-file", "/tmp/r2r-link") != 0 ||
	    rename("/tmp/r2r-link", "/tmp/r2r-renamed") != 0 ||
	    symlink("r2r-renamed", "/tmp/r2r-symlink") != 0 ||
	    readlink("/tmp/r2r-symlink", value, sizeof(value)) != 8 ||
	    memcmp(value, "r2r-rena", 8) != 0)
		return failure("tmpfs");
	(void)unlink("/tmp/r2r-symlink");
	(void)unlink("/tmp/r2r-renamed");
	(void)unlink("/tmp/r2r-file");
	marker("R2R:01:TMPFS\n");
	return 0;
}

static int
test_unix_vfs(void)
{
	struct sockaddr_un address;
	int listener = -1, client = -1, accepted = -1, stale = -1;
	char byte;
	(void)unlink("/tmp/r2r.sock");
	(void)unlink("/tmp/r2r-renamed.sock");
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, "/tmp/r2r.sock");
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	client = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0 || client < 0 || bind(listener,
	    (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 1) != 0 || rename("/tmp/r2r.sock",
	    "/tmp/r2r-renamed.sock") != 0)
		goto fail;
	strcpy(address.sun_path, "/tmp/r2r-renamed.sock");
	if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    (accepted = accept(listener, NULL, NULL)) < 0 ||
	    unlink(address.sun_path) != 0 || write(client, "u", 1) != 1 ||
	    read(accepted, &byte, 1) != 1 || byte != 'u')
		goto fail;
	(void)close(accepted); (void)close(client); (void)close(listener);
	/* Closing does not unlink; the surviving inode is deliberately stale. */
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	stale = socket(AF_UNIX, SOCK_STREAM, 0);
	strcpy(address.sun_path, "/tmp/r2r.sock");
	if (listener < 0 || stale < 0 || bind(listener,
	    (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    close(listener) != 0)
		goto fail;
	listener = -1;
	if (connect(stale, (struct sockaddr *)&address, sizeof(address)) != -1 ||
	    errno != ECONNREFUSED || unlink(address.sun_path) != 0)
		goto fail;
	(void)close(stale);
	marker("R2R:02:UNIX-VFS\n");
	return 0;
fail:
	if (accepted >= 0) (void)close(accepted);
	if (client >= 0) (void)close(client);
	if (listener >= 0) (void)close(listener);
	if (stale >= 0) (void)close(stale);
	(void)unlink("/tmp/r2r.sock");
	(void)unlink("/tmp/r2r-renamed.sock");
	return failure("unix-vfs");
}

static ssize_t
send_fd(int socket_fd, int passed_fd, char byte)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *header;
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = &byte; vector.iov_len = 1;
	message.msg_iov = &vector; message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	header = (struct cmsghdr *)control.bytes;
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
	return sendmsg(socket_fd, &message, 0);
}

static ssize_t
receive_fd(int socket_fd, int *received, char *byte, void *name,
	socklen_t name_length)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *header;
	ssize_t result;
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = byte; vector.iov_len = 1;
	message.msg_iov = &vector; message.msg_iovlen = 1;
	message.msg_name = name; message.msg_namelen = name_length;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	result = recvmsg(socket_fd, &message, 0);
	if (result < 0)
		return result;
	header = message.msg_controllen >= CMSG_LEN(sizeof(int)) ?
	    (struct cmsghdr *)control.bytes : NULL;
	if (header == NULL || header->cmsg_level != SOL_SOCKET ||
	    header->cmsg_type != SCM_RIGHTS ||
	    header->cmsg_len < CMSG_LEN(sizeof(int))) {
		errno = EINVAL;
		return -1;
	}
	memcpy(received, CMSG_DATA(header), sizeof(*received));
	return result;
}

static int
test_scm_rights(void)
{
	int pair[2], data[2], fillers[32], fill_count = 0, received = -1;
	char byte = 0;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pipe(data) != 0 ||
	    send_fd(pair[0], data[1], 'm') != 1)
		return failure("scm-setup");
	while (fill_count < 32 && (fillers[fill_count] =
	    open("/tmp", O_RDONLY | O_DIRECTORY)) >= 0)
		fill_count++;
	if (errno != EMFILE || receive_fd(pair[1], &received, &byte,
	    NULL, 0) != -1 || errno != EMFILE || fill_count == 0)
		return failure("scm-emfile");
	(void)close(fillers[--fill_count]);
	if (receive_fd(pair[1], &received, &byte, NULL, 0) != 1 || byte != 'm' ||
	    write(received, "r", 1) != 1 || read(data[0], &byte, 1) != 1 ||
	    byte != 'r')
		return failure("scm-retry");
	(void)close(received);
	while (fill_count != 0) (void)close(fillers[--fill_count]);
	(void)close(data[0]); (void)close(data[1]);
	(void)close(pair[0]); (void)close(pair[1]);
	/* A copyout fault must also leave a datagram and its rights queued. */
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 || pipe(data) != 0 ||
	    send_fd(pair[0], data[1], 'e') != 1 ||
	    receive_fd(pair[1], &received, &byte, (void *)(uintptr_t)1,
	    sizeof(struct sockaddr_un)) != -1 || errno != EFAULT ||
	    receive_fd(pair[1], &received, &byte, NULL, 0) != 1 || byte != 'e')
		return failure("scm-efault");
	(void)close(received); (void)close(data[0]); (void)close(data[1]);
	(void)close(pair[0]); (void)close(pair[1]);
	marker("R2R:03:SCM-RIGHTS\n");
	return 0;
}

static int
test_fifo(void)
{
	int reader, writer;
	char byte;
	(void)unlink("/tmp/r2r-fifo");
	if (mkfifo("/tmp/r2r-fifo", 0600) != 0 ||
	    (reader = open("/tmp/r2r-fifo", O_RDONLY | O_NONBLOCK)) < 0 ||
	    (writer = open("/tmp/r2r-fifo", O_WRONLY | O_NONBLOCK)) < 0 ||
	    write(writer, "f", 1) != 1 || read(reader, &byte, 1) != 1 ||
	    byte != 'f' || unlink("/tmp/r2r-fifo") != 0 ||
	    write(writer, "g", 1) != 1 || read(reader, &byte, 1) != 1 ||
	    byte != 'g')
		return failure("fifo");
	(void)close(reader); (void)close(writer);
	marker("R2R:04:FIFO\n");
	return 0;
}

static int
test_record_lock(void)
{
	struct flock lock;
	pid_t child;
	int fd, status;
	fd = open("/tmp/r2r-lock", O_CREAT | O_RDWR | O_TRUNC, 0600);
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_len = 1;
	if (fd < 0 || fcntl(fd, F_SETLK, &lock) != 0 || (child = fork()) < 0)
		return failure("record-lock-setup");
	if (child == 0) {
		int result = fcntl(fd, F_SETLK, &lock);
		_exit(result == -1 && (errno == EAGAIN || errno == EACCES) ? 0 : 1);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return failure("record-lock-conflict");
	lock.l_type = F_UNLCK;
	if (fcntl(fd, F_SETLK, &lock) != 0)
		return failure("record-lock-release");
	(void)close(fd); (void)unlink("/tmp/r2r-lock");
	marker("R2R:05:RECORD-LOCK\n");
	return 0;
}

static int
test_rlimit(void)
{
	struct rlimit saved, limited;
	int fd;
	if (getrlimit(RLIMIT_NOFILE, &saved) != 0)
		return failure("rlimit-get");
	limited = saved; limited.rlim_cur = 3;
	if (setrlimit(RLIMIT_NOFILE, &limited) != 0 ||
	    (fd = open("/tmp/r2r-limit", O_CREAT | O_RDWR, 0600)) != -1 ||
	    errno != EMFILE || setrlimit(RLIMIT_NOFILE, &saved) != 0)
		return failure("rlimit-enforce");
	(void)unlink("/tmp/r2r-limit");
	marker("R2R:06:RLIMIT\n");
	return 0;
}

static int
test_waitid(void)
{
	pid_t child;
	int status;
	siginfo_t info;
	child = fork();
	if (child < 0)
		return failure("waitid-fork");
	if (child == 0)
		_exit(31);
	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0 ||
	    info.si_pid != child || info.si_status != 31 ||
	    waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0 ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 31)
		return failure("waitid-nowait");
	marker("R2R:07:WAITID\n");
	return 0;
}

static int
test_integration(void)
{
	const char *name = "/r2r-shared";
	char *mapping;
	int fd;
	(void)shm_unlink(name);
	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, 4096) != 0)
		return failure("integration-shm-open");
	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return failure("integration-shm-map");
	mapping[0] = 'z';
	if (mapping[0] != 'z' || munmap(mapping, 4096) != 0 ||
	    close(fd) != 0 || shm_unlink(name) != 0)
		return failure("integration-shm-lifetime");
	marker("R2R:08:INTEGRATION\n");
	return 0;
}

static int
test_new_required_apis(void)
{
	char *arguments[] = { (char *)"options", (char *)"-ab", (char *)"value", NULL };
	char formatted[32], byte = 0;
	struct aiocb control;
	struct dirent **entries = NULL;
	struct sched_param parameter;
	struct tm calendar;
	struct tms process_times;
	time_t epoch = 0, iso_date = 1609459200;
	pthread_attr_t attributes;
	pid_t child;
	int count, descriptor, pair[2], status;

	optind = 0;
	opterr = 0;
	if (getopt(3, arguments, "ab:") != 'a' ||
	    getopt(3, arguments, "ab:") != 'b' || strcmp(optarg, "value") ||
	    getopt(3, arguments, "ab:") != -1)
		return failure("getopt");
	if (gmtime_r(&epoch, &calendar) == NULL || calendar.tm_year != 70 ||
	    calendar.tm_mon != 0 || calendar.tm_mday != 1 ||
	    calendar.tm_wday != 4 || gmtime_r(&iso_date, &calendar) == NULL ||
	    strftime(formatted, sizeof(formatted), "%G-W%V-%u", &calendar) != 10 ||
	    strcmp(formatted, "2020-W53-5"))
		return failure("calendar");
	if (pthread_attr_init(&attributes) != 0 ||
	    pthread_attr_getschedparam(&attributes, &parameter) != 0 ||
	    parameter.sched_priority != 0 || sched_yield() != 0 ||
	    times(&process_times) == (clock_t)-1)
		return failure("scheduler-apis");
	count = scandir("/tmp", &entries, NULL, alphasort);
	if (count < 0)
		return failure("scandir");
	while (count != 0) free(entries[--count]);
	free(entries);
	descriptor = open("/tmp/r2r-aio", O_CREAT | O_TRUNC | O_RDWR, 0600);
	memset(&control, 0, sizeof(control));
	control.aio_fildes = descriptor;
	control.aio_buf = (void *)"a";
	control.aio_nbytes = 1;
	if (descriptor < 0 || aio_write(&control) != 0 ||
	    aio_error(&control) != 0 || aio_return(&control) != 1 ||
	    pread(descriptor, &byte, 1, 0) != 1 || byte != 'a')
		return failure("aio");
	(void)close(descriptor);
	(void)unlink("/tmp/r2r-aio");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
	    sockatmark(pair[0]) != 0)
		return failure("sockatmark");
	(void)close(pair[0]); (void)close(pair[1]);
	if (isatty(0) && (tcgetsid(0) != getsid(0) || tcsendbreak(0, 0) != 0))
		return failure("terminal-required");
	descriptor = open("/bin/posix-r2-remaining", O_RDONLY);
	child = fork();
	if (descriptor < 0 || child < 0)
		return failure("fexecve-setup");
	if (child == 0) {
		char *const exec_arguments[] = {
			(char *)"posix-r2-remaining",
			(char *)"--fexec-child", NULL
		};
		fexecve(descriptor, exec_arguments, environ);
		_exit(127);
	}
	(void)close(descriptor);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return failure("fexecve");
	marker("R2R:09:REQUIRED-APIS\n");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--fexec-child") == 0)
		return 0;
	if (test_tmpfs() != 0 || test_unix_vfs() != 0 ||
	    test_scm_rights() != 0 || test_fifo() != 0 ||
	    test_record_lock() != 0 || test_rlimit() != 0 ||
	    test_waitid() != 0 || test_integration() != 0 ||
	    test_new_required_apis() != 0)
		return 1;
	marker("R2R:01-09:PASS\n");
	return 0;
}
