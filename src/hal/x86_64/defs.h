/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private amd64 machine constants and source helpers.
 */

#ifndef KERN_HAL_AMD64_DEFS_H
#define KERN_HAL_AMD64_DEFS_H

#define PAGE_SIZE              4096
#define AMD64_IMAGE_BASE       0xffffffff80000000
#define AMD64_DIRECT_BASE      0xffff800000000000
#define AMD64_DIRECT_LIMIT     0x400000000000
#define AMD64_BOOTSTRAP_LIMIT  0x40000000
#define AMD64_ALLOCATOR_LIMIT  AMD64_DIRECT_LIMIT
#define AMD64_LEGACY_MMIO_BASE 0xffffffffc1400000

#define SEG_KERNEL_CODE        0x08
#define SEG_KERNEL_DATA        0x10
/*
 * User data comes before user code: SYSRET loads SS from STAR[63:48] + 8
 * and CS from STAR[63:48] + 16.
 */
#define SEG_USER_DATA          0x18
#define SEG_USER_CODE          0x20
#define SEG_TSS                0x28

/* Per-CPU offsets the SYSCALL entry reads through GS (see percpu.h). */
#define AMD64_PERCPU_SYSCALL_RSP     8
#define AMD64_PERCPU_SYSCALL_SCRATCH 16

#define INT_DEBUG              0x01
#define INT_PAGEFAULT          0x0e
#define INT_SYSCALL            0xc2
/*
 * The frame marker of a system call entered by the SYSCALL instruction;
 * never an IDT vector.  Its second argument is in r10, and it returns by
 * SYSRET while the marker stands.
 */
#define INT_SYSCALL_FAST       0xc3
#define AMD64_VECTOR_MSI_BASE  0xd0
#define AMD64_VECTOR_MSI_COUNT 16
#define INT_IRQ_BASE           0xe0
#define INT_UNDEF              0xffffffff

#define AMD64_PTE_PRESENT      0x001
#define AMD64_PTE_WRITE        0x002
#define AMD64_PTE_USER         0x004
#define AMD64_PTE_WRITETHRU    0x008
#define AMD64_PTE_NOCACHE      0x010
#define AMD64_PTE_ACCESSED     0x020
#define AMD64_PTE_DIRTY        0x040
#define AMD64_PTE_LARGE        0x080
#define AMD64_PTE_GLOBAL       0x100
#define AMD64_PTE_NX           0x8000000000000000
#define AMD64_PTE_ADDR_MASK    0x000ffffffffff000
/* Page-attribute-table selector bit for a 4 KiB leaf (bit 7 at PT level). */
#define AMD64_PTE_PAT_4K       0x080
/* IA32_PAT index programmed to write-combining by amd64_cpu_init(). */
#define AMD64_PAT_INDEX_WC     4

#define AMD64_MSR_IA32_PAT     0x00000277U
#define AMD64_MSR_IA32_MTRR_DEF_TYPE 0x000002ffU
#define AMD64_MSR_EFER         0xc0000080U
#define AMD64_MSR_STAR         0xc0000081U
#define AMD64_MSR_LSTAR        0xc0000082U
#define AMD64_MSR_FMASK        0xc0000084U
#define AMD64_EFER_SCE         0x00000001U
#define AMD64_MSR_FS_BASE      0xc0000100U
#define AMD64_MSR_GS_BASE      0xc0000101U

#define AMD64_SMP_MAX_CPUS     64U
#define AMD64_AP_TRAMPOLINE    0x00008000U
#define AMD64_AP_STACK_SIZE    16384U

#define AMD64_VECTOR_NOTIFY    0xf0U
#define AMD64_VECTOR_TLB       0xf1U
#define AMD64_VECTOR_ERROR     0xfeU
#define AMD64_VECTOR_SPURIOUS  0xffU

/* Marks an intentionally unused private implementation parameter. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

#endif
