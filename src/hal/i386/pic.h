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

#ifndef HAL_I86_PIC_H
#define HAL_I86_PIC_H

#include <hal/types.h>

void pic_init(void);
void pic_set_irq_mask(int irq_num, int mask);
int pic_irq_in_service(int irq_num);
void pic_complete_spurious(int irq_num);
void pic_send_eoi(int irq_num);

#endif
