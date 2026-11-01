/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AES in bitsliced, constant-time form, and its counter mode (FIPS 197, SP
 * 800-38D GCTR).
 *
 * The section numbers in the comments of this file are those of FIPS 197
 * (May 2023 update) and of NIST SP 800-38D (November 2007).
 *
 * The state.  Four blocks are held at once as eight 64-bit words, the
 * slices.  Bit p of slice i is bit i of state byte p, where p is 16 * b + n
 * for byte n of block b, n being the byte's position in the input order of
 * 3.4 (n = r + 4 * c for row r and column c).  So a 16-bit lane of a slice
 * holds one block, and the bytes of a column are four neighbouring bits.
 * A group with fewer than four blocks has zero bytes in the unused lanes
 * and nothing is read back from them.
 *
 * The round keys.  schedule[8 * k + i] is slice i of round key k with the
 * same round key in all four lanes (bit 16 * b + n is bit i of byte n of
 * round key k).  Words after the last round key are zero.
 *
 * Nothing here indexes memory or branches with a key byte or a state byte:
 * the S-box is a circuit of AND and XOR on the slices.  The loop bounds
 * and the indices are the sizes and the round numbers, which are public.
 */

#include <string.h>

#include "cryc-aes.h"

/* The number of bit slices of a state: one per bit of a byte (3.2). */
#define AES_SLICES		8U

/* The number of blocks one group of slices holds. */
#define AES_GROUP_BLOCKS	4U

/* The bytes of one group of blocks. */
#define AES_GROUP_BYTES		(AES_GROUP_BLOCKS * CRYC_AES_BLOCK)

/* The most words a key expansion makes: 4 * (Nr + 1) for Nr = 14 (5.2, Table 3). */
#define AES_MAX_WORDS		60U

/* Bit 0 of every 16-bit lane: multiplying a 16-bit value by it copies the value into all four lanes. */
#define AES_LANE_ONE		CRYC_U64(0x00010001U, 0x00010001U)

/* The bits of the state bytes whose row r is 0, 1, 2 and 3: bit p with p mod 4 equal to r (3.4). */
#define AES_ROW0		CRYC_U64(0x11111111U, 0x11111111U)
#define AES_ROW1		CRYC_U64(0x22222222U, 0x22222222U)
#define AES_ROW2		CRYC_U64(0x44444444U, 0x44444444U)
#define AES_ROW3		CRYC_U64(0x88888888U, 0x88888888U)

/* The bits in rows 0 to 2 and in row 3, the targets of the row rotation of MixColumns. */
#define AES_ROWS_LOW		CRYC_U64(0x77777777U, 0x77777777U)
#define AES_ROWS_HIGH		AES_ROW3

/*
 * The bits of columns r to 3 and of columns 0 to r - 1 in every lane, for
 * the rows r = 1, 2 and 3 of ShiftRows: the part of a row that moves right
 * by 4 * r bits and the part that wraps around to the left (5.1.2).
 */
#define AES_COLUMNS_FROM1	CRYC_U64(0xfff0fff0U, 0xfff0fff0U)
#define AES_COLUMNS_BELOW1	CRYC_U64(0x000f000fU, 0x000f000fU)
#define AES_COLUMNS_FROM2	CRYC_U64(0xff00ff00U, 0xff00ff00U)
#define AES_COLUMNS_BELOW2	CRYC_U64(0x00ff00ffU, 0x00ff00ffU)
#define AES_COLUMNS_FROM3	CRYC_U64(0xf000f000U, 0xf000f000U)
#define AES_COLUMNS_BELOW3	CRYC_U64(0x0fff0fffU, 0x0fff0fffU)

/*
 * The constant added in the affine transformation of the S-box: bit i of c
 * is the constant of equation (5.3) for output bit i, and a slice whose
 * bit of c is 1 is complemented.
 *
 * Values referenced: FIPS 197 (May 2023 update) 5.1.1, equation (5.3) and
 * equation (5.4), c = {63} = 01100011 (bits 0, 1, 5 and 6 set).
 */
#define AES_SBOX_AFFINE		0x63U

/*
 * The reduction polynomial of GF(2^8) without its top term: m(x) = x^8 +
 * x^4 + x^3 + x + 1 gives 0x1b (4.1, equation (4.1)).  Used for the
 * round constant, which is public.
 */
#define AES_REDUCTION		0x1bU

