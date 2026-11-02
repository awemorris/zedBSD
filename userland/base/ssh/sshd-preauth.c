/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pre-authentication process of sshd: transport and user
 * authentication without privileges.
 *
 * It takes its settings from the monitor, uses each crypto operation
 * once while the file system is still there, then (as root) changes its
 * root to SSHD_EMPTY_DIRECTORY and becomes SSHD_PRIVSEP_USER.  It speaks
 * the transport and the service request and user authentication (RFC
 * 4253 10, RFC 4252) with the client; every decision on a login is the
 * monitor's.  The attempts are counted as the monitor counts them (a
 * login of either method, and a refused query, count one; granted
 * queries have their own bound of MaxAuthTries), and also every request
 * of a method that is not asked (another method, a key of another type,
 * a password change), so this side reaches the limit no later than the
 * monitor does and ends the connection itself with
 * SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE before asking once more.
 * A client that changes the user or the service between requests is
 * disconnected (RFC 4252 5 lets a server do so).  After a login is
 * granted it sends USERAUTH_SUCCESS, writes all its output, hands the
 * transport to the monitor (AUTH_DONE) and exits.
 */

#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "ssh-auth.h"
#include "sshd-child.h"

/* The most bytes one read of the connection takes. */
#define PREAUTH_READ_CHUNK	16384U

/* The longest list of methods offered: "publickey,password" and its NUL. */
#define PREAUTH_METHODS_MAX	32U

/* The longest a poll waits before the transport's clock is looked at again, in milliseconds. */
#define PREAUTH_POLL_MS		10000

/* The bytes the warm-up runs the stream cipher and the authenticator over. */
#define PREAUTH_WARM_SIZE	64U

/* What a request to the monitor was answered (or that it was not asked). */
#define PREAUTH_REFUSED		0
#define PREAUTH_GRANTED		1

/*
 * The state of the user authentication of the connection.
 *
 * child holds the settings from CONFIG.  service_accepted is 1 after
 * ssh-userauth was accepted.  user and service are those of the first
 * USERAUTH_REQUEST (have_user).  tries and granted_queries count as the
 * header comment says.  methods is the name-list offered in every
 * USERAUTH_FAILURE.
 */
struct preauth {
	struct sshd_child child;
	struct ssh_transport transport;
	int service_accepted;
	char user[SSH_USER_MAX + 1];
	char service[SSH_NAME_MAX + 1];
	int have_user;
	unsigned tries;
	unsigned granted_queries;
	char methods[PREAUTH_METHODS_MAX];
};

static int preauth_warm_up(void);
static int preauth_confine(void);
static int preauth_serve(struct preauth *preauth);
static int preauth_message(struct preauth *preauth, const struct ssh_buf *payload, int *granted);
static int preauth_service(struct preauth *preauth, const struct ssh_buf *payload);
static int preauth_userauth(struct preauth *preauth, const struct ssh_buf *payload, int *granted);
static int preauth_publickey(struct preauth *preauth, const struct ssh_auth_request *request, const struct ssh_buf *payload, int *granted);
static int preauth_password(struct preauth *preauth, const struct ssh_auth_request *request, int *granted);
static int preauth_counted(struct preauth *preauth, int query);
static int preauth_fail(struct preauth *preauth);
static int preauth_reply(struct preauth *preauth, const struct ssh_buf *message);
static int preauth_hand_over(struct preauth *preauth);
static void preauth_exit(struct preauth *preauth, int status);

/*
 * Runs the pre-authentication process (sshd started with
 * SSHD_EXEC_PREAUTH): reads the settings from the monitor, warms the
 * crypto up, confines itself, then runs the transport and the user
 * authentication.
 */
