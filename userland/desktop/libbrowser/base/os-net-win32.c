/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network of the operating-system layer on Windows.
 */

#include "base/os-net.h"
#include "base/os-win32.h"
#include "base/base.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The size of the buffer a drain reads a waker's pending bytes into. */
#define OS_NET_DRAIN_SIZE	256U

/* How many connections a waker may refuse as not its own before it gives up. */
#define OS_NET_WAKER_ATTEMPTS	8U

/* How long a waker waits for its own connection to be ready to accept, in milliseconds. */
#define OS_NET_ACCEPT_WAIT_MS	5000

/* The socket flag that keeps child processes from inheriting the socket (Windows 7 SP1 and later). */
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT	0x80
#endif

/*
 * The addresses of one host: the list the resolver gave, kept in its own
 * form, and how many entries it holds.
 *
 * The list is owned by this structure and released by wb_os_addresses_free.
 */
struct wb_os_addresses {
	/* The head of the resolver's list; never NULL for a structure the layer hands out. */
	ADDRINFOW *list;

	/* The number of entries in the list, counted once when the list was made. */
	size_t count;
};

/*
 * A waker: two TCP sockets joined on the loopback address.
 *
 * Both are non-blocking.  The wait end is the one a wb_os_poll waits on; a
 * ring sends its byte from the ring end.
 */
struct wb_os_waker {
	/* The end that becomes readable when the waker is rung. */
	SOCKET wait_end;

	/* The end a ring sends its byte from. */
	SOCKET ring_end;
};

/*
 * Whether Windows Sockets has been started, once for the process.
 *
 * The run-once object serializes the start; the status it leaves is read
 * only after the object says the start has run.
 */
static INIT_ONCE os_net_start_once = INIT_ONCE_STATIC_INIT;

/* What the start of Windows Sockets came to: 0 when it worked, EIO when it failed; kept for every later call. */
static int os_net_start_status;

static BOOL CALLBACK os_net_start_run(PINIT_ONCE once, PVOID parameter, PVOID *context);
static int os_net_start(void);
static int os_net_last_error(void);
static int os_net_resolve_error(int code);
static int os_net_create_error(int code);
static int os_net_send_error(int code);
static int os_net_receive_error(int code);
static int os_net_create_socket(int family, int type, int protocol, SOCKET *created);
static int os_net_set_nonblocking(SOCKET handle);
static fd_set *os_net_set_create(size_t capacity);
static void os_net_set_add(fd_set *set, SOCKET handle);
static void os_net_mark(struct wb_os_poll_item *items, size_t count, const fd_set *set, unsigned wants, unsigned flags);
static int os_net_listen_loopback(SOCKET *listener, struct sockaddr_in *bound);
static int os_net_accept_own(SOCKET listener, const struct sockaddr_in *own, SOCKET *accepted);

/*
 * Looks up the TCP addresses of a host.
 *
 * The host and the port are converted to UTF-16 for the W function; the
 * list is kept as the resolver made it and counted once.
 */
int
wb_os_resolve(
	const char *host,
	const char *port,
	struct wb_os_addresses **addresses)
{
	ADDRINFOW hints;
	ADDRINFOW *list;
	ADDRINFOW *entry;
	struct wb_os_addresses *created;
	wchar_t *wide_host;
	wchar_t *wide_port;
	size_t count;
	int status;
	int error;

