/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the run-time hash choice, HMAC and HKDF (cryc-hash.h).
 *
 * Expected values: the test cases of RFC 4231 section 4 and RFC 5869
 * Appendix A, the Wycheproof HMAC and HKDF vector files, and the
 * constructions of RFC 2104 section 2 written out by hand from the hash
 * functions (whose own digests are checked against the NIST vectors in
 * test-sha2).
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-hash.h"

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x6d2b79f5U;

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

/* One test case of RFC 4231 section 4: the key, the data and the MAC of each hash (hex). */
struct rfc4231_case {
	int number;
	const char *key;
	const char *data;
	const char *mac_sha256;
	const char *mac_sha384;
	const char *mac_sha512;
};

/* One test case of RFC 5869 Appendix A (the SHA-256 ones): inputs, L, and the expected PRK and OKM (hex). */
struct rfc5869_case {
	const char *ikm;
	const char *salt;
	const char *info;
	size_t length;
	const char *prk;
	const char *okm;
};

/*
 * The test cases of RFC 4231 section 4.2 to 4.8 (HMAC-SHA-224 values are
 * not used).  Case 5 has MACs truncated to 128 bits.
 */
static const struct rfc4231_case rfc4231_cases[7] = {
	{
		1,
		"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
		"4869205468657265",
		"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
		"afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
		"faea9ea9076ede7f4af152e8b2fa9cb6",
		"87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
		"daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"
	},
	{
		2,
		"4a656665",
		"7768617420646f2079612077616e7420666f72206e6f7468696e673f",
		"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
		"af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
		"8e2240ca5e69e2c78b3239ecfab21649",
		"164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
		"9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"
	},
	{
		3,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
		"dddddddddddddddddddddddddddddddddddd",
		"773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
		"88062608d3e6ad8a0aa2ace014c8a86f0aa635d947ac9febe83ef4e55966144b"
		"2a5ab39dc13814b94e3ab6e101a34f27",
		"fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
		"bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb"
	},
	{
		4,
		"0102030405060708090a0b0c0d0e0f10111213141516171819",
		"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
		"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
		"82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
		"3e8a69b7783c25851933ab6290af6ca77a9981480850009cc5577c6e1f573b4e"
		"6801dd23c4a7d679ccf8a386c674cffb",
		"b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
		"a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd"
	},
	{
		5,
		"0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c",
		"546573742057697468205472756e636174696f6e",
		"a3b6167473100ee06e0c796c2955552b",
		"3abf34c3503b2a23a46efc619baef897",
		"415fad6271580a531d4179bc891d87a6"
	},
	{
		6,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaa",
		"54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a"
		"65204b6579202d2048617368204b6579204669727374",
		"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
		"4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c6"
		"0c2ef6ab4030fe8296248df163f44952",
		"80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
		"6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"
	},
	{
		7,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaa",
		"5468697320697320612074657374207573696e672061206c6172676572207468"
		"616e20626c6f636b2d73697a65206b657920616e642061206c61726765722074"
		"68616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565"
		"647320746f20626520686173686564206265666f7265206265696e6720757365"
		"642062792074686520484d414320616c676f726974686d2e",
		"9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
		"6617178e941f020d351e2f254e8fd32c602420feb0b8fb9adccebb82461e99c5"
		"a678cc31e799176d3860e6110c46523e",
		"e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944"
		"b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58"
	}
};

/* The test cases A.1 to A.3 of RFC 5869 (the ones with SHA-256). */
static const struct rfc5869_case rfc5869_cases[3] = {
	{
		"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
		"000102030405060708090a0b0c",
		"f0f1f2f3f4f5f6f7f8f9",
		42,
		"077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
		"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
		"34007208d5b887185865"
	},
	{
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
		"202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
		"404142434445464748494a4b4c4d4e4f",
		"606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
		"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
		"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
		"b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
		"d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
		"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
		82,
		"06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
		"b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
		"59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
		"cc30c58179ec3e87c14c01d5c1f3434f1d87"
	},
	{
		"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
		"",
		"",
		42,
		"19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
		"8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
		"9d201395faa4b61a96c8"
	}
};

