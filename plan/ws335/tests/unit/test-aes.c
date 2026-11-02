/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks AES and its counter mode (cryc-aes.h).
 *
 * Expected values: the S-box of FIPS 197 Table 4 (all 256 entries, read
 * back through the key expansion, which applies the S-box to chosen
 * bytes), the key expansions of FIPS 197 Appendix A (all three key
 * sizes), the cipher example of Appendix B, the NIST CAVP known-answer
 * files of AES in ECB mode (GFSbox, KeySbox, VarKey and VarTxt for 128,
 * 192 and 256 bits), and a reference cipher written from the text of
 * FIPS 197 (the bytewise Algorithm 1 and Algorithm 2 on the state of 3.4
 * with the S-box taken from Table 4) that is compared with the
 * implementation on pseudo-random keys and blocks.  The counter mode is
 * checked against GCTR (SP 800-38D 6.5) built from the single-block
 * cipher with the counter incremented bytewise, including the wrap of the
 * 32-bit counter, and against the ciphertext of the Wycheproof AES-GCM
 * vectors (the GCM keystream starts at the counter IV || 00000002).
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-aes.h"

/* The sizes of the buffers that hold decoded vectors. */
#define VECTOR_BYTES		4096U

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE		0x5aU

/*
 * The S-box.
 *
 * Values referenced: FIPS 197 (May 2023 update) Table 4, the byte at row
 * x, column y is the entry 16 * x + y; checked against the text of the
 * table and against the page image, and against the definition of 5.1.1
 * (the multiplicative inverse followed by the affine transformation (5.3)).
 */
static const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/*
 * The key of the first key-expansion example, and the words w[4] to w[43]
 * the expansion makes.
 *
 * Values referenced: FIPS 197 (May 2023 update) Appendix A.1, Nk = 4 (the
 * last column of the table, w[i]).
 */
static const char a1_key[] = "2b7e151628aed2a6abf7158809cf4f3c";
static const uint32_t a1_words[40] = {
	0xa0fafe17U, 0x88542cb1U, 0x23a33939U, 0x2a6c7605U, 0xf2c295f2U, 0x7a96b943U, 0x5935807aU, 0x7359f67fU,
	0x3d80477dU, 0x4716fe3eU, 0x1e237e44U, 0x6d7a883bU, 0xef44a541U, 0xa8525b7fU, 0xb671253bU, 0xdb0bad00U,
	0xd4d1c6f8U, 0x7c839d87U, 0xcaf2b8bcU, 0x11f915bcU, 0x6d88a37aU, 0x110b3efdU, 0xdbf98641U, 0xca0093fdU,
	0x4e54f70eU, 0x5f5fc9f3U, 0x84a64fb2U, 0x4ea6dc4fU, 0xead27321U, 0xb58dbad2U, 0x312bf560U, 0x7f8d292fU,
	0xac7766f3U, 0x19fadc21U, 0x28d12941U, 0x575c006eU, 0xd014f9a8U, 0xc9ee2589U, 0xe13f0cc8U, 0xb6630ca6U
};

/*
 * The key of the second example and the words w[6] to w[51].
 *
 * Values referenced: FIPS 197 (May 2023 update) Appendix A.2, Nk = 6.
 */
static const char a2_key[] = "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b";
static const uint32_t a2_words[46] = {
	0xfe0c91f7U, 0x2402f5a5U, 0xec12068eU, 0x6c827f6bU, 0x0e7a95b9U, 0x5c56fec2U, 0x4db7b4bdU, 0x69b54118U,
	0x85a74796U, 0xe92538fdU, 0xe75fad44U, 0xbb095386U, 0x485af057U, 0x21efb14fU, 0xa448f6d9U, 0x4d6dce24U,
	0xaa326360U, 0x113b30e6U, 0xa25e7ed5U, 0x83b1cf9aU, 0x27f93943U, 0x6a94f767U, 0xc0a69407U, 0xd19da4e1U,
	0xec1786ebU, 0x6fa64971U, 0x485f7032U, 0x22cb8755U, 0xe26d1352U, 0x33f0b7b3U, 0x40beeb28U, 0x2f18a259U,
	0x6747d26bU, 0x458c553eU, 0xa7e1466cU, 0x9411f1dfU, 0x821f750aU, 0xad07d753U, 0xca400538U, 0x8fcc5006U,
	0x282d166aU, 0xbc3ce7b5U, 0xe98ba06fU, 0x448c773cU, 0x8ecc7204U, 0x01002202U
};

