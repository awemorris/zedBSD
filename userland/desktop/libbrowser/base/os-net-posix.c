/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network of the operating-system layer on POSIX systems.
 */

#include "base/os-net.h"
#include "base/base.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The size of the buffer a drain reads a waker's pending bytes into. */
#define OS_NET_DRAIN_SIZE	256U

/* The flags every send carries so a closed peer gives EPIPE instead of a signal. */
#ifdef MSG_NOSIGNAL
#define OS_NET_SEND_FLAGS	MSG_NOSIGNAL
#else
#define OS_NET_SEND_FLAGS	0
#endif

/*
 * The addresses of one host: the list the resolver gave, kept in its own
 * form, and how many entries it holds.
 *
 * The list is owned by this structure and released by wb_os_addresses_free.
 */
struct wb_os_addresses {
	/* The head of the resolver's list; never NULL for a structure the layer hands out. */
	struct addrinfo *list;

	/* The number of entries in the list, counted once when the list was made. */
	size_t count;
};

/*
 * A waker: the two ends of a pipe.
 *
 * Both ends are non-blocking.  The wait end is the one a wb_os_poll waits
 * on; a ring writes to the ring end.
 */
struct wb_os_waker {
	/* The end that becomes readable when the waker is rung. */
	int wait_end;

	/* The end a ring writes its byte to. */
	int ring_end;
};

static int os_net_resolve_error(int status);
static int os_net_socket_error(int code);
static int os_net_send_error(int code);
static int os_net_receive_error(int code);
static int os_net_set_nonblocking(int descriptor);
static int os_net_suppress_signal(int descriptor);
static int os_net_wait_connected(int descriptor);
static unsigned os_net_found(unsigned wants, short events);

/*
 * Looks up the TCP addresses of a host.
 *
 * The list is kept as the resolver made it and counted once.
 */
int
wb_os_resolve(
	const char *host,
	const char *port,
	struct wb_os_addresses **addresses)
{
	struct addrinfo hints;
	struct addrinfo *list;
	struct addrinfo *entry;
	struct wb_os_addresses *created;
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

	/* Asks for stream addresses of any family, the way the engine connects. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Asks the system's resolver for the addresses. */
	list = NULL;
	status = getaddrinfo(host, port, &hints, &list);
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
		freeaddrinfo(list);
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
	freeaddrinfo(addresses->list);
	free(addresses);
}

/*
 * Creates a TCP socket for one address of a list and starts connecting it.
 *
 * A blocking connect that a signal cut is carried on until the connection
 * ends, because the system goes on connecting after the signal.
 */
int
wb_os_socket_connect(
	const struct wb_os_addresses *addresses,
	size_t index,
	int blocking,
	wb_os_socket *created)
{
	const struct addrinfo *entry;
	size_t position;
	int descriptor;
	int status;
	int error;
	int saved;

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
	if (entry->ai_addr == NULL || entry->ai_addrlen == 0)
		return EINVAL;

	/* Creates the socket, closed on exec from the start so no thread can leak it. */
	descriptor = socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC, entry->ai_protocol);
	if (descriptor < 0) {
		error = os_net_socket_error(errno);
		return error;
	}

	/* Makes sure a send on this socket can never raise SIGPIPE. */
	error = os_net_suppress_signal(descriptor);
	if (error != 0) {
		close(descriptor);
		return error;
	}

	/* Makes the socket non-blocking when the caller asked for that. */
	if (blocking == 0) {
		error = os_net_set_nonblocking(descriptor);
		if (error != 0) {
			close(descriptor);
			return error;
		}
	}

	/* Starts the connection and classifies how it went. */
	error = 0;
	status = connect(descriptor, entry->ai_addr, entry->ai_addrlen);
	if (status != 0) {
		saved = errno;
		if (blocking == 0 && (saved == EINPROGRESS || saved == EINTR)) {
			/* A non-blocking connect goes on in the system. */
			error = EINPROGRESS;
		} else if (blocking != 0 && saved == EINTR) {
			/* A signal cut a blocking connect; the system goes on connecting, so wait for its end. */
			error = os_net_wait_connected(descriptor);
		} else {
			/* The connect failed. */
			error = saved;
		}
	}

	/* A connect that failed does not keep the socket. */
	if (error != 0 && error != EINPROGRESS) {
		close(descriptor);
		return error;
	}

	/* Hands the socket to the caller. */
	*created = (wb_os_socket)descriptor;

	/* A connect under way is reported as such; the socket is valid. */
	if (error == EINPROGRESS)
		return EINPROGRESS;

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Reports how a non-blocking connect ended.
 *
 * The socket's pending error is read, which clears it.  A socket with no
 * error that has no peer yet is still connecting.
 */
