/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's own integer and handle types, which the kernel's public
 * headers use where they name a CPU or the machine layer's per-thread and
 * per-address-space state.  Each has the representation of the machine
 * layer's type it stands for; the kernel's private headers check that.
 */

#ifndef KERN_KERN_TYPES_H
#define KERN_KERN_TYPES_H

#include <stdint.h>

/* A CPU's number, from 0. */
typedef uint32_t kern_cpu_id_t;

/*
 * The machine layer's state of one thread (its registers and stack) and of
 * one address space (its translation tables), held by the kernel as
 * opaque handles that only the kernel's core reads.
 */
typedef void *kern_task_handle_t;
typedef void *kern_space_handle_t;

#endif
