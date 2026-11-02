/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (RFC 8439).
 *
 * The section numbers in the comments of this file are those of RFC 8439
 * (June 2018).  ChaCha20 is additions, exclusive ors and fixed rotations
 * of 32-bit words.  Poly1305 keeps its accumulator in five limbs of 26
 * bits so that every product of two limbs, and the sum of five of them,
 * fits in an unsigned 64-bit integer; its last reduction and the
 * acceptance of a tag are done with masks.  No branch and no address
 * depends on a key, a plaintext or an accumulator.
 */

#include <string.h>

#include "cryc-chacha.h"

/* The number of 32-bit words of the ChaCha20 state (2.3). */
#define CHACHA_STATE_WORDS	16U

/* The number of double rounds, a column round then a diagonal round, in the 20 rounds (2.3). */
#define CHACHA_DOUBLE_ROUNDS	10U

/* The index of the state word that holds the block counter, or its low half (2.3). */
#define CHACHA_COUNTER_WORD	12U

/* The mask of the low 26 bits, the width of a Poly1305 limb. */
#define POLY1305_LIMB_MASK	0x03ffffffU

/*
 * The value added to limb 4 of a full 16-byte block: the bit beyond the
 * block is 2^128 (2.5), and limb 4 starts at bit 104.
 */
#define POLY1305_FULL_BLOCK_BIT	(1U << 24)

/* The size of a Poly1305 block in bytes (2.5). */
#define POLY1305_BLOCK		16U

/*
 * The most 64-byte blocks the 32-bit counter of the AEAD can reach when it
 * starts at 1 (2.8, note 1): block 0 makes the Poly1305 key.
 */
#define AEAD_MAX_BLOCKS		0xffffffffU

/*
 * The four constant words that begin every ChaCha20 state.
 *
 * They are read by index (a public value) and never written.
 *
 * Values referenced: RFC 8439 (June 2018) 2.3, the first four words of the
 * state, 0x61707865, 0x3320646e, 0x79622d32 and 0x6b206574.
 */
static const uint32_t chacha_constant[4] = {
	0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U
};

/*
 * Sixteen zero bytes, the longest padding the AEAD message needs (2.8).
 */
static const uint8_t poly1305_zero_padding[POLY1305_BLOCK] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Rotates a 32-bit word left by count bits, count from 1 to 31 (2.1). */
static uint32_t chacha_rotate_left(uint32_t value, unsigned int count);

/* Runs the quarter round on the four state words at the given indices (2.1, 2.2). */
static void chacha_quarter_round(uint32_t *state, unsigned int a, unsigned int b, unsigned int c, unsigned int d);

/* Runs one column round and one diagonal round on the state (2.3). */
static void chacha_double_round(uint32_t *state);

/* Fills the constant words and the key words of a state (2.3). */
static void chacha_load_key(uint32_t *state, const uint8_t *key);

/* Runs the block function on a complete state and serializes the 64-byte result (2.3). */
static void chacha_permute(const uint32_t *state, uint8_t *out);

/* XORs size bytes with the keystream of consecutive blocks, keeping each result byte only where keep_mask has ones (2.4). */
static void chacha_stream(uint32_t *state, int wide_counter, const uint8_t *in, uint8_t *out, size_t size, uint8_t keep_mask);

/* Reduces the accumulator by one block multiplication: adds the block and multiplies by r (2.5). */
static void poly1305_absorb_block(struct cryc_poly1305 *mac, const uint8_t *block, uint32_t high_bit);

/* Makes the Poly1305 key from block 0 of the keystream and computes the AEAD tag of the ciphertext (2.6, 2.8). */
static void aead_compute_tag(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext, size_t size, uint8_t *tag);

/*
 * Computes one ChaCha20 block (2.3, the block function) for key, counter
 * and nonce into out (64 bytes).
 */
void
cryc_chacha20_block(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	uint8_t *out)
{
	uint32_t state[CHACHA_STATE_WORDS];

	/* Sets up the constants and the key, then the counter and the nonce words. */
	chacha_load_key(state, key);
	state[12] = counter;
	state[13] = cryc_load_le32(nonce);
	state[14] = cryc_load_le32(nonce + 4);
	state[15] = cryc_load_le32(nonce + 8);

	/* Computes the block. */
	chacha_permute(state, out);

	/* The state holds the key. */
	cryc_wipe(state, sizeof(state));
}

