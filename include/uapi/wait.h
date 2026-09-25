/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wait interface of zedBSD: how a child's status is encoded, the options
 * of the wait calls and the kinds of identifier waitid(2) selects by.
 *
 * The kernel encodes the status and reads the options.  <sys/wait.h> of the
 * C library includes this and adds the functions.
 */

#ifndef KERN_UAPI_WAIT_H
#define KERN_UAPI_WAIT_H

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) ((((status) & 0x7f) != 0) && (((status) & 0x7f) != 0x7f))
#define WTERMSIG(status) ((status) & 0x7f)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status) (((status) >> 8) & 0xff)
#define WIFCONTINUED(status) ((status) == 0xffff)

#define WNOHANG 0x0001
#define WUNTRACED 0x0002
#define WCONTINUED 0x0004
#define WEXITED 0x0008
#define WSTOPPED WUNTRACED
#define WNOWAIT 0x0010

typedef enum {
	P_ALL = 0,
	P_PID = 1,
	P_PGID = 2
} idtype_t;
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the wait interface from it.
 */
#include <sys/wait.h>
#ifdef LIBC_SYS_WAIT_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
