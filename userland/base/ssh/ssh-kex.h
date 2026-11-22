/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages and the arithmetic of a key exchange: SSH_MSG_KEXINIT and
 * the choice of algorithms (RFC 4253 7.1), the strict key exchange
 * markers (draft-ietf-sshm-strict-kex-02 3.1), the messages of
 * mlkem768x25519-sha256 (draft-ietf-sshm-mlkem-hybrid-kex-10 2) and of
 * curve25519-sha256 (RFC 8731 3, which uses the ECDH messages of RFC 5656
 * 4), the exchange hash H (the draft's 2.5, RFC 8731 3.1) and the
 * derivation of keys (RFC 4253 7.2).  The order of the exchange and its
 * state live in ssh-transport.h.
 *
 * The server offers exactly one algorithm of each kind and the client's
 * list must contain it.  The client offers one algorithm of each kind but
 * the key exchange, where it offers mlkem768x25519-sha256 first and
 * curve25519-sha256 (under its standard name and the older name that
 * servers before RFC 8731 know) after it, so that a server without the
 * hybrid method (OpenSSH before 9.9) is still reached.  The choice follows
 * RFC 4253 7.1: the first algorithm on the client's list that is also on
 * the server's.  The lists sent:
 *
 *   kex_algorithms          client: "mlkem768x25519-sha256,
 *                           curve25519-sha256,curve25519-sha256@libssh.org,
 *                           kex-strict-c,kex-strict-c-v00@openssh.com";
 *                           server: "mlkem768x25519-sha256,kex-strict-s,
 *                           kex-strict-s-v00@openssh.com" (the markers only
 *                           in the initial KEXINIT;
 *                           draft-ietf-sshm-strict-kex-02 3.1)
 *   server_host_key_algorithms  "ssh-ed25519"
 *   encryption_algorithms   "chacha20-poly1305@openssh.com" (both ways)
 *   mac_algorithms          empty (both ways): the cipher authenticates,
 *                           and a peer that chose it consults no MAC list
 *                           (draft-ietf-sshm-chacha20-poly1305-04 5)
 *   compression_algorithms  "none" (both ways)
 *   languages               empty (both ways)
 *
 * Strict key exchange is required: a peer whose initial KEXINIT carries
 * neither marker of its side is refused (EPROTO).  Every peer that offers
 * mlkem768x25519-sha256 (OpenSSH 9.9 and later) also offers strict key
 * exchange, and so does every OpenSSH from 9.6 on that this client reaches
 * with curve25519-sha256.  This side sends both the standard and the
 * pre-standard marker, so a peer that sends either name pairs with it
 * (3.1).
 */

#ifndef SSH_SSH_KEX_H
#define SSH_SSH_KEX_H

#include "ssh-buf.h"
#include "ssh-cipher.h"
#include "ssh-crypto.h"

/* The message numbers (RFC 4250 4.1.2; draft-ietf-sshm-mlkem-hybrid-kex-10 2.2). */
#define SSH_MSG_KEXINIT			20
#define SSH_MSG_NEWKEYS			21
#define SSH_MSG_KEX_HYBRID_INIT		30
#define SSH_MSG_KEX_HYBRID_REPLY	31

/* The message numbers of curve25519-sha256 (RFC 5656 7.1, as RFC 8731 3 uses them); the same numbers as the hybrid ones. */
#define SSH_MSG_KEX_ECDH_INIT		30
#define SSH_MSG_KEX_ECDH_REPLY		31

/* The algorithm names. */
#define SSH_KEX_NAME		"mlkem768x25519-sha256"
#define SSH_KEX_CURVE25519_NAME	"curve25519-sha256"
#define SSH_KEX_CURVE25519_OLD_NAME	"curve25519-sha256@libssh.org"
#define SSH_KEX_HOSTKEY_NAME	"ssh-ed25519"
#define SSH_KEX_STRICT_CLIENT	"kex-strict-c"
#define SSH_KEX_STRICT_CLIENT_OLD	"kex-strict-c-v00@openssh.com"
#define SSH_KEX_STRICT_SERVER	"kex-strict-s"
#define SSH_KEX_STRICT_SERVER_OLD	"kex-strict-s-v00@openssh.com"

/* The size of the KEXINIT cookie (RFC 4253 7.1). */
#define SSH_KEX_COOKIE		16U

/* The largest KEXINIT payload accepted from a peer. */
#define SSH_KEXINIT_MAX		16384U

/* The size of the exchange hash and the shared secret K (SHA-256). */
#define SSH_KEX_HASH_SIZE	SSH_SHA256_SIZE

/*
 * The parts of a peer's SSH_MSG_KEXINIT these programs look at.
 *
 * The name-lists point into the payload the caller keeps (its bytes are
 * also hashed into H as they are).  index 0 to 9 are kex_algorithms,
 * server_host_key_algorithms, encryption client to server, encryption
 * server to client, MAC both ways, compression both ways, languages both
 * ways (RFC 4253 7.1 order).
 */
