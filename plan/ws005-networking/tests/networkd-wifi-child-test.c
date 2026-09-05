/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the bounded networkd Wi-Fi child runner.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>

typedef void (*fixture_signal_handler)(int);

enum fixture_fault {
	FIXTURE_FAULT_NONE,
	FIXTURE_FAULT_PREPARE_PARENT,
	FIXTURE_FAULT_SIGNAL
};

int fixture_execv(const char *, char *const []);
int fixture_fcntl(int, int, ...);
fixture_signal_handler fixture_signal(int, fixture_signal_handler);

#define execv fixture_execv
#define fcntl fixture_fcntl
#define signal fixture_signal
#include "userland/base/networkd/wifi-child.c"
#undef execv
#undef fcntl
#undef signal

#include <stdio.h>

#define FIXTURE_SECRET "runner-test-secret"
#define FIXTURE_SECRET_LENGTH (sizeof(FIXTURE_SECRET) - 1U)

extern char **environ;

static enum fixture_fault fixture_fault;
static pid_t fixture_parent;
static int fixture_sync_pipe[2] = { -1, -1 };

static void fixture_fail(const char *);
static void fixture_expect(int, const char *);
static void fixture_write_all(int, const void *, size_t);
static void fixture_wait_for_early_output(void);
static void fixture_open_sync_pipe(void);
static void fixture_close_sync_pipe(void);
static void fixture_verify_invocation(const char *, char *const []);
static int fixture_child_main(const char *);
static void fixture_write_records(unsigned);
static void fixture_write_exact_output(size_t);
static void fixture_write_until_killed(void);
static int fixture_run(const char *, unsigned, struct networkd_wifi_child_result *);
static void fixture_set_list_output(struct networkd_wifi_child_result *, const char *);
static void fixture_expect_list_error(const char *, const char *);
static void fixture_expect_reaped(const char *);
static unsigned fixture_open_descriptor_count(void);
static double fixture_monotonic_seconds(void);
static void test_success_and_contract(void);
static void test_terminal_and_protocol_failures(void);
static void test_output_bounds(void);
static void test_diagnostic_bounds_and_redaction(void);
static void test_preloop_failure_redaction(void);
static void test_timeout_and_reaping(void);
static void test_argument_validation_and_simple_operation(void);
static void test_list_result_parser(void);

/* Runs either one exec fixture or the complete parent-side test suite. */
int
main(
	int argc,
	char **argv)
{
	/* Dispatches an exec-replaced child fixture. */
	if (argc == 3 && strcmp(argv[1], "--fixture-child") == 0)
		return fixture_child_main(argv[2]);

	/* Exercises every independent runner boundary. */
	test_success_and_contract();
	test_terminal_and_protocol_failures();
	test_output_bounds();
	test_diagnostic_bounds_and_redaction();
	test_preloop_failure_redaction();
	test_timeout_and_reaping();
	test_argument_validation_and_simple_operation();
	test_list_result_parser();

	/* Reports successful completion. */
	puts("networkd wifi child test: PASS");
	return 0;
}

/* Injects one selected parent-side fcntl failure after fork. */
int
fixture_fcntl(
	int descriptor,
	int command,
	...)
{
	va_list arguments;
	int argument;

	/* Fails the first parent nonblocking preparation after the child writes. */
	if (getpid() == fixture_parent &&
	    fixture_fault == FIXTURE_FAULT_PREPARE_PARENT &&
	    command == F_GETFL) {
		fixture_wait_for_early_output();
		fixture_fault = FIXTURE_FAULT_NONE;
		errno = EIO;
		return -1;
	}

	/* Forwards each fcntl spelling used by the production runner. */
	if (command == F_GETFL)
		return fcntl(descriptor, command);
	va_start(arguments, command);
	argument = va_arg(arguments, int);
	va_end(arguments);
	return fcntl(descriptor, command, argument);
}

/* Injects one selected parent-side SIGPIPE disposition failure. */
fixture_signal_handler
fixture_signal(
	int signal_number,
	fixture_signal_handler handler)
{
	/* Fails only the initial parent SIGPIPE ignore after the child writes. */
	if (getpid() == fixture_parent && fixture_fault == FIXTURE_FAULT_SIGNAL &&
	    signal_number == SIGPIPE && handler == SIG_IGN) {
		fixture_wait_for_early_output();
		fixture_fault = FIXTURE_FAULT_NONE;
		errno = EIO;
		return SIG_ERR;
	}

	/* Forwards every other disposition operation unchanged. */
	return signal(signal_number, handler);
}

