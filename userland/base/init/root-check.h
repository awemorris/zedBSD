/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of a read-only root by init (root-check.c, BUG-312).
 */

#ifndef USERLAND_BASE_INIT_ROOT_CHECK_H
#define USERLAND_BASE_INIT_ROOT_CHECK_H

/* The checker init runs on a read-only root, and the exit codes it answers with. */
#define ROOT_CHECK_FSCK_PATH		"/sbin/fsck"
#define ROOT_CHECK_FSCK_CLEAN		0
#define ROOT_CHECK_FSCK_REPAIRED	4

/* Checks the root when the kernel mounted it read-only; reboots after a repair. */
void root_check(void);

#endif /* USERLAND_BASE_INIT_ROOT_CHECK_H */
