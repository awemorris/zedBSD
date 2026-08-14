/* amd64 four-level address spaces. */
#ifndef ZEDBSD_HAL_AMD64_SPACE_H
#define ZEDBSD_HAL_AMD64_SPACE_H

#include <hal/hal.h>

#define AMD64_SPACE_MAGIC 0x36435053U

struct amd64_table_page {
	struct pmem_desc memory;
	uint64 *parent;
	unsigned parent_index;
	struct amd64_table_page *next;
};

struct amd64_space {
	uint32 magic;
	int space_id;
	struct pmem_desc pml4_memory;
	uint64 *pml4;
	struct amd64_table_page *tables;
};

void amd64_space_init(void);
uintptr_t amd64_direct_to_phys(const void *address);
void *amd64_phys_to_direct(uintptr_t address);

#endif
