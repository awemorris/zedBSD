/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Process virtual-memory ownership.
 */
#ifndef BOOTS_KERN_VMSPACE_H
#define BOOTS_KERN_VMSPACE_H

#include <hal/hal.h>
#include <stddef.h>
#include <stdint.h>

#define VM_USER_MIN 0x00001000U
#define VM_USER_TOP 0x80000000U

struct vm_region {
	uintptr_t start;
	size_t size;
	uint32_t prot;
	struct hal_pmem pmem;
	struct vm_region *next;
};

struct vmspace {
	hal_space_t space;
	unsigned usecount;
	struct vm_region *regions;
	uintptr_t entry;
	uintptr_t stack_bottom;
	uintptr_t stack_top;
};

extern struct vmspace kernel_vmspace;
struct vmspace *vmspace_create(void);
int vmspace_map_anon(struct vmspace *, uintptr_t, size_t, uint32_t,
		     struct vm_region **);
struct vm_region *vmspace_find_region(struct vmspace *, uintptr_t, size_t);
void vmspace_free(struct vmspace *);

#endif
