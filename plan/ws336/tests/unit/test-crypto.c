/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-crypto.c.  Expected values are the specifications' test
 * vectors: FIPS 180-2 B.1 to B.3 (SHA-256), RFC 8032 7.1 TEST 1 to 3
 * (Ed25519), RFC 8439 2.4.2 (ChaCha20), RFC 8439 2.5.2 and A.3 (Poly1305),
 * and the definitions of the hybrid key exchange (draft-ietf-sshm-mlkem-
 * hybrid-kex-10 2.4: K = SHA-256(K_PQ || K_CL)) and of the ML-KEM-768
 * encapsulation key check (FIPS 203 7.2).  The hybrid key exchange's
 * shared secret is recomputed here from the library's own calls, apart
 * from the module.  The crypto library is the host's OpenSSL 3.5 (or
 * libcrypto-compat); the module only calls it.  Every library object a
 * failing call made is freed (the leak check of the sanitizer).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>

#include "check.h"
#include "wire.h"
#include "ssh-crypto.h"

/* The largest message of the Ed25519 vectors used here, in bytes. */
#define TEST_MESSAGE_MAX	16U

/* One Ed25519 test vector of RFC 8032 7.1, as hexadecimal text. */
struct ed25519_vector {
	const char *seed;
	const char *public_key;
	const char *message;
	const char *signature;
};

/*
 * The vectors TEST 1, TEST 2 and TEST 3 of RFC 8032 7.1: the secret key
 * (seed), the public key, the message (empty, one byte, two bytes) and the
 * signature.
 *
 * Values referenced: RFC 8032 section 7.1, TEST 1, TEST 2 and TEST 3 (SECRET
 * KEY, PUBLIC KEY, MESSAGE, SIGNATURE).
 */
static const struct ed25519_vector ed25519_vectors[3] = {
	{
		"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
		"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
		"",
		"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015"
		"55fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
	},
	{
		"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
		"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
		"72",
		"92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
		"085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
	},
	{
		"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
		"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
		"af82",
		"6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
		"18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
	}
};

/*
 * The group order L of Ed25519 (RFC 8032 5.1) as 32 little-endian bytes,
 * 2^252 + 27742317777372353535851937790883648493.  Adding it to the S half
 * of a signature gives an S that is not below L, which RFC 8032 5.1.7
 * requires a verifier to reject.
 *
 * Values referenced: RFC 8032 section 5.1, the definition of L.
 */
static const uint8_t ed25519_order[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

/* One ChaCha20 block of the 64-bit-counter form: a key, a block counter, an 8-byte nonce and the 64 bytes of keystream, as hexadecimal text. */
struct chacha20_vector {
	const char *key;
	unsigned long counter;
	const char *nonce;
	const char *keystream;
};

/*
 * The block-function vectors of RFC 8439 A.1 whose 12-byte nonce has a zero
 * first word, so that they are also vectors of the 64-bit-counter form (the
 * first nonce word is the high half of the counter): the nonce here is the
 * last eight bytes of the RFC's nonce.  They cover a nonce byte that is not
 * zero in its last place (#5), a key bit in each end (#3, #4) and the
 * counters 0, 1 and 2.
 *
 * Values referenced: RFC 8439 appendix A.1, Test Vectors #1, #2, #3, #4 and
 * #5 (Key, Nonce, Block Counter, Keystream).
 */
static const struct chacha20_vector chacha20_vectors[5] = {
	{
		"0000000000000000000000000000000000000000000000000000000000000000", 0, "0000000000000000",
		"76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
		"da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586"
	},
	{
		"0000000000000000000000000000000000000000000000000000000000000000", 1, "0000000000000000",
		"9f07e7be5551387a98ba977c732d080dcb0f29a048e3656912c6533e32ee7aed"
		"29b721769ce64e43d57133b074d839d531ed1f28510afb45ace10a1f4b794d6f"
	},
	{
		"0000000000000000000000000000000000000000000000000000000000000001", 1, "0000000000000000",
		"3aeb5224ecf849929b9d828db1ced4dd832025e8018b8160b82284f3c949aa5a"
		"8eca00bbb4a73bdad192b5c42f73f2fd4e273644c8b36125a64addeb006c13a0"
	},
	{
		"00ff000000000000000000000000000000000000000000000000000000000000", 2, "0000000000000000",
		"72d54dfbf12ec44b362692df94137f328fea8da73990265ec1bbbea1ae9af0ca"
		"13b25aa26cb4a648cb9b9d1be65b2c0924a66c54d545ec1b7374f4872e99f096"
	},
	{
		"0000000000000000000000000000000000000000000000000000000000000000", 0, "0000000000000002",
		"c2c64d378cd536374ae204b9ef933fcd1a8b2288b3dfa49672ab765b54ee27c7"
		"8a970e0e955c14f3a88e741b97c286f75f8fc299e8148362fa198a39531bed6d"
	}
};

/* The C_INIT of one key exchange, with one spare byte so a length of one more can be passed. */
static uint8_t c_init[SSH_HYBRID_INIT_SIZE + 1U];

/* The S_REPLY of one key exchange, with one spare byte so a length of one more can be passed. */
static uint8_t s_reply[SSH_HYBRID_REPLY_SIZE + 1U];

/* A copy of the C_INIT or the S_REPLY, to be changed for a check (sized for the longer, C_INIT, plus one byte). */
static uint8_t changed[SSH_HYBRID_INIT_SIZE + 1U];

/* The five encodings of the points of small order that give an all-zero X25519 result (0, 1, p - 1, p, p + 1). */
static uint8_t small_order_points[5][SSH_X25519_SIZE];

static void test_sha256(void);
static void test_ed25519(void);
static void test_hybrid(void);
static void test_hybrid_server_refusals(void);
static void test_hybrid_client_refusals(void);
static void test_chacha20(void);
static void test_poly1305(void);
static void test_misc(void);
static void make_small_order_points(void);
static void start_client(struct ssh_hybrid_client *client, uint8_t *init);
static int recompute_k(const struct ssh_hybrid_client *client, const uint8_t *reply, uint8_t *k);
static int all_zero(const uint8_t *data, size_t size);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Builds the points of small order the hybrid checks use. */
	make_small_order_points();

	/* Runs each group of checks. */
	test_sha256();
	test_ed25519();
	test_hybrid();
	test_hybrid_server_refusals();
	test_hybrid_client_refusals();
	test_chacha20();
	test_poly1305();
	test_misc();

	/* Reports the result of the whole test. */
	return check_finish("crypto");
}

