/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network of the operating-system layer: looking up a host's
 * addresses, TCP connections, waiting for several sockets at once, and a
 * waker another thread rings to end such a wait.
 *
 * os-net-posix.c and os-net-win32.c implement it; the conventions are those
 * of os.h, and the socket functions return these values besides:
 *   EINPROGRESS   a non-blocking connect has started and not finished
 *   ECONNREFUSED, ECONNRESET, ECONNABORTED, ETIMEDOUT, ENETUNREACH,
 *   EHOSTUNREACH, ENETDOWN, EADDRNOTAVAIL, EAFNOSUPPORT
 *                 as the system reports them
 *   EPIPE         the connection can no longer send
 * On Windows the first function of this header that needs Windows Sockets
 * starts it (WSAStartup 2.2) exactly once for the process, from whichever
 * thread comes first; it is never stopped.  A failed start is returned as
 * EIO by that call and by every later one.
 */

#ifndef KEILAND_BROWSER_OS_NET_H
#define KEILAND_BROWSER_OS_NET_H

#include "base/os.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A socket or, on POSIX, any descriptor the layer waits on (the waker's
 * pipe).  It holds a POSIX descriptor or a Windows SOCKET; the engine
 * treats it as opaque apart from comparing it with WB_OS_SOCKET_NONE.
 */
typedef intptr_t wb_os_socket;

/* The value of a wb_os_socket that holds nothing. */
#define WB_OS_SOCKET_NONE	((wb_os_socket)-1)

/* What a wait asks for and what it found (struct wb_os_poll_item). */
#define WB_OS_POLL_READABLE	0x1U	/* a receive would not wait: bytes, the end, or an error */
#define WB_OS_POLL_WRITABLE	0x2U	/* a send would not wait, or a connect finished */
#define WB_OS_POLL_FAILED	0x4U	/* the system reports an error or a hang-up (found only) */

/* The most items one wb_os_poll takes. */
#define WB_OS_POLL_MAX		4096U

/*
 * The addresses of one host, in the order the system's resolver gave them
 * (a getaddrinfo list behind an opaque type).  wb_os_resolve creates it and
 * wb_os_addresses_free frees it; the connect functions only read it, so one
 * list may be read by one thread after another but is not shared between
 * threads at the same time.
 */
struct wb_os_addresses;

/*
 * One socket of a wait.  The caller fills socket and wants; wb_os_poll
 * fills found.
 */
struct wb_os_poll_item {
	wb_os_socket socket;	/* WB_OS_SOCKET_NONE: the item is skipped and found is 0 */
	unsigned wants;		/* WB_OS_POLL_READABLE and/or WB_OS_POLL_WRITABLE */
	unsigned found;		/* written by wb_os_poll: any of the three flags */
};

/*
 * A waker: a pair of connected ends.  Ringing writes one byte to one end,
 * so the other end becomes readable in a wb_os_poll; draining reads every
 * byte.  POSIX: a pipe.  Windows: two TCP sockets on the loopback address
 * (select waits only for sockets).
 */
struct wb_os_waker;

/*
 * Looks up the TCP addresses of a host.  Blocks for as long as the
 * system's resolver takes; call it from a thread that may wait.
 *
 * host:      the host name (ASCII; an IDN is already punycode) or an IPv4
 *            or IPv6 literal without brackets; not NULL, not empty.
 * port:      the port as decimal text ("443"); not NULL.
 * addresses: receives the list, or NULL on failure.
 * Hints: any family (AF_UNSPEC), SOCK_STREAM, no flags, as the engine asks
 * today.
 * Returns 0 (with at least one address), EINVAL (an argument NULL or
 * empty), ENOENT (the name has no address: EAI_NONAME, EAI_NODATA,
 * WSAHOST_NOT_FOUND, WSANO_DATA), EAGAIN (a temporary failure: EAI_AGAIN,
 * WSATRY_AGAIN), ENOMEM, EIO (any other failure).
 */
