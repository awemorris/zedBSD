/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's cryptography (ws143-p004, see crypto.h).
 *
 * AES-128 follows FIPS-197 with a table S-box (the round keys and the
 * state live on the stack and are cleared after use; the S-box's lookups
 * are not constant-time, which a pairing's short-lived keys accept);
 * AES-CMAC follows RFC 4493; the Security Manager's functions follow the
 * Core 5.4 Vol 3 Part H §2.2.  The host test checks each against the
 * Core's sample data (Appendix D) and the RFCs' examples.
 */

#include "userland/base/bluetoothd/crypto.h"

#include <string.h>

/* AES-128's rounds, and the size of its expanded key. */
#define CRYPTO_ROUNDS		10U
#define CRYPTO_SCHEDULE		(BTD_BLOCK * (CRYPTO_ROUNDS + 1U))

/* The constant of CMAC's subkeys (R_128). */
#define CRYPTO_RB		0x87U

/* f5's salt and key ID ("btle"), and the length of its output (256 bits). */
static const uint8_t crypto_f5_salt[BTD_BLOCK] = {
	0x6cU, 0x88U, 0x83U, 0x91U, 0xaaU, 0xf5U, 0xa5U, 0x38U, 0x60U, 0x37U, 0x0bU, 0xdbU, 0x5aU, 0x60U, 0x83U, 0xbeU
};
static const uint8_t crypto_f5_key_id[4] = { 0x62U, 0x74U, 0x6cU, 0x65U };
static const uint8_t crypto_f5_length[2] = { 0x01U, 0x00U };

/*
 * AES's substitution box (FIPS-197 §5.1.1, the multiplicative inverse in
 * GF(2^8) followed by the affine transformation).  A constant table.
 */
static const uint8_t crypto_sbox[256] = {
	0x63U, 0x7cU, 0x77U, 0x7bU, 0xf2U, 0x6bU, 0x6fU, 0xc5U, 0x30U, 0x01U, 0x67U, 0x2bU, 0xfeU, 0xd7U, 0xabU, 0x76U,
	0xcaU, 0x82U, 0xc9U, 0x7dU, 0xfaU, 0x59U, 0x47U, 0xf0U, 0xadU, 0xd4U, 0xa2U, 0xafU, 0x9cU, 0xa4U, 0x72U, 0xc0U,
	0xb7U, 0xfdU, 0x93U, 0x26U, 0x36U, 0x3fU, 0xf7U, 0xccU, 0x34U, 0xa5U, 0xe5U, 0xf1U, 0x71U, 0xd8U, 0x31U, 0x15U,
	0x04U, 0xc7U, 0x23U, 0xc3U, 0x18U, 0x96U, 0x05U, 0x9aU, 0x07U, 0x12U, 0x80U, 0xe2U, 0xebU, 0x27U, 0xb2U, 0x75U,
	0x09U, 0x83U, 0x2cU, 0x1aU, 0x1bU, 0x6eU, 0x5aU, 0xa0U, 0x52U, 0x3bU, 0xd6U, 0xb3U, 0x29U, 0xe3U, 0x2fU, 0x84U,
	0x53U, 0xd1U, 0x00U, 0xedU, 0x20U, 0xfcU, 0xb1U, 0x5bU, 0x6aU, 0xcbU, 0xbeU, 0x39U, 0x4aU, 0x4cU, 0x58U, 0xcfU,
	0xd0U, 0xefU, 0xaaU, 0xfbU, 0x43U, 0x4dU, 0x33U, 0x85U, 0x45U, 0xf9U, 0x02U, 0x7fU, 0x50U, 0x3cU, 0x9fU, 0xa8U,
	0x51U, 0xa3U, 0x40U, 0x8fU, 0x92U, 0x9dU, 0x38U, 0xf5U, 0xbcU, 0xb6U, 0xdaU, 0x21U, 0x10U, 0xffU, 0xf3U, 0xd2U,
	0xcdU, 0x0cU, 0x13U, 0xecU, 0x5fU, 0x97U, 0x44U, 0x17U, 0xc4U, 0xa7U, 0x7eU, 0x3dU, 0x64U, 0x5dU, 0x19U, 0x73U,
	0x60U, 0x81U, 0x4fU, 0xdcU, 0x22U, 0x2aU, 0x90U, 0x88U, 0x46U, 0xeeU, 0xb8U, 0x14U, 0xdeU, 0x5eU, 0x0bU, 0xdbU,
	0xe0U, 0x32U, 0x3aU, 0x0aU, 0x49U, 0x06U, 0x24U, 0x5cU, 0xc2U, 0xd3U, 0xacU, 0x62U, 0x91U, 0x95U, 0xe4U, 0x79U,
	0xe7U, 0xc8U, 0x37U, 0x6dU, 0x8dU, 0xd5U, 0x4eU, 0xa9U, 0x6cU, 0x56U, 0xf4U, 0xeaU, 0x65U, 0x7aU, 0xaeU, 0x08U,
	0xbaU, 0x78U, 0x25U, 0x2eU, 0x1cU, 0xa6U, 0xb4U, 0xc6U, 0xe8U, 0xddU, 0x74U, 0x1fU, 0x4bU, 0xbdU, 0x8bU, 0x8aU,
	0x70U, 0x3eU, 0xb5U, 0x66U, 0x48U, 0x03U, 0xf6U, 0x0eU, 0x61U, 0x35U, 0x57U, 0xb9U, 0x86U, 0xc1U, 0x1dU, 0x9eU,
	0xe1U, 0xf8U, 0x98U, 0x11U, 0x69U, 0xd9U, 0x8eU, 0x94U, 0x9bU, 0x1eU, 0x87U, 0xe9U, 0xceU, 0x55U, 0x28U, 0xdfU,
	0x8cU, 0xa1U, 0x89U, 0x0dU, 0xbfU, 0xe6U, 0x42U, 0x68U, 0x41U, 0x99U, 0x2dU, 0x0fU, 0xb0U, 0x54U, 0xbbU, 0x16U
};

static void crypto_expand(const uint8_t *key, uint8_t *schedule);
static uint8_t crypto_double(uint8_t value);
static void crypto_mix(uint8_t *state);
static void crypto_shift(uint8_t *state);
static void crypto_subkey(const uint8_t *input, uint8_t *output);
static void crypto_xor(uint8_t *into, const uint8_t *from, size_t length);

/*
 * Encrypts one 16-byte block with AES-128.
 */
void
btd_aes_encrypt(
	const uint8_t *key,
	const uint8_t *input,
	uint8_t *output)
{
	uint8_t schedule[CRYPTO_SCHEDULE];
	uint8_t state[BTD_BLOCK];
	unsigned round;
	unsigned index;

	/* The round keys, and the state with the first one added. */
	crypto_expand(key, schedule);
	memcpy(state, input, BTD_BLOCK);
	crypto_xor(state, schedule, BTD_BLOCK);

	/* The rounds: substitution, row shifts, column mixing (not in the last), the round key. */
	for (round = 1U; round <= CRYPTO_ROUNDS; round++) {
		for (index = 0U; index < BTD_BLOCK; index++)
			state[index] = crypto_sbox[state[index]];
		crypto_shift(state);
		if (round != CRYPTO_ROUNDS)
			crypto_mix(state);
		crypto_xor(state, schedule + round * BTD_BLOCK, BTD_BLOCK);
	}

	/* The block, and nothing of the key left on the stack. */
	memcpy(output, state, BTD_BLOCK);
	memset(schedule, 0, sizeof(schedule));
	memset(state, 0, sizeof(state));
}

/*
 * Computes AES-CMAC (RFC 4493) of a message with a key.
 */
void
btd_cmac(
	const uint8_t *key,
	const uint8_t *message,
	size_t length,
	uint8_t *mac)
{
	uint8_t zero[BTD_BLOCK];
	uint8_t first[BTD_BLOCK];
	uint8_t second[BTD_BLOCK];
	uint8_t block[BTD_BLOCK];
	uint8_t chain[BTD_BLOCK];
	size_t blocks;
	size_t last;
	size_t index;

	/* The subkeys: L = AES(K, 0), K1 = L doubled, K2 = K1 doubled. */
	memset(zero, 0, sizeof(zero));
	btd_aes_encrypt(key, zero, block);
	crypto_subkey(block, first);
	crypto_subkey(first, second);

	/* How many blocks, and how long the last one is (an empty message is one empty block). */
	blocks = 1U;
	last = 0U;
	if (length != 0U) {
		blocks = (length + BTD_BLOCK - 1U) / BTD_BLOCK;
		last = (length - 1U) % BTD_BLOCK + 1U;
	}

	/* The chain through every block but the last. */
	memset(chain, 0, sizeof(chain));
	for (index = 0U; index + 1U < blocks; index++) {
		crypto_xor(chain, message + index * BTD_BLOCK, BTD_BLOCK);
		btd_aes_encrypt(key, chain, chain);
	}

	/* The last block: whole with K1, or padded with 10..0 and K2. */
	memset(block, 0, sizeof(block));
	memcpy(block, message + (blocks - 1U) * BTD_BLOCK, last);
	if (last == BTD_BLOCK) {
		crypto_xor(block, first, BTD_BLOCK);
	} else {
		block[last] = 0x80U;
		crypto_xor(block, second, BTD_BLOCK);
	}

	/* The tag. */
	crypto_xor(chain, block, BTD_BLOCK);
	btd_aes_encrypt(key, chain, mac);

	/* Nothing of the subkeys left on the stack. */
	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));
	memset(block, 0, sizeof(block));
	memset(chain, 0, sizeof(chain));
}

