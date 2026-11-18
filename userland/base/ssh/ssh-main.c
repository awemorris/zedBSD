/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh program: connects to a server, checks its host key against
 * known_hosts, logs in with a key or a password, and runs a shell or a
 * command on a terminal or on the standard streams.
 *
 * The options and the settings are those of ssh(1) and ssh_config(5)
 * that this program supports (ssh_client_parse_args, ssh-config.h).  The
 * exit value is the remote command's exit status, or 255 when the
 * connection failed or the command ended without a status (ssh(1) EXIT
 * STATUS).
 */

#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The system-wide client configuration, read after the user's. */
#define CLIENT_SYSTEM_CONFIG	"/etc/ssh/ssh_config"

/* The user's client configuration. */
#define CLIENT_USER_CONFIG	"~/.ssh/config"

/* The -F value that reads no configuration file. */
#define CLIENT_NO_CONFIG	"none"

static void client_usage(void);
static int client_read_config(struct ssh_config *config, const char *configured, const char *host, int required);
static int client_settle(struct ssh_client *client, const struct ssh_client_args *args);
static int client_run(struct ssh_client *client, const struct ssh_client_args *args, int *exit_status);

/*
 * The one connection of the program.
 *
 * It is static because it holds the receive buffer and the transport's
 * keys, which are large for the stack; main starts it with
 * ssh_client_init and frees it before the program ends.
 */
static struct ssh_client client_connection;

/*
 * Runs the ssh program.
 */
int
main(
	int argc,
	char **argv)
{
	struct ssh_client_args args;
	struct sigaction ignore;
	char error_text[SSH_CONFIG_ERROR_MAX];
	int exit_status;
	int status;

	/* Takes a closed output or socket as a write error, not as a signal that kills the program. */
	memset(&ignore, 0, sizeof(ignore));
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	(void)sigaction(SIGPIPE, &ignore, NULL);

	/* Reads the command line. */
	status = ssh_client_parse_args(argc, argv, &args, error_text, sizeof(error_text));
	if (status != 0) {
		fprintf(stderr, "ssh: %s\n", error_text);
		client_usage();
		return SSH_CLIENT_EXIT_FAILURE;
	}

	/* Settles the settings, the names and the user of the connection. */
	ssh_client_init(&client_connection);
	status = client_settle(&client_connection, &args);
	if (status != 0) {
		ssh_client_free(&client_connection);
		return SSH_CLIENT_EXIT_FAILURE;
	}

	/* Runs the connection. */
	exit_status = SSH_CLIENT_EXIT_FAILURE;
	status = client_run(&client_connection, &args, &exit_status);
	ssh_client_free(&client_connection);

	/* Ends with 255 for a failed connection. */
	if (status != 0)
		return SSH_CLIENT_EXIT_FAILURE;

	/* Succeeded: ends with the remote command's status. */
	return exit_status;
}

/*
 * Prints the forms of the command line the program understands.
 */
static void
client_usage(void)
{
	/* Lists the options supported. */
	fputs("usage: ssh [-NnqTtv] [-F configfile] [-i identity_file] [-l login_name]\n", stderr);
	fputs("           [-o option] [-p port] destination [command [argument ...]]\n", stderr);
}

/*
 * Reads one configuration file for host.  A file that is not there is
 * passed over unless required (the -F file).
 */
static int
client_read_config(
	struct ssh_config *config,
	const char *configured,
	const char *host,
	int required)
{
	char path[SSH_CLIENT_PATH_MAX];
	char error_text[SSH_CONFIG_ERROR_MAX];
	struct ssh_buf text;
	int error;

	/* Finds the file; without a home directory there is no user file. */
	error = ssh_client_expand_path(configured, path, sizeof(path));
	if (error != 0 && !required)
		return 0;
	if (error != 0) {
		fprintf(stderr, "ssh: %s: %s\n", configured, strerror(error));
		return error;
	}

	/* Reads it, passing over a file that is not there when it may be missing. */
	ssh_buf_init(&text, SSH_CONFIG_FILE_MAX, 0);
	error = ssh_client_read_file(path, &text, SSH_CONFIG_FILE_MAX);
	if (error == ENOENT && !required) {
		ssh_buf_free(&text);
		return 0;
	}

	/* Reports any other read failure. */
	if (error != 0) {
		fprintf(stderr, "ssh: cannot read %s: %s\n", path, strerror(error));
		ssh_buf_free(&text);
		return error;
	}

	/* Applies the lines for the host. */
	error = ssh_config_parse(config, (const char *)text.data, text.size, host, error_text);
	ssh_buf_free(&text);
	if (error != 0) {
		fprintf(stderr, "ssh: %s: %s\n", path, error_text);
		return error;
	}

	/* Succeeded: the file's settings are applied. */
	return 0;
}