/* The three hashes, with their sizes written out here (32, 48 and 64 bytes; blocks 64, 128, 128). */
static const enum cryc_hash_id all_ids[3] = { CRYC_HASH_SHA256, CRYC_HASH_SHA384, CRYC_HASH_SHA512 };
static const size_t all_sizes[3] = { 32U, 48U, 64U };
static const size_t all_blocks[3] = { 64U, 128U, 128U };

/* Returns the vector-file name part of an id: "sha256", "sha384" or "sha512". */
static const char *
id_name(
	int which)
{
	/* The three names. */
	if (which == 0)
		return "sha256";
	if (which == 1)
		return "sha384";
	return "sha512";
}

/* Checks the sizes, the refusal of unknown ids, and the state after init and final. */
static void
test_hash_basics(void)
{
	static const uint8_t zeros[sizeof(struct cryc_hash)] = { 0 };
	struct cryc_hash hash;
	uint8_t out[80];
	uint8_t data[40];
	size_t which;
	int bad;

	/* The sizes of the three hashes; none for the others. */
	for (which = 0; which < 3U; which++) {
		CHECK(cryc_hash_size(all_ids[which]) == all_sizes[which]);
		CHECK(cryc_hash_block_size(all_ids[which]) == all_blocks[which]);
	}
	CHECK(cryc_hash_size(CRYC_HASH_NONE) == 0);
	CHECK(cryc_hash_block_size(CRYC_HASH_NONE) == 0);
	CHECK(CRYC_HASH_MAX_SIZE == 64U && CRYC_HASH_MAX_BLOCK == 128U);
	for (bad = 4; bad < 300; bad += 37) {
		CHECK(cryc_hash_size((enum cryc_hash_id)bad) == 0);
		CHECK(cryc_hash_block_size((enum cryc_hash_id)bad) == 0);
	}
	CHECK(cryc_hash_size((enum cryc_hash_id)-1) == 0);
	CHECK(cryc_hash_block_size((enum cryc_hash_id)-1) == 0);

	/* init refuses an unknown id, leaves a structure that names no hash, and clears it. */
	memset(&hash, 0xaa, sizeof(hash));
	CHECK(cryc_hash_init(&hash, CRYC_HASH_NONE) == ENOTSUP);
	CHECK(hash.id == CRYC_HASH_NONE);
	memset(&hash, 0xaa, sizeof(hash));
	CHECK(cryc_hash_init(&hash, (enum cryc_hash_id)77) == ENOTSUP);
	CHECK(hash.id == CRYC_HASH_NONE);
	CHECK_BYTES(&hash, zeros, sizeof(hash));

	/* A structure that names no hash absorbs and finishes without writing. */
	random_fill(data, sizeof(data));
	cryc_hash_update(&hash, data, sizeof(data));
	memset(out, 0x33, sizeof(out));
	cryc_hash_final(&hash, out);
	CHECK(out[0] == 0x33 && out[31] == 0x33 && out[63] == 0x33);
	CHECK(hash.id == CRYC_HASH_NONE);

	/* init names the hash; final wipes the whole structure and names none. */
	for (which = 0; which < 3U; which++) {
		memset(&hash, 0xaa, sizeof(hash));
		CHECK(cryc_hash_init(&hash, all_ids[which]) == 0);
		CHECK(hash.id == all_ids[which]);
		cryc_hash_update(&hash, data, sizeof(data));
		memset(out, 0x33, sizeof(out));
		cryc_hash_final(&hash, out);
		CHECK(out[all_sizes[which]] == 0x33);
		CHECK_BYTES(&hash, zeros, sizeof(hash));
		CHECK(hash.id == CRYC_HASH_NONE);
	}
}