/*
 * XORs size bytes of in with the ChaCha20 keystream starting at block
 * counter (2.4) into out.
 *
 * The keystream must not reach counter 2^32; a larger size returns
 * EOVERFLOW and writes nothing.
 */
int
cryc_chacha20_xor(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint32_t state[CHACHA_STATE_WORDS];
	uint64_t blocks_left;
	uint64_t bytes_left;

	/*
	 * Counts the blocks from the counter up to the last one the 32-bit
	 * counter has, 2^32 - counter of them, and the bytes they hold.  The
	 * product is at most 2^38.
	 */
	blocks_left = CRYC_U64(1, 0) - (uint64_t)counter;
	bytes_left = blocks_left * 64U;

	/* Refuses a message that would run the counter past 2^32 - 1. */
	if ((uint64_t)size > bytes_left)
		return EOVERFLOW;

	/* Sets up the state with the counter and the nonce. */
	chacha_load_key(state, key);
	state[12] = counter;
	state[13] = cryc_load_le32(nonce);
	state[14] = cryc_load_le32(nonce + 4);
	state[15] = cryc_load_le32(nonce + 8);

	/* Encrypts or decrypts the bytes. */
	chacha_stream(state, 0, in, out, size, 0xffU);

	/* The state holds the key. */
	cryc_wipe(state, sizeof(state));

	/* Succeeded: out holds the XOR of in with the keystream. */
	return 0;
}

/*
 * XORs size bytes with the keystream of ChaCha20 in its original form with
 * a 64-bit block counter and a 64-bit nonce: state words 12 and 13 are the
 * counter's low and high halves and words 14 and 15 the nonce read little-
 * endian.  The counter is not allowed to wrap past 2^64 - 1.
 */
int
cryc_chacha20_xor64(
	const uint8_t *key,
	uint64_t counter,
	const uint8_t *nonce8,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint32_t state[CHACHA_STATE_WORDS];
	uint64_t blocks;
	uint64_t blocks_after_first;
	uint64_t counters_left;

	/* Counts the blocks the bytes need: the whole ones and a part of one. */
	blocks = (uint64_t)size / 64U;
	if (((uint64_t)size % 64U) != 0U)
		blocks++;

	/*
	 * The last block uses counter + blocks - 1.  It must not pass 2^64 - 1,
	 * so the blocks after the first may be at most 2^64 - 1 - counter,
	 * which is the complement of the counter.
	 */
	counters_left = ~counter;
	blocks_after_first = 0;
	if (blocks > 0U)
		blocks_after_first = blocks - 1U;

	/* Refuses a message that would run the counter past 2^64 - 1. */
	if (blocks_after_first > counters_left)
		return EOVERFLOW;

	/* Sets up the state with the two halves of the counter and the nonce. */
	chacha_load_key(state, key);
	state[12] = (uint32_t)(counter & 0xffffffffU);
	state[13] = (uint32_t)(counter >> 32);
	state[14] = cryc_load_le32(nonce8);
	state[15] = cryc_load_le32(nonce8 + 4);

	/* Encrypts or decrypts the bytes. */
	chacha_stream(state, 1, in, out, size, 0xffU);

	/* The state holds the key. */
	cryc_wipe(state, sizeof(state));

	/* Succeeded: out holds the XOR of in with the keystream. */
	return 0;
}

/*
 * Starts Poly1305 with the 32-byte one-time key (r then s, 2.5).
 */
