/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Temporary ring-3 INT 0xc2 observation record.
 */

#ifndef ZEDBSD_KERN_USER_PROBE_H
#define ZEDBSD_KERN_USER_PROBE_H

#include <stdint.h>

#define USER_INT_PROBE_MAGIC	0x42544332U
#define USER_FAULT_PROBE_MAGIC	0x42544654U
#define USER_INT_TEST_EAX	0x49334332U

struct user_int_probe {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t vector;
	volatile uint32_t cs;
#if UINTPTR_MAX == UINT64_MAX
	volatile uint64_t eip;
	volatile uint64_t eax;
#elif UINTPTR_MAX == UINT32_MAX
	volatile uint32_t eip;
	volatile uint32_t eax;
#else
#error "Unsupported pointer size for struct user_int_probe"
#endif
	volatile int32_t pid;
	volatile int32_t tid;
};

extern volatile struct user_int_probe user_int_probe;

struct user_fault_probe {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t vector;
	volatile uint32_t cs;
#if UINTPTR_MAX == UINT64_MAX
	volatile uint64_t eip;
	volatile uint64_t error_code;
	volatile uint64_t fault_address;
#elif UINTPTR_MAX == UINT32_MAX
	volatile uint32_t eip;
	volatile uint32_t error_code;
	volatile uint32_t fault_address;
#else
# error "Unsupported pointer size for struct user_fault_probe"
#endif
	volatile int32_t pid;
	volatile int32_t tid;
};

extern volatile struct user_fault_probe user_fault_probe;

void
user_probe_init(void);

#endif
