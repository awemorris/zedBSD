/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the network of the operating-system layer
 * (base/os-net.h): resolving, connecting, sending and receiving, waiting
 * for several sockets, and the waker.  It runs on POSIX and on Windows;
 * argv[1] says which.  Only the loopback address is used, with listening
 * sockets the test makes itself through the system's own API.
 *
 * The expected values come from the header's contracts, from the system's
 * socket API used separately from the module, and from hand calculation.
 */

#include "check.h"

#include "base/os-net.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32
/* A socket of the system's own API, used by the test beside the module. */
typedef SOCKET sys_socket;

/* The length type the system's address functions take. */
typedef int sys_length;
#define SYS_INVALID	INVALID_SOCKET
#else
/* A socket of the system's own API, used by the test beside the module. */
typedef int sys_socket;

/* The length type the system's address functions take. */
typedef socklen_t sys_length;
#define SYS_INVALID	(-1)
#endif

/* Whether this run is the Windows one (argv[1] is "win32"). */
static int is_win32;

/* Something for the failure checks to point an output at, so a function that skips the reset shows. */
static int sentinel_target;

/* The longest wait for something that must happen, in milliseconds; a wait this long is a failure. */
#define PATIENCE_MS	10000

/* The size of the pattern buffers the transfer tests use. */
#define CHUNK_SIZE	16384U

/* The number of bytes the echo test sends through the peer and expects back. */
#define ECHO_TOTAL	300000U

/* The listening socket, the accepted peer and the module's socket of one connection. */
struct pair {
	sys_socket listener;
	sys_socket peer;
	wb_os_socket client;
	unsigned short port;
};

/* The echo server's state: the listener it accepts on and what it saw. */
struct echo {
	sys_socket listener;
	size_t echoed;
	int failed;
};

/* What a thread that rings a waker after a delay needs. */
struct ringer {
	struct wb_os_waker *waker;
	int delay_ms;
};

/* Starts the system's socket library for the test's own sockets. */
static void
sys_start(
	void)
{
#ifdef _WIN32
	WSADATA data;
	int status;

	status = WSAStartup(MAKEWORD(2, 2), &data);
	CHECK_EQ(status, 0);
#endif
}

/* Closes a system socket. */
static void
sys_close(
	sys_socket handle)
{
#ifdef _WIN32
	closesocket(handle);
#else
	close(handle);
#endif
}

/* Sleeps for some milliseconds. */
static void
sys_sleep(
	unsigned milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	struct timespec span;

	span.tv_sec = (time_t)(milliseconds / 1000U);
	span.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&span, NULL);
#endif
}

/* Reads a monotonic clock in milliseconds, with a fraction. */
static double
sys_now(
	void)
{
#ifdef _WIN32
	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
#endif
}

/* Makes a listening socket on 127.0.0.1 at a port the system picks. */
static sys_socket
sys_listen(
	unsigned short *port,
	int backlog)
{
	struct sockaddr_in address;
	sys_socket handle;
	sys_length length;
	int status;

	handle = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(handle != SYS_INVALID);
	if (handle == SYS_INVALID)
		return SYS_INVALID;

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	status = bind(handle, (struct sockaddr *)&address, sizeof(address));
	CHECK_EQ(status, 0);
	status = listen(handle, backlog);
	CHECK_EQ(status, 0);
	length = (sys_length)sizeof(address);
	status = getsockname(handle, (struct sockaddr *)&address, &length);
	CHECK_EQ(status, 0);
	*port = ntohs(address.sin_port);

	return handle;
}

/* Accepts one connection, waiting at most PATIENCE_MS for it. */
static sys_socket
sys_accept(
	sys_socket listener)
{
	fd_set waiting;
	struct timeval limit;
	sys_socket handle;
	int status;

	FD_ZERO(&waiting);
	FD_SET(listener, &waiting);
	limit.tv_sec = PATIENCE_MS / 1000;
	limit.tv_usec = 0;
	status = select((int)listener + 1, &waiting, NULL, NULL, &limit);
	CHECK_EQ(status, 1);
	if (status != 1)
		return SYS_INVALID;

	handle = accept(listener, NULL, NULL);
	CHECK(handle != SYS_INVALID);

	return handle;
}

/* Receives from a system socket. */
static long
sys_recv(
	sys_socket handle,
	void *bytes,
	size_t length)
{
	return (long)recv(handle, (char *)bytes, (int)length, 0);
}

/* Receives from a system socket, giving up with -1 after PATIENCE_MS when nothing arrives. */
static long
sys_recv_wait(
	sys_socket handle,
	void *bytes,
	size_t length)
{
	fd_set waiting;
	struct timeval limit;
	int status;

	FD_ZERO(&waiting);
	FD_SET(handle, &waiting);
	limit.tv_sec = PATIENCE_MS / 1000;
	limit.tv_usec = 0;
	status = select((int)handle + 1, &waiting, NULL, NULL, &limit);
	if (status != 1)
		return -1;

	return sys_recv(handle, bytes, length);
}

/* Sends on a system socket without raising a signal. */
static long
sys_send(
	sys_socket handle,
	const void *bytes,
	size_t length)
{
#ifdef MSG_NOSIGNAL
	return (long)send(handle, (const char *)bytes, (int)length, MSG_NOSIGNAL);
#else
	return (long)send(handle, (const char *)bytes, (int)length, 0);
#endif
}

/* Makes a system socket close with a reset instead of a graceful end. */
static void
sys_reset_on_close(
	sys_socket handle)
{
	struct linger setting;
	int status;

	setting.l_onoff = 1;
	setting.l_linger = 0;
	status = setsockopt(handle, SOL_SOCKET, SO_LINGER, (const char *)&setting, (int)sizeof(setting));
	CHECK_EQ(status, 0);
}

/* Gives the number of a port as the decimal text a resolve takes. */
static void
port_text(
	unsigned short port,
	char *text)
{
	sprintf(text, "%u", (unsigned)port);
}

/* Finds a port nobody listens on: binds one, learns its number, and closes it again. */
static unsigned short
closed_port(
	void)
{
	unsigned short port;
	sys_socket handle;

	port = 0;
	handle = sys_listen(&port, 1);
	if (handle != SYS_INVALID)
		sys_close(handle);

	return port;
}

/* Resolves 127.0.0.1 and a port, expecting one or more addresses. */
static struct wb_os_addresses *
resolve_loopback(
	unsigned short port)
{
	struct wb_os_addresses *addresses;
	char text[16];
	int error;

	port_text(port, text);
	addresses = NULL;
	error = wb_os_resolve("127.0.0.1", text, &addresses);
	CHECK_EQ(error, 0);
	CHECK(addresses != NULL);
	if (addresses != NULL)
		CHECK(wb_os_addresses_count(addresses) >= 1U);

	return addresses;
}

/* Waits for one module socket to be ready for the asked flags; gives what was found. */
static unsigned
wait_for(
	wb_os_socket socket,
	unsigned wants,
	int timeout_ms)
{
	struct wb_os_poll_item item;
	size_t ready;
	int error;

	item.socket = socket;
	item.wants = wants;
	item.found = 0xf0U;
	ready = 99U;
	error = wb_os_poll(&item, 1U, timeout_ms, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, item.found != 0U ? 1U : 0U);

	return item.found;
}

