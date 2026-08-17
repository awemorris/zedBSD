/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <poll.h>
#include <pty.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <spawn.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t barrier;
static pthread_spinlock_t spin;
static sem_t ready;
static sem_t cancel_wait;
static sem_t detached_ready;
static int shared_value;
static int spin_value;
static int cleanup_called;
static int atfork_prepare_called;
static int atfork_parent_called;
static int atfork_child_called;
#define PTY_STRESS_SIZE 6000U
static char pty_stress_input[PTY_STRESS_SIZE];
static char pty_stress_output[PTY_STRESS_SIZE];

static void atfork_prepare(void) { atfork_prepare_called++; }
static void atfork_parent(void) { atfork_parent_called++; }
static void atfork_child(void) { atfork_child_called++; }

static void *detached_worker(void *argument)
{
	(void)argument;
	(void)sem_post(&detached_ready);
	return NULL;
}

static void cancel_cleanup(void *argument)
{
	(void)argument;
	cleanup_called = 1;
}

static void *cancel_worker(void *argument)
{
	(void)argument;
	pthread_cleanup_push(cancel_cleanup, NULL);
	(void)sem_wait(&cancel_wait);
	pthread_cleanup_pop(0);
	return NULL;
}

static int fail(const char *name)
{
	(void)write(2, "POSIX_R2_FAIL: ", 15);
	(void)write(2, name, strlen(name));
	(void)write(2, "\n", 1);
	return 1;
}

static int fail_errno(const char *name)
{
	char digits[4];
	unsigned value = (unsigned)errno;
	unsigned count = 0;

	(void)write(2, "POSIX_R2_FAIL: ", 15);
	(void)write(2, name, strlen(name));
	(void)write(2, " errno=", 7);
	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && count < sizeof(digits));
	while (count != 0)
		(void)write(2, &digits[--count], 1);
	(void)write(2, "\n", 1);
	return 1;
}

static void *worker(void *argument)
{
	(void)argument;
	if (pthread_mutex_lock(&mutex) != 0)
		return (void *)1;
	shared_value = 42;
	(void)pthread_mutex_unlock(&mutex);
	if (pthread_rwlock_wrlock(&rwlock) != 0)
		return (void *)2;
	shared_value++;
	(void)pthread_rwlock_unlock(&rwlock);
	if (pthread_spin_lock(&spin) != 0)
		return (void *)3;
	spin_value++;
	(void)pthread_spin_unlock(&spin);
	(void)sem_post(&ready);
	(void)pthread_barrier_wait(&barrier);
	return (void *)7;
}

static void *locale_worker(void *argument)
{
	locale_t locale = argument;
	if (uselocale(locale) == NULL || MB_CUR_MAX != 1U)
		return (void *)1;
	return (void *)0;
}

