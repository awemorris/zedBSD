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
#include "userland/base/net/netconf.h"
#include "userland/base/net/wifi-store.h"
#include "userland/base/libedit/readline/history.h"
#include "userland/base/libedit/readline/readline.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
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
static int backend_opcode(const char *operation, uint32_t *opcode);
static int backend_payload(uint32_t opcode, const char *operands,
	unsigned char *payload, size_t capacity, size_t *length);
static int backend_exchange(uint32_t opcode, const void *payload,
	size_t payload_length, int display, unsigned response_seconds,
	int report_errors);
static int backend_response(uint32_t opcode, uint32_t request_id,
	const void *payload, size_t payload_length, int display,
	int report_errors);
static int write_all(int descriptor, const char *buffer, size_t length);
static int interface_name_valid(const char *name);
static int show_configuration(const struct netconf *configuration);
static int dispatch(int argc, char **argv);
static int command_help(void);
static int wifi_set_key_command(int argc, char **argv);
static int wifi_command(int argc, char **argv);
static int wifi_backend(uint32_t, const unsigned char *, size_t, int);
static int boot(void);
static int apply_candidate(const struct netconf *configuration);
static int candidate_supported(const struct netconf *configuration, char *error, size_t capacity);
static int send_up(const char *name);
static int send_dhcp(const char *name, unsigned timeout);
static int prefix_mask(unsigned prefix, char *buffer, size_t capacity);
static int send_static(const char *name, const char *address, const char *mask);
static int decimal_timeout(const char *text, unsigned *result);
static int usage(void);
static int console_configuration(struct console *console, int count, char **words);
static struct netconf_interface *configuration_interface(struct netconf *configuration, const char *name, int create);
static int console_interface(struct console *console, int count, char **words);
static int parse_prefix(const char *text, unsigned *result);

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
	int secret_line;
	struct console console;
	char error[160];
	size_t line_length;

	memset(&console, 0, sizeof(console));

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
			(void)console_operational(&console, count, words);
		else if (console.mode == CONSOLE_CONFIGURATION)
			(void)console_configuration(&console, count, words);
		else
			(void)console_interface(&console, count, words);
		release_console_line(line, line_length);
	}
	clear_history();

	/* Reports successful completion. */
	return 0;
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
		     "  apply | save | discard\n"
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
		{ "RELOAD", NETWORKD_OP_RELOAD }
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
	    (opcode == NETWORKD_OP_RELOAD && count != 0U))
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

	/* Allocates one nonzero request identity. */
	request_id = next_request_id++;
	if (next_request_id == 0U)
		next_request_id = 1U;
	request.request_id = request_id;
	request.opcode = opcode;
	request.payload_length = payload_length;
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		goto unavailable;

	/* Bounds transport stalls independently of operation policy. */
	send_timeout.tv_sec = 5;
	send_timeout.tv_usec = 0;
	receive_timeout.tv_sec = response_seconds;
	receive_timeout.tv_usec = 0;
	if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
	    sizeof(receive_timeout)) != 0 ||
	    setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
	    sizeof(send_timeout)) != 0)
		goto unavailable;

	/* Connects to the one privileged network control endpoint. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0)
		goto unavailable;

	/* Writes exactly one frame and closes the request direction. */
	if (networkd_protocol_write_frame(descriptor, &request, payload) != 0 ||
	    shutdown(descriptor, SHUT_WR) != 0)
		goto unavailable;

	/* Reads one bounded, correlated terminal response. */
	if (networkd_protocol_read_frame(descriptor, &response,
	    response_payload, sizeof(response_payload),
	    NETWORKD_RESPONSE_MAX) != 0)
		goto unavailable;
	close(descriptor);
	descriptor = -1;
	if (response.request_id != request_id || response.opcode != opcode) {
		/* Reports a correlation failure when diagnostics are requested. */
		if (!report_errors)
			return 1;
		fprintf(stderr, "net: mismatched networkd response\n");
		return 1;
	}

	/* Returns the validated typed response result. */
	return backend_response(opcode, request_id, response_payload,
	    response.payload_length, display, report_errors);

