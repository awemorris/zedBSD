/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of the server's host key by the ssh program: the user's and
 * the global known_hosts files, StrictHostKeyChecking and BatchMode
 * (ssh_config(5)), the question to the user, the line added for a new
 * host, and the rule that a re-exchange must show the key of the first
 * exchange.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The mode of a known_hosts file the program creates (the umask may take bits away). */
#define CLIENT_KNOWN_HOSTS_MODE		0644

/* The mode of the user's key directory when the program creates it. */
#define CLIENT_USER_DIRECTORY_MODE	0700

/* The user's key directory under the home directory. */
#define CLIENT_USER_DIRECTORY		"/.ssh"

/* The path that stands for "no file". */
#define CLIENT_NO_FILE			"/dev/null"

/* The longest answer to the question about a new host. */
#define CLIENT_ANSWER_MAX		128U

/* The longest name a known_hosts line uses: "[" host "]:" port. */
#define CLIENT_KNOWN_NAME_MAX		(SSH_HOST_MAX + 9U)

static int client_lookup(const struct ssh_client *client, const char *configured, const uint8_t *public_key, enum ssh_known_result *answer, int *ends_with_line);
static int client_ask_new_host(const struct ssh_client *client, const char *name, const char *fingerprint, int *accepted);
static int client_add_known_host(struct ssh_client *client, const uint8_t *public_key, const char *name);
static int client_open_known_hosts(const char *path);
static void client_known_name(const char *host, uint16_t port, char *out, size_t capacity);
static void client_warn_changed(const struct ssh_client *client, const char *name, const char *fingerprint, const char *path);

/*
 * Decides what to do with a host key.
 */
int
ssh_client_host_decide(
	enum ssh_known_result user_answer,
	enum ssh_known_result global_answer,
	enum ssh_strict_host strict,
	int batch_mode,
	enum ssh_client_host_action *action)
{
	/* Refuses an answer that no lookup gives. */
	if ((unsigned)user_answer > SSH_KNOWN_OTHER_TYPES || (unsigned)global_answer > SSH_KNOWN_OTHER_TYPES)
		return EINVAL;
	if ((unsigned)strict > SSH_STRICT_ACCEPT_NEW)
		return EINVAL;

	/* A revoked key is refused whatever else is known. */
	if (user_answer == SSH_KNOWN_REVOKED || global_answer == SSH_KNOWN_REVOKED) {
		*action = SSH_CLIENT_HOST_REFUSE_REVOKED;
		return 0;
	}

	/* A key known for the host in either file is the host's. */
	if (user_answer == SSH_KNOWN_MATCH || global_answer == SSH_KNOWN_MATCH) {
		*action = SSH_CLIENT_HOST_ACCEPT;
		return 0;
	}

	/* Another key known for the host means the key changed: refused whatever the settings say. */
	if (user_answer == SSH_KNOWN_CHANGED || global_answer == SSH_KNOWN_CHANGED) {
		*action = SSH_CLIENT_HOST_REFUSE_CHANGED;
		return 0;
	}

	/* The host is not known with an ssh-ed25519 key: the settings decide. */
	switch (strict) {
	case SSH_STRICT_YES:
		*action = SSH_CLIENT_HOST_REFUSE_UNKNOWN;
		break;
	case SSH_STRICT_NO:
	case SSH_STRICT_ACCEPT_NEW:
		*action = SSH_CLIENT_HOST_ADD;
		break;
	default:
		/* Asks, unless the program may not ask anything. */
		if (batch_mode) {
			*action = SSH_CLIENT_HOST_REFUSE_UNKNOWN;
		} else {
			*action = SSH_CLIENT_HOST_ASK;
		}
		break;
	}

	/* Succeeded: *action says what to do. */
	return 0;
}

/*
 * Checks the server's host key blob.
 */
