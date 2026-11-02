/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the public calls libpasskey's cryptography uses beyond
 * the ssh subset: the AES inverse cipher (cryc-aes.h) and AES-256-CBC
 * through EVP, HMAC, HKDF through EVP_KDF and the OSSL_PARAM
 * constructors, and the P-256 EC keys (EVP_PKEY_fromdata,
 * EVP_PKEY_public_check, EVP_PKEY_Q_keygen, EVP_PKEY_get_octet_string_param,
 * ECDSA verification and ECDH).
 *
 * Expected values: FIPS 197 Appendix C, NIST SP 800-38A F.2.5 and F.2.6,
 * the CAVP AES KAT files (ECB and CBC, every key size for the inverse
 * cipher), RFC 4231 section 4, RFC 5869 Appendix A, the CAVP ECDSA PKV
 * file, and Wycheproof's hmac_sha256, hkdf_sha256,
 * ecdsa_secp256r1_sha256 and ecdh_secp256r1_ecpoint files.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-api.h"

/* The largest vector value this test decodes. */
#define TEST_MAX		512U

/* Makes a hex string's bytes; returns the size (checked). */
static size_t
hex(
	const char *text,
	uint8_t *out,
	size_t room)
{
	size_t size;

	/* The decoding must succeed. */
	size = hex_decode(text, out, room);
	CHECK(size != (size_t)-1);
	if (size == (size_t)-1)
		return 0U;
	return size;
}

/* Tells whether the error queue holds exactly one code of reason, and empties it. */
static int
one_error(
	unsigned reason)
{
	unsigned long code;
	unsigned long next;

	/* The first code, then nothing. */
	code = ERR_get_error();
	next = ERR_get_error();
	ERR_clear_error();
	if (code == 0UL || next != 0UL)
		return 0;
	if ((unsigned)ERR_GET_REASON(code) != (reason & 0x7fffffU))
		return 0;
	return 1;
}

/* Tells whether the error queue is empty. */
static int
no_error(void)
{
	unsigned long code;

	/* Nothing queued. */
	code = ERR_get_error();
	ERR_clear_error();
	return code == 0UL;
}

/* Checks the inverse cipher with FIPS 197 Appendix C and the round trip of every group size. */
static void
check_aes_inverse(void)
{
	static const char *const keys[3] = {
		"000102030405060708090a0b0c0d0e0f",
		"000102030405060708090a0b0c0d0e0f1011121314151617",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
	};
	static const char *const ciphers[3] = {
		"69c4e0d86a7b0430d8cdb78070b4c55a",
		"dda97ca4864cdfe06eaf70a0ec0d7191",
		"8ea2b7ca516745bfeafc49904b496089"
	};
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t plain[16];
	uint8_t cipher[16];
	uint8_t out[16];
	uint8_t data[9 * 16];
	uint8_t round[9 * 16];
	size_t key_size;
	size_t index;
	size_t count;
	unsigned seed;
	int error;

	/* FIPS 197 C.1 to C.3: the inverse cipher gives the plaintext back. */
	hex("00112233445566778899aabbccddeeff", plain, sizeof(plain));
	for (index = 0U; index < 3U; index++) {
		key_size = hex(keys[index], key, sizeof(key));
		hex(ciphers[index], cipher, sizeof(cipher));
		error = cryc_aes_init(&aes, key, key_size);
		CHECK(error == 0);
		cryc_aes_decrypt_blocks(&aes, cipher, out, 1U);
		CHECK_BYTES(out, plain, 16U);

		/* In place too. */
		memcpy(out, cipher, 16U);
		cryc_aes_decrypt_blocks(&aes, out, out, 1U);
		CHECK_BYTES(out, plain, 16U);
	}

	/* Every count from 0 to 9 blocks (groups of four and the rest), every key size, decrypts what was encrypted. */
	for (seed = 0U; seed < 30U; seed++) {
		key_size = 16U + 8U * (seed % 3U);
		for (index = 0U; index < sizeof(key); index++)
			key[index] = (uint8_t)(seed * 37U + index * 11U + 1U);
		for (index = 0U; index < sizeof(data); index++)
			data[index] = (uint8_t)(seed * 5U + index * 29U);
		error = cryc_aes_init(&aes, key, key_size);
		CHECK(error == 0);
		count = seed % 10U;
		cryc_aes_encrypt_blocks(&aes, data, round, count);
		cryc_aes_decrypt_blocks(&aes, round, round, count);
		CHECK_BYTES(round, data, count * 16U);
	}
	cryc_aes_clear(&aes);
}