/* Opens a connection: a listener, the module's socket connected to it, and the accepted peer. */
static int
pair_open(
	struct pair *pair,
	int blocking)
{
	struct wb_os_addresses *addresses;
	unsigned found;
	int error;

	pair->listener = SYS_INVALID;
	pair->peer = SYS_INVALID;
	pair->client = WB_OS_SOCKET_NONE;
	pair->port = 0;

	pair->listener = sys_listen(&pair->port, 8);
	if (pair->listener == SYS_INVALID)
		return 0;

	addresses = resolve_loopback(pair->port);
	if (addresses == NULL)
		return 0;

	error = wb_os_socket_connect(addresses, 0U, blocking, &pair->client);
	wb_os_addresses_free(addresses);
	if (blocking) {
		CHECK_EQ(error, 0);
	} else {
		CHECK(error == 0 || error == EINPROGRESS);
	}
	if (error != 0 && error != EINPROGRESS)
		return 0;

	CHECK(pair->client != WB_OS_SOCKET_NONE);

	if (error == EINPROGRESS) {
		found = wait_for(pair->client, WB_OS_POLL_WRITABLE, PATIENCE_MS);
		CHECK((found & WB_OS_POLL_WRITABLE) != 0U);
		error = wb_os_socket_connect_result(pair->client);
		CHECK_EQ(error, 0);
	}

	pair->peer = sys_accept(pair->listener);

	return pair->peer != SYS_INVALID;
}

/* Closes everything of a connection. */
static void
pair_close(
	struct pair *pair)
{
	wb_os_socket_close(pair->client);
	if (pair->peer != SYS_INVALID)
		sys_close(pair->peer);
	if (pair->listener != SYS_INVALID)
		sys_close(pair->listener);
	pair->client = WB_OS_SOCKET_NONE;
	pair->peer = SYS_INVALID;
	pair->listener = SYS_INVALID;
}

/* Fills a buffer with the byte pattern of a stream, from an offset in it. */
static void
pattern_fill(
	unsigned char *bytes,
	size_t length,
	size_t offset)
{
	size_t i;

	for (i = 0; i < length; i++)
		bytes[i] = (unsigned char)((offset + i) % 251U);
}

/* Checks that a buffer holds the pattern from an offset on. */
static int
pattern_ok(
	const unsigned char *bytes,
	size_t length,
	size_t offset)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (bytes[i] != (unsigned char)((offset + i) % 251U))
			return 0;
	}

	return 1;
}

/* Checks wb_os_resolve and the address list. */
static void
test_resolve(
	void)
{
	struct wb_os_addresses *addresses;
	struct wb_os_addresses *marker;
	sys_socket probe;
	char long_name[300];
	int error;
	int have_ipv6;

	/* A literal IPv4 address resolves to at least one address. */
	addresses = NULL;
	error = wb_os_resolve("127.0.0.1", "80", &addresses);
	CHECK_EQ(error, 0);
	CHECK(addresses != NULL);
	CHECK(wb_os_addresses_count(addresses) >= 1U);
	wb_os_addresses_free(addresses);

	/* The name localhost has an address. */
	addresses = NULL;
	error = wb_os_resolve("localhost", "80", &addresses);
	CHECK_EQ(error, 0);
	CHECK(addresses != NULL);
	CHECK(wb_os_addresses_count(addresses) >= 1U);
	wb_os_addresses_free(addresses);

	/* A literal IPv6 address resolves where the host has IPv6. */
	probe = socket(AF_INET6, SOCK_STREAM, 0);
	have_ipv6 = probe != SYS_INVALID;
	if (probe != SYS_INVALID)
		sys_close(probe);
	if (have_ipv6) {
		addresses = NULL;
		error = wb_os_resolve("::1", "80", &addresses);
		CHECK_EQ(error, 0);
		CHECK(addresses != NULL);
		CHECK(wb_os_addresses_count(addresses) >= 1U);
		wb_os_addresses_free(addresses);
	} else {
		printf("note: no IPv6 on this host, the ::1 resolve is skipped\n");
	}

	/* An empty or missing argument is EINVAL and leaves *addresses NULL. */
	marker = (struct wb_os_addresses *)&sentinel_target;
	addresses = marker;
	error = wb_os_resolve("", "80", &addresses);
	CHECK_EQ(error, EINVAL);
	CHECK(addresses == NULL);
	addresses = marker;
	error = wb_os_resolve(NULL, "80", &addresses);
	CHECK_EQ(error, EINVAL);
	CHECK(addresses == NULL);
	addresses = marker;
	error = wb_os_resolve("127.0.0.1", NULL, &addresses);
	CHECK_EQ(error, EINVAL);
	CHECK(addresses == NULL);
	addresses = marker;
	error = wb_os_resolve("127.0.0.1", "", &addresses);
	CHECK_EQ(error, EINVAL);
	CHECK(addresses == NULL);
	error = wb_os_resolve("127.0.0.1", "80", NULL);
	CHECK_EQ(error, EINVAL);

	/* A name that cannot exist is ENOENT, or EAGAIN where no resolver can be reached. */
	addresses = marker;
	error = wb_os_resolve("no-such-host.invalid", "80", &addresses);
	CHECK(error == ENOENT || error == EAGAIN);
	CHECK(addresses == NULL);

	/* A port that is not a number fails and leaves *addresses NULL. */
	addresses = marker;
	error = wb_os_resolve("127.0.0.1", "not-a-port-name", &addresses);
	CHECK(error != 0);
	CHECK(addresses == NULL);

	/* A name that is far too long for a host name fails without trouble. */
	memset(long_name, 'a', sizeof(long_name) - 1U);
	long_name[sizeof(long_name) - 1U] = '\0';
	addresses = marker;
	error = wb_os_resolve(long_name, "80", &addresses);
	CHECK(error != 0);
	CHECK(addresses == NULL);

	/* Text that is not valid UTF-8 is EINVAL on Windows, which converts it, and fails elsewhere. */
	addresses = marker;
	error = wb_os_resolve("\xff\xfe.invalid", "80", &addresses);
	if (is_win32)
		CHECK_EQ(error, EINVAL);
	else
		CHECK(error != 0);
	CHECK(addresses == NULL);

	/* The count of no list is 0 and freeing no list does nothing. */
	CHECK_EQ(wb_os_addresses_count(NULL), 0);
	wb_os_addresses_free(NULL);
}

/* The thread of the echo server: accepts one connection and sends back what arrives until it ends. */
static void *
echo_thread(
	void *argument)
{
	struct echo *echo;
	sys_socket connection;
	unsigned char buffer[CHUNK_SIZE];
	long got;
	long put;
	long total;

	echo = (struct echo *)argument;
	connection = sys_accept(echo->listener);
	if (connection == SYS_INVALID) {
		echo->failed = 1;
		return NULL;
	}

	for (;;) {
		got = sys_recv_wait(connection, buffer, sizeof(buffer));
		if (got <= 0)
			break;

		total = 0;
		while (total < got) {
			put = sys_send(connection, buffer + total, (size_t)(got - total));
			if (put <= 0) {
				echo->failed = 1;
				break;
			}
			total += put;
		}
		echo->echoed += (size_t)got;
	}

	sys_close(connection);

	return NULL;
}

