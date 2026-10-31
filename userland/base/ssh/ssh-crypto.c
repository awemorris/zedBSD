/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cryptographic operations of ssh and sshd, as calls of the system crypto library.
 *
 * Every function is a call, or a short sequence of calls, of the library's
 * EVP, RAND and CRYPTO interfaces; no primitive is implemented here.  A
 * library object made by a function is freed on every path of that
 * function, and a secret kept in local memory is wiped on every path.
 */

#include <limits.h>
#include <string.h>

#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>

#include "ssh-crypto.h"

/*
 * The registered name of the ML-KEM-768 key type, used to make a key pair
 * and to import an encapsulation key.
 */
#define SSH_MLKEM768_NAME	"ML-KEM-768"

/* The size of the two shared secrets joined before the hybrid key exchange hashes them (K_PQ, then K_CL). */
#define SSH_HYBRID_JOINED_SIZE	(SSH_MLKEM768_SECRET + SSH_X25519_SIZE)

/*
 * One byte standing for the data of an empty message.
 *
 * A caller may pass a NULL pointer with a size of 0; the library is handed
 * this address instead, so that it never sees a NULL data pointer.  It is
 * never written, and lives as long as the program.
 */
static const uint8_t ssh_crypto_empty[1] = { 0 };

/*
 * Thirty-two zero bytes, the value an X25519 result must not have.
 *
 * A shared secret that equals it comes from a peer point of small order
 * (RFC 7748 6.1, RFC 8731 3) and is refused.  Read only, lives as long as
 * the program.
 */
static const uint8_t ssh_crypto_zero_secret[SSH_X25519_SIZE] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int ssh_x25519_make_public(const uint8_t *private_key, uint8_t *public_key);
static int ssh_x25519_make_pair(uint8_t *private_key, uint8_t *public_key);
static int ssh_x25519_agree(const uint8_t *private_key, const uint8_t *peer_public, uint8_t *shared);
static int ssh_hybrid_combine(const uint8_t *k_pq, const uint8_t *k_cl, uint8_t *k);

/*
 * Overwrites size bytes at memory with zeros in a way the compiler keeps
 * (OPENSSL_cleanse); memory may be NULL only when size is 0.
 */
void
ssh_wipe(
	void *memory,
	size_t size)
{
	/* Leaves an empty range alone, whose pointer may be NULL. */
	if (size == 0)
		return;

	/* Overwrites the range with zeros through the library's cleanse, which the optimizer must keep. */
	OPENSSL_cleanse(memory, size);
}

/*
 * Compares size bytes in constant time (CRYPTO_memcmp).
 *
 * Returns 1 when equal and 0 otherwise; the time depends only on size.
 */
int
ssh_ct_equal(
	const void *left,
	const void *right,
	size_t size)
{
	int difference;

	/* Two empty ranges are equal, and their pointers may be NULL. */
	if (size == 0)
		return 1;

	/* Compares the two ranges in a time that does not depend on where they differ. */
	difference = CRYPTO_memcmp(left, right, size);

	/* Reports that the ranges differ. */
	if (difference != 0)
		return 0;

	/* Reports that the ranges are equal. */
	return 1;
}

/*
 * Fills size bytes at out with random bytes (RAND_bytes, in pieces of at
 * most INT_MAX bytes).
 *
 * Returns 0, or EIO when the library failed (out is then wiped).  There is
 * no weaker fallback.
 */
int
ssh_random(
	void *out,
	size_t size)
{
	uint8_t *cursor;
	size_t remaining;
	size_t piece;
	int status;

	/* Fills nothing when nothing is asked, whatever the pointer is. */
	if (size == 0)
		return 0;

	/* Fills the range piece by piece, because RAND_bytes takes an int count. */
	cursor = out;
	remaining = size;
	while (remaining > 0) {
		/* Takes the next piece, at most INT_MAX bytes. */
		piece = remaining;
		if (piece > (size_t)INT_MAX)
			piece = (size_t)INT_MAX;

		/* Draws the random bytes of the piece from the library's generator. */
		status = RAND_bytes(cursor, (int)piece);
		if (status != 1) {
			/* Leaves no partly random range behind. */
			ssh_wipe(out, size);
			return EIO;
		}

		cursor += piece;
		remaining -= piece;
	}

	/* Succeeded: the whole range is random. */
	return 0;
}

