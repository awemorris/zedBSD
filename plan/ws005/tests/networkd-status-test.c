/* WS005 production networkd status mapping fixture. SPDX-License-Identifier:
 * Zlib */
#include "userland/base/net/netutil.h"

#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static int fixture_ioctl(int, unsigned long, ...);

#define ioctl fixture_ioctl
#define main networkd_program_main
#include "userland/base/networkd/main.c"
#undef main
#undef ioctl

static int fixture_errno;
static int fixture_flags;
static uint32_t fixture_address;

int *
__libc_errno_location(void)
{
	return &fixture_errno;
}

static void
fail(const char *message)
{
	fprintf(stderr, "networkd-status-test: %s\n", message);
	exit(1);
}

int
netutil_ifreq(struct ifreq *request, const char *name)
{
	if (request == NULL || name == NULL || strlen(name) >= IFNAMSIZ)
		return -1;
	memset(request, 0, sizeof(*request));
	strncpy(request->ifr_name, name, IFNAMSIZ - 1U);
	return 0;
}

static int
fixture_ioctl(int descriptor, unsigned long command, ...)
{
	struct ifreq *request;
	va_list arguments;

	(void)descriptor;
	va_start(arguments, command);
	request = va_arg(arguments, struct ifreq *);
	va_end(arguments);
	if (command == SIOCGIFFLAGS) {
		request->ifr_flags = fixture_flags;
		return 0;
	}
	if (command == SIOCGIFADDR) {
		struct sockaddr_in *address =
		    (struct sockaddr_in *)&request->ifr_addr;

		memset(&request->ifr_addr, 0, sizeof(request->ifr_addr));
		address->sin_family = AF_INET;
		address->sin_addr.s_addr = fixture_address;
		return 0;
	}
	errno = EOPNOTSUPP;
	return -1;
}

static void
expect_status(int flags, uint32_t address, const char *expected)
{
	char output[128] = {0};
	size_t used = 0;

	fixture_flags = flags;
	fixture_address = address;
	if (append_interface_status(10, "ue0", output, sizeof(output), &used) != 0 ||
	    strcmp(output, expected) != 0 || used != strlen(expected))
		fail("status mapping");
}

int
main(void)
{
	expect_status(IFF_UP, UINT32_C(0x01020304), "ue0 static offline\n");
	expect_status(IFF_UP | IFF_RUNNING, UINT32_C(0x01020304),
	    "ue0 static online\n");
	expect_status(0, 0, "ue0 unconfigured offline\n");
	puts("networkd status test: PASS");
	return 0;
}
