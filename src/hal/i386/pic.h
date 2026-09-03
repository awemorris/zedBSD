/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PIC (programmable interrupt controller) management, BSP-provided:
 * the 8259 pair sits at different ports on the PC-98 and the PC/AT.
 */

#ifndef _SYS_ARCH_X86_PIC_H_
#define _SYS_ARCH_X86_PIC_H_

#include <hal/types.h>

void pic_init(void);
void pic_set_irq_mask(int irq_num, int mask);
int pic_get_irq_in_service(void);
void pic_send_eoi(int irq_num);

#endif
