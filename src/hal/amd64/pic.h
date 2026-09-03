/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 legacy programmable interrupt controller contract.
 */

#ifndef ZEDBSD_HAL_AMD64_PIC_H
#define ZEDBSD_HAL_AMD64_PIC_H

void pic_init(void);
void pic_set_irq_mask(int irq_num, int mask);
int pic_get_irq_in_service(void);
void pic_send_eoi(int irq_num);

#endif