/*
 * Checks SHA-256 against FIPS 180-2 B.1 to B.3, in one call and in pieces,
 * and the states of the structure.
 *
 * Values referenced: FIPS 180-2 appendix B.1 ("abc"), B.2 (the 448-bit
 * message) and B.3 (one million 'a'); the digest of the empty message from
 * FIPS 180-4's examples.
 */
static void
test_sha256(void)
{
	static uint8_t block[1000];
	uint8_t expected[SSH_SHA256_SIZE];
	uint8_t digest[SSH_SHA256_SIZE];
	struct ssh_sha256 hash;
	struct ssh_sha256 empty;
	size_t index;

	/* B.1: the digest of "abc", in one call. */
	CHECK_EQ(wire_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, sizeof(expected)), 32);
	CHECK_EQ(ssh_sha256("abc", 3, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* The same digest in two pieces. */
	memset(digest, 0, sizeof(digest));
	CHECK_EQ(ssh_sha256_init(&hash), 0);
	CHECK(hash.context != NULL);
	CHECK_EQ(ssh_sha256_update(&hash, "a", 1), 0);
	CHECK_EQ(ssh_sha256_update(&hash, "bc", 2), 0);
	CHECK_EQ(ssh_sha256_final(&hash, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* The computation is freed by the final call: the structure holds nothing. */
	CHECK(hash.context == NULL);

	/* The digest of the empty message, with a NULL pointer and with a pointer. */
	CHECK_EQ(wire_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, sizeof(expected)), 32);
	CHECK_EQ(ssh_sha256(NULL, 0, digest), 0);
	CHECK_BYTES(digest, expected, 32);
	CHECK_EQ(ssh_sha256("", 0, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* B.2: the 448-bit message. */
	CHECK_EQ(wire_hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", expected, sizeof(expected)), 32);
	CHECK_EQ(ssh_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* B.3: one million 'a', in a thousand pieces. */
	CHECK_EQ(wire_hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", expected, sizeof(expected)), 32);
	memset(block, 'a', sizeof(block));
	CHECK_EQ(ssh_sha256_init(&hash), 0);
	for (index = 0; index < 1000; index++)
		CHECK_EQ(ssh_sha256_update(&hash, block, sizeof(block)), 0);
	CHECK_EQ(ssh_sha256_final(&hash, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* An empty update of a NULL pointer changes nothing. */
	CHECK_EQ(wire_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, sizeof(expected)), 32);
	CHECK_EQ(ssh_sha256_init(&hash), 0);
	CHECK_EQ(ssh_sha256_update(&hash, NULL, 0), 0);
	CHECK_EQ(ssh_sha256_update(&hash, "abc", 3), 0);
	CHECK_EQ(ssh_sha256_update(&hash, NULL, 0), 0);
	CHECK_EQ(ssh_sha256_final(&hash, digest), 0);
	CHECK_BYTES(digest, expected, 32);

	/* An abandoned computation is freed and the structure holds nothing; freeing it again is harmless. */
	CHECK_EQ(ssh_sha256_init(&hash), 0);
	CHECK_EQ(ssh_sha256_update(&hash, "abc", 3), 0);
	ssh_sha256_free(&hash);
	CHECK(hash.context == NULL);
	ssh_sha256_free(&hash);
	CHECK(hash.context == NULL);

	/* A zero structure holds no computation: update and final answer EIO, and final wipes the output. */
	memset(&empty, 0, sizeof(empty));
	CHECK_EQ(ssh_sha256_update(&empty, "abc", 3), EIO);
	memset(digest, 0xaa, sizeof(digest));
	CHECK_EQ(ssh_sha256_final(&empty, digest), EIO);
	CHECK(all_zero(digest, sizeof(digest)));
	ssh_sha256_free(&empty);
}

/*
 * Checks Ed25519 against RFC 8032 7.1 TEST 1 to 3: the public key of the
 * seed, the signature, the verification, and the refusal of a changed
 * signature, message and public key.
 */
static void
test_ed25519(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t message[TEST_MESSAGE_MAX];
	uint8_t signature[64];
	uint8_t expected_public[32];
	uint8_t expected_signature[64];
	uint8_t altered[64];
	uint8_t altered_key[32];
	size_t message_size;
	size_t index;
	size_t position;
	int carry;
	int sum;

	/* Each vector in turn. */
	for (index = 0; index < 3; index++) {
		CHECK_EQ(wire_hex(ed25519_vectors[index].seed, seed, sizeof(seed)), 32);
		CHECK_EQ(wire_hex(ed25519_vectors[index].public_key, expected_public, sizeof(expected_public)), 32);
		CHECK_EQ(wire_hex(ed25519_vectors[index].signature, expected_signature, sizeof(expected_signature)), 64);
		message_size = strlen(ed25519_vectors[index].message) / 2;
		if (message_size > 0)
			CHECK_EQ(wire_hex(ed25519_vectors[index].message, message, sizeof(message)), (long)message_size);

		/* The public key of the seed. */
		memset(public_key, 0, sizeof(public_key));
		CHECK_EQ(ssh_ed25519_public(seed, public_key), 0);
		CHECK_BYTES(public_key, expected_public, 32);

		/* The signature of the message. */
		memset(signature, 0, sizeof(signature));
		CHECK_EQ(ssh_ed25519_sign(seed, message_size > 0 ? message : NULL, message_size, signature), 0);
		CHECK_BYTES(signature, expected_signature, 64);

		/* The verification of the signature of the specification. */
		CHECK_EQ(ssh_ed25519_verify(expected_public, message_size > 0 ? message : NULL, message_size, expected_signature), 0);

		/* A 1-bit change in the first, a middle and the last byte of the signature. */
		memcpy(altered, expected_signature, 64);
		altered[0] ^= 0x01;
		CHECK_EQ(ssh_ed25519_verify(expected_public, message, message_size, altered), EBADMSG);
		memcpy(altered, expected_signature, 64);
		altered[32] ^= 0x80;
		CHECK_EQ(ssh_ed25519_verify(expected_public, message, message_size, altered), EBADMSG);
		memcpy(altered, expected_signature, 64);
		altered[63] ^= 0x01;
		CHECK_EQ(ssh_ed25519_verify(expected_public, message, message_size, altered), EBADMSG);

		/* A 1-bit change in the public key. */
		memcpy(altered_key, expected_public, 32);
		altered_key[5] ^= 0x04;
		CHECK_EQ(ssh_ed25519_verify(altered_key, message, message_size, expected_signature), EBADMSG);

		/* A 1-bit change in the message (for the empty message, a message of one byte). */
		if (message_size > 0) {
			message[message_size - 1] ^= 0x01;
			CHECK_EQ(ssh_ed25519_verify(expected_public, message, message_size, expected_signature), EBADMSG);
			message[message_size - 1] ^= 0x01;
		} else {
			message[0] = 0;
			CHECK_EQ(ssh_ed25519_verify(expected_public, message, 1, expected_signature), EBADMSG);
		}

		/* The S half plus the group order is the same value mod L but is not below L: refused. */
		memcpy(altered, expected_signature, 64);
		carry = 0;
		for (position = 0; position < 32; position++) {
			sum = (int)altered[32 + position] + (int)ed25519_order[position] + carry;
			altered[32 + position] = (uint8_t)(sum & 0xff);
			carry = sum >> 8;
		}
		CHECK_EQ(carry, 0);
		CHECK_EQ(ssh_ed25519_verify(expected_public, message, message_size, altered), EBADMSG);
	}

	/* A signature of another message is refused: TEST 2's signature under TEST 3's key. */
	CHECK_EQ(wire_hex(ed25519_vectors[2].public_key, expected_public, sizeof(expected_public)), 32);
	CHECK_EQ(wire_hex(ed25519_vectors[1].signature, expected_signature, sizeof(expected_signature)), 64);
	CHECK_EQ(wire_hex("72", message, sizeof(message)), 1);
	CHECK_EQ(ssh_ed25519_verify(expected_public, message, 1, expected_signature), EBADMSG);

	/* A public key whose encoding does not decode to a point and an R that does not decode are refused (EBADMSG or EINVAL, never success). */
	memset(altered_key, 0xff, sizeof(altered_key));
	CHECK_EQ(wire_hex(ed25519_vectors[0].signature, expected_signature, sizeof(expected_signature)), 64);
	CHECK(ssh_ed25519_verify(altered_key, NULL, 0, expected_signature) != 0);
	memset(altered, 0xff, 32);
	memcpy(altered + 32, expected_signature + 32, 32);
	CHECK_EQ(wire_hex(ed25519_vectors[0].public_key, expected_public, sizeof(expected_public)), 32);
	CHECK_EQ(ssh_ed25519_verify(expected_public, NULL, 0, altered), EBADMSG);

	/* Signing the same message twice gives the same signature (Ed25519 is deterministic), a large message works too. */
	CHECK_EQ(wire_hex(ed25519_vectors[0].seed, seed, sizeof(seed)), 32);
	CHECK_EQ(ssh_ed25519_public(seed, public_key), 0);
	CHECK_EQ(ssh_ed25519_sign(seed, (const uint8_t *)changed, sizeof(changed), signature), 0);
	CHECK_EQ(ssh_ed25519_sign(seed, (const uint8_t *)changed, sizeof(changed), altered), 0);
	CHECK_BYTES(signature, altered, 64);
	CHECK_EQ(ssh_ed25519_verify(public_key, (const uint8_t *)changed, sizeof(changed), signature), 0);
}

/*
 * Checks a whole hybrid key exchange: both sides get the same K, which is
 * SHA-256(K_PQ || K_CL) recomputed apart from the module; two exchanges
 * differ.
 */
static void
test_hybrid(void)
{
	struct ssh_hybrid_client client;
	struct ssh_hybrid_client second;
	uint8_t k_server[SSH_SHA256_SIZE];
	uint8_t k_client[SSH_SHA256_SIZE];
	uint8_t k_expected[SSH_SHA256_SIZE];
	uint8_t first_init[SSH_HYBRID_INIT_SIZE];
	uint8_t zero_private[SSH_X25519_SIZE];

	memset(zero_private, 0, sizeof(zero_private));

	/* The client starts: C_INIT is written, the state holds the key pair. */
	start_client(&client, c_init);
	CHECK(client.mlkem != NULL);
	CHECK(!all_zero(client.x25519_private, sizeof(client.x25519_private)));

	/* The server answers. */
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);

	/* K is SHA-256(K_PQ || K_CL), recomputed from the library's calls with the client's keys. */
	CHECK_EQ(recompute_k(&client, s_reply, k_expected), 0);
	CHECK_BYTES(k_server, k_expected, SSH_SHA256_SIZE);

	/* The client finishes with the same K, and its state is wiped and freed. */
	CHECK_EQ(ssh_hybrid_client_finish(&client, s_reply, SSH_HYBRID_REPLY_SIZE, k_client), 0);
	CHECK_BYTES(k_client, k_server, SSH_SHA256_SIZE);
	CHECK(client.mlkem == NULL);
	CHECK(all_zero(client.x25519_private, sizeof(client.x25519_private)));
	CHECK(!all_zero(k_client, sizeof(k_client)));

	/* A second exchange uses new keys: C_INIT and K differ from the first. */
	memcpy(first_init, c_init, SSH_HYBRID_INIT_SIZE);
	start_client(&second, c_init);
	CHECK(memcmp(first_init, c_init, SSH_HYBRID_INIT_SIZE) != 0);
	CHECK(memcmp(first_init, c_init, SSH_MLKEM768_PUBLIC) != 0);
	CHECK(memcmp(first_init + SSH_MLKEM768_PUBLIC, c_init + SSH_MLKEM768_PUBLIC, SSH_X25519_SIZE) != 0);
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
	CHECK_EQ(ssh_hybrid_client_finish(&second, s_reply, SSH_HYBRID_REPLY_SIZE, k_client), 0);
	CHECK_BYTES(k_client, k_server, SSH_SHA256_SIZE);

	/* A server answering the same C_INIT twice makes a different reply and K (a new key pair each time). */
	start_client(&second, c_init);
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, changed, k_expected), 0);
	CHECK(memcmp(s_reply, changed, SSH_HYBRID_REPLY_SIZE) != 0);
	CHECK(memcmp(k_server, k_expected, SSH_SHA256_SIZE) != 0);

	/* A client half that is not finished is cleared: nothing is left, and clearing again is harmless. */
	ssh_hybrid_client_clear(&second);
	CHECK(second.mlkem == NULL);
	CHECK(all_zero(second.x25519_private, sizeof(second.x25519_private)));
	ssh_hybrid_client_clear(&second);
}

/*
 * Checks the refusals of the server: a C_INIT of a wrong length, an
 * encapsulation key that fails the check of FIPS 203 7.2, and an X25519
 * point of small order.
 */
static void
test_hybrid_server_refusals(void)
{
	struct ssh_hybrid_client client;
	uint8_t k[SSH_SHA256_SIZE];
	size_t index;

	/* A good C_INIT to change. */
	start_client(&client, c_init);

	/* The length minus one and plus one: EINVAL, with the reply and K wiped. */
	memset(s_reply, 0xaa, sizeof(s_reply));
	memset(k, 0xaa, sizeof(k));
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE - 1U, s_reply, k), EINVAL);
	CHECK(all_zero(s_reply, SSH_HYBRID_REPLY_SIZE));
	CHECK(all_zero(k, sizeof(k)));
	memset(s_reply, 0xaa, sizeof(s_reply));
	memset(k, 0xaa, sizeof(k));
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE + 1U, s_reply, k), EINVAL);
	CHECK(all_zero(s_reply, SSH_HYBRID_REPLY_SIZE));
	CHECK(all_zero(k, sizeof(k)));

	/* Length 0 is refused too, with a NULL pointer. */
	CHECK_EQ(ssh_hybrid_server(NULL, 0, s_reply, k), EINVAL);

	/* The first 12-bit coefficient of the encapsulation key set to 4095, not below q = 3329 (FIPS 203 7.2): EINVAL, outputs wiped. */
	memcpy(changed, c_init, SSH_HYBRID_INIT_SIZE);
	changed[0] = 0xff;
	changed[1] = (uint8_t)(changed[1] | 0x0f);
	memset(s_reply, 0xaa, sizeof(s_reply));
	memset(k, 0xaa, sizeof(k));
	CHECK_EQ(ssh_hybrid_server(changed, SSH_HYBRID_INIT_SIZE, s_reply, k), EINVAL);
	CHECK(all_zero(s_reply, SSH_HYBRID_REPLY_SIZE));
	CHECK(all_zero(k, sizeof(k)));

	/* The client's X25519 point is a point of small order: EINVAL, outputs wiped. */
	for (index = 0; index < 5; index++) {
		memcpy(changed, c_init, SSH_HYBRID_INIT_SIZE);
		memcpy(changed + SSH_MLKEM768_PUBLIC, small_order_points[index], SSH_X25519_SIZE);
		memset(s_reply, 0xaa, sizeof(s_reply));
		memset(k, 0xaa, sizeof(k));
		CHECK_EQ(ssh_hybrid_server(changed, SSH_HYBRID_INIT_SIZE, s_reply, k), EINVAL);
		CHECK(all_zero(s_reply, SSH_HYBRID_REPLY_SIZE));
		CHECK(all_zero(k, sizeof(k)));
	}

	/* The unchanged C_INIT still works after all the refusals. */
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k), 0);
	ssh_hybrid_client_clear(&client);
}

