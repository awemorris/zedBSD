/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libpasskey's CTAPHID, CTAP2 and PIN/UV protocols
 * (ws161-p004): a software authenticator in this file answers the
 * library over in-memory reports.  The authenticator's own cryptography
 * calls OpenSSL directly (its ECDH, its derivation of the shared secret by
 * the CTAP 2.1 rules, its AES-256-CBC and HMAC), so the library's pin.c is
 * checked against a second writing of the protocols, not against itself.
 *
 * For PIN/UV protocol 2 (a CTAP 2.1 key, getPinUvAuthTokenUsingPinWithPermissions)
 * and protocol 1 (a CTAP 2.0 key, getPinToken): GetInfo; setPIN; a wrong
 * PIN (PIN_INVALID, one retry fewer); the right PIN's token;
 * MakeCredential with it (the new credential read from the authenticator
 * data); the silent question (up false) for a credential held and one
 * not held (NO_CREDENTIALS); GetAssertion with the token, verified by
 * pk_verify_assertion with the stored key; changePIN and the token of the
 * new PIN.  The reports carry noise the library must skip: another
 * channel's report and a KEEPALIVE before each answer.
 *
 * The same flow runs over NFC (ws161-p005, transport-nfc.c): the
 * authenticator behind an APDU card that answers SELECT with "FIDO_2_0",
 * each NFCCTAP_MSG first with a keepalive ("9100", user presence needed)
 * and then, at NFCCTAP_GETRESPONSE, the answer, in parts of 256 bytes
 * ("61xx", GET RESPONSE) behind a reader of short APDUs, whole behind one
 * of extended APDUs.  A vendor command that the card echoes carries a
 * message of 600 bytes each way: three chained blocks out, three parts
 * back on the short reader.
 *
 * authenticatorReset (ws199-p004, pk_ctap2_reset): taken in the window it
 * forgets every credential and the PIN; out of the window the key says
 * NOT_ALLOWED (0x30) and keeps them; while it waits for the touch, a
 * CTAPHID CANCEL sent from the keepalive makes it answer KEEPALIVE_CANCEL
 * (0x2d) and keep them too.
 */

#include "userland/base/libpasskey/cbor.h"
#include "userland/base/libpasskey/ctap2.h"
#include "userland/base/libpasskey/hid.h"
#include "userland/base/libpasskey/nfc.h"
#include "userland/base/libpasskey/verify.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, what) check((condition), (what))

/* The authenticator's queued input reports. */
#define QUEUE_MAX	512U

/* The transports the flow runs over: CTAPHID, NFC behind a reader of short APDUs, and of extended ones. */
#define RUN_HID		0
#define RUN_NFC_SHORT	1
#define RUN_NFC_EXTENDED	2

/* How the authenticator takes authenticatorReset: at once, out of its window, or waiting for a touch that a CANCEL ends. */
#define RESET_TAKEN	0
#define RESET_LATE	1
#define RESET_WAIT	2

/* The vendor command the NFC card echoes, and the size of the echoed message. */
#define NFC_ECHO	0x40U
#define NFC_ECHO_SIZE	600U

/* The checks that failed. */
static unsigned failures;

/*
 * The software authenticator: its protocol (1 or 2) and options, its
 * agreement key, its PIN's hash and retries, the token, its credentials,
 * the message being received, and the answer's reports.
 */
static struct {
	unsigned protocol;
	int ctap21;
	EVP_PKEY *agreement;
	uint8_t pin_hash[16];
	int pin_set;
	unsigned retries;
	uint8_t token[32];
	int token_valid;
	uint8_t ids[4][16];
	EVP_PKEY *keys[4];
	unsigned count;
	uint32_t sign_count;
	uint32_t channel;
	uint8_t message[PK_HID_MESSAGE_MAX];
	size_t length;
	size_t received;
	uint8_t command;
	uint8_t next_sequence;
	uint8_t queue[QUEUE_MAX][PK_HID_REPORT];
	unsigned head;
	unsigned tail;
	unsigned keepalives;
	int lie_rp;
	int reset_mode;
	int reset_waiting;
	unsigned cancels;
} card;

/*
 * The NFC card in front of the authenticator: whether the applet is
 * selected, the message being chained in, the answer being given out, a
 * keepalive owed before it, and what the library sent (chained blocks,
 * GET RESPONSEs, keepalive polls).
 */
static struct {
	int selected;
	uint8_t message[PK_HID_MESSAGE_MAX];
	size_t length;
	uint8_t answer[PK_HID_MESSAGE_MAX];
	size_t answer_size;
	size_t answer_sent;
	int answer_ready;
	int extended;
	unsigned chained;
	unsigned get_responses;
	unsigned polls;
} nfc;

/* Counts and reports a check that does not hold. */
static void
check(
	int condition,
	const char *what)
{
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}

/* Queues one input report. */
static void
queue_report(
	const uint8_t *report)
{
	memcpy(card.queue[card.tail % QUEUE_MAX], report, PK_HID_REPORT);
	card.tail++;
}

/* Queues a whole message on a channel (with noise before it: another channel's report and a KEEPALIVE). */
static void
queue_message(
	uint32_t channel,
	uint8_t command,
	const uint8_t *data,
	size_t size)
{
	uint8_t report[PK_HID_REPORT];
	size_t sent;
	size_t take;
	uint8_t sequence;

	memset(report, 0, sizeof(report));
	report[0] = 0x12; report[1] = 0x34; report[2] = 0x56; report[3] = 0x78;
	report[4] = PK_HID_PING; report[6] = 1;
	queue_report(report);
	if (command == PK_HID_CBOR) {
		memset(report, 0, sizeof(report));
		report[0] = (uint8_t)(channel >> 24); report[1] = (uint8_t)(channel >> 16);
		report[2] = (uint8_t)(channel >> 8); report[3] = (uint8_t)channel;
		report[4] = PK_HID_KEEPALIVE; report[6] = 1; report[7] = PK_HID_KEEPALIVE_UP_NEEDED;
		queue_report(report);
	}
	memset(report, 0, sizeof(report));
	report[0] = (uint8_t)(channel >> 24); report[1] = (uint8_t)(channel >> 16);
	report[2] = (uint8_t)(channel >> 8); report[3] = (uint8_t)channel;
	report[4] = command; report[5] = (uint8_t)(size >> 8); report[6] = (uint8_t)size;
	take = size < PK_HID_INIT_DATA ? size : PK_HID_INIT_DATA;
	memcpy(report + 7, data, take);
	queue_report(report);
	sent = take;
	sequence = 0;
	while (sent < size) {
		memset(report, 0, sizeof(report));
		memcpy(report, &card.queue[(card.tail - 1U) % QUEUE_MAX][0], 4);
		report[4] = sequence++;
		take = size - sent < PK_HID_CONT_DATA ? size - sent : PK_HID_CONT_DATA;
		memcpy(report + 5, data + sent, take);
		queue_report(report);
		sent += take;
	}
}

