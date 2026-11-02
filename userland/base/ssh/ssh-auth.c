/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The service request and user authentication messages.
 */

#include <string.h>

#include "ssh-auth.h"

/* The message number of SSH_MSG_SERVICE_REQUEST (RFC 4250 4.1.2). */
#define AUTH_MSG_SERVICE_REQUEST	5U

/* The message number of SSH_MSG_SERVICE_ACCEPT (RFC 4250 4.1.2). */
#define AUTH_MSG_SERVICE_ACCEPT		6U

/* The length of the algorithm name "ssh-ed25519" in bytes. */
#define AUTH_KEY_NAME_LENGTH		11U

/*
 * The most bytes the data a signature covers can hold: the session id
 * string, and a request payload that fits a packet.
 */
#define AUTH_SIGNED_DATA_LIMIT		(SSH_PACKET_LIMIT + 64U)

/* The room for the signature blob that ssh_key_sign appends (83 bytes), with some to spare. */
#define AUTH_SIGNATURE_LIMIT		128U

/* The first byte of a UTF-8 two-byte sequence that can stand for a C1 control character (U+0080 to U+009F). */
#define AUTH_UTF8_C1_LEAD		0xc2U

static void auth_record_error(struct ssh_buf *out, int error);
static void auth_put_head(const char *user, const char *method, struct ssh_buf *out);
static void auth_put_key_blob_string(const uint8_t *public_key, struct ssh_buf *out);
static int auth_read_name(struct ssh_reader *reader, char *out, size_t capacity);
static int auth_read_password(struct ssh_reader *reader, char *out);
static int auth_parse_publickey(struct ssh_reader *reader, struct ssh_auth_request *request);
static int auth_is_method(const uint8_t *method, size_t size, const char *expected);
static size_t auth_utf8_length(const uint8_t *bytes, size_t left);
static int auth_in_range(uint8_t byte, uint8_t low, uint8_t high);

/*
 * Appends SSH_MSG_SERVICE_REQUEST (RFC 4253 10) for service to out.
 */
void
ssh_auth_put_service_request(
	const char *service,
	struct ssh_buf *out)
{
	/* Writes the message number. */
	ssh_buf_put_u8(out, AUTH_MSG_SERVICE_REQUEST);

	/* Writes the service name. */
	ssh_buf_put_cstring(out, service);
}

/*
 * Appends SSH_MSG_SERVICE_ACCEPT for service to out.
 */
void
ssh_auth_put_service_accept(
	const char *service,
	struct ssh_buf *out)
{
	/* Writes the message number. */
	ssh_buf_put_u8(out, AUTH_MSG_SERVICE_ACCEPT);

	/* Writes the service name. */
	ssh_buf_put_cstring(out, service);
}

/*
 * Reads SSH_MSG_SERVICE_REQUEST or SSH_MSG_SERVICE_ACCEPT (the number is
 * the payload's first byte and must be one of them) into service
 * (capacity SSH_NAME_MAX + 1).
 *
 * A service name longer than SSH_NAME_MAX is EMSGSIZE; every other
 * malformed message, and a payload of another message, is EINVAL.
 */
