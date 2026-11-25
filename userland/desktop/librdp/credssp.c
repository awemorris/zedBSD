/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * CredSSP (MS-CSSP) + SPNEGO + NTLMv2 server state machine for librdp.
 */

#include "rdp-internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

/* NTLMSSP message types and negotiate flags (MS-NLMP). */
#define NTLM_MSG_NEGOTIATE		1U
#define NTLM_MSG_CHALLENGE		2U
#define NTLM_MSG_AUTHENTICATE		3U

#define NTLM_FLAG_UNICODE		0x00000001U
#define NTLM_FLAG_REQUEST_TARGET	0x00000004U
#define NTLM_FLAG_SIGN			0x00000010U
#define NTLM_FLAG_SEAL			0x00000020U
#define NTLM_FLAG_NTLM			0x00000200U
#define NTLM_FLAG_ALWAYS_SIGN		0x00008000U
#define NTLM_FLAG_TARGET_TYPE_SERVER	0x00020000U
#define NTLM_FLAG_EXT_SESSION_SEC	0x00080000U
#define NTLM_FLAG_TARGET_INFO		0x00800000U
#define NTLM_FLAG_VERSION		0x02000000U
#define NTLM_FLAG_128			0x20000000U
#define NTLM_FLAG_KEY_EXCH		0x40000000U
#define NTLM_FLAG_56			0x80000000U

/* NTLM AV_PAIR IDs. */
#define NTLM_AV_EOL			0U
#define NTLM_AV_NB_COMPUTER		1U
#define NTLM_AV_NB_DOMAIN		2U
#define NTLM_AV_DNS_COMPUTER		3U
#define NTLM_AV_TIMESTAMP		7U

/* CredSSP binding hash magic strings (MS-CSSP 3.1.5.1.2, including NUL). */
static const char CSSP_CS_MAGIC[] = "CredSSP Client-To-Server Binding Hash";
static const char CSSP_SC_MAGIC[] = "CredSSP Server-To-Client Binding Hash";

/* NTLMSSP OID 1.3.6.1.4.1.311.2.2.10. */
static const uint8_t NTLM_OID[10] = {
	0x2BU, 0x06U, 0x01U, 0x04U, 0x01U, 0x82U, 0x37U, 0x02U, 0x02U, 0x0AU
};

/* ---------- Minimal MD4 / MD5 / HMAC-MD5 / RC4 / SHA-256 ---------- */

static uint32_t
rotl32(uint32_t x, uint32_t n)
{
	return (x << n) | (x >> (32U - n));
}

static uint32_t
rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32U - n));
}

static uint32_t
load32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
store32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t
load32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void
store32_be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)((v >> 24) & 0xFFU);
	p[1] = (uint8_t)((v >> 16) & 0xFFU);
	p[2] = (uint8_t)((v >> 8) & 0xFFU);
	p[3] = (uint8_t)(v & 0xFFU);
}

