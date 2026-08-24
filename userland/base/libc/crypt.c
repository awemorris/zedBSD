/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <crypt.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sha512_ctx {
	uint64_t h[8];
	uint64_t bytes_hi, bytes_lo;
	uint8_t block[128];
	size_t used;
};
static const uint64_t k512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static uint64_t
ror(uint64_t x, unsigned n)
{
	return (x >> n) | (x << (64U - n));
}
static uint64_t
load64(const uint8_t *p)
{
	uint64_t v = 0;
	unsigned i;
	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}
static void
store64(uint8_t *p, uint64_t v)
{
	int i;
	for (i = 7; i >= 0; i--) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}
static void
sha_block(struct sha512_ctx *c, const uint8_t *p)
{
	uint64_t w[80], a, b, d, e, f, g, h, t1, t2, cc;
	unsigned i;
	for (i = 0; i < 16; i++)
		w[i] = load64(p + 8U * i);
	for (i = 16; i < 80; i++) {
		uint64_t x = w[i - 15], y = w[i - 2];
		w[i] = w[i - 16] + (ror(x, 1) ^ ror(x, 8) ^ (x >> 7)) +
		       w[i - 7] + (ror(y, 19) ^ ror(y, 61) ^ (y >> 6));
	}
	a = c->h[0];
	b = c->h[1];
	cc = c->h[2];
	d = c->h[3];
	e = c->h[4];
	f = c->h[5];
	g = c->h[6];
	h = c->h[7];
	for (i = 0; i < 80; i++) {
		t1 = h + (ror(e, 14) ^ ror(e, 18) ^ ror(e, 41)) +
		     ((e & f) ^ ((~e) & g)) + k512[i] + w[i];
		t2 = (ror(a, 28) ^ ror(a, 34) ^ ror(a, 39)) +
		     ((a & b) ^ (a & cc) ^ (b & cc));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = cc;
		cc = b;
		b = a;
		a = t1 + t2;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
	c->h[5] += f;
	c->h[6] += g;
	c->h[7] += h;
}
static void
sha_init(struct sha512_ctx *c)
{
	static const uint64_t iv[8] = {
	    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
	    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
	memcpy(c->h, iv, sizeof(iv));
	c->bytes_hi = c->bytes_lo = 0;
	c->used = 0;
}
static void
sha_add(struct sha512_ctx *c, const void *data, size_t n)
{
	const uint8_t *p = data;
	uint64_t old = c->bytes_lo;
	c->bytes_lo += (uint64_t)n;
	if (c->bytes_lo < old)
		c->bytes_hi++;
	while (n) {
		size_t take = 128U - c->used;
		if (take > n)
			take = n;
		memcpy(c->block + c->used, p, take);
		c->used += take;
		p += take;
		n -= take;
		if (c->used == 128) {
			sha_block(c, c->block);
			c->used = 0;
		}
	}
}
static void
sha_final(struct sha512_ctx *c, uint8_t out[64])
{
	uint64_t hi = (c->bytes_hi << 3) | (c->bytes_lo >> 61),
		 lo = c->bytes_lo << 3;
	unsigned i;
	c->block[c->used++] = 0x80;
	if (c->used > 112) {
		memset(c->block + c->used, 0, 128 - c->used);
		sha_block(c, c->block);
		c->used = 0;
	}
	memset(c->block + c->used, 0, 112 - c->used);
	store64(c->block + 112, hi);
	store64(c->block + 120, lo);
	sha_block(c, c->block);
	for (i = 0; i < 8; i++)
		store64(out + 8U * i, c->h[i]);
}
static void
add_repeat(struct sha512_ctx *c, const uint8_t *p, size_t plen, size_t n)
{
	while (n) {
		size_t z = n < plen ? n : plen;
		sha_add(c, p, z);
		n -= z;
	}
}

static const char crypt64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static void
b64(char **out, unsigned b2, unsigned b1, unsigned b0, unsigned count)
{
	unsigned v = (b2 << 16) | (b1 << 8) | b0;
	while (count--) {
		*(*out)++ = crypt64[v & 0x3fU];
		v >>= 6;
	}
}

char *
crypt(const char *password, const char *setting)
{
	static _Thread_local char output[192];
	struct sha512_ctx c;
	uint8_t alt[64], dp[64], ds[64], *pseq, *sseq;
	const char *salt;
	size_t plen, slen, i;
	unsigned long rounds = 5000;
	int custom = 0;
	char *end, *o;
	if (!password || !setting || strncmp(setting, "$6$", 3)) {
		errno = EINVAL;
		return NULL;
	}
	salt = setting + 3;
	if (!strncmp(salt, "rounds=", 7)) {
		unsigned long r = strtoul(salt + 7, &end, 10);
		if (*end != '$') {
			errno = EINVAL;
			return NULL;
		}
		if (r < 1000)
			r = 1000;
		if (r > 999999999UL)
			r = 999999999UL;
		rounds = r;
		custom = 1;
		salt = end + 1;
	}
	for (slen = 0; salt[slen] && salt[slen] != '$'; slen++) {
	}
	if (slen > 16)
		slen = 16;
	plen = strlen(password);
	if (plen > 4096) {
		errno = E2BIG;
		return NULL;
	}
	pseq = malloc(plen ? plen : 1);
	sseq = malloc(slen ? slen : 1);
	if (!pseq || !sseq) {
		free(pseq);
		free(sseq);
		errno = ENOMEM;
		return NULL;
	}
	sha_init(&c);
	sha_add(&c, password, plen);
	sha_add(&c, salt, slen);
	sha_add(&c, password, plen);
	sha_final(&c, alt);
	sha_init(&c);
	sha_add(&c, password, plen);
	sha_add(&c, salt, slen);
	add_repeat(&c, alt, 64, plen);
	for (i = plen; i; i >>= 1)
		sha_add(&c, (i & 1) ? alt : (const uint8_t *)password,
			(i & 1) ? 64 : plen);
	sha_final(&c, alt);
	sha_init(&c);
	for (i = 0; i < plen; i++)
		sha_add(&c, password, plen);
	sha_final(&c, dp);
	for (i = 0; i < plen; i++)
		pseq[i] = dp[i % 64];
	sha_init(&c);
	for (i = 0; i < 16U + alt[0]; i++)
		sha_add(&c, salt, slen);
	sha_final(&c, ds);
	for (i = 0; i < slen; i++)
		sseq[i] = ds[i % 64];
	for (i = 0; i < rounds; i++) {
		uint8_t next[64];
		sha_init(&c);
		if (i & 1)
			sha_add(&c, pseq, plen);
		else
			sha_add(&c, alt, 64);
		if (i % 3)
			sha_add(&c, sseq, slen);
		if (i % 7)
			sha_add(&c, pseq, plen);
		if (i & 1)
			sha_add(&c, alt, 64);
		else
			sha_add(&c, pseq, plen);
		sha_final(&c, next);
		memcpy(alt, next, 64);
	}
	o = output;
	memcpy(o, "$6$", 3);
	o += 3;
	if (custom)
		o += snprintf(o, (size_t)(output + sizeof(output) - o),
			      "rounds=%lu$", rounds);
	memcpy(o, salt, slen);
	o += slen;
	*o++ = '$';
#define B(a, b, c, n) b64(&o, alt[a], alt[b], alt[c], n)
	B(0, 21, 42, 4);
	B(22, 43, 1, 4);
	B(44, 2, 23, 4);
	B(3, 24, 45, 4);
	B(25, 46, 4, 4);
	B(47, 5, 26, 4);
	B(6, 27, 48, 4);
	B(28, 49, 7, 4);
	B(50, 8, 29, 4);
	B(9, 30, 51, 4);
	B(31, 52, 10, 4);
	B(53, 11, 32, 4);
	B(12, 33, 54, 4);
	B(34, 55, 13, 4);
	B(56, 14, 35, 4);
	B(15, 36, 57, 4);
	B(37, 58, 16, 4);
	B(59, 17, 38, 4);
	B(18, 39, 60, 4);
	B(40, 61, 19, 4);
	B(62, 20, 41, 4);
	b64(&o, 0, 0, alt[63], 2);
#undef B
	*o = '\0';
	memset(dp, 0, sizeof(dp));
	memset(ds, 0, sizeof(ds));
	memset(alt, 0, sizeof(alt));
	memset(pseq, 0, plen);
	memset(sseq, 0, slen);
	free(pseq);
	free(sseq);
	return output;
}