	/* Refuses a call that names no place for the answer. */
	if (addresses == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the lookup has worked. */
	*addresses = NULL;

	/* Refuses a missing or empty host. */
	if (host == NULL || host[0] == '\0')
		return EINVAL;

	/* Refuses a missing or empty port. */
	if (port == NULL || port[0] == '\0')
		return EINVAL;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* Converts the host to UTF-16; text that is not valid UTF-8 breaks the contract. */
	wide_host = NULL;
	error = wb_os_wide_from_utf8(host, &wide_host, NULL);
	if (error == EILSEQ || error == ENAMETOOLONG)
		return EINVAL;

	if (error != 0)
		return error;

	/* Converts the port to UTF-16 the same way. */
	wide_port = NULL;
	error = wb_os_wide_from_utf8(port, &wide_port, NULL);
	if (error != 0) {
		free(wide_host);
		if (error == EILSEQ || error == ENAMETOOLONG)
			return EINVAL;

		return error;
	}

	/* Asks for stream addresses of any family, the way the engine connects. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Asks the system's resolver for the addresses. */
	list = NULL;
	status = GetAddrInfoW(wide_host, wide_port, &hints, &list);
	free(wide_host);
	free(wide_port);
	if (status != 0) {
		error = os_net_resolve_error(status);
		return error;
	}

	/* Refuses a success that came with no address. */
	if (list == NULL)
		return EIO;

	/* Counts the entries of the list. */
	count = 0;
	for (entry = list;
	     entry != NULL;
	     entry = entry->ai_next)
		count++;

	/* Allocates the structure that owns the list. */
	created = malloc(sizeof(*created));
	if (created == NULL) {
		FreeAddrInfoW(list);
		return ENOMEM;
	}

	/* Fills the structure and hands it to the caller. */
	created->list = list;
	created->count = count;
	*addresses = created;

	/* Succeeded: the caller owns the list. */
	return 0;
}

/*
 * Reports how many addresses a list holds.
 */
size_t
wb_os_addresses_count(
	const struct wb_os_addresses *addresses)
{
	/* A missing list holds nothing. */
	if (addresses == NULL)
		return 0;

	/* Succeeded: the count made when the list was created. */
	return addresses->count;
}

/*
 * Frees a list of addresses.
 */
void
wb_os_addresses_free(
	struct wb_os_addresses *addresses)
{
	/* Does nothing for no list. */
	if (addresses == NULL)
		return;

