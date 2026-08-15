/*
 * Temporary ring-3 INT 0xc2 observation record.
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_USER_PROBE_H
#define ZEDBSD_KERN_USER_PROBE_H

#include <stdint.h>

#define USER_INT_PROBE_MAGIC 0x42544332U
#define USER_FAULT_PROBE_MAGIC 0x42544654U
#define USER_INT_TEST_EAX 0x49334332U

struct user_int_probe {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t raw_vector;
	/* Kept as 3 for the existing ring-3 probe wire format. */
	volatile uint32_t user_mode;
#if UINTPTR_MAX == UINT64_MAX
	volatile uint64_t pc;
	volatile uint64_t result;
#elif UINTPTR_MAX == UINT32_MAX
	volatile uint32_t pc;
	volatile uint32_t result;
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
	volatile uint32_t raw_vector;
	/* Kept as 3 for the existing ring-3 probe wire format. */
	volatile uint32_t user_mode;
#if UINTPTR_MAX == UINT64_MAX
	volatile uint64_t pc;
	volatile uint64_t status;
	volatile uint64_t fault_address;
#elif UINTPTR_MAX == UINT32_MAX
	volatile uint32_t pc;
	volatile uint32_t status;
	volatile uint32_t fault_address;
#else
#error "Unsupported pointer size for struct user_fault_probe"
#endif
	volatile int32_t pid;
	volatile int32_t tid;
};

extern volatile struct user_fault_probe user_fault_probe;
void user_probe_init(void);

#endif
