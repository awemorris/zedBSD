/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user authentication of the ssh program (RFC 4252): the service
 * request (RFC 4253 10), "none" to learn the methods, each identity file
 * with "publickey" (first asking whether the key would be accepted, then
 * signing), and "password" asked on the terminal.
 *
 * A private key file that others may read is not used (ssh(1) FILES: the
 * files "should be readable by the user but not accessible by others").
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The longest name-list of methods kept from USERAUTH_FAILURE. */
#define CLIENT_METHODS_MAX	1024U

/* The longest password prompt. */
#define CLIENT_PROMPT_MAX	(SSH_USER_MAX + SSH_HOST_MAX + 32U)

/* What one authentication request came to. */
enum client_auth_answer {
	/* The server accepted: authenticated. */
	CLIENT_AUTH_SUCCESS = 0,

	/* The server refused this request; methods holds what may still work. */
	CLIENT_AUTH_FAILURE = 1,

	/* The server would accept the key of a publickey query. */
	CLIENT_AUTH_PK_OK = 2,

	/* The server asks for a new password (not supported). */
	CLIENT_AUTH_CHANGE = 3
};

static int client_auth_wait(struct ssh_client *client, int expect_pk_ok, const uint8_t *public_key, char *methods, enum client_auth_answer *answer);
static int client_auth_send(struct ssh_client *client, struct ssh_buf *out);
static int client_method_offered(const char *methods, const char *method);
static int client_try_keys(struct ssh_client *client, char *methods, int *done);
static int client_try_key(struct ssh_client *client, const struct ssh_key *key, char *methods, int *done);
static int client_load_identity(const struct ssh_client *client, const char *configured, int named, struct ssh_key *key);
static int client_try_passwords(struct ssh_client *client, char *methods, int *done);

/*
 * Runs the user authentication.
 */
int
ssh_client_authenticate(
	struct ssh_client *client)
{
	char service[SSH_NAME_MAX + 1];
	char methods[CLIENT_METHODS_MAX];
	struct ssh_buf out;
	enum client_auth_answer answer;
	uint64_t deadline;
	int compare;
	int done;
	int error;

	/* Asks for the authentication service; the transport sends it after the key exchange. */
	ssh_buf_init(&out, SSH_PAYLOAD_SEND_MAX, 0);
	ssh_auth_put_service_request(SSH_SERVICE_USERAUTH, &out);
	error = client_auth_send(client, &out);
	if (error != 0)
		return error;

	/* Waits for the answer, bounding the wait for the server's identification by ConnectTimeout. */
	deadline = 0;
	if (client->config.connect_timeout != 0)
		deadline = ssh_client_now_ms() + (uint64_t)client->config.connect_timeout * 1000U;
	error = ssh_client_wait(client, deadline);
	if (error != 0)
		return error;

	/* Requires the acceptance of the service asked for (RFC 4253 10). */
	if (client->payload.data[0] != SSH_MSG_SERVICE_ACCEPT)
		return EPROTO;
	error = ssh_auth_parse_service(client->payload.data, client->payload.size, service);
	if (error != 0)
		return EPROTO;
	compare = strcmp(service, SSH_SERVICE_USERAUTH);
	if (compare != 0)
		return EPROTO;
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: authenticating as %s", client->user);

	/* Asks with "none", which shows the methods the server offers (RFC 4252 5.2). */
	ssh_auth_put_none(client->user, &out);
	error = client_auth_send(client, &out);
	if (error != 0)
		return error;
	methods[0] = '\0';
	error = client_auth_wait(client, 0, NULL, methods, &answer);
	if (error != 0)
		return error;
	done = 0;
	if (answer == CLIENT_AUTH_SUCCESS)
		done = 1;

	/* Tries the identity files with publickey. */
	if (!done && client->config.pubkey) {
		error = client_try_keys(client, methods, &done);
		if (error != 0)
			return error;
	}

	/* Tries passwords asked on the terminal. */
	if (!done && client->config.password && !client->config.batch_mode) {
		error = client_try_passwords(client, methods, &done);
		if (error != 0)
			return error;
	}

	/* Gives up when no method worked. */
	if (!done) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "%s@%s: Permission denied (%s).", client->user, client->host_name, methods);
		ssh_client_close(client, SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE, "no more authentication methods");
		return EACCES;
	}

	/* Succeeded: the server accepted the user. */
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: authenticated to %s port %u", client->host_name, (unsigned)client->port);
	return 0;
}

/*
 * Waits for the answer to an authentication request, showing banners
 * that come before it.  expect_pk_ok says that USERAUTH_PK_OK (60) may
 * answer (a publickey query, whose key public_key it must name);
 * otherwise 60 is the password change request.  methods receives the
 * methods of a failure.
 */
