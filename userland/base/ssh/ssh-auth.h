/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages of the service request (RFC 4253 10) and of user
 * authentication (RFC 4252): building and reading them, and the data a
 * public key signature covers.  The order of the exchange and the policy
 * (which methods are offered, how many attempts, what a password is
 * checked against) belong to the programs.
 *
 * Methods: "publickey" with ssh-ed25519 (RFC 4252 7, RFC 8709) and
 * "password" (RFC 4252 8, without the password change of 8's second
 * form, which is refused), and "none" (5.2, which the client sends first
 * to learn the methods and the server always fails).  Every other method
 * is read as SSH_AUTH_OTHER and failed.
 *
 * Passwords are secrets: a structure holding one is wiped with
 * ssh_auth_request_clear, and a buffer a password was put into should be
 * secret.
 */

#ifndef SSH_SSH_AUTH_H
#define SSH_SSH_AUTH_H

#include "ssh-key.h"

/* The message numbers (RFC 4250 4.1.2). */
#define SSH_MSG_USERAUTH_REQUEST	50
#define SSH_MSG_USERAUTH_FAILURE	51
#define SSH_MSG_USERAUTH_SUCCESS	52
#define SSH_MSG_USERAUTH_BANNER		53
#define SSH_MSG_USERAUTH_PK_OK		60
#define SSH_MSG_USERAUTH_PASSWD_CHANGEREQ	60

/* The service names (RFC 4250 4.7). */
#define SSH_SERVICE_USERAUTH	"ssh-userauth"
#define SSH_SERVICE_CONNECTION	"ssh-connection"

/* The longest password accepted, in bytes (longer is refused, not cut). */
#define SSH_PASSWORD_MAX	256U

/* The longest banner text shown, in bytes. */
#define SSH_BANNER_MAX		4096U

/* The methods. */
enum ssh_auth_method {
	SSH_AUTH_NONE = 0,
	SSH_AUTH_PUBLICKEY = 1,
	SSH_AUTH_PASSWORD = 2,
	SSH_AUTH_OTHER = 3
};

/*
 * One USERAUTH_REQUEST as the server read it.
 *
 * user and service are NUL-terminated.  For SSH_AUTH_PUBLICKEY: algorithm
 * the named algorithm, supported 1 when it is ssh-ed25519 and the blob
 * parses (public_key then holds the key; supported 0 means "fail this
 * request"), has_signature the boolean of RFC 4252 7, signature and
 * signature_size the signature blob inside the payload, signed_size the
 * number of payload bytes the signature covers (everything before the
 * signature string).  For SSH_AUTH_PASSWORD: password (secret), change 1
 * when the request was the password change form.  Pointers point into the
 * payload the caller keeps.
 */
struct ssh_auth_request {
	char user[SSH_USER_MAX + 1];
	char service[SSH_NAME_MAX + 1];
	enum ssh_auth_method method;
	char algorithm[SSH_NAME_MAX + 1];
	int supported;
	uint8_t public_key[SSH_ED25519_PUBLIC];
	const uint8_t *key_blob;
	size_t key_blob_size;
	int has_signature;
	const uint8_t *signature;
	size_t signature_size;
	size_t signed_size;
	char password[SSH_PASSWORD_MAX + 1];
	int change;
};

/* Appends SSH_MSG_SERVICE_REQUEST (RFC 4253 10) for service to out. */
void ssh_auth_put_service_request(const char *service, struct ssh_buf *out);

/* Appends SSH_MSG_SERVICE_ACCEPT for service to out. */
void ssh_auth_put_service_accept(const char *service, struct ssh_buf *out);

/*
 * Reads SSH_MSG_SERVICE_REQUEST or SSH_MSG_SERVICE_ACCEPT (the number is
 * the payload's first byte and must be one of them) into service
 * (capacity SSH_NAME_MAX + 1).  Returns 0, EINVAL or EMSGSIZE.
 */
int ssh_auth_parse_service(const uint8_t *payload, size_t size, char *service);

