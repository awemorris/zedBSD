/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program's network on zedBSD, Linux and FreeBSD (<keiland/keiland_socket.h>):
 * BSD sockets, poll for the wait, and a pipe for each waker.
 */

#include <keiland/keiland_socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The waits kept on the stack; a longer list of waits is allocated. */
#define SOCKET_WAITS_STACK	32U

/* The bytes a waker's pipe is drained by at a time. */
#define SOCKET_DRAIN		64U

/* The longest address text kl_address_parse reads, scope and NUL included. */
#define SOCKET_TEXT_MAX		64U

/* The text of a port, with its NUL. */
#define SOCKET_PORT_TEXT	8U

/* Milliseconds in a second, and nanoseconds in a millisecond. */
#define SOCKET_MS_SECOND	1000L
#define SOCKET_NS_MILLISECOND	1000000L

/*
 * A socket: the system's descriptor, and the family and kind it was opened
 * with (the options and the addresses depend on them).
 */
struct kl_socket {
	int fd;
	unsigned family;
	unsigned kind;
};

/*
 * A waker: a pipe whose read end a wait polls, and whether a byte for the
 * rings since the last wait is in it.
 *
 * rung is 1 from the ring that wrote the byte until the wait that drained
 * it: rings meanwhile write nothing, so the pipe never fills.  The wait
 * drains the pipe before it sets rung to 0, so that a ring after that
 * writes a byte the next wait finds, and a ring before it is found by this
 * one.
 */
struct kl_socket_waker {
	int ends[2];
	atomic_int rung;
};

static int socket_to_system(const struct kl_address *address, struct sockaddr_storage *system, socklen_t *length);
static int socket_from_system(const struct sockaddr *system, socklen_t length, struct kl_address *address);
static int socket_domain(unsigned family, int *domain);
static int socket_type(unsigned kind, int *type);
static int socket_resolve_error(int answer);
static int socket_wait_connect(int fd);
static int socket_accept_passes(int error);
static void socket_waker_drain(struct kl_socket_waker *waker);
static int64_t socket_now_ms(void);

/*
 * Reads an address written in numbers, with a port.
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
			index = if_nametoindex(scope);
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
 * Makes the address of every interface of a family.
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
 * Makes the loopback address of a family.
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
 * Finds the addresses of a host for a kind of socket.
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
	struct addrinfo hints;
	struct addrinfo *found;
	struct addrinfo *entry;
	char service[SOCKET_PORT_TEXT];
	int answer;
	int error;

	/* Nothing found yet. */
	*count = 0;
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
	hints.ai_flags = AI_NUMERICSERV;

	/* The system's answer. */
	(void)snprintf(service, sizeof(service), "%u", (unsigned)port);
	answer = getaddrinfo(host, service, &hints, &found);
	if (answer != 0)
		return socket_resolve_error(answer);

	/* Each address of a family the interface knows, in the system's order, while there is room. */
	for (entry = found; entry != NULL && *count < capacity; entry = entry->ai_next) {
		error = socket_from_system(entry->ai_addr, entry->ai_addrlen, &addresses[*count]);
		if (error == 0)
			(*count)++;
	}

	/* The system's list goes. */
	freeaddrinfo(found);

	/* A host with no address of a known family. */
	if (*count == 0U)
		return ENOENT;

	/* Succeeded: *count addresses. */
	return 0;
}

/*
 * Opens a socket.
 */
int
kl_socket_open(
	unsigned family,
	unsigned kind,
	unsigned flags,
	struct kl_socket **socket_made)
{
	struct kl_socket *made;
	int domain;
	int type;
	int on;
	int status;
	int error;

	/* Nothing open yet. */
	*socket_made = NULL;

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

	/* The socket, kept from the programs the application starts, blocking or not. */
	type |= SOCK_CLOEXEC;
	if ((flags & KL_SOCKET_NONBLOCKING) != 0U)
		type |= SOCK_NONBLOCK;
	made->fd = socket(domain, type, 0);
	if (made->fd < 0) {
		error = errno;
		free(made);
		return error;
	}