/* Checks the run-time hash against the SHA-2 functions it is made of. */
static void
test_hash_matches_sha2(void)
{
	uint8_t message[300];
	uint8_t expected[64];
	uint8_t got[64];
	struct cryc_hash hash;
	struct cryc_hash snapshot;
	struct cryc_sha256 small;
	struct cryc_sha512 large;
	size_t size;
	size_t half;
	int round;
	int which;

	/* Random messages of every length from 0 to 299, all three hashes. */
	for (size = 0; size < sizeof(message); size++) {
		random_fill(message, size);
		for (which = 0; which < 3; which++) {
			if (which == 0) {
				cryc_sha256_init(&small);
				cryc_sha256_update(&small, message, size);
				cryc_sha256_final(&small, expected);
			} else {
				if (which == 1)
					cryc_sha384_init(&large);
				else
					cryc_sha512_init(&large);
				cryc_sha512_update(&large, message, size);
				cryc_sha512_final(&large, expected);
			}

			/* In one call. */
			memset(got, 0, sizeof(got));
			CHECK(cryc_hash_digest(all_ids[which], message, size, got) == 0);
			CHECK_BYTES(got, expected, all_sizes[which]);

			/* In steps, with a snapshot in the middle that finishes alone. */
			half = size / 3U;
			CHECK(cryc_hash_init(&hash, all_ids[which]) == 0);
			cryc_hash_update(&hash, message, half);
			snapshot = hash;
			cryc_hash_update(&hash, message + half, size - half);
			cryc_hash_final(&hash, got);
			CHECK_BYTES(got, expected, all_sizes[which]);
			cryc_hash_update(&snapshot, message + half, size - half);
			cryc_hash_final(&snapshot, got);
			CHECK_BYTES(got, expected, all_sizes[which]);
		}
	}

	/* The empty message with an absent buffer. */
	for (round = 0; round < 3; round++) {
		CHECK(cryc_hash_digest(all_ids[round], NULL, 0, got) == 0);
		CHECK(cryc_hash_digest(all_ids[round], message, 0, expected) == 0);
		CHECK_BYTES(got, expected, all_sizes[round]);
	}

	/* An unknown id is refused and the output is not written. */
	memset(got, 0x44, sizeof(got));
	CHECK(cryc_hash_digest(CRYC_HASH_NONE, message, 10U, got) == ENOTSUP);
	CHECK(cryc_hash_digest((enum cryc_hash_id)9, message, 10U, got) == ENOTSUP);
	CHECK(got[0] == 0x44 && got[63] == 0x44);
}

/* Computes HMAC by the formula of RFC 2104 section 2 with the hash functions, for a check. */
static void
reference_hmac(
	enum cryc_hash_id id,
	const uint8_t *key,
	size_t key_size,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	uint8_t block[CRYC_HASH_MAX_BLOCK];
	uint8_t inner[CRYC_HASH_MAX_SIZE];
	struct cryc_hash hash;
	size_t block_size;
	size_t index;

	/* K0: the key hashed when longer than the block, then padded with zeros to the block. */
	block_size = cryc_hash_block_size(id);
	memset(block, 0, sizeof(block));
	if (key_size > block_size)
		cryc_hash_digest(id, key, key_size, block);
	else if (key_size != 0)
		memcpy(block, key, key_size);

	/* H((K0 xor ipad) | text). */
	for (index = 0; index < block_size; index++)
		block[index] ^= 0x36;
	cryc_hash_init(&hash, id);
	cryc_hash_update(&hash, block, block_size);
	cryc_hash_update(&hash, data, size);
	cryc_hash_final(&hash, inner);

	/* H((K0 xor opad) | inner). */
	for (index = 0; index < block_size; index++)
		block[index] ^= (0x36 ^ 0x5c);
	cryc_hash_init(&hash, id);
	cryc_hash_update(&hash, block, block_size);
	cryc_hash_update(&hash, inner, cryc_hash_size(id));
	cryc_hash_final(&hash, out);
}

/* Checks one HMAC through every entry point against its expected MAC (size bytes). */
static void
check_hmac(
	enum cryc_hash_id id,
	const uint8_t *key,
	size_t key_size,
	const uint8_t *data,
	size_t size,
	const uint8_t *expected,
	size_t expected_size)
{
	uint8_t got[64];
	struct cryc_hmac hmac;
	struct cryc_hmac snapshot;
	size_t half;

	/* One call. */
	memset(got, 0, sizeof(got));
	CHECK(cryc_hmac(id, key, key_size, data, size, got) == 0);
	CHECK_BYTES(got, expected, expected_size);

	/* Init, update in two parts with a snapshot, final. */
	half = size / 2U;
	CHECK(cryc_hmac_init(&hmac, id, key, key_size) == 0);
	cryc_hmac_update(&hmac, data, half);
	snapshot = hmac;
	cryc_hmac_update(&hmac, data + half, size - half);
	cryc_hmac_final(&hmac, got);
	CHECK_BYTES(got, expected, expected_size);
	cryc_hmac_update(&snapshot, data + half, size - half);
	cryc_hmac_final(&snapshot, got);
	CHECK_BYTES(got, expected, expected_size);
}

