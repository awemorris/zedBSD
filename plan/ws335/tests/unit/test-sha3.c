/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks Keccak-p[1600, 24], SHA3-256, SHA3-512, SHAKE128 and SHAKE256
 * (cryc-sha3.h).
 *
 * Expected values: the NIST byte-oriented SHA-3 and SHAKE test vectors
 * (short messages, long messages, variable output lengths and the Monte
 * Carlo files, read from the vector files), and a reference written from
 * the text of FIPS 202 alone: the permutation as the bit-array
 * Algorithms 1 to 6 (theta, rho, pi, chi, rc, iota) and Algorithm 7,
 * with the rho offsets and the round constants computed from their
 * definitions, and the sponge of Algorithm 8 with the padding of B.2
 * (Table 6) applied to a byte string.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-sha3.h"

/* The four functions a helper works on. */
#define KIND_SHA3_256	0
#define KIND_SHA3_512	1
#define KIND_SHAKE128	2
#define KIND_SHAKE256	3
#define KIND_COUNT	4

/* The sizes of the state in bytes (b / 8) and of the lanes. */
#define STATE_BYTES	200U

/* The facts about each function, taken from the text of FIPS 202 6.1 and 6.2 and B.2. */
struct kind_info {
	size_t rate;
	unsigned suffix;
	size_t digest;
};

/*
 * Rates are r / 8 with r = 1600 - c: c = 512 and 1024 for SHA3-256 and
 * SHA3-512 (6.1), c = 256 and 512 for the SHAKEs (6.2).  The suffix is the
 * domain bits plus the first padding bit as a byte (B.2, Table 6).
 */
static const struct kind_info kinds[KIND_COUNT] = {
	{ 136U, 0x06U, 32U },
	{  72U, 0x06U, 64U },
	{ 168U, 0x1fU, 0U },
	{ 136U, 0x1fU, 0U }
};

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x2c1b3c6dU;

/* The number of checkpoints and cases the vector files gave, for the summary. */
static unsigned long vector_records;

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
	void *out,
	size_t size)
{
	uint8_t *bytes;
	size_t index;

	/* One byte at a time. */
	bytes = (uint8_t *)out;
	for (index = 0; index < size; index++)
		bytes[index] = (uint8_t)(random_next() >> 24);
}

/* Returns whether size bytes at buffer all equal value. */
static int
all_equal(
	const uint8_t *buffer,
	size_t size,
	uint8_t value)
{
	size_t index;

	/* Any other byte. */
	for (index = 0; index < size; index++)
		if (buffer[index] != value)
			return 0;
	return 1;
}

/*
 * The reference permutation: the state is a bit array a[x][y][z], and a
 * string of 200 bytes becomes the array by S[64(5y + x) + z] = a[x][y][z]
 * with bit i of the string being bit (i mod 8) of byte i / 8 (3.1.2, B.1).
 */
typedef unsigned char ref_array[5][5][64];

/* Turns a state string into the array of bits (3.1.2). */
static void
ref_to_array(
	const uint8_t *string,
	ref_array a)
{
	unsigned x;
	unsigned y;
	unsigned z;
	unsigned index;

	/* Every bit. */
	for (y = 0; y < 5U; y++) {
		for (x = 0; x < 5U; x++) {
			for (z = 0; z < 64U; z++) {
				index = 64U * (5U * y + x) + z;
				a[x][y][z] = (unsigned char)((string[index / 8U] >> (index % 8U)) & 1U);
			}
		}
	}
}

/* Turns the array of bits back into a state string (3.1.3). */
static void
ref_to_string(
	ref_array a,
	uint8_t *string)
{
	unsigned x;
	unsigned y;
	unsigned z;
	unsigned index;

	/* Every bit. */
	memset(string, 0, STATE_BYTES);
	for (y = 0; y < 5U; y++) {
		for (x = 0; x < 5U; x++) {
			for (z = 0; z < 64U; z++) {
				index = 64U * (5U * y + x) + z;
				string[index / 8U] = (uint8_t)(string[index / 8U] | (a[x][y][z] << (index % 8U)));
			}
		}
	}
}

/* Algorithm 1, theta. */
static void
ref_theta(
	ref_array a)
{
	unsigned char c[5][64];
	unsigned char d[5][64];
	unsigned x;
	unsigned y;
	unsigned z;

	/* C[x, z] is the parity of the column. */
	for (x = 0; x < 5U; x++) {
		for (z = 0; z < 64U; z++) {
			c[x][z] = (unsigned char)(a[x][0][z] ^ a[x][1][z] ^ a[x][2][z] ^ a[x][3][z] ^ a[x][4][z]);
		}
	}

	/* D[x, z] = C[(x - 1) mod 5, z] xor C[(x + 1) mod 5, (z - 1) mod w]. */
	for (x = 0; x < 5U; x++) {
		for (z = 0; z < 64U; z++) {
			d[x][z] = (unsigned char)(c[(x + 4U) % 5U][z] ^ c[(x + 1U) % 5U][(z + 63U) % 64U]);
		}
	}

	/* A'[x, y, z] = A[x, y, z] xor D[x, z]. */
	for (y = 0; y < 5U; y++) {
		for (x = 0; x < 5U; x++) {
			for (z = 0; z < 64U; z++) {
				a[x][y][z] = (unsigned char)(a[x][y][z] ^ d[x][z]);
			}
		}
	}
}