/* Checks one CAVP AES KAT file: ECB through cryc_aes, CBC (256-bit keys) through EVP. */
static void
check_aes_kat(
	const char *name,
	int cbc)
{
	struct vec_file file;
	struct cryc_aes aes;
	EVP_CIPHER_CTX *ctx;
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t plain[TEST_MAX];
	uint8_t cipher[TEST_MAX];
	uint8_t out[TEST_MAX + 16];
	const char *decrypt;
	size_t key_size;
	size_t plain_size;
	size_t cipher_size;
	unsigned long records;
	int written;
	int last;
	int good;

	/* The file must be there. */
	records = 0UL;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		key_size = vec_hex(&file, "KEY", key, sizeof(key));
		plain_size = vec_hex(&file, "PLAINTEXT", plain, sizeof(plain));
		cipher_size = vec_hex(&file, "CIPHERTEXT", cipher, sizeof(cipher));
		decrypt = vec_get(&file, "section.DECRYPT");
		if (key_size == (size_t)-1 || plain_size == (size_t)-1 || cipher_size == (size_t)-1)
			continue;
		CHECK(plain_size == cipher_size && plain_size % 16U == 0U);
		records++;

		/* ECB: the inverse cipher of every key size, and the cipher for the encryption records. */
		if (!cbc) {
			CHECK(cryc_aes_init(&aes, key, key_size) == 0);
			if (decrypt != NULL) {
				cryc_aes_decrypt_blocks(&aes, cipher, out, cipher_size / 16U);
				CHECK_BYTES(out, plain, plain_size);
			} else {
				cryc_aes_encrypt_blocks(&aes, plain, out, plain_size / 16U);
				CHECK_BYTES(out, cipher, cipher_size);
			}
			cryc_aes_clear(&aes);
			continue;
		}

		/* CBC through EVP, padding off. */
		CHECK(vec_hex(&file, "IV", iv, sizeof(iv)) == 16U);
		ctx = EVP_CIPHER_CTX_new();
		CHECK(ctx != NULL);
		if (ctx == NULL)
			continue;
		good = EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, decrypt == NULL);
		CHECK(good == 1);
		CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
		written = -1;
		last = -1;
		if (decrypt != NULL) {
			CHECK(EVP_CipherUpdate(ctx, out, &written, cipher, (int)cipher_size) == 1);
			CHECK(EVP_CipherFinal_ex(ctx, out + written, &last) == 1);
			CHECK(written == (int)plain_size && last == 0);
			CHECK_BYTES(out, plain, plain_size);
		} else {
			CHECK(EVP_CipherUpdate(ctx, out, &written, plain, (int)plain_size) == 1);
			CHECK(EVP_CipherFinal_ex(ctx, out + written, &last) == 1);
			CHECK(written == (int)cipher_size && last == 0);
			CHECK_BYTES(out, cipher, cipher_size);
		}
		EVP_CIPHER_CTX_free(ctx);
		CHECK(no_error());
	}
	vec_close(&file);
	CHECK(records > 0UL);
}

/* Checks AES-256-CBC through EVP with NIST SP 800-38A F.2.5 and F.2.6, and its edges. */
static void
check_cbc(void)
{
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t plain[64];
	uint8_t cipher[64];
	uint8_t out[64 + 16];
	uint8_t other[64 + 16];
	EVP_CIPHER_CTX *ctx;
	size_t step;
	size_t offset;
	size_t chunk;
	int written;
	int total;
	int last;
	int direction;

	hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key, sizeof(key));
	hex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));
	hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
	    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", plain, sizeof(plain));
	hex("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
	    "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b", cipher, sizeof(cipher));
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;

	/* Both directions, fed in steps of every size from 1 to 64 bytes: the output is the same. */
	for (direction = 1; direction >= 0; direction--) {
		for (step = 1U; step <= 64U; step++) {
			CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, direction) == 1);
			CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
			total = 0;
			for (offset = 0U; offset < 64U; offset += chunk) {
				chunk = step;
				if (chunk > 64U - offset)
					chunk = 64U - offset;
				written = -1;
				CHECK(EVP_CipherUpdate(ctx, out + total, &written, (direction ? plain : cipher) + offset, (int)chunk) == 1);
				CHECK(written >= 0 && written % 16 == 0);
				total += written;
			}
			CHECK(EVP_CipherFinal_ex(ctx, out + total, &last) == 1);
			CHECK(total == 64 && last == 0);
			CHECK_BYTES(out, direction ? cipher : plain, 64U);
		}
	}

	/* In place, whole blocks; a key and an IV given apart (the IV restarts the chain). */
	CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, NULL, 0) == 1);
	CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
	CHECK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1) == 1);
	memcpy(other, cipher, 64U);
	CHECK(EVP_CipherUpdate(ctx, other, &written, other, 64) == 1);
	CHECK(written == 64);
	CHECK_BYTES(other, plain, 64U);
	CHECK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1) == 1);
	CHECK(EVP_CipherUpdate(ctx, out, &written, cipher, 32) == 1);
	CHECK_BYTES(out, plain, 32U);
	CHECK(no_error());

	/* A partial block left at the end is refused by EVP_CipherFinal_ex. */
	CHECK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, 1) == 1);
	CHECK(EVP_CipherUpdate(ctx, out, &written, plain, 20) == 1);
	CHECK(written == 16);
	CHECK(EVP_CipherFinal_ex(ctx, out + written, &last) == 0);
	CHECK(one_error(CRYC_R_EVP_DATA_NOT_MULTIPLE_OF_BLOCK_LENGTH));

	/* An empty update writes nothing. */
	CHECK(EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, 1) == 1);
	written = -1;
	CHECK(EVP_CipherUpdate(ctx, out, &written, plain, 0) == 1);
	CHECK(written == 0);

	/* Padding on (the default after naming the cipher) is not implemented. */
	CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, 1) == 1);
	CHECK(EVP_CipherUpdate(ctx, out, &written, plain, 16) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));
	CHECK(EVP_CipherFinal_ex(ctx, out, &last) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));

	/* EVP_Cipher is ChaCha20's only. */
	CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
	CHECK(EVP_Cipher(ctx, out, plain, 16U) == -1);
	CHECK(one_error(CRYC_R_UNSUPPORTED));

	/* A context without a key refuses data. */
	CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, NULL, iv, 1) == 1);
	CHECK(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1);
	CHECK(EVP_CipherUpdate(ctx, out, &written, plain, 16) == 0);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));

	/* NULL arguments. */
	CHECK(EVP_CIPHER_CTX_set_padding(NULL, 0) == 0);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(EVP_CipherFinal_ex(ctx, out, NULL) == 0);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	EVP_CIPHER_CTX_free(ctx);

	/* A fresh context has no cipher to end. */
	ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	CHECK(EVP_CipherFinal_ex(ctx, out, &last) == 0);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));

	/* ChaCha20 after AES starts afresh, and its final writes nothing. */
	CHECK(EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, 1) == 1);
	CHECK(EVP_CipherInit_ex(ctx, EVP_chacha20(), NULL, key, iv, 1) == 1);
	CHECK(EVP_CipherUpdate(ctx, out, &written, plain, 5) == 1);
	CHECK(written == 5);
	CHECK(EVP_CipherFinal_ex(ctx, out, &last) == 1);
	CHECK(last == 0);
	CHECK(no_error());
	EVP_CIPHER_CTX_free(ctx);
}