/*
 * Computes ah, the random address hash: the low 24 bits of e(k, r padded).
 */
void
btd_smp_ah(
	const uint8_t *key,
	const uint8_t *r,
	uint8_t *hash)
{
	uint8_t block[BTD_BLOCK];

	/* r' = 104 zero bits and r. */
	memset(block, 0, sizeof(block));
	memcpy(block + 13, r, 3U);
	btd_aes_encrypt(key, block, block);

	/* The low 24 bits. */
	memcpy(hash, block + 13, 3U);
}

/*
 * Computes c1, the legacy pairing's confirm value:
 * e(k, e(k, r XOR p1) XOR p2), p1 = pres || preq || rat || iat,
 * p2 = 32 zero bits || ia || ra (pres and preq are the 7 bytes of the PDUs,
 * the opcode as their least significant byte).
 */
void
btd_smp_c1(
	const uint8_t *key,
	const uint8_t *r,
	const uint8_t *pres,
	const uint8_t *preq,
	uint8_t rat,
	uint8_t iat,
	const uint8_t *ia,
	const uint8_t *ra,
	uint8_t *confirm)
{
	uint8_t p1[BTD_BLOCK];
	uint8_t p2[BTD_BLOCK];
	uint8_t block[BTD_BLOCK];

	/* p1 and p2. */
	memcpy(p1, pres, 7U);
	memcpy(p1 + 7, preq, 7U);
	p1[14] = rat;
	p1[15] = iat;
	memset(p2, 0, 4U);
	memcpy(p2 + 4, ia, 6U);
	memcpy(p2 + 10, ra, 6U);

	/* The two encryptions. */
	memcpy(block, r, BTD_BLOCK);
	crypto_xor(block, p1, BTD_BLOCK);
	btd_aes_encrypt(key, block, block);
	crypto_xor(block, p2, BTD_BLOCK);
	btd_aes_encrypt(key, block, confirm);
}

