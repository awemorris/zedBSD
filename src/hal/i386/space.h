/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private i386 page-table and address-space contract.
 */

#ifndef ZEDBSD_HAL_I386_SPACE_H
#define ZEDBSD_HAL_I386_SPACE_H

#include <hal/hal.h>
#include "asm.h"

#define I386_SPACE_MAGIC 0x42535043U

struct i386_page_table {
	uintptr_t vaddr;
	struct hal_pmem memory;
	uint32_t *pte;
	struct i386_page_table *next;
};

struct i386_space {
	uint32_t magic;
	int space_id;
	unsigned active_ops;
	struct i386_space *registry_next;
	struct hal_pmem directory_memory;
	uint32_t *pdt;
	struct i386_page_table *page_tables;
	volatile unsigned lock;
	volatile unsigned destroying;
};

void i386_space_init(void);
void i386_space_init_secondary(void);
void i386_tlb_interrupt(void);

#endif