/* Checks HMAC with RFC 4231 section 4 (the full-length tags) and its edges. */
static void
check_hmac_rfc(void)
{
	static const char *const tags[6] = {
		"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
		"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
		"773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
		"82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
		"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
		"9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"
	};
	static const char text6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
	static const char text7[] = "This is a test using a larger than block-size key and a larger than block-size data. "
	    "The key needs to be hashed before being used by the HMAC algorithm.";
	uint8_t key[131];
	uint8_t data[160];
	uint8_t want[32];
	uint8_t mac[32];
	const uint8_t *keys[6];
	const uint8_t *datas[6];
	size_t key_sizes[6];
	size_t data_sizes[6];
	unsigned char *made;
	unsigned int length;
	uint8_t key4[25];
	uint8_t aa[131];
	uint8_t dd[50];
	uint8_t cd[50];
	uint8_t eleven[20];
	size_t index;

	/* The inputs of test cases 1, 2, 3, 4, 6 and 7. */
	memset(eleven, 0x0b, sizeof(eleven));
	memset(aa, 0xaa, sizeof(aa));
	memset(dd, 0xdd, sizeof(dd));
	memset(cd, 0xcd, sizeof(cd));
	for (index = 0U; index < sizeof(key4); index++)
		key4[index] = (uint8_t)(index + 1U);
	keys[0] = eleven; key_sizes[0] = 20U; datas[0] = (const uint8_t *)"Hi There"; data_sizes[0] = 8U;
	keys[1] = (const uint8_t *)"Jefe"; key_sizes[1] = 4U;
	datas[1] = (const uint8_t *)"what do ya want for nothing?"; data_sizes[1] = 28U;
	keys[2] = aa; key_sizes[2] = 20U; datas[2] = dd; data_sizes[2] = 50U;
	keys[3] = key4; key_sizes[3] = 25U; datas[3] = cd; data_sizes[3] = 50U;
	keys[4] = aa; key_sizes[4] = 131U; datas[4] = (const uint8_t *)text6; data_sizes[4] = sizeof(text6) - 1U;
	keys[5] = aa; key_sizes[5] = 131U; datas[5] = (const uint8_t *)text7; data_sizes[5] = sizeof(text7) - 1U;

	/* Each tag. */
	for (index = 0U; index < 6U; index++) {
		hex(tags[index], want, sizeof(want));
		length = 0U;
		made = HMAC(EVP_sha256(), keys[index], (int)key_sizes[index], datas[index], data_sizes[index], mac, &length);
		CHECK(made == mac);
		CHECK(length == 32U);
		CHECK_BYTES(mac, want, 32U);
	}
	CHECK(no_error());

	/* An empty key and an empty message may be NULL; md_len may be NULL. */
	made = HMAC(EVP_sha256(), NULL, 0, NULL, 0U, mac, NULL);
	CHECK(made == mac);
	memcpy(key, mac, 32U);
	made = HMAC(EVP_sha256(), "", 0, data, 0U, mac, NULL);
	CHECK(made == mac);
	CHECK_BYTES(mac, key, 32U);
	CHECK(no_error());

	/* The refusals: no output, another digest, a negative length, a missing buffer. */
	CHECK(HMAC(EVP_sha256(), key, 4, data, 4U, NULL, &length) == NULL);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(HMAC(NULL, key, 4, data, 4U, mac, &length) == NULL);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(HMAC((const EVP_MD *)&key, key, 4, data, 4U, mac, &length) == NULL);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	CHECK(HMAC(EVP_sha256(), key, -1, data, 4U, mac, &length) == NULL);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	CHECK(HMAC(EVP_sha256(), NULL, 4, data, 4U, mac, &length) == NULL);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(HMAC(EVP_sha256(), key, 4, NULL, 4U, mac, &length) == NULL);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
}

/* Checks HMAC with Wycheproof's hmac_sha256 file (the full-length tags). */
static void
check_hmac_wycheproof(void)
{
	struct vec_file file;
	uint8_t key[TEST_MAX];
	uint8_t msg[TEST_MAX];
	uint8_t tag[64];
	uint8_t mac[32];
	const char *result;
	const char *tag_bits;
	size_t key_size;
	size_t msg_size;
	size_t tag_size;
	unsigned long count;
	unsigned char *made;

	count = 0UL;
	if (vec_open(&file, "wycheproof-hmac_sha256_test") != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		tag_bits = vec_get(&file, "group.tagSize");
		result = vec_get(&file, "result");
		key_size = vec_hex(&file, "key", key, sizeof(key));
		msg_size = vec_hex(&file, "msg", msg, sizeof(msg));
		tag_size = vec_hex(&file, "tag", tag, sizeof(tag));
		if (tag_bits == NULL || strcmp(tag_bits, "256") != 0 || result == NULL)
			continue;
		if (key_size == (size_t)-1 || msg_size == (size_t)-1 || tag_size != 32U)
			continue;
		made = HMAC(EVP_sha256(), key, (int)key_size, msg, msg_size, mac, NULL);
		CHECK(made == mac);
		if (strcmp(result, "valid") == 0)
			CHECK_BYTES(mac, tag, 32U);
		else
			CHECK(memcmp(mac, tag, 32U) != 0);
		count++;
	}
	vec_close(&file);
	printf("  Wycheproof HMAC-SHA-256: %lu tags\n", count);
	CHECK(count > 50UL);
}

