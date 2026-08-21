/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_MOUSE_DEVICE_H
#define ZEDBSD_KERN_MOUSE_DEVICE_H

#include <stdint.h>

int mouse_device_register(void);
int mouse_device_set_backend(int (*)(void), void (*)(void));
void mouse_input_report(uint32_t, int32_t, int32_t, uint32_t);

#endif
