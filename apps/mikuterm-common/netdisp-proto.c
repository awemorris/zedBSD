/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display UDP discovery and encrypted offer wire protocol.
 */

#include "netdisp-proto.h"

#include <compat/crypto/core_names.h>
#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/hmac.h>
#include <compat/crypto/kdf.h>
#include <compat/crypto/params.h>
#include <compat/crypto/rand.h>

#include <stdio.h>
#include <string.h>

static const uint8_t netdisp_magic[4] = { 0x5AU, 0x4BU, 0x4EU, 0x44U };
static const char netdisp_hkdf_info_prefix[] = "zedbsd-netdisp-v1";

static void
put_be16(
	uint8_t *p,
	uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFFU);
}

static uint16_t
get_be16(
	const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int
derive_keys(
	const uint8_t priv_key[NETDISP_KEY_LEN],
	const uint8_t peer_pub[NETDISP_KEY_LEN],
	const uint8_t eph_pk[NETDISP_KEY_LEN],
	const uint8_t target_pk[NETDISP_KEY_LEN],
	const uint8_t nonce[NETDISP_NONCE_LEN],
	uint8_t out_enc_key[NETDISP_KEY_LEN],
	uint8_t out_mac_key[NETDISP_KEY_LEN])
{
	EVP_PKEY *priv;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *pctx;
	EVP_KDF *kdf;
	EVP_KDF_CTX *kctx;
	uint8_t shared[NETDISP_KEY_LEN];
	size_t shared_len;
	uint8_t okm[NETDISP_KEY_LEN * 2U];
	OSSL_PARAM params[7];
	int ok;

	priv = NULL;
	peer = NULL;
	pctx = NULL;
	kdf = NULL;
	kctx = NULL;
	ok = 0;

	priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
					    priv_key, NETDISP_KEY_LEN);
	if (priv == NULL)
		goto out;
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL,
					   peer_pub, NETDISP_KEY_LEN);
	if (peer == NULL)
		goto out;
	pctx = EVP_PKEY_CTX_new(priv, NULL);
	if (pctx == NULL)
		goto out;
	if (EVP_PKEY_derive_init(pctx) != 1 ||
	    EVP_PKEY_derive_set_peer(pctx, peer) != 1)
		goto out;
	shared_len = sizeof(shared);
	if (EVP_PKEY_derive(pctx, shared, &shared_len) != 1 ||
	    shared_len != NETDISP_KEY_LEN)
		goto out;

	kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
	if (kdf == NULL)
		goto out;
	kctx = EVP_KDF_CTX_new(kdf);
	if (kctx == NULL)
		goto out;

	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
						     "SHA256", 0);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
						      shared, sizeof(shared));
	params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
						      (void *)nonce,
						      NETDISP_NONCE_LEN);
	params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
						      (void *)netdisp_hkdf_info_prefix,
						      sizeof(netdisp_hkdf_info_prefix) - 1U);
	params[4] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
						      (void *)eph_pk,
						      NETDISP_KEY_LEN);
	params[5] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
						      (void *)target_pk,
						      NETDISP_KEY_LEN);
	params[6] = OSSL_PARAM_construct_end();

	if (EVP_KDF_derive(kctx, okm, sizeof(okm), params) != 1)
		goto out;

	memcpy(out_enc_key, okm, NETDISP_KEY_LEN);
	memcpy(out_mac_key, okm + NETDISP_KEY_LEN, NETDISP_KEY_LEN);
	ok = 1;

out:
	OPENSSL_cleanse(shared, sizeof(shared));
	OPENSSL_cleanse(okm, sizeof(okm));
	EVP_KDF_CTX_free(kctx);
	EVP_KDF_free(kdf);
	EVP_PKEY_CTX_free(pctx);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(priv);
	return ok ? 0 : -1;
}