/* Derives with HKDF through EVP_KDF in one call; returns EVP_KDF_derive's answer. */
static int
derive(
	const char *digest,
	const uint8_t *ikm,
	size_t ikm_size,
	const uint8_t *salt,
	size_t salt_size,
	const uint8_t *info,
	size_t info_size,
	uint8_t *out,
	size_t out_size)
{
	OSSL_PARAM params[5];
	EVP_KDF_CTX *ctx;
	EVP_KDF *kdf;
	int good;

	/* The KDF and a context. */
	kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
	CHECK(kdf != NULL);
	ctx = EVP_KDF_CTX_new(kdf);
	EVP_KDF_free(kdf);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return -100;

	/* The parameters as libpasskey gives them. */
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)digest, 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)ikm, ikm_size);
	params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, salt_size);
	params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, info_size);
	params[4] = OSSL_PARAM_construct_end();
	good = EVP_KDF_derive(ctx, out, out_size, params);
	EVP_KDF_CTX_free(ctx);
	return good;
}

/* Checks HKDF with RFC 5869 Appendix A (A.1 to A.3), Wycheproof's hkdf_sha256 file, and the parameter rules. */
static void
check_hkdf(void)
{
	struct vec_file file;
	uint8_t ikm[TEST_MAX];
	uint8_t salt[TEST_MAX];
	uint8_t info[TEST_MAX];
	uint8_t want[8160];
	uint8_t out[8161];
	uint8_t other[64];
	OSSL_PARAM params[6];
	EVP_KDF_CTX *ctx;
	EVP_KDF *kdf;
	const char *result;
	const char *size_text;
	size_t ikm_size;
	size_t salt_size;
	size_t info_size;
	size_t want_size;
	size_t size;
	size_t index;
	unsigned long count;
	int good;

	/* A.1: the basic case. */
	ikm_size = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
	salt_size = hex("000102030405060708090a0b0c", salt, sizeof(salt));
	info_size = hex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));
	want_size = hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", want, sizeof(want));
	CHECK(derive("SHA256", ikm, ikm_size, salt, salt_size, info, info_size, out, want_size) == 1);
	CHECK_BYTES(out, want, want_size);

	/* A.2: longer inputs and output; the other names of SHA-256. */
	for (index = 0U; index < 80U; index++) {
		ikm[index] = (uint8_t)index;
		salt[index] = (uint8_t)(0x60U + index);
		info[index] = (uint8_t)(0xb0U + index);
	}
	want_size = hex("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
	    "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87", want, sizeof(want));
	CHECK(derive("sha2-256", ikm, 80U, salt, 80U, info, 80U, out, want_size) == 1);
	CHECK_BYTES(out, want, want_size);
	CHECK(derive("SHA-256", ikm, 80U, salt, 80U, info, 80U, out, want_size) == 1);
	CHECK_BYTES(out, want, want_size);

	/* A.3: empty salt and info. */
	memset(ikm, 0x0b, 22U);
	want_size = hex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", want, sizeof(want));
	CHECK(derive("SHA256", ikm, 22U, salt, 0U, info, 0U, out, want_size) == 1);
	CHECK_BYTES(out, want, want_size);
	CHECK(no_error());

	/* The size limits: 0 and 255 * 32 + 1 bytes fail, 255 * 32 succeed. */
	CHECK(derive("SHA256", ikm, 22U, salt, 0U, info, 0U, out, 0U) == 0);
	CHECK(one_error(CRYC_R_EVP_INVALID_KEY_LENGTH));
	CHECK(derive("SHA256", ikm, 22U, salt, 0U, info, 0U, out, 8161U) == 0);
	CHECK(one_error(CRYC_R_EVP_INVALID_KEY_LENGTH));
	CHECK(derive("SHA256", ikm, 22U, salt, 0U, info, 0U, out, 8160U) == 1);
	CHECK_BYTES(out, want, want_size);

	/* Another digest is refused. */
	CHECK(derive("SHA384", ikm, 22U, salt, 0U, info, 0U, out, 32U) == 0);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));

	/* Wycheproof. */
	count = 0UL;
	if (vec_open(&file, "wycheproof-hkdf_sha256_test") != 0) {
		CHECK(0);
	} else {
		while (vec_next(&file)) {
			result = vec_get(&file, "result");
			size_text = vec_get(&file, "size");
			ikm_size = vec_hex(&file, "ikm", ikm, sizeof(ikm));
			salt_size = vec_hex(&file, "salt", salt, sizeof(salt));
			info_size = vec_hex(&file, "info", info, sizeof(info));
			want_size = vec_hex(&file, "okm", want, sizeof(want));
			if (result == NULL || size_text == NULL || ikm_size == (size_t)-1 || salt_size == (size_t)-1 ||
			    info_size == (size_t)-1 || want_size == (size_t)-1)
				continue;
			size = (size_t)strtoul(size_text, NULL, 10);
			good = derive("SHA256", ikm, ikm_size, salt, salt_size, info, info_size, out, size);
			if (strcmp(result, "valid") == 0) {
				CHECK(good == 1);
				CHECK(want_size == size);
				CHECK_BYTES(out, want, want_size);
			} else {
				CHECK(good == 0);
				ERR_clear_error();
			}
			count++;
		}
		vec_close(&file);
	}
	printf("  Wycheproof HKDF-SHA-256: %lu cases\n", count);
	CHECK(count > 50UL);

	/* The parameter rules on one context, with A.1's inputs. */
	ikm_size = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
	salt_size = hex("000102030405060708090a0b0c", salt, sizeof(salt));
	info_size = hex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));
	want_size = hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", want, sizeof(want));
	CHECK(EVP_KDF_fetch(NULL, "hkdf", NULL) != NULL);
	CHECK(EVP_KDF_fetch(NULL, "TLS13-KDF", NULL) == NULL);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	CHECK(EVP_KDF_fetch(NULL, "HKDF", "provider=default") == NULL);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	CHECK(EVP_KDF_CTX_new(NULL) == NULL);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
	ctx = EVP_KDF_CTX_new(kdf);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;

	/* Nothing set: no digest, no key. */
	CHECK(EVP_KDF_derive(ctx, out, 32U, NULL) == 0);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));

	/* The info in two parts is joined; the parameters stay for a later call without any. */
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, ikm, ikm_size);
	params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, 3U);
	params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, salt_size);
	params[4] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info + 3U, info_size - 3U);
	params[5] = OSSL_PARAM_construct_end();
	CHECK(EVP_KDF_derive(ctx, out, want_size, params) == 1);
	CHECK_BYTES(out, want, want_size);
	memset(out, 0, sizeof(other));
	CHECK(EVP_KDF_derive(ctx, out, want_size, NULL) == 1);
	CHECK_BYTES(out, want, want_size);

	/* An unknown name, a mode, and a wrong data type are refused and change nothing. */
	params[0] = OSSL_PARAM_construct_utf8_string("mode", (char *)"EXPAND_ONLY", 0U);
	params[1] = OSSL_PARAM_construct_end();
	CHECK(EVP_KDF_derive(ctx, out, want_size, params) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_SALT, (char *)"salt", 0U);
	CHECK(EVP_KDF_derive(ctx, out, want_size, params) == 0);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	params[0] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_DIGEST, (void *)"SHA256", 6U);
	CHECK(EVP_KDF_derive(ctx, out, want_size, params) == 0);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	memset(other, 0, sizeof(other));
	CHECK(EVP_KDF_derive(ctx, other, want_size, NULL) == 1);
	CHECK_BYTES(other, want, want_size);

	/* A new info replaces the old one. */
	params[0] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, 0U);
	params[1] = OSSL_PARAM_construct_end();
	CHECK(EVP_KDF_derive(ctx, out, want_size, params) == 1);
	CHECK(memcmp(out, want, want_size) != 0);

	/* A missing output. */
	CHECK(EVP_KDF_derive(ctx, NULL, 32U, NULL) == 0);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	EVP_KDF_CTX_free(ctx);
	CHECK(EVP_KDF_derive(NULL, out, 32U, NULL) == 0);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	EVP_KDF_CTX_free(NULL);
	EVP_KDF_free(NULL);
}

