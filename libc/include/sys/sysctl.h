/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_SYSCTL_H
#define ZEDBSD_SYS_SYSCTL_H

#include <stddef.h>
#include <zedbsd/sysctl.h>

int sysctl(const int *, unsigned int, void *, size_t *, const void *, size_t);
int sysctlbyname(const char *, void *, size_t *, const void *, size_t);

#endif