void
sshd_preauth_run(void)
{
	static struct preauth preauth;
	struct ssh_buf input;
	struct ssh_transport_ops ops;
	int status;

	/* Starts as a child of the monitor and takes the settings. */
	sshd_child_start();
	memset(&preauth, 0, sizeof(preauth));
	ssh_buf_init(&input, 4U + (size_t)SSH_MON_STATE_MAX, 1);
	status = sshd_child_read_config(&input, &preauth.child);
	ssh_buf_free(&input);
	if (status != 0) {
		syslog(LOG_ERR, "pre-authentication: no settings from the monitor");
		_exit(1);
	}

	/* The methods offered, in the order a client tries them. */
	preauth.methods[0] = '\0';
	if (preauth.child.config.pubkey)
		strcat(preauth.methods, "publickey");
	if (preauth.child.config.password) {
		if (preauth.methods[0] != '\0')
			strcat(preauth.methods, ",");
		strcat(preauth.methods, "password");
	}

	/* Uses every crypto operation once, then gives up the file system and root. */
	status = preauth_warm_up();
	if (status != 0) {
		syslog(LOG_ERR, "pre-authentication: the crypto library failed");
		_exit(1);
	}
	if (preauth.child.privileged) {
		status = preauth_confine();
		if (status != 0) {
			syslog(LOG_ERR, "pre-authentication: cannot confine the process");
			_exit(1);
		}
	}

	/* Starts the server's side of the transport, signing through the monitor. */
	memset(&ops, 0, sizeof(ops));
	ops.host_key = sshd_child_host_key;
	ops.sign = sshd_child_sign;
	ops.context = &preauth.child;
	status = ssh_transport_init(&preauth.transport, SSH_ROLE_SERVER, &ops, sshd_child_now());
	if (status != 0)
		_exit(1);

	/* Runs the connection until a login is granted, then hands it over. */
	status = preauth_serve(&preauth);
	if (status != 0)
		preauth_exit(&preauth, 1);
	status = preauth_hand_over(&preauth);
	if (status != 0)
		preauth_exit(&preauth, 1);
	preauth_exit(&preauth, 0);
}

/*
 * Uses each crypto operation the connection will need once, so that the
 * library has loaded what it loads from the file system before the root
 * is changed: random bytes, SHA-256, the hybrid exchange on both sides
 * (ML-KEM-768 and X25519), an Ed25519 verification, ChaCha20 and
 * Poly1305.  Returns 0 or EIO.
 */
static int
preauth_warm_up(void)
{
	struct ssh_hybrid_client client;
	uint8_t c_init[SSH_HYBRID_INIT_SIZE];
	uint8_t s_reply[SSH_HYBRID_REPLY_SIZE];
	uint8_t k_server[SSH_KEX_HASH_SIZE];
	uint8_t k_client[SSH_KEX_HASH_SIZE];
	uint8_t key[SSH_CHACHA20_KEY];
	uint8_t nonce[SSH_CHACHA20_NONCE];
	uint8_t block[PREAUTH_WARM_SIZE];
	uint8_t digest[SSH_SHA256_SIZE];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	uint8_t signature[SSH_ED25519_SIGNATURE];
	uint8_t tag[SSH_POLY1305_TAG];
	int status;

	/* Random bytes, a hash, and the keys of one Ed25519 key pair made from them. */
	memset(&client, 0, sizeof(client));
	status = ssh_random(key, sizeof(key));
	if (status == 0)
		status = ssh_sha256(key, sizeof(key), digest);
	if (status == 0)
		status = ssh_ed25519_public(digest, public_key);
	if (status == 0)
		status = ssh_ed25519_sign(digest, key, sizeof(key), signature);
	if (status == 0)
		status = ssh_ed25519_verify(public_key, key, sizeof(key), signature);

	/* Both sides of one hybrid exchange. */
	if (status == 0)
		status = ssh_hybrid_client_start(&client, c_init);
	if (status == 0)
		status = ssh_hybrid_server(c_init, sizeof(c_init), s_reply, k_server);
	if (status == 0)
		status = ssh_hybrid_client_finish(&client, s_reply, sizeof(s_reply), k_client);
	ssh_hybrid_client_clear(&client);

	/* The stream cipher and the authenticator. */
	memset(nonce, 0, sizeof(nonce));
	memset(block, 0, sizeof(block));
	if (status == 0)
		status = ssh_chacha20_xor(key, 0, nonce, block, block, sizeof(block));
	if (status == 0)
		status = ssh_poly1305(key, block, sizeof(block), tag);

	/* Wipes what was made. */
	ssh_wipe(key, sizeof(key));
	ssh_wipe(digest, sizeof(digest));
	ssh_wipe(k_server, sizeof(k_server));
	ssh_wipe(k_client, sizeof(k_client));
	ssh_wipe(block, sizeof(block));
	if (status != 0)
		return EIO;

	/* Succeeded: every operation ran once. */
	return 0;
}