/*
 * Checks the client's finish: a reply of a wrong length, a point of small
 * order, a changed ciphertext (a different K, not an error), a state that
 * holds nothing; the state is cleared whatever the result.
 */
static void
test_hybrid_client_refusals(void)
{
	struct ssh_hybrid_client client;
	struct ssh_hybrid_client empty;
	uint8_t k_server[SSH_SHA256_SIZE];
	uint8_t k_client[SSH_SHA256_SIZE];
	size_t index;
	size_t position;

	/* A good exchange to change. */
	start_client(&client, c_init);
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
	ssh_hybrid_client_clear(&client);

	/* The length minus one: EINVAL, K wiped, the state cleared. */
	start_client(&client, c_init);
	CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
	memset(k_client, 0xaa, sizeof(k_client));
	CHECK_EQ(ssh_hybrid_client_finish(&client, s_reply, SSH_HYBRID_REPLY_SIZE - 1U, k_client), EINVAL);
	CHECK(all_zero(k_client, sizeof(k_client)));
	CHECK(client.mlkem == NULL);
	CHECK(all_zero(client.x25519_private, sizeof(client.x25519_private)));

	/* The length plus one, and zero. */
	start_client(&client, c_init);
	memset(k_client, 0xaa, sizeof(k_client));
	CHECK_EQ(ssh_hybrid_client_finish(&client, s_reply, SSH_HYBRID_REPLY_SIZE + 1U, k_client), EINVAL);
	CHECK(all_zero(k_client, sizeof(k_client)));
	CHECK(client.mlkem == NULL);
	start_client(&client, c_init);
	CHECK_EQ(ssh_hybrid_client_finish(&client, NULL, 0, k_client), EINVAL);
	CHECK(client.mlkem == NULL);

	/* A flipped bit of the ciphertext in each of three places: finishing succeeds with a K that differs (implicit rejection). */
	for (index = 0; index < 3; index++) {
		start_client(&client, c_init);
		CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
		memcpy(changed, s_reply, SSH_HYBRID_REPLY_SIZE);
		if (index == 0)
			position = 0U;
		else if (index == 1)
			position = 500U;
		else
			position = (size_t)SSH_MLKEM768_CIPHERTEXT - 1U;
		changed[position] ^= 0x01;
		memset(k_client, 0, sizeof(k_client));
		CHECK_EQ(ssh_hybrid_client_finish(&client, changed, SSH_HYBRID_REPLY_SIZE, k_client), 0);
		CHECK(memcmp(k_client, k_server, SSH_SHA256_SIZE) != 0);
		CHECK(!all_zero(k_client, sizeof(k_client)));
		CHECK(client.mlkem == NULL);
	}

	/* The server's X25519 point is a point of small order: EINVAL, K wiped, the state cleared. */
	for (index = 0; index < 5; index++) {
		start_client(&client, c_init);
		CHECK_EQ(ssh_hybrid_server(c_init, SSH_HYBRID_INIT_SIZE, s_reply, k_server), 0);
		memcpy(changed, s_reply, SSH_HYBRID_REPLY_SIZE);
		memcpy(changed + SSH_MLKEM768_CIPHERTEXT, small_order_points[index], SSH_X25519_SIZE);
		memset(k_client, 0xaa, sizeof(k_client));
		CHECK_EQ(ssh_hybrid_client_finish(&client, changed, SSH_HYBRID_REPLY_SIZE, k_client), EINVAL);
		CHECK(all_zero(k_client, sizeof(k_client)));
		CHECK(client.mlkem == NULL);
		CHECK(all_zero(client.x25519_private, sizeof(client.x25519_private)));
	}

	/* A state that holds nothing cannot be finished: EIO, K wiped; clearing it is harmless. */
	memset(&empty, 0, sizeof(empty));
	memset(k_client, 0xaa, sizeof(k_client));
	CHECK_EQ(ssh_hybrid_client_finish(&empty, s_reply, SSH_HYBRID_REPLY_SIZE, k_client), EIO);
	CHECK(all_zero(k_client, sizeof(k_client)));
	ssh_hybrid_client_clear(&empty);
}

