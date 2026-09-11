/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/system.h>

#include "userland/base/net/protocol.h"

#define TEST_PATH "/tmp/peercred-q040.sock"
#define CLOSE_RACE_PATH "/tmp/peercred-q040-close.sock"
#define CLOSE_RACE_ITERATIONS 32U

struct child_report {
	struct zedbsd_peercred listener;
	int error;
};

struct close_race_context {
	struct sockaddr_un address;
	int gate[2];
	int connector;
	int listener;
	int connect_result;
	int connect_error;
	int close_result;
};

static int
failure(const char *stage)
{
	printf("PEERCRED-NATIVE: FAIL stage=%s errno=%d\n", stage, errno);
	return 1;
}

static int
peer_equal(const struct zedbsd_peercred *peer, pid_t pid, uid_t euid,
	   gid_t egid)
{
	return peer->pid == (int32_t)pid && peer->euid == (uint32_t)euid &&
	    peer->egid == (uint32_t)egid;
}

static int
read_exact(int descriptor, void *buffer, size_t length)
{
	unsigned char *cursor = buffer;

	while (length != 0) {
		ssize_t count = read(descriptor, cursor, length);

		if (count <= 0)
			return -1;
		cursor += (size_t)count;
		length -= (size_t)count;
	}
	return 0;
}

static int
write_exact(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;

	while (length != 0) {
		ssize_t count = write(descriptor, cursor, length);

		if (count <= 0)
			return -1;
		cursor += (size_t)count;
		length -= (size_t)count;
	}
	return 0;
}

static int
get_peer(int descriptor, struct zedbsd_peercred *peer)
{
	socklen_t length = sizeof(*peer);

	memset(peer, 0, sizeof(*peer));
	if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, peer, &length) != 0)
		return -1;
	return length == sizeof(*peer) ? 0 : -1;
}

static int
networkd_exchange(const char *request, char *response, size_t capacity)
{
	struct sockaddr_un address;
	size_t used = 0;
	int descriptor;

	if (request == NULL || response == NULL || capacity < 2U) {
		errno = EINVAL;
		return -1;
	}
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, NETWORKD_SOCKET,
	    sizeof(address.sun_path) - 1U);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0 || write_exact(descriptor, request, strlen(request)) != 0) {
		int saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	while (used + 1U < capacity) {
		ssize_t count = read(descriptor, response + used,
		    capacity - used - 1U);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			int saved = errno;
			(void)close(descriptor);
			errno = saved;
			return -1;
		}
		if (count == 0)
			break;
		used += (size_t)count;
	}
	response[used] = '\0';
	if (close(descriptor) != 0)
		return -1;
	if (used == 0 || used + 1U == capacity) {
		errno = used == 0 ? EPIPE : EOVERFLOW;
		return -1;
	}
	return 0;
}

static int
test_networkd_admission(void)
{
	char denied[32];
	char response[NETWORKD_RESPONSE_MAX + 1U];
	const gid_t network_group = (gid_t)69;
	struct stat status;

	if (lstat(NETWORKD_SOCKET, &status) != 0 ||
	    !S_ISSOCK(status.st_mode) || status.st_uid != (uid_t)0 ||
	    status.st_gid != network_group || (status.st_mode & 07777U) != 0660U)
		return failure("networkd-socket-publication");

	if (networkd_exchange("V1 SHOW\n", response, sizeof(response)) != 0 ||
	    strncmp(response, "V1 OK", 5U) != 0)
		return failure("networkd-root-show");

	/* Admission must honor a supplementary network group, not just egid. */
	if (setgroups(1U, &network_group) != 0 ||
	    setegid((gid_t)124) != 0 || seteuid((uid_t)123) != 0)
		return failure("networkd-group-credentials");
	if (networkd_exchange("V1 SHOW\n", response, sizeof(response)) != 0 ||
	    strncmp(response, "V1 OK", 5U) != 0)
		return failure("networkd-group-show");
	if (snprintf(denied, sizeof(denied), "V1 ERR %d ", EPERM) < 0)
		return failure("networkd-denied-format");
	if (networkd_exchange("V1 UP lo0\n", response, sizeof(response)) != 0 ||
	    strncmp(response, denied, strlen(denied)) != 0)
		return failure("networkd-group-mutation");
	if (seteuid((uid_t)0) != 0 || setegid((gid_t)0) != 0 ||
	    setgroups(0U, NULL) != 0)
		return failure("networkd-group-restore");

	/* A nonmember is rejected by root:network 0660 pathname admission. */
	if (setgroups(0U, NULL) != 0 || setegid((gid_t)124) != 0 ||
	    seteuid((uid_t)123) != 0)
		return failure("networkd-unrelated-credentials");
	errno = 0;
	if (networkd_exchange("V1 SHOW\n", response, sizeof(response)) != -1 ||
	    errno != EACCES)
		return failure("networkd-unrelated-admission");
	if (seteuid((uid_t)0) != 0 || setegid((gid_t)0) != 0)
		return failure("networkd-unrelated-restore");
	return 0;
}