/* Makes a P-256 public key from coordinates. */
static EVP_PKEY *
p256(
	const uint8_t *x,
	const uint8_t *y)
{
	OSSL_PARAM parameters[3];
	EVP_PKEY_CTX *context;
	EVP_PKEY *key;
	uint8_t point[65];

	point[0] = 4;
	memcpy(point + 1, x, 32);
	memcpy(point + 33, y, 32);
	parameters[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0);
	parameters[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point));
	parameters[2] = OSSL_PARAM_construct_end();
	context = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	key = NULL;
	EVP_PKEY_fromdata_init(context);
	EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters);
	EVP_PKEY_CTX_free(context);
	return key;
}

/* Writes a key's COSE form (alg -7 or -25). */
static void
put_cose(
	struct pk_cbor_writer *writer,
	EVP_PKEY *key,
	int64_t alg)
{
	uint8_t point[65];
	size_t length;

	length = 0;
	EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &length);
	pk_cbor_put_map(writer, 5);
	pk_cbor_put_integer(writer, 1); pk_cbor_put_integer(writer, 2);
	pk_cbor_put_integer(writer, 3); pk_cbor_put_integer(writer, alg);
	pk_cbor_put_integer(writer, -1); pk_cbor_put_integer(writer, 1);
	pk_cbor_put_integer(writer, -2); pk_cbor_put_bytes(writer, point + 1, 32);
	pk_cbor_put_integer(writer, -3); pk_cbor_put_bytes(writer, point + 33, 32);
}

/* The shared secret with the platform's key, by the CTAP 2.1 rules (written apart from pin.c). */
static void
shared_secret(
	const uint8_t *cose,
	size_t size,
	uint8_t *secret)
{
	OSSL_PARAM parameters[5];
	EVP_PKEY_CTX *context;
	EVP_KDF_CTX *kdf;
	EVP_KDF *method;
	EVP_PKEY *peer;
	uint8_t x[32];
	uint8_t y[32];
	uint8_t z[32];
	uint8_t salt[32];
	size_t length;
	static const char *infos[2] = { "CTAP2 HMAC key", "CTAP2 AES key" };
	unsigned part;

	CHECK(pk_cose_ec2(cose, size, -25, x, y) == 0, "platform key COSE");
	peer = p256(x, y);
	context = EVP_PKEY_CTX_new(card.agreement, NULL);
	EVP_PKEY_derive_init(context);
	EVP_PKEY_derive_set_peer(context, peer);
	length = 32;
	EVP_PKEY_derive(context, z, &length);
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(peer);
	if (card.protocol == 1) {
		EVP_Digest(z, 32, secret, NULL, EVP_sha256(), NULL);
		return;
	}
	memset(salt, 0, sizeof(salt));
	for (part = 0; part < 2; part++) {
		method = EVP_KDF_fetch(NULL, "HKDF", NULL);
		kdf = EVP_KDF_CTX_new(method);
		EVP_KDF_free(method);
		parameters[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0);
		parameters[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, z, 32);
		parameters[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, 32);
		parameters[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)infos[part], strlen(infos[part]));
		parameters[4] = OSSL_PARAM_construct_end();
		EVP_KDF_derive(kdf, secret + 32 * part, 32, parameters);
		EVP_KDF_CTX_free(kdf);
	}
}

/* AES-256-CBC of whole blocks, protocol 1 with a zero IV, protocol 2 with the IV first. */
static size_t
cipher(
	int encrypt,
	const uint8_t *secret,
	const uint8_t *in,
	size_t size,
	uint8_t *out)
{
	EVP_CIPHER_CTX *context;
	uint8_t iv[16];
	const uint8_t *key;
	int written;
	size_t offset;

	key = card.protocol == 1 ? secret : secret + 32;
	memset(iv, 0, sizeof(iv));
	offset = 0;
	if (card.protocol == 2 && encrypt) {
		RAND_bytes(iv, 16);
		memcpy(out, iv, 16);
		out += 16;
		offset = 16;
	} else if (card.protocol == 2) {
		memcpy(iv, in, 16);
		in += 16;
		size -= 16;
	}
	context = EVP_CIPHER_CTX_new();
	EVP_CipherInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv, encrypt);
	EVP_CIPHER_CTX_set_padding(context, 0);
	written = 0;
	EVP_CipherUpdate(context, out, &written, in, (int)size);
	EVP_CIPHER_CTX_free(context);
	return offset + size;
}

/* Checks a pinUvAuthParam: HMAC-SHA-256 of message under key (16 bytes for protocol 1). */
static int
auth_ok(
	const uint8_t *key,
	size_t key_size,
	const uint8_t *message,
	size_t size,
	const uint8_t *param,
	size_t param_size)
{
	uint8_t mac[32];
	unsigned int length;

	HMAC(EVP_sha256(), key, (int)key_size, message, size, mac, &length);
	if (param_size != (card.protocol == 1 ? 16U : 32U))
		return 0;
	return memcmp(mac, param, param_size) == 0;
}

