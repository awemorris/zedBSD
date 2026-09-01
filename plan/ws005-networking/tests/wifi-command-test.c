/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* NET-T24 minimum direct wifi command fixture. */

#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/wlan.h>

#define FIXTURE_DESCRIPTOR 41
#define FIXTURE_CLOCK_HZ 100U
#define FIXTURE_SCAN_GENERATION UINT64_C(17)
#define FIXTURE_CONNECT_GENERATION UINT64_C(29)
#define FIXTURE_OUTPUT_MAX 8192U
#define FIXTURE_TRACE_MAX 16U
#define FIXTURE_SSID "fixture-ap"
#define FIXTURE_PASSPHRASE "fixture-passphrase"

struct fixture_trace {
	unsigned long command;
	uint32_t scan_action;
};

static struct {
	unsigned socket_calls;
	unsigned close_calls;
	unsigned ioctl_calls;
	unsigned sleep_calls;
	unsigned clock_calls;
	unsigned argv_clear_calls;
	unsigned request_clear_calls;
	unsigned connect_status_polls;
	unsigned scan_starts;
	unsigned scan_stops;
	unsigned connect_seen;
	unsigned connect_redacted;
	unsigned disconnect_seen;
	unsigned output_overflow;
	uint64_t ticks;
	uint64_t connect_deadline;
	void *connect_request;
	void *argv_secret;
	size_t argv_secret_length;
	struct fixture_trace trace[FIXTURE_TRACE_MAX];
	char output[FIXTURE_OUTPUT_MAX];
	size_t output_length;
	char error[FIXTURE_OUTPUT_MAX];
	size_t error_length;
} fixture;

static int fixture_errno;

static void fixture_fail(const char *, const char *);
static void fixture_require(int, const char *, const char *);
static int fixture_bytes_zero(const void *, size_t);
static void fixture_explicit_clear(void *, size_t);
static int fixture_socket(int, int, int);
static int fixture_ioctl(int, unsigned long, ...);
static int fixture_close(int);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static long fixture_sysconf(int);
static int fixture_nanosleep(const struct timespec *, struct timespec *);
static int fixture_printf(const char *, ...);
static int fixture_fprintf(FILE *, const char *, ...);
static int fixture_fputs(const char *, FILE *);
static int fixture_putchar(int);
static int fixture_fflush(FILE *);
static int fixture_ferror(FILE *);

#define socket fixture_socket
#define ioctl fixture_ioctl
#define close fixture_close
#define clock_gettime fixture_clock_gettime
#define sysconf fixture_sysconf
#define nanosleep fixture_nanosleep
#define printf fixture_printf
#define fprintf fixture_fprintf
#define fputs fixture_fputs
#define putchar fixture_putchar
#define fflush fixture_fflush
#define ferror fixture_ferror
#define WIFI_EXPLICIT_CLEAR(buffer, size) \
	fixture_explicit_clear((buffer), (size))
#define main wifi_program_main
#include "userland/base/wifi/main.c"
#undef main
#undef WIFI_EXPLICIT_CLEAR
#undef ferror
#undef fflush
#undef putchar
#undef fputs
#undef fprintf
#undef printf
#undef nanosleep
#undef sysconf
#undef clock_gettime
#undef close
#undef ioctl
#undef socket

int *
__libc_errno_location(void)
{
	return &fixture_errno;
}

static void
fixture_fail(const char *test, const char *message)
{
	fprintf(stderr, "wifi-command-test: %s: %s\n", test, message);
	exit(1);
}

static void
fixture_require(int condition, const char *test, const char *message)
{
	if (!condition)
		fixture_fail(test, message);
}

static int
fixture_bytes_zero(const void *storage, size_t length)
{
	const uint8_t *bytes = storage;

	while (length-- != 0U)
		if (*bytes++ != 0U)
			return 0;
	return 1;
}

static void
fixture_explicit_clear(void *storage, size_t length)
{
	volatile uint8_t *bytes = storage;
	void *original = storage;
	size_t original_length = length;
	int argv_secret = storage == fixture.argv_secret;
	int connect_request = storage == fixture.connect_request;

	while (length-- != 0U)
		*bytes++ = 0U;
	if (argv_secret)
		fixture.argv_clear_calls++;
	if (connect_request) {
		fixture_require(original_length ==
		    sizeof(struct wlan_connect_request), "clear",
		    "connect request clear size");
		fixture_require(fixture_bytes_zero(original, original_length),
		    "clear", "connect request retained bytes");
		fixture.request_clear_calls++;
		fixture.connect_request = NULL;
	}
}

