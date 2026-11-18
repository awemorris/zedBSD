/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How an ioctl request number is built.
 *
 * The number carries the direction of the transfer, the size of the argument,
 * a group letter and the request within the group.  The kernel decodes the
 * size and direction from it, so the encoding is part of the system interface
 * and every uapi header that declares a request builds it with these macros.
 */

#ifndef KERN_UAPI_IOCTL_H
#define KERN_UAPI_IOCTL_H

#include <uapi/hosted.h>

#define KERN_IOC_VOID  0x00000000UL
#define KERN_IOC_OUT   0x40000000UL
#define KERN_IOC_IN    0x80000000UL
#define KERN_IOC_INOUT (KERN_IOC_IN | KERN_IOC_OUT)
#define KERN_IOC(dir, group, nr, size) \
	((unsigned long)(dir) | (((unsigned long)(size) & 0x1fffUL) << 16) | \
	 ((unsigned long)(group) << 8) | (unsigned long)(nr))

#if !KERN_UAPI_HOST_LIBC
#define _IO(g, n)       KERN_IOC(KERN_IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)   KERN_IOC(KERN_IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)   KERN_IOC(KERN_IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t)  KERN_IOC(KERN_IOC_INOUT, (g), (n), sizeof(t))

/*
 * Sets or clears non-blocking I/O on an open file description, like
 * fcntl(F_SETFL) with O_NONBLOCK.  The argument points to an int: nonzero
 * sets it.  It is not POSIX, but every Unix has it and portable software
 * (OpenSSL among it) uses it for sockets.  The kernel answers it for every
 * kind of file.
 */
#define FIONBIO		_IOW('f', 126, int)
#else

/*
 * A host fixture builds the request numbers with the host's own macros, so
 * that a request it hands to the host's ioctl means what the host expects.
 */
#include <sys/ioctl.h>
#ifdef LIBC_SYS_IOCTL_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