/* Signs authData ‖ clientDataHash with a credential's key. */
static size_t
sign(
	EVP_PKEY *key,
	const uint8_t *auth,
	size_t auth_size,
	const uint8_t *hash,
	uint8_t *signature)
{
	EVP_MD_CTX *context;
	size_t length;

	context = EVP_MD_CTX_new();
	length = 80;
	EVP_DigestSignInit(context, NULL, EVP_sha256(), NULL, key);
	EVP_DigestSignUpdate(context, auth, auth_size);
	EVP_DigestSignUpdate(context, hash, 32);
	EVP_DigestSignFinal(context, signature, &length);
	EVP_MD_CTX_free(context);
	return length;
}

/* A map's values by key, from a CTAP2 request (the item after each key is kept as bytes). */
struct fields {
	const uint8_t *value[16];
	size_t size[16];
};

static void
read_fields(
	const uint8_t *data,
	size_t size,
	struct fields *fields)
{
	struct pk_cbor_reader reader;
	struct pk_cbor_item item;
	uint64_t count;
	size_t start;

	memset(fields, 0, sizeof(*fields));
	if (size == 0)
		return;
	pk_cbor_reader_init(&reader, data, size);
	CHECK(pk_cbor_read(&reader, &item) == 0 && item.kind == PK_CBOR_MAP, "request is a map");
	for (count = item.value; count > 0; count--) {
		pk_cbor_read(&reader, &item);
		start = reader.offset;
		pk_cbor_skip(&reader);
		if (item.kind == PK_CBOR_UNSIGNED && item.value < 16) {
			fields->value[item.value] = data + start;
			fields->size[item.value] = reader.offset - start;
		}
	}
}

/* A field's byte string. */
static const uint8_t *
field_bytes(
	const struct fields *fields,
	unsigned key,
	size_t *size)
{
	struct pk_cbor_reader reader;
	struct pk_cbor_item item;

	if (fields->value[key] == NULL)
		return NULL;
	pk_cbor_reader_init(&reader, fields->value[key], fields->size[key]);
	if (pk_cbor_read(&reader, &item) != 0 || item.kind != PK_CBOR_BYTES)
		return NULL;
	*size = (size_t)item.value;
	return item.bytes;
}

/* A field's unsigned number. */
static uint64_t
field_number(
	const struct fields *fields,
	unsigned key)
{
	struct pk_cbor_reader reader;
	struct pk_cbor_item item;

	if (fields->value[key] == NULL)
		return 0;
	pk_cbor_reader_init(&reader, fields->value[key], fields->size[key]);
	pk_cbor_read(&reader, &item);
	return item.value;
}

/* Builds authenticator data. */
static size_t
auth_data(
	uint8_t *out,
	const uint8_t *rp,
	size_t rp_size,
	uint8_t flags,
	unsigned credential)
{
	struct pk_cbor_writer writer;
	size_t at;

	EVP_Digest(rp, rp_size, out, NULL, EVP_sha256(), NULL);
	if (card.lie_rp)
		out[0] ^= 1;
	out[32] = flags;
	card.sign_count++;
	out[33] = (uint8_t)(card.sign_count >> 24); out[34] = (uint8_t)(card.sign_count >> 16);
	out[35] = (uint8_t)(card.sign_count >> 8); out[36] = (uint8_t)card.sign_count;
	at = 37;
	if (flags & PK_FLAG_AT) {
		memset(out + at, 0xaa, 16); at += 16;
		out[at++] = 0; out[at++] = 16;
		memcpy(out + at, card.ids[credential], 16); at += 16;
		pk_cbor_writer_init(&writer, out + at, 128);
		put_cose(&writer, card.keys[credential], -7);
		at += writer.length;
	}
	return at;
}

