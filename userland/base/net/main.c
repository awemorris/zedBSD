/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD net userland command.
 */

#include "userland/base/net/protocol.h"
#include "userland/base/net/reconcile.h"
#include "userland/base/net/netconf.h"
#include "userland/base/net/publication-trace.h"
#include "userland/base/net/netutil.h"
#include "userland/base/net/wifi-store.h"
#include "userland/base/service/rcconf.h"
#include "userland/base/libedit/readline/history.h"
#include "userland/base/libedit/readline/readline.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#define NET_DNS_LIMIT 8
#define NET_CONSOLE_WORDS 12

enum console_mode {
	CONSOLE_OPERATIONAL,
	CONSOLE_CONFIGURATION,
	CONSOLE_INTERFACE
};

struct console {
	enum console_mode mode;
	struct netconf startup;
	struct netconf candidate;
	struct netconf_interface *interface;
	int dirty;
	int confirmed_pending;
	uint32_t confirmed_token;
	int writer_lock;
};

struct rollback_writer {
	FILE *stream;
	unsigned operations;
	size_t bytes;
};

static int interactive(void);
static void configuration_default(struct netconf *configuration);
static const char *console_prompt(const struct console *console);
static int confirm_discard(void);
static int split_words(char *line, char **words, int capacity);
static void release_console_line(char *, size_t);
static void console_help(enum console_mode mode);
static int console_operational(struct console *console, int count, char **words);
static int console_show(struct console *console, int count, char **words);
static int backend(const char *operation, const char *operands, int display);
static int watch_network(void);
static int backend_scoped(const char *, const char *, uint32_t);
static int backend_opcode(const char *operation, uint32_t *opcode);
static int backend_payload(uint32_t opcode, const char *operands,
	unsigned char *payload, size_t capacity, size_t *length);
static int backend_exchange(uint32_t opcode, const void *payload,
	size_t payload_length, int display, unsigned response_seconds,
	int report_errors);
static int backend_exchange_result(uint32_t, const void *, size_t, int,
	unsigned, int, uint32_t *, int *);
static int backend_response(uint32_t opcode, uint32_t request_id,
	const void *payload, size_t payload_length, int display,
	int report_errors, uint32_t *, int *);
static int write_all(int descriptor, const char *buffer, size_t length);
static int interface_name_valid(const char *name);
static int show_configuration(const struct netconf *configuration);
static int dispatch(int argc, char **argv);
static int command_help(void);
static int wifi_set_key_command(int argc, char **argv);
static int wifi_command(int argc, char **argv);
static int wifi_backend(uint32_t, const unsigned char *, size_t, int);
static int lan_command(int argc, char **argv);
static int startup_command(void);
static int start_detached(uint32_t opcode);
static int lan_send_policy(void);
static int configure_loopback(const struct netconf_interface *item);
static int mask_from_prefix(unsigned prefix, char *output, size_t capacity);
static int address_usable(struct in_addr address);
static int any_interface_configured(void);
static int wait_requested(void);
static int wait_for_address(unsigned seconds);
static int decimal_timeout(const char *text, unsigned *result);
/*
 * How long a startup that was asked to wait will wait for an address.  A
 * lease takes a few seconds where there is a server and forever where
 * there is not, so the wait has to end somewhere.
 */
#define NET_STARTUP_WAIT_SECONDS 30U

static int usage(void);
static int console_configuration(struct console *console, int count, char **words);
static struct netconf_interface *configuration_interface(struct netconf *configuration, const char *name, int create);
static int console_interface(struct console *console, int count, char **words);
static int parse_prefix(const char *text, unsigned *result);
static int transaction_exchange(uint32_t, const char *, unsigned, uint32_t,
	uint32_t *, int *, int);
static int reconcile_live(const char *, const char *, void *);
static int reconcile_file(const char *, const char *, void *);
static int write_rollback_program(const struct netconf *, const struct netconf *,
	char *, size_t, char *, size_t);
static int console_commit(struct console *, int, char **);
static int console_rollback(struct console *);
static int startup_matches(const struct console *, char *, size_t);
static void release_writer_lock(struct console *);


/*
 * Watches the network and prints each state the daemon reports.
 *
 * The connection is not half-closed after the request: it stays open in
 * both directions for as long as the watch lasts, and the daemon writes
 * into it whenever something moves.  Each frame is the same text "net show"
 * prints, so a reader that can read one can read the other.
 *
 * Runs until the daemon stops or the watcher is interrupted.
 */
