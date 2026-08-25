/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
fail(struct parser *parser, const char *format, ...)
{
	va_list arguments;
	if (parser->error != NULL && parser->error_capacity != 0) {
		int used = snprintf(parser->error, parser->error_capacity,
				    "line %u: ", parser->line);
		if (used >= 0 && (size_t)used < parser->error_capacity) {
			va_start(arguments, format);
			(void)vsnprintf(parser->error + used,
					parser->error_capacity - (size_t)used,
					format, arguments);
			va_end(arguments);
		}
	}
	errno = EINVAL;
	return -1;
}

static int
copy_value(struct parser *parser, char *output, size_t capacity,
	   const char *value, const char *what)
{
	if (*value == '\0' || strlen(value) >= capacity)
		return fail(parser, "invalid %s", what);
	strcpy(output, value);
	return 0;
}

static int
name_valid(const char *name)
{
	size_t index, length = strlen(name);
	if (length == 0 || length > NETCONF_NAME_MAX ||
	    !isalnum((unsigned char)name[0]))
		return 0;
	for (index = 1; index < length; index++)
		if (!isalnum((unsigned char)name[index]) &&
		    name[index] != '_' && name[index] != '-')
			return 0;
	return 1;
}

static int
unsigned_value(const char *text, unsigned minimum, unsigned maximum,
	       unsigned *result)
{
	char *end;
	unsigned long value;
	if (*text == '\0' || !isdigit((unsigned char)*text))
		return -1;
	value = strtoul(text, &end, 10);
	if (*end != '\0' || value < minimum || value > maximum)
		return -1;
	*result = (unsigned)value;
	return 0;
}

static int
ipv4_valid(const char *text)
{
	unsigned parts = 0;
	const char *cursor = text;
	while (*cursor != '\0') {
		unsigned value = 0, digits = 0;
		while (isdigit((unsigned char)*cursor)) {
			value = value * 10U + (unsigned)(*cursor++ - '0');
			digits++;
			if (digits > 3 || value > 255)
				return 0;
		}
		if (digits == 0 || ++parts > 4)
			return 0;
		if (parts == 4)
			return *cursor == '\0';
		if (*cursor++ != '.')
			return 0;
	}
	return 0;
}

static int
ipv4_prefix_valid(const char *text)
{
	char address[NETCONF_IPV4_MAX + 1];
	const char *slash = strchr(text, '/');
	unsigned prefix;
	size_t length;
	if (slash == NULL || strchr(slash + 1, '/') != NULL)
		return 0;
	length = (size_t)(slash - text);
	if (length == 0 || length >= sizeof(address))
		return 0;
	memcpy(address, text, length);
	address[length] = '\0';
	return ipv4_valid(address) &&
	       unsigned_value(slash + 1, 0, 32, &prefix) == 0;
}

static int
boolean_value(const char *text, int *result)
{
	if (strcmp(text, "true") == 0)
		*result = 1;
	else if (strcmp(text, "false") == 0)
		*result = 0;
	else
		return -1;
	return 0;
}

static int
split_mapping(struct parser *parser, char *text, char **key, char **value)
{
	char *colon = strchr(text, ':');
	char *end;
	if (colon == NULL || (colon[1] != '\0' && colon[1] != ' '))
		return fail(parser, "expected key: value");
	*colon = '\0';
	end = colon;
	while (end > text && end[-1] == ' ')
		*--end = '\0';
	if (*text == '\0')
		return fail(parser, "empty key");
	*key = text;
	*value = colon + 1;
	if (**value == ' ')
		(*value)++;
	return 0;
}

static int
set_once(struct parser *parser, unsigned *bits, unsigned bit, const char *key)
{
	if ((*bits & bit) != 0)
		return fail(parser, "duplicate key %s", key);
	*bits |= bit;
	return 0;
}

