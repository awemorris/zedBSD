#ifndef ZEDBSD_HAL_ARM64_RPI4_GIC_H
#define ZEDBSD_HAL_ARM64_RPI4_GIC_H

#include <hal/types.h>

void rpi4_gic_init(void);
uint32_t rpi4_gic_ack(void);
void rpi4_gic_eoi(uint32_t value);
void rpi4_gic_mask(uint32_t intid);
void rpi4_gic_unmask(uint32_t intid);

#endif