static int
watch_network(
	void)
{
	struct sockaddr_un address;
	struct networkd_protocol_header request, response;
	struct networkd_field_reader reader;
	struct networkd_field field;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	int descriptor;
	int saved;

	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	/* Handles the socket failure. */
	if (descriptor < 0) {
		fprintf(stderr, "net: watch: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);

	/* Handles a daemon that is not listening. */
	if (connect(descriptor, (struct sockaddr *)&address,
	    sizeof(address)) != 0) {
		saved = errno;
		close(descriptor);
		fprintf(stderr, "net: watch: %s\n", strerror(saved));
		return 1;
	}
	request.request_id = 1U;
	request.opcode = NETWORKD_OP_SUBSCRIBE;
	request.payload_length = 0U;

	/* Handles a request that cannot be sent. */
	if (networkd_protocol_write_frame(descriptor, &request, NULL) != 0) {
		saved = errno;
		close(descriptor);
		fprintf(stderr, "net: watch: %s\n", strerror(saved));
		return 1;
	}

	/* Process each state the daemon reports, until it stops reporting. */
	for (;;) {
		uint32_t status, error;
		const unsigned char *stage;
		size_t stage_length;

		/* Stops when the daemon closes or the transport fails. */
		if (networkd_protocol_read_frame(descriptor, &response,
		    payload, sizeof(payload), NETWORKD_RESPONSE_MAX) != 0)
			break;
		networkd_field_reader_init(&reader, payload,
					   response.payload_length);
		status = NETWORKD_RESULT_OK;
		error = 0;
		stage = NULL;
		stage_length = 0;

		/* Process each field of the frame. */
		while (networkd_field_read(&reader, &field) == 0) {
			/* Handles the outcome of the frame. */
			if (field.type == NETWORKD_FIELD_STATUS) {
				(void)networkd_field_read_u32(&field, &status);
				continue;
			}

			/* Handles the reason for a refusal. */
			if (field.type == NETWORKD_FIELD_ERROR) {
				(void)networkd_field_read_u32(&field, &error);
				continue;
			}

			/* Handles the stage a refusal came from. */
			if (field.type == NETWORKD_FIELD_STAGE) {
				stage = field.value;
				stage_length = field.length;
				continue;
			}

			/* Skips every field but the state itself. */
			if (field.type != NETWORKD_FIELD_OUTPUT)
				continue;
			(void)fwrite(field.value, 1U, field.length, stdout);
		}
		(void)fflush(stdout);

		/*
		 * A refusal is said, not swallowed: a watcher that the daemon
		 * turned away would otherwise print nothing and exit as if the
		 * daemon had simply stopped.
		 */
		if (status != NETWORKD_RESULT_OK) {
			fprintf(stderr, "net: watch: %.*s: %s\n",
				(int)stage_length,
				stage != NULL ? (const char *)stage : "refused",
				strerror(error != 0 ? (int)error : EIO));
			close(descriptor);
			return 1;
		}
	}
	close(descriptor);

	/* Succeeded: the watch ended because the daemon did. */
	return 0;
}

/*
 * Runs the net command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	/* Computes the function result. */
	function_result = argc == 1 ? interactive() : dispatch(argc, argv);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the interactive operation. */
static int
interactive(
	void)
{
	char *line;
	char *history_line;
	char *words[NET_CONSOLE_WORDS];
	int count;
	int command_result;
	int secret_line;
	int status;
	struct console console;
	char error[160];
	size_t line_length;

	memset(&console, 0, sizeof(console));
	console.writer_lock = -1;
	status = 0;

	/* Handles an operation failure. */
	if (netconf_load(NETCONF_PATH, &console.startup, error,
			 sizeof(error)) != 0) {
		/* Handles the reported system error. */
		if (errno != ENOENT) {
			fprintf(stderr, "net: cannot load %s: %s\n",
				NETCONF_PATH, error);

			/* Reports operation failure. */
			return 1;
		}
		configuration_default(&console.startup);
	}
	console.candidate = console.startup;
	using_history();

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		line = readline(console_prompt(&console));

		/* Handles the line availability. */
		if (line == NULL) {
			/* Handles a failed confirm discard operation. */
			if (console.dirty && !confirm_discard())
				continue;
			break;
		}
		line_length = strlen(line);
		history_line = strdup(line);
		if (history_line == NULL) {
			release_console_line(line, line_length);
			fprintf(stderr, "net: cannot retain command line\n");
			continue;
		}
		count = split_words(line, words, NET_CONSOLE_WORDS);

		/* Checks the remaining item count. */
		if (count < 0) {
			fprintf(stderr, count == -1 ? "net: too many words\n" :
			    "net: unmatched quote or escape\n");
			release_console_line(history_line, line_length);
			release_console_line(line, line_length);
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 0) {
			release_console_line(history_line, line_length);
			release_console_line(line, line_length);
			continue;
		}
		secret_line = count >= 2 && strcmp(words[0], "wifi") == 0 &&
		    strcmp(words[1], "set-key") == 0;
		if (!secret_line)
			add_history(history_line);
		release_console_line(history_line, line_length);

		/* Selects the matching value. */
		if ((strcmp(words[0], "help") == 0 ||
		     strcmp(words[0], "?") == 0) &&
			    count == 1) {
			console_help(console.mode);
			release_console_line(line, line_length);
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 1 && strcmp(words[0], "end") == 0 &&
		    console.mode != CONSOLE_OPERATIONAL) {
			console.mode = CONSOLE_OPERATIONAL;
			console.interface = NULL;
			release_console_line(line, line_length);
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 1 && strcmp(words[0], "exit") == 0) {
			/* Handles the console condition. */
			if (console.mode == CONSOLE_INTERFACE) {
				console.mode = CONSOLE_CONFIGURATION;
				console.interface = NULL;
			} else if (console.mode == CONSOLE_CONFIGURATION)
				console.mode = CONSOLE_OPERATIONAL;
			else if (!console.dirty || confirm_discard()) {
				release_console_line(line, line_length);
				break;
			}
			release_console_line(line, line_length);
			continue;
		}

		/* Handles the console condition. */
		if (console.mode == CONSOLE_OPERATIONAL)
			command_result = console_operational(&console, count, words);
		else if (console.mode == CONSOLE_CONFIGURATION)
			command_result = console_configuration(&console, count, words);
		else
			command_result = console_interface(&console, count, words);
		release_console_line(line, line_length);
		if (command_result == 2) {
			status = 1;
			break;
		}
	}
	clear_history();
	release_writer_lock(&console);

	/* Reports the terminal console result. */
	return status;
}

/* Supports the configuration default operation. */
static void
configuration_default(
	struct netconf *configuration)
{
	memset(configuration, 0, sizeof(*configuration));
	configuration->version = 1;
	configuration->dns_mode = NETCONF_DNS_DHCP;
}

/* Supports the console prompt operation. */
static const char *
console_prompt(
	const struct console *console)
{
	static char prompt[48];

	/* Handles the console condition. */
	if (console->mode == CONSOLE_OPERATIONAL)
		return "net> ";

	/* Handles the console condition. */
	if (console->mode == CONSOLE_CONFIGURATION)
		return "net(config)> ";
	(void)snprintf(prompt, sizeof(prompt), "net(config-if:%s)> ",
		       console->interface->name);

	/* Returns the computed result. */
	return prompt;
}

/* Supports the confirm discard operation. */
static int
confirm_discard(
	void)
{
	char *answer;
	int discard;

	answer = readline("Discard unsaved changes? [y/N] ");
	discard = answer != NULL &&
		  (strcmp(answer, "y") == 0 || strcmp(answer, "yes") == 0);
	free(answer);

	/* Returns the computed result. */
	return discard;
}

/* Supports the split words operation. */
static int
split_words(
	char *line,
	char **words,
	int capacity)
{
	int count;
	int quote;
	char *input;
	char *output;

	/* Parses shell-like quoting into the same bounded mutable buffer. */
	count = 0;
	input = line;
	output = line;
	while (*input != '\0') {
		while (isspace((unsigned char)*input))
			input++;

		/* Checks the current cursor position. */
		if (*input == '\0')
			break;

		/* Checks the remaining item count. */
		if (count == capacity)
			return -1;

		/* Copies one possibly quoted or escaped operand. */
		words[count++] = output;
		quote = 0;
		while (*input != '\0') {
			if (quote != 0) {
				if (*input == quote) {
					quote = 0;
					input++;
					continue;
				}
				if (quote == '"' && *input == '\\') {
					input++;
					if (*input == '\0')
						return -2;
				}
				*output++ = *input++;
				continue;
			}
			if (*input == '\'' || *input == '"') {
				quote = *input++;
				continue;
			}
			if (*input == '\\') {
				input++;
				if (*input == '\0')
					return -2;
				*output++ = *input++;
				continue;
			}
			if (isspace((unsigned char)*input))
				break;
			*output++ = *input++;
		}
		if (quote != 0)
			return -2;
		if (*input != '\0')
			input++;
		*output++ = '\0';
	}

	/* Returns the computed result. */
	return count;
}

/* Clears and releases one readline-owned command buffer. */
static void
release_console_line(
	char *line,
	size_t length)
{
	/* Prevents a credential-bearing command from surviving in freed storage. */
	if (line == NULL)
		return;
	explicit_bzero(line, length + 1U);
	free(line);
}

/* Supports the console help operation. */
static void
console_help(
	enum console_mode mode)
{
	/* Validates the selected mode. */
	if (mode == CONSOLE_OPERATIONAL) {
		puts("Operational commands:\n"
		     "  show interfaces|interface "
		     "NAME|running-config|startup-config|candidate\n"
		     "  up NAME | down NAME | dhcp NAME [timeout SECONDS]\n"
		     "  wifi set-key SSID PASSPHRASE [auto]\n"
		     "  wifi enable | wifi disable | wifi list\n"
		     "  wifi connect SSID | wifi disconnect\n"
		     "  configure\n"
		     "  help | ? | exit");
	} else if (mode == CONSOLE_CONFIGURATION) {
		puts("Configuration commands:\n"
		     "  interface NAME       select or create an interface\n"
		     "  show candidate|startup-config|running-config\n"
		     "  commit | commit confirmed MINUTES | rollback\n"
		     "  help | ? | end | exit");
	} else {
		puts("Interface commands:\n"
		     "  enable | disable\n"
		     "  dhcp [timeout SECONDS]\n"
		     "  static ipv4 ADDRESS prefix-length BITS\n"
		     "  no ipv4\n"
		     "  up | down\n"
		     "  help | ? | end | exit");
	}
}

/* Supports the console operational operation. */
static int
console_operational(
	struct console *console,
	int count,
	char **words)
{
	int function_result;
	char timeout[32], *arguments[5] = {"net", NULL, NULL, NULL, NULL};
	char *wifi_arguments[NET_CONSOLE_WORDS + 1];
	int index;

	/* Selects the matching value. */
	if (strcmp(words[0], "show") == 0) {
		/* Obtains the console show result. */
		function_result = console_show(console, count, words);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 1 && strcmp(words[0], "configure") == 0) {
		console->mode = CONSOLE_CONFIGURATION;

		/* Reports successful completion. */
		return 0;
	}

	/* Reuses the public Wi-Fi grammar from the interactive console. */
	if (strcmp(words[0], "wifi") == 0) {
		wifi_arguments[0] = "net";
		for (index = 0; index < count; index++)
			wifi_arguments[index + 1] = words[index];
		function_result = dispatch(count + 1, wifi_arguments);

		/* Returns the public command result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 2 &&
	    (strcmp(words[0], "up") == 0 || strcmp(words[0], "down") == 0)) {
		arguments[1] = words[0];
		arguments[2] = words[1];

		/* Obtains the dispatch result. */
		function_result = dispatch(3, arguments);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if ((count == 2 || count == 4) && strcmp(words[0], "dhcp") == 0 &&
	    (count == 2 || strcmp(words[2], "timeout") == 0)) {
		arguments[1] = "dhcp";
		arguments[2] = words[1];

		/* Checks the remaining item count. */
		if (count == 4) {
			/* Handles a failed snprintf operation. */
			if (snprintf(timeout, sizeof(timeout), "--timeout=%s",
				     words[3]) >= (int)sizeof(timeout))

				/* Reports operation failure. */
				return 1;
			arguments[3] = timeout;
		}

		/* Obtains the dispatch result. */
		function_result = dispatch(count == 2 ? 3 : 4, arguments);

		/* Returns the computed result. */
		return function_result;
	}
	fprintf(stderr, "net: invalid operational command\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the console show operation. */
static int
console_show(
	struct console *console,
	int count,
	char **words)
{
	int function_result;

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[1], "interfaces") == 0) {
		/* Obtains the backend result. */
		function_result = backend("SHOW", NULL, 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed interface name valid operation. */
	if (count == 3 && strcmp(words[1], "interface") == 0 &&
	    interface_name_valid(words[2])) {
		/* Obtains the backend result. */
		function_result = backend("SHOW", words[2], 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[1], "running-config") == 0) {
		/* Obtains the backend result. */
		function_result = backend("SHOW", NULL, 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[1], "startup-config") == 0) {
		/* Obtains the show configuration result. */
		function_result = show_configuration(&console->startup);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[1], "candidate") == 0) {
		/* Obtains the show configuration result. */
		function_result = show_configuration(&console->candidate);

		/* Returns the computed result. */
		return function_result;
	}

	fprintf(stderr, "net: invalid show command\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the backend operation. */
static int
backend(
	const char *operation,
	const char *operands,
	int display)
{
	unsigned char payload[NETWORKD_REQUEST_MAX];
	size_t payload_length;
	uint32_t opcode;
	unsigned response_seconds;
	unsigned dhcp_seconds;
	const char *timeout_text;

	/* Converts the internal operation into one typed request. */
	if (backend_opcode(operation, &opcode) != 0 ||
	    backend_payload(opcode, operands, payload, sizeof(payload),
	    &payload_length) != 0) {
		fprintf(stderr, "net: invalid backend request\n");
		return 1;
	}

	/* Selects a response deadline which contains the requested operation. */
	response_seconds = 15U;
	if (opcode == NETWORKD_OP_DHCP && operands != NULL) {
		timeout_text = strrchr(operands, ' ');
		if (timeout_text != NULL &&
		    decimal_timeout(timeout_text + 1, &dhcp_seconds) == 0 &&
		    dhcp_seconds <= UINT_MAX - 5U)
			response_seconds = dhcp_seconds + 5U;
	}

	/* Exchanges the typed request with networkd. */
	return backend_exchange(opcode, payload, payload_length, display,
	    response_seconds, 1);
}

/* Sends one reconcile mutation under an optional confirmed token. */
static int
backend_scoped(
	const char *operation,
	const char *operands,
	uint32_t token)
{
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	size_t payload_length;
	uint32_t opcode;
	unsigned response_seconds;
	unsigned dhcp_seconds;
	const char *timeout_text;

	if (backend_opcode(operation, &opcode) != 0 ||
	    backend_payload(opcode, operands, payload, sizeof(payload),
	    &payload_length) != 0)
		return 1;
	if (token != 0U) {
		networkd_field_writer_init(&writer, payload, sizeof(payload));
		writer.used = payload_length;
		if (networkd_field_write_u32(&writer, NETWORKD_FIELD_TOKEN,
		    token) != 0)
			return 1;
		payload_length = writer.used;
	}
	response_seconds = 15U;
	if (opcode == NETWORKD_OP_DHCP && operands != NULL) {
		timeout_text = strrchr(operands, ' ');
		if (timeout_text != NULL && decimal_timeout(timeout_text + 1,
		    &dhcp_seconds) == 0 && dhcp_seconds <= UINT_MAX - 5U)
			response_seconds = dhcp_seconds + 5U;
	}
	return backend_exchange(opcode, payload, payload_length, 0,
	    response_seconds, 1);
}

/* Maps one internal operation name to its stable wire opcode. */
static int
backend_opcode(
	const char *operation,
	uint32_t *opcode)
{
	struct operation_map {
		const char *name;
		uint32_t opcode;
	};
	static const struct operation_map operations[] = {
		{ "SHOW", NETWORKD_OP_SHOW },
		{ "UP", NETWORKD_OP_UP },
		{ "DOWN", NETWORKD_OP_DOWN },
		{ "DHCP", NETWORKD_OP_DHCP },
		{ "STATIC", NETWORKD_OP_STATIC },
		{ "DEFAULTROUTE", NETWORKD_OP_DEFAULT_ROUTE },
		{ "DNS", NETWORKD_OP_DNS },
		{ "RELOAD", NETWORKD_OP_RELOAD },
		{ "DEFAULTROUTE_CLEAR", NETWORKD_OP_DEFAULT_ROUTE_CLEAR },
		{ "DNS_CLEAR", NETWORKD_OP_DNS_CLEAR },
		{ "LAN_ENABLE", NETWORKD_OP_LAN_ENABLE },
		{ "LAN_DISABLE", NETWORKD_OP_LAN_DISABLE }
	};
	size_t index;

	/* Finds the one exact operation spelling. */
	if (operation == NULL || opcode == NULL)
		return -1;
	for (index = 0U; index < sizeof(operations) / sizeof(operations[0]);
	    index++) {
		if (strcmp(operation, operations[index].name) != 0)
			continue;
		*opcode = operations[index].opcode;
		return 0;
	}

	/* Reports an unknown operation. */
	return -1;
}

/* Converts one existing command operand set into typed fields. */
static int
backend_payload(
	uint32_t opcode,
	const char *operands,
	unsigned char *payload,
	size_t capacity,
	size_t *length)
{
	struct networkd_field_writer writer;
	char copy[NETWORKD_REQUEST_MAX];
	char *item[16];
	char *token;
	unsigned count;
	unsigned index;
	uint32_t timeout;
	uint16_t field;

	/* Splits only the legacy internal wired operands. */
	if (payload == NULL || length == NULL ||
	    (operands != NULL && strlen(operands) >= sizeof(copy)))
		return -1;
	copy[0] = '\0';
	if (operands != NULL)
		strcpy(copy, operands);
	count = 0U;
	for (token = strtok(copy, " "); token != NULL;
	    token = strtok(NULL, " ")) {
		if (count == sizeof(item) / sizeof(item[0]))
			return -1;
		item[count++] = token;
	}

	/* Validates the exact field count for the selected opcode. */
	if ((opcode == NETWORKD_OP_SHOW && count > 1U) ||
	    ((opcode == NETWORKD_OP_UP || opcode == NETWORKD_OP_DOWN) &&
	    count != 1U) ||
	    (opcode == NETWORKD_OP_DHCP && count != 2U) ||
	    (opcode == NETWORKD_OP_STATIC && count != 5U) ||
	    (opcode == NETWORKD_OP_DEFAULT_ROUTE && count != 1U) ||
	    (opcode == NETWORKD_OP_DNS && (count == 0U || count > NET_DNS_LIMIT)) ||
	    ((opcode == NETWORKD_OP_RELOAD ||
	    opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR ||
	    opcode == NETWORKD_OP_DNS_CLEAR ||
	    opcode == NETWORKD_OP_LAN_ENABLE ||
	    opcode == NETWORKD_OP_LAN_DISABLE) && count != 0U))
		return -1;

	/* Encodes each operation through explicit field boundaries. */
	networkd_field_writer_init(&writer, payload, capacity);
	if (opcode == NETWORKD_OP_SHOW) {
		if (count == 1U && networkd_field_write(&writer,
		    NETWORKD_FIELD_INTERFACE, item[0], strlen(item[0])) != 0)
			return -1;
	} else if (opcode == NETWORKD_OP_UP || opcode == NETWORKD_OP_DOWN) {
		if (networkd_field_write(&writer, NETWORKD_FIELD_INTERFACE,
		    item[0], strlen(item[0])) != 0)
			return -1;
	} else if (opcode == NETWORKD_OP_DHCP) {
		if (decimal_timeout(item[1], &timeout) != 0 ||
		    networkd_field_write(&writer, NETWORKD_FIELD_INTERFACE,
		    item[0], strlen(item[0])) != 0 ||
		    networkd_field_write_u32(&writer, NETWORKD_FIELD_TIMEOUT,
		    timeout) != 0)
			return -1;
	} else if (opcode == NETWORKD_OP_STATIC) {
		if (strcmp(item[1], "ipv4") != 0 ||
		    strcmp(item[3], "netmask") != 0 ||
		    networkd_field_write(&writer, NETWORKD_FIELD_INTERFACE,
		    item[0], strlen(item[0])) != 0 ||
		    networkd_field_write(&writer, NETWORKD_FIELD_ADDRESS,
		    item[2], strlen(item[2])) != 0 ||
		    networkd_field_write(&writer, NETWORKD_FIELD_NETMASK,
		    item[4], strlen(item[4])) != 0)
			return -1;
	} else if (opcode == NETWORKD_OP_DEFAULT_ROUTE) {
		if (networkd_field_write(&writer, NETWORKD_FIELD_GATEWAY,
		    item[0], strlen(item[0])) != 0)
			return -1;
	} else if (opcode == NETWORKD_OP_DNS) {
		field = NETWORKD_FIELD_DNS;
		for (index = 0U; index < count; index++) {
			if (networkd_field_write(&writer, field, item[index],
			    strlen(item[index])) != 0)
				return -1;
		}
	}
	*length = writer.used;

	/* Reports successful completion. */
	return 0;
}

/* Exchanges one already encoded request and response. */
static int
backend_exchange(
	uint32_t opcode,
	const void *payload,
	size_t payload_length,
	int display,
	unsigned response_seconds,
	int report_errors)
{
	return backend_exchange_result(opcode, payload, payload_length, display,
	    response_seconds, report_errors, NULL, NULL);
}

/* Exchanges a request which may return a token and exact server error. */
static int
backend_exchange_result(
	uint32_t opcode,
	const void *payload,
	size_t payload_length,
	int display,
	unsigned response_seconds,
	int report_errors,
	uint32_t *returned_token,
	int *server_error)
{
	static uint32_t next_request_id = 1U;
	struct networkd_protocol_header request;
	struct networkd_protocol_header response;
	struct sockaddr_un address;
	struct timeval receive_timeout;
	struct timeval send_timeout;
	unsigned char response_payload[NETWORKD_RESPONSE_MAX];
	uint32_t request_id;
	int descriptor;
	int saved;

	if (returned_token != NULL)
		*returned_token = 0U;
	if (server_error != NULL)
		*server_error = 0;

	/* Allocates one nonzero request identity. */
	request_id = next_request_id++;
	if (next_request_id == 0U)
		next_request_id = 1U;
	request.request_id = request_id;
	request.opcode = opcode;
	request.payload_length = payload_length;
	NCOM_TRACE("client-socket-enter", opcode);
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		goto unavailable;

	/* Bounds transport stalls independently of operation policy. */
	if (opcode < NETWORKD_OP_WIFI_ENABLE &&
	    opcode != NETWORKD_OP_CONFIRMED_DISARM)
		response_seconds += NETWORKD_CONTROL_YIELD_SECONDS;
	if (opcode == NETWORKD_OP_CONFIRMED_DISARM) {
		send_timeout.tv_sec = 0;
		send_timeout.tv_usec = 250000;
		receive_timeout = send_timeout;
	} else {
		send_timeout.tv_sec = 5;
		send_timeout.tv_usec = 0;
		receive_timeout.tv_sec = response_seconds;
		receive_timeout.tv_usec = 0;
	}
	if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
	    sizeof(receive_timeout)) != 0 ||
	    setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
	    sizeof(send_timeout)) != 0)
		goto unavailable;

	/* Connects to the one privileged network control endpoint. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	NCOM_TRACE("client-connect-enter", opcode);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0)
		goto unavailable;
	NCOM_TRACE("client-send-enter", opcode);

	/* Writes exactly one frame and closes the request direction. */
	if (networkd_protocol_write_frame(descriptor, &request, payload) != 0 ||
	    shutdown(descriptor, SHUT_WR) != 0)
		goto unavailable;

	/* Reads one bounded, correlated terminal response. */
	NCOM_TRACE("client-receive-enter", opcode);
	if (networkd_protocol_read_frame_timed(descriptor, &response,
	    response_payload, sizeof(response_payload),
	    NETWORKD_RESPONSE_MAX,
	    opcode == NETWORKD_OP_CONFIRMED_DISARM ? 0U : response_seconds) != 0)
		goto unavailable;
	NCOM_TRACE("client-close-enter", opcode);
	close(descriptor);
	NCOM_TRACE("client-close-done", opcode);
	descriptor = -1;
	if (response.request_id != request_id || response.opcode != opcode) {
		if (server_error != NULL)
			*server_error = EINVAL;
		/* Reports a correlation failure when diagnostics are requested. */
		if (!report_errors)
			return 1;
		fprintf(stderr, "net: mismatched networkd response\n");
		return 1;
	}

	/* Returns the validated typed response result. */
	return backend_response(opcode, request_id, response_payload,
	    response.payload_length, display, report_errors, returned_token,
	    server_error);

unavailable:
	saved = errno != 0 ? errno : EIO;
	NCOM_TRACE("client-error-cleanup-enter", opcode);
	if (descriptor >= 0)
		close(descriptor);
	NCOM_TRACE("client-error-cleanup-done", opcode);
	if (server_error != NULL)
		*server_error = saved;
	if (report_errors)
		fprintf(stderr, "net: networkd is unavailable: %s\n",
		    strerror(saved));
	return 1;
}

/* Validates and presents one typed terminal response. */
static int
backend_response(
	uint32_t opcode,
	uint32_t request_id,
	const void *payload,
	size_t payload_length,
	int display,
	int report_errors,
	uint32_t *returned_token,
	int *server_error)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	const unsigned char *output;
	const unsigned char *stage;
	size_t output_length;
	size_t stage_length;
	uint32_t status;
	uint32_t error;
	uint32_t token;
	unsigned seen;
	int result;

	/* Keeps correlation values visible to fixture builds. */
	(void)opcode;
	(void)request_id;
	status = NETWORKD_RESULT_ERROR;
	error = EINVAL;
	token = 0U;
	output = NULL;
	output_length = 0U;
	stage = NULL;
	stage_length = 0U;
	seen = 0U;
	networkd_field_reader_init(&reader, payload, payload_length);

	/* Decodes each permitted response field exactly once. */
	while ((result = networkd_field_read(&reader, &field)) == 0) {
		if (field.type == NETWORKD_FIELD_STATUS && (seen & 1U) == 0U &&
		    networkd_field_read_u32(&field, &status) == 0)
			seen |= 1U;
		else if (field.type == NETWORKD_FIELD_ERROR && (seen & 2U) == 0U &&
		    networkd_field_read_u32(&field, &error) == 0)
			seen |= 2U;
		else if (field.type == NETWORKD_FIELD_STAGE && (seen & 4U) == 0U) {
			stage = field.value;
			stage_length = field.length;
			seen |= 4U;
		} else if (field.type == NETWORKD_FIELD_OUTPUT &&
		    (seen & 8U) == 0U) {
			output = field.value;
			output_length = field.length;
			seen |= 8U;
		} else if (field.type == NETWORKD_FIELD_TOKEN &&
		    (seen & 16U) == 0U && returned_token != NULL &&
		    networkd_field_read_u32(&field, &token) == 0 && token != 0U) {
			seen |= 16U;
		} else {
			result = -1;
			break;
		}
	}
	if (result < 0 || (seen & 3U) != 3U) {
		if (server_error != NULL)
			*server_error = EINVAL;
		if (report_errors)
			fprintf(stderr, "net: malformed networkd response\n");
		return 1;
	}

	/* Presents a successful optional payload without string assumptions. */
	if (status == NETWORKD_RESULT_OK && error == 0U) {
		if (returned_token != NULL) {
			if ((seen & 16U) == 0U) {
				if (server_error != NULL)
					*server_error = EINVAL;
				return 1;
			}
			*returned_token = token;
		}
		if (display && output_length != 0U &&
		    write_all(STDOUT_FILENO, (const char *)output,
		    output_length) != 0)
			return 1;
		return 0;
	}
	if (server_error != NULL)
		*server_error = error <= INT_MAX ? (int)error : EIO;
	if (display && output_length != 0U &&
	    write_all(STDOUT_FILENO, (const char *)output, output_length) != 0)
		return 1;

	/* Presents only bounded sanitized stage and error information. */
	if (report_errors) {
		fprintf(stderr, "net: %.*s%s%s (%lu)\n", (int)stage_length,
		    stage != NULL ? (const char *)stage : "",
		    stage_length != 0U ? ": " : "",
		    strerror((int)error), (unsigned long)error);
	}
	return 1;
}

/* Encodes one private confirmed-commit control request. */
static int
transaction_exchange(
	uint32_t opcode,
	const char *path,
	unsigned minutes,
	uint32_t token,
	uint32_t *returned_token,
	int *server_error,
	int report_errors)
{
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	int display;

	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (opcode == NETWORKD_OP_CONFIRMED_ARM) {
		if (path == NULL || minutes == 0U ||
		    networkd_field_write(&writer, NETWORKD_FIELD_PATH, path,
		    strlen(path)) != 0 || networkd_field_write_u32(&writer,
		    NETWORKD_FIELD_TIMEOUT, minutes) != 0)
			return 1;
	} else if (opcode == NETWORKD_OP_CONFIRMED_DISARM) {
		if (token == 0U || networkd_field_write_u32(&writer,
		    NETWORKD_FIELD_TOKEN, token) != 0)
			return 1;
	} else if (opcode == NETWORKD_OP_CONFIRMED_CHECK && token != 0U) {
		if (networkd_field_write_u32(&writer, NETWORKD_FIELD_TOKEN,
		    token) != 0)
			return 1;
	} else if (opcode != NETWORKD_OP_CONFIRMED_ROLLBACK &&
	    opcode != NETWORKD_OP_CONFIRMED_CHECK) {
		return 1;
	}
	display = opcode == NETWORKD_OP_CONFIRMED_ROLLBACK;
	return backend_exchange_result(opcode, payload, writer.used, display, 15U,
	    report_errors, returned_token, server_error);
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const char *buffer,
	size_t length)
{
	ssize_t count;
	size_t offset;

	/* Process each remaining element. */
	offset = 0;
	while (offset < length) {
		count = write(descriptor, buffer + offset, length - offset);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the interface name valid operation. */
static int
interface_name_valid(
	const char *name)
{
	size_t index, length;

	/* Handles a failed strlen operation. */
	if (name == NULL || (length = strlen(name)) == 0 || length >= IFNAMSIZ)
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		/* Handles a failed isalnum operation. */
		if (!isalnum((unsigned char)name[index]) &&
		    name[index] != '_' && name[index] != '-')

			/* Reports successful completion. */
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the show configuration operation. */
static int
show_configuration(
	const struct netconf *configuration)
{
	/* Handles the end-of-file condition. */
	if (netconf_write(stdout, configuration) != 0 ||
	    fflush(stdout) == EOF) {
		fprintf(stderr, "net: cannot write configuration\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the dispatch operation. */
static int
dispatch(
	int argc,
	char **argv)
{
	int function_result;
	struct in_addr parsed;
	int index;
	size_t used;
	char operands[NETWORKD_REQUEST_MAX];
	int length;
	unsigned timeout;

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "help") == 0) {
		/* Obtains the command help result. */
		function_result = command_help();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc >= 2 && strcmp(argv[1], "lan") == 0) {
		/* Obtains the lan command result. */
		function_result = lan_command(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "startup") == 0) {
		/* Obtains the startup command result. */
		function_result = startup_command();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc >= 3 && strcmp(argv[1], "wifi") == 0 &&
	    strcmp(argv[2], "set-key") == 0) {
		function_result = wifi_set_key_command(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Dispatches the remaining public Wi-Fi command family. */
	if (argc >= 3 && strcmp(argv[1], "wifi") == 0) {
		function_result = wifi_command(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "watch") == 0) {
		/* Obtains the watch result. */
		function_result = watch_network();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc >= 2 && strcmp(argv[1], "show") == 0 && argc <= 3) {
		/* Obtains the backend result. */
		function_result = backend("SHOW", argc == 3 ? argv[2] : NULL, 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 3 &&
	    (strcmp(argv[1], "up") == 0 || strcmp(argv[1], "down") == 0) &&
	    interface_name_valid(argv[2])) {
		/* Obtains the backend result. */
		function_result = backend(strcmp(argv[1], "up") == 0 ? "UP" : "DOWN",
			       argv[2], 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "dhcp") == 0 &&
	    interface_name_valid(argv[2]) &&
	    (argc == 3 || (strncmp(argv[3], "--timeout=", 10) == 0 &&
			   decimal_timeout(argv[3] + 10, &timeout) == 0))) {
		/* Validates the command-line arguments. */
		if (argc == 3)
			timeout = 10;
		length = snprintf(operands, sizeof(operands), "%s %u", argv[2],
				  timeout);

		/* Computes the function result. */
		function_result = length < 0 || (size_t)length >= sizeof(operands)
			   ? 1
			   : backend("DHCP", operands, 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 7 && strcmp(argv[1], "static") == 0 &&
	    interface_name_valid(argv[2]) && strcmp(argv[3], "ipv4") == 0 &&
	    strcmp(argv[5], "netmask") == 0) {
		length = snprintf(operands, sizeof(operands),
				  "%s ipv4 %s netmask %s", argv[2], argv[4],
				  argv[6]);

		/* Computes the function result. */
		function_result = length < 0 || (size_t)length >= sizeof(operands)
			   ? 1
			   : backend("STATIC", operands, 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 3 && strcmp(argv[1], "defaultroute") == 0) {
		/* Obtains the backend result. */
		function_result = backend("DEFAULTROUTE", argv[2], 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc >= 3 && argc <= NET_DNS_LIMIT + 2 &&
	    strcmp(argv[1], "dns") == 0) {
		/* Process each remaining command-line operand. */
		used = 0;
		for (index = 2; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (inet_aton(argv[index], &parsed) == 0) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			length =
			    snprintf(operands + used, sizeof(operands) - used,
				     "%s%s", used == 0 ? "" : " ", argv[index]);

			/* Checks the current data length. */
			if (length < 0 ||
			    (size_t)length >= sizeof(operands) - used)

				/* Reports operation failure. */
				return 1;
			used += (size_t)length;
		}

		/* Obtains the backend result. */
		function_result = backend("DNS", operands, 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the usage result. */
	function_result = usage();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the command help operation. */
static int
command_help(
	void)
{
	int function_result;

	puts("net commands:\n"
	     "  net                         enter the interactive console\n"
	     "  net help                    show this help\n"
	     "  net show [interface]        show networkd state\n"
	     "  net watch                   print the state each time it changes\n"
	     "  net up interface            bring a link up\n"
	     "  net down interface          bring a link down\n"
	     "  net dhcp interface [--timeout=seconds]\n"
	     "                              acquire an IPv4 lease\n"
	     "  net static interface ipv4 address netmask mask\n"
	     "                              configure a static IPv4 address\n"
	     "  net defaultroute gateway    set the default IPv4 route\n"
	     "  net dns address...          replace resolver name servers\n"
	     "  net wifi set-key SSID PASSPHRASE [auto]\n"
	     "                              save a local WPA2 profile\n"
	     "  net wifi enable             enable policy; association runs in background\n"
	     "                              use net wifi list to observe connection state\n"
	     "  net wifi disable            disable managed Wi-Fi\n"
	     "  net wifi list               show managed Wi-Fi state\n"
	     "  net wifi connect SSID       connect a saved profile\n"
	     "  net wifi disconnect         disconnect managed Wi-Fi\n"
	     "  net lan enable              manage the wired interfaces\n"
	     "  net lan disable             stop managing the wired interfaces\n"
	     "  net startup                 bring the network up, as a boot does");

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the local Wi-Fi credential update operation. */
static int
wifi_set_key_command(
	int argc,
	char **argv)
{
	int function_result;
	char error[WIFI_CONF_DIAGNOSTIC_MAX] = "";
	size_t passphrase_length;
	int automatic;
	int notification_result;
	int result;

	/* A passphrase does not exist yet when the command is incomplete. */
	if (argc < 5) {
		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}
	passphrase_length = strlen(argv[4]);

	/* Clear a supplied secret even when the remaining syntax is invalid. */
	if ((argc != 5 && argc != 6) ||
	    (argc == 6 && strcmp(argv[5], "auto") != 0)) {
		explicit_bzero(argv[4], passphrase_length);

		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}
	automatic = argc == 6;
	result = wifi_store_set_key_for_effective_user(argv[3], argv[4],
	    automatic, error, sizeof(error));
	explicit_bzero(argv[4], passphrase_length);

	/* Reports a credential store failure without retaining diagnostics. */
	if (result != 0) {
		fprintf(stderr, "net: Wi-Fi credential update failed: %s\n",
		    error[0] != '\0' ? error : strerror(errno));
	}
	wifi_conf_explicit_clear(error, sizeof(error));
	if (result != 0)
		return 1;

	/* Notifies a running daemon without sending identity or credentials. */
	notification_result = wifi_backend(NETWORKD_OP_WIFI_PROFILES_CHANGED,
	    NULL, 0U, 0);
	if (notification_result != 0)
		fprintf(stderr,
		    "net: warning: Wi-Fi profile saved; networkd notification failed\n");

	/* The durable local update remains authoritative for command success. */
	return 0;
}

/* Dispatches one public Wi-Fi orchestration request. */
static int
wifi_command(
	int argc,
	char **argv)
{
	const unsigned char *selected_ssid;
	size_t selected_ssid_length;
	uint32_t opcode;
	int display;
	int result;

	/* Recognizes the exact global command grammar. */
	selected_ssid = NULL;
	selected_ssid_length = 0U;
	display = 0;
	opcode = 0U;
	if (argc == 3 && strcmp(argv[2], "enable") == 0) {
		opcode = NETWORKD_OP_WIFI_ENABLE;
		display = 1;
	} else if (argc == 3 && strcmp(argv[2], "disable") == 0) {
		opcode = NETWORKD_OP_WIFI_DISABLE;
	} else if (argc == 3 && strcmp(argv[2], "list") == 0) {
		opcode = NETWORKD_OP_WIFI_LIST;
		display = 1;
	} else if (argc == 4 && strcmp(argv[2], "connect") == 0) {
		opcode = NETWORKD_OP_WIFI_CONNECT;
		selected_ssid = (const unsigned char *)argv[3];
		selected_ssid_length = strlen(argv[3]);
		display = 1;
	} else if (argc == 3 && strcmp(argv[2], "disconnect") == 0) {
		opcode = NETWORKD_OP_WIFI_DISCONNECT;
	} else {
		return usage();
	}
	if (selected_ssid != NULL && (selected_ssid_length == 0U ||
	    selected_ssid_length > WIFI_CONF_SSID_MAX)) {
		fprintf(stderr, "net: invalid Wi-Fi command operand\n");
		return 1;
	}

	/* Sends only the optional bounded SSID to networkd. */
	result = wifi_backend(opcode, selected_ssid, selected_ssid_length,
	    display);

	/* Returns the orchestration result. */
	return result;
}

/* Builds one typed global Wi-Fi request. */
static int
wifi_backend(
	uint32_t opcode,
	const unsigned char *selected_ssid,
	size_t selected_ssid_length,
	int display)
{
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	unsigned response_seconds;
	int report_errors;
	int result;

	/* Encodes only the explicit-connect SSID, when one was supplied. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (selected_ssid != NULL && networkd_field_write(&writer,
	    NETWORKD_FIELD_SSID, selected_ssid, selected_ssid_length) != 0)
		return 1;

	/* Selects the bounded wait and diagnostic policy for this operation. */
	response_seconds = NETWORKD_WIFI_REQUEST_SECONDS(opcode) +
	    NETWORKD_WIFI_TRANSPORT_MARGIN;
	report_errors = 1;
	if (opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED) {
		report_errors = 0;
	}

	/* Exchanges the request; the payload never contains a credential. */
	result = backend_exchange(opcode, payload, writer.used, display,
	    response_seconds, report_errors);
	networkd_protocol_clear(payload, sizeof(payload));

	/* Returns the request result. */
	return result;
}

/* Supports the decimal timeout operation. */
static int
decimal_timeout(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return -1;
	value = strtoul(text, &end, 10);

	/* Checks the current endpoint. */
	if (*end != '\0' || value < 1U || value > 3600U)
		return -1;
	*result = (unsigned)value;
	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the mask from prefix operation.
 *
 * Writes the dotted mask a prefix length stands for.  The configuration
 * counts the bits and the command that applies it takes the four numbers,
 * so one of them has to be turned into the other.
 */
static int
mask_from_prefix(
	unsigned prefix,
	char *output,
	size_t capacity)
{
	uint32_t value;
	int written;

	/* Rejects a length that is not one a mask can have. */
	if (prefix > 32U || output == NULL)
		return -1;
	value = prefix == 0U ? 0U : 0xffffffffU << (32U - prefix);
	written = snprintf(output, capacity, "%u.%u.%u.%u",
			   (value >> 24) & 0xffU, (value >> 16) & 0xffU,
			   (value >> 8) & 0xffU, value & 0xffU);

	/* Handles output that did not fit. */
	if (written < 0 || (size_t)written >= capacity)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the configure loopback operation.
 *
 * Brings the loopback up and gives it the address written for it.  A
 * machine talks to itself over this interface, so it is configured whether
 * or not anything else could be, and before anything else is: what is
 * waiting for the network may be waiting only for this.
 */
static int
configure_loopback(
	const struct netconf_interface *item)
{
	char operands[128];
	char mask[32];
	int length;

	/* Handles a failed backend operation. */
	if (backend("UP", item->name, 0) != 0)
		return 1;

	/* An interface with no address written for it is left up and bare. */
	if (item->address_count == 0)
		return 0;

	/* Handles a prefix length that cannot be written as a mask. */
	if (mask_from_prefix(item->addresses[0].prefix_length, mask,
			     sizeof(mask)) != 0) {
		fprintf(stderr, "net: %s: the prefix length cannot be used\n",
			item->name);

		/* Reports operation failure. */
		return 1;
	}
	length = snprintf(operands, sizeof(operands), "%s ipv4 %s netmask %s",
			  item->name, item->addresses[0].address, mask);

	/* Handles operands that did not fit. */
	if (length < 0 || (size_t)length >= sizeof(operands))
		return 1;

	/* Obtains the backend result. */
	return backend("STATIC", operands, 0) != 0 ? 1 : 0;
}

/*
 * Supports the lan send policy operation.
 *
 * Tells the daemon what the configuration says about the wired
 * interfaces.  The file belongs to this command, so it is read here and
 * handed over; the daemon holds what it is told and never opens the file
 * itself, which keeps one reader and one writer of it.
 *
 * Each interface is one record, because the daemon is being told about all
 * of them at once and the field set the other operations share admits one.
 */
static int
lan_send_policy(
	void)
{
	struct networkd_field_writer writer;
	struct netconf configuration;
	const struct netconf_interface *item;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	char record[128];
	char mask[32];
	char error[160] = "";
	size_t index;
	int length;

	/* Handles a configuration that cannot be read. */
	if (netconf_load(NETCONF_PATH, &configuration, error,
			 sizeof(error)) != 0) {
		fprintf(stderr, "net: cannot load %s: %s\n", NETCONF_PATH,
			error[0] != '\0' ? error : strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	networkd_field_writer_init(&writer, payload, sizeof(payload));

	/* Process each remaining element. */
	for (index = 0; index < configuration.interface_count; index++) {
		item = &configuration.interfaces[index];

		/*
		 * The loopback is not a cable.  Nothing can be plugged into
		 * it or pulled out of it, so there is nothing for the daemon
		 * to watch and nothing for it to decide: it is configured
		 * here, once, and stays as it was configured.
		 */
		if (item->type == NETCONF_INTERFACE_LOOPBACK) {
			if (item->enabled && configure_loopback(item) != 0)
				return 1;
			continue;
		}

		/* Dispatch the selected kind of record. */
		if (!item->enabled) {
			length = snprintf(record, sizeof(record),
					  "%s disabled", item->name);
		} else if (item->dhcp) {
			length = snprintf(record, sizeof(record), "%s dhcp %u",
					  item->name,
					  item->dhcp_timeout_set ?
					  item->dhcp_timeout : 10U);
		} else if (item->address_count != 0) {
			/* Handles an address whose mask cannot be written. */
			if (mask_from_prefix(
			    item->addresses[0].prefix_length, mask,
			    sizeof(mask)) != 0) {
				fprintf(stderr,
					"net: %s: the prefix length cannot be used\n",
					item->name);
				return 1;
			}
			length = snprintf(record, sizeof(record),
					  "%s static %s %s", item->name,
					  item->addresses[0].address, mask);
		} else {
			/*
			 * Enabled, and nothing said about an address.  There
			 * is nothing to configure it with, so it is left for
			 * whoever writes one.
			 */
			continue;
		}

		/* Handles a record that did not fit. */
		if (length < 0 || (size_t)length >= sizeof(record)) {
			fprintf(stderr, "net: %s: the record is too long\n",
				item->name);
			return 1;
		}

		/* Handles a payload with no room for another record. */
		if (networkd_field_write(&writer, NETWORKD_FIELD_OUTPUT,
					 record, (size_t)length) != 0) {
			fprintf(stderr, "net: too many interfaces\n");
			return 1;
		}
	}

	/* Obtains the backend exchange result. */
	return backend_exchange(NETWORKD_OP_LAN_ENABLE, payload, writer.used,
				0, 15U, 1) == 0 ? 0 : 1;
}

/*
 * Supports the address usable operation.
 *
 * Reports whether an address is one a machine can be reached at.  A
 * link-local address is not: it is what an interface is given when it could
 * not be configured, so treating it as an address would make an
 * unconfigured interface look configured.
 */
static int
address_usable(
	struct in_addr address)
{
	uint32_t value = ntohl(address.s_addr);

	/* Nothing at all, and the loopback network, are not reachable addresses. */
	if (value == 0U || (value >> 24) == 127U)
		return 0;

	/* 169.254.0.0/16 says the interface has no address of its own. */
	if ((value >> 16) == 0xa9feU)
		return 0;

	/* Reports successful completion. */
	return 1;
}

/*
 * Supports the any interface configured operation.
 *
 * Reports whether any interface has an address a machine can be reached at.
 */
static int
any_interface_configured(
	void)
{
	struct ifreq *list;
	struct ifreq request;
	unsigned count;
	unsigned index;
	int descriptor;
	int found;

	descriptor = socket(AF_INET, SOCK_DGRAM, 0);

	/* Handles a failed socket operation. */
	if (descriptor < 0)
		return 0;
	list = NULL;
	count = 0U;

	/* Handles a failed interface enumeration. */
	if (netutil_interfaces(descriptor, &list, &count) != 0) {
		(void)close(descriptor);
		return 0;
	}
	found = 0;

	/* Process each remaining element. */
	for (index = 0U; index < count && !found; index++) {
		memset(&request, 0, sizeof(request));
		strncpy(request.ifr_name, list[index].ifr_name,
			sizeof(request.ifr_name) - 1U);

		/* Handles an interface that has no address at all. */
		if (ioctl(descriptor, SIOCGIFADDR, &request) != 0)
			continue;
		if (request.ifr_addr.sa_family != AF_INET)
			continue;
		found = address_usable(((struct sockaddr_in *)
		    (void *)&request.ifr_addr)->sin_addr);
	}
	free(list);
	(void)close(descriptor);

	/* Returns the computed result. */
	return found;
}

/*
 * Supports the wait requested operation.
 *
 * Reads whether the networking service was asked to wait.  The setting
 * belongs to that service, because what it decides is when the service may
 * report itself started: a boot that needs the network must not go on
 * without one, and a boot that does not need it must not stand still.
 */
static int
wait_requested(
	void)
{
	struct rcconf_model *snapshot;
	char value[32];
	int wanted;

	snapshot = malloc(sizeof(*snapshot));

	/* Handles a failed malloc operation. */
	if (snapshot == NULL)
		return 0;
	wanted = 0;

	/* Handles a configuration that cannot be read, which asks for nothing. */
	if (rcconf_load(RCCONF_PATH, snapshot) == 0 &&
	    rcconf_setting_get(snapshot, "networking", "wait", value,
			       sizeof(value)) == 0)
		wanted = strcmp(value, "true") == 0 ||
			 strcmp(value, "yes") == 0 ||
			 strcmp(value, "1") == 0;
	free(snapshot);

	/* Returns the computed result. */
	return wanted;
}

/*
 * Supports the wait for address operation.
 *
 * Waits until some interface has an address a machine can be reached at, or
 * until the time given has passed.  Waiting is the whole point of the
 * option, so a timeout is reported rather than hidden: what follows in the
 * boot may need the network and should be told it is not there.
 */
static int
wait_for_address(
	unsigned seconds)
{
	unsigned waited;

	/* Process each remaining element. */
	for (waited = 0U; waited < seconds; waited++) {
		/* Handles an address that arrived. */
		if (any_interface_configured())
			return 0;
		(void)sleep(1U);
	}

	/* Handles an address that arrived as the last second passed. */
	if (any_interface_configured())
		return 0;
	fprintf(stderr, "net: no interface was configured within %u seconds\n",
		seconds);

	/* Reports operation failure. */
	return 1;
}

/*
 * Supports the lan command operation.
 *
 * enable tells the daemon to manage the wired interfaces, and hands over
 * what the configuration says about them.  It returns as soon as the
 * daemon has heard: the work itself happens in the background, because a
 * cable that is not in yet will not go in any sooner for being waited on.
 * disable says the daemon is to stop deciding, and leaves the interfaces
 * as they stand.
 */
static int
lan_command(
	int argc,
	char **argv)
{
	/* Handles the selected command-line operation. */
	if (argc != 3)
		return usage();
	if (strcmp(argv[2], "enable") == 0)
		return lan_send_policy();
	if (strcmp(argv[2], "disable") == 0)
		return backend("LAN_DISABLE", NULL, 0);

	/* Obtains the usage result. */
	return usage();
}

/*
 * Supports the startup operation.
 *
 * Brings the network up as a machine that has just started needs it: the
 * loopback, which a machine uses to talk to itself; the wired interfaces,
 * which the daemon then goes on watching; and the radio, which is left to
 * associate in its own time.
 *
 * Waiting belongs here rather than to either half.  What a boot waits for
 * is an address it can be reached at, from whichever interface obtains one
 * first, so it is not a question either `net lan' or `net wifi' could
 * answer alone.
 */
static int
startup_command(
	void)
{
	/* Handles a failed enable, which leaves nothing to wait for. */
	if (lan_send_policy() != 0)
		return 1;

	/*
	 * The radio is started in a process of its own.  Associating takes
	 * as long as it takes, and the wired side has no reason to stand
	 * behind it; what the attempt reports goes to the log.
	 */
	(void)start_detached(NETWORKD_OP_WIFI_ENABLE);

	/* A machine that was not told to wait is started. */
	if (!wait_requested())
		return 0;

	/* Obtains the wait for address result. */
	return wait_for_address(NET_STARTUP_WAIT_SECONDS);
}

/*
 * Supports the start detached operation.
 *
 * Sends one operation from a process of its own, so that the caller is not
 * held for as long as the work takes.  The child is a child of a child, so
 * nobody is left to wait for it: the caller is going on to other things,
 * and what the daemon did is reported through the log.
 */
static int
start_detached(
	uint32_t opcode)
{
	pid_t child;
	int status;

	child = fork();

	/* Handles a failed fork operation. */
	if (child < 0) {
		fprintf(stderr, "net: cannot fork: %s\n", strerror(errno));
		return 1;
	}

	/* The middle process exits at once and is waited for here. */
	if (child == 0) {
		if (fork() != 0)
			_exit(0);
		(void)setsid();
		_exit(wifi_backend(opcode, NULL, 0U, 0) == 0 ? 0 : 1);
	}
	status = 0;

	/* Continue while the operation condition remains true. */
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		continue;

	/* Reports successful completion. */
	return 0;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	fprintf(stderr,
		"usage: net [command]\n"
		"       net help\n"
		"       net show [interface]\n"
		"       net watch\n"
		"       net up|down interface\n"
		"       net dhcp interface [--timeout=seconds]\n"
		"       net static interface ipv4 address netmask mask\n"
		"       net defaultroute gateway\n"
		"       net dns address...\n"
		"       net wifi set-key SSID PASSPHRASE [auto]\n"
		"       net wifi enable|disable|list|disconnect\n"
		"       net wifi connect SSID\n"
		"       net lan enable\n"
		"       net lan disable\n"
		"       net startup\n");

	/* Reports operation failure. */
	return 2;
}

/* Sends one operation from the complete-intent reconcile engine. */
static int
reconcile_live(
	const char *operation,
	const char *operands,
	void *context)
{
	uint32_t token = context != NULL ? *(const uint32_t *)context : 0U;

	return backend_scoped(operation, operands, token) == 0 ? 0 : -1;
}

/* Writes one canonical rollback operation. */
static int
reconcile_file(
	const char *operation,
	const char *operands,
	void *context)
{
	struct rollback_writer *writer = context;
	int count;

	if (writer == NULL || writer->stream == NULL ||
	    writer->operations == NETWORKD_ROLLBACK_OPERATION_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	count = fprintf(writer->stream, "V1 %s%s%s\n", operation,
	    operands != NULL ? " " : "", operands != NULL ? operands : "");
	if (count < 0 || (size_t)count > NETWORKD_ROLLBACK_LINE_MAX ||
	    writer->bytes + (size_t)count > NETWORKD_ROLLBACK_PROGRAM_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	writer->operations++;
	writer->bytes += (size_t)count;
	return 0;
}

/* Creates and syncs one private rollback program for networkd to open. */
static int
write_rollback_program(
	const struct netconf *previous,
	const struct netconf *target,
	char *path,
	size_t path_capacity,
	char *error,
	size_t error_capacity)
{
	char temporary[] = "/tmp/net.rollback.XXXXXX";
	struct rollback_writer writer;
	FILE *stream;
	int descriptor;
	int saved;
	int result;

	if (path == NULL || path_capacity < sizeof(temporary)) {
		errno = EINVAL;
		return -1;
	}
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		goto failed;
	if (fchmod(descriptor, 0600) != 0) {
		saved = errno;
		(void)close(descriptor);
		(void)unlink(temporary);
		errno = saved;
		goto failed;
	}
	stream = fdopen(descriptor, "w");
	if (stream == NULL) {
		saved = errno;
		(void)close(descriptor);
		(void)unlink(temporary);
		errno = saved;
		goto failed;
	}
	memset(&writer, 0, sizeof(writer));
	writer.stream = stream;
	result = netconf_reconcile(previous, target, reconcile_file, &writer,
	    error, error_capacity);
	if (result == 0 && (fflush(stream) != 0 || fsync(fileno(stream)) != 0))
		result = -1;
	saved = errno;
	if (fclose(stream) != 0 && result == 0) {
		result = -1;
		saved = errno;
	}
	if (result != 0) {
		(void)unlink(temporary);
		errno = saved != 0 ? saved : EIO;
		goto failed;
	}
	strcpy(path, temporary);
	if (error != NULL && error_capacity != 0U)
		error[0] = '\0';
	return 0;

failed:
	if (error != NULL && error_capacity != 0U && error[0] == '\0')
		(void)snprintf(error, error_capacity, "%s", strerror(errno));
	return -1;
}

/* Confirms that the session's startup generation still owns the writer lock. */
static int
startup_matches(
	const struct console *console,
	char *error,
	size_t capacity)
{
	struct netconf current;

	if (netconf_load(NETCONF_PATH, &current, error, capacity) != 0) {
		if (errno != ENOENT)
			return -1;
		configuration_default(&current);
	}
	if (memcmp(&current, &console->startup, sizeof(current)) != 0) {
		if (error != NULL && capacity != 0U)
			(void)snprintf(error, capacity,
			    "startup configuration changed in another session");
		errno = EBUSY;
		return -1;
	}
	return 0;
}

/* Releases this process's advisory writer ownership. */
static void
release_writer_lock(
	struct console *console)
{
	if (console != NULL && console->writer_lock >= 0) {
		(void)netconf_writer_unlock(console->writer_lock);
		console->writer_lock = -1;
	}
}

/* Implements ordinary and confirmed interactive commit. */
static int
console_commit(
	struct console *console,
	int count,
	char **words)
{
	char rollback_path[NETWORKD_ROLLBACK_PATH_MAX + 1U];
	char error[160];
	unsigned minutes;
	uint32_t token;
	int server_error;
	int confirmed_command;
	int result;
	int attempt;

	error[0] = '\0';
	rollback_path[0] = '\0';
	confirmed_command = count == 3 && strcmp(words[1], "confirmed") == 0 &&
	    decimal_timeout(words[2], &minutes) == 0 &&
	    minutes <= NETWORKD_CONFIRMED_MINUTES_MAX;
	if (!((count == 1) || confirmed_command)) {
		fprintf(stderr, "net: usage: commit [confirmed MINUTES]\n");
		return 1;
	}
	if (confirmed_command && console->confirmed_pending) {
		fprintf(stderr, "net: a confirmed commit is already pending\n");
		return 1;
	}
	if (netconf_reconcile_supported(&console->candidate, error,
	    sizeof(error)) != 0) {
		fprintf(stderr, "net: candidate cannot be committed: %s\n", error);
		return 1;
	}
	if (console->writer_lock < 0) {
		console->writer_lock = netconf_writer_lock(error, sizeof(error));
		if (console->writer_lock < 0) {
			fprintf(stderr, "net: cannot lock %s: %s\n",
			    NETCONF_LOCK_PATH, error);
			return 1;
		}
	}
	if (startup_matches(console, error, sizeof(error)) != 0) {
		fprintf(stderr, "net: cannot commit: %s\n", error);
		release_writer_lock(console);
		return 1;
	}
	server_error = 0;
	if (console->confirmed_pending) {
		result = transaction_exchange(NETWORKD_OP_CONFIRMED_CHECK, NULL, 0U,
		    console->confirmed_token, NULL, &server_error, 1);
		if (result != 0) {
			fprintf(stderr,
			    "net: confirmed transaction is no longer active\n");
			console->confirmed_pending = 0;
			console->confirmed_token = 0U;
			release_writer_lock(console);
			return 1;
		}
	} else if (transaction_exchange(NETWORKD_OP_CONFIRMED_CHECK, NULL, 0U,
	    0U, NULL, &server_error, 1) != 0) {
		release_writer_lock(console);
		return 1;
	}
	if (confirmed_command) {
		if (write_rollback_program(&console->candidate, &console->startup,
		    rollback_path, sizeof(rollback_path), error, sizeof(error)) != 0) {
			fprintf(stderr, "net: cannot create rollback program: %s\n",
			    error);
			release_writer_lock(console);
			return 1;
		}
		token = 0U;
		if (transaction_exchange(NETWORKD_OP_CONFIRMED_ARM, rollback_path,
		    minutes, 0U, &token, &server_error, 1) != 0) {
			(void)unlink(rollback_path);
			release_writer_lock(console);
			return 1;
		}
		console->confirmed_pending = 1;
		console->confirmed_token = token;
		if (netconf_reconcile(&console->startup, &console->candidate,
		    reconcile_live, &console->confirmed_token, error,
		    sizeof(error)) != 0) {
			fprintf(stderr,
			    "net: candidate apply failed; rollback remains armed\n");
			return 1;
		}
		printf("Confirmed commit applied; rollback is armed for %u minute%s.\n",
		    minutes, minutes == 1U ? "" : "s");
		return 0;
	}
	if (netconf_reconcile(&console->startup, &console->candidate,
	    reconcile_live, console->confirmed_pending ?
	    &console->confirmed_token : NULL, error, sizeof(error)) != 0) {
		fprintf(stderr, "net: commit apply failed\n");
		if (!console->confirmed_pending && netconf_reconcile(
		    &console->candidate, &console->startup, reconcile_live, NULL,
		    error, sizeof(error)) != 0)
			fprintf(stderr, "net: immediate rollback degraded\n");
		if (!console->confirmed_pending)
			release_writer_lock(console);
		return 1;
	}
	NCOM_TRACE("commit-reconcile-done", 0);
	if (netconf_save_atomic_locked(NETCONF_PATH, &console->candidate, error,
	    sizeof(error)) != 0) {
		fprintf(stderr, "net: cannot save %s: %s\n", NETCONF_PATH, error);
		if (!console->confirmed_pending && netconf_reconcile(
		    &console->candidate, &console->startup, reconcile_live, NULL,
		    error, sizeof(error)) != 0)
			fprintf(stderr, "net: immediate rollback degraded\n");
		if (!console->confirmed_pending)
			release_writer_lock(console);
		return 1;
	}
	NCOM_TRACE("commit-save-done", 0);
	if (console->confirmed_pending) {
		result = 1;
		for (attempt = 0; attempt < 3; attempt++) {
			result = transaction_exchange(NETWORKD_OP_CONFIRMED_DISARM,
			    NULL, 0U, console->confirmed_token, NULL, &server_error,
			    attempt == 2);
			if (result == 0)
				break;
		}
		if (result != 0) {
			fprintf(stderr,
			    "net: outcome uncertain after publishing %s\n",
			    NETCONF_PATH);
			console->confirmed_pending = 0;
			console->confirmed_token = 0U;
			release_writer_lock(console);
			return 2;
		}
		console->confirmed_pending = 0;
		console->confirmed_token = 0U;
	}
	console->startup = console->candidate;
	console->dirty = 0;
	release_writer_lock(console);
	puts("Commit complete.");
	return 0;
}

/* Abandons local edits or executes the one daemon-owned rollback. */
static int
console_rollback(
	struct console *console)
{
	char error[160];
	struct netconf startup;
	int server_error;
	int result;

	error[0] = '\0';
	server_error = 0;
	result = transaction_exchange(NETWORKD_OP_CONFIRMED_ROLLBACK, NULL, 0U,
	    0U, NULL, &server_error, 0);
	if (result != 0 && server_error != ENOENT &&
	    console->confirmed_pending) {
		fprintf(stderr, "net: rollback request failed: %s\n",
		    strerror(server_error != 0 ? server_error : EIO));
		return 1;
	}
	if (netconf_load(NETCONF_PATH, &startup, error, sizeof(error)) != 0) {
		if (errno != ENOENT) {
			fprintf(stderr, "net: cannot reload %s: %s\n",
			    NETCONF_PATH, error);
			return 1;
		}
		configuration_default(&startup);
	}
	console->startup = startup;
	console->candidate = startup;
	console->interface = NULL;
	console->dirty = 0;
	console->confirmed_pending = 0;
	console->confirmed_token = 0U;
	release_writer_lock(console);
	puts(result == 0 ? "Rollback complete." : "Candidate reloaded.");
	return 0;
}

/* Supports the console configuration operation. */
static int
console_configuration(
	struct console *console,
	int count,
	char **words)
{
	int function_result;
	char error[160];

	/* Selects the matching value. */
	if (strcmp(words[0], "show") == 0) {
		/* Obtains the console show result. */
		function_result = console_show(console, count, words);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[0], "interface") == 0) {
		console->interface =
		    configuration_interface(&console->candidate, words[1], 1);

		/* Handles the interface availability. */
		if (console->interface == NULL) {
			fprintf(stderr,
				"net: invalid or excessive interface\n");

			/* Reports operation failure. */
			return 1;
		}
		console->mode = CONSOLE_INTERFACE;
		console->dirty = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Dispatches only the final interactive transaction grammar. */
	if (strcmp(words[0], "commit") == 0)
		return console_commit(console, count, words);
	if (count == 1 && strcmp(words[0], "rollback") == 0)
		return console_rollback(console);

	/* Handles an operation failure. */
	if (netconf_validate(&console->candidate, error, sizeof(error)) != 0)
		fprintf(stderr, "net: candidate is invalid: %s\n", error);
	else
		fprintf(stderr, "net: invalid configuration command\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the configuration interface operation. */
static struct netconf_interface *
configuration_interface(
	struct netconf *configuration,
	const char *name,
	int create)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++) {
		/* Selects the matching value. */
		if (strcmp(configuration->interfaces[index].name, name) == 0)
			return &configuration->interfaces[index];
	}

	/* Handles a failed interface name valid operation. */
	if (!create || !interface_name_valid(name) ||
	    configuration->interface_count == NETCONF_MAX_INTERFACES)

		/* Reports that no result is available. */
		return NULL;
	index = configuration->interface_count++;
	strcpy(configuration->interfaces[index].name, name);
	configuration->interfaces[index].type =
	    strcmp(name, "lo") == 0 || strcmp(name, "lo0") == 0
		? NETCONF_INTERFACE_LOOPBACK
		: NETCONF_INTERFACE_ETHERNET;
	configuration->interfaces[index].enabled = 1;
	configuration->interfaces[index].enabled_set = 1;

	/* Returns the computed result. */
	return &configuration->interfaces[index];
}

/* Supports the console interface operation. */
static int
console_interface(
	struct console *console,
	int count,
	char **words)
{
	int function_result;
	struct netconf_interface *item;
	struct in_addr parsed;
	unsigned value;

	item = console->interface;

	/* Checks the remaining item count. */
	if (count == 1 && (strcmp(words[0], "enable") == 0 ||
			   strcmp(words[0], "disable") == 0)) {
		item->enabled = strcmp(words[0], "enable") == 0;
		item->enabled_set = 1;
		console->dirty = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed decimal timeout operation. */
	if ((count == 1 || count == 3) && strcmp(words[0], "dhcp") == 0 &&
	    (count == 1 || (strcmp(words[1], "timeout") == 0 &&
			    decimal_timeout(words[2], &value) == 0))) {
		item->dhcp = 1;
		item->dhcp_set = 1;
		item->address_count = 0;

		/* Checks the remaining item count. */
		if (count == 3) {
			item->dhcp_timeout = value;
			item->dhcp_timeout_set = 1;
		} else {
			item->dhcp_timeout_set = 0;
		}
		console->dirty = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed inet aton operation. */
	if (count == 5 && strcmp(words[0], "static") == 0 &&
	    strcmp(words[1], "ipv4") == 0 &&
	    strcmp(words[3], "prefix-length") == 0 &&
	    inet_aton(words[2], &parsed) != 0 &&
	    parse_prefix(words[4], &value) == 0) {
		strcpy(item->addresses[0].address, words[2]);
		item->addresses[0].prefix_length = value;
		item->address_count = 1;
		item->dhcp = 0;
		item->dhcp_set = 1;
		item->dhcp_timeout_set = 0;
		console->dirty = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the remaining item count. */
	if (count == 2 && strcmp(words[0], "no") == 0 &&
	    strcmp(words[1], "ipv4") == 0) {
		item->address_count = 0;
		item->dhcp = 0;
		item->dhcp_set = 0;
		item->dhcp_timeout_set = 0;
		console->dirty = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the remaining item count. */
	if (count == 1 &&
	    (strcmp(words[0], "up") == 0 || strcmp(words[0], "down") == 0)) {
		/* Obtains the backend result. */
		function_result = backend(strcmp(words[0], "up") == 0 ? "UP" : "DOWN",
			       item->name, 0);

		/* Returns the computed result. */
		return function_result;
	}

	fprintf(stderr, "net: invalid interface command\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the parse prefix operation. */
static int
parse_prefix(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	value = strtoul(text, &end, 10);

	/* Validates the current text. */
	if (*text == '\0' || *end != '\0' || value > 32)
		return -1;
	*result = (unsigned)value;
	/* Reports successful completion. */
	return 0;
}