/*
 * Starts a SHA-256 computation (EVP_MD_CTX_new, EVP_DigestInit_ex with
 * EVP_sha256).  Returns 0, ENOMEM or EIO.
 */
int
ssh_sha256_init(
	struct ssh_sha256 *hash)
{
	EVP_MD_CTX *context;
	int status;

	/* Starts holding nothing, so that a failure leaves a zero structure. */
	hash->context = NULL;

	/* Makes the digest context. */
	context = EVP_MD_CTX_new();
	if (context == NULL)
		return ENOMEM;

	/* Selects SHA-256 for the context. */
	status = EVP_DigestInit_ex(context, EVP_sha256(), NULL);
	if (status != 1) {
		EVP_MD_CTX_free(context);
		return EIO;
	}

	/* Hands the started computation to the caller. */
	hash->context = context;

	/* Succeeded: the computation is started. */
	return 0;
}

/*
 * Absorbs size bytes (EVP_DigestUpdate); data may be NULL only when size
 * is 0.  Returns 0 or EIO.
 */
int
ssh_sha256_update(
	struct ssh_sha256 *hash,
	const void *data,
	size_t size)
{
	EVP_MD_CTX *context;
	int status;

	/* Refuses a structure that holds no computation. */
	if (hash->context == NULL)
		return EIO;

	/* Absorbs nothing for an empty range, whose pointer may be NULL. */
	if (size == 0)
		return 0;

	/* Feeds the bytes to the running digest. */
	context = hash->context;
	status = EVP_DigestUpdate(context, data, size);
	if (status != 1)
		return EIO;

	/* Succeeded: the bytes are absorbed. */
	return 0;
}

/*
 * Writes the 32-byte digest to out (EVP_DigestFinal_ex) and frees the
 * computation, also on a failure.  Returns 0 or EIO (out wiped).
 */
int
ssh_sha256_final(
	struct ssh_sha256 *hash,
	uint8_t *out)
{
	EVP_MD_CTX *context;
	unsigned int digest_size;
	int status;

	/* Refuses a structure that holds no computation. */
	if (hash->context == NULL) {
		ssh_wipe(out, SSH_SHA256_SIZE);
		return EIO;
	}

	/* Takes the computation out of the structure, which then holds nothing whatever happens next. */
	context = hash->context;
	hash->context = NULL;

	/* Finishes the digest into the caller's buffer. */
	digest_size = 0;
	status = EVP_DigestFinal_ex(context, out, &digest_size);

	/* Frees the digest context, which has finished. */
	EVP_MD_CTX_free(context);

	/* Refuses a digest the library did not produce in full. */
	if (status != 1) {
		ssh_wipe(out, SSH_SHA256_SIZE);
		return EIO;
	}
	if (digest_size != SSH_SHA256_SIZE) {
		ssh_wipe(out, SSH_SHA256_SIZE);
		return EIO;
	}

	/* Succeeded: out holds the digest. */
	return 0;
}

/*
 * Frees an abandoned computation; a zero structure is ignored.
 */
void
ssh_sha256_free(
	struct ssh_sha256 *hash)
{
	EVP_MD_CTX *context;

	/* Takes the computation out of the structure, which then holds nothing. */
	context = hash->context;
	hash->context = NULL;

	/* Frees the digest context; a NULL context is ignored by the library. */
	EVP_MD_CTX_free(context);
}

/*
 * Hashes size bytes at data into out (32 bytes) in one call.
 *
 * Returns 0, ENOMEM or EIO.
 */
int
ssh_sha256(
	const void *data,
	size_t size,
	uint8_t *out)
{
	struct ssh_sha256 hash;
	int error;

	/* Starts the computation. */
	error = ssh_sha256_init(&hash);
	if (error != 0) {
		ssh_wipe(out, SSH_SHA256_SIZE);
		return error;
	}

	/* Absorbs the data. */
	error = ssh_sha256_update(&hash, data, size);
	if (error != 0) {
		ssh_sha256_free(&hash);
		ssh_wipe(out, SSH_SHA256_SIZE);
		return error;
	}

	/* Finishes the digest, which also frees the computation. */
	error = ssh_sha256_final(&hash, out);
	if (error != 0)
		return error;

	/* Succeeded: out holds the digest. */
	return 0;
}

/*
 * Computes the Ed25519 public key (32 bytes) of a 32-byte seed (RFC 8032
 * 5.1.5): EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519) and
 * EVP_PKEY_get_raw_public_key.  Returns 0 or EIO.
 */