/* Answers one CTAP2 command into reply (status, then CBOR). */
static size_t
answer(
	uint8_t command,
	const uint8_t *request,
	size_t size,
	uint8_t *reply)
{
	struct pk_cbor_writer writer;
	struct pk_cbor_reader reader;
	struct pk_cbor_item item;
	struct fields fields;
	struct fields inner;
	uint8_t secret[64];
	uint8_t plain[128];
	uint8_t hash[32];
	uint8_t data[512];
	uint8_t signature[80];
	uint8_t encrypted[64];
	const uint8_t *bytes;
	const uint8_t *param;
	size_t bytes_size;
	size_t param_size;
	size_t data_size;
	size_t signature_size;
	size_t length;
	uint64_t sub;
	uint64_t count;
	unsigned index;
	unsigned found;
	int presence;

	read_fields(request, size, &fields);
	pk_cbor_writer_init(&writer, reply + 1, PK_HID_MESSAGE_MAX - 1);
	reply[0] = 0;
	switch (command) {
	case PK_CTAP2_GET_INFO:
		pk_cbor_put_map(&writer, 4);
		pk_cbor_put_unsigned(&writer, 1);
		pk_cbor_put_array(&writer, 1);
		pk_cbor_put_text(&writer, card.ctap21 ? "FIDO_2_1" : "FIDO_2_0", 8);
		pk_cbor_put_unsigned(&writer, 3);
		memset(data, 0xaa, 16);
		pk_cbor_put_bytes(&writer, data, 16);
		pk_cbor_put_unsigned(&writer, 4);
		pk_cbor_put_map(&writer, card.ctap21 ? 2 : 1);
		pk_cbor_put_text(&writer, "clientPin", 9);
		pk_cbor_put_bool(&writer, card.pin_set);
		if (card.ctap21) {
			pk_cbor_put_text(&writer, "pinUvAuthToken", 14);
			pk_cbor_put_bool(&writer, 1);
		}
		pk_cbor_put_unsigned(&writer, 6);
		pk_cbor_put_array(&writer, 1);
		pk_cbor_put_unsigned(&writer, card.protocol);
		break;
	case PK_CTAP2_CLIENT_PIN:
		sub = field_number(&fields, 2);
		CHECK(field_number(&fields, 1) == card.protocol, "ClientPIN protocol");
		if (sub == 0x01) {
			pk_cbor_put_map(&writer, 1);
			pk_cbor_put_unsigned(&writer, 3);
			pk_cbor_put_unsigned(&writer, card.retries);
			break;
		}
		if (sub == 0x02) {
			pk_cbor_put_map(&writer, 1);
			pk_cbor_put_unsigned(&writer, 1);
			put_cose(&writer, card.agreement, -25);
			break;
		}
		shared_secret(fields.value[3], fields.size[3], secret);
		if (sub == 0x03 || sub == 0x04) {
			/* setPIN, changePIN: the MAC over newPinEnc (‖ pinHashEnc). */
			bytes = field_bytes(&fields, 5, &bytes_size);
			param = field_bytes(&fields, 4, &param_size);
			length = bytes_size;
			memcpy(data, bytes, bytes_size);
			if (sub == 0x04) {
				const uint8_t *hash_enc;
				size_t hash_size;
				hash_enc = field_bytes(&fields, 6, &hash_size);
				memcpy(data + length, hash_enc, hash_size);
				length += hash_size;
				cipher(0, secret, hash_enc, hash_size, plain);
				if (memcmp(plain, card.pin_hash, 16) != 0) {
					card.retries--;
					reply[0] = PK_CTAP2_PIN_INVALID;
					return 1;
				}
			}
			if (!auth_ok(secret, card.protocol == 1 ? 32 : 32, data, length, param, param_size)) {
				reply[0] = PK_CTAP2_PIN_AUTH_INVALID;
				return 1;
			}
			length = cipher(0, secret, bytes, bytes_size, plain);
			CHECK(length == 64, "new PIN padded to 64");
			EVP_Digest(plain, strlen((char *)plain), hash, NULL, EVP_sha256(), NULL);
			memcpy(card.pin_hash, hash, 16);
			card.pin_set = 1;
			card.retries = 8;
			return 1;
		}
		/* getPinToken (5) or ...WithPermissions (9). */
		CHECK(sub == (card.ctap21 ? 0x09U : 0x05U), "the PIN token subcommand the key has");
		if (sub == 0x09)
			CHECK(field_number(&fields, 9) != 0 && fields.value[10] != NULL, "permissions and rpId");
		bytes = field_bytes(&fields, 6, &bytes_size);
		cipher(0, secret, bytes, bytes_size, plain);
		if (memcmp(plain, card.pin_hash, 16) != 0) {
			card.retries--;
			reply[0] = PK_CTAP2_PIN_INVALID;
			return 1;
		}
		card.retries = 8;
		RAND_bytes(card.token, 32);
		card.token_valid = 1;
		length = cipher(1, secret, card.token, 32, encrypted);
		pk_cbor_put_map(&writer, 1);
		pk_cbor_put_unsigned(&writer, 2);
		pk_cbor_put_bytes(&writer, encrypted, length);
		break;
	case PK_CTAP2_MAKE_CREDENTIAL:
		bytes = field_bytes(&fields, 1, &bytes_size);
		param = field_bytes(&fields, 8, &param_size);
		if (param == NULL || !card.token_valid || !auth_ok(card.token, 32, bytes, 32, param, param_size)) {
			reply[0] = PK_CTAP2_PIN_AUTH_INVALID;
			return 1;
		}
		read_fields(fields.value[2], fields.size[2], &inner);
		pk_cbor_reader_init(&reader, fields.value[2], fields.size[2]);
		pk_cbor_read(&reader, &item);
		pk_cbor_read(&reader, &item);
		pk_cbor_read(&reader, &item);
		index = card.count++;
		RAND_bytes(card.ids[index], 16);
		card.keys[index] = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
		data_size = auth_data(data, item.bytes, (size_t)item.value, PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_AT, index);
		pk_cbor_put_map(&writer, 3);
		pk_cbor_put_unsigned(&writer, 1);
		pk_cbor_put_text(&writer, "none", 4);
		pk_cbor_put_unsigned(&writer, 2);
		pk_cbor_put_bytes(&writer, data, data_size);
		pk_cbor_put_unsigned(&writer, 3);
		pk_cbor_put_map(&writer, 0);
		break;
	case PK_CTAP2_GET_ASSERTION:
		bytes = field_bytes(&fields, 2, &bytes_size);
		param = field_bytes(&fields, 6, &param_size);
		presence = fields.value[5] == NULL;
		if (param != NULL && (!card.token_valid || !auth_ok(card.token, 32, bytes, 32, param, param_size))) {
			reply[0] = PK_CTAP2_PIN_AUTH_INVALID;
			return 1;
		}
		/* The first allowed credential this key holds. */
		pk_cbor_reader_init(&reader, fields.value[3], fields.size[3]);
		pk_cbor_read(&reader, &item);
		found = 99;
		for (count = item.value; count > 0; count--) {
			pk_cbor_read(&reader, &item);
			pk_cbor_read(&reader, &item);
			pk_cbor_read(&reader, &item);
			for (index = 0; index < card.count; index++)
				if (item.value == 16 && memcmp(item.bytes, card.ids[index], 16) == 0 && found == 99)
					found = index;
			pk_cbor_skip(&reader);
			pk_cbor_skip(&reader);
		}
		if (found == 99) {
			reply[0] = PK_CTAP2_NO_CREDENTIALS;
			return 1;
		}
		pk_cbor_reader_init(&reader, fields.value[1], fields.size[1]);
		pk_cbor_read(&reader, &item);
		data_size = auth_data(data, item.bytes, (size_t)item.value,
		    (uint8_t)((presence ? PK_FLAG_UP : 0) | (param != NULL ? PK_FLAG_UV : 0)), found);
		signature_size = sign(card.keys[found], data, data_size, bytes, signature);
		pk_cbor_put_map(&writer, 3);
		pk_cbor_put_unsigned(&writer, 1);
		pk_cbor_put_map(&writer, 2);
		pk_cbor_put_text(&writer, "id", 2);
		pk_cbor_put_bytes(&writer, card.ids[found], 16);
		pk_cbor_put_text(&writer, "type", 4);
		pk_cbor_put_text(&writer, "public-key", 10);
		pk_cbor_put_unsigned(&writer, 2);
		pk_cbor_put_bytes(&writer, data, data_size);
		pk_cbor_put_unsigned(&writer, 3);
		pk_cbor_put_bytes(&writer, signature, signature_size);
		break;
	case PK_CTAP2_RESET:
		CHECK(size == 0, "authenticatorReset carries no request");
		if (card.reset_mode == RESET_LATE) {
			reply[0] = PK_CTAP2_NOT_ALLOWED;
			return 1;
		}
		if (card.reset_mode == RESET_WAIT) {
			/* No answer yet: the touch is awaited, and a CANCEL ends it (io_write). */
			card.reset_waiting = 1;
			return 0;
		}
		for (index = 0; index < card.count; index++)
			EVP_PKEY_free(card.keys[index]);
		card.count = 0;
		card.pin_set = 0;
		card.retries = 8;
		card.token_valid = 0;
		return 1;
	default:
		reply[0] = 0x01;
		return 1;
	}
	CHECK(writer.error == 0, "authenticator answer fits");
	return 1 + writer.length;
}

