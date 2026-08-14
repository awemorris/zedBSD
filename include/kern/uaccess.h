/*
 * uaccess
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_UACCESS_H
#define ZEDBSD_KERN_UACCESS_H

#include <stddef.h>
#include <stdint.h>

int user_range_check(uintptr_t, size_t, uint32_t);
int copyin(uintptr_t, void *, size_t);
int copyout(const void *, uintptr_t, size_t);
int copyinstr(uintptr_t, char *, size_t, size_t *);

#endif