struct ssh_kexinit {
	const char *list[10];
	size_t list_size[10];
	int first_kex_packet_follows;
};

/* The key exchange methods; a server uses only the hybrid one. */
enum ssh_kex_method {
	/* mlkem768x25519-sha256: K enters the hash and the derivation as a string. */
	SSH_KEX_MLKEM768X25519 = 0,

	/* curve25519-sha256 under either name: K enters them as an mpint. */
	SSH_KEX_CURVE25519 = 1
};

/* How the shared secret K is encoded where the hash and the derivation take it. */
enum ssh_kex_secret_form {
	/* uint32 32, then the 32 bytes (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5). */
	SSH_KEX_SECRET_STRING = 0,

	/*
	 * The 32 bytes read as a big-endian unsigned number, as an mpint
	 * (RFC 8731 3.1, RFC 4251 5): leading zero bytes dropped, and one zero
	 * byte put in front when the highest bit of what is left is set.
	 */
	SSH_KEX_SECRET_MPINT = 1
};

/*
 * What the choice of algorithms decided.
 *
 * strict is 1 when strict key exchange is in force (decided by the
 * initial exchange; later exchanges keep it).  ignore_next is 1 when the
 * peer sent a guessed key exchange packet that is wrong and the next key
 * exchange packet from it must be dropped (RFC 4253 7: the guess is right
 * only when the peer's first kex algorithm and first host key algorithm
 * are the ones chosen).  method is the key exchange method chosen for
 * this exchange (each exchange chooses again).
 */
struct ssh_kex_choice {
	int strict;
	int ignore_next;
	enum ssh_kex_method method;
};

/*
 * The values the exchange hash covers (draft-ietf-sshm-mlkem-hybrid-kex-10
 * 2.5), each as raw bytes the hash encodes as a string.
 *
 * version_client and version_server are the identification lines without
 * CR LF; kexinit_client and kexinit_server the whole KEXINIT payloads
 * including the message number; host_key the server's public key blob;
 * c_init and s_reply the hybrid values, or with curve25519-sha256 the
 * client's and the server's ephemeral public keys Q_C and Q_S (RFC 8731
 * 3.1 hashes them as strings in the same places); k the 32-byte shared
 * secret, which the hash encodes as its form says.
 */
struct ssh_kex_hash_input {
	const uint8_t *version_client;
	size_t version_client_size;
	const uint8_t *version_server;
	size_t version_server_size;
	const uint8_t *kexinit_client;
	size_t kexinit_client_size;
	const uint8_t *kexinit_server;
	size_t kexinit_server_size;
	const uint8_t *host_key;
	size_t host_key_size;
	const uint8_t *c_init;
	size_t c_init_size;
	const uint8_t *s_reply;
	size_t s_reply_size;
	const uint8_t *k;
};

/*
 * Appends a KEXINIT payload (message number, 16 random cookie bytes from
 * ssh_random, the ten lists above for server 1 or 0, with the strict
 * markers when initial is 1, first_kex_packet_follows FALSE, reserved
 * uint32 0) to out.
 *
 * Returns 0, EIO (no random bytes) or the error recorded in out.
 */
int ssh_kexinit_put(int server, int initial, struct ssh_buf *out);

/*
 * Reads a peer's KEXINIT payload of size bytes (message number included).
 *
 * Every list must pass ssh_namelist_check; a payload longer than
 * SSH_KEXINIT_MAX is EMSGSIZE; a short or overlong payload is EINVAL (the
 * reserved field must be followed by nothing).  Returns 0, EINVAL or
 * EMSGSIZE.
 */
int ssh_kexinit_parse(const uint8_t *payload, size_t size, struct ssh_kexinit *kexinit);

/*
 * Decides the algorithms from the peer's KEXINIT.
 *
 * server is 1 when this side is the server; initial is 1 for the first
 * exchange of the connection (only then are strict markers looked at, and
 * choice->strict set; a later exchange leaves choice->strict as it was).
 * A server's peer must list SSH_KEX_NAME; a client takes the first of
 * SSH_KEX_NAME, SSH_KEX_CURVE25519_NAME and SSH_KEX_CURVE25519_OLD_NAME
 * that the server lists, into choice->method.  The peer must also list
 * SSH_KEX_HOSTKEY_NAME, SSH_CIPHER_NAME in both directions and "none"
 * compression in both directions (ENOTSUP otherwise: the caller
 * disconnects with SSH_DISCONNECT_KEY_EXCHANGE_FAILED naming the kind).
 * The MAC lists are not looked at.  In the initial exchange the peer must
 * carry a strict marker of its side, the standard or the pre-standard
 * name (this side sends both, so either pairs; EPROTO when it carries
 * neither).
 *
 * Returns 0, ENOTSUP or EPROTO; on a failure choice is left as it was.
 * kind (may be NULL) receives the name of the kind that failed, for the
 * message.
 */
int ssh_kex_choose(int server, int initial, const struct ssh_kexinit *peer, struct ssh_kex_choice *choice, const char **kind);