/* Replaces the production absolute exec with this test binary. */
int
fixture_execv(
	const char *path,
	char *const arguments[])
{
	char *child_arguments[4];
	const char *mode;

	mode = strcmp(arguments[2], "--passphrase-fd=4") == 0 ?
	    arguments[5] : "simple-ok";
	if (strcmp(mode, "early-secret-output") == 0) {
		fixture_write_all(STDOUT_FILENO,
		    "WIFI1 data " FIXTURE_SECRET "\n",
		    sizeof("WIFI1 data " FIXTURE_SECRET "\n") - 1U);
		fixture_write_all(STDERR_FILENO, FIXTURE_SECRET,
		    FIXTURE_SECRET_LENGTH);
		fixture_write_all(fixture_sync_pipe[1], "x", 1U);
		for (;;)
			pause();
	}

	/* Verifies the production argv and descriptor contract before exec. */
	fixture_verify_invocation(path, arguments);
	if (strcmp(mode, "exec-fail") == 0) {
		errno = ENOENT;
		return -1;
	}

	/* Replaces the child with one behavior selected by its public SSID. */
	child_arguments[0] = "/proc/self/exe";
	child_arguments[1] = "--fixture-child";
	child_arguments[2] = (char *)mode;
	child_arguments[3] = NULL;
	execve(child_arguments[0], child_arguments, environ);

	/* Propagates an unexpected fixture exec failure. */
	return -1;
}

/* Reports one fixture failure and exits immediately. */
static void
fixture_fail(
	const char *message)
{
	fprintf(stderr, "networkd-wifi-child-test: %s\n", message);
	exit(1);
}

/* Requires one fixture condition. */
static void
fixture_expect(
	int condition,
	const char *message)
{
	/* Stops at the first violated contract. */
	if (!condition)
		fixture_fail(message);
}