/* The library's output: a report the authenticator takes. */
static int
io_write(
	void *context,
	const uint8_t *report,
	size_t size)
{
	uint8_t reply[PK_HID_MESSAGE_MAX];
	uint8_t init[17];
	const uint8_t *packet;
	uint32_t channel;
	size_t take;
	size_t length;

	(void)context;
	CHECK(size == PK_HID_REPORT + 1 && report[0] == 0, "an output report with ID 0");
	packet = report + 1;
	channel = ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) | ((uint32_t)packet[2] << 8) | packet[3];
	if (packet[4] & 0x80) {
		card.command = packet[4];
		card.length = ((size_t)packet[5] << 8) | packet[6];
		take = card.length < PK_HID_INIT_DATA ? card.length : PK_HID_INIT_DATA;
		memcpy(card.message, packet + 7, take);
		card.received = take;
		card.next_sequence = 0;
		card.channel = channel;
	} else {
		CHECK(packet[4] == card.next_sequence, "continuations in order");
		card.next_sequence++;
		take = card.length - card.received < PK_HID_CONT_DATA ? card.length - card.received : PK_HID_CONT_DATA;
		memcpy(card.message + card.received, packet + 5, take);
		card.received += take;
	}
	if (card.received < card.length)
		return 0;

	/* A whole message. */
	if (card.command == PK_HID_INIT) {
		memcpy(init, card.message, 8);
		init[8] = 0x01; init[9] = 0x02; init[10] = 0x03; init[11] = 0x04;
		init[12] = 2; init[13] = 5; init[14] = 0; init[15] = 0; init[16] = PK_HID_CAPABILITY_CBOR;
		/* Another program's INIT answer first, with another nonce. */
		init[0] ^= 0xff;
		queue_message(PK_HID_BROADCAST, PK_HID_INIT, init, sizeof(init));
		init[0] ^= 0xff;
		queue_message(PK_HID_BROADCAST, PK_HID_INIT, init, sizeof(init));
		return 0;
	}
	if (card.command == PK_HID_CBOR) {
		length = answer(card.message[0], card.message + 1, card.length - 1, reply);
		if (length == 0) {
			/* The touch awaited: KEEPALIVEs until a CANCEL comes. */
			memset(init, 0, sizeof(init));
			init[0] = PK_HID_KEEPALIVE_UP_NEEDED;
			for (take = 0; take < 3; take++)
				queue_message(card.channel, PK_HID_KEEPALIVE, init, 1);
			return 0;
		}
		queue_message(card.channel, PK_HID_CBOR, reply, length);
		return 0;
	}
	if (card.command == PK_HID_CANCEL) {
		card.cancels++;
		if (card.reset_waiting) {
			card.reset_waiting = 0;
			reply[0] = PK_CTAP2_KEEPALIVE_CANCEL;
			queue_message(card.channel, PK_HID_CBOR, reply, 1);
		}
		return 0;
	}
	return 0;
}

/* The library's input: the next queued report. */
static int
io_read(
	void *context,
	uint8_t *report,
	size_t size,
	unsigned timeout_ms)
{
	(void)context;
	(void)timeout_ms;
	if (card.head == card.tail)
		return ETIMEDOUT;
	memcpy(report, card.queue[card.head % QUEUE_MAX], size);
	card.head++;
	return 0;
}

/* Gives the card's answer from where it is: whole on an extended reader, otherwise up to 256 bytes and "61xx" while more is left. */
static void
nfc_give(
	uint8_t *response,
	size_t *response_size)
{
	size_t left;
	size_t take;

	left = nfc.answer_size - nfc.answer_sent;
	take = left;
	if (!nfc.extended && take > 256U)
		take = 256U;
	memcpy(response, nfc.answer + nfc.answer_sent, take);
	nfc.answer_sent += take;
	left -= take;
	if (left == 0U) {
		response[take] = 0x90;
		response[take + 1U] = 0x00;
	} else {
		response[take] = 0x61;
		response[take + 1U] = (uint8_t)(left >= 256U ? 0U : left);
	}
	*response_size = take + 2U;
}

