/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TLS 1.3 server handshake state machine and ECDSA P-256 certificate/signing
 * helpers for libssl-compat.
 */

#include "sslc-hs.h"

#include "../libcrypto-compat/cryc-ecp.h"
#include "../libcrypto-compat/cryc-pem.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int srv_parse_ch(const uint8_t *body, size_t len, uint8_t *sid, size_t *sid_len, uint16_t *group, const uint8_t **peer_key, size_t *peer_key_len);
static int srv_ecdsa_p256_sign(const uint8_t *priv32, const uint8_t *hash32, cryc_random_fn rnd, void *rnd_ctx, uint8_t *der, size_t *der_len);
static size_t der_encode_ecdsa_sig(const uint8_t *r32, const uint8_t *s32, uint8_t *out);
static size_t der_encode_int(const uint8_t *val32, uint8_t *out);
static int srv_send_msg(struct sslc_hs *hs, struct sslc_hs_actions *actions, uint8_t type, const uint8_t *body, size_t body_len);

/*
 * Processes one incoming client handshake message on a TLS 1.3 server.
 */
int
sslc_srv_hs_message(
	struct sslc_hs *hs,
	const struct sslc_hs_config *config,
	uint8_t type,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	const uint8_t *body;
	size_t body_len;
	uint8_t sid[32];
	size_t sid_len;
	uint16_t group;
	const uint8_t *peer_key;
	size_t peer_key_len;
	uint8_t srv_rand[32];
	uint8_t shared[SSLC_HS_SHARED_MAX];
	size_t shared_len;
	uint8_t srv_pub[CRYC_ECP_MAX_POINT];
	size_t srv_pub_len;
	uint8_t sh[256];
	size_t sh_pos;
	uint8_t thash[CRYC_HASH_MAX_SIZE];
	struct sslc_traffic_keys keys;
	const struct sslc_suite *suite;
	uint8_t cert_msg[4096];
	size_t cm_pos;
	uint8_t cv_msg[256];
	size_t cv_pos;
	uint8_t cv_input[130];
	uint8_t cv_hash[32];
	uint8_t sig_der[80];
	size_t sig_len;
	uint8_t fin_msg[64];
	uint8_t expected_fin[CRYC_HASH_MAX_SIZE];
	size_t hlen;
	struct cryc_ecp ec;
	int error;

	memset(actions, 0, sizeof(*actions));
	if (size < 4U) {
		hs->alert = SSLC_ALERT_DECODE_ERROR;
		return EINVAL;
	}

	body = message + 4U;
	body_len = size - 4U;

	if (hs->state == SSLC_HS_START) {
		if (type != SSLC_HS_CLIENT_HELLO) {
			hs->alert = SSLC_ALERT_UNEXPECTED_MESSAGE;
			return EPROTO;
		}

		hs->config = config;
		suite = sslc_tls_suite(0x1301U);
		if (suite == NULL) {
			hs->alert = SSLC_ALERT_INTERNAL_ERROR;
			return EINVAL;
		}

		hs->version = SSLC_TLS13;
		hs->suite = suite;

		sid_len = 0U;
		group = 0U;
		peer_key = NULL;
		peer_key_len = 0U;
		error = srv_parse_ch(body, body_len, sid, &sid_len, &group,
				     &peer_key, &peer_key_len);
		if (error != 0) {
			hs->alert = SSLC_ALERT_HANDSHAKE_FAILURE;
			return error;
		}

		if (config->random == NULL ||
		    config->random(config->random_context, srv_rand, 32U) != 0) {
			hs->alert = SSLC_ALERT_INTERNAL_ERROR;
			return EIO;
		}

		/* Generates server ephemeral key share and computes shared secret. */
		if (group == 0x001DU) {
			if (cryc_x25519_keygen(hs->x25519_private,
					       hs->x25519_public,
					       config->random,
					       config->random_context) != 0) {
				hs->alert = SSLC_ALERT_INTERNAL_ERROR;
				return EIO;
			}

			if (cryc_x25519(shared, hs->x25519_private,
					peer_key) != 0) {
				hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
				return EPROTO;
			}

			shared_len = CRYC_X25519_SIZE;
			memcpy(srv_pub, hs->x25519_public, CRYC_X25519_SIZE);
			srv_pub_len = CRYC_X25519_SIZE;
		} else {
			struct cryc_ecp_point pt;

			if (cryc_ecp_init(&ec, CRYC_CURVE_P256) != 0) {
				hs->alert = SSLC_ALERT_INTERNAL_ERROR;
				return EINVAL;
			}

			if (cryc_ecc_keygen(&ec, hs->ec_private, srv_pub,
					    config->random,
					    config->random_context) != 0) {
				hs->alert = SSLC_ALERT_INTERNAL_ERROR;
				return EIO;
			}

			srv_pub_len = 1U + 2U * ec.size;
			if (cryc_ecp_decode(&ec, &pt, peer_key,
					    peer_key_len) != 0 ||
			    cryc_ecc_ecdh(&ec, hs->ec_private, peer_key,
					  peer_key_len, shared) != 0) {
				hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
				return EPROTO;
			}

			shared_len = ec.size;
		}

		/* Builds ServerHello body. */
		sh_pos = 0U;
		sh[sh_pos++] = 0x03U;
		sh[sh_pos++] = 0x03U;
		memcpy(sh + sh_pos, srv_rand, 32U);
		sh_pos += 32U;
		sh[sh_pos++] = (uint8_t)sid_len;
		if (sid_len > 0U) {
			memcpy(sh + sh_pos, sid, sid_len);
			sh_pos += sid_len;
		}

		sh[sh_pos++] = 0x13U;
		sh[sh_pos++] = 0x01U;
		sh[sh_pos++] = 0x00U;

		/* Extensions: supported_versions (6B) + key_share (8+pub). */
		{
			size_t ext_len;

			ext_len = 6U + 4U + 4U + srv_pub_len;
			sh[sh_pos++] = (uint8_t)(ext_len >> 8);
			sh[sh_pos++] = (uint8_t)(ext_len & 0xFFU);

			/* supported_versions: 0x002B, len=2, 0x0304. */
			sh[sh_pos++] = 0x00U;
			sh[sh_pos++] = 0x2BU;
			sh[sh_pos++] = 0x00U;
			sh[sh_pos++] = 0x02U;
			sh[sh_pos++] = 0x03U;
			sh[sh_pos++] = 0x04U;

			/* key_share: 0x0033, len=4+pub, group, pub_len, pub. */
			sh[sh_pos++] = 0x00U;
			sh[sh_pos++] = 0x33U;
			sh[sh_pos++] = (uint8_t)((4U + srv_pub_len) >> 8);
			sh[sh_pos++] = (uint8_t)((4U + srv_pub_len) & 0xFFU);
			sh[sh_pos++] = (uint8_t)(group >> 8);
			sh[sh_pos++] = (uint8_t)(group & 0xFFU);
			sh[sh_pos++] = (uint8_t)(srv_pub_len >> 8);
			sh[sh_pos++] = (uint8_t)(srv_pub_len & 0xFFU);
			memcpy(sh + sh_pos, srv_pub, srv_pub_len);
			sh_pos += srv_pub_len;
		}

		sslc_transcript_init(&hs->transcript);
		(void)sslc_transcript_select(&hs->transcript, CRYC_HASH_SHA256);
		sslc_transcript_update(&hs->transcript, message, size);

		error = srv_send_msg(hs, actions, SSLC_HS_SERVER_HELLO, sh,
				     sh_pos);
		if (error != 0)
			return error;

		error = sslc_hs_queue_action(actions, SSLC_HS_SEND_CCS, 0, NULL,
					     0U);
		if (error != 0)
			return error;

		hlen = sslc_transcript_hash(&hs->transcript, thash);
		if (hlen == 0U ||
		    sslc_tls13_start(&hs->schedule, CRYC_HASH_SHA256) != 0 ||
		    sslc_tls13_handshake(&hs->schedule, shared, shared_len,
					 thash) != 0) {
			hs->alert = SSLC_ALERT_INTERNAL_ERROR;
			return EINVAL;
		}

		/* Installs handshake write and read keys. */
		(void)sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite,
					      hs->schedule.server_handshake,
					      &keys);
		(void)sslc_hs_queue_action(actions, SSLC_HS_SET_WRITE_KEYS, 1,
					   &keys, 0U);
		(void)sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite,
					      hs->schedule.client_handshake,
					      &keys);
		(void)sslc_hs_queue_action(actions, SSLC_HS_SET_READ_KEYS, 1,
					   &keys, 0U);

		/* EncryptedExtensions (empty: 2-byte length 0). */
		{
			uint8_t ee[2];

			ee[0] = 0x00U;
			ee[1] = 0x00U;
			error = srv_send_msg(hs, actions,
					     SSLC_HS_ENCRYPTED_EXTENSIONS, ee,
					     2U);
			if (error != 0)
				return error;
		}

		/* Certificate message carrying single server DER certificate. */
		if (config->cert_der == NULL || config->cert_der_len == 0U ||
		    config->cert_der_len + 16U > sizeof(cert_msg) ||
		    config->ec_priv == NULL || config->ec_priv_len != 32U) {
			hs->alert = SSLC_ALERT_INTERNAL_ERROR;
			return EINVAL;
		}

		cm_pos = 0U;
		cert_msg[cm_pos++] = 0x00U;
		{
			size_t list_len;

			list_len = 3U + config->cert_der_len + 2U;
			cert_msg[cm_pos++] = (uint8_t)(list_len >> 16);
			cert_msg[cm_pos++] = (uint8_t)((list_len >> 8) & 0xFFU);
			cert_msg[cm_pos++] = (uint8_t)(list_len & 0xFFU);
		}

		cert_msg[cm_pos++] = (uint8_t)(config->cert_der_len >> 16);
		cert_msg[cm_pos++] = (uint8_t)((config->cert_der_len >> 8) & 0xFFU);
		cert_msg[cm_pos++] = (uint8_t)(config->cert_der_len & 0xFFU);
		memcpy(cert_msg + cm_pos, config->cert_der,
		       config->cert_der_len);
		cm_pos += config->cert_der_len;
		cert_msg[cm_pos++] = 0x00U;
		cert_msg[cm_pos++] = 0x00U;

		error = srv_send_msg(hs, actions, SSLC_HS_CERTIFICATE,
				     cert_msg, cm_pos);
		if (error != 0)
			return error;

		/* CertificateVerify using ECDSA-P256-SHA256 (0x0403). */
		(void)sslc_transcript_hash(&hs->transcript, thash);
		memset(cv_input, 0x20U, 64U);
		memcpy(cv_input + 64U, "TLS 1.3, server CertificateVerify",
		       33U);
		cv_input[97] = 0x00U;
		memcpy(cv_input + 98U, thash, 32U);
		cryc_hash_digest(CRYC_HASH_SHA256, cv_input, 130U, cv_hash);

		sig_len = sizeof(sig_der);
		if (srv_ecdsa_p256_sign(config->ec_priv, cv_hash,
					config->random, config->random_context,
					sig_der, &sig_len) != 0) {
			hs->alert = SSLC_ALERT_INTERNAL_ERROR;
			return EINVAL;
		}

		cv_pos = 0U;
		cv_msg[cv_pos++] = 0x04U;
		cv_msg[cv_pos++] = 0x03U;
		cv_msg[cv_pos++] = (uint8_t)(sig_len >> 8);
		cv_msg[cv_pos++] = (uint8_t)(sig_len & 0xFFU);
		memcpy(cv_msg + cv_pos, sig_der, sig_len);
		cv_pos += sig_len;

		error = srv_send_msg(hs, actions, SSLC_HS_CERTIFICATE_VERIFY,
				     cv_msg, cv_pos);
		if (error != 0)
			return error;

		/* Server Finished. */
		(void)sslc_transcript_hash(&hs->transcript, thash);
		(void)sslc_tls13_finished(CRYC_HASH_SHA256,
					  hs->schedule.server_handshake, thash,
					  fin_msg);
		error = srv_send_msg(hs, actions, SSLC_HS_FINISHED, fin_msg,
				     32U);
		if (error != 0)
			return error;

		(void)sslc_transcript_hash(&hs->transcript, thash);
		(void)sslc_tls13_master(&hs->schedule, thash);
		(void)sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite,
					      hs->schedule.server_application,
					      &keys);
		(void)sslc_hs_queue_action(actions, SSLC_HS_SET_WRITE_KEYS, 1,
					   &keys, 0U);

		hs->state = SSLC_HS13_WAIT_FINISHED;
		return 0;
	}

	if (hs->state == SSLC_HS13_WAIT_FINISHED) {
		if (type != SSLC_HS_FINISHED || body_len != 32U) {
			hs->alert = SSLC_ALERT_UNEXPECTED_MESSAGE;
			return EPROTO;
		}

		(void)sslc_transcript_hash(&hs->transcript, thash);
		(void)sslc_tls13_finished(CRYC_HASH_SHA256,
					  hs->schedule.client_handshake, thash,
					  expected_fin);
		if (memcmp(body, expected_fin, 32U) != 0) {
			hs->alert = SSLC_ALERT_DECRYPT_ERROR;
			return EBADMSG;
		}

		sslc_transcript_update(&hs->transcript, message, size);
		(void)sslc_tls13_traffic_keys(CRYC_HASH_SHA256, hs->suite,
					      hs->schedule.client_application,
					      &keys);
		(void)sslc_hs_queue_action(actions, SSLC_HS_SET_READ_KEYS, 1,
					   &keys, 0U);
		(void)sslc_hs_queue_action(actions, SSLC_HS_DONE, 1, NULL, 0U);
		hs->state = SSLC_HS13_CONNECTED;
		return 0;
	}

	hs->alert = SSLC_ALERT_UNEXPECTED_MESSAGE;
	return EPROTO;
}