/* Checks the OSSL_PARAM constructors. */
static void
check_params(void)
{
	OSSL_PARAM param;
	char text[] = "prime256v1";
	uint8_t bytes[3];

	/* A UTF-8 string with its size from strlen, and with a given size. */
	param = OSSL_PARAM_construct_utf8_string("group", text, 0U);
	CHECK(strcmp(param.key, "group") == 0 && param.data_type == OSSL_PARAM_UTF8_STRING);
	CHECK(param.data == text && param.data_size == 10U && param.return_size == OSSL_PARAM_UNMODIFIED);
	param = OSSL_PARAM_construct_utf8_string("group", text, 5U);
	CHECK(param.data_size == 5U);
	param = OSSL_PARAM_construct_utf8_string("group", NULL, 0U);
	CHECK(param.data == NULL && param.data_size == 0U);

	/* An octet string. */
	param = OSSL_PARAM_construct_octet_string("pub", bytes, sizeof(bytes));
	CHECK(param.data_type == OSSL_PARAM_OCTET_STRING && param.data == bytes && param.data_size == 3U);
	CHECK(param.return_size == OSSL_PARAM_UNMODIFIED);

	/* The end. */
	param = OSSL_PARAM_construct_end();
	CHECK(param.key == NULL && param.data_type == 0U && param.data == NULL && param.data_size == 0U && param.return_size == 0U);

	/* The string comparison of the parameters. */
	param = OSSL_PARAM_construct_utf8_string("group", text, 0U);
	CHECK(cryc_param_string_equal(&param, "PRIME256V1") == 1);
	CHECK(cryc_param_string_equal(&param, "PRIME256V") == 0);
	CHECK(cryc_param_string_equal(&param, "PRIME256V12") == 0);
	param = OSSL_PARAM_construct_utf8_string("group", text, 11U);
	CHECK(cryc_param_string_equal(&param, "PRIME256V1") == 1);
	param = OSSL_PARAM_construct_octet_string("group", text, 10U);
	CHECK(cryc_param_string_equal(&param, "PRIME256V1") == 0);
}

/* Makes a P-256 public key with EVP_PKEY_fromdata from an uncompressed point; NULL when it is refused. */
static EVP_PKEY *
p256_public(
	const uint8_t *point,
	size_t size)
{
	OSSL_PARAM params[3];
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *key;
	int good;

	/* As libpasskey makes it. */
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void *)point, size);
	params[2] = OSSL_PARAM_construct_end();
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return NULL;
	key = NULL;
	good = EVP_PKEY_fromdata_init(ctx);
	CHECK(good == 1);
	good = EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params);
	EVP_PKEY_CTX_free(ctx);
	if (good != 1) {
		CHECK(key == NULL);
		ERR_clear_error();
		return NULL;
	}
	CHECK(key != NULL);
	return key;
}

/* Makes a P-256 key with a given private scalar (32 bytes) through the key's structure: a test-only door. */
static EVP_PKEY *
p256_private(
	const uint8_t *scalar)
{
	EVP_PKEY *key;

	/* The object as pkey_new makes it, with the scalar and no public point. */
	key = calloc(1, sizeof(*key));
	CHECK(key != NULL);
	if (key == NULL)
		return NULL;
	key->private_key = malloc(32U);
	CHECK(key->private_key != NULL);
	if (key->private_key == NULL) {
		free(key);
		return NULL;
	}
	memcpy(key->private_key, scalar, 32U);
	key->references = 1L;
	key->type = CRYC_PKEY_EC_P256;
	key->public_size = 65U;
	key->private_size = 32U;
	key->has_private = 1;
	return key;
}