int
ssh_client_check_host_key(
	void *context,
	const uint8_t *blob,
	size_t size)
{
	struct ssh_client *client;
	uint8_t public_key[SSH_ED25519_PUBLIC];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	char name[CLIENT_KNOWN_NAME_MAX];
	char user_path[SSH_CLIENT_PATH_MAX];
	enum ssh_known_result user_answer;
	enum ssh_known_result global_answer;
	enum ssh_client_host_action action;
	int ends_with_line;
	int accepted;
	int compare;
	int error;

	client = context;

	/* Reads the key; the transport passes only an ssh-ed25519 blob, which has one size. */
	error = ssh_key_parse_public_blob(blob, size, public_key);
	if (error != 0)
		return error;
	if (size != sizeof(client->host_key))
		return EINVAL;

	/* A re-exchange must show the key the first exchange accepted. */
	if (client->have_host_key) {
		compare = memcmp(blob, client->host_key, size);
		if (compare != 0) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: the server's host key changed during a key re-exchange; disconnecting");
			return EACCES;
		}
		ssh_client_log(client, SSH_CLIENT_LOG_DEBUG, "ssh: key re-exchange: the host key checked again is the same");
		return 0;
	}

	/* Names the key for the messages. */
	error = ssh_key_fingerprint(public_key, fingerprint, sizeof(fingerprint));
	if (error != 0)
		return error;
	client_known_name(client->host_name, client->port, name, sizeof(name));
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: server host key: %s %s", SSH_KEY_NAME, fingerprint);

	/* Looks the key up in the user's file. */
	error = client_lookup(client, client->config.user_known_hosts, public_key, &user_answer, &ends_with_line);
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: cannot read %s: %s", client->config.user_known_hosts, strerror(error));
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
		return EACCES;
	}

	/* Looks the key up in the global file. */
	error = client_lookup(client, client->config.global_known_hosts, public_key, &global_answer, NULL);
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: cannot read %s: %s", client->config.global_known_hosts, strerror(error));
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
		return EACCES;
	}

	/* Decides from both answers and the settings. */
	error = ssh_client_host_decide(user_answer, global_answer, client->config.strict, client->config.batch_mode, &action);
	if (error != 0)
		return error;

	/* Asks the user about a new host when the settings say so. */
	if (action == SSH_CLIENT_HOST_ASK) {
		error = client_ask_new_host(client, name, fingerprint, &accepted);
		if (error != 0 || !accepted) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
			return EACCES;
		}
		action = SSH_CLIENT_HOST_ADD;
	}

	/* Carries out the decision. */
	switch (action) {
	case SSH_CLIENT_HOST_ACCEPT:
		break;
	case SSH_CLIENT_HOST_ADD:
		/* A failure to write the line does not stop the connection the user accepted. */
		(void)client_add_known_host(client, public_key, name);
		break;
	case SSH_CLIENT_HOST_REFUSE_CHANGED:
		(void)ssh_client_expand_path(client->config.user_known_hosts, user_path, sizeof(user_path));
		client_warn_changed(client, name, fingerprint, user_path);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
		return EACCES;
	case SSH_CLIENT_HOST_REFUSE_REVOKED:
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: the host key %s of %s is marked as revoked.", fingerprint, name);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
		return EACCES;
	default:
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: no %s host key is known for %s and strict checking (or batch mode) forbids adding one.", SSH_KEY_NAME, name);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
		return EACCES;
	}

	/* Keeps the accepted key, which every re-exchange must show again. */
	memcpy(client->host_key, blob, size);
	client->have_host_key = 1;

	/* Succeeded: the host key is accepted. */
	return 0;
}

/*
 * Looks the key up in one known_hosts file (a missing file, and
 * /dev/null, answer SSH_KNOWN_ABSENT).  *ends_with_line (when not NULL)
 * receives whether the file is empty or ends with LF.
 */