int
netdisp_generate_keypair(
	uint8_t out_sk[NETDISP_KEY_LEN],
	uint8_t out_pk[NETDISP_KEY_LEN],
	uint8_t out_nonce[NETDISP_NONCE_LEN])
{
	EVP_PKEY *pkey;
	size_t pk_len;

	if (RAND_bytes(out_sk, NETDISP_KEY_LEN) != 1)
		return -1;
	if (RAND_bytes(out_nonce, NETDISP_NONCE_LEN) != 1) {
		OPENSSL_cleanse(out_sk, NETDISP_KEY_LEN);
		return -1;
	}
	pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL,
					    out_sk, NETDISP_KEY_LEN);
	if (pkey == NULL) {
		OPENSSL_cleanse(out_sk, NETDISP_KEY_LEN);
		return -1;
	}
	pk_len = NETDISP_KEY_LEN;
	if (EVP_PKEY_get_raw_public_key(pkey, out_pk, &pk_len) != 1 ||
	    pk_len != NETDISP_KEY_LEN) {
		EVP_PKEY_free(pkey);
		OPENSSL_cleanse(out_sk, NETDISP_KEY_LEN);
		return -1;
	}
	EVP_PKEY_free(pkey);
	return 0;
}

void
netdisp_fingerprint_hex(
	const uint8_t pk[NETDISP_KEY_LEN],
	const uint8_t nonce[NETDISP_NONCE_LEN],
	char out_hex[10])
{
	EVP_MD_CTX *ctx;
	uint8_t digest[32];
	unsigned int dlen;

	memset(digest, 0, sizeof(digest));
	ctx = EVP_MD_CTX_new();
	if (ctx != NULL) {
		dlen = 0U;
		if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
		    EVP_DigestUpdate(ctx, pk, NETDISP_KEY_LEN) == 1 &&
		    EVP_DigestUpdate(ctx, nonce, NETDISP_NONCE_LEN) == 1)
			(void)EVP_DigestFinal_ex(ctx, digest, &dlen);
		EVP_MD_CTX_free(ctx);
	}
	snprintf(out_hex, 10U, "%02X%02X-%02X%02X",
		 digest[0], digest[1], digest[2], digest[3]);
}

void
netdisp_device_id_hex(
	const uint8_t device_id[NETDISP_DEVICE_ID_LEN],
	char out_hex[33])
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0U; i < NETDISP_DEVICE_ID_LEN; i++) {
		out_hex[i * 2U] = hex[(device_id[i] >> 4) & 0x0FU];
		out_hex[i * 2U + 1U] = hex[device_id[i] & 0x0FU];
	}
	out_hex[32] = '\0';
}

