/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the bounded networkd Wi-Fi child runner.
 */

#include "userland/base/networkd/wifi-child.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/netif.h>
#include <zedbsd/wlan.h>

#define NETWORKD_WIFI_PATH "/sbin/wifi"
#define NETWORKD_WIFI_SECRET_DESCRIPTOR 4
#define NETWORKD_WIFI_CHILD_COPY_SIZE 1024U
#define NETWORKD_WIFI_CHILD_GRACE_SECONDS 1U
#define NETWORKD_WIFI_CHILD_MALFORMED EILSEQ
#define NETWORKD_WIFI_RECORD_PREFIX "WIFI1 "

enum networkd_wifi_operation_kind {
	NETWORKD_WIFI_OPERATION_SIMPLE,
	NETWORKD_WIFI_OPERATION_SEARCH,
	NETWORKD_WIFI_OPERATION_CONNECT
};

struct networkd_wifi_operation {
	const char *name;
	const char *argument;
	const char *subargument;
	enum networkd_wifi_operation_kind kind;
};

struct networkd_wifi_child_state {
	int secret_pipe[2];
	int output_pipe[2];
	int diagnostic_pipe[2];
	pid_t child;
	int child_status;
	int child_reaped;
	unsigned char secret[WLAN_PASSPHRASE_STORAGE];
	size_t secret_length;
	size_t secret_offset;
};

struct networkd_wifi_record_cursor {
	const unsigned char *bytes;
	size_t length;
	size_t offset;
};

static const struct networkd_wifi_operation networkd_wifi_operations[] = {
	{ "up", "up", NULL, NETWORKD_WIFI_OPERATION_SIMPLE },
	{ "down", "down", NULL, NETWORKD_WIFI_OPERATION_SIMPLE },
	{ "search-start", "search", "start", NETWORKD_WIFI_OPERATION_SEARCH },
	{ "search-stop", "search", "stop", NETWORKD_WIFI_OPERATION_SEARCH },
	{ "list", "list", NULL, NETWORKD_WIFI_OPERATION_SIMPLE },
	{ "status", "status", NULL, NETWORKD_WIFI_OPERATION_SIMPLE },
	{ "connect", "connect", NULL, NETWORKD_WIFI_OPERATION_CONNECT },
	{ "disconnect", "disconnect", NULL, NETWORKD_WIFI_OPERATION_SIMPLE }
};

static const struct networkd_wifi_operation *find_operation(const char *);
static int validate_arguments(const char *, const struct networkd_wifi_operation *, const void *, size_t, const void *, size_t, unsigned);
static void clear_bytes(void *, size_t);
static void initialize_state(struct networkd_wifi_child_state *);
static void close_descriptor(int *);
static void close_pipes(struct networkd_wifi_child_state *);
static int open_pipe(int [2]);
static int open_pipes(struct networkd_wifi_child_state *);
static int set_nonblocking(int);
static int prepare_parent_pipes(struct networkd_wifi_child_state *, int);
static int duplicate_child_descriptor(int);
static void exit_child_setup_failure(int, int, int);
static void execute_child(struct networkd_wifi_child_state *, const struct networkd_wifi_operation *, const char *, const char *);
static int monotonic_deadline(struct timespec *, unsigned);
static int remaining_milliseconds(const struct timespec *, int *);
static int append_output(struct networkd_wifi_child_state *, struct networkd_wifi_child_result *, int *);
static int append_diagnostic(struct networkd_wifi_child_state *, struct networkd_wifi_child_result *, int *);
static int write_secret(struct networkd_wifi_child_state *);
static int reap_child(struct networkd_wifi_child_state *, int);
static int poll_child(struct networkd_wifi_child_state *, struct networkd_wifi_child_result *, const struct timespec *);
static void terminate_child(struct networkd_wifi_child_state *, struct networkd_wifi_child_result *);
static void discard_child(struct networkd_wifi_child_state *);
static int validate_output(const struct networkd_wifi_child_result *, int *);
static int next_output_record(const struct networkd_wifi_child_result *, size_t *, const unsigned char **, size_t *);
static int record_consume(struct networkd_wifi_record_cursor *, const char *);
static int record_unsigned(struct networkd_wifi_record_cursor *, unsigned char, uint64_t, uint64_t *);
static int record_signed(struct networkd_wifi_record_cursor *, unsigned char, int *);
static int lower_hex_value(unsigned char, unsigned *);
static int record_fixed_hex(struct networkd_wifi_record_cursor *, size_t, unsigned char);
static int record_u32_hex(struct networkd_wifi_record_cursor *, unsigned char, uint32_t *);
static int record_ssid_hex(struct networkd_wifi_record_cursor *, const unsigned char *, size_t, int *);
static int parse_scan_record(const unsigned char *, size_t, uint32_t *, unsigned *);
static int parse_bss_record(const unsigned char *, size_t, unsigned, const unsigned char *, size_t, int *, int *);
static int parse_positive_decimal(const unsigned char *, size_t, int *);
static int contains_bytes(const void *, size_t, const void *, size_t);
static void sanitize_diagnostic(struct networkd_wifi_child_result *);
static int finish_child(struct networkd_wifi_child_state *, struct networkd_wifi_child_result *, int);

/*
 * Runs one bounded machine-mode Wi-Fi command.
 *
 * The call is synchronous for networkd's serialized p006 request path.  The
 * implementation uses nonblocking pipes and poll so secret input, machine
 * output, and diagnostics make progress together under one monotonic deadline.
 */
int
networkd_wifi_child_run(
	const char *interface,
	const char *operation_name,
	const void *ssid,
	size_t ssid_length,
	const void *passphrase,
	size_t passphrase_length,
	unsigned timeout_seconds,
	struct networkd_wifi_child_result *result)
{
	const struct networkd_wifi_operation *operation;
	struct networkd_wifi_child_state state;
	void (*saved_sigpipe)(int);
	struct timespec deadline;
	char ssid_text[WLAN_SSID_MAX + 1U];
	int function_result;
	int saved_error;
	int has_secret;

	/* Initializes all caller-visible and owned state before validation. */
	if (result == NULL) {
		errno = EINVAL;
		return -1;
	}
	networkd_wifi_child_result_clear(result);
	initialize_state(&state);
	memset(ssid_text, 0, sizeof(ssid_text));

	/* Resolves and validates the complete child invocation. */
	operation = find_operation(operation_name);
	function_result = validate_arguments(interface, operation, ssid,
	    ssid_length, passphrase, passphrase_length, timeout_seconds);
	if (function_result != 0) {
		result->terminal_error = function_result;
		errno = function_result;
		return -1;
	}
	has_secret = operation->kind == NETWORKD_WIFI_OPERATION_CONNECT;
	if (ssid_length != 0U)
		memcpy(ssid_text, ssid, ssid_length);
	if (passphrase_length != 0U) {
		memcpy(state.secret, passphrase, passphrase_length);
		state.secret_length = passphrase_length;
	}

	/* Creates every private child channel before forking. */
	if (open_pipes(&state) != 0) {
		saved_error = errno != 0 ? errno : EIO;
		close_pipes(&state);
		clear_bytes(&state, sizeof(state));
		clear_bytes(ssid_text, sizeof(ssid_text));
		result->terminal_error = saved_error;
		errno = saved_error;
		return -1;
	}