/* Algorithm 2, rho: the lane (0, 0) stays; the others walk the path (x, y) = (1, 0), (y, (2x + 3y) mod 5). */
static void
ref_rho(
	ref_array a)
{
	ref_array b;
	unsigned x;
	unsigned y;
	unsigned t;
	unsigned z;
	unsigned offset;
	unsigned next_y;

	/* The lane (0, 0) is copied. */
	memcpy(b, a, sizeof(b));

	/* The 24 other lanes, each rotated by (t + 1)(t + 2) / 2 modulo w. */
	x = 1;
	y = 0;
	for (t = 0; t < 24U; t++) {
		offset = ((t + 1U) * (t + 2U) / 2U) % 64U;
		for (z = 0; z < 64U; z++)
			b[x][y][z] = a[x][y][(z + 64U - offset) % 64U];
		next_y = (2U * x + 3U * y) % 5U;
		x = y;
		y = next_y;
	}
	memcpy(a, b, sizeof(b));
}

/* Algorithm 3, pi: A'[x, y, z] = A[(x + 3y) mod 5, x, z]. */
static void
ref_pi(
	ref_array a)
{
	ref_array b;
	unsigned x;
	unsigned y;
	unsigned z;

	/* Every bit. */
	for (y = 0; y < 5U; y++)
		for (x = 0; x < 5U; x++)
			for (z = 0; z < 64U; z++)
				b[x][y][z] = a[(x + 3U * y) % 5U][x][z];
	memcpy(a, b, sizeof(b));
}

/* Algorithm 4, chi: A'[x, y, z] = A[x, y, z] xor ((A[(x + 1) mod 5, y, z] xor 1) and A[(x + 2) mod 5, y, z]). */
static void
ref_chi(
	ref_array a)
{
	ref_array b;
	unsigned x;
	unsigned y;
	unsigned z;

	/* Every bit. */
	for (y = 0; y < 5U; y++)
		for (x = 0; x < 5U; x++)
			for (z = 0; z < 64U; z++)
				b[x][y][z] = (unsigned char)(a[x][y][z] ^
				    ((a[(x + 1U) % 5U][y][z] ^ 1U) & a[(x + 2U) % 5U][y][z]));
	memcpy(a, b, sizeof(b));
}

/* Algorithm 5, rc(t): the output bit of the degree 8 linear feedback shift register. */
static unsigned
ref_rc(
	unsigned t)
{
	unsigned char r[9];
	unsigned i;
	unsigned j;

	/* rc(0) is 1. */
	t = t % 255U;
	if (t == 0)
		return 1;

	/* R = 10000000, then t times: R = 0 || R, and R[0], R[4], R[5], R[6] xor R[8], truncated to 8 bits. */
	memset(r, 0, sizeof(r));
	r[0] = 1;
	for (i = 1; i <= t; i++) {
		for (j = 8; j >= 1U; j--)
			r[j] = r[j - 1U];
		r[0] = 0;
		r[0] = (unsigned char)(r[0] ^ r[8]);
		r[4] = (unsigned char)(r[4] ^ r[8]);
		r[5] = (unsigned char)(r[5] ^ r[8]);
		r[6] = (unsigned char)(r[6] ^ r[8]);
	}
	return r[0];
}

/* Algorithm 6, iota, for round ir: RC[2^j - 1] = rc(j + 7ir) for j = 0 to l, with l = 6 for w = 64. */
static void
ref_iota(
	ref_array a,
	unsigned ir)
{
	unsigned char constant[64];
	unsigned j;
	unsigned z;

	/* The constant, zero except at bits 0, 1, 3, 7, 15, 31 and 63. */
	memset(constant, 0, sizeof(constant));
	for (j = 0; j <= 6U; j++)
		constant[(1U << j) - 1U] = (unsigned char)ref_rc(j + 7U * ir);

	/* Added into the lane (0, 0). */
	for (z = 0; z < 64U; z++)
		a[0][0][z] = (unsigned char)(a[0][0][z] ^ constant[z]);
}

/* Algorithm 7, Keccak-p[1600, 24] on a state string: the rounds ir = 12 + 2l - nr to 12 + 2l - 1, that is 0 to 23. */
static void
ref_permute(
	uint8_t *string)
{
	ref_array a;
	unsigned ir;

	/* Each round is iota(chi(pi(rho(theta(A))))). */
	ref_to_array(string, a);
	for (ir = 0; ir < 24U; ir++) {
		ref_theta(a);
		ref_rho(a);
		ref_pi(a);
		ref_chi(a);
		ref_iota(a, ir);
	}
	ref_to_string(a, string);
}

/*
 * The reference sponge (Algorithm 8): pads message || suffix with
 * pad10*1 as the bytes of B.2 Table 6, absorbs the blocks, and squeezes
 * out_size bytes.
 */
static void
ref_sponge(
	size_t rate,
	unsigned suffix,
	const uint8_t *message,
	size_t size,
	uint8_t *out,
	size_t out_size)
{
	uint8_t state[STATE_BYTES];
	uint8_t *padded;
	size_t padded_size;
	size_t offset;
	size_t index;
	size_t take;

	/* The padded message: the message, the suffix byte, zeros, and 0x80 in the last byte of the last block. */
	padded_size = (size / rate + 1U) * rate;
	padded = (uint8_t *)calloc(padded_size, 1);
	CHECK(padded != NULL);
	if (padded == NULL)
		return;
	if (size > 0)
		memcpy(padded, message, size);
	padded[size] = (uint8_t)(padded[size] ^ suffix);
	padded[padded_size - 1U] = (uint8_t)(padded[padded_size - 1U] ^ 0x80U);

	/* Absorbs: each block is added into the first r bits, then permuted. */
	memset(state, 0, sizeof(state));
	for (offset = 0; offset < padded_size; offset += rate) {
		for (index = 0; index < rate; index++)
			state[index] = (uint8_t)(state[index] ^ padded[offset + index]);
		ref_permute(state);
	}
	free(padded);

	/* Squeezes: r bits at a time, permuting between blocks. */
	offset = 0;
	while (offset < out_size) {
		take = out_size - offset;
		if (take > rate)
			take = rate;
		memcpy(out + offset, state, take);
		offset += take;
		if (offset < out_size)
			ref_permute(state);
	}
}

