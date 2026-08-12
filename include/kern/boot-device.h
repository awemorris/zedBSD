/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_BOOT_DEVICE_H
#define ZEDBSD_KERN_BOOT_DEVICE_H

int boot_device_register(void);
int kern_boot_pending(void);
void kern_boot_execute_pending(void) __attribute__((noreturn));

#endif
