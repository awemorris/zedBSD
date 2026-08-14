#ifndef ZEDBSD_HAL_AMD64_BSP_H
#define ZEDBSD_HAL_AMD64_BSP_H

void bsp_boot_init(const void *raw_boot_info);
const void *bsp_kernel_handoff(const void *raw_boot_info);
uint64 bsp_mem_probe(void);
uint32 bsp_mem_range_count(void);
int bsp_mem_range(uint32 index, uint64 *base, uint64 *size, uint32 *type);

#endif