static void
fixture_reset(void)
{
	memset(&fixture, 0, sizeof(fixture));
	fixture.ticks = 100U;
	fixture_errno = 0;
}

static int
fixture_append(char *output, size_t *used, const char *format,
	va_list arguments)
{
	int length;

	if (*used >= FIXTURE_OUTPUT_MAX) {
		fixture.output_overflow = 1U;
		return -1;
	}
	length = vsnprintf(output + *used, FIXTURE_OUTPUT_MAX - *used,
	    format, arguments);
	if (length < 0 || (size_t)length >= FIXTURE_OUTPUT_MAX - *used) {
		fixture.output_overflow = 1U;
		return -1;
	}
	*used += (size_t)length;
	return length;
}

static int
fixture_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = fixture_append(fixture.output, &fixture.output_length, format,
	    arguments);
	va_end(arguments);
	return result;
}

static int
fixture_fprintf(FILE *stream, const char *format, ...)
{
	char *output;
	size_t *used;
	va_list arguments;
	int result;

	fixture_require(stream == stdout || stream == stderr, "output",
	    "unexpected stream");
	output = stream == stdout ? fixture.output : fixture.error;
	used = stream == stdout ? &fixture.output_length :
	    &fixture.error_length;
	va_start(arguments, format);
	result = fixture_append(output, used, format, arguments);
	va_end(arguments);
	return result;
}

static int
fixture_fputs(const char *text, FILE *stream)
{
	char *output;
	size_t *used;
	size_t length = strlen(text);

	fixture_require(stream == stdout || stream == stderr, "output",
	    "unexpected fputs stream");
	output = stream == stdout ? fixture.output : fixture.error;
	used = stream == stdout ? &fixture.output_length :
	    &fixture.error_length;
	if (length >= FIXTURE_OUTPUT_MAX - *used) {
		fixture.output_overflow = 1U;
		return EOF;
	}
	memcpy(output + *used, text, length + 1U);
	*used += length;
	return 0;
}

static int
fixture_putchar(int character)
{
	if (fixture.output_length + 1U >= FIXTURE_OUTPUT_MAX) {
		fixture.output_overflow = 1U;
		return EOF;
	}
	fixture.output[fixture.output_length++] = (char)character;
	fixture.output[fixture.output_length] = '\0';
	return (unsigned char)character;
}

static int
fixture_fflush(FILE *stream)
{
	fixture_require(stream == stdout, "output", "unexpected flush stream");
	return 0;
}

static int
fixture_ferror(FILE *stream)
{
	fixture_require(stream == stdout, "output", "unexpected error stream");
	return 0;
}

static int
fixture_socket(int domain, int type, int protocol)
{
	fixture_require(domain == AF_INET && type == SOCK_DGRAM &&
	    protocol == IPPROTO_UDP, "socket", "socket arguments");
	fixture.socket_calls++;
	return FIXTURE_DESCRIPTOR;
}

static int
fixture_close(int descriptor)
{
	fixture_require(descriptor == FIXTURE_DESCRIPTOR, "close",
	    "descriptor");
	fixture.close_calls++;
	return 0;
}

static int
fixture_clock_gettime(clockid_t clock, struct timespec *now)
{
	fixture_require(clock == CLOCK_MONOTONIC && now != NULL, "clock",
	    "clock request");
	now->tv_sec = (time_t)(fixture.ticks / FIXTURE_CLOCK_HZ);
	now->tv_nsec = (long)((fixture.ticks % FIXTURE_CLOCK_HZ) *
	    (1000000000U / FIXTURE_CLOCK_HZ));
	fixture.clock_calls++;
	return 0;
}

static long
fixture_sysconf(int name)
{
	fixture_require(name == _SC_CLK_TCK, "clock", "clock frequency name");
	return FIXTURE_CLOCK_HZ;
}