/*
 * Changes the root to SSHD_EMPTY_DIRECTORY and becomes SSHD_PRIVSEP_USER
 * with only its own group (as root).  Returns 0 or EPERM.
 */
static int
preauth_confine(void)
{
	struct passwd account;
	struct passwd *found;
	char buffer[1024];
	gid_t group;
	uid_t user;
	uid_t real_user;
	uid_t effective_user;
	int changed;

	/* Finds the user to become before the passwd file is out of reach. */
	found = NULL;
	changed = getpwnam_r(SSHD_PRIVSEP_USER, &account, buffer, sizeof(buffer), &found);
	if (changed != 0 || found == NULL)
		return EPERM;
	group = account.pw_gid;
	user = account.pw_uid;

	/* Changes the root to the empty directory. */
	changed = chroot(SSHD_EMPTY_DIRECTORY);
	if (changed != 0)
		return EPERM;
	changed = chdir("/");
	if (changed != 0)
		return EPERM;

	/* Takes the user's one group, then the group and the user (as root these set real, effective and saved alike). */
	changed = setgroups(1, &group);
	if (changed != 0)
		return EPERM;
	changed = setgid(group);
	if (changed != 0)
		return EPERM;
	changed = setuid(user);
	if (changed != 0)
		return EPERM;

	/* Checks that root is gone for good. */
	real_user = getuid();
	effective_user = geteuid();
	if (real_user != user || effective_user != user)
		return EPERM;
	changed = setuid(0);
	if (changed == 0)
		return EPERM;

	/* Succeeded: the process is confined. */
	return 0;
}

/*
 * Runs the connection until a login is granted (0) or the connection
 * ends (1).  The pending output is written each round; the transport's
 * clock is ticked so that a long login re-keys.
 */
static int
preauth_serve(
	struct preauth *preauth)
{
	struct ssh_buf payload;
	struct pollfd waiting;
	uint8_t chunk[PREAUTH_READ_CHUNK];
	const uint8_t *pending;
	size_t size;
	ssize_t got;
	int granted;
	int status;
	int ready;

