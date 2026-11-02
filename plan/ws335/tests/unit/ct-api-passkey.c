/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the calls libpasskey's cryptography adds:
 * the AES inverse cipher, AES-256-CBC through EVP in both directions,
 * HMAC, HKDF through EVP_KDF, and P-256 ECDH through EVP_PKEY_derive.
 * The keys, the plaintexts, the input keying material and the private
 * scalar are marked secret; the ciphertexts given to a decryption, the
 * IVs, the salts, the infos, the peer's point and every size are public.
 * The outputs are made public only after the call.
 */

#include <string.h>

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-api.h"

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 29U + (index >> 2));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];
	return value & 0xffU;
}

/* The inverse cipher of every key size with a secret key: five blocks (a group of four and one). */
static unsigned
run_inverse(void)
{
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t cipher[80];
	uint8_t plain[80];
	size_t key_size;
	unsigned total;

	total = 0U;
	for (key_size = 16U; key_size <= 32U; key_size += 8U) {
		/* The expansion of a secret key; the ciphertext is public, the plaintext it gives is secret. */
		fill(key, key_size, (unsigned)key_size);
		fill(cipher, sizeof(cipher), (unsigned)key_size + 1U);
		CT_SECRET(key, key_size);
		CHECK(cryc_aes_init(&aes, key, key_size) == 0);
		cryc_aes_decrypt_blocks(&aes, cipher, plain, 5U);
		CT_PUBLIC(plain, sizeof(plain));
		CT_PUBLIC(key, key_size);
		total += fold(plain, sizeof(plain));

		/* A secret input too (the inverse cipher of a secret block, in place). */
		CT_SECRET(plain, sizeof(plain));
		cryc_aes_decrypt_blocks(&aes, plain, plain, 5U);
		CT_PUBLIC(plain, sizeof(plain));
		total += fold(plain, sizeof(plain));
		cryc_aes_clear(&aes);
	}
	return total;
}

/* AES-256-CBC through EVP: a secret key, a secret plaintext encrypted in uneven pieces, then decrypted. */
static unsigned
run_cbc(void)
{
	EVP_CIPHER_CTX *ctx;
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t plain[96];
	uint8_t cipher[96 + 16];
	uint8_t back[96 + 16];
	unsigned total;
	int written;
	int more;
	int last;

	total = 0U;
	fill(key, sizeof(key), 21U);
	fill(iv, sizeof(iv), 22U);
	fill(plain, sizeof(plain), 23U);
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return 0U;

	/* The encryption: the key and the plaintext are secret; the ciphertext is public once made. */
	CT_SECRET(key, sizeof(key));
	CT_SECRET(plain, sizeof(plain));
	CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, 1) == 1);
	CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
	CHECK(EVP_CipherUpdate(ctx, cipher, &written, plain, 37) == 1);
	CHECK(EVP_CipherUpdate(ctx, cipher + written, &more, plain + 37, 59) == 1);
	written += more;
	CHECK(EVP_CipherFinal_ex(ctx, cipher + written, &last) == 1);
	CT_PUBLIC(cipher, sizeof(cipher));
	total += fold(cipher, (size_t)written);

	/* The decryption with the same secret key: the plaintext it gives is secret until it is made public. */
	CHECK(EVP_CipherInit_ex(ctx, NULL, NULL, key, iv, 0) == 1);
	CHECK(EVP_CipherUpdate(ctx, back, &written, cipher, 96) == 1);
	CHECK(EVP_CipherFinal_ex(ctx, back + written, &last) == 1);
	CT_PUBLIC(back, sizeof(back));
	CT_PUBLIC(plain, sizeof(plain));
	CT_PUBLIC(key, sizeof(key));
	CHECK(memcmp(back, plain, sizeof(plain)) == 0);
	total += fold(back, sizeof(plain));
	EVP_CIPHER_CTX_free(ctx);
	return total;
}

/* HMAC-SHA-256 with a secret key and a secret message, and HKDF with secret input keying material. */
static unsigned
run_hmac_hkdf(void)
{
	OSSL_PARAM params[5];
	EVP_KDF_CTX *ctx;
	EVP_KDF *kdf;
	uint8_t key[100];
	uint8_t data[150];
	uint8_t salt[32];
	uint8_t info[14];
	uint8_t mac[32];
	uint8_t okm[80];
	unsigned total;
	unsigned char *made;

	total = 0U;

	/* HMAC: a key longer than the block (hashed first) and a message of a few blocks. */
	fill(key, sizeof(key), 31U);
	fill(data, sizeof(data), 32U);
	CT_SECRET(key, sizeof(key));
	CT_SECRET(data, sizeof(data));
	made = HMAC(EVP_sha256(), key, (int)sizeof(key), data, sizeof(data), mac, NULL);
	CHECK(made == mac);
	CT_PUBLIC(mac, sizeof(mac));
	total += fold(mac, sizeof(mac));

	/* HMAC with a short key. */
	made = HMAC(EVP_sha256(), key, 20, data, 3U, mac, NULL);
	CHECK(made == mac);
	CT_PUBLIC(mac, sizeof(mac));
	total += fold(mac, sizeof(mac));
	CT_PUBLIC(data, sizeof(data));

	/* HKDF as libpasskey asks for it: the key secret, the salt and the info public. */
	fill(salt, sizeof(salt), 33U);
	memcpy(info, "CTAP2 HMAC key", sizeof(info));
	kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
	ctx = EVP_KDF_CTX_new(kdf);
	EVP_KDF_free(kdf);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return total;
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, key, 32U);
	params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, sizeof(salt));
	params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, sizeof(info));
	params[4] = OSSL_PARAM_construct_end();
	CHECK(EVP_KDF_derive(ctx, okm, sizeof(okm), params) == 1);
	CT_PUBLIC(okm, sizeof(okm));
	total += fold(okm, sizeof(okm));
	EVP_KDF_CTX_free(ctx);
	CT_PUBLIC(key, sizeof(key));
	return total;
}

/* P-256 ECDH through EVP_PKEY_derive with a secret private scalar (a new key's) and a public peer. */
static unsigned
run_ecdh(void)
{
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	uint8_t shared[32];
	size_t length;
	unsigned total;

	total = 0U;
	own = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	peer = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	CHECK(own != NULL && peer != NULL);
	if (own == NULL || peer == NULL) {
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
		return 0U;
	}

	/* The scalar is secret while the context is prepared: those calls decide on public state only. */
	CT_SECRET(own->private_key, own->private_size);
	ctx = EVP_PKEY_CTX_new(own, NULL);
	CHECK(ctx != NULL);
	CHECK(EVP_PKEY_derive_init(ctx) == 1);
	CHECK(EVP_PKEY_derive_set_peer(ctx, peer) == 1);

	/*
	 * The verdict of cryc_ecc_ecdh (the scalar in range, the product not
	 * at infinity) is a decision on the secret by design, so the scalar
	 * is declassified for the derivation, as ct-api-pkey.c does for
	 * X25519: the arithmetic itself is checked in ct-ecc.c.
	 */
	CT_PUBLIC(own->private_key, own->private_size);
	length = sizeof(shared);
	CHECK(EVP_PKEY_derive(ctx, shared, &length) == 1);
	total += fold(shared, sizeof(shared));
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(own);
	EVP_PKEY_free(peer);
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* Every part. */
	total = run_inverse();
	total += run_cbc();
	total += run_hmac_hkdf();
	total += run_ecdh();

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-api-passkey");
}
