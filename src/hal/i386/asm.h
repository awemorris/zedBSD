/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 assembly helpers (implemented in locore.S) and the shared
 * constants.  defs.h carries the assembler-visible half.
 */

#ifndef _SYS_ARCH_X86_ASM_H_
#define _SYS_ARCH_X86_ASM_H_

#include "defs.h"

#ifndef _ASM_SRC_
#include <hal/types.h>
void asm_cli(void);
void asm_sti(void);
void asm_outb(uint16 port, uint8 data);
void asm_outw(uint16 port, uint16 data);
uint8 asm_inb(uint16 port);
uint16 asm_inw(uint16 port);
void asm_fnsave(void *save_area);
void asm_fninit(void);
void asm_frstor(void *save_area);
uint32 asm_get_eflags(void);
uint32 asm_get_eip(void);
uint32 asm_get_esp(void);
uint32 asm_get_cr3(void);
uint32 asm_get_cr2(void);
uint32 asm_set_cr3(uint32 addr);
void asm_hlt(void);
void asm_lidt(void *idt_desc);
void asm_load_cr3(uint32 addr);
void asm_flash_tlb(void);
#endif

#endif
