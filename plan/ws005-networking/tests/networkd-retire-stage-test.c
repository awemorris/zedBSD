/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Exercises actual managed retirement with separate child and L3 failures. */

#include "userland/base/net/netutil.h"
#include "userland/base/net/protocol.h"
#include "userland/base/networkd/wifi-child.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fixture_socket(int, int, int);
static int fixture_close(int);
static int fixture_ioctl(int, unsigned long, ...);
static int fixture_open(const char *, int, ...);
static ssize_t fixture_read(int, void *, size_t);
static int fixture_unlink(const char *);
static int fixture_ifindex(int, const char *, uint32_t *);
static int fixture_ifreq(struct ifreq *, const char *);
static int fixture_child(const char *, const char *, const void *, size_t, const void *, size_t, unsigned, struct networkd_wifi_child_result *);
static void fixture_child_clear(struct networkd_wifi_child_result *);
static void fixture_clear(void *, size_t);
static int fixture_fprintf(FILE *, const char *, ...);

#define socket fixture_socket
#define close fixture_close
#define ioctl fixture_ioctl
#define open fixture_open
#define read fixture_read
#define unlink fixture_unlink
#define netutil_ifindex fixture_ifindex
#define netutil_ifreq fixture_ifreq
#define networkd_wifi_child_run fixture_child
#define networkd_wifi_child_result_clear fixture_child_clear
#define networkd_protocol_clear fixture_clear
#define fprintf fixture_fprintf
#define main networkd_program_main
#include "userland/base/networkd/main.c"
#undef main
#undef fprintf
#undef networkd_protocol_clear
#undef networkd_wifi_child_result_clear
#undef networkd_wifi_child_run
#undef netutil_ifreq
#undef netutil_ifindex
#undef unlink
#undef read
#undef open
#undef ioctl
#undef close
#undef socket

static void expect(int, const char *);
static void run_case(int, int, int, unsigned, int, const char *);
static int fixture_errno;
static int identity_error;
static int child_error;
static int l3_error;
static unsigned child_records;
static unsigned socket_calls;
static unsigned child_calls;
static unsigned clear_calls;
static char diagnostic[2048];
static size_t diagnostic_length;

/* Retirement-only calls run outside a client request in this fixture. */
uint64_t
netutil_monotonic_us(void)
{
	return 1000000ULL;
}

/* Supplies the project libc error slot independently of host errno values. */
int *
__libc_errno_location(void)
{
	return &fixture_errno;
}

/* Runs success, child setup/terminal, L3, identity and combined failures. */
int
main(
	void)
{
	run_case(0, 0, 0, 2U, 0, NULL);
	run_case(ENODEV, 0, 0, 0U, 0, NULL);
	run_case(EIO, 0, 0, 0U, EIO, "stage=identity");
	run_case(0, EBUSY, 0, 0U, EBUSY, "stage=wifi-disconnect");
	run_case(0, EBUSY, 0, 1U, EBUSY, "stage=wifi-disconnect");
	run_case(0, 0, EBUSY, 2U, EBUSY, "stage=l3");
	run_case(0, EBUSY, EIO, 1U, EBUSY, "stage=wifi-disconnect");
	puts("networkd-retire-stage-test: PASS");
	return 0;
}

/* Stops on a violated retirement or nonsecret diagnostic contract. */
static void
expect(
	int condition,
	const char *message)
{
	if (!condition) {
		fprintf(stderr, "networkd-retire-stage-test: %s\n", message);
		exit(1);
	}
}

/* Executes the production retirement function with exact injected outcomes. */
static void
run_case(
	int identity,
	int child,
	int l3,
	unsigned records,
	int expected,
	const char *stage)
{
	char expected_metadata[256];
	int result;

	identity_error = identity;
	child_error = child;
	l3_error = l3;
	child_records = records;
	fixture_errno = 0;
	socket_calls = 0U;
	child_calls = 0U;
	clear_calls = 0U;
	diagnostic_length = 0U;
	memset(diagnostic, 0, sizeof(diagnostic));
	memset(&managed_wlan, 0, sizeof(managed_wlan));
	managed_wlan.owner_valid = 1;
	managed_wlan.state = NETWORKD_WLAN_CONNECTED;
	strcpy(managed_wlan.connection.interface, "wlan0");
	managed_wlan.connection.ifindex = 9U;
	managed_wlan.connection.owns_l3 = 1;
	memcpy(managed_wlan.connection.ssid, "private-identity", 16U);
	managed_wlan.connection.ssid_length = 16U;
	result = retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1);
	expect(result == (expected == 0 ? 0 : -1), "original return result");
	expect(child_calls == (identity == 0 ? 1U : 0U), "single child invocation");
	expect(clear_calls == child_calls, "child storage always erased");
	if (expected == 0) {
		expect(diagnostic_length == 0U, "successful retirement remains quiet");
		expect(managed_wlan.state == NETWORKD_WLAN_MANUAL_DISCONNECTED &&
		    managed_wlan.connection.interface[0] == '\0', "success retires token");
	} else {
		expect(fixture_errno == expected, "first error survives diagnostic errno");
		expect(managed_wlan.state == NETWORKD_WLAN_RETIRING &&
		    strcmp(managed_wlan.connection.interface, "wlan0") == 0,
		    "failure preserves connection token");
		expect(managed_wlan.connection.owns_l3 ==
		    (identity != 0 || child != 0 || l3 != 0),
		    "failed L2 retirement preserves L3 until retry");
		expect(strstr(diagnostic, stage) != NULL, "first failing stage");
		(void)snprintf(expected_metadata, sizeof(expected_metadata),
		    "error=%d disconnect-error=%d l3-error=%d child-error=%d "
		    "child-exit=%d child-signal=0 child-records=%u",
		    expected, child, child == 0 ? l3 : 0, child,
		    child != 0 && records != 0U ? 1 : 0,
		    records);
		expect(strstr(diagnostic, expected_metadata) != NULL,
		    "separate structured child and L3 outcomes");
		expect(strstr(diagnostic, "private-child") == NULL &&
		    strstr(diagnostic, "private-identity") == NULL,
		    "no child content or connection identity in diagnostic");
	}
}

