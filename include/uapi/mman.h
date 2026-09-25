/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory mapping interface of zedBSD: protections, mapping flags,
 * synchronization flags and advice.
 *
 * mmap(2), mprotect(2), msync(2) and madvise(2) pass these numbers to the
 * kernel.  <sys/mman.h> of the C library includes this and adds the
 * functions.
 */

#ifndef KERN_UAPI_MMAN_H
#define KERN_UAPI_MMAN_H

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

#define MAP_PRIVATE   0x0001
#define MAP_SHARED    0x0002
#define MAP_FIXED     0x0010
#define MAP_ANONYMOUS 0x0020
#define MAP_FIXED_NOREPLACE 0x100000
/* Accepted for compatibility; commitment is never lazy here. */
#define MAP_NORESERVE 0x4000
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      0x0001
#define MS_INVALIDATE 0x0002
#define MS_SYNC       0x0004

/*
 * Advice about how a mapping will be used.
 *
 * Every value is advice and none of it is an instruction: a system is free
 * to act on what it is told and equally free to ignore it, and a program
 * that depends on the difference is relying on something the interface does
 * not promise.  zedBSD checks what it is given and does nothing with it,
 * which is a conforming answer and is why the values a program may pass are
 * all named here.
 */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       5
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the mapping interface from it.
 */
#include <sys/mman.h>
#ifdef LIBC_SYS_MMAN_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
