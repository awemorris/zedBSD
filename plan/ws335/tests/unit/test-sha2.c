/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks SHA-256, SHA-384 and SHA-512 (cryc-sha2.h).
 *
 * Expected values: the NIST byte-oriented SHA test vectors (short
 * messages, long messages and the Monte Carlo seeds, read from the
 * vector files), the example messages published with FIPS 180-4's
 * predecessors, and the padding rule of FIPS 180-4 5.1.1 and 5.1.2 applied
 * by hand to the length field.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-sha2.h"

/* Which of the three hashes a helper works on. */
#define KIND_SHA256	0
#define KIND_SHA384	1
#define KIND_SHA512	2

/* A computation of any of the three, so the checks are written once. */
struct any_hash {
	int kind;
	struct cryc_sha256 small;
	struct cryc_sha512 large;
};

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x13198a2eU;

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills size bytes at out with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random_next() >> 24);
}

/* Returns the digest size of kind. */
static size_t
digest_size(
	int kind)
{
	/* The three sizes. */
	if (kind == KIND_SHA256)
		return 32U;
	if (kind == KIND_SHA384)
		return 48U;
	return 64U;
}

/* Starts a computation of kind. */
static void
any_init(
	struct any_hash *hash,
	int kind)
{
	/* The structure of the kind. */
	hash->kind = kind;
	if (kind == KIND_SHA256)
		cryc_sha256_init(&hash->small);
	else if (kind == KIND_SHA384)
		cryc_sha384_init(&hash->large);
	else
		cryc_sha512_init(&hash->large);
}

/* Absorbs bytes into a computation. */
static void
any_update(
	struct any_hash *hash,
	const uint8_t *data,
	size_t size)
{
	/* The structure of the kind. */
	if (hash->kind == KIND_SHA256)
		cryc_sha256_update(&hash->small, data, size);
	else
		cryc_sha512_update(&hash->large, data, size);
}

/* Finishes a computation. */
static void
any_final(
	struct any_hash *hash,
	uint8_t *out)
{
	/* The structure of the kind. */
	if (hash->kind == KIND_SHA256)
		cryc_sha256_final(&hash->small, out);
	else
		cryc_sha512_final(&hash->large, out);
}

/* Hashes size bytes in one go. */
static void
hash_all(
	int kind,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	struct any_hash hash;

	/* Init, one update, final. */
	any_init(&hash, kind);
	any_update(&hash, data, size);
	any_final(&hash, out);
}

/* Hashes size bytes fed in pieces of piece bytes (the last one shorter). */
static void
hash_pieces(
	int kind,
	const uint8_t *data,
	size_t size,
	size_t piece,
	uint8_t *out)
{
	struct any_hash hash;
	size_t offset;
	size_t chunk;

	/* The pieces, with an empty update between them. */
	any_init(&hash, kind);
	offset = 0;
	while (offset < size) {
		chunk = size - offset;
		if (chunk > piece)
			chunk = piece;
		any_update(&hash, data + offset, chunk);
		any_update(&hash, NULL, 0);
		offset += chunk;
	}
	any_final(&hash, out);
}

/* Checks one message against its digest in one go, in pieces, and through a snapshot. */
static void
check_message(
	int kind,
	const uint8_t *message,
	size_t size,
	const uint8_t *expected)
{
	static const size_t pieces[] = { 1, 3, 17, 63, 64, 65, 127, 128, 129, 1000 };
	uint8_t got[64];
	struct any_hash hash;
	struct any_hash snapshot;
	size_t which;
	size_t half;

	/* All at once. */
	hash_all(kind, message, size, got);
	CHECK_BYTES(got, expected, digest_size(kind));

	/* In pieces of every size. */
	for (which = 0; which < sizeof(pieces) / sizeof(pieces[0]); which++) {
		if (size > 4000U && pieces[which] < 17U)
			continue;
		hash_pieces(kind, message, size, pieces[which], got);
		CHECK_BYTES(got, expected, digest_size(kind));
	}

	/* A copy taken in the middle finishes to the same digest, and so does the original. */
	half = size / 2U;
	any_init(&hash, kind);
	any_update(&hash, message, half);
	snapshot = hash;
	any_update(&hash, message + half, size - half);
	any_final(&hash, got);
	CHECK_BYTES(got, expected, digest_size(kind));
	any_update(&snapshot, message + half, size - half);
	any_final(&snapshot, got);
	CHECK_BYTES(got, expected, digest_size(kind));
}