/* Checks a blocking connect and a round trip through an echo thread, in blocking and non-blocking use. */
static void
test_echo(
	void)
{
	struct wb_os_addresses *addresses;
	struct echo echo;
	pthread_t thread;
	wb_os_socket client;
	unsigned char *outgoing;
	unsigned char *incoming;
	unsigned char small[64];
	unsigned char reply[64];
	size_t sent;
	size_t received;
	size_t total_sent;
	size_t total_received;
	unsigned short port;
	unsigned found;
	double started;
	int error;
	int created;

	outgoing = (unsigned char *)malloc(ECHO_TOTAL);
	incoming = (unsigned char *)malloc(ECHO_TOTAL);
	CHECK(outgoing != NULL);
	CHECK(incoming != NULL);
	if (outgoing == NULL || incoming == NULL) {
		free(outgoing);
		free(incoming);
		return;
	}
	pattern_fill(outgoing, ECHO_TOTAL, 0);

	/* A blocking connect to a listening port returns 0, and sends and receives wait. */
	memset(&echo, 0, sizeof(echo));
	port = 0;
	echo.listener = sys_listen(&port, 4);
	CHECK(echo.listener != SYS_INVALID);
	created = pthread_create(&thread, NULL, echo_thread, &echo);
	CHECK_EQ(created, 0);
	addresses = resolve_loopback(port);
	client = WB_OS_SOCKET_NONE;
	error = wb_os_socket_connect(addresses, 0U, 1, &client);
	wb_os_addresses_free(addresses);
	CHECK_EQ(error, 0);
	CHECK(client != WB_OS_SOCKET_NONE);

	/* A short message goes out and comes back whole, the receive waiting for it. */
	memcpy(small, "hello over the loopback", 24U);
	sent = 0;
	error = wb_os_socket_send(client, small, 24U, &sent);
	CHECK_EQ(error, 0);
	CHECK_EQ(sent, 24);
	memset(reply, 0, sizeof(reply));
	total_received = 0;
	while (total_received < 24U) {
		received = 0;
		error = wb_os_socket_receive(client, reply + total_received, 24U - total_received, &received);
		CHECK_EQ(error, 0);
		if (error != 0 || received == 0)
			break;
		total_received += received;
	}
	CHECK_EQ(total_received, 24);
	CHECK(memcmp(reply, small, 24U) == 0);
	wb_os_socket_close(client);
	pthread_join(thread, NULL);
	CHECK_EQ(echo.echoed, 24);
	CHECK_EQ(echo.failed, 0);
	sys_close(echo.listener);

	/* A non-blocking connect and a large round trip driven by wb_os_poll, in pieces. */
	memset(&echo, 0, sizeof(echo));
	port = 0;
	echo.listener = sys_listen(&port, 4);
	created = pthread_create(&thread, NULL, echo_thread, &echo);
	CHECK_EQ(created, 0);
	addresses = resolve_loopback(port);
	client = WB_OS_SOCKET_NONE;
	error = wb_os_socket_connect(addresses, 0U, 0, &client);
	wb_os_addresses_free(addresses);
	CHECK(error == 0 || error == EINPROGRESS);
	if (error == EINPROGRESS) {
		found = wait_for(client, WB_OS_POLL_WRITABLE, PATIENCE_MS);
		CHECK((found & WB_OS_POLL_WRITABLE) != 0U);
		error = wb_os_socket_connect_result(client);
		CHECK_EQ(error, 0);
	}
	total_sent = 0;
	total_received = 0;
	started = sys_now();
	while (total_received < ECHO_TOTAL && sys_now() - started < PATIENCE_MS) {
		struct wb_os_poll_item item;
		size_t ready;

		item.socket = client;
		item.wants = WB_OS_POLL_READABLE;
		if (total_sent < ECHO_TOTAL)
			item.wants |= WB_OS_POLL_WRITABLE;
		item.found = 0;
		ready = 0;
		error = wb_os_poll(&item, 1U, 1000, &ready);
		CHECK_EQ(error, 0);
		if (error != 0)
			break;

		if ((item.found & WB_OS_POLL_WRITABLE) != 0U && total_sent < ECHO_TOTAL) {
			sent = 99U;
			error = wb_os_socket_send(client, outgoing + total_sent, ECHO_TOTAL - total_sent, &sent);
			CHECK(error == 0 || error == EAGAIN);
			if (error == 0)
				total_sent += sent;
			else
				CHECK_EQ(sent, 0);
		}
		if ((item.found & WB_OS_POLL_READABLE) != 0U) {
			received = 99U;
			error = wb_os_socket_receive(client, incoming + total_received, ECHO_TOTAL - total_received, &received);
			CHECK(error == 0 || error == EAGAIN);
			if (error == 0) {
				CHECK(received > 0U);
				total_received += received;
			}
		}
	}
	CHECK_EQ(total_sent, ECHO_TOTAL);
	CHECK_EQ(total_received, ECHO_TOTAL);
	CHECK(pattern_ok(incoming, total_received, 0));
	wb_os_socket_close(client);
	pthread_join(thread, NULL);
	CHECK_EQ(echo.echoed, ECHO_TOTAL);
	CHECK_EQ(echo.failed, 0);
	sys_close(echo.listener);

	free(outgoing);
	free(incoming);
}

/* Checks the arguments of the connect functions. */
static void
test_connect_arguments(
	void)
{
	struct wb_os_addresses *addresses;
	wb_os_socket socket;
	unsigned short port;
	sys_socket listener;
	size_t count;
	int error;

	port = 0;
	listener = sys_listen(&port, 2);
	addresses = resolve_loopback(port);
	count = wb_os_addresses_count(addresses);

	/* An index at the count, or far past it, is EINVAL and leaves the socket NONE. */
	socket = 12345;
	error = wb_os_socket_connect(addresses, count, 0, &socket);
	CHECK_EQ(error, EINVAL);
	CHECK(socket == WB_OS_SOCKET_NONE);
	socket = 12345;
	error = wb_os_socket_connect(addresses, SIZE_MAX, 1, &socket);
	CHECK_EQ(error, EINVAL);
	CHECK(socket == WB_OS_SOCKET_NONE);

	/* A missing list or a missing place for the socket is EINVAL. */
	socket = 12345;
	error = wb_os_socket_connect(NULL, 0U, 0, &socket);
	CHECK_EQ(error, EINVAL);
	CHECK(socket == WB_OS_SOCKET_NONE);
	error = wb_os_socket_connect(addresses, 0U, 0, NULL);
	CHECK_EQ(error, EINVAL);

	/* The connect result of NONE is EINVAL. */
	error = wb_os_socket_connect_result(WB_OS_SOCKET_NONE);
	CHECK_EQ(error, EINVAL);

	wb_os_addresses_free(addresses);
	if (listener != SYS_INVALID)
		sys_close(listener);
}