	/* Establishes the absolute operation deadline before child creation. */
	if (monotonic_deadline(&deadline, timeout_seconds) != 0) {
		saved_error = errno != 0 ? errno : EIO;
		close_pipes(&state);
		clear_bytes(&state, sizeof(state));
		clear_bytes(ssid_text, sizeof(ssid_text));
		result->terminal_error = saved_error;
		errno = saved_error;
		return -1;
	}

	/* Creates the child without adding the passphrase to argv or environ. */
	state.child = fork();
	if (state.child == 0)
		execute_child(&state, operation, interface, ssid_text);
	if (state.child < 0) {
		saved_error = errno != 0 ? errno : EIO;
		close_pipes(&state);
		clear_bytes(&state, sizeof(state));
		clear_bytes(ssid_text, sizeof(ssid_text));
		result->terminal_error = saved_error;
		errno = saved_error;
		return -1;
	}

	/* Retains only the nonblocking parent ends of each private channel. */
	function_result = prepare_parent_pipes(&state, has_secret);
	if (function_result != 0) {
		discard_child(&state);
		networkd_wifi_child_result_clear(result);
		result->terminal_error = function_result;
		clear_bytes(&state, sizeof(state));
		clear_bytes(ssid_text, sizeof(ssid_text));
		errno = function_result;
		return -1;
	}

	/* Prevents an early child exit from delivering SIGPIPE to networkd. */
	saved_sigpipe = signal(SIGPIPE, (void (*)(int))SIG_IGN);
	if (saved_sigpipe == SIG_ERR) {
		saved_error = errno != 0 ? errno : EIO;
		discard_child(&state);
		networkd_wifi_child_result_clear(result);
		result->terminal_error = saved_error;
		clear_bytes(&state, sizeof(state));
		clear_bytes(ssid_text, sizeof(ssid_text));
		errno = saved_error;
		return -1;
	}

	/* Drives all channels and child state under the common deadline. */
	function_result = poll_child(&state, result, &deadline);
	saved_error = function_result != 0 ? function_result : errno;
	if (signal(SIGPIPE, saved_sigpipe) == SIG_ERR && function_result == 0) {
		function_result = errno != 0 ? errno : EIO;
		saved_error = function_result;
	}

	/* Finalizes the process result and clears every secret-bearing buffer. */
	function_result = finish_child(&state, result,
	    function_result != 0 ? function_result : 0);
	if (function_result != 0)
		saved_error = function_result;
	close_pipes(&state);
	clear_bytes(&state, sizeof(state));
	clear_bytes(ssid_text, sizeof(ssid_text));

	/* Reports a bounded child or protocol failure. */
	if (function_result != 0) {
		result->terminal_error = function_result;
		errno = saved_error != 0 ? saved_error : function_result;
		return -1;
	}

	/* Reports successful child completion. */
	return 0;
}

/* Clears one complete Wi-Fi child result without an elidable memset. */
void
networkd_wifi_child_result_clear(
	struct networkd_wifi_child_result *result)
{
	/* Clears caller-owned result storage when it is available. */
	if (result != NULL)
		clear_bytes(result, sizeof(*result));
}

/*
 * Parses one successful machine-mode Wi-Fi list result.
 *
 * The parser validates the complete canonical scan and BSS record sequence.
 * It compares the requested SSID as counted bytes while decoding its
 * lower-case hexadecimal representation and retains no decoded copy.
 */
int
networkd_wifi_child_parse_list(
	const struct networkd_wifi_child_result *result,
	const void *ssid,
	size_t ssid_length,
	struct networkd_wifi_list_result *parsed)
{
	static const unsigned char terminal[] = "WIFI1 terminal ok 0\n";
	const unsigned char *ssid_bytes;
	const unsigned char *record;
	size_t record_length;
	size_t offset;
	uint32_t scan_state;
	unsigned bss_count;
	unsigned bss_index;
	int machine_error;
	int found;
	int supported;
	int any_found;
	int any_supported;
	int function_result;

