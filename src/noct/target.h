/*
 * zedBSD Noct target adapters
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NOCT_TARGET_H
#define ZEDBSD_NOCT_TARGET_H

typedef struct rt_env NoctEnv;

struct zedbsd_noct_services;

int zedbsd_noct_target_register(NoctEnv *env,
				const struct zedbsd_noct_services *services);
void zedbsd_noct_target_cleanup(void);

#endif
