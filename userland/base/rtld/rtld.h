/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_RTLD_H
#define ZEDBSD_RTLD_H

#include "userland/base/rtld/elf.h"
#include <stddef.h>
#include <stdint.h>

#ifndef ZEDBSD_USER_PAGE_SIZE
#define ZEDBSD_USER_PAGE_SIZE 4096U
#endif
#define RTLD_PAGE_SIZE ZEDBSD_USER_PAGE_SIZE
#define RTLD_PATH_MAX 256U
#define RTLD_NAME_MAX 64U
#define RTLD_OBJECT_MAX 32U
#define RTLD_NEEDED_MAX 16U
#define RTLD_INTERP_PATH "/lib/ld.so"

intptr_t rtld_syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t,
	uintptr_t, uintptr_t, uintptr_t);
uintptr_t rtld_main(uintptr_t *initial_stack);

size_t rtld_strlen(const char *);
int rtld_strcmp(const char *, const char *);
void *rtld_memcpy(void *, const void *, size_t);
void *rtld_memset(void *, int, size_t);
void rtld_fatal(const char *) __attribute__((noreturn));
void rtld_debug(const char *);

#endif
