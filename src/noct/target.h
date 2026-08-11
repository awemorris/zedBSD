/*
 * Boots Noct target adapters
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_NOCT_TARGET_H
#define BOOTS_NOCT_TARGET_H

typedef struct rt_env NoctEnv;

struct boots_noct_services;

int boots_noct_target_register(NoctEnv *env,
				const struct boots_noct_services *services);
void boots_noct_target_cleanup(void);

#endif
