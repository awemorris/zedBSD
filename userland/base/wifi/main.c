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
#include <signal.h>
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
#define WIFI_FALLBACK_POLLS		((WIFI_CONNECT_SECONDS * 1000000000ULL) / WIFI_POLL_NANOSECONDS)
#define WIFI_ESCAPED_SSID_SIZE		(WLAN_SSID_MAX * 4U + 3U)
#define WIFI_ESCAPED_INTERFACE_SIZE	(IFNAMSIZ * 4U + 3U)
#define WIFI_HEX_SSID_SIZE		(WLAN_SSID_MAX * 2U + 1U)
#define WIFI_HEX_BSSID_SIZE		13U
#define WIFI_MACHINE_BSS_MAX		62U
#define WIFI_SECRET_DESCRIPTOR		4
#define WIFI_SECURITY_TEXT_SIZE		160U

#ifndef WIFI_EXPLICIT_CLEAR
#define WIFI_EXPLICIT_CLEAR(buffer, size) explicit_bzero((buffer), (size))
#endif

enum wifi_operation {
	WIFI_OPERATION_NONE,
	WIFI_OPERATION_UP,
	WIFI_OPERATION_DOWN,
	WIFI_OPERATION_SEARCH_START,
	WIFI_OPERATION_SEARCH_STOP,
	WIFI_OPERATION_LIST,
	WIFI_OPERATION_STATUS,
	WIFI_OPERATION_CONNECT,
	WIFI_OPERATION_DISCONNECT
};

static int wifi_quiet;
static int wifi_machine;
static volatile sig_atomic_t wifi_interrupted;

static void clear_bytes(void *buffer, size_t size);
static void clear_argument(char *argument);
static int usage(void);
static int request_header(void *request, size_t size, const char *interface,
	size_t interface_length);
static int ioctl_error(int descriptor, unsigned long command, void *request);
static const char *scan_state_name(uint32_t state);
static const char *wlan_state_name(uint32_t state);
static int escape_bytes(const uint8_t *input, size_t length, char *output,
	size_t capacity);
static void security_text(uint32_t security, char *output, size_t capacity);
static int search_command(int descriptor, const char *interface,
	size_t interface_length, uint32_t action);
static int interface_command(int descriptor, const char *interface,
	size_t interface_length, int bring_up);
static int list_command(int descriptor, const char *interface,
	size_t interface_length);
static int scan_status_request(int descriptor, const char *interface,
	size_t interface_length, struct wlan_scan_status_request *scan);
static int status_request(int descriptor, const char *interface,
	size_t interface_length, struct wlan_status_request *status);
static int status_command(int descriptor, const char *interface,
	size_t interface_length);
static int connect_command(int descriptor, const char *interface,
	size_t interface_length, const char *ssid, size_t ssid_length,
	char *passphrase, size_t passphrase_length);
static int disconnect_command(int descriptor, const char *interface,
	size_t interface_length);
static int monotonic_ticks(uint64_t *ticks, uint64_t *frequency);
static int wait_slice(uint64_t now, uint64_t deadline, uint64_t frequency);
static int wait_for_connection(int descriptor, const char *interface,
	size_t interface_length, uint64_t generation, uint64_t deadline,
	uint64_t frequency, uint32_t *displayed_states,
	struct wlan_status_request *result);
static int wait_for_scan(int descriptor, const char *interface,
	size_t interface_length, uint64_t generation, uint64_t deadline,
	uint64_t frequency, uint64_t *displayed_seconds);
static int cancel_scan(int descriptor, const char *interface,
	size_t interface_length);
static int cancel_connection(int descriptor, const char *interface,
	size_t interface_length);
static void print_failure_status(int descriptor, const char *interface,
	size_t interface_length);
static int print_connection_progress(uint32_t state, uint64_t generation);
static int print_status(const struct wlan_status_request *status);
static int parse_operation(int argc, char **argv,
	enum wifi_operation *operation);
static const char *operation_name(enum wifi_operation operation);
static int bytes_are_zero(const uint8_t *bytes, size_t length);
static int retryable_connect_error(int error);
static int connection_in_progress(const struct wlan_status_request *status);
static void interrupt_handler(int signal_number);
static int read_machine_passphrase(int descriptor, char *passphrase,
	size_t capacity, size_t *length);
static int machine_terminal(int error);
static int hex_bytes(const uint8_t *input, size_t length, char *output,
	size_t capacity);

/*
 * Runs the wifi command.
 */
