/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the direct, one-shot zedBSD WLAN control command.
 */

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/wlan.h>

#define WIFI_CONNECT_SECONDS		30U
#define WIFI_POLL_NANOSECONDS		100000000L
#define WIFI_FALLBACK_POLLS 		((WIFI_CONNECT_SECONDS * 1000000000ULL) / WIFI_POLL_NANOSECONDS)
#define WIFI_ESCAPED_SSID_SIZE		(WLAN_SSID_MAX * 4U + 3U)
#define WIFI_ESCAPED_INTERFACE_SIZE	(IFNAMSIZ * 4U + 3U)
#define WIFI_SECURITY_TEXT_SIZE		160U

#ifndef WIFI_EXPLICIT_CLEAR
#define WIFI_EXPLICIT_CLEAR(buffer, size) explicit_bzero((buffer), (size))
#endif

enum wifi_operation {
	WIFI_OPERATION_NONE,
	WIFI_OPERATION_SEARCH_START,
	WIFI_OPERATION_SEARCH_STOP,
	WIFI_OPERATION_LIST,
	WIFI_OPERATION_STATUS,
	WIFI_OPERATION_CONNECT,
	WIFI_OPERATION_DISCONNECT
};

static void clear_bytes(void *buffer, size_t size);
static void clear_argument(char *argument);
static int usage(void);
static int request_header(void *request, size_t size, const char *interface, size_t interface_length);
static int ioctl_error(int descriptor, unsigned long command, void *request);
static const char *scan_state_name(uint32_t state);
static const char *wlan_state_name(uint32_t state);
static int escape_bytes(const uint8_t *input, size_t length, char *output, size_t capacity);
static void security_text(uint32_t security, char *output, size_t capacity);
static int search_command(int descriptor, const char *interface, size_t interface_length, uint32_t action);
static int list_command(int descriptor, const char *interface, size_t interface_length);
static int status_request(int descriptor, const char *interface, size_t interface_length, struct wlan_status_request *status);
static int status_command(int descriptor, const char *interface, size_t interface_length);
static int connect_command(int descriptor, const char *interface, size_t interface_length, const char *ssid, size_t ssid_length, char *passphrase, size_t passphrase_length);
static int disconnect_command(int descriptor, const char *interface, size_t interface_length);
static int monotonic_ticks(uint64_t *ticks, uint64_t *frequency);
static int wait_slice(uint64_t now, uint64_t deadline, uint64_t frequency);
static int wait_for_connection(int descriptor, const char *interface, size_t interface_length, uint64_t generation, struct wlan_status_request *result);
static int print_status(const struct wlan_status_request *status);
static int parse_operation(int argc, char **argv, enum wifi_operation *operation);
static const char *operation_name(enum wifi_operation operation);
static int bytes_are_zero(const uint8_t *bytes, size_t length);

/*
 * Runs the wifi command.
 */