/* Multiplies two field elements held as eight slices each: out = left * right in GF(2^8) (4.2). */
static void aes_field_multiply(uint64_t *out, const uint64_t *left, const uint64_t *right);

/* Squares a field element held as eight slices (4.2, equation (4.2) with both factors equal; squaring is linear). */
static void aes_field_square(uint64_t *out, const uint64_t *in);

/* Applies the S-box of 5.1.1 to every byte of the slices at once: the inverse of 4.4 then the affine transformation (5.3). */
static void aes_sub_slices(uint64_t *slices);

/* Rotates the bytes of every column up by one row: the byte of row r + 1 moves to row r (the row 0 byte moves to row 3). */
static uint64_t aes_rotate_rows(uint64_t slice);

/* Applies ShiftRows to the slices (5.1.2). */
static void aes_shift_rows(uint64_t *slices);

/* Applies MixColumns to the slices (5.1.3). */
static void aes_mix_columns(uint64_t *slices);

/* Reads size bytes (at most 64) into the slices; the bytes beyond are zero. */
static void aes_load(uint64_t *slices, const uint8_t *in, size_t size);

/* Writes size bytes (at most 64) from the slices. */
static void aes_store(const uint64_t *slices, uint8_t *out, size_t size);

/* Makes the slices of a round key (16 bytes) with the key in all four lanes. */
static void aes_load_round_key(uint64_t *slices, const uint8_t *round_key);

/* Applies SubWord (5.2) to a word: the S-box on each of its four bytes. */
static uint32_t aes_sub_word(uint32_t word);

/* Encrypts up to four blocks held as slices with the round keys (Algorithm 1, 5.1). */
static void aes_cipher_slices(const struct cryc_aes *aes, uint64_t *slices);

/*
 * Multiplies two elements of GF(2^8) (4.2) held in slice form: the product
 * polynomial is the sum of left_i * right_j * x^(i + j), the AND of two
 * slices and an XOR into the slice i + j, and the terms of degree 8 to 14
 * are folded down with x^8 = x^4 + x^3 + x + 1 (4.1).
 */
static void
aes_field_multiply(
	uint64_t *out,
	const uint64_t *left,
	const uint64_t *right)
{
	uint64_t product[15];
	unsigned int i;
	unsigned int j;

	/* Starts the product with no terms. */
	for (i = 0; i < 15U; i++)
		product[i] = 0;

	/* Adds every partial product of the schoolbook multiplication. */
	for (i = 0; i < AES_SLICES; i++) {
		for (j = 0; j < AES_SLICES; j++)
			product[i + j] ^= left[i] & right[j];
	}

	/*
	 * Folds the high terms into the low ones from the top down, so that a
	 * term produced by a fold (degrees 10 to 12) is folded in turn.
	 */
	for (i = 14U; i >= AES_SLICES; i--) {
		product[i - 8U] ^= product[i];
		product[i - 7U] ^= product[i];
		product[i - 5U] ^= product[i];
		product[i - 4U] ^= product[i];
	}

	/* Takes the low eight terms. */
	for (i = 0; i < AES_SLICES; i++)
		out[i] = product[i];

	/* The partial products are secret. */
	cryc_wipe(product, sizeof(product));
}

/*
 * Squares an element of GF(2^8) held in slice form.  The square of b is
 * the sum of b_i * x^(2 * i); reducing x^0, x^2, ... x^14 modulo m(x) gives
 * the bytes 01, 04, 10, 40, 1b, 6c, ab and 9a, so output bit j is the XOR
 * of the input bits i whose byte has bit j set.
 */
static void
aes_field_square(
	uint64_t *out,
	const uint64_t *in)
{
	uint64_t squared[8];
	unsigned int i;

	/*
	 * The output bits, from the table of the reduced powers: x^8 = 1b is
	 * 00011011, x^10 = 6c is 01101100, x^12 = ab is 10101011 and x^14 = 9a
	 * is 10011010.
	 */
	squared[0] = in[0] ^ in[4] ^ in[6];
	squared[1] = in[4] ^ in[6] ^ in[7];
	squared[2] = in[1] ^ in[5];
	squared[3] = in[4] ^ in[5] ^ in[6] ^ in[7];
	squared[4] = in[2] ^ in[4] ^ in[7];
	squared[5] = in[5] ^ in[6];
	squared[6] = in[3] ^ in[5];
	squared[7] = in[6] ^ in[7];

	/* Copies the result, so that out may be in. */
	for (i = 0; i < AES_SLICES; i++)
		out[i] = squared[i];
}