static int
parse_top(struct parser *parser, char *text)
{
	char *key, *value;
	if (split_mapping(parser, text, &key, &value) != 0)
		return -1;
	parser->interface = NULL;
	parser->address = NULL;
	parser->route = NULL;
	parser->subsection = SUBSECTION_NONE;
	if (strcmp(key, "version") == 0) {
		if (set_once(parser, &parser->top_seen, 1U, key) != 0 ||
		    unsigned_value(value, 1, 1,
				   &parser->configuration->version) != 0)
			return fail(parser, "version must be 1");
		parser->section = SECTION_NONE;
		return 0;
	}
	if (*value != '\0')
		return fail(parser, "top-level section %s cannot have a value",
			    key);
	if (strcmp(key, "interfaces") == 0) {
		if (set_once(parser, &parser->top_seen, 2U, key) != 0)
			return -1;
		parser->section = SECTION_INTERFACES;
	} else if (strcmp(key, "routes") == 0) {
		if (set_once(parser, &parser->top_seen, 4U, key) != 0)
			return -1;
		parser->section = SECTION_ROUTES;
	} else if (strcmp(key, "dns") == 0) {
		if (set_once(parser, &parser->top_seen, 8U, key) != 0)
			return -1;
		parser->section = SECTION_DNS;
	} else
		return fail(parser, "unknown top-level key %s", key);
	return 0;
}

static int
new_interface(struct parser *parser, char *text)
{
	char *key, *value;
	size_t index;
	if (parser->section != SECTION_INTERFACES ||
	    split_mapping(parser, text, &key, &value) != 0)
		return fail(parser, "expected an interface entry");
	if (*value != '\0' || !name_valid(key))
		return fail(parser, "invalid interface name");
	for (index = 0; index < parser->configuration->interface_count; index++)
		if (strcmp(parser->configuration->interfaces[index].name,
			   key) == 0)
			return fail(parser, "duplicate interface %s", key);
	if (parser->configuration->interface_count == NETCONF_MAX_INTERFACES)
		return fail(parser, "too many interfaces");
	parser->interface =
	    &parser->configuration
		 ->interfaces[parser->configuration->interface_count++];
	strcpy(parser->interface->name, key);
	parser->interface_seen = 0;
	parser->ipv4_seen = 0;
	parser->subsection = SUBSECTION_NONE;
	return 0;
}