/* Converts 25 lanes to a state string, little-endian within a lane (3.1.2, B.1). */
static void
lanes_to_string(
	const uint64_t *lanes,
	uint8_t *string)
{
	unsigned lane;
	unsigned byte;

	/* Every lane. */
	for (lane = 0; lane < 25U; lane++)
		for (byte = 0; byte < 8U; byte++)
			string[lane * 8U + byte] = (uint8_t)(lanes[lane] >> (8U * byte));
}

/* Converts a state string to 25 lanes. */
static void
string_to_lanes(
	const uint8_t *string,
	uint64_t *lanes)
{
	unsigned lane;
	unsigned byte;

	/* Every lane. */
	for (lane = 0; lane < 25U; lane++) {
		lanes[lane] = 0;
		for (byte = 0; byte < 8U; byte++)
			lanes[lane] |= (uint64_t)string[lane * 8U + byte] << (8U * byte);
	}
}

/* Checks the permutation against the reference on one state string. */
static void
check_permute_one(
	const uint8_t *string)
{
	uint64_t lanes[25];
	uint8_t expected[STATE_BYTES];
	uint8_t got[STATE_BYTES];

	/* The same state through both. */
	memcpy(expected, string, STATE_BYTES);
	ref_permute(expected);
	string_to_lanes(string, lanes);
	cryc_keccak_permute(lanes);
	lanes_to_string(lanes, got);
	CHECK_BYTES(got, expected, STATE_BYTES);
}

/* Checks the permutation on the zero state, the all-ones state, single bits and random states. */
static void
test_permute(void)
{
	uint8_t string[STATE_BYTES];
	uint8_t chained[STATE_BYTES];
	uint64_t lanes[25];
	uint8_t expected[STATE_BYTES];
	uint8_t got[STATE_BYTES];
	unsigned index;

	/* The zero state and the all-ones state. */
	memset(string, 0, sizeof(string));
	check_permute_one(string);
	memset(string, 0xff, sizeof(string));
	check_permute_one(string);

	/* Each lane with only its lowest bit and only its highest bit set. */
	for (index = 0; index < 50U; index++) {
		memset(string, 0, sizeof(string));
		if (index < 25U)
			string[index * 8U] = 1;
		else
			string[(index - 25U) * 8U + 7U] = 0x80;
		check_permute_one(string);
	}

	/* Random states. */
	for (index = 0; index < 40U; index++) {
		random_fill(string, sizeof(string));
		check_permute_one(string);
	}

	/* The permutation applied again and again from zero: ten steps against the reference. */
	memset(chained, 0, sizeof(chained));
	memset(lanes, 0, sizeof(lanes));
	for (index = 0; index < 10U; index++) {
		ref_permute(chained);
		cryc_keccak_permute(lanes);
		lanes_to_string(lanes, got);
		CHECK_BYTES(got, chained, STATE_BYTES);
	}
	memcpy(expected, chained, sizeof(expected));

	/* A missing state is left alone (no crash). */
	cryc_keccak_permute(NULL);
	CHECK(1);
}

/* Starts a sponge of kind. */
static void
sponge_init(
	struct cryc_sha3 *sponge,
	int kind)
{
	/* The init function of the kind. */
	if (kind == KIND_SHA3_256)
		cryc_sha3_256_init(sponge);
	else if (kind == KIND_SHA3_512)
		cryc_sha3_512_init(sponge);
	else if (kind == KIND_SHAKE128)
		cryc_shake128_init(sponge);
	else
		cryc_shake256_init(sponge);
}

/* Absorbs size bytes in pieces of piece bytes (0 is all at once), with an empty absorb between the pieces. */
static void
absorb_pieces(
	struct cryc_sha3 *sponge,
	const uint8_t *data,
	size_t size,
	size_t piece)
{
	size_t offset;
	size_t chunk;

	/* All at once. */
	if (piece == 0) {
		cryc_sha3_absorb(sponge, data, size);
		return;
	}

	/* The pieces. */
	offset = 0;
	while (offset < size) {
		chunk = size - offset;
		if (chunk > piece)
			chunk = piece;
		cryc_sha3_absorb(sponge, data + offset, chunk);
		cryc_sha3_absorb(sponge, NULL, 0);
		offset += chunk;
	}
}

/* Squeezes size bytes in pieces of piece bytes (0 is all at once), with an empty squeeze between the pieces. */
static void
squeeze_pieces(
	struct cryc_sha3 *sponge,
	uint8_t *out,
	size_t size,
	size_t piece)
{
	size_t offset;
	size_t chunk;

	/* All at once. */
	if (piece == 0) {
		cryc_shake_squeeze(sponge, out, size);
		return;
	}

	/* The pieces. */
	offset = 0;
	while (offset < size) {
		chunk = size - offset;
		if (chunk > piece)
			chunk = piece;
		cryc_shake_squeeze(sponge, out + offset, chunk);
		cryc_shake_squeeze(sponge, out + offset, 0);
		offset += chunk;
	}
}

