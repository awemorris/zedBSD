/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The service request and user authentication messages.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-auth.h"

/*
 * Appends SSH_MSG_SERVICE_REQUEST (RFC 4253 10) for service to out.
 */
void
ssh_auth_put_service_request(
	const char *service,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)service;
	(void)out;
}

/*
 * Appends SSH_MSG_SERVICE_ACCEPT for service to out.
 */
void
ssh_auth_put_service_accept(
	const char *service,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)service;
	(void)out;
}

/*
 * Reads SSH_MSG_SERVICE_REQUEST or SSH_MSG_SERVICE_ACCEPT (the number is
 * the payload's first byte and must be one of them) into service
 * (capacity SSH_NAME_MAX + 1).
 */
int
ssh_auth_parse_service(
	const uint8_t *payload,
	size_t size,
	char *service)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)service;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a USERAUTH_REQUEST payload of size bytes into request.
 */
int
ssh_auth_parse_request(
	const uint8_t *payload,
	size_t size,
	struct ssh_auth_request *request)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)request;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Wipes the password and clears the request.
 */
void
ssh_auth_request_clear(
	struct ssh_auth_request *request)
{
	/* Marks the arguments used until the function is implemented. */
	(void)request;
}

/*
 * Appends the data a USERAUTH_REQUEST signature covers (RFC 4252 7):
 * string session_id (32 bytes), then the first signed_size bytes of the
 * request payload (message number, user, service, "publickey", TRUE,
 * algorithm, key blob).
 */
void
ssh_auth_put_signed_data(
	const uint8_t *session_id,
	const uint8_t *payload,
	size_t signed_size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)session_id;
	(void)payload;
	(void)signed_size;
	(void)out;
}

/*
 * Appends USERAUTH_FAILURE: the name-list methods (NUL-terminated, e.g.
 */
void
ssh_auth_put_failure(
	const char *methods,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)methods;
	(void)out;
}

/*
 * Appends USERAUTH_SUCCESS.
 */
void
ssh_auth_put_success(
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)out;
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
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)out;
}

/*
 * Appends a "none" request for user and the connection service.
 */
void
ssh_auth_put_none(
	const char *user,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)user;
	(void)out;
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
	/* Marks the arguments used until the function is implemented. */
	(void)user;
	(void)public_key;
	(void)out;
}

/*
 * Appends a signed "publickey" request for user: builds the request up
 * to the key blob with has_signature TRUE, signs
 * ssh_auth_put_signed_data of it with key (ssh_key_sign) and appends the
 * signature blob.
 */
int
ssh_auth_put_publickey_signed(
	const char *user,
	const struct ssh_key *key,
	const uint8_t *session_id,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)user;
	(void)key;
	(void)session_id;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)user;
	(void)password;
	(void)out;
}

/*
 * Reads USERAUTH_FAILURE into methods (a name-list, capacity bytes with
 * its NUL) and *partial.
 */
int
ssh_auth_parse_failure(
	const uint8_t *payload,
	size_t size,
	char *methods,
	size_t capacity,
	int *partial)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)methods;
	(void)capacity;
	(void)partial;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)public_key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads USERAUTH_BANNER (RFC 4252 5.4) into text (capacity bytes with
 * the NUL), replacing every control character other than LF and TAB, and
 * every byte that does not belong to valid UTF-8, with '?', so that a
 * server cannot drive the user's terminal.
 */
int
ssh_auth_parse_banner(
	const uint8_t *payload,
	size_t size,
	char *text,
	size_t capacity)
{
	/* Marks the arguments used until the function is implemented. */
	(void)payload;
	(void)size;
	(void)text;
	(void)capacity;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
