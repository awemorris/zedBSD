/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The contract ssh, sshd and ssh-keygen expect of the crypto library: each
 * library call ssh-crypto.c makes, with the arguments it passes, gives the
 * answer the specifications give.  Built against the host's OpenSSL 3.5
 * it confirms the assumptions of the design; built against libcrypto-compat
 * it is that library's acceptance for the calls ssh makes.  Expected values
 * are the specifications' test vectors (RFC 8439 2.4.2 and 2.5.2, RFC 8032
 * 7.1 TEST 1, RFC 7748 6.1, FIPS 180-2 B.1), or properties (an ML-KEM-768
 * round trip, the refusal of a bad encapsulation key, implicit rejection).
 */

#include <stdint.h>
#include <string.h>

#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>

#include "check.h"
#include "wire.h"

static void check_random(void);
static void check_sha256(void);
static void check_chacha20(void);
static void check_poly1305(void);
static void check_ed25519(void);
static void check_x25519(void);
static void check_mlkem768(void);
static void check_wipe_compare(void);

/*
 * Runs every check of the contract.
 */
int
main(void)
{
	/* Each primitive in turn. */
	check_random();
	check_sha256();
	check_chacha20();
	check_poly1305();
	check_ed25519();
	check_x25519();
	check_mlkem768();
	check_wipe_compare();

	/* Reports the whole contract. */
	return check_finish("crypto-contract");
}

/* RAND_bytes fills the buffer and two draws differ. */
static void
check_random(void)
{
	uint8_t first[32];
	uint8_t second[32];

	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));

	/* Two draws of 32 bytes. */
	CHECK_EQ(RAND_bytes(first, (int)sizeof(first)), 1);
	CHECK_EQ(RAND_bytes(second, (int)sizeof(second)), 1);

	/* They differ (a failure here has probability 2^-256). */
	CHECK(memcmp(first, second, sizeof(first)) != 0);
}

/* SHA-256 of "abc" (FIPS 180-2 appendix B.1), one-shot and in pieces. */
static void
check_sha256(void)
{
	uint8_t expected[32];
	uint8_t digest[32];
	unsigned int digest_size;
	EVP_MD_CTX *context;

	/* The digest of FIPS 180-2 B.1. */
	CHECK_EQ(wire_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, sizeof(expected)), 32);

	/* One call. */
	digest_size = 0;
	CHECK_EQ(EVP_Digest("abc", 3, digest, &digest_size, EVP_sha256(), NULL), 1);
	CHECK_EQ(digest_size, 32);
	CHECK_BYTES(digest, expected, 32);

	/* In two pieces through a context. */
	context = EVP_MD_CTX_new();
	CHECK(context != NULL);
	if (context == NULL)
		return;
	CHECK_EQ(EVP_DigestInit_ex(context, EVP_sha256(), NULL), 1);
	CHECK_EQ(EVP_DigestUpdate(context, "a", 1), 1);
	CHECK_EQ(EVP_DigestUpdate(context, "bc", 2), 1);
	CHECK_EQ(EVP_DigestFinal_ex(context, digest, &digest_size), 1);
	CHECK_BYTES(digest, expected, 32);
	EVP_MD_CTX_free(context);
}

/*
 * ChaCha20 with the 16-byte IV that ssh passes (8 little-endian counter
 * bytes, then the 8-byte nonce): RFC 8439 2.4.2's vector, whose state has
 * word 12 = 1, word 13 = 0, word 14 = 0x4a000000, word 15 = 0, which is
 * the IV 01 00 00 00 00 00 00 00 | 00 00 00 4a 00 00 00 00.
 */
