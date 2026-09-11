#ifndef KERN_HAL_ARM64_IRQ_H
#define KERN_HAL_ARM64_IRQ_H

#include <hal/types.h>
#include <hal/hal.h>

void arm64_irq_dispatch(uint32_t intid, hal_irq_ack_t acknowledge);

#endif