/*
 * Runs a function of kind over a message and writes out_size bytes: the
 * digest through final for the SHA-3 hashes when out_size is the digest
 * size, else the squeeze.  The sponge is wiped at the end and that is
 * checked.
 */
static void
run_sponge(
	int kind,
	const uint8_t *data,
	size_t size,
	size_t in_piece,
	uint8_t *out,
	size_t out_size,
	size_t out_piece)
{
	static const uint8_t zeros[sizeof(struct cryc_sha3)] = { 0 };
	struct cryc_sha3 sponge;

	/* The input in pieces. */
	sponge_init(&sponge, kind);
	absorb_pieces(&sponge, data, size, in_piece);

	/* A digest through final (which wipes), or output through squeeze (then clear wipes). */
	if (kind < KIND_SHAKE128 && out_size == kinds[kind].digest) {
		cryc_sha3_final(&sponge, out);
	} else {
		squeeze_pieces(&sponge, out, out_size, out_piece);
		cryc_sha3_clear(&sponge);
	}
	CHECK_BYTES(&sponge, zeros, sizeof(sponge));
}

/* The sizes of the pieces a message is fed in, and the sizes the output is taken in. */
static const size_t in_pieces[] = { 0, 1, 3, 7, 8, 9, 71, 72, 73, 135, 136, 137, 167, 168, 169, 1000 };
static const size_t out_pieces[] = { 0, 1, 5, 8, 135, 136, 137, 167, 168, 169, 300 };

/* Checks one message against its output in one go and in pieces. */
static void
check_message(
	int kind,
	const uint8_t *message,
	size_t size,
	const uint8_t *expected,
	size_t out_size)
{
	uint8_t *got;
	size_t which;

	/* A buffer larger than the output, so that a write past the end shows. */
	got = (uint8_t *)malloc(out_size + 1U);
	CHECK(got != NULL);
	if (got == NULL)
		return;

	/* Every piece size for the input, paired with a size for the output. */
	for (which = 0; which < sizeof(in_pieces) / sizeof(in_pieces[0]); which++) {
		memset(got, 0xa5, out_size + 1U);
		run_sponge(kind, message, size, in_pieces[which], got,
		    out_size, out_pieces[which % (sizeof(out_pieces) / sizeof(out_pieces[0]))]);
		CHECK_BYTES(got, expected, out_size);
		CHECK(got[out_size] == 0xa5U);
	}
	free(got);
}

/* Maps a vector file name part to the function and the output size it checks. */
static unsigned long
run_message_file(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t *message;
	uint8_t *expected;
	const char *text;
	const char *answer_key;
	size_t size;
	size_t answer_size;
	unsigned long count;

	/* The SHA-3 files give MD, the SHAKE files give Output. */
	answer_key = kind < KIND_SHAKE128 ? "MD" : "Output";
	count = 0;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		text = vec_get(&file, "Len");
		if (text == NULL || vec_get(&file, answer_key) == NULL)
			continue;
		size = (size_t)strtoul(text, NULL, 10) / 8U;
		message = (uint8_t *)malloc(strlen(vec_get(&file, "Msg")) / 2U + 1U);
		expected = (uint8_t *)malloc(strlen(vec_get(&file, answer_key)) / 2U + 1U);
		CHECK(message != NULL && expected != NULL);
		if (message == NULL || expected == NULL)
			break;
		CHECK(vec_hex(&file, "Msg", message, strlen(vec_get(&file, "Msg")) / 2U + 1U) != (size_t)-1);
		answer_size = vec_hex(&file, answer_key, expected, strlen(vec_get(&file, answer_key)) / 2U + 1U);
		CHECK(answer_size != (size_t)-1);
		if (kind < KIND_SHAKE128)
			CHECK(answer_size == kinds[kind].digest);
		else if (vec_get(&file, "Outputlen") != NULL)
			CHECK(answer_size * 8U == (size_t)strtoul(vec_get(&file, "Outputlen"), NULL, 10));
		if (answer_size != (size_t)-1 && answer_size > 0)
			check_message(kind, message, size, expected, answer_size);
		free(message);
		free(expected);
		count++;
	}
	vec_close(&file);
	return count;
}

/* Runs the SHAKE variable-output file: Msg (hex, Len given by "Input Length" bits) and Outputlen. */
static unsigned long
run_variable_file(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t *message;
	uint8_t *expected;
	const char *text;
	size_t size;
	size_t answer_size;
	unsigned long count;

	/* Every record has COUNT, Outputlen, Msg and Output. */
	count = 0;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		text = vec_get(&file, "Outputlen");
		if (text == NULL || vec_get(&file, "Output") == NULL)
			continue;
		text = vec_get(&file, "section.Input Length");
		CHECK(text != NULL);
		if (text == NULL)
			break;
		size = (size_t)strtoul(text, NULL, 10) / 8U;
		message = (uint8_t *)malloc(strlen(vec_get(&file, "Msg")) / 2U + 1U);
		expected = (uint8_t *)malloc(strlen(vec_get(&file, "Output")) / 2U + 1U);
		CHECK(message != NULL && expected != NULL);
		if (message == NULL || expected == NULL)
			break;
		CHECK(vec_hex(&file, "Msg", message, strlen(vec_get(&file, "Msg")) / 2U + 1U) == size);
		answer_size = vec_hex(&file, "Output", expected, strlen(vec_get(&file, "Output")) / 2U + 1U);
		CHECK(answer_size != (size_t)-1);
		CHECK(answer_size * 8U == (size_t)strtoul(vec_get(&file, "Outputlen"), NULL, 10));
		if (answer_size != (size_t)-1 && answer_size > 0)
			check_message(kind, message, size, expected, answer_size);
		free(message);
		free(expected);
		count++;
	}
	vec_close(&file);
	return count;
}

