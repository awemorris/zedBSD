/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private i386 per-CPU descriptor and TSS contract.
 */

#ifndef ZEDBSD_HAL_I386_PERCPU_H
#define ZEDBSD_HAL_I386_PERCPU_H

#include <hal/hal.h>

void i386_percpu_init(hal_cpu_id_t, uintptr_t);
void i386_percpu_set_kernel_stack(hal_cpu_id_t, uintptr_t);

#endif