/* Checks a non-blocking connect to a listening port and that the socket is private and non-blocking. */
static void
test_connect_flags(
	void)
{
	struct pair pair;
	unsigned char byte;
	size_t received;
	int error;
	int descriptor;

	/* A non-blocking connect ends connected, with the right error before that. */
	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* A receive with nothing arrived does not wait. */
	received = 99U;
	error = wb_os_socket_receive(pair.client, &byte, 1U, &received);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(received, 0);

	error = wb_os_socket_descriptor(pair.client, &descriptor);
	CHECK_EQ(error, 0);

#ifdef _WIN32
	{
		DWORD flags;
		BOOL queried;

		flags = HANDLE_FLAG_INHERIT;
		queried = GetHandleInformation((HANDLE)pair.client, &flags);
		CHECK(queried);
		CHECK_EQ(flags & HANDLE_FLAG_INHERIT, 0);
	}
#else
	{
		int flags;

		flags = fcntl(descriptor, F_GETFD);
		CHECK(flags >= 0);
		CHECK((flags & FD_CLOEXEC) != 0);
		flags = fcntl(descriptor, F_GETFL);
		CHECK(flags >= 0);
		CHECK((flags & O_NONBLOCK) != 0);
	}
#endif
	pair_close(&pair);

	/* A blocking socket is not set non-blocking, but is private too. */
	if (!pair_open(&pair, 1)) {
		pair_close(&pair);
		return;
	}
	error = wb_os_socket_descriptor(pair.client, &descriptor);
	CHECK_EQ(error, 0);
#ifdef _WIN32
	{
		DWORD flags;
		BOOL queried;

		flags = HANDLE_FLAG_INHERIT;
		queried = GetHandleInformation((HANDLE)pair.client, &flags);
		CHECK(queried);
		CHECK_EQ(flags & HANDLE_FLAG_INHERIT, 0);
	}
#else
	{
		int flags;

		flags = fcntl(descriptor, F_GETFD);
		CHECK((flags & FD_CLOEXEC) != 0);
		flags = fcntl(descriptor, F_GETFL);
		CHECK((flags & O_NONBLOCK) == 0);
	}
#endif
	pair_close(&pair);
}

/* Checks a connect that cannot finish at once: the result says EINPROGRESS until it ends. */
static void
test_connect_pending(
	void)
{
	struct wb_os_addresses *addresses;
	wb_os_socket clients[6];
	sys_socket accepted[8];
	unsigned short port;
	sys_socket listener;
	unsigned found;
	size_t made;
	size_t completed;
	size_t pending_index;
	size_t i;
	int error;
	int after;

	/* A listener whose queue holds one connection, then more connects than it can queue. */
	port = 0;
	listener = sys_listen(&port, 1);
	addresses = resolve_loopback(port);
	made = 0;
	completed = 0;
	pending_index = 6U;
	for (i = 0; i < 6U; i++) {
		clients[i] = WB_OS_SOCKET_NONE;
		error = wb_os_socket_connect(addresses, 0U, 0, &clients[i]);
		CHECK(error == 0 || error == EINPROGRESS);
		if (error != 0 && error != EINPROGRESS)
			break;
		made++;

		/* Asked at once, a connect still under way says so. */
		after = wb_os_socket_connect_result(clients[i]);
		CHECK(after == 0 || after == EINPROGRESS);

		/* One that does not finish soon is the one under way; the rest are connected. */
		found = wait_for(clients[i], WB_OS_POLL_WRITABLE, 300);
		if (found == 0U) {
			CHECK_EQ(after, EINPROGRESS);
			pending_index = i;
			break;
		}
		completed++;
		CHECK_EQ(wb_os_socket_connect_result(clients[i]), 0);
	}
	CHECK(pending_index < 6U);

	if (pending_index < 6U) {
		/* Asked again after the wait, it still says it is under way. */
		CHECK_EQ(wb_os_socket_connect_result(clients[pending_index]), EINPROGRESS);

		/* Taking the queued connections makes room, and the connect then ends. */
		for (i = 0; i < completed && i < 8U; i++)
			accepted[i] = sys_accept(listener);
		found = wait_for(clients[pending_index], WB_OS_POLL_WRITABLE, PATIENCE_MS);
		CHECK((found & WB_OS_POLL_WRITABLE) != 0U);
		CHECK_EQ(wb_os_socket_connect_result(clients[pending_index]), 0);
		for (i = 0; i < completed && i < 8U; i++) {
			if (accepted[i] != SYS_INVALID)
				sys_close(accepted[i]);
		}
	}

	for (i = 0; i < made; i++)
		wb_os_socket_close(clients[i]);
	wb_os_addresses_free(addresses);
	if (listener != SYS_INVALID)
		sys_close(listener);
}

/* Checks a connect to a port nobody listens on. */
static void
test_connect_refused(
	void)
{
	struct wb_os_addresses *addresses;
	wb_os_socket socket;
	unsigned short port;
	unsigned found;
	int error;

	port = closed_port();
	addresses = resolve_loopback(port);
	socket = 12345;
	error = wb_os_socket_connect(addresses, 0U, 0, &socket);
	CHECK(error == ECONNREFUSED || error == EINPROGRESS);
	if (error == ECONNREFUSED) {
		/* The connect failed at once: the socket is closed and not given out. */
		CHECK(socket == WB_OS_SOCKET_NONE);
	} else if (error == EINPROGRESS) {
		/* The wait ends with WRITABLE and FAILED, and the result is the refusal. */
		CHECK(socket != WB_OS_SOCKET_NONE);
		found = wait_for(socket, WB_OS_POLL_WRITABLE, PATIENCE_MS);
		CHECK((found & WB_OS_POLL_WRITABLE) != 0U);
		CHECK((found & WB_OS_POLL_FAILED) != 0U);
		CHECK_EQ(wb_os_socket_connect_result(socket), ECONNREFUSED);
		wb_os_socket_close(socket);
	}

	/* A blocking connect to the same port fails with the refusal and gives no socket. */
	socket = 12345;
	error = wb_os_socket_connect(addresses, 0U, 1, &socket);
	CHECK_EQ(error, ECONNREFUSED);
	CHECK(socket == WB_OS_SOCKET_NONE);

	wb_os_addresses_free(addresses);
}