/*
 * Computes s1, the legacy pairing's short-term key: e(k, r1' || r2'), the
 * low 64 bits of each nonce.
 */
void
btd_smp_s1(
	const uint8_t *key,
	const uint8_t *r1,
	const uint8_t *r2,
	uint8_t *stk)
{
	uint8_t block[BTD_BLOCK];

	/* The two halves. */
	memcpy(block, r1 + 8, 8U);
	memcpy(block + 8, r2 + 8, 8U);

	/* The key. */
	btd_aes_encrypt(key, block, stk);
}

/*
 * Computes f4, LE Secure Connections' confirm value: AES-CMAC_X(U || V || Z).
 */
void
btd_smp_f4(
	const uint8_t *u,
	const uint8_t *v,
	const uint8_t *x,
	uint8_t z,
	uint8_t *confirm)
{
	uint8_t message[65];

	/* U, V and Z. */
	memcpy(message, u, 32U);
	memcpy(message + 32, v, 32U);
	message[64] = z;

	/* The CMAC. */
	btd_cmac(x, message, sizeof(message), confirm);
}

/*
 * Computes f5, LE Secure Connections' key generation: T = AES-CMAC_SALT(W),
 * then MacKey and LTK as AES-CMAC_T of the counter (0, 1), "btle", N1, N2,
 * A1, A2 and the length 256.
 */