int
ssh_ed25519_public(
	const uint8_t *seed,
	uint8_t *public_key)
{
	EVP_PKEY *key;
	size_t public_size;
	int status;

	/* Makes the key from the seed; the library computes the public half. */
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, SSH_ED25519_SEED);
	if (key == NULL) {
		ssh_wipe(public_key, SSH_ED25519_PUBLIC);
		return EIO;
	}

	/* Reads the public half out of the key. */
	public_size = SSH_ED25519_PUBLIC;
	status = EVP_PKEY_get_raw_public_key(key, public_key, &public_size);

	/* Frees the key, which holds the seed and wipes it. */
	EVP_PKEY_free(key);

	/* Refuses a public key the library did not give in full. */
	if (status != 1) {
		ssh_wipe(public_key, SSH_ED25519_PUBLIC);
		return EIO;
	}
	if (public_size != SSH_ED25519_PUBLIC) {
		ssh_wipe(public_key, SSH_ED25519_PUBLIC);
		return EIO;
	}

	/* Succeeded: public_key holds the public key. */
	return 0;
}

/*
 * Signs size bytes at message with the seed (RFC 8032 5.1.6, pure
 * Ed25519), writing 64 bytes to signature: EVP_PKEY_new_raw_private_key,
 * EVP_DigestSignInit with a NULL digest, EVP_DigestSign.
 *
 * message may be NULL only when size is 0.  Returns 0 or EIO (signature
 * wiped).
 */