static int
client_lookup(
	const struct ssh_client *client,
	const char *configured,
	const uint8_t *public_key,
	enum ssh_known_result *answer,
	int *ends_with_line)
{
	char path[SSH_CLIENT_PATH_MAX];
	struct ssh_buf text;
	int error;

	/* Starts with nothing known. */
	*answer = SSH_KNOWN_ABSENT;
	if (ends_with_line != NULL)
		*ends_with_line = 1;

	/* Finds the file. */
	error = ssh_client_expand_path(configured, path, sizeof(path));
	if (error != 0)
		return error;

	/* Reads it; a file that is not there knows nothing. */
	ssh_buf_init(&text, SSH_HOSTFILE_MAX, 0);
	error = ssh_client_read_file(path, &text, SSH_HOSTFILE_MAX);
	if (error == ENOENT) {
		ssh_buf_free(&text);
		return 0;
	}
	if (error != 0) {
		ssh_buf_free(&text);
		return error;
	}

	/* Looks for the host and the key. */
	error = ssh_known_hosts_check((const char *)text.data, text.size, client->host_name, client->port, public_key, answer, NULL);

	/* Notes whether a line added later starts on a line of its own. */
	if (ends_with_line != NULL &&
	    text.size > 0 &&
	    text.data[text.size - 1U] != '\n')
		*ends_with_line = 0;
	ssh_buf_free(&text);

	/* Reports a file the lookup refused. */
	if (error != 0)
		return error;

	/* Succeeded: *answer holds what the file knows. */
	return 0;
}

/*
 * Asks whether to trust a host seen for the first time; the answer is
 * yes, no, or the key's fingerprint (which counts as yes).
 */
static int
client_ask_new_host(
	const struct ssh_client *client,
	const char *name,
	const char *fingerprint,
	int *accepted)
{
	char prompt[SSH_HOST_MAX + SSH_KEY_FINGERPRINT_SIZE + 256U];
	char answer[CLIENT_ANSWER_MAX];
	const char *question;
	int is_yes;
	int is_fingerprint;
	int is_no;
	int error;

	/* Shows what is known about the host. */
	*accepted = 0;
	(void)client;
	sprintf(prompt, "The host '%s' is not known yet.\nIts %s key has the fingerprint %s.\nTrust it and go on connecting (yes/no/[fingerprint])? ", name, SSH_KEY_NAME, fingerprint);
	question = prompt;

	/* Asks until the answer is one of the three. */
	for (;;) {
		error = ssh_client_ask(question, 1, answer, sizeof(answer));
		if (error != 0)
			return error;

		/* Takes yes, or the fingerprint the user compared. */
		is_yes = strcmp(answer, "yes");
		is_fingerprint = strcmp(answer, fingerprint);
		if (is_yes == 0 || is_fingerprint == 0) {
			*accepted = 1;
			return 0;
		}

		/* Takes no. */
		is_no = strcmp(answer, "no");
		if (is_no == 0)
			return 0;

		/* Asks again, shorter. */
		question = "Please answer 'yes', 'no' or the fingerprint: ";
	}
}

/*
 * Adds the host's line to the user's known_hosts file and tells the user.
 */
static int
client_add_known_host(
	struct ssh_client *client,
	const uint8_t *public_key,
	const char *name)
{
	char path[SSH_CLIENT_PATH_MAX];
	enum ssh_known_result answer;
	struct ssh_buf line;
	ssize_t written;
	int ends_with_line;
	int descriptor;
	int compare;
	int error;

	/* Adds nothing to "no file". */
	compare = strcmp(client->config.user_known_hosts, CLIENT_NO_FILE);
	if (compare == 0)
		return 0;

	/* Finds the file and whether its last line is ended. */
	error = ssh_client_expand_path(client->config.user_known_hosts, path, sizeof(path));
	if (error != 0)
		return error;
	error = client_lookup(client, client->config.user_known_hosts, public_key, &answer, &ends_with_line);
	if (error != 0)
		return error;

	/* Builds the line, on a line of its own. */
	ssh_buf_init(&line, SSH_HOSTFILE_LINE_MAX, 0);
	if (!ends_with_line)
		ssh_buf_put_u8(&line, '\n');
	error = ssh_known_hosts_put_line(client->host_name, client->port, public_key, &line);
	if (error != 0) {
		ssh_buf_free(&line);
		return error;
	}

	/* Opens the file for appending, making it (and the user's key directory) when needed. */
	descriptor = client_open_known_hosts(path);
	if (descriptor < 0) {
		error = errno;
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: failed to add the host to %s: %s", path, strerror(error));
		ssh_buf_free(&line);
		return error;
	}

	/* Writes the line in one write, so that it is not interleaved with another writer. */
	written = write(descriptor, line.data, line.size);
	error = 0;
	if (written < 0)
		error = errno;
	if (written >= 0 && (size_t)written != line.size)
		error = EIO;
	close(descriptor);
	ssh_buf_free(&line);

	/* Reports a line that could not be written. */
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: failed to add the host to %s: %s", path, strerror(error));
		return error;
	}

	/* Succeeded: tells the user the host is now known. */
	ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Warning: Permanently added '%s' (ED25519) to the list of known hosts.", name);
	return 0;
}