/*
 * Generates an ephemeral ECDSA P-256 keypair and minimal self-signed X.509
 * v3 DER certificate for cn.
 */
int
sslc_srv_generate_self_signed(
	const char *cn,
	cryc_random_fn rnd,
	void *rnd_ctx,
	uint8_t **out_cert_der,
	size_t *out_cert_len,
	uint8_t **out_priv32)
{
	struct cryc_ecp ec;
	uint8_t *priv;
	uint8_t pub[65];
	uint8_t tbs[512];
	size_t pos;
	uint8_t hash[32];
	uint8_t sig[80];
	size_t sig_len;
	uint8_t *cert;
	size_t cpos;
	size_t cn_len;
	static const uint8_t alg_ecdsa_sha256[12] = {
		0x30U, 0x0AU, 0x06U, 0x08U, 0x2AU, 0x86U, 0x48U, 0xCEU,
		0x3DU, 0x04U, 0x03U, 0x02U
	};
	static const uint8_t validity[32] = {
		0x30U, 0x1EU,
		0x17U, 0x0DU, '2', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z',
		0x17U, 0x0DU, '3', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'
	};
	static const uint8_t spki_hdr[26] = {
		0x30U, 0x59U, 0x30U, 0x13U, 0x06U, 0x07U, 0x2AU, 0x86U,
		0x48U, 0xCEU, 0x3DU, 0x02U, 0x01U, 0x06U, 0x08U, 0x2AU,
		0x86U, 0x48U, 0xCEU, 0x3DU, 0x03U, 0x01U, 0x07U, 0x03U,
		0x42U, 0x00U
	};
	size_t name_len;
	size_t tbs_body_len;

	if (cn == NULL || cn[0] == '\0')
		cn = "zedbsd";

	cn_len = strlen(cn);
	if (cn_len > 64U)
		cn_len = 64U;

	if (cryc_ecp_init(&ec, CRYC_CURVE_P256) != 0)
		return EINVAL;

	priv = (uint8_t *)malloc(32U);
	if (priv == NULL)
		return ENOMEM;

	if (cryc_ecc_keygen(&ec, priv, pub, rnd, rnd_ctx) != 0) {
		free(priv);
		return EIO;
	}

	/* Builds TBSCertificate content after outer SEQUENCE header (4 bytes). */
	pos = 4U;
	/* version [0] EXPLICIT INTEGER 2. */
	tbs[pos++] = 0xA0U;
	tbs[pos++] = 0x03U;
	tbs[pos++] = 0x02U;
	tbs[pos++] = 0x01U;
	tbs[pos++] = 0x02U;
	/* serialNumber INTEGER 1. */
	tbs[pos++] = 0x02U;
	tbs[pos++] = 0x01U;
	tbs[pos++] = 0x01U;
	/* signature AlgorithmIdentifier. */
	memcpy(tbs + pos, alg_ecdsa_sha256, sizeof(alg_ecdsa_sha256));
	pos += sizeof(alg_ecdsa_sha256);

	/* issuer Name (CN=cn). */
	name_len = 2U + 2U + 5U + 2U + cn_len;
	tbs[pos++] = 0x30U;
	tbs[pos++] = (uint8_t)name_len;
	tbs[pos++] = 0x31U;
	tbs[pos++] = (uint8_t)(name_len - 2U);
	tbs[pos++] = 0x30U;
	tbs[pos++] = (uint8_t)(name_len - 4U);
	tbs[pos++] = 0x06U;
	tbs[pos++] = 0x03U;
	tbs[pos++] = 0x55U;
	tbs[pos++] = 0x04U;
	tbs[pos++] = 0x03U;
	tbs[pos++] = 0x0CU;
	tbs[pos++] = (uint8_t)cn_len;
	memcpy(tbs + pos, cn, cn_len);
	pos += cn_len;

	/* validity. */
	memcpy(tbs + pos, validity, sizeof(validity));
	pos += sizeof(validity);

	/* subject Name (same CN=cn). */
	tbs[pos++] = 0x30U;
	tbs[pos++] = (uint8_t)name_len;
	tbs[pos++] = 0x31U;
	tbs[pos++] = (uint8_t)(name_len - 2U);
	tbs[pos++] = 0x30U;
	tbs[pos++] = (uint8_t)(name_len - 4U);
	tbs[pos++] = 0x06U;
	tbs[pos++] = 0x03U;
	tbs[pos++] = 0x55U;
	tbs[pos++] = 0x04U;
	tbs[pos++] = 0x03U;
	tbs[pos++] = 0x0CU;
	tbs[pos++] = (uint8_t)cn_len;
	memcpy(tbs + pos, cn, cn_len);
	pos += cn_len;

	/* subjectPublicKeyInfo (P-256 uncompressed 65 bytes). */
	memcpy(tbs + pos, spki_hdr, sizeof(spki_hdr));
	pos += sizeof(spki_hdr);
	memcpy(tbs + pos, pub, 65U);
	pos += 65U;

	{
		uint8_t *tbs_ptr;
		size_t tbs_len;

		tbs_body_len = pos - 4U;
		if (tbs_body_len < 128U) {
			tbs_ptr = tbs + 2U;
			tbs_ptr[0] = 0x30U;
			tbs_ptr[1] = (uint8_t)tbs_body_len;
			tbs_len = pos - 2U;
		} else if (tbs_body_len < 256U) {
			tbs_ptr = tbs + 1U;
			tbs_ptr[0] = 0x30U;
			tbs_ptr[1] = 0x81U;
			tbs_ptr[2] = (uint8_t)tbs_body_len;
			tbs_len = pos - 1U;
		} else {
			tbs_ptr = tbs;
			tbs_ptr[0] = 0x30U;
			tbs_ptr[1] = 0x82U;
			tbs_ptr[2] = (uint8_t)(tbs_body_len >> 8);
			tbs_ptr[3] = (uint8_t)(tbs_body_len & 0xFFU);
			tbs_len = pos;
		}

		cryc_hash_digest(CRYC_HASH_SHA256, tbs_ptr, tbs_len, hash);
		sig_len = sizeof(sig);
		if (srv_ecdsa_p256_sign(priv, hash, rnd, rnd_ctx, sig,
					&sig_len) != 0) {
			free(priv);
			return EINVAL;
		}

		cert = (uint8_t *)malloc(tbs_len + sizeof(alg_ecdsa_sha256) +
					 sig_len + 16U);
		if (cert == NULL) {
			free(priv);
			return ENOMEM;
		}

		cpos = 4U;
		memcpy(cert + cpos, tbs_ptr, tbs_len);
		cpos += tbs_len;
	}
	memcpy(cert + cpos, alg_ecdsa_sha256, sizeof(alg_ecdsa_sha256));
	cpos += sizeof(alg_ecdsa_sha256);
	cert[cpos++] = 0x03U;
	cert[cpos++] = (uint8_t)(sig_len + 1U);
	cert[cpos++] = 0x00U;
	memcpy(cert + cpos, sig, sig_len);
	cpos += sig_len;

	cert[0] = 0x30U;
	cert[1] = 0x82U;
	cert[2] = (uint8_t)((cpos - 4U) >> 8);
	cert[3] = (uint8_t)((cpos - 4U) & 0xFFU);

	*out_cert_der = cert;
	*out_cert_len = cpos;
	*out_priv32 = priv;
	return 0;
}