void
cryc_poly1305_init(
	struct cryc_poly1305 *mac,
	const uint8_t *key)
{
	uint8_t clamped[16];
	unsigned int index;

	/* Copies r so that the key itself stays untouched. */
	memcpy(clamped, key, sizeof(clamped));

	/*
	 * Clamps r: the top four bits of bytes 3, 7, 11 and 15 and the bottom
	 * two bits of bytes 4, 8 and 12 are cleared.
	 */
	clamped[3] &= 15U;
	clamped[7] &= 15U;
	clamped[11] &= 15U;
	clamped[15] &= 15U;
	clamped[4] &= 252U;
	clamped[8] &= 252U;
	clamped[12] &= 252U;

	/*
	 * Splits r into five limbs of 26 bits.  Limb n starts at bit 26 * n,
	 * which is bit (26 * n) mod 8 of byte (26 * n) / 8; the four bytes
	 * read from there cover the 26 bits of limbs 0 to 3, and limb 4 is the
	 * top 24 bits of the last four bytes.
	 */
	mac->r[0] = cryc_load_le32(clamped) & POLY1305_LIMB_MASK;
	mac->r[1] = (cryc_load_le32(clamped + 3) >> 2) & POLY1305_LIMB_MASK;
	mac->r[2] = (cryc_load_le32(clamped + 6) >> 4) & POLY1305_LIMB_MASK;
	mac->r[3] = (cryc_load_le32(clamped + 9) >> 6) & POLY1305_LIMB_MASK;
	mac->r[4] = cryc_load_le32(clamped + 12) >> 8;

	/* Takes s as four little-endian words. */
	mac->s[0] = cryc_load_le32(key + 16);
	mac->s[1] = cryc_load_le32(key + 20);
	mac->s[2] = cryc_load_le32(key + 24);
	mac->s[3] = cryc_load_le32(key + 28);

	/* Starts with a zero accumulator and no pending bytes. */
	for (index = 0; index < 5U; index++)
		mac->accumulator[index] = 0;
	memset(mac->buffer, 0, sizeof(mac->buffer));
	mac->used = 0;

	/* The copy of r is secret. */
	cryc_wipe(clamped, sizeof(clamped));
}

/*
 * Absorbs size bytes (each 16-byte block with its 0x01 byte appended,
 * 2.5); data may be NULL only when size is 0.
 */
void
cryc_poly1305_update(
	struct cryc_poly1305 *mac,
	const uint8_t *data,
	size_t size)
{
	size_t room;
	size_t take;

	/* With no bytes there is nothing to absorb, and data may be NULL. */
	if (size == 0)
		return;

	/* Completes the pending block first, when there is one. */
	if (mac->used > 0U) {
		room = POLY1305_BLOCK - mac->used;
		take = size;
		if (take > room)
			take = room;
		memcpy(mac->buffer + mac->used, data, take);
		mac->used += take;
		data += take;
		size -= take;

		/* Waits for more bytes when the block is still not full. */
		if (mac->used < POLY1305_BLOCK)
			return;

		/* Absorbs the completed block. */
		poly1305_absorb_block(mac, mac->buffer, POLY1305_FULL_BLOCK_BIT);
		mac->used = 0;
	}

	/* Absorbs every whole block straight from the caller's bytes. */
	while (size >= POLY1305_BLOCK) {
		poly1305_absorb_block(mac, data, POLY1305_FULL_BLOCK_BIT);
		data += POLY1305_BLOCK;
		size -= POLY1305_BLOCK;
	}

	/* Keeps the rest, fewer than 16 bytes, for the next call or the end. */
	if (size > 0) {
		memcpy(mac->buffer, data, size);
		mac->used = size;
	}
}

/*
 * Absorbs the last partial block, reduces modulo 2^130 - 5, adds s, writes
 * the 16-byte tag, and wipes *mac.
 */
