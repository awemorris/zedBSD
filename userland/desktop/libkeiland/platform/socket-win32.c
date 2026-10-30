/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program's network on Windows (<keiland/keiland_socket.h>): WinSock 2
 * sockets, select for the wait (not WSAPoll, below), and a pair of loopback
 * TCP sockets for each waker.
 *
 * The contract of every function is the header's; the errors are the POSIX
 * names of <errno.h> (socket_error turns WinSock's into them).  What
 * differs from POSIX underneath:
 *
 * - WinSock is started once (WSAStartup 2.2 under a kl_once) by every
 *   function that needs it; its failure is that function's error (EIO, or
 *   ENOMEM for WSA_NOT_ENOUGH_MEMORY).  WSACleanup is not called: the
 *   system ends WinSock with the process.
 * - A socket is opened overlapped (WSA_FLAG_OVERLAPPED): a socket opened
 *   without it is a synchronous file object, and one thread's blocking
 *   receive would hold up another thread's send.  It is not inherited by a
 *   child process (WSA_FLAG_NO_HANDLE_INHERIT).
 * - There is no SIGPIPE.
 * - WSAPoll before Windows 10 version 2004 does not report a connect that
 *   failed (no POLLERR, no POLLHUP: the wait runs to its end), so the wait
 *   is select, which reports it in the exception set.
 */

#include <keiland/keiland_socket.h>
#include <keiland/keiland_thread.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <iphlpapi.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bytes a waker's socket is drained by at a time. */
#define SOCKET_DRAIN		64U

/* The longest address text kl_address_parse reads, scope and NUL included. */
#define SOCKET_TEXT_MAX		64U

/* Milliseconds in a second, and microseconds in a millisecond. */
#define SOCKET_MS_SECOND	1000
#define SOCKET_US_MILLISECOND	1000

/* The connections a waker's listening socket holds while it takes its own. */
#define SOCKET_WAKER_BACKLOG	4

/*
 * A socket the library opened: the system's handle, its family and kind
 * (KL_ADDRESS_*, KL_SOCKET_*), whether it is non-blocking (WinSock has no
 * call that reads that back, and kl_socket_accept and the blocking
 * connect's wait need it), and whether a connect that did not block is
 * under way (older WinSock answers a second connect then with WSAEINVAL,
 * which is also what a connect on a listening socket gets).
 */
struct kl_socket {
	SOCKET handle;
	unsigned family;
	unsigned kind;
	int nonblocking;
	int connecting;
};

/*
 * A waker: a connected pair of loopback TCP sockets (ends[0] is waited for
 * and read, ends[1] written; both non-blocking) and whether a byte waits
 * unread (rung: 0 or 1, changed with InterlockedCompareExchange and
 * InterlockedExchange).
 *
 * kl_socket_waker_ring sends one byte only when it moves rung from 0 to 1.
 * kl_socket_poll, finding ends[0] readable, first reads it empty and only
 * then puts rung back to 0: the other order would let a ring between the
 * two leave rung at 1 with nothing to read, and no ring would ever wake a
 * wait again (the same order as the POSIX pipe's).  UDP is not used: a
 * datagram may be dropped, and a dropped ring leaves rung at 1 for ever.
 */
struct kl_socket_waker {
	SOCKET ends[2];
	LONG rung;
};

/*
 * Whether WinSock was started for the process: the kl_once socket_start
 * passes.
 */
static struct kl_once socket_started = KL_ONCE_INIT;

/*
 * What WinSock's start gave: 0, or the error every function then returns.
 * Written once inside the kl_once and read after it.
 */
static int socket_start_error;

static int socket_start(void);
static void socket_start_once(void);
static int socket_error(int error);
static int socket_to_system(const struct kl_address *address, struct sockaddr_storage *system, int *length);
static int socket_from_system(const struct sockaddr *system, int length, struct kl_address *address);
static int socket_domain(unsigned family, int *domain);
static int socket_type(unsigned kind, int *type);
static int socket_resolve_error(int answer);
static int socket_set_blocking(SOCKET handle, int nonblocking);
static int socket_make(int domain, int type, SOCKET *handle);
static int socket_no_inherit(SOCKET handle);
static int socket_set_int(SOCKET handle, int level, int name, int value);
static int socket_poll_check(struct kl_socket_wait *waits, size_t count, int milliseconds);
static void socket_clock(ULONGLONG *rate, ULONGLONG *now);
static int socket_poll_left(ULONGLONG rate, ULONGLONG start, ULONGLONG wanted);
static void socket_poll_fill(const struct kl_socket_wait *waits, size_t count, fd_set *readable, fd_set *writable, fd_set *failed);
static int socket_in_set(const fd_set *set, SOCKET handle);
static unsigned socket_poll_found(const struct kl_socket_wait *wait, const fd_set *readable, const fd_set *writable, const fd_set *failed);
static void socket_waker_drain(struct kl_socket_waker *waker);
static int socket_waker_pair(SOCKET ends[2]);

/*
 * Reads an address written in numbers, with a port.
 *
 * inet_pton (InetPtonA, Windows Vista and later) reads each family; the
 * IPv6 "%scope" part is cut off first and read as a number, or as an
 * interface's name with if_nametoindex.
 */
int
kl_address_parse(
	const char *text,
	uint16_t port,
	struct kl_address *address)
{
	char copy[SOCKET_TEXT_MAX];
	struct in_addr ipv4;
	struct in6_addr ipv6;
	unsigned long number;
	unsigned index;
	size_t length;
	char *scope;
	char *end;
	int found;

	/* Nothing read yet. */
	memset(address, 0, sizeof(*address));

	/* A copy the scope can be cut from. */
	length = strlen(text);
	if (length == 0U || length >= sizeof(copy))
		return EINVAL;
	memcpy(copy, text, length + 1U);
	scope = strchr(copy, '%');
	if (scope != NULL) {
		*scope = '\0';
		scope++;
	}

	/* An IPv4 address, which has no scope. */
	found = inet_pton(AF_INET, copy, &ipv4);
	if (found == 1) {
		if (scope != NULL)
			return EINVAL;
		address->family = KL_ADDRESS_IPV4;
		address->port = port;
		memcpy(address->bytes, &ipv4, sizeof(ipv4));
		return 0;
	}

	/* An IPv6 address. */
	found = inet_pton(AF_INET6, copy, &ipv6);
	if (found != 1)
		return EINVAL;
	address->family = KL_ADDRESS_IPV6;
	address->port = port;
	memcpy(address->bytes, &ipv6, sizeof(ipv6));

	/* Its scope: an interface's index, or its name. */
	if (scope != NULL) {
		if (*scope == '\0')
			return EINVAL;
		errno = 0;
		number = strtoul(scope, &end, 10);
		if (*end != '\0') {
			index = (unsigned)if_nametoindex(scope);
			if (index == 0U)
				return EINVAL;
			number = index;
		} else if (errno != 0 || number > UINT32_MAX) {
			return EINVAL;
		}

		/* The scope is the interface's index. */
		address->scope = (uint32_t)number;
	}

	/* Succeeded: the address is read. */
	return 0;
}

