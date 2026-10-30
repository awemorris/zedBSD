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

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A socket the library opened: the system's handle, its family and kind
 * (KL_ADDRESS_*, KL_SOCKET_*), and whether it is non-blocking (WinSock has
 * no call that reads that back, and kl_socket_accept and the blocking
 * connect's wait need it).
 */
struct kl_socket {
	SOCKET handle;
	unsigned family;
	unsigned kind;
	int nonblocking;
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
 * WinSock started once for the process (socket_start's kl_once), and what
 * its start gave: 0, or the error every function then returns.  Written
 * once inside the kl_once and read after it.
 */
static struct kl_once socket_started = KL_ONCE_INIT;
static int socket_start_error;

static int socket_start(void);
static void socket_start_once(void);
static int socket_error(int error);
static int socket_to_system(const struct kl_address *address, struct sockaddr_storage *system, int *length);
static int socket_from_system(const struct sockaddr *system, int length, struct kl_address *address);
static int socket_resolve_error(int answer);
static int socket_set_blocking(SOCKET handle, int nonblocking);

/*
 * Reads a numeric address ("192.0.2.1", "2001:db8::1", "fe80::1%2") with a
 * port.
 *
 * Win32: inet_pton (InetPtonA, Windows Vista and later) for each family;
 * the IPv6 "%scope" part is cut off first and read as a number, or as an
 * interface's name with if_nametoindex (iphlpapi).
 */
int
kl_address_parse(
	const char *text,
	uint16_t port,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)text;
	(void)port;
	(void)address;
	return ENOSYS;
}

/*
 * Writes an address with its port ("192.0.2.1:993", "[2001:db8::1]:993").
 *
 * Win32: inet_ntop (InetNtopA), the "%scope" and the port added as on
 * POSIX.  ENOSPC when it does not fit.
 */
int
kl_address_format(
	const struct kl_address *address,
	char *text,
	size_t size)
{
	/* Not written yet. */
	(void)address;
	(void)text;
	(void)size;
	return ENOSYS;
}

/*
 * Sets the address of every interface of a family (0.0.0.0, ::) with a
 * port.  The same bytes as on POSIX (no system call).
 */
void
kl_address_any(
	unsigned family,
	uint16_t port,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)family;
	(void)port;
	(void)address;
}

/*
 * Sets the loopback address of a family (127.0.0.1, ::1) with a port.
 * The same bytes as on POSIX (no system call).
 */
void
kl_address_loopback(
	unsigned family,
	uint16_t port,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)family;
	(void)port;
	(void)address;
}

/*
 * Resolves a name (or a numeric address) to the addresses of a kind of
 * socket, in the system's order.
 *
 * Win32: GetAddrInfoW on the name made UTF-16 (MultiByteToWideChar(CP_UTF8);
 * a name that is not UTF-8 is ENOENT), AI_ADDRCONFIG not given (as on
 * POSIX), the answers turned with socket_from_system, FreeAddrInfoW.  Its
 * errors through socket_resolve_error: WSAHOST_NOT_FOUND and WSANO_DATA
 * ENOENT, WSATRY_AGAIN EAGAIN, WSAEAFNOSUPPORT EAFNOSUPPORT,
 * WSA_NOT_ENOUGH_MEMORY ENOMEM, others EIO.
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
	/* Not written yet. */
	(void)host;
	(void)port;
	(void)family;
	(void)kind;
	(void)addresses;
	(void)capacity;
	*count = 0U;
	return ENOSYS;
}

/*
 * Opens a socket of a family and kind.
 *
 * Win32: WSASocketW(domain, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED |
 * WSA_FLAG_NO_HANDLE_INHERIT); KL_SOCKET_NONBLOCKING by
 * ioctlsocket(FIONBIO); IPv6 with IPV6_V6ONLY 1 (Windows' default too, set
 * all the same); UDP with SIO_UDP_CONNRESET off (WSAIoctl, FALSE): Windows
 * alone makes an ICMP port unreachable a WSAECONNRESET of the next receive
 * of a socket that is not connected.  A failure after the socket was made
 * closes it.
 */
int
kl_socket_open(
	unsigned family,
	unsigned kind,
	unsigned flags,
	struct kl_socket **socket)
{
	/* Not written yet. */
	(void)family;
	(void)kind;
	(void)flags;
	*socket = NULL;
	return ENOSYS;
}