	/* Gives the resolver's entries back, then the structure around them. */
	FreeAddrInfoW(addresses->list);
	free(addresses);
}

/*
 * Creates a TCP socket for one address of a list and starts connecting it.
 *
 * A non-blocking connect on Windows fails with WSAEWOULDBLOCK while it goes
 * on; that is reported as EINPROGRESS.
 */
int
wb_os_socket_connect(
	const struct wb_os_addresses *addresses,
	size_t index,
	int blocking,
	wb_os_socket *created)
{
	const ADDRINFOW *entry;
	size_t position;
	SOCKET handle;
	int status;
	int error;
	int code;

	/* Refuses a call that names no place for the socket. */
	if (created == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the socket is made. */
	*created = WB_OS_SOCKET_NONE;

	/* Refuses a missing list. */
	if (addresses == NULL)
		return EINVAL;

	/* Refuses an index past the end of the list. */
	if (index >= addresses->count)
		return EINVAL;

	/* Walks to the chosen entry. */
	entry = addresses->list;
	for (position = 0;
	     position < index;
	     position++)
		entry = entry->ai_next;

	/* Refuses an entry the resolver left without an address to connect to. */
	if (entry->ai_addr == NULL || entry->ai_addrlen == 0 || entry->ai_addrlen > (size_t)INT_MAX)
		return EINVAL;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* Creates the socket, not inherited by child processes. */
	error = os_net_create_socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol, &handle);
	if (error != 0)
		return error;

	/* Makes the socket non-blocking when the caller asked for that. */
	if (blocking == 0) {
		error = os_net_set_nonblocking(handle);
		if (error != 0) {
			closesocket(handle);
			return error;
		}
	}

	/* Starts the connection and classifies how it went. */
	error = 0;
	status = connect(handle, entry->ai_addr, (int)entry->ai_addrlen);
	if (status == SOCKET_ERROR) {
		code = WSAGetLastError();
		if (blocking == 0 && (code == WSAEWOULDBLOCK || code == WSAEINPROGRESS)) {
			/* A non-blocking connect goes on in the system. */
			error = EINPROGRESS;
		} else {
			/* The connect failed; a code that maps to nothing is still a failure. */
			error = wb_os_error_from_socket(code);
			if (error == 0)
				error = EIO;
		}
	}

	/* A connect that failed does not keep the socket. */
	if (error != 0 && error != EINPROGRESS) {
		closesocket(handle);
		return error;
	}

	/* Hands the socket to the caller. */
	*created = (wb_os_socket)handle;

	/* A connect under way is reported as such; the socket is valid. */
	if (error == EINPROGRESS)
		return EINPROGRESS;

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Reports how a non-blocking connect ended.
 *
 * The socket's pending error is read.  A socket with no error that is not
 * yet writable, or has no peer yet, is still connecting.
 */
int
wb_os_socket_connect_result(
	wb_os_socket socket)
{
	struct sockaddr_storage peer;
	struct timeval immediate;
	fd_set writing;
	int peer_length;
	int pending_length;
	int pending;
	int status;
	int error;

	/* Refuses a socket that holds nothing. */
	if (socket < 0)
		return EINVAL;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* Reads the error the connect left on the socket. */
	pending = 0;
	pending_length = (int)sizeof(pending);
	status = getsockopt((SOCKET)socket, SOL_SOCKET, SO_ERROR, (char *)&pending, &pending_length);
	if (status == SOCKET_ERROR) {
		error = os_net_last_error();
		if (error == 0)
			error = EIO;

		return error;
	}

	/* A connect still under way reports its progress as an error. */
	if (pending == WSAEWOULDBLOCK || pending == WSAEINPROGRESS || pending == WSAEALREADY)
		return EINPROGRESS;

	/* A real error ends the connect. */
	if (pending != 0) {
		error = wb_os_error_from_socket(pending);
		return error;
	}

	/* A connect that has not finished does not make the socket writable; some systems also report a peer early. */
	FD_ZERO(&writing);
	FD_SET((SOCKET)socket, &writing);
	immediate.tv_sec = 0;
	immediate.tv_usec = 0;
	status = select(0, NULL, &writing, NULL, &immediate);
	if (status == 0)
		return EINPROGRESS;

	/* Asks for the peer; a socket without one has not finished connecting. */
	peer_length = (int)sizeof(peer);
	status = getpeername((SOCKET)socket, (struct sockaddr *)&peer, &peer_length);
	if (status == SOCKET_ERROR) {
		if (WSAGetLastError() == WSAENOTCONN)
			return EINPROGRESS;

		error = os_net_last_error();
		if (error == 0)
			error = EIO;

		return error;
	}

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Sends bytes on a connected socket.
 *
 * One call sends at most INT_MAX bytes; fewer than asked is not an error.
 */
int
wb_os_socket_send(
	wb_os_socket socket,
	const void *bytes,
	size_t length,
	size_t *sent)
{
	size_t request;
	int taken;
	int code;
	int error;

	/* Refuses a call that names no place for the count. */
	if (sent == NULL)
		return EINVAL;

	/* Leaves the count at 0 until bytes were taken. */
	*sent = 0;

	/* Refuses a socket that holds nothing. */
	if (socket < 0)
		return EINVAL;

	/* Refuses bytes that are missing. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* A send of nothing sends nothing. */
	if (length == 0)
		return 0;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* The system counts in an int, so one call takes at most INT_MAX bytes. */
	request = length;
	if (request > (size_t)INT_MAX)
		request = (size_t)INT_MAX;

	/* Sends, again when the call was interrupted before it took anything. */
	do {
		taken = send((SOCKET)socket, (const char *)bytes, (int)request, 0);
		code = 0;
		if (taken == SOCKET_ERROR)
			code = WSAGetLastError();
	} while (taken == SOCKET_ERROR && code == WSAEINTR);

	/* Reports why nothing was taken. */
	if (taken == SOCKET_ERROR) {
		error = os_net_send_error(wb_os_error_from_socket(code));
		return error;
	}

	/* Reports how much the system took. */
	*sent = (size_t)taken;

	/* Succeeded: some or all of the bytes were taken. */
	return 0;
}

/*
 * Receives bytes from a connected socket.
 *
 * One call receives at most INT_MAX bytes; 0 bytes with a return of 0 is
 * the end of the stream.
 */
int
wb_os_socket_receive(
	wb_os_socket socket,
	void *bytes,
	size_t length,
	size_t *received)
{
	size_t request;
	int taken;
	int code;
	int error;

	/* Refuses a call that names no place for the count. */
	if (received == NULL)
		return EINVAL;

	/* Leaves the count at 0 until bytes arrived. */
	*received = 0;

	/* Refuses a socket that holds nothing. */
	if (socket < 0)
		return EINVAL;

	/* Refuses room that is missing or empty. */
	if (bytes == NULL || length == 0)
		return EINVAL;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* The system counts in an int, so one call takes at most INT_MAX bytes. */
	request = length;
	if (request > (size_t)INT_MAX)
		request = (size_t)INT_MAX;

	/* Receives, again when the call was interrupted before anything arrived. */
	do {
		taken = recv((SOCKET)socket, (char *)bytes, (int)request, 0);
		code = 0;
		if (taken == SOCKET_ERROR)
			code = WSAGetLastError();
	} while (taken == SOCKET_ERROR && code == WSAEINTR);

	/* Reports why nothing arrived. */
	if (taken == SOCKET_ERROR) {
		error = os_net_receive_error(wb_os_error_from_socket(code));
		return error;
	}

	/* Reports how much arrived; 0 is the peer's close. */
	*received = (size_t)taken;

	/* Succeeded: the bytes are in the caller's buffer. */
	return 0;
}

/*
 * Closes a socket.
 */
void
wb_os_socket_close(
	wb_os_socket socket)
{
	/* Does nothing for a socket that holds nothing. */
	if (socket < 0)
		return;

	/* Releases the socket. */
	closesocket((SOCKET)socket);
}

/*
 * Gives the socket as the int a TLS library's SSL_set_fd takes.
 */
int
wb_os_socket_descriptor(
	wb_os_socket socket,
	int *descriptor)
{
	/* Refuses a call that names no place for the answer. */
	if (descriptor == NULL)
		return EINVAL;

	/* Refuses a socket that holds nothing. */
	if (socket < 0)
		return EINVAL;

	/* Refuses a value an int cannot hold. */
	if (socket > INT_MAX)
		return ERANGE;

	/* Hands the descriptor over. */
	*descriptor = (int)socket;

	/* Succeeded: the descriptor is the socket's own value. */
	return 0;
}

/*
 * Reports the calling thread's last socket error.
 */
int
wb_os_socket_last_error(
	void)
{
	int error;

	/* Maps the thread's last Windows Sockets error to the layer's value. */
	error = os_net_last_error();

	/* Succeeded: the mapped error, 0 when there is none. */
	return error;
}

/*
 * Waits until one of the items is ready or the time runs out.
 *
 * select is used with sets allocated for the number of items, so the wait
 * is not limited to FD_SETSIZE sockets.  Items that wait for a send are also
 * put in the exception set, where select reports a connect that failed.
 */
int
wb_os_poll(
	struct wb_os_poll_item *items,
	size_t count,
	int timeout_ms,
	size_t *ready)
{
	struct timeval limit;
	struct timeval *limit_pointer;
	fd_set *read_set;
	fd_set *write_set;
	fd_set *except_set;
	size_t position;
	size_t active;
	size_t readers;
	size_t writers;
	size_t found_count;
	int status;
	int code;
	int error;

	/* Refuses a call that names no place for the count. */
	if (ready == NULL)
		return EINVAL;

	/* Leaves the count at 0 until the wait has ended. */
	*ready = 0;

	/* Refuses more items than one wait takes. */
	if (count > WB_OS_POLL_MAX)
		return EINVAL;

	/* Refuses missing items. */
	if (count > 0 && items == NULL)
		return EINVAL;

	/* Refuses a timeout below the endless wait. */
	if (timeout_ms < -1)
		return EINVAL;

	/* Checks every item before any is touched, and counts the sockets to wait for. */
	active = 0;
	readers = 0;
	writers = 0;
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].socket == WB_OS_SOCKET_NONE)
			continue;

		if (items[position].socket < 0)
			return EINVAL;

		if ((items[position].wants & (WB_OS_POLL_READABLE | WB_OS_POLL_WRITABLE)) == 0)
			return EINVAL;

		active++;
		if ((items[position].wants & WB_OS_POLL_READABLE) != 0)
			readers++;
		if ((items[position].wants & WB_OS_POLL_WRITABLE) != 0)
			writers++;
	}

