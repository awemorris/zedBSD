/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland rtld interface.
 */

#ifndef KERN_RTLD_H
#define KERN_RTLD_H

#include "src/rtld/elf.h"
#include <stddef.h>
#include <stdint.h>

#ifndef KERN_USER_PAGE_SIZE
#define KERN_USER_PAGE_SIZE 4096U
#endif
#define RTLD_PAGE_SIZE KERN_USER_PAGE_SIZE
#define RTLD_PATH_MAX 256U
/*
 * The tables of the loaded objects grow as far as memory allows (WS140):
 * these are only the sizes kept without a mapping of their own, the
 * objects in one chunk of the object table, and the dependencies, program
 * headers and TLSDESC arguments one object holds inside itself.
 */
#define RTLD_OBJECT_CHUNK 32U
#define RTLD_NEEDED_INLINE 16U
#define RTLD_PROGRAM_INLINE 16U
#define RTLD_TLSDESC_INLINE 16U
#define RTLD_INTERP_PATH "/lib/ld.so"

intptr_t rtld_syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
		       uintptr_t, uintptr_t);
uintptr_t rtld_main(uintptr_t *initial_stack);

size_t rtld_strlen(const char *);
int rtld_strcmp(const char *, const char *);
void *rtld_memcpy(void *, const void *, size_t);
void *rtld_memset(void *, int, size_t);
void rtld_fatal(const char *) __attribute__((noreturn));
void rtld_debug(const char *);

#endif