static void
check_chacha20(void)
{
	static const char plaintext[] =
	    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t expected[114];
	uint8_t output[114];
	uint8_t again[114];
	int written;
	EVP_CIPHER_CTX *context;

	/* The key 00 01 ... 1f, the IV and the ciphertext of 2.4.2. */
	CHECK_EQ(wire_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("0100000000000000 000000 4a 00000000", iv, sizeof(iv)), 16);
	CHECK_EQ(wire_hex(
	    "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
	    "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
	    "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
	    "5af90bbf74a35be6b40b8eedf2785e42874d", expected, sizeof(expected)), 114);
	CHECK_EQ(sizeof(plaintext) - 1, 114);

	/* Encrypts the plaintext. */
	context = EVP_CIPHER_CTX_new();
	CHECK(context != NULL);
	if (context == NULL)
		return;
	CHECK_EQ(EVP_CipherInit_ex(context, EVP_chacha20(), NULL, key, iv, 1), 1);
	written = 0;
	CHECK_EQ(EVP_CipherUpdate(context, output, &written, (const uint8_t *)plaintext, 114), 1);
	CHECK_EQ(written, 114);
	CHECK_BYTES(output, expected, 114);

	/* A new IV on the same context (as ssh does for every packet) starts the stream again. */
	CHECK_EQ(EVP_CipherInit_ex(context, NULL, NULL, NULL, iv, -1), 1);
	CHECK_EQ(EVP_CipherUpdate(context, again, &written, (const uint8_t *)plaintext, 114), 1);
	CHECK_BYTES(again, expected, 114);
	EVP_CIPHER_CTX_free(context);
}

/* Poly1305 through EVP_MAC "POLY1305": RFC 8439 2.5.2, and a re-init with a new key on the same context. */
static void
check_poly1305(void)
{
	static const char message[] = "Cryptographic Forum Research Group";
	uint8_t key[32];
	uint8_t expected[16];
	uint8_t tag[16];
	size_t tag_size;
	EVP_MAC *mac;
	EVP_MAC_CTX *context;

	/* The key and the tag of 2.5.2. */
	CHECK_EQ(wire_hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("a8061dc1305136c6c22b8baf0c0127a9", expected, sizeof(expected)), 16);

	/* Fetches the MAC and makes a context. */
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	CHECK(mac != NULL);
	if (mac == NULL)
		return;
	context = EVP_MAC_CTX_new(mac);
	CHECK(context != NULL);
	if (context == NULL) {
		EVP_MAC_free(mac);
		return;
	}

	/* The tag of the vector, twice on the same context. */
	tag_size = 0;
	CHECK_EQ(EVP_MAC_init(context, key, 32, NULL), 1);
	CHECK_EQ(EVP_MAC_update(context, (const uint8_t *)message, 34), 1);
	CHECK_EQ(EVP_MAC_final(context, tag, &tag_size, sizeof(tag)), 1);
	CHECK_EQ(tag_size, 16);
	CHECK_BYTES(tag, expected, 16);
	CHECK_EQ(EVP_MAC_init(context, key, 32, NULL), 1);
	CHECK_EQ(EVP_MAC_update(context, (const uint8_t *)message, 34), 1);
	CHECK_EQ(EVP_MAC_final(context, tag, &tag_size, sizeof(tag)), 1);
	CHECK_BYTES(tag, expected, 16);

	/* Frees them. */
	EVP_MAC_CTX_free(context);
	EVP_MAC_free(mac);
}

/* Ed25519 through raw keys and EVP_DigestSign/Verify: RFC 8032 7.1 TEST 1, and a rejected forgery. */
static void
check_ed25519(void)
{
	uint8_t seed[32];
	uint8_t expected_public[32];
	uint8_t expected_signature[64];
	uint8_t public_key[32];
	uint8_t signature[64];
	size_t size;
	EVP_PKEY *private_key;
	EVP_PKEY *verify_key;
	EVP_MD_CTX *context;

	/* TEST 1's secret key, public key and signature of the empty message. */
	CHECK_EQ(wire_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, sizeof(seed)), 32);
	CHECK_EQ(wire_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", expected_public, 32), 32);
	CHECK_EQ(wire_hex(
	    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b"
	    "d25bf5f0595bbe24655141438e7a100b", expected_signature, 64), 64);

	/* The public key of the seed. */
	private_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
	CHECK(private_key != NULL);
	if (private_key == NULL)
		return;
	size = sizeof(public_key);
	CHECK_EQ(EVP_PKEY_get_raw_public_key(private_key, public_key, &size), 1);
	CHECK_EQ(size, 32);
	CHECK_BYTES(public_key, expected_public, 32);

	/* The signature of the empty message. */
	context = EVP_MD_CTX_new();
	CHECK(context != NULL);
	if (context == NULL) {
		EVP_PKEY_free(private_key);
		return;
	}
	CHECK_EQ(EVP_DigestSignInit(context, NULL, NULL, NULL, private_key), 1);
	size = sizeof(signature);
	CHECK_EQ(EVP_DigestSign(context, signature, &size, (const uint8_t *)"", 0), 1);
	CHECK_EQ(size, 64);
	CHECK_BYTES(signature, expected_signature, 64);
	EVP_MD_CTX_free(context);
	EVP_PKEY_free(private_key);

	/* Verifies it, then a changed one. */
	verify_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, expected_public, 32);
	CHECK(verify_key != NULL);
	if (verify_key == NULL)
		return;
	context = EVP_MD_CTX_new();
	CHECK_EQ(EVP_DigestVerifyInit(context, NULL, NULL, NULL, verify_key), 1);
	CHECK_EQ(EVP_DigestVerify(context, expected_signature, 64, (const uint8_t *)"", 0), 1);
	EVP_MD_CTX_free(context);
	signature[10] ^= 0x01;
	context = EVP_MD_CTX_new();
	CHECK_EQ(EVP_DigestVerifyInit(context, NULL, NULL, NULL, verify_key), 1);
	CHECK(EVP_DigestVerify(context, signature, 64, (const uint8_t *)"", 0) != 1);
	EVP_MD_CTX_free(context);
	EVP_PKEY_free(verify_key);
}