int
main(int argc, char **argv)
{
	enum wifi_operation operation = WIFI_OPERATION_NONE;
	char escaped_interface[WIFI_ESCAPED_INTERFACE_SIZE];
	char *passphrase = NULL;
	size_t interface_length = 0U;
	size_t ssid_length = 0U;
	size_t passphrase_length = 0U;
	int descriptor = -1;
	int error = 0;
	int status = 1;
	int syntax_error;

	/*
	 * Remember a direct-command secret early so every ordinary
	 * exit clears it, including an otherwise malformed connect
	 * invocation.
	 */
	if (argc >= 5 && strcmp(argv[2], "connect") == 0) {
		passphrase = argv[4];
		passphrase_length = strnlen(passphrase, WLAN_PASSPHRASE_MAX + 1U);
	}

	syntax_error = parse_operation(argc, argv, &operation);
	if (syntax_error != 0) {
		if (passphrase != NULL)
			clear_argument(passphrase);
		return usage();
	}

	interface_length = strnlen(argv[1], IFNAMSIZ);
	if (interface_length == 0U || interface_length == IFNAMSIZ) {
		if (passphrase != NULL)
			clear_argument(passphrase);

		fprintf(stderr,
			"wifi: interface must contain 1 to %u bytes\n",
			IFNAMSIZ - 1U);
		return 1;
	}

	if (escape_bytes((const uint8_t *)argv[1], interface_length,
		escaped_interface, sizeof(escaped_interface)) != 0) {
		if (passphrase != NULL)
			clear_argument(passphrase);

		fprintf(stderr, "wifi: invalid interface\n");

		return 1;
	}

	if (operation == WIFI_OPERATION_CONNECT) {
		size_t index;

		ssid_length = strnlen(argv[3], WLAN_SSID_MAX + 1U);
		if (ssid_length == 0U || ssid_length > WLAN_SSID_MAX) {
			clear_argument(passphrase);
			fprintf(stderr,
				"wifi: SSID must contain 1 to %u bytes\n",
				WLAN_SSID_MAX);
			return 1;
		}

		if (passphrase_length < WLAN_PASSPHRASE_MIN ||
		    passphrase_length > WLAN_PASSPHRASE_MAX) {
			clear_argument(passphrase);
			fprintf(stderr,
				"wifi: passphrase must contain %u to %u bytes\n",
				WLAN_PASSPHRASE_MIN,
				WLAN_PASSPHRASE_MAX);
			return 1;
		}
		for (index = 0U; index < passphrase_length; index++) {
			unsigned char byte = (unsigned char)passphrase[index];

			if (byte < 0x20U || byte > 0x7eU) {
				clear_argument(passphrase);
				fprintf(stderr, "wifi: passphrase must be printable ASCII\n");
				return 1;
			}
		}
	}

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0) {
		error = errno != 0 ? errno : EIO;

		if (passphrase != NULL)
			clear_argument(passphrase);

		fprintf(stderr,
			"wifi: %s: socket: %s (%d)\n",
			escaped_interface, strerror(error), error);
		return 1;
	}

	switch (operation) {
	case WIFI_OPERATION_SEARCH_START:
		error = search_command(descriptor,
				       argv[1],
				       interface_length,
				       WLAN_SCAN_START);
		break;
	case WIFI_OPERATION_SEARCH_STOP:
		error = search_command(descriptor,
				       argv[1],
				       interface_length,
				       WLAN_SCAN_STOP);
		break;
	case WIFI_OPERATION_LIST:
		error = list_command(descriptor,
				     argv[1],
				     interface_length);
		break;
	case WIFI_OPERATION_STATUS:
		error = status_command(descriptor,
				       argv[1],
				       interface_length);
		break;
	case WIFI_OPERATION_CONNECT:
		error = connect_command(descriptor,
					argv[1],
					interface_length,
					argv[3],
					ssid_length,
					passphrase,
					passphrase_length);
		break;
	case WIFI_OPERATION_DISCONNECT:
		error = disconnect_command(descriptor,
					   argv[1],
					   interface_length);
		break;
	case WIFI_OPERATION_NONE:
		/* fall-thru */
	default:
		error = EINVAL;
		break;
	}

	if (passphrase != NULL)
		clear_argument(passphrase);

	if (close(descriptor) != 0 && error == 0)
		error = errno != 0 ? errno : EIO;

	if (error == 0 && (ferror(stdout) || fflush(stdout) != 0))
		error = errno != 0 ? errno : EIO;

	if (error == 0) {
		status = 0;
	} else {
		fprintf(stderr,
			"wifi: %s: %s: %s (%d)\n",
			escaped_interface,
			operation_name(operation),
			strerror(error),
			error);
	}

	return status;
}

/* Explicitly clears storage which may have held a credential. */
static void
clear_bytes(void *buffer, size_t size)
{
	WIFI_EXPLICIT_CLEAR(buffer, size);
}

/* Clears one kernel-bounded argv string, including an oversized secret. */
static void
clear_argument(char *argument)
{
	size_t length;

	if (argument == NULL)
		return;
	length = strnlen(argument, ARG_MAX);
	clear_bytes(argument, length);
}