	/* Every payload may hold a password: the buffer is secret. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);
	granted = 0;
	status = 0;

	/* Takes the messages, writes the output, waits for the client, until a grant or the end. */
	for (;;) {
		/* Handles every message the input holds. */
		for (;;) {
			status = ssh_transport_next(&preauth->transport, &payload);
			if (status == EAGAIN) {
				status = 0;
				break;
			}
			if (status != 0)
				break;
			status = preauth_message(preauth, &payload, &granted);
			ssh_buf_reset(&payload);
			if (status != 0 || granted)
				break;
		}
		if (status != 0 || granted)
			break;

		/* Writes what is pending, as much as the connection takes now. */
		pending = ssh_transport_pending(&preauth->transport, &size);
		while (size > 0) {
			got = write(SSHD_FD_CLIENT, pending, size);
			if (got <= 0)
				break;
			ssh_transport_written(&preauth->transport, (size_t)got);
			pending = ssh_transport_pending(&preauth->transport, &size);
		}
		if (preauth->transport.state == SSH_TRANSPORT_CLOSED) {
			status = EPIPE;
			break;
		}

		/* Waits for the client, or for room to write. */
		waiting.fd = SSHD_FD_CLIENT;
		waiting.events = POLLIN;
		if (size > 0)
			waiting.events |= POLLOUT;
		waiting.revents = 0;
		ready = poll(&waiting, 1, PREAUTH_POLL_MS);
		if (ready < 0 && errno != EINTR) {
			status = EIO;
			break;
		}

		/* Takes what came; an end of file is the client gone. */
		if (ready > 0 && (waiting.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			got = read(SSHD_FD_CLIENT, chunk, sizeof(chunk));
			if (got == 0) {
				status = EPIPE;
				break;
			}
			if (got > 0) {
				status = ssh_transport_input(&preauth->transport, chunk, (size_t)got);
				if (status != 0)
					break;
			} else if (errno != EINTR &&
			    errno != EAGAIN &&
			    errno != EWOULDBLOCK) {
				status = EIO;
				break;
			}
		}

		/* Lets the transport re-key a login that takes long. */
		status = ssh_transport_tick(&preauth->transport, sshd_child_now());
		if (status != 0)
			break;
	}

	/* Wipes what was read. */
	ssh_wipe(chunk, sizeof(chunk));
	ssh_buf_free(&payload);

	/* Reports a connection that ended without a login. */
	if (status != 0 || !granted)
		return 1;

	/* Succeeded: a login was granted and USERAUTH_SUCCESS is queued. */
	return 0;
}

/*
 * Handles one upper-layer message before the login: the service request,
 * then user authentication requests.  Anything else from a client that
 * has not logged in ends the connection.  Returns 0 or an error (the
 * connection is then closed with a DISCONNECT queued).
 */
static int
preauth_message(
	struct preauth *preauth,
	const struct ssh_buf *payload,
	int *granted)
{
	int status;

	/* The first byte says which message it is. */
	if (payload->size == 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "empty message");
		return EPROTO;
	}

	/* Takes the service request first, then the authentication requests. */
	if (payload->data[0] == SSH_MSG_SERVICE_REQUEST && !preauth->service_accepted) {
		status = preauth_service(preauth, payload);
		return status;
	}
	if (payload->data[0] == SSH_MSG_USERAUTH_REQUEST && preauth->service_accepted) {
		status = preauth_userauth(preauth, payload, granted);
		return status;
	}

	/* Anything else before the login is refused with the connection. */
	ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected message before authentication");
	return EPROTO;
}

/*
 * Accepts the service request for ssh-userauth (RFC 4253 10); another
 * service ends the connection with SSH_DISCONNECT_SERVICE_NOT_AVAILABLE.
 */
static int
preauth_service(
	struct preauth *preauth,
	const struct ssh_buf *payload)
{
	struct ssh_buf reply;
	char service[SSH_NAME_MAX + 1];
	int parsed;
	int same;
	int sent;

	/* Reads the service asked for. */
	parsed = ssh_auth_parse_service(payload->data, payload->size, service);
	if (parsed != 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "bad service request");
		return EPROTO;
	}
	same = strcmp(service, SSH_SERVICE_USERAUTH);
	if (same != 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "service not available");
		return EPROTO;
	}

	/* Accepts it. */
	ssh_buf_init(&reply, 64, 0);
	ssh_auth_put_service_accept(SSH_SERVICE_USERAUTH, &reply);
	sent = preauth_reply(preauth, &reply);
	ssh_buf_free(&reply);
	if (sent != 0)
		return sent;
	preauth->service_accepted = 1;

	/* Succeeded: authentication requests may follow. */
	return 0;
}

/*
 * Handles one USERAUTH_REQUEST: keeps the first user and service and
 * disconnects a client that changes either; fails "none" and every
 * method not offered; asks the monitor about a key or a password.
 */