int
ssh_ed25519_sign(
	const uint8_t *seed,
	const uint8_t *message,
	size_t size,
	uint8_t *signature)
{
	EVP_PKEY *key;
	EVP_MD_CTX *context;
	const uint8_t *data;
	size_t signature_size;
	int status;
	int error;

	key = NULL;
	context = NULL;
	error = 0;

	/* Gives the library a real address for an empty message. */
	data = message;
	if (size == 0)
		data = ssh_crypto_empty;

	/* Makes the signing key from the seed. */
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, SSH_ED25519_SEED);
	if (key == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Makes the context the signature is computed in. */
	context = EVP_MD_CTX_new();
	if (context == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Starts a pure Ed25519 signature: the digest is NULL because the algorithm hashes inside. */
	status = EVP_DigestSignInit(context, NULL, NULL, NULL, key);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Signs the message in one shot. */
	signature_size = SSH_ED25519_SIGNATURE;
	status = EVP_DigestSign(context, signature, &signature_size, data, size);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Refuses a signature that is not 64 bytes. */
	if (signature_size != SSH_ED25519_SIGNATURE)
		error = EIO;

cleanup:
	/* Frees the context and the key, which holds the seed and wipes it. */
	EVP_MD_CTX_free(context);
	EVP_PKEY_free(key);

	/* Leaves no partly written signature behind. */
	if (error != 0) {
		ssh_wipe(signature, SSH_ED25519_SIGNATURE);
		return error;
	}

	/* Succeeded: signature holds the 64 bytes. */
	return 0;
}

/*
 * Verifies a 64-byte signature of size bytes at message under a 32-byte
 * public key (RFC 8032 5.1.7): EVP_PKEY_new_raw_public_key,
 * EVP_DigestVerifyInit, EVP_DigestVerify.
 *
 * Returns 0 when it verifies; EBADMSG when the library answers 0 (a bad
 * signature, a public key or an R that does not decode); EINVAL when the
 * public key is refused at import; EIO for another failure.
 */
int
ssh_ed25519_verify(
	const uint8_t *public_key,
	const uint8_t *message,
	size_t size,
	const uint8_t *signature)
{
	EVP_PKEY *key;
	EVP_MD_CTX *context;
	const uint8_t *data;
	int status;
	int error;

	key = NULL;
	context = NULL;
	error = 0;

	/* Gives the library a real address for an empty message. */
	data = message;
	if (size == 0)
		data = ssh_crypto_empty;

	/* Imports the public key; a key the library refuses is a bad value of the peer. */
	key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, SSH_ED25519_PUBLIC);
	if (key == NULL) {
		error = EINVAL;
		goto cleanup;
	}

	/* Makes the context the verification runs in. */
	context = EVP_MD_CTX_new();
	if (context == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Starts a pure Ed25519 verification: the digest is NULL because the algorithm hashes inside. */
	status = EVP_DigestVerifyInit(context, NULL, NULL, NULL, key);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Verifies the signature in one shot; 1 is valid, 0 is invalid, a negative value is a failure. */
	status = EVP_DigestVerify(context, signature, SSH_ED25519_SIGNATURE, data, size);
	if (status == 0) {
		error = EBADMSG;
	} else if (status != 1) {
		error = EIO;
	}

cleanup:
	/* Frees the context and the key. */
	EVP_MD_CTX_free(context);
	EVP_PKEY_free(key);

	/* Reports why the signature was not accepted. */
	if (error != 0)
		return error;

	/* Succeeded: the signature is valid. */
	return 0;
}

/*
 * Starts the client's half of mlkem768x25519-sha256 (draft-ietf-sshm-
 * mlkem-hybrid-kex-10 2.1, 2.3.3) and writes C_INIT
 * (SSH_HYBRID_INIT_SIZE bytes) to c_init.
 *
 * Makes an ML-KEM-768 key pair and writes its encapsulation key as C_PK2,
 * then makes an X25519 key pair from 32 random bytes and writes its public
 * key as C_PK1.  A new pair of each for every key exchange.
 *
 * client must hold nothing.  Returns 0, ENOMEM or EIO (client then holds
 * nothing and c_init is wiped).
 */
int
ssh_hybrid_client_start(
	struct ssh_hybrid_client *client,
	uint8_t *c_init)
{
	EVP_PKEY_CTX *generator;
	EVP_PKEY *pair;
	size_t key_size;
	int status;
	int error;

	generator = NULL;
	pair = NULL;
	error = 0;

	/* Starts holding nothing, so that a failure leaves a zero structure. */
	client->mlkem = NULL;
	memset(client->x25519_private, 0, sizeof(client->x25519_private));

	/* Makes the context the ML-KEM-768 key pair is generated in. */
	generator = EVP_PKEY_CTX_new_from_name(NULL, SSH_MLKEM768_NAME, NULL);
	if (generator == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Prepares the key generation. */
	status = EVP_PKEY_keygen_init(generator);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Generates the ML-KEM-768 key pair, whose decapsulation key stays in the library's object. */
	status = EVP_PKEY_keygen(generator, &pair);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}
	if (pair == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Writes the encapsulation key as the first part of C_INIT (C_PK2). */
	key_size = SSH_MLKEM768_PUBLIC;
	status = EVP_PKEY_get_raw_public_key(pair, c_init, &key_size);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}
	if (key_size != SSH_MLKEM768_PUBLIC) {
		error = EIO;
		goto cleanup;
	}

	/* Makes the X25519 pair and writes its public key as the second part of C_INIT (C_PK1). */
	error = ssh_x25519_make_pair(client->x25519_private, c_init + SSH_MLKEM768_PUBLIC);
	if (error != 0)
		goto cleanup;

	/* Keeps the ML-KEM pair in the client's state; the state now owns it. */
	client->mlkem = pair;
	pair = NULL;

cleanup:
	/* Frees the generation context and, on a failure, the pair that is not kept. */
	EVP_PKEY_CTX_free(generator);
	EVP_PKEY_free(pair);

	/* Leaves nothing held and nothing written after a failure. */
	if (error != 0) {
		ssh_wipe(client->x25519_private, sizeof(client->x25519_private));
		client->mlkem = NULL;
		ssh_wipe(c_init, SSH_HYBRID_INIT_SIZE);
		return error;
	}

	/* Succeeded: c_init holds C_INIT and client holds the private halves. */
	return 0;
}

/*
 * Does the server's half of mlkem768x25519-sha256: checks C_INIT, makes
 * S_REPLY and the shared secret K.
 *
 * c_init_size must be SSH_HYBRID_INIT_SIZE exactly (EINVAL otherwise).  The
 * encapsulation key C_PK2 is imported with the checks of FIPS 203 7.2 (a
 * refusal is EINVAL) and encapsulated to, giving S_CT2 and K_PQ.  An X25519
 * pair is made, giving S_PK1, and K_CL = X25519(server private, C_PK1).
 * The peer's point is refused (EINVAL) when the library refuses it at
 * import or at the derivation, and also when the derivation succeeds with
 * an all-zero K_CL, which this function checks itself.  K_CL is the 32
 * bytes of the X25519 output as they are.  Then k = SHA-256(K_PQ || K_CL).
 *
 * s_reply receives SSH_HYBRID_REPLY_SIZE bytes (S_CT2 || S_PK1) and k 32
 * bytes.  Returns 0, EINVAL (the peer's values are refused), ENOMEM or EIO.
 * On an error s_reply and k are wiped.
 */
int
ssh_hybrid_server(
	const uint8_t *c_init,
	size_t c_init_size,
	uint8_t *s_reply,
	uint8_t *k)
{
	uint8_t x25519_private[SSH_X25519_SIZE];
	uint8_t k_pq[SSH_MLKEM768_SECRET];
	uint8_t k_cl[SSH_X25519_SIZE];
	EVP_PKEY *client_key;
	EVP_PKEY_CTX *encapsulator;
	size_t ciphertext_size;
	size_t secret_size;
	int status;
	int error;

	client_key = NULL;
	encapsulator = NULL;
	error = 0;

	/* Refuses an initial message of the wrong length (the length check of the draft's 2.1). */
	if (c_init_size != SSH_HYBRID_INIT_SIZE) {
		error = EINVAL;
		goto cleanup;
	}

	/* Imports the client's encapsulation key; the library applies the checks of FIPS 203 7.2 and a key it refuses is a bad value of the peer. */
	client_key = EVP_PKEY_new_raw_public_key_ex(NULL, SSH_MLKEM768_NAME, NULL, c_init, SSH_MLKEM768_PUBLIC);
	if (client_key == NULL) {
		error = EINVAL;
		goto cleanup;
	}

	/* Makes the context the encapsulation runs in. */
	encapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, client_key, NULL);
	if (encapsulator == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Prepares the encapsulation, with no parameters. */
	status = EVP_PKEY_encapsulate_init(encapsulator, NULL);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Encapsulates to the client's key, writing S_CT2 into the reply and K_PQ. */
	ciphertext_size = SSH_MLKEM768_CIPHERTEXT;
	secret_size = SSH_MLKEM768_SECRET;
	status = EVP_PKEY_encapsulate(encapsulator, s_reply, &ciphertext_size, k_pq, &secret_size);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}
	if (ciphertext_size != SSH_MLKEM768_CIPHERTEXT) {
		error = EIO;
		goto cleanup;
	}
	if (secret_size != SSH_MLKEM768_SECRET) {
		error = EIO;
		goto cleanup;
	}

	/* Makes the server's X25519 pair and writes its public key as the second part of the reply (S_PK1). */
	error = ssh_x25519_make_pair(x25519_private, s_reply + SSH_MLKEM768_CIPHERTEXT);
	if (error != 0)
		goto cleanup;

	/* Computes K_CL from the server's private key and the client's point C_PK1, refusing a point of small order. */
	error = ssh_x25519_agree(x25519_private, c_init + SSH_MLKEM768_PUBLIC, k_cl);
	if (error != 0)
		goto cleanup;

	/* Combines the two secrets into K = SHA-256(K_PQ || K_CL). */
	error = ssh_hybrid_combine(k_pq, k_cl, k);

cleanup:
	/* Frees the encapsulation context and the client's key. */
	EVP_PKEY_CTX_free(encapsulator);
	EVP_PKEY_free(client_key);

	/* Wipes the private key and the two partial secrets. */
	ssh_wipe(x25519_private, sizeof(x25519_private));
	ssh_wipe(k_pq, sizeof(k_pq));
	ssh_wipe(k_cl, sizeof(k_cl));

	/* Leaves nothing the peer could use after a failure. */
	if (error != 0) {
		ssh_wipe(s_reply, SSH_HYBRID_REPLY_SIZE);
		ssh_wipe(k, SSH_SHA256_SIZE);
		return error;
	}

	/* Succeeded: s_reply holds S_REPLY and k holds K. */
	return 0;
}