static int
hex_nibble(
	char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

int
netdisp_device_id_from_hex(
	const char *hex,
	uint8_t out_id[NETDISP_DEVICE_ID_LEN])
{
	size_t i;

	if (hex == NULL || strlen(hex) != 32U)
		return -1;
	for (i = 0U; i < NETDISP_DEVICE_ID_LEN; i++) {
		int hi = hex_nibble(hex[i * 2U]);
		int lo = hex_nibble(hex[i * 2U + 1U]);
		if (hi < 0 || lo < 0)
			return -1;
		out_id[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

void
netdisp_sanitize_name(
	const char *in,
	char out[NETDISP_MAX_NAME_LEN + 1U],
	uint8_t *out_len)
{
	size_t i;
	size_t j;

	j = 0U;
	if (in != NULL) {
		for (i = 0U; in[i] != '\0' && j < NETDISP_MAX_NAME_LEN; i++) {
			uint8_t c = (uint8_t)in[i];
			if (c < 0x20U || c == 0x7FU)
				continue;
			out[j++] = (char)c;
		}
	}
	while (j > 0U && (((uint8_t)out[j - 1U] & 0xC0U) == 0x80U))
		j--;
	if (j > 0U && ((uint8_t)out[j - 1U] & 0x80U) != 0U) {
		uint8_t lead = (uint8_t)out[j - 1U];
		if ((lead & 0xE0U) == 0xC0U ||
		    (lead & 0xF0U) == 0xE0U ||
		    (lead & 0xF8U) == 0xF0U)
			j--;
	}
	if (j == 0U) {
		static const char fallback[] = "Network Display";
		j = sizeof(fallback) - 1U;
		memcpy(out, fallback, j);
	}
	out[j] = '\0';
	if (out_len != NULL)
		*out_len = (uint8_t)j;
}

int
netdisp_adv_encode(
	const struct netdisp_adv *adv,
	uint8_t *out,
	size_t out_cap,
	size_t *out_len)
{
	char clean[NETDISP_MAX_NAME_LEN + 1U];
	uint8_t name_len;
	size_t total;

	if (adv == NULL || out == NULL || out_len == NULL)
		return -1;
	netdisp_sanitize_name(adv->device_name, clean, &name_len);
	total = NETDISP_ADV_HDR_LEN + (size_t)name_len;
	if (out_cap < total)
		return -1;

	memcpy(out, netdisp_magic, 4U);
	out[4] = NETDISP_VERSION;
	out[5] = NETDISP_MSG_ADV;
	put_be16(out + 6U, adv->flags);
	memcpy(out + 8U, adv->device_id, NETDISP_DEVICE_ID_LEN);
	memcpy(out + 24U, adv->nonce, NETDISP_NONCE_LEN);
	memcpy(out + 40U, adv->pk, NETDISP_KEY_LEN);
	put_be16(out + 72U, adv->reply_port);
	put_be16(out + 74U, adv->pref_width);
	put_be16(out + 76U, adv->pref_height);
	put_be16(out + 78U, adv->pref_scale120);
	out[80] = name_len;
	memcpy(out + 81U, clean, name_len);
	*out_len = total;
	return 0;
}

int
netdisp_adv_decode(
	const uint8_t *in,
	size_t in_len,
	struct netdisp_adv *out)
{
	uint8_t name_len;
	char raw_name[NETDISP_MAX_NAME_LEN + 1U];

	if (in == NULL || out == NULL || in_len < NETDISP_ADV_HDR_LEN + 1U)
		return -1;
	if (memcmp(in, netdisp_magic, 4U) != 0)
		return -1;
	if (in[4] != NETDISP_VERSION || in[5] != NETDISP_MSG_ADV)
		return -1;
	name_len = in[80];
	if (name_len == 0U || name_len > NETDISP_MAX_NAME_LEN)
		return -1;
	if (in_len != NETDISP_ADV_HDR_LEN + (size_t)name_len)
		return -1;

	memset(out, 0, sizeof(*out));
	out->flags = get_be16(in + 6U);
	memcpy(out->device_id, in + 8U, NETDISP_DEVICE_ID_LEN);
	memcpy(out->nonce, in + 24U, NETDISP_NONCE_LEN);
	memcpy(out->pk, in + 40U, NETDISP_KEY_LEN);
	out->reply_port = get_be16(in + 72U);
	out->pref_width = get_be16(in + 74U);
	out->pref_height = get_be16(in + 76U);
	out->pref_scale120 = get_be16(in + 78U);
	if (out->reply_port == 0U)
		return -1;
	memcpy(raw_name, in + 81U, name_len);
	raw_name[name_len] = '\0';
	netdisp_sanitize_name(raw_name, out->device_name, &out->name_len);
	return 0;
}

int
netdisp_offer_encrypt(
	const uint8_t device_id[NETDISP_DEVICE_ID_LEN],
	const uint8_t peer_pk[NETDISP_KEY_LEN],
	const struct netdisp_offer_plain *plain,
	uint8_t *out,
	size_t out_cap,
	size_t *out_len)
{
	uint8_t eph_sk[NETDISP_KEY_LEN];
	uint8_t eph_pk[NETDISP_KEY_LEN];
	uint8_t dummy_nonce[NETDISP_NONCE_LEN];
	uint8_t enc_key[NETDISP_KEY_LEN];
	uint8_t mac_key[NETDISP_KEY_LEN];
	uint8_t packet_iv[NETDISP_IV_LEN];
	uint8_t chacha_iv[16];
	uint8_t plain_buf[NETDISP_PLAIN_HDR_LEN + NETDISP_MAX_USER_LEN + NETDISP_MAX_PASS_LEN];
	EVP_CIPHER_CTX *cctx;
	size_t user_len;
	size_t pass_len;
	size_t ct_len;
	size_t total;
	int outl;
	unsigned int tag_len;
	int rc;

	if (device_id == NULL || peer_pk == NULL || plain == NULL ||
	    out == NULL || out_len == NULL)
		return -1;

	user_len = strlen(plain->username);
	pass_len = strlen(plain->password);
	if (user_len == 0U || user_len > NETDISP_MAX_USER_LEN ||
	    pass_len == 0U || pass_len > NETDISP_MAX_PASS_LEN ||
	    plain->rdp_port == 0U)
		return -1;

	ct_len = NETDISP_PLAIN_HDR_LEN + user_len + pass_len;
	total = NETDISP_OFFER_HDR_LEN + ct_len + NETDISP_TAG_LEN;
	if (out_cap < total)
		return -1;

	cctx = NULL;
	rc = -1;
	memset(eph_sk, 0, sizeof(eph_sk));
	memset(enc_key, 0, sizeof(enc_key));
	memset(mac_key, 0, sizeof(mac_key));
	memset(plain_buf, 0, sizeof(plain_buf));

	if (netdisp_generate_keypair(eph_sk, eph_pk, dummy_nonce) != 0)
		goto out;
	if (derive_keys(eph_sk, peer_pk, eph_pk, peer_pk, plain->nonce,
			enc_key, mac_key) != 0)
		goto out;
	if (RAND_bytes(packet_iv, NETDISP_IV_LEN) != 1)
		goto out;

	memcpy(plain_buf, plain->nonce, NETDISP_NONCE_LEN);
	put_be16(plain_buf + 16U, plain->rdp_port);
	plain_buf[18] = (uint8_t)user_len;
	plain_buf[19] = (uint8_t)pass_len;
	memcpy(plain_buf + 20U, plain->username, user_len);
	memcpy(plain_buf + 20U + user_len, plain->password, pass_len);

	memcpy(out, netdisp_magic, 4U);
	out[4] = NETDISP_VERSION;
	out[5] = NETDISP_MSG_OFFER;
	put_be16(out + 6U, 0U);
	memcpy(out + 8U, device_id, NETDISP_DEVICE_ID_LEN);
	memcpy(out + 24U, eph_pk, NETDISP_KEY_LEN);
	memcpy(out + 56U, packet_iv, NETDISP_IV_LEN);
	put_be16(out + 68U, (uint16_t)ct_len);

	memset(chacha_iv, 0, 4U);
	memcpy(chacha_iv + 4U, packet_iv, NETDISP_IV_LEN);

	cctx = EVP_CIPHER_CTX_new();
	if (cctx == NULL)
		goto out;
	if (EVP_CipherInit_ex(cctx, EVP_chacha20(), NULL, enc_key, chacha_iv, 1) != 1)
		goto out;
	outl = 0;
	if (EVP_CipherUpdate(cctx, out + NETDISP_OFFER_HDR_LEN, &outl,
			     plain_buf, (int)ct_len) != 1 ||
	    (size_t)outl != ct_len)
		goto out;

	tag_len = 0U;
	if (HMAC(EVP_sha256(), mac_key, NETDISP_KEY_LEN,
		 out, NETDISP_OFFER_HDR_LEN + ct_len,
		 out + NETDISP_OFFER_HDR_LEN + ct_len, &tag_len) == NULL ||
	    tag_len != NETDISP_TAG_LEN)
		goto out;

	*out_len = total;
	rc = 0;

out:
	EVP_CIPHER_CTX_free(cctx);
	OPENSSL_cleanse(eph_sk, sizeof(eph_sk));
	OPENSSL_cleanse(enc_key, sizeof(enc_key));
	OPENSSL_cleanse(mac_key, sizeof(mac_key));
	OPENSSL_cleanse(plain_buf, sizeof(plain_buf));
	return rc;
}

int
netdisp_offer_decrypt(
	const uint8_t *in,
	size_t in_len,
	const uint8_t expected_device_id[NETDISP_DEVICE_ID_LEN],
	const uint8_t local_sk[NETDISP_KEY_LEN],
	const uint8_t local_pk[NETDISP_KEY_LEN],
	const uint8_t expected_nonce[NETDISP_NONCE_LEN],
	struct netdisp_offer_plain *out)
{
	uint8_t enc_key[NETDISP_KEY_LEN];
	uint8_t mac_key[NETDISP_KEY_LEN];
	uint8_t chacha_iv[16];
	uint8_t expected_tag[NETDISP_TAG_LEN];
	uint8_t plain_buf[NETDISP_PLAIN_HDR_LEN + NETDISP_MAX_USER_LEN + NETDISP_MAX_PASS_LEN];
	EVP_CIPHER_CTX *cctx;
	uint16_t ct_len;
	unsigned int tag_len;
	int outl;
	uint8_t user_len;
	uint8_t pass_len;
	int rc;

	if (in == NULL || expected_device_id == NULL || local_sk == NULL ||
	    local_pk == NULL || expected_nonce == NULL || out == NULL)
		return -1;
	if (in_len < NETDISP_OFFER_HDR_LEN + NETDISP_PLAIN_HDR_LEN + 2U + NETDISP_TAG_LEN ||
	    in_len > NETDISP_OFFER_MAX_LEN)
		return -1;
	if (memcmp(in, netdisp_magic, 4U) != 0)
		return -1;
	if (in[4] != NETDISP_VERSION || in[5] != NETDISP_MSG_OFFER)
		return -1;
	if (CRYPTO_memcmp(in + 8U, expected_device_id, NETDISP_DEVICE_ID_LEN) != 0)
		return -1;

	ct_len = get_be16(in + 68U);
	if ((size_t)ct_len + NETDISP_OFFER_HDR_LEN + NETDISP_TAG_LEN != in_len)
		return -1;
	if (ct_len < NETDISP_PLAIN_HDR_LEN + 2U ||
	    ct_len > sizeof(plain_buf))
		return -1;

	cctx = NULL;
	rc = -1;
	memset(enc_key, 0, sizeof(enc_key));
	memset(mac_key, 0, sizeof(mac_key));
	memset(plain_buf, 0, sizeof(plain_buf));

	if (derive_keys(local_sk, in + 24U, in + 24U, local_pk, expected_nonce,
			enc_key, mac_key) != 0)
		goto out;

	tag_len = 0U;
	if (HMAC(EVP_sha256(), mac_key, NETDISP_KEY_LEN,
		 in, NETDISP_OFFER_HDR_LEN + (size_t)ct_len,
		 expected_tag, &tag_len) == NULL ||
	    tag_len != NETDISP_TAG_LEN)
		goto out;
	if (CRYPTO_memcmp(expected_tag,
			  in + NETDISP_OFFER_HDR_LEN + (size_t)ct_len,
			  NETDISP_TAG_LEN) != 0)
		goto out;

	memset(chacha_iv, 0, 4U);
	memcpy(chacha_iv + 4U, in + 56U, NETDISP_IV_LEN);

	cctx = EVP_CIPHER_CTX_new();
	if (cctx == NULL)
		goto out;
	if (EVP_CipherInit_ex(cctx, EVP_chacha20(), NULL, enc_key, chacha_iv, 0) != 1)
		goto out;
	outl = 0;
	if (EVP_CipherUpdate(cctx, plain_buf, &outl,
			     in + NETDISP_OFFER_HDR_LEN, (int)ct_len) != 1 ||
	    (uint16_t)outl != ct_len)
		goto out;

	if (CRYPTO_memcmp(plain_buf, expected_nonce, NETDISP_NONCE_LEN) != 0)
		goto out;

	user_len = plain_buf[18];
	pass_len = plain_buf[19];
	if (user_len == 0U || user_len > NETDISP_MAX_USER_LEN ||
	    pass_len == 0U || pass_len > NETDISP_MAX_PASS_LEN)
		goto out;
	if ((size_t)NETDISP_PLAIN_HDR_LEN + (size_t)user_len + (size_t)pass_len != (size_t)ct_len)
		goto out;

	memset(out, 0, sizeof(*out));
	memcpy(out->nonce, plain_buf, NETDISP_NONCE_LEN);
	out->rdp_port = get_be16(plain_buf + 16U);
	if (out->rdp_port == 0U)
		goto out;
	memcpy(out->username, plain_buf + 20U, user_len);
	out->username[user_len] = '\0';
	memcpy(out->password, plain_buf + 20U + user_len, pass_len);
	out->password[pass_len] = '\0';
	rc = 0;

out:
	EVP_CIPHER_CTX_free(cctx);
	OPENSSL_cleanse(enc_key, sizeof(enc_key));
	OPENSSL_cleanse(mac_key, sizeof(mac_key));
	OPENSSL_cleanse(plain_buf, sizeof(plain_buf));
	return rc;
}