/*
 * Writes an address and its port as text.
 *
 * inet_ntop (InetNtopA) writes the numbers, and the "%scope" and the port
 * are added as on POSIX.  ENOSPC when it does not fit.
 */
int
kl_address_format(
	const struct kl_address *address,
	char *text,
	size_t size)
{
	char numbers[INET6_ADDRSTRLEN];
	const char *written;
	int length;

	/* The address in numbers. */
	if (address->family == KL_ADDRESS_IPV4) {
		written = inet_ntop(AF_INET, address->bytes, numbers, sizeof(numbers));
	} else if (address->family == KL_ADDRESS_IPV6) {
		written = inet_ntop(AF_INET6, address->bytes, numbers, sizeof(numbers));
	} else {
		return EAFNOSUPPORT;
	}

	/* An address that could not be written has no text. */
	if (written == NULL)
		return EAFNOSUPPORT;

	/* With the port: an IPv6 address in brackets, with its scope. */
	if (address->family == KL_ADDRESS_IPV4) {
		length = snprintf(text, size, "%s:%u", numbers, (unsigned)address->port);
	} else if (address->scope != 0U) {
		length = snprintf(text, size, "[%s%%%u]:%u", numbers, (unsigned)address->scope, (unsigned)address->port);
	} else {
		length = snprintf(text, size, "[%s]:%u", numbers, (unsigned)address->port);
	}

	/* The text must fit in the room given. */
	if (length < 0 || (size_t)length >= size)
		return ENOSPC;

	/* Succeeded: the text is whole. */
	return 0;
}

/*
 * Makes the address of every interface of a family (0.0.0.0, ::) with a
 * port: the same bytes as on POSIX, no system call.
 */
void
kl_address_any(
	unsigned family,
	uint16_t port,
	struct kl_address *address)
{
	/* All zero bytes: 0.0.0.0 or ::. */
	memset(address, 0, sizeof(*address));
	address->family = family;
	address->port = port;
}

/*
 * Makes the loopback address of a family (127.0.0.1, ::1) with a port: the
 * same bytes as on POSIX, no system call.
 */
void
kl_address_loopback(
	unsigned family,
	uint16_t port,
	struct kl_address *address)
{
	/* 127.0.0.1, or ::1. */
	memset(address, 0, sizeof(*address));
	address->family = family;
	address->port = port;
	if (family == KL_ADDRESS_IPV4) {
		address->bytes[0] = 127U;
		address->bytes[3] = 1U;
	} else {
		address->bytes[15] = 1U;
	}
}

/*
 * Finds the addresses of a host for a kind of socket, in the system's
 * order.
 *
 * GetAddrInfoW answers for the name made UTF-16 (a name that is not UTF-8
 * is ENOENT), with AI_ADDRCONFIG not given (as on POSIX); the port is put
 * into each answer here, since no service name is passed.
 */
int
kl_socket_resolve(
	const char *host,
	uint16_t port,
	unsigned family,
	unsigned kind,
	struct kl_address *addresses,
	size_t capacity,
	size_t *count)
{
	ADDRINFOW hints;
	ADDRINFOW *found;
	ADDRINFOW *entry;
	WCHAR *name;
	int name_count;
	int converted;
	int answer;
	int error;

	/* Nothing found yet. */
	*count = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* Refuses a call without a host or without room for the answers. */
	if (host == NULL || capacity == 0U)
		return EINVAL;

	/* The family and the kind asked for. */
	memset(&hints, 0, sizeof(hints));
	if (family == KL_ADDRESS_ANY) {
		hints.ai_family = AF_UNSPEC;
	} else {
		error = socket_domain(family, &hints.ai_family);
		if (error != 0)
			return error;
	}

	/* The system's type of the kind asked for. */
	error = socket_type(kind, &hints.ai_socktype);
	if (error != 0)
		return error;

	/* The size the name needs in UTF-16 (0: it is not UTF-8). */
	name_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host, -1, NULL, 0);
	if (name_count <= 0)
		return ENOENT;

	/* The name in UTF-16. */
	name = malloc((size_t)name_count * sizeof(*name));
	if (name == NULL)
		return ENOMEM;
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host, -1, name, name_count);
	if (converted <= 0) {
		free(name);
		return ENOENT;
	}

	/* The system's answer. */
	found = NULL;
	answer = GetAddrInfoW(name, NULL, &hints, &found);
	free(name);
	if (answer != 0)
		return socket_resolve_error(answer);

	/* Each address of a family the interface knows, in the system's order, while there is room. */
	for (entry = found; entry != NULL && *count < capacity; entry = entry->ai_next) {
		error = socket_from_system(entry->ai_addr, (int)entry->ai_addrlen, &addresses[*count]);
		if (error == 0) {
			addresses[*count].port = port;
			(*count)++;
		}
	}

	/* The system's list goes. */
	FreeAddrInfoW(found);

	/* A host with no address of a known family. */
	if (*count == 0U)
		return ENOENT;

	/* Succeeded: *count addresses. */
	return 0;
}

/*
 * Opens a socket of a family and kind.
 *
 * It is overlapped and kept from child processes (socket_make), made
 * non-blocking with ioctlsocket(FIONBIO) when asked, and given IPV6_V6ONLY 1
 * (Windows' default too, set all the same).  A UDP socket has
 * SIO_UDP_CONNRESET turned off: Windows alone makes an ICMP port
 * unreachable a WSAECONNRESET of the next receive of a socket that is not
 * connected.  A failure after the socket was made closes it.
 */
int
kl_socket_open(
	unsigned family,
	unsigned kind,
	unsigned flags,
	struct kl_socket **socket_made)
{
	struct kl_socket *made;
	DWORD returned;
	BOOL behavior;
	int domain;
	int type;
	int status;
	int wsa;
	int error;

	/* Nothing open yet. */
	*socket_made = NULL;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's names of the family and the kind. */
	error = socket_domain(family, &domain);
	if (error != 0)
		return error;
	error = socket_type(kind, &type);
	if (error != 0)
		return error;
	if ((flags & ~KL_SOCKET_NONBLOCKING) != 0U)
		return EINVAL;

	/* The record. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->family = family;
	made->kind = kind;

	/* The socket, overlapped and kept from the programs the application starts. */
	error = socket_make(domain, type, &made->handle);
	if (error != 0) {
		free(made);
		return error;
	}

	/* Non-blocking when asked (a new socket blocks). */
	if ((flags & KL_SOCKET_NONBLOCKING) != 0U) {
		error = socket_set_blocking(made->handle, 1);
		if (error != 0) {
			(void)closesocket(made->handle);
			free(made);
			return error;
		}

		/* The record keeps the mode, which the system cannot tell. */
		made->nonblocking = 1;
	}

	/* IPv6 alone on an IPv6 socket, whatever the system's default. */
	if (family == KL_ADDRESS_IPV6) {
		error = socket_set_int(made->handle, IPPROTO_IPV6, IPV6_V6ONLY, 1);
		if (error != 0) {
			(void)closesocket(made->handle);
			free(made);
			return error;
		}
	}