	/* Validates the caller-owned objects and exact counted SSID bound. */
	if (result == NULL || ssid == NULL || ssid_length == 0U ||
	    ssid_length > WLAN_SSID_MAX || parsed == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(parsed, 0, sizeof(*parsed));
	ssid_bytes = ssid;

	/* Rejects corrupt runner metadata before bounded output inspection. */
	if (result->output_length > sizeof(result->output)) {
		errno = NETWORKD_WIFI_CHILD_MALFORMED;
		return -1;
	}

	/* Requires the runner's successful validated terminal contract. */
	machine_error = -1;
	function_result = validate_output(result, &machine_error);
	if (function_result != 0 || machine_error != 0 ||
	    result->terminal_error != 0 || result->child_exit_status != 0 ||
	    result->child_term_signal != 0) {
		errno = NETWORKD_WIFI_CHILD_MALFORMED;
		return -1;
	}

	/* Parses the sole leading scan snapshot record. */
	offset = 0U;
	function_result = next_output_record(
		result,
		&offset,
		&record,
		&record_length);
	if (function_result != 0) {
		errno = NETWORKD_WIFI_CHILD_MALFORMED;
		return -1;
	}
	function_result = parse_scan_record(
		record,
		record_length,
		&scan_state,
		&bss_count);
	if (function_result != 0) {
		errno = NETWORKD_WIFI_CHILD_MALFORMED;
		return -1;
	}

	/* Parses exactly the BSS count declared by the scan record. */
	any_found = 0;
	any_supported = 0;
	for (bss_index = 0U; bss_index < bss_count; bss_index++) {
		function_result = next_output_record(
			result,
			&offset,
			&record,
			&record_length);
		if (function_result != 0) {
			errno = NETWORKD_WIFI_CHILD_MALFORMED;
			return -1;
		}
		found = 0;
		supported = 0;
		function_result = parse_bss_record(
			record,
			record_length,
			bss_index,
			ssid_bytes,
			ssid_length,
			&found,
			&supported);
		if (function_result != 0) {
			errno = NETWORKD_WIFI_CHILD_MALFORMED;
			return -1;
		}
		if (found)
			any_found = 1;
		if (supported)
			any_supported = 1;
	}

	/* Requires the sole terminal record immediately after the BSS set. */
	function_result = next_output_record(
		result,
		&offset,
		&record,
		&record_length);
	if (function_result != 0 || record_length != sizeof(terminal) - 1U ||
	    memcmp(record, terminal, sizeof(terminal) - 1U) != 0 ||
	    offset != result->output_length ||
	    result->output_records != bss_count + 2U) {
		errno = NETWORKD_WIFI_CHILD_MALFORMED;
		return -1;
	}

	/* Publishes the exact scan-state classification after full validation. */
	parsed->scan_state = scan_state;
	parsed->scan_complete = scan_state == WLAN_SCAN_COMPLETE;
	parsed->scan_terminal = scan_state == WLAN_SCAN_COMPLETE ||
	    scan_state == WLAN_SCAN_CANCELLED ||
	    scan_state == WLAN_SCAN_FAILED;
	parsed->ssid_found = any_found;
	parsed->ssid_supported = any_supported;

	/* Reports a fully parsed list result. */
	return 0;
}

/* Finds one supported primitive operation. */
static const struct networkd_wifi_operation *
find_operation(
	const char *name)
{
	size_t index;

	/* Matches only complete fixed operation names. */
	if (name == NULL)
		return NULL;
	for (index = 0U;
	     index < sizeof(networkd_wifi_operations) /
	     sizeof(networkd_wifi_operations[0]);
	     index++) {
		if (strcmp(name, networkd_wifi_operations[index].name) == 0)
			return &networkd_wifi_operations[index];
	}

	/* Reports an unsupported operation. */
	return NULL;
}

/* Validates one complete invocation before creating any descriptors. */
static int
validate_arguments(
	const char *interface,
	const struct networkd_wifi_operation *operation,
	const void *ssid,
	size_t ssid_length,
	const void *passphrase,
	size_t passphrase_length,
	unsigned timeout_seconds)
{
	const unsigned char *ssid_bytes;
	const unsigned char *secret_bytes;
	size_t interface_length;
	size_t index;

	/* Validates fixed identifiers and the finite stage deadline. */
	if (interface == NULL || operation == NULL || timeout_seconds == 0U ||
	    timeout_seconds > NETWORKD_WIFI_CHILD_TIMEOUT_MAX)
		return EINVAL;
	interface_length = strnlen(interface, IFNAMSIZ);
	if (interface_length == 0U || interface_length >= IFNAMSIZ)
		return EINVAL;

	/* Rejects control, separator, and pathname bytes in the interface name. */
	for (index = 0U; index < interface_length; index++) {
		if ((unsigned char)interface[index] <= 32U ||
		    (unsigned char)interface[index] == 127U ||
		    interface[index] == '/')
			return EINVAL;
	}

	/* Validates the operation-specific counted values. */
	if (operation->kind != NETWORKD_WIFI_OPERATION_CONNECT) {
		if (ssid != NULL || ssid_length != 0U || passphrase != NULL ||
		    passphrase_length != 0U)
			return EINVAL;
		return 0;
	}
	if (ssid == NULL || ssid_length == 0U || ssid_length > WLAN_SSID_MAX ||
	    passphrase == NULL || passphrase_length < WLAN_PASSPHRASE_MIN ||
	    passphrase_length > WLAN_PASSPHRASE_MAX)
		return EINVAL;

	/* Ensures argv can represent the SSID and WPA2 can accept the secret. */
	ssid_bytes = ssid;
	secret_bytes = passphrase;
	for (index = 0U; index < ssid_length; index++) {
		if (ssid_bytes[index] == 0U)
			return EINVAL;
	}
	for (index = 0U; index < passphrase_length; index++) {
		if (secret_bytes[index] < 0x20U || secret_bytes[index] > 0x7eU)
			return EINVAL;
	}

	/* Reports valid bounded arguments. */
	return 0;
}

/* Clears bytes through a volatile view. */
static void
clear_bytes(
	void *storage,
	size_t length)
{
	volatile unsigned char *bytes;

	/* Clears every byte without permitting dead-store removal. */
	bytes = storage;
	while (length != 0U) {
		*bytes++ = 0U;
		length--;
	}
}

/* Initializes all descriptors and process fields to inactive values. */
static void
initialize_state(
	struct networkd_wifi_child_state *state)
{
	/* Initializes the complete private process state. */
	memset(state, 0, sizeof(*state));
	state->secret_pipe[0] = -1;
	state->secret_pipe[1] = -1;
	state->output_pipe[0] = -1;
	state->output_pipe[1] = -1;
	state->diagnostic_pipe[0] = -1;
	state->diagnostic_pipe[1] = -1;
	state->child = -1;
}

/* Closes one owned descriptor exactly once. */
static void
close_descriptor(
	int *descriptor)
{
	/* Retires an active descriptor. */
	if (*descriptor >= 0) {
		(void)close(*descriptor);
		*descriptor = -1;
	}
}

/* Closes every pipe descriptor still owned by this invocation. */
static void
close_pipes(
	struct networkd_wifi_child_state *state)
{
	/* Retires all channel ends in fixed ownership order. */
	close_descriptor(&state->secret_pipe[0]);
	close_descriptor(&state->secret_pipe[1]);
	close_descriptor(&state->output_pipe[0]);
	close_descriptor(&state->output_pipe[1]);
	close_descriptor(&state->diagnostic_pipe[0]);
	close_descriptor(&state->diagnostic_pipe[1]);
}

/* Opens one close-on-exec pipe. */
static int
open_pipe(
	int descriptors[2])
{
	/* Creates and marks both pipe ends close-on-exec. */
	if (pipe(descriptors) != 0)
		return -1;
	if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) != 0) {
		close_descriptor(&descriptors[0]);
		close_descriptor(&descriptors[1]);
		return -1;
	}

	/* Reports successful channel creation. */
	return 0;
}

/* Opens the secret, machine-output, and diagnostic channels. */
static int
open_pipes(
	struct networkd_wifi_child_state *state)
{
	/* Creates each independently owned pipe. */
	if (open_pipe(state->secret_pipe) != 0)
		return -1;
	if (open_pipe(state->output_pipe) != 0)
		return -1;
	if (open_pipe(state->diagnostic_pipe) != 0)
		return -1;

	/* Reports successful channel creation. */
	return 0;
}

/* Adds nonblocking mode to one parent endpoint. */
static int
set_nonblocking(
	int descriptor)
{
	int flags;

	/* Reads and updates only the status flags for this endpoint. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0)
		return -1;
	if (fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
		return -1;

	/* Reports successful endpoint preparation. */
	return 0;
}

/* Retains and prepares only the parent-owned pipe endpoints. */
static int
prepare_parent_pipes(
	struct networkd_wifi_child_state *state,
	int has_secret)
{
	int saved_error;

	/* Closes every endpoint used exclusively by the child. */
	close_descriptor(&state->secret_pipe[0]);
	close_descriptor(&state->output_pipe[1]);
	close_descriptor(&state->diagnostic_pipe[1]);
	if (!has_secret) {
		close_descriptor(&state->secret_pipe[1]);
		clear_bytes(state->secret, sizeof(state->secret));
	}

	/* Makes every active parent endpoint safe for the common poll loop. */
	if ((state->secret_pipe[1] >= 0 &&
	    set_nonblocking(state->secret_pipe[1]) != 0) ||
	    set_nonblocking(state->output_pipe[0]) != 0 ||
	    set_nonblocking(state->diagnostic_pipe[0]) != 0) {
		saved_error = errno != 0 ? errno : EIO;
		errno = saved_error;
		return saved_error;
	}

	/* Reports successful parent endpoint preparation. */
	return 0;
}

/* Duplicates one child source away from standard and reserved descriptors. */
static int
duplicate_child_descriptor(
	int descriptor)
{
	int duplicate;

	/* Creates an independent source above all fixed child descriptors. */
	duplicate = fcntl(descriptor, F_DUPFD, 10);
	if (duplicate < 0)
		return -1;

	/* Returns the safe duplicate. */
	return duplicate;
}

/* Closes every child endpoint before reporting a setup failure. */
static void
exit_child_setup_failure(
	int secret_source,
	int output_source,
	int diagnostic_source)
{
	/* Retires each duplicated source and any installed destination. */
	if (secret_source >= 0)
		(void)close(secret_source);
	if (output_source >= 0)
		(void)close(output_source);
	if (diagnostic_source >= 0)
		(void)close(diagnostic_source);
	(void)close(NETWORKD_WIFI_SECRET_DESCRIPTOR);
	(void)close(STDOUT_FILENO);
	(void)close(STDERR_FILENO);

	/* Terminates without running inherited process cleanup. */
	_exit(126);
}

