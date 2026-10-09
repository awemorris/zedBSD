/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CTAP2 (ctap2.h; ws161-p004; FIDO CTAP 2.1 sections 6.1 to
 * 6.5): GetInfo, ClientPIN (retries, setPIN, changePIN, the PIN token by
 * getPinToken or getPinUvAuthTokenUsingPinWithPermissions),
 * MakeCredential, GetAssertion and Selection.
 *
 * Requests are maps with integer keys written in canonical order.  Answers
 * are read field by field: unknown keys are skipped, the known ones must
 * have the type CTAP2 gives them.  A new credential's authenticator data
 * is read here, not trusted from elsewhere: the relying party's hash, the
 * flags, the credential ID and an ES256 key on the curve.
 */

#include "ctap2.h"

#include "cbor.h"
#include "pin.h"
#include "verify.h"

#include <errno.h>
#include <string.h>

/* ClientPIN's subcommands and the keys of its request and answer. */
#define CTAP2_PIN_GET_RETRIES		0x01U
#define CTAP2_PIN_GET_KEY_AGREEMENT	0x02U
#define CTAP2_PIN_SET			0x03U
#define CTAP2_PIN_CHANGE		0x04U
#define CTAP2_PIN_GET_TOKEN		0x05U
#define CTAP2_PIN_GET_TOKEN_PERMISSIONS	0x09U

/* The COSE algorithm of the PIN/UV key agreement (ECDH-ES+HKDF-256). */
#define CTAP2_COSE_ECDH			(-25)

/* A PIN: at least 4 code points (or GetInfo's minimum), at most 63 bytes, padded to 64. */
#define CTAP2_PIN_MIN			4U
#define CTAP2_PIN_MAX			63U
#define CTAP2_PIN_PADDED		64U

/* The room of a request and of an answer. */
#define CTAP2_REQUEST_MAX		2048U

static int ctap2_call(struct pk_device *device, uint8_t command, const struct pk_cbor_writer *request,
    uint8_t *reply, size_t capacity, struct pk_cbor_reader *answer);
static int ctap2_hid_command(void *context, uint8_t command, const uint8_t *request, size_t size, uint8_t *reply,
    size_t capacity, size_t *reply_size, unsigned timeout_ms, pk_hid_keepalive_t keepalive, void *keepalive_context);
static int ctap2_hid_cancel(void *context);
static int ctap2_agree(struct pk_device *device, unsigned protocol, struct pk_pin_shared *shared);
static int ctap2_check_pin(const char *pin, uint32_t minimum, uint8_t *padded);
static int ctap2_map(struct pk_cbor_reader *reader, uint64_t *count);
static int ctap2_key(struct pk_cbor_reader *reader, uint64_t *key);
static int ctap2_text_is(const struct pk_cbor_item *item, const char *text);
static int ctap2_read_info_options(struct pk_cbor_reader *reader, struct pk_info *info);
static void ctap2_put_descriptor(struct pk_cbor_writer *writer, const uint8_t *id, size_t size);
static size_t ctap2_hmac_key_size(const struct pk_pin_shared *shared);

/* Starts a device on a transport, with the time a command may take. */
void
pk_device_init(
	struct pk_device *device,
	const struct pk_transport *transport,
	unsigned timeout_ms)
{
	/* The transport, nothing waiting, no status yet. */
	memset(device, 0, sizeof(*device));
	device->transport = *transport;
	device->timeout_ms = timeout_ms;
}

/* Makes a CTAPHID channel the transport of CTAP2 commands. */
int
pk_hid_transport(
	struct pk_transport *transport,
	struct pk_hid *hid)
{
	/* The channel and its two functions. */
	transport->context = hid;
	transport->command = ctap2_hid_command;
	transport->cancel = ctap2_hid_cancel;

	/* Succeeded: the transport. */
	return 0;
}

/* Asks the key what it is (authenticatorGetInfo). */
int
pk_ctap2_get_info(
	struct pk_device *device,
	struct pk_info *info)
{
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	uint8_t reply[PK_MESSAGE_MAX];
	uint64_t count;
	uint64_t key;
	uint64_t index;
	uint64_t entry;
	int error;