void
cryc_poly1305_final(
	struct cryc_poly1305 *mac,
	uint8_t *tag)
{
	uint32_t h[5];
	uint32_t g[5];
	uint32_t word[4];
	uint32_t carry;
	uint32_t take_g;
	uint64_t sum;
	unsigned int index;

	/*
	 * Absorbs the last partial block.  The byte after its data is 0x01 and
	 * the rest of the 16 bytes zero, which already places the bit beyond
	 * the block, so no further bit is added.
	 */
	if (mac->used > 0U) {
		mac->buffer[mac->used] = 1;
		for (index = (unsigned int)mac->used + 1U; index < POLY1305_BLOCK; index++)
			mac->buffer[index] = 0;
		poly1305_absorb_block(mac, mac->buffer, 0);
	}

	/* Takes the accumulator. */
	for (index = 0; index < 5U; index++)
		h[index] = mac->accumulator[index];

	/*
	 * Carries between the limbs so that each is below 2^26.  Only limb 1
	 * can be above that when the last block was absorbed, by less than
	 * 2^11, so the carries ripple at most once around: out of limb 4 they
	 * return to limb 0 multiplied by 5, since 2^130 is 5 modulo 2^130 - 5.
	 */
	carry = h[1] >> 26;
	h[1] &= POLY1305_LIMB_MASK;
	h[2] += carry;
	carry = h[2] >> 26;
	h[2] &= POLY1305_LIMB_MASK;
	h[3] += carry;
	carry = h[3] >> 26;
	h[3] &= POLY1305_LIMB_MASK;
	h[4] += carry;
	carry = h[4] >> 26;
	h[4] &= POLY1305_LIMB_MASK;
	h[0] += carry * 5U;
	carry = h[0] >> 26;
	h[0] &= POLY1305_LIMB_MASK;
	h[1] += carry;

	/*
	 * Computes g = h + 5 - 2^130, which is h - (2^130 - 5) when h is at
	 * least 2^130 - 5.  The carry out of limb 4 of h + 5 says which case it
	 * is: it is 1 exactly when h + 5 reaches 2^130.
	 */
	carry = 5U;
	for (index = 0; index < 5U; index++) {
		g[index] = h[index] + carry;
		carry = g[index] >> 26;
		g[index] &= POLY1305_LIMB_MASK;
	}

	/* Takes g where the carry is 1 and h where it is 0, with a mask. */
	take_g = 0U - carry;
	for (index = 0; index < 5U; index++)
		h[index] = (g[index] & take_g) | (h[index] & ~take_g);

	/*
	 * Packs the five limbs into four 32-bit words, dropping the bits
	 * above 2^128: limb n starts at bit 26 * n.
	 */
	word[0] = h[0] | (h[1] << 26);
	word[1] = (h[1] >> 6) | (h[2] << 20);
	word[2] = (h[2] >> 12) | (h[3] << 14);
	word[3] = (h[3] >> 18) | (h[4] << 8);

	/* Adds s modulo 2^128, a word at a time with the carry. */
	sum = 0;
	for (index = 0; index < 4U; index++) {
		sum = (uint64_t)word[index] + (uint64_t)mac->s[index] + (sum >> 32);
		word[index] = (uint32_t)(sum & 0xffffffffU);
	}

	/* Writes the tag little-endian. */
	cryc_store_le32(tag, word[0]);
	cryc_store_le32(tag + 4, word[1]);
	cryc_store_le32(tag + 8, word[2]);
	cryc_store_le32(tag + 12, word[3]);

	/* The accumulator, r and s are secret, and so are the copies here. */
	cryc_wipe(h, sizeof(h));
	cryc_wipe(g, sizeof(g));
	cryc_wipe(word, sizeof(word));
	cryc_wipe(mac, sizeof(*mac));
}

/*
 * Encrypts and authenticates with AEAD_CHACHA20_POLY1305 (2.8).
 *
 * Returns 0 or EOVERFLOW.
 */
int
cryc_chacha20_poly1305_seal(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	uint8_t *tag)
{
	uint32_t state[CHACHA_STATE_WORDS];
	uint64_t blocks;

	/* Counts the blocks of the plaintext: the whole ones and a part of one. */
	blocks = (uint64_t)size / 64U;
	if (((uint64_t)size % 64U) != 0U)
		blocks++;

	/* Refuses a message whose blocks 1 and up would pass 2^32 - 1. */
	if (blocks > (uint64_t)AEAD_MAX_BLOCKS)
		return EOVERFLOW;

	/* Encrypts the plaintext with the blocks from 1. */
	chacha_load_key(state, key);
	state[12] = 1;
	state[13] = cryc_load_le32(nonce);
	state[14] = cryc_load_le32(nonce + 4);
	state[15] = cryc_load_le32(nonce + 8);
	chacha_stream(state, 0, in, out, size, 0xffU);
	cryc_wipe(state, sizeof(state));

	/* Authenticates the ciphertext, which is now in out. */
	aead_compute_tag(key, nonce, aad, aad_size, out, size, tag);

	/* Succeeded: out holds the ciphertext and tag the 16-byte tag. */
	return 0;
}