/* Builds and executes one absolute shell-free child invocation. */
static void
execute_child(
	struct networkd_wifi_child_state *state,
	const struct networkd_wifi_operation *operation,
	const char *interface,
	const char *ssid)
{
	char *arguments[8];
	int secret_source;
	int output_source;
	int diagnostic_source;
	int duplicate_result;
	int index;

	/* Duplicates every source before fixed descriptor replacement. */
	secret_source = -1;
	if (operation->kind == NETWORKD_WIFI_OPERATION_CONNECT)
		secret_source = duplicate_child_descriptor(state->secret_pipe[0]);
	output_source = duplicate_child_descriptor(state->output_pipe[1]);
	diagnostic_source = duplicate_child_descriptor(state->diagnostic_pipe[1]);
	if ((operation->kind == NETWORKD_WIFI_OPERATION_CONNECT &&
	    secret_source < 0) || output_source < 0 || diagnostic_source < 0)
		exit_child_setup_failure(secret_source, output_source,
		    diagnostic_source);

	/* Closes inherited channel ends and reserved init readiness descriptor. */
	close_pipes(state);
	if (fcntl(3, F_GETFD) >= 0)
		(void)close(3);
	if (secret_source < 0 &&
	    fcntl(NETWORKD_WIFI_SECRET_DESCRIPTOR, F_GETFD) >= 0)
		(void)close(NETWORKD_WIFI_SECRET_DESCRIPTOR);

	/* Installs only the three documented child channels. */
	if (secret_source >= 0) {
		duplicate_result = dup2(secret_source,
		    NETWORKD_WIFI_SECRET_DESCRIPTOR);
		if (duplicate_result < 0)
			exit_child_setup_failure(secret_source, output_source,
			    diagnostic_source);
	}
	duplicate_result = dup2(output_source, STDOUT_FILENO);
	if (duplicate_result < 0)
		exit_child_setup_failure(secret_source, output_source,
		    diagnostic_source);
	duplicate_result = dup2(diagnostic_source, STDERR_FILENO);
	if (duplicate_result < 0)
		exit_child_setup_failure(secret_source, output_source,
		    diagnostic_source);
	if (secret_source >= 0)
		(void)close(secret_source);
	(void)close(output_source);
	(void)close(diagnostic_source);

	/* Builds the fixed argv shape without ever inserting the passphrase. */
	index = 0;
	arguments[index++] = (char *)NETWORKD_WIFI_PATH;
	arguments[index++] = "--machine";
	if (operation->kind == NETWORKD_WIFI_OPERATION_CONNECT)
		arguments[index++] = "--passphrase-fd=4";
	arguments[index++] = (char *)interface;
	arguments[index++] = (char *)operation->argument;
	if (operation->kind == NETWORKD_WIFI_OPERATION_SEARCH)
		arguments[index++] = (char *)operation->subargument;
	if (operation->kind == NETWORKD_WIFI_OPERATION_CONNECT)
		arguments[index++] = (char *)ssid;
	arguments[index] = NULL;

	/* Replaces the child with the absolute primitive executable. */
	execv(NETWORKD_WIFI_PATH, arguments);
	(void)close(STDOUT_FILENO);
	(void)close(STDERR_FILENO);
	if (secret_source >= 0)
		(void)close(NETWORKD_WIFI_SECRET_DESCRIPTOR);
	_exit(127);
}

/* Computes one finite absolute monotonic deadline. */
static int
monotonic_deadline(
	struct timespec *deadline,
	unsigned seconds)
{
	/* Reads the monotonic time and adds the bounded duration. */
	if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
		return -1;
	deadline->tv_sec += (time_t)seconds;

	/* Reports successful deadline construction. */
	return 0;
}

/* Computes a ceil-rounded finite poll interval for one deadline. */
static int
remaining_milliseconds(
	const struct timespec *deadline,
	int *milliseconds)
{
	struct timespec now;
	time_t seconds;
	long nanoseconds;
	uint64_t result;

	/* Reads the current monotonic time. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;

	/* Recognizes an expired deadline before unsigned conversion. */
	if (now.tv_sec > deadline->tv_sec ||
	    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
		*milliseconds = 0;
		return 0;
	}

	/* Normalizes the positive difference and rounds it up to milliseconds. */
	seconds = deadline->tv_sec - now.tv_sec;
	nanoseconds = deadline->tv_nsec - now.tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}
	result = (uint64_t)seconds * 1000U;
	result += ((uint64_t)nanoseconds + 999999U) / 1000000U;
	*milliseconds = result > (uint64_t)INT_MAX ? INT_MAX : (int)result;

	/* Reports successful interval calculation. */
	return 0;
}

