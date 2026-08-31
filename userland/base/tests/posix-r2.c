/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD posix r2 userland behavior.
 */

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
#include <limits.h>
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
#include <termios.h>
#include <wchar.h>
#include <wctype.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t barrier;
static pthread_spinlock_t spin;
static sem_t ready;
static sem_t cancel_wait;
static sem_t detached_ready;
static sem_t timer_ready;
static int shared_value;
static int spin_value;
static int cleanup_called;
static int atfork_prepare_called;
static int atfork_parent_called;
static int atfork_child_called;
static volatile int timer_callback_value;
static volatile unsigned timer_callback_bits;
#define PTY_STRESS_SIZE 6000U
static char pty_stress_input[PTY_STRESS_SIZE];
static char pty_stress_output[PTY_STRESS_SIZE];

static int fail(const char *name);
static int fail_errno(const char *name);
static int same_signal_action(const struct sigaction *left, const struct sigaction *right);
static int test_pty_line_discipline(int master, int slave);
static void atfork_prepare(void);
static void atfork_parent(void);
static void atfork_child(void);
static void *detached_worker(void *argument);
static void thread_timer_callback(union sigval value);
static void public_realtime_handler(int signo);
static void cancel_cleanup(void *argument);
static void *cancel_worker(void *argument);
static void *worker(void *argument);
static void *locale_worker(void *argument);

/*
 * Runs the tests command.
 */