int
main(int argc, char **argv)
{
	enum wifi_operation operation = WIFI_OPERATION_NONE;
	char escaped_interface[WIFI_ESCAPED_INTERFACE_SIZE];
	char machine_passphrase[WLAN_PASSPHRASE_STORAGE];
	char *passphrase = NULL;
	sighandler_t previous_interrupt = SIG_ERR;
	sighandler_t previous_terminate = SIG_ERR;
	size_t interface_length = 0U;
	size_t ssid_length = 0U;
	size_t passphrase_length = 0U;
	int descriptor = -1;
	int error = 0;
	int passphrase_descriptor = -1;
	int secret_close_error;
	int status = 1;
	int syntax_error;

	memset(machine_passphrase, 0, sizeof(machine_passphrase));
	wifi_quiet = 0;
	wifi_machine = 0;
	if (argc >= 2 && strcmp(argv[1], "--quiet") == 0) {
		wifi_quiet = 1;
		argc--;
		argv++;
	} else if (argc >= 2 && strcmp(argv[1], "--machine") == 0) {
		wifi_machine = 1;
		argc--;
		argv++;
		if (argc >= 2 && strcmp(argv[1], "--passphrase-fd=4") == 0) {
			passphrase_descriptor = WIFI_SECRET_DESCRIPTOR;
			argc--;
			argv++;
		}
	}

	/*
	 * Remember a direct-command secret early so every ordinary
	 * exit clears it, including an otherwise malformed connect
	 * invocation.
	 */
	if (!wifi_machine && argc >= 5 && strcmp(argv[2], "connect") == 0) {
		passphrase = argv[4];
		passphrase_length = strnlen(passphrase, WLAN_PASSPHRASE_MAX + 1U);
	}

	syntax_error = parse_operation(argc, argv, &operation);
	if (syntax_error == 0 && wifi_machine &&
	    ((operation == WIFI_OPERATION_CONNECT) !=
	    (passphrase_descriptor == WIFI_SECRET_DESCRIPTOR)))
		syntax_error = EINVAL;
	if (syntax_error != 0) {
		if (passphrase != NULL)
			clear_argument(passphrase);
		clear_bytes(machine_passphrase, sizeof(machine_passphrase));
		if (wifi_machine)
			return machine_terminal(EINVAL);
		return usage();
	}

	interface_length = strnlen(argv[1], IFNAMSIZ);
	if (interface_length == 0U || interface_length == IFNAMSIZ) {
		if (passphrase != NULL)
			clear_argument(passphrase);
		clear_bytes(machine_passphrase, sizeof(machine_passphrase));

		if (wifi_machine)
			return machine_terminal(EINVAL);
		if (!wifi_quiet)
			fprintf(stderr,
			"wifi: interface must contain 1 to %u bytes\n",
			IFNAMSIZ - 1U);
		return 1;
	}

	if (escape_bytes((const uint8_t *)argv[1], interface_length,
		escaped_interface, sizeof(escaped_interface)) != 0) {
		if (passphrase != NULL)
			clear_argument(passphrase);
		clear_bytes(machine_passphrase, sizeof(machine_passphrase));

		if (wifi_machine)
			return machine_terminal(EINVAL);
		if (!wifi_quiet)
			fprintf(stderr, "wifi: invalid interface\n");

		return 1;
	}

	if (operation == WIFI_OPERATION_CONNECT) {
		size_t index;

		ssid_length = strnlen(argv[3], WLAN_SSID_MAX + 1U);
		if (ssid_length == 0U || ssid_length > WLAN_SSID_MAX) {
			clear_argument(passphrase);
			clear_bytes(machine_passphrase, sizeof(machine_passphrase));
			if (wifi_machine)
				return machine_terminal(EINVAL);
			if (!wifi_quiet)
				fprintf(stderr,
				"wifi: SSID must contain 1 to %u bytes\n",
				WLAN_SSID_MAX);
			return 1;
		}

		if (wifi_machine) {
			error = read_machine_passphrase(passphrase_descriptor,
			    machine_passphrase, sizeof(machine_passphrase),
			    &passphrase_length);
			secret_close_error = close(passphrase_descriptor);
			if (error == 0 && secret_close_error != 0)
				error = errno != 0 ? errno : EIO;
			if (error != 0) {
				clear_bytes(machine_passphrase,
				    sizeof(machine_passphrase));
				return machine_terminal(error);
			}
			passphrase = machine_passphrase;
		}

		if (passphrase_length < WLAN_PASSPHRASE_MIN ||
		    passphrase_length > WLAN_PASSPHRASE_MAX) {
			clear_argument(passphrase);
			clear_bytes(machine_passphrase, sizeof(machine_passphrase));
			if (wifi_machine)
				return machine_terminal(EINVAL);
			if (!wifi_quiet)
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
				clear_bytes(machine_passphrase,
				    sizeof(machine_passphrase));
				if (wifi_machine)
					return machine_terminal(EINVAL);
				if (!wifi_quiet) {
					fprintf(stderr,
					    "wifi: passphrase must be printable ASCII\n");
				}
				return 1;
			}
		}
	}

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0) {
		error = errno != 0 ? errno : EIO;

		if (passphrase != NULL)
			clear_argument(passphrase);
		clear_bytes(machine_passphrase, sizeof(machine_passphrase));

		if (wifi_machine)
			return machine_terminal(error);
		if (!wifi_quiet)
			fprintf(stderr,
			"wifi: %s: socket: %s (%d)\n",
			escaped_interface, strerror(error), error);
		return 1;
	}

	switch (operation) {
	case WIFI_OPERATION_UP:
		error = interface_command(descriptor, argv[1], interface_length, 1);
		break;
	case WIFI_OPERATION_DOWN:
		error = interface_command(descriptor, argv[1], interface_length, 0);
		break;
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
		wifi_interrupted = 0;
		previous_interrupt = signal(SIGINT, interrupt_handler);
		if (previous_interrupt == SIG_ERR) {
			error = errno != 0 ? errno : EIO;
			break;
		}
		previous_terminate = signal(SIGTERM, interrupt_handler);
		if (previous_terminate == SIG_ERR) {
			error = errno != 0 ? errno : EIO;
			(void)signal(SIGINT, previous_interrupt);
			break;
		}
		error = connect_command(descriptor,
					argv[1],
					interface_length,
					argv[3],
					ssid_length,
					passphrase,
					passphrase_length);
		if (signal(SIGTERM, previous_terminate) == SIG_ERR && error == 0)
			error = errno != 0 ? errno : EIO;
		if (signal(SIGINT, previous_interrupt) == SIG_ERR && error == 0)
			error = errno != 0 ? errno : EIO;
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
	clear_bytes(machine_passphrase, sizeof(machine_passphrase));

	if (close(descriptor) != 0 && error == 0)
		error = errno != 0 ? errno : EIO;
	if (wifi_machine) {
		if (error == 0 && (ferror(stdout) || fflush(stdout) != 0))
			error = errno != 0 ? errno : EIO;
		return machine_terminal(error);
	}

	if (!wifi_quiet && error == 0 &&
	    (ferror(stdout) || fflush(stdout) != 0))
		error = errno != 0 ? errno : EIO;

	if (error == 0) {
		status = 0;
	} else {
		if (!wifi_quiet)
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
	if (wifi_quiet)
		return 1;
	if (fputs("usage:\n"
	    "  wifi [--quiet] INTERFACE up\n"
	    "  wifi [--quiet] INTERFACE down\n"
	    "  wifi [--quiet] INTERFACE search start\n"
	    "  wifi [--quiet] INTERFACE search stop\n"
	    "  wifi [--quiet] INTERFACE list\n"
	    "  wifi [--quiet] INTERFACE status\n"
	    "  wifi [--quiet] INTERFACE connect SSID PASSPHRASE\n"
	    "  wifi [--quiet] INTERFACE disconnect\n", stderr) == EOF)
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
	if (error == 0) {
		if (wifi_machine) {
			if (printf("WIFI1 scan action=%u state=%u generation=%llu "
			    "error=%d\n", action, request.state,
			    (unsigned long long)request.generation,
			    request.terminal_error) < 0)
				error = EIO;
		} else if (!wifi_quiet &&
		    printf("search state=%s generation=%llu error=%d\n",
		    scan_state_name(request.state),
		    (unsigned long long)request.generation,
		    request.terminal_error) < 0) {
			error = EIO;
		}
	}
	clear_bytes(&request, sizeof(request));
	return error;
}

/* Changes only the existing generic administrative interface state. */
static int
interface_command(int descriptor, const char *interface,
	size_t interface_length, int bring_up)
{
	struct ifreq request;
	int error;

	if (interface == NULL || interface_length == 0U ||
	    interface_length >= sizeof(request.ifr_name))
		return EINVAL;
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, interface, interface_length);
	error = ioctl_error(descriptor, SIOCGIFFLAGS, &request);
	if (error != 0)
		return error;
	if (bring_up)
		request.ifr_flags |= IFF_UP;
	else
		request.ifr_flags &= (int)~IFF_UP;
	error = ioctl_error(descriptor, SIOCSIFFLAGS, &request);
	if (error == 0) {
		if (wifi_machine) {
			if (printf("WIFI1 interface administrative=%u\n",
			    bring_up != 0 ? 1U : 0U) < 0)
				error = EIO;
		} else if (!wifi_quiet &&
		    printf("interface %s administrative=%s\n", interface,
		    bring_up ? "up" : "down") < 0) {
			error = EIO;
		}
	}
	memset(&request, 0, sizeof(request));
	return error;
}