	/* An endless wait with nothing to wait for would never end. */
	if (active == 0 && timeout_ms == -1)
		return EINVAL;

	/* Starts every item as not ready. */
	for (position = 0;
	     position < count;
	     position++)
		items[position].found = 0;

	/* With nothing to wait for the call only sleeps, since select refuses empty sets. */
	if (active == 0) {
		if (timeout_ms > 0)
			Sleep((DWORD)timeout_ms);

		return 0;
	}

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* Allocates the three sets, each large enough for the items that go in it. */
	read_set = NULL;
	write_set = NULL;
	except_set = NULL;
	if (readers > 0) {
		read_set = os_net_set_create(readers);
		if (read_set == NULL)
			return ENOMEM;
	}

	if (writers > 0) {
		write_set = os_net_set_create(writers);
		if (write_set == NULL) {
			free(read_set);
			return ENOMEM;
		}

		except_set = os_net_set_create(writers);
		if (except_set == NULL) {
			free(read_set);
			free(write_set);
			return ENOMEM;
		}
	}

	/* Puts each item's socket in the sets of what it waits for. */
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].socket == WB_OS_SOCKET_NONE)
			continue;

		if ((items[position].wants & WB_OS_POLL_READABLE) != 0)
			os_net_set_add(read_set, (SOCKET)items[position].socket);

		if ((items[position].wants & WB_OS_POLL_WRITABLE) != 0) {
			os_net_set_add(write_set, (SOCKET)items[position].socket);
			os_net_set_add(except_set, (SOCKET)items[position].socket);
		}
	}

	/* Turns the timeout into the form select takes; no limit is a NULL pointer. */
	limit_pointer = NULL;
	if (timeout_ms >= 0) {
		limit.tv_sec = (long)(timeout_ms / 1000);
		limit.tv_usec = (long)(timeout_ms % 1000) * 1000L;
		limit_pointer = &limit;
	}

	/* Waits; the first argument is ignored by Windows. */
	status = select(0, read_set, write_set, except_set, limit_pointer);
	if (status == SOCKET_ERROR) {
		code = WSAGetLastError();
		free(read_set);
		free(write_set);
		free(except_set);

		/* An interrupted wait ends with nothing ready. */
		if (code == WSAEINTR)
			return 0;

		if (code == WSA_NOT_ENOUGH_MEMORY || code == WSAENOBUFS)
			return ENOMEM;

		return EIO;
	}

	/* Marks the items whose sockets select left in each set. */
	if (read_set != NULL)
		os_net_mark(items, count, read_set, WB_OS_POLL_READABLE, WB_OS_POLL_READABLE);

	if (write_set != NULL)
		os_net_mark(items, count, write_set, WB_OS_POLL_WRITABLE, WB_OS_POLL_WRITABLE);

	if (except_set != NULL)
		os_net_mark(items, count, except_set, WB_OS_POLL_WRITABLE, WB_OS_POLL_WRITABLE | WB_OS_POLL_FAILED);

	/* Gives the sets back. */
	free(read_set);
	free(write_set);
	free(except_set);

	/* Counts the items that are ready. */
	found_count = 0;
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].found != 0)
			found_count++;
	}

	/* Reports how many items were ready. */
	*ready = found_count;

	/* Succeeded: also when the time ran out with nothing ready. */
	return 0;
}