/*
 * Finishes the client's half: checks S_REPLY and computes K.
 *
 * s_reply_size must be SSH_HYBRID_REPLY_SIZE exactly (EINVAL otherwise).
 * K_PQ is decapsulated from S_CT2 with the client's ML-KEM key (a forged
 * ciphertext gives a wrong K_PQ, not an error, by ML-KEM's implicit
 * rejection); K_CL = X25519(client private, S_PK1), refused (EINVAL) as in
 * ssh_hybrid_server; k = SHA-256(K_PQ || K_CL).
 *
 * The client's state is wiped and freed whatever the result.  Returns 0,
 * EINVAL, ENOMEM or EIO; on an error k is wiped.
 */
int
ssh_hybrid_client_finish(
	struct ssh_hybrid_client *client,
	const uint8_t *s_reply,
	size_t s_reply_size,
	uint8_t *k)
{
	uint8_t k_pq[SSH_MLKEM768_SECRET];
	uint8_t k_cl[SSH_X25519_SIZE];
	EVP_PKEY_CTX *decapsulator;
	size_t secret_size;
	int status;
	int error;

	decapsulator = NULL;
	error = 0;

	/* Refuses a state that holds no key pair. */
	if (client->mlkem == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Refuses a reply of the wrong length (the length check of the draft's 2.1). */
	if (s_reply_size != SSH_HYBRID_REPLY_SIZE) {
		error = EINVAL;
		goto cleanup;
	}

	/* Makes the context the decapsulation runs in. */
	decapsulator = EVP_PKEY_CTX_new_from_pkey(NULL, client->mlkem, NULL);
	if (decapsulator == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Prepares the decapsulation, with no parameters. */
	status = EVP_PKEY_decapsulate_init(decapsulator, NULL);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Decapsulates S_CT2 into K_PQ; a forged ciphertext still succeeds with another secret. */
	secret_size = SSH_MLKEM768_SECRET;
	status = EVP_PKEY_decapsulate(decapsulator, k_pq, &secret_size, s_reply, SSH_MLKEM768_CIPHERTEXT);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}
	if (secret_size != SSH_MLKEM768_SECRET) {
		error = EIO;
		goto cleanup;
	}

	/* Computes K_CL from the client's private key and the server's point S_PK1, refusing a point of small order. */
	error = ssh_x25519_agree(client->x25519_private, s_reply + SSH_MLKEM768_CIPHERTEXT, k_cl);
	if (error != 0)
		goto cleanup;

	/* Combines the two secrets into K = SHA-256(K_PQ || K_CL). */
	error = ssh_hybrid_combine(k_pq, k_cl, k);

cleanup:
	/* Frees the decapsulation context. */
	EVP_PKEY_CTX_free(decapsulator);

	/* Wipes and frees the client's state, which is finished whatever the result. */
	ssh_hybrid_client_clear(client);

	/* Wipes the two partial secrets. */
	ssh_wipe(k_pq, sizeof(k_pq));
	ssh_wipe(k_cl, sizeof(k_cl));

	/* Leaves no key behind after a failure. */
	if (error != 0) {
		ssh_wipe(k, SSH_SHA256_SIZE);
		return error;
	}

	/* Succeeded: k holds K. */
	return 0;
}

/*
 * Wipes and frees a client half that will not be finished; a zero
 * structure is ignored.
 */
void
ssh_hybrid_client_clear(
	struct ssh_hybrid_client *client)
{
	EVP_PKEY *pair;

	/* Takes the key pair out of the structure, which then holds nothing. */
	pair = client->mlkem;
	client->mlkem = NULL;

	/* Frees the ML-KEM key pair, whose decapsulation key the library wipes; a NULL pair is ignored. */
	EVP_PKEY_free(pair);

	/* Wipes the X25519 private key. */
	ssh_wipe(client->x25519_private, sizeof(client->x25519_private));
}

/*
 * XORs size bytes of in with the ChaCha20 keystream of the
 * 64-bit-counter form (draft-ietf-sshm-chacha20-poly1305-04 3: state
 * words 12 and 13 a 64-bit block counter, words 14 and 15 an 8-byte
 * nonce, each word read little-endian) starting at block counter, into
 * out.
 *
 * The library's 16-byte IV becomes state words 12 to 15 read
 * little-endian, so the IV passed is counter as 8 little-endian bytes,
 * then nonce.  in and out may be the same buffer.
 *
 * Returns 0 or EIO (out wiped).
 */
int
ssh_chacha20_xor(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint8_t iv[16];
	EVP_CIPHER_CTX *context;
	const uint8_t *source;
	uint8_t *destination;
	size_t remaining;
	size_t piece;
	int written;
	int status;
	int error;

	context = NULL;
	error = 0;

	/* Builds the IV: the 64-bit block counter little-endian (the high half is zero), then the 8-byte nonce. */
	iv[0] = (uint8_t)(counter & 0xffU);
	iv[1] = (uint8_t)((counter >> 8) & 0xffU);
	iv[2] = (uint8_t)((counter >> 16) & 0xffU);
	iv[3] = (uint8_t)((counter >> 24) & 0xffU);
	iv[4] = 0;
	iv[5] = 0;
	iv[6] = 0;
	iv[7] = 0;
	memcpy(iv + 8, nonce, SSH_CHACHA20_NONCE);

	/* Makes the cipher context. */
	context = EVP_CIPHER_CTX_new();
	if (context == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Sets the key and the IV; ChaCha20 is the same in both directions. */
	status = EVP_CipherInit_ex(context, EVP_chacha20(), NULL, key, iv, 1);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Runs the keystream over the data in pieces, because EVP_CipherUpdate takes an int length. */
	source = in;
	destination = out;
	remaining = size;
	while (remaining > 0) {
		/* Takes the next piece, at most INT_MAX bytes. */
		piece = remaining;
		if (piece > (size_t)INT_MAX)
			piece = (size_t)INT_MAX;

		/* XORs the piece with the next part of the keystream. */
		written = 0;
		status = EVP_CipherUpdate(context, destination, &written, source, (int)piece);
		if (status != 1) {
			error = EIO;
			goto cleanup;
		}
		if ((size_t)written != piece) {
			error = EIO;
			goto cleanup;
		}

		source += piece;
		destination += piece;
		remaining -= piece;
	}

cleanup:
	/* Frees the cipher context, which holds the key and wipes it. */
	EVP_CIPHER_CTX_free(context);

	/* Leaves no partly processed data behind. */
	if (error != 0) {
		ssh_wipe(out, size);
		return error;
	}

	/* Succeeded: out holds the data XOR the keystream. */
	return 0;
}

/*
 * Computes the Poly1305 tag (16 bytes) of size bytes at message under a
 * 32-byte one-time key (draft-ietf-sshm-chacha20-poly1305-04 4; the r
 * half first, then the n half, as RFC 8439 2.5): EVP_MAC_fetch(NULL,
 * "POLY1305", NULL), EVP_MAC_CTX_new, EVP_MAC_init with the key,
 * EVP_MAC_update, EVP_MAC_final.
 *
 * message may be NULL only when size is 0.  Returns 0 or EIO (tag wiped).
 */
int
ssh_poly1305(
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	uint8_t *tag)
{
	EVP_MAC *mac;
	EVP_MAC_CTX *context;
	size_t tag_size;
	int status;
	int error;

	mac = NULL;
	context = NULL;
	error = 0;

	/* Fetches the Poly1305 algorithm. */
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	if (mac == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Makes the context the tag is computed in. */
	context = EVP_MAC_CTX_new(mac);
	if (context == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Starts the computation with the one-time key. */
	status = EVP_MAC_init(context, key, SSH_POLY1305_KEY, NULL);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Absorbs the message, if there is one. */
	if (size > 0) {
		status = EVP_MAC_update(context, message, size);
		if (status != 1) {
			error = EIO;
			goto cleanup;
		}
	}

	/* Writes the 16-byte tag. */
	tag_size = 0;
	status = EVP_MAC_final(context, tag, &tag_size, SSH_POLY1305_TAG);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}
	if (tag_size != SSH_POLY1305_TAG)
		error = EIO;

cleanup:
	/* Frees the context, which holds the key and wipes it, and the algorithm. */
	EVP_MAC_CTX_free(context);
	EVP_MAC_free(mac);

	/* Leaves no partly written tag behind. */
	if (error != 0) {
		ssh_wipe(tag, SSH_POLY1305_TAG);
		return error;
	}

	/* Succeeded: tag holds the 16 bytes. */
	return 0;
}

/*
 * Computes the X25519 public key of a 32-byte private key.
 */
static int
ssh_x25519_make_public(
	const uint8_t *private_key,
	uint8_t *public_key)
{
	EVP_PKEY *key;
	size_t public_size;
	int status;

	/* Makes the key from the private bytes; the library computes the public half. */
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, SSH_X25519_SIZE);
	if (key == NULL)
		return EIO;

	/* Reads the public half out of the key. */
	public_size = SSH_X25519_SIZE;
	status = EVP_PKEY_get_raw_public_key(key, public_key, &public_size);

	/* Frees the key, which holds the private bytes and wipes them. */
	EVP_PKEY_free(key);

	/* Refuses a public key the library did not give in full. */
	if (status != 1)
		return EIO;
	if (public_size != SSH_X25519_SIZE)
		return EIO;

	/* Succeeded: public_key holds the public key. */
	return 0;
}

/*
 * Makes an X25519 key pair from 32 random bytes: the private key (secret)
 * and its public key.  Wipes both on a failure.
 */
static int
ssh_x25519_make_pair(
	uint8_t *private_key,
	uint8_t *public_key)
{
	int error;

	/* Draws the private key from the random generator. */
	error = ssh_random(private_key, SSH_X25519_SIZE);
	if (error != 0) {
		ssh_wipe(public_key, SSH_X25519_SIZE);
		return error;
	}

	/* Computes the public key of the private key. */
	error = ssh_x25519_make_public(private_key, public_key);
	if (error != 0) {
		ssh_wipe(private_key, SSH_X25519_SIZE);
		ssh_wipe(public_key, SSH_X25519_SIZE);
		return error;
	}

	/* Succeeded: both halves are written. */
	return 0;
}

/*
 * Computes the X25519 shared secret of a private key and a peer's public
 * point, as the 32 bytes the library gives.
 *
 * The peer's point is refused with EINVAL when the library refuses it at
 * import or at the derivation, and when the result is all zero, which this
 * function checks itself so that it does not depend on the library.
 * Returns 0, EINVAL, ENOMEM or EIO; shared is wiped on an error.
 */
static int
ssh_x25519_agree(
	const uint8_t *private_key,
	const uint8_t *peer_public,
	uint8_t *shared)
{
	EVP_PKEY *own;
	EVP_PKEY *peer;
	EVP_PKEY_CTX *context;
	size_t shared_size;
	int zero;
	int status;
	int error;

	own = NULL;
	peer = NULL;
	context = NULL;
	error = 0;

	/* Makes our key from the private bytes. */
	own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, SSH_X25519_SIZE);
	if (own == NULL) {
		error = EIO;
		goto cleanup;
	}

	/* Imports the peer's point; a point the library refuses is a bad value of the peer. */
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public, SSH_X25519_SIZE);
	if (peer == NULL) {
		error = EINVAL;
		goto cleanup;
	}

	/* Makes the context the derivation runs in. */
	context = EVP_PKEY_CTX_new(own, NULL);
	if (context == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Prepares the derivation. */
	status = EVP_PKEY_derive_init(context);
	if (status != 1) {
		error = EIO;
		goto cleanup;
	}

	/* Sets the peer's point; a refusal here is a bad value of the peer. */
	status = EVP_PKEY_derive_set_peer(context, peer);
	if (status != 1) {
		error = EINVAL;
		goto cleanup;
	}

	/* Derives the shared secret; the library fails the derivation of an all-zero result, which is a bad value of the peer. */
	shared_size = SSH_X25519_SIZE;
	status = EVP_PKEY_derive(context, shared, &shared_size);
	if (status != 1) {
		error = EINVAL;
		goto cleanup;
	}
	if (shared_size != SSH_X25519_SIZE) {
		error = EIO;
		goto cleanup;
	}

	/* Refuses an all-zero result (a point of small order) even when the library let it through. */
	zero = ssh_ct_equal(shared, ssh_crypto_zero_secret, SSH_X25519_SIZE);
	if (zero != 0)
		error = EINVAL;

cleanup:
	/* Frees the context and both keys; ours holds the private bytes and is wiped. */
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(own);

	/* Leaves no secret behind after a failure. */
	if (error != 0) {
		ssh_wipe(shared, SSH_X25519_SIZE);
		return error;
	}

	/* Succeeded: shared holds the 32 bytes. */
	return 0;
}

/*
 * Computes K = SHA-256(K_PQ || K_CL), the shared secret of the hybrid key
 * exchange, from the two fixed-size secrets.
 */
static int
ssh_hybrid_combine(
	const uint8_t *k_pq,
	const uint8_t *k_cl,
	uint8_t *k)
{
	uint8_t joined[SSH_HYBRID_JOINED_SIZE];
	int error;

	/* Joins the two secrets, K_PQ first. */
	memcpy(joined, k_pq, SSH_MLKEM768_SECRET);
	memcpy(joined + SSH_MLKEM768_SECRET, k_cl, SSH_X25519_SIZE);

	/* Hashes the joined secrets into K. */
	error = ssh_sha256(joined, sizeof(joined), k);

	/* Wipes the joined secrets. */
	ssh_wipe(joined, sizeof(joined));

	/* Reports why K could not be computed. */
	if (error != 0)
		return error;

	/* Succeeded: k holds K. */
	return 0;
}