/* Runs the short-message or long-message vector file of a hash; returns the records read. */
static unsigned long
run_message_file(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t *message;
	uint8_t expected[64];
	const char *text;
	size_t size;
	size_t length;
	unsigned long count;

	/* Every record: Len (bits), Msg (hex), MD (hex). */
	count = 0;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		text = vec_get(&file, "Len");
		if (text == NULL)
			continue;
		length = (size_t)strtoul(text, NULL, 10);
		size = length / 8U;
		message = malloc(strlen(vec_get(&file, "Msg")) / 2U + 1U);
		CHECK(message != NULL);
		if (message == NULL)
			break;
		CHECK(vec_hex(&file, "Msg", message, strlen(vec_get(&file, "Msg")) / 2U + 1U) != (size_t)-1);
		CHECK(vec_hex(&file, "MD", expected, sizeof(expected)) == digest_size(kind));
		check_message(kind, message, size, expected);
		free(message);
		count++;
	}
	vec_close(&file);
	return count;
}

/*
 * Runs the Monte Carlo vector file of a hash: from a seed, 1000 steps of
 * hashing the concatenation of the last three digests, 100 times.
 */
static unsigned long
run_monte_file(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t expected[64];
	uint8_t chain[3 * 64];
	uint8_t next[64];
	size_t size;
	size_t step;
	int have_seed;
	unsigned long count;

	/* The seed record, then the 100 checkpoints in order. */
	count = 0;
	have_seed = 0;
	size = digest_size(kind);
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		if (vec_get(&file, "Seed") != NULL) {
			CHECK(vec_hex(&file, "Seed", seed, sizeof(seed)) == size);
			have_seed = 1;
			continue;
		}
		if (!have_seed || vec_get(&file, "MD") == NULL)
			continue;
		CHECK(vec_hex(&file, "MD", expected, sizeof(expected)) == size);

		/* MD(0) = MD(1) = MD(2) = seed; MD(i) = hash(MD(i-3) | MD(i-2) | MD(i-1)). */
		memcpy(chain, seed, size);
		memcpy(chain + size, seed, size);
		memcpy(chain + 2U * size, seed, size);
		for (step = 0; step < 1000U; step++) {
			hash_all(kind, chain, 3U * size, next);
			memmove(chain, chain + size, 2U * size);
			memcpy(chain + 2U * size, next, size);
		}
		CHECK_BYTES(next, expected, size);
		memcpy(seed, next, size);
		count++;
	}
	vec_close(&file);
	return count;
}

/*
 * Checks the example messages published for the SHA-2 family: "abc", the
 * two-block message of 448 bits, and a million letters a.
 */
static void
test_examples(void)
{
	static const char abc[] = "abc";
	static const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static const char *const abc_digest[3] = {
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
		"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
	};
	static const char *const two_block_digest = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
	static const char *const million_digest[3] = {
		"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
		"9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985",
		"e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"
	};
	uint8_t expected[64];
	uint8_t got[64];
	uint8_t *million;
	int kind;

	/* "abc" and the million letters for each hash. */
	million = malloc(1000000U);
	CHECK(million != NULL);
	if (million == NULL)
		return;
	memset(million, 'a', 1000000U);
	for (kind = 0; kind < 3; kind++) {
		CHECK(hex_decode(abc_digest[kind], expected, sizeof(expected)) == digest_size(kind));
		hash_all(kind, (const uint8_t *)abc, 3U, got);
		CHECK_BYTES(got, expected, digest_size(kind));
		CHECK(hex_decode(million_digest[kind], expected, sizeof(expected)) == digest_size(kind));
		hash_all(kind, million, 1000000U, got);
		CHECK_BYTES(got, expected, digest_size(kind));
		hash_pieces(kind, million, 1000000U, 1000U, got);
		CHECK_BYTES(got, expected, digest_size(kind));
	}
	free(million);

	/* The 448-bit message (SHA-256 only: its padding needs a second block). */
	CHECK(hex_decode(two_block_digest, expected, sizeof(expected)) == 32U);
	hash_all(KIND_SHA256, (const uint8_t *)two_block, 56U, got);
	CHECK_BYTES(got, expected, 32U);
}