/* Writes one complete fixture buffer. */
static void
fixture_write_all(
	int descriptor,
	const void *buffer,
	size_t length)
{
	const unsigned char *bytes;
	ssize_t count;
	size_t offset;

	/* Writes every byte while tolerating interrupted host calls. */
	bytes = buffer;
	offset = 0U;
	while (offset < length) {
		count = write(descriptor, bytes + offset, length - offset);
		if (count > 0) {
			offset += (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		_exit(120);
	}
}

/* Waits until the adversarial child has filled both output channels. */
static void
fixture_wait_for_early_output(
	void)
{
	unsigned char marker;
	ssize_t count;

	/* Leaves only the child holding the marker's write endpoint. */
	if (fixture_sync_pipe[1] >= 0) {
		(void)close(fixture_sync_pipe[1]);
		fixture_sync_pipe[1] = -1;
	}

	/* Reads the exact synchronization marker while tolerating interruption. */
	do {
		count = read(fixture_sync_pipe[0], &marker, sizeof(marker));
	} while (count < 0 && errno == EINTR);
	if (count != 1 || marker != 'x')
		fixture_fail("early output synchronization");
}

/* Creates one private test-only child-output synchronization channel. */
static void
fixture_open_sync_pipe(
	void)
{
	int descriptors[2];

	/* Creates one fresh channel for the next fault-injection invocation. */
	if (pipe(descriptors) != 0)
		fixture_fail("open early output synchronization pipe");
	fixture_sync_pipe[0] = fcntl(descriptors[0], F_DUPFD, 100);
	fixture_sync_pipe[1] = fcntl(descriptors[1], F_DUPFD, 100);
	(void)close(descriptors[0]);
	(void)close(descriptors[1]);
	if (fixture_sync_pipe[0] < 0 || fixture_sync_pipe[1] < 0) {
		fixture_close_sync_pipe();
		fixture_fail("duplicate early output synchronization pipe");
	}
}

/* Closes the test-only child-output synchronization channel. */
static void
fixture_close_sync_pipe(
	void)
{
	/* Retires both synchronization endpoints after child collection. */
	if (fixture_sync_pipe[0] >= 0)
		(void)close(fixture_sync_pipe[0]);
	if (fixture_sync_pipe[1] >= 0)
		(void)close(fixture_sync_pipe[1]);
	fixture_sync_pipe[0] = -1;
	fixture_sync_pipe[1] = -1;
}

/* Verifies the shell-free argv, fd-3 reservation, and exact fd-4 secret. */
static void
fixture_verify_invocation(
	const char *path,
	char *const arguments[])
{
	unsigned char secret[WLAN_PASSPHRASE_STORAGE];
	ssize_t count;
	size_t used;
	unsigned index;
	int connecting;

	/* Validates the absolute executable and fixed machine-mode prefix. */
	if (strcmp(path, NETWORKD_WIFI_PATH) != 0 ||
	    strcmp(arguments[0], NETWORKD_WIFI_PATH) != 0 ||
	    strcmp(arguments[1], "--machine") != 0)
		_exit(121);
	connecting = strcmp(arguments[2], "--passphrase-fd=4") == 0;

	/* Ensures neither reserved readiness nor secret appears unexpectedly. */
	errno = 0;
	if (fcntl(3, F_GETFD) >= 0 || errno != EBADF)
		_exit(122);
	for (index = 0U; arguments[index] != NULL; index++) {
		if (strcmp(arguments[index], FIXTURE_SECRET) == 0)
			_exit(123);
	}

	/* Validates the connect shape and exact EOF-terminated secret bytes. */
	if (connecting) {
		if (arguments[3] == NULL || strcmp(arguments[3], "wlan0") != 0 ||
		    arguments[4] == NULL || strcmp(arguments[4], "connect") != 0 ||
		    arguments[5] == NULL || arguments[6] != NULL)
			_exit(124);
		used = 0U;
		while (used < sizeof(secret)) {
			count = read(NETWORKD_WIFI_SECRET_DESCRIPTOR,
			    secret + used, sizeof(secret) - used);
			if (count > 0) {
				used += (size_t)count;
				continue;
			}
			if (count == 0)
				break;
			if (errno == EINTR)
				continue;
			_exit(125);
		}
		if (used != FIXTURE_SECRET_LENGTH ||
		    memcmp(secret, FIXTURE_SECRET, used) != 0 ||
		    read(NETWORKD_WIFI_SECRET_DESCRIPTOR, secret, 1U) != 0)
			_exit(126);
		clear_bytes(secret, sizeof(secret));
	} else {
		if (strcmp(arguments[2], "wlan0") != 0 ||
		    strcmp(arguments[3], "status") != 0 || arguments[4] != NULL)
			_exit(127);
		errno = 0;
		if (fcntl(NETWORKD_WIFI_SECRET_DESCRIPTOR, F_GETFD) >= 0 ||
		    errno != EBADF)
			_exit(128);
	}
}

/* Runs one selected machine-output child behavior. */
static int
fixture_child_main(
	const char *mode)
{
	char diagnostic[NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX + 2U];

	/* Selects one deterministic child behavior. */
	if (strcmp(mode, "success") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 state connected\n"
		    "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 state connected\nWIFI1 terminal ok 0\n") - 1U);
		fixture_write_all(STDERR_FILENO, "ready\n\t", 7U);
		return 0;
	}
	if (strcmp(mode, "terminal-error") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal error 25\n",
		    sizeof("WIFI1 terminal error 25\n") - 1U);
		return 1;
	}
	if (strcmp(mode, "missing-terminal") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 state failed\n",
		    sizeof("WIFI1 state failed\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "after-terminal") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n"
		    "WIFI1 extra forbidden\n",
		    sizeof("WIFI1 terminal ok 0\nWIFI1 extra forbidden\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "bad-prefix") == 0) {
		fixture_write_all(STDOUT_FILENO, "human output\n"
		    "WIFI1 terminal ok 0\n",
		    sizeof("human output\nWIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "max-records") == 0) {
		fixture_write_records(NETWORKD_WIFI_CHILD_RECORD_MAX - 1U);
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "too-many-records") == 0) {
		fixture_write_records(NETWORKD_WIFI_CHILD_RECORD_MAX);
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "exact-output") == 0) {
		fixture_write_exact_output(NETWORKD_WIFI_CHILD_OUTPUT_MAX);
		return 0;
	}
	if (strcmp(mode, "too-much-output") == 0) {
		fixture_write_exact_output(NETWORKD_WIFI_CHILD_OUTPUT_MAX + 1U);
		return 0;
	}
	if (strcmp(mode, "blocked-output") == 0) {
		fixture_write_until_killed();
		return 115;
	}
	if (strcmp(mode, "diagnostic-max") == 0) {
		memset(diagnostic, 'd', NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX);
		fixture_write_all(STDERR_FILENO, diagnostic,
		    NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX);
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "diagnostic-over") == 0) {
		memset(diagnostic, 'd', sizeof(diagnostic));
		fixture_write_all(STDERR_FILENO, diagnostic, sizeof(diagnostic));
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "secret-output") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 data " FIXTURE_SECRET "\n"
		    "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 data " FIXTURE_SECRET "\n"
		    "WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "secret-diagnostic") == 0) {
		fixture_write_all(STDERR_FILENO, FIXTURE_SECRET,
		    FIXTURE_SECRET_LENGTH);
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}
	if (strcmp(mode, "timeout-term") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 state waiting\n",
		    sizeof("WIFI1 state waiting\n") - 1U);
		for (;;)
			pause();
	}
	if (strcmp(mode, "timeout-kill") == 0) {
		(void)signal(SIGTERM, (void (*)(int))SIG_IGN);
		for (;;)
			pause();
	}
	if (strcmp(mode, "crash") == 0) {
		(void)raise(SIGSEGV);
		return 119;
	}
	if (strcmp(mode, "simple-ok") == 0) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 terminal ok 0\n",
		    sizeof("WIFI1 terminal ok 0\n") - 1U);
		return 0;
	}

	/* Reports an unknown fixture selection. */
	return 118;
}