static int
interface_property(struct parser *parser, char *text)
{
	char *key, *value;
	struct netconf_interface *interface = parser->interface;
	if (interface == NULL || split_mapping(parser, text, &key, &value) != 0)
		return fail(parser, "interface property without interface");
	parser->address = NULL;
	if (strcmp(key, "type") == 0) {
		if (set_once(parser, &parser->interface_seen, 1U, key) != 0)
			return -1;
		if (strcmp(value, "loopback") == 0)
			interface->type = NETCONF_INTERFACE_LOOPBACK;
		else if (strcmp(value, "ethernet") == 0)
			interface->type = NETCONF_INTERFACE_ETHERNET;
		else if (strcmp(value, "vlan") == 0)
			interface->type = NETCONF_INTERFACE_VLAN;
		else if (strcmp(value, "bridge") == 0)
			interface->type = NETCONF_INTERFACE_BRIDGE;
		else
			return fail(parser, "invalid interface type");
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "enabled") == 0) {
		if (set_once(parser, &parser->interface_seen, 2U, key) != 0 ||
		    boolean_value(value, &interface->enabled) != 0)
			return fail(parser, "enabled must be true or false");
		interface->enabled_set = 1;
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "parent") == 0) {
		if (set_once(parser, &parser->interface_seen, 4U, key) != 0 ||
		    !name_valid(value))
			return fail(parser, "invalid VLAN parent");
		strcpy(interface->parent, value);
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "vlan-id") == 0) {
		if (set_once(parser, &parser->interface_seen, 8U, key) != 0 ||
		    unsigned_value(value, 1, 4094, &interface->vlan_id) != 0)
			return fail(parser, "VLAN ID must be 1 through 4094");
		interface->vlan_id_set = 1;
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "ipv4") == 0 && *value == '\0') {
		if (set_once(parser, &parser->interface_seen, 16U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_IPV4;
	} else if (strcmp(key, "members") == 0 && *value == '\0') {
		if (set_once(parser, &parser->interface_seen, 32U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_MEMBERS;
	} else
		return fail(parser, "unknown interface key %s", key);
	return 0;
}

static int
ipv4_property(struct parser *parser, char *text)
{
	char *key, *value;
	struct netconf_interface *interface = parser->interface;
	if (interface == NULL || parser->subsection != SUBSECTION_IPV4 ||
	    split_mapping(parser, text, &key, &value) != 0)
		return fail(parser, "expected an IPv4 property");
	if (strcmp(key, "dhcp") == 0) {
		if (set_once(parser, &parser->ipv4_seen, 1U, key) != 0 ||
		    boolean_value(value, &interface->dhcp) != 0)
			return fail(parser, "dhcp must be true or false");
		interface->dhcp_set = 1;
	} else if (strcmp(key, "dhcp-timeout") == 0) {
		if (set_once(parser, &parser->ipv4_seen, 2U, key) != 0 ||
		    unsigned_value(value, 1, 3600, &interface->dhcp_timeout) !=
			0)
			return fail(parser, "invalid DHCP timeout");
		interface->dhcp_timeout_set = 1;
	} else if (strcmp(key, "addresses") == 0 && *value == '\0') {
		if (set_once(parser, &parser->ipv4_seen, 4U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_ADDRESSES;
	} else
		return fail(parser, "unknown IPv4 key %s", key);
	return 0;
}

static int
new_address(struct parser *parser, char *text)
{
	char *key, *value;
	if (parser->interface == NULL ||
	    parser->subsection != SUBSECTION_ADDRESSES || text[0] != '-' ||
	    text[1] != ' ' ||
	    split_mapping(parser, text + 2, &key, &value) != 0 ||
	    strcmp(key, "address") != 0 || !ipv4_valid(value))
		return fail(parser, "expected - address: IPv4");
	if (parser->interface->address_count == NETCONF_MAX_ADDRESSES)
		return fail(parser, "too many addresses");
	parser->address =
	    &parser->interface->addresses[parser->interface->address_count++];
	strcpy(parser->address->address, value);
	parser->address->prefix_length = 33;
	return 0;
}

static int
address_property(struct parser *parser, char *text)
{
	char *key, *value;
	if (parser->address == NULL ||
	    split_mapping(parser, text, &key, &value) != 0 ||
	    strcmp(key, "prefix-length") != 0 ||
	    parser->address->prefix_length != 33 ||
	    unsigned_value(value, 0, 32, &parser->address->prefix_length) != 0)
		return fail(parser, "expected one prefix-length: 0..32");
	return 0;
}

static int
member_entry(struct parser *parser, char *text)
{
	struct netconf_interface *interface = parser->interface;
	const char *name;
	size_t index;
	if (interface == NULL || parser->subsection != SUBSECTION_MEMBERS ||
	    text[0] != '-' || text[1] != ' ' || !name_valid(name = text + 2))
		return fail(parser, "expected - interface-name");
	for (index = 0; index < interface->member_count; index++)
		if (strcmp(interface->members[index], name) == 0)
			return fail(parser, "duplicate bridge member %s", name);
	if (interface->member_count == NETCONF_MAX_MEMBERS)
		return fail(parser, "too many bridge members");
	strcpy(interface->members[interface->member_count++], name);
	return 0;
}

static int
route_entry(struct parser *parser, char *text, int continuation)
{
	char *key, *value;
	if (!continuation) {
		if (parser->section != SECTION_ROUTES || text[0] != '-' ||
		    text[1] != ' ' ||
		    parser->configuration->route_count == NETCONF_MAX_ROUTES)
			return fail(parser, "expected a route entry");
		parser->route =
		    &parser->configuration
			 ->routes[parser->configuration->route_count++];
		parser->route_seen = 0;
		text += 2;
	} else if (parser->route == NULL)
		return fail(parser, "route property without route");
	if (split_mapping(parser, text, &key, &value) != 0)
		return -1;
	if (strcmp(key, "destination") == 0) {
		if (set_once(parser, &parser->route_seen, 1U, key) != 0 ||
		    (strcmp(value, "default") != 0 &&
		     !ipv4_prefix_valid(value)))
			return fail(parser, "invalid route destination");
		return copy_value(parser, parser->route->destination,
				  sizeof(parser->route->destination), value,
				  "route destination");
	}
	if (strcmp(key, "gateway") == 0) {
		if (set_once(parser, &parser->route_seen, 2U, key) != 0 ||
		    !ipv4_valid(value))
			return fail(parser, "invalid route gateway");
		strcpy(parser->route->gateway, value);
		return 0;
	}
	return fail(parser, "unknown route key %s", key);
}

static int
dns_property(struct parser *parser, char *text)
{
	char *key, *value;
	if (parser->section != SECTION_DNS ||
	    split_mapping(parser, text, &key, &value) != 0)
		return fail(parser, "expected DNS property");
	if (strcmp(key, "mode") == 0) {
		if (set_once(parser, &parser->dns_seen, 1U, key) != 0)
			return -1;
		if (strcmp(value, "dhcp") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_DHCP;
		else if (strcmp(value, "static") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_STATIC;
		else if (strcmp(value, "merge") == 0)
			parser->configuration->dns_mode = NETCONF_DNS_MERGE;
		else
			return fail(parser, "invalid DNS mode");
		parser->subsection = SUBSECTION_NONE;
	} else if (strcmp(key, "servers") == 0 && *value == '\0') {
		if (set_once(parser, &parser->dns_seen, 2U, key) != 0)
			return -1;
		parser->subsection = SUBSECTION_DNS_SERVERS;
	} else
		return fail(parser, "unknown DNS key %s", key);
	return 0;
}

static int
dns_entry(struct parser *parser, char *text)
{
	const char *address;
	size_t index;
	if (parser->section != SECTION_DNS ||
	    parser->subsection != SUBSECTION_DNS_SERVERS || text[0] != '-' ||
	    text[1] != ' ' || !ipv4_valid(address = text + 2))
		return fail(parser, "expected - DNS-address");
	for (index = 0; index < parser->configuration->dns_count; index++)
		if (strcmp(parser->configuration->dns_servers[index],
			   address) == 0)
			return fail(parser, "duplicate DNS server %s", address);
	if (parser->configuration->dns_count == NETCONF_MAX_DNS)
		return fail(parser, "too many DNS servers");
	strcpy(parser->configuration
		   ->dns_servers[parser->configuration->dns_count++],
	       address);
	return 0;
}

static int
parse_content(struct parser *parser, unsigned indent, char *text)
{
	if (indent == 0)
		return parse_top(parser, text);
	if (parser->section == SECTION_INTERFACES) {
		if (indent == 2)
			return new_interface(parser, text);
		if (indent == 4)
			return interface_property(parser, text);
		if (indent == 6 && parser->subsection == SUBSECTION_IPV4)
			return ipv4_property(parser, text);
		if (indent == 6 && parser->subsection == SUBSECTION_MEMBERS)
			return member_entry(parser, text);
		if (indent == 8)
			return new_address(parser, text);
		if (indent == 10)
			return address_property(parser, text);
	}
	if (parser->section == SECTION_ROUTES) {
		if (indent == 2)
			return route_entry(parser, text, 0);
		if (indent == 4)
			return route_entry(parser, text, 1);
	}
	if (parser->section == SECTION_DNS) {
		if (indent == 2)
			return dns_property(parser, text);
		if (indent == 4)
			return dns_entry(parser, text);
	}
	return fail(parser, "unexpected indentation or nesting");
}

static struct netconf_interface *
find_interface(const struct netconf *configuration, const char *name)
{
	size_t index;
	for (index = 0; index < configuration->interface_count; index++)
		if (strcmp(configuration->interfaces[index].name, name) == 0)
			return (struct netconf_interface *)&configuration
			    ->interfaces[index];
	return NULL;
}

static int
validate_graph(const struct netconf *configuration, size_t index,
	       unsigned *visiting, unsigned *visited)
{
	const struct netconf_interface *interface =
	    &configuration->interfaces[index];
	size_t child, member;
	unsigned bit = 1U << index;
	if ((*visiting & bit) != 0)
		return -1;
	if ((*visited & bit) != 0)
		return 0;
	*visiting |= bit;
	if (*interface->parent != '\0') {
		struct netconf_interface *target =
		    find_interface(configuration, interface->parent);
		if (target == NULL)
			return -1;
		child = (size_t)(target - configuration->interfaces);
		if (validate_graph(configuration, child, visiting, visited) !=
		    0)
			return -1;
	}
	for (member = 0; member < interface->member_count; member++) {
		struct netconf_interface *target =
		    find_interface(configuration, interface->members[member]);
		if (target == NULL)
			return -1;
		child = (size_t)(target - configuration->interfaces);
		if (validate_graph(configuration, child, visiting, visited) !=
		    0)
			return -1;
	}
	*visiting &= ~bit;
	*visited |= bit;
	return 0;
}

int
netconf_validate(const struct netconf *configuration, char *error,
		 size_t capacity)
{
	size_t index, address;
	unsigned visiting = 0, visited = 0;
#define VALIDATE_ERROR(...)                                                    \
	do {                                                                   \
		if (error != NULL && capacity != 0)                            \
			(void)snprintf(error, capacity, __VA_ARGS__);          \
		errno = EINVAL;                                                \
		return -1;                                                     \
	} while (0)
	if (configuration->version != 1)
		VALIDATE_ERROR("version must be 1");
	for (index = 0; index < configuration->interface_count; index++) {
		const struct netconf_interface *item =
		    &configuration->interfaces[index];
		if (item->type == NETCONF_INTERFACE_UNSET || !item->enabled_set)
			VALIDATE_ERROR("interface %s lacks type or enabled",
				       item->name);
		if (item->dhcp && item->address_count != 0)
			VALIDATE_ERROR(
			    "interface %s mixes DHCP and static addresses",
			    item->name);
		if (item->dhcp_timeout_set && !item->dhcp)
			VALIDATE_ERROR("interface %s has timeout without DHCP",
				       item->name);
		for (address = 0; address < item->address_count; address++)
			if (item->addresses[address].prefix_length > 32)
				VALIDATE_ERROR(
				    "interface %s address lacks prefix",
				    item->name);
		if (item->type == NETCONF_INTERFACE_VLAN) {
			if (*item->parent == '\0' || !item->vlan_id_set)
				VALIDATE_ERROR("VLAN %s lacks parent or ID",
					       item->name);
		} else if (*item->parent != '\0' || item->vlan_id_set)
			VALIDATE_ERROR("non-VLAN %s has VLAN fields",
				       item->name);
		if (item->type != NETCONF_INTERFACE_BRIDGE &&
		    item->member_count != 0)
			VALIDATE_ERROR("non-bridge %s has members", item->name);
	}
	for (index = 0; index < configuration->route_count; index++)
		if (*configuration->routes[index].destination == '\0' ||
		    *configuration->routes[index].gateway == '\0')
			VALIDATE_ERROR("route lacks destination or gateway");
	if (configuration->dns_mode == NETCONF_DNS_UNSET)
		VALIDATE_ERROR("DNS mode is required");
	if (configuration->dns_mode == NETCONF_DNS_STATIC &&
	    configuration->dns_count == 0)
		VALIDATE_ERROR("static DNS requires servers");
	for (index = 0; index < configuration->interface_count; index++)
		if (validate_graph(configuration, index, &visiting, &visited) !=
		    0)
			VALIDATE_ERROR("invalid or cyclic interface topology");
	return 0;
#undef VALIDATE_ERROR
}

int
netconf_parse(FILE *stream, struct netconf *configuration, char *error,
	      size_t capacity)
{
	struct parser parser;
	char line[NETCONF_LINE_MAX];
	if (stream == NULL || configuration == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(configuration, 0, sizeof(*configuration));
	memset(&parser, 0, sizeof(parser));
	parser.configuration = configuration;
	parser.error = error;
	parser.error_capacity = capacity;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *cursor, *end, *comment;
		unsigned indent = 0;
		parser.line++;
		if (strchr(line, '\n') == NULL && !feof(stream))
			return fail(&parser, "line is too long");
		end = line + strlen(line);
		while (end > line &&
		       (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
			*--end = '\0';
		for (cursor = line; *cursor == ' '; cursor++)
			indent++;
		if (*cursor == '\t' || strchr(cursor, '\t') != NULL)
			return fail(&parser, "tabs are forbidden");
		comment = strchr(cursor, '#');
		if (comment != NULL &&
		    (comment == cursor || comment[-1] == ' ')) {
			*comment = '\0';
			end = comment;
			while (end > cursor && end[-1] == ' ')
				*--end = '\0';
		}
		if (*cursor == '\0')
			continue;
		if ((indent & 1U) != 0)
			return fail(&parser,
				    "indentation must use two-space units");
		if (strchr(cursor, '{') != NULL ||
		    strchr(cursor, '}') != NULL ||
		    strchr(cursor, '[') != NULL ||
		    strchr(cursor, ']') != NULL ||
		    strchr(cursor, '&') != NULL ||
		    strchr(cursor, '*') != NULL ||
		    strncmp(cursor, "---", 3) == 0)
			return fail(&parser, "unsupported YAML feature");
		if (parse_content(&parser, indent, cursor) != 0)
			return -1;
	}
	if (ferror(stream)) {
		errno = EIO;
		return -1;
	}
	if ((parser.top_seen & 3U) != 3U)
		return fail(&parser, "version and interfaces are required");
	return netconf_validate(configuration, error, capacity);
}

int
netconf_load(const char *path, struct netconf *configuration, char *error,
	     size_t capacity)
{
	FILE *stream = fopen(path, "r");
	int result;
	if (stream == NULL)
		return -1;
	result = netconf_parse(stream, configuration, error, capacity);
	if (fclose(stream) != 0 && result == 0)
		return -1;
	return result;
}

static const char *
type_name(enum netconf_interface_type type)
{
	static const char *const names[] = {"unset", "loopback", "ethernet",
					    "vlan", "bridge"};
	return names[type];
}

static const char *
dns_name(enum netconf_dns_mode mode)
{
	static const char *const names[] = {"unset", "dhcp", "static", "merge"};
	return names[mode];
}

int
netconf_write(FILE *stream, const struct netconf *configuration)
{
	size_t index, child;
	char error[128];
	if (stream == NULL || configuration == NULL ||
	    netconf_validate(configuration, error, sizeof(error)) != 0)
		return -1;
	if (fprintf(stream, "version: 1\n\ninterfaces:\n") < 0)
		return -1;
	for (index = 0; index < configuration->interface_count; index++) {
		const struct netconf_interface *item =
		    &configuration->interfaces[index];
		if (fprintf(stream, "  %s:\n    type: %s\n    enabled: %s\n",
			    item->name, type_name(item->type),
			    item->enabled ? "true" : "false") < 0)
			return -1;
		if (*item->parent != '\0' &&
		    fprintf(stream, "    parent: %s\n", item->parent) < 0)
			return -1;
		if (item->vlan_id_set &&
		    fprintf(stream, "    vlan-id: %u\n", item->vlan_id) < 0)
			return -1;
		if (item->member_count != 0) {
			if (fputs("    members:\n", stream) == EOF)
				return -1;
			for (child = 0; child < item->member_count; child++)
				if (fprintf(stream, "      - %s\n",
					    item->members[child]) < 0)
					return -1;
		}
		if (item->dhcp_set || item->address_count != 0) {
			if (fputs("    ipv4:\n", stream) == EOF)
				return -1;
			if (item->dhcp_set &&
			    fprintf(stream, "      dhcp: %s\n",
				    item->dhcp ? "true" : "false") < 0)
				return -1;
			if (item->dhcp_timeout_set &&
			    fprintf(stream, "      dhcp-timeout: %u\n",
				    item->dhcp_timeout) < 0)
				return -1;
			if (item->address_count != 0) {
				if (fputs("      addresses:\n", stream) == EOF)
					return -1;
				for (child = 0; child < item->address_count;
				     child++)
					if (fprintf(
						stream,
						"        - address: %s\n"
						"          prefix-length: %u\n",
						item->addresses[child].address,
						item->addresses[child]
						    .prefix_length) < 0)
						return -1;
			}
		}
		if (fputc('\n', stream) == EOF)
			return -1;
	}
	if (configuration->route_count != 0) {
		if (fputs("routes:\n", stream) == EOF)
			return -1;
		for (index = 0; index < configuration->route_count; index++)
			if (fprintf(stream,
				    "  - destination: %s\n    gateway: %s\n",
				    configuration->routes[index].destination,
				    configuration->routes[index].gateway) < 0)
				return -1;
		if (fputc('\n', stream) == EOF)
			return -1;
	}
	if (fprintf(stream, "dns:\n  mode: %s\n",
		    dns_name(configuration->dns_mode)) < 0)
		return -1;
	if (configuration->dns_count != 0) {
		if (fputs("  servers:\n", stream) == EOF)
			return -1;
		for (index = 0; index < configuration->dns_count; index++)
			if (fprintf(stream, "    - %s\n",
				    configuration->dns_servers[index]) < 0)
				return -1;
	}
	return ferror(stream) ? -1 : 0;
}

int
netconf_save_atomic(const char *path, const struct netconf *configuration,
		    char *error, size_t capacity)
{
	char temporary[512], validation[160];
	FILE *stream = NULL;
	int descriptor = -1, saved_errno, temporary_created = 0;

	if (path == NULL || configuration == NULL ||
	    netconf_validate(configuration, validation, sizeof(validation)) !=
		0) {
		if (error != NULL && capacity != 0)
			(void)snprintf(error, capacity, "%s",
				       path == NULL || configuration == NULL
					   ? "invalid save request"
					   : validation);
		errno = EINVAL;
		return -1;
	}
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		if (error != NULL && capacity != 0)
			(void)snprintf(error, capacity, "path is too long");
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (descriptor < 0)
		goto failed;
	temporary_created = 1;
	stream = fdopen(descriptor, "w");
	if (stream == NULL)
		goto failed;
	descriptor = -1;
	if (netconf_write(stream, configuration) != 0 || fflush(stream) != 0 ||
	    fsync(fileno(stream)) != 0)
		goto failed;
	if (fclose(stream) != 0) {
		stream = NULL;
		goto failed;
	}
	stream = NULL;
	if (rename(temporary, path) != 0)
		goto failed;
	if (error != NULL && capacity != 0)
		error[0] = '\0';
	return 0;

failed:
	saved_errno = errno;
	if (stream != NULL)
		(void)fclose(stream);
	else if (descriptor >= 0)
		(void)close(descriptor);
	if (temporary_created)
		(void)unlink(temporary);
	if (error != NULL && capacity != 0)
		(void)snprintf(error, capacity, "%s", strerror(saved_errno));
	errno = saved_errno;
	return -1;
}