/*
 * Checks ChaCha20 against RFC 8439 2.4.2 in the 64-bit-counter form, in
 * place and apart, and that blocks continue across a call.
 *
 * Values referenced: RFC 8439 section 2.4.2 (key, nonce, initial counter 1,
 * plaintext and ciphertext "Sunscreen"), and appendix A.1 through the table
 * chacha20_vectors.
 */
static void
test_chacha20(void)
{
	static const char plaintext[] =
	    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	uint8_t key[SSH_CHACHA20_KEY];
	uint8_t nonce[SSH_CHACHA20_NONCE];
	uint8_t expected[114];
	uint8_t buffer[114];
	uint8_t output[114];
	uint8_t stream_zero[128];
	uint8_t stream_one[64];
	uint8_t zeros[128];
	uint8_t vector_key[SSH_CHACHA20_KEY];
	uint8_t vector_stream[64];
	size_t index;

	/* The key 00 01 ... 1f, the nonce (state words 14 and 15) and the ciphertext of 2.4.2. */
	CHECK_EQ(wire_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("00 00 00 4a 00 00 00 00", nonce, sizeof(nonce)), 8);
	CHECK_EQ(wire_hex(
	    "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
	    "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
	    "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
	    "5af90bbf74a35be6b40b8eedf2785e42874d", expected, sizeof(expected)), 114);
	CHECK_EQ(sizeof(plaintext) - 1, 114);

	/* The plaintext encrypted from block counter 1, into another buffer. */
	memset(output, 0, sizeof(output));
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, (const uint8_t *)plaintext, output, 114), 0);
	CHECK_BYTES(output, expected, 114);

	/* The same with in and out the same buffer. */
	memcpy(buffer, plaintext, 114);
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, buffer, buffer, 114), 0);
	CHECK_BYTES(buffer, expected, 114);

	/* Decrypting is the same operation. */
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, buffer, buffer, 114), 0);
	CHECK_BYTES(buffer, plaintext, 114);

	/* The second block of the stream from counter 0 is the first block of the stream from counter 1. */
	memset(zeros, 0, sizeof(zeros));
	CHECK_EQ(ssh_chacha20_xor(key, 0, nonce, zeros, stream_zero, 128), 0);
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, zeros, stream_one, 64), 0);
	CHECK_BYTES(stream_zero + 64, stream_one, 64);
	CHECK(memcmp(stream_zero, stream_zero + 64, 64) != 0);

	/* The first 114 bytes from counter 1 are the plaintext xor the stream: the vector's keystream is the ciphertext xor the plaintext. */
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, zeros, stream_one, 64), 0);
	CHECK_EQ(stream_one[0], expected[0] ^ (uint8_t)plaintext[0]);
	CHECK_EQ(stream_one[63], expected[63] ^ (uint8_t)plaintext[63]);

	/* The block-function vectors of RFC 8439 A.1: the keystream is the XOR of zeros. */
	for (index = 0; index < 5; index++) {
		CHECK_EQ(wire_hex(chacha20_vectors[index].key, vector_key, sizeof(vector_key)), 32);
		CHECK_EQ(wire_hex(chacha20_vectors[index].nonce, nonce, sizeof(nonce)), 8);
		CHECK_EQ(wire_hex(chacha20_vectors[index].keystream, expected, sizeof(expected)), 64);
		memset(vector_stream, 0xaa, sizeof(vector_stream));
		memset(zeros, 0, sizeof(zeros));
		CHECK_EQ(ssh_chacha20_xor(vector_key, (uint32_t)chacha20_vectors[index].counter, nonce, zeros, vector_stream, 64), 0);
		CHECK_BYTES(vector_stream, expected, 64);
	}

	/* An empty range succeeds with NULL pointers. */
	CHECK_EQ(ssh_chacha20_xor(key, 1, nonce, NULL, NULL, 0), 0);
}

