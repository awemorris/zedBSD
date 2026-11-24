/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for userland/desktop/rdp CLI parsing and input mapping.
 */

#include "userland/desktop/rdp/rdp-client.h"
#include "userland/desktop/librdp/rdp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Runs all zedBSD rdp client host unit tests.
 */
int
main(
	void)
{
	struct rdp_app_target target;
	char *argv1[] = {
		"rdp", "--width=1920", "--height=1080", "--domain=ZED",
		"--password=secret", "awe@10.0.2.2:3390"
	};
	char *argv2[] = {
		"rdp", "rdp://CORP\\bob@workstation"
	};
	char *argv_bad[] = {
		"rdp", "--width=800"
	};
	uint16_t scan;
	uint16_t flags;

	/* Tests full CLI option and user@host:port parsing. */
	assert(rdp_parse_args(6, argv1, &target) == 0);
	assert(target.width == 1920U);
	assert(target.height == 1080U);
	assert(strcmp(target.domain, "ZED") == 0);
	assert(strcmp(target.password, "secret") == 0);
	assert(strcmp(target.user, "awe") == 0);
	assert(strcmp(target.host, "10.0.2.2") == 0);
	assert(target.port == 3390U);

	/* Tests rdp://URI with domain\user and default port. */
	assert(rdp_parse_args(2, argv2, &target) == 0);
	assert(strcmp(target.domain, "CORP") == 0);
	assert(strcmp(target.user, "bob") == 0);
	assert(strcmp(target.host, "workstation") == 0);
	assert(target.port == RDP_DEFAULT_PORT);

	/* Rejects missing host argument. */
	assert(rdp_parse_args(2, argv_bad, &target) != 0);

	/* Tests evdev-to-PS/2 Set 1 scancode mapping. */
	assert(rdp_evdev_to_scancode(30U, &scan, &flags) == 1);
	assert(scan == 30U && flags == 0U);
	assert(rdp_evdev_to_scancode(103U, &scan, &flags) == 1);
	assert(scan == 0x48U && flags == RDP_KBD_FLAG_EXTENDED);
	assert(rdp_evdev_to_scancode(999U, &scan, &flags) == 0);
	printf("rdp-client-host-test: all tests passed\n");
	return 0;
}