static int
preauth_userauth(
	struct preauth *preauth,
	const struct ssh_buf *payload,
	int *granted)
{
	struct ssh_auth_request request;
	int parsed;
	int same;
	int status;

	/* Reads the request; a malformed one ends the connection. */
	parsed = ssh_auth_parse_request(payload->data, payload->size, &request);
	if (parsed != 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "bad authentication request");
		return EPROTO;
	}

	/* Keeps the first user and service, which may not change, and offers only the connection service. */
	if (!preauth->have_user) {
		same = strcmp(request.service, SSH_SERVICE_CONNECTION);
		if (same != 0) {
			ssh_auth_request_clear(&request);
			ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "service not available");
			return EPROTO;
		}
		strcpy(preauth->user, request.user);
		strcpy(preauth->service, request.service);
		preauth->have_user = 1;
	}
	same = strcmp(preauth->user, request.user);
	if (same == 0)
		same = strcmp(preauth->service, request.service);
	if (same != 0) {
		ssh_auth_request_clear(&request);
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "change of user or service not allowed");
		return EPROTO;
	}

	/* Answers the request by its method. */
	status = 0;
	if (request.method == SSH_AUTH_NONE) {
		/* "none" only learns the methods (RFC 4252 5.2); it is not an attempt. */
		status = preauth_fail(preauth);
	} else if (request.method == SSH_AUTH_PUBLICKEY && preauth->child.config.pubkey) {
		status = preauth_publickey(preauth, &request, payload, granted);
	} else if (request.method == SSH_AUTH_PASSWORD && preauth->child.config.password) {
		status = preauth_password(preauth, &request, granted);
	} else {
		/* Another method, or one that is off: counted, and failed. */
		status = preauth_counted(preauth, 0);
		if (status == 0) {
			preauth->tries++;
			status = preauth_fail(preauth);
		}
	}
	ssh_auth_request_clear(&request);

	/* Reports a failure that ended the connection. */
	if (status != 0)
		return status;

	/* Succeeded: the request is answered (or the login granted). */
	return 0;
}

/*
 * Handles a publickey request: a query asks the monitor whether the key
 * is authorized (PK_OK or FAILURE); a signed one hands the monitor the
 * data the signature covers, built from this connection's session
 * identifier and the request itself.
 */
static int
preauth_publickey(
	struct preauth *preauth,
	const struct ssh_auth_request *request,
	const struct ssh_buf *payload,
	int *granted)
{
	struct ssh_buf body;
	struct ssh_buf signed_data;
	struct ssh_buf reply;
	uint32_t result;
	int status;
	int asked;

	/* A key of another type, or a blob that does not parse, is failed and counted. */
	if (!request->supported) {
		status = preauth_counted(preauth, 0);
		if (status != 0)
			return status;
		preauth->tries++;
		status = preauth_fail(preauth);
		return status;
	}

	/* Counts the request, ending the connection at the limit. */
	status = preauth_counted(preauth, !request->has_signature);
	if (status != 0)
		return status;

	/* Builds the request to the monitor. */
	ssh_buf_init(&body, SSH_MON_FRAME_MAX, 0);
	ssh_buf_init(&signed_data, SSH_PACKET_LIMIT + 64U, 0);
	ssh_buf_put_cstring(&body, request->user);
	ssh_buf_put_string(&body, request->key_blob, request->key_blob_size);
	if (request->has_signature) {
		ssh_auth_put_signed_data(preauth->transport.session_id, payload->data, request->signed_size, &signed_data);
		ssh_buf_put_string(&body, signed_data.data, signed_data.size);
		ssh_buf_put_string(&body, request->signature, request->signature_size);
	}
	status = ssh_buf_error(&signed_data);
	if (status == 0)
		status = ssh_buf_error(&body);
	ssh_buf_free(&signed_data);
	if (status != 0) {
		/* A request too large to pass on is a failed attempt. */
		ssh_buf_free(&body);
		preauth->tries++;
		status = preauth_fail(preauth);
		return status;
	}

	/* Asks the monitor: a query, or a login. */
	result = EACCES;
	if (request->has_signature)
		asked = sshd_child_ask(SSH_MON_PUBKEY_LOGIN, body.data, body.size, &result);
	else
		asked = sshd_child_ask(SSH_MON_PUBKEY_QUERY, body.data, body.size, &result);
	ssh_buf_free(&body);
	if (asked != 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_BY_APPLICATION, "internal error");
		return EIO;
	}

	/* A granted login ends the authentication. */
	if (request->has_signature && result == 0) {
		*granted = 1;
		ssh_buf_init(&reply, 16, 0);
		ssh_auth_put_success(&reply);
		status = preauth_reply(preauth, &reply);
		ssh_buf_free(&reply);
		return status;
	}

	/* A key the monitor would accept is answered with PK_OK; it counts against the queries' own bound. */
	if (!request->has_signature && result == 0) {
		preauth->granted_queries++;
		ssh_buf_init(&reply, 128, 0);
		ssh_auth_put_pk_ok(request->public_key, &reply);
		status = preauth_reply(preauth, &reply);
		ssh_buf_free(&reply);
		return status;
	}

	/* Anything else failed and was an attempt. */
	preauth->tries++;
	status = preauth_fail(preauth);
	if (status != 0)
		return status;

	/* Succeeded: the request is answered. */
	return 0;
}

