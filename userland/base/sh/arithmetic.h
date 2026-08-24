/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_ARITHMETIC_H
#define ZEDBSD_USERLAND_SH_ARITHMETIC_H

int sh_arithmetic_eval(const char *, const char *(*)(void *, const char *),
		       void *, long *, const char **);

#endif