/* Writes the selected number of short intermediate records. */
static void
fixture_write_records(
	unsigned count)
{
	unsigned index;

	/* Emits each complete bounded intermediate record. */
	for (index = 0U; index < count; index++) {
		fixture_write_all(STDOUT_FILENO, "WIFI1 data x\n",
		    sizeof("WIFI1 data x\n") - 1U);
	}
}

/* Writes one exact-sized valid stream ending in a success terminal. */
static void
fixture_write_exact_output(
	size_t length)
{
	static const char prefix[] = "WIFI1 data ";
	static const char terminal[] = "WIFI1 terminal ok 0\n";
	unsigned char *bytes;
	size_t first_length;

	/* Constructs one large intermediate record plus one terminal record. */
	if (length <= sizeof(prefix) + sizeof(terminal))
		_exit(117);
	bytes = malloc(length);
	if (bytes == NULL)
		_exit(116);
	first_length = length - (sizeof(terminal) - 1U);
	memset(bytes, 'x', length);
	memcpy(bytes, prefix, sizeof(prefix) - 1U);
	bytes[first_length - 1U] = '\n';
	memcpy(bytes + first_length, terminal, sizeof(terminal) - 1U);
	fixture_write_all(STDOUT_FILENO, bytes, length);
	clear_bytes(bytes, length);
	free(bytes);
}

/* Keeps a machine-output pipe full until the bounded reader terminates it. */
static void
fixture_write_until_killed(
	void)
{
	unsigned char bytes[NETWORKD_WIFI_CHILD_COPY_SIZE];

	/* Starts one versioned record and continuously fills its payload. */
	memset(bytes, 'x', sizeof(bytes));
	memcpy(bytes, NETWORKD_WIFI_RECORD_PREFIX,
	    sizeof(NETWORKD_WIFI_RECORD_PREFIX) - 1U);
	for (;;) {
		fixture_write_all(STDOUT_FILENO, bytes, sizeof(bytes));
		memset(bytes, 'x', sizeof(bytes));
	}
}

/* Runs one valid connect fixture. */
static int
fixture_run(
	const char *mode,
	unsigned timeout_seconds,
	struct networkd_wifi_child_result *result)
{
	int function_result;

	/* Invokes the exact production connect API with a non-sensitive fixture. */
	function_result = networkd_wifi_child_run(
	    "wlan0",
	    "connect",
	    mode,
	    strlen(mode),
	    FIXTURE_SECRET,
	    FIXTURE_SECRET_LENGTH,
	    timeout_seconds,
	    result);

	/* Returns the runner result. */
	return function_result;
}

/* Builds one successful child result from a complete counted test stream. */
static void
fixture_set_list_output(
	struct networkd_wifi_child_result *result,
	const char *output)
{
	size_t length;
	size_t index;

	/* Copies only streams which fit the production child-result ceiling. */
	networkd_wifi_child_result_clear(result);
	length = strlen(output);
	fixture_expect(length <= sizeof(result->output), "list fixture capacity");
	memcpy(result->output, output, length);
	result->output_length = length;

	/* Counts the complete records exactly as the production reader does. */
	for (index = 0U; index < length; index++) {
		if (result->output[index] == '\n')
			result->output_records++;
	}
}

/* Requires one malformed list stream to fail without publishing partial data. */
static void
fixture_expect_list_error(
	const char *output,
	const char *message)
{
	static const unsigned char target[] = { 'a', 0U, 'b' };
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;
	int function_result;

	/* Parses one adversarial stream into recognizable nonzero result storage. */
	fixture_set_list_output(&result, output);
	memset(&parsed, 0xa5, sizeof(parsed));
	errno = 0;
	function_result = networkd_wifi_child_parse_list(
		&result,
		target,
		sizeof(target),
		&parsed);

	/* Requires canonical protocol rejection and an all-zero public result. */
	fixture_expect(function_result != 0 && errno == EILSEQ &&
	    parsed.scan_state == 0U && parsed.scan_terminal == 0 &&
	    parsed.scan_complete == 0 && parsed.ssid_found == 0 &&
	    parsed.ssid_supported == 0, message);
	networkd_wifi_child_result_clear(&result);
}