int
wb_os_socket_connect_result(
	wb_os_socket socket)
{
	struct sockaddr_storage peer;
	socklen_t peer_length;
	socklen_t pending_length;
	int pending;
	int status;

	/* Refuses a socket that holds nothing. */
	if (socket < 0 || socket > INT_MAX)
		return EINVAL;

	/* Reads and clears the error the connect left on the socket. */
	pending = 0;
	pending_length = sizeof(pending);
	status = getsockopt((int)socket, SOL_SOCKET, SO_ERROR, &pending, &pending_length);
	if (status != 0)
		return errno;

	/* A connect still under way reports its progress as an error. */
	if (pending == EINPROGRESS || pending == EALREADY)
		return EINPROGRESS;

	/* A real error ends the connect. */
	if (pending != 0)
		return pending;

	/* Asks for the peer; a socket without one has not finished connecting. */
	peer_length = sizeof(peer);
	status = getpeername((int)socket, (struct sockaddr *)&peer, &peer_length);
	if (status != 0) {
		if (errno == ENOTCONN)
			return EINPROGRESS;

		return errno;
	}

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Sends bytes on a connected socket.
 *
 * A send that a signal cut is retried; one that took only some of the
 * bytes reports how many.
 */
int
wb_os_socket_send(
	wb_os_socket socket,
	const void *bytes,
	size_t length,
	size_t *sent)
{
	ssize_t taken;
	size_t request;
	int error;

	/* Refuses a call that names no place for the count. */
	if (sent == NULL)
		return EINVAL;

	/* Leaves the count at 0 until bytes were taken. */
	*sent = 0;

	/* Refuses a socket that holds nothing. */
	if (socket < 0 || socket > INT_MAX)
		return EINVAL;

	/* Refuses bytes that are missing. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* A send of nothing sends nothing. */
	if (length == 0)
		return 0;

	/* The system counts in a signed size, so one call takes at most SSIZE_MAX bytes. */
	request = length;
	if (request > (size_t)SSIZE_MAX)
		request = (size_t)SSIZE_MAX;

	/* Sends, again when a signal cut the call before it took anything. */
	do {
		taken = send((int)socket, bytes, request, OS_NET_SEND_FLAGS);
	} while (taken < 0 && errno == EINTR);

	/* Reports why nothing was taken. */
	if (taken < 0) {
		error = os_net_send_error(errno);
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
 * A receive that a signal cut is retried; 0 bytes with a return of 0 is
 * the end of the stream.
 */
int
wb_os_socket_receive(
	wb_os_socket socket,
	void *bytes,
	size_t length,
	size_t *received)
{
	ssize_t taken;
	size_t request;
	int error;

	/* Refuses a call that names no place for the count. */
	if (received == NULL)
		return EINVAL;

	/* Leaves the count at 0 until bytes arrived. */
	*received = 0;

	/* Refuses a socket that holds nothing. */
	if (socket < 0 || socket > INT_MAX)
		return EINVAL;

	/* Refuses room that is missing or empty. */
	if (bytes == NULL || length == 0)
		return EINVAL;

	/* The system counts in a signed size, so one call takes at most SSIZE_MAX bytes. */
	request = length;
	if (request > (size_t)SSIZE_MAX)
		request = (size_t)SSIZE_MAX;

	/* Receives, again when a signal cut the call before anything arrived. */
	do {
		taken = recv((int)socket, bytes, request, 0);
	} while (taken < 0 && errno == EINTR);

	/* Reports why nothing arrived. */
	if (taken < 0) {
		error = os_net_receive_error(errno);
		return error;
	}

	/* Reports how much arrived; 0 is the peer's close. */
	*received = (size_t)taken;

	/* Succeeded: the bytes are in the caller's buffer. */
	return 0;
}

/*
 * Closes a socket.
 *
 * A close is not repeated after a signal: the descriptor is gone either way.
 */
void
wb_os_socket_close(
	wb_os_socket socket)
{
	/* Does nothing for a socket that holds nothing. */
	if (socket < 0 || socket > INT_MAX)
		return;

	/* Releases the descriptor. */
	close((int)socket);
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

	/* Succeeded: the descriptor is the socket itself. */
	return 0;
}

/*
 * Reports the calling thread's last socket error.
 *
 * On POSIX the socket calls set errno, which is the layer's own form.
 */
int
wb_os_socket_last_error(
	void)
{
	/* Succeeded: errno is already the layer's value. */
	return errno;
}

/*
 * Waits until one of the items is ready or the time runs out.
 *
 * Only the items that hold a socket go to the system; the others get a
 * found of 0.  A signal that cuts the wait ends it with nothing ready.
 */
int
wb_os_poll(
	struct wb_os_poll_item *items,
	size_t count,
	int timeout_ms,
	size_t *ready)
{
	struct pollfd *waits;
	size_t position;
	size_t active;
	size_t slot;
	size_t found_count;
	unsigned flags;
	int status;

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

	/* Checks every item before any is touched, and counts the ones that hold a socket. */
	active = 0;
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].socket == WB_OS_SOCKET_NONE)
			continue;

		if (items[position].socket < 0 || items[position].socket > INT_MAX)
			return EINVAL;

		if ((items[position].wants & (WB_OS_POLL_READABLE | WB_OS_POLL_WRITABLE)) == 0)
			return EINVAL;

		active++;
	}

	/* An endless wait with nothing to wait for would never end. */
	if (active == 0 && timeout_ms == -1)
		return EINVAL;

	/* Starts every item as not ready. */
	for (position = 0;
	     position < count;
	     position++)
		items[position].found = 0;

	/* Allocates the system's list of waits, one per item that holds a socket. */
	waits = NULL;
	if (active > 0) {
		waits = malloc(active * sizeof(*waits));
		if (waits == NULL)
			return ENOMEM;
	}

	/* Fills one wait for each item that holds a socket. */
	slot = 0;
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].socket == WB_OS_SOCKET_NONE)
			continue;

		waits[slot].fd = (int)items[position].socket;
		waits[slot].events = 0;
		waits[slot].revents = 0;
		if ((items[position].wants & WB_OS_POLL_READABLE) != 0)
			waits[slot].events |= POLLIN;
		if ((items[position].wants & WB_OS_POLL_WRITABLE) != 0)
			waits[slot].events |= POLLOUT;
		slot++;
	}

	/* Waits; with no socket at all this only sleeps for the timeout. */
	status = poll(waits, (nfds_t)active, timeout_ms);
	if (status < 0) {
		free(waits);

		/* A signal that cut the wait ends it with nothing ready. */
		if (errno == EINTR)
			return 0;

		/* The system could not get the memory the wait needs. */
		if (errno == ENOMEM)
			return ENOMEM;

		/* Any other failure of the wait. */
		return EIO;
	}

	/* Turns what the system found into the layer's flags and counts the ready items. */
	slot = 0;
	found_count = 0;
	for (position = 0;
	     position < count;
	     position++) {
		if (items[position].socket == WB_OS_SOCKET_NONE)
			continue;

		flags = os_net_found(items[position].wants, waits[slot].revents);
		items[position].found = flags;
		if (flags != 0)
			found_count++;
		slot++;
	}

	/* Gives the list of waits back. */
	free(waits);

	/* Reports how many items were ready. */
	*ready = found_count;

	/* Succeeded: also when the time ran out with nothing ready. */
	return 0;
}