static int
fixture_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
	uint64_t nanoseconds;

	fixture_require(delay != NULL && remaining == NULL, "wait",
	    "sleep arguments");
	fixture_require(delay->tv_sec == 0 && delay->tv_nsec > 0 &&
	    delay->tv_nsec <= 100000000L, "wait", "sleep bound");
	nanoseconds = (uint64_t)delay->tv_nsec;
	fixture.ticks += (nanoseconds * FIXTURE_CLOCK_HZ + 999999999ULL) /
	    1000000000ULL;
	fixture.sleep_calls++;
	return 0;
}

static size_t
fixture_request_size(unsigned long command)
{
	switch (command) {
	case SIOCSWLANSCAN:
		return sizeof(struct wlan_scan_request);
	case SIOCGWLANSCAN:
		return sizeof(struct wlan_scan_status_request);
	case SIOCGWLANBSS:
		return sizeof(struct wlan_bss_request);
	case SIOCSWLANCONNECT:
		return sizeof(struct wlan_connect_request);
	case SIOCSWLANDISCONNECT:
		return sizeof(struct wlan_disconnect_request);
	case SIOCGWLANSTATUS:
		return sizeof(struct wlan_status_request);
	default:
		fixture_fail("ioctl", "unknown command");
		return 0U;
	}
}

static void
fixture_request_header(const struct wlan_ioctl_header *header,
	unsigned long command)
{
	fixture_require(header != NULL, "ioctl", "null request");
	fixture_require(strcmp(header->ifr_name, "wlan0") == 0, "ioctl",
	    "interface name");
	fixture_require(header->version == WLAN_ABI_VERSION, "ioctl",
	    "ABI version");
	fixture_require(header->size == fixture_request_size(command), "ioctl",
	    "request size");
}

static void
fixture_record(unsigned long command, uint32_t scan_action)
{
	unsigned index = fixture.ioctl_calls;

	fixture_require(index < FIXTURE_TRACE_MAX, "ioctl", "trace overflow");
	fixture.trace[index].command = command;
	fixture.trace[index].scan_action = scan_action;
	fixture.ioctl_calls++;
}

static void
fixture_fill_status(struct wlan_status_request *status, uint32_t state)
{
	status->operation_generation = FIXTURE_CONNECT_GENERATION;
	status->scan_generation = FIXTURE_SCAN_GENERATION;
	status->snapshot_generation = FIXTURE_SCAN_GENERATION;
	status->deadline_ticks = fixture.connect_deadline;
	status->cache_sequence = 1U;
	status->state = state;
	status->scan_state = WLAN_SCAN_COMPLETE;
	status->administrative_up = 1U;
	status->terminal_error = 0;
	if (state == WLAN_STATE_ASSOCIATING) {
		status->authenticated = 1U;
	} else if (state == WLAN_STATE_FOUR_WAY) {
		status->authenticated = 1U;
		status->associated = 1U;
	} else if (state == WLAN_STATE_CONNECTED) {
		status->authenticated = 1U;
		status->associated = 1U;
		status->key_installed = 1U;
		status->controlled_port = 1U;
		memcpy(status->bssid,
		    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x42U }, 6U);
		status->channel = 6U;
		status->center_frequency_mhz = 2437U;
		status->rssi_dbm = -35;
		status->security = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 |
		    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
	}
}

