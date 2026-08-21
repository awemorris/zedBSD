/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USER_SYSCALL_H
#define ZEDBSD_USER_SYSCALL_H
#include <stdint.h>
intptr_t __syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t,
		       uintptr_t, uintptr_t, uintptr_t);
intptr_t syscall_result(intptr_t);
#endif
