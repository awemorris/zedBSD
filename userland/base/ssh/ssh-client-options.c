/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command line of the ssh program: the options of ssh(1) this
 * program supports, the destination, the command, and how they become
 * settings of ssh_config(5); the settings the program refuses; and the
 * %-tokens of RemoteCommand.
 */

#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ssh-client-common.h"

static int client_add_setting(struct ssh_client_args *args, enum ssh_client_setting_kind kind, const char *value, char *error, size_t capacity);
static int client_split_destination(const char *destination, struct ssh_client_args *args, char *error, size_t capacity);
static int client_apply_line(struct ssh_config *config, const char *keyword, const char *value, char *error);
static int client_apply_forward(struct ssh_config *config, enum ssh_forward_kind kind, const char *spec, char *error);
static int client_token_value(const struct ssh_client *client, char token, const struct passwd *account, const char *local_host, const char *short_host, char *number, size_t number_size, const char **value, char *error);

/*
 * Reads the command line of the ssh program.
 *
 * The options are -t, -T, -n, -q, -v, -N and -g without an argument, and
 * -p, -l, -i, -o, -L, -R and -F with one (in the same argument, "-p22",
 * or in the next, "-p 22"); options without an argument may be put
 * together ("-tt", "-vN").  "--" ends the options.  Options after the
 * destination belong to the command.
 */
int
ssh_client_parse_args(
	int argc,
	char **argv,
	struct ssh_client_args *args,
	char *error,
	size_t capacity)
{
	const char *argument;
	const char *value;
	enum ssh_client_setting_kind kind;
	int index;
	size_t position;
	int takes_value;
	int compare;
	int status;

	/* Starts with nothing given. */
	memset(args, 0, sizeof(*args));
	if (capacity > 0)
		error[0] = '\0';

	/* Reads the options up to the destination. */
	index = 1;
	while (index < argc) {
		argument = argv[index];

		/* Stops at the first argument that is not an option. */
		if (argument[0] != '-' || argument[1] == '\0')
			break;

		/* Takes "--" as the end of the options. */
		compare = strcmp(argument, "--");
		if (compare == 0) {
			index++;
			break;
		}

		/* Reads each letter of the argument. */
		for (position = 1; argument[position] != '\0'; position++) {
			takes_value = 0;
			kind = SSH_CLIENT_SETTING_OPTION;

			/* Notes a flag, or which setting the letter gives. */
			switch (argument[position]) {
			case 't':
				args->tty_count++;
				break;
			case 'T':
				args->no_tty = 1;
				break;
			case 'n':
				args->null_stdin = 1;
				break;
			case 'q':
				args->quiet = 1;
				break;
			case 'v':
				args->verbose++;
				break;
			case 'N':
				args->no_command = 1;
				break;
			case 'g':
				args->gateway_ports = 1;
				break;
			case 'p':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_PORT;
				break;
			case 'l':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_USER;
				break;
			case 'i':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_IDENTITY;
				break;
			case 'o':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_OPTION;
				break;
			case 'L':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_LOCAL_FORWARD;
				break;
			case 'R':
				takes_value = 1;
				kind = SSH_CLIENT_SETTING_REMOTE_FORWARD;
				break;
			case 'F':
				takes_value = 1;
				break;
			default:
				snprintf(error, capacity, "unknown option -%c", argument[position]);
				return EINVAL;
			}

			/* Goes on with the next letter of a flag. */
			if (!takes_value)
				continue;

			/* Takes the value from the rest of the argument, or from the next argument. */
			if (argument[position + 1] != '\0') {
				value = argument + position + 1;
			} else {
				if (index + 1 >= argc) {
					snprintf(error, capacity, "option -%c needs an argument", argument[position]);
					return EINVAL;
				}

				/* Takes the value from the next argument. */
				index++;
				value = argv[index];
			}

			/* Keeps the -F file, or adds the setting in the order given. */
			if (argument[position] == 'F') {
				args->config_file = value;
			} else {
				status = client_add_setting(args, kind, value, error, capacity);
				if (status != 0)
					return status;
			}

			/* Ends this argument: the value used the rest of it. */
			break;
		}

		/* Moves on to the next argument. */
		index++;
	}

	/* Refuses a command line without a destination. */
	if (index >= argc) {
		snprintf(error, capacity, "no destination given");
		return EINVAL;
	}

	/* Splits the destination into its user and host. */
	status = client_split_destination(argv[index], args, error, capacity);
	if (status != 0)
		return status;

	/* Keeps the arguments after the destination as the command. */
	args->command = argv + index + 1;
	args->command_count = argc - index - 1;

	/* Refuses -N with a command: there is no session to run it in. */
	if (args->no_command && args->command_count > 0) {
		snprintf(error, capacity, "-N cannot be used with a command");
		return EINVAL;
	}

	/* Succeeded: args holds the command line. */
	return 0;
}

