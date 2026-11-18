/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CTAP2 (ws161-p004; FIDO CTAP 2.1): the commands /sbin/passkey
 * and the tools use, over any transport that carries one CBOR command and
 * its answer (CTAPHID over a raw HID node, or NFC over a smart card slot).
 *
 * Every function answers 0, or an errno value: EPROTO when the key answered
 * with a CTAP2 status other than success (the status is then in the
 * device's last_status), EBADMSG for an answer that is not well formed, or
 * the transport's error.
 */

#ifndef LIBPASSKEY_CTAP2_H
#define LIBPASSKEY_CTAP2_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"
#include "hid.h"

/* The commands. */
#define PK_CTAP2_MAKE_CREDENTIAL	0x01U
#define PK_CTAP2_GET_ASSERTION		0x02U
#define PK_CTAP2_GET_INFO		0x04U
#define PK_CTAP2_CLIENT_PIN		0x06U
#define PK_CTAP2_RESET			0x07U
#define PK_CTAP2_SELECTION		0x0bU

/* The statuses the library and its callers act on. */
#define PK_CTAP2_OK			0x00U
#define PK_CTAP2_NOT_ALLOWED		0x30U
#define PK_CTAP2_CREDENTIAL_EXCLUDED	0x19U
#define PK_CTAP2_KEEPALIVE_CANCEL	0x2dU
#define PK_CTAP2_NO_CREDENTIALS		0x2eU
#define PK_CTAP2_OPERATION_DENIED	0x27U
#define PK_CTAP2_PIN_INVALID		0x31U
#define PK_CTAP2_PIN_BLOCKED		0x32U
#define PK_CTAP2_PIN_AUTH_INVALID	0x33U
#define PK_CTAP2_PIN_AUTH_BLOCKED	0x34U
#define PK_CTAP2_PIN_NOT_SET		0x35U
#define PK_CTAP2_PIN_REQUIRED		0x36U
#define PK_CTAP2_PIN_POLICY_VIOLATION	0x37U
#define PK_CTAP2_USER_ACTION_TIMEOUT	0x2fU

/* GetInfo's versions and options, as bits. */
#define PK_INFO_FIDO_2_0		0x0001U
#define PK_INFO_FIDO_2_1		0x0002U
#define PK_INFO_U2F_V2			0x0004U
#define PK_OPTION_CLIENT_PIN		0x0001U		/* the key has the PIN function */
#define PK_OPTION_CLIENT_PIN_SET	0x0002U		/* and a PIN is set */
#define PK_OPTION_PIN_UV_TOKEN		0x0004U		/* getPinUvAuthTokenUsingPinWithPermissions */
#define PK_OPTION_UV			0x0008U		/* built-in user verification */
#define PK_OPTION_RK			0x0010U		/* resident credentials */

/* The PIN/UV protocols, as bits of pin_protocols. */
#define PK_PIN_PROTOCOL_1		0x0001U
#define PK_PIN_PROTOCOL_2		0x0002U

/* The longest credential ID kept, the longest COSE key, and the longest authenticator data and signature. */
#define PK_CREDENTIAL_ID_MAX		1024U
#define PK_COSE_KEY_MAX			256U
#define PK_AUTH_DATA_MAX		1024U
#define PK_SIGNATURE_MAX		80U

/* The largest message the library sends or takes. */
#define PK_MESSAGE_MAX			PK_HID_MESSAGE_MAX

/*
 * A transport: one CBOR command (its byte, then the request's CBOR) and its
 * answer (the status byte, then the CBOR), within timeout_ms; keepalive is
 * told while the key waits for the user.  cancel stops the command under
 * way (it may be NULL).
 */
struct pk_transport {
	void *context;
	int (*command)(void *context, uint8_t command, const uint8_t *request, size_t size, uint8_t *reply,
	    size_t capacity, size_t *reply_size, unsigned timeout_ms, pk_hid_keepalive_t keepalive,
	    void *keepalive_context);
	int (*cancel)(void *context);
};

/*
 * A key: its transport, what it waits for (keepalive), how long a command
 * may take, and the last CTAP2 status it answered.
 */
struct pk_device {
	struct pk_transport transport;
	pk_hid_keepalive_t keepalive;
	void *keepalive_context;
	unsigned timeout_ms;
	uint8_t last_status;
};

/* What GetInfo says. */
struct pk_info {
	unsigned versions;
	unsigned options;
	unsigned pin_protocols;
	uint8_t aaguid[16];
	uint32_t max_message;
	uint32_t min_pin_length;
	uint32_t max_credential_count;
	uint32_t max_credential_id_length;
};

/* What a new credential is to be: the relying party, the user's ID and name, whether to verify the user, the IDs to exclude. */
struct pk_make_request {
	const char *rp_id;
	const uint8_t *user_id;
	size_t user_id_size;
	const char *user_name;
	uint8_t client_data_hash[PK_SHA256_SIZE];
	const uint8_t *pin_token;
	size_t pin_token_size;
	unsigned pin_protocol;
	const uint8_t *const *exclude_ids;
	const size_t *exclude_sizes;
	size_t exclude_count;
};

/*
 * The new credential, read from the key's authenticator data by the
 * library itself: its ID, its COSE public key (ES256 on P-256, checked to
 * be on the curve), its signature count, and the flags; and the
 * authenticator data as the key gave it (ws172-p003: the process that
 * keeps the credential reads it again with pk_ctap2_read_made).
 */
struct pk_made_credential {
	uint8_t flags;
	uint8_t id[PK_CREDENTIAL_ID_MAX];
	size_t id_size;
	uint8_t cose_key[PK_COSE_KEY_MAX];
	size_t cose_key_size;
	uint32_t sign_count;
	uint8_t auth_data[PK_AUTH_DATA_MAX];
	size_t auth_data_size;
};

/*
 * An assertion to ask for: the relying party, the client data hash, the
 * credentials allowed, whether the user must be present (0: the silent
 * question of which key holds a credential), and the PIN token for UV
 * (none: no user verification asked).
 */
struct pk_assertion_request {
	const char *rp_id;
	int presence;
	uint8_t client_data_hash[PK_SHA256_SIZE];
	const uint8_t *const *allow_ids;
	const size_t *allow_sizes;
	size_t allow_count;
	const uint8_t *pin_token;
	size_t pin_token_size;
	unsigned pin_protocol;
};

/* The key's answer: the credential it used, the authenticator data and the signature (DER). */
struct pk_assertion_reply {
	uint8_t credential_id[PK_CREDENTIAL_ID_MAX];
	size_t credential_id_size;
	uint8_t auth_data[PK_AUTH_DATA_MAX];
	size_t auth_data_size;
	uint8_t signature[PK_SIGNATURE_MAX];
	size_t signature_size;
};

/* GetPinUvAuthTokenUsingPinWithPermissions's permissions. */
#define PK_PERMISSION_MAKE_CREDENTIAL	0x01U
#define PK_PERMISSION_GET_ASSERTION	0x02U

/* The longest PIN token. */
#define PK_PIN_TOKEN_MAX		32U

void pk_device_init(struct pk_device *device, const struct pk_transport *transport, unsigned timeout_ms);
int pk_hid_transport(struct pk_transport *transport, struct pk_hid *hid);
int pk_ctap2_get_info(struct pk_device *device, struct pk_info *info);
unsigned pk_ctap2_choose_protocol(const struct pk_info *info);
int pk_ctap2_pin_retries(struct pk_device *device, unsigned protocol, unsigned *retries);
int pk_ctap2_set_pin(struct pk_device *device, unsigned protocol, const char *pin);
int pk_ctap2_change_pin(struct pk_device *device, unsigned protocol, const char *current, const char *pin);
int pk_ctap2_pin_token(struct pk_device *device, const struct pk_info *info, unsigned protocol, const char *pin,
    unsigned permissions, const char *rp_id, uint8_t *token, size_t *token_size);
int pk_ctap2_make_credential(struct pk_device *device, const struct pk_make_request *request,
    struct pk_made_credential *credential);
int pk_ctap2_get_assertion(struct pk_device *device, const struct pk_assertion_request *request,
    struct pk_assertion_reply *reply);
int pk_ctap2_selection(struct pk_device *device);
int pk_ctap2_reset(struct pk_device *device);
int pk_ctap2_read_made(const char *rp_id, const uint8_t *data, size_t size, struct pk_made_credential *credential);

#endif
