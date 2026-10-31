/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages and the arithmetic of the key exchange.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-kex.h"

/*
 * Appends a KEXINIT payload (message number, 16 random cookie bytes from
 * ssh_random, the ten lists above for server 1 or 0, with the strict
 * markers when initial is 1, first_kex_packet_follows FALSE, reserved
 * uint32 0) to out.
 */
int
ssh_kexinit_put(
	int server,
	int initial,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)server;
	(void)initial;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a peer's KEXINIT payload of size bytes (message number
 * included).
 */
int
ssh_kexinit_parse(
	const uint8_t *payload,
	size_t size,
	struct ssh_kexinit *kexinit)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)kexinit;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decides the algorithms from the peer's KEXINIT.
 */
int
ssh_kex_choose(
	int server,
	int initial,
	const struct ssh_kexinit *peer,
	struct ssh_kex_choice *choice,
	const char **kind)
{
	/* Marks the arguments used until the function is implemented. */
	(void)server;
	(void)initial;
	(void)peer;
	(void)choice;
	(void)kind;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends SSH_MSG_KEX_HYBRID_INIT with C_INIT (SSH_HYBRID_INIT_SIZE
 * bytes) as a string to out.
 */
void
ssh_kex_put_hybrid_init(
	const uint8_t *c_init,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)c_init;
	(void)out;
}

/*
 * Reads SSH_MSG_KEX_HYBRID_INIT: *c_init and *c_init_size point at the
 * string inside payload.
 */
int
ssh_kex_parse_hybrid_init(
	const uint8_t *payload,
	size_t size,
	const uint8_t **c_init,
	size_t *c_init_size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)c_init;
	(void)c_init_size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends SSH_MSG_KEX_HYBRID_REPLY: string K_S (the host key blob),
 * string S_REPLY, string signature (the signature blob over H), to out.
 */
void
ssh_kex_put_hybrid_reply(
	const uint8_t *host_key,
	size_t host_key_size,
	const uint8_t *s_reply,
	const uint8_t *signature,
	size_t signature_size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)host_key;
	(void)host_key_size;
	(void)s_reply;
	(void)signature;
	(void)signature_size;
	(void)out;
}

/*
 * Reads SSH_MSG_KEX_HYBRID_REPLY: pointers into payload for K_S, S_REPLY
 * and the signature blob.
 */
int
ssh_kex_parse_hybrid_reply(
	const uint8_t *payload,
	size_t size,
	const uint8_t **host_key,
	size_t *host_key_size,
	const uint8_t **s_reply,
	size_t *s_reply_size,
	const uint8_t **signature,
	size_t *signature_size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)host_key;
	(void)host_key_size;
	(void)s_reply;
	(void)s_reply_size;
	(void)signature;
	(void)signature_size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Computes the exchange hash H = SHA-256(string V_C, string V_S, string
 * I_C, string I_S, string K_S, string C_INIT, string S_REPLY, string K)
 * (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5) into h (32 bytes).
 */
int
ssh_kex_hash(
	const struct ssh_kex_hash_input *input,
	uint8_t *h)
{
	/* Marks the arguments used until the function is implemented. */
	(void)input;
	(void)h;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Derives size bytes of key material for one letter (RFC 4253 7.2): with
 * K encoded as a string (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5: uint32
 * 32, then the 32 bytes), K1 = HASH(K || H || letter || session_id), Kn
 * = HASH(K || H || K1 || ...
 */
int
ssh_kex_derive(
	const uint8_t *k,
	const uint8_t *h,
	const uint8_t *session_id,
	char letter,
	uint8_t *out,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)k;
	(void)h;
	(void)session_id;
	(void)letter;
	(void)out;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