/* Prints the direct human command grammar. */
static int
usage(void)
{
	if (fputs("usage:\n"
	    "  wifi INTERFACE search start\n"
	    "  wifi INTERFACE search stop\n"
	    "  wifi INTERFACE list\n"
	    "  wifi INTERFACE status\n"
	    "  wifi INTERFACE connect SSID PASSPHRASE\n"
	    "  wifi INTERFACE disconnect\n", stderr) == EOF)
		return 1;
	return 1;
}

/* Initializes the common, versioned WLAN ioctl prefix. */
static int
request_header(void *request_pointer, size_t size, const char *interface,
	size_t interface_length)
{
	struct wlan_ioctl_header *header = request_pointer;

	if (request_pointer == NULL || interface == NULL ||
	    interface_length == 0U || interface_length >= IFNAMSIZ ||
	    size > UINT32_MAX)
		return EINVAL;
	memset(request_pointer, 0, size);
	memcpy(header->ifr_name, interface, interface_length);
	header->version = WLAN_ABI_VERSION;
	header->size = (uint32_t)size;
	return 0;
}

/* Issues one production WLAN ioctl and returns a stable errno value. */
static int
ioctl_error(int descriptor, unsigned long command, void *request)
{
	if (ioctl(descriptor, command, request) == 0)
		return 0;
	return errno != 0 ? errno : EIO;
}

/* Returns a bounded human scan-state name. */
static const char *
scan_state_name(uint32_t state)
{
	switch (state) {
	case WLAN_SCAN_IDLE:
		return "idle";
	case WLAN_SCAN_RUNNING:
		return "scanning";
	case WLAN_SCAN_COMPLETE:
		return "complete";
	case WLAN_SCAN_CANCELLED:
		return "cancelled";
	case WLAN_SCAN_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}

/* Returns a bounded human connection-state name. */
static const char *
wlan_state_name(uint32_t state)
{
	switch (state) {
	case WLAN_STATE_DOWN:
		return "down";
	case WLAN_STATE_IDLE:
		return "idle";
	case WLAN_STATE_SCANNING:
		return "scanning";
	case WLAN_STATE_AUTHENTICATING:
		return "authenticating";
	case WLAN_STATE_ASSOCIATING:
		return "associating";
	case WLAN_STATE_FOUR_WAY:
		return "four-way";
	case WLAN_STATE_CONNECTED:
		return "connected";
	case WLAN_STATE_DISCONNECTING:
		return "disconnecting";
	case WLAN_STATE_FAILED:
		return "failed";
	case WLAN_STATE_REMOVED:
		return "removed";
	default:
		return "unknown";
	}
}

/* Quotes counted bytes without allowing terminal-control output. */
static int
escape_bytes(const uint8_t *input, size_t length, char *output,
	size_t capacity)
{
	static const char hexadecimal[] = "0123456789abcdef";
	size_t index;
	size_t used = 0U;

	if (input == NULL || output == NULL || capacity < 3U ||
	    length > (capacity - 3U) / 4U)
		return EINVAL;
	output[used++] = '"';
	for (index = 0U; index < length; index++) {
		uint8_t byte = input[index];

		if (byte >= 0x20U && byte <= 0x7eU && byte != '"' &&
		    byte != '\\') {
			output[used++] = (char)byte;
		} else if (byte == '"' || byte == '\\') {
			output[used++] = '\\';
			output[used++] = (char)byte;
		} else {
			output[used++] = '\\';
			output[used++] = 'x';
			output[used++] = hexadecimal[byte >> 4];
			output[used++] = hexadecimal[byte & 0x0fU];
		}
	}
	output[used++] = '"';
	output[used] = '\0';
	return 0;
}

/* Appends one fixed security token if capacity remains. */
static void
append_security(char *output, size_t capacity, size_t *used,
	const char *token)
{
	size_t length = strlen(token);

	if (*used != 0U) {
		if (*used + 1U >= capacity)
			return;
		output[(*used)++] = '+';
	}
	if (length >= capacity - *used)
		length = capacity - *used - 1U;
	memcpy(output + *used, token, length);
	*used += length;
	output[*used] = '\0';
}

/* Renders the normalized public security flags into a fixed buffer. */
static void
security_text(uint32_t security, char *output, size_t capacity)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} descriptions[] = {
		{ WLAN_SECURITY_PRIVACY, "privacy" },
		{ WLAN_SECURITY_WPA1, "WPA1" },
		{ WLAN_SECURITY_WPA2, "WPA2" },
		{ WLAN_SECURITY_TKIP, "TKIP" },
		{ WLAN_SECURITY_CCMP, "CCMP" },
		{ WLAN_SECURITY_PSK, "PSK" },
		{ WLAN_SECURITY_IEEE8021X, "802.1X" },
		{ WLAN_SECURITY_SAE, "SAE" },
		{ WLAN_SECURITY_PMF_CAPABLE, "PMF-capable" },
		{ WLAN_SECURITY_PMF_REQUIRED, "PMF-required" },
		{ WLAN_SECURITY_UNSUPPORTED_SUITE, "unsupported-suite" }
	};
	uint32_t known = 0U;
	size_t index;
	size_t used = 0U;

	if (capacity == 0U)
		return;
	output[0] = '\0';
	if (security == 0U) {
		append_security(output, capacity, &used, "open");
		return;
	}
	for (index = 0U; index < sizeof(descriptions) /
	    sizeof(descriptions[0]); index++) {
		known |= descriptions[index].flag;
		if ((security & descriptions[index].flag) != 0U)
			append_security(output, capacity, &used,
			    descriptions[index].name);
	}
	if ((security & ~known) != 0U)
		append_security(output, capacity, &used, "other");
}

