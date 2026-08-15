#ifndef ZEDBSD_HAL_ARM64_IRQ_H
#define ZEDBSD_HAL_ARM64_IRQ_H

#include <hal/types.h>
#include <hal/hal.h>

void arm64_irq_dispatch(uint32 intid, hal_irq_ack_t acknowledge);

#endif
