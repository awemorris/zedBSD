/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD Noct target adapter
 * This will be removed after Noct is moved to userspace.
 */

#ifndef ZEDBSD_NOCT_PLATFORM_H
#define ZEDBSD_NOCT_PLATFORM_H

struct zedbsd_filesystem;
struct zedbsd_namespace;
struct zedbsd_environment;
struct noct_beui_hal;
typedef int (*zedbsd_noct_key_fn)(void *context);
typedef int (*zedbsd_noct_clock_fn)(void *context);

/*
 * Installed once by the PC-98 Stage 2 target before a Noct VM is created.
 * Real-time key state and the type-ahead drain travel inside the HAL's
 * input section, which upstream BeUI owns.
 */
void zedbsd_noct_set_beui_hal(const struct noct_beui_hal *hal);

/* Select and reserve the temporary embedded-Noct arena before pmem use. */
int zedbsd_noct_prepare_memory(void);

int zedbsd_noct_run_embedded(unsigned repeat_count);
int zedbsd_noct_run_file(struct zedbsd_namespace *namespace,
			 struct zedbsd_filesystem *filesystem,
			 struct zedbsd_environment *environment,
			 const char *path, int argc, char *const argv[],
			 zedbsd_noct_key_fn key_read,
			 zedbsd_noct_key_fn key_poll,
			 zedbsd_noct_clock_fn clock_second, void *key_context);
int zedbsd_noct_run_repl(struct zedbsd_namespace *namespace,
			 struct zedbsd_filesystem *filesystem,
			 struct zedbsd_environment *environment,
			 zedbsd_noct_key_fn key_read,
			 zedbsd_noct_key_fn key_poll,
			 zedbsd_noct_clock_fn clock_second, void *key_context);

#endif