/* Obtains one public scan-operation and immutable-snapshot record. */
static int
scan_status_request(int descriptor, const char *interface,
	size_t interface_length, struct wlan_scan_status_request *scan)
{
	int error;

	error = request_header(scan, sizeof(*scan), interface,
	    interface_length);
	if (error != 0)
		return error;
	return ioctl_error(descriptor, SIOCGWLANSCAN, scan);
}

/* Prints one immutable, bounded scan-cache snapshot. */
static int
list_command(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_scan_status_request scan;
	struct wlan_bss_request request;
	char escaped[WIFI_ESCAPED_SSID_SIZE];
	char hexadecimal_bssid[WIFI_HEX_BSSID_SIZE];
	char hexadecimal_ssid[WIFI_HEX_SSID_SIZE];
	char security[WIFI_SECURITY_TEXT_SIZE];
	uint32_t index;
	uint32_t output_count;
	uint32_t output_truncated;
	int error;
	int terminal_error = 0;

	error = scan_status_request(descriptor, interface, interface_length,
	    &scan);
	if (error != 0)
		return error;
	if (scan.result_count > WLAN_BSS_MAX ||
	    (scan.result_count != 0U && scan.generation == 0U))
		return EIO;
	output_count = scan.result_count;
	if (wifi_machine && output_count > WIFI_MACHINE_BSS_MAX)
		output_count = WIFI_MACHINE_BSS_MAX;
	output_truncated = scan.truncated != 0U ||
	    output_count != scan.result_count;
	if (wifi_machine) {
		if (printf("WIFI1 scan state=%u generation=%llu results=%u "
		    "available=%u truncated=%u error=%d\n", scan.state,
		    (unsigned long long)scan.generation, output_count,
		    scan.result_count, output_truncated, scan.terminal_error) < 0)
			return EIO;
	} else if (!wifi_quiet &&
	    printf("scan state=%s generation=%llu results=%u truncated=%s "
	    "error=%d\n", scan_state_name(scan.state),
	    (unsigned long long)scan.generation, scan.result_count,
	    scan.truncated != 0U ? "yes" : "no", scan.terminal_error) < 0) {
		return EIO;
	}
	for (index = 0U; index < output_count; index++) {
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
		if (wifi_machine) {
			error = hex_bytes(request.bss.ssid,
			    request.bss.ssid_length, hexadecimal_ssid,
			    sizeof(hexadecimal_ssid));
			if (error == 0)
				error = hex_bytes(request.bss.bssid,
				    sizeof(request.bss.bssid), hexadecimal_bssid,
				    sizeof(hexadecimal_bssid));
			if (error != 0 ||
			    printf("WIFI1 bss index=%u ssid=%s bssid=%s channel=%u "
			    "frequency=%u rssi=%d age=%u security=%08x "
			    "flags=%08x\n", index, hexadecimal_ssid,
			    hexadecimal_bssid, request.bss.channel,
			    request.bss.center_frequency_mhz, request.bss.rssi_dbm,
			    request.bss.age_ms, request.bss.security,
			    request.bss.capability) < 0) {
				clear_bytes(&request, sizeof(request));
				return error != 0 ? error : EIO;
			}
		} else {
			security_text(request.bss.security, security,
			    sizeof(security));
		}
		if (!wifi_machine && !wifi_quiet &&
		    printf("%u ssid=%s bssid=%02x:%02x:%02x:%02x:%02x:%02x "
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

/* Scans as needed, then connects under one command-wide deadline. */
static int
connect_command(int descriptor, const char *interface,
	size_t interface_length, const char *ssid, size_t ssid_length,
	char *passphrase, size_t passphrase_length)
{
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	struct wlan_connect_request request;
	struct wlan_scan_request scan_request;
	struct wlan_scan_status_request scan;
	struct wlan_status_request status;
	uint64_t deadline;
	uint64_t displayed_scan_seconds = UINT64_MAX;
	uint64_t frequency;
	uint64_t generation = 0U;
	uint64_t now;
	uint64_t scan_generation = 0U;
	uint32_t displayed_states = 0U;
	int admitted = 0;
	int attempt_error = 0;
	int busy_announced = 0;
	int retry_announced = 0;
	int scan_owned = 0;
	int start_announced = 0;
	int error;

	memset(credential, 0, sizeof(credential));
	memset(&request, 0, sizeof(request));
	memset(&scan_request, 0, sizeof(scan_request));
	memset(&scan, 0, sizeof(scan));
	memset(&status, 0, sizeof(status));
	/* Retain one private credential for the command-owned retry window and
	 * remove it from argv before the first kernel admission. */
	memcpy(credential, passphrase, passphrase_length);
	clear_argument(passphrase);

	error = monotonic_ticks(&now, &frequency);
	if (error != 0 || frequency == 0U ||
	    now > UINT64_MAX - WIFI_CONNECT_SECONDS * frequency) {
		error = error != 0 ? error : EOVERFLOW;
		goto out_secret;
	}
	deadline = now + WIFI_CONNECT_SECONDS * frequency;
	for (;;) {
		if (wifi_interrupted) {
			error = EINTR;
			goto out_secret;
		}
		if (!wifi_quiet && !start_announced) {
			if ((wifi_machine &&
			    printf("WIFI1 connect state=starting generation=0 "
			    "error=0\n") < 0) ||
			    (!wifi_machine &&
			    fputs("Selecting a supported BSS and preparing the radio...\n",
			    stdout) == EOF) || fflush(stdout) != 0) {
				error = EIO;
				goto out_secret;
			}
			start_announced = 1;
		}
		error = request_header(&request, sizeof(request), interface,
		    interface_length);
		if (error != 0)
			goto out_secret;
		memcpy(request.ssid, ssid, ssid_length);
		memcpy(request.passphrase, credential, passphrase_length);
		request.ssid_length = (uint32_t)ssid_length;
		request.passphrase_length = (uint32_t)passphrase_length;
		error = ioctl_error(descriptor, SIOCSWLANCONNECT, &request);
		generation = request.generation;
		clear_bytes(&request, sizeof(request));
		if (error == 0) {
			admitted = 1;
			if (generation == 0U) {
				error = EIO;
				goto out_cancel_connection;
			}
			error = wait_for_connection(descriptor, interface,
			    interface_length, generation, deadline, frequency,
			    &displayed_states, &status);
			if (error == 0) {
				if (!wifi_quiet &&
				    ((wifi_machine &&
				    printf("WIFI1 connect state=%u generation=%llu "
				    "authorized=1 error=0\n", WLAN_STATE_CONNECTED,
				    (unsigned long long)generation) < 0) ||
				    (!wifi_machine &&
				    printf("Connected: controlled port authorized "
				    "(generation=%llu).\n",
				    (unsigned long long)generation) < 0))) {
					error = EIO;
					goto out_cancel_connection;
				}
				clear_bytes(credential, sizeof(credential));
				clear_bytes(&scan_request, sizeof(scan_request));
				clear_bytes(&status, sizeof(status));
				return 0;
			}
			if (!retryable_connect_error(error))
				goto out_cancel_connection;
			attempt_error = error;
			error = monotonic_ticks(&now, &frequency);
			if (error != 0 || now >= deadline) {
				error = error != 0 ? error : ETIMEDOUT;
				goto out_cancel_connection;
			}
			if (connection_in_progress(&status))
				(void)cancel_connection(descriptor, interface,
				    interface_length);
			admitted = 0;
			clear_bytes(&status, sizeof(status));
			if (!wifi_quiet && !retry_announced) {
				if ((wifi_machine &&
				    printf("WIFI1 connect state=retrying generation=%llu "
				    "error=%d\n", (unsigned long long)generation,
				    attempt_error) < 0) ||
				    (!wifi_machine &&
				    fputs("Connection attempt failed; rescanning and retrying...\n",
				    stdout) == EOF)) {
					error = EIO;
					goto out_secret;
				}
				retry_announced = 1;
			}
			error = wait_slice(now, deadline, frequency);
			if (error != 0)
				goto out_secret;
			error = ENOENT;
		}
		if (error != ENOENT && error != EBUSY) {
			/* A failed admission may report the preceding generation.  Only
			 * an ioctl which returned success grants cancellation ownership. */
			if (!retryable_connect_error(error))
				goto out_secret;
			error = monotonic_ticks(&now, &frequency);
			if (error != 0 || now >= deadline) {
				error = error != 0 ? error : ETIMEDOUT;
				goto out_secret;
			}
			error = wait_slice(now, deadline, frequency);
			if (error != 0)
				goto out_secret;
			continue;
		}
		if (error == EBUSY) {
			/* EBUSY is a transient ownership barrier, not evidence that the
			 * BSS cache is missing.  Starting a new scan here used to consume
			 * the shared 30-second deadline and discard a usable snapshot. */
			if (generation != 0U)
				goto out_secret;
			error = scan_status_request(descriptor, interface,
			    interface_length, &scan);
			if (error != 0)
				goto out_secret;
			if (scan.state == WLAN_SCAN_RUNNING) {
				scan_generation = scan.scan_generation;
				scan_owned = 0;
				clear_bytes(&scan, sizeof(scan));
				error = wait_for_scan(descriptor, interface,
				    interface_length, scan_generation, deadline,
				    frequency, &displayed_scan_seconds);
				if (error != 0) {
					if (!retryable_connect_error(error))
						goto out_cancel_scan;
					error = monotonic_ticks(&now, &frequency);
					if (error != 0 || now >= deadline) {
						error = error != 0 ? error : ETIMEDOUT;
						goto out_cancel_scan;
					}
					error = wait_slice(now, deadline, frequency);
					if (error != 0)
						goto out_cancel_scan;
					error = ENOENT;
				}
			} else {
				clear_bytes(&scan, sizeof(scan));
				if (!wifi_quiet && !busy_announced) {
					if ((wifi_machine &&
					    printf("WIFI1 connect state=waiting generation=0 "
					    "error=%d\n", EBUSY) < 0) ||
					    (!wifi_machine &&
					    fputs("Waiting for the radio to become ready...\n",
					    stdout) == EOF) || fflush(stdout) != 0) {
						error = EIO;
						goto out_secret;
					}
					busy_announced = 1;
				}
				error = monotonic_ticks(&now, &frequency);
				if (error != 0 || now >= deadline) {
					error = error != 0 ? error : ETIMEDOUT;
					goto out_secret;
				}
				error = wait_slice(now, deadline, frequency);
				if (error != 0)
					goto out_secret;
			}
			if (error != ENOENT)
				continue;
		}
		for (;;) {
			if (wifi_interrupted) {
				error = EINTR;
				goto out_cancel_scan;
			}
			error = scan_status_request(descriptor, interface,
			    interface_length, &scan);
			if (error != 0)
				goto out_secret;
			if (scan.state != WLAN_SCAN_RUNNING) {
				clear_bytes(&scan, sizeof(scan));
				if (request_header(&scan_request,
				    sizeof(scan_request), interface,
				    interface_length) != 0) {
					error = EINVAL;
					goto out_secret;
				}
				scan_request.action = WLAN_SCAN_START;
				error = ioctl_error(descriptor, SIOCSWLANSCAN,
				    &scan_request);
				scan_generation = scan_request.generation;
				clear_bytes(&scan_request, sizeof(scan_request));
				if (error != 0) {
					if (!retryable_connect_error(error))
						goto out_secret;
					error = monotonic_ticks(&now, &frequency);
					if (error != 0 || now >= deadline) {
						error = error != 0 ? error : ETIMEDOUT;
						goto out_secret;
					}
					error = wait_slice(now, deadline, frequency);
					if (error != 0)
						goto out_secret;
					continue;
				}
				scan_owned = 1;
			} else {
				scan_generation = scan.scan_generation;
				scan_owned = 0;
				clear_bytes(&scan, sizeof(scan));
			}
			error = wait_for_scan(descriptor, interface,
			    interface_length, scan_generation, deadline, frequency,
			    &displayed_scan_seconds);
			if (error != 0) {
				if (error == ESTALE)
					scan_owned = 0;
				if (scan_owned)
					(void)cancel_scan(descriptor, interface,
					    interface_length);
				scan_owned = 0;
				if (!retryable_connect_error(error))
					goto out_cancel_scan;
				error = monotonic_ticks(&now, &frequency);
				if (error != 0 || now >= deadline) {
					error = error != 0 ? error : ETIMEDOUT;
					goto out_cancel_scan;
				}
				error = wait_slice(now, deadline, frequency);
				if (error != 0)
					goto out_cancel_scan;
				continue;
			}
			scan_owned = 0;
			if (!wifi_quiet &&
			    ((wifi_machine &&
			    printf("WIFI1 connect state=selecting generation=0 "
			    "error=0\n") < 0) ||
			    (!wifi_machine &&
			    printf("Selecting a supported BSS...\n") < 0))) {
				error = EIO;
				goto out_secret;
			}
			error = monotonic_ticks(&now, &frequency);
			if (error != 0 || now >= deadline) {
				error = error != 0 ? error : ETIMEDOUT;
				goto out_secret;
			}
			break;
		}
	}

out_cancel_scan:
	print_failure_status(descriptor, interface, interface_length);
	if (scan_owned)
		(void)cancel_scan(descriptor, interface, interface_length);
out_secret:
	clear_argument(passphrase);
	clear_bytes(credential, sizeof(credential));
	clear_bytes(&request, sizeof(request));
	clear_bytes(&scan_request, sizeof(scan_request));
	clear_bytes(&status, sizeof(status));
	return error;

out_cancel_connection:
	print_failure_status(descriptor, interface, interface_length);
	if (admitted && status.operation_generation == generation &&
	    connection_in_progress(&status))
		(void)cancel_connection(descriptor, interface, interface_length);
	clear_argument(passphrase);
	clear_bytes(credential, sizeof(credential));
	clear_bytes(&request, sizeof(request));
	clear_bytes(&scan_request, sizeof(scan_request));
	clear_bytes(&status, sizeof(status));
	return error;
}

/* Names the public failing stage before a connect error is reported.  The
 * summary is best effort and never masks or replaces the original error. */
static void
print_failure_status(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_status_request status;

	if (wifi_quiet)
		return;
	if (status_request(descriptor, interface, interface_length,
	    &status) == 0)
		(void)print_status(&status);
	clear_bytes(&status, sizeof(status));
}

/* Best-effort cancellation used only while retiring this command's scan. */
static int
cancel_scan(int descriptor, const char *interface, size_t interface_length)
{
	struct wlan_scan_request request;
	int error;

	error = request_header(&request, sizeof(request), interface,
	    interface_length);
	if (error != 0)
		return error;
	request.action = WLAN_SCAN_STOP;
	error = ioctl_error(descriptor, SIOCSWLANSCAN, &request);
	clear_bytes(&request, sizeof(request));
	return error;
}

/* Best-effort cancellation used only after this command admitted connect. */
static int
cancel_connection(int descriptor, const char *interface,
	size_t interface_length)
{
	struct wlan_disconnect_request request;
	int error;

	error = request_header(&request, sizeof(request), interface,
	    interface_length);
	if (error != 0)
		return error;
	error = ioctl_error(descriptor, SIOCSWLANDISCONNECT, &request);
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
	if (error == 0) {
		if (wifi_machine) {
			if (printf("WIFI1 connect state=%u generation=%llu error=%d\n",
			    request.state, (unsigned long long)request.generation,
			    request.terminal_error) < 0)
				error = EIO;
		} else if (!wifi_quiet &&
		    printf("disconnected state=%s generation=%llu\n",
		    wlan_state_name(request.state),
		    (unsigned long long)request.generation) < 0) {
			error = EIO;
		}
	}
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
	int sleep_result;

	if (wifi_interrupted)
		return EINTR;
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
	sleep_result = nanosleep(&delay, NULL);
	if (wifi_interrupted)
		return EINTR;
	if (sleep_result != 0)
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Polls the public state until authorized carrier or a bounded failure. */
static int
wait_for_connection(int descriptor, const char *interface,
	size_t interface_length, uint64_t generation, uint64_t deadline,
	uint64_t frequency, uint32_t *displayed_states,
	struct wlan_status_request *result)
{
	uint64_t effective_deadline;
	uint64_t now;
	uint32_t state_bit;
	unsigned polls;
	int error;

	for (polls = 0U; polls <= WIFI_FALLBACK_POLLS; polls++) {
		if (wifi_interrupted)
			return EINTR;
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
		state_bit = result->state < 32U ?
		    1U << result->state : 0U;
		if (state_bit != 0U && (*displayed_states & state_bit) == 0U) {
			error = print_connection_progress(result->state, generation);
			if (error != 0)
				return error;
			*displayed_states |= state_bit;
		}
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
		error = monotonic_ticks(&now, &frequency);
		if (error != 0)
			return error;
		effective_deadline = deadline;
		if (result->deadline_ticks != 0U &&
		    result->deadline_ticks < effective_deadline)
			effective_deadline = result->deadline_ticks;
		if (now >= effective_deadline)
			return ETIMEDOUT;
		error = wait_slice(now, effective_deadline, frequency);
		if (error != 0)
			return error;
	}
	return ETIMEDOUT;
}

/* Waits for one scan snapshot without extending the command-wide deadline. */
static int
wait_for_scan(int descriptor, const char *interface,
	size_t interface_length, uint64_t generation, uint64_t deadline,
	uint64_t frequency, uint64_t *displayed_seconds)
{
	struct wlan_scan_status_request scan;
	uint64_t remaining;
	uint64_t now;
	unsigned polls;
	/* An interactive terminal refreshes one countdown line in place; a
	 * pipe or log receives one complete line per refresh instead. */
	int overwrite = !wifi_quiet && !wifi_machine &&
	    isatty(STDOUT_FILENO) == 1;
	int line_open = 0;
	int error;

	memset(&scan, 0, sizeof(scan));
	for (polls = 0U; polls <= WIFI_FALLBACK_POLLS; polls++) {
		if (wifi_interrupted) {
			error = EINTR;
			goto out;
		}
		error = scan_status_request(descriptor, interface,
		    interface_length, &scan);
		if (error != 0)
			goto out;
		if (generation == 0U || scan.scan_generation != generation) {
			error = ESTALE;
			goto out;
		}
		if (scan.state == WLAN_SCAN_COMPLETE) {
			if (scan.generation != generation) {
				error = ESTALE;
				goto out;
			}
			error = 0;
			goto out;
		}
		if (scan.state == WLAN_SCAN_FAILED) {
			error = scan.terminal_error != 0 ?
			    scan.terminal_error : EIO;
			goto out;
		}
		if (scan.state == WLAN_SCAN_CANCELLED ||
		    scan.state == WLAN_SCAN_IDLE) {
			error = ECANCELED;
			goto out;
		}
		if (scan.state != WLAN_SCAN_RUNNING) {
			error = EIO;
			goto out;
		}
		error = monotonic_ticks(&now, &frequency);
		if (error != 0)
			goto out;
		if (now >= deadline || polls == WIFI_FALLBACK_POLLS) {
			error = ETIMEDOUT;
			goto out;
		}
		remaining = (deadline - now + frequency - 1U) / frequency;
		if (!wifi_quiet && remaining != *displayed_seconds) {
			if (wifi_machine) {
				if (printf("WIFI1 scan state=%u generation=%llu "
				    "results=%u truncated=%u error=%d remaining=%llu\n",
				    scan.state,
				    (unsigned long long)scan.scan_generation,
				    scan.result_count, scan.truncated,
				    scan.terminal_error,
				    (unsigned long long)remaining) < 0) {
					error = EIO;
					goto out;
				}
			} else if (printf(overwrite ?
			    "\rScanning... (%2llu seconds remaining)" :
			    "Scanning... (%2llu seconds remaining)\n",
			    (unsigned long long)remaining) < 0 ||
			    (overwrite && fflush(stdout) != 0)) {
				error = EIO;
				goto out;
			}
			line_open = overwrite;
			*displayed_seconds = remaining;
		}
		error = wait_slice(now, deadline, frequency);
		if (error != 0)
			goto out;
		clear_bytes(&scan, sizeof(scan));
	}
	error = ETIMEDOUT;
out:
	/* Whatever follows starts on its own line, even after a failure. */
	if (line_open && putchar('\n') == EOF && error == 0)
		error = EIO;
	clear_bytes(&scan, sizeof(scan));
	return error;
}

/* Announces meaningful state transitions without exposing credentials. */
static int
print_connection_progress(uint32_t state, uint64_t generation)
{
	const char *message;

	if (wifi_quiet)
		return 0;
	if (wifi_machine) {
		if (printf("WIFI1 connect state=%u generation=%llu error=0\n",
		    state, (unsigned long long)generation) < 0)
			return EIO;
		return 0;
	}
	switch (state) {
	case WLAN_STATE_AUTHENTICATING:
		message = "Preparing the radio and starting 802.11 authentication...\n";
		break;
	case WLAN_STATE_ASSOCIATING:
		message = "Associating with the access point...\n";
		break;
	case WLAN_STATE_FOUR_WAY:
		message = "Completing the WPA2 four-way handshake...\n";
		break;
	default:
		return 0;
	}
	return fputs(message, stdout) == EOF ? EIO : 0;
}

/* Prints a bounded public status record. */
static int
print_status(const struct wlan_status_request *status)
{
	char hexadecimal_bssid[WIFI_HEX_BSSID_SIZE];
	char security[WIFI_SECURITY_TEXT_SIZE];
	int error;

	if (wifi_quiet)
		return 0;
	if (wifi_machine) {
		error = hex_bytes(status->bssid, sizeof(status->bssid),
		    hexadecimal_bssid, sizeof(hexadecimal_bssid));
		if (error != 0)
			return error;
		if (printf("WIFI1 status state=%u scan=%u "
		    "operation-generation=%llu scan-generation=%llu "
		    "snapshot-generation=%llu administrative=%u "
		    "authenticated=%u associated=%u key=%u authorized=%u "
		    "retries=%u error=%d deadline=%llu bssid=%s channel=%u "
		    "frequency=%u rssi=%d security=%08x\n", status->state,
		    status->scan_state,
		    (unsigned long long)status->operation_generation,
		    (unsigned long long)status->scan_generation,
		    (unsigned long long)status->snapshot_generation,
		    status->administrative_up, status->authenticated,
		    status->associated, status->key_installed,
		    status->controlled_port, status->retry_count,
		    status->terminal_error,
		    (unsigned long long)status->deadline_ticks,
		    hexadecimal_bssid, status->channel,
		    status->center_frequency_mhz, status->rssi_dbm,
		    status->security) < 0)
			return EIO;
		return 0;
	}
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

/* Accepts the direct human forms after the optional global --quiet. */
static int
parse_operation(int argc, char **argv, enum wifi_operation *operation)
{
	if (argc == 3 && strcmp(argv[2], "up") == 0) {
		*operation = WIFI_OPERATION_UP;
		return 0;
	}
	if (argc == 3 && strcmp(argv[2], "down") == 0) {
		*operation = WIFI_OPERATION_DOWN;
		return 0;
	}
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
	if ((!wifi_machine && argc == 5 && strcmp(argv[2], "connect") == 0) ||
	    (wifi_machine && argc == 4 && strcmp(argv[2], "connect") == 0)) {
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
	case WIFI_OPERATION_UP:
		return "up";
	case WIFI_OPERATION_DOWN:
		return "down";
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

/* Reads one exact EOF-terminated machine credential from its private fd. */
static int
read_machine_passphrase(int descriptor, char *passphrase, size_t capacity,
	size_t *length)
{
	ssize_t count;
	size_t index;
	size_t used;
	int error;

	if (descriptor != WIFI_SECRET_DESCRIPTOR || passphrase == NULL ||
	    length == NULL || capacity != WLAN_PASSPHRASE_STORAGE)
		return EINVAL;
	memset(passphrase, 0, capacity);
	*length = 0U;
	used = 0U;
	while (used < capacity) {
		count = read(descriptor, passphrase + used, capacity - used);
		if (count > 0) {
			used += (size_t)count;
			continue;
		}
		if (count == 0)
			break;
		if (errno == EINTR)
			continue;
		error = errno != 0 ? errno : EIO;
		clear_bytes(passphrase, capacity);
		return error;
	}
	if (used == capacity) {
		clear_bytes(passphrase, capacity);
		return E2BIG;
	}
	if (used < WLAN_PASSPHRASE_MIN || used > WLAN_PASSPHRASE_MAX) {
		clear_bytes(passphrase, capacity);
		return EINVAL;
	}
	for (index = 0U; index < used; index++) {
		if ((unsigned char)passphrase[index] < 0x20U ||
		    (unsigned char)passphrase[index] > 0x7eU) {
			clear_bytes(passphrase, capacity);
			return EINVAL;
		}
	}
	*length = used;
	return 0;
}

/* Emits the sole final machine record and nothing after it. */
static int
machine_terminal(int error)
{
	int print_error;

	if (error < 0)
		error = EIO;
	if (error == 0)
		print_error = printf("WIFI1 terminal ok 0\n") < 0;
	else
		print_error = printf("WIFI1 terminal error %d\n", error) < 0;
	if (print_error || fflush(stdout) != 0)
		return 1;
	return error == 0 ? 0 : 1;
}

/* Encodes counted bytes as canonical lower-case hexadecimal. */
static int
hex_bytes(const uint8_t *input, size_t length, char *output, size_t capacity)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	if (input == NULL || output == NULL || length > (capacity - 1U) / 2U)
		return EINVAL;
	for (index = 0U; index < length; index++) {
		output[index * 2U] = digits[input[index] >> 4];
		output[index * 2U + 1U] = digits[input[index] & 0x0fU];
	}
	output[length * 2U] = '\0';
	return 0;
}

/* Classifies failures for which a fresh scan and attempt can help. */
static int
retryable_connect_error(int error)
{
	switch (error) {
	case EAGAIN:
	case EBUSY:
	case ECONNREFUSED:
	case ECONNRESET:
	case ENOENT:
	case ETIMEDOUT:
		return 1;
	default:
		return 0;
	}
}

/* Reports whether a command-owned generation still needs cancellation. */
static int
connection_in_progress(const struct wlan_status_request *status)
{
	switch (status->state) {
	case WLAN_STATE_AUTHENTICATING:
	case WLAN_STATE_ASSOCIATING:
	case WLAN_STATE_FOUR_WAY:
		return 1;
	default:
		return 0;
	}
}

/* Requests orderly cancellation from the synchronous command loop. */
static void
interrupt_handler(int signal_number)
{
	(void)signal_number;
	wifi_interrupted = 1;
}
