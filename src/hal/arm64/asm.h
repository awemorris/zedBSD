#ifndef ZEDBSD_HAL_ARM64_ASM_H
#define ZEDBSD_HAL_ARM64_ASM_H

#include <hal/types.h>

void arm64_irq_mask(void);
void arm64_irq_unmask(void);
void arm64_wfe(void);
void arm64_wfi(void);
uint64 arm64_current_el(void);
void arm64_dsb_sy(void);
void arm64_isb(void);
uint64 arm64_irq_save(void);
void arm64_irq_restore(uint64 state);
void arm64_write_ttbr0(uintptr_t physical);
void arm64_write_ttbr1(uintptr_t physical);
void arm64_flush_tlb(void);

#endif