/*
 * Settles everything the connection needs before it starts: the
 * settings (command line, then the -F file or the user's and the
 * system's files, then the defaults), the name connected to, the user and
 * the port.  Prints why on a failure.
 */
static int
client_settle(
	struct ssh_client *client,
	const struct ssh_client_args *args)
{
	char error_text[SSH_CONFIG_ERROR_MAX];
	struct passwd *account;
	const char *name;
	size_t length;
	int is_none;
	int error;

	/* Applies the command line first: the first value of a keyword wins. */
	ssh_config_init(&client->config);
	error = ssh_client_apply_args(args, &client->config, error_text);
	if (error != 0) {
		fprintf(stderr, "ssh: %s\n", error_text);
		return error;
	}

	/* Applies the -F file alone (none for no file), or the user's file and then the system's. */
	if (args->config_file != NULL) {
		is_none = strcmp(args->config_file, CLIENT_NO_CONFIG);
		if (is_none != 0) {
			error = client_read_config(&client->config, args->config_file, args->host, 1);
			if (error != 0)
				return error;
		}
	} else {
		error = client_read_config(&client->config, CLIENT_USER_CONFIG, args->host, 0);
		if (error != 0)
			return error;
		error = client_read_config(&client->config, CLIENT_SYSTEM_CONFIG, args->host, 0);
		if (error != 0)
			return error;
	}

	/* Notes whether a setting named the identity files, then fills in every default. */
	client->identities_named = 0;
	if (client->config.identity_count > 0)
		client->identities_named = 1;
	ssh_config_defaults(&client->config);
	client->log_level = client->config.log_level;

	/* Keeps the name given, and connects to HostName when a setting gave one. */
	memcpy(client->host, args->host, sizeof(client->host));
	name = args->host;
	if (client->config.host_name[0] != '\0')
		name = client->config.host_name;
	length = strlen(name);
	if (length > SSH_HOST_MAX) {
		fprintf(stderr, "ssh: host name too long\n");
		return EINVAL;
	}

	/* Copies the host name into the client. */
	memcpy(client->host_name, name, length + 1U);

	/* Takes the user from the settings, or the local login name. */
	name = client->config.user;
	if (name[0] == '\0') {
		account = getpwuid(getuid());
		if (account == NULL || account->pw_name == NULL) {
			fprintf(stderr, "ssh: no user name for uid %lu; use -l\n", (unsigned long)getuid());
			return ENOENT;
		}

		/* Takes the account name as the user. */
		name = account->pw_name;
	}

	/* Checks the length of the user name. */
	length = strlen(name);
	if (length == 0 || length > SSH_USER_MAX) {
		fprintf(stderr, "ssh: the user name is empty or too long\n");
		return EINVAL;
	}

	/* Copies the user name into the client. */
	memcpy(client->user, name, length + 1U);

	/* Takes the port. */
	client->port = client->config.port;

	/* Succeeded: the connection may start. */
	return 0;
}

/*
 * Connects, authenticates and runs the session or the connection without
 * one.  Returns 0 when the session ran (its exit value in *exit_status),
 * or an error after the reason was printed.
 */
static int
client_run(
	struct ssh_client *client,
	const struct ssh_client_args *args,
	int *exit_status)
{
	struct ssh_buf command;
	uint32_t code;
	int error;

	/* Builds the command from the arguments after the destination. */
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	error = ssh_client_join_command(args, &command);
	if (error != 0) {
		fprintf(stderr, "ssh: the command is too long\n");
		ssh_buf_free(&command);
		return error;
	}

	/* Connects to the server. */
	error = ssh_client_connect(client);
	if (error != 0) {
		ssh_buf_free(&command);
		return error;
	}

	/* Starts the transport and logs in. */
	error = ssh_client_start(client);
	if (error == 0)
		error = ssh_client_authenticate(client);
	if (error != 0) {
		ssh_client_report(client, error);
		ssh_client_close(client, SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
		ssh_buf_free(&command);
		return error;
	}

	/* Runs the session, or keeps the connection without one (-N). */
	if (args->no_command) {
		error = ssh_client_run_without_session(client, exit_status);
	} else {
		error = ssh_client_run_session(client, args, command.data, command.size, exit_status);
	}

	/* Releases the command text. */
	ssh_buf_free(&command);

	/* Ends the connection, telling the server why. */
	code = SSH_DISCONNECT_BY_APPLICATION;
	if (error != 0)
		code = SSH_DISCONNECT_PROTOCOL_ERROR;
	ssh_client_close(client, code, "disconnected by user");

	/* Reports a connection that broke (the reason is already printed). */
	if (error != 0)
		return error;

	/* Succeeded: *exit_status holds the session's exit value. */
	return 0;
}
