/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sharing on zedBSD (ws089-p025): the Remote Login (sshd) through
 * sessiond's SERVICE request on the session's descriptor (sessiond's
 * service.c: root or a member of wheel, sshd only), answered by
 *
 *   SERVICE available=0|1 enabled=0|1 running=0|1 port=N
 *
 * or DENIED or ERROR (session-zedbsd.c hands the line here).  Whether this
 * user may change it is read from the compositor's own groups (wheel, as
 * sudo's rule); the host key's fingerprint is the SHA-256 of the public
 * key's blob in base64 without its padding, as ssh-keygen -l shows it,
 * read from the world-readable public key files.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include "userland/base/common/sha256.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The folder of the host keys (a build may name another, as the host tests do). */
#ifndef SHARING_KEY_DIR
#define SHARING_KEY_DIR "/etc/ssh"
#endif

/* The public host keys looked at, the first that is there. */
static const char *const sharing_keys[] = {
	SHARING_KEY_DIR "/ssh_host_ed25519_key.pub",
	SHARING_KEY_DIR "/ssh_host_ecdsa_key.pub",
	SHARING_KEY_DIR "/ssh_host_rsa_key.pub"
};

/* The longest public key line read, and the longest key blob. */
#define SHARING_LINE_MAX	4096U
#define SHARING_BLOB_MAX	3072U

static const char sharing_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned sharing_allowed(void);
static void sharing_fingerprint(char *text, size_t size);
static int sharing_decode(const char *text, unsigned char *blob, size_t capacity, size_t *length);
static int sharing_digit(int character);
static unsigned sharing_number(const char *line, const char *key);

/*
 * Asks sessiond for Remote Login's state, or to turn it on or off.
 */
int
kl_backend_sharing_request(
	struct kl_backend *backend,
	unsigned action)
{
	const char *line;
	int managed;
	int error;

	/* Only a session sessiond started and still listens to. */
	managed = kl_backend_session_managed(backend);
	if (!managed)
		return ENOTSUP;

	/* The request's line. */
	switch (action) {
	case KL_BACKEND_SHARING_STATUS:
		line = "SERVICE sshd status\n";
		break;
	case KL_BACKEND_SHARING_ON:
		line = "SERVICE sshd on\n";
		break;
	case KL_BACKEND_SHARING_OFF:
		line = "SERVICE sshd off\n";
		break;
	default:
		return EINVAL;
	}

	/* Sent; the answer comes through session_answer. */
	error = kl_backend_session_send(backend, KL_BACKEND_SESSION_SERVICE, line);
	return error;
}

/*
 * Copies Remote Login's state as last answered, with whether this user may
 * change it and the host key's fingerprint now.
 */
void
kl_backend_sharing_get(
	const struct kl_backend *backend,
	struct kl_backend_sharing *state)
{
	/* The last answer. */
	memset(state, 0, sizeof(*state));
	if (backend != NULL)
		*state = backend->sharing;

	/* Who may change it, and the key. */
	state->allowed = sharing_allowed();
	sharing_fingerprint(state->fingerprint, sizeof(state->fingerprint));
}

/*
 * Takes sessiond's answer to a SERVICE request (session-zedbsd.c): the
 * state line kept, DENIED as EPERM, anything else as EIO.  Returns the
 * errno value of the answer.
 */
int
kl_backend_sharing_take(
	struct kl_backend *backend,
	const char *line)
{
	int differs;

	/* Refused to this user. */
	differs = strcmp(line, "DENIED");
	if (differs == 0)
		return EPERM;

	/* The state. */
	differs = strncmp(line, "SERVICE ", 8U);
	if (differs != 0)
		return EIO;
	backend->sharing.available = sharing_number(line, "available=");
	backend->sharing.enabled = sharing_number(line, "enabled=");
	backend->sharing.running = sharing_number(line, "running=");
	backend->sharing.port = sharing_number(line, "port=");
	backend->sharing.known = 1U;

	/* Succeeded: the state is kept. */
	return 0;
}

