/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD Noct target adapter
 * This will be removed after Noct is moved to userspace.
 */

#ifndef ZEDBSD_NOCT_PLATFORM_H
#define ZEDBSD_NOCT_PLATFORM_H

struct bootfs;
struct bootfs_namespace;
struct environment;
struct noct_beui_hal;
typedef int (*noct_key_fn)(void *context);
typedef int (*noct_clock_fn)(void *context);

/*
 * Installed once by the PC-98 Stage 2 target before a Noct VM is created.
 * Real-time key state and the type-ahead drain travel inside the HAL's
 * input section, which upstream BeUI owns.
 */
void noct_set_beui_hal(const struct noct_beui_hal *hal);

/* Select and reserve the temporary embedded-Noct arena before pmem use. */
int noct_prepare_memory(void);

int noct_run_embedded(unsigned repeat_count);
int noct_run_file(struct bootfs_namespace *namespace,
			 struct bootfs *filesystem,
			 struct environment *environment,
			 const char *path, int argc, char *const argv[],
			 noct_key_fn key_read,
			 noct_key_fn key_poll,
			 noct_clock_fn clock_second, void *key_context);
int noct_run_repl(struct bootfs_namespace *namespace,
			 struct bootfs *filesystem,
			 struct environment *environment,
			 noct_key_fn key_read,
			 noct_key_fn key_poll,
			 noct_clock_fn clock_second, void *key_context);

#endif