int
ssh_auth_parse_service(
	const uint8_t *payload,
	size_t size,
	char *service)
{
	struct ssh_reader reader;
	uint8_t number;
	int error;

	/* Refuses arguments that cannot be read or written. */
	if (service == NULL)
		return EINVAL;
	service[0] = '\0';
	if (payload == NULL && size != 0)
		return EINVAL;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	number = ssh_reader_get_u8(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* Accepts the request and the accept, and nothing else. */
	if (number != AUTH_MSG_SERVICE_REQUEST && number != AUTH_MSG_SERVICE_ACCEPT)
		return EINVAL;

	/* Reads the service name; a name that is too long keeps its own error. */
	ssh_reader_get_cstring(&reader, service, SSH_NAME_MAX + 1U);
	error = ssh_reader_error(&reader);
	if (error != 0) {
		service[0] = '\0';
		return error;
	}

	/* Refuses bytes after the name. */
	error = ssh_reader_finish(&reader);
	if (error != 0) {
		service[0] = '\0';
		return EINVAL;
	}

	/* Succeeded: the service name is in place. */
	return 0;
}

/*
 * Reads a USERAUTH_REQUEST payload of size bytes into request.
 *
 * The password of a request is copied into the structure, which the
 * caller wipes with ssh_auth_request_clear; nothing else of it is kept
 * (the new password of a change request is only length-checked).
 */
int
ssh_auth_parse_request(
	const uint8_t *payload,
	size_t size,
	struct ssh_auth_request *request)
{
	struct ssh_reader reader;
	const uint8_t *method;
	size_t method_size;
	uint8_t number;
	int is_none;
	int is_publickey;
	int is_password;
	int error;

	/* Refuses a request structure that does not exist. */
	if (request == NULL)
		return EINVAL;

	/* Starts from an empty request, so an error leaves nothing behind. */
	ssh_wipe(request, sizeof(*request));

	/* Refuses bytes that are missing. */
	if (payload == NULL && size != 0)
		return EINVAL;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	number = ssh_reader_get_u8(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;
	if (number != SSH_MSG_USERAUTH_REQUEST)
		return EINVAL;

	/* Reads the user name. */
	error = auth_read_name(&reader, request->user, sizeof(request->user));
	if (error != 0) {
		ssh_auth_request_clear(request);
		return EINVAL;
	}

	/* Reads the service name. */
	error = auth_read_name(&reader, request->service, sizeof(request->service));
	if (error != 0) {
		ssh_auth_request_clear(request);
		return EINVAL;
	}

	/* Reads the method name, which may be any string: an unknown method is not an error. */
	ssh_reader_get_string(&reader, &method, &method_size);
	error = ssh_reader_error(&reader);
	if (error != 0) {
		ssh_auth_request_clear(request);
		return EINVAL;
	}

	/* Recognizes the method by its name. */
	is_none = auth_is_method(method, method_size, "none");
	is_publickey = auth_is_method(method, method_size, "publickey");
	is_password = auth_is_method(method, method_size, "password");

	/* Reads the fields of the method. */
	if (is_none) {
		/* "none" has no fields of its own. */
		request->method = SSH_AUTH_NONE;
		error = ssh_reader_finish(&reader);
	} else if (is_publickey) {
		/* "publickey" carries an algorithm, a key blob and maybe a signature. */
		request->method = SSH_AUTH_PUBLICKEY;
		error = auth_parse_publickey(&reader, request);
	} else if (is_password) {
		/* "password" carries a flag and the password, and with the flag the new password. */
		request->method = SSH_AUTH_PASSWORD;
		request->change = ssh_reader_get_bool(&reader);
		error = auth_read_password(&reader, request->password);
		if (error == 0 && request->change != 0)
			error = auth_read_password(&reader, NULL);
		if (error == 0)
			error = ssh_reader_finish(&reader);
	} else {
		/* Any other method is failed by the caller; the rest of the payload is not read. */
		request->method = SSH_AUTH_OTHER;
		error = 0;
	}
	if (error != 0) {
		ssh_auth_request_clear(request);
		return EINVAL;
	}

	/* Succeeded: the request is in place. */
	return 0;
}

/*
 * Wipes the password and clears the request.
 */
void
ssh_auth_request_clear(
	struct ssh_auth_request *request)
{
	/* Refuses a request structure that does not exist. */
	if (request == NULL)
		return;

	/* Overwrites the password and every other field in a way the compiler keeps. */
	ssh_wipe(request, sizeof(*request));
}

/*
 * Appends the data a USERAUTH_REQUEST signature covers (RFC 4252 7):
 * string session_id (32 bytes), then the first signed_size bytes of the
 * request payload (message number, user, service, "publickey", TRUE,
 * algorithm, key blob).
 *
 * A missing session id, or missing payload bytes, records EINVAL in out.
 */
void
ssh_auth_put_signed_data(
	const uint8_t *session_id,
	const uint8_t *payload,
	size_t signed_size,
	struct ssh_buf *out)
{
	/* Refuses arguments that cannot be read. */
	if (session_id == NULL || (payload == NULL && signed_size != 0)) {
		auth_record_error(out, EINVAL);
		return;
	}

	/* Writes the session identifier as a string. */
	ssh_buf_put_string(out, session_id, SSH_SHA256_SIZE);

	/* Writes the request up to the signature, as the request has it. */
	ssh_buf_put_bytes(out, payload, signed_size);
}

/*
 * Appends USERAUTH_FAILURE: the name-list methods (NUL-terminated, e.g.
 * "publickey,password") and the partial success boolean (always FALSE
 * here).
 */
void
ssh_auth_put_failure(
	const char *methods,
	struct ssh_buf *out)
{
	/* Writes the message number. */
	ssh_buf_put_u8(out, SSH_MSG_USERAUTH_FAILURE);

	/* Writes the methods that can continue. */
	ssh_buf_put_cstring(out, methods);

	/* Writes partial success, which is FALSE because the request was not successful. */
	ssh_buf_put_bool(out, 0);
}

/*
 * Appends USERAUTH_SUCCESS.
 */
void
ssh_auth_put_success(
	struct ssh_buf *out)
{
	/* Writes the message number, which is the whole message. */
	ssh_buf_put_u8(out, SSH_MSG_USERAUTH_SUCCESS);
}

/*
 * Appends USERAUTH_PK_OK (RFC 4252 7): string algorithm, string the key
 * blob of public_key.
 */
void
ssh_auth_put_pk_ok(
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	/* Writes the message number. */
	ssh_buf_put_u8(out, SSH_MSG_USERAUTH_PK_OK);

	/* Writes the algorithm of the key. */
	ssh_buf_put_string(out, SSH_KEY_NAME, AUTH_KEY_NAME_LENGTH);

	/* Writes the key blob. */
	auth_put_key_blob_string(public_key, out);
}

/*
 * Appends a "none" request for user and the connection service.
 */
void
ssh_auth_put_none(
	const char *user,
	struct ssh_buf *out)
{
	/* Writes the message number, the user, the service and the method. */
	auth_put_head(user, "none", out);
}

/*
 * Appends a "publickey" request for user without a signature (the query
 * of RFC 4252 7 that asks whether the key would be accepted).
 */
void
ssh_auth_put_publickey_query(
	const char *user,
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	/* Writes the message number, the user, the service and the method. */
	auth_put_head(user, "publickey", out);

	/* Writes has_signature as FALSE, which makes the request a query. */
	ssh_buf_put_bool(out, 0);

	/* Writes the algorithm of the key. */
	ssh_buf_put_cstring(out, SSH_KEY_NAME);

	/* Writes the key blob. */
	auth_put_key_blob_string(public_key, out);
}

/*
 * Appends a signed "publickey" request for user: builds the request up
 * to the key blob with has_signature TRUE, signs
 * ssh_auth_put_signed_data of it with key (ssh_key_sign) and appends the
 * signature blob.
 *
 * When signing fails, what this call appended is taken back; when out
 * itself holds an error, that error is returned.
 */
int
ssh_auth_put_publickey_signed(
	const char *user,
	const struct ssh_key *key,
	const uint8_t *session_id,
	struct ssh_buf *out)
{
	struct ssh_buf signed_data;
	struct ssh_buf signature;
	size_t start;
	size_t signed_size;
	int error;

	/* Refuses arguments that cannot be used: a public-only key signs nothing. */
	if (user == NULL || key == NULL ||
	    session_id == NULL || out == NULL)
		return EINVAL;
	if (key->private == 0)
		return EINVAL;

	/* Refuses a buffer that already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Writes the request up to the key blob, with has_signature TRUE. */
	start = out->size;
	auth_put_head(user, "publickey", out);
	ssh_buf_put_bool(out, 1);
	ssh_buf_put_cstring(out, SSH_KEY_NAME);
	auth_put_key_blob_string(key->public_key, out);
	error = ssh_buf_error(out);
	if (error != 0)
		return error;
	signed_size = out->size - start;

	/* Builds the data the signature covers: the session id and the request so far. */
	ssh_buf_init(&signed_data, AUTH_SIGNED_DATA_LIMIT, 0);
	ssh_auth_put_signed_data(session_id, out->data + start, signed_size, &signed_data);
	error = ssh_buf_error(&signed_data);
	if (error != 0) {
		ssh_buf_free(&signed_data);
		out->size = start;
		return error;
	}

	/* Signs it with the private key. */
	ssh_buf_init(&signature, AUTH_SIGNATURE_LIMIT, 0);
	error = ssh_key_sign(key, signed_data.data, signed_data.size, &signature);
	ssh_buf_free(&signed_data);
	if (error != 0) {
		ssh_buf_free(&signature);
		out->size = start;
		return error;
	}

	/* Appends the signature blob as the last string of the request. */
	ssh_buf_put_string(out, signature.data, signature.size);
	ssh_buf_free(&signature);
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the signed request is appended. */
	return 0;
}

/*
 * Appends a "password" request for user (FALSE, then the password; out
 * should be secret).
 */
void
ssh_auth_put_password(
	const char *user,
	const char *password,
	struct ssh_buf *out)
{
	/* Writes the message number, the user, the service and the method. */
	auth_put_head(user, "password", out);

	/* Writes the flag that says this is not a password change. */
	ssh_buf_put_bool(out, 0);

	/* Writes the password. */
	ssh_buf_put_cstring(out, password);
}

/*
 * Reads USERAUTH_FAILURE into methods (a name-list, capacity bytes with
 * its NUL) and *partial.
 *
 * partial may be NULL when the caller does not need the flag.  A name-list
 * that does not fit capacity is EMSGSIZE.
 */
int
ssh_auth_parse_failure(
	const uint8_t *payload,
	size_t size,
	char *methods,
	size_t capacity,
	int *partial)
{
	struct ssh_reader reader;
	const char *list;
	size_t list_size;
	uint8_t number;
	int flag;
	int error;

	/* Refuses arguments that cannot be written. */
	if (methods == NULL || capacity == 0)
		return EINVAL;
	methods[0] = '\0';
	if (partial != NULL)
		*partial = 0;
	if (payload == NULL && size != 0)
		return EINVAL;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	number = ssh_reader_get_u8(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;
	if (number != SSH_MSG_USERAUTH_FAILURE)
		return EINVAL;

	/* Reads the methods that can continue and the partial success flag. */
	ssh_reader_get_namelist(&reader, &list, &list_size);
	flag = ssh_reader_get_bool(&reader);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a list that does not fit the caller's room with its NUL. */
	if (list_size >= capacity)
		return EMSGSIZE;

	/* Hands the list and the flag to the caller. */
	if (list_size > 0)
		memcpy(methods, list, list_size);
	methods[list_size] = '\0';
	if (partial != NULL)
		*partial = flag;

	/* Succeeded: the failure message is read. */
	return 0;
}

/*
 * Reads USERAUTH_PK_OK and checks that it names ssh-ed25519 and
 * public_key (EPROTO when it names another key).
 */
int
ssh_auth_parse_pk_ok(
	const uint8_t *payload,
	size_t size,
	const uint8_t *public_key)
{
	struct ssh_reader reader;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *blob;
	size_t blob_size;
	uint8_t named_key[SSH_ED25519_PUBLIC];
	uint8_t number;
	int same;
	int error;

	/* Refuses arguments that cannot be read. */
	if (public_key == NULL)
		return EINVAL;
	if (payload == NULL && size != 0)
		return EINVAL;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	number = ssh_reader_get_u8(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;
	if (number != SSH_MSG_USERAUTH_PK_OK)
		return EINVAL;

	/* Reads the algorithm and the key blob. */
	ssh_reader_get_string(&reader, &name, &name_size);
	ssh_reader_get_string(&reader, &blob, &blob_size);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a reply that names another algorithm. */
	same = auth_is_method(name, name_size, SSH_KEY_NAME);
	if (same == 0)
		return EPROTO;

	/* Reads the key the reply names; a key of another type is a reply for another key. */
	error = ssh_key_parse_public_blob(blob, blob_size, named_key);
	if (error == ENOTSUP)
		return EPROTO;
	if (error != 0)
		return EINVAL;

	/* Refuses a reply for a key other than the one asked about. */
	same = ssh_ct_equal(named_key, public_key, SSH_ED25519_PUBLIC);
	if (same == 0)
		return EPROTO;

	/* Succeeded: the server accepts the key. */
	return 0;
}

/*
 * Reads USERAUTH_BANNER (RFC 4252 5.4) into text (capacity bytes with the
 * NUL), replacing every control character other than LF and TAB, and
 * every byte that does not belong to valid UTF-8, with '?', so that a
 * server cannot drive the user's terminal.  A longer banner is cut.
 *
 * The C1 controls (U+0080 to U+009F, which some terminals act on) are
 * control characters too; each is replaced by one '?'.  Each byte of an
 * invalid sequence is replaced by one '?'.  The text is cut at a
 * character boundary at SSH_BANNER_MAX bytes or capacity - 1 bytes,
 * whichever is smaller.
 */
int
ssh_auth_parse_banner(
	const uint8_t *payload,
	size_t size,
	char *text,
	size_t capacity)
{
	struct ssh_reader reader;
	const uint8_t *message;
	size_t message_size;
	const uint8_t *language;
	size_t language_size;
	uint8_t number;
	size_t limit;
	size_t offset;
	size_t used;
	size_t length;
	int keep;
	int error;

	/* Refuses arguments that cannot be written. */
	if (text == NULL || capacity == 0)
		return EINVAL;
	text[0] = '\0';
	if (payload == NULL && size != 0)
		return EINVAL;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	number = ssh_reader_get_u8(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;
	if (number != SSH_MSG_USERAUTH_BANNER)
		return EINVAL;

	/* Reads the message and the language tag. */
	ssh_reader_get_string(&reader, &message, &message_size);
	ssh_reader_get_string(&reader, &language, &language_size);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Decides how many bytes of text are kept. */
	limit = capacity - 1U;
	if (limit > SSH_BANNER_MAX)
		limit = SSH_BANNER_MAX;

	/* Copies the message character by character, replacing what could drive a terminal. */
	offset = 0;
	used = 0;
	while (offset < message_size) {
		length = auth_utf8_length(message + offset, message_size - offset);
		keep = 1;
		if (length == 0) {
			/* A byte that does not belong to valid UTF-8 is replaced by itself alone. */
			keep = 0;
			length = 1;
		} else if (length == 1) {
			/* ASCII keeps LF and TAB and replaces every other control character and DEL. */
			if (message[offset] < 0x20U &&
			    message[offset] != 0x0aU &&
			    message[offset] != 0x09U)
				keep = 0;
			if (message[offset] == 0x7fU)
				keep = 0;
		} else if (length == 2 &&
		    message[offset] == AUTH_UTF8_C1_LEAD &&
		    message[offset + 1U] < 0xa0U) {
			/* A C1 control character, which is replaced as one character. */
			keep = 0;
		}

		/* Stops at the character that does not fit. */
		if (keep != 0 && length > limit - used)
			break;
		if (keep == 0 && used >= limit)
			break;

		/* Adds the character, or its replacement. */
		if (keep != 0) {
			memcpy(text + used, message + offset, length);
			used += length;
		} else {
			text[used] = '?';
			used++;
		}
		offset += length;
	}

	/* Ends the text. */
	text[used] = '\0';

	/* Succeeded: the banner is safe to show. */
	return 0;
}

/*
 * Records error in out when it holds no error yet.
 */
static void
auth_record_error(
	struct ssh_buf *out,
	int error)
{
	/* Keeps the first failure, as the buffer's contract says. */
	if (out->error == 0)
		out->error = error;
}

/*
 * Appends the start every request has: the message number, the user, the
 * connection service and the method name.
 */
static void
auth_put_head(
	const char *user,
	const char *method,
	struct ssh_buf *out)
{
	/* Writes the message number. */
	ssh_buf_put_u8(out, SSH_MSG_USERAUTH_REQUEST);

	/* Writes the user name. */
	ssh_buf_put_cstring(out, user);

	/* Writes the service that starts after authentication. */
	ssh_buf_put_cstring(out, SSH_SERVICE_CONNECTION);

	/* Writes the method name. */
	ssh_buf_put_cstring(out, method);
}

/*
 * Appends the key blob of public_key as a string (the blob is
 * SSH_KEY_BLOB_SIZE bytes, so its length is known without building it
 * apart).
 */
static void
auth_put_key_blob_string(
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	/* Writes the length of the blob. */
	ssh_buf_put_u32(out, SSH_KEY_BLOB_SIZE);

	/* Writes the blob itself. */
	ssh_key_put_public_blob(public_key, out);
}

/*
 * Reads a string into out as a name, turning every failure into EINVAL.
 */
static int
auth_read_name(
	struct ssh_reader *reader,
	char *out,
	size_t capacity)
{
	int error;

	/* Reads the string; a name that is too long and a NUL inside are both malformed here. */
	ssh_reader_get_cstring(reader, out, capacity);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the name is in place. */
	return 0;
}

/*
 * Reads a password string.  The password is copied into out (SSH_PASSWORD_MAX
 * + 1 bytes, with a NUL) when out is not NULL, and only checked otherwise.
 * A password longer than SSH_PASSWORD_MAX, or with a NUL inside, is EINVAL.
 */
static int
auth_read_password(
	struct ssh_reader *reader,
	char *out)
{
	const uint8_t *password;
	size_t password_size;
	const void *nul;
	int error;

	/* Reads the string. */
	ssh_reader_get_string(reader, &password, &password_size);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a password that is too long, which is refused whole and never cut. */
	if (password_size > SSH_PASSWORD_MAX)
		return EINVAL;

	/* Looks for a NUL inside, which would end the password early for a program that uses it as text. */
	nul = NULL;
	if (password_size > 0)
		nul = memchr(password, 0, password_size);
	if (nul != NULL)
		return EINVAL;

	/* Keeps the password when the caller wants it. */
	if (out != NULL) {
		if (password_size > 0)
			memcpy(out, password, password_size);
		out[password_size] = '\0';
	}

	/* Succeeded: the password is acceptable. */
	return 0;
}

/*
 * Reads the fields of a "publickey" request (RFC 4252 7) into request.
 */
static int
auth_parse_publickey(
	struct ssh_reader *reader,
	struct ssh_auth_request *request)
{
	const uint8_t *blob;
	size_t blob_size;
	const uint8_t *signature;
	size_t signature_size;
	int same;
	int error;

	/* Reads whether a signature follows. */
	request->has_signature = ssh_reader_get_bool(reader);

	/* Reads the algorithm name. */
	error = auth_read_name(reader, request->algorithm, sizeof(request->algorithm));
	if (error != 0)
		return EINVAL;

	/* Reads the key blob, which stays where it is in the payload. */
	ssh_reader_get_string(reader, &blob, &blob_size);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;
	request->key_blob = blob;
	request->key_blob_size = blob_size;

	/* Reads the signature blob, which also stays where it is; the signed part of the payload ends before its string. */
	if (request->has_signature != 0) {
		request->signed_size = reader->offset;
		ssh_reader_get_string(reader, &signature, &signature_size);
		error = ssh_reader_error(reader);
		if (error != 0)
			return EINVAL;
		request->signature = signature;
		request->signature_size = signature_size;
	}

	/* Refuses bytes after the request. */
	error = ssh_reader_finish(reader);
	if (error != 0)
		return EINVAL;

	/* Leaves the request unsupported unless the algorithm is ssh-ed25519. */
	same = strcmp(request->algorithm, SSH_KEY_NAME);
	if (same != 0)
		return 0;

	/* Leaves the request unsupported unless the blob is an ssh-ed25519 key too. */
	error = ssh_key_parse_public_blob(blob, blob_size, request->public_key);
	if (error != 0)
		return 0;

	/* Marks the key as one this server can use. */
	request->supported = 1;

	/* Succeeded: the request is read, usable or not. */
	return 0;
}

/*
 * Reports whether the string of size bytes at method is exactly expected.
 */
static int
auth_is_method(
	const uint8_t *method,
	size_t size,
	const char *expected)
{
	size_t length;
	int differs;

	/* Compares the lengths first, so a string with a NUL or a longer tail never matches. */
	length = strlen(expected);
	if (size != length)
		return 0;

	/* Compares the bytes. */
	if (length == 0)
		return 1;
	differs = memcmp(method, expected, length);
	if (differs != 0)
		return 0;

	/* The string is the expected name. */
	return 1;
}

/*
 * Returns the length (1 to 4) of the valid UTF-8 sequence that starts at
 * bytes, with left bytes available, or 0 when what starts there is not
 * valid.
 *
 * Values referenced: RFC 3629 4, the syntax of UTF-8 byte sequences:
 * 00-7F; C2-DF and a continuation; E0 and A0-BF and a continuation;
 * E1-EC and two continuations; ED and 80-9F and a continuation; EE-EF and
 * two continuations; F0 and 90-BF and two continuations; F1-F3 and three
 * continuations; F4 and 80-8F and two continuations.  A continuation is
 * 80-BF.  The ranges that start at A0, 80-9F, 90 and 80-8F rule out
 * overlong forms, surrogates and values above U+10FFFF.
 */
static size_t
auth_utf8_length(
	const uint8_t *bytes,
	size_t left)
{
	uint8_t lead;
	size_t need;
	size_t index;
	uint8_t low;
	uint8_t high;
	int fits;

	/* An ASCII byte is a sequence of its own. */
	lead = bytes[0];
	if (lead < 0x80U)
		return 1;

	/* Decides how many bytes the lead byte calls for and the range of the second byte. */
	low = 0x80U;
	high = 0xbfU;
	if (lead >= 0xc2U && lead <= 0xdfU) {
		need = 2;
	} else if (lead == 0xe0U) {
		need = 3;
		low = 0xa0U;
	} else if (lead >= 0xe1U && lead <= 0xecU) {
		need = 3;
	} else if (lead == 0xedU) {
		need = 3;
		high = 0x9fU;
	} else if (lead >= 0xeeU && lead <= 0xefU) {
		need = 3;
	} else if (lead == 0xf0U) {
		need = 4;
		low = 0x90U;
	} else if (lead >= 0xf1U && lead <= 0xf3U) {
		need = 4;
	} else if (lead == 0xf4U) {
		need = 4;
		high = 0x8fU;
	} else {
		/* 80-C1 and F5-FF never start a valid sequence. */
		return 0;
	}

	/* Refuses a sequence that is cut short. */
	if (left < need)
		return 0;

	/* Checks the second byte against its own range. */
	fits = auth_in_range(bytes[1], low, high);
	if (fits == 0)
		return 0;

	/* Checks the remaining bytes as ordinary continuations. */
	for (index = 2;
	     index < need;
	     index++) {
		fits = auth_in_range(bytes[index], 0x80U, 0xbfU);
		if (fits == 0)
			return 0;
	}

	/* The sequence is valid. */
	return need;
}

/*
 * Reports whether byte lies in low to high.
 */
static int
auth_in_range(
	uint8_t byte,
	uint8_t low,
	uint8_t high)
{
	/* Refuses a byte below the range. */
	if (byte < low)
		return 0;

	/* Refuses a byte above the range. */
	if (byte > high)
		return 0;

	/* The byte is in the range. */
	return 1;
}