/*
 * Makes a socket non-blocking (on 1) or blocking (0).
 *
 * Win32: ioctlsocket(FIONBIO) (socket_set_blocking), and the record's
 * nonblocking.
 */
int
kl_socket_set_nonblocking(
	struct kl_socket *socket,
	int on)
{
	/* Not written yet. */
	(void)socket;
	(void)on;
	(void)socket_set_blocking;
	return ENOSYS;
}

/*
 * Connects a socket to an address.
 *
 * Win32: connect; on a non-blocking socket WSAEWOULDBLOCK is EINPROGRESS,
 * and a second connect while one is under way (WSAEALREADY, or WSAEINVAL
 * that older WinSock gives then) is EALREADY.  A blocking connect waits
 * itself (no EINTR on Windows).
 */
int
kl_socket_connect(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	/* Not written yet. */
	(void)socket;
	(void)address;
	(void)socket_to_system;
	return ENOSYS;
}

/*
 * Tells how a connect that did not block ended: 0, or why it failed.
 *
 * Win32: getsockopt(SOL_SOCKET, SO_ERROR), the value through socket_error.
 */
int
kl_socket_connect_result(
	struct kl_socket *socket)
{
	/* Not written yet. */
	(void)socket;
	return ENOSYS;
}

/*
 * Binds a socket to an address.
 *
 * Win32: bind; SO_EXCLUSIVEADDRUSE is not set (KL_SOCKET_REUSE_ADDRESS
 * below).
 */
int
kl_socket_bind(
	struct kl_socket *socket,
	const struct kl_address *address)
{
	/* Not written yet. */
	(void)socket;
	(void)address;
	return ENOSYS;
}

/*
 * Listens for connections (backlog 0: the system's, SOMAXCONN).
 *
 * Win32: listen.
 */
int
kl_socket_listen(
	struct kl_socket *socket,
	unsigned backlog)
{
	/* Not written yet. */
	(void)socket;
	(void)backlog;
	return ENOSYS;
}

/*
 * Accepts a connection as a new socket, blocking or not as flags says.
 *
 * Win32: accept; the new socket inherits the listening one's non-blocking
 * mode, so it is set to what flags says (ioctlsocket(FIONBIO)), and its
 * handle is kept from children (SetHandleInformation(HANDLE_FLAG_INHERIT,
 * 0)).  WSAECONNRESET (a connection reset before it was taken: Windows'
 * ECONNABORTED) is passed over and the next one taken (a non-blocking
 * socket with none left: EAGAIN).
 */
int
kl_socket_accept(
	struct kl_socket *socket,
	unsigned flags,
	struct kl_socket **accepted,
	struct kl_address *peer)
{
	/* Not written yet. */
	(void)socket;
	(void)flags;
	(void)peer;
	*accepted = NULL;
	return ENOSYS;
}

/*
 * Sends bytes on a connected socket; *sent tells how many went.
 *
 * Win32: send with flags 0; a size above INT_MAX sends INT_MAX (a short
 * send, as the header allows).  WSAEWOULDBLOCK is EAGAIN, WSAESHUTDOWN
 * EPIPE.
 */
int
kl_socket_send(
	struct kl_socket *socket,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	/* Not written yet. */
	(void)socket;
	(void)bytes;
	(void)size;
	*sent = 0U;
	return ENOSYS;
}

/*
 * Receives bytes from a connected socket; 0 bytes is the stream's end.
 *
 * Win32: recv; a size above INT_MAX receives at most INT_MAX.
 * WSAEWOULDBLOCK is EAGAIN.
 */
int
kl_socket_receive(
	struct kl_socket *socket,
	void *buffer,
	size_t size,
	size_t *received)
{
	/* Not written yet. */
	(void)socket;
	(void)buffer;
	(void)size;
	*received = 0U;
	return ENOSYS;
}

/*
 * Sends one datagram to an address.
 *
 * Win32: sendto (the size above INT_MAX is EMSGSIZE's EINVAL: a datagram is
 * not cut).
 */
int
kl_socket_send_to(
	struct kl_socket *socket,
	const void *bytes,
	size_t size,
	const struct kl_address *to,
	size_t *sent)
{
	/* Not written yet. */
	(void)socket;
	(void)bytes;
	(void)size;
	(void)to;
	*sent = 0U;
	return ENOSYS;
}