static int
client_auth_wait(
	struct ssh_client *client,
	int expect_pk_ok,
	const uint8_t *public_key,
	char *methods,
	enum client_auth_answer *answer)
{
	char banner[SSH_BANNER_MAX + 1];
	int partial;
	int error;

	/* Takes messages until one answers the request. */
	for (;;) {
		error = ssh_client_wait(client, 0);
		if (error != 0)
			return error;

		/* Reads the message by its number. */
		switch (client->payload.data[0]) {
		case SSH_MSG_USERAUTH_BANNER:
			/* Shows the banner (made safe for the terminal) unless the user asked for quiet. */
			error = ssh_auth_parse_banner(client->payload.data, client->payload.size, banner, sizeof(banner));
			if (error != 0)
				return EPROTO;
			if (client->log_level >= SSH_CLIENT_LOG_INFO) {
				fputs(banner, stderr);
				fflush(stderr);
			}
			break;
		case SSH_MSG_USERAUTH_SUCCESS:
			*answer = CLIENT_AUTH_SUCCESS;
			return 0;
		case SSH_MSG_USERAUTH_FAILURE:
			error = ssh_auth_parse_failure(client->payload.data, client->payload.size, methods, CLIENT_METHODS_MAX, &partial);
			if (error != 0)
				return EPROTO;
			*answer = CLIENT_AUTH_FAILURE;
			return 0;
		case SSH_MSG_USERAUTH_PK_OK:
			/* 60 is PK_OK after a query and the change request after a password (RFC 4252 7, 8). */
			if (!expect_pk_ok) {
				*answer = CLIENT_AUTH_CHANGE;
				return 0;
			}
			error = ssh_auth_parse_pk_ok(client->payload.data, client->payload.size, public_key);
			if (error != 0)
				return EPROTO;
			*answer = CLIENT_AUTH_PK_OK;
			return 0;
		default:
			/* Nothing else may come before the authentication ends (RFC 4252 6). */
			return EPROTO;
		}
	}
}

/*
 * Sends the request built in out, wipes out and starts it empty again.
 */
static int
client_auth_send(
	struct ssh_client *client,
	struct ssh_buf *out)
{
	int error;

	/* Sends the request. */
	error = ssh_client_send_buf(client, out);
	ssh_buf_free(out);

	/* Reports a request that could not be sent. */
	if (error != 0)
		return error;

	/* Succeeded: the request is on its way. */
	return 0;
}

/*
 * Reports whether the name-list methods has method.
 */
static int
client_method_offered(
	const char *methods,
	const char *method)
{
	size_t size;
	int offered;

	/* Looks the name up in the list. */
	size = strlen(methods);
	offered = ssh_namelist_contains(methods, size, method);

	/* Reports whether the server offers it. */
	return offered;
}

/*
 * Tries each identity file with publickey while the server offers it.
 */
static int
client_try_keys(
	struct ssh_client *client,
	char *methods,
	int *done)
{
	struct ssh_key key;
	unsigned index;
	int offered;
	int error;

	/* Tries the files in order until one is accepted or publickey is no longer offered. */
	for (index = 0; index < client->config.identity_count; index++) {
		offered = client_method_offered(methods, "publickey");
		if (!offered)
			return 0;

		/* Loads the key; a file that cannot be used is passed over. */
		error = client_load_identity(client, client->config.identity[index], client->identities_named, &key);
		if (error != 0)
			continue;

		/* Offers the key, then signs with it. */
		error = client_try_key(client, &key, methods, done);
		ssh_key_clear(&key);
		if (error != 0)
			return error;
		if (*done)
			return 0;
	}

	/* Succeeded: no key was accepted, and the connection goes on. */
	return 0;
}

/*
 * Asks whether the server would accept a key, and when it would, signs
 * the request with it (RFC 4252 7).
 */