/* X25519 through raw keys and EVP_PKEY_derive: RFC 7748 6.1, and the refusal of an all-zero result. */
static void
check_x25519(void)
{
	uint8_t alice_private[32];
	uint8_t alice_public[32];
	uint8_t bob_public[32];
	uint8_t expected_shared[32];
	uint8_t public_key[32];
	uint8_t shared[32];
	uint8_t zero_point[32];
	size_t size;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *context;

	/* Alice's private and public keys, Bob's public key, the shared secret of 6.1. */
	CHECK_EQ(wire_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_private, 32), 32);
	CHECK_EQ(wire_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alice_public, 32), 32);
	CHECK_EQ(wire_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bob_public, 32), 32);
	CHECK_EQ(wire_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", expected_shared, 32), 32);

	/* Alice's public key from her private key. */
	own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, alice_private, 32);
	CHECK(own != NULL);
	if (own == NULL)
		return;
	size = sizeof(public_key);
	CHECK_EQ(EVP_PKEY_get_raw_public_key(own, public_key, &size), 1);
	CHECK_BYTES(public_key, alice_public, 32);

	/* The shared secret with Bob. */
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, bob_public, 32);
	CHECK(peer != NULL);
	context = EVP_PKEY_CTX_new(own, NULL);
	CHECK(context != NULL);
	if (peer == NULL || context == NULL) {
		EVP_PKEY_free(peer);
		EVP_PKEY_CTX_free(context);
		EVP_PKEY_free(own);
		return;
	}
	CHECK_EQ(EVP_PKEY_derive_init(context), 1);
	CHECK_EQ(EVP_PKEY_derive_set_peer(context, peer), 1);
	size = sizeof(shared);
	CHECK_EQ(EVP_PKEY_derive(context, shared, &size), 1);
	CHECK_EQ(size, 32);
	CHECK_BYTES(shared, expected_shared, 32);
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(peer);

	/* The point 0 (of small order) gives an all-zero secret, which must be refused. */
	memset(zero_point, 0, sizeof(zero_point));
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, zero_point, 32);
	context = EVP_PKEY_CTX_new(own, NULL);
	if (peer != NULL && context != NULL && EVP_PKEY_derive_init(context) == 1 && EVP_PKEY_derive_set_peer(context, peer) == 1) {
		size = sizeof(shared);
		CHECK(EVP_PKEY_derive(context, shared, &size) != 1);
	}
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(own);
}

/*
 * ML-KEM-768 as ssh uses it: key generation by name, the raw
 * encapsulation key (1184 bytes), its import by name with the FIPS 203 7.2
 * check, encapsulation (1088-byte ciphertext, 32-byte secret),
 * decapsulation to the same secret, implicit rejection of a changed
 * ciphertext, and the refusal of an encapsulation key whose first
 * coefficient is not reduced.
 */