/*
 * Receives one datagram and where it came from.
 *
 * Win32: recvfrom; WSAEMSGSIZE (a datagram longer than the buffer, cut) is
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
	/* Not written yet. */
	(void)socket;
	(void)buffer;
	(void)size;
	(void)from;
	*received = 0U;
	return ENOSYS;
}

/*
 * Closes a socket's receiving, sending or both.
 *
 * Win32: shutdown with SD_RECEIVE, SD_SEND, SD_BOTH.
 */
int
kl_socket_shutdown(
	struct kl_socket *socket,
	unsigned how)
{
	/* Not written yet. */
	(void)socket;
	(void)how;
	return ENOSYS;
}

/*
 * Gives a socket's own address.
 *
 * Win32: getsockname, through socket_from_system.
 */
int
kl_socket_local_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)socket;
	(void)address;
	(void)socket_from_system;
	return ENOSYS;
}

/*
 * Gives the address a socket is connected to.
 *
 * Win32: getpeername, through socket_from_system.
 */
int
kl_socket_peer_address(
	const struct kl_socket *socket,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)socket;
	(void)address;
	return ENOSYS;
}

/*
 * Sets one of a socket's options on (1) or off (0).
 *
 * Win32: KL_SOCKET_REUSE_ADDRESS does nothing and succeeds (Windows'
 * SO_REUSEADDR lets a socket take a port another one uses, which is not
 * the POSIX meaning; a port left in TIME_WAIT is bound by default there),
 * KL_SOCKET_NO_DELAY TCP_NODELAY, KL_SOCKET_KEEP_ALIVE SO_KEEPALIVE,
 * KL_SOCKET_IPV6_ONLY IPV6_V6ONLY, KL_SOCKET_BROADCAST SO_BROADCAST; the
 * same ENOPROTOOPT and EINVAL as on POSIX for the wrong kind of socket.
 */
int
kl_socket_set_option(
	struct kl_socket *socket,
	unsigned option,
	int value)
{
	/* Not written yet. */
	(void)socket;
	(void)option;
	(void)value;
	return ENOSYS;
}

/*
 * Gives the system's handle of a socket (a SOCKET, which fits intptr_t).
 */
intptr_t
kl_socket_native(
	const struct kl_socket *socket)
{
	/* Not written yet. */
	(void)socket;
	return -1;
}

/*
 * Closes a socket and frees its record.
 *
 * Win32: closesocket.  NULL does nothing, as on POSIX (a connection that
 * never opened is closed with NULL).
 */
void
kl_socket_close(
	struct kl_socket *socket)
{
	/* Not written yet. */
	(void)socket;
}

/*
 * Waits until one of waits is ready, or for so many milliseconds.
 *
 * Win32: select.  Its fd_set is a count and an array of SOCKETs, so the
 * three sets are allocated for count entries (offsetof(fd_set, fd_array)
 * plus count SOCKETs; malloc, ENOMEM) and passed as fd_set pointers,
 * unbound by FD_SETSIZE (64).  A socket's READABLE goes in the read set,
 * WRITABLE in the write set, and every socket in the exception set (a
 * connect that failed shows there: FAILED).  A waker is ends[0] in the read
 * set.  The time is a timeval (NULL for KL_SOCKET_FOREVER).  select with no
 * socket at all is WSAEINVAL, so no waits only sleeps (Sleep; with no end
 * EINVAL).  After select: ready is what events asked for and was found,
 * and FAILED; a waker found readable is read empty and then made quiet
 * (struct kl_socket_waker).  The same EINVAL checks as on POSIX come first.
 */
int
kl_socket_poll(
	struct kl_socket_wait *waits,
	size_t count,
	int milliseconds,
	size_t *ready)
{
	/* Not written yet. */
	(void)waits;
	(void)count;
	(void)milliseconds;
	*ready = 0U;
	return ENOSYS;
}

/*
 * Opens a waker.
 *
 * Win32: a listening TCP socket on 127.0.0.1 port 0 with
 * SO_EXCLUSIVEADDRUSE (no other program can take the port), its port read
 * back (getsockname), a socket connected to it (blocking) and the
 * connection accepted; the accepted one's peer must be the connecting
 * socket's own address (getpeername against getsockname), else it is
 * another program's and is closed and the next taken.  The listening
 * socket is then closed, both ends made non-blocking and the writing end
 * given TCP_NODELAY.  WSAEMFILE is EMFILE, WSAENOBUFS ENOMEM.
 */