/*
 * Handles a password request: the change form is failed (and counted);
 * otherwise the monitor checks the password.
 */
static int
preauth_password(
	struct preauth *preauth,
	const struct ssh_auth_request *request,
	int *granted)
{
	struct ssh_buf body;
	struct ssh_buf reply;
	uint32_t result;
	int status;
	int asked;

	/* Counts the request, ending the connection at the limit. */
	status = preauth_counted(preauth, 0);
	if (status != 0)
		return status;

	/* A password change is not offered: a failed attempt. */
	if (request->change) {
		preauth->tries++;
		status = preauth_fail(preauth);
		return status;
	}

	/* Asks the monitor with the password, in a buffer that is wiped. */
	ssh_buf_init(&body, SSH_MON_FRAME_MAX, 1);
	ssh_buf_put_cstring(&body, request->user);
	ssh_buf_put_cstring(&body, request->password);
	status = ssh_buf_error(&body);
	result = EACCES;
	asked = EIO;
	if (status == 0)
		asked = sshd_child_ask(SSH_MON_PASSWORD_LOGIN, body.data, body.size, &result);
	ssh_buf_free(&body);
	if (asked != 0) {
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_BY_APPLICATION, "internal error");
		return EIO;
	}
	preauth->tries++;

	/* A granted login ends the authentication. */
	if (result == 0) {
		*granted = 1;
		ssh_buf_init(&reply, 16, 0);
		ssh_auth_put_success(&reply);
		status = preauth_reply(preauth, &reply);
		ssh_buf_free(&reply);
		return status;
	}

	/* A refusal is failed. */
	status = preauth_fail(preauth);
	if (status != 0)
		return status;

	/* Succeeded: the request is answered. */
	return 0;
}

/*
 * Checks that one more attempt may be made (a query when query is 1):
 * when MaxAuthTries attempts were made (or, for a query, that many
 * queries were granted), the connection ends with
 * SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE before the monitor is
 * asked again.  A request that is not passed to the monitor counts here
 * at once.  Returns 0 or EACCES (the connection is closed).
 */
static int
preauth_counted(
	struct preauth *preauth,
	int query)
{
	/* Ends the connection at the limit. */
	if (preauth->tries >= preauth->child.config.max_auth_tries ||
	    (query && preauth->granted_queries >= preauth->child.config.max_auth_tries)) {
		syslog(LOG_NOTICE, "too many authentication failures for %.64s from %.63s", preauth->user, preauth->child.peer);
		ssh_transport_disconnect(&preauth->transport, SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE, "Too many authentication failures");
		return EACCES;
	}

	/* Succeeded: the attempt may be made. */
	return 0;
}