static int
fixture_ioctl(int descriptor, unsigned long command, ...)
{
	struct wlan_ioctl_header *header;
	void *argument;
	va_list arguments;

	fixture_require(descriptor == FIXTURE_DESCRIPTOR, "ioctl",
	    "descriptor");
	va_start(arguments, command);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	header = argument;
	fixture_request_header(header, command);

	if (command == SIOCSWLANSCAN) {
		struct wlan_scan_request *request = argument;

		fixture_require(request->flags == 0U &&
		    request->generation == 0U && request->state == 0U &&
		    request->terminal_error == 0 &&
		    fixture_bytes_zero(request->reserved,
			sizeof(request->reserved)), "scan", "request input");
		fixture_require(request->action == WLAN_SCAN_START ||
		    request->action == WLAN_SCAN_STOP, "scan", "action");
		fixture_record(command, request->action);
		request->generation = FIXTURE_SCAN_GENERATION;
		request->terminal_error = 0;
		if (request->action == WLAN_SCAN_START) {
			fixture_require(fixture.scan_starts == 0U, "scan",
			    "duplicate start");
			fixture.scan_starts++;
			request->state = WLAN_SCAN_RUNNING;
		} else {
			fixture_require(fixture.scan_starts == 1U &&
			    fixture.disconnect_seen == 1U, "scan",
			    "stop ordering");
			fixture.scan_stops++;
			request->state = WLAN_SCAN_CANCELLED;
		}
		return 0;
	}
	if (command == SIOCGWLANSCAN) {
		struct wlan_scan_status_request *request = argument;

		fixture_require(fixture_bytes_zero((uint8_t *)request +
		    sizeof(*header), sizeof(*request) - sizeof(*header)), "list",
		    "scan-status input");
		fixture_require(fixture.scan_starts == 1U, "list",
		    "list before search");
		fixture_record(command, 0U);
		request->generation = FIXTURE_SCAN_GENERATION;
		request->scan_generation = FIXTURE_SCAN_GENERATION;
		request->cache_sequence = 1U;
		request->state = WLAN_SCAN_COMPLETE;
		request->result_count = 1U;
		return 0;
	}
	if (command == SIOCGWLANBSS) {
		struct wlan_bss_request *request = argument;

		fixture_require(request->generation == FIXTURE_SCAN_GENERATION &&
		    request->index == 0U && request->reserved0 == 0U &&
		    fixture_bytes_zero(&request->bss, sizeof(request->bss)) &&
		    fixture_bytes_zero(request->reserved,
			sizeof(request->reserved)), "list", "BSS request input");
		fixture_record(command, 0U);
		memcpy(request->bss.ssid, FIXTURE_SSID,
		    sizeof(FIXTURE_SSID) - 1U);
		request->bss.ssid_length = sizeof(FIXTURE_SSID) - 1U;
		memcpy(request->bss.bssid,
		    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x42U }, 6U);
		request->bss.channel = 6U;
		request->bss.center_frequency_mhz = 2437U;
		request->bss.rssi_dbm = -35;
		request->bss.age_ms = 25U;
		request->bss.capability = 0x0011U;
		request->bss.security = WLAN_SECURITY_PRIVACY |
		    WLAN_SECURITY_WPA2 | WLAN_SECURITY_CCMP |
		    WLAN_SECURITY_PSK;
		return 0;
	}
	if (command == SIOCSWLANCONNECT) {
		struct wlan_connect_request *request = argument;

		fixture_require(fixture.connect_seen == 0U, "connect",
		    "duplicate connect");
		fixture_require(request->ssid_length == sizeof(FIXTURE_SSID) - 1U &&
		    memcmp(request->ssid, FIXTURE_SSID,
			sizeof(FIXTURE_SSID) - 1U) == 0, "connect", "SSID input");
		fixture_require(request->passphrase_length ==
		    sizeof(FIXTURE_PASSPHRASE) - 1U &&
		    memcmp(request->passphrase, FIXTURE_PASSPHRASE,
			sizeof(FIXTURE_PASSPHRASE) - 1U) == 0, "connect",
		    "passphrase input");
		fixture_require(request->generation == 0U && request->state == 0U &&
		    request->terminal_error == 0 &&
		    fixture_bytes_zero(request->reserved,
			sizeof(request->reserved)), "connect", "request input");
		fixture_record(command, 0U);
		fixture.connect_seen = 1U;
		fixture.connect_request = request;
		request->generation = FIXTURE_CONNECT_GENERATION;
		request->state = WLAN_STATE_AUTHENTICATING;
		fixture.connect_deadline = fixture.ticks + 30U * FIXTURE_CLOCK_HZ;
		memset(request->passphrase, 0, sizeof(request->passphrase));
		request->passphrase_length = 0U;
		fixture.connect_redacted = fixture_bytes_zero(request->passphrase,
		    sizeof(request->passphrase));
		return 0;
	}
	if (command == SIOCGWLANSTATUS) {
		struct wlan_status_request *request = argument;
		uint32_t state;

		fixture_require(fixture_bytes_zero((uint8_t *)request +
		    sizeof(*header), sizeof(*request) - sizeof(*header)), "status",
		    "status request input");
		fixture_record(command, 0U);
		if (!fixture.connect_seen) {
			request->scan_generation = FIXTURE_SCAN_GENERATION;
			request->snapshot_generation = FIXTURE_SCAN_GENERATION;
			request->cache_sequence = 1U;
			request->state = WLAN_STATE_IDLE;
			request->scan_state = WLAN_SCAN_COMPLETE;
			request->administrative_up = 1U;
			return 0;
		}
		fixture_require(fixture.request_clear_calls == 1U, "connect",
		    "connect request was not cleared before status polling");
		fixture_require(fixture.connect_status_polls < 4U, "connect",
		    "excess status polls");
		state = fixture.connect_status_polls == 0U ?
		    WLAN_STATE_AUTHENTICATING :
		    (fixture.connect_status_polls == 1U ? WLAN_STATE_ASSOCIATING :
		    (fixture.connect_status_polls == 2U ? WLAN_STATE_FOUR_WAY :
		    WLAN_STATE_CONNECTED));
		fixture.connect_status_polls++;
		fixture_fill_status(request, state);
		return 0;
	}
	if (command == SIOCSWLANDISCONNECT) {
		struct wlan_disconnect_request *request = argument;

		fixture_require(fixture.connect_status_polls == 4U,
		    "disconnect", "disconnect before authorized status");
		fixture_require(request->generation == 0U && request->flags == 0U &&
		    request->state == 0U && request->terminal_error == 0 &&
		    fixture_bytes_zero(request->reserved,
			sizeof(request->reserved)), "disconnect", "request input");
		fixture_record(command, 0U);
		request->generation = FIXTURE_CONNECT_GENERATION;
		request->state = WLAN_STATE_IDLE;
		fixture.disconnect_seen = 1U;
		return 0;
	}
	fixture_fail("ioctl", "unhandled command");
	return -1;
}