/* Parses a TLS 1.3 ClientHello for session_id and supported key_share. */
static int
srv_parse_ch(
	const uint8_t *body,
	size_t len,
	uint8_t *sid,
	size_t *sid_len,
	uint16_t *group,
	const uint8_t **peer_key,
	size_t *peer_key_len)
{
	size_t pos;
	size_t cs_len;
	size_t ext_len;
	size_t ext_end;
	size_t slen;
	uint8_t comp_len;

	if (len < 38U)
		return EINVAL;

	pos = 34U;
	slen = body[pos++];
	if (slen > 32U || pos + slen + 2U > len)
		return EINVAL;

	memcpy(sid, body + pos, slen);
	*sid_len = slen;
	pos += slen;

	cs_len = ((size_t)body[pos] << 8) | (size_t)body[pos + 1U];
	pos += 2U;
	if (pos + cs_len + 1U > len)
		return EINVAL;

	pos += cs_len;
	comp_len = body[pos++];
	if (pos + comp_len + 2U > len)
		return EINVAL;

	pos += comp_len;
	ext_len = ((size_t)body[pos] << 8) | (size_t)body[pos + 1U];
	pos += 2U;
	if (pos + ext_len > len)
		return EINVAL;

	ext_end = pos + ext_len;
	while (pos + 4U <= ext_end) {
		uint16_t etype;
		size_t elen;

		etype = ((uint16_t)body[pos] << 8) | (uint16_t)body[pos + 1U];
		elen = ((size_t)body[pos + 2U] << 8) | (size_t)body[pos + 3U];
		pos += 4U;
		if (pos + elen > ext_end)
			return EINVAL;

		if (etype == 0x0033U && elen >= 2U) {
			size_t ks_end;
			size_t kpos;

			ks_end = pos + elen;
			kpos = pos + 2U;
			while (kpos + 4U <= ks_end) {
				uint16_t grp;
				size_t klen;

				grp = ((uint16_t)body[kpos] << 8) |
				      (uint16_t)body[kpos + 1U];
				klen = ((size_t)body[kpos + 2U] << 8) |
				       (size_t)body[kpos + 3U];
				kpos += 4U;
				if (kpos + klen > ks_end)
					return EINVAL;

				if ((grp == 0x001DU && klen == 32U) ||
				    (grp == 0x0017U && klen == 65U)) {
					if (*group == 0U || grp == 0x001DU) {
						*group = grp;
						*peer_key = body + kpos;
						*peer_key_len = klen;
					}
				}

				kpos += klen;
			}
		}

		pos += elen;
	}

	if (*group == 0U)
		return EPROTO;

	return 0;
}

