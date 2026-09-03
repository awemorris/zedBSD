/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board-provided i386 programmable interrupt-controller contract.
 *
 * The 8259 pair uses different ports on PC-98 and PC/AT systems.
 */

#ifndef _SYS_ARCH_X86_PIC_H_
#define _SYS_ARCH_X86_PIC_H_

#include <hal/types.h>

void pic_init(void);
void pic_set_irq_mask(int irq_num, int mask);
int pic_get_irq_in_service(void);
void pic_send_eoi(int irq_num);

#endif