/*
 * The key of the third example and the words w[8] to w[59].
 *
 * Values referenced: FIPS 197 (May 2023 update) Appendix A.3, Nk = 8.
 */
static const char a3_key[] = "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4";
static const uint32_t a3_words[52] = {
	0x9ba35411U, 0x8e6925afU, 0xa51a8b5fU, 0x2067fcdeU, 0xa8b09c1aU, 0x93d194cdU, 0xbe49846eU, 0xb75d5b9aU,
	0xd59aecb8U, 0x5bf3c917U, 0xfee94248U, 0xde8ebe96U, 0xb5a9328aU, 0x2678a647U, 0x98312229U, 0x2f6c79b3U,
	0x812c81adU, 0xdadf48baU, 0x24360af2U, 0xfab8b464U, 0x98c5bfc9U, 0xbebd198eU, 0x268c3ba7U, 0x09e04214U,
	0x68007bacU, 0xb2df3316U, 0x96e939e4U, 0x6c518d80U, 0xc814e204U, 0x76a9fb8aU, 0x5025c02dU, 0x59c58239U,
	0xde136967U, 0x6ccc5a71U, 0xfa256395U, 0x9674ee15U, 0x5886ca5dU, 0x2e2f31d7U, 0x7e0af1faU, 0x27cf73c3U,
	0x749c47abU, 0x18501ddaU, 0xe2757e4fU, 0x7401905aU, 0xcafaaae3U, 0xe4d59b34U, 0x9adf6aceU, 0xbd10190dU,
	0xfe4890d1U, 0xe6188d0bU, 0x046df344U, 0x706c631eU
};

/*
 * The cipher example: input, key and output.
 *
 * Values referenced: FIPS 197 (May 2023 update) Appendix B, the input
 * and the key at the top, the output at the bottom of the table (the
 * state is written column by column).
 */
static const char b_input[] = "3243f6a8885a308d313198a2e0370734";
static const char b_key[] = "2b7e151628aed2a6abf7158809cf4f3c";
static const char b_output[] = "3925841d02dc09fbdc118597196a0b32";

/* The state of a pseudo-random generator (xorshift64, Marsaglia 2003). */
static uint64_t random_state = CRYC_U64(0x2545f491U, 0x4f6cdd1dU);

/* The reference cipher: the bytewise Algorithm 1 and 2 with the S-box of Table 4. */
struct reference {
	uint8_t round_key[15][16];
	unsigned int rounds;
};

/* Returns the next pseudo-random byte. */
static uint8_t random_byte(void);

/* Fills size bytes with pseudo-random bytes. */
static void random_fill(uint8_t *out, size_t size);

/* Reads byte n of round key k out of the slices of the schedule (the layout is the one described at the top of cryc-aes.c). */
static uint8_t round_key_byte(const struct cryc_aes *aes, unsigned int round, unsigned int byte);

/* Checks that the round key is the same in all four lanes of every slice. */
static int round_key_in_four_lanes(const struct cryc_aes *aes, unsigned int round);

/* Reads word w of the expansion out of round key w / 4. */
static uint32_t schedule_word(const struct cryc_aes *aes, unsigned int word);

/* Checks the whole expansion of a key against the words of Appendix A. */
static void check_expansion(const char *key_hex, size_t key_size, const uint32_t *words, size_t word_count, unsigned int rounds);

/* Checks the 256 entries of the S-box through the key expansion of AES-128 (the first SubWord of the first round key). */
static void check_sbox(void);

/* Multiplies by {02} in GF(2^8) (4.5, XTIMES). */
static uint8_t reference_xtime(uint8_t value);

/* Expands a key (5.2, Algorithm 2) bytewise. */
static void reference_expand(struct reference *reference, const uint8_t *key, size_t key_size);

/* Encrypts one block bytewise (5.1, Algorithm 1). */
static void reference_encrypt(const struct reference *reference, const uint8_t *in, uint8_t *out);

