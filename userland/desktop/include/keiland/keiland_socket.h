/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network of a program, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_SOCKET_H
#define KEILAND_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KL_VERSION 104: a thin layer over the system's sockets (BSD sockets, or
 * WinSock), the same on every system the desktop runs on: IP addresses
 * (IPv4 and IPv6) and the names of hosts, TCP and UDP sockets, and a wait
 * for several sockets at once (kl_socket_poll) that another thread can end
 * early (a waker).
 *
 * Sockets block, or not (KL_SOCKET_NONBLOCKING), as the system's do.  The
 * application's thread does not wait for a socket: a program talks over
 * the network on a thread of its own (<keiland/keiland_thread.h>), and
 * hands what it got to the application with kl_app_post.
 *
 * A socket is used by one thread at a time, but for one thread sending
 * while another receives.  Every call may be made from any thread.  The
 * calls return 0 or an errno value (EAGAIN, never EWOULDBLOCK, for a socket
 * that cannot go on without blocking); a send to a socket the peer closed
 * returns EPIPE and never raises a signal.
 */
struct kl_socket;
struct kl_socket_waker;

/* An address's family. */
#define KL_ADDRESS_IPV4	4U
#define KL_ADDRESS_IPV6	6U

/* Either family (kl_socket_resolve). */
#define KL_ADDRESS_ANY	0U

/*
 * An IP address and a port, a value of its own (copied as it is): its
 * family (KL_ADDRESS_IPV4 or KL_ADDRESS_IPV6), its port in the host's byte
 * order, its bytes in the network's order (the first 4 of an IPv4 address,
 * all 16 of an IPv6 one), and an IPv6 address's scope (the index of the
 * interface a link-local address is on; 0 for none).
 */
struct kl_address {
	unsigned family;
	uint16_t port;
	uint8_t bytes[16];
	uint32_t scope;
};

/* The room kl_address_format needs, with its NUL ("[address%scope]:port"). */
#define KL_ADDRESS_TEXT	64U

/*
 * Reads an address written in numbers ("192.0.2.1", "2001:db8::1",
 * "fe80::1%2", without brackets) with a port.  Returns 0, or EINVAL for
 * anything else (a host's name is kl_socket_resolve's).
 */
int kl_address_parse(const char *text, uint16_t port, struct kl_address *address);

/*
 * Writes an address and its port as text ("192.0.2.1:993",
 * "[2001:db8::1]:993").  Returns 0, ENOSPC when size is too small, or
 * EAFNOSUPPORT.
 */
int kl_address_format(const struct kl_address *address, char *text, size_t size);

/* Makes the address of every interface of a family (0.0.0.0, ::) with a port. */
void kl_address_any(unsigned family, uint16_t port, struct kl_address *address);

/* Makes the loopback address of a family (127.0.0.1, ::1) with a port. */
void kl_address_loopback(unsigned family, uint16_t port, struct kl_address *address);

/* A socket's kind: a stream (TCP) or datagrams (UDP). */
#define KL_SOCKET_STREAM	1U
#define KL_SOCKET_DATAGRAM	2U

/* A socket that does not block (kl_socket_open's and kl_socket_accept's flags). */
#define KL_SOCKET_NONBLOCKING	0x1U

/*
 * Finds the addresses of a host (a name, or an address in numbers) for a
 * kind of socket and a port, in the order the system prefers them: up to
 * capacity of them, *count telling how many.  family is KL_ADDRESS_ANY or
 * one family.  It waits for the system's answer, which can take long: call
 * it from a thread of the program's network.  Returns 0, ENOENT when the
 * host has no address, EAGAIN when the answer failed for now, EINVAL,
 * EAFNOSUPPORT, ENOMEM or EIO.
 */
int kl_socket_resolve(const char *host, uint16_t port, unsigned family, unsigned kind,
    struct kl_address *addresses, size_t capacity, size_t *count);