/* Checks the test cases of RFC 4231 for the three hashes. */
static void
test_rfc4231(void)
{
	uint8_t key[200];
	uint8_t data[200];
	uint8_t expected[64];
	size_t key_size;
	size_t size;
	size_t expected_size;
	size_t which;
	size_t index;
	const char *macs[3];

	/* Each case, each hash; case 5 compares the first 128 bits only. */
	for (index = 0; index < 7U; index++) {
		key_size = hex_decode(rfc4231_cases[index].key, key, sizeof(key));
		size = hex_decode(rfc4231_cases[index].data, data, sizeof(data));
		CHECK(key_size != (size_t)-1 && size != (size_t)-1);
		macs[0] = rfc4231_cases[index].mac_sha256;
		macs[1] = rfc4231_cases[index].mac_sha384;
		macs[2] = rfc4231_cases[index].mac_sha512;
		for (which = 0; which < 3U; which++) {
			expected_size = hex_decode(macs[which], expected, sizeof(expected));
			if (rfc4231_cases[index].number == 5) {
				uint8_t got[64];

				CHECK(expected_size == 16U);
				CHECK(cryc_hmac(all_ids[which], key, key_size, data, size, got) == 0);
				CHECK_BYTES(got, expected, expected_size);
			} else {
				CHECK(expected_size == all_sizes[which]);
				check_hmac(all_ids[which], key, key_size, data, size, expected, expected_size);
			}
		}
	}
}

/* Checks the properties RFC 2104 gives: zero padding, long keys hashed, and the formula. */
static void
test_hmac_properties(void)
{
	uint8_t key[300];
	uint8_t padded[300];
	uint8_t hashed[64];
	uint8_t data[200];
	uint8_t expected[64];
	uint8_t got[64];
	struct cryc_hmac hmac;
	size_t key_size;
	size_t size;
	size_t block;
	size_t which;
	int round;

	/* Unknown ids are refused; a missing key with a length is refused. */
	memset(key, 0, sizeof(key));
	memset(data, 0, sizeof(data));
	CHECK(cryc_hmac_init(&hmac, CRYC_HASH_NONE, key, 5U) == ENOTSUP);
	CHECK(cryc_hmac_init(&hmac, (enum cryc_hash_id)8, key, 5U) == ENOTSUP);
	CHECK(cryc_hmac(CRYC_HASH_NONE, key, 5U, data, 5U, got) == ENOTSUP);
	CHECK(cryc_hmac_init(&hmac, CRYC_HASH_SHA256, NULL, 5U) == EINVAL);

	/* Every key length around and beyond the block, against the formula and the two rules. */
	for (which = 0; which < 3U; which++) {
		block = all_blocks[which];
		for (key_size = 0; key_size <= block + 3U; key_size++) {
			random_fill(key, key_size);
			size = (random_next() >> 8) % sizeof(data);
			random_fill(data, size);
			reference_hmac(all_ids[which], key, key_size, data, size, expected);
			CHECK(cryc_hmac(all_ids[which], key, key_size, data, size, got) == 0);
			CHECK_BYTES(got, expected, all_sizes[which]);

			if (key_size <= block) {
				/* A short key behaves as the key padded with zeros to the block size. */
				memset(padded, 0, block);
				memcpy(padded, key, key_size);
				CHECK(cryc_hmac(all_ids[which], padded, block, data, size, got) == 0);
				CHECK_BYTES(got, expected, all_sizes[which]);
			} else {
				/* A long key behaves as its digest. */
				CHECK(cryc_hash_digest(all_ids[which], key, key_size, hashed) == 0);
				CHECK(cryc_hmac(all_ids[which], hashed, all_sizes[which], data, size, got) == 0);
				CHECK_BYTES(got, expected, all_sizes[which]);
			}
		}

		/* An absent key of size 0 is the empty key. */
		random_fill(data, 50U);
		CHECK(cryc_hmac(all_ids[which], NULL, 0, data, 50U, got) == 0);
		memset(padded, 0, block);
		CHECK(cryc_hmac(all_ids[which], padded, block, data, 50U, expected) == 0);
		CHECK_BYTES(got, expected, all_sizes[which]);

		/* An empty message with an absent buffer. */
		CHECK(cryc_hmac(all_ids[which], key, 20U, NULL, 0, got) == 0);
		CHECK(cryc_hmac(all_ids[which], key, 20U, data, 0, expected) == 0);
		CHECK_BYTES(got, expected, all_sizes[which]);
	}

	/* Random keys and messages, fed in random pieces. */
	for (round = 0; round < 300; round++) {
		which = (size_t)(round % 3);
		key_size = (random_next() >> 8) % 260U;
		size = (random_next() >> 8) % sizeof(data);
		random_fill(key, key_size);
		random_fill(data, size);
		reference_hmac(all_ids[which], key, key_size, data, size, expected);
		check_hmac(all_ids[which], key, key_size, data, size, expected, all_sizes[which]);
	}

	/* The key is not kept: wiping it after init does not change the MAC. */
	random_fill(key, 40U);
	random_fill(data, 90U);
	reference_hmac(CRYC_HASH_SHA256, key, 40U, data, 90U, expected);
	CHECK(cryc_hmac_init(&hmac, CRYC_HASH_SHA256, key, 40U) == 0);
	memset(key, 0, 40U);
	cryc_hmac_update(&hmac, data, 90U);
	cryc_hmac_final(&hmac, got);
	CHECK_BYTES(got, expected, 32U);

	/* clear and final wipe the structure. */
	{
		static const uint8_t zeros[sizeof(struct cryc_hmac)] = { 0 };

		CHECK(cryc_hmac_init(&hmac, CRYC_HASH_SHA512, key, 40U) == 0);
		cryc_hmac_update(&hmac, data, 90U);
		cryc_hmac_clear(&hmac);
		CHECK_BYTES(&hmac, zeros, sizeof(hmac));
		CHECK(cryc_hmac_init(&hmac, CRYC_HASH_SHA384, key, 40U) == 0);
		cryc_hmac_final(&hmac, got);
		CHECK_BYTES(&hmac, zeros, sizeof(hmac));
	}

	/* A refused init leaves a structure that finishes without writing. */
	memset(got, 0x55, sizeof(got));
	CHECK(cryc_hmac_init(&hmac, CRYC_HASH_NONE, key, 3U) == ENOTSUP);
	cryc_hmac_update(&hmac, data, 10U);
	cryc_hmac_final(&hmac, got);
	CHECK(got[0] == 0x55 && got[31] == 0x55);
}

