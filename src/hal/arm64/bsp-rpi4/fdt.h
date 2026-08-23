#ifndef ZEDBSD_HAL_ARM64_RPI4_FDT_H
#define ZEDBSD_HAL_ARM64_RPI4_FDT_H

#include <hal/types.h>

#define RPI4_FDT_MAX_MEMORY 8
#define RPI4_FDT_MAX_RESERVED 24

struct rpi4_fdt_range {
	uint64_t base;
	uint64_t size;
};

struct rpi4_fdt_info {
	uint32_t totalsize;
	int compatible_rpi4;
	unsigned memory_count;
	unsigned reserved_count;
	struct rpi4_fdt_range memory[RPI4_FDT_MAX_MEMORY];
	struct rpi4_fdt_range reserved[RPI4_FDT_MAX_RESERVED];
	uint64_t uart_base;
	uint32_t uart_irq;
	uint64_t mailbox_base;
	uint64_t gic_dist_base;
	uint64_t gic_cpu_base;
	uint64_t sdhci_base;
	uint32_t sdhci_irq;
};

int rpi4_fdt_parse(const void *blob, size_t available,
	struct rpi4_fdt_info *info);
const char *rpi4_fdt_error(int error);

#endif
