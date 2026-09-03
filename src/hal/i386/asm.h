/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 assembly-helper contract.
 *
 * The implementations live in locore.S and defs.h carries the
 * assembler-visible constants.
 */

#ifndef _SYS_ARCH_X86_ASM_H_
#define _SYS_ARCH_X86_ASM_H_

#include "defs.h"

#ifndef _ASM_SRC_
#include <hal/types.h>
void asm_cli(void);
void asm_sti(void);
void asm_outb(uint16_t port, uint8_t data);
void asm_outw(uint16_t port, uint16_t data);
uint8_t asm_inb(uint16_t port);
uint16_t asm_inw(uint16_t port);
void asm_fnsave(void *save_area);
void asm_frstor(void *save_area);
uint32_t asm_get_eflags(void);
uint32_t asm_get_eip(void);
uint32_t asm_get_esp(void);
uint32_t asm_get_cr3(void);
uint32_t asm_get_cr2(void);
uint32_t asm_set_cr3(uint32_t addr);
void asm_hlt(void);
void asm_lidt(void *idt_desc);
void asm_load_cr3(uint32_t addr);
void asm_flash_tlb(void);
#endif

#endif