unavailable:
	saved = errno != 0 ? errno : EIO;
	if (descriptor >= 0)
		close(descriptor);
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
	int report_errors)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	const unsigned char *output;
	const unsigned char *stage;
	size_t output_length;
	size_t stage_length;
	uint32_t status;
	uint32_t error;
	unsigned seen;
	int result;

	/* Keeps correlation values visible to fixture builds. */
	(void)opcode;
	(void)request_id;
	status = NETWORKD_RESULT_ERROR;
	error = EINVAL;
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
		} else {
			result = -1;
			break;
		}
	}
	if (result < 0 || (seen & 3U) != 3U) {
		if (report_errors)
			fprintf(stderr, "net: malformed networkd response\n");
		return 1;
	}

	/* Presents a successful optional payload without string assumptions. */
	if (status == NETWORKD_RESULT_OK && error == 0U) {
		if (display && output_length != 0U &&
		    write_all(STDOUT_FILENO, (const char *)output,
		    output_length) != 0)
			return 1;
		return 0;
	}

	/* Presents only bounded sanitized stage and error information. */
	if (report_errors) {
		fprintf(stderr, "net: %.*s%s%s (%lu)\n", (int)stage_length,
		    stage != NULL ? (const char *)stage : "",
		    stage_length != 0U ? ": " : "",
		    strerror((int)error), (unsigned long)error);
	}
	return 1;
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
	if (argc == 2 && strcmp(argv[1], "boot") == 0) {
		/* Obtains the boot result. */
		function_result = boot();

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
	     "  net wifi enable             enable managed Wi-Fi\n"
	     "  net wifi disable            disable managed Wi-Fi\n"
	     "  net wifi list               show managed Wi-Fi state\n"
	     "  net wifi connect SSID       connect a saved profile\n"
	     "  net wifi disconnect         disconnect managed Wi-Fi\n"
	     "  net boot                    apply boot network configuration");

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
	response_seconds = 15U;
	report_errors = 1;
	if (opcode == NETWORKD_OP_WIFI_ENABLE ||
	    opcode == NETWORKD_OP_WIFI_CONNECT)
		response_seconds = 100U;
	else if (opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED) {
		response_seconds = 5U;
		report_errors = 0;
	}

	/* Exchanges the request; the payload never contains a credential. */
	result = backend_exchange(opcode, payload, writer.used, display,
	    response_seconds, report_errors);
	networkd_protocol_clear(payload, sizeof(payload));

	/* Returns the request result. */
	return result;
}