/*
 * Opens a known_hosts file for appending, creating it, and creating the
 * user's key directory when the file belongs in it and it is missing.
 * Returns the descriptor or -1 with errno set.
 */
static int
client_open_known_hosts(
	const char *path)
{
	char directory[SSH_CLIENT_PATH_MAX];
	const char *slash;
	size_t directory_length;
	size_t length;
	int descriptor;
	int compare;
	int status;

	/* Opens the file, creating it; only a missing directory is worth another try. */
	descriptor = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOCTTY, CLIENT_KNOWN_HOSTS_MODE);
	if (descriptor >= 0)
		return descriptor;
	if (errno != ENOENT)
		return -1;

	/* Builds the user's key directory. */
	status = ssh_client_expand_path("~" CLIENT_USER_DIRECTORY, directory, sizeof(directory));
	if (status != 0) {
		errno = ENOENT;
		return -1;
	}

	/* Takes the directory the file is in, which must be that directory: ssh makes no other. */
	slash = strrchr(path, '/');
	if (slash == NULL) {
		errno = ENOENT;
		return -1;
	}
	length = (size_t)(slash - path);
	directory_length = strlen(directory);
	compare = 1;
	if (length == directory_length)
		compare = memcmp(path, directory, length);
	if (compare != 0) {
		errno = ENOENT;
		return -1;
	}

	/* Makes the directory, readable only by the user. */
	status = mkdir(directory, CLIENT_USER_DIRECTORY_MODE);
	if (status != 0)
		return -1;

	/* Opens the file in the new directory. */
	descriptor = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOCTTY, CLIENT_KNOWN_HOSTS_MODE);
	if (descriptor < 0)
		return -1;

	/* Succeeded: the file is open for appending. */
	return descriptor;
}

/*
 * Writes the name known_hosts uses for host and port: host for port 22,
 * "[host]:port" otherwise (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT).
 */
static void
client_known_name(
	const char *host,
	uint16_t port,
	char *out,
	size_t capacity)
{
	/* The standard port goes without its number. */
	if (port == 22) {
		snprintf(out, capacity, "%s", host);
		return;
	}

	/* Any other port goes in brackets. */
	snprintf(out, capacity, "[%s]:%u", host, (unsigned)port);
}

/*
 * Warns that the host shows a key other than the one known for it.
 */
static void
client_warn_changed(
	const struct ssh_client *client,
	const char *name,
	const char *fingerprint,
	const char *path)
{
	/* Says loudly that someone may be in the middle, and how to clear a key that really changed. */
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "@    WARNING: THE HOST KEY OF THE SERVER HAS CHANGED!     @");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "The server %s showed a %s key with the fingerprint", name, SSH_KEY_NAME);
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "%s,", fingerprint);
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "but another key is known for it.  Someone may be listening in on the");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "connection (a man-in-the-middle attack), or the host key was replaced.");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "If the key was replaced on purpose, remove the old line with");
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "  ssh-keygen -f \"%s\" -R \"%s\"", path, name);
}