/* Checks the sending and receiving functions on one connection. */
static void
test_send_receive(
	void)
{
	struct pair pair;
	unsigned char bytes[64];
	unsigned char back[64];
	size_t sent;
	size_t received;
	size_t total;
	unsigned found;
	long got;
	int error;
	int tries;

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* Nothing has arrived: a non-blocking receive is EAGAIN and reports 0 bytes. */
	received = 99U;
	error = wb_os_socket_receive(pair.client, back, sizeof(back), &received);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(received, 0);

	/* A send of 0 bytes succeeds with nothing sent, even with no byte pointer. */
	sent = 99U;
	error = wb_os_socket_send(pair.client, bytes, 0U, &sent);
	CHECK_EQ(error, 0);
	CHECK_EQ(sent, 0);
	sent = 99U;
	error = wb_os_socket_send(pair.client, NULL, 0U, &sent);
	CHECK_EQ(error, 0);
	CHECK_EQ(sent, 0);

	/* Bytes with no pointer, no place for the count, no socket, are EINVAL. */
	sent = 99U;
	error = wb_os_socket_send(pair.client, NULL, 5U, &sent);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(sent, 0);
	error = wb_os_socket_send(pair.client, bytes, 5U, NULL);
	CHECK_EQ(error, EINVAL);
	sent = 99U;
	error = wb_os_socket_send(WB_OS_SOCKET_NONE, bytes, 5U, &sent);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(sent, 0);
	received = 99U;
	error = wb_os_socket_receive(pair.client, back, 0U, &received);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(received, 0);
	received = 99U;
	error = wb_os_socket_receive(pair.client, NULL, 5U, &received);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(received, 0);
	error = wb_os_socket_receive(pair.client, back, 5U, NULL);
	CHECK_EQ(error, EINVAL);
	received = 99U;
	error = wb_os_socket_receive(WB_OS_SOCKET_NONE, back, 5U, &received);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(received, 0);

	/* Bytes sent reach the peer exactly. */
	pattern_fill(bytes, 40U, 7U);
	sent = 0;
	error = wb_os_socket_send(pair.client, bytes, 40U, &sent);
	CHECK_EQ(error, 0);
	CHECK_EQ(sent, 40);
	total = 0;
	memset(back, 0, sizeof(back));
	for (tries = 0; tries < 200 && total < 40U; tries++) {
		got = sys_recv_wait(pair.peer, back + total, 40U - total);
		if (got > 0)
			total += (size_t)got;
		else
			break;
	}
	CHECK_EQ(total, 40);
	CHECK(memcmp(back, bytes, 40U) == 0);

	/* Bytes the peer sends make the socket readable, and a receive takes them. */
	got = sys_send(pair.peer, "abc", 3U);
	CHECK_EQ(got, 3);
	found = wait_for(pair.client, WB_OS_POLL_READABLE, PATIENCE_MS);
	CHECK_EQ(found, WB_OS_POLL_READABLE);
	received = 0;
	error = wb_os_socket_receive(pair.client, back, sizeof(back), &received);
	CHECK_EQ(error, 0);
	CHECK_EQ(received, 3);
	CHECK(memcmp(back, "abc", 3U) == 0);

	/* The peer's close is readable, and a receive then gives 0 bytes with 0, every time. */
	sys_close(pair.peer);
	pair.peer = SYS_INVALID;
	found = wait_for(pair.client, WB_OS_POLL_READABLE, PATIENCE_MS);
	CHECK((found & WB_OS_POLL_READABLE) != 0U);
	received = 99U;
	error = wb_os_socket_receive(pair.client, back, sizeof(back), &received);
	CHECK_EQ(error, 0);
	CHECK_EQ(received, 0);
	received = 99U;
	error = wb_os_socket_receive(pair.client, back, sizeof(back), &received);
	CHECK_EQ(error, 0);
	CHECK_EQ(received, 0);

	pair_close(&pair);
}

/* Checks a send to a peer that never reads: the buffers fill and the send says EAGAIN. */
static void
test_send_full(
	void)
{
	struct pair pair;
	unsigned char *outgoing;
	unsigned char *incoming;
	size_t total;
	size_t sent;
	size_t taken;
	size_t i;
	unsigned found;
	long got;
	int error;
	int full;

	outgoing = (unsigned char *)malloc(CHUNK_SIZE);
	incoming = (unsigned char *)malloc(CHUNK_SIZE);
	CHECK(outgoing != NULL);
	CHECK(incoming != NULL);
	if (outgoing == NULL || incoming == NULL) {
		free(outgoing);
		free(incoming);
		return;
	}

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		free(outgoing);
		free(incoming);
		return;
	}

	/* Sends a numbered stream until the system takes no more. */
	total = 0;
	full = 0;
	for (i = 0; i < 20000U; i++) {
		pattern_fill(outgoing, CHUNK_SIZE, total);
		sent = 99U;
		error = wb_os_socket_send(pair.client, outgoing, CHUNK_SIZE, &sent);
		if (error == EAGAIN) {
			CHECK_EQ(sent, 0);
			full = 1;
			break;
		}
		CHECK_EQ(error, 0);
		if (error != 0)
			break;
		CHECK(sent <= CHUNK_SIZE);
		total += sent;
	}
	CHECK(full);
	CHECK(total > 0U);
	printf("note: %zu bytes were sent before the send said EAGAIN\n", total);

	/* The full socket is not writable. */
	found = wait_for(pair.client, WB_OS_POLL_WRITABLE, 0);
	CHECK_EQ(found, 0);

	/* The peer reads the whole stream: every byte arrives in order, so the counts of sent were exact. */
	taken = 0;
	while (taken < total) {
		got = sys_recv_wait(pair.peer, incoming, CHUNK_SIZE);
		CHECK(got > 0);
		if (got <= 0)
			break;
		CHECK(pattern_ok(incoming, (size_t)got, taken));
		taken += (size_t)got;
	}
	CHECK_EQ(taken, total);

	/* With room again the socket is writable and a send takes bytes. */
	found = wait_for(pair.client, WB_OS_POLL_WRITABLE, PATIENCE_MS);
	CHECK((found & WB_OS_POLL_WRITABLE) != 0U);
	sent = 0;
	error = wb_os_socket_send(pair.client, "x", 1U, &sent);
	CHECK_EQ(error, 0);
	CHECK_EQ(sent, 1);

	pair_close(&pair);
	free(outgoing);
	free(incoming);
}

/* Checks a send after the peer closed: an error, not a signal that ends the process. */
static void
test_send_closed(
	void)
{
	struct pair pair;
	size_t sent;
	int error;
	int tries;
	int failed;

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* The peer closes at once; the first send may go through, a later one fails. */
	sys_close(pair.peer);
	pair.peer = SYS_INVALID;
	failed = 0;
	error = 0;
	for (tries = 0; tries < 100 && !failed; tries++) {
		sent = 99U;
		error = wb_os_socket_send(pair.client, "x", 1U, &sent);
		if (error != 0) {
			failed = 1;
			CHECK_EQ(sent, 0);
		} else {
			CHECK_EQ(sent, 1);
			sys_sleep(20U);
		}
	}
	CHECK(failed);
	CHECK(error == EPIPE || error == ECONNRESET || error == ECONNABORTED);

	pair_close(&pair);
}