/* Checks a Wycheproof HMAC file; returns the records read. */
static unsigned long
run_wycheproof_hmac(
	int which)
{
	char name[64];
	struct vec_file file;
	uint8_t key[256];
	uint8_t message[512];
	uint8_t tag[128];
	uint8_t got[64];
	size_t key_size;
	size_t size;
	size_t tag_size;
	const char *result;
	unsigned long count;
	int matched;

	/* Every record: key, msg, tag (maybe truncated), result. */
	sprintf(name, "wycheproof-hmac_%s_test", id_name(which));
	count = 0;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		key_size = vec_hex(&file, "key", key, sizeof(key));
		size = vec_hex(&file, "msg", message, sizeof(message));
		tag_size = vec_hex(&file, "tag", tag, sizeof(tag));
		result = vec_get(&file, "result");
		CHECK(key_size != (size_t)-1 && size != (size_t)-1 && tag_size != (size_t)-1 && result != NULL);
		if (result == NULL || key_size == (size_t)-1 || size == (size_t)-1 || tag_size == (size_t)-1)
			continue;

		memset(got, 0, sizeof(got));
		CHECK(cryc_hmac(all_ids[which], key, key_size, message, size, got) == 0);

		/* A tag counts as matching when it is a prefix of the full MAC. */
		matched = tag_size != 0 && tag_size <= all_sizes[which] && memcmp(got, tag, tag_size) == 0;
		if (strcmp(result, "valid") == 0)
			CHECK(matched);
		else
			CHECK(!matched);
		count++;
	}
	vec_close(&file);
	return count;
}

