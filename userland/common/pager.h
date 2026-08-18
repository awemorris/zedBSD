/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_PAGER_H
#define ZEDBSD_USERLAND_PAGER_H
enum pager_style { PAGER_MORE, PAGER_LESS };
int pager_main(enum pager_style, int, char **);
#endif