/* Puts a hex coordinate of up to 32 bytes into 32 bytes, left-padded; returns 0 when it is wider. */
static int
coordinate(
	const char *text,
	uint8_t *out)
{
	char padded[80];
	uint8_t bytes[40];
	size_t length;
	size_t size;
	size_t skip;

	/* An odd number of digits gets a leading zero. */
	length = strlen(text);
	if (length + 2U > sizeof(padded))
		return 0;
	padded[0] = '0';
	memcpy(padded + 1, text, length + 1U);
	size = hex_decode(length % 2U != 0U ? padded : padded + 1, bytes, sizeof(bytes));
	if (size == (size_t)-1)
		return 0;

	/* Leading zero bytes are not width. */
	for (skip = 0U; skip < size && size - skip > 32U && bytes[skip] == 0U; skip++)
		;
	if (size - skip > 32U)
		return 0;
	memset(out, 0, 32U);
	memcpy(out + 32U - (size - skip), bytes + skip, size - skip);
	return 1;
}

/* Checks EVP_PKEY_fromdata and EVP_PKEY_public_check with the P-256 records of the CAVP PKV file. */
static void
check_pkv(void)
{
	struct vec_file file;
	uint8_t point[65];
	EVP_PKEY_CTX *check;
	EVP_PKEY *key;
	const char *section;
	const char *later;
	const char *qx;
	const char *qy;
	const char *result;
	unsigned long passed;
	unsigned long failed;
	int fits;

	passed = 0UL;
	failed = 0UL;
	if (vec_open(&file, "cavp-186-4ecdsatestvectors-PKV") != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		/* The converted file lists every section seen so far: a P-256 record has P-256 as its last. */
		section = vec_get(&file, "section.P-256");
		later = vec_get(&file, "section.P-384");
		qx = vec_get(&file, "Qx");
		qy = vec_get(&file, "Qy");
		result = vec_get(&file, "Result");
		if (section == NULL || later != NULL || qx == NULL || qy == NULL || result == NULL)
			continue;

		/* A coordinate wider than 32 bytes cannot be given: it is a failing record. */
		point[0] = 0x04U;
		fits = coordinate(qx, point + 1U);
		if (fits)
			fits = coordinate(qy, point + 33U);
		if (!fits) {
			CHECK(result[0] == 'F');
			failed++;
			continue;
		}

		/* The key is made exactly when the record passes, and then it checks. */
		key = p256_public(point, sizeof(point));
		if (result[0] == 'P') {
			CHECK(key != NULL);
			check = EVP_PKEY_CTX_new(key, NULL);
			CHECK(check != NULL);
			CHECK(EVP_PKEY_public_check(check) == 1);
			EVP_PKEY_CTX_free(check);
			passed++;
		} else {
			CHECK(key == NULL);
			failed++;
		}
		EVP_PKEY_free(key);
	}
	vec_close(&file);
	printf("  CAVP PKV P-256: %lu pass, %lu fail\n", passed, failed);
	CHECK(passed > 0UL && failed > 0UL);
	CHECK(no_error());
}

/* Checks ECDSA verification with Wycheproof's ecdsa_secp256r1_sha256 file (DER signatures). */
static void
check_ecdsa(void)
{
	struct vec_file file;
	uint8_t point[TEST_MAX];
	uint8_t msg[TEST_MAX * 4];
	uint8_t sig[TEST_MAX];
	uint8_t hash[32];
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *key;
	const char *result;
	size_t point_size;
	size_t msg_size;
	size_t sig_size;
	size_t size;
	unsigned long good_count;
	unsigned long bad_count;
	int verdict;
	int plain;

	good_count = 0UL;
	bad_count = 0UL;
	if (vec_open(&file, "wycheproof-ecdsa_secp256r1_sha256_test") != 0) {
		CHECK(0);
		return;
	}
	point_size = 0U;
	while (vec_next(&file)) {
		/* The group's key comes in a record of its own (its PEM form holds a blank line); the tests after it use it. */
		size = vec_hex(&file, "group.publicKey.uncompressed", point, sizeof(point));
		if (size != (size_t)-1)
			point_size = size;
		result = vec_get(&file, "result");
		msg_size = vec_hex(&file, "msg", msg, sizeof(msg));
		sig_size = vec_hex(&file, "sig", sig, sizeof(sig));
		if (result == NULL || point_size != 65U || msg_size == (size_t)-1 || sig_size == (size_t)-1)
			continue;
		CHECK(EVP_Digest(msg, msg_size, hash, NULL, EVP_sha256(), NULL) == 1);
		key = p256_public(point, point_size);
		CHECK(key != NULL);
		if (key == NULL)
			continue;

		/* With the digest set, then without one: the same verdict. */
		for (plain = 0; plain < 2; plain++) {
			ctx = EVP_PKEY_CTX_new(key, NULL);
			CHECK(ctx != NULL);
			CHECK(EVP_PKEY_verify_init(ctx) == 1);
			if (!plain)
				CHECK(EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1);
			verdict = EVP_PKEY_verify(ctx, sig, sig_size, hash, sizeof(hash));
			EVP_PKEY_CTX_free(ctx);
			ERR_clear_error();
			if (strcmp(result, "valid") == 0) {
				CHECK(verdict == 1);
				good_count++;
			} else if (strcmp(result, "invalid") == 0) {
				CHECK(verdict != 1);
				bad_count++;
			}
		}
		EVP_PKEY_free(key);
	}
	vec_close(&file);
	printf("  Wycheproof ECDSA P-256 SHA-256: %lu valid, %lu invalid verdicts\n", good_count, bad_count);
	CHECK(good_count > 50UL && bad_count > 50UL);
}