/*
 * Applies the S-box.  The multiplicative inverse of 4.4 (0 maps to 0) is
 * the power 254 of the byte (4.4: the group of nonzero elements has order
 * 255), found with the addition chain 2, 3, 12, 15, 240, 252, 254 using
 * four multiplications and seven squarings.  The affine transformation of
 * equation (5.3) follows: output bit i is the XOR of the inverse's bits
 * i, i + 4, i + 5, i + 6 and i + 7 (mod 8), complemented where c has bit i.
 */
static void
aes_sub_slices(
	uint64_t *slices)
{
	uint64_t power2[8];
	uint64_t power3[8];
	uint64_t power12[8];
	uint64_t power15[8];
	uint64_t power240[8];
	uint64_t power252[8];
	uint64_t inverse[8];
	uint64_t temp[8];
	uint64_t complement;
	unsigned int i;

	/* The powers 2 and 3 of the byte. */
	aes_field_square(power2, slices);
	aes_field_multiply(power3, power2, slices);

	/* The power 12, the square of the square of the power 3. */
	aes_field_square(temp, power3);
	aes_field_square(power12, temp);

	/* The power 15 = 12 + 3. */
	aes_field_multiply(power15, power12, power3);

	/* The power 240 = 15 * 16, four squarings. */
	aes_field_square(temp, power15);
	aes_field_square(temp, temp);
	aes_field_square(temp, temp);
	aes_field_square(power240, temp);

	/* The power 252 = 240 + 12 and the power 254 = 252 + 2, the inverse. */
	aes_field_multiply(power252, power240, power12);
	aes_field_multiply(inverse, power252, power2);

	/* The affine transformation, slice by slice. */
	for (i = 0; i < AES_SLICES; i++) {
		slices[i] = inverse[i] ^
		    inverse[(i + 4U) & 7U] ^
		    inverse[(i + 5U) & 7U] ^
		    inverse[(i + 6U) & 7U] ^
		    inverse[(i + 7U) & 7U];

		/* A slice is complemented where the constant c has a 1. */
		complement = 0 - (uint64_t)((AES_SBOX_AFFINE >> i) & 1U);
		slices[i] ^= complement;
	}

	/* The intermediate powers are secret. */
	cryc_wipe(power2, sizeof(power2));
	cryc_wipe(power3, sizeof(power3));
	cryc_wipe(power12, sizeof(power12));
	cryc_wipe(power15, sizeof(power15));
	cryc_wipe(power240, sizeof(power240));
	cryc_wipe(power252, sizeof(power252));
	cryc_wipe(inverse, sizeof(inverse));
	cryc_wipe(temp, sizeof(temp));
}

/*
 * Moves the byte of row r + 1 of every column to row r.  Bit p of the
 * result is bit p + 1 of the input for the rows 0 to 2, and for row 3 it is
 * bit p - 3 of the input (row 0 of the same column).
 */
static uint64_t
aes_rotate_rows(
	uint64_t slice)
{
	uint64_t rotated;

	/* Rows 1, 2 and 3 take the place of rows 0, 1 and 2; row 0 becomes row 3. */
	rotated = (slice >> 1) & AES_ROWS_LOW;
	rotated |= (slice << 3) & AES_ROWS_HIGH;

	return rotated;
}

/*
 * ShiftRows (5.1.2, equation (5.5)): s'[r, c] = s[r, (c + r) mod 4], so
 * the bytes of row r move right (toward column 0) by r columns, that is by
 * 4 * r bit positions, and the ones that leave on the right of the lane
 * wrap around to the left end of the same row of the same lane.
 */
