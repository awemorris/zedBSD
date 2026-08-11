/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_USER_NOCT_API_H
#define BOOTS_USER_NOCT_API_H
#include <noct/beui.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rt_env NoctEnv;
typedef size_t (*boots_noct_write_fn)(void *, const char *, size_t);

struct boots_environment {
	uint16_t used;
	uint8_t count;
	uint8_t reserved;
	char storage[4096];
};
void boots_env_init(struct boots_environment *);
const char *boots_env_get(const struct boots_environment *, const char *);
int boots_env_set(struct boots_environment *, const char *, const char *);

struct boots_noct_dirent {
	char name[256];
	uint64_t size;
	uint8_t attributes;
};
struct boots_noct_services {
	void *context;
	const struct noct_beui_hal *beui;
	int (*screen_clear)(void *);
	int (*screen_clear_row)(void *, unsigned);
	int (*screen_put)(void *, unsigned, unsigned, const char *, uint8_t);
	int (*screen_put_utf8)(void *, unsigned, unsigned, const char *, unsigned, uint8_t);
	int (*screen_clear_to_eol)(void *, unsigned, unsigned);
	int (*screen_set_cursor)(void *, unsigned, unsigned);
	int (*screen_show_cursor)(void *, int);
	int (*keyboard_poll)(void *);
	int (*keyboard_read)(void *);
	int (*clock_second)(void *);
	int (*file_size)(void *, const char *, uint32_t *);
	int (*file_read)(void *, const char *, uint32_t, void *, uint32_t);
	int (*directory_read)(void *, const char *, unsigned, struct boots_noct_dirent *);
};
struct boots_noct_options {
	void *arena;
	size_t arena_size;
	size_t fail_after;
	int jit_enable;
	int jit_threshold;
	boots_noct_write_fn write;
	void *write_context;
	void (*observe_jit_code)(void *, const void *, size_t);
	void *jit_context;
	const struct boots_noct_services *services;
	void *filesystem;
	void *environment;
	const void *memory;
};

int boots_noct_napi_register(NoctEnv *, const struct boots_noct_options *);
void boots_noct_napi_cleanup(void);
int boots_noct_target_register(NoctEnv *, const struct boots_noct_services *);
void boots_noct_target_cleanup(void);
const struct boots_noct_services *boots_user_noct_services(void);
#endif