/* Expands with HKDF into a buffer of size + 2 canary bytes and checks the canaries. */
static int
expand_checked(
	enum cryc_hash_id id,
	const uint8_t *prk,
	size_t prk_size,
	const uint8_t *info,
	size_t info_size,
	uint8_t *out,
	size_t out_size)
{
	int error;

	/* Canaries before and after the output. */
	memset(out - 1, 0x5a, out_size + 2U);
	error = cryc_hkdf_expand(id, prk, prk_size, info, info_size, out, out_size);
	CHECK(out[-1] == 0x5a);
	CHECK(out[out_size] == 0x5a);
	return error;
}

/* Checks a Wycheproof HKDF file; returns the records read. */
static unsigned long
run_wycheproof_hkdf(
	int which)
{
	char name[64];
	struct vec_file file;
	uint8_t ikm[256];
	uint8_t salt[256];
	uint8_t info[256];
	uint8_t expected[20000];
	uint8_t storage[20010];
	uint8_t prk[64];
	uint8_t *out;
	size_t ikm_size;
	size_t salt_size;
	size_t info_size;
	size_t expected_size;
	size_t length;
	const char *result;
	unsigned long count;
	int error;

	/* Every record: ikm, salt, info, size, okm, result. */
	sprintf(name, "wycheproof-hkdf_%s_test", id_name(which));
	count = 0;
	out = storage + 1;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		ikm_size = vec_hex(&file, "ikm", ikm, sizeof(ikm));
		salt_size = vec_hex(&file, "salt", salt, sizeof(salt));
		info_size = vec_hex(&file, "info", info, sizeof(info));
		expected_size = vec_hex(&file, "okm", expected, sizeof(expected));
		result = vec_get(&file, "result");
		length = (size_t)strtoul(vec_get(&file, "size"), NULL, 10);
		CHECK(ikm_size != (size_t)-1 && salt_size != (size_t)-1 && info_size != (size_t)-1);
		CHECK(expected_size != (size_t)-1 && result != NULL && length <= 16400U);
		if (result == NULL || ikm_size == (size_t)-1 || salt_size == (size_t)-1 || info_size == (size_t)-1)
			continue;

		CHECK(cryc_hkdf_extract(all_ids[which], salt, salt_size, ikm, ikm_size, prk) == 0);
		error = expand_checked(all_ids[which], prk, all_sizes[which], info, info_size, out, length);
		if (strcmp(result, "valid") == 0) {
			CHECK(error == 0);
			CHECK(expected_size == length);
			CHECK_BYTES(out, expected, length);
		} else {
			/* The invalid cases ask for more than 255 blocks; nothing is written. */
			CHECK(error == EINVAL);
			CHECK(length > 255U * all_sizes[which]);
		}
		count++;
	}
	vec_close(&file);
	return count;
}

/* Checks RFC 5869 Appendix A (SHA-256), step by step. */
static void
test_rfc5869(void)
{
	uint8_t ikm[100];
	uint8_t salt[100];
	uint8_t info[100];
	uint8_t prk[32];
	uint8_t expected_prk[32];
	uint8_t expected_okm[100];
	uint8_t storage[110];
	uint8_t *out;
	size_t ikm_size;
	size_t salt_size;
	size_t info_size;
	size_t okm_size;
	size_t index;

	/* Each case: extract gives the PRK, expand gives the OKM. */
	out = storage + 1;
	for (index = 0; index < 3U; index++) {
		ikm_size = hex_decode(rfc5869_cases[index].ikm, ikm, sizeof(ikm));
		salt_size = hex_decode(rfc5869_cases[index].salt, salt, sizeof(salt));
		info_size = hex_decode(rfc5869_cases[index].info, info, sizeof(info));
		okm_size = hex_decode(rfc5869_cases[index].okm, expected_okm, sizeof(expected_okm));
		CHECK(hex_decode(rfc5869_cases[index].prk, expected_prk, sizeof(expected_prk)) == 32U);
		CHECK(okm_size == rfc5869_cases[index].length);

		memset(prk, 0, sizeof(prk));
		CHECK(cryc_hkdf_extract(CRYC_HASH_SHA256, salt, salt_size, ikm, ikm_size, prk) == 0);
		CHECK_BYTES(prk, expected_prk, 32U);
		CHECK(expand_checked(CRYC_HASH_SHA256, prk, 32U, info, info_size, out, okm_size) == 0);
		CHECK_BYTES(out, expected_okm, okm_size);
	}

	/* Case 3: the absent salt, the salt of size 0 and a salt of HashLen zeros all agree. */
	ikm_size = hex_decode(rfc5869_cases[2].ikm, ikm, sizeof(ikm));
	CHECK(hex_decode(rfc5869_cases[2].prk, expected_prk, sizeof(expected_prk)) == 32U);
	memset(salt, 0, 32U);
	CHECK(cryc_hkdf_extract(CRYC_HASH_SHA256, NULL, 0, ikm, ikm_size, prk) == 0);
	CHECK_BYTES(prk, expected_prk, 32U);
	CHECK(cryc_hkdf_extract(CRYC_HASH_SHA256, salt, 0, ikm, ikm_size, prk) == 0);
	CHECK_BYTES(prk, expected_prk, 32U);
	CHECK(cryc_hkdf_extract(CRYC_HASH_SHA256, salt, 32U, ikm, ikm_size, prk) == 0);
	CHECK_BYTES(prk, expected_prk, 32U);
}

