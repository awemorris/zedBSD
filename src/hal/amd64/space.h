/* amd64 four-level address spaces. */
#ifndef ZEDBSD_HAL_AMD64_SPACE_H
#define ZEDBSD_HAL_AMD64_SPACE_H

#include <hal/hal.h>

#define AMD64_SPACE_MAGIC 0x36435053U

struct amd64_table_page {
	struct hal_pmem memory;
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
	struct hal_pmem pml4_memory;
	uint64_t *pml4;
	struct amd64_table_page *tables;
};

void amd64_space_init(void);
int amd64_mmio_map_ecam(paddr_t physical, size_t size, void **result);
uintptr_t amd64_direct_to_phys(const void *address);
void *amd64_phys_to_direct(uintptr_t address);
uintptr_t amd64_system_cr3(void);
void amd64_tlb_interrupt(void);

#endif