static int
fixture_invoke(int argc, char **argv, char *secret, size_t secret_length)
{
	fixture.argv_secret = secret;
	fixture.argv_secret_length = secret_length;
	return wifi_program_main(argc, argv);
}

static void
fixture_expect_output_order(void)
{
	static const char *const markers[] = {
		"search state=scanning",
		"scan state=complete",
		"ssid=\"" FIXTURE_SSID "\"",
		"state=idle scan=complete",
		"connected state=connected",
		"disconnected state=idle",
		"search state=cancelled"
	};
	const char *cursor = fixture.output;
	const char *found;
	unsigned index;

	for (index = 0U; index < sizeof(markers) / sizeof(markers[0]); index++) {
		found = strstr(cursor, markers[index]);
		fixture_require(found != NULL, "normal", "missing output marker");
		cursor = found + strlen(markers[index]);
	}
}

static void
fixture_normal_sequence(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANSCAN,
		SIOCGWLANSCAN,
		SIOCGWLANBSS,
		SIOCGWLANSTATUS,
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCSWLANDISCONNECT,
		SIOCSWLANSCAN
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *search_start[] = { "wifi", interface, "search", "start", NULL };
	char *list[] = { "wifi", interface, "list", NULL };
	char *status[] = { "wifi", interface, "status", NULL };
	char *connect[] = { "wifi", interface, "connect", ssid, passphrase,
	    NULL };
	char *disconnect[] = { "wifi", interface, "disconnect", NULL };
	char *search_stop[] = { "wifi", interface, "search", "stop", NULL };
	unsigned index;

	fixture_reset();
	fixture_require(fixture_invoke(4, search_start, NULL, 0U) == 0,
	    "normal", "search start status");
	fixture_require(fixture_invoke(3, list, NULL, 0U) == 0, "normal",
	    "list status");
	fixture_require(fixture_invoke(3, status, NULL, 0U) == 0, "normal",
	    "status status");
	fixture_require(fixture_invoke(5, connect, passphrase,
	    sizeof(passphrase) - 1U) == 0, "normal", "connect status");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "normal", "argv secret retained");
	fixture_require(fixture_invoke(3, disconnect, NULL, 0U) == 0,
	    "normal", "disconnect status");
	fixture_require(fixture_invoke(4, search_stop, NULL, 0U) == 0,
	    "normal", "search stop status");

	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "normal", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "normal", "ioctl sequence");
	fixture_require(fixture.trace[0].scan_action == WLAN_SCAN_START &&
	    fixture.trace[10].scan_action == WLAN_SCAN_STOP, "normal",
	    "scan action sequence");
	fixture_require(fixture.socket_calls == 6U && fixture.close_calls == 6U,
	    "normal", "socket lifecycle");
	fixture_require(fixture.scan_starts == 1U && fixture.scan_stops == 1U &&
	    fixture.connect_seen == 1U && fixture.disconnect_seen == 1U,
	    "normal", "operation sequence");
	fixture_require(fixture.connect_redacted == 1U &&
	    fixture.request_clear_calls == 1U &&
	    fixture.argv_clear_calls >= 2U, "normal", "secret clearing");
	fixture_require(fixture.connect_status_polls == 4U &&
	    fixture.sleep_calls == 3U && fixture.clock_calls == 4U, "normal",
	    "bounded connect wait");
	fixture_require(!fixture.output_overflow &&
	    fixture.output_length < FIXTURE_OUTPUT_MAX &&
	    fixture.error_length == 0U, "normal", "bounded clean output");
	fixture_require(strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL, "normal",
	    "secret output");
	fixture_expect_output_order();
}