/* The NFC card: one APDU in, its response out. */
static int
nfc_transmit(
	void *context,
	const uint8_t *command,
	size_t size,
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	unsigned timeout_ms)
{
	const uint8_t *data;
	size_t length;
	uint8_t cla;
	uint8_t ins;

	(void)context;
	(void)timeout_ms;
	CHECK(size >= 4U && capacity >= 256U + 2U, "an APDU and room for an answer");
	cla = command[0];
	ins = command[1];

	/* The data: none (a header, maybe an expected length), short (Lc, data, Le), or extended (0, two of Lc, data, two of Le). */
	data = NULL;
	length = 0U;
	if (size == 7U && command[4] == 0U) {
		CHECK(nfc.extended, "an extended APDU (no data) on the extended reader");
	} else if (size > 7U && command[4] == 0U) {
		length = ((size_t)command[5] << 8) | command[6];
		data = command + 7;
		CHECK(nfc.extended && size == 7U + length + 2U, "an extended APDU on the extended reader");
	} else if (size > 5U) {
		length = command[4];
		data = command + 5;
		CHECK(size == 5U + length || size == 5U + length + 1U, "a short APDU's length");
	}

	/* SELECT of the FIDO applet. */
	if (cla == 0x00 && ins == 0xa4) {
		CHECK(command[2] == 0x04 && length == 8U && memcmp(data, "\xa0\x00\x00\x06\x47\x2f\x00\x01", 8) == 0, "SELECT the FIDO AID");
		nfc.selected = 1;
		memcpy(response, "FIDO_2_0\x90\x00", 10);
		*response_size = 10U;
		return 0;
	}
	CHECK(nfc.selected, "the applet selected first");

	/* NFCCTAP_MSG: a block of the message; the last one owes a keepalive, the answer made. */
	if ((cla & 0xefU) == 0x80 && ins == 0x10) {
		CHECK(command[2] == 0x80, "NFCCTAP_MSG takes NFCCTAP_GETRESPONSE");
		memcpy(nfc.message + nfc.length, data, length);
		nfc.length += length;
		if ((cla & 0x10U) != 0U) {
			nfc.chained++;
			response[0] = 0x90;
			response[1] = 0x00;
			*response_size = 2U;
			return 0;
		}
		if (nfc.message[0] == NFC_ECHO) {
			nfc.answer[0] = 0;
			memcpy(nfc.answer + 1, nfc.message + 1, nfc.length - 1U);
			nfc.answer_size = nfc.length;
		} else {
			nfc.answer_size = answer(nfc.message[0], nfc.message + 1, nfc.length - 1U, nfc.answer);
		}
		nfc.answer_sent = 0U;
		nfc.answer_ready = 0;
		nfc.length = 0U;
		response[0] = PK_HID_KEEPALIVE_UP_NEEDED;
		response[1] = 0x91;
		response[2] = 0x00;
		*response_size = 3U;
		return 0;
	}

	/* NFCCTAP_GETRESPONSE: the answer's first part, after one more keepalive on every other poll. */
	if (cla == 0x80 && ins == 0x11) {
		nfc.polls++;
		nfc_give(response, response_size);
		return 0;
	}

	/* GET RESPONSE: the next part. */
	if (cla == 0x00 && ins == 0xc0) {
		nfc.get_responses++;
		CHECK(nfc.answer_sent < nfc.answer_size, "GET RESPONSE only while more is left");
		nfc_give(response, response_size);
		return 0;
	}

	/* Anything else. */
	CHECK(0, "an APDU the card knows");
	response[0] = 0x6d;
	response[1] = 0x00;
	*response_size = 2U;
	return 0;
}

/* Counts the KEEPALIVEs the library was told of. */
static void
keepalive(
	void *context,
	uint8_t status)
{
	(void)context;
	if (status == PK_HID_KEEPALIVE_UP_NEEDED)
		card.keepalives++;
}