/*
 * Runs the Monte Carlo file of a SHA-3 hash: MD(0) is the seed and
 * MD(i) = hash(MD(i - 1)) for i = 1 to 1000; the file gives MD(1000) for
 * each of 100 checkpoints, and the next seed is that value.
 */
static unsigned long
run_hash_monte_file(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t expected[64];
	uint8_t next[64];
	size_t size;
	size_t step;
	int have_seed;
	unsigned long count;

	/* The seed record, then the checkpoints in order. */
	count = 0;
	have_seed = 0;
	size = kinds[kind].digest;
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
		for (step = 0; step < 1000U; step++) {
			run_sponge(kind, seed, size, 0, next, size, 0);
			memcpy(seed, next, size);
		}
		CHECK_BYTES(seed, expected, size);
		count++;
	}
	vec_close(&file);
	return count;
}

/* The examples and the lengths around the block sizes against the reference sponge. */
static void
test_reference(void)
{
	static uint8_t message[2U * 168U + 8U];
	static uint8_t expected[3U * 168U + 5U];
	static uint8_t got[3U * 168U + 5U];
	static const size_t wanted[] = { 1, 7, 135, 136, 137, 168, 169, 2 * 168, 3 * 168 + 5 };
	int kind;
	size_t size;
	size_t rate;
	size_t out_size;
	size_t which;
	size_t in_piece;

	/* Each function, each message length from empty to just over two blocks. */
	random_fill(message, sizeof(message));
	for (kind = 0; kind < KIND_COUNT; kind++) {
		rate = kinds[kind].rate;
		for (size = 0; size <= 2U * rate + 3U; size++) {
			if (kind < KIND_SHAKE128) {
				/* The digest, by final. */
				out_size = kinds[kind].digest;
				ref_sponge(rate, kinds[kind].suffix, message, size, expected, out_size);
				run_sponge(kind, message, size, 0, got, out_size, 0);
				CHECK_BYTES(got, expected, out_size);
				in_piece = size % 11U + 1U;
				run_sponge(kind, message, size, in_piece, got, out_size, 0);
				CHECK_BYTES(got, expected, out_size);

				/* The same sponge used as an extendable-output function (domain bits 01). */
				out_size = 2U * rate + 5U;
				ref_sponge(rate, kinds[kind].suffix, message, size, expected, out_size);
				run_sponge(kind, message, size, 0, got, out_size, 0);
				CHECK_BYTES(got, expected, out_size);
				run_sponge(kind, message, size, in_piece, got, out_size, 17U);
				CHECK_BYTES(got, expected, out_size);
			} else {
				/* The longest output, then prefixes of it by size and by pieces. */
				ref_sponge(rate, kinds[kind].suffix, message, size, expected, 3U * 168U + 5U);
				for (which = 0; which < sizeof(wanted) / sizeof(wanted[0]); which++) {
					run_sponge(kind, message, size, 0, got, wanted[which], 0);
					CHECK_BYTES(got, expected, wanted[which]);
				}
				in_piece = size % 13U + 1U;
				run_sponge(kind, message, size, in_piece, got, 3U * 168U + 5U, size % 29U + 1U);
				CHECK_BYTES(got, expected, 3U * 168U + 5U);
			}
		}
	}
}

/* The long messages: 100000 bytes against the reference, in one go and in awkward pieces. */
static void
test_long(void)
{
	uint8_t *message;
	uint8_t expected[168U + 8U];
	uint8_t got[168U + 8U];
	int kind;
	size_t size;

	/* A pseudo-random message. */
	size = 100000U;
	message = (uint8_t *)malloc(size);
	CHECK(message != NULL);
	if (message == NULL)
		return;
	random_fill(message, size);
	for (kind = 0; kind < KIND_COUNT; kind++) {
		ref_sponge(kinds[kind].rate, kinds[kind].suffix, message, size, expected,
		    kind < KIND_SHAKE128 ? kinds[kind].digest : 168U + 8U);
		run_sponge(kind, message, size, 0, got, kind < KIND_SHAKE128 ? kinds[kind].digest : 168U + 8U, 0);
		CHECK_BYTES(got, expected, kind < KIND_SHAKE128 ? kinds[kind].digest : 168U + 8U);
		run_sponge(kind, message, size, 997U, got, kind < KIND_SHAKE128 ? kinds[kind].digest : 168U + 8U, 33U);
		CHECK_BYTES(got, expected, kind < KIND_SHAKE128 ? kinds[kind].digest : 168U + 8U);
	}
	free(message);
}