static void
fixture_expect_rejected(const char *test, char *interface, char *ssid,
	char *passphrase, size_t passphrase_length)
{
	char *arguments[] = { "wifi", interface, "connect", ssid, passphrase,
	    NULL };

	fixture_reset();
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    passphrase_length) == 1, test, "status");
	fixture_require(fixture.socket_calls == 0U && fixture.ioctl_calls == 0U &&
	    fixture.close_calls == 0U, test, "reached socket or ioctl");
	fixture_require(fixture.argv_clear_calls >= 1U, test,
	    "argv clear call");
	fixture_require(fixture_bytes_zero(passphrase, passphrase_length), test,
	    "argv secret retained");
	fixture_require(!fixture.output_overflow && fixture.output_length == 0U &&
	    fixture.error_length < FIXTURE_OUTPUT_MAX, test, "bounded output");
	fixture_require(strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL, test,
	    "secret diagnostic");
}

static void
fixture_basic_bounds(void)
{
	char empty_interface[] = "";
	char long_interface[IFNAMSIZ + 1U];
	char interface[] = "wlan0";
	char empty_ssid[] = "";
	char long_ssid[WLAN_SSID_MAX + 2U];
	char ssid[] = FIXTURE_SSID;
	char empty_passphrase[] = "";
	char long_passphrase[WLAN_PASSPHRASE_MAX + 2U];
	char passphrase1[] = FIXTURE_PASSPHRASE;
	char passphrase2[] = FIXTURE_PASSPHRASE;
	char passphrase3[] = FIXTURE_PASSPHRASE;
	char passphrase4[] = FIXTURE_PASSPHRASE;

	memset(long_interface, 'i', IFNAMSIZ);
	long_interface[IFNAMSIZ] = '\0';
	memset(long_ssid, 's', WLAN_SSID_MAX + 1U);
	long_ssid[WLAN_SSID_MAX + 1U] = '\0';
	memset(long_passphrase, 'p', WLAN_PASSPHRASE_MAX + 1U);
	long_passphrase[WLAN_PASSPHRASE_MAX + 1U] = '\0';

	fixture_expect_rejected("empty interface", empty_interface, ssid,
	    passphrase1, sizeof(passphrase1) - 1U);
	fixture_expect_rejected("long interface", long_interface, ssid,
	    passphrase2, sizeof(passphrase2) - 1U);
	fixture_expect_rejected("empty SSID", interface, empty_ssid,
	    passphrase3, sizeof(passphrase3) - 1U);
	fixture_expect_rejected("long SSID", interface, long_ssid,
	    passphrase4, sizeof(passphrase4) - 1U);
	fixture_expect_rejected("empty passphrase", interface, ssid,
	    empty_passphrase, 0U);
	fixture_expect_rejected("long passphrase", interface, ssid,
	    long_passphrase, WLAN_PASSPHRASE_MAX + 1U);
}

int
main(void)
{
	fixture_normal_sequence();
	fixture_basic_bounds();
	puts("NET-T24 minimum wifi command fixture: PASS");
	return 0;
}