/* Checks ECDH with Wycheproof's ecdh_secp256r1_ecpoint file. */
static void
check_ecdh(void)
{
	struct vec_file file;
	uint8_t pub[TEST_MAX];
	uint8_t priv[TEST_MAX];
	uint8_t want[64];
	uint8_t scalar[32];
	uint8_t shared[32];
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	const char *result;
	size_t pub_size;
	size_t priv_size;
	size_t want_size;
	size_t length;
	size_t skip;
	unsigned long computed;
	unsigned long refused;
	int good;

	computed = 0UL;
	refused = 0UL;
	if (vec_open(&file, "wycheproof-ecdh_secp256r1_ecpoint_test") != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		pub_size = vec_hex(&file, "public", pub, sizeof(pub));
		priv_size = vec_hex(&file, "private", priv, sizeof(priv));
		want_size = vec_hex(&file, "shared", want, sizeof(want));
		if (result == NULL || pub_size == (size_t)-1 || priv_size == (size_t)-1 || want_size == (size_t)-1)
			continue;

		/* The private scalar in 32 bytes (Wycheproof may add a leading zero). */
		for (skip = 0U; skip < priv_size && priv_size - skip > 32U && priv[skip] == 0U; skip++)
			;
		if (priv_size - skip > 32U)
			continue;
		memset(scalar, 0, sizeof(scalar));
		memcpy(scalar + 32U - (priv_size - skip), priv + skip, priv_size - skip);

		/* The peer: an invalid or compressed point is refused by EVP_PKEY_fromdata. */
		peer = p256_public(pub, pub_size);
		if (peer == NULL) {
			CHECK(strcmp(result, "valid") != 0);
			refused++;
			continue;
		}
		own = p256_private(scalar);
		ctx = EVP_PKEY_CTX_new(own, NULL);
		CHECK(ctx != NULL);
		good = EVP_PKEY_derive_init(ctx);
		CHECK(good == 1);
		good = EVP_PKEY_derive_set_peer(ctx, peer);
		CHECK(good == 1);
		length = sizeof(shared);
		good = EVP_PKEY_derive(ctx, shared, &length);
		if (strcmp(result, "invalid") == 0) {
			CHECK(good != 1);
			ERR_clear_error();
			refused++;
		} else {
			CHECK(good == 1);
			CHECK(length == 32U && want_size == 32U);
			CHECK_BYTES(shared, want, 32U);
			computed++;
		}
		EVP_PKEY_CTX_free(ctx);
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
	}
	vec_close(&file);
	printf("  Wycheproof ECDH P-256: %lu computed, %lu refused\n", computed, refused);
	CHECK(computed > 50UL && refused > 0UL);
}

/* Derives the shared secret of own with peer into shared (32 bytes); returns EVP_PKEY_derive's answer. */
static int
agree(
	EVP_PKEY *own,
	EVP_PKEY *peer,
	uint8_t *shared)
{
	EVP_PKEY_CTX *ctx;
	size_t length;
	int good;

	/* One derivation. */
	ctx = EVP_PKEY_CTX_new(own, NULL);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return 0;
	good = EVP_PKEY_derive_init(ctx);
	if (good == 1)
		good = EVP_PKEY_derive_set_peer(ctx, peer);
	length = 0U;
	if (good == 1)
		good = EVP_PKEY_derive(ctx, NULL, &length);
	CHECK(length == 32U);
	if (good == 1)
		good = EVP_PKEY_derive(ctx, shared, &length);
	EVP_PKEY_CTX_free(ctx);
	return good;
}