int wb_os_resolve(const char *host, const char *port, struct wb_os_addresses **addresses);

/* Reports how many addresses a list holds (0 for NULL). */
size_t wb_os_addresses_count(const struct wb_os_addresses *addresses);

/* Frees a list.  NULL does nothing. */
void wb_os_addresses_free(struct wb_os_addresses *addresses);

/*
 * Creates a TCP socket for one address of a list and starts connecting it.
 *
 * addresses: from wb_os_resolve; not NULL.
 * index:     which address, below wb_os_addresses_count.
 * blocking:  0 for a non-blocking socket (the connect returns at once and
 *            every later send and receive returns EAGAIN rather than wait),
 *            1 for a blocking one (the connect waits as long as the system
 *            lets it; sends and receives wait).
 * socket:    receives the socket; WB_OS_SOCKET_NONE on failure.
 * The socket is not inherited by child processes (POSIX close-on-exec,
 * Windows WSA_FLAG_NO_HANDLE_INHERIT) and never raises SIGPIPE.
 * Returns 0 (connected), EINPROGRESS (non-blocking only: the connect goes
 * on; *socket is valid and wb_os_poll reports WRITABLE when it ends, then
 * wb_os_socket_connect_result tells how), EINVAL (an argument NULL, index
 * out of range), ENOMEM, EMFILE, or the connect's network error (the socket
 * is then closed and *socket is WB_OS_SOCKET_NONE).
 */
int wb_os_socket_connect(const struct wb_os_addresses *addresses, size_t index, int blocking, wb_os_socket *socket);

/*
 * Reports how a non-blocking connect ended, once wb_os_poll found the
 * socket WRITABLE or FAILED.
 *
 * Returns 0 (connected), EINPROGRESS (not finished yet: a wake of the
 * wait that came early; wait for WRITABLE again), the network error
 * (ECONNREFUSED, ETIMEDOUT, ...), EINVAL (socket is WB_OS_SOCKET_NONE).
 * POSIX: the socket's SO_ERROR; an SO_ERROR of EINPROGRESS or EALREADY, or
 * an SO_ERROR of 0 with getpeername failing with ENOTCONN, is EINPROGRESS
 * (as the engine checks today).  Windows: SO_ERROR; 0 with getpeername
 * failing with WSAENOTCONN is EINPROGRESS.  A Windows non-blocking connect
 * itself fails with WSAEWOULDBLOCK, which wb_os_socket_connect reports as
 * EINPROGRESS (not the EAGAIN of wb_os_error_from_socket).
 */
int wb_os_socket_connect_result(wb_os_socket socket);

/*
 * Sends bytes on a connected socket.
 *
 * bytes:  length bytes; NULL only when length is 0.
 * length: any; a send of 0 bytes sends nothing and succeeds.  Windows
 *         sends at most INT_MAX bytes per call.
 * sent:   receives how many bytes the system took (0 on failure); fewer
 *         than length is not an error.
 * Returns 0, EAGAIN (non-blocking and nothing could be taken), EINVAL,
 * EPIPE, ECONNRESET, ECONNABORTED, ENOBUFS, EIO.  Never raises SIGPIPE
 * (POSIX: MSG_NOSIGNAL, or SO_NOSIGPIPE where that is the system's way).
 * A send cut by a signal is retried.
 */
int wb_os_socket_send(wb_os_socket socket, const void *bytes, size_t length, size_t *sent);

/*
 * Receives bytes from a connected socket.
 *
 * bytes:    room for length bytes; length more than 0.
 * received: receives how many bytes arrived; 0 with a return of 0 means the
 *           peer closed its side (the end of the stream).
 * Returns 0, EAGAIN (non-blocking and nothing has arrived), EINVAL,
 * ECONNRESET, ECONNABORTED, ETIMEDOUT, EIO.  Windows receives at most
 * INT_MAX bytes per call.  A receive cut by a signal is retried.
 */
