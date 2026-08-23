#ifndef ZEDBSD_HAL_ARM64_SPACE_H
#define ZEDBSD_HAL_ARM64_SPACE_H

#include <hal/hal.h>

#define ARM64_SPACE_MAGIC 0x41363453U

struct arm64_table_page {
	struct hal_pmem memory;
	uint64_t *parent;
	unsigned parent_index;
	struct arm64_table_page *next;
};

struct arm64_space {
	uint32_t magic;
	int space_id;
	unsigned lock;
	unsigned destroying;
	struct arm64_space *registry_next;
	struct hal_pmem l0_memory;
	uint64_t *l0;
	struct arm64_table_page *tables;
};

void arm64_page_init(void);
void arm64_space_init(void);
uintptr_t arm64_direct_to_phys(const void *address);
void *arm64_phys_to_direct(uintptr_t address);

#endif
