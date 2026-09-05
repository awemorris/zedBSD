/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/net/reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
	char output[2048];
	size_t used;
	unsigned count;
	unsigned fail_at;
};

static void fail(const char *);
static void expect(int, const char *);
static int emit(const char *, const char *, void *);
static void base(struct netconf *);
static struct netconf_interface *add_interface(struct netconf *, const char *);

int
main(
	void)
{
	struct netconf old;
	struct netconf candidate;
	struct netconf_interface *item;
	struct fixture fixture;
	char error[160];

	base(&old);
	item = add_interface(&old, "em0");
	item->addresses[0].prefix_length = 16U;
	strcpy(item->addresses[0].address, "10.0.0.100");
	item->address_count = 1U;
	strcpy(old.routes[0].destination, "default");
	strcpy(old.routes[0].gateway, "10.0.0.1");
	old.route_count = 1U;
	old.dns_mode = NETCONF_DNS_STATIC;
	strcpy(old.dns_servers[0], "10.0.0.1");
	old.dns_count = 1U;

	base(&candidate);
	item = add_interface(&candidate, "ne0");
	item->dhcp = 1;
	item->dhcp_set = 1;
	item->dhcp_timeout = 17U;
	item->dhcp_timeout_set = 1;

	memset(&fixture, 0, sizeof(fixture));
	expect(netconf_reconcile(&old, &candidate, emit, &fixture, error,
	    sizeof(error)) == 0, "forward reconcile");
	expect(strcmp(fixture.output,
	    "DEFAULTROUTE_CLEAR\nDNS_CLEAR\nDOWN em0\nUP ne0\nDHCP ne0 17\n") == 0,
	    "forward canonical sequence");

	memset(&fixture, 0, sizeof(fixture));
	expect(netconf_reconcile(&candidate, &old, emit, &fixture, error,
	    sizeof(error)) == 0, "rollback reconcile");
	expect(strstr(fixture.output, "DOWN ne0\nUP em0\n") != NULL &&
	    strstr(fixture.output,
	    "STATIC em0 ipv4 10.0.0.100 netmask 255.255.0.0\n") != NULL &&
	    strstr(fixture.output,
	    "DEFAULTROUTE_CLEAR\nDEFAULTROUTE 10.0.0.1\n") != NULL &&
	    strstr(fixture.output, "DNS 10.0.0.1\n") != NULL,
	    "rollback restores complete intent");

	memset(&fixture, 0, sizeof(fixture));
	fixture.fail_at = 3U;
	expect(netconf_reconcile(&old, &candidate, emit, &fixture, error,
	    sizeof(error)) != 0 && fixture.count == 3U,
	    "stop forward sequence on failure");

	candidate.route_count = 2U;
	strcpy(candidate.routes[0].destination, "default");
	strcpy(candidate.routes[0].gateway, "10.0.0.1");
	strcpy(candidate.routes[1].destination, "default");
	strcpy(candidate.routes[1].gateway, "10.0.0.2");
	expect(netconf_reconcile_supported(&candidate, error, sizeof(error)) != 0,
	    "reject multiple default routes");
	puts("WS011 netconf reconcile: PASS");
	return 0;
}

static void
fail(
	const char *message)
{
	fprintf(stderr, "netconf-reconcile-test: %s\n", message);
	exit(1);
}

static void
expect(
	int condition,
	const char *message)
{
	if (!condition)
		fail(message);
}

static int
emit(
	const char *operation,
	const char *operands,
	void *context)
{
	struct fixture *fixture = context;
	int count;

	fixture->count++;
	if (fixture->fail_at == fixture->count)
		return -1;
	count = snprintf(fixture->output + fixture->used,
	    sizeof(fixture->output) - fixture->used, "%s%s%s\n", operation,
	    operands != NULL ? " " : "", operands != NULL ? operands : "");
	if (count < 0 || (size_t)count >= sizeof(fixture->output) - fixture->used)
		return -1;
	fixture->used += (size_t)count;
	return 0;
}

static void
base(
	struct netconf *configuration)
{
	memset(configuration, 0, sizeof(*configuration));
	configuration->version = 1U;
	configuration->dns_mode = NETCONF_DNS_DHCP;
}

static struct netconf_interface *
add_interface(
	struct netconf *configuration,
	const char *name)
{
	struct netconf_interface *item;

	item = &configuration->interfaces[configuration->interface_count++];
	strcpy(item->name, name);
	item->type = NETCONF_INTERFACE_ETHERNET;
	item->enabled = 1;
	item->enabled_set = 1;
	return item;
}
