/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 assembly and board-console contract.
 *
 * The assembly implementations live in locore.S.  The remaining board
 * interfaces are declared by their owning private headers.
 */

#ifndef SYS_HAL_I386_ASM_H
#define SYS_HAL_I386_ASM_H

#include <hal/types.h>
#include "defs.h"

/*
 * i386 Assembly Routines
 */
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
void asm_hlt(void);
void asm_lidt(void *idt_desc);
void asm_load_cr3(uint32_t addr);
void asm_flash_tlb(void);
void i386_bsp_cons_init(void);
void i386_bsp_cons_irq_init(void);

#endif
