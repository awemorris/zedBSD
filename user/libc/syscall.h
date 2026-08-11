/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_USER_SYSCALL_H
#define BOOTS_USER_SYSCALL_H
#include <stdint.h>
intptr_t boots_syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t,
		       uintptr_t, uintptr_t, uintptr_t);
intptr_t boots_syscall_result(intptr_t);
#endif