/* Checks a receive on a connection the peer reset. */
static void
test_receive_reset(
	void)
{
	struct pair pair;
	unsigned char back[16];
	size_t received;
	unsigned found;
	int error;

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* The peer closes with a reset; the socket is readable and failed, and the receive says reset. */
	sys_reset_on_close(pair.peer);
	sys_close(pair.peer);
	pair.peer = SYS_INVALID;
	found = wait_for(pair.client, WB_OS_POLL_READABLE, PATIENCE_MS);
	CHECK((found & WB_OS_POLL_READABLE) != 0U);
	received = 99U;
	error = wb_os_socket_receive(pair.client, back, sizeof(back), &received);
	CHECK_EQ(error, ECONNRESET);
	CHECK_EQ(received, 0);

	pair_close(&pair);
}

/* Checks the socket functions on a socket that was closed. */
static void
test_closed_socket(
	void)
{
	struct pair pair;
	wb_os_socket gone;
	unsigned char byte;
	size_t count;
	int error;

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* A closed socket is not a socket: sends and receives are EINVAL. */
	gone = pair.client;
	wb_os_socket_close(gone);
	pair.client = WB_OS_SOCKET_NONE;
	count = 99U;
	error = wb_os_socket_receive(gone, &byte, 1U, &count);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(count, 0);
	count = 99U;
	error = wb_os_socket_send(gone, "x", 1U, &count);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(count, 0);

	/* Closing NONE does nothing. */
	wb_os_socket_close(WB_OS_SOCKET_NONE);

#ifndef _WIN32
	/* A closed descriptor in a wait is reported as failed, and ready for whatever was asked. */
	{
		struct wb_os_poll_item item;
		size_t ready;

		item.socket = gone;
		item.wants = WB_OS_POLL_READABLE;
		item.found = 0;
		ready = 0;
		error = wb_os_poll(&item, 1U, 0, &ready);
		CHECK_EQ(error, 0);
		CHECK_EQ(ready, 1);
		CHECK_EQ(item.found, WB_OS_POLL_READABLE | WB_OS_POLL_FAILED);
		item.wants = WB_OS_POLL_WRITABLE;
		error = wb_os_poll(&item, 1U, 0, &ready);
		CHECK_EQ(error, 0);
		CHECK_EQ(item.found, WB_OS_POLL_WRITABLE | WB_OS_POLL_FAILED);
	}
#endif

	pair_close(&pair);
}

/* Checks wb_os_socket_descriptor and wb_os_socket_last_error. */
static void
test_descriptor_and_error(
	void)
{
	struct pair pair;
	unsigned char byte;
	wb_os_socket gone;
	int descriptor;
	int error;

	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}

	/* The descriptor is a positive int that is the socket's own number. */
	descriptor = -5;
	error = wb_os_socket_descriptor(pair.client, &descriptor);
	CHECK_EQ(error, 0);
	CHECK(descriptor > 0);
	CHECK_EQ(descriptor, (int)pair.client);

	/* NONE, a missing place, and a value an int cannot hold are refused. */
	descriptor = -5;
	error = wb_os_socket_descriptor(WB_OS_SOCKET_NONE, &descriptor);
	CHECK_EQ(error, EINVAL);
	error = wb_os_socket_descriptor(pair.client, NULL);
	CHECK_EQ(error, EINVAL);
	error = wb_os_socket_descriptor((wb_os_socket)-7, &descriptor);
	CHECK_EQ(error, EINVAL);
	if (sizeof(wb_os_socket) > sizeof(int)) {
		error = wb_os_socket_descriptor((wb_os_socket)INT_MAX + 1, &descriptor);
		CHECK_EQ(error, ERANGE);
		error = wb_os_socket_descriptor((wb_os_socket)INT_MAX, &descriptor);
		CHECK_EQ(error, 0);
		CHECK_EQ(descriptor, INT_MAX);
	}

#ifdef _WIN32
	/* A receive on a closed socket leaves WSAENOTSOCK, which the layer reports as ENOTSOCK. */
	gone = pair.client;
	closesocket((SOCKET)gone);
	pair.client = WB_OS_SOCKET_NONE;
	recv((SOCKET)gone, (char *)&byte, 1, 0);
	CHECK_EQ(WSAGetLastError(), WSAENOTSOCK);
	CHECK_EQ(wb_os_socket_last_error(), ENOTSOCK);

	/* The error is read from the calling thread's Windows Sockets error, mapped by the header's table. */
	WSASetLastError(WSAECONNRESET);
	CHECK_EQ(wb_os_socket_last_error(), ECONNRESET);
	WSASetLastError(WSAETIMEDOUT);
	CHECK_EQ(wb_os_socket_last_error(), ETIMEDOUT);
	WSASetLastError(WSAEWOULDBLOCK);
	CHECK_EQ(wb_os_socket_last_error(), EAGAIN);
	WSASetLastError(0);
	CHECK_EQ(wb_os_socket_last_error(), 0);
#else
	{
		int pipe_ends[2];
		int status;

		/* A receive on a closed descriptor leaves EBADF, and on a descriptor that is no socket ENOTSOCK. */
		gone = pair.client;
		close((int)gone);
		pair.client = WB_OS_SOCKET_NONE;
		errno = 0;
		recv((int)gone, &byte, 1, 0);
		CHECK_EQ(wb_os_socket_last_error(), EBADF);
		status = pipe(pipe_ends);
		CHECK_EQ(status, 0);
		errno = 0;
		recv(pipe_ends[0], &byte, 1, 0);
		CHECK_EQ(wb_os_socket_last_error(), ENOTSOCK);
		close(pipe_ends[0]);
		close(pipe_ends[1]);

		/* The error is errno itself, and 0 when there is none. */
		errno = ECONNRESET;
		CHECK_EQ(wb_os_socket_last_error(), ECONNRESET);
		errno = 0;
		CHECK_EQ(wb_os_socket_last_error(), 0);
	}
#endif

	pair_close(&pair);
}

