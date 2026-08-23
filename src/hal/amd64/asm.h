/* amd64 assembly helpers. */
#ifndef ZEDBSD_HAL_AMD64_ASM_H
#define ZEDBSD_HAL_AMD64_ASM_H

#include <hal/types.h>

void asm_cli(void);
void asm_sti(void);
void asm_outb(uint16_t port, uint8_t data);
void asm_outw(uint16_t port, uint16_t data);
uint8_t asm_inb(uint16_t port);
uint16_t asm_inw(uint16_t port);
uint64_t asm_get_rflags(void);
uintptr_t asm_get_cr2(void);
uintptr_t asm_get_cr3(void);
void asm_load_cr3(uintptr_t address);
void asm_flush_tlb(void);
void asm_lidt(const void *descriptor);
void asm_hlt(void);
uint64_t asm_read_msr(uint32_t msr);
void asm_write_msr(uint32_t msr, uint64_t value);
void amd64_cpu_init(void);

#endif
