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