/*
 * Checks and decrypts with AEAD_CHACHA20_POLY1305 (2.8).
 *
 * The tag is computed over the ciphertext and compared in constant time;
 * the decryption writes each byte through a mask that is all ones only when
 * the tag matched, so on a mismatch out is filled with zeros and the
 * plaintext is never stored.  Neither the time nor an address depends on
 * whether the tag matched.
 *
 * Returns 0, EBADMSG or EOVERFLOW.
 */
int
cryc_chacha20_poly1305_open(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	const uint8_t *tag)
{
	uint32_t state[CHACHA_STATE_WORDS];
	uint8_t computed[CRYC_POLY1305_TAG];
	uint64_t blocks;
	uint32_t good;
	uint32_t keep;
	int error;

	/* Counts the blocks of the ciphertext: the whole ones and a part of one. */
	blocks = (uint64_t)size / 64U;
	if (((uint64_t)size % 64U) != 0U)
		blocks++;

	/* Refuses a message whose blocks 1 and up would pass 2^32 - 1. */
	if (blocks > (uint64_t)AEAD_MAX_BLOCKS)
		return EOVERFLOW;

	/* Computes the tag over the ciphertext, which is still in in. */
	aead_compute_tag(key, nonce, aad, aad_size, in, size, computed);

	/*
	 * Compares the tags into a mask: all ones when they are equal, zero
	 * otherwise.  The comparison result is 1 or 0 and never decides a
	 * branch.
	 */
	good = (uint32_t)cryc_ct_equal(computed, tag, CRYC_POLY1305_TAG);
	keep = 0U - good;

	/* Decrypts, storing the plaintext bytes where the tag matched and zero elsewhere. */
	chacha_load_key(state, key);
	state[12] = 1;
	state[13] = cryc_load_le32(nonce);
	state[14] = cryc_load_le32(nonce + 4);
	state[15] = cryc_load_le32(nonce + 8);
	chacha_stream(state, 0, in, out, size, (uint8_t)(keep & 0xffU));
	cryc_wipe(state, sizeof(state));
	cryc_wipe(computed, sizeof(computed));

	/* The error value is EBADMSG exactly when the mask is zero. */
	error = (int)(~keep & (uint32_t)EBADMSG);

	/* Reports 0 for a good tag and EBADMSG for a bad one. */
	return error;
}

/*
 * Rotates a 32-bit word left by count bits, count from 1 to 31 (2.1).
 */
static uint32_t
chacha_rotate_left(
	uint32_t value,
	unsigned int count)
{
	uint32_t rotated;

	/* The bits shifted out on the left come back on the right. */
	rotated = (value << count) | (value >> (32U - count));

	/* Reports the rotated word. */
	return rotated;
}

/*
 * Runs the quarter round on the four state words at the given indices
 * (2.1, 2.2).
 */
static void
chacha_quarter_round(
	uint32_t *state,
	unsigned int a,
	unsigned int b,
	unsigned int c,
	unsigned int d)
{
	/* The first line of the quarter round. */
	state[a] += state[b];
	state[d] ^= state[a];
	state[d] = chacha_rotate_left(state[d], 16);

	/* The second line. */
	state[c] += state[d];
	state[b] ^= state[c];
	state[b] = chacha_rotate_left(state[b], 12);

	/* The third line. */
	state[a] += state[b];
	state[d] ^= state[a];
	state[d] = chacha_rotate_left(state[d], 8);

	/* The fourth line. */
	state[c] += state[d];
	state[b] ^= state[c];
	state[b] = chacha_rotate_left(state[b], 7);
}

/*
 * Runs one column round and one diagonal round on the state (2.3).
 */
static void
chacha_double_round(
	uint32_t *state)
{
	/* The column round: the four columns of the 4 by 4 matrix. */
	chacha_quarter_round(state, 0, 4, 8, 12);
	chacha_quarter_round(state, 1, 5, 9, 13);
	chacha_quarter_round(state, 2, 6, 10, 14);
	chacha_quarter_round(state, 3, 7, 11, 15);

	/* The diagonal round: the four diagonals. */
	chacha_quarter_round(state, 0, 5, 10, 15);
	chacha_quarter_round(state, 1, 6, 11, 12);
	chacha_quarter_round(state, 2, 7, 8, 13);
	chacha_quarter_round(state, 3, 4, 9, 14);
}

/*
 * Fills the constant words and the key words of a state (2.3).
 */