/* Encrypts the blocks of one pseudo-random message with the reference and with the implementation and compares. */
static void check_random_blocks(size_t key_size, size_t blocks);

/* Reads one CAVP known-answer file and checks every record (the encryption direction of each). */
static void check_cavp(const char *name);

/* Adds one to the last four bytes of a counter block, bytewise, as a big-endian integer (6.2, inc32). */
static void reference_inc32(uint8_t *block);

/* The reference GCTR (6.5): block by block with the single-block cipher. */
static void reference_gctr(const struct cryc_aes *aes, uint8_t *counter, const uint8_t *in, uint8_t *out, size_t size);

/* Checks the counter mode on one size, one key size and one initial counter. */
static void check_ctr_one(size_t key_size, size_t size, uint32_t low_counter, int in_place);

/* Checks the counter mode on many sizes and counters, including the wrap of the 32-bit counter. */
static void check_ctr(void);

/* Checks the counter mode against the ciphertext of the Wycheproof AES-GCM vectors with a 96-bit IV. */
static void check_gcm_keystream(void);

/* Checks that init refuses every key size but 16, 24 and 32, and leaves the structure wiped. */
static void check_init_errors(void);

/* Checks that the encryption does not change the key and that count 0 touches nothing. */
static void check_const_and_zero(void);