/*
 * Creates a waker.
 *
 * A listening socket on the loopback address is connected to and accepted
 * from; the accepted connection must be the one this call made, so a
 * stranger that connected first is turned away.  The listening socket is
 * closed before returning.
 */
int
wb_os_waker_open(
	struct wb_os_waker **waker)
{
	struct wb_os_waker *created;
	struct sockaddr_in bound;
	struct sockaddr_in own;
	SOCKET listener;
	SOCKET ring_end;
	SOCKET wait_end;
	BOOL changed;
	int own_length;
	int disabled;
	int status;
	int error;

	/* Refuses a call that names no place for the waker. */
	if (waker == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the waker is made. */
	*waker = NULL;

	/* Starts Windows Sockets if this is the first call that needs it. */
	error = os_net_start();
	if (error != 0)
		return error;

	/* Nothing is held yet; the cleanup closes only what was made. */
	listener = INVALID_SOCKET;
	ring_end = INVALID_SOCKET;
	wait_end = INVALID_SOCKET;
	created = NULL;

	/* Listens on the loopback address at a port the system picks, with room for one connection. */
	error = os_net_listen_loopback(&listener, &bound);
	if (error != 0)
		goto cleanup;

	/* Creates the socket that will ring. */
	error = os_net_create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &ring_end);
	if (error != 0)
		goto cleanup;

	/* Connects it to the listener; on the loopback address this finishes at once. */
	status = connect(ring_end, (const struct sockaddr *)&bound, (int)sizeof(bound));
	if (status == SOCKET_ERROR) {
		error = os_net_last_error();
		if (error == 0)
			error = EIO;

		goto cleanup;
	}

	/* Learns the local address and port of the connecting socket, to recognize it on the other side. */
	own_length = (int)sizeof(own);
	status = getsockname(ring_end, (struct sockaddr *)&own, &own_length);
	if (status == SOCKET_ERROR) {
		error = EIO;
		goto cleanup;
	}

	/* Accepts the connection that is this call's own. */
	error = os_net_accept_own(listener, &own, &wait_end);
	if (error != 0)
		goto cleanup;

	/* Makes both ends non-blocking. */
	error = os_net_set_nonblocking(ring_end);
	if (error != 0)
		goto cleanup;

	error = os_net_set_nonblocking(wait_end);
	if (error != 0)
		goto cleanup;

	/* An accepted socket may inherit the listener's properties; makes sure child processes do not inherit it. */
	changed = SetHandleInformation((HANDLE)wait_end, HANDLE_FLAG_INHERIT, 0);
	if (!changed) {
		error = EIO;
		goto cleanup;
	}

	/* Sends a ring at once instead of holding the byte back for more. */
	disabled = 1;
	status = setsockopt(ring_end, IPPROTO_TCP, TCP_NODELAY, (const char *)&disabled, (int)sizeof(disabled));
	if (status == SOCKET_ERROR) {
		error = EIO;
		goto cleanup;
	}

