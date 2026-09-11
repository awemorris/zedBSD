/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 address-space interface.
 */

#ifndef KERN_HAL_AMD64_SPACE_H
#define KERN_HAL_AMD64_SPACE_H

#include <hal/hal.h>

#define AMD64_SPACE_MAGIC 0x36435053U

struct amd64_table_page {
	hal_physaddr_t paddr;
	uint64_t *parent;
	unsigned parent_index;
	struct amd64_table_page *next;
};

struct amd64_space {
	uint32_t magic;
	int space_id;
	volatile unsigned lock;
	volatile unsigned destroying;
	unsigned active_ops;
	struct amd64_space *registry_next;
	hal_physaddr_t pml4_paddr;
	uint64_t *pml4;
	struct amd64_table_page *tables;
};

void prekern_amd64_space_init(void);
uintptr_t amd64_image_to_phys(const void *address);
int amd64_early_table_page(uint64_t *physical, int mapped);
int amd64_early_reservation(uint32_t index, uint64_t *physical, uint64_t *size);
uint64_t amd64_direct_mapped_bytes(void);
int amd64_mmio_map_ecam(paddr_t physical, size_t size, void **result);
const void *amd64_acpi_map_physical(paddr_t physical, size_t size);
uintptr_t amd64_direct_to_phys(const void *address);
void *amd64_device_vaddr(hal_physaddr_t physical);
int amd64_device_map(hal_physaddr_t physical, size_t size, void **vaddr);
void *amd64_phys_to_direct(uintptr_t address);
uintptr_t amd64_system_cr3(void);
void amd64_tlb_interrupt(void);

int amd64_acpi_page_reserved(uint64_t physical);
void amd64_acpi_finish_discovery(void);
void prekern_amd64_boot_memory_release(void);

#endif
