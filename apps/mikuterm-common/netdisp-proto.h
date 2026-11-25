/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display UDP discovery and encrypted offer wire protocol.
 * Shared across wayland and mikuterm (zedBSD, Win32, Android).
 */

#ifndef MIKUTERM_NETDISP_PROTO_H
#define MIKUTERM_NETDISP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETDISP_DISCOVERY_PORT	33890U
#define NETDISP_VERSION		1U
#define NETDISP_MSG_ADV		1U
#define NETDISP_MSG_OFFER	2U

#define NETDISP_DEVICE_ID_LEN	16U
#define NETDISP_NONCE_LEN	16U
#define NETDISP_KEY_LEN		32U
#define NETDISP_IV_LEN		12U
#define NETDISP_TAG_LEN		32U
#define NETDISP_MAX_NAME_LEN	63U

#define NETDISP_ADV_HDR_LEN	81U
#define NETDISP_ADV_MAX_LEN	(NETDISP_ADV_HDR_LEN + NETDISP_MAX_NAME_LEN)
#define NETDISP_OFFER_HDR_LEN	70U
#define NETDISP_PLAIN_HDR_LEN	20U
#define NETDISP_MAX_USER_LEN	64U
#define NETDISP_MAX_PASS_LEN	64U
#define NETDISP_OFFER_MAX_LEN	(NETDISP_OFFER_HDR_LEN + NETDISP_PLAIN_HDR_LEN + NETDISP_MAX_USER_LEN + NETDISP_MAX_PASS_LEN + NETDISP_TAG_LEN)

struct netdisp_adv {
	uint16_t flags;
	uint8_t device_id[NETDISP_DEVICE_ID_LEN];
	uint8_t nonce[NETDISP_NONCE_LEN];
	uint8_t pk[NETDISP_KEY_LEN];
	uint16_t reply_port;
	uint16_t pref_width;
	uint16_t pref_height;
	uint16_t pref_scale120;
	uint8_t name_len;
	char device_name[NETDISP_MAX_NAME_LEN + 1U];
};

struct netdisp_offer_plain {
	uint8_t nonce[NETDISP_NONCE_LEN];
	uint16_t rdp_port;
	char username[NETDISP_MAX_USER_LEN + 1U];
	char password[NETDISP_MAX_PASS_LEN + 1U];
};

/* Generates an ephemeral X25519 keypair and 16-byte nonce. Returns 0 on success. */
int netdisp_generate_keypair(uint8_t out_sk[NETDISP_KEY_LEN], uint8_t out_pk[NETDISP_KEY_LEN], uint8_t out_nonce[NETDISP_NONCE_LEN]);

/* Computes an 8-hex-digit visual verification fingerprint "XXXX-XXXX" from SHA-256(pk || nonce). */
void netdisp_fingerprint_hex(const uint8_t pk[NETDISP_KEY_LEN], const uint8_t nonce[NETDISP_NONCE_LEN], char out_hex[10]);

/* Formats a 16-byte device_id as 32 lowercase hex characters + NUL. */
void netdisp_device_id_hex(const uint8_t device_id[NETDISP_DEVICE_ID_LEN], char out_hex[33]);

/* Parses a 32-character hex string into a 16-byte device_id. Returns 0 on success. */
int netdisp_device_id_from_hex(const char *hex, uint8_t out_id[NETDISP_DEVICE_ID_LEN]);

/* Strips control characters and clamps a UTF-8 device name to NETDISP_MAX_NAME_LEN bytes. */
void netdisp_sanitize_name(const char *in, char out[NETDISP_MAX_NAME_LEN + 1U], uint8_t *out_len);

/* Encodes an ADV packet into out. Returns 0 on success. */
int netdisp_adv_encode(const struct netdisp_adv *adv, uint8_t *out, size_t out_cap, size_t *out_len);

/* Decodes and validates an ADV packet from in. Returns 0 on success. */
int netdisp_adv_decode(const uint8_t *in, size_t in_len, struct netdisp_adv *out);

/* Encrypts and MACs an OFFER packet for peer_pk using X25519 + HKDF-SHA256 + ChaCha20 + HMAC-SHA256. */
int netdisp_offer_encrypt(const uint8_t device_id[NETDISP_DEVICE_ID_LEN], const uint8_t peer_pk[NETDISP_KEY_LEN], const struct netdisp_offer_plain *plain, uint8_t *out, size_t out_cap, size_t *out_len);

/* Verifies MAC, decrypts, and validates nonce of an OFFER packet. Returns 0 on success. */
int netdisp_offer_decrypt(const uint8_t *in, size_t in_len, const uint8_t expected_device_id[NETDISP_DEVICE_ID_LEN], const uint8_t local_sk[NETDISP_KEY_LEN], const uint8_t local_pk[NETDISP_KEY_LEN], const uint8_t expected_nonce[NETDISP_NONCE_LEN], struct netdisp_offer_plain *out);

#ifdef __cplusplus
}
#endif

#endif
