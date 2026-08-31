/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland netconf component.
 */

#include "userland/base/net/netconf.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NETCONF_LINE_MAX 512

enum section { SECTION_NONE, SECTION_INTERFACES, SECTION_ROUTES, SECTION_DNS };
enum subsection {
	SUBSECTION_NONE,
	SUBSECTION_IPV4,
	SUBSECTION_ADDRESSES,
	SUBSECTION_MEMBERS,
	SUBSECTION_DNS_SERVERS
};

struct parser {
	struct netconf *configuration;
	char *error;
	size_t error_capacity;
	unsigned line;
	enum section section;
	enum subsection subsection;
	struct netconf_interface *interface;
	struct netconf_address *address;
	struct netconf_route *route;
	unsigned top_seen;
	unsigned interface_seen;
	unsigned ipv4_seen;
	unsigned dns_seen;
	unsigned route_seen;
};

static int validate_graph(const struct netconf *configuration, size_t index, unsigned *visiting, unsigned *visited);
static struct netconf_interface *find_interface(const struct netconf *configuration, const char *name);
static int fail(struct parser *parser, const char *format, ...);
static int parse_content(struct parser *parser, unsigned indent, char *text);
static int parse_top(struct parser *parser, char *text);
static int split_mapping(struct parser *parser, char *text, char **key, char **value);
static int set_once(struct parser *parser, unsigned *bits, unsigned bit, const char *key);
static int unsigned_value(const char *text, unsigned minimum, unsigned maximum, unsigned *result);
static int new_interface(struct parser *parser, char *text);
static int name_valid(const char *name);
static int interface_property(struct parser *parser, char *text);
static int boolean_value(const char *text, int *result);
static int ipv4_property(struct parser *parser, char *text);
static int member_entry(struct parser *parser, char *text);
static int new_address(struct parser *parser, char *text);
static int ipv4_valid(const char *text);
static int address_property(struct parser *parser, char *text);
static int route_entry(struct parser *parser, char *text, int continuation);
static int ipv4_prefix_valid(const char *text);
static int copy_value(struct parser *parser, char *output, size_t capacity, const char *value, const char *what);
static int dns_property(struct parser *parser, char *text);
static int dns_entry(struct parser *parser, char *text);
static const char *type_name(enum netconf_interface_type type);
static const char *dns_name(enum netconf_dns_mode mode);

/*
 * Implements the netconf validate operation.
 */
int
netconf_validate(
	const struct netconf *configuration,
	char *error,
	size_t capacity)
{
	const struct netconf_interface *item;
	size_t index, address;
	unsigned visiting, visited;

	visiting = 0;
	visited = 0;
#define VALIDATE_ERROR(...)                                                    \
	do {                                                                   \
		if (error != NULL && capacity != 0)                            \
			(void)snprintf(error, capacity, __VA_ARGS__);          \
		errno = EINVAL;                                                \
		return -1;                                                     \
	} while (0)

	/* Handles the configuration condition. */
	if (configuration->version != 1)
		VALIDATE_ERROR("version must be 1");

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++) {
				item = &configuration->interfaces[index];

		/* Handles the item condition. */
		if (item->type == NETCONF_INTERFACE_UNSET || !item->enabled_set)
			VALIDATE_ERROR("interface %s lacks type or enabled",
				       item->name);

		/* Handles the item condition. */
		if (item->dhcp && item->address_count != 0)
			VALIDATE_ERROR(
			    "interface %s mixes DHCP and static addresses",
			    item->name);

		/* Handles the item condition. */
		if (item->dhcp_timeout_set && !item->dhcp)
			VALIDATE_ERROR("interface %s has timeout without DHCP",
				       item->name);

		/* Process each remaining element. */
		for (address = 0; address < item->address_count; address++)

			/* Handles the item condition. */
			if (item->addresses[address].prefix_length > 32)
				VALIDATE_ERROR(
				    "interface %s address lacks prefix",
				    item->name);

		/* Handles the item condition. */
		if (item->type == NETCONF_INTERFACE_VLAN) {
			/* Handles the item condition. */
			if (*item->parent == '\0' || !item->vlan_id_set)
				VALIDATE_ERROR("VLAN %s lacks parent or ID",
					       item->name);
		} else if (*item->parent != '\0' || item->vlan_id_set)
			VALIDATE_ERROR("non-VLAN %s has VLAN fields",
				       item->name);

		/* Handles the item condition. */
		if (item->type != NETCONF_INTERFACE_BRIDGE &&
		    item->member_count != 0)
			VALIDATE_ERROR("non-bridge %s has members", item->name);
	}

	/* Process each remaining element. */
	for (index = 0; index < configuration->route_count; index++)

		/* Handles the configuration condition. */
		if (*configuration->routes[index].destination == '\0' ||
		    *configuration->routes[index].gateway == '\0')
			VALIDATE_ERROR("route lacks destination or gateway");

	/* Handles the configuration condition. */
	if (configuration->dns_mode == NETCONF_DNS_UNSET)
		VALIDATE_ERROR("DNS mode is required");

	/* Handles the configuration condition. */
	if (configuration->dns_mode == NETCONF_DNS_STATIC &&
	    configuration->dns_count == 0)
		VALIDATE_ERROR("static DNS requires servers");

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++)

		/* Handles a failed validate graph operation. */
		if (validate_graph(configuration, index, &visiting, &visited) !=
		    0)
			VALIDATE_ERROR("invalid or cyclic interface topology");

	/* Reports successful completion. */
	return 0;