/*
 * Joins the command arguments with single spaces.
 */
int
ssh_client_join_command(
	const struct ssh_client_args *args,
	struct ssh_buf *out)
{
	size_t total;
	size_t length;
	int index;
	int error;

	/* Joins the arguments, a space between each two, within the limit. */
	total = 0;
	for (index = 0; index < args->command_count; index++) {
		length = strlen(args->command[index]);

		/* Puts a space before every argument after the first. */
		if (index > 0) {
			if (total >= SSH_CLIENT_COMMAND_MAX)
				return EMSGSIZE;
			ssh_buf_put_u8(out, ' ');
			total++;
		}

		/* Refuses a command longer than the server takes. */
		if (length > SSH_CLIENT_COMMAND_MAX - total)
			return EMSGSIZE;

		/* Appends the argument. */
		ssh_buf_put_bytes(out, args->command[index], length);
		total += length;
	}

	/* Reports a failure of the buffer. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: out holds the command. */
	return 0;
}

/*
 * Applies the command-line settings to config.
 */
int
ssh_client_apply_args(
	const struct ssh_client_args *args,
	struct ssh_config *config,
	char *error)
{
	const char *keyword;
	unsigned index;
	int status;

	/* The user of "user@host" comes first, so it wins over -l and every file. */
	if (args->user[0] != '\0') {
		status = client_apply_line(config, "User", args->user, error);
		if (status != 0)
			return status;
	}

	/* -T, -t and -tt say whether a terminal is asked for (-T wins). */
	keyword = NULL;
	if (args->no_tty) {
		keyword = "no";
	} else if (args->tty_count >= 2) {
		keyword = "force";
	} else if (args->tty_count == 1) {
		keyword = "yes";
	}

	/* Applies the terminal setting when a flag gave one. */
	if (keyword != NULL) {
		status = client_apply_line(config, "RequestTTY", keyword, error);
		if (status != 0)
			return status;
	}

	/* -q and -v set how much the program reports (-q wins). */
	keyword = NULL;
	if (args->quiet) {
		keyword = "QUIET";
	} else if (args->verbose >= 2) {
		keyword = "DEBUG";
	} else if (args->verbose == 1) {
		keyword = "VERBOSE";
	}

	/* Applies the log level when a flag gave one. */
	if (keyword != NULL) {
		status = client_apply_line(config, "LogLevel", keyword, error);
		if (status != 0)
			return status;
	}

	/* -g lets the local forwardings listen on every address. */
	if (args->gateway_ports) {
		status = client_apply_line(config, "GatewayPorts", "yes", error);
		if (status != 0)
			return status;
	}

	/* Applies -l, -p, -i, -o, -L and -R in the order given. */
	for (index = 0; index < args->setting_count; index++) {
		/* Chooses the keyword the flag stands for, or takes the -o line as it is. */
		switch (args->settings[index].kind) {
		case SSH_CLIENT_SETTING_USER:
			status = client_apply_line(config, "User", args->settings[index].value, error);
			break;
		case SSH_CLIENT_SETTING_PORT:
			status = client_apply_line(config, "Port", args->settings[index].value, error);
			break;
		case SSH_CLIENT_SETTING_IDENTITY:
			status = client_apply_line(config, "IdentityFile", args->settings[index].value, error);
			break;
		case SSH_CLIENT_SETTING_LOCAL_FORWARD:
			status = client_apply_forward(config, SSH_FORWARD_LOCAL, args->settings[index].value, error);
			break;
		case SSH_CLIENT_SETTING_REMOTE_FORWARD:
			status = client_apply_forward(config, SSH_FORWARD_REMOTE, args->settings[index].value, error);
			break;
		default:
			status = ssh_config_option(config, args->settings[index].value, error);
			break;
		}

		/* Stops at the first setting the configuration refuses. */
		if (status != 0)
			return status;
	}

	/* Succeeded: config holds the command-line settings. */
	return 0;
}

/*
 * Refuses the settings that ask for what the program does not do.
 */
int
ssh_client_check_settings(
	const struct ssh_config *config,
	char *error)
{
	/* X11 forwarding is not done. */
	if (config->forward_x11) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "ForwardX11 yes is not supported");
		return EINVAL;
	}

	/* The trust of X11 forwarding and its time limit mean nothing without it. */
	if (config->forward_x11_trusted) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "ForwardX11Trusted yes is not supported");
		return EINVAL;
	}

	/* A time limit on X11 channels is refused too (0 is no limit). */
	if (config->forward_x11_timeout != 0) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "ForwardX11Timeout is not supported");
		return EINVAL;
	}

	/* Succeeded: every setting is one the program acts on or leaves off. */
	return 0;
}