	/* IPv6 alone on an IPv6 socket, whatever the system's default. */
	if (family == KL_ADDRESS_IPV6) {
		on = 1;
		status = setsockopt(made->fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
		if (status != 0) {
			error = errno;
			(void)close(made->fd);
			free(made);
			return error;
		}
	}

	/* No signal for a write after the peer closed, also for a TLS library's own writes (FreeBSD). */
#if defined(SO_NOSIGPIPE)
	on = 1;
	(void)setsockopt(made->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

	/* Succeeded: the socket is open. */
	*socket_made = made;
	return 0;
}

/*
 * Makes a socket block or not.
 */
int
kl_socket_set_nonblocking(
	struct kl_socket *socket,
	int on)
{
	int flags;
	int status;

	/* The descriptor's flags with O_NONBLOCK on or off. */
	flags = fcntl(socket->fd, F_GETFL);
	if (flags < 0)
		return errno;
	if (on) {
		flags |= O_NONBLOCK;
	} else {
		flags &= ~O_NONBLOCK;
	}

	/* The new flags set. */
	status = fcntl(socket->fd, F_SETFL, flags);
	if (status != 0)
		return errno;

	/* Succeeded: the socket blocks as asked. */
	return 0;
}

/*
 * Connects a socket to an address.
 */
int
kl_socket_connect(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	struct sockaddr_storage system;
	socklen_t length;
	int status;
	int error;

	/* The system's address of the socket's family. */
	if (address->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(address, &system, &length);
	if (error != 0)
		return error;

	/* The connection; one a signal cut short goes on, and a socket that blocks waits for its end. */
	status = connect(socket->fd, (const struct sockaddr *)&system, length);
	if (status == 0)
		return 0;

	/* A signal cut it short: the end is waited for. */
	error = errno;
	if (error == EINTR) {
		error = socket_wait_connect(socket->fd);
		if (error != 0)
			return error;
		return 0;
	}

	/* Reports EINPROGRESS for a socket that does not block, or why it failed. */
	if (error != 0)
		return error;

	/* Succeeded: connected. */
	return 0;
}

/*
 * Tells how a connection that did not block ended.
 */
int
kl_socket_connect_result(
	struct kl_socket *socket)
{
	socklen_t length;
	int status;
	int error;

	/* The socket's pending error: 0 when it connected. */
	error = 0;
	length = sizeof(error);
	status = getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &error, &length);
	if (status != 0)
		return errno;

	/* Reports why the connection failed. */
	if (error != 0)
		return error;

	/* Succeeded: the socket connected. */
	return 0;
}

/*
 * Binds a socket to a local address.
 */
int
kl_socket_bind(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	struct sockaddr_storage system;
	socklen_t length;
	int status;
	int error;

	/* The system's address of the socket's family. */
	if (address->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(address, &system, &length);
	if (error != 0)
		return error;

	/* The binding. */
	status = bind(socket->fd, (const struct sockaddr *)&system, length);
	if (status != 0)
		return errno;

	/* Succeeded: the socket has its address. */
	return 0;
}

/*
 * Listens for connections.
 */
int
kl_socket_listen(
	struct kl_socket *socket,
	unsigned backlog)
{
	int queue;
	int status;

	/* The queue of connections not taken yet: the program's, or the system's. */
	queue = SOMAXCONN;
	if (backlog != 0U && backlog < (unsigned)INT_MAX)
		queue = (int)backlog;
	status = listen(socket->fd, queue);
	if (status != 0)
		return errno;

	/* Succeeded: connections are taken in. */
	return 0;
}

/*
 * Takes a connection a listening socket holds.
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
	socklen_t length;
	int passed;
	int type;
	int fd;
	int error;

	/* Nothing taken yet. */
	*accepted = NULL;
	if ((flags & ~KL_SOCKET_NONBLOCKING) != 0U)
		return EINVAL;

	/* The record. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->family = socket->family;
	made->kind = socket->kind;

	/*
	 * The connection, kept from started programs and blocking as asked.  A
	 * signal does not end the wait, and a connection that went away before
	 * it was taken (or a network error Linux passes on to accept) is passed
	 * over for the next.
	 */
	type = SOCK_CLOEXEC;
	if ((flags & KL_SOCKET_NONBLOCKING) != 0U)
		type |= SOCK_NONBLOCK;
	for (;;) {
		length = sizeof(system);
		fd = accept4(socket->fd, (struct sockaddr *)&system, &length, type);
		if (fd >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;
		passed = socket_accept_passes(error);
		if (passed)
			continue;
		free(made);
		if (error == EWOULDBLOCK)
			return EAGAIN;
		return error;
	}

	/* The new socket owns the descriptor. */
	made->fd = fd;

	/* Where it came from. */
	if (peer != NULL) {
		error = socket_from_system((const struct sockaddr *)&system, length, peer);
		if (error != 0)
			memset(peer, 0, sizeof(*peer));
	}

	/* No signal for a write after the peer closed (FreeBSD). */
#if defined(SO_NOSIGPIPE)
	type = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &type, sizeof(type));
#endif

	/* Succeeded: the connection is a socket. */
	*accepted = made;
	return 0;
}

/*
 * Sends bytes on a connected socket.
 */
int
kl_socket_send(
	struct kl_socket *socket,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	ssize_t written;
	int error;

	/* The bytes, without a signal when the peer closed; a signal before any went sends again. */
	*sent = 0;
	for (;;) {
		written = send(socket->fd, bytes, size, MSG_NOSIGNAL);
		if (written >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;
		if (error == EWOULDBLOCK)
			return EAGAIN;
		return error;
	}

	/* Succeeded: so many went. */
	*sent = (size_t)written;
	return 0;
}

/*
 * Receives bytes from a connected socket.
 */
int
kl_socket_receive(
	struct kl_socket *socket,
	void *buffer,
	size_t size,
	size_t *received)
{
	ssize_t read_size;
	int error;

	/* What waits, or the end; a signal before any came receives again. */
	*received = 0;
	for (;;) {
		read_size = recv(socket->fd, buffer, size, 0);
		if (read_size >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;
		if (error == EWOULDBLOCK)
			return EAGAIN;
		return error;
	}

	/* Succeeded: so many came (0: the end). */
	*received = (size_t)read_size;
	return 0;
}

/*
 * Sends one datagram to an address.
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
	socklen_t length;
	ssize_t written;
	int error;

	/* The system's address of the socket's family. */
	*sent = 0;
	if (to->family != socket->family)
		return EAFNOSUPPORT;
	error = socket_to_system(to, &system, &length);
	if (error != 0)
		return error;

	/* The datagram; a signal before it went sends again. */
	for (;;) {
		written = sendto(socket->fd, bytes, size, MSG_NOSIGNAL, (const struct sockaddr *)&system, length);
		if (written >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;
		if (error == EWOULDBLOCK)
			return EAGAIN;
		return error;
	}

	/* Succeeded: the datagram went. */
	*sent = (size_t)written;
	return 0;
}

/*
 * Receives one datagram and where it came from.
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
	socklen_t length;
	ssize_t read_size;
	int error;

	/* The datagram (cut to the buffer); a signal before it came receives again. */
	*received = 0;
	for (;;) {
		length = sizeof(system);
		read_size = recvfrom(socket->fd, buffer, size, 0, (struct sockaddr *)&system, &length);
		if (read_size >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;
		if (error == EWOULDBLOCK)
			return EAGAIN;
		return error;
	}

	/* Where it came from. */
	if (from != NULL) {
		error = socket_from_system((const struct sockaddr *)&system, length, from);
		if (error != 0)
			memset(from, 0, sizeof(*from));
	}

	/* Succeeded: so many bytes of it. */
	*received = (size_t)read_size;
	return 0;
}

/*
 * Closes a connection's receiving or sending or both.
 */
int
kl_socket_shutdown(
	struct kl_socket *socket,
	unsigned how)
{
	int system_how;
	int status;

	/* The system's name of what closes. */
	switch (how) {
	case KL_SOCKET_SHUT_RECEIVE:
		system_how = SHUT_RD;
		break;
	case KL_SOCKET_SHUT_SEND:
		system_how = SHUT_WR;
		break;
	case KL_SOCKET_SHUT_BOTH:
		system_how = SHUT_RDWR;
		break;
	default:
		return EINVAL;
	}

	/* The shutdown. */
	status = shutdown(socket->fd, system_how);
	if (status != 0)
		return errno;

	/* Succeeded: that side is closed. */
	return 0;
}

/*
 * Reads the local address a socket is bound to.
 */
int
kl_socket_local_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	struct sockaddr_storage system;
	socklen_t length;
	int status;
	int error;

	/* The system's address. */
	length = sizeof(system);
	status = getsockname(socket->fd, (struct sockaddr *)&system, &length);
	if (status != 0)
		return errno;

	/* As the interface's. */
	error = socket_from_system((const struct sockaddr *)&system, length, address);
	if (error != 0)
		return error;

	/* Succeeded: the local address. */
	return 0;
}

/*
 * Reads the address a socket is connected to.
 */
int
kl_socket_peer_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	struct sockaddr_storage system;
	socklen_t length;
	int status;
	int error;

	/* The system's address of the peer. */
	length = sizeof(system);
	status = getpeername(socket->fd, (struct sockaddr *)&system, &length);
	if (status != 0)
		return errno;

	/* As the interface's. */
	error = socket_from_system((const struct sockaddr *)&system, length, address);
	if (error != 0)
		return error;

	/* Succeeded: the peer's address. */
	return 0;
}

/*
 * Sets one of a socket's options.
 */
int
kl_socket_set_option(
	struct kl_socket *socket,
	unsigned option,
	int value)
{
	int level;
	int name;
	int status;

	/* On or off. */
	if (value != 0 && value != 1)
		return EINVAL;

	/* The system's level and name of the option, for the sockets it means something to. */
	switch (option) {
	case KL_SOCKET_REUSE_ADDRESS:
		level = SOL_SOCKET;
		name = SO_REUSEADDR;
		break;
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
	status = setsockopt(socket->fd, level, name, &value, sizeof(value));
	if (status != 0)
		return errno;

	/* Succeeded: the option is set. */
	return 0;
}

/*
 * Gives the system's handle of a socket.
 */
intptr_t
kl_socket_native(
	const struct kl_socket *socket)
{
	/* The descriptor. */
	return (intptr_t)socket->fd;
}

/*
 * Closes a socket.
 *
 * A NULL socket is ignored.
 */
void
kl_socket_close(
	struct kl_socket *socket)
{
	/* Nothing to close. */
	if (socket == NULL)
		return;

	/* The descriptor (closed even when a signal cuts the close short), and the record. */
	(void)close(socket->fd);
	free(socket);
}

/*
 * Waits for several sockets and wakers.
 */
int
kl_socket_poll(
	struct kl_socket_wait *waits,
	size_t count,
	int milliseconds,
	size_t *ready)
{
	struct pollfd stack[SOCKET_WAITS_STACK];
	struct pollfd *polled;
	struct kl_socket_wait *wait;
	int64_t deadline;
	int64_t left;
	unsigned events;
	unsigned found;
	size_t index;
	int timeout;
	int status;
	int error;

	/* Nothing ready yet. */
	*ready = 0;
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

	/* The system's list: on the stack, or allocated for a long one. */
	polled = stack;
	if (count > SOCKET_WAITS_STACK) {
		polled = calloc(count, sizeof(*polled));
		if (polled == NULL)
			return ENOMEM;
	}

	/* A socket for what it asks, a waker's pipe for its byte. */
	for (index = 0; index < count; index++) {
		wait = &waits[index];
		polled[index].revents = 0;
		if (wait->waker != NULL) {
			polled[index].fd = wait->waker->ends[0];
			polled[index].events = POLLIN;
			continue;
		}

		/* A socket asks for what the wait asks. */
		polled[index].fd = wait->socket->fd;
		polled[index].events = 0;
		if ((wait->events & KL_SOCKET_READABLE) != 0U)
			polled[index].events |= POLLIN;
		if ((wait->events & KL_SOCKET_WRITABLE) != 0U)
			polled[index].events |= POLLOUT;
	}

	/* The wait, again for what is left of the time after a signal. */
	deadline = 0;
	if (milliseconds > 0)
		deadline = socket_now_ms() + milliseconds;
	timeout = milliseconds;
	for (;;) {
		status = poll(polled, (nfds_t)count, timeout);
		if (status >= 0)
			break;
		error = errno;
		if (error != EINTR) {
			if (polled != stack)
				free(polled);
			return error;
		}

		/* A signal cut the wait short: the rest of the time is left. */
		if (milliseconds > 0) {
			left = deadline - socket_now_ms();
			if (left < 0)
				left = 0;
			timeout = (int)left;
		}
	}

	/* What each wait found: a hang-up lets a receive and a send return, an error is a failure. */
	for (index = 0; index < count && status > 0; index++) {
		wait = &waits[index];
		events = (unsigned)polled[index].revents;
		found = 0;
		if (wait->waker != NULL) {
			if ((events & POLLIN) != 0U) {
				socket_waker_drain(wait->waker);
				found = KL_SOCKET_READABLE;
			}
		} else {
			if ((events & POLLIN) != 0U)
				found |= KL_SOCKET_READABLE;
			if ((events & POLLOUT) != 0U)
				found |= KL_SOCKET_WRITABLE;
			if ((events & POLLHUP) != 0U)
				found |= wait->events;
			if ((events & (POLLERR | POLLNVAL)) != 0U)
				found |= KL_SOCKET_FAILED;
		}

		/* The wait's result, counted when it found anything. */
		wait->ready = found;
		if (found != 0U)
			(*ready)++;
	}

	/* The list allocated for a long wait goes. */
	if (polled != stack)
		free(polled);

	/* Succeeded: *ready waits are ready (none: the time ran out). */
	return 0;
}

/*
 * Opens a waker.
 */
int
kl_socket_waker_open(
	struct kl_socket_waker **waker)
{
	struct kl_socket_waker *made;
	int status;
	int error;

	/* Nothing open yet. */
	*waker = NULL;

	/* The record, quiet. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	atomic_init(&made->rung, 0);

	/* The pipe, whose ends neither block nor pass to started programs. */
	status = pipe2(made->ends, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		error = errno;
		free(made);
		return error;
	}

	/* Succeeded: the waker is quiet. */
	*waker = made;
	return 0;
}

/*
 * Rings a waker.
 */
void
kl_socket_waker_ring(
	struct kl_socket_waker *waker)
{
	unsigned char byte;
	ssize_t written;
	int was;

	/* The first ring since the last wait writes the byte the wait finds; the others merge with it. */
	was = atomic_exchange(&waker->rung, 1);
	if (was != 0)
		return;
	byte = 1U;
	written = write(waker->ends[1], &byte, 1U);
	(void)written;
}

/*
 * Closes a waker.
 */
void
kl_socket_waker_close(
	struct kl_socket_waker *waker)
{
	/* Nothing to close. */
	if (waker == NULL)
		return;

	/* Its pipe, and the record. */
	(void)close(waker->ends[0]);
	(void)close(waker->ends[1]);
	free(waker);
}

/* Writes an address as the system's (sockaddr_in or sockaddr_in6). */
static int
socket_to_system(
	const struct kl_address *address,
	struct sockaddr_storage *system,
	socklen_t *length)
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
		*length = sizeof(*ipv4);
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
	*length = sizeof(*ipv6);

	/* Succeeded: the IPv6 address. */
	return 0;
}

/* Reads the system's address (sockaddr_in or sockaddr_in6) as the interface's. */
static int
socket_from_system(
	const struct sockaddr *system,
	socklen_t length,
	struct kl_address *address)
{
	const struct sockaddr_in *ipv4;
	const struct sockaddr_in6 *ipv6;

	/* Nothing read yet. */
	memset(address, 0, sizeof(*address));

	/* An IPv4 address. */
	if (system->sa_family == AF_INET && length >= (socklen_t)sizeof(*ipv4)) {
		ipv4 = (const struct sockaddr_in *)(const void *)system;
		address->family = KL_ADDRESS_IPV4;
		address->port = ntohs(ipv4->sin_port);
		memcpy(address->bytes, &ipv4->sin_addr, sizeof(ipv4->sin_addr));
		return 0;
	}

	/* Only IPv6 besides. */
	if (system->sa_family != AF_INET6 || length < (socklen_t)sizeof(*ipv6))
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

/* Gives the errno value of a failed getaddrinfo. */
static int
socket_resolve_error(
	int answer)
{
	/* The host has no address. */
	if (answer == EAI_NONAME)
		return ENOENT;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
	if (answer == EAI_NODATA)
		return ENOENT;
#endif
#if defined(EAI_ADDRFAMILY)
	if (answer == EAI_ADDRFAMILY)
		return ENOENT;
#endif

	/* The answer failed for now, or for a reason of its own. */
	if (answer == EAI_AGAIN)
		return EAGAIN;
	if (answer == EAI_FAMILY)
		return EAFNOSUPPORT;
	if (answer == EAI_MEMORY)
		return ENOMEM;
	if (answer == EAI_SYSTEM && errno != 0)
		return errno;

	/* Anything else. */
	return EIO;
}

/* Waits for the end of a connection a signal cut short, as the system goes on with it. */
static int
socket_wait_connect(
	int fd)
{
	struct pollfd watched;
	socklen_t length;
	int status;
	int error;

	/* Writable when it ended, connected or not; a signal does not end the wait. */
	watched.fd = fd;
	watched.events = POLLOUT;
	for (;;) {
		watched.revents = 0;
		status = poll(&watched, 1, -1);
		if (status > 0)
			break;
		if (status < 0 && errno != EINTR)
			return errno;
	}

	/* How it ended. */
	error = 0;
	length = sizeof(error);
	status = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
	if (status != 0)
		return errno;

	/* Reports why the connection failed. */
	if (error != 0)
		return error;

	/* Succeeded: connected. */
	return 0;
}

/* Tells whether accept's error is a connection gone before it was taken, to pass over for the next. */
static int
socket_accept_passes(
	int error)
{
	/* The connection was given up, or the network failed under it (Linux reports such errors to accept). */
	switch (error) {
	case ECONNABORTED:
	case EPROTO:
	case ENETDOWN:
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ENOPROTOOPT:
	case EOPNOTSUPP:
#if defined(EHOSTDOWN)
	case EHOSTDOWN:
#endif
#if defined(ENONET)
	case ENONET:
#endif
		return 1;
	default:
		return 0;
	}
}

/* Empties a waker's pipe found readable, so that it is quiet until the next ring. */
static void
socket_waker_drain(
	struct kl_socket_waker *waker)
{
	unsigned char bytes[SOCKET_DRAIN];
	ssize_t drained;

	/* Every byte in the pipe (the pipe never holds more than one, but a full read costs nothing). */
	for (;;) {
		drained = read(waker->ends[0], bytes, sizeof(bytes));
		if (drained > 0)
			continue;
		if (drained < 0 && errno == EINTR)
			continue;
		break;
	}

	/* Quiet: the next ring writes a byte again. */
	atomic_store(&waker->rung, 0);
}

/* Reads the monotonic clock in milliseconds. */
static int64_t
socket_now_ms(void)
{
	struct timespec now;

	/* The clock that only goes forward. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Milliseconds. */
	return (int64_t)now.tv_sec * SOCKET_MS_SECOND + (int64_t)(now.tv_nsec / SOCKET_NS_MILLISECOND);
}