#undef VALIDATE_ERROR
}

/*
 * Implements the netconf parse operation.
 */
int
netconf_parse(
	FILE *stream,
	struct netconf *configuration,
	char *error,
	size_t capacity)
{
	int function_result;
	char *cursor, *end, *comment;
	unsigned indent;
	struct parser parser;
	char line[NETCONF_LINE_MAX];

	/* Handles the stream availability. */
	if (stream == NULL || configuration == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(configuration, 0, sizeof(*configuration));
	memset(&parser, 0, sizeof(parser));

	/* Process input until it is exhausted. */
	parser.configuration = configuration;
	parser.error = error;
	parser.error_capacity = capacity;
	while (fgets(line, sizeof(line), stream) != NULL) {

		indent = 0;
		parser.line++;

		/* Handles a failed strchr operation. */
		if (strchr(line, '\n') == NULL && !feof(stream)) {
			/* Obtains the fail result. */
			function_result = fail(&parser, "line is too long");

			/* Returns the computed result. */
			return function_result;
		}

		/* Continue while the operation condition remains true. */
		end = line + strlen(line);
		while (end > line &&
		       (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))

		/* Process each element required by the operation. */
			*--end = '\0';
		for (cursor = line; *cursor == ' '; cursor++)
			indent++;

		/* Handles a failed strchr operation. */
		if (*cursor == '\t' || strchr(cursor, '\t') != NULL) {
			/* Obtains the fail result. */
			function_result = fail(&parser, "tabs are forbidden");

			/* Returns the computed result. */
			return function_result;
		}
		comment = strchr(cursor, '#');

		/* Handles the comment availability. */
		if (comment != NULL &&
		    (comment == cursor || comment[-1] == ' ')) {
			/* Continue while the operation condition remains true. */
			*comment = '\0';
			end = comment;
			while (end > cursor && end[-1] == ' ')
				*--end = '\0';
		}

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			continue;

		/* Handles the indent condition. */
		if ((indent & 1U) != 0) {
			/* Obtains the fail result. */
			function_result = fail(&parser,
				    "indentation must use two-space units");

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed strchr operation. */
		if (strchr(cursor, '{') != NULL ||
		    strchr(cursor, '}') != NULL ||
		    strchr(cursor, '[') != NULL ||
		    strchr(cursor, ']') != NULL ||
		    strchr(cursor, '&') != NULL ||
		    strchr(cursor, '*') != NULL ||
		    strncmp(cursor, "---", 3) == 0) {
			/* Obtains the fail result. */
			function_result = fail(&parser, "unsupported YAML feature");

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed parse content operation. */
		if (parse_content(&parser, indent, cursor) != 0)
			return -1;
	}

	/* Handles an operation failure. */
	if (ferror(stream)) {
		errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the parser state. */
	if ((parser.top_seen & 3U) != 3U) {
		/* Obtains the fail result. */
		function_result = fail(&parser, "version and interfaces are required");

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the netconf validate result. */
	function_result = netconf_validate(configuration, error, capacity);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the netconf load operation.
 */
int
netconf_load(
	const char *path,
	struct netconf *configuration,
	char *error,
	size_t capacity)
{
	FILE *stream;
	int result;

	stream = fopen(path, "r");

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;
	result = netconf_parse(stream, configuration, error, capacity);

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0 && result == 0)
		return -1;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the netconf write operation.
 */
int
netconf_write(
	FILE *stream,
	const struct netconf *configuration)
{
	int function_result;
	const struct netconf_interface *item;
	size_t index, child;
	char error[128];

	/* Handles an operation failure. */
	if (stream == NULL || configuration == NULL ||
	    netconf_validate(configuration, error, sizeof(error)) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles a failed fprintf operation. */
	if (fprintf(stream, "version: 1\n\ninterfaces:\n") < 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++) {
				item = &configuration->interfaces[index];

		/* Handles a failed fprintf operation. */
		if (fprintf(stream, "  %s:\n    type: %s\n    enabled: %s\n",
			    item->name, type_name(item->type),
			    item->enabled ? "true" : "false") < 0)

			/* Reports operation failure. */
			return -1;

		/* Handles a failed fprintf operation. */
		if (*item->parent != '\0' &&
		    fprintf(stream, "    parent: %s\n", item->parent) < 0)

			/* Reports operation failure. */
			return -1;

		/* Handles a failed fprintf operation. */
		if (item->vlan_id_set &&
		    fprintf(stream, "    vlan-id: %u\n", item->vlan_id) < 0)

			/* Reports operation failure. */
			return -1;

		/* Handles the item condition. */
		if (item->member_count != 0) {
			/* Handles the end-of-file condition. */
			if (fputs("    members:\n", stream) == EOF)
				return -1;

			/* Process each remaining element. */
			for (child = 0; child < item->member_count; child++)

				/* Handles a failed fprintf operation. */
				if (fprintf(stream, "      - %s\n",
					    item->members[child]) < 0)

					/* Reports operation failure. */
					return -1;
		}

		/* Handles the item condition. */
		if (item->dhcp_set || item->address_count != 0) {
			/* Handles the end-of-file condition. */
			if (fputs("    ipv4:\n", stream) == EOF)
				return -1;

			/* Handles a failed fprintf operation. */
			if (item->dhcp_set &&
			    fprintf(stream, "      dhcp: %s\n",
				    item->dhcp ? "true" : "false") < 0)

				/* Reports operation failure. */
				return -1;

			/* Handles a failed fprintf operation. */
			if (item->dhcp_timeout_set &&
			    fprintf(stream, "      dhcp-timeout: %u\n",
				    item->dhcp_timeout) < 0)

				/* Reports operation failure. */
				return -1;

			/* Handles the item condition. */
			if (item->address_count != 0) {
				/* Handles the end-of-file condition. */
				if (fputs("      addresses:\n", stream) == EOF)
					return -1;

				/* Process each remaining element. */
				for (child = 0; child < item->address_count;
				     child++)

					/* Handles a failed fprintf operation. */
					if (fprintf(
						stream,
						"        - address: %s\n"
						"          prefix-length: %u\n",
						item->addresses[child].address,
						item->addresses[child]
						    .prefix_length) < 0)

						/* Reports operation failure. */
						return -1;
			}
		}

		/* Handles the end-of-file condition. */
		if (fputc('\n', stream) == EOF)
			return -1;
	}

	/* Handles the configuration condition. */
	if (configuration->route_count != 0) {
		/* Handles the end-of-file condition. */
		if (fputs("routes:\n", stream) == EOF)
			return -1;

		/* Process each remaining element. */
		for (index = 0; index < configuration->route_count; index++)

			/* Handles a failed fprintf operation. */
			if (fprintf(stream,
				    "  - destination: %s\n    gateway: %s\n",
				    configuration->routes[index].destination,
				    configuration->routes[index].gateway) < 0)

				/* Reports operation failure. */
				return -1;

		/* Handles the end-of-file condition. */
		if (fputc('\n', stream) == EOF)
			return -1;
	}

	/* Handles a failed fprintf operation. */
	if (fprintf(stream, "dns:\n  mode: %s\n",
		    dns_name(configuration->dns_mode)) < 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the configuration condition. */
	if (configuration->dns_count != 0) {
		/* Handles the end-of-file condition. */
		if (fputs("  servers:\n", stream) == EOF)
			return -1;

		/* Process each remaining element. */
		for (index = 0; index < configuration->dns_count; index++)

			/* Handles a failed fprintf operation. */
			if (fprintf(stream, "    - %s\n",
				    configuration->dns_servers[index]) < 0)

				/* Reports operation failure. */
				return -1;
	}

	/* Computes the function result. */
	function_result = ferror(stream) ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the netconf save atomic operation.
 */
int
netconf_save_atomic(
	const char *path,
	const struct netconf *configuration,
	char *error,
	size_t capacity)
{
	char temporary[512], validation[160];
	FILE *stream;
	int descriptor, saved_errno, temporary_created;

	stream = NULL;
	descriptor = -1;
	temporary_created = 0;

	/* Handles a failed netconf validate operation. */
	if (path == NULL || configuration == NULL ||
	    netconf_validate(configuration, validation, sizeof(validation)) !=
		0) {
		/* Handles an operation failure. */
		if (error != NULL && capacity != 0)
			(void)snprintf(error, capacity, "%s",
				       path == NULL || configuration == NULL
					   ? "invalid save request"
					   : validation);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;

		/* Handles an operation failure. */
		if (error != NULL && capacity != 0)
			(void)snprintf(error, capacity, "path is too long");

		/* Reports operation failure. */
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		goto failed;
	temporary_created = 1;
	stream = fdopen(descriptor, "w");

	/* Handles the stream availability. */
	if (stream == NULL)
		goto failed;
	descriptor = -1;

	/* Handles a failed netconf write operation. */
	if (netconf_write(stream, configuration) != 0 || fflush(stream) != 0 ||
	    fsync(fileno(stream)) != 0)
		goto failed;

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0) {
		stream = NULL;
		goto failed;
	}
	stream = NULL;

	/* Handles a failed rename operation. */
	if (rename(temporary, path) != 0)
		goto failed;

	/* Handles an operation failure. */
	if (error != NULL && capacity != 0)
		error[0] = '\0';

	/* Reports successful completion. */
	return 0;

failed:
	saved_errno = errno;

	/* Handles the stream availability. */
	if (stream != NULL)
		(void)fclose(stream);
	else if (descriptor >= 0)
		(void)close(descriptor);

	/* Handles the temporary created condition. */
	if (temporary_created)
		(void)unlink(temporary);

	/* Handles an operation failure. */
	if (error != NULL && capacity != 0)
		(void)snprintf(error, capacity, "%s", strerror(saved_errno));
	errno = saved_errno;

	/* Reports operation failure. */
	return -1;
}

/* Supports the validate graph operation. */
static int
validate_graph(
	const struct netconf *configuration,
	size_t index,
	unsigned *visiting,
	unsigned *visited)
{
	struct netconf_interface *target_local;
	struct netconf_interface *target_local1;
	const struct netconf_interface *interface =
	    &configuration->interfaces[index];
	size_t child, member;
	unsigned bit = 1U << index;

	/* Handles the visiting condition. */
	if ((*visiting & bit) != 0)
		return -1;

	/* Handles the visited condition. */
	if ((*visited & bit) != 0)
		return 0;
	*visiting |= bit;
	/* Handles the interface condition. */
	if (*interface->parent != '\0') {
				target_local = find_interface(configuration, interface->parent);

		/* Handles the target local availability. */
		if (target_local == NULL)
			return -1;
		child = (size_t)(target_local - configuration->interfaces);

		/* Handles a failed validate graph operation. */
		if (validate_graph(configuration, child, visiting, visited) !=
		    0)

			/* Reports operation failure. */
			return -1;
	}

	/* Process each remaining element. */
	for (member = 0; member < interface->member_count; member++) {
				target_local1 = find_interface(configuration, interface->members[member]);

		/* Handles the target local1 availability. */
		if (target_local1 == NULL)
			return -1;
		child = (size_t)(target_local1 - configuration->interfaces);

		/* Handles a failed validate graph operation. */
		if (validate_graph(configuration, child, visiting, visited) !=
		    0)

			/* Reports operation failure. */
			return -1;
	}
	*visiting &= ~bit;
	*visited |= bit;
	/* Reports successful completion. */
	return 0;
}

/* Supports the find interface operation. */
static struct netconf_interface *
find_interface(
	const struct netconf *configuration,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < configuration->interface_count; index++)

		/* Selects the matching value. */
		if (strcmp(configuration->interfaces[index].name, name) == 0)
			return (struct netconf_interface *)&configuration
			    ->interfaces[index];

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the fail operation. */
static int
fail(
	struct parser *parser,
	const char *format,
	...)
{
	int used;

	va_list arguments;

	/* Handles an operation failure. */
	if (parser->error != NULL && parser->error_capacity != 0) {
				used = snprintf(parser->error, parser->error_capacity,
				    "line %u: ", parser->line);

		/* Handles an operation failure. */
		if (used >= 0 && (size_t)used < parser->error_capacity) {
			va_start(arguments, format);
			(void)vsnprintf(parser->error + used,
					parser->error_capacity - (size_t)used,
					format, arguments);
			va_end(arguments);
		}
	}
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the parse content operation. */
static int
parse_content(
	struct parser *parser,
	unsigned indent,
	char *text)
{
	int function_result;

	/* Handles the indent condition. */
	if (indent == 0) {
		/* Obtains the parse top result. */
		function_result = parse_top(parser, text);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the parser state. */
	if (parser->section == SECTION_INTERFACES) {
		/* Handles the indent condition. */
		if (indent == 2) {
			/* Obtains the new interface result. */
			function_result = new_interface(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 4) {
			/* Obtains the interface property result. */
			function_result = interface_property(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 6 && parser->subsection == SUBSECTION_IPV4) {
			/* Obtains the ipv4 property result. */
			function_result = ipv4_property(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 6 && parser->subsection == SUBSECTION_MEMBERS) {
			/* Obtains the member entry result. */
			function_result = member_entry(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 8) {
			/* Obtains the new address result. */
			function_result = new_address(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 10) {
			/* Obtains the address property result. */
			function_result = address_property(parser, text);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Checks the parser state. */
	if (parser->section == SECTION_ROUTES) {
		/* Handles the indent condition. */
		if (indent == 2) {
			/* Obtains the route entry result. */
			function_result = route_entry(parser, text, 0);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 4) {
			/* Obtains the route entry result. */
			function_result = route_entry(parser, text, 1);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Checks the parser state. */
	if (parser->section == SECTION_DNS) {
		/* Handles the indent condition. */
		if (indent == 2) {
			/* Obtains the dns property result. */
			function_result = dns_property(parser, text);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the indent condition. */
		if (indent == 4) {
			/* Obtains the dns entry result. */
			function_result = dns_entry(parser, text);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Obtains the fail result. */
	function_result = fail(parser, "unexpected indentation or nesting");

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse top operation. */
static int
parse_top(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;

	/* Handles a failed split mapping operation. */
	if (split_mapping(parser, text, &key, &value) != 0)
		return -1;
	parser->interface = NULL;
	parser->address = NULL;
	parser->route = NULL;
	parser->subsection = SUBSECTION_NONE;

	/* Selects the matching value. */
	if (strcmp(key, "version") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->top_seen, 1U, key) != 0 ||
		    unsigned_value(value, 1, 1,
				   &parser->configuration->version) != 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "version must be 1");

			/* Returns the computed result. */
			return function_result;
		}
		parser->section = SECTION_NONE;

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the current value. */
	if (*value != '\0') {
		/* Obtains the fail result. */
		function_result = fail(parser, "top-level section %s cannot have a value",
			    key);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(key, "interfaces") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->top_seen, 2U, key) != 0)
			return -1;
		parser->section = SECTION_INTERFACES;
	} else if (strcmp(key, "routes") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->top_seen, 4U, key) != 0)
			return -1;
		parser->section = SECTION_ROUTES;
	} else if (strcmp(key, "dns") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->top_seen, 8U, key) != 0)
			return -1;
		parser->section = SECTION_DNS;
	} else {
		/* Obtains the fail result. */
		function_result = fail(parser, "unknown top-level key %s", key);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the split mapping operation. */
static int
split_mapping(
	struct parser *parser,
	char *text,
	char **key,
	char **value)
{
	int function_result;
	char *colon;
	char *end;

	colon = strchr(text, ':');

	/* Handles the colon availability. */
	if (colon == NULL || (colon[1] != '\0' && colon[1] != ' ')) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected key: value");

		/* Returns the computed result. */
		return function_result;
	}

	/* Continue while the operation condition remains true. */
	*colon = '\0';
	end = colon;
	while (end > text && end[-1] == ' ')
		*--end = '\0';
	/* Validates the current text. */
	if (*text == '\0') {
		/* Obtains the fail result. */
		function_result = fail(parser, "empty key");

		/* Returns the computed result. */
		return function_result;
	}
	*key = text;
	*value = colon + 1;
	/* Validates the current value. */
	if (**value == ' ')
		(*value)++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the set once operation. */
static int
set_once(
	struct parser *parser,
	unsigned *bits,
	unsigned bit,
	const char *key)
{
	int function_result;

	/* Handles the bits condition. */
	if ((*bits & bit) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "duplicate key %s", key);

		/* Returns the computed result. */
		return function_result;
	}
	*bits |= bit;
	/* Reports successful completion. */
	return 0;
}

/* Supports the unsigned value operation. */
static int
unsigned_value(
	const char *text,
	unsigned minimum,
	unsigned maximum,
	unsigned *result)
{
	char *end;
	unsigned long value;

	/* Handles a failed isdigit operation. */
	if (*text == '\0' || !isdigit((unsigned char)*text))
		return -1;
	value = strtoul(text, &end, 10);

	/* Checks the current endpoint. */
	if (*end != '\0' || value < minimum || value > maximum)
		return -1;
	*result = (unsigned)value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the new interface operation. */
static int
new_interface(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;
	size_t index;

	/* Handles a failed split mapping operation. */
	if (parser->section != SECTION_INTERFACES ||
	    split_mapping(parser, text, &key, &value) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected an interface entry");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed name valid operation. */
	if (*value != '\0' || !name_valid(key)) {
		/* Obtains the fail result. */
		function_result = fail(parser, "invalid interface name");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (index = 0; index < parser->configuration->interface_count; index++)

		/* Selects the matching value. */
		if (strcmp(parser->configuration->interfaces[index].name,
			   key) == 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "duplicate interface %s", key);

			/* Returns the computed result. */
			return function_result;
		}

	/* Checks the parser state. */
	if (parser->configuration->interface_count == NETCONF_MAX_INTERFACES) {
		/* Obtains the fail result. */
		function_result = fail(parser, "too many interfaces");

		/* Returns the computed result. */
		return function_result;
	}

	parser->interface =
	    &parser->configuration
		 ->interfaces[parser->configuration->interface_count++];
	strcpy(parser->interface->name, key);
	parser->interface_seen = 0;
	parser->ipv4_seen = 0;
	parser->subsection = SUBSECTION_NONE;

	/* Reports successful completion. */
	return 0;
}

/* Supports the name valid operation. */
static int
name_valid(
	const char *name)
{
	size_t index, length;

	length = strlen(name);

	/* Handles a failed isalnum operation. */
	if (length == 0 || length > NETCONF_NAME_MAX ||
	    !isalnum((unsigned char)name[0]))

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	for (index = 1; index < length; index++)

		/* Handles a failed isalnum operation. */
		if (!isalnum((unsigned char)name[index]) &&
		    name[index] != '_' && name[index] != '-')

			/* Reports successful completion. */
			return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the interface property operation. */
static int
interface_property(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;
	struct netconf_interface *interface;

	interface = parser->interface;

	/* Handles a failed split mapping operation. */
	if (interface == NULL || split_mapping(parser, text, &key, &value) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "interface property without interface");

		/* Returns the computed result. */
		return function_result;
	}

	parser->address = NULL;

	/* Selects the matching value. */
	if (strcmp(key, "type") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 1U, key) != 0)
			return -1;

		/* Selects the matching value. */
		if (strcmp(value, "loopback") == 0)
			interface->type = NETCONF_INTERFACE_LOOPBACK;
		else if (strcmp(value, "ethernet") == 0)
			interface->type = NETCONF_INTERFACE_ETHERNET;
		else if (strcmp(value, "vlan") == 0)
			interface->type = NETCONF_INTERFACE_VLAN;
		else if (strcmp(value, "bridge") == 0)
			interface->type = NETCONF_INTERFACE_BRIDGE;
		else {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid interface type");

			/* Returns the computed result. */
			return function_result;
		}
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "enabled") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 2U, key) != 0 ||
		    boolean_value(value, &interface->enabled) != 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "enabled must be true or false");

			/* Returns the computed result. */
			return function_result;
		}
		interface->enabled_set = 1;
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "parent") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 4U, key) != 0 ||
		    !name_valid(value)) {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid VLAN parent");

			/* Returns the computed result. */
			return function_result;
		}
		strcpy(interface->parent, value);
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "vlan-id") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 8U, key) != 0 ||
		    unsigned_value(value, 1, 4094, &interface->vlan_id) != 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "VLAN ID must be 1 through 4094");

			/* Returns the computed result. */
			return function_result;
		}
		interface->vlan_id_set = 1;
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "ipv4") == 0 && *value == '\0') {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 16U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_IPV4;
	} else if (strcmp(key, "members") == 0 && *value == '\0') {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->interface_seen, 32U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_MEMBERS;
	} else {
		/* Obtains the fail result. */
		function_result = fail(parser, "unknown interface key %s", key);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the boolean value operation. */
static int
boolean_value(
	const char *text,
	int *result)
{
	/* Selects the matching value. */
	if (strcmp(text, "true") == 0)
		*result = 1;
	else if (strcmp(text, "false") == 0)
		*result = 0;
	else

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the ipv4 property operation. */
static int
ipv4_property(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;
	struct netconf_interface *interface;

	interface = parser->interface;

	/* Handles a failed split mapping operation. */
	if (interface == NULL || parser->subsection != SUBSECTION_IPV4 ||
	    split_mapping(parser, text, &key, &value) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected an IPv4 property");

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(key, "dhcp") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->ipv4_seen, 1U, key) != 0 ||
		    boolean_value(value, &interface->dhcp) != 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "dhcp must be true or false");

			/* Returns the computed result. */
			return function_result;
		}
		interface->dhcp_set = 1;
	} else if (strcmp(key, "dhcp-timeout") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->ipv4_seen, 2U, key) != 0 ||
		    unsigned_value(value, 1, 3600, &interface->dhcp_timeout) !=
			0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid DHCP timeout");

			/* Returns the computed result. */
			return function_result;
		}
		interface->dhcp_timeout_set = 1;
	} else if (strcmp(key, "addresses") == 0 && *value == '\0') {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->ipv4_seen, 4U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_ADDRESSES;
	} else {
		/* Obtains the fail result. */
		function_result = fail(parser, "unknown IPv4 key %s", key);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the member entry operation. */
static int
member_entry(
	struct parser *parser,
	char *text)
{
	int function_result;
	struct netconf_interface *interface;
	const char *name;
	size_t index;

	interface = parser->interface;

	/* Handles a failed name valid operation. */
	if (interface == NULL || parser->subsection != SUBSECTION_MEMBERS ||
	    text[0] != '-' || text[1] != ' ' || !name_valid(name = text + 2)) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected - interface-name");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (index = 0; index < interface->member_count; index++)

		/* Selects the matching value. */
		if (strcmp(interface->members[index], name) == 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "duplicate bridge member %s", name);

			/* Returns the computed result. */
			return function_result;
		}

	/* Handles the interface condition. */
	if (interface->member_count == NETCONF_MAX_MEMBERS) {
		/* Obtains the fail result. */
		function_result = fail(parser, "too many bridge members");

		/* Returns the computed result. */
		return function_result;
	}

	strcpy(interface->members[interface->member_count++], name);

	/* Reports successful completion. */
	return 0;
}

/* Supports the new address operation. */
static int
new_address(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;

	/* Handles a failed split mapping operation. */
	if (parser->interface == NULL ||
	    parser->subsection != SUBSECTION_ADDRESSES || text[0] != '-' ||
	    text[1] != ' ' ||
	    split_mapping(parser, text + 2, &key, &value) != 0 ||
	    strcmp(key, "address") != 0 || !ipv4_valid(value)) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected - address: IPv4");

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the parser state. */
	if (parser->interface->address_count == NETCONF_MAX_ADDRESSES) {
		/* Obtains the fail result. */
		function_result = fail(parser, "too many addresses");

		/* Returns the computed result. */
		return function_result;
	}

	parser->address =
	    &parser->interface->addresses[parser->interface->address_count++];
	strcpy(parser->address->address, value);
	parser->address->prefix_length = 33;

	/* Reports successful completion. */
	return 0;
}

/* Supports the ipv4 valid operation. */
static int
ipv4_valid(
	const char *text)
{
	unsigned value, digits;
	unsigned parts;
	const char *cursor;

	/* Continue while the operation condition remains true. */
	parts = 0;
	cursor = text;
	while (*cursor != '\0') {
		/* Continue while the operation condition remains true. */
		value = 0;
		digits = 0;
		while (isdigit((unsigned char)*cursor)) {
			value = value * 10U + (unsigned)(*cursor++ - '0');
			digits++;

			/* Handles the digits condition. */
			if (digits > 3 || value > 255)
				return 0;
		}

		/* Handles the digits condition. */
		if (digits == 0 || ++parts > 4)
			return 0;

		/* Handles the parts condition. */
		if (parts == 4)
			return *cursor == '\0';

		/* Checks the current cursor position. */
		if (*cursor++ != '.')
			return 0;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the address property operation. */
static int
address_property(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;

	/* Handles a failed split mapping operation. */
	if (parser->address == NULL ||
	    split_mapping(parser, text, &key, &value) != 0 ||
	    strcmp(key, "prefix-length") != 0 ||
	    parser->address->prefix_length != 33 ||
	    unsigned_value(value, 0, 32, &parser->address->prefix_length) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected one prefix-length: 0..32");

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the route entry operation. */
static int
route_entry(
	struct parser *parser,
	char *text,
	int continuation)
{
	int function_result;
	char *key, *value;

	/* Handles the continuation condition. */
	if (!continuation) {
		/* Checks the parser state. */
		if (parser->section != SECTION_ROUTES || text[0] != '-' ||
		    text[1] != ' ' ||
		    parser->configuration->route_count == NETCONF_MAX_ROUTES) {
			/* Obtains the fail result. */
			function_result = fail(parser, "expected a route entry");

			/* Returns the computed result. */
			return function_result;
		}
		parser->route =
		    &parser->configuration
			 ->routes[parser->configuration->route_count++];
		parser->route_seen = 0;
		text += 2;
	} else if (parser->route == NULL) {
		/* Obtains the fail result. */
		function_result = fail(parser, "route property without route");

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed split mapping operation. */
	if (split_mapping(parser, text, &key, &value) != 0)
		return -1;

	/* Selects the matching value. */
	if (strcmp(key, "destination") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->route_seen, 1U, key) != 0 ||
		    (strcmp(value, "default") != 0 &&
		     !ipv4_prefix_valid(value))) {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid route destination");

			/* Returns the computed result. */
			return function_result;
		}

		/* Obtains the copy value result. */
		function_result = copy_value(parser, parser->route->destination,
				  sizeof(parser->route->destination), value,
				  "route destination");

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(key, "gateway") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->route_seen, 2U, key) != 0 ||
		    !ipv4_valid(value)) {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid route gateway");

			/* Returns the computed result. */
			return function_result;
		}
		strcpy(parser->route->gateway, value);

		/* Reports successful completion. */
		return 0;
	}

	/* Obtains the fail result. */
	function_result = fail(parser, "unknown route key %s", key);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ipv4 prefix valid operation. */
static int
ipv4_prefix_valid(
	const char *text)
{
	int function_result;
	char address[NETCONF_IPV4_MAX + 1];
	const char *slash;
	unsigned prefix;
	size_t length;

	slash = strchr(text, '/');

	/* Handles a failed strchr operation. */
	if (slash == NULL || strchr(slash + 1, '/') != NULL)
		return 0;
	length = (size_t)(slash - text);

	/* Checks the current data length. */
	if (length == 0 || length >= sizeof(address))
		return 0;
	memcpy(address, text, length);
	address[length] = '\0';

	/* Computes the function result. */
	function_result = ipv4_valid(address) &&
	       unsigned_value(slash + 1, 0, 32, &prefix) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the copy value operation. */
static int
copy_value(
	struct parser *parser,
	char *output,
	size_t capacity,
	const char *value,
	const char *what)
{
	int function_result;

	/* Handles a failed strlen operation. */
	if (*value == '\0' || strlen(value) >= capacity) {
		/* Obtains the fail result. */
		function_result = fail(parser, "invalid %s", what);

		/* Returns the computed result. */
		return function_result;
	}

	strcpy(output, value);

	/* Reports successful completion. */
	return 0;
}

/* Supports the dns property operation. */
static int
dns_property(
	struct parser *parser,
	char *text)
{
	int function_result;
	char *key, *value;

	/* Handles a failed split mapping operation. */
	if (parser->section != SECTION_DNS ||
	    split_mapping(parser, text, &key, &value) != 0) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected DNS property");

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(key, "mode") == 0) {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->dns_seen, 1U, key) != 0)
			return -1;

		/* Selects the matching value. */
		if (strcmp(value, "dhcp") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_DHCP;
		else if (strcmp(value, "static") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_STATIC;
		else if (strcmp(value, "merge") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_MERGE;
		else {
			/* Obtains the fail result. */
			function_result = fail(parser, "invalid DNS mode");

			/* Returns the computed result. */
			return function_result;
		}
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "servers") == 0 && *value == '\0') {
		/* Handles a failed set once operation. */
		if (set_once(parser, &parser->dns_seen, 2U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_DNS_SERVERS;
	} else {
		/* Obtains the fail result. */
		function_result = fail(parser, "unknown DNS key %s", key);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the dns entry operation. */
static int
dns_entry(
	struct parser *parser,
	char *text)
{
	int function_result;
	const char *address;
	size_t index;

	/* Handles a failed ipv4 valid operation. */
	if (parser->section != SECTION_DNS ||
	    parser->subsection != SUBSECTION_DNS_SERVERS || text[0] != '-' ||
	    text[1] != ' ' || !ipv4_valid(address = text + 2)) {
		/* Obtains the fail result. */
		function_result = fail(parser, "expected - DNS-address");

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (index = 0; index < parser->configuration->dns_count; index++)

		/* Selects the matching value. */
		if (strcmp(parser->configuration->dns_servers[index],
			   address) == 0) {
			/* Obtains the fail result. */
			function_result = fail(parser, "duplicate DNS server %s", address);

			/* Returns the computed result. */
			return function_result;
		}

	/* Checks the parser state. */
	if (parser->configuration->dns_count == NETCONF_MAX_DNS) {
		/* Obtains the fail result. */
		function_result = fail(parser, "too many DNS servers");

		/* Returns the computed result. */
		return function_result;
	}

	strcpy(parser->configuration
		   ->dns_servers[parser->configuration->dns_count++],
	       address);

	/* Reports successful completion. */
	return 0;
}

/* Supports the type name operation. */
static const char *
type_name(
	enum netconf_interface_type type)
{
	static const char *const names[] = {"unset", "loopback", "ethernet",
					    "vlan", "bridge"};

	/* Returns the computed result. */
	return names[type];
}

/* Supports the dns name operation. */
static const char *
dns_name(
	enum netconf_dns_mode mode)
{
	static const char *const names[] = {"unset", "dhcp", "static", "merge"};

	/* Returns the computed result. */
	return names[mode];
}
