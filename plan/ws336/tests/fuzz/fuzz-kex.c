/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: KEXINIT, the choice for both roles, and the hybrid messages, on any payload. */

#include "ssh-kex.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_kexinit kexinit;
	struct ssh_kex_choice choice;
	const char *kind;
	const uint8_t *a;
	const uint8_t *b;
	const uint8_t *c;
	size_t a_size;
	size_t b_size;
	size_t c_size;

	if (ssh_kexinit_parse(data, size, &kexinit) == 0) {
		(void)ssh_kex_choose(0, 1, &kexinit, &choice, &kind);
		(void)ssh_kex_choose(1, 1, &kexinit, &choice, &kind);
		(void)ssh_kex_choose(1, 0, &kexinit, &choice, &kind);
	}
	(void)ssh_kex_parse_hybrid_init(data, size, &a, &a_size);
	(void)ssh_kex_parse_hybrid_reply(data, size, &a, &a_size, &b, &b_size, &c, &c_size);
	return 0;
}