/*
 * Checks Poly1305 against RFC 8439 2.5.2 and the edge vectors of A.3.
 *
 * Values referenced: RFC 8439 section 2.5.2 (key material, message, tag) and
 * appendix A.3, Test Vectors #1, #5, #6 and #7 (R, S, data, tag; the key is R
 * then S).
 */
static void
test_poly1305(void)
{
	uint8_t key[SSH_POLY1305_KEY];
	uint8_t expected[SSH_POLY1305_TAG];
	uint8_t tag[SSH_POLY1305_TAG];
	uint8_t message[64];
	uint8_t zeros[64];

	/* 2.5.2: the key, the message "Cryptographic Forum Research Group" and the tag. */
	CHECK_EQ(wire_hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("a8061dc1305136c6c22b8baf0c0127a9", expected, sizeof(expected)), 16);
	memset(tag, 0, sizeof(tag));
	CHECK_EQ(ssh_poly1305(key, (const uint8_t *)"Cryptographic Forum Research Group", 34, tag), 0);
	CHECK_BYTES(tag, expected, 16);

	/* A changed message gives a different tag. */
	CHECK_EQ(ssh_poly1305(key, (const uint8_t *)"Cryptographic Forum Research Grouq", 34, tag), 0);
	CHECK(memcmp(tag, expected, 16) != 0);

	/* The tag of an empty message is s, the second half of the key (RFC 8439 2.5.1: the accumulator is 0 and s is added). */
	CHECK_EQ(wire_hex("0103808afb0db2fd4abff6af4149f51b", expected, sizeof(expected)), 16);
	CHECK_EQ(ssh_poly1305(key, NULL, 0, tag), 0);
	CHECK_BYTES(tag, expected, 16);

	/* A.3 #1: a zero key and 64 zero bytes give a zero tag. */
	memset(key, 0, sizeof(key));
	memset(zeros, 0, sizeof(zeros));
	memset(tag, 0xaa, sizeof(tag));
	CHECK_EQ(ssh_poly1305(key, zeros, 64, tag), 0);
	CHECK(all_zero(tag, sizeof(tag)));

	/* A.3 #5: r = 2, s = 0, data FF x 16: tag 03 00 ... 00 (the result 2^130 - 5 reduced only partly). */
	CHECK_EQ(wire_hex("02000000000000000000000000000000 00000000000000000000000000000000", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("ffffffffffffffffffffffffffffffff", message, sizeof(message)), 16);
	CHECK_EQ(wire_hex("03000000000000000000000000000000", expected, sizeof(expected)), 16);
	CHECK_EQ(ssh_poly1305(key, message, 16, tag), 0);
	CHECK_BYTES(tag, expected, 16);

	/* A.3 #6: r = 2, s = FF x 16, data 02 00 ...: the sum with s overflows 2^128. */
	CHECK_EQ(wire_hex("02000000000000000000000000000000 ffffffffffffffffffffffffffffffff", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex("02000000000000000000000000000000", message, sizeof(message)), 16);
	CHECK_EQ(wire_hex("03000000000000000000000000000000", expected, sizeof(expected)), 16);
	CHECK_EQ(ssh_poly1305(key, message, 16, tag), 0);
	CHECK_BYTES(tag, expected, 16);

	/* A.3 #7: r = 1, s = 0, three blocks with a carry between limbs: tag 05 00 ... 00. */
	CHECK_EQ(wire_hex("01000000000000000000000000000000 00000000000000000000000000000000", key, sizeof(key)), 32);
	CHECK_EQ(wire_hex(
	    "ffffffffffffffffffffffffffffffff"
	    "f0ffffffffffffffffffffffffffffff"
	    "11000000000000000000000000000000", message, sizeof(message)), 48);
	CHECK_EQ(wire_hex("05000000000000000000000000000000", expected, sizeof(expected)), 16);
	CHECK_EQ(ssh_poly1305(key, message, 48, tag), 0);
	CHECK_BYTES(tag, expected, 16);
}

/*
 * Checks the comparison, the wipe and the random bytes.
 */
static void
test_misc(void)
{
	uint8_t left[40];
	uint8_t right[40];
	uint8_t first[64];
	uint8_t second[64];
	static uint8_t large[70000];
	size_t index;
	size_t bit;
	size_t count;

	/* Equal ranges compare equal, an empty range too (with NULL pointers). */
	memset(left, 0x5a, sizeof(left));
	memset(right, 0x5a, sizeof(right));
	CHECK_EQ(ssh_ct_equal(left, right, sizeof(left)), 1);
	CHECK_EQ(ssh_ct_equal(NULL, NULL, 0), 1);
	CHECK_EQ(ssh_ct_equal(left, right, 0), 1);

	/* A range that differs in any single bit of any byte compares unequal. */
	for (index = 0; index < sizeof(left); index++) {
		for (bit = 0; bit < 8; bit++) {
			right[index] = (uint8_t)(0x5a ^ (1U << bit));
			CHECK_EQ(ssh_ct_equal(left, right, sizeof(left)), 0);
		}
		right[index] = 0x5a;
	}

	/* Only the first size bytes are compared. */
	right[39] = 0;
	CHECK_EQ(ssh_ct_equal(left, right, 39), 1);
	CHECK_EQ(ssh_ct_equal(left, right, 40), 0);

	/* Wiping zeroes the range and nothing beyond it. */
	memset(left, 0xa5, sizeof(left));
	ssh_wipe(left + 4, 20);
	for (index = 0; index < sizeof(left); index++) {
		if (index >= 4 && index < 24)
			CHECK_EQ(left[index], 0);
		else
			CHECK_EQ(left[index], 0xa5);
	}
	ssh_wipe(NULL, 0);

	/* Random bytes: two draws differ, and a draw is not all zero. */
	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));
	CHECK_EQ(ssh_random(first, sizeof(first)), 0);
	CHECK_EQ(ssh_random(second, sizeof(second)), 0);
	CHECK(memcmp(first, second, sizeof(first)) != 0);
	CHECK(!all_zero(first, sizeof(first)));

	/* An empty draw succeeds with NULL; a large draw fills the whole range (no long zero tail). */
	CHECK_EQ(ssh_random(NULL, 0), 0);
	memset(large, 0, sizeof(large));
	CHECK_EQ(ssh_random(large, sizeof(large)), 0);
	count = 0;
	for (index = 0; index < sizeof(large); index++) {
		if (large[index] == 0)
			count++;
	}
	CHECK(count < 600);
	CHECK(!all_zero(large + sizeof(large) - 64, 64));
}