/* Drains bounded machine output and counts complete records. */
static int
append_output(
	struct networkd_wifi_child_state *state,
	struct networkd_wifi_child_result *result,
	int *end_of_file)
{
	unsigned char temporary[NETWORKD_WIFI_CHILD_COPY_SIZE];
	ssize_t count;
	size_t available;
	size_t copied;
	size_t index;
	int function_result;

	function_result = 0;
	*end_of_file = 0;

	/* Drains one bounded chunk so a continuous writer cannot hide deadline. */
	while (state->output_pipe[0] >= 0) {
		count = read(state->output_pipe[0], temporary, sizeof(temporary));
		if (count > 0) {
			available = NETWORKD_WIFI_CHILD_OUTPUT_MAX -
			    result->output_length;
			copied = (size_t)count < available ? (size_t)count :
			    available;
			if (copied != 0U) {
				memcpy(result->output + result->output_length,
				    temporary, copied);
				result->output_length += copied;
			}
			for (index = 0U; index < (size_t)count; index++) {
				if (temporary[index] == '\n')
					result->output_records++;
			}
			if (copied != (size_t)count || result->output_records >
			    NETWORKD_WIFI_CHILD_RECORD_MAX)
				function_result = EOVERFLOW;
			break;
		}
		if (count == 0) {
			close_descriptor(&state->output_pipe[0]);
			*end_of_file = 1;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		function_result = errno != 0 ? errno : EIO;
		close_descriptor(&state->output_pipe[0]);
		break;
	}
	clear_bytes(temporary, sizeof(temporary));

	/* Returns the first bounded drain failure. */
	return function_result;
}

/* Drains and bounds the separate child diagnostic channel. */
static int
append_diagnostic(
	struct networkd_wifi_child_state *state,
	struct networkd_wifi_child_result *result,
	int *end_of_file)
{
	unsigned char temporary[NETWORKD_WIFI_CHILD_COPY_SIZE];
	ssize_t count;
	size_t available;
	size_t copied;
	int function_result;

	function_result = 0;
	*end_of_file = 0;

	/* Drains one bounded chunk so a continuous writer cannot hide deadline. */
	while (state->diagnostic_pipe[0] >= 0) {
		count = read(state->diagnostic_pipe[0], temporary,
		    sizeof(temporary));
		if (count > 0) {
			available = NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX -
			    result->diagnostic_length;
			copied = (size_t)count < available ? (size_t)count :
			    available;
			if (copied != 0U) {
				memcpy(result->diagnostic +
				    result->diagnostic_length, temporary, copied);
				result->diagnostic_length += copied;
				result->diagnostic[result->diagnostic_length] = '\0';
			}
			if (copied != (size_t)count)
				function_result = EOVERFLOW;
			break;
		}
		if (count == 0) {
			close_descriptor(&state->diagnostic_pipe[0]);
			*end_of_file = 1;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		function_result = errno != 0 ? errno : EIO;
		close_descriptor(&state->diagnostic_pipe[0]);
		break;
	}
	clear_bytes(temporary, sizeof(temporary));

	/* Returns the first bounded drain failure. */
	return function_result;
}

/* Writes the exact counted secret and closes the channel at EOF. */
static int
write_secret(
	struct networkd_wifi_child_state *state)
{
	ssize_t count;

	/* Writes every currently accepted secret byte without blocking. */
	while (state->secret_pipe[1] >= 0 &&
	    state->secret_offset < state->secret_length) {
		count = write(state->secret_pipe[1],
		    state->secret + state->secret_offset,
		    state->secret_length - state->secret_offset);
		if (count > 0) {
			state->secret_offset += (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count < 0 && errno == EPIPE)
			return EPIPE;
		return errno != 0 ? errno : EIO;
	}

	/* Publishes EOF and clears the local secret immediately after delivery. */
	if (state->secret_pipe[1] >= 0 &&
	    state->secret_offset == state->secret_length) {
		close_descriptor(&state->secret_pipe[1]);
	}

	/* Reports successful secret progress. */
	return 0;
}

/* Reaps the child with the selected wait behavior. */
static int
reap_child(
	struct networkd_wifi_child_state *state,
	int options)
{
	pid_t waited;

	/* Recognizes an already reaped process. */
	if (state->child_reaped)
		return 0;

	/* Retries an interrupted wait without changing the surrounding deadline. */
	do {
		waited = waitpid(state->child, &state->child_status, options);
	} while (waited < 0 && errno == EINTR);
	if (waited == state->child) {
		state->child_reaped = 1;
		return 0;
	}
	if (waited == 0)
		return EAGAIN;

	/* Reports an unrecoverable wait failure. */
	return errno != 0 ? errno : ECHILD;
}

/* Polls all child channels until clean exit or the absolute deadline. */
static int
poll_child(
	struct networkd_wifi_child_state *state,
	struct networkd_wifi_child_result *result,
	const struct timespec *deadline)
{
	struct pollfd descriptors[3];
	int milliseconds;
	int poll_result;
	int function_result;
	int drain_result;
	int end_of_file;

	function_result = 0;

	/* Advances all process and channel state without extending the deadline. */
	while (!state->child_reaped || state->output_pipe[0] >= 0 ||
	    state->diagnostic_pipe[0] >= 0) {
		drain_result = append_output(state, result, &end_of_file);
		if (drain_result != 0 && function_result == 0)
			function_result = drain_result;
		drain_result = append_diagnostic(state, result, &end_of_file);
		if (drain_result != 0 && function_result == 0)
			function_result = drain_result;
		if (state->secret_pipe[1] >= 0) {
			drain_result = write_secret(state);
			if (drain_result != 0 && function_result == 0)
				function_result = drain_result;
		}
		drain_result = reap_child(state, WNOHANG);
		if (drain_result != 0 && drain_result != EAGAIN &&
		    function_result == 0)
			function_result = drain_result;
		if (function_result != 0)
			break;
		if (state->child_reaped && state->output_pipe[0] < 0 &&
		    state->diagnostic_pipe[0] < 0)
			break;

		/* Computes the sole remaining operation interval. */
		if (remaining_milliseconds(deadline, &milliseconds) != 0) {
			function_result = errno != 0 ? errno : EIO;
			break;
		}
		if (milliseconds == 0) {
			function_result = ETIMEDOUT;
			break;
		}

		/* Waits for any channel transition under the finite interval. */
		descriptors[0].fd = state->secret_pipe[1];
		descriptors[0].events = state->secret_pipe[1] >= 0 ? POLLOUT : 0;
		descriptors[0].revents = 0;
		descriptors[1].fd = state->output_pipe[0];
		descriptors[1].events = state->output_pipe[0] >= 0 ? POLLIN : 0;
		descriptors[1].revents = 0;
		descriptors[2].fd = state->diagnostic_pipe[0];
		descriptors[2].events = state->diagnostic_pipe[0] >= 0 ? POLLIN : 0;
		descriptors[2].revents = 0;
		poll_result = poll(descriptors, 3U, milliseconds);
		if (poll_result < 0 && errno != EINTR) {
			function_result = errno != 0 ? errno : EIO;
			break;
		}
	}

	/* Terminates and reaps every unsuccessful invocation. */
	if (function_result != 0) {
		result->terminal_error = function_result;
		terminate_child(state, result);
	}

	/* Returns the complete monitoring result. */
	return function_result;
}

/* Gives one failed child a finite SIGTERM grace before SIGKILL and reap. */
static void
terminate_child(
	struct networkd_wifi_child_state *state,
	struct networkd_wifi_child_result *result)
{
	struct timespec grace_deadline;
	struct pollfd descriptors[2];
	int milliseconds;
	int poll_result;
	int drain_result;
	int end_of_file;

	/* Stops secret delivery before requesting child termination. */
	close_descriptor(&state->secret_pipe[1]);
	if (!state->child_reaped)
		(void)kill(state->child, SIGTERM);

	/* Drains both streams while allowing exactly one second of grace. */
	if (!state->child_reaped && monotonic_deadline(&grace_deadline,
	    NETWORKD_WIFI_CHILD_GRACE_SECONDS) == 0) {
		while (!state->child_reaped) {
			drain_result = append_output(state, result, &end_of_file);
			if (drain_result != 0 && result->terminal_error == 0)
				result->terminal_error = drain_result;
			drain_result = append_diagnostic(state, result,
			    &end_of_file);
			if (drain_result != 0 && result->terminal_error == 0)
				result->terminal_error = drain_result;
			drain_result = reap_child(state, WNOHANG);
			if (drain_result == 0)
				break;
			if (drain_result != EAGAIN)
				break;
			if (remaining_milliseconds(&grace_deadline,
			    &milliseconds) != 0 || milliseconds == 0)
				break;
			descriptors[0].fd = state->output_pipe[0];
			descriptors[0].events = state->output_pipe[0] >= 0 ?
			    POLLIN : 0;
			descriptors[0].revents = 0;
			descriptors[1].fd = state->diagnostic_pipe[0];
			descriptors[1].events = state->diagnostic_pipe[0] >= 0 ?
			    POLLIN : 0;
			descriptors[1].revents = 0;
			poll_result = poll(descriptors, 2U, milliseconds);
			if (poll_result < 0 && errno != EINTR)
				break;
		}
	}

	/* Forces retirement after grace and performs the mandatory reap. */
	if (!state->child_reaped) {
		(void)kill(state->child, SIGKILL);
		(void)reap_child(state, 0);
	}
	(void)append_output(state, result, &end_of_file);
	(void)append_diagnostic(state, result, &end_of_file);
	close_descriptor(&state->output_pipe[0]);
	close_descriptor(&state->diagnostic_pipe[0]);
}

/* Discards all child output and retires one child under a finite grace. */
static void
discard_child(
	struct networkd_wifi_child_state *state)
{
	struct timespec grace_deadline;
	struct timespec delay;
	int milliseconds;
	int reap_result;

	/* Makes disclosure impossible before requesting child termination. */
	close_pipes(state);
	if (!state->child_reaped)
		(void)kill(state->child, SIGTERM);

	/* Gives the child a bounded grace without reading either output stream. */
	if (!state->child_reaped && monotonic_deadline(&grace_deadline,
	    NETWORKD_WIFI_CHILD_GRACE_SECONDS) == 0) {
		while (!state->child_reaped) {
			reap_result = reap_child(state, WNOHANG);
			if (reap_result != EAGAIN)
				break;
			if (remaining_milliseconds(&grace_deadline,
			    &milliseconds) != 0 || milliseconds == 0)
				break;
			delay.tv_sec = 0;
			delay.tv_nsec = milliseconds < 10 ?
			    (long)milliseconds * 1000000L : 10000000L;
			while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
				;
		}
	}

	/* Forces and synchronously reaps a child which ignored the grace. */
	if (!state->child_reaped) {
		(void)kill(state->child, SIGKILL);
		(void)reap_child(state, 0);
	}
}

/* Selects one complete newline-terminated machine-output record. */
static int
next_output_record(
	const struct networkd_wifi_child_result *result,
	size_t *offset,
	const unsigned char **record,
	size_t *record_length)
{
	const unsigned char *newline;
	size_t remaining;

	/* Validates the cursor and requires at least one remaining byte. */
	if (result == NULL || offset == NULL || record == NULL ||
	    record_length == NULL || *offset >= result->output_length)
		return NETWORKD_WIFI_CHILD_MALFORMED;

	/* Finds the exact end of this counted record. */
	remaining = result->output_length - *offset;
	newline = memchr(result->output + *offset, '\n', remaining);
	if (newline == NULL)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	*record = result->output + *offset;
	*record_length = (size_t)(newline - *record) + 1U;
	*offset += *record_length;

	/* Reports one complete record view. */
	return 0;
}

/* Consumes one exact fixed machine-record token. */
static int
record_consume(
	struct networkd_wifi_record_cursor *cursor,
	const char *literal)
{
	size_t literal_length;

	/* Validates the cursor and computes the fixed token length. */
	if (cursor == NULL || literal == NULL || cursor->bytes == NULL ||
	    cursor->offset > cursor->length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	literal_length = strlen(literal);

	/* Requires the complete token at the current record position. */
	if (literal_length > cursor->length - cursor->offset ||
	    memcmp(cursor->bytes + cursor->offset, literal,
	    literal_length) != 0)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor->offset += literal_length;

	/* Reports exact token consumption. */
	return 0;
}

/* Parses one canonical unsigned decimal followed by its delimiter. */
static int
record_unsigned(
	struct networkd_wifi_record_cursor *cursor,
	unsigned char delimiter,
	uint64_t maximum,
	uint64_t *value)
{
	uint64_t parsed;
	uint64_t digit;
	size_t start;

	/* Validates the output and records the first decimal position. */
	if (cursor == NULL || value == NULL || cursor->bytes == NULL ||
	    cursor->offset >= cursor->length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	start = cursor->offset;
	parsed = 0U;

	/* Accumulates every digit without crossing the supplied bound. */
	while (cursor->offset < cursor->length &&
	    cursor->bytes[cursor->offset] != delimiter) {
		if (cursor->bytes[cursor->offset] < '0' ||
		    cursor->bytes[cursor->offset] > '9')
			return NETWORKD_WIFI_CHILD_MALFORMED;
		digit = (uint64_t)(cursor->bytes[cursor->offset] - '0');
		if (digit > maximum || parsed > (maximum - digit) / 10U)
			return NETWORKD_WIFI_CHILD_MALFORMED;
		parsed = parsed * 10U + digit;
		cursor->offset++;
	}

	/* Rejects empty, padded, unterminated, or excessive values. */
	if (cursor->offset == start || cursor->offset >= cursor->length ||
	    (cursor->offset - start > 1U && cursor->bytes[start] == '0') ||
	    parsed > maximum)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor->offset++;
	*value = parsed;

	/* Reports one canonical decimal value. */
	return 0;
}

/* Parses one canonical signed int followed by its delimiter. */
static int
record_signed(
	struct networkd_wifi_record_cursor *cursor,
	unsigned char delimiter,
	int *value)
{
	uint64_t maximum;
	uint64_t magnitude;
	int negative;
	int function_result;

	/* Validates and consumes the optional minus sign. */
	if (cursor == NULL || value == NULL || cursor->bytes == NULL ||
	    cursor->offset >= cursor->length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	negative = cursor->bytes[cursor->offset] == '-';
	if (negative)
		cursor->offset++;
	maximum = negative ? (uint64_t)INT_MAX + 1U : (uint64_t)INT_MAX;

	/* Parses the canonical magnitude under the signed bound. */
	function_result = record_unsigned(
		cursor,
		delimiter,
		maximum,
		&magnitude);
	if (function_result != 0)
		return function_result;

	/* Rejects negative zero and publishes the bounded signed value. */
	if (negative && magnitude == 0U)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	if (negative && magnitude == (uint64_t)INT_MAX + 1U)
		*value = INT_MIN;
	else if (negative)
		*value = -(int)magnitude;
	else
		*value = (int)magnitude;

	/* Reports one canonical signed value. */
	return 0;
}

/* Decodes one canonical lower-case hexadecimal digit. */
static int
lower_hex_value(
	unsigned char byte,
	unsigned *value)
{
	/* Rejects a missing output before decoding the byte. */
	if (value == NULL)
		return NETWORKD_WIFI_CHILD_MALFORMED;

	/* Maps only the lower-case hexadecimal alphabet. */
	if (byte >= '0' && byte <= '9') {
		*value = (unsigned)(byte - '0');
		return 0;
	}
	if (byte >= 'a' && byte <= 'f') {
		*value = (unsigned)(byte - 'a') + 10U;
		return 0;
	}

	/* Reports a noncanonical hexadecimal digit. */
	return NETWORKD_WIFI_CHILD_MALFORMED;
}

/* Consumes one exact-width lower-case hexadecimal field. */
static int
record_fixed_hex(
	struct networkd_wifi_record_cursor *cursor,
	size_t digits,
	unsigned char delimiter)
{
	unsigned value;
	size_t index;
	int function_result;

	/* Requires the complete digit field and its exact delimiter. */
	if (cursor == NULL || cursor->bytes == NULL ||
	    cursor->offset > cursor->length ||
	    digits >= cursor->length - cursor->offset)
		return NETWORKD_WIFI_CHILD_MALFORMED;

	/* Validates every digit without retaining the decoded value. */
	for (index = 0U; index < digits; index++) {
		function_result = lower_hex_value(
			cursor->bytes[cursor->offset + index],
			&value);
		if (function_result != 0)
			return function_result;
	}
	if (cursor->bytes[cursor->offset + digits] != delimiter)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor->offset += digits + 1U;

	/* Reports exact hexadecimal field consumption. */
	return 0;
}

/* Parses one exact-width lower-case hexadecimal uint32 value. */
static int
record_u32_hex(
	struct networkd_wifi_record_cursor *cursor,
	unsigned char delimiter,
	uint32_t *value)
{
	uint32_t parsed;
	unsigned digit;
	size_t index;
	int function_result;

	/* Requires eight complete digits, the delimiter, and an output object. */
	if (cursor == NULL || cursor->bytes == NULL || value == NULL ||
	    cursor->offset > cursor->length ||
	    8U >= cursor->length - cursor->offset)
		return NETWORKD_WIFI_CHILD_MALFORMED;

	/* Accumulates all eight nibbles without signed conversion or overflow. */
	parsed = 0U;
	for (index = 0U; index < 8U; index++) {
		function_result = lower_hex_value(
			cursor->bytes[cursor->offset + index],
			&digit);
		if (function_result != 0)
			return function_result;
		parsed = (parsed << 4U) | (uint32_t)digit;
	}
	if (cursor->bytes[cursor->offset + 8U] != delimiter)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor->offset += 9U;
	*value = parsed;

	/* Reports one canonical hexadecimal uint32 value. */
	return 0;
}

/* Compares one variable-width hexadecimal SSID with counted bytes. */
static int
record_ssid_hex(
	struct networkd_wifi_record_cursor *cursor,
	const unsigned char *ssid,
	size_t ssid_length,
	int *matches)
{
	size_t start;
	size_t digits;
	size_t decoded_length;
	size_t index;
	unsigned high;
	unsigned low;
	unsigned byte;
	int function_result;

	/* Finds the space which terminates the hexadecimal SSID field. */
	if (cursor == NULL || cursor->bytes == NULL || ssid == NULL ||
	    matches == NULL || cursor->offset > cursor->length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	start = cursor->offset;
	while (cursor->offset < cursor->length &&
	    cursor->bytes[cursor->offset] != ' ')
		cursor->offset++;
	if (cursor->offset >= cursor->length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	digits = cursor->offset - start;
	if ((digits & 1U) != 0U || digits > WLAN_SSID_MAX * 2U)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	decoded_length = digits / 2U;
	*matches = decoded_length == ssid_length;

	/* Decodes and compares each byte without creating a retained copy. */
	for (index = 0U; index < decoded_length; index++) {
		function_result = lower_hex_value(
			cursor->bytes[start + index * 2U],
			&high);
		if (function_result != 0)
			return function_result;
		function_result = lower_hex_value(
			cursor->bytes[start + index * 2U + 1U],
			&low);
		if (function_result != 0)
			return function_result;
		byte = high * 16U + low;
		if (*matches && byte != ssid[index])
			*matches = 0;
	}
	cursor->offset++;

	/* Reports one canonical SSID field and its exact comparison. */
	return 0;
}

/* Parses the sole leading machine-mode scan record. */
static int
parse_scan_record(
	const unsigned char *record,
	size_t record_length,
	uint32_t *scan_state,
	unsigned *bss_count)
{
	struct networkd_wifi_record_cursor cursor;
	uint64_t state;
	uint64_t generation;
	uint64_t results;
	uint64_t available;
	uint64_t truncated;
	uint64_t terminal_error;
	int function_result;

	/* Initializes a counted cursor over exactly one record. */
	if (record == NULL || scan_state == NULL || bss_count == NULL)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor.bytes = record;
	cursor.length = record_length;
	cursor.offset = 0U;

	/* Parses every fixed scan field in its canonical order. */
	function_result = record_consume(&cursor, "WIFI1 scan state=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', WLAN_SCAN_FAILED,
	    &state);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "generation=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', UINT64_MAX,
	    &generation);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "results=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', WLAN_BSS_MAX, &results);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "available=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', WLAN_BSS_MAX,
	    &available);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "truncated=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', 1U, &truncated);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "error=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, '\n', INT_MAX,
	    &terminal_error);
	if (function_result != 0)
		return function_result;

	/* Rejects impossible list counts and unsuccessful scan snapshots. */
	if (cursor.offset != cursor.length ||
	    results > NETWORKD_WIFI_CHILD_RECORD_MAX - 2U ||
	    results > available ||
	    (results < available && truncated == 0U) ||
	    (results != 0U && generation == 0U) ||
	    state == WLAN_SCAN_FAILED ||
	    (state != WLAN_SCAN_CANCELLED && terminal_error != 0U))
		return NETWORKD_WIFI_CHILD_MALFORMED;
	*scan_state = (uint32_t)state;
	*bss_count = (unsigned)results;

	/* Reports one complete canonical scan record. */
	return 0;
}

/* Parses one canonical BSS record and compares its counted SSID. */
static int
parse_bss_record(
	const unsigned char *record,
	size_t record_length,
	unsigned expected_index,
	const unsigned char *ssid,
	size_t ssid_length,
	int *matches,
	int *supported)
{
	struct networkd_wifi_record_cursor cursor;
	const uint32_t required = WLAN_SECURITY_PRIVACY |
	    WLAN_SECURITY_WPA2 | WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
	const uint32_t rejected = WLAN_SECURITY_WPA1 |
	    WLAN_SECURITY_PMF_REQUIRED | WLAN_SECURITY_UNSUPPORTED_SUITE;
	uint64_t index;
	uint64_t value;
	uint32_t security;
	int rssi;
	int function_result;

	/* Initializes a counted cursor over exactly one BSS record. */
	if (record == NULL || ssid == NULL || matches == NULL ||
	    supported == NULL)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	cursor.bytes = record;
	cursor.length = record_length;
	cursor.offset = 0U;

	/* Parses every fixed BSS field in its canonical order. */
	function_result = record_consume(&cursor, "WIFI1 bss index=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', UINT32_MAX, &index);
	if (function_result != 0 || index != expected_index)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	function_result = record_consume(&cursor, "ssid=");
	if (function_result != 0)
		return function_result;
	function_result = record_ssid_hex(&cursor, ssid, ssid_length, matches);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "bssid=");
	if (function_result != 0)
		return function_result;
	function_result = record_fixed_hex(&cursor, 12U, ' ');
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "channel=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', UINT8_MAX, &value);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "frequency=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', UINT32_MAX, &value);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "rssi=");
	if (function_result != 0)
		return function_result;
	function_result = record_signed(&cursor, ' ', &rssi);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "age=");
	if (function_result != 0)
		return function_result;
	function_result = record_unsigned(&cursor, ' ', UINT32_MAX, &value);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "security=");
	if (function_result != 0)
		return function_result;
	function_result = record_u32_hex(&cursor, ' ', &security);
	if (function_result != 0)
		return function_result;
	function_result = record_consume(&cursor, "flags=");
	if (function_result != 0)
		return function_result;
	function_result = record_fixed_hex(&cursor, 8U, '\n');
	if (function_result != 0)
		return function_result;

	/* Requires every byte to belong to the canonical BSS record. */
	if (cursor.offset != cursor.length)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	*supported = *matches && (security & required) == required &&
	    (security & rejected) == 0U;

	/* Reports one complete canonical BSS record. */
	return 0;
}

/* Validates complete bounded WIFI1 records after the child closes stdout. */
static int
validate_output(
	const struct networkd_wifi_child_result *result,
	int *terminal_error)
{
	static const unsigned char terminal_prefix[] = "WIFI1 terminal ";
	static const unsigned char terminal_ok[] = "WIFI1 terminal ok 0\n";
	static const unsigned char terminal_error_prefix[] =
	    "WIFI1 terminal error ";
	size_t offset;
	size_t line_start;
	size_t line_length;
	unsigned terminal_count;
	int parsed_error;

	/* Requires at least one complete versioned machine record. */
	if (terminal_error == NULL || result->output_length == 0U ||
	    result->output_records == 0U ||
	    result->output_records > NETWORKD_WIFI_CHILD_RECORD_MAX ||
	    result->output[result->output_length - 1U] != '\n')
		return NETWORKD_WIFI_CHILD_MALFORMED;
	*terminal_error = -1;
	terminal_count = 0U;

	/* Validates each prefix and accepts one final canonical terminal record. */
	line_start = 0U;
	for (offset = 0U; offset < result->output_length; offset++) {
		if (result->output[offset] != '\n')
			continue;
		line_length = offset - line_start + 1U;
		if (line_length <
		    sizeof(NETWORKD_WIFI_RECORD_PREFIX) - 1U ||
		    memcmp(result->output + line_start,
		    NETWORKD_WIFI_RECORD_PREFIX,
		    sizeof(NETWORKD_WIFI_RECORD_PREFIX) - 1U) != 0)
			return NETWORKD_WIFI_CHILD_MALFORMED;
		if (line_length >= sizeof(terminal_prefix) - 1U &&
		    memcmp(result->output + line_start, terminal_prefix,
		    sizeof(terminal_prefix) - 1U) == 0) {
			terminal_count++;
			if (offset + 1U != result->output_length)
				return NETWORKD_WIFI_CHILD_MALFORMED;
			if (line_length == sizeof(terminal_ok) - 1U &&
			    memcmp(result->output + line_start, terminal_ok,
			    sizeof(terminal_ok) - 1U) == 0) {
				*terminal_error = 0;
			} else if (line_length >
			    sizeof(terminal_error_prefix) &&
			    memcmp(result->output + line_start,
			    terminal_error_prefix,
			    sizeof(terminal_error_prefix) - 1U) == 0) {
				if (parse_positive_decimal(result->output + line_start +
				    sizeof(terminal_error_prefix) - 1U,
				    line_length - sizeof(terminal_error_prefix) + 1U,
				    &parsed_error) != 0)
					return NETWORKD_WIFI_CHILD_MALFORMED;
				*terminal_error = parsed_error;
			} else {
				return NETWORKD_WIFI_CHILD_MALFORMED;
			}
		}
		line_start = offset + 1U;
	}
	if (terminal_count != 1U || *terminal_error < 0)
		return NETWORKD_WIFI_CHILD_MALFORMED;

	/* Reports a complete bounded record stream. */
	return 0;
}

/* Parses one canonical positive errno decimal followed by a newline. */
static int
parse_positive_decimal(
	const unsigned char *text,
	size_t length,
	int *value)
{
	unsigned long parsed;
	size_t index;

	/* Rejects an empty, signed, zero, padded, or unterminated spelling. */
	if (text == NULL || value == NULL || length < 2U ||
	    text[length - 1U] != '\n' || text[0] < '1' || text[0] > '9')
		return NETWORKD_WIFI_CHILD_MALFORMED;
	parsed = 0U;

	/* Accumulates each canonical decimal digit under the int bound. */
	for (index = 0U; index + 1U < length; index++) {
		if (text[index] < '0' || text[index] > '9')
			return NETWORKD_WIFI_CHILD_MALFORMED;
		if (parsed > ((unsigned long)INT_MAX -
		    (unsigned long)(text[index] - '0')) / 10U)
			return NETWORKD_WIFI_CHILD_MALFORMED;
		parsed = parsed * 10U + (unsigned long)(text[index] - '0');
	}
	if (parsed == 0U)
		return NETWORKD_WIFI_CHILD_MALFORMED;
	*value = (int)parsed;

	/* Reports one valid positive error number. */
	return 0;
}

/* Finds one exact byte string inside another counted byte string. */
static int
contains_bytes(
	const void *haystack,
	size_t haystack_length,
	const void *needle,
	size_t needle_length)
{
	const unsigned char *haystack_bytes;
	size_t offset;

	/* Rejects an empty or impossible search. */
	if (needle == NULL || needle_length == 0U || haystack == NULL ||
	    needle_length > haystack_length)
		return 0;
	haystack_bytes = haystack;

	/* Checks each complete candidate extent. */
	for (offset = 0U; offset <= haystack_length - needle_length; offset++) {
		if (memcmp(haystack_bytes + offset, needle, needle_length) == 0)
			return 1;
	}

	/* Reports that the byte string was absent. */
	return 0;
}

/* Normalizes bounded diagnostics into one safe printable string. */
static void
sanitize_diagnostic(
	struct networkd_wifi_child_result *result)
{
	size_t length;
	size_t index;

	/* Replaces every control byte before returning diagnostic text. */
	length = result->diagnostic_length;
	for (index = 0U; index < length; index++) {
		if ((unsigned char)result->diagnostic[index] < 32U ||
		    (unsigned char)result->diagnostic[index] == 127U)
			result->diagnostic[index] = ' ';
	}

	/* Removes trailing normalization padding. */
	while (length != 0U && result->diagnostic[length - 1U] == ' ')
		result->diagnostic[--length] = '\0';
	result->diagnostic_length = length;
}

/* Validates child exit, records, and secret-redaction boundaries. */
static int
finish_child(
	struct networkd_wifi_child_state *state,
	struct networkd_wifi_child_result *result,
	int prior_error)
{
	int machine_error;
	int output_error;
	int function_result;

	function_result = prior_error;
	sanitize_diagnostic(result);

	/* Completes a mandatory reap if monitoring ended with closed streams. */
	if (!state->child_reaped) {
		if (function_result == 0)
			function_result = ECHILD;
		terminate_child(state, result);
	}

	/* Publishes the independent child exit or signal status. */
	if (state->child_reaped && WIFEXITED(state->child_status)) {
		result->child_exit_status = WEXITSTATUS(state->child_status);
		if (function_result == 0 && result->child_exit_status == 127 &&
		    result->output_length == 0U)
			function_result = ENOENT;
		if (function_result == 0 && result->child_exit_status == 126 &&
		    result->output_length == 0U)
			function_result = EIO;
	} else if (state->child_reaped && WIFSIGNALED(state->child_status)) {
		result->child_term_signal = WTERMSIG(state->child_status);
		if (function_result == 0)
			function_result = EINTR;
	} else if (function_result == 0) {
		function_result = ECHILD;
	}

	/* Rejects any child attempt to disclose the secret on either channel. */
	if (contains_bytes(result->output, result->output_length, state->secret,
	    state->secret_length) || contains_bytes(result->diagnostic,
	    result->diagnostic_length, state->secret, state->secret_length)) {
		networkd_wifi_child_result_clear(result);
		(void)strcpy(result->diagnostic, "child output redacted");
		result->diagnostic_length = strlen(result->diagnostic);
		function_result = NETWORKD_WIFI_CHILD_MALFORMED;
	}

	/* Validates the terminal record against the independent exit status. */
	if (function_result == 0) {
		machine_error = -1;
		output_error = validate_output(result, &machine_error);
		if (output_error != 0) {
			function_result = output_error;
		} else if (result->child_exit_status == 0 && machine_error != 0) {
			function_result = NETWORKD_WIFI_CHILD_MALFORMED;
		} else if (result->child_exit_status != 0 && machine_error <= 0) {
			function_result = result->child_exit_status == 127 ?
			    ENOENT : NETWORKD_WIFI_CHILD_MALFORMED;
		} else if (result->child_exit_status != 0) {
			function_result = machine_error;
		}
	}

	/* Returns the complete terminal status. */
	return function_result;
}
