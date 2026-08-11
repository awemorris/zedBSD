/*
 * zedBSD persistent environment store
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_ENV_H
#define ZEDBSD_ENV_H

#include <stddef.h>
#include <stdint.h>

#define ZEDBSD_ENV_STORAGE_SIZE 4096U
#define ZEDBSD_ENV_MAX_ENTRIES 32U
#define ZEDBSD_ENV_NAME_MAX 31U
#define ZEDBSD_ENV_VALUE_MAX 255U

/*
 * Entries are stored as consecutive NAME\0VALUE\0 pairs.  The store belongs
 * to zedBSD rather than a Noct VM, so values survive script and REPL teardown.
 */
struct zedbsd_environment {
	uint16_t used;
	uint8_t count;
	uint8_t reserved;
	char storage[ZEDBSD_ENV_STORAGE_SIZE];
};

void zedbsd_env_init(struct zedbsd_environment *environment);
int zedbsd_env_name_valid(const char *name);
const char *zedbsd_env_get(const struct zedbsd_environment *environment,
			   const char *name);
int zedbsd_env_set(struct zedbsd_environment *environment, const char *name,
		   const char *value);
int zedbsd_env_unset(struct zedbsd_environment *environment, const char *name);
size_t zedbsd_env_count(const struct zedbsd_environment *environment);
int zedbsd_env_at(const struct zedbsd_environment *environment, size_t index,
		  const char **name, const char **value);

#endif