static void
md4_block(uint32_t st[4], const uint8_t blk[64])
{
	static const uint32_t r1[16] = {
		3, 7, 11, 19, 3, 7, 11, 19, 3, 7, 11, 19, 3, 7, 11, 19
	};
	static const uint32_t idx2[16] = {
		0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15
	};
	static const uint32_t r2[16] = {
		3, 5, 9, 13, 3, 5, 9, 13, 3, 5, 9, 13, 3, 5, 9, 13
	};
	static const uint32_t idx3[16] = {
		0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
	};
	static const uint32_t r3[16] = {
		3, 9, 11, 15, 3, 9, 11, 15, 3, 9, 11, 15, 3, 9, 11, 15
	};
	uint32_t x[16];
	uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
	uint32_t i, f, t;

	for (i = 0U; i < 16U; i++)
		x[i] = load32_le(blk + i * 4U);
	for (i = 0U; i < 16U; i++) {
		f = (b & c) | ((~b) & d);
		t = rotl32(a + f + x[i], r1[i]);
		a = d; d = c; c = b; b = t;
	}
	for (i = 0U; i < 16U; i++) {
		f = (b & c) | (b & d) | (c & d);
		t = rotl32(a + f + x[idx2[i]] + 0x5A827999U, r2[i]);
		a = d; d = c; c = b; b = t;
	}
	for (i = 0U; i < 16U; i++) {
		f = b ^ c ^ d;
		t = rotl32(a + f + x[idx3[i]] + 0x6ED9EBA1U, r3[i]);
		a = d; d = c; c = b; b = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void
md4_hash(const uint8_t *data, size_t len, uint8_t out[16])
{
	uint32_t st[4] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U };
	uint8_t blk[64];
	size_t off = 0U;
	uint64_t bits = (uint64_t)len * 8ULL;

	while (off + 64U <= len) {
		md4_block(st, data + off);
		off += 64U;
	}
	memset(blk, 0, sizeof(blk));
	if (len > off)
		memcpy(blk, data + off, len - off);
	blk[len - off] = 0x80U;
	if (len - off >= 56U) {
		md4_block(st, blk);
		memset(blk, 0, sizeof(blk));
	}
	store32_le(blk + 56, (uint32_t)(bits & 0xFFFFFFFFULL));
	store32_le(blk + 60, (uint32_t)(bits >> 32));
	md4_block(st, blk);
	for (off = 0U; off < 4U; off++)
		store32_le(out + off * 4U, st[off]);
}

static void
md5_block(uint32_t st[4], const uint8_t blk[64])
{
	static const uint32_t k[64] = {
		0xD76AA478U, 0xE8C7B756U, 0x242070DBU, 0xC1BDCEEEU,
		0xF57C0FAFU, 0x4787C62AU, 0xA8304613U, 0xFD469501U,
		0x698098D8U, 0x8B44F7AFU, 0xFFFF5BB1U, 0x895CD7BEU,
		0x6B901122U, 0xFD987193U, 0xA679438EU, 0x49B40821U,
		0xF61E2562U, 0xC040B340U, 0x265E5A51U, 0xE9B6C7AAU,
		0xD62F105DU, 0x02441453U, 0xD8A1E681U, 0xE7D3FBC8U,
		0x21E1CDE6U, 0xC33707D6U, 0xF4D50D87U, 0x455A14EDU,
		0xA9E3E905U, 0xFCEFA3F8U, 0x676F02D9U, 0x8D2A4C8AU,
		0xFFFA3942U, 0x8771F681U, 0x6D9D6122U, 0xFDE5380CU,
		0xA4BEEA44U, 0x4BDECFA9U, 0xF6BB4B60U, 0xBEBFBC70U,
		0x289B7EC6U, 0xEAA127FAU, 0xD4EF3085U, 0x04881D05U,
		0xD9D4D039U, 0xE6DB99E5U, 0x1FA27CF8U, 0xC4AC5665U,
		0xF4292244U, 0x432AFF97U, 0xAB9423A7U, 0xFC93A039U,
		0x655B59C3U, 0x8F0CCC92U, 0xFFEFF47DU, 0x85845DD1U,
		0x6FA87E4FU, 0xFE2CE6E0U, 0xA3014314U, 0x4E0811A1U,
		0xF7537E82U, 0xBD3AF235U, 0x2AD7D2BBU, 0xEB86D391U
	};
	static const uint32_t s[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
	};
	uint32_t m[16];
	uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
	uint32_t i, f, g, t;

	for (i = 0U; i < 16U; i++)
		m[i] = load32_le(blk + i * 4U);
	for (i = 0U; i < 64U; i++) {
		if (i < 16U) {
			f = (b & c) | ((~b) & d);
			g = i;
		} else if (i < 32U) {
			f = (d & b) | ((~d) & c);
			g = (5U * i + 1U) & 15U;
		} else if (i < 48U) {
			f = b ^ c ^ d;
			g = (3U * i + 5U) & 15U;
		} else {
			f = c ^ (b | (~d));
			g = (7U * i) & 15U;
		}
		t = d;
		d = c;
		c = b;
		b = b + rotl32(a + f + k[i] + m[g], s[i]);
		a = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

struct md5_ctx {
	uint32_t st[4];
	uint8_t buf[64];
	size_t buf_len;
	uint64_t total_len;
};

static void
md5_init(struct md5_ctx *c)
{
	c->st[0] = 0x67452301U;
	c->st[1] = 0xEFCDAB89U;
	c->st[2] = 0x98BADCFEU;
	c->st[3] = 0x10325476U;
	c->buf_len = 0U;
	c->total_len = 0ULL;
}

static void
md5_update(struct md5_ctx *c, const uint8_t *data, size_t len)
{
	size_t off = 0U;

	c->total_len += (uint64_t)len;
	if (c->buf_len > 0U && c->buf_len + len >= 64U) {
		size_t take = 64U - c->buf_len;
		memcpy(c->buf + c->buf_len, data, take);
		md5_block(c->st, c->buf);
		c->buf_len = 0U;
		off = take;
	}
	while (off + 64U <= len) {
		md5_block(c->st, data + off);
		off += 64U;
	}
	if (off < len) {
		memcpy(c->buf + c->buf_len, data + off, len - off);
		c->buf_len += len - off;
	}
}

static void
md5_final(struct md5_ctx *c, uint8_t out[16])
{
	uint64_t bits = c->total_len * 8ULL;
	size_t i;

	c->buf[c->buf_len++] = 0x80U;
	if (c->buf_len > 56U) {
		while (c->buf_len < 64U)
			c->buf[c->buf_len++] = 0U;
		md5_block(c->st, c->buf);
		c->buf_len = 0U;
	}
	while (c->buf_len < 56U)
		c->buf[c->buf_len++] = 0U;
	store32_le(c->buf + 56, (uint32_t)(bits & 0xFFFFFFFFULL));
	store32_le(c->buf + 60, (uint32_t)(bits >> 32));
	md5_block(c->st, c->buf);
	for (i = 0U; i < 4U; i++)
		store32_le(out + i * 4U, c->st[i]);
}

static void
hmac_md5_2(
	const uint8_t *key,
	size_t klen,
	const uint8_t *d1,
	size_t l1,
	const uint8_t *d2,
	size_t l2,
	uint8_t out[16])
{
	uint8_t k0[64], ipad[64], opad[64], inner[16];
	struct md5_ctx c;
	size_t i;

	memset(k0, 0, sizeof(k0));
	if (klen > 64U) {
		md5_init(&c);
		md5_update(&c, key, klen);
		md5_final(&c, k0);
	} else if (klen > 0U) {
		memcpy(k0, key, klen);
	}
	for (i = 0U; i < 64U; i++) {
		ipad[i] = k0[i] ^ 0x36U;
		opad[i] = k0[i] ^ 0x5CU;
	}
	md5_init(&c);
	md5_update(&c, ipad, 64U);
	if (l1 > 0U)
		md5_update(&c, d1, l1);
	if (l2 > 0U)
		md5_update(&c, d2, l2);
	md5_final(&c, inner);

	md5_init(&c);
	md5_update(&c, opad, 64U);
	md5_update(&c, inner, 16U);
	md5_final(&c, out);
}

static void
sha256_block(uint32_t st[8], const uint8_t blk[64])
{
	static const uint32_t k[64] = {
		0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
		0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
		0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
		0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
		0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
		0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
		0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
		0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
		0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
		0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
		0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
		0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
		0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
		0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
		0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
		0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
	};
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h, i, s0, s1, ch, maj, t1, t2;

	for (i = 0U; i < 16U; i++)
		w[i] = load32_be(blk + i * 4U);
	for (i = 16U; i < 64U; i++) {
		s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}
	a = st[0]; b = st[1]; c = st[2]; d = st[3];
	e = st[4]; f = st[5]; g = st[6]; h = st[7];
	for (i = 0U; i < 64U; i++) {
		s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
		ch = (e & f) ^ ((~e) & g);
		t1 = h + s1 + ch + k[i] + w[i];
		s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
		maj = (a & b) ^ (a & c) ^ (b & c);
		t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
	st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void
sha256_hash3(
	const uint8_t *d1, size_t l1,
	const uint8_t *d2, size_t l2,
	const uint8_t *d3, size_t l3,
	uint8_t out[32])
{
	uint32_t st[8] = {
		0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
		0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
	};
	const uint8_t *parts[3] = { d1, d2, d3 };
	size_t lens[3] = { l1, l2, l3 };
	uint8_t blk[64];
	size_t blen = 0U;
	uint64_t total = (uint64_t)(l1 + l2 + l3) * 8ULL;
	size_t p, off;

	for (p = 0U; p < 3U; p++) {
		const uint8_t *d = parts[p];
		size_t l = lens[p];
		off = 0U;
		if (blen > 0U && blen + l >= 64U) {
			size_t take = 64U - blen;
			memcpy(blk + blen, d, take);
			sha256_block(st, blk);
			blen = 0U;
			off = take;
		}
		while (off + 64U <= l) {
			sha256_block(st, d + off);
			off += 64U;
		}
		if (off < l) {
			memcpy(blk + blen, d + off, l - off);
			blen += l - off;
		}
	}
	blk[blen++] = 0x80U;
	if (blen > 56U) {
		while (blen < 64U)
			blk[blen++] = 0U;
		sha256_block(st, blk);
		blen = 0U;
	}
	while (blen < 56U)
		blk[blen++] = 0U;
	store32_be(blk + 56, (uint32_t)(total >> 32));
	store32_be(blk + 60, (uint32_t)(total & 0xFFFFFFFFULL));
	sha256_block(st, blk);
	for (p = 0U; p < 8U; p++)
		store32_be(out + p * 4U, st[p]);
}

static void
rc4_init(struct rdp_rc4 *rc4, const uint8_t *key, size_t klen)
{
	uint32_t i;
	uint8_t j = 0U, tmp;

	for (i = 0U; i < 256U; i++)
		rc4->s[i] = (uint8_t)i;
	for (i = 0U; i < 256U; i++) {
		j = (uint8_t)(j + rc4->s[i] + key[i % klen]);
		tmp = rc4->s[i];
		rc4->s[i] = rc4->s[j];
		rc4->s[j] = tmp;
	}
	rc4->i = 0U;
	rc4->j = 0U;
}

static void
rc4_crypt(struct rdp_rc4 *rc4, const uint8_t *in, uint8_t *out, size_t len)
{
	size_t k;
	uint8_t tmp;

	for (k = 0U; k < len; k++) {
		rc4->i = (uint8_t)(rc4->i + 1U);
		rc4->j = (uint8_t)(rc4->j + rc4->s[rc4->i]);
		tmp = rc4->s[rc4->i];
		rc4->s[rc4->i] = rc4->s[rc4->j];
		rc4->s[rc4->j] = tmp;
		out[k] = in[k] ^ rc4->s[(uint8_t)(rc4->s[rc4->i] + rc4->s[rc4->j])];
	}
}

static void
random_bytes(uint8_t *out, size_t len)
{
#if !defined(_WIN32)
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		size_t off = 0U;
		while (off < len) {
			ssize_t r = read(fd, out + off, len - off);
			if (r <= 0)
				break;
			off += (size_t)r;
		}
		close(fd);
		if (off == len)
			return;
	}
#endif
	{
		size_t i;
		uint32_t v = (uint32_t)time(NULL) ^ 0xA55AA55AU;
		for (i = 0U; i < len; i++) {
			v = v * 1664525U + 1013904223U;
			out[i] = (uint8_t)(v >> 24);
		}
	}
}

/*
 * Computes the 16-byte NTLM NT hash = MD4(UTF16LE(password)).
 */
void
rdp_ntlm_nt_hash(
	const char *password,
	uint8_t out[16])
{
	struct rdp_buf u16;
	size_t i;
	size_t plen = (password != NULL) ? strlen(password) : 0U;

	rdp_buf_init(&u16);
	for (i = 0U; i < plen; i++)
		(void)rdp_buf_append_u16_le(&u16, (uint8_t)password[i]);
	md4_hash(u16.data != NULL ? u16.data : (const uint8_t *)"", u16.len, out);
	rdp_buf_free(&u16);
}

/* ---------- UTF-16LE helpers ---------- */

static int
append_utf8_as_utf16le(struct rdp_buf *out, const char *s)
{
	size_t i;
	size_t n = (s != NULL) ? strlen(s) : 0U;
	int error;

	for (i = 0U; i < n; i++) {
		error = rdp_buf_append_u16_le(out, (uint8_t)s[i]);
		if (error != RDP_OK)
			return error;
	}
	return RDP_OK;
}

static void
utf16le_to_utf8(const uint8_t *src, size_t len, char *dst, size_t cap)
{
	size_t i;
	size_t o = 0U;

	if (cap == 0U)
		return;
	for (i = 0U; i + 1U < len && o + 1U < cap; i += 2U) {
		uint16_t ch = (uint16_t)src[i] | ((uint16_t)src[i + 1U] << 8);
		if (ch == 0U)
			break;
		dst[o++] = (ch < 0x80U) ? (char)ch : '?';
	}
	dst[o] = '\0';
}

/* ---------- ASN.1 DER helpers ---------- */

static int
der_read_tlv(
	struct rdp_reader *r,
	uint8_t expected_tag,
	const uint8_t **val,
	size_t *val_len)
{
	uint8_t tag;
	uint8_t len0;
	size_t len = 0U;
	size_t nbytes;
	size_t i;
	int error;

	error = rdp_read_u8(r, &tag);
	if (error != RDP_OK)
		return error;
	if (expected_tag != 0U && tag != expected_tag)
		return RDP_ERR_PROTO;
	error = rdp_read_u8(r, &len0);
	if (error != RDP_OK)
		return error;
	if ((len0 & 0x80U) == 0U) {
		len = len0;
	} else {
		nbytes = (size_t)(len0 & 0x7FU);
		if (nbytes == 0U || nbytes > 4U)
			return RDP_ERR_PROTO;
		for (i = 0U; i < nbytes; i++) {
			uint8_t b;
			error = rdp_read_u8(r, &b);
			if (error != RDP_OK)
				return error;
			len = (len << 8) | b;
		}
	}
	if (rdp_reader_rem(r) < len)
		return RDP_ERR_PROTO;
	*val_len = len;
	return rdp_read_bytes(r, val, len);
}

static int
der_read_any(
	struct rdp_reader *r,
	uint8_t *out_tag,
	const uint8_t **val,
	size_t *val_len)
{
	uint8_t tag;
	uint8_t len0;
	size_t len = 0U;
	size_t nbytes;
	size_t i;
	int error;

	error = rdp_read_u8(r, &tag);
	if (error != RDP_OK)
		return error;
	*out_tag = tag;
	error = rdp_read_u8(r, &len0);
	if (error != RDP_OK)
		return error;
	if ((len0 & 0x80U) == 0U) {
		len = len0;
	} else {
		nbytes = (size_t)(len0 & 0x7FU);
		if (nbytes == 0U || nbytes > 4U)
			return RDP_ERR_PROTO;
		for (i = 0U; i < nbytes; i++) {
			uint8_t b;
			error = rdp_read_u8(r, &b);
			if (error != RDP_OK)
				return error;
			len = (len << 8) | b;
		}
	}
	if (rdp_reader_rem(r) < len)
		return RDP_ERR_PROTO;
	*val_len = len;
	return rdp_read_bytes(r, val, len);
}

static int
der_append_len(struct rdp_buf *out, size_t len)
{
	int error;
	if (len < 0x80U)
		return rdp_buf_append_u8(out, (uint8_t)len);
	if (len <= 0xFFU) {
		error = rdp_buf_append_u8(out, 0x81U);
		if (error != RDP_OK)
			return error;
		return rdp_buf_append_u8(out, (uint8_t)len);
	}
	if (len <= 0xFFFFU) {
		error = rdp_buf_append_u8(out, 0x82U);
		if (error != RDP_OK)
			return error;
		error = rdp_buf_append_u8(out, (uint8_t)(len >> 8));
		if (error != RDP_OK)
			return error;
		return rdp_buf_append_u8(out, (uint8_t)(len & 0xFFU));
	}
	return RDP_ERR_INVAL;
}

static int
der_append_tlv(struct rdp_buf *out, uint8_t tag, const uint8_t *val, size_t len)
{
	int error;
	error = rdp_buf_append_u8(out, tag);
	if (error != RDP_OK)
		return error;
	error = der_append_len(out, len);
	if (error != RDP_OK)
		return error;
	if (len > 0U)
		return rdp_buf_append(out, val, len);
	return RDP_OK;
}

static int
der_append_int_u32(struct rdp_buf *out, uint32_t v)
{
	uint8_t b[5];
	size_t n = 0U;

	if (v < 0x80U) {
		b[0] = (uint8_t)v;
		n = 1U;
	} else if (v <= 0x7FFFU) {
		b[0] = (uint8_t)(v >> 8);
		b[1] = (uint8_t)(v & 0xFFU);
		n = 2U;
	} else {
		if ((v & 0x80000000U) != 0U) {
			b[n++] = 0x00U;
		}
		b[n++] = (uint8_t)(v >> 24);
		b[n++] = (uint8_t)((v >> 16) & 0xFFU);
		b[n++] = (uint8_t)((v >> 8) & 0xFFU);
		b[n++] = (uint8_t)(v & 0xFFU);
	}
	return der_append_tlv(out, 0x02U, b, n);
}

/*
 * Checks if a complete DER SEQUENCE PDU is buffered and returns its total byte length.
 */
int
rdp_credssp_pdu_len(
	const uint8_t *data,
	size_t len,
	size_t *out_pdu_len)
{
	size_t hdr_len;
	size_t body_len = 0U;
	size_t nbytes;
	size_t i;

	if (len < 2U)
		return 0;
	if (data[0] != 0x30U)
		return -1;
	if ((data[1] & 0x80U) == 0U) {
		hdr_len = 2U;
		body_len = data[1];
	} else {
		nbytes = (size_t)(data[1] & 0x7FU);
		if (nbytes == 0U || nbytes > 3U)
			return -1;
		hdr_len = 2U + nbytes;
		if (len < hdr_len)
			return 0;
		for (i = 0U; i < nbytes; i++)
			body_len = (body_len << 8) | data[2U + i];
	}
	if (len < hdr_len + body_len)
		return 0;
	*out_pdu_len = hdr_len + body_len;
	return 1;
}

/*
 * Extracts the leaf subjectPublicKey BIT STRING payload from a DER X.509 cert.
 */
int
rdp_credssp_extract_pubkey(
	const uint8_t *cert_der,
	size_t cert_len,
	const uint8_t **out_pk,
	size_t *out_pk_len)
{
	struct rdp_reader r, tr, spki;
	const uint8_t *cert_body, *tbs, *val, *spki_val, *bits;
	size_t cert_blen, tbs_len, vlen, spki_len, bits_len;
	uint8_t tag;
	int error;

	rdp_reader_init(&r, cert_der, cert_len);
	error = der_read_any(&r, &tag, &cert_body, &cert_blen);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&r, cert_body, cert_blen);
	error = der_read_any(&r, &tag, &tbs, &tbs_len);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&tr, tbs, tbs_len);

	/* Optional [0] version. */
	if (rdp_reader_rem(&tr) > 0U && tr.data[tr.pos] == 0xA0U) {
		error = der_read_any(&tr, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
	}
	/* serialNumber, signature, issuer, validity, subject. */
	for (tag = 0U; tag < 5U; tag++) {
		uint8_t t2;
		error = der_read_any(&tr, &t2, &val, &vlen);
		if (error != RDP_OK)
			return error;
	}
	/* subjectPublicKeyInfo SEQUENCE. */
	error = der_read_any(&tr, &tag, &spki_val, &spki_len);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&spki, spki_val, spki_len);
	error = der_read_any(&spki, &tag, &val, &vlen); /* algorithm */
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	error = der_read_any(&spki, &tag, &bits, &bits_len); /* BIT STRING */
	if (error != RDP_OK || tag != 0x03U || bits_len < 2U)
		return RDP_ERR_PROTO;
	*out_pk = bits + 1U;
	*out_pk_len = bits_len - 1U;
	return RDP_OK;
}

/* ---------- SPNEGO helpers ---------- */

static int
spnego_unwrap_ntlm(
	const uint8_t *spnego,
	size_t len,
	const uint8_t **out_ntlm,
	size_t *out_len)
{
	struct rdp_reader r, ir;
	uint8_t tag;
	const uint8_t *val, *inner;
	size_t vlen, ilen;
	int error;

	/* Raw NTLMSSP fallback if not SPNEGO-wrapped. */
	if (len >= 8U && memcmp(spnego, "NTLMSSP\0", 8U) == 0) {
		*out_ntlm = spnego;
		*out_len = len;
		return RDP_OK;
	}
	rdp_reader_init(&r, spnego, len);
	error = der_read_any(&r, &tag, &val, &vlen);
	if (error != RDP_OK)
		return error;
	if (tag == 0x60U) {
		/* GSS-API InitialContextToken: OID + [0] NegTokenInit. */
		rdp_reader_init(&r, val, vlen);
		error = der_read_any(&r, &tag, &inner, &ilen);
		if (error != RDP_OK || tag != 0x06U)
			return RDP_ERR_PROTO;
		error = der_read_any(&r, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
	}
	if (tag != 0xA0U && tag != 0xA1U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&r, val, vlen);
	error = der_read_any(&r, &tag, &inner, &ilen);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&ir, inner, ilen);
	while (rdp_reader_rem(&ir) > 0U) {
		error = der_read_any(&ir, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
		if (tag == 0xA2U) {
			struct rdp_reader or;
			rdp_reader_init(&or, val, vlen);
			error = der_read_any(&or, &tag, out_ntlm, out_len);
			if (error != RDP_OK || tag != 0x04U)
				return RDP_ERR_PROTO;
			return RDP_OK;
		}
	}
	return RDP_ERR_PROTO;
}

static int
spnego_wrap_challenge(
	struct rdp_buf *out,
	const uint8_t *ntlm,
	size_t ntlm_len)
{
	struct rdp_buf seq, tmp;
	uint8_t enum_val[3] = { 0x0AU, 0x01U, 0x01U }; /* accept-incomplete */
	int error;

	rdp_buf_init(&seq);
	rdp_buf_init(&tmp);

	/* [0] negState = accept-incomplete (1). */
	error = der_append_tlv(&seq, 0xA0U, enum_val, sizeof(enum_val));
	if (error != RDP_OK)
		goto fail;
	/* [1] supportedMech = NTLMSSP OID. */
	error = der_append_tlv(&tmp, 0x06U, NTLM_OID, sizeof(NTLM_OID));
	if (error != RDP_OK)
		goto fail;
	error = der_append_tlv(&seq, 0xA1U, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;
	/* [2] responseToken = OCTET STRING(ntlm). */
	rdp_buf_clear(&tmp);
	error = der_append_tlv(&tmp, 0x04U, ntlm, ntlm_len);
	if (error != RDP_OK)
		goto fail;
	error = der_append_tlv(&seq, 0xA2U, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;

	/* NegTokenResp SEQUENCE wrapped in [1]. */
	rdp_buf_clear(&tmp);
	error = der_append_tlv(&tmp, 0x30U, seq.data, seq.len);
	if (error != RDP_OK)
		goto fail;
	error = der_append_tlv(out, 0xA1U, tmp.data, tmp.len);
fail:
	rdp_buf_free(&seq);
	rdp_buf_free(&tmp);
	return error;
}

/* ---------- NTLMv2 server ---------- */

static int
ntlm_build_challenge(
	struct rdp_credssp *cs,
	uint32_t cli_flags,
	struct rdp_buf *out_ntlm)
{
	struct rdp_buf tname, tinfo;
	uint32_t flags;
	uint32_t tname_off, tinfo_off;
	uint64_t win_time;
	int error;

	rdp_buf_init(&tname);
	rdp_buf_init(&tinfo);
	random_bytes(cs->server_challenge, 8U);

	flags = NTLM_FLAG_UNICODE | NTLM_FLAG_REQUEST_TARGET |
		NTLM_FLAG_NTLM | NTLM_FLAG_EXT_SESSION_SEC |
		NTLM_FLAG_ALWAYS_SIGN | NTLM_FLAG_SEAL | NTLM_FLAG_SIGN |
		NTLM_FLAG_128 | NTLM_FLAG_TARGET_TYPE_SERVER |
		NTLM_FLAG_TARGET_INFO | NTLM_FLAG_VERSION;
	if ((cli_flags & NTLM_FLAG_KEY_EXCH) != 0U)
		flags |= NTLM_FLAG_KEY_EXCH;
	if ((cli_flags & NTLM_FLAG_56) != 0U)
		flags |= NTLM_FLAG_56;
	cs->ntlm_flags = flags;

	/* TargetName: "ZEDBSD". */
	error = append_utf8_as_utf16le(&tname, "ZEDBSD");
	if (error != RDP_OK)
		goto fail;

	/* TargetInfo AV_PAIRs. */
	error = rdp_buf_append_u16_le(&tinfo, NTLM_AV_NB_DOMAIN);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(&tinfo, (uint16_t)tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append(&tinfo, tname.data, tname.len);
	if (error != RDP_OK) goto fail;

	error = rdp_buf_append_u16_le(&tinfo, NTLM_AV_NB_COMPUTER);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(&tinfo, (uint16_t)tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append(&tinfo, tname.data, tname.len);
	if (error != RDP_OK) goto fail;

	error = rdp_buf_append_u16_le(&tinfo, NTLM_AV_DNS_COMPUTER);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(&tinfo, (uint16_t)tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append(&tinfo, tname.data, tname.len);
	if (error != RDP_OK) goto fail;

	win_time = ((uint64_t)time(NULL) + 11644473600ULL) * 10000000ULL;
	error = rdp_buf_append_u16_le(&tinfo, NTLM_AV_TIMESTAMP);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(&tinfo, 8U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u32_le(&tinfo, (uint32_t)(win_time & 0xFFFFFFFFULL));
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u32_le(&tinfo, (uint32_t)(win_time >> 32));
	if (error != RDP_OK) goto fail;

	error = rdp_buf_append_u16_le(&tinfo, NTLM_AV_EOL);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(&tinfo, 0U);
	if (error != RDP_OK) goto fail;

	tname_off = 56U;
	tinfo_off = 56U + (uint32_t)tname.len;

	error = rdp_buf_append(out_ntlm, "NTLMSSP\0", 8U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u32_le(out_ntlm, NTLM_MSG_CHALLENGE);
	if (error != RDP_OK) goto fail;
	/* TargetNameFields. */
	error = rdp_buf_append_u16_le(out_ntlm, (uint16_t)tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(out_ntlm, (uint16_t)tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u32_le(out_ntlm, tname_off);
	if (error != RDP_OK) goto fail;
	/* NegotiateFlags. */
	error = rdp_buf_append_u32_le(out_ntlm, flags);
	if (error != RDP_OK) goto fail;
	/* ServerChallenge. */
	error = rdp_buf_append(out_ntlm, cs->server_challenge, 8U);
	if (error != RDP_OK) goto fail;
	/* Reserved (8 bytes). */
	error = rdp_buf_append_zeros(out_ntlm, 8U);
	if (error != RDP_OK) goto fail;
	/* TargetInfoFields. */
	error = rdp_buf_append_u16_le(out_ntlm, (uint16_t)tinfo.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(out_ntlm, (uint16_t)tinfo.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u32_le(out_ntlm, tinfo_off);
	if (error != RDP_OK) goto fail;
	/* Version (8 bytes: Windows 10.0 build 19041, NTLMSSP rev 15). */
	error = rdp_buf_append_u8(out_ntlm, 10U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u8(out_ntlm, 0U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u16_le(out_ntlm, 19041U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u8(out_ntlm, 0U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u8(out_ntlm, 0U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u8(out_ntlm, 0U);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append_u8(out_ntlm, 15U);
	if (error != RDP_OK) goto fail;

	error = rdp_buf_append(out_ntlm, tname.data, tname.len);
	if (error != RDP_OK) goto fail;
	error = rdp_buf_append(out_ntlm, tinfo.data, tinfo.len);
fail:
	rdp_buf_free(&tname);
	rdp_buf_free(&tinfo);
	return error;
}

static int
ntlm_read_secbuf(
	const uint8_t *msg,
	size_t msg_len,
	size_t hdr_off,
	const uint8_t **out_ptr,
	size_t *out_len)
{
	uint16_t slen;
	uint32_t soff;

	if (hdr_off + 8U > msg_len)
		return RDP_ERR_PROTO;
	slen = (uint16_t)msg[hdr_off] | ((uint16_t)msg[hdr_off + 1U] << 8);
	soff = load32_le(msg + hdr_off + 4U);
	if ((size_t)soff + (size_t)slen > msg_len)
		return RDP_ERR_PROTO;
	*out_ptr = msg + soff;
	*out_len = slen;
	return RDP_OK;
}

static int
ntlm_verify_authenticate(
	struct rdp_credssp *cs,
	const uint8_t *msg,
	size_t len,
	int (*lookup_cb)(void *, const char *, const char *, uint8_t[16]),
	void *cb_user)
{
	const uint8_t *nt_resp, *dom_raw, *usr_raw, *sess_raw;
	size_t nt_len, dom_len, usr_len, sess_len;
	char dom[128], usr[128];
	uint8_t nt_hash[16], resp_key_nt[16], expected_proof[16], base_key[16];
	uint8_t exported_key[16];
	struct rdp_buf id_buf;
	struct md5_ctx mc;
	size_t i;
	uint8_t diff = 0U;
	int error;

	if (len < 64U || memcmp(msg, "NTLMSSP\0", 8U) != 0 ||
	    load32_le(msg + 8U) != NTLM_MSG_AUTHENTICATE)
		return RDP_ERR_PROTO;

	error = ntlm_read_secbuf(msg, len, 20U, &nt_resp, &nt_len);
	if (error != RDP_OK || nt_len < 24U)
		return RDP_ERR_PROTO;
	error = ntlm_read_secbuf(msg, len, 28U, &dom_raw, &dom_len);
	if (error != RDP_OK)
		return error;
	error = ntlm_read_secbuf(msg, len, 36U, &usr_raw, &usr_len);
	if (error != RDP_OK)
		return error;
	error = ntlm_read_secbuf(msg, len, 52U, &sess_raw, &sess_len);
	if (error != RDP_OK)
		return error;

	utf16le_to_utf8(dom_raw, dom_len, dom, sizeof(dom));
	utf16le_to_utf8(usr_raw, usr_len, usr, sizeof(usr));
	if (lookup_cb == NULL || lookup_cb(cb_user, dom, usr, nt_hash) != 0)
		return RDP_ERR_AUTH;

	/* ResponseKeyNT = HMAC-MD5(nt_hash, UTF16LE(Upper(user) + domain)). */
	rdp_buf_init(&id_buf);
	for (i = 0U; i + 1U < usr_len; i += 2U) {
		uint16_t ch = (uint16_t)usr_raw[i] | ((uint16_t)usr_raw[i + 1U] << 8);
		if (ch >= 'a' && ch <= 'z')
			ch = (uint16_t)(ch - 'a' + 'A');
		error = rdp_buf_append_u16_le(&id_buf, ch);
		if (error != RDP_OK) {
			rdp_buf_free(&id_buf);
			return error;
		}
	}
	error = rdp_buf_append(&id_buf, dom_raw, dom_len);
	if (error != RDP_OK) {
		rdp_buf_free(&id_buf);
		return error;
	}
	hmac_md5_2(nt_hash, 16U, id_buf.data, id_buf.len, NULL, 0U, resp_key_nt);
	rdp_buf_free(&id_buf);

	/* NTProofStr = HMAC-MD5(ResponseKeyNT, ServerChallenge || blob). */
	hmac_md5_2(resp_key_nt, 16U, cs->server_challenge, 8U,
		   nt_resp + 16U, nt_len - 16U, expected_proof);
	for (i = 0U; i < 16U; i++)
		diff |= (uint8_t)(expected_proof[i] ^ nt_resp[i]);
	if (diff != 0U)
		return RDP_ERR_AUTH;

	/* SessionBaseKey = HMAC-MD5(ResponseKeyNT, NTProofStr). */
	hmac_md5_2(resp_key_nt, 16U, nt_resp, 16U, NULL, 0U, base_key);

	if ((cs->ntlm_flags & NTLM_FLAG_KEY_EXCH) != 0U && sess_len == 16U) {
		struct rdp_rc4 rc4;
		rc4_init(&rc4, base_key, 16U);
		rc4_crypt(&rc4, sess_raw, exported_key, 16U);
	} else {
		memcpy(exported_key, base_key, 16U);
	}

	/* Derives NTLM2 signing and sealing keys (MS-NLMP 3.4.5.2 / 3.4.5.3). */
	{
		static const char c_sign[] = "session key to client-to-server signing key magic constant";
		static const char s_sign[] = "session key to server-to-client signing key magic constant";
		static const char c_seal[] = "session key to client-to-server sealing key magic constant";
		static const char s_seal[] = "session key to server-to-client sealing key magic constant";
		uint8_t c_seal_key[16], s_seal_key[16];

		md5_init(&mc);
		md5_update(&mc, exported_key, 16U);
		md5_update(&mc, (const uint8_t *)c_sign, sizeof(c_sign));
		md5_final(&mc, cs->cli_sign_key);

		md5_init(&mc);
		md5_update(&mc, exported_key, 16U);
		md5_update(&mc, (const uint8_t *)s_sign, sizeof(s_sign));
		md5_final(&mc, cs->srv_sign_key);

		md5_init(&mc);
		md5_update(&mc, exported_key, 16U);
		md5_update(&mc, (const uint8_t *)c_seal, sizeof(c_seal));
		md5_final(&mc, c_seal_key);

		md5_init(&mc);
		md5_update(&mc, exported_key, 16U);
		md5_update(&mc, (const uint8_t *)s_seal, sizeof(s_seal));
		md5_final(&mc, s_seal_key);

		rc4_init(&cs->seal_rx, c_seal_key, 16U);
		rc4_init(&cs->seal_tx, s_seal_key, 16U);
	}
	cs->rx_seq = 0U;
	cs->tx_seq = 0U;
	cs->keys_ready = 1;
	return RDP_OK;
}

static int
ntlm_unseal(
	struct rdp_credssp *cs,
	const uint8_t *sealed,
	size_t sealed_len,
	struct rdp_buf *plain)
{
	uint8_t seq_le[4], mac[16], enc_mac[8];
	const uint8_t *sig;
	const uint8_t *ct;
	size_t ct_len;
	size_t i;
	uint8_t diff = 0U;
	int error;

	if (!cs->keys_ready || sealed_len < 16U)
		return RDP_ERR_PROTO;
	sig = sealed;
	ct = sealed + 16U;
	ct_len = sealed_len - 16U;
	if (load32_le(sig) != 1U || load32_le(sig + 12U) != cs->rx_seq)
		return RDP_ERR_PROTO;

	rdp_buf_clear(plain);
	if (ct_len > 0U) {
		error = rdp_buf_append_zeros(plain, ct_len);
		if (error != RDP_OK)
			return error;
		rc4_crypt(&cs->seal_rx, ct, plain->data, ct_len);
	}
	store32_le(seq_le, cs->rx_seq);
	hmac_md5_2(cs->cli_sign_key, 16U, seq_le, 4U,
		   plain->data, plain->len, mac);
	if ((cs->ntlm_flags & NTLM_FLAG_KEY_EXCH) != 0U) {
		rc4_crypt(&cs->seal_rx, mac, enc_mac, 8U);
	} else {
		memcpy(enc_mac, mac, 8U);
	}
	for (i = 0U; i < 8U; i++)
		diff |= (uint8_t)(enc_mac[i] ^ sig[4U + i]);
	if (diff != 0U)
		return RDP_ERR_AUTH;
	cs->rx_seq++;
	return RDP_OK;
}

static int
ntlm_seal(
	struct rdp_credssp *cs,
	const uint8_t *plain,
	size_t plain_len,
	struct rdp_buf *out)
{
	uint8_t seq_le[4], mac[16], enc_mac[8], sig[16];
	size_t ct_off;
	int error;

	if (!cs->keys_ready)
		return RDP_ERR_PROTO;
	rdp_buf_clear(out);
	error = rdp_buf_append_zeros(out, 16U + plain_len);
	if (error != RDP_OK)
		return error;
	ct_off = 16U;
	if (plain_len > 0U)
		rc4_crypt(&cs->seal_tx, plain, out->data + ct_off, plain_len);
	store32_le(seq_le, cs->tx_seq);
	hmac_md5_2(cs->srv_sign_key, 16U, seq_le, 4U, plain, plain_len, mac);
	if ((cs->ntlm_flags & NTLM_FLAG_KEY_EXCH) != 0U) {
		rc4_crypt(&cs->seal_tx, mac, enc_mac, 8U);
	} else {
		memcpy(enc_mac, mac, 8U);
	}
	store32_le(sig, 1U);
	memcpy(sig + 4U, enc_mac, 8U);
	memcpy(sig + 12U, seq_le, 4U);
	memcpy(out->data, sig, 16U);
	cs->tx_seq++;
	return RDP_OK;
}

/* ---------- TSRequest encode/decode & CredSSP state machine ---------- */

struct ts_request {
	uint32_t version;
	const uint8_t *nego_token;
	size_t nego_len;
	const uint8_t *auth_info;
	size_t auth_len;
	const uint8_t *pub_key_auth;
	size_t pub_key_auth_len;
	const uint8_t *client_nonce;
	size_t client_nonce_len;
};

static int
decode_ts_request(const uint8_t *data, size_t len, struct ts_request *ts)
{
	struct rdp_reader r, sr;
	uint8_t tag;
	const uint8_t *seq, *val;
	size_t slen, vlen;
	int error;

	memset(ts, 0, sizeof(*ts));
	rdp_reader_init(&r, data, len);
	error = der_read_any(&r, &tag, &seq, &slen);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&sr, seq, slen);
	while (rdp_reader_rem(&sr) > 0U) {
		error = der_read_any(&sr, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
		if (tag == 0xA0U) {
			struct rdp_reader ir;
			const uint8_t *iv;
			size_t ilen, i;
			rdp_reader_init(&ir, val, vlen);
			error = der_read_tlv(&ir, 0x02U, &iv, &ilen);
			if (error != RDP_OK)
				return error;
			ts->version = 0U;
			for (i = 0U; i < ilen; i++)
				ts->version = (ts->version << 8) | iv[i];
		} else if (tag == 0xA1U) {
			/* NegoData ::= SEQUENCE OF SEQUENCE { [0] negoToken OCTET STRING }. */
			struct rdp_reader nr, n1, n2;
			const uint8_t *s1, *s2;
			size_t l1, l2;
			rdp_reader_init(&nr, val, vlen);
			error = der_read_tlv(&nr, 0x30U, &s1, &l1);
			if (error != RDP_OK) return error;
			rdp_reader_init(&n1, s1, l1);
			error = der_read_tlv(&n1, 0x30U, &s2, &l2);
			if (error != RDP_OK) return error;
			rdp_reader_init(&n2, s2, l2);
			error = der_read_tlv(&n2, 0xA0U, &s1, &l1);
			if (error != RDP_OK) return error;
			rdp_reader_init(&n1, s1, l1);
			error = der_read_tlv(&n1, 0x04U, &ts->nego_token, &ts->nego_len);
			if (error != RDP_OK) return error;
		} else if (tag == 0xA2U) {
			struct rdp_reader ar;
			rdp_reader_init(&ar, val, vlen);
			error = der_read_tlv(&ar, 0x04U, &ts->auth_info, &ts->auth_len);
			if (error != RDP_OK) return error;
		} else if (tag == 0xA3U) {
			struct rdp_reader pr;
			rdp_reader_init(&pr, val, vlen);
			error = der_read_tlv(&pr, 0x04U, &ts->pub_key_auth, &ts->pub_key_auth_len);
			if (error != RDP_OK) return error;
		} else if (tag == 0xA5U) {
			struct rdp_reader cr;
			rdp_reader_init(&cr, val, vlen);
			error = der_read_tlv(&cr, 0x04U, &ts->client_nonce, &ts->client_nonce_len);
			if (error != RDP_OK) return error;
		}
	}
	return RDP_OK;
}

static int
encode_ts_request(
	struct rdp_buf *out,
	uint32_t version,
	const uint8_t *nego,
	size_t nego_len,
	const uint8_t *pub_key_auth,
	size_t pub_key_auth_len,
	uint32_t error_code,
	int has_error_code)
{
	struct rdp_buf body, tmp, n1, n2;
	int error;

	rdp_buf_init(&body);
	rdp_buf_init(&tmp);
	rdp_buf_init(&n1);
	rdp_buf_init(&n2);

	/* [0] version. */
	error = der_append_int_u32(&tmp, version);
	if (error != RDP_OK) goto fail;
	error = der_append_tlv(&body, 0xA0U, tmp.data, tmp.len);
	if (error != RDP_OK) goto fail;

	/* [1] negoTokens. */
	if (nego != NULL && nego_len > 0U) {
		rdp_buf_clear(&tmp);
		error = der_append_tlv(&tmp, 0x04U, nego, nego_len);
		if (error != RDP_OK) goto fail;
		error = der_append_tlv(&n1, 0xA0U, tmp.data, tmp.len);
		if (error != RDP_OK) goto fail;
		error = der_append_tlv(&n2, 0x30U, n1.data, n1.len);
		if (error != RDP_OK) goto fail;
		rdp_buf_clear(&n1);
		error = der_append_tlv(&n1, 0x30U, n2.data, n2.len);
		if (error != RDP_OK) goto fail;
		error = der_append_tlv(&body, 0xA1U, n1.data, n1.len);
		if (error != RDP_OK) goto fail;
	}

	/* [3] pubKeyAuth. */
	if (pub_key_auth != NULL && pub_key_auth_len > 0U) {
		rdp_buf_clear(&tmp);
		error = der_append_tlv(&tmp, 0x04U, pub_key_auth, pub_key_auth_len);
		if (error != RDP_OK) goto fail;
		error = der_append_tlv(&body, 0xA3U, tmp.data, tmp.len);
		if (error != RDP_OK) goto fail;
	}

	/* [4] errorCode. */
	if (has_error_code) {
		rdp_buf_clear(&tmp);
		error = der_append_int_u32(&tmp, error_code);
		if (error != RDP_OK) goto fail;
		error = der_append_tlv(&body, 0xA4U, tmp.data, tmp.len);
		if (error != RDP_OK) goto fail;
	}

	error = der_append_tlv(out, 0x30U, body.data, body.len);
fail:
	rdp_buf_free(&body);
	rdp_buf_free(&tmp);
	rdp_buf_free(&n1);
	rdp_buf_free(&n2);
	return error;
}

static int
decode_ts_credentials(
	const uint8_t *data,
	size_t len,
	char *domain,
	size_t domain_cap,
	char *user,
	size_t user_cap,
	char *password,
	size_t pass_cap)
{
	struct rdp_reader r, cr, pr;
	uint8_t tag;
	const uint8_t *seq, *val, *creds = NULL;
	size_t slen, vlen, creds_len = 0U;
	int error;

	domain[0] = '\0';
	user[0] = '\0';
	password[0] = '\0';

	rdp_reader_init(&r, data, len);
	error = der_read_any(&r, &tag, &seq, &slen);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&cr, seq, slen);
	while (rdp_reader_rem(&cr) > 0U) {
		error = der_read_any(&cr, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
		if (tag == 0xA1U) {
			struct rdp_reader or;
			rdp_reader_init(&or, val, vlen);
			error = der_read_tlv(&or, 0x04U, &creds, &creds_len);
			if (error != RDP_OK)
				return error;
		}
	}
	if (creds == NULL)
		return RDP_ERR_PROTO;
	rdp_reader_init(&r, creds, creds_len);
	error = der_read_any(&r, &tag, &seq, &slen);
	if (error != RDP_OK || tag != 0x30U)
		return RDP_ERR_PROTO;
	rdp_reader_init(&pr, seq, slen);
	while (rdp_reader_rem(&pr) > 0U) {
		struct rdp_reader or;
		const uint8_t *ov;
		size_t olen;
		error = der_read_any(&pr, &tag, &val, &vlen);
		if (error != RDP_OK)
			return error;
		rdp_reader_init(&or, val, vlen);
		error = der_read_tlv(&or, 0x04U, &ov, &olen);
		if (error != RDP_OK)
			return error;
		if (tag == 0xA0U)
			utf16le_to_utf8(ov, olen, domain, domain_cap);
		else if (tag == 0xA1U)
			utf16le_to_utf8(ov, olen, user, user_cap);
		else if (tag == 0xA2U)
			utf16le_to_utf8(ov, olen, password, pass_cap);
	}
	return RDP_OK;
}

int
rdp_credssp_handle_pdu(
	struct rdp_credssp *cs,
	enum rdp_server_state *state,
	const uint8_t *pdu,
	size_t pdu_len,
	const uint8_t *pubkey,
	size_t pubkey_len,
	int (*lookup_cb)(void *, const char *, const char *, uint8_t[16]),
	void (*auth_cb)(void *, const char *, const char *, const char *),
	void *app_user,
	int (*send_raw_cb)(void *, const struct rdp_buf *),
	void *send_ctx)
{
	struct ts_request ts;
	const uint8_t *ntlm;
	size_t ntlm_len;
	struct rdp_buf chal, spnego, out, plain, sealed;
	int error;

	error = decode_ts_request(pdu, pdu_len, &ts);
	if (error != RDP_OK)
		return error;
	if (ts.version > 0U && cs->version == 0U)
		cs->version = ts.version;
	if (ts.client_nonce != NULL && ts.client_nonce_len == 32U) {
		memcpy(cs->client_nonce, ts.client_nonce, 32U);
		cs->has_client_nonce = 1;
	}

	if (*state == RDP_SRV_WAIT_CREDSSP_NEG) {
		uint32_t cli_flags;
		if (ts.nego_token == NULL)
			return RDP_ERR_PROTO;
		cs->use_spnego = !(ts.nego_len >= 8U &&
				   memcmp(ts.nego_token, "NTLMSSP\0", 8U) == 0);
		error = spnego_unwrap_ntlm(ts.nego_token, ts.nego_len,
					   &ntlm, &ntlm_len);
		if (error != RDP_OK || ntlm_len < 16U ||
		    memcmp(ntlm, "NTLMSSP\0", 8U) != 0 ||
		    load32_le(ntlm + 8U) != NTLM_MSG_NEGOTIATE)
			return RDP_ERR_PROTO;
		cli_flags = load32_le(ntlm + 12U);

		rdp_buf_init(&chal);
		rdp_buf_init(&spnego);
		rdp_buf_init(&out);
		error = ntlm_build_challenge(cs, cli_flags, &chal);
		if (error == RDP_OK) {
			if (cs->use_spnego) {
				error = spnego_wrap_challenge(&spnego,
					chal.data, chal.len);
			} else {
				error = rdp_buf_append(&spnego,
					chal.data, chal.len);
			}
		}
		if (error == RDP_OK) {
			error = encode_ts_request(&out,
			    cs->version >= 2U ? cs->version : 2U,
			    spnego.data, spnego.len, NULL, 0U, 0U, 0);
		}
		if (error == RDP_OK) {
			*state = RDP_SRV_WAIT_CREDSSP_AUTH;
			error = send_raw_cb(send_ctx, &out);
		}
		rdp_buf_free(&chal);
		rdp_buf_free(&spnego);
		rdp_buf_free(&out);
		return error;
	}

	if (*state == RDP_SRV_WAIT_CREDSSP_AUTH) {
		uint8_t exp_hash[32], rsp_hash[32];
		const uint8_t *exp_ptr;
		size_t exp_len;
		uint8_t *pk_copy = NULL;
		size_t i;
		uint8_t diff = 0U;

		if (ts.nego_token == NULL || ts.pub_key_auth == NULL ||
		    pubkey == NULL || pubkey_len == 0U)
			return RDP_ERR_PROTO;
		error = spnego_unwrap_ntlm(ts.nego_token, ts.nego_len,
					   &ntlm, &ntlm_len);
		if (error != RDP_OK)
			return error;
		error = ntlm_verify_authenticate(cs, ntlm, ntlm_len,
						 lookup_cb, app_user);
		if (error != RDP_OK)
			return error;

		rdp_buf_init(&plain);
		error = ntlm_unseal(cs, ts.pub_key_auth, ts.pub_key_auth_len, &plain);
		if (error != RDP_OK) {
			rdp_buf_free(&plain);
			return error;
		}
		if (cs->version >= 5U && cs->has_client_nonce) {
			sha256_hash3((const uint8_t *)CSSP_CS_MAGIC,
				     sizeof(CSSP_CS_MAGIC),
				     cs->client_nonce, 32U,
				     pubkey, pubkey_len, exp_hash);
			exp_ptr = exp_hash;
			exp_len = 32U;
		} else {
			exp_ptr = pubkey;
			exp_len = pubkey_len;
		}
		if (plain.len != exp_len) {
			rdp_buf_free(&plain);
			return RDP_ERR_AUTH;
		}
		for (i = 0U; i < exp_len; i++)
			diff |= (uint8_t)(plain.data[i] ^ exp_ptr[i]);
		rdp_buf_free(&plain);
		if (diff != 0U)
			return RDP_ERR_AUTH;

		rdp_buf_init(&sealed);
		rdp_buf_init(&out);
		if (cs->version >= 5U && cs->has_client_nonce) {
			sha256_hash3((const uint8_t *)CSSP_SC_MAGIC,
				     sizeof(CSSP_SC_MAGIC),
				     cs->client_nonce, 32U,
				     pubkey, pubkey_len, rsp_hash);
			error = ntlm_seal(cs, rsp_hash, 32U, &sealed);
		} else {
			pk_copy = (uint8_t *)malloc(pubkey_len);
			if (pk_copy == NULL) {
				error = RDP_ERR_NOMEM;
			} else {
				memcpy(pk_copy, pubkey, pubkey_len);
				pk_copy[0] = (uint8_t)(pk_copy[0] + 1U);
				error = ntlm_seal(cs, pk_copy, pubkey_len, &sealed);
				free(pk_copy);
			}
		}
		if (error == RDP_OK) {
			error = encode_ts_request(&out, cs->version,
			    NULL, 0U, sealed.data, sealed.len, 0U, 0);
		}
		if (error == RDP_OK) {
			*state = RDP_SRV_WAIT_CREDSSP_CREDS;
			error = send_raw_cb(send_ctx, &out);
		}
		rdp_buf_free(&sealed);
		rdp_buf_free(&out);
		return error;
	}

	if (*state == RDP_SRV_WAIT_CREDSSP_CREDS) {
		char dom[128], usr[128], pwd[256];
		if (ts.auth_info == NULL)
			return RDP_ERR_PROTO;
		rdp_buf_init(&plain);
		error = ntlm_unseal(cs, ts.auth_info, ts.auth_len, &plain);
		if (error != RDP_OK) {
			rdp_buf_free(&plain);
			return error;
		}
		error = decode_ts_credentials(plain.data, plain.len,
					      dom, sizeof(dom),
					      usr, sizeof(usr),
					      pwd, sizeof(pwd));
		rdp_buf_free(&plain);
		if (error != RDP_OK)
			return error;
		*state = RDP_SRV_WAIT_CREDSSP_DECISION;
		if (auth_cb != NULL) {
			auth_cb(app_user, dom, usr, pwd);
		} else {
			*state = RDP_SRV_WAIT_MCS_CONNECT_INIT;
		}
		memset(pwd, 0, sizeof(pwd));
		return RDP_OK;
	}

	return RDP_ERR_PROTO;
}

int
rdp_credssp_send_error(
	struct rdp_credssp *cs,
	uint32_t ntstatus,
	int (*send_raw_cb)(void *, const struct rdp_buf *),
	void *cb_user)
{
	struct rdp_buf out;
	int error;

	rdp_buf_init(&out);
	error = encode_ts_request(&out, cs->version >= 2U ? cs->version : 2U,
				  NULL, 0U, NULL, 0U, ntstatus, 1);
	if (error == RDP_OK)
		error = send_raw_cb(cb_user, &out);
	rdp_buf_free(&out);
	return error;
}