/* Answers USERAUTH_FAILURE with the methods offered, partial success FALSE. */
static int
preauth_fail(
	struct preauth *preauth)
{
	struct ssh_buf reply;
	int status;

	/* The failure. */
	ssh_buf_init(&reply, 128, 0);
	ssh_auth_put_failure(preauth->methods, &reply);
	status = preauth_reply(preauth, &reply);
	ssh_buf_free(&reply);
	if (status != 0)
		return status;

	/* Succeeded: the failure is queued. */
	return 0;
}

/* Sends one message built in message; 0 or the transport's error. */
static int
preauth_reply(
	struct preauth *preauth,
	const struct ssh_buf *message)
{
	int status;

	/* Refuses a message that was not built. */
	status = ssh_buf_error(message);
	if (status != 0)
		return status;

	/* Queues it. */
	status = ssh_transport_send(&preauth->transport, message->data, message->size);
	if (status != 0)
		return status;

	/* Succeeded: the message is queued. */
	return 0;
}

/*
 * Hands the connection to the monitor: writes all pending output (the
 * USERAUTH_SUCCESS among it), exports the transport and sends it in
 * AUTH_DONE.  A re-exchange the client started in the meantime is
 * carried on until it ends, as long as no upper-layer message arrives
 * (one would be lost).  Returns 0 or an error.
 */
static int
preauth_hand_over(
	struct preauth *preauth)
{
	struct ssh_buf state;
	struct ssh_buf body;
	struct ssh_buf payload;
	struct pollfd waiting;
	uint8_t chunk[PREAUTH_READ_CHUNK];
	ssize_t got;
	int status;
	int ready;

	/* The exported transport carries the keys: every buffer is secret. */
	ssh_buf_init(&state, SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);

	/* Writes the output, then exports; an exchange in progress is carried on first. */
	for (;;) {
		status = sshd_child_flush(&preauth->transport);
		if (status != 0)
			break;
		ssh_buf_reset(&state);
		status = ssh_transport_export(&preauth->transport, &state);
		if (status != EBUSY)
			break;

		/* Takes the next part of the exchange from the client. */
		status = ssh_transport_next(&preauth->transport, &payload);
		if (status == 0) {
			/* An upper-layer message now would be lost: the connection ends. */
			status = EPROTO;
			break;
		}
		if (status != EAGAIN)
			break;
		waiting.fd = SSHD_FD_CLIENT;
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, PREAUTH_POLL_MS);
		if (ready < 0 && errno != EINTR) {
			status = EIO;
			break;
		}
		if (ready <= 0)
			continue;
		got = read(SSHD_FD_CLIENT, chunk, sizeof(chunk));
		if (got == 0) {
			status = EPIPE;
			break;
		}
		if (got > 0) {
			status = ssh_transport_input(&preauth->transport, chunk, (size_t)got);
			if (status != 0)
				break;
		}
	}
	ssh_wipe(chunk, sizeof(chunk));
	ssh_buf_free(&payload);

	/* Sends the transport to the monitor. */
	if (status == 0) {
		ssh_buf_put_string(&body, state.data, state.size);
		status = ssh_buf_error(&body);
	}
	if (status == 0)
		status = sshd_child_send(SSH_MON_AUTH_DONE, body.data, body.size);
	ssh_buf_free(&state);
	ssh_buf_free(&body);
	if (status != 0)
		return status;

	/* Succeeded: the monitor has the connection. */
	return 0;
}

/*
 * Ends the process: writes what is pending (a DISCONNECT among it, when
 * the connection failed), wipes the transport and exits with status.
 */
static void
preauth_exit(
	struct preauth *preauth,
	int status)
{
	/* The last output, then nothing of the keys is left. */
	if (status != 0)
		(void)sshd_child_flush(&preauth->transport);
	ssh_transport_free(&preauth->transport);
	_exit(status);
}
