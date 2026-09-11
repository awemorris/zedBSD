/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/netconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char valid_configuration[] = "version: 1\n"
					  "interfaces:\n"
					  "  lo0:\n"
					  "    type: loopback\n"
					  "    enabled: true\n"
					  "    ipv4:\n"
					  "      addresses:\n"
					  "        - address: 127.0.0.1\n"
					  "          prefix-length: 8\n"
					  "  ne0:\n"
					  "    type: ethernet\n"
					  "    enabled: true\n"
					  "    ipv4:\n"
					  "      dhcp: true\n"
					  "      dhcp-timeout: 10\n"
					  "  vlan10:\n"
					  "    type: vlan\n"
					  "    enabled: true\n"
					  "    parent: ne0\n"
					  "    vlan-id: 10\n"
					  "  bridge0:\n"
					  "    type: bridge\n"
					  "    enabled: true\n"
					  "    members:\n"
					  "      - vlan10\n"
					  "routes:\n"
					  "  - destination: default\n"
					  "    gateway: 10.0.2.2\n"
					  "dns:\n"
					  "  mode: merge\n"
					  "  servers:\n"
					  "    - 1.1.1.1\n";

static int
parse_text(const char *text, struct netconf *configuration, char error[160])
{
	FILE *stream = tmpfile();
	int result;
	if (stream == NULL || fputs(text, stream) == EOF ||
	    fflush(stream) != 0 || fseek(stream, 0, SEEK_SET) != 0)
		return -2;
	result = netconf_parse(stream, configuration, error, 160);
	fclose(stream);
	return result;
}

static void
require_invalid(const char *name, const char *text)
{
	struct netconf configuration;
	char error[160] = "";
	if (parse_text(text, &configuration, error) == 0) {
		fprintf(stderr, "%s unexpectedly accepted\n", name);
		exit(1);
	}
	if (*error == '\0') {
		fprintf(stderr, "%s lacks diagnostic\n", name);
		exit(1);
	}
}

int
main(void)
{
	struct netconf first, second;
	char error[160] = "";
	FILE *canonical;
	char output[8192];
	size_t length;

	if (parse_text(valid_configuration, &first, error) != 0) {
		fprintf(stderr, "valid configuration rejected: %s\n", error);
		return 1;
	}
	if (first.interface_count != 4 || first.route_count != 1 ||
	    first.dns_count != 1 || first.interfaces[1].dhcp_timeout != 10 ||
	    first.interfaces[3].member_count != 1) {
		fprintf(stderr, "parsed model mismatch\n");
		return 1;
	}
	canonical = tmpfile();
	if (canonical == NULL || netconf_write(canonical, &first) != 0 ||
	    fflush(canonical) != 0 || fseek(canonical, 0, SEEK_SET) != 0 ||
	    (length = fread(output, 1, sizeof(output) - 1, canonical)) == 0) {
		fprintf(stderr, "canonical write failed\n");
		return 1;
	}
	output[length] = '\0';
	fclose(canonical);
	if (parse_text(output, &second, error) != 0 ||
	    netconf_write((canonical = tmpfile()), &second) != 0) {
		fprintf(stderr, "canonical round trip failed: %s\n", error);
		return 1;
	}
	fclose(canonical);

	require_invalid("tab", "version: 1\ninterfaces:\n\tlo0:\n");
	require_invalid("duplicate", "version: 1\nversion: 1\ninterfaces:\n"
				     "  lo0:\n    type: loopback\n    enabled: "
				     "true\ndns:\n  mode: dhcp\n");
	require_invalid("unknown", "version: 1\ninterfaces:\n  lo0:\n"
				   "    type: loopback\n    enabled: true\n    "
				   "mystery: value\ndns:\n  mode: dhcp\n");
	require_invalid("bad IPv4", "version: 1\ninterfaces:\n  lo0:\n"
				    "    type: loopback\n    enabled: true\n   "
				    " ipv4:\n      addresses:\n"
				    "        - address: 999.0.0.1\n          "
				    "prefix-length: 8\ndns:\n  mode: dhcp\n");
	require_invalid("DHCP static conflict",
			"version: 1\ninterfaces:\n  ne0:\n"
			"    type: ethernet\n    enabled: true\n    ipv4:\n    "
			"  dhcp: true\n"
			"      addresses:\n        - address: 10.0.0.2\n       "
			"   prefix-length: 24\n"
			"dns:\n  mode: dhcp\n");
	require_invalid("missing VLAN parent",
			"version: 1\ninterfaces:\n  vlan10:\n"
			"    type: vlan\n    enabled: true\n    vlan-id: "
			"10\ndns:\n  mode: dhcp\n");
	require_invalid(
	    "cycle",
	    "version: 1\ninterfaces:\n  br0:\n"
	    "    type: bridge\n    enabled: true\n    members:\n      - br1\n"
	    "  br1:\n    type: bridge\n    enabled: true\n    members:\n      "
	    "- br0\n"
	    "dns:\n  mode: dhcp\n");
	require_invalid("flow syntax",
			"version: 1\ninterfaces: {}\ndns:\n  mode: dhcp\n");

	puts("WS011 net.conf parser: PASS");
	return 0;
}