/*
 * Opens a socket of a family and a kind (KL_SOCKET_NONBLOCKING in flags for
 * one that does not block).  It does not pass to the programs the
 * application starts.  An IPv6 socket talks IPv6 alone
 * (KL_SOCKET_IPV6_ONLY is 1).  Returns 0 with *socket set, EAFNOSUPPORT
 * (the system has no such network), EMFILE, ENFILE, ENOMEM or EACCES.
 */
int kl_socket_open(unsigned family, unsigned kind, unsigned flags, struct kl_socket **socket);

/* Makes a socket block (on 0) or not (on 1).  Returns 0. */
int kl_socket_set_nonblocking(struct kl_socket *socket, int on);

/*
 * Connects a socket to an address.  A socket that blocks returns when the
 * connection is made (0) or failed (ECONNREFUSED, ETIMEDOUT, ENETUNREACH,
 * EHOSTUNREACH...).  One that does not block returns EINPROGRESS: the
 * program waits for KL_SOCKET_WRITABLE (or KL_SOCKET_FAILED) with
 * kl_socket_poll and then asks kl_socket_connect_result; calling this again
 * meanwhile returns EALREADY.  A datagram socket's connect chooses its peer.
 */
int kl_socket_connect(struct kl_socket *socket, const struct kl_address *address);

/*
 * Tells how a connection that did not block ended, once kl_socket_poll
 * found it WRITABLE or FAILED: 0 (connected) or why it failed.
 */
int kl_socket_connect_result(struct kl_socket *socket);

/* Binds a socket to a local address.  Returns 0, EADDRINUSE, EADDRNOTAVAIL or EACCES. */
int kl_socket_bind(struct kl_socket *socket, const struct kl_address *address);

/* Listens for connections on a bound stream socket (backlog 0: the system's).  Returns 0 or an errno value. */
int kl_socket_listen(struct kl_socket *socket, unsigned backlog);

/*
 * Takes a connection a listening socket holds, as a new socket that blocks
 * or not as flags say (KL_SOCKET_NONBLOCKING), whatever the listening one
 * does; *peer (NULL: not wanted) is where it came from.  A connection that
 * went away before it was taken is passed over for the next.  Returns 0,
 * EAGAIN when none waits on a socket that does not block, EMFILE, ENFILE or
 * ENOMEM.
 */
int kl_socket_accept(struct kl_socket *socket, unsigned flags, struct kl_socket **accepted, struct kl_address *peer);

/*
 * Sends bytes on a connected socket: *sent tells how many went (fewer than
 * size is possible; the program sends the rest).  Returns 0, EAGAIN when
 * a socket that does not block has no room, EPIPE when the peer closed,
 * ECONNRESET, or another errno value.
 */
int kl_socket_send(struct kl_socket *socket, const void *bytes, size_t size, size_t *sent);

/*
 * Receives bytes from a connected socket: *received tells how many came; 0
 * of them (with 0 returned) is the end, the peer having closed its sending.
 * Returns 0, EAGAIN when nothing waits on a socket that does not block,
 * ECONNRESET, or another errno value.
 */
int kl_socket_receive(struct kl_socket *socket, void *buffer, size_t size, size_t *received);

/* Sends one datagram to an address.  Returns as kl_socket_send. */
int kl_socket_send_to(struct kl_socket *socket, const void *bytes, size_t size, const struct kl_address *to, size_t *sent);

/*
 * Receives one datagram and where it came from (*from, NULL: not wanted).
 * A datagram longer than size is cut to it; one of 0 bytes is a datagram
 * like any other.  Returns as kl_socket_receive.
 */
int kl_socket_receive_from(struct kl_socket *socket, void *buffer, size_t size, struct kl_address *from, size_t *received);

/* What kl_socket_shutdown closes: the receiving, the sending, or both. */
#define KL_SOCKET_SHUT_RECEIVE	1U
#define KL_SOCKET_SHUT_SEND	2U
#define KL_SOCKET_SHUT_BOTH	3U

/* Closes a connection's receiving or sending or both.  Returns 0, ENOTCONN or EINVAL. */
int kl_socket_shutdown(struct kl_socket *socket, unsigned how);