int
main(
	void)
{
	int function_result;
	size_t amount;
	ssize_t sent;
	ssize_t received;
	union sigval probe_value;
	union sigval queued;
	ssize_t count;
	int locked;
	char *exec_argv[] = {"/bin/sh", NULL};
	char *exec_envp[] = {"R2_EXEC_FINAL=1", NULL};
	char *spawn_envp[] = {"R2_SPAWN_CHILD=1", NULL};
	pthread_t thread;
	pthread_attr_t detached_attributes;
	pthread_attr_t timer_attributes;
	void *thread_result;
	struct pollfd event;
	int pipefd[2], pair[2], listener, client, accepted, poll_result;
	int pty_master, pty_slave;
	int datagram_server, datagram_client;
	int rights_pipe[2], received_fd;
	struct msghdr message;
	struct iovec vector;

	thread_result = NULL;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct sockaddr_un local;
	char byte = 0;
	char mq_buffer[256];
	char stream_buffer[8192], stream_readback[8192];
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
	struct timespec signal_timeout = {1, 0};
	struct rlimit saved_limit, small_limit;
	struct statvfs filesystem_status, descriptor_status;
	struct flock file_lock;
	size_t pty_received;
	struct sigevent notification;
	struct itimerspec timer_value, timer_current;
	timer_t process_timer, second_timer;
	sigset_t notify_set, old_mask;
	sigset_t realtime_set, realtime_old_mask;
	sigset_t public_top_set, public_top_old_mask;
	sigset_t public_top_expected_mask, public_top_observed_mask;
	siginfo_t notify_info;
	struct sigaction public_top_install, public_top_saved;
	struct sigaction public_top_expected, public_top_observed;
	char path_buffer[64], file_buffer[8], login_buffer[16], tty_buffer[32];
	mbstate_t multibyte_state;
	wchar_t wide_character;
	char encoded[4];
	locale_t c_locale;
	size_t stream_queued, stream_drained, stream_index;
	socklen_t option_length;
	int option_value, saved_flags;
	int realtime_offset, realtime_round;

	/* Handles a failed getenv operation. */
	if (getenv("R2_EXEC_FINAL") != NULL) {
		(void)write(1, "R2:01-06:PASS\n", 15);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed getenv operation. */
	if (getenv("R2_SPAWN_CHILD") != NULL)
		return 23;

	/* Handles a failed sem init operation. */
	if (sem_init(&ready, 0, 0) != 0) {
		/* Obtains the fail result. */
		function_result = fail("sem_init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sem init operation. */
	if (sem_init(&cancel_wait, 0, 0) != 0) {
		/* Obtains the fail result. */
		function_result = fail("cancel-sem-init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sem init operation. */
	if (sem_init(&detached_ready, 0, 0) != 0) {
		/* Obtains the fail result. */
		function_result = fail("detached-sem-init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sem init operation. */
	if (sem_init(&timer_ready, 0, 0) != 0) {
		/* Obtains the fail result. */
		function_result = fail("timer-sem-init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sysconf operation. */
	if (sizeof(sigset_t) != 8U || RTSIG_MAX < 8 ||
	    sysconf(_SC_RTSIG_MAX) != RTSIG_MAX ||
	    sysconf(_SC_SIGQUEUE_MAX) < _POSIX_SIGQUEUE_MAX) {
		/* Obtains the fail result. */
		function_result = fail("realtime-signal-capacity");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread barrier init operation. */
	if (pthread_barrier_init(&barrier, NULL, 2) != 0) {
		/* Obtains the fail result. */
		function_result = fail("barrier_init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread spin init operation. */
	if (pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) != 0) {
		/* Obtains the fail result. */
		function_result = fail("spin_init");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread create operation. */
	if (pthread_create(&thread, NULL, worker, NULL) != 0) {
		/* Obtains the fail result. */
		function_result = fail("pthread_create");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sem wait operation. */
	if (sem_wait(&ready) != 0) {
		/* Obtains the fail result. */
		function_result = fail("sem_wait");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_barrier_wait(&barrier);

	/* Handles a failed pthread join operation. */
	if (pthread_join(thread, &thread_result) != 0 ||
	    thread_result != (void *)7 || shared_value != 43 || spin_value != 1) {
		/* Obtains the fail result. */
		function_result = fail("pthread_join");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread create operation. */
	if (pthread_create(&thread, NULL, cancel_worker, NULL) != 0 ||
	    pthread_cancel(thread) != 0 ||
	    pthread_join(thread, &thread_result) != 0 ||
	    thread_result != PTHREAD_CANCELED || cleanup_called != 1) {
		/* Obtains the fail result. */
		function_result = fail("pthread_cancel");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pthread attr init operation. */
	if (pthread_attr_init(&detached_attributes) != 0 ||
	    pthread_attr_setdetachstate(&detached_attributes,
					PTHREAD_CREATE_DETACHED) != 0 ||
	    pthread_create(&thread, &detached_attributes, detached_worker,
			   NULL) != 0 ||
	    sem_wait(&detached_ready) != 0) {
		/* Obtains the fail result. */
		function_result = fail("pthread_detach");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_attr_destroy(&detached_attributes);

	/* Handles a failed pthread atfork operation. */
	if (pthread_atfork(atfork_prepare, atfork_parent, atfork_child) != 0) {
		/* Obtains the fail result. */
		function_result = fail("pthread_atfork-register");

		/* Returns the computed result. */
		return function_result;
	}

	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pthread-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0)
		_exit(atfork_prepare_called == 1 && atfork_child_called == 1 &&
			      atfork_parent_called == 0
			  ? 0
			  : 41);
	memset(&child_information, 0, sizeof(child_information));

	/* Handles a failed waitid operation. */
	if (waitid(P_PID, (id_t)child, &child_information, WEXITED) != 0 ||
	    child_information.si_pid != child ||
	    child_information.si_code != CLD_EXITED ||
	    child_information.si_status != 0 || atfork_prepare_called != 1 ||
	    atfork_parent_called != 1 || atfork_child_called != 0) {
		/* Obtains the fail result. */
		function_result = fail("pthread-atfork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pipe operation. */
	if (pipe(pipefd) != 0) {
		/* Obtains the fail result. */
		function_result = fail("pipe");

		/* Returns the computed result. */
		return function_result;
	}
	event.fd = pipefd[0];
	event.events = POLLIN;
	event.revents = 0;
	poll_result = poll(&event, 1, 0);

	/* Handles the poll result condition. */
	if (poll_result < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("poll-empty-error");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the poll result condition. */
	if (poll_result != 0) {
		/* Obtains the fail result. */
		function_result = fail((event.revents & POLLHUP) != 0
				? "poll-empty-hup"
				: "poll-empty-ready");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed write operation. */
	if (write(pipefd[1], "p", 1) != 1 || poll(&event, 1, 1000) != 1 ||
	    (event.revents & POLLIN) == 0 || read(pipefd[0], &byte, 1) != 1 ||
	    byte != 'p') {
		/* Obtains the fail result. */
		function_result = fail("poll-pipe");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pipefd[0]);
	(void)close(pipefd[1]);

	/* Handles a failed socketpair operation. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		/* Obtains the fail result. */
		function_result = fail("socketpair");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed send operation. */
	if (send(pair[0], "u", 1, 0) != 1 || recv(pair[1], &byte, 1, 0) != 1 ||
	    byte != 'u') {
		/* Obtains the fail result. */
		function_result = fail("unix-stream");

		/* Returns the computed result. */
		return function_result;
	}

	option_length = sizeof(option_value);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &option_value,
		       &option_length) != 0 ||
	    option_length != sizeof(option_value) || option_value != 65536) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-default-sndbuf");

		/* Returns the computed result. */
		return function_result;
	}

	option_length = sizeof(option_value);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(pair[1], SOL_SOCKET, SO_RCVBUF, &option_value,
		       &option_length) != 0 ||
	    option_length != sizeof(option_value) || option_value != 65536) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-default-rcvbuf");

		/* Returns the computed result. */
		return function_result;
	}

	option_value = 4096;

	/* Handles a failed setsockopt operation. */
	if (setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &option_value,
		       sizeof(option_value)) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-set-sndbuf");

		/* Returns the computed result. */
		return function_result;
	}

	option_value = 0;
	option_length = sizeof(option_value);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &option_value,
		       &option_length) != 0 ||
	    option_value != 4096) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-get-sndbuf");

		/* Returns the computed result. */
		return function_result;
	}

	option_value = 65536;

	/* Handles a failed setsockopt operation. */
	if (setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &option_value,
		       sizeof(option_value)) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-restore-sndbuf");

		/* Returns the computed result. */
		return function_result;
	}

	option_value = 1;
	errno = 0;

	/* Handles the reported system error. */
	if (setsockopt(pair[0], SOL_SOCKET, SO_RCVBUF, &option_value,
		       sizeof(option_value)) != -1 ||
	    errno != EINVAL) {
		/* Obtains the fail result. */
		function_result = fail("unix-stream-invalid-rcvbuf");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (stream_index = 0; stream_index < 16; stream_index++)

		/* Handles a failed send operation. */
		if (send(pair[0], "w", 1, 0) != 1) {
			/* Obtains the fail errno result. */
			function_result = fail_errno("unix-stream-many-writes");

			/* Returns the computed result. */
			return function_result;
		}

	/* Handles a failed recv operation. */
	if (recv(pair[1], stream_readback, 16, MSG_WAITALL) != 16) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-many-writes-receive");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (stream_index = 0; stream_index < sizeof(stream_buffer);
	     stream_index++)
		stream_buffer[stream_index] = (char)(stream_index * 29U + 7U);

	/* Handles a failed send operation. */
	if (send(pair[0], stream_buffer, sizeof(stream_buffer), 0) !=
		(ssize_t)sizeof(stream_buffer) ||
	    recv(pair[1], stream_readback, sizeof(stream_readback),
		 MSG_WAITALL) != (ssize_t)sizeof(stream_readback) ||
	    memcmp(stream_buffer, stream_readback, sizeof(stream_buffer)) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-fragmentation");

		/* Returns the computed result. */
		return function_result;
	}

	saved_flags = fcntl(pair[0], F_GETFL, 0);

	/* Handles a failed fcntl operation. */
	if (saved_flags < 0 ||
	    fcntl(pair[0], F_SETFL, saved_flags | O_NONBLOCK) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-nonblock");

		/* Returns the computed result. */
		return function_result;
	}

	/* Continue while the operation condition remains true. */
	stream_queued = 0;
	while (stream_queued < 65536U) {

		amount = 65536U - stream_queued;

		/* Handles the amount condition. */
		if (amount > sizeof(stream_buffer))
			amount = sizeof(stream_buffer);
		sent = send(pair[0], stream_buffer, amount, 0);

		/* Handles the sent condition. */
		if (sent <= 0) {
			/* Obtains the fail errno result. */
			function_result = fail_errno("unix-stream-fill");

			/* Returns the computed result. */
			return function_result;
		}
		stream_queued += (size_t)sent;
	}
	errno = 0;

	/* Handles the reported system error. */
	if (send(pair[0], "x", 1, 0) != -1 || errno != EAGAIN) {
		/* Obtains the fail result. */
		function_result = fail("unix-stream-full-eagain");

		/* Returns the computed result. */
		return function_result;
	}
	event.fd = pair[0];
	event.events = POLLOUT;
	event.revents = 0;

	/* Handles a failed poll operation. */
	if (poll(&event, 1, 0) != 0) {
		/* Obtains the fail result. */
		function_result = fail("unix-stream-full-poll");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed recv operation. */
	if (recv(pair[1], &byte, 1, 0) != 1) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-stream-release-byte");

		/* Returns the computed result. */
		return function_result;
	}
	event.revents = 0;

	/* Handles a failed poll operation. */
	if (poll(&event, 1, 0) != 1 || (event.revents & POLLOUT) == 0) {
		/* Obtains the fail result. */
		function_result = fail("unix-stream-writable-poll");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[0]);

	/* Continue until the operation reaches a terminal state. */
	stream_drained = 1;
	for (;;) {

		received = recv(pair[1], stream_readback, sizeof(stream_readback), 0);

		/* Handles the received condition. */
		if (received < 0) {
			/* Obtains the fail errno result. */
			function_result = fail_errno("unix-stream-drain");

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the received condition. */
		if (received == 0)
			break;
		stream_drained += (size_t)received;
	}

	/* Handles the stream drained condition. */
	if (stream_drained != 65536U) {
		/* Obtains the fail result. */
		function_result = fail("unix-stream-eof-count");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[1]);

	/* Handles a failed pipe operation. */
	if (pipe(rights_pipe) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-rights-setup");

		/* Returns the computed result. */
		return function_result;
	}

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

	/* Handles a failed sendmsg operation. */
	if (sendmsg(pair[0], &message, 0) != 1) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-rights-send");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(rights_pipe[1]);
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = &byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);

	/* Handles a failed recvmsg operation. */
	if (recvmsg(pair[1], &message, 0) != 1 || byte != 'f' ||
	    message.msg_controllen < CMSG_LEN(sizeof(int)) ||
	    ((struct cmsghdr *)control.bytes)->cmsg_level != SOL_SOCKET ||
	    ((struct cmsghdr *)control.bytes)->cmsg_type != SCM_RIGHTS) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-rights-receive");

		/* Returns the computed result. */
		return function_result;
	}

	memcpy(&received_fd, CMSG_DATA((struct cmsghdr *)control.bytes),
	       sizeof(received_fd));

	/* Handles a failed write operation. */
	if (write(received_fd, "r", 1) != 1 ||
	    read(rights_pipe[0], &byte, 1) != 1 || byte != 'r') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-rights-use");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(received_fd);
	(void)close(rights_pipe[0]);
	(void)close(pair[0]);
	(void)close(pair[1]);

	/* Handles a failed socketpair operation. */
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 ||
	    send(pair[0], "d", 1, 0) != 1 || recv(pair[1], &byte, 1, 0) != 1 ||
	    byte != 'd') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-dgram-pair");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pair[0]);
	(void)close(pair[1]);
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/posix-r2.sock");
	(void)unlink(local.sun_path);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	client = socket(AF_UNIX, SOCK_STREAM, 0);

	/* Handles a failed bind operation. */
	if (listener < 0 || client < 0 ||
	    bind(listener, (struct sockaddr *)&local, sizeof(local)) != 0 ||
	    listen(listener, 2) != 0 ||
	    connect(client, (struct sockaddr *)&local, sizeof(local)) != 0 ||
	    (accepted = accept(listener, NULL, NULL)) < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-path");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed write operation. */
	if (write(client, "s", 1) != 1 || read(accepted, &byte, 1) != 1 ||
	    byte != 's') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-accept-stream");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(accepted);
	(void)close(client);
	(void)close(listener);
	(void)unlink(local.sun_path);
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/posix-r2.dgram");
	(void)unlink(local.sun_path);
	datagram_server = socket(AF_UNIX, SOCK_DGRAM, 0);
	datagram_client = socket(AF_UNIX, SOCK_DGRAM, 0);

	/* Handles a failed bind operation. */
	if (datagram_server < 0 || datagram_client < 0 ||
	    bind(datagram_server, (struct sockaddr *)&local, sizeof(local)) !=
		0 ||
	    sendto(datagram_client, "g", 1, 0, (struct sockaddr *)&local,
		   sizeof(local)) != 1 ||
	    recv(datagram_server, &byte, 1, 0) != 1 || byte != 'g') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unix-dgram-path");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(datagram_client);
	(void)close(datagram_server);
	(void)unlink(local.sun_path);
	(void)mq_unlink("/posix-r2");
	queue = mq_open("/posix-r2", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);

	/* Handles the queue condition. */
	if (queue == (mqd_t)-1) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("mq_open");

		/* Returns the computed result. */
		return function_result;
	}
	(void)sigemptyset(&notify_set);
	(void)sigaddset(&notify_set, SIGUSR1);

	/* Handles a failed pthread sigmask operation. */
	if (pthread_sigmask(SIG_BLOCK, &notify_set, &old_mask) != 0) {
		/* Obtains the fail result. */
		function_result = fail("mqueue-notify-mask");

		/* Returns the computed result. */
		return function_result;
	}

	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_SIGNAL;
	notification.sigev_signo = SIGUSR1;
	notification.sigev_value.sival_int = 73;

	/* Handles a failed mq notify operation. */
	if (mq_notify(queue, &notification) != 0 ||
	    mq_send(queue, "mq", 2, 7) != 0 ||
	    sigtimedwait(&notify_set, &notify_info, &signal_timeout) !=
		SIGUSR1 ||
	    notify_info.si_value.sival_int != 73 ||
	    mq_receive(queue, mq_buffer, sizeof(mq_buffer), &mq_priority) !=
		2 ||
	    mq_buffer[0] != 'm' || mq_buffer[1] != 'q' || mq_priority != 7) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("mqueue");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	(void)mq_close(queue);
	(void)mq_unlink("/posix-r2");
	directory_fd = open("/tmp", O_RDONLY | O_DIRECTORY);

	/* Handles a failed fchdir operation. */
	if (directory_fd < 0 || fchdir(directory_fd) != 0 ||
	    getcwd(path_buffer, sizeof(path_buffer)) == NULL ||
	    strcmp(path_buffer, "/tmp") != 0 || chdir("/") != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-fchdir");

		/* Returns the computed result. */
		return function_result;
	}

	directory = fdopendir(directory_fd);

	/* Handles a failed dirfd operation. */
	if (directory == NULL || dirfd(directory) != directory_fd ||
	    telldir(directory) < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-fdopendir");

		/* Returns the computed result. */
		return function_result;
	}

	rewinddir(directory);
	(void)readdir(directory);
	(void)closedir(directory);
	regular_fd = creat("/tmp/posix-r2-file", 0600);

	/* Handles the regular fd condition. */
	if (regular_fd < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-creat");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(regular_fd);
	regular_fd = open("/tmp/posix-r2-file", O_RDWR);
	file_vectors[0].iov_base = (void *)"ab";
	file_vectors[0].iov_len = 2;
	file_vectors[1].iov_base = (void *)"cd";
	file_vectors[1].iov_len = 2;

	/* Handles a failed pwritev operation. */
	if (regular_fd < 0 || pwritev(regular_fd, file_vectors, 2, 0) != 4) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-pwritev");

		/* Returns the computed result. */
		return function_result;
	}

	memset(file_buffer, 0, sizeof(file_buffer));
	file_vectors[0].iov_base = file_buffer;
	file_vectors[1].iov_base = file_buffer + 2;

	/* Handles a failed preadv operation. */
	if (preadv(regular_fd, file_vectors, 2, 0) != 4 ||
	    memcmp(file_buffer, "abcd", 4) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-preadv");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(regular_fd);
	(void)unlink("/tmp/posix-r2-file");

	/* Handles a failed uname operation. */
	if (uname(&uts) != 0 || strcmp(uts.sysname, "zedBSD") != 0 ||
	    getlogin_r(login_buffer, sizeof(login_buffer)) != 0 ||
	    strcmp(login_buffer, "root") != 0 ||
	    ttyname_r(1, tty_buffer, sizeof(tty_buffer)) != 0 ||
	    strcmp(tty_buffer, "/dev/console") != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-identity");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
	    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-clock-nanosleep");

		/* Returns the computed result. */
		return function_result;
	}
	(void)sigemptyset(&notify_set);
	(void)sigaddset(&notify_set, SIGALRM);

	/* Handles a failed pthread sigmask operation. */
	if (pthread_sigmask(SIG_BLOCK, &notify_set, &old_mask) != 0) {
		/* Obtains the fail result. */
		function_result = fail("timer-signal-mask");

		/* Returns the computed result. */
		return function_result;
	}

	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_SIGNAL;
	notification.sigev_signo = SIGALRM;
	notification.sigev_value.sival_int = 91;
	memset(&timer_value, 0, sizeof(timer_value));
	timer_value.it_value.tv_nsec = 20000000L;

	/* Handles a failed timer create operation. */
	if (timer_create(CLOCK_MONOTONIC, &notification, &process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-create");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed timer settime operation. */
	if (timer_settime(process_timer, 0, &timer_value, NULL) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-settime");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sigtimedwait operation. */
	if (sigtimedwait(&notify_set, &notify_info, &signal_timeout) !=
		SIGALRM ||
	    notify_info.si_code != SI_TIMER ||
	    notify_info.si_value.sival_int != 91) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-signal");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed timer gettime operation. */
	if (timer_gettime(process_timer, &timer_current) != 0 ||
	    timer_current.it_value.tv_sec != 0 ||
	    timer_current.it_value.tv_nsec != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-gettime");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed timer getoverrun operation. */
	if (timer_getoverrun(process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-overrun");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed timer delete operation. */
	if (timer_delete(process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("process-timer-delete");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);

	/*
 * Exercise eight distinct public realtime signals.  POSIX chooses the
	 * lowest pending realtime number first and preserves FIFO order between
	 * instances of the same number. */

	memset(&probe_value, 0, sizeof(probe_value));

	/* Handles a failed sigqueue operation. */
	if (sigqueue(getpid(), 0, probe_value) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("signal-zero-probe");

		/* Returns the computed result. */
		return function_result;
	}
	(void)sigemptyset(&realtime_set);

	/* Process each element required by the operation. */
	for (realtime_offset = 0; realtime_offset < 8; realtime_offset++)
		(void)sigaddset(&realtime_set, SIGRTMIN + realtime_offset);

	/* Handles a failed pthread sigmask operation. */
	if (pthread_sigmask(SIG_BLOCK, &realtime_set, &realtime_old_mask) != 0) {
		/* Obtains the fail result. */
		function_result = fail("realtime-signal-mask");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each element required by the operation. */
	for (realtime_offset = 7; realtime_offset >= 0; realtime_offset--)

		/* Process each element required by the operation. */
		for (realtime_round = 0; realtime_round < 2; realtime_round++) {

			queued.sival_int =
			    realtime_offset * 10 + realtime_round;

			/* Handles a failed sigqueue operation. */
			if (sigqueue(getpid(), SIGRTMIN + realtime_offset,
				     queued) != 0) {
				/* Obtains the fail errno result. */
				function_result = fail_errno("realtime-signal-queue");

				/* Returns the computed result. */
				return function_result;
			}
		}

	/* Process each element required by the operation. */
	for (realtime_offset = 0; realtime_offset < 8; realtime_offset++)

		/* Process each element required by the operation. */
		for (realtime_round = 0; realtime_round < 2; realtime_round++) {
			memset(&notify_info, 0, sizeof(notify_info));

			/* Handles a failed sigtimedwait operation. */
			if (sigtimedwait(&realtime_set, &notify_info,
					 &signal_timeout) !=
				SIGRTMIN + realtime_offset ||
			    notify_info.si_code != SI_QUEUE ||
			    notify_info.si_value.sival_int !=
				realtime_offset * 10 + realtime_round) {
				/* Obtains the fail errno result. */
				function_result = fail_errno("realtime-signal-fifo");

				/* Returns the computed result. */
				return function_result;
			}
		}
	(void)pthread_sigmask(SIG_SETMASK, &realtime_old_mask, NULL);

	/*
 * SIGEV_THREAD uses a libc-private number, never the public RT upper
	 * end. Preserve an application disposition and mask across all
	 * lifecycle paths. */
	memset(&public_top_install, 0, sizeof(public_top_install));
	public_top_install.sa_handler =
	    (uint64_t)(uintptr_t)public_realtime_handler;
	(void)sigemptyset(&public_top_install.sa_mask);
	(void)sigaddset(&public_top_install.sa_mask, SIGUSR1);

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGRTMAX, &public_top_install, &public_top_saved) != 0 ||
	    sigaction(SIGRTMAX, NULL, &public_top_expected) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-public-action-setup");

		/* Returns the computed result. */
		return function_result;
	}
	(void)sigemptyset(&public_top_set);
	(void)sigaddset(&public_top_set, SIGRTMAX);

	/* Handles a failed pthread sigmask operation. */
	if (pthread_sigmask(SIG_BLOCK, &public_top_set, &public_top_old_mask) !=
		0 ||
	    pthread_sigmask(SIG_SETMASK, NULL, &public_top_expected_mask) != 0) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-public-mask-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * SIGEV_THREAD is backed by a kernel SIGEV_SIGNAL timer, but all unsafe
	 * work is performed by libc's worker before this detached callback
	 * runs. */
	if (pthread_attr_init(&timer_attributes) != 0 ||
	    pthread_attr_setguardsize(&timer_attributes, 8192U) != 0) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-attributes");

		/* Returns the computed result. */
		return function_result;
	}

	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_THREAD;
	notification.sigev_value.sival_int = 117;
	notification.sigev_notify_function = thread_timer_callback;
	notification.sigev_notify_attributes = &timer_attributes;
	memset(&timer_value, 0, sizeof(timer_value));
	timer_value.it_value.tv_nsec = 20000000L;

	/* Handles a failed timer create operation. */
	if (timer_create(CLOCK_MONOTONIC, &notification, &process_timer) != 0 ||
	    (((uint32_t)process_timer & 0xffU) != 0) ||
	    timer_settime(process_timer, 0, &timer_value, NULL) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-create");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGRTMAX, NULL, &public_top_observed) != 0 ||
	    pthread_sigmask(SIG_SETMASK, NULL, &public_top_observed_mask) !=
		0 ||
	    !same_signal_action(&public_top_observed, &public_top_expected) ||
	    public_top_observed_mask != public_top_expected_mask) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-public-state-create");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_attr_destroy(&timer_attributes);

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-clock");

		/* Returns the computed result. */
		return function_result;
	}
	now.tv_sec += 2;

	/* Handles a failed sem timedwait operation. */
	if (sem_timedwait(&timer_ready, &now) != 0 ||
	    __atomic_load_n(&timer_callback_value, __ATOMIC_ACQUIRE) != 117 ||
	    timer_getoverrun(process_timer) < 0 ||
	    timer_delete(process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-callback");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGRTMAX, NULL, &public_top_observed) != 0 ||
	    pthread_sigmask(SIG_SETMASK, NULL, &public_top_observed_mask) !=
		0 ||
	    !same_signal_action(&public_top_observed, &public_top_expected) ||
	    public_top_observed_mask != public_top_expected_mask) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-public-state-delete");

		/* Returns the computed result. */
		return function_result;
	}

	errno = 0;

	/* Handles the reported system error. */
	if (timer_gettime(process_timer, &timer_current) != -1 ||
	    errno != EINVAL) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-stale-id");

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * Independent slots retain their callback values and each expiration
	 * gets a newly-created detached thread. */
	__atomic_store_n(&timer_callback_bits, 0, __ATOMIC_RELEASE);
	notification.sigev_value.sival_int = 1;

	/* Handles a failed timer create operation. */
	if (timer_create(CLOCK_MONOTONIC, &notification, &process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-multiple-first");

		/* Returns the computed result. */
		return function_result;
	}
	notification.sigev_value.sival_int = 2;

	/* Handles a failed timer create operation. */
	if (timer_create(CLOCK_MONOTONIC, &notification, &second_timer) != 0 ||
	    timer_settime(process_timer, 0, &timer_value, NULL) != 0 ||
	    timer_settime(second_timer, 0, &timer_value, NULL) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-multiple-second");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-multiple-clock");

		/* Returns the computed result. */
		return function_result;
	}
	now.tv_sec += 2;

	/* Handles a failed sem timedwait operation. */
	if (sem_timedwait(&timer_ready, &now) != 0 ||
	    sem_timedwait(&timer_ready, &now) != 0 ||
	    __atomic_load_n(&timer_callback_bits, __ATOMIC_ACQUIRE) !=
		((1U << 1) | (1U << 2)) ||
	    timer_delete(process_timer) != 0 || timer_delete(second_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-multiple-callback");

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * A libc timer handle must become invalid in the child while remaining
	 * usable in the parent; the worker is not inherited. */
	memset(&notification, 0, sizeof(notification));
	notification.sigev_notify = SIGEV_THREAD;
	notification.sigev_notify_function = thread_timer_callback;

	/* Handles a failed timer create operation. */
	if (timer_create(CLOCK_MONOTONIC, &notification, &process_timer) != 0 ||
	    (child = fork()) < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-fork-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
		errno = 0;
		_exit(timer_gettime(process_timer, &timer_current) == -1 &&
			      errno == EINVAL &&
			      sigaction(SIGRTMAX, NULL, &public_top_observed) ==
				  0 &&
			      pthread_sigmask(SIG_SETMASK, NULL,
					      &public_top_observed_mask) == 0 &&
			      same_signal_action(&public_top_observed,
						 &public_top_expected) &&
			      public_top_observed_mask ==
				  public_top_expected_mask
			  ? 0
			  : 46);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0 ||
	    timer_gettime(process_timer, &timer_current) != 0 ||
	    timer_delete(process_timer) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("thread-timer-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGRTMAX, NULL, &public_top_observed) != 0 ||
	    pthread_sigmask(SIG_SETMASK, NULL, &public_top_observed_mask) !=
		0 ||
	    !same_signal_action(&public_top_observed, &public_top_expected) ||
	    public_top_observed_mask != public_top_expected_mask) {
		/* Obtains the fail result. */
		function_result = fail("thread-timer-public-state-fork");

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_sigmask(SIG_SETMASK, &public_top_old_mask, NULL);
	(void)sigaction(SIGRTMAX, &public_top_saved, NULL);

	/* Handles a failed statvfs operation. */
	if (statvfs("/", &filesystem_status) != 0 ||
	    filesystem_status.f_bsize == 0 || filesystem_status.f_namemax == 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("statvfs-root");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the reported system error. */
	if (mkdir("/mnt", 0755) != 0 && errno != EEXIST) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("mount-mkdir");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed mount operation. */
	if (mount("tmpfs", "/mnt", 0, NULL) != 0 ||
	    statvfs("/mnt", &filesystem_status) != 0 ||
	    filesystem_status.f_blocks == 0 || filesystem_status.f_bfree == 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("mount-tmpfs");

		/* Returns the computed result. */
		return function_result;
	}

	directory_fd = open("/mnt", O_RDONLY | O_DIRECTORY);

	/* Handles a failed fstatvfs operation. */
	if (directory_fd < 0 ||
	    fstatvfs(directory_fd, &descriptor_status) != 0 ||
	    descriptor_status.f_bsize != filesystem_status.f_bsize) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("fstatvfs-tmpfs");

		/* Returns the computed result. */
		return function_result;
	}

	errno = 0;

	/* Handles the reported system error. */
	if (unmount("/mnt", 0) != -1 || errno != EBUSY) {
		/* Obtains the fail result. */
		function_result = fail("unmount-busy");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(directory_fd);

	/* Handles a failed unmount operation. */
	if (unmount("/mnt", 0) != 0 || rmdir("/mnt") != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("unmount-tmpfs");

		/* Returns the computed result. */
		return function_result;
	}

	pty_master = posix_openpt(O_RDWR | O_NOCTTY);

	/* Handles a failed ptsname r operation. */
	if (pty_master < 0 ||
	    ptsname_r(pty_master, path_buffer, sizeof(path_buffer)) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("ptmx-open");

		/* Returns the computed result. */
		return function_result;
	}

	errno = 0;

	/* Handles the reported system error. */
	if (open(path_buffer, O_RDWR | O_NOCTTY) != -1 || errno != EACCES) {
		/* Obtains the fail result. */
		function_result = fail("ptmx-lock");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed grantpt operation. */
	if (grantpt(pty_master) != 0 || unlockpt(pty_master) != 0 ||
	    (pty_slave = open(path_buffer, O_RDWR | O_NOCTTY)) < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-slave-open");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed write operation. */
	if (write(pty_master, "pty\n", 4) != 4 ||
	    read(pty_slave, file_buffer, 4) != 4 ||
	    memcmp(file_buffer, "pty\n", 4) != 0 ||
	    read(pty_master, file_buffer, 5) != 5 ||
	    memcmp(file_buffer, "pty\r\n", 5) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-input-echo");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed write operation. */
	if (write(pty_slave, "out\n", 4) != 4 ||
	    read(pty_master, file_buffer, 5) != 5 ||
	    memcmp(file_buffer, "out\r\n", 5) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-output");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed test pty line discipline operation. */
	if (test_pty_line_discipline(pty_master, pty_slave) != 0)
		return 1;
	memset(pty_stress_input, 'q', sizeof(pty_stress_input));
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-backpressure-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0)
		_exit(write(pty_slave, pty_stress_input,
			    sizeof(pty_stress_input)) ==
			      (ssize_t)sizeof(pty_stress_input)
			  ? 0
			  : 42);

	/* Process each remaining element. */
	pty_received = 0;
	while (pty_received < sizeof(pty_stress_output)) {

		count = read(pty_master, pty_stress_output + pty_received,
			 sizeof(pty_stress_output) - pty_received);

		/* Checks the remaining item count. */
		if (count <= 0) {
			/* Obtains the fail errno result. */
			function_result = fail_errno("pty-backpressure-read");

			/* Returns the computed result. */
			return function_result;
		}
		pty_received += (size_t)count;
	}

	/* Handles a failed waitpid operation. */
	if (memcmp(pty_stress_input, pty_stress_output,
		   sizeof(pty_stress_input)) != 0 ||
	    waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-backpressure-data");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pty_slave);

	/* Handles a failed read operation. */
	if (read(pty_master, file_buffer, sizeof(file_buffer)) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-hangup");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pty_master);

	/* Handles a failed setlocale operation. */
	if (setlocale(LC_ALL, "C.UTF-8") == NULL || MB_CUR_MAX != 4U ||
	    strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("locale-utf8");

		/* Returns the computed result. */
		return function_result;
	}

	memset(&multibyte_state, 0, sizeof(multibyte_state));

	/* Handles a failed mbrtowc operation. */
	if (mbrtowc(&wide_character, "\xe2", 1, &multibyte_state) !=
		(size_t)-2 ||
	    mbrtowc(&wide_character, "\x82\xac", 2, &multibyte_state) != 2 ||
	    wide_character != (wchar_t)0x20acU) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("locale-split-utf8");

		/* Returns the computed result. */
		return function_result;
	}

	memset(&multibyte_state, 0, sizeof(multibyte_state));
	errno = 0;

	/* Handles the reported system error. */
	if (mbrtowc(&wide_character, "\xc0\x80", 2, &multibyte_state) !=
		(size_t)-1 ||
	    errno != EILSEQ) {
		/* Obtains the fail result. */
		function_result = fail("locale-overlong");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed wcrtomb operation. */
	if (wcrtomb(encoded, (wchar_t)0x3042U, NULL) != 3 ||
	    (unsigned char)encoded[0] != 0xe3U ||
	    wcwidth((wchar_t)0x3042U) != 2 || !iswalpha((wint_t)0x3042U)) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("locale-wide");

		/* Returns the computed result. */
		return function_result;
	}

	c_locale = newlocale(LC_ALL_MASK, "C", NULL);

	/* Handles a failed pthread create operation. */
	if (c_locale == NULL ||
	    pthread_create(&thread, NULL, locale_worker, c_locale) != 0 ||
	    pthread_join(thread, &thread_result) != 0 ||
	    thread_result != NULL || MB_CUR_MAX != 4U) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("locale-thread");

		/* Returns the computed result. */
		return function_result;
	}

	freelocale(c_locale);
	(void)write(1, "R2:TIMER:PASS\n", 14);

	/* WNOWAIT must report without consuming the child event. */
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("waitid-nowait-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0)
		_exit(19);
	memset(&child_information, 0, sizeof(child_information));

	/* Handles a failed waitid operation. */
	if (waitid(P_PID, (id_t)child, &child_information, WEXITED | WNOWAIT) !=
		0 ||
	    child_information.si_pid != child ||
	    child_information.si_status != 19 ||
	    waitid(P_PID, (id_t)child, &child_information, WEXITED | WNOWAIT) !=
		0 ||
	    waitpid(child, &child_status, 0) != child ||
	    WEXITSTATUS(child_status) != 19) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("waitid-nowait");

		/* Returns the computed result. */
		return function_result;
	}

	/* tmpfs special nodes and POSIX process-owned byte-range locks. */
	(void)unlink("/tmp/posix-r2-fifo");

	/* Handles a failed mkfifo operation. */
	if (mkfifo("/tmp/posix-r2-fifo", 0600) != 0 ||
	    (pipefd[0] = open("/tmp/posix-r2-fifo", O_RDONLY | O_NONBLOCK)) <
		0 ||
	    (pipefd[1] = open("/tmp/posix-r2-fifo", O_WRONLY | O_NONBLOCK)) <
		0 ||
	    write(pipefd[1], "n", 1) != 1 || read(pipefd[0], &byte, 1) != 1 ||
	    byte != 'n') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("named-fifo");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(pipefd[0]);
	(void)close(pipefd[1]);
	(void)unlink("/tmp/posix-r2-fifo");
	regular_fd = open("/tmp/posix-r2-lock", O_CREAT | O_RDWR, 0600);
	memset(&file_lock, 0, sizeof(file_lock));
	file_lock.l_type = F_WRLCK;
	file_lock.l_whence = SEEK_SET;
	file_lock.l_len = 1;

	/* Handles a failed fcntl operation. */
	if (regular_fd < 0 || fcntl(regular_fd, F_SETLK, &file_lock) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("record-lock-parent");

		/* Returns the computed result. */
		return function_result;
	}

	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("record-lock-fork");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
				locked = fcntl(regular_fd, F_SETLK, &file_lock);
		_exit(locked == -1 && (errno == EAGAIN || errno == EACCES) ? 0
									   : 1);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("record-lock-conflict");

		/* Returns the computed result. */
		return function_result;
	}
	file_lock.l_type = F_UNLCK;

	/* Handles a failed fcntl operation. */
	if (fcntl(regular_fd, F_SETLK, &file_lock) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("record-unlock");

		/* Returns the computed result. */
		return function_result;
	}
	(void)close(regular_fd);
	(void)unlink("/tmp/posix-r2-lock");

	/*
 * Lowering NOFILE leaves existing descriptors valid but blocks new
	 * ones. */
	if (getrlimit(RLIMIT_NOFILE, &saved_limit) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("getrlimit-nofile");

		/* Returns the computed result. */
		return function_result;
	}

	small_limit = saved_limit;
	small_limit.rlim_cur = 3;

	/* Handles the reported system error. */
	if (setrlimit(RLIMIT_NOFILE, &small_limit) != 0 ||
	    open("/tmp/limit-must-fail", O_CREAT | O_RDWR, 0600) != -1 ||
	    errno != EMFILE || setrlimit(RLIMIT_NOFILE, &saved_limit) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("setrlimit-nofile");

		/* Returns the computed result. */
		return function_result;
	}
	(void)unlink("/tmp/limit-must-fail");

	/* Handles a failed posix spawn operation. */
	if (posix_spawn(&child, "/bin/sh", NULL, NULL, exec_argv, spawn_envp) !=
		0 ||
	    waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 23) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("conformance-posix-spawn");

		/* Returns the computed result. */
		return function_result;
	}
	(void)sem_destroy(&ready);
	(void)sem_destroy(&cancel_wait);
	(void)sem_destroy(&detached_ready);
	(void)sem_destroy(&timer_ready);
	(void)pthread_barrier_destroy(&barrier);
	(void)pthread_spin_destroy(&spin);
	(void)execve(exec_argv[0], exec_argv, exec_envp);

	/* Obtains the fail errno result. */
	function_result = fail_errno("pthread-exec");

	/* Returns the computed result. */
	return function_result;
}

/* Supports the fail operation. */
static int
fail(
	const char *name)
{
	(void)write(2, "POSIX_R2_FAIL: ", 15);
	(void)write(2, name, strlen(name));
	(void)write(2, "\n", 1);

	/* Reports operation failure. */
	return 1;
}

/* Supports the fail errno operation. */
static int
fail_errno(
	const char *name)
{
	char digits[4];
	unsigned value;
	unsigned count;

	value = (unsigned)errno;
	count = 0;

	(void)write(2, "POSIX_R2_FAIL: ", 15);
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
	return 1;
}

/* Supports the same signal action operation. */
static int
same_signal_action(
	const struct sigaction *left,
	const struct sigaction *right)
{
	/* Returns the computed result. */
	return left->sa_handler == right->sa_handler &&
	       left->sa_mask == right->sa_mask &&
	       left->sa_flags == right->sa_flags &&
	       left->sa_restorer == right->sa_restorer;
}

/* Supports the test pty line discipline operation. */
static int
test_pty_line_discipline(
	int master,
	int slave)
{
	int function_result;
	struct termios saved, settings;
	unsigned char input[8], byte;
	char readback[8];
	int master_flags, slave_flags;
	int status;
	pid_t child;
	struct timespec interbyte = {0, 300000000L};

	/* Handles a failed tcgetattr operation. */
	if (tcgetattr(slave, &saved) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-termios-get");

		/* Returns the computed result. */
		return function_result;
	}

	settings = saved;
	settings.c_iflag |= IXON;
	settings.c_lflag |= ICANON | IEXTEN;
	settings.c_lflag &= ~ECHO;
	settings.c_cc[VWERASE] = 23;
	settings.c_cc[VLNEXT] = 22;
	settings.c_cc[VREPRINT] = 18;

	/* Handles a failed tcsetattr operation. */
	if (tcsetattr(slave, TCSANOW, &settings) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-termios-set");

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * IEXTEN word erase and literal-next are shared by console and PTY
	 * input. */
	memcpy(input, "ab cd", 5);
	input[5] = settings.c_cc[VWERASE];
	input[6] = 'x';
	input[7] = '\n';

	/* Handles a failed write operation. */
	if (write(master, input, sizeof(input)) != (ssize_t)sizeof(input) ||
	    read(slave, readback, 5) != 5 || memcmp(readback, "ab x\n", 5) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-ixten-werase");

		/* Returns the computed result. */
		return function_result;
	}
	input[0] = 'q';
	input[1] = settings.c_cc[VLNEXT];
	input[2] = settings.c_cc[VINTR];
	input[3] = '\n';

	/* Handles a failed write operation. */
	if (write(master, input, 4) != 4 || read(slave, readback, 3) != 3 ||
	    readback[0] != 'q' || (unsigned char)readback[1] != input[2] ||
	    readback[2] != '\n') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-ixten-lnext");

		/* Returns the computed result. */
		return function_result;
	}

	/* With MIN and TIME both nonzero, TIME restarts after every byte. */
	settings.c_lflag &= ~ICANON;
	settings.c_cc[VMIN] = 3;
	settings.c_cc[VTIME] = 5;

	/* Handles a failed tcsetattr operation. */
	if (tcsetattr(slave, TCSANOW, &settings) != 0 || (child = fork()) < 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-vmin-vtime-setup");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the child process state. */
	if (child == 0) {
		/* Handles a failed write operation. */
		if (write(master, "1", 1) != 1 ||
		    nanosleep(&interbyte, NULL) != 0 ||
		    write(master, "2", 1) != 1 ||
		    nanosleep(&interbyte, NULL) != 0 ||
		    write(master, "3", 1) != 1)
			_exit(43);
		_exit(0);
	}

	/* Handles a failed read operation. */
	if (read(slave, readback, 3) != 3 || memcmp(readback, "123", 3) != 0 ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-vmin-vtime");

		/* Returns the computed result. */
		return function_result;
	}

	master_flags = fcntl(master, F_GETFL);
	slave_flags = fcntl(slave, F_GETFL);

	/* Handles a failed fcntl operation. */
	if (master_flags < 0 || slave_flags < 0 ||
	    fcntl(slave, F_SETFL, slave_flags | O_NONBLOCK) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-flow-flags");

		/* Returns the computed result. */
		return function_result;
	}

	byte = settings.c_cc[VSTOP];

	/* Handles the reported system error. */
	if (write(master, &byte, 1) != 1 || write(slave, "s", 1) != -1 ||
	    errno != EAGAIN) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-ixon-stop");

		/* Returns the computed result. */
		return function_result;
	}

	byte = settings.c_cc[VSTART];

	/* Handles a failed write operation. */
	if (write(master, &byte, 1) != 1 || write(slave, "f", 1) != 1 ||
	    read(master, &byte, 1) != 1 || byte != 'f') {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-ixon-start");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the reported system error. */
	if (tcflow(slave, TCOOFF) != 0 || write(slave, "s", 1) != -1 ||
	    errno != EAGAIN || tcflow(slave, TCOON) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-tcflow-local");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed fcntl operation. */
	if (fcntl(slave, F_SETFL, slave_flags) != 0 ||
	    tcflow(slave, TCIOFF) != 0 || read(master, &byte, 1) != 1 ||
	    byte != settings.c_cc[VSTOP] || tcflow(slave, TCION) != 0 ||
	    read(master, &byte, 1) != 1 || byte != settings.c_cc[VSTART]) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-tcflow-peer");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the reported system error. */
	if (write(slave, "drop", 4) != 4 || tcflush(slave, TCOFLUSH) != 0 ||
	    fcntl(master, F_SETFL, master_flags | O_NONBLOCK) != 0 ||
	    read(master, &byte, 1) != -1 || errno != EAGAIN ||
	    fcntl(master, F_SETFL, master_flags) != 0 || tcdrain(slave) != 0 ||
	    tcsetattr(slave, TCSANOW, &saved) != 0) {
		/* Obtains the fail errno result. */
		function_result = fail_errno("pty-drain-flush");

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the atfork prepare operation. */
static void
atfork_prepare(
	void)
{
	atfork_prepare_called++;
}

/* Supports the atfork parent operation. */
static void
atfork_parent(
	void)
{
	atfork_parent_called++;
}

/* Supports the atfork child operation. */
static void
atfork_child(
	void)
{
	atfork_child_called++;
}

/* Supports the detached worker operation. */
static void *
detached_worker(
	void *argument)
{
	(void)argument;
	(void)sem_post(&detached_ready);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the thread timer callback operation. */
static void
thread_timer_callback(
	union sigval value)
{
	__atomic_store_n(&timer_callback_value, value.sival_int,
			 __ATOMIC_RELEASE);

	/* Validates the current value. */
	if (value.sival_int > 0 && value.sival_int < 32)
		(void)__atomic_fetch_or(&timer_callback_bits,
					1U << (unsigned)value.sival_int,
					__ATOMIC_RELEASE);
	(void)sem_post(&timer_ready);
}

/* Supports the public realtime handler operation. */
static void
public_realtime_handler(
	int signo)
{
	(void)signo;
}

/* Supports the cancel cleanup operation. */
static void
cancel_cleanup(
	void *argument)
{
	(void)argument;
	cleanup_called = 1;
}

/* Supports the cancel worker operation. */
static void *
cancel_worker(
	void *argument)
{
	(void)argument;
	pthread_cleanup_push(cancel_cleanup, NULL);
	(void)sem_wait(&cancel_wait);
	pthread_cleanup_pop(0);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the worker operation. */
static void *
worker(
	void *argument)
{
	(void)argument;

	/* Handles a failed pthread mutex lock operation. */
	if (pthread_mutex_lock(&mutex) != 0)
		return (void *)1;
	shared_value = 42;
	(void)pthread_mutex_unlock(&mutex);

	/* Handles a failed pthread rwlock wrlock operation. */
	if (pthread_rwlock_wrlock(&rwlock) != 0)
		return (void *)2;
	shared_value++;
	(void)pthread_rwlock_unlock(&rwlock);

	/* Handles a failed pthread spin lock operation. */
	if (pthread_spin_lock(&spin) != 0)
		return (void *)3;
	spin_value++;
	(void)pthread_spin_unlock(&spin);
	(void)sem_post(&ready);
	(void)pthread_barrier_wait(&barrier);

	/* Returns the computed result. */
	return (void *)7;
}

/* Supports the locale worker operation. */
static void *
locale_worker(
	void *argument)
{
	locale_t locale;

	locale = argument;

	/* Handles a failed uselocale operation. */
	if (uselocale(locale) == NULL || MB_CUR_MAX != 1U)
		return (void *)1;

	/* Returns the computed result. */
	return (void *)0;
}
