/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the ssh program gives the engine for the user authentication (RFC
 * 4252): the identity files for "publickey", read and checked here, and
 * the passwords for "password", asked on the terminal.  The order of the
 * requests is the engine's.
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

/* The longest password prompt. */
#define CLIENT_PROMPT_MAX	(SSH_USER_MAX + SSH_HOST_MAX + 32U)

static int client_load_identity(const struct ssh_client *client, const char *configured, int named, struct ssh_buf *file);

/*
 * Loads the identity files that can be used.
 */
int
ssh_client_load_identities(
	const struct ssh_client *client,
	struct ssh_client_identities *identities)
{
	struct ssh_buf *file;
	unsigned index;
	int error;

	/* Starts with every file's bytes empty (secret: they hold the seeds). */
	for (index = 0; index < SSH_ENGINE_IDENTITIES_MAX; index++)
		ssh_buf_init(&identities->files[index], SSH_KEY_FILE_MAX, 1);
	identities->count = 0;

	/* Offers no key when publickey is off. */
	if (!client->config.pubkey)
		return 0;

	/* Loads each file in order; a file that cannot be used is passed over. */
	for (index = 0; index < client->config.identity_count && index < SSH_ENGINE_IDENTITIES_MAX; index++) {
		file = &identities->files[identities->count];
		error = client_load_identity(client, client->config.identity[index], client->identities_named, file);
		if (error == ENOMEM)
			return error;
		if (error != 0) {
			ssh_buf_free(file);
			ssh_buf_init(file, SSH_KEY_FILE_MAX, 1);
			continue;
		}

		/* Names the file by its setting and points at its bytes. */
		identities->identities[identities->count].name = client->config.identity[index];
		identities->identities[identities->count].file = file->data;
		identities->identities[identities->count].size = file->size;
		identities->count++;
	}

	/* Succeeded: identities holds the files that can be used. */
	return 0;
}

/*
 * Wipes and frees the identity files' bytes.
 */
void
ssh_client_free_identities(
	struct ssh_client_identities *identities)
{
	unsigned index;

	/* Wipes each file's bytes. */
	for (index = 0; index < SSH_ENGINE_IDENTITIES_MAX; index++)
		ssh_buf_free(&identities->files[index]);
	identities->count = 0;
}

/*
 * Answers the engine's question for a password.
 */
int
ssh_client_answer_password(
	struct ssh_client *client,
	unsigned tries,
	int *sent)
{
	char password[SSH_PASSWORD_MAX + 1];
	char prompt[CLIENT_PROMPT_MAX];
	int error;

	/* Starts with nothing sent. */
	*sent = 0;

	/* Tells the user that the last password was refused. */
	if (tries > 0)
		ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Permission denied, please try again.");

	/* Asks with the user and the host, as the user expects to see them. */
	sprintf(prompt, "%s@%s's password: ", client->user, client->host_name);
	error = ssh_client_ask(prompt, 0, password, sizeof(password));

	/* Without a terminal nobody can answer, which ends the passwords. */
	if (error != 0) {
		ssh_wipe(password, sizeof(password));
		error = ssh_engine_password(client->engine, NULL);
		if (error != 0)
			return error;
		return 0;
	}

	/* Gives the password to the engine, and forgets it. */
	error = ssh_engine_password(client->engine, password);
	ssh_wipe(password, sizeof(password));
	if (error != 0)
		return error;

	/* Notes that a password went to the server. */
	*sent = 1;

	/* Succeeded: the password is on its way. */
	return 0;
}

/*
 * Loads one identity file into file.  A missing file is passed over
 * silently unless a setting named it (named); other failures are
 * reported.  A file that is not an unencrypted private key is refused
 * here, so that the user hears why.
 */
static int
client_load_identity(
	const struct ssh_client *client,
	const char *configured,
	int named,
	struct ssh_buf *file)
{
	char path[SSH_CLIENT_PATH_MAX];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	struct ssh_key key;
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

	/* Reads the mode of the file that was opened. */
	status = fstat(descriptor, &information);
	close(descriptor);
	if (status != 0)
		return EIO;
	if ((information.st_mode & 077) != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: permissions %04o of %s are too open: others may read it; the key is not used.", (unsigned)(information.st_mode & 07777), path);
		return EACCES;
	}

	/* Reads the file. */
	error = ssh_client_read_file(path, file, SSH_KEY_FILE_MAX);
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: cannot read %s: %s", path, strerror(error));
		return error;
	}

	/* Reads the key from the file, to refuse here one the engine could not use. */
	memset(&key, 0, sizeof(key));
	error = ssh_key_parse_private((const char *)file->data, file->size, &key);
	if (error == ENOTSUP) {
		ssh_key_clear(&key);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s: only unencrypted %s keys are supported", path, SSH_KEY_NAME);
		return error;
	}

	/* Reports a file that is not a private key. */
	if (error != 0) {
		ssh_key_clear(&key);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s: not a valid private key file", path);
		return error;
	}

	/* Names the key the engine will offer, and forgets it (the engine reads it again from the bytes). */
	(void)ssh_key_fingerprint(key.public_key, fingerprint, sizeof(fingerprint));
	ssh_key_clear(&key);
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: public key %s of %s will be offered", fingerprint, path);

	/* Succeeded: file holds the private key file. */
	return 0;
}
