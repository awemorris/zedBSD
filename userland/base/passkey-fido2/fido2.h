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
 *   info N,CARD,INDEX,PIN,RETRIES,MIN  how many keys, and the one key: a
 *                                  card or a USB key, its place, whether
 *                                  it has a PIN, its retries, its PIN's
 *                                  fewest characters (ws199-p001)
 *   done                           the key's PIN set or changed
 *   reset MASK                     the key was reset; the credentials of
 *                                  the job it held (bits, hexadecimal)
 *   owner MASK,CARD[ ID AUTH SIG]  the accounts whose credentials the one
 *                                  key holds (bits of the job's groups,
 *                                  hexadecimal), whether it is a card,
 *                                  and the first one's silent answer
 *                                  (ws199-p001 section 4.2)
 *   fail REASON                    one of passkey's reasons
 */

#ifndef USERLAND_BASE_PASSKEY_FIDO2_H
#define USERLAND_BASE_PASSKEY_FIDO2_H

#include <signal.h>
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
#define FIDO2_JOB_INFO		3
#define FIDO2_JOB_SET_PIN	4
#define FIDO2_JOB_CHANGE_PIN	5
#define FIDO2_JOB_RESET		6
#define FIDO2_JOB_OWNER		7

/* The most credentials a job names (a reset asks for every account's, ws199-p001). */
#define FIDO2_IDS_MAX		32U

/* The messages. */
#define FIDO2_MESSAGE_TOUCH	1
#define FIDO2_MESSAGE_ASSERTION	2
#define FIDO2_MESSAGE_MADE	3
#define FIDO2_MESSAGE_FAIL	4
#define FIDO2_MESSAGE_INFO	5
#define FIDO2_MESSAGE_DONE	6
#define FIDO2_MESSAGE_RESET	7
#define FIDO2_MESSAGE_OWNER	8

/*
 * The helper's job, made by passkey-fido2 before the helper starts: what
 * to do, the client data hash, the key's PIN (the current one to change
 * it) and a new PIN, the credentials (to allow, to exclude, or a reset's
 * or an owner's question's to look for, the latter with the account each
 * belongs to: its group, in order), the user's ID and name for a new one.
 */
struct fido2_job {
	int kind;
	uint8_t client_data_hash[32];
	char pin[72];
	char new_pin[72];
	int presence;
	const uint8_t *ids[FIDO2_IDS_MAX];
	size_t id_sizes[FIDO2_IDS_MAX];
	size_t id_count;
	unsigned groups[FIDO2_IDS_MAX];
	uint8_t user_id[FIDO2_USER_ID_SIZE];
	const char *user_name;
};

/*
 * A message read back: its kind, and for an assertion or a new
 * credential its bytes; for a key's information its numbers; for a reset
 * the credentials held; for an owner's question the groups held, whether
 * the key is a card and the first group's answer (its bytes, none when no
 * group is held); for a failure its reason.
 */
struct fido2_message {
	int kind;
	unsigned info_count;
	unsigned info_card;
	unsigned info_index;
	unsigned info_pin;
	unsigned info_retries;
	unsigned info_min;
	unsigned held;
	unsigned owner_card;
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
	char names[PK_OS_DEVICES_MAX][PK_OS_NAME_MAX];
	size_t count;
	struct pk_os_card cards[PK_OS_DEVICES_MAX];
	char card_names[PK_OS_DEVICES_MAX][PK_OS_NAME_MAX];
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
extern volatile sig_atomic_t fido2_ended;
void fido2_catch_end(void);
int fido2_devices_open(struct fido2_devices *devices);
void fido2_devices_close(struct fido2_devices *devices);
int fido2_run_helper(struct fido2_devices *devices, const struct fido2_job *job, uid_t uid, gid_t gid,
    struct fido2_message *message);

/* helper.c */
void fido2_helper(struct fido2_devices *devices, const struct fido2_job *job, int pipe_out, uid_t uid, gid_t gid);

#endif