/*
 * Builds the encodings 0, 1, p - 1, p and p + 1 of the points of small
 * order of Curve25519 (RFC 7748 6.1), p = 2^255 - 19.
 */
static void
make_small_order_points(void)
{
	size_t index;

	/* The points 0 and 1. */
	memset(small_order_points, 0, sizeof(small_order_points));
	small_order_points[1][0] = 0x01;

	/* The points p - 1, p and p + 1: 0xec, 0xed and 0xee, then 30 bytes of 0xff and a final 0x7f. */
	for (index = 2; index < 5; index++) {
		memset(small_order_points[index], 0xff, SSH_X25519_SIZE);
		small_order_points[index][0] = (uint8_t)(0xec + (index - 2));
		small_order_points[index][31] = 0x7f;
	}
}

/*
 * Starts a client half, checking that it succeeded.
 */
static void
start_client(
	struct ssh_hybrid_client *client,
	uint8_t *init)
{
	memset(client, 0, sizeof(*client));
	CHECK_EQ(ssh_hybrid_client_start(client, init), 0);
}

/*
 * Computes K for a client half and a reply from the library's own calls,
 * apart from the module: K_PQ by decapsulation, K_CL by derivation, K =
 * SHA-256 of the two joined.  Returns 0 or 1 when a call failed.
 */