	/* Allocates the structure that owns the two ends. */
	created = malloc(sizeof(*created));
	if (created == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Fills the structure; it now owns the two ends. */
	created->wait_end = wait_end;
	created->ring_end = ring_end;
	*waker = created;
	wait_end = INVALID_SOCKET;
	ring_end = INVALID_SOCKET;
	error = 0;

cleanup:
	/* Closes the listener always, and the ends if the waker was not made. */
	if (listener != INVALID_SOCKET)
		closesocket(listener);

	if (ring_end != INVALID_SOCKET)
		closesocket(ring_end);

	if (wait_end != INVALID_SOCKET)
		closesocket(wait_end);

	/* Reports why the waker was not made. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the waker. */
	return 0;
}

/*
 * Rings a waker.
 *
 * One byte goes to the other end; when the socket is full a ring is
 * already pending and nothing more is needed.
 */
void
wb_os_waker_ring(
	struct wb_os_waker *waker)
{
	static const char ring_byte = 1;
	int taken;
	int code;

	/* Does nothing for no waker. */
	if (waker == NULL)
		return;

	/* Sends the byte, again when the call was interrupted; any other failure is ignored. */
	do {
		taken = send(waker->ring_end, &ring_byte, 1, 0);
		code = 0;
		if (taken == SOCKET_ERROR)
			code = WSAGetLastError();
	} while (taken == SOCKET_ERROR && code == WSAEINTR);
}

/*
 * Reads every pending byte of a waker.
 */
void
wb_os_waker_drain(
	struct wb_os_waker *waker)
{
	char buffer[OS_NET_DRAIN_SIZE];
	int taken;

	/* Does nothing for no waker. */
	if (waker == NULL)
		return;

	/* Reads until nothing is pending or the socket fails; the socket is non-blocking, so this never waits. */
	for (;;) {
		taken = recv(waker->wait_end, buffer, (int)sizeof(buffer), 0);
		if (taken == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;

		if (taken <= 0)
			break;
	}
}

/*
 * Gives the end of a waker to put in a poll item.
 */
wb_os_socket
wb_os_waker_socket(
	const struct wb_os_waker *waker)
{
	/* A missing waker has no end to wait on. */
	if (waker == NULL)
		return WB_OS_SOCKET_NONE;

	/* Succeeded: the end that becomes readable when the waker is rung. */
	return (wb_os_socket)waker->wait_end;
}

/*
 * Closes both ends of a waker and frees it.
 */
void
wb_os_waker_close(
	struct wb_os_waker *waker)
{
	/* Does nothing for no waker. */
	if (waker == NULL)
		return;