/* Supports the boot operation. */
static int
boot(
	void)
{
	int function_result;
	struct netconf configuration;
	char error[160] = "";

	/* Handles an operation failure. */
	if (netconf_load(NETCONF_PATH, &configuration, error, sizeof(error)) !=
	    0) {
		fprintf(stderr, "net: cannot load %s: %s\n", NETCONF_PATH,
			error[0] != '\0' ? error : strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Obtains the apply candidate result. */
	function_result = apply_candidate(&configuration);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the apply candidate operation. */
static int
apply_candidate(
	const struct netconf *configuration)
{
	const struct netconf_interface *item;
	char error[160], mask[32], operands[256];
	size_t index, dns_used;
	int length;

	dns_used = 0;

	/* Handles an operation failure. */
	if (candidate_supported(configuration, error, sizeof(error)) != 0) {
		fprintf(stderr, "net: candidate cannot be applied: %s\n",
			error);

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++) {
		item = &configuration->interfaces[index];

		/* Handles the item condition. */
		if (!item->enabled) {
			/* Handles a failed backend operation. */
			if (backend("DOWN", item->name, 0) != 0)
				return 1;
			continue;
		}

		/* Handles a failed send up operation. */
		if (send_up(item->name) != 0)
			return 1;

		/* Handles a failed send dhcp operation. */
		if (item->dhcp && send_dhcp(item->name, item->dhcp_timeout_set
							    ? item->dhcp_timeout
							    : 10) != 0)

			/* Reports operation failure. */
			return 1;

		/* Handles the item condition. */
		if (item->address_count != 0) {
			/* Handles a failed prefix mask operation. */
			if (prefix_mask(item->addresses[0].prefix_length, mask,
					sizeof(mask)) != 0 ||
			    send_static(item->name, item->addresses[0].address,
					mask) != 0)

				/* Reports operation failure. */
				return 1;
		}
	}

	/* Process each remaining element. */
	for (index = 0; index < configuration->route_count; index++) {
		/* Handles a failed backend operation. */
		if (backend("DEFAULTROUTE",
			    configuration->routes[index].gateway, 0) != 0)

			/* Reports operation failure. */
			return 1;
	}

	/* Handles the configuration condition. */
	if (configuration->dns_count != 0) {
		/* Process each remaining element. */
		for (index = 0; index < configuration->dns_count; index++) {
			length = snprintf(operands + dns_used,
					  sizeof(operands) - dns_used, "%s%s",
					  dns_used == 0 ? "" : " ",
					  configuration->dns_servers[index]);

			/* Checks the current data length. */
			if (length < 0 ||
			    (size_t)length >= sizeof(operands) - dns_used)

				/* Reports operation failure. */
				return 1;
			dns_used += (size_t)length;
		}

		/* Handles a failed backend operation. */
		if (backend("DNS", operands, 0) != 0)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the candidate supported operation. */
static int
candidate_supported(
	const struct netconf *configuration,
	char *error,
	size_t capacity)
{
	const struct netconf_interface *item;
	size_t index;

	/* Handles an operation failure. */
	if (netconf_validate(configuration, error, capacity) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++) {
		item = &configuration->interfaces[index];

		/* Handles the item condition. */
		if (item->type != NETCONF_INTERFACE_LOOPBACK &&
		    item->type != NETCONF_INTERFACE_ETHERNET) {
			(void)snprintf(
			    error, capacity,
			    "interface %s type is not yet applicable",
			    item->name);

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the item condition. */
		if (item->address_count > 1) {
			(void)snprintf(error, capacity,
				       "interface %s has multiple addresses",
				       item->name);

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Process each remaining element. */
	for (index = 0; index < configuration->route_count; index++) {
		/* Selects the matching value. */
		if (strcmp(configuration->routes[index].destination,
			   "default") != 0) {
			(void)snprintf(
			    error, capacity,
			    "only a default route is currently applicable");

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the send up operation. */
static int
send_up(
	const char *name)
{
	int function_result;

	/* Obtains the backend result. */
	function_result = backend("UP", name, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the send dhcp operation. */
static int
send_dhcp(
	const char *name,
	unsigned timeout)
{
	int function_result;
	char operands[96];

	/* Handles a failed snprintf operation. */
	if (snprintf(operands, sizeof(operands), "%s %u", name, timeout) >=
	    (int)sizeof(operands))

		/* Reports operation failure. */
		return 1;

	/* Obtains the backend result. */
	function_result = backend("DHCP", operands, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the prefix mask operation. */
static int
prefix_mask(
	unsigned prefix,
	char *buffer,
	size_t capacity)
{
	int function_result;
	uint32_t mask = prefix == 0 ? 0 : 0xffffffffU << (32U - prefix);

	/* Computes the function result. */
	function_result = snprintf(buffer, capacity, "%u.%u.%u.%u", (mask >> 24) & 0xffU,
			(mask >> 16) & 0xffU, (mask >> 8) & 0xffU,
			mask & 0xffU) >= (int)capacity
		   ? -1
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the send static operation. */
static int
send_static(
	const char *name,
	const char *address,
	const char *mask)
{
	int function_result;
	char operands[160];

	/* Handles a failed snprintf operation. */
	if (snprintf(operands, sizeof(operands), "%s ipv4 %s netmask %s", name,
		     address, mask) >= (int)sizeof(operands))

		/* Reports operation failure. */
		return 1;

	/* Obtains the backend result. */
	function_result = backend("STATIC", operands, 0);

	/* Returns the computed result. */
	return function_result;
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

/* Supports the usage operation. */
static int
usage(
	void)
{
	fprintf(stderr,
		"usage: net [command]\n"
		"       net help\n"
		"       net show [interface]\n"
		"       net up|down interface\n"
		"       net dhcp interface [--timeout=seconds]\n"
		"       net static interface ipv4 address netmask mask\n"
		"       net defaultroute gateway\n"
		"       net dns address...\n"
		"       net wifi set-key SSID PASSPHRASE [auto]\n"
		"       net wifi enable|disable|list|disconnect\n"
		"       net wifi connect SSID\n"
		"       net boot\n");

	/* Reports operation failure. */
	return 2;
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

	/* Checks the remaining item count. */
	if (count == 1 && strcmp(words[0], "apply") == 0) {
		/* Obtains the apply candidate result. */
		function_result = apply_candidate(&console->candidate);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the remaining item count. */
	if (count == 1 && strcmp(words[0], "save") == 0) {
		/* Handles an operation failure. */
		if (netconf_save_atomic(NETCONF_PATH, &console->candidate,
					error, sizeof(error)) != 0) {
			fprintf(stderr, "net: cannot save %s: %s\n",
				NETCONF_PATH, error);

			/* Reports operation failure. */
			return 1;
		}
		console->startup = console->candidate;
		console->dirty = 0;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the remaining item count. */
	if (count == 1 && strcmp(words[0], "discard") == 0) {
		console->candidate = console->startup;
		console->interface = NULL;
		console->dirty = 0;

		/* Reports successful completion. */
		return 0;
	}

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