/* The local address a socket is bound to.  Returns 0 or an errno value. */
int kl_socket_local_address(const struct kl_socket *socket, struct kl_address *address);

/* The address a socket is connected to.  Returns 0, ENOTCONN or an errno value. */
int kl_socket_peer_address(const struct kl_socket *socket, struct kl_address *address);

/*
 * A socket's options (kl_socket_set_option): binding an address a socket
 * that closed lately still holds, sending small writes of a stream at
 * once, probing an idle stream, an IPv6 socket that talks IPv6 alone (1
 * when it opens), and datagrams to a broadcast address.
 */
#define KL_SOCKET_REUSE_ADDRESS	1U
#define KL_SOCKET_NO_DELAY	2U
#define KL_SOCKET_KEEP_ALIVE	3U
#define KL_SOCKET_IPV6_ONLY	4U
#define KL_SOCKET_BROADCAST	5U

/* Sets one of a socket's options on (1) or off (0).  Returns 0, ENOPROTOOPT or EINVAL. */
int kl_socket_set_option(struct kl_socket *socket, unsigned option, int value);

/*
 * The system's own handle of a socket (a descriptor, or a WinSock SOCKET),
 * to hand to a TLS library that reads and writes it itself.  The program
 * does not wait for it nor close it.  What the library writes through it
 * may raise SIGPIPE where the system has it: such writes run on a thread
 * started by kl_thread_start, which blocks the signal (the write then
 * returns EPIPE).
 */
intptr_t kl_socket_native(const struct kl_socket *socket);

/*
 * Closes a socket.  No other thread may be using it (waiting for it,
 * sending or receiving) meanwhile.
 */
void kl_socket_close(struct kl_socket *socket);

/*
 * What a wait waits for (events) and what it found (ready): bytes or the
 * end to receive, a connection to accept, or a waker rung (READABLE), room
 * to send or a connection that did not block ended (WRITABLE), and a socket
 * that failed (FAILED, found whatever events says; a receive or
 * kl_socket_connect_result tells why).  ready holds only what events asked
 * for, and FAILED: a connection closed both ways makes each asked one
 * ready, since a receive and a send then return at once.
 */
#define KL_SOCKET_READABLE	0x1U
#define KL_SOCKET_WRITABLE	0x2U
#define KL_SOCKET_FAILED	0x4U

/* One thing kl_socket_poll waits for: a socket, or a waker (exactly one of the two). */
struct kl_socket_wait {
	struct kl_socket *socket;
	struct kl_socket_waker *waker;
	unsigned events;
	unsigned ready;
};

/* A wait without an end (kl_socket_poll's milliseconds). */
#define KL_SOCKET_FOREVER	(-1)

/*
 * Waits until one of waits is ready, or for so many milliseconds (0: not
 * at all; KL_SOCKET_FOREVER), and sets each one's ready; *ready tells how
 * many are (0: the time ran out).  A waker that rang is ready (READABLE)
 * and quiet again: a ring before the wait returned may be merged into it,
 * a ring after it ends the next wait, so the program looks at what the
 * ringing thread left for it after the wait returns.  With no waits it
 * only sleeps.  A signal does not end the wait early.  Returns 0, EINVAL
 * (a wait with neither a socket nor a waker or with both, an event a wait
 * cannot ask for, a time below KL_SOCKET_FOREVER, no waits and no end) or
 * ENOMEM.
 */
int kl_socket_poll(struct kl_socket_wait *waits, size_t count, int milliseconds, size_t *ready);

/*
 * Opens a waker: rung from any thread, it ends the wait of the
 * kl_socket_poll it is among (or the next one).  Returns 0, EMFILE, ENFILE
 * or ENOMEM.
 */
int kl_socket_waker_open(struct kl_socket_waker **waker);

/* Rings a waker; rings before a wait finds it count as one. */
void kl_socket_waker_ring(struct kl_socket_waker *waker);

/* Closes a waker nobody waits for. */
void kl_socket_waker_close(struct kl_socket_waker *waker);

#ifdef __cplusplus
}
#endif

#endif