int
kl_socket_waker_open(
	struct kl_socket_waker **waker)
{
	/* Not written yet. */
	*waker = NULL;
	return ENOSYS;
}

/*
 * Rings a waker.
 *
 * Win32: InterlockedCompareExchange(rung, 1, 0); only when it was 0, one
 * byte sent on ends[1] (a full buffer, WSAEWOULDBLOCK, is a ring already
 * waiting: nothing more).
 */
void
kl_socket_waker_ring(
	struct kl_socket_waker *waker)
{
	/* Not written yet. */
	(void)waker;
}

/*
 * Closes a waker.
 *
 * Win32: closesocket on both ends, the record freed.  NULL does nothing.
 */
void
kl_socket_waker_close(
	struct kl_socket_waker *waker)
{
	/* Not written yet. */
	(void)waker;
}

/* Starts WinSock once (kl_once); 0, or the error its start gave. */
static int
socket_start(void)
{
	/* Not written yet. */
	(void)socket_started;
	(void)socket_start_error;
	(void)socket_start_once;
	(void)socket_error;
	(void)socket_resolve_error;
	return ENOSYS;
}

/* WSAStartup(MAKEWORD(2, 2)) and its outcome in socket_start_error. */
static void
socket_start_once(void)
{
	/* Not written yet. */
	(void)socket_start;
}

/*
 * Turns a WinSock or Win32 error into its POSIX name: WSAEWOULDBLOCK
 * EAGAIN (never EWOULDBLOCK: the two differ on Windows), WSAEINPROGRESS
 * EALREADY (a WinSock 1.1 blocking call under way, not a connect that did
 * not block: kl_socket_connect turns WSAEWOULDBLOCK into EINPROGRESS
 * itself), WSAECONNREFUSED ECONNREFUSED, WSAECONNRESET ECONNRESET,
 * WSAETIMEDOUT ETIMEDOUT, WSAENETUNREACH ENETUNREACH, WSAEHOSTUNREACH
 * EHOSTUNREACH, WSAEADDRINUSE EADDRINUSE, WSAEADDRNOTAVAIL EADDRNOTAVAIL,
 * WSAEAFNOSUPPORT EAFNOSUPPORT, WSAEMFILE EMFILE, WSAENOTCONN ENOTCONN,
 * WSAESHUTDOWN EPIPE, WSAENOBUFS ENOMEM, WSAEALREADY EALREADY,
 * WSAECONNABORTED ECONNABORTED, WSAENETRESET ECONNRESET, WSAENETDOWN
 * ENETDOWN, WSAEACCES EACCES, WSAEINVAL EINVAL, WSAEISCONN EISCONN,
 * WSAENOPROTOOPT ENOPROTOOPT, WSAEHOSTDOWN EHOSTUNREACH (mingw's errno.h has
 * no EHOSTDOWN), anything else EIO.
 */
static int
socket_error(
	int error)
{
	/* Not written yet. */
	(void)error;
	return EIO;
}

/* Turns an address into the system's (sockaddr_in, sockaddr_in6 with sin6_scope_id); EAFNOSUPPORT. */
static int
socket_to_system(
	const struct kl_address *address,
	struct sockaddr_storage *system,
	int *length)
{
	/* Not written yet. */
	(void)address;
	(void)system;
	(void)length;
	return ENOSYS;
}

/* Turns the system's address into the interface's; EAFNOSUPPORT for another family. */
static int
socket_from_system(
	const struct sockaddr *system,
	int length,
	struct kl_address *address)
{
	/* Not written yet. */
	(void)system;
	(void)length;
	(void)address;
	return ENOSYS;
}

/* Turns GetAddrInfoW's answer into an errno value (kl_socket_resolve's list). */
static int
socket_resolve_error(
	int answer)
{
	/* Not written yet. */
	(void)answer;
	return EIO;
}

/* Sets a handle non-blocking (1) or blocking (0) with ioctlsocket(FIONBIO); 0 or the error. */
static int
socket_set_blocking(
	SOCKET handle,
	int nonblocking)
{
	/* Not written yet. */
	(void)handle;
	(void)nonblocking;
	return ENOSYS;
}