/* Tells whether this user may change Remote Login: root, or a member of wheel. */
static unsigned
sharing_allowed(void)
{
	struct group *wheel;
	gid_t groups[64];
	gid_t group;
	uid_t user;
	int count;
	int index;

	/* Root. */
	user = geteuid();
	if (user == 0)
		return 1U;

	/* wheel as the compositor's group, or among its groups. */
	wheel = getgrnam("wheel");
	if (wheel == NULL)
		return 0U;
	group = getegid();
	if (group == wheel->gr_gid)
		return 1U;
	count = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);
	for (index = 0; index < count; index++) {
		if (groups[index] == wheel->gr_gid)
			return 1U;
	}

	/* Not a member. */
	return 0U;
}

/* Gives the first host key's fingerprint ("SHA256:..."), or empty when there is none. */
static void
sharing_fingerprint(
	char *text,
	size_t size)
{
	struct command_sha256_context context;
	unsigned char blob[SHARING_BLOB_MAX];
	unsigned char digest[32];
	char line[SHARING_LINE_MAX];
	char *space;
	char *end;
	size_t length;
	size_t index;
	size_t used;
	unsigned value;
	unsigned bits;
	char *got;
	FILE *file;
	int error;

	/* The first public key that is there. */
	text[0] = '\0';
	file = NULL;
	for (index = 0; index < sizeof(sharing_keys) / sizeof(sharing_keys[0]) && file == NULL; index++)
		file = fopen(sharing_keys[index], "r");
	if (file == NULL)
		return;
	got = fgets(line, sizeof(line), file);
	(void)fclose(file);
	if (got == NULL)
		return;

	/* Its blob: the second word, in base64. */
	space = strchr(line, ' ');
	if (space == NULL)
		return;
	end = strchr(space + 1, ' ');
	if (end == NULL)
		end = strchr(space + 1, '\n');
	if (end != NULL)
		*end = '\0';
	error = sharing_decode(space + 1, blob, sizeof(blob), &length);
	if (error != 0)
		return;

	/* Its SHA-256. */
	command_sha256_init(&context);
	(void)command_sha256_update(&context, blob, length);
	command_sha256_final(&context, digest);

	/* "SHA256:" and the digest in base64 without its padding. */
	used = (size_t)snprintf(text, size, "%s", "SHA256:");
	value = 0U;
	bits = 0U;
	for (index = 0; index < sizeof(digest); index++) {
		value = (value << 8) | digest[index];
		bits += 8U;
		while (bits >= 6U && used + 1U < size) {
			bits -= 6U;
			text[used] = sharing_base64[(value >> bits) & 0x3fU];
			used++;
		}
	}

	/* The last bits, and the end. */
	if (bits != 0U && used + 1U < size) {
		text[used] = sharing_base64[(value << (6U - bits)) & 0x3fU];
		used++;
	}

	/* Its end. */
	text[used] = '\0';
}

/* Decodes base64 (with its padding); returns 0, or EINVAL for a text that is not or a blob too long. */
static int
sharing_decode(
	const char *text,
	unsigned char *blob,
	size_t capacity,
	size_t *length)
{
	unsigned value;
	unsigned bits;
	size_t used;
	int digit;

	/* Each character, six bits at a time; the padding ends it. */
	value = 0U;
	bits = 0U;
	used = 0U;
	for (; *text != '\0' && *text != '='; text++) {
		digit = sharing_digit((unsigned char)*text);
		if (digit < 0)
			return EINVAL;
		value = (value << 6) | (unsigned)digit;
		bits += 6U;
		if (bits < 8U)
			continue;

		/* A whole byte. */
		bits -= 8U;
		if (used >= capacity)
			return EINVAL;
		blob[used] = (unsigned char)((value >> bits) & 0xffU);
		used++;
	}

	/* Succeeded: the blob. */
	*length = used;
	return 0;
}

/* Gives a base64 character's value, or -1. */
static int
sharing_digit(
	int character)
{
	const char *found;

	/* Its place in the alphabet. */
	if (character == '\0')
		return -1;
	found = strchr(sharing_base64, character);
	if (found == NULL)
		return -1;

	/* Succeeded: the value. */
	return (int)(found - sharing_base64);
}

/* Reads the number after a key of the state line (0 when it is not there). */
static unsigned
sharing_number(
	const char *line,
	const char *key)
{
	const char *found;

	/* The key, then its number. */
	found = strstr(line, key);
	if (found == NULL)
		return 0U;
	return (unsigned)strtoul(found + strlen(key), NULL, 10);
}