int wb_os_socket_receive(wb_os_socket socket, void *bytes, size_t length, size_t *received);

/* Closes a socket.  WB_OS_SOCKET_NONE does nothing. */
void wb_os_socket_close(wb_os_socket socket);

/*
 * Gives the socket as the int OpenSSL's SSL_set_fd takes.
 *
 * Returns 0 and writes descriptor, EINVAL (WB_OS_SOCKET_NONE), ERANGE (the
 * value does not fit an int; a Windows SOCKET is a handle that in practice
 * does, and OpenSSL on Windows takes it this way).
 */
int wb_os_socket_descriptor(wb_os_socket socket, int *descriptor);

/*
 * Reports the calling thread's last socket error as the layer's errno
 * value, for a library (OpenSSL's SSL_ERROR_SYSCALL) that failed inside a
 * socket call the layer did not make.  POSIX: errno.  Windows:
 * WSAGetLastError mapped by wb_os_error_from_socket.  0 when there is none.
 */
int wb_os_socket_last_error(void);

/*
 * Waits until one of the items is ready or the time runs out.
 *
 * items:      count items; NULL only when count is 0.
 * count:      at most WB_OS_POLL_MAX.  Items whose socket is
 *             WB_OS_SOCKET_NONE are skipped.
 * timeout_ms: -1 waits without end, 0 does not wait, above 0 waits at most
 *             that many milliseconds.  With no item to wait for, the call
 *             sleeps for timeout_ms (-1 with nothing to wait for is EINVAL).
 * ready:      receives how many items have a found other than 0.
 * Every item's found is written: the asked flags that hold, plus FAILED
 * when the system reports an error or a hang-up on it (asked or not).  A
 * non-blocking connect that ended, well or badly, reports WRITABLE (on
 * Windows a failed one comes in select's except set and is reported as
 * WRITABLE | FAILED).  A peer's close reports READABLE.
 * Returns 0 (also when the time ran out: ready 0; and when a POSIX signal
 * cut the wait: ready 0, no retry), EINVAL (items NULL with count above 0,
 * count above WB_OS_POLL_MAX, wants with no flag on a non-NONE socket, a
 * timeout below -1), ENOMEM, EIO.
 * POSIX: poll.  Windows: select with fd_set structures allocated for the
 * count (not limited to FD_SETSIZE); WSAPoll is not used.
 */
int wb_os_poll(struct wb_os_poll_item *items, size_t count, int timeout_ms, size_t *ready);

/*
 * Creates a waker.
 *
 * waker: receives the waker, or NULL on failure.
 * Both ends are non-blocking and not inherited.  Windows: listens on
 * 127.0.0.1 port 0 with a backlog of 1, connects to it, and accepts; an
 * accepted connection whose peer address and port are not the connecting
 * socket's own local address and port (another process that connected
 * first) is closed and the accept repeated, at most 8 times (EIO after
 * that).  The listening socket is closed before returning.  TCP_NODELAY is
 * set on the ringing end.
 * Returns 0, EINVAL, ENOMEM, EMFILE, EIO.
 */
int wb_os_waker_open(struct wb_os_waker **waker);

/*
 * Rings a waker: its socket becomes readable until it is drained.  Any
 * thread; never blocks; ringing again before a drain may add nothing (when
 * the pipe or socket buffer is full the ring is already pending).  A
 * failure is ignored (there is nothing a ringing thread could do).
 */
void wb_os_waker_ring(struct wb_os_waker *waker);

/*
 * Reads every pending byte of a waker, so it is no longer readable until
 * the next ring.  Only the thread that waits for the waker calls it.
 */
void wb_os_waker_drain(struct wb_os_waker *waker);

/* Gives the end of a waker to put in a wb_os_poll_item (wants READABLE). */
wb_os_socket wb_os_waker_socket(const struct wb_os_waker *waker);

/* Closes both ends and frees the waker.  NULL does nothing. */
void wb_os_waker_close(struct wb_os_waker *waker);

#endif
