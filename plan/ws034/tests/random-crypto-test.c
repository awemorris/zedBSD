/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws034-p055 host test: ChaCha20 (RFC 8439 2.3.2) and BLAKE2s-256
 * (RFC 7693 appendix B, the empty message, and 64 and 65 bytes as
 * Python's hashlib.blake2s gives them) against published vectors.
 *   cc -I include -I . plan/ws034/tests/random-crypto-test.c \
 *       src/kern/random-crypto.c && ./a.out
 */

#include <stdio.h>
#include <string.h>

#include "kern/random-crypto.h"

static int failures;

static void
expect(const char *name, const uint8_t *got, const char *hex, size_t length)
{
	char text[2 * 64 + 1];
	size_t index;

	for (index = 0; index < length; index++)
		sprintf(text + 2 * index, "%02x", got[index]);
	if (strcmp(text, hex) != 0) {
		printf("FAIL %s\n  got  %s\n  want %s\n", name, text, hex);
		failures++;
	} else {
		printf("ok %s\n", name);
	}
}

int
main(void)
{
	static const uint8_t nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
	struct blake2s_state state;
	uint8_t key[32];
	uint8_t block[64];
	uint8_t digest[32];
	unsigned index;

	for (index = 0; index < 32; index++)
		key[index] = (uint8_t)index;
	chacha20_block(key, 1, nonce, block);
	expect("chacha20 rfc8439 2.3.2", block,
	    "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
	    "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
	    64);

	blake2s_init(&state);
	blake2s_update(&state, "abc", 3);
	blake2s_final(&state, digest);
	expect("blake2s abc", digest,
	    "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982",
	    32);

	blake2s_init(&state);
	blake2s_final(&state, digest);
	expect("blake2s empty", digest,
	    "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9",
	    32);

	/* 64 bytes exactly, then 65: the last block is not compressed early. */
	blake2s_init(&state);
	for (index = 0; index < 64; index++)
		blake2s_update(&state, "a", 1);
	blake2s_final(&state, digest);
	expect("blake2s 64 x a", digest,
	    "651d2f5f20952eacaea2fba2f2af2bcd633e511ea2d2e4c9ae2ac0d9ffb7b252",
	    32);

	blake2s_init(&state);
	for (index = 0; index < 65; index++)
		blake2s_update(&state, "a", 1);
	blake2s_final(&state, digest);
	expect("blake2s 65 x a", digest,
	    "045f8ae18932119bd051ac7ba5c73db59892055fad5c32f82d79a6543d92a497",
	    32);

	printf("RANDOM-CRYPTO %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures != 0;
}