/* Checks the structure's fields after init and that final writes only the digest and wipes the state. */
static void
test_structure(void)
{
	static const uint8_t zeros[sizeof(struct cryc_sha512)] = { 0 };
	struct cryc_sha256 small;
	struct cryc_sha512 large;
	uint8_t out[80];
	uint8_t data[200];
	int kind;

	/* init: an empty partial block, nothing absorbed, the sizes of 384 and 512. */
	memset(&small, 0xaa, sizeof(small));
	cryc_sha256_init(&small);
	CHECK(small.used == 0 && small.total == 0);
	memset(&large, 0xaa, sizeof(large));
	cryc_sha384_init(&large);
	CHECK(large.used == 0 && large.total == 0 && large.out_size == 48U);
	memset(&large, 0xaa, sizeof(large));
	cryc_sha512_init(&large);
	CHECK(large.used == 0 && large.total == 0 && large.out_size == 64U);

	/* update counts the bytes and keeps the partial block short. */
	random_fill(data, sizeof(data));
	cryc_sha256_init(&small);
	cryc_sha256_update(&small, data, 100U);
	CHECK(small.total == 100U && small.used == 36U);
	cryc_sha512_init(&large);
	cryc_sha512_update(&large, data, 200U);
	CHECK(large.total == 200U && large.used == 72U);

	/* An absent buffer of size 0 changes nothing. */
	cryc_sha256_update(&small, NULL, 0);
	CHECK(small.total == 100U && small.used == 36U);
	cryc_sha512_update(&large, NULL, 0);
	CHECK(large.total == 200U && large.used == 72U);

	/* final writes exactly the digest and leaves the structure all zero. */
	memset(out, 0xcc, sizeof(out));
	cryc_sha256_final(&small, out);
	CHECK(out[32] == 0xcc);
	CHECK_BYTES(&small, zeros, sizeof(small));
	memset(out, 0xcc, sizeof(out));
	cryc_sha512_final(&large, out);
	CHECK(out[64] == 0xcc);
	CHECK_BYTES(&large, zeros, sizeof(large));
	cryc_sha384_init(&large);
	cryc_sha512_update(&large, data, 200U);
	memset(out, 0xcc, sizeof(out));
	cryc_sha512_final(&large, out);
	CHECK(out[48] == 0xcc);
	CHECK_BYTES(&large, zeros, sizeof(large));

	/* Reusing a structure after init gives the same digest as a fresh one. */
	for (kind = 0; kind < 3; kind++) {
		uint8_t first[64];
		uint8_t second[64];

		hash_all(kind, data, 150U, first);
		hash_all(kind, data, 150U, second);
		CHECK_BYTES(first, second, digest_size(kind));
	}
}

/*
 * Checks the length field of the padding for totals beyond 32 bits: the
 * state after final must be the state after compressing the padded last
 * block that the rule of 5.1.1 / 5.1.2 gives, built here by hand.
 */
static void
test_length_field(void)
{
	static const uint64_t blocks[] = {
		CRYC_U64(0, 0), CRYC_U64(0, 1), CRYC_U64(0, 2), CRYC_U64(0, 0x3fffffff),
		CRYC_U64(0, 0x40000000), CRYC_U64(0, 0x7fffffff), CRYC_U64(0, 0x80000000),
		CRYC_U64(0, 0xffffffff), CRYC_U64(1, 0), CRYC_U64(1, 0x23456789),
		CRYC_U64(0x1f, 0xffffffff), CRYC_U64(0x100, 0), CRYC_U64(0x3fff, 0xffffffff),
		CRYC_U64(0x00100000, 0), CRYC_U64(0x001fffff, 0xfffffffe),
		CRYC_U64(0x00200000, 0), CRYC_U64(0x003fffff, 0xfffffffe)
	};
	struct cryc_sha256 small;
	struct cryc_sha256 other;
	struct cryc_sha512 large;
	struct cryc_sha512 large_other;
	uint32_t seed_small[8];
	uint64_t seed_large[8];
	uint8_t tail[128];
	uint8_t padded[256];
	uint8_t digest[64];
	uint8_t expected[64];
	uint64_t total;
	uint64_t bits;
	size_t used;
	size_t padded_size;
	size_t which;
	size_t index;

	/* SHA-256: every partial-block fill, every total. */
	for (which = 0; which < sizeof(blocks) / sizeof(blocks[0]); which++) {
		for (used = 0; used < 64U; used++) {
			for (index = 0; index < 8U; index++)
				seed_small[index] = random_next();
			random_fill(tail, 64U);
			total = blocks[which] * 64U + used;

			/* The structure as it would be after absorbing total bytes. */
			cryc_sha256_init(&small);
			for (index = 0; index < 8U; index++)
				small.state[index] = seed_small[index];
			memcpy(small.block, tail, used);
			small.used = used;
			small.total = total;
			cryc_sha256_final(&small, digest);

			/* The padded last block(s) by hand: the tail, a 1 bit, zeros, the bit length as 64 bits. */
			memset(padded, 0, sizeof(padded));
			memcpy(padded, tail, used);
			padded[used] = 0x80;
			padded_size = used < 56U ? 64U : 128U;
			bits = total * 8U;
			for (index = 0; index < 8U; index++)
				padded[padded_size - 1U - index] = (uint8_t)(bits >> (8U * index));

			/* Compress them from the same state with a fresh structure (its total is not used). */
			cryc_sha256_init(&other);
			for (index = 0; index < 8U; index++)
				other.state[index] = seed_small[index];
			cryc_sha256_update(&other, padded, padded_size);
			for (index = 0; index < 8U; index++) {
				expected[index * 4U] = (uint8_t)(other.state[index] >> 24);
				expected[index * 4U + 1U] = (uint8_t)(other.state[index] >> 16);
				expected[index * 4U + 2U] = (uint8_t)(other.state[index] >> 8);
				expected[index * 4U + 3U] = (uint8_t)other.state[index];
			}
			CHECK_BYTES(digest, expected, 32U);
		}
	}

	/* SHA-512 (and SHA-384, the same compression): every fill, every total. */
	for (which = 0; which < sizeof(blocks) / sizeof(blocks[0]); which++) {
		for (used = 0; used < 128U; used++) {
			for (index = 0; index < 8U; index++)
				seed_large[index] = CRYC_U64(random_next(), random_next());
			random_fill(tail, 128U);
			total = blocks[which] * 128U + used;

			/* Total below 2^61 so the high half of the 128-bit length is zero. */
			if (total >= CRYC_U64(0x20000000, 0))
				continue;
			cryc_sha512_init(&large);
			for (index = 0; index < 8U; index++)
				large.state[index] = seed_large[index];
			memcpy(large.block, tail, used);
			large.used = used;
			large.total = total;
			cryc_sha512_final(&large, digest);

			memset(padded, 0, sizeof(padded));
			memcpy(padded, tail, used);
			padded[used] = 0x80;
			padded_size = used < 112U ? 128U : 256U;
			bits = total * 8U;
			for (index = 0; index < 8U; index++)
				padded[padded_size - 1U - index] = (uint8_t)(bits >> (8U * index));

			cryc_sha512_init(&large_other);
			for (index = 0; index < 8U; index++)
				large_other.state[index] = seed_large[index];
			cryc_sha512_update(&large_other, padded, padded_size);
			for (index = 0; index < 8U; index++) {
				expected[index * 8U] = (uint8_t)(large_other.state[index] >> 56);
				expected[index * 8U + 1U] = (uint8_t)(large_other.state[index] >> 48);
				expected[index * 8U + 2U] = (uint8_t)(large_other.state[index] >> 40);
				expected[index * 8U + 3U] = (uint8_t)(large_other.state[index] >> 32);
				expected[index * 8U + 4U] = (uint8_t)(large_other.state[index] >> 24);
				expected[index * 8U + 5U] = (uint8_t)(large_other.state[index] >> 16);
				expected[index * 8U + 6U] = (uint8_t)(large_other.state[index] >> 8);
				expected[index * 8U + 7U] = (uint8_t)large_other.state[index];
			}
			CHECK_BYTES(digest, expected, 64U);
		}
	}
}

