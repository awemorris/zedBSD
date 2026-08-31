/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland pager support.
 */

#ifndef ZEDBSD_USERLAND_PAGER_H
#define ZEDBSD_USERLAND_PAGER_H
enum pager_style { PAGER_MORE, PAGER_LESS };
int pager_main(enum pager_style, int, char **);
#endif
