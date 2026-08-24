/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_IOCTL_H
#define ZEDBSD_SYS_IOCTL_H

#include <zedbsd/features.h>
#include <stddef.h>

#define ZEDBSD_IOC_VOID  0x00000000UL
#define ZEDBSD_IOC_OUT   0x40000000UL
#define ZEDBSD_IOC_IN    0x80000000UL
#define ZEDBSD_IOC_INOUT (ZEDBSD_IOC_IN | ZEDBSD_IOC_OUT)
#define ZEDBSD_IOC(dir, group, nr, size) \
	((unsigned long)(dir) | (((unsigned long)(size) & 0x1fffUL) << 16) | \
	 ((unsigned long)(group) << 8) | (unsigned long)(nr))
#define _IO(g, n)       ZEDBSD_IOC(ZEDBSD_IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)   ZEDBSD_IOC(ZEDBSD_IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)   ZEDBSD_IOC(ZEDBSD_IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t)  ZEDBSD_IOC(ZEDBSD_IOC_INOUT, (g), (n), sizeof(t))

#if __ZEDBSD_LEGACY_VISIBLE
int ioctl(int descriptor, unsigned long request, ...);
#endif

#endif