static void
aes_shift_rows(
	uint64_t *slices)
{
	uint64_t slice;
	uint64_t shifted;
	unsigned int i;

	/* Each slice is shifted on its own. */
	for (i = 0; i < AES_SLICES; i++) {
		slice = slices[i];

		/* Row 0 stays. */
		shifted = slice & AES_ROW0;

		/* Row 1 by one column. */
		shifted |= (slice & AES_ROW1 & AES_COLUMNS_FROM1) >> 4;
		shifted |= (slice & AES_ROW1 & AES_COLUMNS_BELOW1) << 12;

		/* Row 2 by two columns. */
		shifted |= (slice & AES_ROW2 & AES_COLUMNS_FROM2) >> 8;
		shifted |= (slice & AES_ROW2 & AES_COLUMNS_BELOW2) << 8;

		/* Row 3 by three columns. */
		shifted |= (slice & AES_ROW3 & AES_COLUMNS_FROM3) >> 12;
		shifted |= (slice & AES_ROW3 & AES_COLUMNS_BELOW3) << 4;

		slices[i] = shifted;
	}
}

/*
 * MixColumns (5.1.3): s'[r] = {02} * s[r] + {03} * s[r + 1] + s[r + 2] +
 * s[r + 3] (indices mod 4) in every column, which is {02} * (s[r] + s[r +
 * 1]) + s[r + 1] + s[r + 2] + s[r + 3].  The multiplication by {02} is
 * XTIMES (4.5) on the slices: slice 0 takes slice 7 of the operand and
 * slices 1, 3 and 4 also add it.
 */
static void
aes_mix_columns(
	uint64_t *slices)
{
	uint64_t next1[8];
	uint64_t next2[8];
	uint64_t next3[8];
	uint64_t sum[8];
	uint64_t mixed[8];
	unsigned int i;

	/* The bytes one, two and three rows further down in the column. */
	for (i = 0; i < AES_SLICES; i++) {
		next1[i] = aes_rotate_rows(slices[i]);
		next2[i] = aes_rotate_rows(next1[i]);
		next3[i] = aes_rotate_rows(next2[i]);
		sum[i] = slices[i] ^ next1[i];
	}

	/* {02} * (s[r] + s[r + 1]) by XTIMES (4.5): shift up one bit, and fold the top bit back by m(x). */
	mixed[0] = sum[7];
	mixed[1] = sum[0] ^ sum[7];
	mixed[2] = sum[1];
	mixed[3] = sum[2] ^ sum[7];
	mixed[4] = sum[3] ^ sum[7];
	mixed[5] = sum[4];
	mixed[6] = sum[5];
	mixed[7] = sum[6];

	/* Adds s[r + 1], s[r + 2] and s[r + 3]. */
	for (i = 0; i < AES_SLICES; i++)
		slices[i] = mixed[i] ^ next1[i] ^ next2[i] ^ next3[i];

	/* The state is secret. */
	cryc_wipe(next1, sizeof(next1));
	cryc_wipe(next2, sizeof(next2));
	cryc_wipe(next3, sizeof(next3));
	cryc_wipe(sum, sizeof(sum));
	cryc_wipe(mixed, sizeof(mixed));
}

/*
 * Puts bit i of byte p into bit p of slice i.  The positions are public
 * (the byte index and the bit index), the values are only shifted.
 */
static void
aes_load(
	uint64_t *slices,
	const uint8_t *in,
	size_t size)
{
	size_t position;
	unsigned int i;

	/* Starts with zero slices. */
	for (i = 0; i < AES_SLICES; i++)
		slices[i] = 0;

	/* Each byte contributes one bit to every slice. */
	for (position = 0; position < size; position++) {
		for (i = 0; i < AES_SLICES; i++)
			slices[i] |= (uint64_t)((in[position] >> i) & 1U) << position;
	}
}

/*
 * Collects bit p of the slices into byte p.
 */
static void
aes_store(
	const uint64_t *slices,
	uint8_t *out,
	size_t size)
{
	size_t position;
	unsigned int value;
	unsigned int i;

	/* Each byte is rebuilt from one bit of every slice. */
	for (position = 0; position < size; position++) {
		value = 0;
		for (i = 0; i < AES_SLICES; i++)
			value |= (unsigned int)((slices[i] >> position) & 1U) << i;
		out[position] = (uint8_t)value;
	}
}

/*
 * Makes the slices of one round key: 16 bytes, the same in each of the four
 * lanes.  A lane value collects bit i of the 16 bytes (bit n for byte n), and
 * multiplying it by AES_LANE_ONE copies it to all four lanes.
 */
static void
aes_load_round_key(
	uint64_t *slices,
	const uint8_t *round_key)
{
	uint64_t lane;
	unsigned int byte;
	unsigned int i;

	/* One slice for every bit of a byte. */
	for (i = 0; i < AES_SLICES; i++) {
		lane = 0;
		for (byte = 0; byte < CRYC_AES_BLOCK; byte++)
			lane |= (uint64_t)((round_key[byte] >> i) & 1U) << byte;
		slices[i] = lane * AES_LANE_ONE;
	}
}