/* Checks that the digest does not depend on how the input was cut, for random messages. */
static void
test_random_cuts(void)
{
	uint8_t message[1500];
	uint8_t whole[64];
	uint8_t cut[64];
	struct any_hash hash;
	size_t size;
	size_t offset;
	size_t chunk;
	int kind;
	int round;

	/* Random messages cut at random places. */
	for (round = 0; round < 300; round++) {
		size = (random_next() >> 8) % sizeof(message);
		random_fill(message, size);
		kind = round % 3;
		hash_all(kind, message, size, whole);

		any_init(&hash, kind);
		offset = 0;
		while (offset < size) {
			chunk = 1U + (random_next() >> 8) % 300U;
			if (chunk > size - offset)
				chunk = size - offset;
			any_update(&hash, message + offset, chunk);
			offset += chunk;
		}
		any_final(&hash, cut);
		CHECK_BYTES(cut, whole, digest_size(kind));
	}
}

/* Runs the checks. */
int
main(void)
{
	/* Examples, structure, padding, and random cuts. */
	test_examples();
	test_structure();
	test_length_field();
	test_random_cuts();

	/* The vector files, with the number of records each holds. */
	CHECK(run_message_file(KIND_SHA256, "cavp-shabytetestvectors-SHA256ShortMsg") == 65UL);
	CHECK(run_message_file(KIND_SHA256, "cavp-shabytetestvectors-SHA256LongMsg") == 64UL);
	CHECK(run_message_file(KIND_SHA384, "cavp-shabytetestvectors-SHA384ShortMsg") == 129UL);
	CHECK(run_message_file(KIND_SHA384, "cavp-shabytetestvectors-SHA384LongMsg") == 128UL);
	CHECK(run_message_file(KIND_SHA512, "cavp-shabytetestvectors-SHA512ShortMsg") == 129UL);
	CHECK(run_message_file(KIND_SHA512, "cavp-shabytetestvectors-SHA512LongMsg") == 128UL);
	CHECK(run_monte_file(KIND_SHA256, "cavp-shabytetestvectors-SHA256Monte") == 100UL);
	CHECK(run_monte_file(KIND_SHA384, "cavp-shabytetestvectors-SHA384Monte") == 100UL);
	CHECK(run_monte_file(KIND_SHA512, "cavp-shabytetestvectors-SHA512Monte") == 100UL);

	/* The result. */
	return check_finish("test-sha2");
}