/* The whole flow on one authenticator. */
static void
run(
	unsigned protocol,
	int ctap21,
	int kind)
{
	struct pk_hid hid;
	struct pk_hid_io io;
	struct pk_nfc nfc_key;
	struct pk_nfc_io nfc_io;
	uint8_t echo[NFC_ECHO_SIZE];
	uint8_t echoed[NFC_ECHO_SIZE + 16U];
	size_t echoed_size;
	struct pk_transport transport;
	struct pk_device device;
	struct pk_info info;
	struct pk_make_request make;
	struct pk_made_credential made;
	struct pk_assertion_request ask;
	struct pk_assertion_reply reply;
	struct pk_expectation expectation;
	struct pk_credential stored;
	struct pk_assertion assertion;
	const uint8_t *allow[2];
	size_t allow_sizes[2];
	uint8_t other[16];
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t token_size;
	size_t matched;
	uint32_t count;
	unsigned retries;
	char label[64];
	int error;

	memset(&card, 0, sizeof(card));
	card.protocol = protocol;
	card.ctap21 = ctap21;
	card.retries = 8;
	card.agreement = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");

	/* The channel, or the applet behind the NFC reader. */
	if (kind == RUN_HID) {
		io.context = NULL;
		io.write = io_write;
		io.read = io_read;
		error = pk_hid_open(&hid, &io, 1000);
		snprintf(label, sizeof(label), "p%u: INIT", protocol);
		CHECK(error == 0 && hid.channel == 0x01020304U && (hid.capabilities & PK_HID_CAPABILITY_CBOR) != 0, label);
		pk_hid_transport(&transport, &hid);
	} else {
		memset(&nfc, 0, sizeof(nfc));
		nfc.extended = kind == RUN_NFC_EXTENDED;
		nfc_io.context = NULL;
		nfc_io.transmit = nfc_transmit;
		nfc_io.max_command = nfc.extended ? 65544U : PK_NFC_SHORT_COMMAND;
		nfc_io.extended = nfc.extended;
		error = pk_nfc_open(&nfc_key, &nfc_io, 1000);
		snprintf(label, sizeof(label), "p%u nfc%d: SELECT", protocol, kind);
		CHECK(error == 0 && nfc_key.versions == PK_NFC_VERSION_FIDO2, label);
		pk_nfc_transport(&transport, &nfc_key);
	}
	pk_device_init(&device, &transport, 1000);
	device.keepalive = keepalive;

	/* GetInfo. */
	error = pk_ctap2_get_info(&device, &info);
	CHECK(error == 0, "GetInfo");
	CHECK(pk_ctap2_choose_protocol(&info) == protocol, "the protocol chosen");
	CHECK(((info.options & PK_OPTION_PIN_UV_TOKEN) != 0) == ctap21, "pinUvAuthToken option");
	CHECK((info.options & PK_OPTION_CLIENT_PIN) != 0 && (info.options & PK_OPTION_CLIENT_PIN_SET) == 0, "no PIN yet");
	CHECK(card.keepalives > 0, "KEEPALIVE told");

	/* setPIN, a wrong PIN, the right one. */
	error = pk_ctap2_set_pin(&device, protocol, "1234");
	CHECK(error == 0 && card.pin_set, "setPIN");
	error = pk_ctap2_get_info(&device, &info);
	CHECK(error == 0 && (info.options & PK_OPTION_CLIENT_PIN_SET) != 0, "PIN set in GetInfo");
	error = pk_ctap2_pin_token(&device, &info, protocol, "9999", PK_PERMISSION_GET_ASSERTION, "zedbsd.login", token, &token_size);
	CHECK(error == EPROTO && device.last_status == PK_CTAP2_PIN_INVALID, "wrong PIN: PIN_INVALID");
	error = pk_ctap2_pin_retries(&device, protocol, &retries);
	CHECK(error == 0 && retries == 7, "one retry fewer");
	error = pk_ctap2_pin_token(&device, &info, protocol, "1234", PK_PERMISSION_MAKE_CREDENTIAL, "zedbsd.login", token, &token_size);
	CHECK(error == 0 && token_size == 32 && memcmp(token, card.token, 32) == 0, "the token decrypted");

	/* MakeCredential. */
	memset(&make, 0, sizeof(make));
	make.rp_id = "zedbsd.login";
	make.user_id = (const uint8_t *)"kei-user-id-0001";
	make.user_id_size = 16;
	make.user_name = "kei";
	memset(make.client_data_hash, 0x11, 32);
	make.pin_token = token;
	make.pin_token_size = token_size;
	make.pin_protocol = protocol;
	error = pk_ctap2_make_credential(&device, &make, &made);
	CHECK(error == 0 && made.id_size == 16 && memcmp(made.id, card.ids[0], 16) == 0, "MakeCredential");
	CHECK((made.flags & (PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_AT)) == (PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_AT), "new credential flags");
	card.lie_rp = 1;
	error = pk_ctap2_make_credential(&device, &make, &made);
	CHECK(error == EBADMSG, "a new credential with another relying party's hash is refused");
	card.lie_rp = 0;
	error = pk_ctap2_make_credential(&device, &make, &made);
	CHECK(error == 0, "MakeCredential again");

	/* The silent question: held, and not held. */
	memset(&ask, 0, sizeof(ask));
	ask.rp_id = "zedbsd.login";
	memset(ask.client_data_hash, 0x22, 32);
	memset(other, 0x77, sizeof(other));
	allow[0] = other;
	allow_sizes[0] = 16;
	allow[1] = made.id;
	allow_sizes[1] = made.id_size;
	ask.allow_ids = allow;
	ask.allow_sizes = allow_sizes;
	ask.allow_count = 1;
	error = pk_ctap2_get_assertion(&device, &ask, &reply);
	CHECK(error == EPROTO && device.last_status == PK_CTAP2_NO_CREDENTIALS, "silent question: not held");
	ask.allow_count = 2;
	error = pk_ctap2_get_assertion(&device, &ask, &reply);
	CHECK(error == 0 && reply.credential_id_size == 16 && (reply.auth_data[32] & PK_FLAG_UP) == 0, "silent question: held, no UP");

	/* GetAssertion with the token, verified. */
	error = pk_ctap2_pin_token(&device, &info, protocol, "1234", PK_PERMISSION_GET_ASSERTION, "zedbsd.login", token, &token_size);
	CHECK(error == 0, "token for the assertion");
	ask.presence = 1;
	ask.pin_token = token;
	ask.pin_token_size = token_size;
	ask.pin_protocol = protocol;
	error = pk_ctap2_get_assertion(&device, &ask, &reply);
	CHECK(error == 0 && reply.signature_size > 0, "GetAssertion");
	memset(&stored, 0, sizeof(stored));
	stored.id = made.id;
	stored.id_size = made.id_size;
	stored.cose_key = made.cose_key;
	stored.cose_key_size = made.cose_key_size;
	stored.sign_count = made.sign_count;
	memset(&expectation, 0, sizeof(expectation));
	expectation.rp_id = "zedbsd.login";
	memcpy(expectation.client_data_hash, ask.client_data_hash, 32);
	expectation.required_flags = PK_FLAG_UP | PK_FLAG_UV;
	expectation.credentials = &stored;
	expectation.credential_count = 1;
	assertion.credential_id = reply.credential_id;
	assertion.credential_id_size = reply.credential_id_size;
	assertion.auth_data = reply.auth_data;
	assertion.auth_data_size = reply.auth_data_size;
	assertion.signature = reply.signature;
	assertion.signature_size = reply.signature_size;
	error = pk_verify_assertion(&expectation, &assertion, &matched, &count);
	CHECK(error == 0 && matched == 0 && count > stored.sign_count, "the assertion verifies");

	/* changePIN, then the new PIN's token. */
	error = pk_ctap2_change_pin(&device, protocol, "1234", "567890");
	CHECK(error == 0, "changePIN");
	error = pk_ctap2_pin_token(&device, &info, protocol, "567890", PK_PERMISSION_GET_ASSERTION, "zedbsd.login", token, &token_size);
	CHECK(error == 0, "the new PIN's token");
	error = pk_ctap2_change_pin(&device, protocol, "0000", "111111");
	CHECK(error == EPROTO && device.last_status == PK_CTAP2_PIN_INVALID, "changePIN with a wrong PIN");
	error = pk_ctap2_set_pin(&device, protocol, "12");
	CHECK(error == EINVAL, "a PIN shorter than 4 is refused before sending");

	/* Over NFC: a long message each way (command chaining out, GET RESPONSE back on the short reader), and the polls. */
	if (kind != RUN_HID) {
		echo[0] = NFC_ECHO;
		for (count = 1; count < NFC_ECHO_SIZE; count++)
			echo[count] = (uint8_t)count;
		nfc.chained = 0U;
		nfc.get_responses = 0U;
		error = pk_nfc_transact(&nfc_key, echo, sizeof(echo), echoed, sizeof(echoed), &echoed_size, 1000, keepalive, NULL);
		CHECK(error == 0 && echoed_size == NFC_ECHO_SIZE && echoed[0] == 0 && memcmp(echoed + 1, echo + 1, NFC_ECHO_SIZE - 1U) == 0, "a long message echoed");
		if (kind == RUN_NFC_SHORT)
			CHECK(nfc.chained == 2U && nfc.get_responses == 2U, "two chained blocks before the last, two GET RESPONSEs");
		else
			CHECK(nfc.chained == 0U && nfc.get_responses == 0U, "one extended APDU each way");
		CHECK(nfc.polls > 0U, "NFCCTAP_GETRESPONSE after each keepalive");
		error = pk_nfc_transact(&nfc_key, echo, sizeof(echo), echoed, 100U, &echoed_size, 1000, keepalive, NULL);
		CHECK(error == EMSGSIZE, "an answer past the room is refused");
	}

	EVP_PKEY_free(card.agreement);
	for (count = 0; count < card.count; count++)
		EVP_PKEY_free(card.keys[count]);
}

