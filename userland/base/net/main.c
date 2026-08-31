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
#include "userland/base/libedit/readline/history.h"
#include "userland/base/libedit/readline/readline.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void console_help(enum console_mode mode);
static int console_operational(struct console *console, int count, char **words);
static int console_show(struct console *console, int count, char **words);
static int backend(const char *operation, const char *operands, int display);
static int write_all(int descriptor, const char *buffer, size_t length);
static int interface_name_valid(const char *name);
static int show_configuration(const struct netconf *configuration);
static int dispatch(int argc, char **argv);
static int command_help(void);
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
	char *words[NET_CONSOLE_WORDS];
	int count;
	struct console console;
	char error[160];

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
		count = split_words(line, words, NET_CONSOLE_WORDS);

		/* Checks the remaining item count. */
		if (count < 0) {
			fprintf(stderr, "net: too many words\n");
			free(line);
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 0) {
			free(line);
			continue;
		}
		add_history(line);

		/* Selects the matching value. */
		if ((strcmp(words[0], "help") == 0 ||
		     strcmp(words[0], "?") == 0) &&
		    count == 1) {
			console_help(console.mode);
			free(line);
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 1 && strcmp(words[0], "end") == 0 &&
		    console.mode != CONSOLE_OPERATIONAL) {
			console.mode = CONSOLE_OPERATIONAL;
			console.interface = NULL;
			free(line);
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
				free(line);
				break;
			}
			free(line);
			continue;
		}

		/* Handles the console condition. */
		if (console.mode == CONSOLE_OPERATIONAL)
			(void)console_operational(&console, count, words);
		else if (console.mode == CONSOLE_CONFIGURATION)
			(void)console_configuration(&console, count, words);
		else
			(void)console_interface(&console, count, words);
		free(line);
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
	char *cursor;

	/* Continue while the operation condition remains true. */
	count = 0;
	cursor = line;
	while (*cursor != '\0') {
		/* Continue while the operation condition remains true. */
		while (isspace((unsigned char)*cursor))
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			break;

		/* Checks the remaining item count. */
		if (count == capacity)
			return -1;

		/* Continue while the operation condition remains true. */
		words[count++] = cursor;
		while (*cursor != '\0' && !isspace((unsigned char)*cursor))
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor != '\0')
			*cursor++ = '\0';
	}

	/* Returns the computed result. */
	return count;
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
	const char *payload;
	struct sockaddr_un address;
	char request[NETWORKD_REQUEST_MAX], response[NETWORKD_RESPONSE_MAX];
	size_t used;
	int descriptor;
	ssize_t count;

	used = 0;
	descriptor = -1;

	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		goto unavailable;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);

	/* Handles a failed connect operation. */
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0)
		goto unavailable;

	/* Handles a failed snprintf operation. */
	if (snprintf(request, sizeof(request), "%s %s%s%s\n",
		     NETWORKD_PROTOCOL_VERSION, operation,
		     operands != NULL && *operands != '\0' ? " " : "",
		     operands != NULL ? operands : "") >=
		(int)sizeof(request) ||
	    write_all(descriptor, request, strlen(request)) != 0)
		goto unavailable;
	(void)shutdown(descriptor, SHUT_WR);

	/* Process input until it is exhausted. */
	while (used + 1U < sizeof(response) &&
	       (count = read(descriptor, response + used,
			     sizeof(response) - used - 1U)) != 0) {
		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			goto unavailable;
		used += (size_t)count;
	}
	close(descriptor);
	descriptor = -1;
	response[used] = '\0';

	/* Selects the matching prefix. */
	if (strncmp(response, NETWORKD_PROTOCOL_VERSION " OK", 5) == 0 &&
	    (response[5] == '\n' || response[5] == ' ')) {
		payload = response + 6;

		/* Handles a failed write all operation. */
		if (display && *payload != '\0' &&
		    write_all(STDOUT_FILENO, payload, strlen(payload)) != 0)

			/* Reports operation failure. */
			return 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching prefix. */
	if (strncmp(response, NETWORKD_PROTOCOL_VERSION " ERR ", 7) == 0)
		fprintf(stderr, "net: %s", response + 7);
	else
		fprintf(stderr, "net: malformed networkd response\n");

	/* Reports operation failure. */
	return 1;

unavailable:
	fprintf(stderr, "net: networkd is unavailable: %s\n", strerror(errno));

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);

	/* Reports operation failure. */
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
	     "  net boot                    apply boot network configuration");

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
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