static void
chacha_load_key(
	uint32_t *state,
	const uint8_t *key)
{
	unsigned int index;

	/* Words 0 to 3 are the constants. */
	for (index = 0; index < 4U; index++)
		state[index] = chacha_constant[index];

	/* Words 4 to 11 are the key, eight little-endian words. */
	for (index = 0; index < 8U; index++)
		state[4U + index] = cryc_load_le32(key + 4U * index);
}

/*
 * Runs the block function on a complete state and serializes the 64-byte
 * result (2.3).
 */
static void
chacha_permute(
	const uint32_t *state,
	uint8_t *out)
{
	uint32_t work[CHACHA_STATE_WORDS];
	unsigned int index;

	/* Works on a copy, so the input state can be reused for the next block. */
	for (index = 0; index < CHACHA_STATE_WORDS; index++)
		work[index] = state[index];

	/* Runs the 20 rounds, as ten double rounds. */
	for (index = 0; index < CHACHA_DOUBLE_ROUNDS; index++)
		chacha_double_round(work);

	/* Adds the input state to the result and serializes each word little-endian. */
	for (index = 0; index < CHACHA_STATE_WORDS; index++)
		cryc_store_le32(out + 4U * index, work[index] + state[index]);

	/* The working copy holds key material. */
	cryc_wipe(work, sizeof(work));
}

/*
 * XORs size bytes with the keystream of consecutive blocks, keeping each
 * result byte only where keep_mask has ones (2.4).
 */
static void
chacha_stream(
	uint32_t *state,
	int wide_counter,
	const uint8_t *in,
	uint8_t *out,
	size_t size,
	uint8_t keep_mask)
{
	uint8_t keystream[CRYC_CHACHA20_BLOCK];
	size_t done;
	size_t chunk;
	size_t index;
	uint8_t byte;

	/*
	 * Takes one block at a time.  The callers have checked that the counter
	 * does not pass its limit, so the increment after the last block may
	 * wrap harmlessly.
	 */
	done = 0;
	while (done < size) {
		chacha_permute(state, keystream);

		/* Limits the chunk to one block. */
		chunk = size - done;
		if (chunk > CRYC_CHACHA20_BLOCK)
			chunk = CRYC_CHACHA20_BLOCK;

		/* Combines the keystream with the input; in and out may be the same buffer. */
		for (index = 0; index < chunk; index++) {
			byte = (uint8_t)(in[done + index] ^ keystream[index]);
			out[done + index] = (uint8_t)(byte & keep_mask);
		}

		/* Moves past the bytes taken. */
		done += chunk;

		/* Advances the counter: its low word, and for the 64-bit form its high word on a carry. */
		state[CHACHA_COUNTER_WORD]++;
		if (wide_counter != 0 && state[CHACHA_COUNTER_WORD] == 0U)
			state[CHACHA_COUNTER_WORD + 1U]++;
	}

	/* The keystream is secret. */
	cryc_wipe(keystream, sizeof(keystream));
}

/*
 * Reduces the accumulator by one block multiplication: adds the block and
 * multiplies by r (2.5).
 */
