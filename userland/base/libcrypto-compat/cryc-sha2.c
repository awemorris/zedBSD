/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SHA-256, SHA-384 and SHA-512 (FIPS 180-4).
 *
 * The section numbers in the comments of this file are those of FIPS
 * 180-4 (August 2015).  Every operation on a message word is a rotation,
 * a shift, an exclusive or, an and, a complement or a sum modulo 2^32 (2^64
 * for SHA-384 and SHA-512), so no branch and no table index depends on
 * the data.
 */

#include <string.h>

#include "cryc-sha2.h"

/* The number of rounds of the SHA-256 compression (6.2.2 step 3). */
#define SHA256_ROUNDS	64U

/* The number of rounds of the SHA-512 compression (6.4.2 step 3). */
#define SHA512_ROUNDS	80U

/* Where the length field begins in the last block: 64 bits at the end of 64 (5.1.1). */
#define SHA256_LENGTH_OFFSET	56U

/* Where the 128-bit length field begins in the last block of 128 (5.1.2). */
#define SHA512_LENGTH_OFFSET	112U

/*
 * The SHA-256 round constants, one per round.
 *
 * They are fixed data read by index = round number (a public value); the
 * table is never written.
 *
 * Values referenced: FIPS 180-4 (August 2015) 4.2.2, the sixty-four
 * constant words K0 to K63 of SHA-224 and SHA-256, in the order given.
 */
static const uint32_t sha256_round_constant[SHA256_ROUNDS] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/*
 * The SHA-512 round constants, one per round (SHA-384 uses the same).
 *
 * They are fixed data read by index = round number (a public value); the
 * table is never written.
 *
 * Values referenced: FIPS 180-4 (August 2015) 4.2.3, the eighty constant
 * words K0 to K79 of SHA-384 and SHA-512, in the order given.
 */