/* Checks the fields init sets, over a sponge full of other bytes. */
static void
test_init(void)
{
	static const uint8_t zeros[sizeof(struct cryc_sha3)] = { 0 };
	struct cryc_sha3 sponge;
	int kind;
	unsigned lane;

	/* The constants of the header are the rates of the text. */
	CHECK(CRYC_SHA3_256_RATE == 136U);
	CHECK(CRYC_SHA3_512_RATE == 72U);
	CHECK(CRYC_SHAKE128_RATE == 168U);
	CHECK(CRYC_SHAKE256_RATE == 136U);

	/* Each init resets every field, whatever the sponge held. */
	for (kind = 0; kind < KIND_COUNT; kind++) {
		memset(&sponge, 0xff, sizeof(sponge));
		sponge_init(&sponge, kind);
		CHECK(sponge.rate == kinds[kind].rate);
		CHECK(sponge.suffix == kinds[kind].suffix);
		CHECK(sponge.position == 0);
		CHECK(sponge.squeezing == 0);
		for (lane = 0; lane < 25U; lane++)
			CHECK(sponge.lanes[lane] == 0);
		cryc_sha3_clear(&sponge);
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
	}

	/* Missing sponges do nothing (no crash). */
	cryc_sha3_256_init(NULL);
	cryc_sha3_512_init(NULL);
	cryc_shake128_init(NULL);
	cryc_shake256_init(NULL);
	cryc_sha3_absorb(NULL, (const uint8_t *)"a", 1U);
	cryc_sha3_final(NULL, (uint8_t *)&sponge);
	cryc_shake_squeeze(NULL, (uint8_t *)&sponge, 4U);
	cryc_sha3_clear(NULL);
	CHECK(1);
}

/* Checks the absorb and squeeze calls that are mistakes: they change nothing or give zeros. */
static void
test_misuse(void)
{
	static const uint8_t zeros[sizeof(struct cryc_sha3)] = { 0 };
	uint8_t message[300];
	uint8_t out[400];
	uint8_t first[64];
	uint8_t second[64];
	uint8_t reference[64];
	struct cryc_sha3 sponge;
	struct cryc_sha3 before;
	struct cryc_sha3 copy;
	int kind;

	/* Missing data with a length changes nothing, even a huge length. */
	random_fill(message, sizeof(message));
	for (kind = 0; kind < KIND_COUNT; kind++) {
		sponge_init(&sponge, kind);
		cryc_sha3_absorb(&sponge, message, 20U);
		before = sponge;
		cryc_sha3_absorb(&sponge, NULL, 1U);
		CHECK_BYTES(&sponge, &before, sizeof(sponge));
		cryc_sha3_absorb(&sponge, NULL, (size_t)-1);
		CHECK_BYTES(&sponge, &before, sizeof(sponge));
		cryc_sha3_absorb(&sponge, message, 0);
		CHECK_BYTES(&sponge, &before, sizeof(sponge));

		/* It still finishes as if only the 20 bytes were absorbed. */
		ref_sponge(kinds[kind].rate, kinds[kind].suffix, message, 20U, reference, 32U);
		if (kind < KIND_SHAKE128) {
			memset(first, 0, sizeof(first));
			cryc_sha3_final(&sponge, first);
			CHECK_BYTES(first, reference, 32U);
		} else {
			cryc_shake_squeeze(&sponge, first, 32U);
			CHECK_BYTES(first, reference, 32U);
		}
	}

	/* A SHAKE: an absorb after the first squeeze changes nothing, even a size 0 first squeeze closes the input. */
	for (kind = KIND_SHAKE128; kind < KIND_COUNT; kind++) {
		sponge_init(&sponge, kind);
		cryc_sha3_absorb(&sponge, message, 50U);
		cryc_shake_squeeze(&sponge, out, 0);
		CHECK(sponge.squeezing != 0);
		before = sponge;
		cryc_sha3_absorb(&sponge, message + 50, 200U);
		CHECK_BYTES(&sponge, &before, sizeof(sponge));
		ref_sponge(kinds[kind].rate, kinds[kind].suffix, message, 50U, reference, 64U);
		cryc_shake_squeeze(&sponge, first, 64U);
		CHECK_BYTES(first, reference, 64U);

		/* More output after an absorb that was refused is the next part of the same stream. */
		cryc_sha3_absorb(&sponge, message, 10U);
		ref_sponge(kinds[kind].rate, kinds[kind].suffix, message, 50U, out, 64U + 40U);
		cryc_shake_squeeze(&sponge, second, 40U);
		CHECK_BYTES(second, out + 64, 40U);
		cryc_sha3_clear(&sponge);
	}

	/* A squeeze with no place to write still closes the input and moves nothing else wrongly. */
	sponge_init(&sponge, KIND_SHAKE128);
	cryc_sha3_absorb(&sponge, message, 7U);
	cryc_shake_squeeze(&sponge, NULL, 100U);
	CHECK(sponge.squeezing != 0);
	CHECK(sponge.position == 0);
	cryc_sha3_clear(&sponge);

	/* A final with no place to write wipes the sponge and writes nothing. */
	sponge_init(&sponge, KIND_SHA3_256);
	cryc_sha3_absorb(&sponge, message, 7U);
	cryc_sha3_final(&sponge, NULL);
	CHECK_BYTES(&sponge, zeros, sizeof(sponge));

	/* A final on a SHAKE writes nothing and wipes. */
	for (kind = KIND_SHAKE128; kind < KIND_COUNT; kind++) {
		sponge_init(&sponge, kind);
		cryc_sha3_absorb(&sponge, message, 7U);
		memset(out, 0xa5, sizeof(out));
		cryc_sha3_final(&sponge, out);
		CHECK(all_equal(out, sizeof(out), 0xa5U));
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
	}

	/* A final on a hash that already began to squeeze writes nothing and wipes. */
	sponge_init(&sponge, KIND_SHA3_512);
	cryc_sha3_absorb(&sponge, message, 7U);
	cryc_shake_squeeze(&sponge, first, 10U);
	memset(out, 0xa5, sizeof(out));
	cryc_sha3_final(&sponge, out);
	CHECK(all_equal(out, sizeof(out), 0xa5U));
	CHECK_BYTES(&sponge, zeros, sizeof(sponge));

	/* After final or clear every call does nothing: absorb leaves zeros, squeeze gives zeros, final writes nothing. */
	for (kind = 0; kind < KIND_COUNT; kind++) {
		sponge_init(&sponge, kind);
		cryc_sha3_absorb(&sponge, message, 100U);
		if (kind < KIND_SHAKE128)
			cryc_sha3_final(&sponge, first);
		else
			cryc_sha3_clear(&sponge);
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
		cryc_sha3_absorb(&sponge, message, 100U);
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
		memset(out, 0xa5, sizeof(out));
		cryc_shake_squeeze(&sponge, out, 300U);
		CHECK(all_equal(out, 300U, 0));
		CHECK(out[300] == 0xa5U);
		memset(out, 0xa5, sizeof(out));
		cryc_sha3_final(&sponge, out);
		CHECK(all_equal(out, sizeof(out), 0xa5U));
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
		cryc_sha3_clear(&sponge);
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
	}

	/* A copy taken mid-absorb and mid-squeeze goes on to the same result as the original. */
	for (kind = 0; kind < KIND_COUNT; kind++) {
		sponge_init(&sponge, kind);
		cryc_sha3_absorb(&sponge, message, kinds[kind].rate + 3U);
		copy = sponge;
		cryc_sha3_absorb(&sponge, message + 200, 50U);
		cryc_sha3_absorb(&copy, message + 200, 50U);
		CHECK_BYTES(&sponge, &copy, sizeof(sponge));
		if (kind >= KIND_SHAKE128) {
			cryc_shake_squeeze(&sponge, first, 40U);
			copy = sponge;
			cryc_shake_squeeze(&sponge, first, 64U);
			cryc_shake_squeeze(&copy, second, 64U);
			CHECK_BYTES(first, second, 64U);
		}
		cryc_sha3_clear(&sponge);
		cryc_sha3_clear(&copy);
	}
}