/*
 * Reads a USERAUTH_REQUEST payload of size bytes into request.
 *
 * A malformed request (a field that runs past the end, trailing bytes, a
 * NUL inside a name, a password that is longer than SSH_PASSWORD_MAX) is
 * EINVAL; an unknown method is not an error (SSH_AUTH_OTHER, the rest of
 * the payload not read).  A publickey request with another algorithm, or
 * whose blob is not a parsable ssh-ed25519 key, or whose blob's type
 * differs from algorithm, is read with supported 0.  Returns 0 or EINVAL;
 * on an error request is cleared.
 */
int ssh_auth_parse_request(const uint8_t *payload, size_t size, struct ssh_auth_request *request);

/* Wipes the password and clears the request. */
void ssh_auth_request_clear(struct ssh_auth_request *request);

/*
 * Appends the data a USERAUTH_REQUEST signature covers (RFC 4252 7):
 * string session_id (32 bytes), then the first signed_size bytes of the
 * request payload (message number, user, service, "publickey", TRUE,
 * algorithm, key blob).  The server calls it with the received payload;
 * the client builds the same bytes before signing.
 */
void ssh_auth_put_signed_data(const uint8_t *session_id, const uint8_t *payload, size_t signed_size, struct ssh_buf *out);

/*
 * Appends USERAUTH_FAILURE: the name-list methods (NUL-terminated, e.g.
 * "publickey,password") and the partial success boolean (always FALSE
 * here: no method needs another).
 */
void ssh_auth_put_failure(const char *methods, struct ssh_buf *out);

/* Appends USERAUTH_SUCCESS. */
void ssh_auth_put_success(struct ssh_buf *out);

/* Appends USERAUTH_PK_OK (RFC 4252 7): string algorithm, string the key blob of public_key. */
void ssh_auth_put_pk_ok(const uint8_t *public_key, struct ssh_buf *out);

/* Appends a "none" request for user and the connection service. */
void ssh_auth_put_none(const char *user, struct ssh_buf *out);

/*
 * Appends a "publickey" request for user without a signature (the query
 * of RFC 4252 7 that asks whether the key would be accepted).
 */
void ssh_auth_put_publickey_query(const char *user, const uint8_t *public_key, struct ssh_buf *out);

/*
 * Appends a signed "publickey" request for user: builds the request up to
 * the key blob with has_signature TRUE, signs ssh_auth_put_signed_data of
 * it with key (ssh_key_sign) and appends the signature blob.
 *
 * key is private.  Returns 0, EINVAL, EIO or the error recorded in out.
 */
int ssh_auth_put_publickey_signed(const char *user, const struct ssh_key *key, const uint8_t *session_id, struct ssh_buf *out);

/* Appends a "password" request for user (FALSE, then the password; out should be secret). */
void ssh_auth_put_password(const char *user, const char *password, struct ssh_buf *out);

/*
 * Reads USERAUTH_FAILURE into methods (a name-list, capacity bytes with
 * its NUL) and *partial.  Returns 0, EINVAL or EMSGSIZE.
 */
int ssh_auth_parse_failure(const uint8_t *payload, size_t size, char *methods, size_t capacity, int *partial);

/*
 * Reads USERAUTH_PK_OK and checks that it names ssh-ed25519 and
 * public_key (EPROTO when it names another key).  Returns 0, EINVAL or
 * EPROTO.
 */
int ssh_auth_parse_pk_ok(const uint8_t *payload, size_t size, const uint8_t *public_key);

/*
 * Reads USERAUTH_BANNER (RFC 4252 5.4) into text (capacity bytes with the
 * NUL), replacing every control character other than LF and TAB, and
 * every byte that does not belong to valid UTF-8, with '?', so that a
 * server cannot drive the user's terminal.  A longer banner is cut.
 * Returns 0 or EINVAL.
 */
int ssh_auth_parse_banner(const uint8_t *payload, size_t size, char *text, size_t capacity);

#endif