int main(void)
{
	char *exec_argv[] = { "/bin/sh", NULL };
	char *exec_envp[] = { "R2_EXEC_FINAL=1", NULL };
	char *spawn_envp[] = { "R2_SPAWN_CHILD=1", NULL };
	pthread_t thread;
	pthread_attr_t detached_attributes;
	void *thread_result = NULL;
	struct pollfd event;
	int pipefd[2], pair[2], listener, client, accepted, poll_result;
	int pty_master, pty_slave;
	int datagram_server, datagram_client;
	int rights_pipe[2], received_fd;
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct sockaddr_un local;
	char byte = 0;
	char mq_buffer[256];
	unsigned mq_priority = 0;
	mqd_t queue;
	pid_t child;
	int child_status;
	siginfo_t child_information;
	int directory_fd, regular_fd;
	DIR *directory;
	struct iovec file_vectors[2];
	struct utsname uts;
	struct timespec now;
	struct timespec signal_timeout = { 1, 0 };
	struct rlimit saved_limit, small_limit;
	struct statvfs filesystem_status, descriptor_status;
	struct flock file_lock;
	size_t pty_received;
	struct sigevent notification;
	struct itimerspec timer_value, timer_current;
	timer_t process_timer;
	sigset_t notify_set, old_mask;
	siginfo_t notify_info;
	char path_buffer[64], file_buffer[8], login_buffer[16], tty_buffer[32];
	mbstate_t multibyte_state;
	wchar_t wide_character;
	char encoded[4];
	locale_t c_locale;

	if (getenv("R2_EXEC_FINAL") != NULL) {
		(void)write(1, "R2:01-06:PASS\n", 15);
		return 0;
	}
	if (getenv("R2_SPAWN_CHILD") != NULL)
		return 23;

	if (sem_init(&ready, 0, 0) != 0)
		return fail("sem_init");
	if (sem_init(&cancel_wait, 0, 0) != 0)
		return fail("cancel-sem-init");
	if (sem_init(&detached_ready, 0, 0) != 0)
		return fail("detached-sem-init");
	if (pthread_barrier_init(&barrier, NULL, 2) != 0)
		return fail("barrier_init");
	if (pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) != 0)
		return fail("spin_init");
	if (pthread_create(&thread, NULL, worker, NULL) != 0)
		return fail("pthread_create");
	if (sem_wait(&ready) != 0)
		return fail("sem_wait");
	(void)pthread_barrier_wait(&barrier);
	if (pthread_join(thread, &thread_result) != 0 ||
	    thread_result != (void *)7 || shared_value != 43 || spin_value != 1)
		return fail("pthread_join");
	if (pthread_create(&thread, NULL, cancel_worker, NULL) != 0 ||
	    pthread_cancel(thread) != 0 ||
	    pthread_join(thread, &thread_result) != 0 ||
	    thread_result != PTHREAD_CANCELED || cleanup_called != 1)
		return fail("pthread_cancel");
	if (pthread_attr_init(&detached_attributes) != 0 ||
	    pthread_attr_setdetachstate(&detached_attributes,
	    PTHREAD_CREATE_DETACHED) != 0 ||
	    pthread_create(&thread, &detached_attributes,
	    detached_worker, NULL) != 0 || sem_wait(&detached_ready) != 0)
		return fail("pthread_detach");
	(void)pthread_attr_destroy(&detached_attributes);
	if (pthread_atfork(atfork_prepare, atfork_parent, atfork_child) != 0)
		return fail("pthread_atfork-register");
	child = fork();
	if (child < 0)
		return fail_errno("pthread-fork");
	if (child == 0)
		_exit(atfork_prepare_called == 1 && atfork_child_called == 1 &&
		    atfork_parent_called == 0 ? 0 : 41);
	memset(&child_information, 0, sizeof(child_information));
	if (waitid(P_PID, (id_t)child, &child_information, WEXITED) != 0 ||
	    child_information.si_pid != child ||
	    child_information.si_code != CLD_EXITED ||
	    child_information.si_status != 0 ||
	    atfork_prepare_called != 1 || atfork_parent_called != 1 ||
	    atfork_child_called != 0)
		return fail("pthread-atfork");
	if (pipe(pipefd) != 0)
		return fail("pipe");
	event.fd = pipefd[0]; event.events = POLLIN; event.revents = 0;
	poll_result = poll(&event, 1, 0);
	if (poll_result < 0)
		return fail_errno("poll-empty-error");
	if (poll_result != 0)
		return fail((event.revents & POLLHUP) != 0 ?
		    "poll-empty-hup" : "poll-empty-ready");
	if (write(pipefd[1], "p", 1) != 1 || poll(&event, 1, 1000) != 1 ||
	    (event.revents & POLLIN) == 0 || read(pipefd[0], &byte, 1) != 1 ||
	    byte != 'p')
		return fail("poll-pipe");
	(void)close(pipefd[0]); (void)close(pipefd[1]);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		return fail("socketpair");
	if (send(pair[0], "u", 1, 0) != 1 || recv(pair[1], &byte, 1, 0) != 1 ||
	    byte != 'u')
		return fail("unix-stream");
	(void)close(pair[0]); (void)close(pair[1]);
	if (pipe(rights_pipe) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		return fail_errno("unix-rights-setup");
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	byte = 'f';
	vector.iov_base = &byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	((struct cmsghdr *)control.bytes)->cmsg_level = SOL_SOCKET;
	((struct cmsghdr *)control.bytes)->cmsg_type = SCM_RIGHTS;
	((struct cmsghdr *)control.bytes)->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA((struct cmsghdr *)control.bytes), &rights_pipe[1],
	    sizeof(int));
	if (sendmsg(pair[0], &message, 0) != 1)
		return fail_errno("unix-rights-send");
	(void)close(rights_pipe[1]);
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = &byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	if (recvmsg(pair[1], &message, 0) != 1 || byte != 'f' ||
	    message.msg_controllen < CMSG_LEN(sizeof(int)) ||
	    ((struct cmsghdr *)control.bytes)->cmsg_level != SOL_SOCKET ||
	    ((struct cmsghdr *)control.bytes)->cmsg_type != SCM_RIGHTS)
		return fail_errno("unix-rights-receive");
	memcpy(&received_fd, CMSG_DATA((struct cmsghdr *)control.bytes),
	    sizeof(received_fd));
	if (write(received_fd, "r", 1) != 1 ||
	    read(rights_pipe[0], &byte, 1) != 1 || byte != 'r')
		return fail_errno("unix-rights-use");
	(void)close(received_fd); (void)close(rights_pipe[0]);
	(void)close(pair[0]); (void)close(pair[1]);
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 ||
	    send(pair[0], "d", 1, 0) != 1 ||
	    recv(pair[1], &byte, 1, 0) != 1 || byte != 'd')
		return fail_errno("unix-dgram-pair");
	(void)close(pair[0]); (void)close(pair[1]);
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/posix-r2.sock");
	(void)unlink(local.sun_path);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	client = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0 || client < 0 ||
	    bind(listener, (struct sockaddr *)&local, sizeof(local)) != 0 ||
	    listen(listener, 2) != 0 ||
	    connect(client, (struct sockaddr *)&local, sizeof(local)) != 0 ||
	    (accepted = accept(listener, NULL, NULL)) < 0)
		return fail_errno("unix-path");
	if (write(client, "s", 1) != 1 || read(accepted, &byte, 1) != 1 ||
	    byte != 's')
		return fail_errno("unix-accept-stream");
	(void)close(accepted); (void)close(client); (void)close(listener);
	(void)unlink(local.sun_path);
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/posix-r2.dgram");
	(void)unlink(local.sun_path);
	datagram_server = socket(AF_UNIX, SOCK_DGRAM, 0);
	datagram_client = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (datagram_server < 0 || datagram_client < 0 ||
	    bind(datagram_server, (struct sockaddr *)&local, sizeof(local)) != 0 ||
	    sendto(datagram_client, "g", 1, 0,
	    (struct sockaddr *)&local, sizeof(local)) != 1 ||
	    recv(datagram_server, &byte, 1, 0) != 1 || byte != 'g')
		return fail_errno("unix-dgram-path");
	(void)close(datagram_client); (void)close(datagram_server);
	(void)unlink(local.sun_path);
	(void)mq_unlink("/posix-r2");
	queue = mq_open("/posix-r2", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
	if (queue == (mqd_t)-1)
		return fail_errno("mq_open");
	(void)sigemptyset(&notify_set);
	(void)sigaddset(&notify_set, SIGUSR1);
	if (pthread_sigmask(SIG_BLOCK, &notify_set, &old_mask) != 0)
		return fail("mqueue-notify-mask");
	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_SIGNAL;
	notification.sigev_signo = SIGUSR1;
	notification.sigev_value.sival_int = 73;
	if (mq_notify(queue, &notification) != 0 ||
	    mq_send(queue, "mq", 2, 7) != 0 ||
	    sigtimedwait(&notify_set, &notify_info, &signal_timeout) != SIGUSR1 ||
	    notify_info.si_value.sival_int != 73 ||
	    mq_receive(queue, mq_buffer, sizeof(mq_buffer), &mq_priority) != 2 ||
	    mq_buffer[0] != 'm' || mq_buffer[1] != 'q' || mq_priority != 7)
		return fail_errno("mqueue");
	(void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	(void)mq_close(queue);
	(void)mq_unlink("/posix-r2");
	directory_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 || fchdir(directory_fd) != 0 ||
	    getcwd(path_buffer, sizeof(path_buffer)) == NULL ||
	    strcmp(path_buffer, "/tmp") != 0 || chdir("/") != 0)
		return fail_errno("conformance-fchdir");
	directory = fdopendir(directory_fd);
	if (directory == NULL || dirfd(directory) != directory_fd ||
	    telldir(directory) < 0)
		return fail_errno("conformance-fdopendir");
	rewinddir(directory);
	(void)readdir(directory);
	(void)closedir(directory);
	regular_fd = creat("/tmp/posix-r2-file", 0600);
	if (regular_fd < 0) return fail_errno("conformance-creat");
	(void)close(regular_fd);
	regular_fd = open("/tmp/posix-r2-file", O_RDWR);
	file_vectors[0].iov_base = (void *)"ab";
	file_vectors[0].iov_len = 2;
	file_vectors[1].iov_base = (void *)"cd";
	file_vectors[1].iov_len = 2;
	if (regular_fd < 0 || pwritev(regular_fd, file_vectors, 2, 0) != 4)
		return fail_errno("conformance-pwritev");
	memset(file_buffer, 0, sizeof(file_buffer));
	file_vectors[0].iov_base = file_buffer;
	file_vectors[1].iov_base = file_buffer + 2;
	if (preadv(regular_fd, file_vectors, 2, 0) != 4 ||
	    memcmp(file_buffer, "abcd", 4) != 0)
		return fail_errno("conformance-preadv");
	(void)close(regular_fd);
	(void)unlink("/tmp/posix-r2-file");
	if (uname(&uts) != 0 || strcmp(uts.sysname, "zedBSD") != 0 ||
	    getlogin_r(login_buffer, sizeof(login_buffer)) != 0 ||
	    strcmp(login_buffer, "root") != 0 ||
	    ttyname_r(1, tty_buffer, sizeof(tty_buffer)) != 0 ||
	    strcmp(tty_buffer, "/dev/console") != 0)
		return fail_errno("conformance-identity");
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
	    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL) != 0)
		return fail_errno("conformance-clock-nanosleep");
	(void)sigemptyset(&notify_set);
	(void)sigaddset(&notify_set, SIGALRM);
	if (pthread_sigmask(SIG_BLOCK, &notify_set, &old_mask) != 0)
		return fail("timer-signal-mask");
	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_SIGNAL;
	notification.sigev_signo = SIGALRM;
	notification.sigev_value.sival_int = 91;
	memset(&timer_value, 0, sizeof(timer_value));
	timer_value.it_value.tv_nsec = 20000000L;
	if (timer_create(CLOCK_MONOTONIC, &notification, &process_timer) != 0)
		return fail_errno("process-timer-create");
	if (timer_settime(process_timer, 0, &timer_value, NULL) != 0)
		return fail_errno("process-timer-settime");
	if (sigtimedwait(&notify_set, &notify_info, &signal_timeout) != SIGALRM ||
	    notify_info.si_code != SI_TIMER || notify_info.si_value.sival_int != 91)
		return fail_errno("process-timer-signal");
	if (timer_gettime(process_timer, &timer_current) != 0 ||
	    timer_current.it_value.tv_sec != 0 ||
	    timer_current.it_value.tv_nsec != 0)
		return fail_errno("process-timer-gettime");
	if (timer_getoverrun(process_timer) != 0)
		return fail_errno("process-timer-overrun");
	if (timer_delete(process_timer) != 0)
		return fail_errno("process-timer-delete");
	(void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	if (statvfs("/", &filesystem_status) != 0 ||
	    filesystem_status.f_bsize == 0 || filesystem_status.f_namemax == 0)
		return fail_errno("statvfs-root");
	if (mkdir("/mnt", 0755) != 0 && errno != EEXIST)
		return fail_errno("mount-mkdir");
	if (mount("tmpfs", "/mnt", 0, NULL) != 0 ||
	    statvfs("/mnt", &filesystem_status) != 0 ||
	    filesystem_status.f_blocks == 0 || filesystem_status.f_bfree == 0)
		return fail_errno("mount-tmpfs");
	directory_fd = open("/mnt", O_RDONLY | O_DIRECTORY);
	if (directory_fd < 0 || fstatvfs(directory_fd, &descriptor_status) != 0 ||
	    descriptor_status.f_bsize != filesystem_status.f_bsize)
		return fail_errno("fstatvfs-tmpfs");
	errno = 0;
	if (unmount("/mnt", 0) != -1 || errno != EBUSY)
		return fail("unmount-busy");
	(void)close(directory_fd);
	if (unmount("/mnt", 0) != 0 || rmdir("/mnt") != 0)
		return fail_errno("unmount-tmpfs");
	pty_master = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty_master < 0 || ptsname_r(pty_master, path_buffer,
	    sizeof(path_buffer)) != 0)
		return fail_errno("ptmx-open");
	errno = 0;
	if (open(path_buffer, O_RDWR | O_NOCTTY) != -1 || errno != EACCES)
		return fail("ptmx-lock");
	if (grantpt(pty_master) != 0 || unlockpt(pty_master) != 0 ||
	    (pty_slave = open(path_buffer, O_RDWR | O_NOCTTY)) < 0)
		return fail_errno("pty-slave-open");
	if (write(pty_master, "pty\n", 4) != 4 ||
	    read(pty_slave, file_buffer, 4) != 4 ||
	    memcmp(file_buffer, "pty\n", 4) != 0 ||
	    read(pty_master, file_buffer, 5) != 5 ||
	    memcmp(file_buffer, "pty\r\n", 5) != 0)
		return fail_errno("pty-input-echo");
	if (write(pty_slave, "out\n", 4) != 4 ||
	    read(pty_master, file_buffer, 5) != 5 ||
	    memcmp(file_buffer, "out\r\n", 5) != 0)
		return fail_errno("pty-output");
	memset(pty_stress_input, 'q', sizeof(pty_stress_input));
	child = fork();
	if (child < 0)
		return fail_errno("pty-backpressure-fork");
	if (child == 0)
		_exit(write(pty_slave, pty_stress_input,
		    sizeof(pty_stress_input)) == (ssize_t)sizeof(pty_stress_input) ?
		    0 : 42);
	pty_received = 0;
	while (pty_received < sizeof(pty_stress_output)) {
		ssize_t count = read(pty_master, pty_stress_output + pty_received,
		    sizeof(pty_stress_output) - pty_received);
		if (count <= 0)
			return fail_errno("pty-backpressure-read");
		pty_received += (size_t)count;
	}
	if (memcmp(pty_stress_input, pty_stress_output,
	    sizeof(pty_stress_input)) != 0 ||
	    waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
		return fail_errno("pty-backpressure-data");
	(void)close(pty_slave);
	if (read(pty_master, file_buffer, sizeof(file_buffer)) != 0)
		return fail_errno("pty-hangup");
	(void)close(pty_master);
	if (setlocale(LC_ALL, "C.UTF-8") == NULL || MB_CUR_MAX != 4U ||
	    strcmp(nl_langinfo(CODESET), "UTF-8") != 0)
		return fail_errno("locale-utf8");
	memset(&multibyte_state, 0, sizeof(multibyte_state));
	if (mbrtowc(&wide_character, "\xe2", 1, &multibyte_state) !=
	    (size_t)-2 || mbrtowc(&wide_character, "\x82\xac", 2,
	    &multibyte_state) != 2 || wide_character != (wchar_t)0x20acU)
		return fail_errno("locale-split-utf8");
	memset(&multibyte_state, 0, sizeof(multibyte_state));
	errno = 0;
	if (mbrtowc(&wide_character, "\xc0\x80", 2, &multibyte_state) !=
	    (size_t)-1 || errno != EILSEQ)
		return fail("locale-overlong");
	if (wcrtomb(encoded, (wchar_t)0x3042U, NULL) != 3 ||
	    (unsigned char)encoded[0] != 0xe3U || wcwidth((wchar_t)0x3042U) != 2 ||
	    !iswalpha((wint_t)0x3042U))
		return fail_errno("locale-wide");
	c_locale = newlocale(LC_ALL_MASK, "C", NULL);
	if (c_locale == NULL || pthread_create(&thread, NULL, locale_worker,
	    c_locale) != 0 || pthread_join(thread, &thread_result) != 0 ||
	    thread_result != NULL || MB_CUR_MAX != 4U)
		return fail_errno("locale-thread");
	freelocale(c_locale);
	(void)write(1, "R2:TIMER:PASS\n", 14);
	/* WNOWAIT must report without consuming the child event. */
	child = fork();
	if (child < 0) return fail_errno("waitid-nowait-fork");
	if (child == 0) _exit(19);
	memset(&child_information, 0, sizeof(child_information));
	if (waitid(P_PID, (id_t)child, &child_information,
	    WEXITED | WNOWAIT) != 0 || child_information.si_pid != child ||
	    child_information.si_status != 19 ||
	    waitid(P_PID, (id_t)child, &child_information,
	    WEXITED | WNOWAIT) != 0 || waitpid(child, &child_status, 0) != child ||
	    WEXITSTATUS(child_status) != 19)
		return fail_errno("waitid-nowait");
	/* tmpfs special nodes and POSIX process-owned byte-range locks. */
	(void)unlink("/tmp/posix-r2-fifo");
	if (mkfifo("/tmp/posix-r2-fifo", 0600) != 0 ||
	    (pipefd[0] = open("/tmp/posix-r2-fifo",
	    O_RDONLY | O_NONBLOCK)) < 0 ||
	    (pipefd[1] = open("/tmp/posix-r2-fifo",
	    O_WRONLY | O_NONBLOCK)) < 0 || write(pipefd[1], "n", 1) != 1 ||
	    read(pipefd[0], &byte, 1) != 1 || byte != 'n')
		return fail_errno("named-fifo");
	(void)close(pipefd[0]); (void)close(pipefd[1]);
	(void)unlink("/tmp/posix-r2-fifo");
	regular_fd = open("/tmp/posix-r2-lock", O_CREAT | O_RDWR, 0600);
	memset(&file_lock, 0, sizeof(file_lock));
	file_lock.l_type = F_WRLCK; file_lock.l_whence = SEEK_SET;
	file_lock.l_len = 1;
	if (regular_fd < 0 || fcntl(regular_fd, F_SETLK, &file_lock) != 0)
		return fail_errno("record-lock-parent");
	child = fork();
	if (child < 0) return fail_errno("record-lock-fork");
	if (child == 0) {
		int locked = fcntl(regular_fd, F_SETLK, &file_lock);
		_exit(locked == -1 && (errno == EAGAIN || errno == EACCES) ? 0 : 1);
	}
	if (waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
		return fail_errno("record-lock-conflict");
	file_lock.l_type = F_UNLCK;
	if (fcntl(regular_fd, F_SETLK, &file_lock) != 0)
		return fail_errno("record-unlock");
	(void)close(regular_fd); (void)unlink("/tmp/posix-r2-lock");
	/* Lowering NOFILE leaves existing descriptors valid but blocks new ones. */
	if (getrlimit(RLIMIT_NOFILE, &saved_limit) != 0)
		return fail_errno("getrlimit-nofile");
	small_limit = saved_limit; small_limit.rlim_cur = 3;
	if (setrlimit(RLIMIT_NOFILE, &small_limit) != 0 ||
	    open("/tmp/limit-must-fail", O_CREAT | O_RDWR, 0600) != -1 ||
	    errno != EMFILE || setrlimit(RLIMIT_NOFILE, &saved_limit) != 0)
		return fail_errno("setrlimit-nofile");
	(void)unlink("/tmp/limit-must-fail");
	if (posix_spawn(&child, "/bin/sh", NULL, NULL, exec_argv,
	    spawn_envp) != 0 || waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 23)
		return fail_errno("conformance-posix-spawn");
	(void)sem_destroy(&ready);
	(void)sem_destroy(&cancel_wait);
	(void)sem_destroy(&detached_ready);
	(void)pthread_barrier_destroy(&barrier);
	(void)pthread_spin_destroy(&spin);
	(void)execve(exec_argv[0], exec_argv, exec_envp);
	return fail_errno("pthread-exec");
}
