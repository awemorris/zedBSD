/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USER_NOCT_API_H
#define ZEDBSD_USER_NOCT_API_H
#include <noct/beui.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rt_env NoctEnv;
typedef size_t (*noct_write_fn)(void *, const char *, size_t);

#define ZEDBSD_ENV_STORAGE_SIZE 4096U
#define ZEDBSD_ENV_MAX_ENTRIES 32U
#define ZEDBSD_ENV_NAME_MAX 31U
#define ZEDBSD_ENV_VALUE_MAX 255U
#define ZEDBSD_NOCT_DIRECTORY_MAX 256U
#define ZEDBSD_NOCT_PATH_MAX 256U
#define ZEDBSD_NOCT_SOURCE_MAX (256U * 1024U)

struct environment {
	uint16_t used;
	uint8_t count;
	uint8_t reserved;
	char storage[ZEDBSD_ENV_STORAGE_SIZE];
};
void env_init(struct environment *);
int env_name_valid(const char *);
const char *env_get(const struct environment *, const char *);
int env_set(struct environment *, const char *, const char *);
int env_unset(struct environment *, const char *);
size_t env_count(const struct environment *);
int env_at(const struct environment *, size_t, const char **, const char **);

struct noct_dirent {
	char name[ZEDBSD_NOCT_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};
struct noct_services {
	void *context;
	const struct noct_beui_hal *beui;
	int (*screen_clear)(void *);
	int (*screen_clear_row)(void *, unsigned);
	int (*screen_put)(void *, unsigned, unsigned, const char *, uint8_t);
	int (*screen_put_utf8)(void *, unsigned, unsigned, const char *,
			       unsigned, uint8_t);
	int (*screen_clear_to_eol)(void *, unsigned, unsigned);
	int (*screen_set_cursor)(void *, unsigned, unsigned);
	int (*screen_show_cursor)(void *, int);
	int (*keyboard_poll)(void *);
	int (*keyboard_read)(void *);
	int (*clock_second)(void *);
	int (*file_size)(void *, const char *, uint32_t *);
	int (*file_read)(void *, const char *, uint32_t, void *, uint32_t);
	int (*directory_read)(void *, const char *, unsigned,
			      struct noct_dirent *);
};
struct noct_options {
	void *arena;
	size_t arena_size;
	size_t fail_after;
	int jit_enable;
	int jit_threshold;
	noct_write_fn write;
	void *write_context;
	void (*observe_jit_code)(void *, const void *, size_t);
	void *jit_context;
	const struct noct_services *services;
	void *filesystem;
	void *environment;
};

int key_normalize_bios_ax(uint16_t);
int noct_napi_register(NoctEnv *, const struct noct_options *);
void noct_napi_cleanup(void);
int noct_target_register(NoctEnv *, const struct noct_services *);
void noct_target_cleanup(void);
const struct noct_services *user_noct_services(void);
#endif