/* Signs a 32-byte digest using ECDSA P-256 and encodes DER (r, s). */
static int
srv_ecdsa_p256_sign(
	const uint8_t *priv32,
	const uint8_t *hash32,
	cryc_random_fn rnd,
	void *rnd_ctx,
	uint8_t *der,
	size_t *der_len)
{
	struct cryc_ecp ec;
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	uint32_t k[CRYC_ECP_MAX_LIMBS];
	uint32_t e[CRYC_ECP_MAX_LIMBS];
	uint32_t r[CRYC_ECP_MAX_LIMBS];
	uint32_t s[CRYC_ECP_MAX_LIMBS];
	uint32_t k_inv[CRYC_ECP_MAX_LIMBS];
	uint32_t r_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t d_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t rd[CRYC_ECP_MAX_LIMBS];
	uint32_t erd[CRYC_ECP_MAX_LIMBS];
	uint32_t kinv_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t s_mont[CRYC_ECP_MAX_LIMBS];
	struct cryc_ecp_point pt;
	uint8_t k_bytes[32];
	uint8_t rx_bytes[32];
	uint8_t s_bytes[32];
	int error;

	error = cryc_ecp_init(&ec, CRYC_CURVE_P256);
	if (error != 0)
		return error;

	cryc_bn_decode(d, ec.limbs, priv32, 32U);
	cryc_bn_decode(e, ec.limbs, hash32, 32U);
	cryc_bn_reduce_once(e, e, &ec.order);

	if (rnd == NULL || rnd(rnd_ctx, k_bytes, 32U) != 0)
		return EIO;

	k_bytes[0] &= 0x7FU;
	cryc_bn_decode(k, ec.limbs, k_bytes, 32U);
	if (cryc_bn_is_zero(k, ec.limbs) != 0)
		k[0] = 1U;

	cryc_ecp_mul_base(&ec, &pt, k);
	if (cryc_ecp_affine_x(&ec, &pt, rx_bytes) != 0)
		return EINVAL;

	cryc_bn_decode(r, ec.limbs, rx_bytes, 32U);
	cryc_bn_reduce_once(r, r, &ec.order);
	cryc_bn_encode(r, ec.limbs, rx_bytes, 32U);

	/* s = k^-1 * (e + r * d) mod n. */
	cryc_bn_to_mont(r_mont, r, &ec.order);
	cryc_bn_to_mont(d_mont, d, &ec.order);
	cryc_bn_mont_mul(rd, r_mont, d_mont, &ec.order);
	cryc_bn_from_mont(rd, rd, &ec.order);
	cryc_bn_mod_add(erd, e, rd, &ec.order);

	/* k^-1 mod n via Fermat's little theorem: k^(n - 2) mod n. */
	{
		static const uint8_t n_minus_2[32] = {
			0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
			0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
			0xBCU, 0xE6U, 0xFAU, 0xADU, 0xA7U, 0x17U, 0x9EU, 0x84U,
			0xF3U, 0xB9U, 0xCAU, 0xC2U, 0xFCU, 0x63U, 0x25U, 0x4FU
		};
		uint32_t base_m[CRYC_ECP_MAX_LIMBS];
		uint32_t acc_m[CRYC_ECP_MAX_LIMBS];
		unsigned bit;

		cryc_bn_to_mont(base_m, k, &ec.order);
		memset(acc_m, 0, sizeof(acc_m));
		acc_m[0] = 1U;
		cryc_bn_to_mont(acc_m, acc_m, &ec.order);
		for (bit = 0U; bit < 256U; bit++) {
			cryc_bn_mont_mul(acc_m, acc_m, acc_m, &ec.order);
			if (((n_minus_2[bit / 8U] >> (7U - (bit % 8U))) & 1U) != 0U)
				cryc_bn_mont_mul(acc_m, acc_m, base_m, &ec.order);
		}

		cryc_bn_from_mont(k_inv, acc_m, &ec.order);
	}
	cryc_bn_to_mont(kinv_mont, k_inv, &ec.order);
	cryc_bn_to_mont(erd, erd, &ec.order);
	cryc_bn_mont_mul(s_mont, kinv_mont, erd, &ec.order);
	cryc_bn_from_mont(s, s_mont, &ec.order);
	cryc_bn_encode(s, ec.limbs, s_bytes, 32U);

	*der_len = der_encode_ecdsa_sig(rx_bytes, s_bytes, der);
	return 0;
}