/* Returns whether a started-looking sponge is one the module accepts: a rate of the four functions, a position not past it. */
static int
looks_usable(
	const struct cryc_sha3 *sponge)
{
	/* The rates 72, 136 and 168 only. */
	if (sponge->rate != 72U && sponge->rate != 136U && sponge->rate != 168U)
		return 0;
	return sponge->position <= sponge->rate;
}

/* Calls everything on damaged and random structures; ASan and UBSan must stay quiet and unusable ones must not change. */
static void
test_damaged(void)
{
	static const size_t rates[] = { 0, 1, 7, 71, 72, 73, 100, 135, 136, 137, 167, 168, 169, 200, 1000, (size_t)-1 };
	static uint8_t message[400];
	static uint8_t out[700];
	static const uint8_t zeros[sizeof(struct cryc_sha3)] = { 0 };
	struct cryc_sha3 sponge;
	struct cryc_sha3 before;
	size_t which;
	size_t position;
	unsigned round;
	size_t size;

	/* Each rate, with positions around the end of the block and beyond. */
	random_fill(message, sizeof(message));
	for (which = 0; which < sizeof(rates) / sizeof(rates[0]); which++) {
		for (position = 0; position < 4U * 169U; position += (position < 180U ? 1U : 97U)) {
			memset(&sponge, 0, sizeof(sponge));
			random_fill(sponge.lanes, sizeof(sponge.lanes));
			sponge.rate = rates[which];
			sponge.position = position;
			sponge.suffix = 0x1fU;

			/* Absorb, on an unusable one nothing changes (the size is huge to catch any read). */
			before = sponge;
			if (!looks_usable(&sponge)) {
				cryc_sha3_absorb(&sponge, message, (size_t)-1);
				CHECK_BYTES(&sponge, &before, sizeof(sponge));
				memset(out, 0xa5, sizeof(out));
				cryc_shake_squeeze(&sponge, out, 500U);
				CHECK(all_equal(out, 500U, 0));
				CHECK(out[500] == 0xa5U);
				CHECK_BYTES(&sponge, &before, sizeof(sponge));
			} else {
				cryc_sha3_absorb(&sponge, message, 300U);
				CHECK(sponge.position < sponge.rate);
				cryc_shake_squeeze(&sponge, out, 500U);
				CHECK(sponge.position <= sponge.rate);
			}

			/* Final always wipes, whatever the sponge was. */
			sponge = before;
			memset(out, 0xa5, sizeof(out));
			cryc_sha3_final(&sponge, out);
			CHECK_BYTES(&sponge, zeros, sizeof(sponge));
		}
	}

	/* Fully random structures, as secrets or leftovers might leave them. */
	for (round = 0; round < 3000U; round++) {
		random_fill(&sponge, sizeof(sponge));
		if (round % 3U == 0)
			sponge.rate = rates[random_next() % (sizeof(rates) / sizeof(rates[0]))];
		else if (round % 3U == 1) {
			sponge.rate = kinds[random_next() % KIND_COUNT].rate;
			sponge.position = random_next() % (sponge.rate + 40U);
		}
		before = sponge;
		size = random_next() % 300U;
		if (!looks_usable(&sponge)) {
			cryc_sha3_absorb(&sponge, message, size);
			CHECK_BYTES(&sponge, &before, sizeof(sponge));
		} else {
			cryc_sha3_absorb(&sponge, message, size);
			cryc_shake_squeeze(&sponge, out, size);
		}
		sponge = before;
		cryc_sha3_final(&sponge, out);
		CHECK_BYTES(&sponge, zeros, sizeof(sponge));
	}
}