/*
 * SubWord (5.2): the four bytes of the word go through the S-box circuit
 * as the first four byte positions of one group of slices.
 */
static uint32_t
aes_sub_word(
	uint32_t word)
{
	uint64_t slices[8];
	uint8_t bytes[4];
	uint32_t result;

	/* The word as bytes in the order of 3.4, the first byte the most significant. */
	cryc_store_be32(bytes, word);

	/* The S-box on each byte. */
	aes_load(slices, bytes, sizeof(bytes));
	aes_sub_slices(slices);
	aes_store(slices, bytes, sizeof(bytes));
	result = cryc_load_be32(bytes);

	/* The bytes come from the key. */
	cryc_wipe(slices, sizeof(slices));
	cryc_wipe(bytes, sizeof(bytes));

	return result;
}

/*
 * Cipher (5.1, Algorithm 1): AddRoundKey with round key 0; Nr - 1 rounds of
 * SubBytes, ShiftRows, MixColumns and AddRoundKey; a last round without
 * MixColumns.
 */
static void
aes_cipher_slices(
	const struct cryc_aes *aes,
	uint64_t *slices)
{
	unsigned int round;
	unsigned int i;

	/* Round key 0. */
	for (i = 0; i < AES_SLICES; i++)
		slices[i] ^= aes->schedule[i];

	/* The rounds 1 to Nr - 1. */
	for (round = 1; round < aes->rounds; round++) {
		aes_sub_slices(slices);
		aes_shift_rows(slices);
		aes_mix_columns(slices);
		for (i = 0; i < AES_SLICES; i++)
			slices[i] ^= aes->schedule[round * AES_SLICES + i];
	}

	/* The last round, without MixColumns. */
	aes_sub_slices(slices);
	aes_shift_rows(slices);
	for (i = 0; i < AES_SLICES; i++)
		slices[i] ^= aes->schedule[aes->rounds * AES_SLICES + i];
}

/*
 * Expands key (KeyExpansion, 5.2) into *aes.
 *
 * The words w[0..4 * (Nr + 1) - 1] are made as Algorithm 2 says (the
 * values of RotWord, SubWord and Rcon are those of equations (5.10) and
 * (5.11) and Table 5); every fourth word makes a round key, which is
 * turned into slices.
 */
int
cryc_aes_init(
	struct cryc_aes *aes,
	const uint8_t *key,
	size_t key_size)
{
	uint32_t words[AES_MAX_WORDS];
	uint8_t round_key[CRYC_AES_BLOCK];
	uint32_t temp;
	uint32_t rcon;
	unsigned int key_words;
	unsigned int total_words;
	unsigned int word_index;
	unsigned int round;
	unsigned int part;

	/* Starts wiped, so that every error leaves the structure wiped. */
	cryc_wipe(aes, sizeof(*aes));

	/* The size of the key sets the number of rounds (Table 3). */
	if (key_size != CRYC_AES128_KEY &&
	    key_size != CRYC_AES192_KEY &&
	    key_size != CRYC_AES256_KEY)
		return EINVAL;

	/* A missing key is a caller's mistake, refused the same way. */
	if (key == NULL)
		return EINVAL;

	/* Nk, Nr and the number of words of the expansion. */
	key_words = (unsigned int)(key_size / 4U);
	aes->rounds = key_words + 6U;
	total_words = 4U * (aes->rounds + 1U);

	/* The first Nk words are the key (Algorithm 2, 3.4). */
	for (word_index = 0; word_index < key_words; word_index++)
		words[word_index] = cryc_load_be32(key + 4U * word_index);

	/*
	 * The other words.  The round constant is x^(j - 1) for the j-th group
	 * of Nk words (Table 5), found by repeated XTIMES; it is public.
	 */
	rcon = 1U;
	for (word_index = key_words; word_index < total_words; word_index++) {
		temp = words[word_index - 1U];
		if (word_index % key_words == 0U) {
			/* RotWord, SubWord, then the round constant in the first byte. */
			temp = (temp << 8) | (temp >> 24);
			temp = aes_sub_word(temp);
			temp ^= rcon << 24;
			rcon <<= 1;
			if ((rcon & 0x100U) != 0U)
				rcon = (rcon ^ 0x100U) ^ AES_REDUCTION;
		} else if (key_words > 6U && word_index % key_words == 4U) {
			/* AES-256 only: a SubWord in the middle of the group. */
			temp = aes_sub_word(temp);
		}
		words[word_index] = words[word_index - key_words] ^ temp;
	}

	/* Each round key is four words, as 16 bytes, then slices. */
	for (round = 0; round <= aes->rounds; round++) {
		for (part = 0; part < 4U; part++)
			cryc_store_be32(round_key + 4U * part, words[4U * round + part]);
		aes_load_round_key(aes->schedule + AES_SLICES * round, round_key);
	}

	/* The words and the bytes are the key. */
	cryc_wipe(words, sizeof(words));
	cryc_wipe(round_key, sizeof(round_key));

	/* Succeeded: *aes holds the round keys. */
	return 0;
}