static void
check_mlkem768(void)
{
	static uint8_t encapsulation_key[1184];
	static uint8_t ciphertext[1088];
	uint8_t secret[32];
	uint8_t decapsulated[32];
	size_t key_size;
	size_t ciphertext_size;
	size_t secret_size;
	EVP_PKEY_CTX *generator;
	EVP_PKEY *pair;
	EVP_PKEY *public_key;
	EVP_PKEY *bad_key;
	EVP_PKEY_CTX *encapsulator;
	EVP_PKEY_CTX *decapsulator;

	pair = NULL;

	/* Makes a key pair. */
	generator = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
	CHECK(generator != NULL);
	if (generator == NULL)
		return;
	CHECK_EQ(EVP_PKEY_keygen_init(generator), 1);
	CHECK_EQ(EVP_PKEY_keygen(generator, &pair), 1);
	EVP_PKEY_CTX_free(generator);
	if (pair == NULL)
		return;

	/* Its encapsulation key. */
	key_size = sizeof(encapsulation_key);
	CHECK_EQ(EVP_PKEY_get_raw_public_key(pair, encapsulation_key, &key_size), 1);
	CHECK_EQ(key_size, 1184);

	/* The other side imports it and encapsulates. */
	public_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, encapsulation_key, key_size);
	CHECK(public_key != NULL);
	if (public_key == NULL) {
		EVP_PKEY_free(pair);
		return;
	}
	encapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, public_key, NULL);
	CHECK(encapsulator != NULL);
	CHECK_EQ(EVP_PKEY_encapsulate_init(encapsulator, NULL), 1);
	ciphertext_size = sizeof(ciphertext);
	secret_size = sizeof(secret);
	CHECK_EQ(EVP_PKEY_encapsulate(encapsulator, ciphertext, &ciphertext_size, secret, &secret_size), 1);
	CHECK_EQ(ciphertext_size, 1088);
	CHECK_EQ(secret_size, 32);
	EVP_PKEY_CTX_free(encapsulator);
	EVP_PKEY_free(public_key);

	/* Decapsulation gives the same secret. */
	decapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, pair, NULL);
	CHECK(decapsulator != NULL);
	CHECK_EQ(EVP_PKEY_decapsulate_init(decapsulator, NULL), 1);
	secret_size = sizeof(decapsulated);
	CHECK_EQ(EVP_PKEY_decapsulate(decapsulator, decapsulated, &secret_size, ciphertext, ciphertext_size), 1);
	CHECK_EQ(secret_size, 32);
	CHECK_BYTES(decapsulated, secret, 32);

	/* A changed ciphertext still decapsulates (implicit rejection), to another secret. */
	ciphertext[100] ^= 0x01;
	secret_size = sizeof(decapsulated);
	CHECK_EQ(EVP_PKEY_decapsulate(decapsulator, decapsulated, &secret_size, ciphertext, ciphertext_size), 1);
	CHECK(memcmp(decapsulated, secret, 32) != 0);
	EVP_PKEY_CTX_free(decapsulator);
	EVP_PKEY_free(pair);

	/*
	 * An encapsulation key whose first 12-bit coefficient is 4095 (not
	 * below q = 3329) fails the modulus check of FIPS 203 7.2.
	 */
	encapsulation_key[0] = 0xff;
	encapsulation_key[1] = (uint8_t)(encapsulation_key[1] | 0x0f);
	bad_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, encapsulation_key, key_size);
	CHECK(bad_key == NULL);
	EVP_PKEY_free(bad_key);

	/* A wrong length is refused too. */
	bad_key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, encapsulation_key, key_size - 1);
	CHECK(bad_key == NULL);
	EVP_PKEY_free(bad_key);
}

/* CRYPTO_memcmp answers 0 only for equal bytes; OPENSSL_cleanse zeroes. */
static void
check_wipe_compare(void)
{
	uint8_t left[16];
	uint8_t right[16];
	unsigned index;

	/* Two equal arrays, then one byte apart. */
	memset(left, 0x5a, sizeof(left));
	memset(right, 0x5a, sizeof(right));
	CHECK_EQ(CRYPTO_memcmp(left, right, sizeof(left)), 0);
	right[15] = 0x5b;
	CHECK(CRYPTO_memcmp(left, right, sizeof(left)) != 0);

	/* Wipes one and checks every byte. */
	OPENSSL_cleanse(left, sizeof(left));
	for (index = 0; index < sizeof(left); index++)
		CHECK_EQ(left[index], 0);
}
