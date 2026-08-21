/*
 * zedBSD Noct target adapters
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NOCT_TARGET_H
#define ZEDBSD_NOCT_TARGET_H

typedef struct rt_env NoctEnv;

struct noct_services;

int noct_target_register(NoctEnv *env,
				const struct noct_services *services);
void noct_target_cleanup(void);

#endif
