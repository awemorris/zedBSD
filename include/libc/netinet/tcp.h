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
 * Only the names POSIX lists appear here.  Vendor-specific option names are
 * deliberately absent: a name a program can detect but not use would make it
 * take a path that fails at run time.
 */

#include <uapi/netinet.h>

#endif