/*
 * Expands the %-tokens of RemoteCommand.
 */
int
ssh_client_remote_command(
	const struct ssh_client *client,
	struct ssh_buf *out,
	char *error)
{
	char local_host[SSH_HOST_MAX + 1];
	char short_host[SSH_HOST_MAX + 1];
	char number[24];
	struct passwd *account;
	const char *text;
	const char *value;
	size_t length;
	size_t total;
	int status;

	/* Takes the local account (%d, %u) and the local host name (%l, %L). */
	account = getpwuid(getuid());
	status = gethostname(local_host, sizeof(local_host));
	if (status != 0)
		local_host[0] = '\0';
	local_host[sizeof(local_host) - 1U] = '\0';

	/* The local host name up to its first dot (%L). */
	length = strcspn(local_host, ".");
	memcpy(short_host, local_host, length);
	short_host[length] = '\0';

	/* Copies the text, putting each token's value in its place. */
	text = client->config.remote_command;
	total = 0;
	while (*text != '\0') {
		/* A byte that starts no token is copied as it is. */
		if (*text != '%') {
			value = text;
			length = 1;
			text++;
		} else {
			/* Reads the token after the '%'. */
			text++;
			status = client_token_value(client, *text, account, local_host, short_host, number, sizeof(number), &value, error);
			if (status != 0)
				return status;
			length = strlen(value);
			text++;
		}

		/* Refuses a command longer than the server takes. */
		if (length > SSH_CLIENT_COMMAND_MAX - total)
			return EMSGSIZE;

		/* Appends the byte or the value. */
		ssh_buf_put_bytes(out, value, length);
		total += length;
	}

	/* Reports a failure of the buffer. */
	status = ssh_buf_error(out);
	if (status != 0)
		return status;

	/* Succeeded: out holds the command. */
	return 0;
}

/*
 * Adds one setting to the list, refusing more than the list holds.
 */
static int
client_add_setting(
	struct ssh_client_args *args,
	enum ssh_client_setting_kind kind,
	const char *value,
	char *error,
	size_t capacity)
{
	/* Refuses a setting the list has no room for. */
	if (args->setting_count >= SSH_CLIENT_SETTINGS_MAX) {
		snprintf(error, capacity, "too many options");
		return EINVAL;
	}

	/* Keeps the setting at the end of the list. */
	args->settings[args->setting_count].kind = kind;
	args->settings[args->setting_count].value = value;
	args->setting_count++;

	/* Succeeded: the setting is in the list. */
	return 0;
}

/*
 * Splits "[user@]host" at the last '@'.
 */
static int
client_split_destination(
	const char *destination,
	struct ssh_client_args *args,
	char *error,
	size_t capacity)
{
	const char *at;
	const char *host;
	size_t user_length;
	size_t host_length;

	/* Finds where the user ends, if a user is given. */
	at = strrchr(destination, '@');
	host = destination;
	user_length = 0;
	if (at != NULL) {
		user_length = (size_t)(at - destination);
		host = at + 1;
	}

	/* Refuses an empty user before the '@', or one longer than a user name may be. */
	if (at != NULL && user_length == 0) {
		snprintf(error, capacity, "empty user name in the destination");
		return EINVAL;
	}

	/* Refuses a user name that is longer than the limit. */
	if (user_length > SSH_USER_MAX) {
		snprintf(error, capacity, "user name too long");
		return EINVAL;
	}

	/* Refuses an empty host, or one that would be read as an option. */
	host_length = strlen(host);
	if (host_length == 0) {
		snprintf(error, capacity, "empty host name");
		return EINVAL;
	}

	/* Refuses a host name that could be read as an option. */
	if (host[0] == '-') {
		snprintf(error, capacity, "host name may not start with '-'");
		return EINVAL;
	}

	/* Refuses a host longer than a host name may be. */
	if (host_length > SSH_HOST_MAX) {
		snprintf(error, capacity, "host name too long");
		return EINVAL;
	}

	/* Keeps both parts. */
	memcpy(args->user, destination, user_length);
	args->user[user_length] = '\0';
	memcpy(args->host, host, host_length);
	args->host[host_length] = '\0';

	/* Succeeded: user and host are set. */
	return 0;
}

/*
 * Applies "keyword value" as a -o option, with the value in double
 * quotes so that a path with spaces stays one value.
 */