/*
 * Encrypts count blocks of 16 bytes (Cipher, 5.1, Algorithm 1) from in to
 * out, four at a time; the last group may have fewer blocks.
 */
void
cryc_aes_encrypt_blocks(
	const struct cryc_aes *aes,
	const uint8_t *in,
	uint8_t *out,
	size_t count)
{
	uint64_t slices[8];
	size_t group;

	/* Each group is read completely before it is written, so in may be out. */
	while (count > 0) {
		group = count;
		if (group > AES_GROUP_BLOCKS)
			group = AES_GROUP_BLOCKS;

		/* Encrypts the group. */
		aes_load(slices, in, group * CRYC_AES_BLOCK);
		aes_cipher_slices(aes, slices);
		aes_store(slices, out, group * CRYC_AES_BLOCK);

		in += group * CRYC_AES_BLOCK;
		out += group * CRYC_AES_BLOCK;
		count -= group;
	}

	/* The slices hold plaintext and ciphertext. */
	cryc_wipe(slices, sizeof(slices));
}

/*
 * XORs size bytes of in with the keystream E(CB), E(inc32(CB)), ... (SP
 * 800-38D 6.5, GCTR).  Up to four counter blocks are made and encrypted
 * together; inc32 (6.2) adds 1 modulo 2^32 to the last four bytes.
 */
void
cryc_aes_ctr32_xor(
	const struct cryc_aes *aes,
	uint8_t *counter,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint8_t blocks[AES_GROUP_BYTES];
	uint8_t keystream[AES_GROUP_BYTES];
	uint32_t count32;
	size_t chunk;
	size_t block_count;
	size_t block;
	size_t index;

	/* With no bytes the counter stays as it is. */
	if (size == 0)
		return;

	/* The last four bytes of the counter block are the 32-bit counter. */
	count32 = cryc_load_be32(counter + 12);

	/* Each pass handles up to 64 bytes. */
	while (size > 0) {
		chunk = size;
		if (chunk > AES_GROUP_BYTES)
			chunk = AES_GROUP_BYTES;
		block_count = (chunk + CRYC_AES_BLOCK - 1U) / CRYC_AES_BLOCK;

		/* The counter blocks: the first 12 bytes are the counter's, the last four count up. */
		for (block = 0; block < block_count; block++) {
			memcpy(blocks + block * CRYC_AES_BLOCK, counter, 12);
			cryc_store_be32(blocks + block * CRYC_AES_BLOCK + 12, count32 + (uint32_t)block);
		}
		count32 = count32 + (uint32_t)block_count;

		/* The keystream. */
		cryc_aes_encrypt_blocks(aes, blocks, keystream, block_count);

		/* XORs the bytes in use; the rest of the last block's keystream is dropped. */
		for (index = 0; index < chunk; index++)
			out[index] = (uint8_t)(in[index] ^ keystream[index]);

		in += chunk;
		out += chunk;
		size -= chunk;
	}

	/* Hands the next counter block back. */
	cryc_store_be32(counter + 12, count32);

	/* The keystream is secret. */
	cryc_wipe(keystream, sizeof(keystream));
	cryc_wipe(blocks, sizeof(blocks));
}

/*
 * Wipes the expanded key.
 */
void
cryc_aes_clear(
	struct cryc_aes *aes)
{
	/* The round keys and the round count. */
	cryc_wipe(aes, sizeof(*aes));
}
