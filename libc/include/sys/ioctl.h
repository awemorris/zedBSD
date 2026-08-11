/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_SYS_IOCTL_H
#define BOOTS_SYS_IOCTL_H

#include <stddef.h>

#define BOOTS_IOC_VOID  0x00000000UL
#define BOOTS_IOC_OUT   0x40000000UL
#define BOOTS_IOC_IN    0x80000000UL
#define BOOTS_IOC_INOUT (BOOTS_IOC_IN | BOOTS_IOC_OUT)
#define BOOTS_IOC(dir, group, nr, size) \
	((unsigned long)(dir) | (((unsigned long)(size) & 0x1fffUL) << 16) | \
	 ((unsigned long)(group) << 8) | (unsigned long)(nr))
#define _IO(g, n)       BOOTS_IOC(BOOTS_IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)   BOOTS_IOC(BOOTS_IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)   BOOTS_IOC(BOOTS_IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t)  BOOTS_IOC(BOOTS_IOC_INOUT, (g), (n), sizeof(t))

int ioctl(int descriptor, unsigned long request, ...);

#endif