/*
 * Runs the Monte Carlo file of a SHAKE: starting from the 16-byte message
 * and the longest output length, 1000 times the message is the first 16
 * bytes of the last output (padded with zeros when shorter), and the next
 * output length is the minimum plus the last two bytes of the output as a
 * big-endian integer, modulo the number of lengths from minimum to maximum
 * in bytes.  The file gives the last output at each of 100 checkpoints.
 */
static unsigned long
run_shake_monte(
	int kind,
	const char *name)
{
	struct vec_file file;
	uint8_t message[16];
	uint8_t output[260];
	uint8_t expected[260];
	size_t min_size;
	size_t max_size;
	size_t out_size;
	size_t produced;
	size_t answer_size;
	size_t step;
	int have_seed;
	unsigned long count;

	/* The first record gives the message and the range. */
	count = 0;
	have_seed = 0;
	min_size = 0;
	max_size = 0;
	out_size = 0;
	produced = 0;
	memset(output, 0, sizeof(output));
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0;
	}
	while (vec_next(&file)) {
		if (vec_get(&file, "COUNT") == NULL) {
			if (vec_get(&file, "Msg") == NULL)
				continue;
			CHECK(vec_hex(&file, "Msg", message, sizeof(message)) == sizeof(message));
			min_size = (size_t)strtoul(vec_get(&file, "section.Minimum Output Length (bits)"), NULL, 10) / 8U;
			max_size = (size_t)strtoul(vec_get(&file, "section.Maximum Output Length (bits)"), NULL, 10) / 8U;
			CHECK(max_size <= sizeof(output) && max_size >= min_size && min_size >= 2U);
			out_size = max_size;
			memcpy(output, message, sizeof(message));
			have_seed = 1;
			continue;
		}
		if (!have_seed || max_size > sizeof(output))
			break;
		for (step = 0; step < 1000U; step++) {
			/* The message is the first 16 bytes of the last output, zero padded. */
			memcpy(message, output, sizeof(message));
			run_sponge(kind, message, sizeof(message), 0, output, out_size, 0);
			produced = out_size;
			if (produced < sizeof(message))
				memset(output + produced, 0, sizeof(message) - produced);
			out_size = min_size + ((((size_t)output[produced - 2U] << 8) | output[produced - 1U]) %
			    (max_size - min_size + 1U));
		}

		/* The checkpoint: the last output and its length. */
		answer_size = vec_hex(&file, "Output", expected, sizeof(expected));
		CHECK(answer_size == produced);
		if (answer_size == produced)
			CHECK_BYTES(output, expected, produced);
		count++;
	}
	vec_close(&file);
	return count;
}

/* Runs the checks. */
int
main(void)
{
	unsigned long count;
	unsigned long total;

	/* The permutation and the sponge against the references written from the text. */
	test_permute();
	test_init();
	test_reference();
	test_long();
	test_misuse();
	test_damaged();

	/* The published vectors: short and long messages of both SHA-3 hashes and both SHAKEs. */
	total = 0;
	count = run_message_file(KIND_SHA3_256, "cavp-sha-3bytetestvectors-SHA3_256ShortMsg");
	CHECK(count == 137U);
	total += count;
	count = run_message_file(KIND_SHA3_256, "cavp-sha-3bytetestvectors-SHA3_256LongMsg");
	CHECK(count == 100U);
	total += count;
	count = run_message_file(KIND_SHA3_512, "cavp-sha-3bytetestvectors-SHA3_512ShortMsg");
	CHECK(count == 73U);
	total += count;
	count = run_message_file(KIND_SHA3_512, "cavp-sha-3bytetestvectors-SHA3_512LongMsg");
	CHECK(count == 100U);
	total += count;
	count = run_message_file(KIND_SHAKE128, "cavp-shakebytetestvectors-SHAKE128ShortMsg");
	CHECK(count == 337U);
	total += count;
	count = run_message_file(KIND_SHAKE128, "cavp-shakebytetestvectors-SHAKE128LongMsg");
	CHECK(count == 100U);
	total += count;
	count = run_message_file(KIND_SHAKE256, "cavp-shakebytetestvectors-SHAKE256ShortMsg");
	CHECK(count == 273U);
	total += count;
	count = run_message_file(KIND_SHAKE256, "cavp-shakebytetestvectors-SHAKE256LongMsg");
	CHECK(count == 100U);
	total += count;
	count = run_variable_file(KIND_SHAKE128, "cavp-shakebytetestvectors-SHAKE128VariableOut");
	CHECK(count == 1126U);
	total += count;
	count = run_variable_file(KIND_SHAKE256, "cavp-shakebytetestvectors-SHAKE256VariableOut");
	CHECK(count == 1246U);
	total += count;

	/* The Monte Carlo files. */
	count = run_hash_monte_file(KIND_SHA3_256, "cavp-sha-3bytetestvectors-SHA3_256Monte");
	CHECK(count == 100U);
	total += count;
	count = run_hash_monte_file(KIND_SHA3_512, "cavp-sha-3bytetestvectors-SHA3_512Monte");
	CHECK(count == 100U);
	total += count;
	count = run_shake_monte(KIND_SHAKE128, "cavp-shakebytetestvectors-SHAKE128Monte");
	CHECK(count == 100U);
	total += count;
	count = run_shake_monte(KIND_SHAKE256, "cavp-shakebytetestvectors-SHAKE256Monte");
	CHECK(count == 100U);
	total += count;

	/* The number of vector records, for the summary line above the result. */
	vector_records = total;
	printf("test-sha3: %lu vector records\n", vector_records);

	/* The result. */
	return check_finish("test-sha3");
}