void
btd_smp_f5(
	const uint8_t *w,
	const uint8_t *n1,
	const uint8_t *n2,
	const uint8_t *a1,
	const uint8_t *a2,
	uint8_t *mac_key,
	uint8_t *ltk)
{
	uint8_t key[BTD_BLOCK];
	uint8_t message[53];

	/* T. */
	btd_cmac(crypto_f5_salt, w, 32U, key);

	/* The message after its counter. */
	memcpy(message + 1, crypto_f5_key_id, 4U);
	memcpy(message + 5, n1, BTD_BLOCK);
	memcpy(message + 21, n2, BTD_BLOCK);
	memcpy(message + 37, a1, BTD_SMP_ADDRESS);
	memcpy(message + 44, a2, BTD_SMP_ADDRESS);
	memcpy(message + 51, crypto_f5_length, 2U);

	/* MacKey with counter 0, LTK with counter 1. */
	message[0] = 0U;
	btd_cmac(key, message, sizeof(message), mac_key);
	message[0] = 1U;
	btd_cmac(key, message, sizeof(message), ltk);

	/* Nothing of T left on the stack. */
	memset(key, 0, sizeof(key));
}

/*
 * Computes f6, LE Secure Connections' DHKey check:
 * AES-CMAC_W(N1 || N2 || R || IOcap || A1 || A2).
 */
void
btd_smp_f6(
	const uint8_t *w,
	const uint8_t *n1,
	const uint8_t *n2,
	const uint8_t *r,
	const uint8_t *io_capability,
	const uint8_t *a1,
	const uint8_t *a2,
	uint8_t *check)
{
	uint8_t message[65];

	/* The fields in order. */
	memcpy(message, n1, BTD_BLOCK);
	memcpy(message + 16, n2, BTD_BLOCK);
	memcpy(message + 32, r, BTD_BLOCK);
	memcpy(message + 48, io_capability, 3U);
	memcpy(message + 51, a1, BTD_SMP_ADDRESS);
	memcpy(message + 58, a2, BTD_SMP_ADDRESS);

	/* The CMAC. */
	btd_cmac(w, message, sizeof(message), check);
}

/*
 * Computes g2, numeric comparison's value: the low 32 bits of
 * AES-CMAC_X(U || V || Y) (the six digits shown are this mod 10^6).
 */
uint32_t
btd_smp_g2(
	const uint8_t *u,
	const uint8_t *v,
	const uint8_t *x,
	const uint8_t *y)
{
	uint8_t message[80];
	uint8_t mac[BTD_BLOCK];
	uint32_t value;

	/* U, V and Y. */
	memcpy(message, u, 32U);
	memcpy(message + 32, v, 32U);
	memcpy(message + 64, y, BTD_BLOCK);

	/* The CMAC's low 32 bits. */
	btd_cmac(x, message, sizeof(message), mac);
	value = (uint32_t)mac[12] << 24;
	value |= (uint32_t)mac[13] << 16;
	value |= (uint32_t)mac[14] << 8;
	value |= (uint32_t)mac[15];

	/* Succeeded: the value. */
	return value;
}