static int
expect_option_error(int descriptor, int expected)
{
	struct zedbsd_peercred peer;
	socklen_t length = sizeof(peer);

	errno = 0;
	if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &length) !=
	    -1)
		return -1;
	return errno == expected ? 0 : -1;
}

static void *
close_race_connect(void *argument)
{
	struct close_race_context *context = argument;
	char start;

	if (read_exact(context->gate[0], &start, sizeof(start)) != 0) {
		context->connect_result = -1;
		context->connect_error = EIO;
		return NULL;
	}
	errno = 0;
	context->connect_result = connect(context->connector,
	    (struct sockaddr *)&context->address, sizeof(context->address));
	context->connect_error = errno;
	return NULL;
}

static void *
close_race_close(void *argument)
{
	struct close_race_context *context = argument;
	char start;

	if (read_exact(context->gate[0], &start, sizeof(start)) != 0) {
		context->close_result = -1;
		return NULL;
	}
	errno = 0;
	context->close_result = close(context->listener);
	return NULL;
}

static int
test_listener_close_race(void)
{
	struct system_resource_info before, after;
	struct close_race_context context;
	struct zedbsd_peercred peer;
	pthread_t connector_thread, close_thread;
	unsigned iteration;
	int system_descriptor;

	system_descriptor = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (system_descriptor < 0 ||
	    ioctl(system_descriptor, ZEDBSD_SYSTEM_GET_RESOURCES, &before) != 0)
		return failure("close-race-resource-before");
	for (iteration = 0; iteration < CLOSE_RACE_ITERATIONS; iteration++) {
		const char start[2] = { 'c', 'l' };

		memset(&context, 0, sizeof(context));
		context.gate[0] = -1;
		context.gate[1] = -1;
		context.connector = -1;
		context.listener = -1;
		(void)unlink(CLOSE_RACE_PATH);
		context.listener = socket(AF_UNIX, SOCK_STREAM, 0);
		context.connector = socket(AF_UNIX, SOCK_STREAM, 0);
		memset(&context.address, 0, sizeof(context.address));
		context.address.sun_family = AF_UNIX;
		strncpy(context.address.sun_path, CLOSE_RACE_PATH,
		    sizeof(context.address.sun_path) - 1U);
		if (context.listener < 0 || context.connector < 0 ||
		    bind(context.listener, (struct sockaddr *)&context.address,
		    sizeof(context.address)) != 0 ||
		    listen(context.listener, 4) != 0 ||
		    pipe(context.gate) != 0)
			return failure("close-race-setup");
		if (pthread_create(&connector_thread, NULL, close_race_connect,
		    &context) != 0)
			return failure("close-race-create");
		if (pthread_create(&close_thread, NULL, close_race_close,
		    &context) != 0) {
			(void)write_exact(context.gate[1], start, 1U);
			(void)pthread_join(connector_thread, NULL);
			(void)close(context.listener);
			(void)close(context.connector);
			(void)close(context.gate[0]);
			(void)close(context.gate[1]);
			return failure("close-race-create");
		}
		if (write_exact(context.gate[1], start, sizeof(start)) != 0)
			return failure("close-race-release");
		if (pthread_join(connector_thread, NULL) != 0 ||
		    pthread_join(close_thread, NULL) != 0)
			return failure("close-race-join");
		(void)close(context.gate[0]);
		(void)close(context.gate[1]);
		if (context.close_result != 0)
			return failure("close-race-listener-close");
		if (context.connect_result == 0) {
			if (get_peer(context.connector, &peer) != 0 ||
			    !peer_equal(&peer, getpid(), 0, 0))
				return failure("close-race-success-snapshot");
		} else if (context.connect_error == EALREADY ||
		    context.connect_error == EISCONN) {
			return failure("close-race-connect-state");
		}
		(void)close(context.connector);
		(void)unlink(CLOSE_RACE_PATH);
	}
	if (ioctl(system_descriptor, ZEDBSD_SYSTEM_GET_RESOURCES, &after) != 0 ||
	    close(system_descriptor) != 0)
		return failure("close-race-resource-after");
	if (before.socket != after.socket)
		return failure("close-race-socket-leak");
	return 0;
}

