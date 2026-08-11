/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USER_NOCT_API_H
#define ZEDBSD_USER_NOCT_API_H
#include <noct/beui.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rt_env NoctEnv;
typedef size_t (*zedbsd_noct_write_fn)(void *, const char *, size_t);

struct zedbsd_environment {
	uint16_t used;
	uint8_t count;
	uint8_t reserved;
	char storage[4096];
};
void zedbsd_env_init(struct zedbsd_environment *);
const char *zedbsd_env_get(const struct zedbsd_environment *, const char *);
int zedbsd_env_set(struct zedbsd_environment *, const char *, const char *);

struct zedbsd_noct_dirent {
	char name[256];
	uint64_t size;
	uint8_t attributes;
};
struct zedbsd_noct_services {
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
	int (*directory_read)(void *, const char *, unsigned, struct zedbsd_noct_dirent *);
};
struct zedbsd_noct_options {
	void *arena;
	size_t arena_size;
	size_t fail_after;
	int jit_enable;
	int jit_threshold;
	zedbsd_noct_write_fn write;
	void *write_context;
	void (*observe_jit_code)(void *, const void *, size_t);
	void *jit_context;
	const struct zedbsd_noct_services *services;
	void *filesystem;
	void *environment;
	const void *memory;
};

int zedbsd_noct_napi_register(NoctEnv *, const struct zedbsd_noct_options *);
void zedbsd_noct_napi_cleanup(void);
int zedbsd_noct_target_register(NoctEnv *, const struct zedbsd_noct_services *);
void zedbsd_noct_target_cleanup(void);
const struct zedbsd_noct_services *zedbsd_user_noct_services(void);
#endif