static int
recompute_k(
	const struct ssh_hybrid_client *client,
	const uint8_t *reply,
	uint8_t *k)
{
	uint8_t joined[64];
	size_t secret_size;
	size_t shared_size;
	EVP_PKEY_CTX *decapsulator;
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *deriver;
	unsigned int digest_size;
	int status;
	int failed;

	failed = 0;
	decapsulator = NULL;
	own = NULL;
	peer = NULL;
	deriver = NULL;

	/* Decapsulates S_CT2 with the client's ML-KEM key into the first half of the joined secrets (K_PQ). */
	secret_size = 32;
	decapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, (EVP_PKEY *)client->mlkem, NULL);
	if (decapsulator == NULL)
		failed = 1;
	if (!failed) {
		status = EVP_PKEY_decapsulate_init(decapsulator, NULL);
		if (status != 1)
			failed = 1;
	}
	if (!failed) {
		status = EVP_PKEY_decapsulate(decapsulator, joined, &secret_size, reply, SSH_MLKEM768_CIPHERTEXT);
		if (status != 1)
			failed = 1;
	}

	/* Makes the client's X25519 key and the server's point. */
	own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, client->x25519_private, 32);
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, reply + SSH_MLKEM768_CIPHERTEXT, 32);
	if (own == NULL || peer == NULL)
		failed = 1;
	if (!failed) {
		deriver = EVP_PKEY_CTX_new(own, NULL);
		if (deriver == NULL)
			failed = 1;
	}

	/* Derives the X25519 shared secret into the second half of the joined secrets (K_CL). */
	shared_size = 32;
	if (!failed) {
		status = EVP_PKEY_derive_init(deriver);
		if (status != 1)
			failed = 1;
	}
	if (!failed) {
		status = EVP_PKEY_derive_set_peer(deriver, peer);
		if (status != 1)
			failed = 1;
	}
	if (!failed) {
		status = EVP_PKEY_derive(deriver, joined + 32, &shared_size);
		if (status != 1)
			failed = 1;
	}

	/* Hashes K_PQ then K_CL with SHA-256 into K. */
	digest_size = 0;
	if (!failed) {
		status = EVP_Digest(joined, 64, k, &digest_size, EVP_sha256(), NULL);
		if (status != 1)
			failed = 1;
	}

	/* Frees what was made and wipes the joined secrets. */
	EVP_PKEY_CTX_free(deriver);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(own);
	EVP_PKEY_CTX_free(decapsulator);
	memset(joined, 0, sizeof(joined));

	/* Succeeded when no call failed: the caller compares K. */
	return failed;
}

/*
 * Reports whether all size bytes are zero.
 */
static int
all_zero(
	const uint8_t *data,
	size_t size)
{
	size_t index;

	for (index = 0; index < size; index++) {
		if (data[index] != 0)
			return 0;
	}
	return 1;
}