static const uint64_t sha512_round_constant[SHA512_ROUNDS] = {
	CRYC_U64(0x428a2f98, 0xd728ae22), CRYC_U64(0x71374491, 0x23ef65cd),
	CRYC_U64(0xb5c0fbcf, 0xec4d3b2f), CRYC_U64(0xe9b5dba5, 0x8189dbbc),
	CRYC_U64(0x3956c25b, 0xf348b538), CRYC_U64(0x59f111f1, 0xb605d019),
	CRYC_U64(0x923f82a4, 0xaf194f9b), CRYC_U64(0xab1c5ed5, 0xda6d8118),
	CRYC_U64(0xd807aa98, 0xa3030242), CRYC_U64(0x12835b01, 0x45706fbe),
	CRYC_U64(0x243185be, 0x4ee4b28c), CRYC_U64(0x550c7dc3, 0xd5ffb4e2),
	CRYC_U64(0x72be5d74, 0xf27b896f), CRYC_U64(0x80deb1fe, 0x3b1696b1),
	CRYC_U64(0x9bdc06a7, 0x25c71235), CRYC_U64(0xc19bf174, 0xcf692694),
	CRYC_U64(0xe49b69c1, 0x9ef14ad2), CRYC_U64(0xefbe4786, 0x384f25e3),
	CRYC_U64(0x0fc19dc6, 0x8b8cd5b5), CRYC_U64(0x240ca1cc, 0x77ac9c65),
	CRYC_U64(0x2de92c6f, 0x592b0275), CRYC_U64(0x4a7484aa, 0x6ea6e483),
	CRYC_U64(0x5cb0a9dc, 0xbd41fbd4), CRYC_U64(0x76f988da, 0x831153b5),
	CRYC_U64(0x983e5152, 0xee66dfab), CRYC_U64(0xa831c66d, 0x2db43210),
	CRYC_U64(0xb00327c8, 0x98fb213f), CRYC_U64(0xbf597fc7, 0xbeef0ee4),
	CRYC_U64(0xc6e00bf3, 0x3da88fc2), CRYC_U64(0xd5a79147, 0x930aa725),
	CRYC_U64(0x06ca6351, 0xe003826f), CRYC_U64(0x14292967, 0x0a0e6e70),
	CRYC_U64(0x27b70a85, 0x46d22ffc), CRYC_U64(0x2e1b2138, 0x5c26c926),
	CRYC_U64(0x4d2c6dfc, 0x5ac42aed), CRYC_U64(0x53380d13, 0x9d95b3df),
	CRYC_U64(0x650a7354, 0x8baf63de), CRYC_U64(0x766a0abb, 0x3c77b2a8),
	CRYC_U64(0x81c2c92e, 0x47edaee6), CRYC_U64(0x92722c85, 0x1482353b),
	CRYC_U64(0xa2bfe8a1, 0x4cf10364), CRYC_U64(0xa81a664b, 0xbc423001),
	CRYC_U64(0xc24b8b70, 0xd0f89791), CRYC_U64(0xc76c51a3, 0x0654be30),
	CRYC_U64(0xd192e819, 0xd6ef5218), CRYC_U64(0xd6990624, 0x5565a910),
	CRYC_U64(0xf40e3585, 0x5771202a), CRYC_U64(0x106aa070, 0x32bbd1b8),
	CRYC_U64(0x19a4c116, 0xb8d2d0c8), CRYC_U64(0x1e376c08, 0x5141ab53),
	CRYC_U64(0x2748774c, 0xdf8eeb99), CRYC_U64(0x34b0bcb5, 0xe19b48a8),
	CRYC_U64(0x391c0cb3, 0xc5c95a63), CRYC_U64(0x4ed8aa4a, 0xe3418acb),
	CRYC_U64(0x5b9cca4f, 0x7763e373), CRYC_U64(0x682e6ff3, 0xd6b2b8a3),
	CRYC_U64(0x748f82ee, 0x5defb2fc), CRYC_U64(0x78a5636f, 0x43172f60),
	CRYC_U64(0x84c87814, 0xa1f0ab72), CRYC_U64(0x8cc70208, 0x1a6439ec),
	CRYC_U64(0x90befffa, 0x23631e28), CRYC_U64(0xa4506ceb, 0xde82bde9),
	CRYC_U64(0xbef9a3f7, 0xb2c67915), CRYC_U64(0xc67178f2, 0xe372532b),
	CRYC_U64(0xca273ece, 0xea26619c), CRYC_U64(0xd186b8c7, 0x21c0c207),
	CRYC_U64(0xeada7dd6, 0xcde0eb1e), CRYC_U64(0xf57d4f7f, 0xee6ed178),
	CRYC_U64(0x06f067aa, 0x72176fba), CRYC_U64(0x0a637dc5, 0xa2c898a6),
	CRYC_U64(0x113f9804, 0xbef90dae), CRYC_U64(0x1b710b35, 0x131c471b),
	CRYC_U64(0x28db77f5, 0x23047d84), CRYC_U64(0x32caab7b, 0x40c72493),
	CRYC_U64(0x3c9ebe0a, 0x15c9bebc), CRYC_U64(0x431d67c4, 0x9c100d4c),
	CRYC_U64(0x4cc5d4be, 0xcb3e42b6), CRYC_U64(0x597f299c, 0xfc657e2a),
	CRYC_U64(0x5fcb6fab, 0x3ad6faec), CRYC_U64(0x6c44198c, 0x4a475817)
};

/*
 * The initial hash value H(0) of SHA-256.
 *
 * Values referenced: FIPS 180-4 (August 2015) 5.3.3, H0(0) to H7(0).
 */
