/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_COMMAND_SHA256_H
#define ZEDBSD_COMMAND_SHA256_H
#include <stddef.h>
#include <stdint.h>
struct command_sha256_context {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};
void command_sha256_init(struct command_sha256_context *);
int command_sha256_update(struct command_sha256_context *, const uint8_t *, size_t);
void command_sha256_final(struct command_sha256_context *, uint8_t[32]);
#endif
