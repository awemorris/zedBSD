/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /usr/libexec/passkey-fido2 (ws172-p003; docs/architecture/security.md,
 * "Login authentication", "The security key"): the security key style of
 * /sbin/passkey, started by it with the same request.
 *
 *   main.c    the request, the account, /etc/passkey, the challenge and
 *             the check of the key's answer (root)
 *   device.c  the keys' nodes opened and claimed, the helper started and
 *             its messages read (root)
 *   helper.c  the device helper: as _passkey in an empty root, it talks
 *             CTAP to the keys and sends back the chosen key's answer
 *   wire.c    the pure parts: base64url, hexadecimal, the helper's
 *             messages, a key's line of /etc/passkey, the client data
 *             hash (the host test runs them alone)
 *
 * The helper's messages are lines on its pipe to passkey-fido2:
 *
 *   touch                          the key waits for the user's touch
 *   assertion ID AUTH-DATA SIG     the chosen key's answer (hexadecimal)
 *   made AUTH-DATA                 a new credential's authenticator data
 *   fail REASON                    one of passkey's reasons
 */

#ifndef USERLAND_BASE_PASSKEY_FIDO2_H
#define USERLAND_BASE_PASSKEY_FIDO2_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "userland/base/libpasskey/ctap2.h"
#include "userland/base/libpasskey/os.h"

/* The relying party of the login's credentials. */
#define FIDO2_RP		"zedbsd.login"

/* The challenge's size, the user ID's size (the first bytes of the name's SHA-256), and a label's longest. */
#define FIDO2_CHALLENGE_SIZE	32U
#define FIDO2_USER_ID_SIZE	16U
#define FIDO2_LABEL_MAX		32U

/* How long the user has to touch the key, and how much longer the helper may live. */
#define FIDO2_TOUCH_MS		30000U
#define FIDO2_HELPER_SECONDS	33U

/* The longest helper's message line, with its end. */
#define FIDO2_MESSAGE_MAX	8192U

/* The longest base64url of a credential ID or a COSE key, with the NUL. */
#define FIDO2_BASE64_MAX	1400U

/* What the helper is asked to do. */
#define FIDO2_JOB_ASSERT	1
#define FIDO2_JOB_MAKE		2

/* The messages. */
#define FIDO2_MESSAGE_TOUCH	1
#define FIDO2_MESSAGE_ASSERTION	2
#define FIDO2_MESSAGE_MADE	3
#define FIDO2_MESSAGE_FAIL	4

/*
 * The helper's job, made by passkey-fido2 before the helper starts: what
 * to do, the client data hash, the key's PIN, the account's credentials
 * (to allow, or to exclude), the user's ID and name for a new one.
 */
struct fido2_job {
	int kind;
	uint8_t client_data_hash[32];
	char pin[72];
	const uint8_t *ids[5];
	size_t id_sizes[5];
	size_t id_count;
	uint8_t user_id[FIDO2_USER_ID_SIZE];
	const char *user_name;
};

/*
 * A message read back: its kind, and for an assertion or a new
 * credential its bytes; for a failure its reason.
 */
struct fido2_message {
	int kind;
	uint8_t id[PK_CREDENTIAL_ID_MAX];
	size_t id_size;
	uint8_t auth_data[PK_AUTH_DATA_MAX];
	size_t auth_data_size;
	uint8_t signature[PK_SIGNATURE_MAX];
	size_t signature_size;
	char reason[32];
};

/*
 * A key's line of /etc/passkey taken apart: its credential ID and COSE key
 * (decoded), its count, its relying party and label (in the line's own
 * buffer).
 */
struct fido2_record {
	uint8_t id[PK_CREDENTIAL_ID_MAX];
	size_t id_size;
	uint8_t cose_key[PK_COSE_KEY_MAX];
	size_t cose_key_size;
	uint32_t sign_count;
	char id_text[FIDO2_BASE64_MAX];
	char label[FIDO2_LABEL_MAX + 1U];
};

/*
 * The nodes passkey-fido2 opened: the USB keys' handles and how many, and
 * the smart card slots attached (a key may be held to an NFC reader,
 * ws199-p001 section 5) and how many.
 */
struct fido2_devices {
	struct pk_os_hid handles[PK_OS_DEVICES_MAX];
	size_t count;
	struct pk_os_card cards[PK_OS_DEVICES_MAX];
	size_t card_count;
};

/* wire.c */
int fido2_base64_encode(const uint8_t *bytes, size_t size, char *text, size_t capacity);
int fido2_base64_decode(const char *text, uint8_t *bytes, size_t capacity, size_t *size);
int fido2_hex_encode(const uint8_t *bytes, size_t size, char *text, size_t capacity);
int fido2_message_parse(char *line, struct fido2_message *message);
int fido2_record_parse(const char *line, struct fido2_record *record);
int fido2_record_line(const char *name, uid_t uid, const char *id, const char *cose_key, uint32_t count, const char *label,
    const char *date, char *line, size_t size);
int fido2_record_recount(const char *line, uint32_t count, char *output, size_t size);
int fido2_label_valid(const char *label);
int fido2_client_data_hash(const char *name, const uint8_t *challenge, uint8_t *hash);
int fido2_user_id(const char *name, uint8_t *id);

/* device.c */
int fido2_devices_open(struct fido2_devices *devices);
void fido2_devices_close(struct fido2_devices *devices);
int fido2_run_helper(struct fido2_devices *devices, const struct fido2_job *job, uid_t uid, gid_t gid,
    struct fido2_message *message);

/* helper.c */
void fido2_helper(struct fido2_devices *devices, const struct fido2_job *job, int pipe_out, uid_t uid, gid_t gid);

#endif
