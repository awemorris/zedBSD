/*
 * Boots persistent environment store
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_ENV_H
#define BOOTS_ENV_H

#include <stddef.h>
#include <stdint.h>

#define BOOTS_ENV_STORAGE_SIZE 4096U
#define BOOTS_ENV_MAX_ENTRIES 32U
#define BOOTS_ENV_NAME_MAX 31U
#define BOOTS_ENV_VALUE_MAX 255U

/*
 * Entries are stored as consecutive NAME\0VALUE\0 pairs.  The store belongs
 * to Boots rather than a Noct VM, so values survive script and REPL teardown.
 */
struct boots_environment {
	uint16_t used;
	uint8_t count;
	uint8_t reserved;
	char storage[BOOTS_ENV_STORAGE_SIZE];
};

void boots_env_init(struct boots_environment *environment);
int boots_env_name_valid(const char *name);
const char *boots_env_get(const struct boots_environment *environment,
			   const char *name);
int boots_env_set(struct boots_environment *environment, const char *name,
		   const char *value);
int boots_env_unset(struct boots_environment *environment, const char *name);
size_t boots_env_count(const struct boots_environment *environment);
int boots_env_at(const struct boots_environment *environment, size_t index,
		  const char **name, const char **value);

#endif
