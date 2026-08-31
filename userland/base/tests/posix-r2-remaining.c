/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD posix r2 remaining userland behavior.
 */

#include <errno.h>
#include <aio.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
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

struct atomic_record {
	uint32_t counter;
	uint32_t left;
	uint32_t right;
};

static int test_tmpfs(void);
static int failure(const char *name);
static void marker(const char *text);
static int test_unix_vfs(void);
static int test_scm_rights(void);
static ssize_t send_fd(int socket_fd, int passed_fd, char byte);
static ssize_t receive_fd(int socket_fd, int *received, char *byte, void *name, socklen_t name_length);
static int test_fifo(void);
static int test_record_lock(void);
static int test_rlimit(void);
static int test_waitid(void);
static int test_integration(void);
static int test_new_required_apis(void);
static int test_exec_scripts(void);
static int write_test_file(const char *path, const char *contents, mode_t mode);
static int wait_exec_status(const char *path, char *const arguments[], int expected);
static int copy_test_executable(const char *source, const char *destination);
static int wait_setid_mutation(const char *path, int truncate);
static int wait_setid_noop_chown(const char *path);
static int test_posix2024_apis(void);
static int test_generic_atomics(void);
static void atomic_increment(_Atomic(struct atomic_record) *record);

/*
 * Runs the tests command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "--fexec-child") == 0)
		return 0;

	/* Handles the selected command-line operation. */
	if (argc >= 2 && strcmp(argv[1], "--setid-child") == 0) {
		/* Computes the function result. */
		function_result = getuid() == 0 && geteuid() == 123 && getgid() == 0 &&
			       getegid() == 200
			   ? 0
			   : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc >= 2 && strcmp(argv[1], "--setid-script-child extra") == 0) {
		/* Computes the function result. */
		function_result = getuid() == 0 && geteuid() == 0 && getgid() == 0 &&
			       getegid() == 0
			   ? 0
			   : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed test tmpfs operation. */
	if (test_tmpfs() != 0 || test_unix_vfs() != 0 ||
	    test_scm_rights() != 0 || test_fifo() != 0 ||
	    test_record_lock() != 0 || test_rlimit() != 0 ||
	    test_waitid() != 0 || test_integration() != 0 ||
	    test_new_required_apis() != 0 || test_exec_scripts() != 0 ||
	    test_posix2024_apis() != 0 || test_generic_atomics() != 0)

		/* Reports operation failure. */
		return 1;
	marker("R2R:01-12:PASS\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test tmpfs operation. */
static int
test_tmpfs(
	void)
{
	int function_result;
	struct stat status;
	char value[8];
	int fd;

	(void)unlink("/tmp/r2r-link");
	(void)unlink("/tmp/r2r-renamed");
	(void)unlink("/tmp/r2r-symlink");
	(void)unlink("/tmp/r2r-file");
	fd = open("/tmp/r2r-file", O_CREAT | O_EXCL | O_RDWR, 0600);

	/* Handles a failed lseek operation. */
	if (fd < 0 || lseek(fd, 8192, SEEK_SET) != 8192 ||
	    write(fd, "x", 1) != 1 || fstat(fd, &status) != 0 ||
	    status.st_size != 8193 || close(fd) != 0 ||
	    link("/tmp/r2r-file", "/tmp/r2r-link") != 0 ||
	    rename("/tmp/r2r-link", "/tmp/r2r-renamed") != 0 ||
	    symlink("r2r-renamed", "/tmp/r2r-symlink") != 0 ||
	    readlink("/tmp/r2r-symlink", value, sizeof(value)) != 8 ||
	    memcmp(value, "r2r-rena", 8) != 0) {
		/* Obtains the failure result. */
		function_result = failure("tmpfs");

		/* Returns the computed result. */
		return function_result;
	}
	(void)unlink("/tmp/r2r-symlink");
	(void)unlink("/tmp/r2r-renamed");
	(void)unlink("/tmp/r2r-file");
	marker("R2R:01:TMPFS\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the failure operation. */
static int
failure(
	const char *name)
{
	char digits[12];
	unsigned value, count;

	value = (unsigned)errno;
	count = 0;
	(void)write(2, "POSIX_R2R_FAIL: ", 17);
	(void)write(2, name, strlen(name));
	(void)write(2, " errno=", 7);
	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && count < sizeof(digits));

	/* Process each remaining element. */
	while (count != 0)
		(void)write(2, &digits[--count], 1);
	(void)write(2, "\n", 1);

	/* Reports operation failure. */
	return -1;
}

/* Supports the marker operation. */
static void
marker(
	const char *text)
{
	(void)write(1, text, strlen(text));
}

/* Supports the test unix vfs operation. */
static int
test_unix_vfs(
	void)
{
	int function_result;
	struct sockaddr_un address;
	int listener, client, accepted, stale;
	char byte;

	(void)unlink("/tmp/r2r.sock");
	(void)unlink("/tmp/r2r-renamed.sock");

	listener = -1;
	client = -1;
	accepted = -1;
	stale = -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, "/tmp/r2r.sock");
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	client = socket(AF_UNIX, SOCK_STREAM, 0);

	/* Handles a failed bind operation. */
	if (listener < 0 || client < 0 ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 1) != 0 ||
	    rename("/tmp/r2r.sock", "/tmp/r2r-renamed.sock") != 0)
		goto fail;
	strcpy(address.sun_path, "/tmp/r2r-renamed.sock");

	/* Handles a failed connect operation. */
	if (connect(client, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    (accepted = accept(listener, NULL, NULL)) < 0 ||
	    unlink(address.sun_path) != 0 || write(client, "u", 1) != 1 ||
	    read(accepted, &byte, 1) != 1 || byte != 'u')
		goto fail;
	(void)close(accepted);
	(void)close(client);
	(void)close(listener);

	/*
 * Closing does not unlink; the surviving inode is deliberately stale.
	 */
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	stale = socket(AF_UNIX, SOCK_STREAM, 0);
	strcpy(address.sun_path, "/tmp/r2r.sock");

	/* Handles a failed bind operation. */
	if (listener < 0 || stale < 0 ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    close(listener) != 0)
		goto fail;
	listener = -1;

	/* Handles the reported system error. */
	if (connect(stale, (struct sockaddr *)&address, sizeof(address)) !=
		-1 ||
	    errno != ECONNREFUSED || unlink(address.sun_path) != 0)
		goto fail;
	(void)close(stale);
	marker("R2R:02:UNIX-VFS\n");

	/* Reports successful completion. */
	return 0;
fail:

	/* Handles the accepted condition. */
	if (accepted >= 0)
		(void)close(accepted);

	/* Handles the client condition. */
	if (client >= 0)
		(void)close(client);

	/* Handles the listener condition. */
	if (listener >= 0)
		(void)close(listener);

	/* Handles the stale condition. */
	if (stale >= 0)
		(void)close(stale);
	(void)unlink("/tmp/r2r.sock");
	(void)unlink("/tmp/r2r-renamed.sock");

	/* Obtains the failure result. */
	function_result = failure("unix-vfs");

	/* Returns the computed result. */
	return function_result;
}

/* Supports the test scm rights operation. */
static int
test_scm_rights(
	void)
{
	int function_result;
	int pair[2], data[2], fillers[32], fill_count, received;
	char byte;

	fill_count = 0;
	received = -1;
	byte = 0;

	/* Handles a failed socketpair operation. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pipe(data) != 0 ||
	    send_fd(pair[0], data[1], 'm') != 1) {
		/* Obtains the failure result. */
		function_result = failure("scm-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	while (fill_count < 32 && (fillers[fill_count] = open(
				       "/tmp", O_RDONLY | O_DIRECTORY)) >= 0)
		fill_count++;

	/* Handles the reported system error. */
	if (errno != EMFILE ||
	    receive_fd(pair[1], &received, &byte, NULL, 0) != -1 ||
	    errno != EMFILE || fill_count == 0) {
		/* Obtains the failure result. */
		function_result = failure("scm-emfile");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(fillers[--fill_count]);

	/* Handles a failed receive fd operation. */
	if (receive_fd(pair[1], &received, &byte, NULL, 0) != 1 ||
	    byte != 'm' || write(received, "r", 1) != 1 ||
	    read(data[0], &byte, 1) != 1 || byte != 'r') {
		/* Obtains the failure result. */
		function_result = failure("scm-retry");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(received);

	/* Process each remaining element. */
	while (fill_count != 0)
		(void)close(fillers[--fill_count]);
	(void)close(data[0]);
	(void)close(data[1]);
	(void)close(pair[0]);
	(void)close(pair[1]);

	/* A copyout fault must also leave a datagram and its rights queued. */
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 || pipe(data) != 0 ||
	    send_fd(pair[0], data[1], 'e') != 1 ||
	    receive_fd(pair[1], &received, &byte, (void *)(uintptr_t)1,
		       sizeof(struct sockaddr_un)) != -1 ||
	    errno != EFAULT ||
	    receive_fd(pair[1], &received, &byte, NULL, 0) != 1 || byte != 'e') {
		/* Obtains the failure result. */
		function_result = failure("scm-efault");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(received);
	(void)close(data[0]);
	(void)close(data[1]);
	(void)close(pair[0]);
	(void)close(pair[1]);
	marker("R2R:03:SCM-RIGHTS\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the send fd operation. */
static ssize_t
send_fd(
	int socket_fd,
	int passed_fd,
	char byte)
{
	ssize_t function_result;
	struct msghdr message;
	struct iovec vector;

	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *header;
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = &byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	header = (struct cmsghdr *)control.bytes;
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));

	/* Obtains the sendmsg result. */
	function_result = sendmsg(socket_fd, &message, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the receive fd operation. */
static ssize_t
receive_fd(
	int socket_fd,
	int *received,
	char *byte,
	void *name,
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
	vector.iov_base = byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_name = name;
	message.msg_namelen = name_length;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	result = recvmsg(socket_fd, &message, 0);

	/* Checks the operation result. */
	if (result < 0)
		return result;
	header = message.msg_controllen >= CMSG_LEN(sizeof(int))
		     ? (struct cmsghdr *)control.bytes
		     : NULL;

	/* Handles a failed CMSG LEN operation. */
	if (header == NULL || header->cmsg_level != SOL_SOCKET ||
	    header->cmsg_type != SCM_RIGHTS ||
	    header->cmsg_len < CMSG_LEN(sizeof(int))) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(received, CMSG_DATA(header), sizeof(*received));

	/* Returns the computed result. */
	return result;
}

/* Supports the test fifo operation. */
static int
test_fifo(
	void)
{
	int function_result;
	int reader, writer;
	char byte;

	(void)unlink("/tmp/r2r-fifo");

	/* Handles a failed mkfifo operation. */
	if (mkfifo("/tmp/r2r-fifo", 0600) != 0 ||
	    (reader = open("/tmp/r2r-fifo", O_RDONLY | O_NONBLOCK)) < 0 ||
	    (writer = open("/tmp/r2r-fifo", O_WRONLY | O_NONBLOCK)) < 0 ||
	    write(writer, "f", 1) != 1 || read(reader, &byte, 1) != 1 ||
	    byte != 'f' || unlink("/tmp/r2r-fifo") != 0 ||
	    write(writer, "g", 1) != 1 || read(reader, &byte, 1) != 1 ||
	    byte != 'g') {
		/* Obtains the failure result. */
		function_result = failure("fifo");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(reader);
	(void)close(writer);
	marker("R2R:04:FIFO\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test record lock operation. */
static int
test_record_lock(
	void)
{
	int function_result;
	int result;
	int independent;
	struct flock query;
	struct flock lock;
	pid_t child;
	int duplicate, fd, status;

	fd = open("/tmp/r2r-lock", O_CREAT | O_RDWR | O_TRUNC, 0600);
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_len = 1;

	/* Handles a failed fcntl operation. */
	if (fd < 0 || fcntl(fd, F_SETLK, &lock) != 0 || (child = fork()) < 0) {
		/* Obtains the failure result. */
		function_result = failure("record-lock-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
				result = fcntl(fd, F_SETLK, &lock);
		_exit(result == -1 && (errno == EAGAIN || errno == EACCES) ? 0
									   : 1);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		/* Obtains the failure result. */
		function_result = failure("record-lock-conflict");

		/* Returns the computed result. */
		return function_result;
	}
	lock.l_type = F_UNLCK;

	/* Handles a failed fcntl operation. */
	if (fcntl(fd, F_SETLK, &lock) != 0) {
		/* Obtains the failure result. */
		function_result = failure("record-lock-release");

		/* Returns the computed result. */
		return function_result;
	}
	lock.l_type = F_WRLCK;
	lock.l_pid = 0;

	/* Handles a failed fcntl operation. */
	if (fcntl(fd, F_OFD_SETLK, &lock) != 0 || (duplicate = dup(fd)) < 0 ||
	    close(fd) != 0 || (child = fork()) < 0) {
		/* Obtains the failure result. */
		function_result = failure("ofd-lock-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {

				query = lock;

		/* Handles a failed fcntl operation. */
		if (fcntl(duplicate, F_OFD_SETLK, &lock) != 0)
			_exit(1);
		independent = open("/tmp/r2r-lock", O_RDWR);

		/* Handles the reported system error. */
		if (independent < 0 ||
		    fcntl(independent, F_OFD_GETLK, &query) != 0 ||
		    query.l_type != F_WRLCK || query.l_pid != -1 ||
		    fcntl(independent, F_OFD_SETLK, &lock) != -1 ||
		    errno != EAGAIN)
			_exit(2);
		(void)close(independent);
		_exit(0);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 || close(duplicate) != 0 ||
	    (fd = open("/tmp/r2r-lock", O_RDWR)) < 0 ||
	    fcntl(fd, F_OFD_SETLK, &lock) != 0) {
		/* Obtains the failure result. */
		function_result = failure("ofd-lock-lifetime");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(fd);
	(void)unlink("/tmp/r2r-lock");
	marker("R2R:05:RECORD-LOCK\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test rlimit operation. */
static int
test_rlimit(
	void)
{
	int function_result;
	struct rlimit saved, limited;
	sighandler_t previous;
	char bytes[8] = {0};
	int fd;

	/* Handles a failed getrlimit operation. */
	if (getrlimit(RLIMIT_NOFILE, &saved) != 0) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-get");

		/* Returns the computed result. */
		return function_result;
	}

	limited = saved;
	limited.rlim_cur = 3;

	/* Handles the reported system error. */
	if (setrlimit(RLIMIT_NOFILE, &limited) != 0 ||
	    sysconf(_SC_OPEN_MAX) != 3 ||
	    (fd = open("/tmp/r2r-limit", O_CREAT | O_RDWR, 0600)) != -1 ||
	    errno != EMFILE || setrlimit(RLIMIT_NOFILE, &saved) != 0 ||
	    sysconf(_SC_OPEN_MAX) != (long)saved.rlim_cur) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-enforce");

		/* Returns the computed result. */
		return function_result;
	}
	(void)unlink("/tmp/r2r-limit");

	/* Handles a failed getrlimit operation. */
	if (getrlimit(RLIMIT_FSIZE, &saved) != 0) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-fsize-get");

		/* Returns the computed result. */
		return function_result;
	}

	limited = saved;
	limited.rlim_cur = 4;
	previous = signal(SIGXFSZ, (sighandler_t)SIG_IGN);
	fd = open("/tmp/r2r-fsize", O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Handles the reported system error. */
	if (previous == SIG_ERR || fd < 0 ||
	    setrlimit(RLIMIT_FSIZE, &limited) != 0 ||
	    write(fd, bytes, sizeof(bytes)) != 4 || write(fd, bytes, 1) != -1 ||
	    errno != EFBIG || ftruncate(fd, 5) != -1 || errno != EFBIG ||
	    setrlimit(RLIMIT_FSIZE, &saved) != 0 || ftruncate(fd, 8) != 0 ||
	    setrlimit(RLIMIT_FSIZE, &limited) != 0 ||
	    pwrite(fd, bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes) ||
	    ftruncate(fd, 6) != 0 || ftruncate(fd, 7) != -1 || errno != EFBIG ||
	    setrlimit(RLIMIT_FSIZE, &saved) != 0 ||
	    signal(SIGXFSZ, previous) == SIG_ERR) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-fsize");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(fd);
	(void)unlink("/tmp/r2r-fsize");

	/* Handles a failed getrlimit operation. */
	if (getrlimit(RLIMIT_DATA, &saved) != 0) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-data-get");

		/* Returns the computed result. */
		return function_result;
	}

	limited = saved;
	limited.rlim_cur = 0;

	/* Handles the reported system error. */
	if (setrlimit(RLIMIT_DATA, &limited) != 0 || sbrk(1) != (void *)-1 ||
	    errno != ENOMEM || setrlimit(RLIMIT_DATA, &saved) != 0) {
		/* Obtains the failure result. */
		function_result = failure("rlimit-data");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:06:RLIMIT\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test waitid operation. */
static int
test_waitid(
	void)
{
	int function_result;
	pid_t child;
	int status;
	siginfo_t info;

	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		/* Obtains the failure result. */
		function_result = failure("waitid-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0)
		_exit(31);
	memset(&info, 0, sizeof(info));

	/* Handles a failed waitid operation. */
	if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0 ||
	    info.si_pid != child || info.si_status != 31 ||
	    waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0 ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 31) {
		/* Obtains the failure result. */
		function_result = failure("waitid-nowait");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:07:WAITID\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test integration operation. */
static int
test_integration(
	void)
{
	int function_result;
	const char *name;
	char *mapping;
	int fd;

	name = "/r2r-shared";
	(void)shm_unlink(name);
	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);

	/* Handles a failed ftruncate operation. */
	if (fd < 0 || ftruncate(fd, 4096) != 0) {
		/* Obtains the failure result. */
		function_result = failure("integration-shm-open");

		/* Returns the computed result. */
		return function_result;
	}

	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	/* Handles an operation failure. */
	if (mapping == MAP_FAILED) {
		/* Obtains the failure result. */
		function_result = failure("integration-shm-map");

		/* Returns the computed result. */
		return function_result;
	}
	mapping[0] = 'z';

	/* Handles a failed munmap operation. */
	if (mapping[0] != 'z' || munmap(mapping, 4096) != 0 || close(fd) != 0 ||
	    shm_unlink(name) != 0) {
		/* Obtains the failure result. */
		function_result = failure("integration-shm-lifetime");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:08:INTEGRATION\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test new required apis operation. */
static int
test_new_required_apis(
	void)
{
	int function_result;
	char *arguments[] = {(char *)"options", (char *)"-ab", (char *)"value",
			     NULL};
	char formatted[32], byte;
	struct aiocb control;
	struct dirent **entries;
	struct sched_param parameter;
	struct tm calendar;
	struct tms process_times;
	time_t epoch, iso_date;
	pthread_attr_t attributes;
	pid_t child;
	int count, descriptor, pair[2], status;
	char *exec_arguments[3];

	byte = 0;
	entries = NULL;
	epoch = 0;
	iso_date = 1609459200;

	optind = 0;
	opterr = 0;

	/* Handles a failed getopt operation. */
	if (getopt(3, arguments, "ab:") != 'a' ||
	    getopt(3, arguments, "ab:") != 'b' || strcmp(optarg, "value") ||
	    getopt(3, arguments, "ab:") != -1) {
		/* Obtains the failure result. */
		function_result = failure("getopt");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed gmtime r operation. */
	if (gmtime_r(&epoch, &calendar) == NULL || calendar.tm_year != 70 ||
	    calendar.tm_mon != 0 || calendar.tm_mday != 1 ||
	    calendar.tm_wday != 4 || gmtime_r(&iso_date, &calendar) == NULL ||
	    strftime(formatted, sizeof(formatted), "%G-W%V-%u", &calendar) !=
		10 ||
	    strcmp(formatted, "2020-W53-5")) {
		/* Obtains the failure result. */
		function_result = failure("calendar");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread attr init operation. */
	if (pthread_attr_init(&attributes) != 0 ||
	    pthread_attr_getschedparam(&attributes, &parameter) != 0 ||
	    parameter.sched_priority != 0 || sched_yield() != 0 ||
	    times(&process_times) == (clock_t)-1) {
		/* Obtains the failure result. */
		function_result = failure("scheduler-apis");

		/* Returns the computed result. */
		return function_result;
	}

	count = scandir("/tmp", &entries, NULL, alphasort);

	/* Checks the remaining item count. */
	if (count < 0) {
		/* Obtains the failure result. */
		function_result = failure("scandir");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	while (count != 0)
		free(entries[--count]);
	free(entries);
	descriptor = open("/tmp/r2r-aio", O_CREAT | O_TRUNC | O_RDWR, 0600);
	memset(&control, 0, sizeof(control));
	control.aio_fildes = descriptor;
	control.aio_buf = (void *)"a";
	control.aio_nbytes = 1;

	/* Handles an operation failure. */
	if (descriptor < 0 || aio_write(&control) != 0 ||
	    aio_error(&control) != 0 || aio_return(&control) != 1 ||
	    pread(descriptor, &byte, 1, 0) != 1 || byte != 'a') {
		/* Obtains the failure result. */
		function_result = failure("aio");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(descriptor);
	(void)unlink("/tmp/r2r-aio");
	descriptor = open("/tmp/r2r-owner", O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Handles a failed fcntl operation. */
	if (descriptor < 0 || fcntl(descriptor, F_SETOWN, getpid()) != 0 ||
	    fcntl(descriptor, F_GETOWN) != getpid() ||
	    fcntl(descriptor, F_SETOWN, -getpgrp()) != 0 ||
	    fcntl(descriptor, F_GETOWN) != -getpgrp()) {
		/* Obtains the failure result. */
		function_result = failure("fcntl-owner");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(descriptor);
	(void)unlink("/tmp/r2r-owner");

	/* Handles a failed socketpair operation. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
	    sockatmark(pair[0]) != 0) {
		/* Obtains the failure result. */
		function_result = failure("sockatmark");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[0]);
	(void)close(pair[1]);

	/* Handles a failed isatty operation. */
	if (isatty(0) && (tcgetsid(0) != getsid(0) || tcsendbreak(0, 0) != 0)) {
		/* Obtains the failure result. */
		function_result = failure("terminal-required");

		/* Returns the computed result. */
		return function_result;
	}

	descriptor = open("/bin/posix-r2-remaining", O_RDONLY);
	child = fork();

	/* Checks the file descriptor. */
	if (descriptor < 0 || child < 0) {
		/* Obtains the failure result. */
		function_result = failure("fexecve-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
		exec_arguments[0] = (char *)"posix-r2-remaining";
		exec_arguments[1] = (char *)"--fexec-child";
		exec_arguments[2] = NULL;
		fexecve(descriptor, exec_arguments, environ);
		_exit(127);
	}
	(void)close(descriptor);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		/* Obtains the failure result. */
		function_result = failure("fexecve");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:09:REQUIRED-APIS\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test exec scripts operation. */
static int
test_exec_scripts(
	void)
{
	int function_result;
	char path[32];
	static const char *const cleanup[] = {
	    "/tmp/r2r-script",	"/tmp/r2r-setid-script", "/tmp/r2r-setid-image",
	    "/tmp/r2r-cycle-a", "/tmp/r2r-cycle-b",	 "/tmp/r2r-depth-0",
	    "/tmp/r2r-depth-1", "/tmp/r2r-depth-2",	 "/tmp/r2r-depth-3",
	    "/tmp/r2r-depth-4"};
	char shebang[96];
	char *arguments[] = {(char *)"caller-zero", (char *)"tail", NULL};
	char *setid_arguments[3];
	unsigned i;
	int descriptor;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++)
		(void)unlink(cleanup[i]);

	/* Handles a failed write test file operation. */
	if (write_test_file("/tmp/r2r-script", "#!/bin/sh\nexit 37\n", 0700) !=
		0 ||
	    wait_exec_status("/tmp/r2r-script", arguments, 37) != 0) {
		/* Obtains the failure result. */
		function_result = failure("exec-script");

		/* Returns the computed result. */
		return function_result;
	}

	descriptor = open("/tmp/r2r-script", O_RDONLY);

	/* Handles the reported system error. */
	if (descriptor < 0 || fexecve(descriptor, arguments, environ) != -1 ||
	    errno != ENOEXEC) {
		/* Checks the file descriptor. */
		if (descriptor >= 0)
			(void)close(descriptor);

		/* Obtains the failure result. */
		function_result = failure("fexecve-script-path");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(descriptor);

	/* Handles the reported system error. */
	if (write_test_file("/tmp/r2r-cycle-a", "#!/tmp/r2r-cycle-b\n", 0700) !=
		0 ||
	    write_test_file("/tmp/r2r-cycle-b", "#!/tmp/r2r-cycle-a\n", 0700) !=
		0 ||
	    execve("/tmp/r2r-cycle-a", arguments, environ) != -1 ||
	    errno != ELOOP) {
		/* Obtains the failure result. */
		function_result = failure("exec-script-cycle");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < 4U; i++) {
		(void)snprintf(shebang, sizeof(shebang),
			       "#!/tmp/r2r-depth-%u\n", i + 1U);

		(void)snprintf(path, sizeof(path), "/tmp/r2r-depth-%u",
			       i);

		/* Handles a failed write test file operation. */
		if (write_test_file(path, shebang, 0700) != 0) {
			/* Obtains the failure result. */
			function_result = failure("exec-script-depth-setup");

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Handles the reported system error. */
	if (write_test_file("/tmp/r2r-depth-4", "#!/bin/sh\nexit 0\n", 0700) !=
		0 ||
	    wait_exec_status("/tmp/r2r-depth-1", arguments, 0) != 0 ||
	    execve("/tmp/r2r-depth-0", arguments, environ) != -1 ||
	    errno != ELOOP) {
		/* Obtains the failure result. */
		function_result = failure("exec-script-depth");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed geteuid operation. */
	if (geteuid() == 0) {
		setid_arguments[0] = (char *)"setid-image";
		setid_arguments[1] = (char *)"--setid-child";
		setid_arguments[2] = NULL;

		/* Handles a failed copy test executable operation. */
		if (copy_test_executable("/bin/posix-r2-remaining",
					 "/tmp/r2r-setid-image") != 0 ||
		    chown("/tmp/r2r-setid-image", 123, 200) != 0 ||
		    chmod("/tmp/r2r-setid-image", 06755) != 0 ||
		    wait_exec_status("/tmp/r2r-setid-image", setid_arguments,
				     0) != 0) {
			/* Obtains the failure result. */
			function_result = failure("exec-setid-binary");

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed write test file operation. */
		if (write_test_file("/tmp/r2r-setid-script",
				    "#!/bin/posix-r2-remaining "
				    "--setid-script-child extra\n",
				    0700) != 0 ||
		    chown("/tmp/r2r-setid-script", 123, 200) != 0 ||
		    chmod("/tmp/r2r-setid-script", 06755) != 0 ||
		    wait_exec_status("/tmp/r2r-setid-script", arguments, 0) !=
			0) {
			/* Obtains the failure result. */
			function_result = failure("exec-setid-script");

			/* Returns the computed result. */
			return function_result;
		}

		/*
 * Both ordinary write and O_TRUNC must remove privilege bits
		 * before mutating file contents for a non-superuser. */
		if (chmod("/tmp/r2r-setid-image", 06755) != 0 ||
		    wait_setid_mutation("/tmp/r2r-setid-image", 0) != 0 ||
		    chmod("/tmp/r2r-setid-image", 06755) != 0 ||
		    wait_setid_mutation("/tmp/r2r-setid-image", 1) != 0 ||
		    chmod("/tmp/r2r-setid-image", 06755) != 0 ||
		    wait_setid_noop_chown("/tmp/r2r-setid-image") != 0) {
			/* Obtains the failure result. */
			function_result = failure("write-truncate-clear-setid");

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Process each remaining element. */
	for (i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++)
		(void)unlink(cleanup[i]);
	marker("R2R:10:EXEC-SCRIPT\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the write test file operation. */
static int
write_test_file(
	const char *path,
	const char *contents,
	mode_t mode)
{
	ssize_t count;
	size_t length, done;
	int descriptor;

	length = strlen(contents);
	done = 0;
	descriptor = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Process each remaining element. */
	while (done < length) {

		count = write(descriptor, contents + done, length - done);

		/* Checks the remaining item count. */
		if (count <= 0) {
			(void)close(descriptor);

			/* Reports operation failure. */
			return -1;
		}
		done += (size_t)count;
	}

	/* Handles a failed close operation. */
	if (close(descriptor) != 0 || chmod(path, mode) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the wait exec status operation. */
static int
wait_exec_status(
	const char *path,
	char *const arguments[],
	int expected)
{
	int function_result;
	pid_t child;
	int status;

	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return -1;

	/* Checks the child process state. */
	if (child == 0) {
		execve(path, arguments, environ);
		_exit(127);
	}

	/* Computes the function result. */
	function_result = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		       WEXITSTATUS(status) == expected
		   ? 0
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the copy test executable operation. */
static int
copy_test_executable(
	const char *source,
	const char *destination)
{
	ssize_t written;
	ssize_t done;
	int failed;
	char buffer[1024];
	int input, output;
	ssize_t count;

	input = -1;
	output = -1;

	input = open(source, O_RDONLY);
	output = open(destination, O_CREAT | O_TRUNC | O_WRONLY, 0700);

	/* Validates the current input. */
	if (input < 0 || output < 0)
		goto fail;

	/* Process input until it is exhausted. */
	while ((count = read(input, buffer, sizeof(buffer))) > 0) {
		/* Process each remaining element. */
		done = 0;
		while (done < count) {

			written = write(output, buffer + done,
						(size_t)(count - done));

			/* Handles the written condition. */
			if (written <= 0)
				goto fail;
			done += written;
		}
	}

	failed = count < 0;

	/* Handles a failed close operation. */
	if (close(input) != 0)
		failed = 1;
	input = -1;

	/* Handles a failed close operation. */
	if (close(output) != 0)
		failed = 1;
	output = -1;

	/* Handles an operation failure. */
	if (failed)
		goto fail_closed;

	/* Reports successful completion. */
	return 0;
fail:

	/* Validates the current input. */
	if (input >= 0)
		(void)close(input);

	/* Handles the output condition. */
	if (output >= 0)
		(void)close(output);
fail_closed:
	(void)unlink(destination);

	/* Reports operation failure. */
	return -1;
}

/* Supports the wait setid mutation operation. */
static int
wait_setid_mutation(
	const char *path,
	int truncate)
{
	int function_result;
	struct stat result;
	int flags;
	int descriptor;
	pid_t child;
	int status;

	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return -1;

	/* Checks the child process state. */
	if (child == 0) {

				flags = O_WRONLY | (truncate ? O_TRUNC : 0);

		/* Handles a failed setgid operation. */
		if (setgid(200) != 0 || setuid(123) != 0)
			_exit(2);
		descriptor = open(path, flags);

		/* Handles a failed write operation. */
		if (descriptor < 0 ||
		    (!truncate && write(descriptor, "W", 1) != 1) ||
		    fstat(descriptor, &result) != 0 || close(descriptor) != 0)
			_exit(3);

		/* Checks the operation result. */
		if ((result.st_mode & (S_ISUID | S_ISGID)) != 0 ||
		    (truncate && result.st_size != 0))
			_exit(4);
		_exit(0);
	}

	/* Computes the function result. */
	function_result = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		       WEXITSTATUS(status) == 0
		   ? 0
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the wait setid noop chown operation. */
static int
wait_setid_noop_chown(
	const char *path)
{
	int function_result;
	struct stat result;
	pid_t child;
	int status;

	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return -1;

	/* Checks the child process state. */
	if (child == 0) {
		/* Handles a failed setgid operation. */
		if (setgid(200) != 0 || setuid(123) != 0 ||
		    chown(path, (uid_t)-1, (gid_t)-1) != 0 ||
		    stat(path, &result) != 0 ||
		    (result.st_mode & (S_ISUID | S_ISGID)) != 0)
			_exit(5);
		_exit(0);
	}

	/* Computes the function result. */
	function_result = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		       WEXITSTATUS(status) == 0
		   ? 0
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the test posix2024 apis operation. */
static int
test_posix2024_apis(
	void)
{
	union {
		max_align_t alignment;
		unsigned char bytes[512];
	} directory_buffer;
	char signal_name[SIG2STR_MAX];
	unsigned char entropy[16];
	uid_t real_uid, effective_uid, saved_uid;
	gid_t real_gid, effective_gid, saved_gid;
	char *shared;
	pid_t child;
	int descriptor, flags, pair[2], status;
	ssize_t count;
	int function_result;

	descriptor = open("/tmp/r2r-clofork", O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Handles a failed fcntl operation. */
	if (descriptor < 0 || fcntl(descriptor, F_SETFD, FD_CLOFORK) != 0 ||
	    (child = fork()) < 0) {
		/* Obtains the failure result. */
		function_result = failure("clofork-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0)
		_exit(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF ? 0
									 : 1);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    fcntl(descriptor, F_GETFD) != FD_CLOFORK || close(descriptor) != 0) {
		/* Obtains the failure result. */
		function_result = failure("clofork-fork");

		/* Returns the computed result. */
		return function_result;
	}
	(void)unlink("/tmp/r2r-clofork");

	/* Handles a failed pipe2 operation. */
	if (pipe2(pair, O_CLOEXEC | O_CLOFORK) != 0 ||
	    (fcntl(pair[0], F_GETFD) & (FD_CLOEXEC | FD_CLOFORK)) !=
		(FD_CLOEXEC | FD_CLOFORK) ||
	    (fcntl(pair[1], F_GETFD) & (FD_CLOEXEC | FD_CLOFORK)) !=
		(FD_CLOEXEC | FD_CLOFORK)) {
		/* Obtains the failure result. */
		function_result = failure("pipe2-flags");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[0]);
	(void)close(pair[1]);

	/* Handles a failed socketpair operation. */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_CLOFORK, 0,
		       pair) != 0 ||
	    (fcntl(pair[0], F_GETFD) & (FD_CLOEXEC | FD_CLOFORK)) !=
		(FD_CLOEXEC | FD_CLOFORK) ||
	    (fcntl(pair[1], F_GETFD) & (FD_CLOEXEC | FD_CLOFORK)) !=
		(FD_CLOEXEC | FD_CLOFORK)) {
		/* Obtains the failure result. */
		function_result = failure("socket-flags");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[0]);
	(void)close(pair[1]);

	memset(entropy, 0, sizeof(entropy));

	/* Handles the reported system error. */
	if (getentropy(entropy, sizeof(entropy)) != 0 ||
	    getentropy(entropy, 257) != -1 || errno != EINVAL) {
		/* Obtains the failure result. */
		function_result = failure("getentropy");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed getresuid operation. */
	if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0 ||
	    real_uid != getuid() || effective_uid != geteuid() ||
	    getresgid(&real_gid, &effective_gid, &saved_gid) != 0 ||
	    real_gid != getgid() || effective_gid != getegid()) {
		/* Obtains the failure result. */
		function_result = failure("getresid");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sig2str operation. */
	if (sig2str(SIGTERM, signal_name) != 0 || strcmp(signal_name, "TERM") ||
	    str2sig(signal_name, &flags) != 0 || flags != SIGTERM) {
		/* Obtains the failure result. */
		function_result = failure("signal-names");

		/* Returns the computed result. */
		return function_result;
	}

	descriptor =
	    open("/tmp/r2r-posix-close", O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Handles the reported system error. */
	if (descriptor < 0 || posix_close(descriptor, 1) != EINVAL ||
	    fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) {
		/* Obtains the failure result. */
		function_result = failure("posix-close");

		/* Returns the computed result. */
		return function_result;
	}
	(void)unlink("/tmp/r2r-posix-close");
	descriptor = open("/tmp", O_RDONLY | O_DIRECTORY);
	count = descriptor >= 0
		    ? posix_getdents(descriptor, directory_buffer.bytes,
				     sizeof(directory_buffer.bytes), 0)
		    : -1;

	/* Handles a failed close operation. */
	if (count <= 0 || close(descriptor) != 0) {
		/* Obtains the failure result. */
		function_result = failure("posix-getdents");

		/* Returns the computed result. */
		return function_result;
	}

	shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	/* Handles an operation failure. */
	if (shared == MAP_FAILED || (child = fork()) < 0) {
		/* Obtains the failure result. */
		function_result = failure("shared-anonymous-setup");

		/* Returns the computed result. */
		return function_result;
	}
	shared[0] = 0;

	/* Checks the child process state. */
	if (child == 0) {
		shared[0] = '8';
		_exit(0);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 || shared[0] != '8' ||
	    munmap(shared, 4096) != 0) {
		/* Obtains the failure result. */
		function_result = failure("shared-anonymous");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:11:POSIX-2024\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test generic atomics operation. */
static int
test_generic_atomics(
	void)
{
	int function_result;

	_Atomic(struct atomic_record) *record;
	struct atomic_record expected;
	struct atomic_record initial = {0U, 17U, 29U};
	struct atomic_record wrong = {UINT32_MAX, 0U, 0U};
	pid_t child;
	int status;
	unsigned index;

	record = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	/* Handles an operation failure. */
	if (record == MAP_FAILED) {
		/* Obtains the failure result. */
		function_result = failure("atomic-map");

		/* Returns the computed result. */
		return function_result;
	}

	atomic_init(record, initial);
	expected = wrong;

	/* Handles a failed atomic compare exchange strong operation. */
	if (atomic_compare_exchange_strong(record, &expected, initial) ||
	    expected.counter != 0U || expected.left != 17U ||
	    expected.right != 29U || (child = fork()) < 0) {
		/* Obtains the failure result. */
		function_result = failure("atomic-compare");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
		/* Process each remaining element. */
		for (index = 0; index < 200U; index++)
			atomic_increment(record);
		_exit(0);
	}

	/* Process each remaining element. */
	for (index = 0; index < 200U; index++)
		atomic_increment(record);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 || atomic_load(record).counter != 400U ||
	    munmap(record, 4096) != 0) {
		/* Obtains the failure result. */
		function_result = failure("atomic-shared");

		/* Returns the computed result. */
		return function_result;
	}

	marker("R2R:12:ATOMIC\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the atomic increment operation. */
static void
atomic_increment(
	_Atomic(struct atomic_record) *record)
{
	struct atomic_record expected;
	struct atomic_record desired;

	expected = atomic_load_explicit(record, memory_order_relaxed);
	do {
		desired = expected;
		desired.counter++;
	} while (!atomic_compare_exchange_weak_explicit(
	    record, &expected, desired, memory_order_seq_cst,
	    memory_order_relaxed));
}
