/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_USYNC_H
#define ZEDBSD_KERN_USYNC_H
#include <stdint.h>
void usync_init(void);
int usync_wait(uintptr_t, uint32_t, uintptr_t, uintptr_t, uint64_t, int);
int usync_wake(uintptr_t, uintptr_t, uintptr_t, unsigned);
#endif