/* Starts or stops one bounded kernel scan operation. */
static int
search_command(int descriptor, const char *interface,
	size_t interface_length, uint32_t action)
{
	struct wlan_scan_request request;
	int error;

	error = request_header(&request, sizeof(request), interface,
	    interface_length);
	if (error != 0)
		return error;
	request.action = action;
	error = ioctl_error(descriptor, SIOCSWLANSCAN, &request);
	if (error == 0 && action == WLAN_SCAN_STOP &&
	    request.state == WLAN_SCAN_RUNNING)
		error = EIO;
	if (error == 0 && printf("search state=%s generation=%llu error=%d\n",
	    scan_state_name(request.state),
	    (unsigned long long)request.generation,
	    request.terminal_error) < 0)
		error = EIO;
	clear_bytes(&request, sizeof(request));
	return error;
}

/* Prints one immutable, bounded scan-cache snapshot. */
static int
list_command(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_scan_status_request scan;
	struct wlan_bss_request request;
	char escaped[WIFI_ESCAPED_SSID_SIZE];
	char security[WIFI_SECURITY_TEXT_SIZE];
	uint32_t index;
	int error;
	int terminal_error = 0;

	error = request_header(&scan, sizeof(scan), interface,
	    interface_length);
	if (error != 0)
		return error;
	error = ioctl_error(descriptor, SIOCGWLANSCAN, &scan);
	if (error != 0)
		return error;
	if (scan.result_count > WLAN_BSS_MAX ||
	    (scan.result_count != 0U && scan.generation == 0U))
		return EIO;
	if (printf("scan state=%s generation=%llu results=%u truncated=%s "
	    "error=%d\n", scan_state_name(scan.state),
	    (unsigned long long)scan.generation, scan.result_count,
	    scan.truncated != 0U ? "yes" : "no", scan.terminal_error) < 0)
		return EIO;
	for (index = 0U; index < scan.result_count; index++) {
		error = request_header(&request, sizeof(request), interface,
		    interface_length);
		if (error != 0)
			return error;
		request.generation = scan.generation;
		request.index = index;
		error = ioctl_error(descriptor, SIOCGWLANBSS, &request);
		if (error != 0) {
			clear_bytes(&request, sizeof(request));
			return error;
		}
		if (request.bss.ssid_length > WLAN_SSID_MAX ||
		    escape_bytes(request.bss.ssid, request.bss.ssid_length,
			escaped, sizeof(escaped)) != 0) {
			clear_bytes(&request, sizeof(request));
			return EIO;
		}
		security_text(request.bss.security, security,
		    sizeof(security));
		if (printf("%u ssid=%s bssid=%02x:%02x:%02x:%02x:%02x:%02x "
		    "channel=%u frequency=%u rssi=%d age=%u "
		    "security=%s flags=0x%08x\n", index, escaped,
		    request.bss.bssid[0], request.bss.bssid[1],
		    request.bss.bssid[2], request.bss.bssid[3],
		    request.bss.bssid[4], request.bss.bssid[5],
		    request.bss.channel, request.bss.center_frequency_mhz,
		    request.bss.rssi_dbm, request.bss.age_ms, security,
		    request.bss.security) < 0) {
			clear_bytes(&request, sizeof(request));
			return EIO;
		}
		clear_bytes(&request, sizeof(request));
	}
	if (scan.state == WLAN_SCAN_FAILED)
		terminal_error = scan.terminal_error != 0 ?
		    scan.terminal_error : EIO;
	clear_bytes(&scan, sizeof(scan));
	return terminal_error;
}

