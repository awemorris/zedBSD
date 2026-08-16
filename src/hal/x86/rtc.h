/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_X86_RTC_H
#define ZEDBSD_HAL_X86_RTC_H

#include <hal/types.h>

typedef uint8 (*x86_cmos_read_fn)(uint8 index, void *context);

bool x86_cmos_rtc_read(x86_cmos_read_fn, void *, uint64 *);

#endif