/* Checks EVP_PKEY_Q_keygen, EVP_PKEY_get_octet_string_param and ECDH between two new keys, and the refusals. */
static void
check_keygen(void)
{
	OSSL_PARAM params[4];
	uint8_t point_a[65];
	uint8_t point_b[65];
	uint8_t shared_a[32];
	uint8_t shared_b[32];
	uint8_t sig[8];
	uint8_t raw[80];
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *a;
	EVP_PKEY *b;
	EVP_PKEY *public_b;
	EVP_PKEY *made;
	EVP_PKEY *x25519;
	size_t length;
	int round;

	for (round = 0; round < 8; round++) {
		/* Two key pairs; each public point is on the curve. */
		a = EVP_PKEY_Q_keygen(NULL, NULL, "EC", round % 2 ? "P-256" : "prime256v1");
		b = EVP_PKEY_Q_keygen(NULL, NULL, "ec", "secp256r1");
		CHECK(a != NULL && b != NULL);
		if (a == NULL || b == NULL) {
			EVP_PKEY_free(a);
			EVP_PKEY_free(b);
			return;
		}
		length = 0U;
		CHECK(EVP_PKEY_get_octet_string_param(a, OSSL_PKEY_PARAM_PUB_KEY, point_a, sizeof(point_a), &length) == 1);
		CHECK(length == 65U && point_a[0] == 0x04U);
		CHECK(EVP_PKEY_get_octet_string_param(b, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, point_b, sizeof(point_b), NULL) == 1);
		CHECK(point_b[0] == 0x04U);
		CHECK(memcmp(point_a, point_b, 65U) != 0);

		/* The public point alone makes a valid key, and both sides agree. */
		public_b = p256_public(point_b, sizeof(point_b));
		CHECK(public_b != NULL);
		made = p256_public(point_a, sizeof(point_a));
		CHECK(made != NULL);
		CHECK(agree(a, public_b, shared_a) == 1);
		CHECK(agree(b, made, shared_b) == 1);
		CHECK_BYTES(shared_a, shared_b, 32U);
		EVP_PKEY_free(made);
		EVP_PKEY_free(public_b);
		EVP_PKEY_free(a);
		EVP_PKEY_free(b);
	}
	CHECK(no_error());

	/* The octet string parameter: the size query, a small buffer, another name. */
	a = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	CHECK(a != NULL);
	if (a == NULL)
		return;
	length = 0U;
	CHECK(EVP_PKEY_get_octet_string_param(a, "pub", NULL, 0U, &length) == 1);
	CHECK(length == 65U);
	CHECK(EVP_PKEY_get_octet_string_param(a, "pub", point_a, 64U, &length) == 0);
	CHECK(one_error(CRYC_R_EVP_BUFFER_TOO_SMALL));
	CHECK(EVP_PKEY_get_octet_string_param(a, "priv", point_a, sizeof(point_a), &length) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));

	/* An EC key has no raw form. */
	length = sizeof(raw);
	CHECK(EVP_PKEY_get_raw_public_key(a, raw, &length) == 0);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE));
	CHECK(EVP_PKEY_get_raw_private_key(a, raw, &length) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));

	/* Q_keygen refuses other types and groups. */
	CHECK(EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-384") == NULL);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	CHECK(EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048) == NULL);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	CHECK(EVP_PKEY_Q_keygen(NULL, "fips=yes", "EC", "P-256") == NULL);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));

	/* EVP_PKEY_fromdata's refusals: a private key, a compressed point, a key already there, no public selection. */
	EVP_PKEY_get_octet_string_param(a, "pub", point_a, sizeof(point_a), &length);
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	CHECK(EVP_PKEY_keygen_init(ctx) == 0);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	made = NULL;
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"P-256", 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point_a, sizeof(point_a));
	params[2] = OSSL_PARAM_construct_end();
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_PUBLIC_KEY, params) == 0);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));
	CHECK(EVP_PKEY_fromdata_init(ctx) == 1);
	params[2] = OSSL_PARAM_construct_octet_string("priv", shared_a, 32U);
	params[3] = OSSL_PARAM_construct_end();
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_KEYPAIR, params) == 0);
	CHECK(one_error(CRYC_R_UNSUPPORTED));
	params[2] = OSSL_PARAM_construct_end();
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_KEY_PARAMETERS, params) == 0);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	made = a;
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_PUBLIC_KEY, params) == 0);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	made = NULL;
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_KEYPAIR, params) == 1);
	CHECK(made != NULL);
	EVP_PKEY_free(made);
	made = NULL;
	point_b[0] = (uint8_t)(0x02U + (point_a[64] & 1U));
	memcpy(point_b + 1U, point_a + 1U, 32U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point_b, 33U);
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_PUBLIC_KEY, params) == 0);
	CHECK(one_error(CRYC_R_EVP_INVALID_KEY));
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"secp384r1", 0U);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point_a, sizeof(point_a));
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_PUBLIC_KEY, params) == 0);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	params[0] = params[1];
	params[1] = OSSL_PARAM_construct_end();
	CHECK(EVP_PKEY_fromdata(ctx, &made, EVP_PKEY_PUBLIC_KEY, params) == 0);
	CHECK(one_error(CRYC_R_PASSED_INVALID_ARGUMENT));
	CHECK(made == NULL);
	EVP_PKEY_CTX_free(ctx);

	/* Verification: not for another key type; the digest needs the verification first; the hash size; malformed DER. */
	x25519 = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, shared_a, 32U);
	CHECK(x25519 != NULL);
	ctx = EVP_PKEY_CTX_new(x25519, NULL);
	CHECK(EVP_PKEY_verify_init(ctx) == -2);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE));
	CHECK(EVP_PKEY_public_check(ctx) == -2);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE));
	CHECK(EVP_PKEY_derive_init(ctx) == 1);
	CHECK(EVP_PKEY_derive_set_peer(ctx, a) == 0);
	CHECK(one_error(CRYC_R_EVP_DIFFERENT_KEY_TYPES));
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(x25519);
	ctx = EVP_PKEY_CTX_new(a, NULL);
	CHECK(EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == -2);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));
	CHECK(EVP_PKEY_verify(ctx, sig, sizeof(sig), shared_a, 32U) == -1);
	CHECK(one_error(CRYC_R_EVP_OPERATION_NOT_INITIALIZED));
	CHECK(EVP_PKEY_verify_init(ctx) == 1);
	CHECK(EVP_PKEY_CTX_set_signature_md(ctx, (const EVP_MD *)&length) == 0);
	CHECK(one_error(CRYC_R_EVP_UNSUPPORTED_ALGORITHM));
	CHECK(EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1);
	CHECK(EVP_PKEY_verify(ctx, sig, sizeof(sig), shared_a, 31U) == 0);
	CHECK(no_error());
	memset(sig, 0, sizeof(sig));
	CHECK(EVP_PKEY_verify(ctx, sig, sizeof(sig), shared_a, 32U) == -1);
	CHECK(one_error(CRYC_R_ASN1_WRONG_TAG));
	CHECK(EVP_PKEY_verify(ctx, NULL, 0U, shared_a, 32U) == -1);
	CHECK(one_error(CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(EVP_PKEY_public_check(ctx) == 1);
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(a);
	CHECK(no_error());
}

/* Runs every check. */
int
main(void)
{
	/* The AES inverse cipher and AES-256-CBC. */
	check_aes_inverse();
	check_aes_kat("cavp-KAT_AES-ECBGFSbox128", 0);
	check_aes_kat("cavp-KAT_AES-ECBKeySbox192", 0);
	check_aes_kat("cavp-KAT_AES-ECBVarKey256", 0);
	check_aes_kat("cavp-KAT_AES-ECBVarTxt128", 0);
	check_aes_kat("cavp-KAT_AES-ECBVarTxt192", 0);
	check_aes_kat("cavp-KAT_AES-ECBVarTxt256", 0);
	check_aes_kat("cavp-KAT_AES-CBCGFSbox256", 1);
	check_aes_kat("cavp-KAT_AES-CBCKeySbox256", 1);
	check_aes_kat("cavp-KAT_AES-CBCVarKey256", 1);
	check_aes_kat("cavp-KAT_AES-CBCVarTxt256", 1);
	check_cbc();

	/* HMAC, HKDF and the parameters. */
	check_hmac_rfc();
	check_hmac_wycheproof();
	check_params();
	check_hkdf();

	/* The P-256 keys. */
	check_pkv();
	check_ecdsa();
	check_ecdh();
	check_keygen();

	return check_finish("test-api-passkey");
}
