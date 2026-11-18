/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NETINET_TCP_H
#define LIBC_NETINET_TCP_H

/*
 * POSIX requires this header to define TCP_NODELAY, the option controlling
 * Nagle's algorithm on a stream socket, at protocol level IPPROTO_TCP.  The
 * value is shared with the kernel, so it comes from the same place the rest
 * of the protocol's constants do.
 *
 * The transport sends each write as its own segment once the previous one is
 * acknowledged, so it never holds small writes back.  setsockopt and
 * getsockopt carry the option faithfully, but clearing it does not introduce
 * the delay the name refers to, because there is no such delay to restore.
 *
 * Besides TCP_NODELAY, the header shows the vendor options the transport
 * really implements, and only those: TCP_MAXUNACKTIME (FreeBSD's name and
 * number) drops a connection that has made no progress for that many
 * seconds, and TCP_CONGESTION (FreeBSD's too) names its congestion
 * control, of at most TCP_CA_NAME_MAX - 1 bytes.  A name a program can
 * detect but not use would make it take a path that fails at run time, so
 * no other vendor name appears.
 */

#include <uapi/netinet.h>

#endif