/* Injects identity failure before the child or L3 failure after it. */
static int
fixture_socket(
	int domain,
	int type,
	int protocol)
{
	expect(domain == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP,
	    "ordinary control socket");
	socket_calls++;
	if ((socket_calls == 1U && identity_error != 0) ||
	    (socket_calls == 2U && l3_error != 0)) {
		fixture_errno = socket_calls == 1U ? identity_error : l3_error;
		return -1;
	}
	return 30;
}

/* Closes only synthetic descriptors. */
static int
fixture_close(
	int descriptor)
{
	expect(descriptor == 30, "synthetic descriptor close");
	return 0;
}

/* Supplies stable identity and empty unowned L3 values without host IO. */
static int
fixture_ioctl(
	int descriptor,
	unsigned long command,
	...)
{
	struct ifreq *request;
	va_list arguments;

	expect(descriptor == 30, "synthetic ioctl descriptor");
	if (command == SIOCGRTENTRY) {
		fixture_errno = ENOENT;
		return -1;
	}
	va_start(arguments, command);
	request = va_arg(arguments, struct ifreq *);
	va_end(arguments);
	if (command == SIOCGIFNAME) {
		strcpy(request->ifr_name, "wlan0");
		return 0;
	}
	expect(command == SIOCGIFADDR || command == SIOCGIFNETMASK ||
	    command == SIOCGIFBRDADDR, "only readonly empty L3 queries");
	memset(&request->ifr_addr, 0, sizeof(request->ifr_addr));
	return 0;
}

/* Models an absent resolver without opening any host file. */
static int
fixture_open(
	const char *path,
	int flags,
	...)
{
	(void)flags;
	expect(strcmp(path, "/etc/resolv.conf") == 0, "resolver path");
	fixture_errno = ENOENT;
	return -1;
}

/* Rejects all file reads in the absent-resolver scenario. */
static ssize_t
fixture_read(
	int descriptor,
	void *bytes,
	size_t length)
{
	(void)descriptor;
	(void)bytes;
	(void)length;
	expect(0, "unexpected file read");
	return -1;
}

/* Rejects any attempted host file deletion. */
static int
fixture_unlink(
	const char *path)
{
	(void)path;
	expect(0, "unexpected file unlink");
	return -1;
}

/* Returns the recorded interface index. */
static int
fixture_ifindex(
	int descriptor,
	const char *name,
	uint32_t *index)
{
	expect(descriptor == 30 && strcmp(name, "wlan0") == 0, "interface index");
	*index = 9U;
	return 0;
}

/* Initializes a generic interface request. */
static int
fixture_ifreq(
	struct ifreq *request,
	const char *name)
{
	memset(request, 0, sizeof(*request));
	strcpy(request->ifr_name, name);
	return 0;
}

/* Injects child-boundary metadata while retaining deliberately private content. */
static int
fixture_child(
	const char *interface,
	const char *operation,
	const void *ssid,
	size_t ssid_length,
	const void *passphrase,
	size_t passphrase_length,
	unsigned timeout,
	struct networkd_wifi_child_result *result)
{
	expect(strcmp(interface, "wlan0") == 0 && strcmp(operation, "disconnect") == 0,
	    "disconnect primitive");
	expect(ssid == NULL && ssid_length == 0U && passphrase == NULL &&
	    passphrase_length == 0U && timeout == 10U, "unchanged child contract");
	child_calls++;
	memset(result, 0, sizeof(*result));
	strcpy((char *)result->output, "private-child-output");
	strcpy(result->diagnostic, "private-child-diagnostic");
	result->terminal_error = child_error;
	result->child_exit_status = child_error != 0 && child_records != 0U ? 1 : 0;
	result->output_records = child_records;
	fixture_errno = child_error;
	return child_error != 0 ? -1 : 0;
}

/* Clears the complete child result after its numeric metadata is captured. */
static void
fixture_child_clear(
	struct networkd_wifi_child_result *result)
{
	clear_calls++;
	memset(result, 0, sizeof(*result));
}

/* Clears private buffers using the fixture's ordinary memory model. */
static void
fixture_clear(
	void *bytes,
	size_t length)
{
	memset(bytes, 0, length);
}

/* Captures diagnostics and deliberately changes errno to test preservation. */
static int
fixture_fprintf(
	FILE *stream,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	expect(stream == stderr, "diagnostic stream");
	va_start(arguments, format);
	length = vsnprintf(diagnostic + diagnostic_length,
	    sizeof(diagnostic) - diagnostic_length, format, arguments);
	va_end(arguments);
	expect(length >= 0 && (size_t)length < sizeof(diagnostic) - diagnostic_length,
	    "bounded diagnostic");
	diagnostic_length += (size_t)length;
	fixture_errno = EPIPE;
	return length;
}
