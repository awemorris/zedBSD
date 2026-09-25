/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The address of a local (AF_UNIX) socket in the zedBSD system interface.
 *
 * bind(2), connect(2) and accept(2) pass it between a program and the kernel.
 * <sys/un.h> of the C library includes this.
 */

#ifndef KERN_UAPI_UN_H
#define KERN_UAPI_UN_H

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#include <uapi/socket.h>

#define UNIX_PATH_MAX 108
struct sockaddr_un {
	sa_family_t sun_family;
	char sun_path[UNIX_PATH_MAX];
};
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the local socket address from it.
 */
#include <sys/un.h>
#ifdef LIBC_SYS_UN_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
