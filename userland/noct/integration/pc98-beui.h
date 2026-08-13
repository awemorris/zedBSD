/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NOCT_PC98_BEUI_H
#define ZEDBSD_NOCT_PC98_BEUI_H

#include <stdint.h>

int zedbsd_pc98_beui_init(uint64_t (*milliseconds)(void *),
			 int (*key_state)(void *, int),
			 void (*drain)(void *));
int zedbsd_pc98_beui_clear_graphics(void);

#endif