static int
client_try_key(
	struct ssh_client *client,
	const struct ssh_key *key,
	char *methods,
	int *done)
{
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	struct ssh_buf out;
	enum client_auth_answer answer;
	int error;

	/* Asks about the key without a signature. */
	(void)ssh_key_fingerprint(key->public_key, fingerprint, sizeof(fingerprint));
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: offering public key %s", fingerprint);
	ssh_buf_init(&out, SSH_PAYLOAD_SEND_MAX, 0);
	ssh_auth_put_publickey_query(client->user, key->public_key, &out);
	error = client_auth_send(client, &out);
	if (error != 0)
		return error;
	error = client_auth_wait(client, 1, key->public_key, methods, &answer);
	if (error != 0)
		return error;

	/* Stops at a key the server would not take, or one the server takes outright. */
	if (answer == CLIENT_AUTH_SUCCESS) {
		*done = 1;
		return 0;
	}
	if (answer != CLIENT_AUTH_PK_OK)
		return 0;

	/* Signs the request over the session identifier and sends it. */
	error = ssh_auth_put_publickey_signed(client->user, key, client->transport.session_id, &out);
	if (error != 0) {
		ssh_buf_free(&out);
		return error;
	}
	error = client_auth_send(client, &out);
	if (error != 0)
		return error;
	error = client_auth_wait(client, 0, NULL, methods, &answer);
	if (error != 0)
		return error;

	/* Notes whether the signature was accepted. */
	if (answer == CLIENT_AUTH_SUCCESS)
		*done = 1;

	/* Succeeded: *done says whether the user is authenticated. */
	return 0;
}

/*
 * Loads one identity file.  A missing file is passed over silently unless
 * a setting named it (named); other failures are reported.
 */
static int
client_load_identity(
	const struct ssh_client *client,
	const char *configured,
	int named,
	struct ssh_key *key)
{
	char path[SSH_CLIENT_PATH_MAX];
	struct ssh_buf text;
	struct stat information;
	int descriptor;
	int status;
	int error;

	/* Finds the file. */
	error = ssh_client_expand_path(configured, path, sizeof(path));
	if (error != 0)
		return error;

	/* Refuses a file others may read or change. */
	descriptor = open(path, O_RDONLY | O_NOCTTY);
	if (descriptor < 0) {
		error = errno;
		if (error != ENOENT || named)
			ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Warning: Identity file %s not accessible: %s.", path, strerror(error));
		return error;
	}
	status = fstat(descriptor, &information);
	close(descriptor);
	if (status != 0)
		return EIO;
	if ((information.st_mode & 077) != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: permissions %04o of %s are too open: others may read it; the key is not used.", (unsigned)(information.st_mode & 07777), path);
		return EACCES;
	}

	/* Reads the file (secret: it holds the seed). */
	ssh_buf_init(&text, SSH_KEY_FILE_MAX, 1);
	error = ssh_client_read_file(path, &text, SSH_KEY_FILE_MAX);
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: cannot read %s: %s", path, strerror(error));
		ssh_buf_free(&text);
		return error;
	}

	/* Reads the key from the file. */
	error = ssh_key_parse_private((const char *)text.data, text.size, key);
	ssh_buf_free(&text);
	if (error == ENOTSUP) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s: only unencrypted %s keys are supported", path, SSH_KEY_NAME);
		return error;
	}
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s: not a valid private key file", path);
		return error;
	}

	/* Succeeded: key holds the private key. */
	return 0;
}

/*
 * Asks for the password on the terminal and tries it, up to
 * SSH_CLIENT_PASSWORD_TRIES times while the server offers password.
 */
static int
client_try_passwords(
	struct ssh_client *client,
	char *methods,
	int *done)
{
	char password[SSH_PASSWORD_MAX + 1];
	char prompt[CLIENT_PROMPT_MAX];
	struct ssh_buf out;
	enum client_auth_answer answer;
	unsigned tries;
	int offered;
	int error;

	/* Asks with the user and the host, as the user expects to see them. */
	sprintf(prompt, "%s@%s's password: ", client->user, client->host_name);

	/* Tries while tries are left and the server still offers password. */
	for (tries = 0; tries < SSH_CLIENT_PASSWORD_TRIES; tries++) {
		offered = client_method_offered(methods, "password");
		if (!offered)
			return 0;

		/* Asks; without a terminal nobody can answer, which ends the tries. */
		error = ssh_client_ask(prompt, 0, password, sizeof(password));
		if (error != 0) {
			ssh_wipe(password, sizeof(password));
			return 0;
		}

		/* Sends the password in a secret buffer, and forgets it. */
		ssh_buf_init(&out, SSH_PAYLOAD_SEND_MAX, 1);
		ssh_auth_put_password(client->user, password, &out);
		ssh_wipe(password, sizeof(password));
		error = client_auth_send(client, &out);
		if (error != 0)
			return error;
		error = client_auth_wait(client, 0, NULL, methods, &answer);
		if (error != 0)
			return error;

		/* Ends on success. */
		if (answer == CLIENT_AUTH_SUCCESS) {
			*done = 1;
			return 0;
		}

		/* A server that wants the password changed is not served: that ends the tries. */
		if (answer == CLIENT_AUTH_CHANGE) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: the server asks for a new password, which is not supported");
			return 0;
		}

		ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Permission denied, please try again.");
	}

	/* Succeeded: the tries are used up, and the connection goes on to give up. */
	return 0;
}