	/* The command, with no request. */
	memset(info, 0, sizeof(*info));
	error = ctap2_call(device, PK_CTAP2_GET_INFO, NULL, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;
	error = ctap2_map(&answer, &count);
	if (error != 0)
		return error;

	/* Each field the library uses; the others are skipped. */
	for (index = 0U; index < count; index++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			return error;
		switch (key) {
		case 0x01U:
			/* versions: an array of texts. */
			error = pk_cbor_read(&answer, &item);
			if (error != 0 || item.kind != PK_CBOR_ARRAY)
				return EBADMSG;
			for (entry = item.value; entry > 0U; entry--) {
				error = pk_cbor_read(&answer, &item);
				if (error != 0 || item.kind != PK_CBOR_TEXT)
					return EBADMSG;
				if (ctap2_text_is(&item, "FIDO_2_0"))
					info->versions |= PK_INFO_FIDO_2_0;
				else if (ctap2_text_is(&item, "FIDO_2_1"))
					info->versions |= PK_INFO_FIDO_2_1;
				else if (ctap2_text_is(&item, "U2F_V2"))
					info->versions |= PK_INFO_U2F_V2;
			}
			break;
		case 0x03U:
			/* aaguid: 16 bytes. */
			error = pk_cbor_read(&answer, &item);
			if (error != 0 || item.kind != PK_CBOR_BYTES || item.value != sizeof(info->aaguid))
				return EBADMSG;
			memcpy(info->aaguid, item.bytes, sizeof(info->aaguid));
			break;
		case 0x04U:
			/* options: a map of texts to booleans. */
			error = ctap2_read_info_options(&answer, info);
			if (error != 0)
				return error;
			break;
		case 0x05U:
		case 0x07U:
		case 0x08U:
		case 0x0dU:
			/* maxMsgSize, maxCredentialCountInList, maxCredentialIdLength, minPINLength. */
			error = pk_cbor_read(&answer, &item);
			if (error != 0 || item.kind != PK_CBOR_UNSIGNED || item.value > UINT32_MAX)
				return EBADMSG;
			if (key == 0x05U)
				info->max_message = (uint32_t)item.value;
			else if (key == 0x07U)
				info->max_credential_count = (uint32_t)item.value;
			else if (key == 0x08U)
				info->max_credential_id_length = (uint32_t)item.value;
			else
				info->min_pin_length = (uint32_t)item.value;
			break;
		case 0x06U:
			/* pinUvAuthProtocols: an array of numbers. */
			error = pk_cbor_read(&answer, &item);
			if (error != 0 || item.kind != PK_CBOR_ARRAY)
				return EBADMSG;
			for (entry = item.value; entry > 0U; entry--) {
				error = pk_cbor_read(&answer, &item);
				if (error != 0 || item.kind != PK_CBOR_UNSIGNED)
					return EBADMSG;
				if (item.value == 1U)
					info->pin_protocols |= PK_PIN_PROTOCOL_1;
				else if (item.value == 2U)
					info->pin_protocols |= PK_PIN_PROTOCOL_2;
			}
			break;
		default:
			/* A field the library does not use. */
			error = pk_cbor_skip(&answer);
			if (error != 0)
				return EBADMSG;
			break;
		}
	}

	/* A CTAP2 key without a protocol list has protocol 1 (CTAP 2.0). */
	if (info->pin_protocols == 0U && (info->options & PK_OPTION_CLIENT_PIN) != 0U)
		info->pin_protocols = PK_PIN_PROTOCOL_1;

	/* Succeeded: what the key is. */
	return 0;
}

/* Chooses the PIN/UV protocol: 2 when the key has it, else 1, else 0 (none). */
unsigned
pk_ctap2_choose_protocol(
	const struct pk_info *info)
{
	/* The newer one first. */
	if ((info->pin_protocols & PK_PIN_PROTOCOL_2) != 0U)
		return 2U;
	if ((info->pin_protocols & PK_PIN_PROTOCOL_1) != 0U)
		return 1U;

	/* None. */
	return 0U;
}

/* Asks how many wrong PINs the key still takes (getPINRetries). */
int
pk_ctap2_pin_retries(
	struct pk_device *device,
	unsigned protocol,
	unsigned *retries)
{
	struct pk_cbor_writer request;
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	uint8_t buffer[16];
	uint8_t reply[256];
	uint64_t count;
	uint64_t key;
	uint64_t index;
	int error;

	/* {1: protocol, 2: getPINRetries}. */
	pk_cbor_writer_init(&request, buffer, sizeof(buffer));
	pk_cbor_put_map(&request, 2U);
	pk_cbor_put_unsigned(&request, 0x01U);
	pk_cbor_put_unsigned(&request, protocol);
	pk_cbor_put_unsigned(&request, 0x02U);
	pk_cbor_put_unsigned(&request, CTAP2_PIN_GET_RETRIES);
	error = ctap2_call(device, PK_CTAP2_CLIENT_PIN, &request, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;

	/* {3: pinRetries}. */
	error = ctap2_map(&answer, &count);
	if (error != 0)
		return error;
	*retries = 0U;
	for (index = 0U; index < count; index++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			return error;
		if (key != 0x03U) {
			error = pk_cbor_skip(&answer);
			if (error != 0)
				return EBADMSG;
			continue;
		}
		error = pk_cbor_read(&answer, &item);
		if (error != 0 || item.kind != PK_CBOR_UNSIGNED || item.value > 255U)
			return EBADMSG;
		*retries = (unsigned)item.value;
	}

	/* Succeeded: the retries. */
	return 0;
}

/* Sets the key's first PIN (setPIN). */
int
pk_ctap2_set_pin(
	struct pk_device *device,
	unsigned protocol,
	const char *pin)
{
	struct pk_pin_shared shared;
	struct pk_cbor_writer request;
	struct pk_cbor_reader answer;
	uint8_t padded[CTAP2_PIN_PADDED];
	uint8_t new_pin[CTAP2_PIN_PADDED + PK_AES_BLOCK_SIZE];
	uint8_t mac[PK_SHA256_SIZE];
	uint8_t buffer[CTAP2_REQUEST_MAX];
	uint8_t reply[64];
	size_t new_pin_size;
	size_t mac_size;
	int error;

	/* A PIN that keeps the rules, padded. */
	error = ctap2_check_pin(pin, 0U, padded);
	if (error != 0)
		return error;

	/* The secret, the PIN under it, and its MAC. */
	error = ctap2_agree(device, protocol, &shared);
	if (error == 0)
		error = pk_pin_encrypt(&shared, padded, sizeof(padded), new_pin, &new_pin_size);
	if (error == 0)
		error = pk_pin_authenticate(protocol, shared.key, ctap2_hmac_key_size(&shared), new_pin, new_pin_size,
		    mac, &mac_size);
	pk_crypto_wipe(padded, sizeof(padded));
	if (error != 0) {
		pk_pin_wipe(&shared);
		return error;
	}

	/* {1: protocol, 2: setPIN, 3: keyAgreement, 4: pinUvAuthParam, 5: newPinEnc}. */
	pk_cbor_writer_init(&request, buffer, sizeof(buffer));
	pk_cbor_put_map(&request, 5U);
	pk_cbor_put_unsigned(&request, 0x01U);
	pk_cbor_put_unsigned(&request, protocol);
	pk_cbor_put_unsigned(&request, 0x02U);
	pk_cbor_put_unsigned(&request, CTAP2_PIN_SET);
	pk_cbor_put_unsigned(&request, 0x03U);
	pk_cbor_put_encoded(&request, shared.platform_cose, shared.platform_cose_size);
	pk_cbor_put_unsigned(&request, 0x04U);
	pk_cbor_put_bytes(&request, mac, mac_size);
	pk_cbor_put_unsigned(&request, 0x05U);
	pk_cbor_put_bytes(&request, new_pin, new_pin_size);
	pk_pin_wipe(&shared);
	error = ctap2_call(device, PK_CTAP2_CLIENT_PIN, &request, reply, sizeof(reply), &answer);
	pk_crypto_wipe(buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Succeeded: the key has its PIN. */
	return 0;
}

/* Changes the key's PIN (changePIN). */
int
pk_ctap2_change_pin(
	struct pk_device *device,
	unsigned protocol,
	const char *current,
	const char *pin)
{
	struct pk_pin_shared shared;
	struct pk_cbor_writer request;
	struct pk_cbor_reader answer;
	struct pk_crypto_part part;
	uint8_t padded[CTAP2_PIN_PADDED];
	uint8_t new_pin[CTAP2_PIN_PADDED + PK_AES_BLOCK_SIZE];
	uint8_t hash[PK_SHA256_SIZE];
	uint8_t hash_enc[PK_AES_BLOCK_SIZE * 2U];
	uint8_t both[sizeof(new_pin) + sizeof(hash_enc)];
	uint8_t mac[PK_SHA256_SIZE];
	uint8_t buffer[CTAP2_REQUEST_MAX];
	uint8_t reply[64];
	size_t new_pin_size;
	size_t hash_enc_size;
	size_t mac_size;
	int error;

	/* The new PIN keeps the rules; the current one's hash. */
	error = ctap2_check_pin(pin, 0U, padded);
	if (error != 0)
		return error;
	part.data = (const uint8_t *)current;
	part.size = strlen(current);
	error = pk_crypto_sha256(&part, 1U, hash);
	if (error != 0) {
		pk_crypto_wipe(padded, sizeof(padded));
		return error;
	}

	/* The secret, both PINs under it, and the MAC of both. */
	new_pin_size = 0U;
	hash_enc_size = 0U;
	error = ctap2_agree(device, protocol, &shared);
	if (error == 0)
		error = pk_pin_encrypt(&shared, padded, sizeof(padded), new_pin, &new_pin_size);
	if (error == 0)
		error = pk_pin_encrypt(&shared, hash, PK_AES_BLOCK_SIZE, hash_enc, &hash_enc_size);
	if (error == 0) {
		memcpy(both, new_pin, new_pin_size);
		memcpy(both + new_pin_size, hash_enc, hash_enc_size);
		error = pk_pin_authenticate(protocol, shared.key, ctap2_hmac_key_size(&shared), both,
		    new_pin_size + hash_enc_size, mac, &mac_size);
	}
	pk_crypto_wipe(padded, sizeof(padded));
	pk_crypto_wipe(hash, sizeof(hash));
	if (error != 0) {
		pk_pin_wipe(&shared);
		return error;
	}

	/* {1: protocol, 2: changePIN, 3: keyAgreement, 4: pinUvAuthParam, 5: newPinEnc, 6: pinHashEnc}. */
	pk_cbor_writer_init(&request, buffer, sizeof(buffer));
	pk_cbor_put_map(&request, 6U);
	pk_cbor_put_unsigned(&request, 0x01U);
	pk_cbor_put_unsigned(&request, protocol);
	pk_cbor_put_unsigned(&request, 0x02U);
	pk_cbor_put_unsigned(&request, CTAP2_PIN_CHANGE);
	pk_cbor_put_unsigned(&request, 0x03U);
	pk_cbor_put_encoded(&request, shared.platform_cose, shared.platform_cose_size);
	pk_cbor_put_unsigned(&request, 0x04U);
	pk_cbor_put_bytes(&request, mac, mac_size);
	pk_cbor_put_unsigned(&request, 0x05U);
	pk_cbor_put_bytes(&request, new_pin, new_pin_size);
	pk_cbor_put_unsigned(&request, 0x06U);
	pk_cbor_put_bytes(&request, hash_enc, hash_enc_size);
	pk_pin_wipe(&shared);
	error = ctap2_call(device, PK_CTAP2_CLIENT_PIN, &request, reply, sizeof(reply), &answer);
	pk_crypto_wipe(buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Succeeded: the key has the new PIN. */
	return 0;
}

/*
 * Gets a PIN token for the key's PIN: with permissions for a relying
 * party when the key has getPinUvAuthTokenUsingPinWithPermissions (CTAP
 * 2.1), else getPinToken (CTAP 2.0).  Returns 0 with the token, EPROTO
 * with the key's status (a wrong PIN: PIN_INVALID), or another error.
 */
int
pk_ctap2_pin_token(
	struct pk_device *device,
	const struct pk_info *info,
	unsigned protocol,
	const char *pin,
	unsigned permissions,
	const char *rp_id,
	uint8_t *token,
	size_t *token_size)
{
	struct pk_pin_shared shared;
	struct pk_cbor_writer request;
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	struct pk_crypto_part part;
	uint8_t hash[PK_SHA256_SIZE];
	uint8_t hash_enc[PK_AES_BLOCK_SIZE * 2U];
	uint8_t plain[64];
	uint8_t buffer[CTAP2_REQUEST_MAX];
	uint8_t reply[256];
	size_t hash_enc_size;
	size_t plain_size;
	uint64_t count;
	uint64_t key;
	uint64_t index;
	int with_permissions;
	int error;

	/* The PIN's hash, the first 16 bytes of it under the secret. */
	part.data = (const uint8_t *)pin;
	part.size = strlen(pin);
	error = pk_crypto_sha256(&part, 1U, hash);
	if (error != 0)
		return error;
	error = ctap2_agree(device, protocol, &shared);
	if (error == 0)
		error = pk_pin_encrypt(&shared, hash, PK_AES_BLOCK_SIZE, hash_enc, &hash_enc_size);
	pk_crypto_wipe(hash, sizeof(hash));
	if (error != 0) {
		pk_pin_wipe(&shared);
		return error;
	}

	/* {1: protocol, 2: the subcommand, 3: keyAgreement, 6: pinHashEnc[, 9: permissions, 10: rpId]}. */
	with_permissions = (info->options & PK_OPTION_PIN_UV_TOKEN) != 0U;
	pk_cbor_writer_init(&request, buffer, sizeof(buffer));
	pk_cbor_put_map(&request, with_permissions ? 6U : 4U);
	pk_cbor_put_unsigned(&request, 0x01U);
	pk_cbor_put_unsigned(&request, protocol);
	pk_cbor_put_unsigned(&request, 0x02U);
	pk_cbor_put_unsigned(&request, with_permissions ? CTAP2_PIN_GET_TOKEN_PERMISSIONS : CTAP2_PIN_GET_TOKEN);
	pk_cbor_put_unsigned(&request, 0x03U);
	pk_cbor_put_encoded(&request, shared.platform_cose, shared.platform_cose_size);
	pk_cbor_put_unsigned(&request, 0x06U);
	pk_cbor_put_bytes(&request, hash_enc, hash_enc_size);
	if (with_permissions) {
		pk_cbor_put_unsigned(&request, 0x09U);
		pk_cbor_put_unsigned(&request, permissions);
		pk_cbor_put_unsigned(&request, 0x0aU);
		pk_cbor_put_text(&request, rp_id, strlen(rp_id));
	}
	error = ctap2_call(device, PK_CTAP2_CLIENT_PIN, &request, reply, sizeof(reply), &answer);
	if (error == 0)
		error = ctap2_map(&answer, &count);
	if (error != 0) {
		pk_pin_wipe(&shared);
		return error;
	}

	/* {2: pinUvAuthToken under the secret}. */
	plain_size = 0U;
	for (index = 0U; index < count; index++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			break;
		if (key != 0x02U) {
			error = pk_cbor_skip(&answer);
			if (error != 0)
				break;
			continue;
		}
		error = pk_cbor_read(&answer, &item);
		if (error != 0 || item.kind != PK_CBOR_BYTES || item.value > sizeof(plain) + PK_AES_BLOCK_SIZE ||
		    item.value == 0U) {
			error = EBADMSG;
			break;
		}
		error = pk_pin_decrypt(&shared, item.bytes, (size_t)item.value, plain, &plain_size);
		if (error != 0)
			break;
	}
	pk_pin_wipe(&shared);
	if (error == 0 && (plain_size == 0U || plain_size > PK_PIN_TOKEN_MAX))
		error = EBADMSG;
	if (error != 0) {
		pk_crypto_wipe(plain, sizeof(plain));
		return error == EBADMSG ? EBADMSG : error;
	}

	/* Succeeded: the token. */
	memcpy(token, plain, plain_size);
	*token_size = plain_size;
	pk_crypto_wipe(plain, sizeof(plain));
	return 0;
}

/*
 * Makes a new credential (authenticatorMakeCredential): ES256 only, not
 * resident, verified with the PIN token when one is given.
 */
int
pk_ctap2_make_credential(
	struct pk_device *device,
	const struct pk_make_request *request,
	struct pk_made_credential *credential)
{
	struct pk_cbor_writer writer;
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	uint8_t buffer[CTAP2_REQUEST_MAX];
	uint8_t reply[PK_MESSAGE_MAX];
	uint8_t mac[PK_SHA256_SIZE];
	size_t mac_size;
	size_t pairs;
	size_t index;
	uint64_t count;
	uint64_t key;
	uint64_t field;
	int found;
	int error;

	/* The MAC of the client data hash under the PIN token. */
	mac_size = 0U;
	if (request->pin_token != NULL) {
		error = pk_pin_authenticate(request->pin_protocol, request->pin_token, request->pin_token_size,
		    request->client_data_hash, sizeof(request->client_data_hash), mac, &mac_size);
		if (error != 0)
			return error;
	}

	/* {1: clientDataHash, 2: rp, 3: user, 4: pubKeyCredParams[, 5: excludeList][, 8: pinUvAuthParam, 9: protocol]}. */
	pairs = 4U;
	if (request->exclude_count != 0U)
		pairs++;
	if (request->pin_token != NULL)
		pairs += 2U;
	pk_cbor_writer_init(&writer, buffer, sizeof(buffer));
	pk_cbor_put_map(&writer, pairs);
	pk_cbor_put_unsigned(&writer, 0x01U);
	pk_cbor_put_bytes(&writer, request->client_data_hash, sizeof(request->client_data_hash));
	pk_cbor_put_unsigned(&writer, 0x02U);
	pk_cbor_put_map(&writer, 1U);
	pk_cbor_put_text(&writer, "id", 2U);
	pk_cbor_put_text(&writer, request->rp_id, strlen(request->rp_id));
	pk_cbor_put_unsigned(&writer, 0x03U);
	pk_cbor_put_map(&writer, 2U);
	pk_cbor_put_text(&writer, "id", 2U);
	pk_cbor_put_bytes(&writer, request->user_id, request->user_id_size);
	pk_cbor_put_text(&writer, "name", 4U);
	pk_cbor_put_text(&writer, request->user_name, strlen(request->user_name));
	pk_cbor_put_unsigned(&writer, 0x04U);
	pk_cbor_put_array(&writer, 1U);
	pk_cbor_put_map(&writer, 2U);
	pk_cbor_put_text(&writer, "alg", 3U);
	pk_cbor_put_integer(&writer, -7);
	pk_cbor_put_text(&writer, "type", 4U);
	pk_cbor_put_text(&writer, "public-key", 10U);
	if (request->exclude_count != 0U) {
		pk_cbor_put_unsigned(&writer, 0x05U);
		pk_cbor_put_array(&writer, request->exclude_count);
		for (index = 0U; index < request->exclude_count; index++)
			ctap2_put_descriptor(&writer, request->exclude_ids[index], request->exclude_sizes[index]);
	}
	if (request->pin_token != NULL) {
		pk_cbor_put_unsigned(&writer, 0x08U);
		pk_cbor_put_bytes(&writer, mac, mac_size);
		pk_cbor_put_unsigned(&writer, 0x09U);
		pk_cbor_put_unsigned(&writer, request->pin_protocol);
	}
	error = ctap2_call(device, PK_CTAP2_MAKE_CREDENTIAL, &writer, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;
	error = ctap2_map(&answer, &count);
	if (error != 0)
		return error;

	/* {2: authData}; the format and the statement are not checked (no attestation, ws172). */
	found = 0;
	for (field = 0U; field < count; field++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			return error;
		if (key != 0x02U) {
			error = pk_cbor_skip(&answer);
			if (error != 0)
				return EBADMSG;
			continue;
		}
		error = pk_cbor_read(&answer, &item);
		if (error != 0 || item.kind != PK_CBOR_BYTES)
			return EBADMSG;
		error = pk_ctap2_read_made(request->rp_id, item.bytes, (size_t)item.value, credential);
		if (error != 0)
			return error;
		found = 1;
	}
	if (!found)
		return EBADMSG;

	/* Succeeded: the new credential. */
	return 0;
}

/*
 * Asks for an assertion (authenticatorGetAssertion) over the credentials
 * allowed: with the user present (presence), and verified with the PIN
 * token when one is given.  The credential used comes from the answer, or
 * is the only one allowed.
 */
int
pk_ctap2_get_assertion(
	struct pk_device *device,
	const struct pk_assertion_request *request,
	struct pk_assertion_reply *reply)
{
	struct pk_cbor_writer writer;
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	uint8_t buffer[CTAP2_REQUEST_MAX];
	uint8_t message[PK_MESSAGE_MAX];
	uint8_t mac[PK_SHA256_SIZE];
	size_t mac_size;
	size_t pairs;
	size_t index;
	uint64_t count;
	uint64_t inner;
	uint64_t key;
	uint64_t field;
	int error;

	/* The MAC of the client data hash under the PIN token. */
	memset(reply, 0, sizeof(*reply));
	mac_size = 0U;
	if (request->pin_token != NULL) {
		error = pk_pin_authenticate(request->pin_protocol, request->pin_token, request->pin_token_size,
		    request->client_data_hash, sizeof(request->client_data_hash), mac, &mac_size);
		if (error != 0)
			return error;
	}

	/* {1: rpId, 2: clientDataHash, 3: allowList[, 5: {"up": false}][, 6: pinUvAuthParam, 7: protocol]}. */
	pairs = 3U;
	if (!request->presence)
		pairs++;
	if (request->pin_token != NULL)
		pairs += 2U;
	pk_cbor_writer_init(&writer, buffer, sizeof(buffer));
	pk_cbor_put_map(&writer, pairs);
	pk_cbor_put_unsigned(&writer, 0x01U);
	pk_cbor_put_text(&writer, request->rp_id, strlen(request->rp_id));
	pk_cbor_put_unsigned(&writer, 0x02U);
	pk_cbor_put_bytes(&writer, request->client_data_hash, sizeof(request->client_data_hash));
	pk_cbor_put_unsigned(&writer, 0x03U);
	pk_cbor_put_array(&writer, request->allow_count);
	for (index = 0U; index < request->allow_count; index++)
		ctap2_put_descriptor(&writer, request->allow_ids[index], request->allow_sizes[index]);
	if (!request->presence) {
		pk_cbor_put_unsigned(&writer, 0x05U);
		pk_cbor_put_map(&writer, 1U);
		pk_cbor_put_text(&writer, "up", 2U);
		pk_cbor_put_bool(&writer, 0);
	}
	if (request->pin_token != NULL) {
		pk_cbor_put_unsigned(&writer, 0x06U);
		pk_cbor_put_bytes(&writer, mac, mac_size);
		pk_cbor_put_unsigned(&writer, 0x07U);
		pk_cbor_put_unsigned(&writer, request->pin_protocol);
	}
	error = ctap2_call(device, PK_CTAP2_GET_ASSERTION, &writer, message, sizeof(message), &answer);
	if (error != 0)
		return error;
	error = ctap2_map(&answer, &count);
	if (error != 0)
		return error;

	/* {1: credential {"id", "type"}, 2: authData, 3: signature}; the others are skipped. */
	for (field = 0U; field < count; field++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			return error;
		if (key == 0x01U) {
			/* The credential's descriptor: its "id". */
			error = ctap2_map(&answer, &inner);
			if (error != 0)
				return error;
			for (; inner > 0U; inner--) {
				error = pk_cbor_read(&answer, &item);
				if (error != 0 || item.kind != PK_CBOR_TEXT)
					return EBADMSG;
				if (!ctap2_text_is(&item, "id")) {
					error = pk_cbor_skip(&answer);
					if (error != 0)
						return EBADMSG;
					continue;
				}
				error = pk_cbor_read(&answer, &item);
				if (error != 0 || item.kind != PK_CBOR_BYTES || item.value > sizeof(reply->credential_id))
					return EBADMSG;
				memcpy(reply->credential_id, item.bytes, (size_t)item.value);
				reply->credential_id_size = (size_t)item.value;
			}
		} else if (key == 0x02U || key == 0x03U) {
			/* The authenticator data, or the signature. */
			error = pk_cbor_read(&answer, &item);
			if (error != 0 || item.kind != PK_CBOR_BYTES)
				return EBADMSG;
			if (key == 0x02U) {
				if (item.value > sizeof(reply->auth_data))
					return EBADMSG;
				memcpy(reply->auth_data, item.bytes, (size_t)item.value);
				reply->auth_data_size = (size_t)item.value;
			} else {
				if (item.value > sizeof(reply->signature))
					return EBADMSG;
				memcpy(reply->signature, item.bytes, (size_t)item.value);
				reply->signature_size = (size_t)item.value;
			}
		} else {
			error = pk_cbor_skip(&answer);
			if (error != 0)
				return EBADMSG;
		}
	}

	/* The credential is the only one allowed when the answer does not name it. */
	if (reply->credential_id_size == 0U && request->allow_count == 1U &&
	    request->allow_sizes[0] <= sizeof(reply->credential_id)) {
		memcpy(reply->credential_id, request->allow_ids[0], request->allow_sizes[0]);
		reply->credential_id_size = request->allow_sizes[0];
	}

	/* An answer without its parts is not one. */
	if (reply->credential_id_size == 0U || reply->auth_data_size == 0U ||
	    (request->presence && reply->signature_size == 0U))
		return EBADMSG;

	/* Succeeded: the assertion. */
	return 0;
}

/*
 * Resets the key (authenticatorReset, ws199-p001): every credential and
 * the PIN go.  The key takes it only soon after it was powered (a few
 * seconds) and with the user's touch.  Returns 0, or EPROTO with the
 * key's status in last_status (NOT_ALLOWED 0x30 out of the window,
 * OPERATION_DENIED 0x27, USER_ACTION_TIMEOUT 0x2F, KEEPALIVE_CANCEL 0x2D),
 * or another errno value.
 */
int
pk_ctap2_reset(
	struct pk_device *device)
{
	struct pk_cbor_reader answer;
	uint8_t reply[16];
	int error;

	/* The command, with no request and no answer but its status. */
	error = ctap2_call(device, PK_CTAP2_RESET, NULL, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;

	/* Succeeded: the key is as new. */
	return 0;
}

/* Asks the user to touch this key among several (authenticatorSelection, CTAP 2.1). */
int
pk_ctap2_selection(
	struct pk_device *device)
{
	struct pk_cbor_reader answer;
	uint8_t reply[16];
	int error;

	/* The command, with no request and no answer but its status. */
	error = ctap2_call(device, PK_CTAP2_SELECTION, NULL, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;

	/* Succeeded: the user touched this key. */
	return 0;
}

/*
 * Sends one command and reads its status: 0 with a reader on the answer's
 * CBOR (empty when the key sent only the status), EPROTO with the status in
 * last_status, EBADMSG for an answer that is not well-formed CBOR.
 */
static int
ctap2_call(
	struct pk_device *device,
	uint8_t command,
	const struct pk_cbor_writer *request,
	uint8_t *reply,
	size_t capacity,
	struct pk_cbor_reader *answer)
{
	const uint8_t *data;
	size_t data_size;
	size_t reply_size;
	size_t length;
	int error;

	/* The request (none for some commands), if it was all written. */
	data = NULL;
	data_size = 0U;
	if (request != NULL) {
		if (request->error != 0)
			return request->error;
		data = request->buffer;
		data_size = request->length;
	}

	/* The command, and the status first in its answer. */
	reply_size = 0U;
	error = device->transport.command(device->transport.context, command, data, data_size, reply, capacity,
	    &reply_size, device->timeout_ms, device->keepalive, device->keepalive_context);
	if (error != 0)
		return error;
	if (reply_size == 0U)
		return EBADMSG;
	device->last_status = reply[0];
	if (reply[0] != PK_CTAP2_OK)
		return EPROTO;

	/* The CBOR after the status: one well-formed item, or nothing. */
	pk_cbor_reader_init(answer, reply + 1U, reply_size - 1U);
	if (reply_size == 1U)
		return 0;
	error = pk_cbor_check(reply + 1U, reply_size - 1U, 0U, &length);
	if (error != 0 || length != reply_size - 1U)
		return EBADMSG;

	/* Succeeded: the answer. */
	return 0;
}

/* The CTAPHID transport's command: CBOR (0x90) with the command's byte before the request. */
static int
ctap2_hid_command(
	void *context,
	uint8_t command,
	const uint8_t *request,
	size_t size,
	uint8_t *reply,
	size_t capacity,
	size_t *reply_size,
	unsigned timeout_ms,
	pk_hid_keepalive_t keepalive,
	void *keepalive_context)
{
	uint8_t message[PK_HID_MESSAGE_MAX];
	uint8_t answer_command;
	int error;

	/* The command's byte, then the request. */
	if (size + 1U > sizeof(message))
		return EMSGSIZE;
	message[0] = command;
	if (size != 0U)
		memcpy(message + 1U, request, size);

	/* The transaction. */
	error = pk_hid_transact(context, PK_HID_CBOR, message, size + 1U, &answer_command, reply, capacity, reply_size,
	    timeout_ms, keepalive, keepalive_context);
	pk_crypto_wipe(message, size + 1U);
	if (error != 0)
		return error;

	/* An ERROR answer: a busy channel, or another failure of the transport. */
	if (answer_command == PK_HID_ERROR) {
		if (*reply_size >= 1U && reply[0] == PK_HID_ERR_CHANNEL_BUSY)
			return EBUSY;
		return EIO;
	}
	if (answer_command != PK_HID_CBOR)
		return EBADMSG;

	/* Succeeded: the status and the CBOR. */
	return 0;
}

/* The CTAPHID transport's cancel. */
static int
ctap2_hid_cancel(
	void *context)
{
	int error;

	/* CANCEL on the channel. */
	error = pk_hid_cancel(context);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/* Agrees a PIN/UV secret with the key (getKeyAgreement). */
static int
ctap2_agree(
	struct pk_device *device,
	unsigned protocol,
	struct pk_pin_shared *shared)
{
	struct pk_cbor_writer request;
	struct pk_cbor_reader answer;
	struct pk_cbor_item item;
	uint8_t buffer[16];
	uint8_t reply[256];
	uint8_t x[PK_P256_SIZE];
	uint8_t y[PK_P256_SIZE];
	const uint8_t *cose;
	uint64_t count;
	uint64_t key;
	uint64_t index;
	size_t start;
	int found;
	int error;

	/* {1: protocol, 2: getKeyAgreement}. */
	if (protocol != 1U && protocol != 2U)
		return EINVAL;
	pk_cbor_writer_init(&request, buffer, sizeof(buffer));
	pk_cbor_put_map(&request, 2U);
	pk_cbor_put_unsigned(&request, 0x01U);
	pk_cbor_put_unsigned(&request, protocol);
	pk_cbor_put_unsigned(&request, 0x02U);
	pk_cbor_put_unsigned(&request, CTAP2_PIN_GET_KEY_AGREEMENT);
	error = ctap2_call(device, PK_CTAP2_CLIENT_PIN, &request, reply, sizeof(reply), &answer);
	if (error != 0)
		return error;
	error = ctap2_map(&answer, &count);
	if (error != 0)
		return error;

	/* {1: the key's agreement key, COSE}. */
	found = 0;
	for (index = 0U; index < count; index++) {
		error = ctap2_key(&answer, &key);
		if (error != 0)
			return error;
		start = answer.offset;
		error = pk_cbor_skip(&answer);
		if (error != 0)
			return EBADMSG;
		if (key != 0x01U)
			continue;
		cose = answer.data + start;
		error = pk_cose_ec2(cose, answer.offset - start, CTAP2_COSE_ECDH, x, y);
		if (error != 0)
			return EBADMSG;
		found = 1;
	}
	if (!found)
		return EBADMSG;

	/* The secret. */
	(void)item;
	error = pk_pin_derive(protocol, x, y, shared);
	if (error != 0)
		return error;

	/* Succeeded: the secret is agreed. */
	return 0;
}

/*
 * Checks a new PIN against CTAP2's rules (at least minimum or 4 code
 * points, at most 63 bytes, no NUL) and pads it with zeros to 64 bytes.
 * Returns 0 or EINVAL.
 */
static int
ctap2_check_pin(
	const char *pin,
	uint32_t minimum,
	uint8_t *padded)
{
	size_t length;
	size_t index;
	size_t points;

	/* Its bytes. */
	length = strlen(pin);
	if (length > CTAP2_PIN_MAX)
		return EINVAL;

	/* Its code points: every byte that does not continue a UTF-8 sequence. */
	points = 0U;
	for (index = 0U; index < length; index++) {
		if (((uint8_t)pin[index] & 0xc0U) != 0x80U)
			points++;
	}
	if (minimum < CTAP2_PIN_MIN)
		minimum = CTAP2_PIN_MIN;
	if (points < minimum)
		return EINVAL;

	/* Padded with zeros. */
	memset(padded, 0, CTAP2_PIN_PADDED);
	memcpy(padded, pin, length);

	/* Succeeded: the padded PIN. */
	return 0;
}

/* Reads a map's head; returns 0 with its count, or EBADMSG for anything else. */
static int
ctap2_map(
	struct pk_cbor_reader *reader,
	uint64_t *count)
{
	struct pk_cbor_item item;
	int error;

	/* A map. */
	error = pk_cbor_read(reader, &item);
	if (error != 0 || item.kind != PK_CBOR_MAP)
		return EBADMSG;

	/* Succeeded: its count. */
	*count = item.value;
	return 0;
}

/* Reads an answer's integer key; returns 0 or EBADMSG. */
static int
ctap2_key(
	struct pk_cbor_reader *reader,
	uint64_t *key)
{
	struct pk_cbor_item item;
	int error;

	/* An unsigned integer. */
	error = pk_cbor_read(reader, &item);
	if (error != 0 || item.kind != PK_CBOR_UNSIGNED)
		return EBADMSG;

	/* Succeeded: the key. */
	*key = item.value;
	return 0;
}

/* Tells whether a text item is a given text. */
static int
ctap2_text_is(
	const struct pk_cbor_item *item,
	const char *text)
{
	size_t length;
	int compared;

	/* The same length and bytes. */
	length = strlen(text);
	if (item->kind != PK_CBOR_TEXT || item->value != length)
		return 0;
	compared = memcmp(item->bytes, text, length);
	if (compared != 0)
		return 0;

	/* The same text. */
	return 1;
}

/* Reads GetInfo's options map into the option bits. */
static int
ctap2_read_info_options(
	struct pk_cbor_reader *reader,
	struct pk_info *info)
{
	struct pk_cbor_item name;
	struct pk_cbor_item value;
	uint64_t count;
	int truth;
	int error;

	/* A map of texts to booleans. */
	error = ctap2_map(reader, &count);
	if (error != 0)
		return error;
	for (; count > 0U; count--) {
		error = pk_cbor_read(reader, &name);
		if (error != 0 || name.kind != PK_CBOR_TEXT)
			return EBADMSG;
		error = pk_cbor_read(reader, &value);
		if (error != 0 || (value.kind != PK_CBOR_TRUE && value.kind != PK_CBOR_FALSE))
			return EBADMSG;
		truth = value.kind == PK_CBOR_TRUE;

		/* clientPin present (the function), and true (a PIN is set). */
		if (ctap2_text_is(&name, "clientPin")) {
			info->options |= PK_OPTION_CLIENT_PIN;
			if (truth)
				info->options |= PK_OPTION_CLIENT_PIN_SET;
		} else if (ctap2_text_is(&name, "pinUvAuthToken") && truth) {
			info->options |= PK_OPTION_PIN_UV_TOKEN;
		} else if (ctap2_text_is(&name, "uv") && truth) {
			info->options |= PK_OPTION_UV;
		} else if (ctap2_text_is(&name, "rk") && truth) {
			info->options |= PK_OPTION_RK;
		}
	}

	/* Succeeded: the options. */
	return 0;
}

/*
 * Reads a new credential's authenticator data: the relying party's hash,
 * the flags (present, attested data), the count, the AAGUID, the
 * credential ID and its COSE key (ES256, on the curve), and nothing after
 * but an extensions map when its flag says so; the data itself is kept
 * with them.  Returns 0 or EBADMSG.
 */
int
pk_ctap2_read_made(
	const char *rp_id,
	const uint8_t *data,
	size_t size,
	struct pk_made_credential *credential)
{
	struct pk_crypto_part party;
	uint8_t party_hash[PK_SHA256_SIZE];
	uint8_t x[PK_P256_SIZE];
	uint8_t y[PK_P256_SIZE];
	size_t offset;
	size_t id_size;
	size_t key_size;
	size_t rest;
	int valid;
	int same;
	int error;

	/* The fixed part, the AAGUID and the ID's length; the data kept. */
	memset(credential, 0, sizeof(*credential));
	if (size < PK_AUTH_DATA_MIN + 18U || size > sizeof(credential->auth_data))
		return EBADMSG;
	memcpy(credential->auth_data, data, size);
	credential->auth_data_size = size;

	/* The relying party's hash is ours. */
	party.data = (const uint8_t *)rp_id;
	party.size = strlen(rp_id);
	error = pk_crypto_sha256(&party, 1U, party_hash);
	if (error != 0)
		return error;
	same = pk_crypto_equal(party_hash, data, PK_SHA256_SIZE);
	if (!same)
		return EBADMSG;

	/* The flags: the user was present, and the attested data is there. */
	credential->flags = data[PK_SHA256_SIZE];
	if ((credential->flags & PK_FLAG_UP) == 0U || (credential->flags & PK_FLAG_AT) == 0U)
		return EBADMSG;
	credential->sign_count = ((uint32_t)data[33] << 24U) | ((uint32_t)data[34] << 16U) |
	    ((uint32_t)data[35] << 8U) | (uint32_t)data[36];

	/* The credential ID, after the AAGUID. */
	offset = PK_AUTH_DATA_MIN + 16U;
	id_size = ((size_t)data[offset] << 8U) | data[offset + 1U];
	offset += 2U;
	if (id_size == 0U || id_size > sizeof(credential->id) || id_size > size - offset)
		return EBADMSG;
	memcpy(credential->id, data + offset, id_size);
	credential->id_size = id_size;
	offset += id_size;

	/* The COSE key: one CBOR item, ES256 on P-256, a point on the curve. */
	error = pk_cbor_check(data + offset, size - offset, PK_CBOR_STRICT_MAPS, &key_size);
	if (error != 0 || key_size > sizeof(credential->cose_key))
		return EBADMSG;
	error = pk_cose_p256(data + offset, key_size, x, y);
	if (error != 0)
		return EBADMSG;
	valid = pk_crypto_p256_valid(x, y);
	if (!valid)
		return EBADMSG;
	memcpy(credential->cose_key, data + offset, key_size);
	credential->cose_key_size = key_size;
	offset += key_size;

	/* Nothing after, or one extensions map when the flag says so. */
	rest = size - offset;
	if ((credential->flags & PK_FLAG_ED) == 0U) {
		if (rest != 0U)
			return EBADMSG;
		return 0;
	}
	error = pk_cbor_check(data + offset, rest, PK_CBOR_STRICT_MAPS, &key_size);
	if (error != 0 || key_size != rest)
		return EBADMSG;

	/* Succeeded: the credential. */
	return 0;
}

/* Writes a credential descriptor: {"id": id, "type": "public-key"}. */
static void
ctap2_put_descriptor(
	struct pk_cbor_writer *writer,
	const uint8_t *id,
	size_t size)
{
	/* The two fields in canonical order. */
	pk_cbor_put_map(writer, 2U);
	pk_cbor_put_text(writer, "id", 2U);
	pk_cbor_put_bytes(writer, id, size);
	pk_cbor_put_text(writer, "type", 4U);
	pk_cbor_put_text(writer, "public-key", 10U);
}

/* Gives the size of the secret's HMAC key: all of it for protocol 1, the first half for protocol 2. */
static size_t
ctap2_hmac_key_size(
	const struct pk_pin_shared *shared)
{
	/* Protocol 1's secret is one key. */
	if (shared->protocol == 1U)
		return shared->key_size;

	/* Protocol 2's HMAC key is the first half. */
	return PK_SHA256_SIZE;
}