	/* No WSAECONNRESET from an ICMP port unreachable on a UDP socket that is not connected. */
	if (kind == KL_SOCKET_DATAGRAM) {
		behavior = FALSE;
		returned = 0;
		status = WSAIoctl(made->handle, SIO_UDP_CONNRESET, &behavior, sizeof(behavior), NULL, 0, &returned, NULL, NULL);
		if (status != 0) {
			wsa = WSAGetLastError();
			(void)closesocket(made->handle);
			free(made);
			error = socket_error(wsa);
			return error;
		}
	}

	/* Succeeded: the socket is open. */
	*socket_made = made;
	return 0;
}

/*
 * Makes a socket block (on 0) or not (on 1).
 *
 * ioctlsocket(FIONBIO), and the record keeps the mode.
 */
int
kl_socket_set_nonblocking(
	struct kl_socket *socket,
	int on)
{
	int nonblocking;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The mode, 1 or 0. */
	nonblocking = 0;
	if (on)
		nonblocking = 1;
	error = socket_set_blocking(socket->handle, nonblocking);
	if (error != 0)
		return error;

	/* The record keeps what the system cannot tell. */
	socket->nonblocking = nonblocking;

	/* Succeeded: the socket blocks as asked. */
	return 0;
}

/*
 * Connects a socket to an address.
 *
 * On a socket that does not block WSAEWOULDBLOCK is EINPROGRESS, and a
 * second connect while one is under way (WSAEALREADY, or the WSAEINVAL that
 * older WinSock gives then) is EALREADY.  A blocking connect waits itself
 * (there is no EINTR on Windows).
 */