/* Requires that the preceding runner invocation left no waitable child. */
static void
fixture_expect_reaped(
	const char *message)
{
	int status;
	pid_t waited;

	/* Checks the complete process-ownership boundary. */
	errno = 0;
	waited = waitpid(-1, &status, WNOHANG);
	fixture_expect(waited < 0 && errno == ECHILD, message);
}

/* Counts all open descriptors in a conservative test-only range. */
static unsigned
fixture_open_descriptor_count(
	void)
{
	unsigned count;
	int descriptor;

	/* Counts descriptors without retaining any additional resource. */
	count = 0U;
	for (descriptor = 0; descriptor < 128; descriptor++) {
		if (fcntl(descriptor, F_GETFD) >= 0)
			count++;
	}

	/* Returns the observed descriptor count. */
	return count;
}

/* Reads monotonic time as test-only fractional seconds. */
static double
fixture_monotonic_seconds(
	void)
{
	struct timespec now;

	/* Requires the same clock used by the production runner. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		fixture_fail("monotonic clock");

	/* Returns the converted test timestamp. */
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

/* Exercises successful invocation, output, diagnostics, and cleanup. */
static void
test_success_and_contract(
	void)
{
	struct networkd_wifi_child_result result;
	unsigned before;
	unsigned after;

	/* Captures descriptor ownership around one successful child. */
	before = fixture_open_descriptor_count();
	fixture_expect(fixture_run("success", 5U, &result) == 0,
	    "successful child");
	after = fixture_open_descriptor_count();
	fixture_expect(before == after, "successful descriptor cleanup");
	fixture_expect(result.terminal_error == 0 &&
	    result.child_exit_status == 0 && result.child_term_signal == 0,
	    "successful terminal state");
	fixture_expect(result.output_records == 2U &&
	    result.output_length ==
	    sizeof("WIFI1 state connected\nWIFI1 terminal ok 0\n") - 1U,
	    "successful machine output");
	fixture_expect(strcmp(result.diagnostic, "ready") == 0 &&
	    result.diagnostic_length == 5U, "diagnostic normalization");
	fixture_expect_reaped("successful reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises terminal/exit agreement and malformed machine records. */
static void
test_terminal_and_protocol_failures(
	void)
{
	struct networkd_wifi_child_result result;

	/* Maps one canonical child terminal error independently of exit status. */
	fixture_expect(fixture_run("terminal-error", 5U, &result) != 0 &&
	    errno == 25 && result.terminal_error == 25 &&
	    result.child_exit_status == 1, "terminal error mapping");
	fixture_expect_reaped("terminal error reap");

	/* Rejects missing, non-final, and non-versioned terminal streams. */
	fixture_expect(fixture_run("missing-terminal", 5U, &result) != 0 &&
	    errno == EILSEQ, "missing terminal");
	fixture_expect_reaped("missing terminal reap");
	fixture_expect(fixture_run("after-terminal", 5U, &result) != 0 &&
	    errno == EILSEQ, "output after terminal");
	fixture_expect_reaped("output after terminal reap");
	fixture_expect(fixture_run("bad-prefix", 5U, &result) != 0 &&
	    errno == EILSEQ, "bad record prefix");
	fixture_expect_reaped("bad prefix reap");

	/* Distinguishes an absolute exec failure from malformed child output. */
	fixture_expect(fixture_run("exec-fail", 5U, &result) != 0 &&
	    errno == ENOENT && result.child_exit_status == 127,
	    "exec failure");
	fixture_expect_reaped("exec failure reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises exact byte and record ceilings with continuously drained pipes. */
static void
test_output_bounds(
	void)
{
	struct networkd_wifi_child_result result;
	double started;
	double elapsed;

	/* Accepts the exact record and byte maxima. */
	fixture_expect(NETWORKD_WIFI_CHILD_OUTPUT_MAX +
	    NETWORKD_WIFI_CHILD_ZNV2_OVERHEAD <= NETWORKD_RESPONSE_MAX,
	    "ZNV2 response capacity");
	fixture_expect(fixture_run("max-records", 5U, &result) == 0 &&
	    result.output_records == NETWORKD_WIFI_CHILD_RECORD_MAX,
	    "exact record maximum");
	fixture_expect_reaped("record maximum reap");
	fixture_expect(fixture_run("exact-output", 5U, &result) == 0 &&
	    result.output_length == NETWORKD_WIFI_CHILD_OUTPUT_MAX,
	    "exact byte maximum");
	fixture_expect_reaped("byte maximum reap");

	/* Rejects one record or one byte beyond either fixed ceiling. */
	fixture_expect(fixture_run("too-many-records", 5U, &result) != 0 &&
	    errno == EOVERFLOW, "record overflow");
	fixture_expect_reaped("record overflow reap");
	fixture_expect(fixture_run("too-much-output", 5U, &result) != 0 &&
	    errno == EOVERFLOW, "byte overflow");
	fixture_expect_reaped("byte overflow reap");

	/* Drains and retires a child which would otherwise remain pipe-blocked. */
	started = fixture_monotonic_seconds();
	fixture_expect(fixture_run("blocked-output", 5U, &result) != 0 &&
	    errno == EOVERFLOW, "blocked writer overflow");
	elapsed = fixture_monotonic_seconds() - started;
	fixture_expect(elapsed < 2.0, "blocked writer bounded retirement");
	fixture_expect_reaped("blocked writer reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises the separate diagnostic ceiling and secret redaction. */
static void
test_diagnostic_bounds_and_redaction(
	void)
{
	struct networkd_wifi_child_result result;

	/* Accepts the exact diagnostic maximum and rejects one byte beyond it. */
	fixture_expect(fixture_run("diagnostic-max", 5U, &result) == 0 &&
	    result.diagnostic_length == NETWORKD_WIFI_CHILD_DIAGNOSTIC_MAX,
	    "exact diagnostic maximum");
	fixture_expect_reaped("diagnostic maximum reap");
	fixture_expect(fixture_run("diagnostic-over", 5U, &result) != 0 &&
	    errno == EOVERFLOW, "diagnostic overflow");
	fixture_expect_reaped("diagnostic overflow reap");

	/* Clears either output channel completely if a child discloses a secret. */
	fixture_expect(fixture_run("secret-output", 5U, &result) != 0 &&
	    errno == EILSEQ && result.output_length == 0U &&
	    strcmp(result.diagnostic, "child output redacted") == 0,
	    "machine secret redaction");
	fixture_expect_reaped("machine redaction reap");
	fixture_expect(fixture_run("secret-diagnostic", 5U, &result) != 0 &&
	    errno == EILSEQ && result.output_length == 0U &&
	    strcmp(result.diagnostic, "child output redacted") == 0,
	    "diagnostic secret redaction");
	fixture_expect_reaped("diagnostic redaction reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises disclosure attempts on both failures before the poll loop. */
static void
test_preloop_failure_redaction(
	void)
{
	struct networkd_wifi_child_result result;

	/* Discards output if parent endpoint preparation fails after fork. */
	fixture_parent = getpid();
	fixture_open_sync_pipe();
	fixture_fault = FIXTURE_FAULT_PREPARE_PARENT;
	fixture_expect(fixture_run("early-secret-output", 5U, &result) != 0 &&
	    errno == EIO && result.terminal_error == EIO &&
	    result.output_length == 0U && result.diagnostic_length == 0U,
	    "prepare failure output discard");
	fixture_expect(!contains_bytes(&result, sizeof(result), FIXTURE_SECRET,
	    FIXTURE_SECRET_LENGTH), "prepare failure secret redaction");
	fixture_expect_reaped("prepare failure reap");
	fixture_close_sync_pipe();

	/* Discards output if installing the parent SIGPIPE disposition fails. */
	fixture_open_sync_pipe();
	fixture_fault = FIXTURE_FAULT_SIGNAL;
	fixture_expect(fixture_run("early-secret-output", 5U, &result) != 0 &&
	    errno == EIO && result.terminal_error == EIO &&
	    result.output_length == 0U && result.diagnostic_length == 0U,
	    "signal failure output discard");
	fixture_expect(!contains_bytes(&result, sizeof(result), FIXTURE_SECRET,
	    FIXTURE_SECRET_LENGTH), "signal failure secret redaction");
	fixture_expect_reaped("signal failure reap");
	fixture_close_sync_pipe();
	fixture_fault = FIXTURE_FAULT_NONE;
	networkd_wifi_child_result_clear(&result);
}

/* Exercises crash, timeout, SIGTERM grace, SIGKILL, and mandatory reaping. */
static void
test_timeout_and_reaping(
	void)
{
	struct networkd_wifi_child_result result;
	double started;
	double elapsed;

	/* Reports a child crash independently from a normal exit. */
	fixture_expect(fixture_run("crash", 5U, &result) != 0 &&
	    errno == EINTR && result.child_term_signal == SIGSEGV,
	    "child crash");
	fixture_expect_reaped("crash reap");

	/* Terminates a deadline-bound child without waiting through full grace. */
	started = fixture_monotonic_seconds();
	fixture_expect(fixture_run("timeout-term", 1U, &result) != 0 &&
	    errno == ETIMEDOUT && result.terminal_error == ETIMEDOUT &&
	    result.child_term_signal == SIGTERM, "SIGTERM timeout");
	elapsed = fixture_monotonic_seconds() - started;
	fixture_expect(elapsed >= 0.8 && elapsed < 2.0,
	    "SIGTERM timeout duration");
	fixture_expect_reaped("SIGTERM timeout reap");

	/* Escalates an ignored SIGTERM after the fixed one-second grace. */
	started = fixture_monotonic_seconds();
	fixture_expect(fixture_run("timeout-kill", 1U, &result) != 0 &&
	    errno == ETIMEDOUT && result.terminal_error == ETIMEDOUT &&
	    result.child_term_signal == SIGKILL, "SIGKILL timeout");
	elapsed = fixture_monotonic_seconds() - started;
	fixture_expect(elapsed >= 1.8 && elapsed < 3.5,
	    "SIGKILL grace duration");
	fixture_expect_reaped("SIGKILL timeout reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises pre-fork validation and a non-secret primitive operation. */
static void
test_argument_validation_and_simple_operation(
	void)
{
	struct networkd_wifi_child_result result;
	static const unsigned char nul_ssid[] = { 'a', 0U, 'b' };

	/* Rejects invalid deadlines, operations, counted SSIDs, and secrets. */
	fixture_expect(networkd_wifi_child_run("wlan0", "unknown", NULL, 0U,
	    NULL, 0U, 1U, &result) != 0 && errno == EINVAL,
	    "unknown operation");
	fixture_expect(networkd_wifi_child_run("wlan0", "connect", nul_ssid,
	    sizeof(nul_ssid), FIXTURE_SECRET, FIXTURE_SECRET_LENGTH, 1U,
	    &result) != 0 && errno == EINVAL, "NUL SSID");
	fixture_expect(networkd_wifi_child_run("wlan0", "connect", "ssid", 4U,
	    "short", 5U, 1U, &result) != 0 && errno == EINVAL,
	    "short secret");
	fixture_expect(networkd_wifi_child_run("wlan0", "connect", "ssid", 4U,
	    FIXTURE_SECRET, FIXTURE_SECRET_LENGTH, 0U, &result) != 0 &&
	    errno == EINVAL, "zero deadline");

	/* Runs one operation without creating or inheriting secret fd 4. */
	fixture_expect(networkd_wifi_child_run("wlan0", "status", NULL, 0U,
	    NULL, 0U, 5U, &result) == 0, "simple operation");
	fixture_expect_reaped("simple operation reap");
	networkd_wifi_child_result_clear(&result);
}

/* Exercises strict list parsing, scan classification, and counted SSIDs. */
static void
test_list_result_parser(
	void)
{
	static const char complete[] =
	    "WIFI1 scan state=2 generation=7 results=2 available=2 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\n"
	    "WIFI1 bss index=1 ssid=686f6d65 bssid=aabbccddeeff channel=36 "
	    "frequency=5180 rssi=-55 age=0 security=00000035 "
	    "flags=00000000\n"
	    "WIFI1 terminal ok 0\n";
	static const char running[] =
	    "WIFI1 scan state=1 generation=8 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\n"
	    "WIFI1 terminal ok 0\n";
	static const char idle[] =
	    "WIFI1 scan state=0 generation=0 results=0 available=0 "
	    "truncated=0 error=0\n"
	    "WIFI1 terminal ok 0\n";
	static const char cancelled[] =
	    "WIFI1 scan state=3 generation=8 results=0 available=0 "
	    "truncated=0 error=125\n"
	    "WIFI1 terminal ok 0\n";
	static const char unsupported[] =
	    "WIFI1 scan state=2 generation=9 results=4 available=4 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000025 "
	    "flags=00000000\n"
	    "WIFI1 bss index=1 ssid=610062 bssid=001122334456 channel=1 "
	    "frequency=2412 rssi=-43 age=16 security=00000037 "
	    "flags=00000000\n"
	    "WIFI1 bss index=2 ssid=610062 bssid=001122334457 channel=1 "
	    "frequency=2412 rssi=-44 age=17 security=00000235 "
	    "flags=00000000\n"
	    "WIFI1 bss index=3 ssid=610062 bssid=001122334458 channel=1 "
	    "frequency=2412 rssi=-45 age=18 security=00000435 "
	    "flags=00000000\n"
	    "WIFI1 terminal ok 0\n";
	static const char mixed[] =
	    "WIFI1 scan state=2 generation=10 results=2 available=2 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000235 "
	    "flags=00000000\n"
	    "WIFI1 bss index=1 ssid=610062 bssid=001122334456 channel=1 "
	    "frequency=2412 rssi=-43 age=16 security=00000135 "
	    "flags=00000000\n"
	    "WIFI1 terminal ok 0\n";
	static const unsigned char target[] = { 'a', 0U, 'b' };
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;

	/* Finds one binary counted SSID in a complete canonical snapshot. */
	fixture_set_list_output(&result, complete);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 &&
	    parsed.scan_state == WLAN_SCAN_COMPLETE && parsed.scan_terminal &&
	    parsed.scan_complete && parsed.ssid_found &&
	    parsed.ssid_supported,
	    "complete list counted SSID");
	fixture_expect(networkd_wifi_child_parse_list(&result, "absent", 6U,
	    &parsed) == 0 && !parsed.ssid_found && !parsed.ssid_supported,
	    "complete list absent SSID");

	/* Separates visible SSIDs from those with a supported WPA2 BSS. */
	fixture_set_list_output(&result, unsupported);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 && parsed.ssid_found &&
	    !parsed.ssid_supported,
	    "visible unsupported SSID");
	fixture_set_list_output(&result, mixed);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 && parsed.ssid_found &&
	    parsed.ssid_supported,
	    "mixed BSS support");

	/* Classifies valid nonterminal and cancelled scan snapshots distinctly. */
	fixture_set_list_output(&result, running);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 &&
	    parsed.scan_state == WLAN_SCAN_RUNNING && !parsed.scan_terminal &&
	    !parsed.scan_complete && parsed.ssid_found &&
	    parsed.ssid_supported,
	    "running list snapshot");
	fixture_set_list_output(&result, idle);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 &&
	    parsed.scan_state == WLAN_SCAN_IDLE && !parsed.scan_terminal &&
	    !parsed.scan_complete && !parsed.ssid_found &&
	    !parsed.ssid_supported,
	    "idle list snapshot");
	fixture_set_list_output(&result, cancelled);
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) == 0 &&
	    parsed.scan_state == WLAN_SCAN_CANCELLED && parsed.scan_terminal &&
	    !parsed.scan_complete && !parsed.ssid_found &&
	    !parsed.ssid_supported,
	    "cancelled list snapshot");

	/* Rejects malformed hexadecimal SSIDs and fixed-width BSS fields. */
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=61006B bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "upper-case SSID hex");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=61006 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "odd SSID hex");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=61006g bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "invalid SSID hex");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=00112233445A channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "upper-case BSSID hex");

	/* Rejects noncanonical ordering, counts, indexes, and trailing records. */
	fixture_expect_list_error(
	    "WIFI1 scan state=02 generation=7 results=0 available=0 "
	    "truncated=0 error=0\nWIFI1 terminal ok 0\n",
	    "padded scan state");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=1 available=1 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=1 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "nonsequential BSS index");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=2 available=2 "
	    "truncated=0 error=0\n"
	    "WIFI1 bss index=0 ssid=610062 bssid=001122334455 channel=1 "
	    "frequency=2412 rssi=-42 age=15 security=00000035 "
	    "flags=00000001\nWIFI1 terminal ok 0\n",
	    "missing declared BSS record");
	fixture_expect_list_error(
	    "WIFI1 scan state=2 generation=7 results=0 available=0 "
	    "truncated=0 error=0\nWIFI1 terminal ok 0\n"
	    "WIFI1 data trailing\n",
	    "record after terminal");

	/* Rejects corrupt caller metadata before indexing the bounded output. */
	networkd_wifi_child_result_clear(&result);
	result.output_length = sizeof(result.output) + 1U;
	result.output_records = 1U;
	memset(&parsed, 0xa5, sizeof(parsed));
	errno = 0;
	fixture_expect(networkd_wifi_child_parse_list(&result, target,
	    sizeof(target), &parsed) != 0 && errno == EILSEQ &&
	    parsed.scan_state == 0U && parsed.scan_terminal == 0 &&
	    parsed.scan_complete == 0 && parsed.ssid_found == 0 &&
	    parsed.ssid_supported == 0,
	    "oversized list result metadata");
	networkd_wifi_child_result_clear(&result);
}
