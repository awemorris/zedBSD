/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 page-table implementation; private to the HAL.
 */

#ifndef BOOTS_HAL_I386_SPACE_H
#define BOOTS_HAL_I386_SPACE_H

#include <hal/hal.h>
#include "asm.h"

#define I386_SPACE_MAGIC 0x42535043U

struct i386_page_table {
	uintptr_t vaddr;
	struct pmem_desc memory;
	uint32 *pte;
	struct i386_page_table *next;
};

struct i386_space {
	uint32 magic;
	int space_id;
	struct pmem_desc directory_memory;
	uint32 *pdt;
	struct i386_page_table *page_tables;
};

void i386_space_init(void);

#endif
