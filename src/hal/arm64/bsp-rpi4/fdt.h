#ifndef ZEDBSD_HAL_ARM64_RPI4_FDT_H
#define ZEDBSD_HAL_ARM64_RPI4_FDT_H

#include <hal/types.h>

#define RPI4_FDT_MAX_MEMORY 8
#define RPI4_FDT_MAX_RESERVED 24

struct rpi4_fdt_range {
	uint64 base;
	uint64 size;
};

struct rpi4_fdt_info {
	uint32 totalsize;
	int compatible_rpi4;
	unsigned memory_count;
	unsigned reserved_count;
	struct rpi4_fdt_range memory[RPI4_FDT_MAX_MEMORY];
	struct rpi4_fdt_range reserved[RPI4_FDT_MAX_RESERVED];
	uint64 uart_base;
	uint32 uart_irq;
	uint64 mailbox_base;
	uint64 gic_dist_base;
	uint64 gic_cpu_base;
	uint64 sdhci_base;
	uint32 sdhci_irq;
};

int rpi4_fdt_parse(const void *blob, size_t available,
	struct rpi4_fdt_info *info);
const char *rpi4_fdt_error(int error);

#endif