/* Expands a 128-bit key into the 11 round keys (FIPS-197 §5.2). */
static void
crypto_expand(
	const uint8_t *key,
	uint8_t *schedule)
{
	uint8_t word[4];
	uint8_t first;
	uint8_t constant;
	unsigned index;

	/* The key is the first round key. */
	memcpy(schedule, key, BTD_BLOCK);

	/* Each next word: the word before, every fourth one rotated, substituted and given the round constant. */
	constant = 0x01U;
	for (index = 4U; index < 4U * (CRYPTO_ROUNDS + 1U); index++) {
		memcpy(word, schedule + (index - 1U) * 4U, 4U);
		if ((index % 4U) == 0U) {
			first = word[0];
			word[0] = (uint8_t)(crypto_sbox[word[1]] ^ constant);
			word[1] = crypto_sbox[word[2]];
			word[2] = crypto_sbox[word[3]];
			word[3] = crypto_sbox[first];
			constant = crypto_double(constant);
		}

		/* XORed with the word four before. */
		schedule[index * 4U] = (uint8_t)(schedule[(index - 4U) * 4U] ^ word[0]);
		schedule[index * 4U + 1U] = (uint8_t)(schedule[(index - 4U) * 4U + 1U] ^ word[1]);
		schedule[index * 4U + 2U] = (uint8_t)(schedule[(index - 4U) * 4U + 2U] ^ word[2]);
		schedule[index * 4U + 3U] = (uint8_t)(schedule[(index - 4U) * 4U + 3U] ^ word[3]);
	}
}

/* Multiplies by x in GF(2^8) (xtime). */
static uint8_t
crypto_double(
	uint8_t value)
{
	uint8_t doubled;

	/* Shifted, reduced by the field's polynomial when the top bit falls out. */
	doubled = (uint8_t)(value << 1);
	if ((value & 0x80U) != 0U)
		doubled ^= 0x1bU;

	/* Succeeded. */
	return doubled;
}

/* Mixes each column (FIPS-197 §5.1.3). */
static void
crypto_mix(
	uint8_t *state)
{
	uint8_t *column;
	uint8_t all;
	uint8_t first;
	unsigned index;

	/* Each column of four bytes. */
	for (index = 0U; index < 4U; index++) {
		column = state + index * 4U;
		all = (uint8_t)(column[0] ^ column[1] ^ column[2] ^ column[3]);
		first = column[0];
		column[0] ^= (uint8_t)(all ^ crypto_double((uint8_t)(column[0] ^ column[1])));
		column[1] ^= (uint8_t)(all ^ crypto_double((uint8_t)(column[1] ^ column[2])));
		column[2] ^= (uint8_t)(all ^ crypto_double((uint8_t)(column[2] ^ column[3])));
		column[3] ^= (uint8_t)(all ^ crypto_double((uint8_t)(column[3] ^ first)));
	}
}

/* Shifts row r left by r (FIPS-197 §5.1.2); the state is column after column. */
static void
crypto_shift(
	uint8_t *state)
{
	uint8_t kept;

	/* Row 1, by one. */
	kept = state[1];
	state[1] = state[5];
	state[5] = state[9];
	state[9] = state[13];
	state[13] = kept;

	/* Row 2, by two. */
	kept = state[2];
	state[2] = state[10];
	state[10] = kept;
	kept = state[6];
	state[6] = state[14];
	state[14] = kept;

	/* Row 3, by three (one to the right). */
	kept = state[15];
	state[15] = state[11];
	state[11] = state[7];
	state[7] = state[3];
	state[3] = kept;
}

/* Doubles a block for CMAC's subkeys: shifted left one bit, R_128 added when the top bit falls out. */
static void
crypto_subkey(
	const uint8_t *input,
	uint8_t *output)
{
	unsigned index;

	/* Each byte takes the next one's top bit. */
	for (index = 0U; index < BTD_BLOCK; index++) {
		output[index] = (uint8_t)(input[index] << 1);
		if (index + 1U < BTD_BLOCK)
			output[index] |= (uint8_t)(input[index + 1U] >> 7);
	}

	/* The reduction. */
	if ((input[0] & 0x80U) != 0U)
		output[BTD_BLOCK - 1U] ^= CRYPTO_RB;
}

/* XORs bytes into others. */
static void
crypto_xor(
	uint8_t *into,
	const uint8_t *from,
	size_t length)
{
	size_t index;

	/* Each byte. */
	for (index = 0U; index < length; index++)
		into[index] ^= from[index];
}