static const uint32_t sha256_initial_state[8] = {
	0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
	0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/*
 * The initial hash value H(0) of SHA-384.
 *
 * Values referenced: FIPS 180-4 (August 2015) 5.3.4, H0(0) to H7(0).
 */
static const uint64_t sha384_initial_state[8] = {
	CRYC_U64(0xcbbb9d5d, 0xc1059ed8),
	CRYC_U64(0x629a292a, 0x367cd507),
	CRYC_U64(0x9159015a, 0x3070dd17),
	CRYC_U64(0x152fecd8, 0xf70e5939),
	CRYC_U64(0x67332667, 0xffc00b31),
	CRYC_U64(0x8eb44a87, 0x68581511),
	CRYC_U64(0xdb0c2e0d, 0x64f98fa7),
	CRYC_U64(0x47b5481d, 0xbefa4fa4)
};

/*
 * The initial hash value H(0) of SHA-512.
 *
 * Values referenced: FIPS 180-4 (August 2015) 5.3.5, H0(0) to H7(0).
 */
static const uint64_t sha512_initial_state[8] = {
	CRYC_U64(0x6a09e667, 0xf3bcc908),
	CRYC_U64(0xbb67ae85, 0x84caa73b),
	CRYC_U64(0x3c6ef372, 0xfe94f82b),
	CRYC_U64(0xa54ff53a, 0x5f1d36f1),
	CRYC_U64(0x510e527f, 0xade682d1),
	CRYC_U64(0x9b05688c, 0x2b3e6c1f),
	CRYC_U64(0x1f83d9ab, 0xfb41bd6b),
	CRYC_U64(0x5be0cd19, 0x137e2179)
};

/* Rotates a 32-bit word right by count bits, count from 1 to 31 (ROTR of 3). */
static uint32_t sha256_rotate_right(uint32_t value, unsigned int count);

/* Rotates a 64-bit word right by count bits, count from 1 to 63 (ROTR of 3). */
static uint64_t sha512_rotate_right(uint64_t value, unsigned int count);

/* Selects bits of y where x has ones and bits of z where x has zeros (Ch, 4.2). */
static uint32_t sha256_choose(uint32_t x, uint32_t y, uint32_t z);

/* Gives the majority of each bit position of x, y and z (Maj, 4.3). */
static uint32_t sha256_majority(uint32_t x, uint32_t y, uint32_t z);

/* Computes the upper-case sigma 0 of SHA-256 (4.4). */
static uint32_t sha256_big_sigma0(uint32_t x);

/* Computes the upper-case sigma 1 of SHA-256 (4.5). */
static uint32_t sha256_big_sigma1(uint32_t x);

/* Computes the lower-case sigma 0 of SHA-256 (4.6). */
static uint32_t sha256_small_sigma0(uint32_t x);

/* Computes the lower-case sigma 1 of SHA-256 (4.7). */
static uint32_t sha256_small_sigma1(uint32_t x);

/* Selects bits of y where x has ones and bits of z where x has zeros (Ch, 4.8). */
static uint64_t sha512_choose(uint64_t x, uint64_t y, uint64_t z);

/* Gives the majority of each bit position of x, y and z (Maj, 4.9). */
static uint64_t sha512_majority(uint64_t x, uint64_t y, uint64_t z);

/* Computes the upper-case sigma 0 of SHA-512 (4.10). */
static uint64_t sha512_big_sigma0(uint64_t x);

/* Computes the upper-case sigma 1 of SHA-512 (4.11). */
static uint64_t sha512_big_sigma1(uint64_t x);

/* Computes the lower-case sigma 0 of SHA-512 (4.12). */
static uint64_t sha512_small_sigma0(uint64_t x);

/* Computes the lower-case sigma 1 of SHA-512 (4.13). */
static uint64_t sha512_small_sigma1(uint64_t x);

/* Compresses one 64-byte block into the SHA-256 intermediate hash value (6.2.2). */
static void sha256_compress(uint32_t state[8], const uint8_t *block);

/* Compresses one 128-byte block into the SHA-512 intermediate hash value (6.4.2). */
static void sha512_compress(uint64_t state[8], const uint8_t *block);

/* Starts a SHA-384 or SHA-512 computation from an initial hash value and an output size. */
static void sha512_start(struct cryc_sha512 *hash, const uint64_t *initial_state, size_t out_size);

/*
 * Starts a SHA-256 computation (the initial hash value of 5.3.3).
 *
 * hash is not NULL.  Any previous content of *hash is overwritten.
 */
void
cryc_sha256_init(
	struct cryc_sha256 *hash)
{
	size_t index;

	/* Loads the initial hash value H(0). */
	for (index = 0; index < 8; index++)
		hash->state[index] = sha256_initial_state[index];

	/* Starts with an empty partial block and no bytes absorbed. */
	memset(hash->block, 0, sizeof(hash->block));
	hash->used = 0;
	hash->total = 0;
}

/*
 * Absorbs size bytes (6.2.2 for each complete block, the rest kept).
 *
 * data may be NULL only when size is 0.  May be called any number of
 * times between _init and _final.
 */
void
cryc_sha256_update(
	struct cryc_sha256 *hash,
	const uint8_t *data,
	size_t size)
{
	size_t room;
	size_t chunk;

	/* Nothing to absorb. */
	if (size == 0)
		return;

	/* Missing data with a length is a caller's mistake that must not be read. */
	if (data == NULL)
		return;

	/* A damaged partial-block count would overrun the block, so stop. */
	if (hash->used >= CRYC_SHA256_BLOCK)
		return;

	/* Counts the bytes in the message length that padding will report. */
	hash->total += (uint64_t)size;

	/*
	 * Tops up a partly filled block first; it is compressed once it is
	 * full, and the input may end before that.
	 */
	if (hash->used != 0) {
		room = CRYC_SHA256_BLOCK - hash->used;
		chunk = size;
		if (chunk > room)
			chunk = room;

		memcpy(hash->block + hash->used, data, chunk);
		hash->used += chunk;
		data += chunk;
		size -= chunk;

		if (hash->used < CRYC_SHA256_BLOCK)
			return;

		sha256_compress(hash->state, hash->block);
		hash->used = 0;
	}

	/* Compresses every complete block straight from the input. */
	while (size >= CRYC_SHA256_BLOCK) {
		sha256_compress(hash->state, data);
		data += CRYC_SHA256_BLOCK;
		size -= CRYC_SHA256_BLOCK;
	}

	/* Keeps the incomplete rest for the next call or for padding. */
	if (size != 0) {
		memcpy(hash->block, data, size);
		hash->used = size;
	}
}

/*
 * Pads (5.1.1), compresses the last blocks, writes the 32-byte digest
 * to out, and wipes *hash.
 *
 * out has room for 32 bytes.  After _final the
 * structure must be started again with an _init function before reuse.
 */
void
cryc_sha256_final(
	struct cryc_sha256 *hash,
	uint8_t *out)
{
	size_t used;
	size_t index;

	/* A damaged partial-block count is taken as an empty block. */
	used = hash->used;
	if (used >= CRYC_SHA256_BLOCK)
		used = 0;

	/* Appends the bit that follows the message (5.1.1 step a). */
	hash->block[used] = 0x80;
	used++;

	/*
	 * Pads with zero bits up to the length field.  When the one bit left
	 * no room for the field in this block, that block is compressed as it
	 * is and the padding continues in a block of its own.
	 */
	if (used > SHA256_LENGTH_OFFSET) {
		memset(hash->block + used, 0, CRYC_SHA256_BLOCK - used);
		sha256_compress(hash->state, hash->block);
		used = 0;
	}
	memset(hash->block + used, 0, SHA256_LENGTH_OFFSET - used);

	/* Appends the message length in bits as a 64-bit big-endian number (5.1.1 step c). */
	cryc_store_be64(hash->block + SHA256_LENGTH_OFFSET, hash->total << 3);

	/* Compresses the last block. */
	sha256_compress(hash->state, hash->block);

	/* Writes the digest: the eight words of H(N), each big-endian. */
	for (index = 0; index < 8; index++)
		cryc_store_be32(out + (index * 4U), hash->state[index]);

	/* Wipes the state, which has absorbed everything that was hashed. */
	cryc_wipe(hash, sizeof(*hash));
}

/*
 * Starts a SHA-512 computation (5.3.5).
 */
void
cryc_sha512_init(
	struct cryc_sha512 *hash)
{
	/* Starts from the SHA-512 initial value with a 64-byte digest. */
	sha512_start(hash, sha512_initial_state, CRYC_SHA512_SIZE);
}

/*
 * Starts a SHA-384 computation (5.3.4), in the same structure.
 */
void
cryc_sha384_init(
	struct cryc_sha512 *hash)
{
	/* Starts from the SHA-384 initial value with a 48-byte digest. */
	sha512_start(hash, sha384_initial_state, CRYC_SHA384_SIZE);
}

/*
 * Absorbs size bytes (6.4.2 for each complete block); data may be NULL
 * only when size is 0.
 */
void
cryc_sha512_update(
	struct cryc_sha512 *hash,
	const uint8_t *data,
	size_t size)
{
	size_t room;
	size_t chunk;

	/* Nothing to absorb. */
	if (size == 0)
		return;

	/* Missing data with a length is a caller's mistake that must not be read. */
	if (data == NULL)
		return;

	/* A damaged partial-block count would overrun the block, so stop. */
	if (hash->used >= CRYC_SHA512_BLOCK)
		return;

	/* Counts the bytes in the message length that padding will report. */
	hash->total += (uint64_t)size;

	/*
	 * Tops up a partly filled block first; it is compressed once it is
	 * full, and the input may end before that.
	 */
	if (hash->used != 0) {
		room = CRYC_SHA512_BLOCK - hash->used;
		chunk = size;
		if (chunk > room)
			chunk = room;

		memcpy(hash->block + hash->used, data, chunk);
		hash->used += chunk;
		data += chunk;
		size -= chunk;

		if (hash->used < CRYC_SHA512_BLOCK)
			return;

		sha512_compress(hash->state, hash->block);
		hash->used = 0;
	}

	/* Compresses every complete block straight from the input. */
	while (size >= CRYC_SHA512_BLOCK) {
		sha512_compress(hash->state, data);
		data += CRYC_SHA512_BLOCK;
		size -= CRYC_SHA512_BLOCK;
	}

	/* Keeps the incomplete rest for the next call or for padding. */
	if (size != 0) {
		memcpy(hash->block, data, size);
		hash->used = size;
	}
}

/*
 * Pads (5.1.2), compresses the last blocks, writes hash->out_size bytes
 * (48 or 64) to out, and wipes *hash.
 */
void
cryc_sha512_final(
	struct cryc_sha512 *hash,
	uint8_t *out)
{
	uint8_t digest[CRYC_SHA512_SIZE];
	size_t used;
	size_t out_size;
	size_t index;

	/* A damaged partial-block count is taken as an empty block. */
	used = hash->used;
	if (used >= CRYC_SHA512_BLOCK)
		used = 0;

	/* A damaged digest size is cut to the largest the state can give. */
	out_size = hash->out_size;
	if (out_size > CRYC_SHA512_SIZE)
		out_size = CRYC_SHA512_SIZE;

	/* Appends the bit that follows the message (5.1.2 step a). */
	hash->block[used] = 0x80;
	used++;

	/*
	 * Pads with zero bits up to the length field.  When the one bit left
	 * no room for the field in this block, that block is compressed as it
	 * is and the padding continues in a block of its own.
	 */
	if (used > SHA512_LENGTH_OFFSET) {
		memset(hash->block + used, 0, CRYC_SHA512_BLOCK - used);
		sha512_compress(hash->state, hash->block);
		used = 0;
	}
	memset(hash->block + used, 0, SHA512_LENGTH_OFFSET - used);

	/*
	 * Appends the message length in bits as a 128-bit big-endian number
	 * (5.1.2 step c).  The byte count shifted left by three gives the low
	 * 64 bits; the bits shifted out of it (the byte count shifted right
	 * by 61) are the high 64 bits.
	 */
	cryc_store_be64(hash->block + SHA512_LENGTH_OFFSET, hash->total >> 61);
	cryc_store_be64(hash->block + SHA512_LENGTH_OFFSET + 8U, hash->total << 3);

	/* Compresses the last block. */
	sha512_compress(hash->state, hash->block);

	/* Writes the eight words of H(N), each big-endian, and gives the first out_size bytes. */
	for (index = 0; index < 8; index++)
		cryc_store_be64(digest + (index * 8U), hash->state[index]);
	memcpy(out, digest, out_size);

	/* Wipes the digest copy and the state, which has absorbed everything that was hashed. */
	cryc_wipe(digest, sizeof(digest));
	cryc_wipe(hash, sizeof(*hash));
}

/*
 * Rotates a 32-bit word right by count bits, count from 1 to 31 (ROTR of
 * 3).
 */
static uint32_t
sha256_rotate_right(
	uint32_t value,
	unsigned int count)
{
	uint32_t rotated;

	/* The bits that fall off the right end re-enter at the left. */
	rotated = (value >> count) | (value << (32U - count));

	/* Reports the rotated word. */
	return rotated;
}

/*
 * Rotates a 64-bit word right by count bits, count from 1 to 63 (ROTR of
 * 3).
 */
static uint64_t
sha512_rotate_right(
	uint64_t value,
	unsigned int count)
{
	uint64_t rotated;

	/* The bits that fall off the right end re-enter at the left. */
	rotated = (value >> count) | (value << (64U - count));

	/* Reports the rotated word. */
	return rotated;
}

/*
 * Selects bits of y where x has ones and bits of z where x has zeros (Ch,
 * 4.2).
 */
static uint32_t
sha256_choose(
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	/* Ch(x, y, z) = (x and y) xor ((not x) and z). */
	return (x & y) ^ (~x & z);
}

/*
 * Gives the majority of each bit position of x, y and z (Maj, 4.3).
 */
static uint32_t
sha256_majority(
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	/* Maj(x, y, z) = (x and y) xor (x and z) xor (y and z). */
	return (x & y) ^ (x & z) ^ (y & z);
}

/*
 * Computes the upper-case sigma 0 of SHA-256 (4.4).
 */
static uint32_t
sha256_big_sigma0(
	uint32_t x)
{
	/* ROTR 2 xor ROTR 13 xor ROTR 22. */
	return sha256_rotate_right(x, 2) ^ sha256_rotate_right(x, 13) ^ sha256_rotate_right(x, 22);
}

/*
 * Computes the upper-case sigma 1 of SHA-256 (4.5).
 */
static uint32_t
sha256_big_sigma1(
	uint32_t x)
{
	/* ROTR 6 xor ROTR 11 xor ROTR 25. */
	return sha256_rotate_right(x, 6) ^ sha256_rotate_right(x, 11) ^ sha256_rotate_right(x, 25);
}

/*
 * Computes the lower-case sigma 0 of SHA-256 (4.6).
 */
static uint32_t
sha256_small_sigma0(
	uint32_t x)
{
	/* ROTR 7 xor ROTR 18 xor SHR 3. */
	return sha256_rotate_right(x, 7) ^ sha256_rotate_right(x, 18) ^ (x >> 3);
}

/*
 * Computes the lower-case sigma 1 of SHA-256 (4.7).
 */
static uint32_t
sha256_small_sigma1(
	uint32_t x)
{
	/* ROTR 17 xor ROTR 19 xor SHR 10. */
	return sha256_rotate_right(x, 17) ^ sha256_rotate_right(x, 19) ^ (x >> 10);
}

/*
 * Selects bits of y where x has ones and bits of z where x has zeros (Ch,
 * 4.8).
 */
static uint64_t
sha512_choose(
	uint64_t x,
	uint64_t y,
	uint64_t z)
{
	/* Ch(x, y, z) = (x and y) xor ((not x) and z). */
	return (x & y) ^ (~x & z);
}

/*
 * Gives the majority of each bit position of x, y and z (Maj, 4.9).
 */
static uint64_t
sha512_majority(
	uint64_t x,
	uint64_t y,
	uint64_t z)
{
	/* Maj(x, y, z) = (x and y) xor (x and z) xor (y and z). */
	return (x & y) ^ (x & z) ^ (y & z);
}

/*
 * Computes the upper-case sigma 0 of SHA-512 (4.10).
 */
static uint64_t
sha512_big_sigma0(
	uint64_t x)
{
	/* ROTR 28 xor ROTR 34 xor ROTR 39. */
	return sha512_rotate_right(x, 28) ^ sha512_rotate_right(x, 34) ^ sha512_rotate_right(x, 39);
}

/*
 * Computes the upper-case sigma 1 of SHA-512 (4.11).
 */
static uint64_t
sha512_big_sigma1(
	uint64_t x)
{
	/* ROTR 14 xor ROTR 18 xor ROTR 41. */
	return sha512_rotate_right(x, 14) ^ sha512_rotate_right(x, 18) ^ sha512_rotate_right(x, 41);
}

/*
 * Computes the lower-case sigma 0 of SHA-512 (4.12).
 */
static uint64_t
sha512_small_sigma0(
	uint64_t x)
{
	/* ROTR 1 xor ROTR 8 xor SHR 7. */
	return sha512_rotate_right(x, 1) ^ sha512_rotate_right(x, 8) ^ (x >> 7);
}

/*
 * Computes the lower-case sigma 1 of SHA-512 (4.13).
 */
static uint64_t
sha512_small_sigma1(
	uint64_t x)
{
	/* ROTR 19 xor ROTR 61 xor SHR 6. */
	return sha512_rotate_right(x, 19) ^ sha512_rotate_right(x, 61) ^ (x >> 6);
}

/*
 * Compresses one 64-byte block into the SHA-256 intermediate hash value
 * (6.2.2).
 */
static void
sha256_compress(
	uint32_t state[8],
	const uint8_t *block)
{
	uint32_t schedule[SHA256_ROUNDS];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t g;
	uint32_t h;
	uint32_t t1;
	uint32_t t2;
	size_t t;

	/* Prepares the message schedule, step 1: the block's sixteen words, then the expansion. */
	for (t = 0; t < 16; t++)
		schedule[t] = cryc_load_be32(block + (t * 4U));
	for (t = 16; t < SHA256_ROUNDS; t++) {
		schedule[t] = sha256_small_sigma1(schedule[t - 2]) + schedule[t - 7] +
		    sha256_small_sigma0(schedule[t - 15]) + schedule[t - 16];
	}

	/* Initializes the eight working variables with the previous hash value, step 2. */
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	/* Runs the sixty-four rounds, step 3. */
	for (t = 0; t < SHA256_ROUNDS; t++) {
		t1 = h + sha256_big_sigma1(e) + sha256_choose(e, f, g) + sha256_round_constant[t] + schedule[t];
		t2 = sha256_big_sigma0(a) + sha256_majority(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	/* Computes the new intermediate hash value, step 4. */
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;

	/* Wipes the schedule, which holds the message block (it may be a secret). */
	cryc_wipe(schedule, sizeof(schedule));
}

/*
 * Compresses one 128-byte block into the SHA-512 intermediate hash value
 * (6.4.2).
 */
static void
sha512_compress(
	uint64_t state[8],
	const uint8_t *block)
{
	uint64_t schedule[SHA512_ROUNDS];
	uint64_t a;
	uint64_t b;
	uint64_t c;
	uint64_t d;
	uint64_t e;
	uint64_t f;
	uint64_t g;
	uint64_t h;
	uint64_t t1;
	uint64_t t2;
	size_t t;

	/* Prepares the message schedule, step 1: the block's sixteen words, then the expansion. */
	for (t = 0; t < 16; t++)
		schedule[t] = cryc_load_be64(block + (t * 8U));
	for (t = 16; t < SHA512_ROUNDS; t++) {
		schedule[t] = sha512_small_sigma1(schedule[t - 2]) + schedule[t - 7] +
		    sha512_small_sigma0(schedule[t - 15]) + schedule[t - 16];
	}

	/* Initializes the eight working variables with the previous hash value, step 2. */
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	/* Runs the eighty rounds, step 3. */
	for (t = 0; t < SHA512_ROUNDS; t++) {
		t1 = h + sha512_big_sigma1(e) + sha512_choose(e, f, g) + sha512_round_constant[t] + schedule[t];
		t2 = sha512_big_sigma0(a) + sha512_majority(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	/* Computes the new intermediate hash value, step 4. */
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;

	/* Wipes the schedule, which holds the message block (it may be a secret). */
	cryc_wipe(schedule, sizeof(schedule));
}

/*
 * Starts a SHA-384 or SHA-512 computation from an initial hash value and
 * an output size.
 */
static void
sha512_start(
	struct cryc_sha512 *hash,
	const uint64_t *initial_state,
	size_t out_size)
{
	size_t index;

	/* Loads the initial hash value H(0). */
	for (index = 0; index < 8; index++)
		hash->state[index] = initial_state[index];

	/* Starts with an empty partial block and no bytes absorbed. */
	memset(hash->block, 0, sizeof(hash->block));
	hash->used = 0;
	hash->total = 0;

	/* Records how many bytes of the final state the digest keeps. */
	hash->out_size = out_size;
}
