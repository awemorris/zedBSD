/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_SYSCTL_H
#define ZEDBSD_KERN_SYSCTL_H

#include <stddef.h>

void sysctl_init(void);
int kern_sysctl(const int *, unsigned, void *, size_t *, const void *, size_t,
	int);

#endif