int
kl_socket_connect(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	struct sockaddr_storage system;
	int length;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's address of the socket's family. */
	if (address->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(address, &system, &length);
	if (error != 0)
		return error;

	/* The connection; a socket that blocks returns when it ended. */
	status = connect(socket->handle, (const struct sockaddr *)&system, length);
	if (status != 0) {
		wsa = WSAGetLastError();

		/* A connection that does not block is under way. */
		if (wsa == WSAEWOULDBLOCK) {
			socket->connecting = 1;
			return EINPROGRESS;
		}

		/* A connect while one is under way. */
		if (wsa == WSAEALREADY)
			return EALREADY;
		if (wsa == WSAEINVAL && socket->nonblocking && socket->connecting)
			return EALREADY;

		/* Why it failed (WSAEISCONN is EISCONN, as on POSIX). */
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: connected, and nothing is under way. */
	socket->connecting = 0;
	return 0;
}

/*
 * Tells how a connect that did not block ended.
 *
 * getsockopt(SO_ERROR), the value turned by socket_error.
 */
int
kl_socket_connect_result(
	struct kl_socket *socket)
{
	int pending;
	int length;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The socket's pending error: 0 when it connected. */
	pending = 0;
	length = (int)sizeof(pending);
	status = getsockopt(socket->handle, SOL_SOCKET, SO_ERROR, (char *)&pending, &length);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* The connection has ended. */
	socket->connecting = 0;

	/* The connection ended well, or with the system's error. */
	if (pending != 0) {
		error = socket_error(pending);
		return error;
	}

	/* Succeeded: connected. */
	return 0;
}

/*
 * Binds a socket to a local address.
 *
 * SO_EXCLUSIVEADDRUSE is not set (KL_SOCKET_REUSE_ADDRESS, below).
 */
int
kl_socket_bind(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	struct sockaddr_storage system;
	int length;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's address of the socket's family. */
	if (address->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(address, &system, &length);
	if (error != 0)
		return error;

	/* The binding. */
	status = bind(socket->handle, (const struct sockaddr *)&system, length);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: the socket has its address. */
	return 0;
}

/*
 * Listens for connections (backlog 0: the system's, SOMAXCONN).
 */
int
kl_socket_listen(
	struct kl_socket *socket,
	unsigned backlog)
{
	int queue;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The queue of connections not taken yet: the program's, or the system's. */
	queue = SOMAXCONN;
	if (backlog != 0U && backlog < (unsigned)INT_MAX)
		queue = (int)backlog;
	status = listen(socket->handle, queue);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: connections are taken in. */
	return 0;
}

/*
 * Takes a connection a listening socket holds, as a new socket blocking or
 * not as flags says.
 *
 * The new socket inherits the listening one's non-blocking mode on
 * Windows, so it is set to what flags says, and its handle is kept from
 * children.  A connection reset or aborted before it was taken (WSAECONNRESET,
 * WSAECONNABORTED) is passed over and the next one taken; a socket that
 * does not block with none left gives EAGAIN.
 */
int
kl_socket_accept(
	struct kl_socket *socket,
	unsigned flags,
	struct kl_socket **accepted,
	struct kl_address *peer)
{
	struct sockaddr_storage system;
	struct kl_socket *made;
	SOCKET handle;
	int length;
	int nonblocking;
	int wsa;
	int error;

	/* Nothing taken yet. */
	*accepted = NULL;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* Refuses flags the library does not know. */
	if ((flags & ~KL_SOCKET_NONBLOCKING) != 0U)
		return EINVAL;

	/* The record. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->family = socket->family;
	made->kind = socket->kind;

	/* The connection; one that went away before it was taken is passed over for the next. */
	for (;;) {
		length = (int)sizeof(system);
		handle = accept(socket->handle, (struct sockaddr *)&system, &length);
		if (handle != INVALID_SOCKET)
			break;
		wsa = WSAGetLastError();
		if (wsa == WSAECONNRESET)
			continue;
		if (wsa == WSAECONNABORTED)
			continue;
		free(made);
		error = socket_error(wsa);
		return error;
	}

	/* The new socket owns the handle. */
	made->handle = handle;

	/* Blocking or not as flags says, whatever the listening socket does. */
	nonblocking = 0;
	if ((flags & KL_SOCKET_NONBLOCKING) != 0U)
		nonblocking = 1;
	error = socket_set_blocking(handle, nonblocking);
	if (error != 0) {
		(void)closesocket(handle);
		free(made);
		return error;
	}

	/* The record keeps the mode, which the system cannot tell. */
	made->nonblocking = nonblocking;

	/* The handle is kept from the programs the application starts. */
	error = socket_no_inherit(handle);
	if (error != 0) {
		(void)closesocket(handle);
		free(made);
		return error;
	}

	/* Where it came from. */
	if (peer != NULL) {
		error = socket_from_system((const struct sockaddr *)&system, length, peer);
		if (error != 0)
			memset(peer, 0, sizeof(*peer));
	}

	/* Succeeded: the connection is a socket. */
	*accepted = made;
	return 0;
}

/*
 * Sends bytes on a connected socket; *sent tells how many went.
 *
 * send takes an int: a size above INT_MAX sends INT_MAX, a short send as
 * the header allows.  WSAEWOULDBLOCK is EAGAIN, WSAESHUTDOWN EPIPE.
 */
int
kl_socket_send(
	struct kl_socket *socket,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	int chunk;
	int written;
	int wsa;
	int error;

	/* Nothing sent yet. */
	*sent = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The bytes, cut to what send takes. */
	if (size > (size_t)INT_MAX) {
		chunk = INT_MAX;
	} else {
		chunk = (int)size;
	}

	/* The send. */
	written = send(socket->handle, (const char *)bytes, chunk, 0);
	if (written == SOCKET_ERROR) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: so many went. */
	*sent = (size_t)written;
	return 0;
}

/*
 * Receives bytes from a connected socket; 0 bytes is the stream's end.
 *
 * recv takes an int: at most INT_MAX are received.  WSAEWOULDBLOCK is
 * EAGAIN.  A datagram socket's datagram longer than the buffer (WSAEMSGSIZE)
 * is cut to it, as on POSIX, and a stream whose receiving was shut down
 * (WSAESHUTDOWN) is at its end, as on POSIX.
 */
int
kl_socket_receive(
	struct kl_socket *socket,
	void *buffer,
	size_t size,
	size_t *received)
{
	int chunk;
	int count;
	int wsa;
	int error;

	/* Nothing received yet. */
	*received = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* What waits, or the end; a size above INT_MAX is cut. */
	if (size > (size_t)INT_MAX) {
		chunk = INT_MAX;
	} else {
		chunk = (int)size;
	}

	/* The receive. */
	count = recv(socket->handle, (char *)buffer, chunk, 0);
	if (count == SOCKET_ERROR) {
		wsa = WSAGetLastError();

		/* A datagram longer than the buffer: the buffer's worth came. */
		if (wsa == WSAEMSGSIZE) {
			*received = (size_t)chunk;
			return 0;
		}

		/* The receiving was shut down: the end, as on POSIX. */
		if (wsa == WSAESHUTDOWN)
			return 0;

		/* Why nothing came. */
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: so many came (0: the end). */
	*received = (size_t)count;
	return 0;
}

/*
 * Sends one datagram to an address.
 *
 * sendto takes an int; a datagram longer than INT_MAX is not cut and is
 * EINVAL.
 */
int
kl_socket_send_to(
	struct kl_socket *socket,
	const void *bytes,
	size_t size,
	const struct kl_address *to,
	size_t *sent)
{
	struct sockaddr_storage system;
	int length;
	int written;
	int wsa;
	int error;

	/* Nothing sent yet. */
	*sent = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's address of the socket's family. */
	if (to->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(to, &system, &length);
	if (error != 0)
		return error;

	/* A datagram is sent whole or not at all. */
	if (size > (size_t)INT_MAX)
		return EINVAL;

	/* The datagram. */
	written = sendto(socket->handle, (const char *)bytes, (int)size, 0, (const struct sockaddr *)&system, length);
	if (written == SOCKET_ERROR) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: the datagram went. */
	*sent = (size_t)written;
	return 0;
}

/*
 * Receives one datagram and where it came from.
 *
 * recvfrom; WSAEMSGSIZE (a datagram longer than the buffer, cut to it) is
 * not an error: *received is the buffer's size, as on POSIX.
 */
int
kl_socket_receive_from(
	struct kl_socket *socket,
	void *buffer,
	size_t size,
	struct kl_address *from,
	size_t *received)
{
	struct sockaddr_storage system;
	int length;
	int chunk;
	int count;
	int wsa;
	int error;

	/* Nothing received yet. */
	*received = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The datagram (cut to the buffer); a size above INT_MAX is cut. */
	if (size > (size_t)INT_MAX) {
		chunk = INT_MAX;
	} else {
		chunk = (int)size;
	}

	/* The room for the sender's address, and the receive. */
	length = (int)sizeof(system);
	memset(&system, 0, sizeof(system));
	count = recvfrom(socket->handle, (char *)buffer, chunk, 0, (struct sockaddr *)&system, &length);
	if (count == SOCKET_ERROR) {
		wsa = WSAGetLastError();

		/* A datagram longer than the buffer: the buffer's worth came. */
		if (wsa != WSAEMSGSIZE) {
			error = socket_error(wsa);
			return error;
		}

		/* The buffer's worth came. */
		count = chunk;
	}

	/* Where it came from. */
	if (from != NULL) {
		error = socket_from_system((const struct sockaddr *)&system, length, from);
		if (error != 0)
			memset(from, 0, sizeof(*from));
	}

	/* Succeeded: so many bytes of it. */
	*received = (size_t)count;
	return 0;
}

/*
 * Closes a connection's receiving or sending or both.
 *
 * shutdown with SD_RECEIVE, SD_SEND or SD_BOTH.
 */
int
kl_socket_shutdown(
	struct kl_socket *socket,
	unsigned how)
{
	int system_how;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's name of what closes. */
	switch (how) {
	case KL_SOCKET_SHUT_RECEIVE:
		system_how = SD_RECEIVE;
		break;
	case KL_SOCKET_SHUT_SEND:
		system_how = SD_SEND;
		break;
	case KL_SOCKET_SHUT_BOTH:
		system_how = SD_BOTH;
		break;
	default:
		return EINVAL;
	}

	/* The shutdown. */
	status = shutdown(socket->handle, system_how);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: that side is closed. */
	return 0;
}

/*
 * Reads the local address a socket is bound to (getsockname).
 *
 * A socket not bound yet (getsockname's WSAEINVAL) gives the unspecified
 * address of its family and port 0, as on POSIX.
 */
int
kl_socket_local_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	struct sockaddr_storage system;
	int length;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's address. */
	length = (int)sizeof(system);
	status = getsockname(socket->handle, (struct sockaddr *)&system, &length);
	if (status != 0) {
		wsa = WSAGetLastError();

		/* A socket not bound yet has the unspecified address and port 0, as on POSIX. */
		if (wsa == WSAEINVAL) {
			kl_address_any(socket->family, 0U, address);
			return 0;
		}

		/* Why the address cannot be read. */
		error = socket_error(wsa);
		return error;
	}

	/* As the interface's. */
	error = socket_from_system((const struct sockaddr *)&system, length, address);
	if (error != 0)
		return error;

	/* Succeeded: the local address. */
	return 0;
}

/*
 * Reads the address a socket is connected to (getpeername).
 */
int
kl_socket_peer_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	struct sockaddr_storage system;
	int length;
	int status;
	int wsa;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The system's address of the peer. */
	length = (int)sizeof(system);
	status = getpeername(socket->handle, (struct sockaddr *)&system, &length);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* As the interface's. */
	error = socket_from_system((const struct sockaddr *)&system, length, address);
	if (error != 0)
		return error;

	/* Succeeded: the peer's address. */
	return 0;
}

/*
 * Sets one of a socket's options on (1) or off (0).
 *
 * KL_SOCKET_REUSE_ADDRESS does nothing and succeeds: Windows' SO_REUSEADDR
 * lets a socket take a port another one uses, which is not the POSIX
 * meaning, and a port left in TIME_WAIT is bound by default there.  The
 * others are TCP_NODELAY, SO_KEEPALIVE, IPV6_V6ONLY and SO_BROADCAST, with
 * the same ENOPROTOOPT and EINVAL as on POSIX for the wrong kind of socket.
 */
int
kl_socket_set_option(
	struct kl_socket *socket,
	unsigned option,
	int value)
{
	int level;
	int name;
	int error;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* On or off. */
	if (value != 0 && value != 1)
		return EINVAL;

	/* The system's level and name of the option, for the sockets it means something to. */
	switch (option) {
	case KL_SOCKET_REUSE_ADDRESS:
		/* Nothing to set: binding after a close already works. */
		return 0;
	case KL_SOCKET_NO_DELAY:
		if (socket->kind != KL_SOCKET_STREAM)
			return ENOPROTOOPT;
		level = IPPROTO_TCP;
		name = TCP_NODELAY;
		break;
	case KL_SOCKET_KEEP_ALIVE:
		if (socket->kind != KL_SOCKET_STREAM)
			return ENOPROTOOPT;
		level = SOL_SOCKET;
		name = SO_KEEPALIVE;
		break;
	case KL_SOCKET_IPV6_ONLY:
		if (socket->family != KL_ADDRESS_IPV6)
			return ENOPROTOOPT;
		level = IPPROTO_IPV6;
		name = IPV6_V6ONLY;
		break;
	case KL_SOCKET_BROADCAST:
		if (socket->kind != KL_SOCKET_DATAGRAM)
			return ENOPROTOOPT;
		level = SOL_SOCKET;
		name = SO_BROADCAST;
		break;
	default:
		return EINVAL;
	}

	/* The option. */
	error = socket_set_int(socket->handle, level, name, value);
	if (error != 0)
		return error;

	/* Succeeded: the option is set. */
	return 0;
}

/*
 * Gives the system's handle of a socket (a SOCKET, which fits intptr_t).
 */
intptr_t
kl_socket_native(
	const struct kl_socket *socket)
{
	/* The SOCKET. */
	return (intptr_t)socket->handle;
}

/*
 * Closes a socket and frees its record.
 *
 * NULL does nothing, as on POSIX (a connection that never opened is closed
 * with NULL).
 */
void
kl_socket_close(
	struct kl_socket *socket)
{
	/* Nothing to close. */
	if (socket == NULL)
		return;

	/* The handle, and the record. */
	(void)closesocket(socket->handle);
	free(socket);
}

/*
 * Waits for several sockets and wakers, until one is ready or for so many
 * milliseconds.
 *
 * select waits: its fd_set is a count and an array of SOCKETs, so the three
 * sets are made for count entries each (offsetof(fd_set, fd_array) plus
 * count SOCKETs) and passed as fd_set pointers, unbound by FD_SETSIZE (64).
 * A socket's READABLE goes in the read set, WRITABLE in the write set, and
 * every socket in the exception set (a connect that failed shows there:
 * FAILED).  A waker is ends[0] in the read set.  The time is a timeval
 * (NULL for KL_SOCKET_FOREVER).  select may end on the system's tick, a
 * little before the time, so the time is measured with the performance
 * counter and a select that found nothing early waits again for the rest
 * (as kl_thread_sleep does).  With no waits it only sleeps
 * (kl_thread_sleep), since select with no socket at all is WSAEINVAL.  A
 * waker found readable is read empty and then made quiet (struct
 * kl_socket_waker).
 */
int
kl_socket_poll(
	struct kl_socket_wait *waits,
	size_t count,
	int milliseconds,
	size_t *ready)
{
	struct timeval limit;
	struct timeval *limit_pointer;
	ULONGLONG rate;
	ULONGLONG start;
	ULONGLONG wanted;
	unsigned char *room;
	fd_set *readable;
	fd_set *writable;
	fd_set *failed;
	size_t header_bytes;
	size_t set_bytes;
	size_t index;
	unsigned found;
	int left;
	int status;
	int wsa;
	int error;

	/* Nothing ready yet. */
	*ready = 0;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The arguments, as on POSIX. */
	error = socket_poll_check(waits, count, milliseconds);
	if (error != 0)
		return error;

	/* No waits: only the time passes, all of it (select with no socket is WSAEINVAL). */
	if (count == 0U) {
		if (milliseconds > 0)
			kl_thread_sleep((uint32_t)milliseconds);
		return 0;
	}

	/* Room for the three sets, each of count entries. */
	header_bytes = offsetof(fd_set, fd_array);
	if (count > (SIZE_MAX / 3U - header_bytes) / sizeof(SOCKET))
		return ENOMEM;
	set_bytes = header_bytes + count * sizeof(SOCKET);
	room = malloc(set_bytes * 3U);
	if (room == NULL)
		return ENOMEM;
	readable = (fd_set *)(void *)room;
	writable = (fd_set *)(void *)(room + set_bytes);
	failed = (fd_set *)(void *)(room + set_bytes * 2U);

	/* Where the counter stands, and the counter's ticks of the whole time (rounded up; seconds apart so that it cannot overflow). */
	socket_clock(&rate, &start);
	wanted = 0;
	if (milliseconds > 0) {
		wanted = ((ULONGLONG)milliseconds / 1000U) * rate;
		wanted += (((ULONGLONG)milliseconds % 1000U) * rate + 999U) / 1000U;
	}

	/* Waits until something is ready or the counter has run the whole time. */
	left = milliseconds;
	for (;;) {
		/* The sockets and wakers in the sets (select leaves only the ready ones in them). */
		socket_poll_fill(waits, count, readable, writable, failed);

		/* The time: none (forever), or what is left, in seconds and microseconds. */
		limit_pointer = NULL;
		if (milliseconds != KL_SOCKET_FOREVER) {
			limit.tv_sec = left / SOCKET_MS_SECOND;
			limit.tv_usec = (left % SOCKET_MS_SECOND) * SOCKET_US_MILLISECOND;
			limit_pointer = &limit;
		}

		/* The wait (its first argument is ignored on Windows). */
		status = select(0, readable, writable, failed, limit_pointer);
		if (status == SOCKET_ERROR) {
			wsa = WSAGetLastError();
			free(room);
			error = socket_error(wsa);
			return error;
		}

		/* Something is ready, or there is no time to run out. */
		if (status > 0 || milliseconds == KL_SOCKET_FOREVER)
			break;

		/* Nothing: the time ran out, or select ended early and the rest is waited for. */
		left = socket_poll_left(rate, start, wanted);
		if (left == 0)
			break;
	}

	/* What each wait found; none when the time ran out. */
	for (index = 0; index < count && status > 0; index++) {
		found = socket_poll_found(&waits[index], readable, writable, failed);
		waits[index].ready = found;
		if (found != 0U)
			(*ready)++;
	}

	/* The sets go. */
	free(room);

	/* Succeeded: *ready waits are ready (none: the time ran out). */
	return 0;
}

/*
 * Opens a waker.
 *
 * A pair of connected loopback TCP sockets (socket_waker_pair), quiet.
 * WSAEMFILE is EMFILE, WSAENOBUFS ENOMEM.
 */
int
kl_socket_waker_open(
	struct kl_socket_waker **waker)
{
	struct kl_socket_waker *made;
	int error;

	/* Nothing open yet. */
	*waker = NULL;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return error;

	/* The record, quiet. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->ends[0] = INVALID_SOCKET;
	made->ends[1] = INVALID_SOCKET;
	made->rung = 0;

	/* The connected pair, whose ends neither block nor pass to started programs. */
	error = socket_waker_pair(made->ends);
	if (error != 0) {
		free(made);
		return error;
	}

	/* Succeeded: the waker is quiet. */
	*waker = made;
	return 0;
}

/*
 * Rings a waker.
 *
 * Only the ring that moves rung from 0 to 1 sends the byte a wait finds;
 * the others merge with it.  A full buffer (WSAEWOULDBLOCK) is a ring
 * already waiting: nothing more.
 */
void
kl_socket_waker_ring(
	struct kl_socket_waker *waker)
{
	unsigned char byte;
	LONG was;
	int error;
	int written;

	/* WinSock is started before the first call into it. */
	error = socket_start();
	if (error != 0)
		return;

	/* The first ring since the last wait sends the byte the wait finds. */
	was = InterlockedCompareExchange(&waker->rung, 1, 0);
	if (was != 0)
		return;

	/* The byte; a failure to send leaves nothing a retry could mend. */
	byte = 1U;
	written = send(waker->ends[1], (const char *)&byte, 1, 0);
	(void)written;
}

/*
 * Closes a waker: both ends, and the record.  NULL does nothing.
 */
void
kl_socket_waker_close(
	struct kl_socket_waker *waker)
{
	/* Nothing to close. */
	if (waker == NULL)
		return;

	/* Its sockets, and the record. */
	(void)closesocket(waker->ends[0]);
	(void)closesocket(waker->ends[1]);
	free(waker);
}

/* Starts WinSock once (kl_once); 0, or the error its start gave. */
static int
socket_start(void)
{
	/* The start, the first time. */
	kl_once(&socket_started, socket_start_once);

	/* What the first start gave. */
	return socket_start_error;
}

/* Starts WinSock 2.2 and keeps the outcome in socket_start_error. */
static void
socket_start_once(void)
{
	WSADATA data;
	BYTE major;
	BYTE minor;
	int status;

	/* WinSock 2.2. */
	status = WSAStartup(MAKEWORD(2, 2), &data);
	if (status != 0) {
		if (status == WSA_NOT_ENOUGH_MEMORY) {
			socket_start_error = ENOMEM;
		} else {
			socket_start_error = EIO;
		}

		/* The start failed. */
		return;
	}

	/* A WinSock older than the one asked for cannot be used. */
	major = LOBYTE(data.wVersion);
	minor = HIBYTE(data.wVersion);
	if (major != 2 || minor != 2) {
		(void)WSACleanup();
		socket_start_error = EIO;
		return;
	}

	/* Succeeded: WinSock is started. */
	socket_start_error = 0;
}

/*
 * Turns a WinSock or Win32 error into its POSIX name: WSAEWOULDBLOCK
 * EAGAIN (never EWOULDBLOCK: the two differ on Windows), WSAEINPROGRESS
 * EALREADY (a WinSock 1.1 blocking call under way, not a connect that did
 * not block: kl_socket_connect turns WSAEWOULDBLOCK into EINPROGRESS
 * itself), WSAESHUTDOWN EPIPE, WSAENETRESET ECONNRESET, WSAEHOSTDOWN
 * EHOSTUNREACH (mingw's errno.h has no EHOSTDOWN), and the rest by name;
 * anything else EIO.
 */
static int
socket_error(
	int error)
{
	/* The error by name. */
	switch (error) {
	case WSAEWOULDBLOCK:
		return EAGAIN;
	case WSAEINPROGRESS:
	case WSAEALREADY:
		return EALREADY;
	case WSAECONNREFUSED:
		return ECONNREFUSED;
	case WSAECONNRESET:
	case WSAENETRESET:
		return ECONNRESET;
	case WSAETIMEDOUT:
		return ETIMEDOUT;
	case WSAENETUNREACH:
		return ENETUNREACH;
	case WSAEHOSTUNREACH:
	case WSAEHOSTDOWN:
		return EHOSTUNREACH;
	case WSAEADDRINUSE:
		return EADDRINUSE;
	case WSAEADDRNOTAVAIL:
		return EADDRNOTAVAIL;
	case WSAEAFNOSUPPORT:
		return EAFNOSUPPORT;
	case WSAEMFILE:
		return EMFILE;
	case WSAENOTCONN:
		return ENOTCONN;
	case WSAESHUTDOWN:
		return EPIPE;
	case WSAENOBUFS:
	case WSA_NOT_ENOUGH_MEMORY:
		return ENOMEM;
	case WSAECONNABORTED:
		return ECONNABORTED;
	case WSAENETDOWN:
		return ENETDOWN;
	case WSAEACCES:
		return EACCES;
	case WSAEINVAL:
		return EINVAL;
	case WSAEISCONN:
		return EISCONN;
	case WSAENOPROTOOPT:
		return ENOPROTOOPT;
	case WSAEMSGSIZE:
		return EMSGSIZE;
	case WSAENOTSOCK:
		return ENOTSOCK;
	case WSAEOPNOTSUPP:
		return EOPNOTSUPP;
	default:
		return EIO;
	}
}

/* Turns an address into the system's (sockaddr_in, sockaddr_in6 with sin6_scope_id); EAFNOSUPPORT. */
static int
socket_to_system(
	const struct kl_address *address,
	struct sockaddr_storage *system,
	int *length)
{
	struct sockaddr_in *ipv4;
	struct sockaddr_in6 *ipv6;

	/* The family's own layout. */
	memset(system, 0, sizeof(*system));
	if (address->family == KL_ADDRESS_IPV4) {
		ipv4 = (struct sockaddr_in *)system;
		ipv4->sin_family = AF_INET;
		ipv4->sin_port = htons(address->port);
		memcpy(&ipv4->sin_addr, address->bytes, sizeof(ipv4->sin_addr));
		*length = (int)sizeof(*ipv4);
		return 0;
	}

	/* Only IPv6 besides. */
	if (address->family != KL_ADDRESS_IPV6)
		return EAFNOSUPPORT;

	/* The IPv6 layout. */
	ipv6 = (struct sockaddr_in6 *)system;
	ipv6->sin6_family = AF_INET6;
	ipv6->sin6_port = htons(address->port);
	memcpy(&ipv6->sin6_addr, address->bytes, sizeof(ipv6->sin6_addr));
	ipv6->sin6_scope_id = address->scope;
	*length = (int)sizeof(*ipv6);

	/* Succeeded: the IPv6 address. */
	return 0;
}

/* Turns the system's address into the interface's; EAFNOSUPPORT for another family. */
static int
socket_from_system(
	const struct sockaddr *system,
	int length,
	struct kl_address *address)
{
	const struct sockaddr_in *ipv4;
	const struct sockaddr_in6 *ipv6;

	/* Nothing read yet. */
	memset(address, 0, sizeof(*address));

	/* An IPv4 address. */
	if (system->sa_family == AF_INET && length >= (int)sizeof(*ipv4)) {
		ipv4 = (const struct sockaddr_in *)(const void *)system;
		address->family = KL_ADDRESS_IPV4;
		address->port = ntohs(ipv4->sin_port);
		memcpy(address->bytes, &ipv4->sin_addr, sizeof(ipv4->sin_addr));
		return 0;
	}

	/* Only IPv6 besides. */
	if (system->sa_family != AF_INET6 || length < (int)sizeof(*ipv6))
		return EAFNOSUPPORT;

	/* The IPv6 layout. */
	ipv6 = (const struct sockaddr_in6 *)(const void *)system;
	address->family = KL_ADDRESS_IPV6;
	address->port = ntohs(ipv6->sin6_port);
	memcpy(address->bytes, &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
	address->scope = ipv6->sin6_scope_id;

	/* Succeeded: the IPv6 address. */
	return 0;
}

/* Gives the system's domain of a family. */
static int
socket_domain(
	unsigned family,
	int *domain)
{
	/* IPv4 or IPv6. */
	switch (family) {
	case KL_ADDRESS_IPV4:
		*domain = AF_INET;
		return 0;
	case KL_ADDRESS_IPV6:
		*domain = AF_INET6;
		return 0;
	default:
		return EAFNOSUPPORT;
	}
}

/* Gives the system's type of a kind of socket. */
static int
socket_type(
	unsigned kind,
	int *type)
{
	/* A stream or datagrams. */
	switch (kind) {
	case KL_SOCKET_STREAM:
		*type = SOCK_STREAM;
		return 0;
	case KL_SOCKET_DATAGRAM:
		*type = SOCK_DGRAM;
		return 0;
	default:
		return EINVAL;
	}
}

/*
 * Turns GetAddrInfoW's answer into an errno value: WSAHOST_NOT_FOUND and
 * WSANO_DATA ENOENT, WSATRY_AGAIN EAGAIN, WSAEAFNOSUPPORT EAFNOSUPPORT,
 * WSA_NOT_ENOUGH_MEMORY ENOMEM, others EIO.
 */
static int
socket_resolve_error(
	int answer)
{
	/* The host has no address. */
	if (answer == WSAHOST_NOT_FOUND)
		return ENOENT;
	if (answer == WSANO_DATA)
		return ENOENT;

	/* The answer failed for now, or for a reason of its own. */
	if (answer == WSATRY_AGAIN)
		return EAGAIN;
	if (answer == WSAEAFNOSUPPORT)
		return EAFNOSUPPORT;
	if (answer == WSA_NOT_ENOUGH_MEMORY)
		return ENOMEM;

	/* Anything else. */
	return EIO;
}

/* Sets a handle non-blocking (1) or blocking (0) with ioctlsocket(FIONBIO); 0 or the error. */
static int
socket_set_blocking(
	SOCKET handle,
	int nonblocking)
{
	u_long mode;
	int status;
	int wsa;
	int error;

	/* The mode: any non-zero value is non-blocking. */
	mode = 0;
	if (nonblocking)
		mode = 1;
	status = ioctlsocket(handle, FIONBIO, &mode);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: the handle blocks as asked. */
	return 0;
}

/* Makes an overlapped socket that child processes do not inherit; 0 or the error. */
static int
socket_make(
	int domain,
	int type,
	SOCKET *handle)
{
	SOCKET made;
	int wsa;
	int error;

	/* The socket: overlapped (a plain one is a synchronous file object), and not inherited. */
	made = WSASocketW(domain, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (made == INVALID_SOCKET) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: the socket is made. */
	*handle = made;
	return 0;
}

/* Keeps a handle from child processes (a socket accept made inherits the listening one's mode); 0 or the error. */
static int
socket_no_inherit(
	SOCKET handle)
{
	BOOL done;

	/* The inherit flag off. */
	done = SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0);
	if (!done)
		return EIO;

	/* Succeeded: children do not get the handle. */
	return 0;
}

/* Sets an integer option of a socket (a DWORD, as WinSock takes it); 0 or the error. */
static int
socket_set_int(
	SOCKET handle,
	int level,
	int name,
	int value)
{
	DWORD option;
	int status;
	int wsa;
	int error;

	/* The option. */
	option = (DWORD)value;
	status = setsockopt(handle, level, name, (const char *)&option, (int)sizeof(option));
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		return error;
	}

	/* Succeeded: the option is set. */
	return 0;
}

/* Checks kl_socket_poll's arguments as the POSIX layer does; EINVAL, or 0 with every ready cleared. */
static int
socket_poll_check(
	struct kl_socket_wait *waits,
	size_t count,
	int milliseconds)
{
	struct kl_socket_wait *wait;
	size_t index;

	/* The time: not below forever, and an end when there is nothing to wait for. */
	if (milliseconds < KL_SOCKET_FOREVER)
		return EINVAL;
	if (count == 0U && milliseconds == KL_SOCKET_FOREVER)
		return EINVAL;
	if (count > (size_t)INT_MAX)
		return EINVAL;

	/* Each wait names a socket or a waker, and asks for what a wait can find. */
	for (index = 0; index < count; index++) {
		wait = &waits[index];
		wait->ready = 0;
		if (wait->socket == NULL && wait->waker == NULL)
			return EINVAL;
		if (wait->socket != NULL && wait->waker != NULL)
			return EINVAL;
		if ((wait->events & ~(KL_SOCKET_READABLE | KL_SOCKET_WRITABLE)) != 0U)
			return EINVAL;
	}

	/* Succeeded: the waits are well formed. */
	return 0;
}

/*
 * The performance counter's rate (ticks a second; never 0 on Windows XP and
 * later, 1000 if it were) and where it stands.
 */
static void
socket_clock(
	ULONGLONG *rate,
	ULONGLONG *now)
{
	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	/* The rate, and the counter now. */
	(void)QueryPerformanceFrequency(&frequency);
	(void)QueryPerformanceCounter(&counter);
	*rate = (ULONGLONG)frequency.QuadPart;
	if (*rate == 0U)
		*rate = 1000U;
	*now = (ULONGLONG)counter.QuadPart;
}

/*
 * The milliseconds left of a wait of wanted counter ticks from start,
 * rounded up so that the last wait does not end early; 0 once the counter
 * has run them all.
 */
static int
socket_poll_left(
	ULONGLONG rate,
	ULONGLONG start,
	ULONGLONG wanted)
{
	ULONGLONG now;
	ULONGLONG same_rate;
	ULONGLONG elapsed;
	ULONGLONG left;
	ULONGLONG milliseconds;

	/* How far the counter ran (its rate is the same as at the start). */
	socket_clock(&same_rate, &now);
	elapsed = now - start;
	if (elapsed >= wanted)
		return 0;

	/* The rest in whole milliseconds, rounded up; never more than the whole wait, which was an int. */
	left = wanted - elapsed;
	milliseconds = (left / rate) * 1000U;
	milliseconds += ((left % rate) * 1000U + rate - 1U) / rate;
	if (milliseconds > (ULONGLONG)INT_MAX)
		milliseconds = (ULONGLONG)INT_MAX;
	return (int)milliseconds;
}

/* Puts each wait's sockets in the three sets (each has room for count entries). */
static void
socket_poll_fill(
	const struct kl_socket_wait *waits,
	size_t count,
	fd_set *readable,
	fd_set *writable,
	fd_set *failed)
{
	const struct kl_socket_wait *wait;
	size_t index;

	/* The sets are empty. */
	readable->fd_count = 0;
	writable->fd_count = 0;
	failed->fd_count = 0;

	/* A waker's socket for its byte; a socket for what it asks, and for its failure. */
	for (index = 0; index < count; index++) {
		wait = &waits[index];
		if (wait->waker != NULL) {
			readable->fd_array[readable->fd_count] = wait->waker->ends[0];
			readable->fd_count++;
			continue;
		}

		/* A socket in the sets of what it asks for. */
		if ((wait->events & KL_SOCKET_READABLE) != 0U) {
			readable->fd_array[readable->fd_count] = wait->socket->handle;
			readable->fd_count++;
		}

		/* And in the set of what it can send. */
		if ((wait->events & KL_SOCKET_WRITABLE) != 0U) {
			writable->fd_array[writable->fd_count] = wait->socket->handle;
			writable->fd_count++;
		}

		/* Every socket in the set of failures. */
		failed->fd_array[failed->fd_count] = wait->socket->handle;
		failed->fd_count++;
	}
}

/* Tells whether a set holds a socket. */
static int
socket_in_set(
	const fd_set *set,
	SOCKET handle)
{
	u_int index;

	/* Each entry of the set. */
	for (index = 0; index < set->fd_count; index++) {
		if (set->fd_array[index] == handle)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/* Reads what select found for a wait: its ready bits (a waker found readable is emptied and made quiet). */
static unsigned
socket_poll_found(
	const struct kl_socket_wait *wait,
	const fd_set *readable,
	const fd_set *writable,
	const fd_set *failed)
{
	unsigned found;
	int member;

	/* A waker's byte. */
	found = 0;
	if (wait->waker != NULL) {
		member = socket_in_set(readable, wait->waker->ends[0]);
		if (member) {
			socket_waker_drain(wait->waker);
			found = KL_SOCKET_READABLE;
		}

		/* The waker's bits. */
		return found;
	}

	/* A socket: bytes or a connection to take, room to send or a connect that ended, and a failure. */
	member = socket_in_set(readable, wait->socket->handle);
	if (member)
		found |= KL_SOCKET_READABLE;
	member = socket_in_set(writable, wait->socket->handle);
	if (member)
		found |= KL_SOCKET_WRITABLE;
	member = socket_in_set(failed, wait->socket->handle);
	if (member)
		found |= KL_SOCKET_FAILED;

	/* What the wait found. */
	return found;
}

/* Empties a waker's socket found readable, so that it is quiet until the next ring. */
static void
socket_waker_drain(
	struct kl_socket_waker *waker)
{
	char bytes[SOCKET_DRAIN];
	int drained;

	/* Every byte waiting (there is one, but a full read costs nothing) until the socket would block. */
	for (;;) {
		drained = recv(waker->ends[0], bytes, (int)sizeof(bytes), 0);
		if (drained > 0)
			continue;
		break;
	}

	/* Quiet: the next ring sends a byte again (after the drain, never before). */
	(void)InterlockedExchange(&waker->rung, 0);
}

/*
 * Makes a connected pair of loopback TCP sockets for a waker.
 *
 * A listening socket on 127.0.0.1 port 0 with SO_EXCLUSIVEADDRUSE (no other
 * program can take the port), its port read back with getsockname, a
 * socket connected to it (blocking) and the connection accepted; the
 * accepted one's peer must be the connecting socket's own address, else it
 * is another program's and is closed and the next taken.  The listening
 * socket is then closed, both ends made non-blocking and the writing end
 * given TCP_NODELAY.  ends[0] is the accepted one, ends[1] the connecting
 * one.
 */
static int
socket_waker_pair(
	SOCKET ends[2])
{
	struct sockaddr_in listen_address;
	struct sockaddr_in client_address;
	struct sockaddr_in peer_address;
	SOCKET listener;
	SOCKET client;
	SOCKET accepted;
	int length;
	int status;
	int matched;
	int wsa;
	int error;

	/* Nothing made yet. */
	listener = INVALID_SOCKET;
	client = INVALID_SOCKET;
	accepted = INVALID_SOCKET;

	/* The listening socket, which only this program can bind to the port. */
	error = socket_make(AF_INET, SOCK_STREAM, &listener);
	if (error != 0)
		goto done;
	error = socket_set_int(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
	if (error != 0)
		goto done;

	/* The loopback address, any port. */
	memset(&listen_address, 0, sizeof(listen_address));
	listen_address.sin_family = AF_INET;
	listen_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	listen_address.sin_port = 0;
	status = bind(listener, (const struct sockaddr *)&listen_address, (int)sizeof(listen_address));
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		goto done;
	}

	/* The listening. */
	status = listen(listener, SOCKET_WAKER_BACKLOG);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		goto done;
	}

	/* The port the system chose. */
	length = (int)sizeof(listen_address);
	status = getsockname(listener, (struct sockaddr *)&listen_address, &length);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		goto done;
	}

	/* The connecting socket, connected (it blocks, and the connection waits in the queue). */
	error = socket_make(AF_INET, SOCK_STREAM, &client);
	if (error != 0)
		goto done;
	status = connect(client, (const struct sockaddr *)&listen_address, (int)sizeof(listen_address));
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		goto done;
	}

	/* The connecting socket's own address, which the accepted peer must match. */
	length = (int)sizeof(client_address);
	status = getsockname(client, (struct sockaddr *)&client_address, &length);
	if (status != 0) {
		wsa = WSAGetLastError();
		error = socket_error(wsa);
		goto done;
	}

	/* Our own connection: one from another program is closed and the next taken. */
	for (;;) {
		length = (int)sizeof(peer_address);
		accepted = accept(listener, (struct sockaddr *)&peer_address, &length);
		if (accepted == INVALID_SOCKET) {
			wsa = WSAGetLastError();
			error = socket_error(wsa);
			goto done;
		}

		/* Whether the peer is the connecting socket itself. */
		matched = 0;
		if (peer_address.sin_addr.s_addr == client_address.sin_addr.s_addr &&
		    peer_address.sin_port == client_address.sin_port)
			matched = 1;
		if (matched)
			break;
		(void)closesocket(accepted);
		accepted = INVALID_SOCKET;
	}

	/* The accepted handle is kept from the programs the application starts. */
	error = socket_no_inherit(accepted);
	if (error != 0)
		goto done;

	/* Neither end blocks, and the writing end sends a small write at once. */
	error = socket_set_blocking(accepted, 1);
	if (error != 0)
		goto done;
	error = socket_set_blocking(client, 1);
	if (error != 0)
		goto done;
	error = socket_set_int(client, IPPROTO_TCP, TCP_NODELAY, 1);
	if (error != 0)
		goto done;

	/* Succeeded: the pair is made. */
	ends[0] = accepted;
	ends[1] = client;
	accepted = INVALID_SOCKET;
	client = INVALID_SOCKET;
	error = 0;

done:
	/* The listening socket is no longer needed, and the ends of a pair that failed are closed. */
	if (listener != INVALID_SOCKET)
		(void)closesocket(listener);
	if (accepted != INVALID_SOCKET)
		(void)closesocket(accepted);
	if (client != INVALID_SOCKET)
		(void)closesocket(client);

	/* Reports why the pair could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the pair is made. */
	return 0;
}