/* Checks HKDF against the definition of RFC 5869 section 2, and its error values. */
static void
test_hkdf_properties(void)
{
	uint8_t prk[200];
	uint8_t info[100];
	uint8_t salt[200];
	uint8_t ikm[100];
	uint8_t out[20100];
	uint8_t longer[20100];
	uint8_t message[200];
	uint8_t block_one[64];
	uint8_t block_two[64];
	uint8_t canary[20100];
	size_t digest;
	size_t which;
	size_t length;
	size_t info_size;
	size_t prk_size;
	size_t index;
	int round;

	/* Each hash. */
	for (which = 0; which < 3U; which++) {
		digest = all_sizes[which];
		random_fill(prk, sizeof(prk));
		random_fill(info, sizeof(info));
		info_size = 37U;

		/* T(1) = HMAC(PRK, info | 0x01); T(2) = HMAC(PRK, T(1) | info | 0x02). */
		memcpy(message, info, info_size);
		message[info_size] = 1;
		CHECK(cryc_hmac(all_ids[which], prk, digest, message, info_size + 1U, block_one) == 0);
		memcpy(message, block_one, digest);
		memcpy(message + digest, info, info_size);
		message[digest + info_size] = 2;
		CHECK(cryc_hmac(all_ids[which], prk, digest, message, digest + info_size + 1U, block_two) == 0);

		/* A full output of 255 blocks is allowed; one byte more is refused and not written. */
		memset(out, 0x5a, sizeof(out));
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, out, 255U * digest) == 0);
		CHECK_BYTES(out, block_one, digest);
		CHECK_BYTES(out + digest, block_two, digest);
		CHECK(out[255U * digest] == 0x5a);
		memset(canary, 0x5a, sizeof(canary));
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, canary, 255U * digest + 1U) == EINVAL);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, canary, (size_t)-1) == EINVAL);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, canary, (size_t)1 << (sizeof(size_t) * 8U - 1U)) == EINVAL);
		for (index = 0; index < sizeof(canary); index++)
			if (canary[index] != 0x5a)
				break;
		CHECK(index == sizeof(canary));

		/* A shorter output is a prefix of a longer one, for every length from 0 to 3 blocks + 1. */
		for (length = 0; length <= 3U * digest + 1U; length++) {
			memset(longer, 0x5a, 3U * digest + 3U);
			CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, longer + 1, length) == 0);
			CHECK(longer[0] == 0x5a && longer[length + 1U] == 0x5a);
			CHECK_BYTES(longer + 1, out, length);
		}

		/* A PRK longer than HashLen is used whole; one shorter is refused. */
		prk_size = digest + 17U;
		CHECK(cryc_hkdf_expand(all_ids[which], prk, prk_size, info, info_size, longer, 70U) == 0);
		CHECK(cryc_hmac(all_ids[which], prk, prk_size, message, 0, block_one) == 0);
		memcpy(message, info, info_size);
		message[info_size] = 1;
		CHECK(cryc_hmac(all_ids[which], prk, prk_size, message, info_size + 1U, block_one) == 0);
		CHECK_BYTES(longer, block_one, digest);
		memset(canary, 0x5a, 100U);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest - 1U, info, info_size, canary, 10U) == EINVAL);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, 0, info, info_size, canary, 10U) == EINVAL);
		CHECK(cryc_hkdf_expand(all_ids[which], NULL, 0, info, info_size, canary, 10U) == EINVAL);
		CHECK(canary[0] == 0x5a && canary[9] == 0x5a);

		/* An empty output, with an absent buffer, is allowed and writes nothing. */
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, info_size, NULL, 0) == 0);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, NULL, 0, canary, 0) == 0);
		CHECK(canary[0] == 0x5a);

		/* An absent info of size 0 equals an empty info. */
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, NULL, 0, out, 70U) == 0);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, info, 0, longer, 70U) == 0);
		CHECK_BYTES(out, longer, 70U);
		CHECK(cryc_hkdf_expand(all_ids[which], prk, digest, NULL, 3U, longer, 70U) == EINVAL);

		/* Extract is HMAC with the salt as the key (the zero salt when absent). */
		random_fill(salt, sizeof(salt));
		random_fill(ikm, sizeof(ikm));
		for (length = 0; length <= 140U; length += 7U) {
			CHECK(cryc_hkdf_extract(all_ids[which], salt, length, ikm, 60U, block_one) == 0);
			if (length == 0) {
				memset(prk, 0, digest);
				CHECK(cryc_hmac(all_ids[which], prk, digest, ikm, 60U, block_two) == 0);
			} else {
				CHECK(cryc_hmac(all_ids[which], salt, length, ikm, 60U, block_two) == 0);
			}
			CHECK_BYTES(block_one, block_two, digest);
		}
		CHECK(cryc_hkdf_extract(all_ids[which], NULL, 5U, ikm, 60U, block_one) == 0);
		memset(prk, 0, digest);
		CHECK(cryc_hmac(all_ids[which], prk, digest, ikm, 60U, block_two) == 0);
		CHECK_BYTES(block_one, block_two, digest);
		CHECK(cryc_hkdf_extract(all_ids[which], salt, 20U, NULL, 0, block_one) == 0);
		CHECK(cryc_hmac(all_ids[which], salt, 20U, NULL, 0, block_two) == 0);
		CHECK_BYTES(block_one, block_two, digest);
		CHECK(cryc_hkdf_extract(all_ids[which], salt, 20U, NULL, 4U, block_one) == EINVAL);
	}

	/* Unknown ids are refused by both. */
	CHECK(cryc_hkdf_extract(CRYC_HASH_NONE, salt, 4U, ikm, 4U, block_one) == ENOTSUP);
	CHECK(cryc_hkdf_extract((enum cryc_hash_id)5, salt, 4U, ikm, 4U, block_one) == ENOTSUP);
	CHECK(cryc_hkdf_expand(CRYC_HASH_NONE, prk, 32U, info, 4U, out, 10U) == ENOTSUP);
	CHECK(cryc_hkdf_expand((enum cryc_hash_id)5, prk, 32U, info, 4U, out, 10U) == ENOTSUP);

	/* Random sizes and a random pointer-less call mix never fault (the sanitizers watch). */
	for (round = 0; round < 200; round++) {
		which = (size_t)(random_next() >> 8) % 3U;
		length = (random_next() >> 8) % 400U;
		prk_size = (random_next() >> 8) % 200U;
		info_size = (random_next() >> 8) % 100U;
		CHECK(cryc_hkdf_expand(all_ids[which], prk, prk_size, info, info_size, out, length) ==
		    (prk_size < all_sizes[which] ? EINVAL : (length > 255U * all_sizes[which] ? EINVAL : 0)));
	}
}

/* Runs the checks. */
int
main(void)
{
	int which;

	/* The run-time hash. */
	test_hash_basics();
	test_hash_matches_sha2();

	/* HMAC: RFC 4231, the properties of RFC 2104, and the Wycheproof files (174 records each). */
	test_rfc4231();
	test_hmac_properties();
	for (which = 0; which < 3; which++)
		CHECK(run_wycheproof_hmac(which) == 174UL);

	/* HKDF: RFC 5869, the definition, and the Wycheproof files. */
	test_rfc5869();
	test_hkdf_properties();
	CHECK(run_wycheproof_hkdf(0) == 86UL);
	CHECK(run_wycheproof_hkdf(1) == 83UL);
	CHECK(run_wycheproof_hkdf(2) == 83UL);

	/* The result. */
	return check_finish("test-hash");
}