/* Checks the arguments and the timing of wb_os_poll. */
static void
test_poll_basics(
	void)
{
	static struct wb_os_poll_item big[WB_OS_POLL_MAX + 1U];
	struct wb_os_poll_item items[3];
	struct wb_os_waker *waker;
	struct pair pair;
	size_t ready;
	size_t i;
	double started;
	double elapsed;
	int error;

	error = wb_os_waker_open(&waker);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* A timeout of 0 does not wait and finds nothing on a quiet socket. */
	items[0].socket = wb_os_waker_socket(waker);
	items[0].wants = WB_OS_POLL_READABLE;
	items[0].found = 0xffU;
	ready = 99U;
	started = sys_now();
	error = wb_os_poll(items, 1U, 0, &ready);
	elapsed = sys_now() - started;
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	CHECK_EQ(items[0].found, 0);
	CHECK(elapsed < 1000.0);

	/* A wait of 50 ms on a quiet socket takes at least 40 ms by the test's own clock and finds nothing. */
	items[0].found = 0xffU;
	ready = 99U;
	started = sys_now();
	error = wb_os_poll(items, 1U, 50, &ready);
	elapsed = sys_now() - started;
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	CHECK_EQ(items[0].found, 0);
	CHECK(elapsed >= 40.0);
	CHECK(elapsed < 5000.0);

	/* An item that holds NONE is skipped: its found is 0 and the others are still waited for. */
	wb_os_waker_ring(waker);
	items[0].socket = WB_OS_SOCKET_NONE;
	items[0].wants = WB_OS_POLL_READABLE;
	items[0].found = 0xffU;
	items[1].socket = wb_os_waker_socket(waker);
	items[1].wants = WB_OS_POLL_READABLE;
	items[1].found = 0;
	items[2].socket = WB_OS_SOCKET_NONE;
	items[2].wants = 0;
	items[2].found = 0xffU;
	ready = 99U;
	error = wb_os_poll(items, 3U, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 1);
	CHECK_EQ(items[0].found, 0);
	CHECK_EQ(items[1].found, WB_OS_POLL_READABLE);
	CHECK_EQ(items[2].found, 0);
	wb_os_waker_drain(waker);

	/* The same socket in two items marks both. */
	wb_os_waker_ring(waker);
	items[0].socket = wb_os_waker_socket(waker);
	items[0].wants = WB_OS_POLL_READABLE;
	items[0].found = 0;
	items[1] = items[0];
	ready = 99U;
	error = wb_os_poll(items, 2U, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 2);
	CHECK_EQ(items[0].found, WB_OS_POLL_READABLE);
	CHECK_EQ(items[1].found, WB_OS_POLL_READABLE);
	wb_os_waker_drain(waker);

	/* An item that asks for nothing is EINVAL, whatever else is in the list. */
	items[0].socket = wb_os_waker_socket(waker);
	items[0].wants = 0;
	items[0].found = 0;
	ready = 99U;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, EINVAL);
	items[0].wants = WB_OS_POLL_FAILED;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, EINVAL);

	/* A timeout below -1, missing items, a missing place for the count are EINVAL. */
	items[0].wants = WB_OS_POLL_READABLE;
	error = wb_os_poll(items, 1U, -2, &ready);
	CHECK_EQ(error, EINVAL);
	error = wb_os_poll(items, 1U, INT_MIN, &ready);
	CHECK_EQ(error, EINVAL);
	error = wb_os_poll(NULL, 1U, 0, &ready);
	CHECK_EQ(error, EINVAL);
	error = wb_os_poll(items, 1U, 0, NULL);
	CHECK_EQ(error, EINVAL);

	/* No item at all: 0 does not wait, and an endless wait for nothing is EINVAL. */
	ready = 99U;
	error = wb_os_poll(NULL, 0U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	error = wb_os_poll(NULL, 0U, -1, &ready);
	CHECK_EQ(error, EINVAL);
	items[0].socket = WB_OS_SOCKET_NONE;
	error = wb_os_poll(items, 1U, -1, &ready);
	CHECK_EQ(error, EINVAL);

	/* With no item and a timeout of 20 ms the call sleeps for it. */
	ready = 99U;
	started = sys_now();
	error = wb_os_poll(NULL, 0U, 20, &ready);
	elapsed = sys_now() - started;
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	CHECK(elapsed >= 15.0);
	CHECK(elapsed < 5000.0);
	ready = 99U;
	started = sys_now();
	items[0].socket = WB_OS_SOCKET_NONE;
	error = wb_os_poll(items, 1U, 20, &ready);
	elapsed = sys_now() - started;
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	CHECK(elapsed >= 15.0);

	/* The largest list is taken; one more is EINVAL. */
	for (i = 0; i < WB_OS_POLL_MAX + 1U; i++) {
		big[i].socket = WB_OS_SOCKET_NONE;
		big[i].wants = WB_OS_POLL_READABLE;
		big[i].found = 0xffU;
	}
	big[WB_OS_POLL_MAX - 1U].socket = wb_os_waker_socket(waker);
	wb_os_waker_ring(waker);
	ready = 99U;
	error = wb_os_poll(big, WB_OS_POLL_MAX, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 1);
	CHECK_EQ(big[WB_OS_POLL_MAX - 1U].found, WB_OS_POLL_READABLE);
	CHECK_EQ(big[0].found, 0);
	wb_os_waker_drain(waker);
	ready = 99U;
	error = wb_os_poll(big, WB_OS_POLL_MAX + 1U, 0, &ready);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(ready, 0);

	wb_os_waker_close(waker);

	/* A connected socket that wants a send is writable at once; one that wants a receive waits for data. */
	if (!pair_open(&pair, 0)) {
		pair_close(&pair);
		return;
	}
	items[0].socket = pair.client;
	items[0].wants = WB_OS_POLL_READABLE | WB_OS_POLL_WRITABLE;
	items[0].found = 0;
	ready = 99U;
	error = wb_os_poll(items, 1U, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 1);
	CHECK_EQ(items[0].found, WB_OS_POLL_WRITABLE);
	sys_send(pair.peer, "z", 1U);
	for (i = 0; i < 100U; i++) {
		ready = 0;
		items[0].found = 0;
		error = wb_os_poll(items, 1U, 100, &ready);
		CHECK_EQ(error, 0);
		if ((items[0].found & WB_OS_POLL_READABLE) != 0U)
			break;
	}
	CHECK_EQ(items[0].found, WB_OS_POLL_READABLE | WB_OS_POLL_WRITABLE);
	pair_close(&pair);
}

/* Checks a wait over many wakers, more than Windows' FD_SETSIZE of 64, with one of them ringing. */
static void
test_poll_many(
	void)
{
	struct wb_os_waker *wakers[100];
	struct wb_os_poll_item items[100];
	size_t ready;
	size_t i;
	size_t opened;
	int error;

	opened = 0;
	for (i = 0; i < 100U; i++) {
		wakers[i] = NULL;
		error = wb_os_waker_open(&wakers[i]);
		CHECK_EQ(error, 0);
		if (error != 0)
			break;
		opened++;
		items[i].socket = wb_os_waker_socket(wakers[i]);
		items[i].wants = WB_OS_POLL_READABLE;
		items[i].found = 0xffU;
	}

	if (opened == 100U) {
		/* Nothing rung: nothing ready. */
		ready = 99U;
		error = wb_os_poll(items, 100U, 0, &ready);
		CHECK_EQ(error, 0);
		CHECK_EQ(ready, 0);

		/* One rung waker, the last and the first and one in the middle in turn: exactly that item is readable. */
		for (i = 0; i < 100U; i += 33U) {
			wb_os_waker_ring(wakers[i]);
			ready = 99U;
			error = wb_os_poll(items, 100U, PATIENCE_MS, &ready);
			CHECK_EQ(error, 0);
			CHECK_EQ(ready, 1);
			CHECK_EQ(items[i].found, WB_OS_POLL_READABLE);
			CHECK_EQ(items[(i + 1U) % 100U].found, 0);
			CHECK_EQ(items[(i + 50U) % 100U].found, 0);
			wb_os_waker_drain(wakers[i]);
		}
		ready = 99U;
		wb_os_waker_ring(wakers[99]);
		error = wb_os_poll(items, 100U, PATIENCE_MS, &ready);
		CHECK_EQ(error, 0);
		CHECK_EQ(ready, 1);
		CHECK_EQ(items[99].found, WB_OS_POLL_READABLE);
		CHECK_EQ(items[0].found, 0);
		wb_os_waker_drain(wakers[99]);
	}

	for (i = 0; i < opened; i++)
		wb_os_waker_close(wakers[i]);
}