/* Encodes (r32, s32) into a DER SEQUENCE { INTEGER r, INTEGER s }. */
static size_t
der_encode_ecdsa_sig(
	const uint8_t *r32,
	const uint8_t *s32,
	uint8_t *out)
{
	uint8_t rbuf[36];
	uint8_t sbuf[36];
	size_t rlen;
	size_t slen;

	rlen = der_encode_int(r32, rbuf);
	slen = der_encode_int(s32, sbuf);
	out[0] = 0x30U;
	out[1] = (uint8_t)(rlen + slen);
	memcpy(out + 2U, rbuf, rlen);
	memcpy(out + 2U + rlen, sbuf, slen);
	return 2U + rlen + slen;
}

/* Encodes a 32-byte big-endian unsigned integer as a DER INTEGER TLV. */
static size_t
der_encode_int(
	const uint8_t *val32,
	uint8_t *out)
{
	size_t off;
	size_t len;
	size_t pos;

	off = 0U;
	while (off < 31U && val32[off] == 0U)
		off++;

	len = 32U - off;
	pos = 0U;
	out[pos++] = 0x02U;
	if ((val32[off] & 0x80U) != 0U) {
		out[pos++] = (uint8_t)(len + 1U);
		out[pos++] = 0x00U;
	} else {
		out[pos++] = (uint8_t)len;
	}

	memcpy(out + pos, val32 + off, len);
	return pos + len;
}

/* Prepends a 4-byte handshake header and queues the message. */
static int
srv_send_msg(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions,
	uint8_t type,
	const uint8_t *body,
	size_t body_len)
{
	uint8_t msg[4160];

	if (body_len + 4U > sizeof(msg))
		return ENOBUFS;

	msg[0] = type;
	msg[1] = (uint8_t)(body_len >> 16);
	msg[2] = (uint8_t)((body_len >> 8) & 0xFFU);
	msg[3] = (uint8_t)(body_len & 0xFFU);
	if (body_len > 0U)
		memcpy(msg + 4U, body, body_len);

	return sslc_hs_queue_message(hs, actions, msg, body_len + 4U);
}
