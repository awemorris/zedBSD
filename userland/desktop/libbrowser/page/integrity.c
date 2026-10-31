/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Subresource Integrity: the SHA-256, SHA-384 and SHA-512 digests of a resource against an integrity attribute's metadata. */

#include "page/page.h"

#include <stdint.h>
#include <string.h>

/* SHA-256's round constants and initial state. */
static const uint32_t integrity_k256[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static const uint32_t integrity_h256[8] = {
	0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

/* SHA-512's round constants, and the initial states of SHA-512 and SHA-384. */
static const uint64_t integrity_k512[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static const uint64_t integrity_h512[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static const uint64_t integrity_h384[8] = {
	0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
	0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};

/* Rotates right. */
static uint32_t
integrity_rotr32(
	uint32_t value,
	unsigned count)
{
	return (value >> count) | (value << (32U - count));
}

static uint64_t
integrity_rotr64(
	uint64_t value,
	unsigned count)
{
	return (value >> count) | (value << (64U - count));
}

/* Hashes with SHA-256 into 32 bytes. */
static void
integrity_sha256(
	const unsigned char *data,
	size_t length,
	unsigned char *digest)
{
	uint32_t state[8];
	uint32_t w[64];
	uint32_t a[8];
	unsigned char block[128];
	uint32_t s0;
	uint32_t s1;
	uint32_t t1;
	uint32_t t2;
	size_t offset;
	size_t rest;
	size_t blocks;
	size_t index;
	unsigned round;
	uint64_t bits;

	memcpy(state, integrity_h256, sizeof(state));
	bits = (uint64_t)length * 8U;
	blocks = (length + 9U + 63U) / 64U;
	for (index = 0; index < blocks; index++) {
		/* The block: message bytes, the 1 bit, zeros, and the bit length at the end. */
		memset(block, 0, 64);
		offset = index * 64U;
		if (offset < length) {
			rest = length - offset;
			if (rest > 64U)
				rest = 64U;
			memcpy(block, data + offset, rest);
		} else {
			rest = 0;
		}
		if (offset <= length && length - offset < 64U)
			block[length - offset] = 0x80U;
		if (index == blocks - 1U) {
			for (round = 0; round < 8U; round++)
				block[63U - round] = (unsigned char)(bits >> (8U * round));
		}
		for (round = 0; round < 16U; round++)
			w[round] = ((uint32_t)block[round * 4U] << 24) | ((uint32_t)block[round * 4U + 1U] << 16) |
			    ((uint32_t)block[round * 4U + 2U] << 8) | (uint32_t)block[round * 4U + 3U];
		for (round = 16U; round < 64U; round++) {
			s0 = integrity_rotr32(w[round - 15U], 7) ^ integrity_rotr32(w[round - 15U], 18) ^ (w[round - 15U] >> 3);
			s1 = integrity_rotr32(w[round - 2U], 17) ^ integrity_rotr32(w[round - 2U], 19) ^ (w[round - 2U] >> 10);
			w[round] = w[round - 16U] + s0 + w[round - 7U] + s1;
		}
		memcpy(a, state, sizeof(a));
		for (round = 0; round < 64U; round++) {
			s1 = integrity_rotr32(a[4], 6) ^ integrity_rotr32(a[4], 11) ^ integrity_rotr32(a[4], 25);
			t1 = a[7] + s1 + ((a[4] & a[5]) ^ (~a[4] & a[6])) + integrity_k256[round] + w[round];
			s0 = integrity_rotr32(a[0], 2) ^ integrity_rotr32(a[0], 13) ^ integrity_rotr32(a[0], 22);
			t2 = s0 + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
			a[7] = a[6];
			a[6] = a[5];
			a[5] = a[4];
			a[4] = a[3] + t1;
			a[3] = a[2];
			a[2] = a[1];
			a[1] = a[0];
			a[0] = t1 + t2;
		}
		for (round = 0; round < 8U; round++)
			state[round] += a[round];
	}
	for (index = 0; index < 8U; index++) {
		digest[index * 4U] = (unsigned char)(state[index] >> 24);
		digest[index * 4U + 1U] = (unsigned char)(state[index] >> 16);
		digest[index * 4U + 2U] = (unsigned char)(state[index] >> 8);
		digest[index * 4U + 3U] = (unsigned char)state[index];
	}
}

/* Hashes with SHA-512 (or SHA-384, which starts from another state and keeps 48 bytes) into 64 bytes. */
static void
integrity_sha512(
	const unsigned char *data,
	size_t length,
	int is384,
	unsigned char *digest)
{
	uint64_t state[8];
	uint64_t w[80];
	uint64_t a[8];
	unsigned char block[128];
	uint64_t s0;
	uint64_t s1;
	uint64_t t1;
	uint64_t t2;
	uint64_t bits;
	size_t offset;
	size_t rest;
	size_t blocks;
	size_t index;
	unsigned round;

	memcpy(state, is384 ? integrity_h384 : integrity_h512, sizeof(state));
	bits = (uint64_t)length * 8U;
	blocks = (length + 17U + 127U) / 128U;
	for (index = 0; index < blocks; index++) {
		memset(block, 0, 128);
		offset = index * 128U;
		if (offset < length) {
			rest = length - offset;
			if (rest > 128U)
				rest = 128U;
			memcpy(block, data + offset, rest);
		}
		if (offset <= length && length - offset < 128U)
			block[length - offset] = 0x80U;
		if (index == blocks - 1U) {
			for (round = 0; round < 8U; round++)
				block[127U - round] = (unsigned char)(bits >> (8U * round));
		}
		for (round = 0; round < 16U; round++) {
			w[round] = 0;
			for (rest = 0; rest < 8U; rest++)
				w[round] = (w[round] << 8) | block[round * 8U + rest];
		}
		for (round = 16U; round < 80U; round++) {
			s0 = integrity_rotr64(w[round - 15U], 1) ^ integrity_rotr64(w[round - 15U], 8) ^ (w[round - 15U] >> 7);
			s1 = integrity_rotr64(w[round - 2U], 19) ^ integrity_rotr64(w[round - 2U], 61) ^ (w[round - 2U] >> 6);
			w[round] = w[round - 16U] + s0 + w[round - 7U] + s1;
		}
		memcpy(a, state, sizeof(a));
		for (round = 0; round < 80U; round++) {
			s1 = integrity_rotr64(a[4], 14) ^ integrity_rotr64(a[4], 18) ^ integrity_rotr64(a[4], 41);
			t1 = a[7] + s1 + ((a[4] & a[5]) ^ (~a[4] & a[6])) + integrity_k512[round] + w[round];
			s0 = integrity_rotr64(a[0], 28) ^ integrity_rotr64(a[0], 34) ^ integrity_rotr64(a[0], 39);
			t2 = s0 + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
			a[7] = a[6];
			a[6] = a[5];
			a[5] = a[4];
			a[4] = a[3] + t1;
			a[3] = a[2];
			a[2] = a[1];
			a[1] = a[0];
			a[0] = t1 + t2;
		}
		for (round = 0; round < 8U; round++)
			state[round] += a[round];
	}
	for (index = 0; index < 8U; index++) {
		for (round = 0; round < 8U; round++)
			digest[index * 8U + round] = (unsigned char)(state[index] >> (56U - 8U * round));
	}
}

/* Writes bytes in the standard base64 alphabet with padding; reports the length. */
static size_t
integrity_base64(
	const unsigned char *bytes,
	size_t length,
	char *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t index;
	size_t made;
	uint32_t group;

	made = 0;
	for (index = 0; index < length; index += 3U) {
		group = (uint32_t)bytes[index] << 16;
		if (index + 1U < length)
			group |= (uint32_t)bytes[index + 1U] << 8;
		if (index + 2U < length)
			group |= bytes[index + 2U];
		out[made++] = alphabet[(group >> 18) & 63U];
		out[made++] = alphabet[(group >> 12) & 63U];
		out[made++] = index + 1U < length ? alphabet[(group >> 6) & 63U] : '=';
		out[made++] = index + 2U < length ? alphabet[group & 63U] : '=';
	}
	return made;
}

/*
 * Tells whether a resource matches an integrity attribute's metadata (space separated "algorithm-digest" tokens, an
 * option after '?' ignored): only the strongest of the supported algorithms named counts, and any of its digests matching
 * is enough; metadata with no supported token matches anything.
 */
int
page_integrity_matches(
	const char *metadata,
	const unsigned char *data,
	size_t length)
{
	static const char *const names[3] = { "sha256", "sha384", "sha512" };
	static const size_t sizes[3] = { 32, 48, 64 };
	unsigned char digest[64];
	char encoded[100];
	const char *cursor;
	const char *end;
	const char *dash;
	const char *stop;
	size_t span;
	size_t made;
	size_t digits;
	int algorithm;
	int best;
	int pass;

	/* The strongest supported algorithm named. */
	best = -1;
	for (pass = 0; pass < 2; pass++) {
		cursor = metadata;
		while (*cursor != '\0') {
			while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r' || *cursor == '\f')
				cursor++;
			end = cursor;
			while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != '\f')
				end++;
			if (end == cursor)
				break;
			dash = memchr(cursor, '-', (size_t)(end - cursor));
			if (dash != NULL) {
				span = (size_t)(dash - cursor);
				for (algorithm = 0; algorithm < 3; algorithm++) {
					if (strlen(names[algorithm]) != span || strncmp(cursor, names[algorithm], span) != 0)
						continue;
					if (pass == 0 && algorithm > best)
						best = algorithm;
					if (pass == 1 && algorithm == best) {
						/* The digest: up to an option, in either base64 alphabet, padding optional. */
						stop = memchr(dash + 1, '?', (size_t)(end - dash - 1));
						if (stop == NULL)
							stop = end;
						if (algorithm == 0)
							integrity_sha256(data, length, digest);
						else
							integrity_sha512(data, length, algorithm == 1, digest);
						made = integrity_base64(digest, sizes[algorithm], encoded);
						digits = 0;
						while (digits < made && encoded[digits] != '=')
							digits++;
						span = (size_t)(stop - dash - 1);
						while (span > 0 && dash[span] == '=')
							span--;
						if (span == digits) {
							for (made = 0; made < digits; made++) {
								char c;

								c = dash[1U + made];
								if (c == '-')
									c = '+';
								else if (c == '_')
									c = '/';
								if (c != encoded[made])
									break;
							}
							if (made == digits)
								return 1;
						}
					}
				}
			}
			cursor = end;
		}
		if (pass == 0 && best < 0)
			return 1;
	}
	return 0;
}