/* Obtains one public connection-status record. */
static int
status_request(int descriptor, const char *interface,
	size_t interface_length, struct wlan_status_request *status)
{
	int error = request_header(status, sizeof(*status), interface,
	    interface_length);

	if (error != 0)
		return error;
	return ioctl_error(descriptor, SIOCGWLANSTATUS, status);
}

/* Prints the current public WLAN state without credential material. */
static int
status_command(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_status_request status;
	int error;

	error = status_request(descriptor, interface, interface_length, &status);
	if (error == 0)
		error = print_status(&status);
	clear_bytes(&status, sizeof(status));
	return error;
}

/* Submits one credential and waits only for the kernel operation deadline. */
static int
connect_command(int descriptor, const char *interface,
	size_t interface_length, const char *ssid, size_t ssid_length,
	char *passphrase, size_t passphrase_length)
{
	struct wlan_connect_request request;
	struct wlan_status_request status;
	uint64_t generation;
	int error;

	error = request_header(&request, sizeof(request), interface,
	    interface_length);
	if (error != 0)
		goto out_secret;
	memcpy(request.ssid, ssid, ssid_length);
	memcpy(request.passphrase, passphrase, passphrase_length);
	request.ssid_length = (uint32_t)ssid_length;
	request.passphrase_length = (uint32_t)passphrase_length;
	/* argv need not retain the credential while the kernel owns the bounded
	 * connection attempt. */
	clear_argument(passphrase);
	error = ioctl_error(descriptor, SIOCSWLANCONNECT, &request);
	generation = request.generation;
	clear_bytes(&request, sizeof(request));
	if (error != 0)
		return error;
	if (generation == 0U)
		return EIO;
	error = wait_for_connection(descriptor, interface, interface_length,
	    generation, &status);
	if (error == 0 && printf("connected state=%s generation=%llu "
	    "authorized=yes\n", wlan_state_name(status.state),
	    (unsigned long long)generation) < 0)
		error = EIO;
	clear_bytes(&status, sizeof(status));
	return error;

out_secret:
	clear_argument(passphrase);
	clear_bytes(&request, sizeof(request));
	return error;
}

/* Disconnects synchronously; the kernel barrier has already lowered carrier. */
static int
disconnect_command(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_disconnect_request request;
	int error;

	error = request_header(&request, sizeof(request), interface,
	    interface_length);
	if (error != 0)
		return error;
	error = ioctl_error(descriptor, SIOCSWLANDISCONNECT, &request);
	if (error == 0 && request.state != WLAN_STATE_IDLE &&
	    request.state != WLAN_STATE_DOWN)
		error = request.terminal_error != 0 ?
		    request.terminal_error : EIO;
	if (error == 0 && printf("disconnected state=%s generation=%llu\n",
	    wlan_state_name(request.state),
	    (unsigned long long)request.generation) < 0)
		error = EIO;
	clear_bytes(&request, sizeof(request));
	return error;
}