static void
poly1305_absorb_block(
	struct cryc_poly1305 *mac,
	const uint8_t *block,
	uint32_t high_bit)
{
	uint32_t h0;
	uint32_t h1;
	uint32_t h2;
	uint32_t h3;
	uint32_t h4;
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
	uint32_t r4;
	uint32_t five_r1;
	uint32_t five_r2;
	uint32_t five_r3;
	uint32_t five_r4;
	uint64_t d0;
	uint64_t d1;
	uint64_t d2;
	uint64_t d3;
	uint64_t d4;
	uint64_t carry;

	/*
	 * Adds the block to the accumulator, limb by limb.  The block is read
	 * as a 128-bit little-endian number split at bits 26, 52, 78 and 104;
	 * high_bit is the bit beyond a full block.
	 */
	h0 = mac->accumulator[0] + (cryc_load_le32(block) & POLY1305_LIMB_MASK);
	h1 = mac->accumulator[1] + ((cryc_load_le32(block + 3) >> 2) & POLY1305_LIMB_MASK);
	h2 = mac->accumulator[2] + ((cryc_load_le32(block + 6) >> 4) & POLY1305_LIMB_MASK);
	h3 = mac->accumulator[3] + ((cryc_load_le32(block + 9) >> 6) & POLY1305_LIMB_MASK);
	h4 = mac->accumulator[4] + ((cryc_load_le32(block + 12) >> 8) | high_bit);

	/*
	 * Takes r, and 5 times r for the products that reach 2^130: a limb
	 * product of weight 2^(26 * n) with n at least 5 is 5 * 2^(26 * (n -
	 * 5)) modulo 2^130 - 5.
	 */
	r0 = mac->r[0];
	r1 = mac->r[1];
	r2 = mac->r[2];
	r3 = mac->r[3];
	r4 = mac->r[4];
	five_r1 = r1 * 5U;
	five_r2 = r2 * 5U;
	five_r3 = r3 * 5U;
	five_r4 = r4 * 5U;

	/*
	 * Multiplies, collecting the products of equal weight.  A limb of h is
	 * below 2^27 and a limb of r or 5r below 2^29, so each sum of five
	 * products is below 2^59.
	 */
	d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * five_r4 + (uint64_t)h2 * five_r3 + (uint64_t)h3 * five_r2 + (uint64_t)h4 * five_r1;
	d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * five_r4 + (uint64_t)h3 * five_r3 + (uint64_t)h4 * five_r2;
	d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * five_r4 + (uint64_t)h4 * five_r3;
	d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * five_r4;
	d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

	/*
	 * Carries each sum into the next limb.  The carry out of limb 4 is of
	 * weight 2^130, so it returns to limb 0 multiplied by 5.
	 */
	carry = d0 >> 26;
	d0 &= POLY1305_LIMB_MASK;
	d1 += carry;
	carry = d1 >> 26;
	d1 &= POLY1305_LIMB_MASK;
	d2 += carry;
	carry = d2 >> 26;
	d2 &= POLY1305_LIMB_MASK;
	d3 += carry;
	carry = d3 >> 26;
	d3 &= POLY1305_LIMB_MASK;
	d4 += carry;
	carry = d4 >> 26;
	d4 &= POLY1305_LIMB_MASK;
	d0 += carry * 5U;
	carry = d0 >> 26;
	d0 &= POLY1305_LIMB_MASK;
	d1 += carry;

	/* Stores the accumulator: limb 1 may be a little above 2^26. */
	mac->accumulator[0] = (uint32_t)d0;
	mac->accumulator[1] = (uint32_t)d1;
	mac->accumulator[2] = (uint32_t)d2;
	mac->accumulator[3] = (uint32_t)d3;
	mac->accumulator[4] = (uint32_t)d4;
}

/*
 * Makes the Poly1305 key from block 0 of the keystream and computes the
 * AEAD tag of the ciphertext (2.6, 2.8).
 */
static void
aead_compute_tag(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *ciphertext,
	size_t size,
	uint8_t *tag)
{
	uint8_t block[CRYC_CHACHA20_BLOCK];
	uint8_t lengths[16];
	struct cryc_poly1305 mac;
	size_t padding;

	/* Generates the one-time key: the first 32 bytes of block 0. */
	cryc_chacha20_block(key, 0, nonce, block);
	cryc_poly1305_init(&mac, block);
	cryc_wipe(block, sizeof(block));

	/* Absorbs the additional data and zeros up to a multiple of 16 bytes. */
	cryc_poly1305_update(&mac, aad, aad_size);
	padding = (POLY1305_BLOCK - (aad_size % POLY1305_BLOCK)) % POLY1305_BLOCK;
	cryc_poly1305_update(&mac, poly1305_zero_padding, padding);

	/* Absorbs the ciphertext and its padding. */
	cryc_poly1305_update(&mac, ciphertext, size);
	padding = (POLY1305_BLOCK - (size % POLY1305_BLOCK)) % POLY1305_BLOCK;
	cryc_poly1305_update(&mac, poly1305_zero_padding, padding);

	/* Absorbs the two lengths as 64-bit little-endian integers. */
	cryc_store_le64(lengths, (uint64_t)aad_size);
	cryc_store_le64(lengths + 8, (uint64_t)size);
	cryc_poly1305_update(&mac, lengths, sizeof(lengths));

	/* Finishes the tag, which wipes the Poly1305 state. */
	cryc_poly1305_final(&mac, tag);
}