static int
send_fd(int descriptor, int passed)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *header;
	char byte = 'p';

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
	memcpy(CMSG_DATA(header), &passed, sizeof(passed));
	return sendmsg(descriptor, &message, 0) == 1 ? 0 : -1;
}

static int
receive_fd(int descriptor)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *header;
	char byte = 0;
	int received = -1;

	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = &byte;
	vector.iov_len = 1;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	if (recvmsg(descriptor, &message, 0) != 1 || byte != 'p' ||
	    message.msg_controllen < CMSG_LEN(sizeof(int)))
		return -1;
	header = (struct cmsghdr *)control.bytes;
	if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
	    header->cmsg_len != CMSG_LEN(sizeof(int)))
		return -1;
	memcpy(&received, CMSG_DATA(header), sizeof(received));
	return received;
}

static int
test_socketpair_and_lengths(void)
{
	struct zedbsd_peercred peer;
	unsigned char short_buffer[sizeof(peer)];
	unsigned char large_buffer[sizeof(peer) + 8U];
	socklen_t length;
	int pair[2] = { -1, -1 };
	int datagram[2] = { -1, -1 };
	int datagram_listener = -1;
	int failed_connector = -1;
	int unconnected = -1;
	int inet_socket = -1;
	unsigned index;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		return failure("socketpair-create");
	if (get_peer(pair[0], &peer) != 0 ||
	    !peer_equal(&peer, getpid(), geteuid(), getegid()) ||
	    get_peer(pair[1], &peer) != 0 ||
	    !peer_equal(&peer, getpid(), geteuid(), getegid()))
		return failure("socketpair-creator");

	memset(short_buffer, 0xa5, sizeof(short_buffer));
	length = sizeof(peer) - 1U;
	errno = 0;
	if (getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, short_buffer, &length) !=
	    -1 || errno != EINVAL || length != sizeof(peer) - 1U)
		return failure("short-length-error");
	for (index = 0; index < sizeof(short_buffer); index++) {
		if (short_buffer[index] != 0xa5)
			return failure("short-length-partial-copy");
	}

	memset(large_buffer, 0x5a, sizeof(large_buffer));
	length = sizeof(large_buffer);
	if (getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, large_buffer, &length) !=
	    0 || length != sizeof(peer))
		return failure("large-length-success");
	memcpy(&peer, large_buffer, sizeof(peer));
	if (!peer_equal(&peer, getpid(), geteuid(), getegid()))
		return failure("large-length-value");
	for (index = sizeof(peer); index < sizeof(large_buffer); index++) {
		if (large_buffer[index] != 0x5a)
			return failure("large-length-overwrite");
	}
	length = sizeof(peer);
	errno = 0;
	if (getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, NULL, &length) != -1 ||
	    errno != EINVAL || length != sizeof(peer))
		return failure("null-result-error");
	errno = 0;
	if (getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &peer, NULL) != -1 ||
	    errno != EINVAL)
		return failure("null-length-error");

	unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
	if (unconnected < 0 || expect_option_error(unconnected, ENOTCONN) != 0)
		return failure("unconnected-state");
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, datagram) != 0 ||
	    expect_option_error(datagram[0], ENOPROTOOPT) != 0)
		return failure("datagram-family");
	{
		struct sockaddr_un datagram_address;

		datagram_listener = socket(AF_UNIX, SOCK_DGRAM, 0);
		memset(&datagram_address, 0, sizeof(datagram_address));
		datagram_address.sun_family = AF_UNIX;
		strncpy(datagram_address.sun_path,
		    "/tmp/peercred-q040-datagram.sock",
		    sizeof(datagram_address.sun_path) - 1U);
		(void)unlink(datagram_address.sun_path);
		if (datagram_listener < 0 ||
		    bind(datagram_listener, (struct sockaddr *)&datagram_address,
		    sizeof(datagram_address)) != 0)
			return failure("datagram-listener");
		errno = 0;
		if (connect(datagram[0], (struct sockaddr *)&datagram_address,
		    sizeof(datagram_address)) != -1 || errno != EISCONN)
			return failure("datagram-pair-reconnect");
		(void)unlink(datagram_address.sun_path);
	}
	inet_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (inet_socket < 0 ||
	    expect_option_error(inet_socket, ENOPROTOOPT) != 0)
		return failure("inet-family");
	failed_connector = socket(AF_UNIX, SOCK_STREAM, 0);
	if (failed_connector < 0)
		return failure("failed-connect-socket");
	{
		struct sockaddr_un missing;

		memset(&missing, 0, sizeof(missing));
		missing.sun_family = AF_UNIX;
		strncpy(missing.sun_path, "/tmp/peercred-q040-missing.sock",
		    sizeof(missing.sun_path) - 1U);
		(void)unlink(missing.sun_path);
		errno = 0;
		if (connect(failed_connector, (struct sockaddr *)&missing,
		    sizeof(missing)) != -1 || errno == EALREADY || errno == EISCONN ||
		    expect_option_error(failed_connector, ENOTCONN) != 0)
			return failure("failed-connect-state");
		/* A failed resolution must release the in-progress reservation. */
		errno = 0;
		if (connect(failed_connector, (struct sockaddr *)&missing,
		    sizeof(missing)) != -1 || errno == EALREADY || errno == EISCONN)
			return failure("failed-connect-retry");
	}

	(void)close(failed_connector);
	(void)close(inet_socket);
	(void)close(unconnected);
	(void)close(datagram_listener);
	(void)close(datagram[0]);
	(void)close(datagram[1]);
	(void)close(pair[0]);
	(void)close(pair[1]);
	return 0;
}

