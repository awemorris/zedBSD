/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD C library syscall interface.
 */

#ifndef ZEDBSD_USER_SYSCALL_H
#define ZEDBSD_USER_SYSCALL_H
#include <stdint.h>
intptr_t __syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
		    uintptr_t, uintptr_t);
intptr_t syscall_result(intptr_t);
#endif
