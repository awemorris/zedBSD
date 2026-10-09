/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_SOCKET_H
#define LIBC_SYS_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/socket.h>

/* POSIX: <sys/socket.h> defines struct iovec as <sys/uio.h> does. */
#include <sys/uio.h>

/*
 * The largest backlog listen() is worth asking for.  It is the same value as
 * the kernel's unix stream listener limit (UNIX_LISTEN_BACKLOG_MAX in
 * src/kern/net/unix-socket.c); a TCP listener keeps fewer and silently trims
 * a larger request, as listen() is allowed to.
 */
#define SOMAXCONN 128

/*
 * The socket type, message flags and socket options POSIX names that the
 * kernel does not implement.  They are defined here rather than in
 * <uapi/socket.h> because the kernel has no meaning for them.  Their values
 * come from the free part of the kernel's number space (the message flags
 * follow the Linux values the existing flags use, the options the BSD values),
 * so that a program asking for one is refused rather than given something
 * else:
 *
 * - socket() and socketpair() refuse SOCK_SEQPACKET with EINVAL,
 * - send(), sendto() and sendmsg() refuse MSG_OOB, MSG_DONTROUTE and MSG_EOR,
 *   and recv(), recvfrom() and recvmsg() refuse MSG_OOB, with EOPNOTSUPP,
 * - setsockopt() and getsockopt() refuse the options with ENOPROTOOPT (an
 *   internet datagram or ICMP socket reports EOPNOTSUPP).
 *
 * If the kernel takes one of them up, the definition moves to <uapi/socket.h>
 * with the same value.
 */
#define SOCK_SEQPACKET	5
#define MSG_OOB	0x0001
#define MSG_DONTROUTE	0x0004
#define MSG_EOR	0x0080
#define SO_DEBUG	0x0001
#define SO_ACCEPTCONN	0x0002
#define SO_DONTROUTE	0x0010
#define SO_LINGER	0x0080
#define SO_OOBINLINE	0x0100
#define SO_SNDLOWAT	0x1003
#define SO_RCVLOWAT	0x1004

/*
 * The value of the SO_LINGER option: whether close() waits for data not yet
 * sent, and for how many seconds.
 */
struct linger {
	int l_onoff;
	int l_linger;
};

/* The standard macro evaluates its message pointer once through a bounded helper. */
#define CMSG_FIRSTHDR(message) __libc_cmsg_firsthdr(message)

/* Return the first ancillary header only when the supplied control buffer contains it. */
static __inline struct cmsghdr *
__libc_cmsg_firsthdr(
	const struct msghdr *message)
{
	/* A truncated ancillary buffer contains no complete first message header. */
	if (message->msg_controllen < sizeof(struct cmsghdr))
		return (struct cmsghdr *)0;

	/* Succeeded: the caller owns the complete first control header, if any. */
	return (struct cmsghdr *)message->msg_control;
}

/* The standard macro steps through the control buffer through a bounded helper. */
#define CMSG_NXTHDR(message, header) __libc_cmsg_nxthdr(message, header)

/*
 * Return the ancillary header after the given one, only when the control
 * buffer holds all of it.
 *
 * The next header starts at the current one's aligned length.  A current
 * header outside the buffer or shorter than its own fixed part, a step that
 * runs past the end, and a next header whose fixed part or recorded length
 * does not fit in what is left all end the walk with a null pointer.  A null
 * current header asks for the first one, as the BSD and glibc versions do.
 */
static __inline struct cmsghdr *
__libc_cmsg_nxthdr(
	const struct msghdr *message,
	const struct cmsghdr *header)
{
	const unsigned char *start;
	const unsigned char *position;
	const struct cmsghdr *next;
	size_t offset;
	size_t remaining;
	size_t step;

	/* A walk that has not started begins at the first header. */
	if (header == (const struct cmsghdr *)0)
		return __libc_cmsg_firsthdr(message);

	/* Locates the current header inside the control buffer. */
	start = (const unsigned char *)message->msg_control;
	position = (const unsigned char *)header;
	if (position < start || position >= start + message->msg_controllen)
		return (struct cmsghdr *)0;
	offset = (size_t)(position - start);
	remaining = message->msg_controllen - offset;

	/* A header shorter than its fixed part, or longer than the buffer, has no next. */
	if (header->cmsg_len < sizeof(struct cmsghdr) || header->cmsg_len > remaining)
		return (struct cmsghdr *)0;

	/* The step to the next header, which alignment may carry to the end or beyond. */
	step = CMSG_ALIGN(header->cmsg_len);
	if (step >= remaining)
		return (struct cmsghdr *)0;
	remaining -= step;

	/* The next header's fixed part has to fit in what is left. */
	if (remaining < sizeof(struct cmsghdr))
		return (struct cmsghdr *)0;

	/* So does the length the next header records for itself. */
	next = (const struct cmsghdr *)(position + step);
	if (next->cmsg_len > remaining)
		return (struct cmsghdr *)0;

	/* Succeeded: the caller owns the complete next control header. */
	return (struct cmsghdr *)next;
}

#ifdef __cplusplus
}
#endif

#endif