/* The thread that rings a waker after a delay. */
static void *
ringer_thread(
	void *argument)
{
	struct ringer *ringer;

	ringer = (struct ringer *)argument;
	sys_sleep((unsigned)ringer->delay_ms);
	wb_os_waker_ring(ringer->waker);

	return NULL;
}

/* Checks the waker: ringing, draining, a full buffer and ringing from another thread. */
static void
test_waker(
	void)
{
	struct wb_os_waker *waker;
	struct wb_os_waker *backup;
	struct wb_os_waker *other;
	struct wb_os_poll_item items[2];
	struct ringer ringer;
	struct ringer watchdog;
	pthread_t thread;
	pthread_t watch_thread;
	wb_os_socket first;
	wb_os_socket second;
	size_t ready;
	double started;
	double elapsed;
	int descriptor;
	int error;
	int round;
	int created;
	int i;

	/* Argument checks: no place for the waker is EINVAL; NULL wakers are accepted everywhere. */
	error = wb_os_waker_open(NULL);
	CHECK_EQ(error, EINVAL);
	wb_os_waker_ring(NULL);
	wb_os_waker_drain(NULL);
	wb_os_waker_close(NULL);
	CHECK(wb_os_waker_socket(NULL) == WB_OS_SOCKET_NONE);
	waker = (struct wb_os_waker *)&sentinel_target;
	error = wb_os_waker_open(&waker);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(waker != NULL);
	first = wb_os_waker_socket(waker);
	CHECK(first != WB_OS_SOCKET_NONE);
	error = wb_os_socket_descriptor(first, &descriptor);
	CHECK_EQ(error, 0);
	CHECK(descriptor > 0);

	/* Two wakers have sockets of their own. */
	other = NULL;
	error = wb_os_waker_open(&other);
	CHECK_EQ(error, 0);
	second = wb_os_waker_socket(other);
	CHECK(second != WB_OS_SOCKET_NONE);
	CHECK(second != first);

	/* The waker's socket is private to this process and, on POSIX, non-blocking. */
#ifdef _WIN32
	{
		DWORD flags;
		BOOL queried;

		flags = HANDLE_FLAG_INHERIT;
		queried = GetHandleInformation((HANDLE)first, &flags);
		CHECK(queried);
		CHECK_EQ(flags & HANDLE_FLAG_INHERIT, 0);
	}
#else
	{
		int flags;

		flags = fcntl((int)first, F_GETFD);
		CHECK(flags >= 0);
		CHECK((flags & FD_CLOEXEC) != 0);
		flags = fcntl((int)first, F_GETFL);
		CHECK(flags >= 0);
		CHECK((flags & O_NONBLOCK) != 0);
	}
#endif

	/* A new waker is quiet; ringing makes it readable; draining makes it quiet. */
	items[0].socket = first;
	items[0].wants = WB_OS_POLL_READABLE;
	items[0].found = 0;
	ready = 99U;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	wb_os_waker_ring(waker);
	ready = 0;
	error = wb_os_poll(items, 1U, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 1);
	CHECK_EQ(items[0].found, WB_OS_POLL_READABLE);
	wb_os_waker_drain(waker);
	ready = 99U;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);
	CHECK_EQ(items[0].found, 0);

	/* Several rings are all taken by one drain. */
	for (i = 0; i < 5; i++)
		wb_os_waker_ring(waker);
	wb_os_waker_drain(waker);
	ready = 99U;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);

	/* Draining a quiet waker does nothing and does not wait. */
	wb_os_waker_drain(waker);

	/* Ringing 100000 times without a drain does not block, and one drain empties it. */
	started = sys_now();
	for (i = 0; i < 100000; i++)
		wb_os_waker_ring(waker);
	elapsed = sys_now() - started;
	CHECK(elapsed < 60000.0);
	ready = 0;
	error = wb_os_poll(items, 1U, PATIENCE_MS, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 1);
	wb_os_waker_drain(waker);
	ready = 99U;
	error = wb_os_poll(items, 1U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);

	/* The other waker did not hear any of this. */
	items[1].socket = second;
	items[1].wants = WB_OS_POLL_READABLE;
	items[1].found = 0xffU;
	ready = 99U;
	error = wb_os_poll(&items[1], 1U, 0, &ready);
	CHECK_EQ(error, 0);
	CHECK_EQ(ready, 0);

	/* A thread's ring ends another thread's endless wait; a backup ring after 8 s keeps a broken wake from hanging the test. */
	backup = NULL;
	error = wb_os_waker_open(&backup);
	CHECK_EQ(error, 0);
	if (error == 0) {
		for (round = 0; round < 10; round++) {
			ringer.waker = waker;
			ringer.delay_ms = 20;
			watchdog.waker = backup;
			watchdog.delay_ms = 8000;
			items[0].socket = wb_os_waker_socket(waker);
			items[0].wants = WB_OS_POLL_READABLE;
			items[0].found = 0;
			items[1].socket = wb_os_waker_socket(backup);
			items[1].wants = WB_OS_POLL_READABLE;
			items[1].found = 0;
			created = pthread_create(&thread, NULL, ringer_thread, &ringer);
			CHECK_EQ(created, 0);
			created = pthread_create(&watch_thread, NULL, ringer_thread, &watchdog);
			CHECK_EQ(created, 0);
			ready = 0;
			started = sys_now();
			error = wb_os_poll(items, 2U, -1, &ready);
			elapsed = sys_now() - started;
			CHECK_EQ(error, 0);
			CHECK_EQ(items[0].found, WB_OS_POLL_READABLE);
			CHECK_EQ(items[1].found, 0);
			CHECK(elapsed < 5000.0);
			pthread_join(thread, NULL);
			wb_os_waker_drain(waker);
			if (round == 9)
				pthread_join(watch_thread, NULL);
			else
				pthread_detach(watch_thread);
			wb_os_waker_drain(backup);
		}
		/* The detached watchdogs ring the backup late; waits for them to finish before it is closed. */
		sys_sleep(8500U);
		wb_os_waker_close(backup);
	}

	wb_os_waker_close(other);
	wb_os_waker_close(waker);
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name. */
	CHECK(argc == 2);
	if (argc != 2)
		return check_finish("os-net");
	is_win32 = strcmp(argv[1], "win32") == 0;
	CHECK(is_win32 || strcmp(argv[1], "posix") == 0);

#ifndef _WIN32
	/* The default action of SIGPIPE must hold, so only the module's own care keeps a closed peer from killing the test. */
	signal(SIGPIPE, SIG_DFL);
#endif
	sys_start();

	test_resolve();
	test_connect_arguments();
	test_connect_flags();
	test_connect_refused();
	test_connect_pending();
	test_echo();
	test_send_receive();
	test_send_full();
	test_send_closed();
	test_receive_reset();
	test_closed_socket();
	test_descriptor_and_error();
	test_poll_basics();
	test_poll_many();
	test_waker();

	return check_finish("os-net");
}
