/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Whether the standard names are zedBSD's own or the host C library's.
 *
 * A zedBSD build of the kernel, the C library or a program is compiled for a
 * zedBSD target, whose compiler defines __ZEDBSD__.  A host fixture compiles
 * kernel source with the host compiler and the host C library, and the
 * standard names there (memcpy, EINVAL, struct stat) must stay the host's so
 * that the fixture can include the host headers beside the kernel ones.
 *
 * Any other zedBSD build -- a target whose compiler does not define
 * __ZEDBSD__, or a fixture that reads the zedBSD C library headers through
 * -I -- defines KERN_UAPI_NATIVE to ask for the zedBSD definitions.
 *
 * KERN_UAPI_HOST_LIBC is 1 when the standard names are left to the host C
 * library, and 0 when zedBSD defines them.
 */

#ifndef KERN_UAPI_HOSTED_H
#define KERN_UAPI_HOSTED_H

#if defined(__ZEDBSD__) || defined(KERN_UAPI_NATIVE)
#define KERN_UAPI_HOST_LIBC 0
#else
#define KERN_UAPI_HOST_LIBC 1
#endif

#endif
