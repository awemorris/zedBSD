/*
 * Boots Noct target adapter
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_NOCT_PLATFORM_H
#define BOOTS_NOCT_PLATFORM_H

struct boots_filesystem;
struct boots_namespace;
struct boots_environment;
struct noct_beui_hal;
typedef int (*boots_noct_key_fn)(void *context);
typedef int (*boots_noct_clock_fn)(void *context);

/*
 * Installed once by the PC-98 Stage 2 target before a Noct VM is created.
 * Real-time key state and the type-ahead drain travel inside the HAL's
 * input section, which upstream BeUI owns.
 */
void boots_noct_set_beui_hal(const struct noct_beui_hal *hal);

int boots_noct_run_embedded(unsigned repeat_count);
int boots_noct_run_file(struct boots_namespace *namespace,
			 struct boots_filesystem *filesystem,
			 struct boots_environment *environment,
			 const char *path, int argc, char *const argv[],
			 boots_noct_key_fn key_read,
			 boots_noct_key_fn key_poll,
			 boots_noct_clock_fn clock_second, void *key_context);
int boots_noct_run_repl(struct boots_namespace *namespace,
			 struct boots_filesystem *filesystem,
			 struct boots_environment *environment,
			 boots_noct_key_fn key_read,
			 boots_noct_key_fn key_poll,
			 boots_noct_clock_fn clock_second, void *key_context);

#endif