	/* Releases both ends, then the structure. */
	closesocket(waker->wait_end);
	closesocket(waker->ring_end);
	free(waker);
}

/*
 * Runs the start of Windows Sockets, once.
 *
 * Version 2.2 is asked for; a start that fails or gives another version is
 * recorded as EIO for every call that comes after.
 */
static BOOL CALLBACK
os_net_start_run(
	PINIT_ONCE once,
	PVOID parameter,
	PVOID *context)
{
	WSADATA data;
	int status;

	UNUSED_PARAMETER(once);
	UNUSED_PARAMETER(parameter);
	UNUSED_PARAMETER(context);

	/* Starts Windows Sockets at version 2.2. */
	status = WSAStartup(MAKEWORD(2, 2), &data);
	if (status != 0) {
		os_net_start_status = EIO;
		return TRUE;
	}

	/* Gives the start back if the system offers another version. */
	if (data.wVersion != MAKEWORD(2, 2)) {
		WSACleanup();
		os_net_start_status = EIO;
		return TRUE;
	}

	/* Records that the start worked. */
	os_net_start_status = 0;

	/* Succeeded: the run-once object marks the start as done. */
	return TRUE;
}

/*
 * Starts Windows Sockets if no call has yet, and reports whether it is running.
 */
static int
os_net_start(
	void)
{
	BOOL done;

	/* Runs the start once, from whichever thread comes first. */
	done = InitOnceExecuteOnce(&os_net_start_once, os_net_start_run, NULL, NULL);
	if (!done)
		return EIO;

	/* Succeeded: 0 when the start worked, EIO when it failed (for this and every later call). */
	return os_net_start_status;
}

/*
 * Maps the calling thread's last Windows Sockets error to the layer's value.
 */
static int
os_net_last_error(
	void)
{
	int error;

	/* Maps the code; 0 stays 0. */
	error = wb_os_error_from_socket(WSAGetLastError());

	/* Succeeded: the layer's value. */
	return error;
}

/*
 * Maps the code GetAddrInfoW returned to the layer's errno value.
 */
static int
os_net_resolve_error(
	int code)
{
	/* A name with no address is the name's absence. */
	if (code == WSAHOST_NOT_FOUND || code == WSANO_DATA)
		return ENOENT;

	/* A temporary failure of the resolver may pass. */
	if (code == WSATRY_AGAIN)
		return EAGAIN;

	/* The resolver ran out of memory. */
	if (code == WSA_NOT_ENOUGH_MEMORY)
		return ENOMEM;

	/* Every other failure is a general failure. */
	return EIO;
}

/*
 * Maps the layer's value of a failed socket creation to the ones os-net.h lists.
 */
static int
os_net_create_error(
	int code)
{
	/* A shortage of network buffers is a shortage of memory. */
	if (code == ENOBUFS)
		return ENOMEM;

	/* Nothing to report is still a failure. */
	if (code == 0)
		return EIO;

	/* The other values are reported as mapped. */
	return code;
}

/*
 * Maps the layer's value of a failed send to one of the values os-net.h lists.
 */
static int
os_net_send_error(
	int code)
{
	/* A full buffer on a non-blocking socket. */
	if (code == EAGAIN)
		return EAGAIN;

	/* The connection can no longer send; an unconnected socket cannot either. */
	if (code == EPIPE || code == ENOTCONN)
		return EPIPE;

	/* The connection was reset or aborted. */
	if (code == ECONNRESET || code == ECONNABORTED)
		return code;

	/* The system lacks buffers for the data. */
	if (code == ENOBUFS || code == ENOMEM)
		return ENOBUFS;

	/* The call itself was wrong: not a socket, a bad address, or a bad argument. */
	if (code == ENOTSOCK || code == EINVAL)
		return EINVAL;

	/* Any other failure is a general failure. */
	return EIO;
}

/*
 * Maps the layer's value of a failed receive to one of the values os-net.h lists.
 */
static int
os_net_receive_error(
	int code)
{
	/* Nothing has arrived on a non-blocking socket. */
	if (code == EAGAIN)
		return EAGAIN;

	/* The connection was reset, aborted or timed out. */
	if (code == ECONNRESET || code == ECONNABORTED || code == ETIMEDOUT)
		return code;

	/* The call itself was wrong: not a socket, a bad address, or a bad argument. */
	if (code == ENOTSOCK || code == EINVAL)
		return EINVAL;

	/* Any other failure is a general failure. */
	return EIO;
}

/*
 * Creates a socket that child processes do not inherit.
 */
static int
os_net_create_socket(
	int family,
	int type,
	int protocol,
	SOCKET *created)
{
	SOCKET handle;
	int error;

	/* Creates the socket as an overlapped one, the kind socket() makes, with inheritance off. */
	*created = INVALID_SOCKET;
	handle = WSASocketW(family, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (handle == INVALID_SOCKET) {
		error = os_net_create_error(os_net_last_error());
		return error;
	}

	/* Hands the socket to the caller. */
	*created = handle;

	/* Succeeded: the caller owns the socket. */
	return 0;
}

/*
 * Makes a socket non-blocking.
 */
static int
os_net_set_nonblocking(
	SOCKET handle)
{
	u_long enabled;
	int status;

	/* Switches the socket to non-blocking mode. */
	enabled = 1;
	status = ioctlsocket(handle, FIONBIO, &enabled);
	if (status == SOCKET_ERROR)
		return EIO;

	/* Succeeded: sends, receives and connects no longer wait. */
	return 0;
}

/*
 * Allocates a set of sockets with room for the given number of sockets.
 *
 * The fixed array in fd_set holds FD_SETSIZE sockets; select reads as many
 * as fd_count says, so a larger block of the same shape is passed instead.
 */
static fd_set *
os_net_set_create(
	size_t capacity)
{
	fd_set *set;
	size_t bytes;

	/* Sizes the block as the count field plus the sockets. */
	bytes = offsetof(fd_set, fd_array) + capacity * sizeof(SOCKET);

	/* Allocates the block. */
	set = malloc(bytes);
	if (set == NULL)
		return NULL;

	/* Starts the set empty. */
	set->fd_count = 0;

	/* Succeeded: an empty set with room for capacity sockets. */
	return set;
}

/*
 * Adds a socket to a set made by os_net_set_create.
 *
 * The caller adds no more sockets than the set was made for.
 */
static void
os_net_set_add(
	fd_set *set,
	SOCKET handle)
{
	/* Stores the socket after the ones already in the set. */
	set->fd_array[set->fd_count] = handle;
	set->fd_count++;
}

/*
 * Adds flags to every item whose socket select left in a set.
 *
 * Only the items that asked for the matching kind are marked.
 */
static void
os_net_mark(
	struct wb_os_poll_item *items,
	size_t count,
	const fd_set *set,
	unsigned wants,
	unsigned flags)
{
	size_t position;
	size_t entry;

	/* Looks up each socket the set still holds among the items. */
	for (entry = 0;
	     entry < set->fd_count;
	     entry++) {
		for (position = 0;
		     position < count;
		     position++) {
			if (items[position].socket == WB_OS_SOCKET_NONE)
				continue;

			if ((items[position].wants & wants) == 0)
				continue;

			if ((SOCKET)items[position].socket != set->fd_array[entry])
				continue;

			items[position].found |= flags;
		}
	}
}

/*
 * Creates a socket that listens on the loopback address at a port the system picks.
 */
static int
os_net_listen_loopback(
	SOCKET *listener,
	struct sockaddr_in *bound)
{
	SOCKET handle;
	int length;
	int status;
	int error;

	/* Creates the socket. */
	*listener = INVALID_SOCKET;
	error = os_net_create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &handle);
	if (error != 0)
		return error;

	/* Names the loopback address with port 0, which asks the system to pick a port. */
	memset(bound, 0, sizeof(*bound));
	bound->sin_family = AF_INET;
	bound->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bound->sin_port = 0;

	/* Binds the socket to that address. */
	status = bind(handle, (const struct sockaddr *)bound, (int)sizeof(*bound));
	if (status == SOCKET_ERROR) {
		error = os_net_create_error(os_net_last_error());
		closesocket(handle);
		return error;
	}

	/* Listens, with room for one waiting connection. */
	status = listen(handle, 1);
	if (status == SOCKET_ERROR) {
		error = os_net_create_error(os_net_last_error());
		closesocket(handle);
		return error;
	}

	/* Reads back the port the system picked. */
	length = (int)sizeof(*bound);
	status = getsockname(handle, (struct sockaddr *)bound, &length);
	if (status == SOCKET_ERROR) {
		closesocket(handle);
		return EIO;
	}

	/* Hands the listening socket to the caller. */
	*listener = handle;

	/* Succeeded: the caller owns the listening socket. */
	return 0;
}

/*
 * Accepts connections from a listener until one is the caller's own.
 *
 * A connection is the caller's own when its peer address and port are the
 * local address and port of the socket the caller connected with.  Any other
 * is closed and the accept is repeated, a limited number of times.
 */
static int
os_net_accept_own(
	SOCKET listener,
	const struct sockaddr_in *own,
	SOCKET *accepted)
{
	struct sockaddr_in peer;
	struct timeval limit;
	fd_set waiting;
	SOCKET handle;
	unsigned attempt;
	int peer_length;
	int status;

	/* Tries a limited number of connections. */
	*accepted = INVALID_SOCKET;
	for (attempt = 0;
	     attempt < OS_NET_WAKER_ATTEMPTS;
	     attempt++) {
		/* Waits until a connection is ready, so a connection that never comes does not hang the call. */
		FD_ZERO(&waiting);
		FD_SET(listener, &waiting);
		limit.tv_sec = OS_NET_ACCEPT_WAIT_MS / 1000;
		limit.tv_usec = (OS_NET_ACCEPT_WAIT_MS % 1000) * 1000;
		status = select(0, &waiting, NULL, NULL, &limit);
		if (status != 1)
			return EIO;

		/* Takes the connection. */
		handle = accept(listener, NULL, NULL);
		if (handle == INVALID_SOCKET)
			return EIO;

		/* Reads the connection's peer. */
		memset(&peer, 0, sizeof(peer));
		peer_length = (int)sizeof(peer);
		status = getpeername(handle, (struct sockaddr *)&peer, &peer_length);
		if (status == SOCKET_ERROR) {
			closesocket(handle);
			return EIO;
		}

		/* Keeps the connection when its peer is the caller's own socket. */
		if (peer.sin_family == AF_INET &&
		    peer.sin_addr.s_addr == own->sin_addr.s_addr &&
		    peer.sin_port == own->sin_port) {
			*accepted = handle;
			return 0;
		}

		/* Turns away a connection that belongs to somebody else. */
		closesocket(handle);
	}

	/* Every connection was somebody else's. */
	return EIO;
}