/* Returns the next pseudo-random byte. */
static uint8_t
random_byte(void)
{
	/* One xorshift step, then one byte of the state. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (uint8_t)(random_state >> 24);
}

/* Fills size bytes with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = random_byte();
}

/* Reads byte n of round key k out of the slices of the schedule (the layout is the one described at the top of cryc-aes.c). */
static uint8_t
round_key_byte(
	const struct cryc_aes *aes,
	unsigned int round,
	unsigned int byte)
{
	unsigned int value;
	unsigned int bit;

	/* Bit i of the byte is bit n of slice i of the round key. */
	value = 0;
	for (bit = 0; bit < 8U; bit++)
		value |= (unsigned int)((aes->schedule[round * 8U + bit] >> byte) & 1U) << bit;
	return (uint8_t)value;
}

/* Checks that the round key is the same in all four lanes of every slice. */
static int
round_key_in_four_lanes(
	const struct cryc_aes *aes,
	unsigned int round)
{
	uint64_t slice;
	uint64_t repeater;
	unsigned int bit;

	/* A lane is 16 bits and the other three lanes must repeat it. */
	repeater = CRYC_U64(0x00010001U, 0x00010001U);
	for (bit = 0; bit < 8U; bit++) {
		slice = aes->schedule[round * 8U + bit];
		if ((slice & 0xffffU) * repeater != slice)
			return 0;
	}

	/* Every slice repeated its first lane. */
	return 1;
}

/* Reads word w of the expansion out of round key w / 4. */
static uint32_t
schedule_word(
	const struct cryc_aes *aes,
	unsigned int word)
{
	uint8_t bytes[4];
	unsigned int part;

	/* The four bytes of the word are bytes 4 * (w mod 4) to 4 * (w mod 4) + 3 of the round key. */
	for (part = 0; part < 4U; part++)
		bytes[part] = round_key_byte(aes, word / 4U, 4U * (word % 4U) + part);
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Checks the whole expansion of a key against the words of Appendix A. */
static void
check_expansion(
	const char *key_hex,
	size_t key_size,
	const uint32_t *words,
	size_t word_count,
	unsigned int rounds)
{
	struct cryc_aes aes;
	uint8_t key[32];
	unsigned int word;
	unsigned int key_words;
	uint32_t expected;
	int lanes;
	int in_lanes;
	unsigned int round;

	/* The key is expanded. */
	CHECK(hex_decode(key_hex, key, sizeof(key)) == key_size);
	CHECK(cryc_aes_init(&aes, key, key_size) == 0);
	CHECK(aes.rounds == rounds);

	/* The first words are the key. */
	key_words = (unsigned int)(key_size / 4U);
	for (word = 0; word < key_words; word++) {
		expected = ((uint32_t)key[4U * word] << 24) | ((uint32_t)key[4U * word + 1U] << 16) |
		    ((uint32_t)key[4U * word + 2U] << 8) | (uint32_t)key[4U * word + 3U];
		CHECK(schedule_word(&aes, word) == expected);
	}

	/* The words the table lists. */
	for (word = 0; word < word_count; word++)
		CHECK(schedule_word(&aes, key_words + word) == words[word]);
	CHECK(key_words + word_count == 4U * (rounds + 1U));

	/* Every round key is in all four lanes. */
	lanes = 1;
	for (round = 0; round <= rounds; round++) {
		in_lanes = round_key_in_four_lanes(&aes, round);
		if (!in_lanes)
			lanes = 0;
	}

	/* Records that every round key was in all four lanes. */
	CHECK(lanes);

	/* The slices after the last round key are zero. */
	for (word = (rounds + 1U) * 8U; word < CRYC_AES_SCHEDULE_WORDS; word++)
		CHECK(aes.schedule[word] == 0);
}

/* Checks the 256 entries of the S-box through the key expansion of AES-128 (the first SubWord of the first round key). */
static void
check_sbox(void)
{
	struct cryc_aes aes;
	uint8_t key[16];
	unsigned int base;
	unsigned int part;
	unsigned int rotated;

	/*
	 * With w[0], w[1] and w[2] zero and w[3] = (b0, b1, b2, b3), the word w[4] is
	 * SubWord(RotWord(w[3])) xor Rcon[1] = (S(b1) ^ 01, S(b2), S(b3), S(b0)): four S-box entries per key,
	 * and the 64 keys with b_j = 4 * base + j cover every byte value once.
	 */
	for (base = 0; base < 64U; base++) {
		memset(key, 0, sizeof(key));
		for (part = 0; part < 4U; part++)
			key[12U + part] = (uint8_t)(4U * base + part);
		CHECK(cryc_aes_init(&aes, key, sizeof(key)) == 0);

		/* The four bytes of round key 1, the first word. */
		for (part = 0; part < 4U; part++) {
			rotated = (part + 1U) % 4U;
			if (part == 0U)
				CHECK(round_key_byte(&aes, 1U, part) == (uint8_t)(sbox[key[12U + rotated]] ^ 0x01U));
			else
				CHECK(round_key_byte(&aes, 1U, part) == sbox[key[12U + rotated]]);
		}
	}
}

/* Multiplies by {02} in GF(2^8) (4.5, XTIMES). */
static uint8_t
reference_xtime(
	uint8_t value)
{
	unsigned int doubled;

	/* A shift, and the reduction by m(x) when the top bit was set. */
	doubled = (unsigned int)value << 1;
	if ((doubled & 0x100U) != 0U)
		doubled ^= 0x11bU;
	return (uint8_t)doubled;
}

/* Expands a key (5.2, Algorithm 2) bytewise. */
static void
reference_expand(
	struct reference *reference,
	const uint8_t *key,
	size_t key_size)
{
	uint8_t words[60][4];
	uint8_t temp[4];
	uint8_t rcon;
	unsigned int key_words;
	unsigned int total;
	unsigned int word;
	unsigned int part;
	uint8_t first;

	/* Nk, Nr and the number of words. */
	key_words = (unsigned int)(key_size / 4U);
	reference->rounds = key_words + 6U;
	total = 4U * (reference->rounds + 1U);
	for (word = 0; word < key_words; word++)
		memcpy(words[word], key + 4U * word, 4);

	/* The loop of Algorithm 2. */
	rcon = 1;
	for (word = key_words; word < total; word++) {
		memcpy(temp, words[word - 1U], 4);
		if (word % key_words == 0U) {
			/* RotWord, SubWord and the round constant. */
			first = temp[0];
			temp[0] = sbox[temp[1]];
			temp[1] = sbox[temp[2]];
			temp[2] = sbox[temp[3]];
			temp[3] = sbox[first];
			temp[0] = (uint8_t)(temp[0] ^ rcon);
			rcon = reference_xtime(rcon);
		} else if (key_words > 6U && word % key_words == 4U) {
			for (part = 0; part < 4U; part++)
				temp[part] = sbox[temp[part]];
		}

		/* The word is the word Nk back plus the transformed previous word. */
		for (part = 0; part < 4U; part++)
			words[word][part] = (uint8_t)(words[word - key_words][part] ^ temp[part]);
	}

	/* The words, four per round key. */
	for (word = 0; word < total; word++)
		memcpy(&reference->round_key[word / 4U][4U * (word % 4U)], words[word], 4);
}

/* Encrypts one block bytewise (5.1, Algorithm 1). */
static void
reference_encrypt(
	const struct reference *reference,
	const uint8_t *in,
	uint8_t *out)
{
	uint8_t state[16];
	uint8_t shifted[16];
	uint8_t column[4];
	unsigned int round;
	unsigned int row;
	unsigned int col;
	unsigned int index;

	/* The state is the input, byte n at row n mod 4, column n / 4 (3.4), and round key 0 is added. */
	for (index = 0; index < 16U; index++)
		state[index] = (uint8_t)(in[index] ^ reference->round_key[0][index]);

	/* The rounds. */
	for (round = 1; round <= reference->rounds; round++) {
		/* SubBytes. */
		for (index = 0; index < 16U; index++)
			state[index] = sbox[state[index]];

		/* ShiftRows: s'[r, c] = s[r, (c + r) mod 4] (5.1.2). */
		for (row = 0; row < 4U; row++) {
			for (col = 0; col < 4U; col++)
				shifted[row + 4U * col] = state[row + 4U * ((col + row) % 4U)];
		}

		/* The shifted rows are the state from here on. */
		memcpy(state, shifted, sizeof(state));

		/* MixColumns (5.1.3), except in the last round. */
		if (round != reference->rounds) {
			for (col = 0; col < 4U; col++) {
				memcpy(column, state + 4U * col, 4);
				for (row = 0; row < 4U; row++) {
					state[4U * col + row] = (uint8_t)(reference_xtime(column[row]) ^
					    (reference_xtime(column[(row + 1U) % 4U]) ^ column[(row + 1U) % 4U]) ^
					    column[(row + 2U) % 4U] ^ column[(row + 3U) % 4U]);
				}
			}
		}

		/* AddRoundKey. */
		for (index = 0; index < 16U; index++)
			state[index] = (uint8_t)(state[index] ^ reference->round_key[round][index]);
	}

	/* The state after the last round is the output block. */
	memcpy(out, state, 16);
}

/* Encrypts the blocks of one pseudo-random message with the reference and with the implementation and compares. */
static void
check_random_blocks(
	size_t key_size,
	size_t blocks)
{
	struct cryc_aes aes;
	struct reference reference;
	uint8_t key[32];
	uint8_t *in;
	uint8_t *out;
	uint8_t *expected;
	size_t index;

	/* The implementation and the reference share a key. */
	random_fill(key, key_size);
	CHECK(cryc_aes_init(&aes, key, key_size) == 0);
	reference_expand(&reference, key, key_size);

	/* Buffers of exactly the right size, for the sanitizer to see any read or write beyond them. */
	in = malloc(blocks * 16U + 1U);
	out = malloc(blocks * 16U + 1U);
	expected = malloc(blocks * 16U + 1U);
	CHECK(in != NULL && out != NULL && expected != NULL);
	if (in == NULL || out == NULL || expected == NULL)
		exit(EXIT_FAILURE);
	random_fill(in, blocks * 16U);
	for (index = 0; index < blocks; index++)
		reference_encrypt(&reference, in + index * 16U, expected + index * 16U);

	/* Into a separate buffer. */
	memset(out, SENTINEL_BYTE, blocks * 16U + 1U);
	cryc_aes_encrypt_blocks(&aes, in, out, blocks);
	CHECK_BYTES(out, expected, blocks * 16U);
	CHECK(out[blocks * 16U] == SENTINEL_BYTE);

	/* In place. */
	memcpy(out, in, blocks * 16U);
	cryc_aes_encrypt_blocks(&aes, out, out, blocks);
	CHECK_BYTES(out, expected, blocks * 16U);

	/* Releases the buffers and wipes the key schedule. */
	free(in);
	free(out);
	free(expected);
	cryc_aes_clear(&aes);
}

/* Reads one CAVP known-answer file and checks every record (the encryption direction of each). */
static void
check_cavp(
	const char *name)
{
	struct vec_file file;
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t plain[16];
	uint8_t cipher[16];
	uint8_t out[16];
	size_t key_size;
	const char *key_text;
	int opened;
	int more;

	/* Opens the known-answer file. */
	opened = vec_open(&file, name);
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* The records. */
	for (more = vec_next(&file); more != 0; more = vec_next(&file)) {
		key_text = vec_get(&file, "KEY");
		if (key_text == NULL)
			continue;
		key_size = vec_hex(&file, "KEY", key, sizeof(key));
		CHECK(key_size == 16U || key_size == 24U || key_size == 32U);
		CHECK(vec_hex(&file, "PLAINTEXT", plain, sizeof(plain)) == 16U);
		CHECK(vec_hex(&file, "CIPHERTEXT", cipher, sizeof(cipher)) == 16U);
		if (key_size != 16U &&
		    key_size != 24U &&
		    key_size != 32U)
			continue;
		CHECK(cryc_aes_init(&aes, key, key_size) == 0);
		cryc_aes_encrypt_blocks(&aes, plain, out, 1);
		CHECK_BYTES(out, cipher, 16);
	}

	/* The file held at least one record, and it is closed. */
	CHECK(file.records > 0);
	vec_close(&file);
}

/* Adds one to the last four bytes of a counter block, bytewise, as a big-endian integer (6.2, inc32). */
static void
reference_inc32(
	uint8_t *block)
{
	unsigned int index;

	/* The carry runs from byte 15 toward byte 12 and stops there. */
	for (index = 16; index > 12U; index--) {
		block[index - 1U] = (uint8_t)(block[index - 1U] + 1U);
		if (block[index - 1U] != 0U)
			return;
	}
}

/* The reference GCTR (6.5): block by block with the single-block cipher. */
static void
reference_gctr(
	const struct cryc_aes *aes,
	uint8_t *counter,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint8_t keystream[16];
	size_t done;
	size_t index;
	size_t take;

	/* Each block of the input, the last one maybe partial. */
	for (done = 0; done < size; done += 16U) {
		cryc_aes_encrypt_blocks(aes, counter, keystream, 1);
		take = size - done;
		if (take > 16U)
			take = 16U;
		for (index = 0; index < take; index++)
			out[done + index] = (uint8_t)(in[done + index] ^ keystream[index]);
		reference_inc32(counter);
	}
}

/* Checks the counter mode on one size, one key size and one initial counter. */
static void
check_ctr_one(
	size_t key_size,
	size_t size,
	uint32_t low_counter,
	int in_place)
{
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t counter[16];
	uint8_t reference_counter[16];
	uint8_t *in;
	uint8_t *out;
	uint8_t *expected;

	/* A pseudo-random key and counter block with the chosen 32-bit counter. */
	random_fill(key, key_size);
	random_fill(counter, 12);
	cryc_store_be32(counter + 12, low_counter);
	memcpy(reference_counter, counter, sizeof(counter));
	CHECK(cryc_aes_init(&aes, key, key_size) == 0);

	/* Exactly sized buffers. */
	in = malloc(size + 1U);
	out = malloc(size + 1U);
	expected = malloc(size + 1U);
	if (in == NULL || out == NULL || expected == NULL)
		exit(EXIT_FAILURE);
	random_fill(in, size);
	memset(out, SENTINEL_BYTE, size + 1U);
	reference_gctr(&aes, reference_counter, in, expected, size);

	/* The call under test. */
	if (in_place) {
		memcpy(out, in, size);
		cryc_aes_ctr32_xor(&aes, counter, out, out, size);
	} else {
		cryc_aes_ctr32_xor(&aes, counter, in, out, size);
	}

	/* The output equals the reference, and nothing was written past it. */
	CHECK_BYTES(out, expected, size);
	CHECK(out[size] == SENTINEL_BYTE);

	/* The counter after the call is the next block not used, as the reference left it (and size 0 changes nothing). */
	CHECK_BYTES(counter, reference_counter, 16);

	/* Releases the buffers and wipes the key schedule. */
	free(in);
	free(out);
	free(expected);
	cryc_aes_clear(&aes);
}

/* Checks the counter mode on many sizes and counters, including the wrap of the 32-bit counter. */
static void
check_ctr(void)
{
	static const uint32_t counters[] = {
		0U, 1U, 2U, 0xfffffffeU, 0xffffffffU, 0xfffffff0U, 0x00ffffffU, 0x7fffffffU, 0x80000000U
	};
	static const size_t key_sizes[] = {16U, 24U, 32U};
	size_t size;
	size_t index;
	size_t key_index;

	/* Every size from 0 to 150 bytes (up to 10 blocks, around each block and group boundary). */
	for (key_index = 0; key_index < 3U; key_index++) {
		for (size = 0; size <= 150U; size++) {
			for (index = 0; index < sizeof(counters) / sizeof(counters[0]); index++)
				check_ctr_one(key_sizes[key_index], size, counters[index], (int)(size % 2U));
		}
	}

	/* A long message that crosses the wrap of the counter in the middle. */
	check_ctr_one(16U, 1000U, 0xffffff80U, 0);
	check_ctr_one(32U, 1000U, 0xffffff80U, 1);
}

/* Checks the counter mode against the ciphertext of the Wycheproof AES-GCM vectors with a 96-bit IV. */
static void
check_gcm_keystream(void)
{
	struct vec_file file;
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t counter[16];
	uint8_t message[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t out[VECTOR_BYTES];
	size_t key_size;
	size_t iv_size;
	size_t message_size;
	size_t cipher_size;
	const char *result;
	unsigned long used;
	int opened;
	int more;
	int differs;

	/* Opens the Wycheproof AES-GCM file. */
	opened = vec_open(&file, "wycheproof-aes_gcm_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* The records with a valid result and a 12-byte IV. */
	used = 0;
	for (more = vec_next(&file); more != 0; more = vec_next(&file)) {
		result = vec_get(&file, "result");
		if (result == NULL)
			continue;
		differs = strcmp(result, "valid");
		if (differs != 0)
			continue;
		iv_size = vec_hex(&file, "iv", iv, sizeof(iv));
		key_size = vec_hex(&file, "key", key, sizeof(key));
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		cipher_size = vec_hex(&file, "ct", cipher, sizeof(cipher));
		if (iv_size != 12U ||
		    message_size == (size_t)-1 ||
		    message_size != cipher_size)
			continue;
		if (key_size != 16U &&
		    key_size != 24U &&
		    key_size != 32U)
			continue;

		/* GCM encrypts with the keystream that starts at inc32(IV || 00000001) (SP 800-38D 7.1, step 3). */
		memcpy(counter, iv, 12);
		cryc_store_be32(counter + 12, 2U);
		CHECK(cryc_aes_init(&aes, key, key_size) == 0);
		cryc_aes_ctr32_xor(&aes, counter, message, out, message_size);
		CHECK_BYTES(out, cipher, message_size);
		CHECK(cryc_load_be32(counter + 12) == 2U + (uint32_t)((message_size + 15U) / 16U));
		used++;
	}

	/* Enough records were used for the check to mean something, and the file is closed. */
	CHECK(used > 100UL);
	vec_close(&file);
}

/* Checks that init refuses every key size but 16, 24 and 32, and leaves the structure wiped. */
static void
check_init_errors(void)
{
	struct cryc_aes aes;
	struct cryc_aes zero;
	uint8_t key[64];
	size_t size;

	/* The wiped structure to compare with. */
	memset(&zero, 0, sizeof(zero));
	memset(key, 0x11, sizeof(key));

	/* Every size from 0 to 64. */
	for (size = 0; size <= 64U; size++) {
		memset(&aes, 0xff, sizeof(aes));
		if (size == 16U || size == 24U || size == 32U) {
			CHECK(cryc_aes_init(&aes, key, size) == 0);
			CHECK(aes.rounds == size / 4U + 6U);
			cryc_aes_clear(&aes);
			CHECK(memcmp(&aes, &zero, sizeof(aes)) == 0);
		} else {
			CHECK(cryc_aes_init(&aes, key, size) == EINVAL);
			CHECK(memcmp(&aes, &zero, sizeof(aes)) == 0);
		}
	}

	/* A very large size, the sizes next to the valid ones and a missing key. */
	memset(&aes, 0xff, sizeof(aes));
	CHECK(cryc_aes_init(&aes, key, (size_t)-1) == EINVAL);
	CHECK(memcmp(&aes, &zero, sizeof(aes)) == 0);
	memset(&aes, 0xff, sizeof(aes));
	CHECK(cryc_aes_init(&aes, NULL, 16U) == EINVAL);
	CHECK(memcmp(&aes, &zero, sizeof(aes)) == 0);
}

/* Checks that the encryption does not change the key and that count 0 touches nothing. */
static void
check_const_and_zero(void)
{
	struct cryc_aes aes;
	struct cryc_aes copy;
	uint8_t key[16];
	uint8_t in[16];
	uint8_t out[16];

	/* A key and its copy. */
	random_fill(key, sizeof(key));
	CHECK(cryc_aes_init(&aes, key, sizeof(key)) == 0);
	copy = aes;
	random_fill(in, sizeof(in));
	memset(out, SENTINEL_BYTE, sizeof(out));

	/* Count 0 writes nothing (and may be given NULL buffers). */
	cryc_aes_encrypt_blocks(&aes, NULL, NULL, 0);
	cryc_aes_encrypt_blocks(&aes, in, out, 0);
	CHECK(out[0] == SENTINEL_BYTE && out[15] == SENTINEL_BYTE);

	/* An encryption leaves the key as it was. */
	cryc_aes_encrypt_blocks(&aes, in, out, 1);
	CHECK(memcmp(&aes, &copy, sizeof(aes)) == 0);
	cryc_aes_clear(&aes);
}

/* Runs the checks. */
int
main(void)
{
	static const char *const cavp_names[] = {
		"cavp-KAT_AES-ECBGFSbox128", "cavp-KAT_AES-ECBGFSbox192", "cavp-KAT_AES-ECBGFSbox256",
		"cavp-KAT_AES-ECBKeySbox128", "cavp-KAT_AES-ECBKeySbox192", "cavp-KAT_AES-ECBKeySbox256",
		"cavp-KAT_AES-ECBVarKey128", "cavp-KAT_AES-ECBVarKey192", "cavp-KAT_AES-ECBVarKey256",
		"cavp-KAT_AES-ECBVarTxt128", "cavp-KAT_AES-ECBVarTxt192", "cavp-KAT_AES-ECBVarTxt256"
	};
	static const size_t key_sizes[] = {16U, 24U, 32U};
	struct cryc_aes aes;
	uint8_t input[16];
	uint8_t key[16];
	uint8_t expected[16];
	uint8_t out[16];
	size_t index;
	size_t key_index;
	size_t blocks;
	unsigned int repeat;

	/* The S-box, the three key expansions of Appendix A. */
	check_sbox();
	check_expansion(a1_key, 16U, a1_words, 40U, 10U);
	check_expansion(a2_key, 24U, a2_words, 46U, 12U);
	check_expansion(a3_key, 32U, a3_words, 52U, 14U);

	/* The cipher example of Appendix B. */
	CHECK(hex_decode(b_input, input, sizeof(input)) == 16U);
	CHECK(hex_decode(b_key, key, sizeof(key)) == 16U);
	CHECK(hex_decode(b_output, expected, sizeof(expected)) == 16U);
	CHECK(cryc_aes_init(&aes, key, sizeof(key)) == 0);
	cryc_aes_encrypt_blocks(&aes, input, out, 1);
	CHECK_BYTES(out, expected, 16);
	cryc_aes_clear(&aes);

	/* The CAVP known answers. */
	for (index = 0; index < sizeof(cavp_names) / sizeof(cavp_names[0]); index++)
		check_cavp(cavp_names[index]);

	/* The reference cipher on pseudo-random keys, 0 to 9 blocks (the group of four and its remainders), a few repeats. */
	for (key_index = 0; key_index < 3U; key_index++) {
		for (blocks = 0; blocks <= 9U; blocks++) {
			for (repeat = 0; repeat < 5U; repeat++)
				check_random_blocks(key_sizes[key_index], blocks);
		}

		/* One long message of 64 blocks. */
		check_random_blocks(key_sizes[key_index], 64U);
	}

	/* The counter mode, its keystream against GCM's, the errors, the key kept. */
	check_ctr();
	check_gcm_keystream();
	check_init_errors();
	check_const_and_zero();

	/* The result. */
	return check_finish("test-aes");
}