/* Appends SSH_MSG_KEX_HYBRID_INIT with C_INIT (SSH_HYBRID_INIT_SIZE bytes) as a string to out. */
void ssh_kex_put_hybrid_init(const uint8_t *c_init, struct ssh_buf *out);

/*
 * Reads SSH_MSG_KEX_HYBRID_INIT: *c_init and *c_init_size point at the
 * string inside payload.  The size is not checked here (ssh_hybrid_server
 * checks it).  Returns 0 or EINVAL (malformed, trailing bytes).
 */
int ssh_kex_parse_hybrid_init(const uint8_t *payload, size_t size, const uint8_t **c_init, size_t *c_init_size);

/*
 * Appends SSH_MSG_KEX_HYBRID_REPLY: string K_S (the host key blob), string
 * S_REPLY, string signature (the signature blob over H), to out.
 */
void ssh_kex_put_hybrid_reply(const uint8_t *host_key, size_t host_key_size, const uint8_t *s_reply, const uint8_t *signature, size_t signature_size, struct ssh_buf *out);

/*
 * Reads SSH_MSG_KEX_HYBRID_REPLY: pointers into payload for K_S, S_REPLY
 * and the signature blob.  Returns 0 or EINVAL.
 */
int ssh_kex_parse_hybrid_reply(const uint8_t *payload, size_t size, const uint8_t **host_key, size_t *host_key_size, const uint8_t **s_reply, size_t *s_reply_size, const uint8_t **signature, size_t *signature_size);

/* Appends SSH_MSG_KEX_ECDH_INIT with Q_C (SSH_X25519_SIZE bytes) as a string to out (RFC 5656 4, RFC 8731 3). */
void ssh_kex_put_ecdh_init(const uint8_t *q_c, struct ssh_buf *out);

/*
 * Reads SSH_MSG_KEX_ECDH_REPLY (RFC 5656 4): pointers into payload for
 * K_S, Q_S and the signature blob.  The sizes are not checked here.
 * Returns 0 or EINVAL.
 */
int ssh_kex_parse_ecdh_reply(const uint8_t *payload, size_t size, const uint8_t **host_key, size_t *host_key_size, const uint8_t **q_s, size_t *q_s_size, const uint8_t **signature, size_t *signature_size);

/*
 * Computes the exchange hash H = SHA-256(string V_C, string V_S, string
 * I_C, string I_S, string K_S, string C_INIT, string S_REPLY, string K)
 * (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5) into h (32 bytes).
 *
 * Returns 0, ENOMEM or EIO (h wiped).  Constant time in k (lengths are
 * public).  The same as ssh_kex_hash_form with SSH_KEX_SECRET_STRING.
 */
int ssh_kex_hash(const struct ssh_kex_hash_input *input, uint8_t *h);

/*
 * Computes the exchange hash as ssh_kex_hash does, with K encoded in form:
 * SSH_KEX_SECRET_MPINT gives H = SHA-256(string V_C, string V_S, string
 * I_C, string I_S, string K_S, string Q_C, string Q_S, mpint K) of
 * curve25519-sha256 (RFC 8731 3.1, RFC 5656 4), with Q_C and Q_S in c_init
 * and s_reply.  The mpint's length depends on the leading bytes of K, as
 * in every implementation of the method; the bytes are fed as they are.
 * Returns 0, EINVAL (a form out of range), ENOMEM or EIO (h wiped).
 */
int ssh_kex_hash_form(const struct ssh_kex_hash_input *input, enum ssh_kex_secret_form form, uint8_t *h);

/*
 * Derives size bytes of key material for one letter (RFC 4253 7.2): with
 * K encoded as a string (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5:
 * uint32 32, then the 32 bytes), K1 = HASH(K || H || letter ||
 * session_id), Kn = HASH(K || H || K1 || ... || K(n-1)), and the output
 * is the first size bytes of K1 || K2 || ...
 *
 * letter is 'A' to 'F'; with this cipher only 'C' (client to server
 * encryption) and 'D' (server to client) are used, each 64 bytes
 * (SSH_CHACHAPOLY_KEY).  size is 1 to 256 (EINVAL otherwise).  session_id
 * is the H of the connection's first exchange (32 bytes).  Returns 0,
 * EINVAL, ENOMEM or EIO (out wiped).  The same as ssh_kex_derive_form
 * with SSH_KEX_SECRET_STRING.
 */
int ssh_kex_derive(const uint8_t *k, const uint8_t *h, const uint8_t *session_id, char letter, uint8_t *out, size_t size);

/*
 * Derives key material as ssh_kex_derive does, with K encoded in form
 * (RFC 4253 7.2 "K is encoded as mpint" for curve25519-sha256, RFC 8731
 * 3.1).  Returns 0, EINVAL (also a form out of range), ENOMEM or EIO (out
 * wiped).
 */
int ssh_kex_derive_form(const uint8_t *k, enum ssh_kex_secret_form form, const uint8_t *h, const uint8_t *session_id, char letter, uint8_t *out, size_t size);

#endif