/*
 * Creates a waker.
 *
 * The waker is a pipe whose two ends are non-blocking and close on exec.
 */
int
wb_os_waker_open(
	struct wb_os_waker **waker)
{
	struct wb_os_waker *created;
	int ends[2];
	int status;
	int saved;

	/* Refuses a call that names no place for the waker. */
	if (waker == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the waker is made. */
	*waker = NULL;

	/* Creates the pipe with both flags set from the start. */
	status = pipe2(ends, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		saved = errno;
		if (saved == EMFILE || saved == ENFILE)
			return EMFILE;

		if (saved == ENOMEM)
			return ENOMEM;

		return EIO;
	}

	/* Allocates the structure that owns the two ends. */
	created = malloc(sizeof(*created));
	if (created == NULL) {
		close(ends[0]);
		close(ends[1]);
		return ENOMEM;
	}

	/* Fills the structure and hands it to the caller. */
	created->wait_end = ends[0];
	created->ring_end = ends[1];
	*waker = created;

	/* Succeeded: the caller owns the waker. */
	return 0;
}

/*
 * Rings a waker.
 *
 * One byte goes into the pipe; when the pipe is full a ring is already
 * pending and nothing more is needed.
 */
void
wb_os_waker_ring(
	struct wb_os_waker *waker)
{
	static const unsigned char ring_byte = 1;
	ssize_t written;