static int
child_connect(int listener, int report_descriptor, pid_t listener_pid)
{
	struct sockaddr_un address;
	struct child_report report;
	int connector;

	memset(&report, 0, sizeof(report));
	(void)close(listener);
	connector = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connector < 0)
		report.error = 1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, TEST_PATH, sizeof(address.sun_path) - 1U);
	if (report.error == 0 &&
	    connect(connector, (struct sockaddr *)&address, sizeof(address)) != 0)
		report.error = 2;
	if (report.error == 0 && get_peer(connector, &report.listener) != 0)
		report.error = 3;
	if (report.error == 0 &&
	    !peer_equal(&report.listener, listener_pid, 0, 0))
		report.error = 4;
	/* A later credential change must not rewrite the pending peer record. */
	if (report.error == 0 &&
	    (setegid((gid_t)124) != 0 || seteuid((uid_t)123) != 0 ||
	     getegid() != (gid_t)124 || geteuid() != (uid_t)123))
		report.error = 5;
	if (write_exact(report_descriptor, &report, sizeof(report)) != 0)
		_exit(20);
	(void)close(connector);
	(void)close(report_descriptor);
	_exit(report.error == 0 ? 0 : 21);
}

static int
test_path_accept_and_rights(void)
{
	struct sockaddr_un address;
	struct child_report report;
	struct zedbsd_peercred peer;
	pid_t listener_pid = getpid();
	pid_t child;
	int report_pipe[2] = { -1, -1 };
	int transfer[2] = { -1, -1 };
	int listener = -1, accepted = -1, transferred = -1;
	int status = 0;

	(void)unlink(TEST_PATH);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		return failure("pathname-socket");
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, TEST_PATH, sizeof(address.sun_path) - 1U);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 4) != 0)
		return failure("pathname-listen");
	if (expect_option_error(listener, ENOTCONN) != 0)
		return failure("listening-state");
	/* Repeated listen may resize backlog but must not relabel the listener. */
	if (setegid((gid_t)126) != 0 || seteuid((uid_t)125) != 0 ||
	    listen(listener, 8) != 0)
		return failure("pathname-relisten");
	if (seteuid((uid_t)0) != 0 || setegid((gid_t)0) != 0)
		return failure("pathname-relisten-restore");
	if (pipe(report_pipe) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, transfer) != 0)
		return failure("pathname-control");
	child = fork();
	if (child < 0)
		return failure("pathname-fork");
	if (child == 0) {
		(void)close(report_pipe[0]);
		(void)close(transfer[0]);
		(void)close(transfer[1]);
		child_connect(listener, report_pipe[1], listener_pid);
	}
	(void)close(report_pipe[1]);

	/* The listener identity is the successful listen-time identity. */
	if (setegid((gid_t)126) != 0 || seteuid((uid_t)125) != 0)
		return failure("listener-credential-change");
	if (read_exact(report_pipe[0], &report, sizeof(report)) != 0)
		return failure("pathname-child-report");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 || report.error != 0)
		return failure("pathname-child-exit");
	accepted = accept(listener, NULL, NULL);
	if (accepted < 0)
		return failure("pathname-delayed-accept");
	if (get_peer(accepted, &peer) != 0 ||
	    !peer_equal(&peer, child, 0, 0))
		return failure("pathname-connector-snapshot");
	if (!peer_equal(&report.listener, listener_pid, 0, 0))
		return failure("pathname-listener-snapshot");

	if (send_fd(transfer[0], accepted) != 0)
		return failure("rights-send");
	transferred = receive_fd(transfer[1]);
	if (transferred < 0)
		return failure("rights-receive");
	(void)close(accepted);
	accepted = -1;
	if (get_peer(transferred, &peer) != 0 ||
	    !peer_equal(&peer, child, 0, 0))
		return failure("rights-snapshot");

	if (seteuid((uid_t)0) != 0 || setegid((gid_t)0) != 0)
		return failure("listener-credential-restore");
	(void)unlink(TEST_PATH);
	(void)close(transferred);
	(void)close(transfer[0]);
	(void)close(transfer[1]);
	(void)close(report_pipe[0]);
	(void)close(listener);
	return 0;
}

int
main(void)
{
	_Static_assert(sizeof(struct zedbsd_peercred) == 12U,
	    "peer credential ABI size");
	_Static_assert(offsetof(struct zedbsd_peercred, pid) == 0U,
	    "peer credential pid offset");
	_Static_assert(offsetof(struct zedbsd_peercred, euid) == 4U,
	    "peer credential euid offset");
	_Static_assert(offsetof(struct zedbsd_peercred, egid) == 8U,
	    "peer credential egid offset");

	if (geteuid() != 0 || getegid() != 0)
		return failure("requires-root");
	if (test_socketpair_and_lengths() != 0)
		return 1;
	if (test_path_accept_and_rights() != 0)
		return 1;
	if (test_listener_close_race() != 0)
		return 1;
	if (test_networkd_admission() != 0)
		return 1;
	puts("PEERCRED-NATIVE: PASS pathname socketpair accept short-length "
	     "scm-rights snapshot datagram-reconnect close-race networkd-auth");
	return 0;
}
