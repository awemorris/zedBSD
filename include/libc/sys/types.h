/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

/*
 * Which user ABI these declarations describe.  A build inside this repository
 * says so on the command line, because the kernel also compiles these headers
 * and may be describing a user ABI narrower than its own.  A program compiled
 * on the machine has no such build to speak for it, and for a program the
 * answer is never in doubt: the user ABI it is part of is the one it is being
 * compiled for, which the compiler already states.
 */
#ifndef KERN_USER_ABI_LP64
#ifdef __LP64__
#define KERN_USER_ABI_LP64 1
#endif
#endif

#include <uapi/types.h>

/*
 * The type a memory address had before void * existed.  It is kept because
 * interfaces written then still name it.
 */
typedef char *caddr_t;

#endif