	/* Does nothing for no waker. */
	if (waker == NULL)
		return;

	/* Writes the byte, again when a signal cut the call; any other failure is ignored. */
	do {
		written = write(waker->ring_end, &ring_byte, 1);
	} while (written < 0 && errno == EINTR);
}

/*
 * Reads every pending byte of a waker.
 */
void
wb_os_waker_drain(
	struct wb_os_waker *waker)
{
	unsigned char buffer[OS_NET_DRAIN_SIZE];
	ssize_t taken;

	/* Does nothing for no waker. */
	if (waker == NULL)
		return;

	/* Reads until the pipe is empty or fails; the ends are non-blocking, so this never waits. */
	for (;;) {
		taken = read(waker->wait_end, buffer, sizeof(buffer));
		if (taken < 0 && errno == EINTR)
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
	close(waker->wait_end);
	close(waker->ring_end);
	free(waker);
}

/*
 * Maps the status getaddrinfo returned to the layer's errno value.
 */
static int
os_net_resolve_error(
	int status)
{
	/* A name with no address is the name's absence. */
	if (status == EAI_NONAME)
		return ENOENT;

#ifdef EAI_NODATA
	if (status == EAI_NODATA)
		return ENOENT;
#endif

	/* A temporary failure of the resolver may pass. */
	if (status == EAI_AGAIN)
		return EAGAIN;

	/* The resolver ran out of memory. */
	if (status == EAI_MEMORY)
		return ENOMEM;

	/* Every other failure is a general failure. */
	return EIO;
}

/*
 * Maps the errno of a failed socket creation to the layer's value.
 */
static int
os_net_socket_error(
	int code)
{
	/* A full system table of descriptors is the same shortage as a full process table. */
	if (code == ENFILE)
		return EMFILE;

	/* A shortage of network buffers is a shortage of memory. */
	if (code == ENOBUFS)
		return ENOMEM;

	/* The other errors are reported as the system gave them. */
	return code;
}

/*
 * Maps the errno of a failed send to one of the values os-net.h lists.
 */
static int
os_net_send_error(
	int code)
{
	/* A full buffer on a non-blocking socket. */
	if (code == EAGAIN || code == EWOULDBLOCK)
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
	if (code == EBADF || code == ENOTSOCK || code == EFAULT || code == EINVAL)
		return EINVAL;

	/* Any other failure is a general failure. */
	return EIO;
}

/*
 * Maps the errno of a failed receive to one of the values os-net.h lists.
 */
static int
os_net_receive_error(
	int code)
{
	/* Nothing has arrived on a non-blocking socket. */
	if (code == EAGAIN || code == EWOULDBLOCK)
		return EAGAIN;

	/* The connection was reset, aborted or timed out. */
	if (code == ECONNRESET || code == ECONNABORTED || code == ETIMEDOUT)
		return code;

	/* The call itself was wrong: not a socket, a bad address, or a bad argument. */
	if (code == EBADF || code == ENOTSOCK || code == EFAULT || code == EINVAL)
		return EINVAL;

	/* Any other failure is a general failure. */
	return EIO;
}

/*
 * Makes a descriptor non-blocking.
 */
static int
os_net_set_nonblocking(
	int descriptor)
{
	int flags;
	int status;

	/* Reads the descriptor's flags. */
	flags = fcntl(descriptor, F_GETFL, 0);
	if (flags < 0)
		return errno;

	/* Writes them back with the non-blocking flag added. */
	status = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (status != 0)
		return errno;

	/* Succeeded: sends, receives and connects no longer wait. */
	return 0;
}

/*
 * Makes sure a send on a socket never raises SIGPIPE.
 *
 * Where every send carries MSG_NOSIGNAL nothing is needed; where the system
 * offers the socket option instead, the option is set.
 */
static int
os_net_suppress_signal(
	int descriptor)
{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
	int enabled;
	int status;

	/* Asks the socket itself to refuse the signal. */
	enabled = 1;
	status = setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
	if (status != 0)
		return os_net_socket_error(errno);
#else
	UNUSED_PARAMETER(descriptor);
#endif

	/* Succeeded: a send cannot raise the signal. */
	return 0;
}

/*
 * Waits for a connect that a signal cut to end, and reports how it ended.
 */
static int
os_net_wait_connected(
	int descriptor)
{
	struct pollfd wait;
	int status;
	int error;

	/* Waits for the socket to become writable, which a connect's end does, until it is settled. */
	for (;;) {
		wait.fd = descriptor;
		wait.events = POLLOUT;
		wait.revents = 0;
		status = poll(&wait, 1, -1);
		if (status < 0 && errno == EINTR)
			continue;

		if (status < 0)
			return EIO;

		error = wb_os_socket_connect_result((wb_os_socket)descriptor);
		if (error != EINPROGRESS)
			break;
	}

	/* Reports how the connect ended. */
	if (error != 0)
		return error;

	/* Succeeded: the connection is made. */
	return 0;
}

/*
 * Turns the events the system found on one descriptor into the layer's flags.
 *
 * An error or a hang-up is a failure, and it also makes whatever was asked
 * ready, since the call that would follow does not wait.
 */
static unsigned
os_net_found(
	unsigned wants,
	short events)
{
	unsigned flags;

	flags = 0;

	/* A receive would not wait when data, the end of the stream or an error is there. */
	if ((wants & WB_OS_POLL_READABLE) != 0) {
		if ((events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
			flags |= WB_OS_POLL_READABLE;
	}

	/* A send would not wait when there is room, a connect ended, or an error is there. */
	if ((wants & WB_OS_POLL_WRITABLE) != 0) {
		if ((events & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0)
			flags |= WB_OS_POLL_WRITABLE;
	}

	/* The system reported an error or a hang-up, asked for or not. */
	if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0)
		flags |= WB_OS_POLL_FAILED;

	/* Succeeded: the flags that hold. */
	return flags;
}
