/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Temporary ring-3 INT 0xc2 observation record.
 */
#ifndef BOOTS_KERN_USER_PROBE_H
#define BOOTS_KERN_USER_PROBE_H

#include <stdint.h>

#define USER_INT_PROBE_MAGIC 0x42544332U
#define USER_FAULT_PROBE_MAGIC 0x42544654U
#define USER_INT_TEST_EAX 0x49334332U

struct user_int_probe {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t vector;
	volatile uint32_t cs;
	volatile uint32_t eip;
	volatile uint32_t eax;
	volatile int32_t pid;
	volatile int32_t tid;
};

extern volatile struct user_int_probe user_int_probe;

struct user_fault_probe {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t vector;
	volatile uint32_t cs;
	volatile uint32_t eip;
	volatile uint32_t error_code;
	volatile uint32_t fault_address;
	volatile int32_t pid;
	volatile int32_t tid;
};

extern volatile struct user_fault_probe user_fault_probe;
void user_probe_init(void);

#endif