static int
client_apply_line(
	struct ssh_config *config,
	const char *keyword,
	const char *value,
	char *error)
{
	char line[SSH_CLIENT_SETTING_MAX];
	const char *quote;
	int written;
	int status;

	/* Refuses a value with a double quote, which the quoted form cannot hold. */
	quote = strchr(value, '"');
	if (quote != NULL) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "a double quote in the value of %s is not supported", keyword);
		return EINVAL;
	}

	/* Builds the line, refusing a value that does not fit. */
	written = snprintf(line, sizeof(line), "%s \"%s\"", keyword, value);
	if (written < 0 || (size_t)written >= sizeof(line)) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "the value of %s is too long", keyword);
		return EINVAL;
	}

	/* Gives the line to the configuration. */
	status = ssh_config_option(config, line, error);
	if (status != 0)
		return status;

	/* Succeeded: the setting is applied. */
	return 0;
}

/*
 * Reads a -L or -R spec and adds it to the forwardings, with the words
 * OpenSSH's ssh uses for a spec it cannot read.
 */
static int
client_apply_forward(
	struct ssh_config *config,
	enum ssh_forward_kind kind,
	const char *spec,
	char *error)
{
	struct ssh_forward forward;
	char reason[SSH_CONFIG_ERROR_MAX];
	const char *kind_name;
	int status;

	/* Names the kind as the message does. */
	kind_name = "local";
	if (kind == SSH_FORWARD_REMOTE)
		kind_name = "remote";

	/* Reads the spec; a form these programs do not have adds why. */
	status = ssh_config_parse_forward(kind, spec, &forward, reason);
	if (status == ENOTSUP) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "Bad %s forwarding specification '%s': %s", kind_name, spec, reason);
		return EINVAL;
	}
	if (status != 0) {
		snprintf(error, SSH_CONFIG_ERROR_MAX, "Bad %s forwarding specification '%s'", kind_name, spec);
		return EINVAL;
	}

	/* Adds it, unless the same forwarding is there already. */
	status = ssh_config_add_forward(config, &forward, error);
	if (status != 0)
		return status;

	/* Succeeded: the forwarding is in the settings. */
	return 0;
}

/*
 * Finds the value of one %-token of RemoteCommand (ssh_config(5)
 * TOKENS): *value points at the value, at number (number_size bytes) for
 * a number.  Returns 0, or EINVAL with error set for a token that has no
 * value here.
 */
static int
client_token_value(
	const struct ssh_client *client,
	char token,
	const struct passwd *account,
	const char *local_host,
	const char *short_host,
	char *number,
	size_t number_size,
	const char **value,
	char *error)
{
	/* Gives the token's value. */
	switch (token) {
	case '%':
		/* A literal '%'. */
		*value = "%";
		return 0;
	case 'h':
		/* The remote host name, as connected to. */
		*value = client->host_name;
		return 0;
	case 'n':
	case 'k':
		/* The host name as given (also the host key alias, which this program does not set). */
		*value = client->host;
		return 0;
	case 'p':
		/* The remote port. */
		snprintf(number, number_size, "%u", (unsigned)client->port);
		*value = number;
		return 0;
	case 'r':
		/* The remote user. */
		*value = client->user;
		return 0;
	case 'j':
		/* The jump host, of which this program has none. */
		*value = "";
		return 0;
	case 'i':
		/* The local user id. */
		snprintf(number, number_size, "%lu", (unsigned long)getuid());
		*value = number;
		return 0;
	case 'l':
		/* The local host name. */
		*value = local_host;
		return 0;
	case 'L':
		/* The local host name up to its first dot. */
		*value = short_host;
		return 0;
	case 'u':
	case 'd':
		/* The local user and its home directory, which need the account. */
		if (account == NULL || account->pw_name == NULL || account->pw_dir == NULL) {
			snprintf(error, SSH_CONFIG_ERROR_MAX, "RemoteCommand: no account for uid %lu", (unsigned long)getuid());
			return EINVAL;
		}

		/* Chooses the name or the directory. */
		*value = account->pw_name;
		if (token == 'd')
			*value = account->pw_dir;
		return 0;
	case 'C':
		/* The connection hash is SHA-1, which this program does not compute. */
		snprintf(error, SSH_CONFIG_ERROR_MAX, "RemoteCommand: %%C is not supported");
		return EINVAL;
	case '\0':
		/* A '%' at the end has no token. */
		snprintf(error, SSH_CONFIG_ERROR_MAX, "RemoteCommand: invalid format");
		return EINVAL;
	default:
		/* Any other token is unknown, as in OpenSSH. */
		snprintf(error, SSH_CONFIG_ERROR_MAX, "RemoteCommand: unknown key %%%c", token);
		return EINVAL;
	}
}