/* Sends CANCEL on the channel at the first KEEPALIVE, as the helper does on SIGTERM. */
static void
keepalive_cancel(
	void *context,
	uint8_t status)
{
	if (status != PK_HID_KEEPALIVE_UP_NEEDED)
		return;
	card.keepalives++;
	if (card.keepalives == 1U)
		CHECK(pk_hid_cancel(context) == 0, "CANCEL sent");
}

/* authenticatorReset over CTAPHID: out of the window, cancelled while it waits for the touch, and taken. */
static void
run_reset(void)
{
	struct pk_hid hid;
	struct pk_hid_io io;
	struct pk_transport transport;
	struct pk_device device;
	struct pk_make_request make;
	struct pk_made_credential made;
	uint8_t token[PK_PIN_TOKEN_MAX];
	size_t token_size;
	struct pk_info info;
	unsigned index;
	int error;

	memset(&card, 0, sizeof(card));
	card.protocol = 2;
	card.ctap21 = 1;
	card.retries = 8;
	card.agreement = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	io.context = NULL;
	io.write = io_write;
	io.read = io_read;
	error = pk_hid_open(&hid, &io, 1000);
	CHECK(error == 0, "reset: INIT");
	pk_hid_transport(&transport, &hid);
	pk_device_init(&device, &transport, 1000);
	device.keepalive = keepalive;

	/* A key with a PIN and a credential. */
	error = pk_ctap2_set_pin(&device, 2, "1234");
	CHECK(error == 0, "reset: setPIN");
	error = pk_ctap2_get_info(&device, &info);
	CHECK(error == 0, "reset: GetInfo");
	error = pk_ctap2_pin_token(&device, &info, 2, "1234", PK_PERMISSION_MAKE_CREDENTIAL, "zedbsd.login", token, &token_size);
	CHECK(error == 0, "reset: token");
	memset(&make, 0, sizeof(make));
	make.rp_id = "zedbsd.login";
	make.user_id = (const uint8_t *)"kei-user-id-0001";
	make.user_id_size = 16;
	make.user_name = "kei";
	memset(make.client_data_hash, 0x11, 32);
	make.pin_token = token;
	make.pin_token_size = token_size;
	make.pin_protocol = 2;
	error = pk_ctap2_make_credential(&device, &make, &made);
	CHECK(error == 0 && card.count == 1, "reset: a credential to forget");

	/* Out of the window: NOT_ALLOWED, nothing forgotten. */
	card.reset_mode = RESET_LATE;
	error = pk_ctap2_reset(&device);
	CHECK(error == EPROTO && device.last_status == PK_CTAP2_NOT_ALLOWED, "reset out of the window: NOT_ALLOWED");
	CHECK(card.count == 1 && card.pin_set, "reset out of the window: the credential and the PIN kept");

	/* Waiting for the touch, cancelled: KEEPALIVE_CANCEL, nothing forgotten. */
	card.reset_mode = RESET_WAIT;
	card.keepalives = 0;
	device.keepalive = keepalive_cancel;
	device.keepalive_context = &hid;
	error = pk_ctap2_reset(&device);
	CHECK(error == EPROTO && device.last_status == PK_CTAP2_KEEPALIVE_CANCEL, "reset cancelled: KEEPALIVE_CANCEL");
	CHECK(card.cancels == 1 && !card.reset_waiting, "reset cancelled: one CANCEL ended the wait");
	CHECK(card.count == 1 && card.pin_set, "reset cancelled: the credential and the PIN kept");

	/* Taken: every credential and the PIN go. */
	card.reset_mode = RESET_TAKEN;
	device.keepalive = keepalive;
	device.keepalive_context = NULL;
	error = pk_ctap2_reset(&device);
	CHECK(error == 0, "reset taken");
	CHECK(card.count == 0 && !card.pin_set, "reset taken: the credential and the PIN gone");
	error = pk_ctap2_get_info(&device, &info);
	CHECK(error == 0 && (info.options & PK_OPTION_CLIENT_PIN_SET) == 0, "reset taken: no PIN in GetInfo");

	EVP_PKEY_free(card.agreement);
	for (index = 0; index < card.count; index++)
		EVP_PKEY_free(card.keys[index]);
}

int
main(void)
{
	/* A CTAP 2.1 key with protocol 2, and a CTAP 2.0 key with protocol 1, over CTAPHID and over NFC (short and extended APDUs). */
	run(2, 1, RUN_HID);
	run(1, 0, RUN_HID);
	run(2, 1, RUN_NFC_SHORT);
	run(1, 0, RUN_NFC_EXTENDED);
	run_reset();

	if (failures != 0) {
		printf("libpasskey-ctap2-host-test: FAIL (%u)\n", failures);
		return 1;
	}
	printf("libpasskey-ctap2-host-test: PASS\n");
	return 0;
}