/* Converts the shared monotonic clock into the public kernel tick domain. */
static int
monotonic_ticks(uint64_t *ticks, uint64_t *frequency)
{
	struct timespec now;
	long clock_frequency;
	uint64_t seconds;
	uint64_t nanoseconds;

	clock_frequency = sysconf(_SC_CLK_TCK);
	if (ticks == NULL || frequency == NULL || clock_frequency <= 0 ||
	    (uint64_t)clock_frequency > 1000000000ULL ||
	    clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
	    now.tv_nsec < 0 || now.tv_nsec >= 1000000000L)
		return EIO;
	seconds = (uint64_t)now.tv_sec;
	*frequency = (uint64_t)clock_frequency;
	if (seconds > UINT64_MAX / *frequency)
		return EOVERFLOW;
	nanoseconds = (uint64_t)now.tv_nsec;
	*ticks = seconds * *frequency +
	    (nanoseconds * *frequency) / 1000000000ULL;
	return 0;
}

/* Sleeps for at most one polling slice and never past the known deadline. */
static int
wait_slice(uint64_t now, uint64_t deadline, uint64_t frequency)
{
	struct timespec delay;
	uint64_t remaining;
	uint64_t nanoseconds = (uint64_t)WIFI_POLL_NANOSECONDS;

	if (deadline <= now || frequency == 0U)
		return ETIMEDOUT;
	remaining = deadline - now;
	if (remaining < frequency) {
		nanoseconds = (remaining * 1000000000ULL + frequency - 1U) /
		    frequency;
		if (nanoseconds > (uint64_t)WIFI_POLL_NANOSECONDS)
			nanoseconds = (uint64_t)WIFI_POLL_NANOSECONDS;
	}
	delay.tv_sec = 0;
	delay.tv_nsec = (long)nanoseconds;
	if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Polls the public state until authorized carrier or a bounded failure. */
static int
wait_for_connection(int descriptor, const char *interface,
	size_t interface_length, uint64_t generation,
	struct wlan_status_request *result)
{
	uint64_t start = 0U;
	uint64_t frequency = 0U;
	uint64_t local_deadline = 0U;
	uint64_t now;
	uint64_t deadline;
	unsigned polls;
	int have_clock;
	int error;

	have_clock = monotonic_ticks(&start, &frequency) == 0 &&
	    start <= UINT64_MAX - WIFI_CONNECT_SECONDS * frequency;
	if (have_clock)
		local_deadline = start + WIFI_CONNECT_SECONDS * frequency;
	for (polls = 0U; polls <= WIFI_FALLBACK_POLLS; polls++) {
		error = status_request(descriptor, interface, interface_length,
		    result);
		if (error != 0)
			return error;
		if (result->operation_generation != generation)
			return ESTALE;
		if (result->state == WLAN_STATE_CONNECTED) {
			if (result->controlled_port != 0U)
				return 0;
			return EIO;
		}
		if (result->terminal_error != 0)
			return result->terminal_error;
		switch (result->state) {
		case WLAN_STATE_AUTHENTICATING:
		case WLAN_STATE_ASSOCIATING:
		case WLAN_STATE_FOUR_WAY:
			break;
		case WLAN_STATE_DOWN:
			return ENETDOWN;
		case WLAN_STATE_IDLE:
		case WLAN_STATE_DISCONNECTING:
			return ECANCELED;
		case WLAN_STATE_FAILED:
			return EIO;
		case WLAN_STATE_REMOVED:
			return ENODEV;
		default:
			return EIO;
		}
		if (polls == WIFI_FALLBACK_POLLS)
			return ETIMEDOUT;
		if (!have_clock) {
			struct timespec delay = { 0, WIFI_POLL_NANOSECONDS };

			if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
				return errno != 0 ? errno : EIO;
			continue;
		}
		error = monotonic_ticks(&now, &frequency);
		if (error != 0)
			return error;
		deadline = local_deadline;
		if (result->deadline_ticks != 0U &&
		    result->deadline_ticks < deadline)
			deadline = result->deadline_ticks;
		if (now >= deadline)
			return ETIMEDOUT;
		error = wait_slice(now, deadline, frequency);
		if (error != 0)
			return error;
	}
	return ETIMEDOUT;
}

/* Prints a bounded public status record. */
static int
print_status(const struct wlan_status_request *status)
{
	char security[WIFI_SECURITY_TEXT_SIZE];

	security_text(status->security, security, sizeof(security));
	if (printf("state=%s scan=%s administrative=%s authenticated=%s "
	    "associated=%s key=%s authorized=%s retries=%u error=%d",
	    wlan_state_name(status->state), scan_state_name(status->scan_state),
	    status->administrative_up != 0U ? "up" : "down",
	    status->authenticated != 0U ? "yes" : "no",
	    status->associated != 0U ? "yes" : "no",
	    status->key_installed != 0U ? "yes" : "no",
	    status->controlled_port != 0U ? "yes" : "no",
	    status->retry_count, status->terminal_error) < 0)
		return EIO;
	if (status->deadline_ticks != 0U &&
	    printf(" deadline=%llu",
		(unsigned long long)status->deadline_ticks) < 0)
		return EIO;
	if (!bytes_are_zero(status->bssid, sizeof(status->bssid)) &&
	    printf(" bssid=%02x:%02x:%02x:%02x:%02x:%02x channel=%u "
		"frequency=%u rssi=%d security=%s flags=0x%08x",
		status->bssid[0], status->bssid[1], status->bssid[2],
		status->bssid[3], status->bssid[4], status->bssid[5],
		status->channel, status->center_frequency_mhz,
		status->rssi_dbm, security, status->security) < 0)
		return EIO;
	if (putchar('\n') == EOF)
		return EIO;
	return 0;
}

/* Accepts exactly the six p004 human forms. */
static int
parse_operation(int argc, char **argv, enum wifi_operation *operation)
{
	if (argc == 4 && strcmp(argv[2], "search") == 0) {
		if (strcmp(argv[3], "start") == 0)
			*operation = WIFI_OPERATION_SEARCH_START;
		else if (strcmp(argv[3], "stop") == 0)
			*operation = WIFI_OPERATION_SEARCH_STOP;
		else
			return EINVAL;
		return 0;
	}
	if (argc == 3 && strcmp(argv[2], "list") == 0) {
		*operation = WIFI_OPERATION_LIST;
		return 0;
	}
	if (argc == 3 && strcmp(argv[2], "status") == 0) {
		*operation = WIFI_OPERATION_STATUS;
		return 0;
	}
	if (argc == 5 && strcmp(argv[2], "connect") == 0) {
		*operation = WIFI_OPERATION_CONNECT;
		return 0;
	}
	if (argc == 3 && strcmp(argv[2], "disconnect") == 0) {
		*operation = WIFI_OPERATION_DISCONNECT;
		return 0;
	}
	return EINVAL;
}

/* Returns a fixed diagnostic operation label. */
static const char *
operation_name(enum wifi_operation operation)
{
	switch (operation) {
	case WIFI_OPERATION_SEARCH_START:
		return "search start";
	case WIFI_OPERATION_SEARCH_STOP:
		return "search stop";
	case WIFI_OPERATION_LIST:
		return "list";
	case WIFI_OPERATION_STATUS:
		return "status";
	case WIFI_OPERATION_CONNECT:
		return "connect";
	case WIFI_OPERATION_DISCONNECT:
		return "disconnect";
	case WIFI_OPERATION_NONE:
	default:
		return "command";
	}
}

/* Tests a fixed public field without relying on alignment. */
static int
bytes_are_zero(const uint8_t *bytes, size_t length)
{
	while (length-- != 0U) {
		if (*bytes++ != 0U)
			return 0;
	}
	return 1;
}
