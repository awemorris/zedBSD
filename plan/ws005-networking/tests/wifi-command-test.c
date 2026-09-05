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
#define FIXTURE_OUTPUT_MAX 32769U
#define FIXTURE_TRACE_MAX 512U
#define FIXTURE_SSID "fixture-ap"
#define FIXTURE_PASSPHRASE "fixture-passphrase"

struct fixture_trace {
	unsigned long command;
	uint32_t scan_action;
};

static struct {
	unsigned socket_calls;
	unsigned close_calls;
	unsigned secret_close_calls;
	unsigned secret_read_calls;
	unsigned ioctl_calls;
	unsigned sleep_calls;
	unsigned clock_calls;
	unsigned flush_calls;
	unsigned argv_clear_calls;
	unsigned request_clear_calls;
	unsigned connect_status_polls;
	unsigned connect_attempt_status_polls;
	unsigned connect_attempts;
	unsigned connect_admissions;
	unsigned scan_status_polls;
	uint32_t scan_result_count;
	unsigned scan_starts;
	unsigned scan_stops;
	unsigned connect_seen;
	unsigned connect_redacted;
	unsigned disconnect_seen;
	unsigned auto_scan;
	unsigned connect_busy_once;
	unsigned connect_fatal;
	unsigned connect_immediate_fatal;
	unsigned connect_generation_replaced;
	unsigned interrupt_on_sleep;
	unsigned connect_slow_timeout;
	unsigned scan_never_completes;
	unsigned scan_generation_replaced;
	unsigned connect_retry_once;
	unsigned stdout_tty;
	unsigned interface_up;
	unsigned output_overflow;
	unsigned secret_eintr_once;
	unsigned secret_eintr_seen;
	uint64_t ticks;
	uint64_t connect_deadline;
	uint64_t active_connect_generation;
	void *connect_request;
	const uint8_t *secret_input;
	const uint8_t *expected_passphrase;
	size_t secret_input_length;
	size_t secret_input_offset;
	size_t secret_read_chunk;
	size_t expected_passphrase_length;
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
static ssize_t fixture_read(int, void *, size_t);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static long fixture_sysconf(int);
static int fixture_nanosleep(const struct timespec *, struct timespec *);
static int fixture_printf(const char *, ...);
static int fixture_fprintf(FILE *, const char *, ...);
static int fixture_fputs(const char *, FILE *);
static int fixture_putchar(int);
static int fixture_fflush(FILE *);
static int fixture_ferror(FILE *);
static int fixture_isatty(int);
static void fixture_expect_machine_terminal(const char *, int);
static void fixture_machine_connect(void);
static void fixture_machine_secret_failures(void);
static void fixture_machine_simple_operations(void);

#define socket fixture_socket
#define ioctl fixture_ioctl
#define close fixture_close
#define read fixture_read
#define clock_gettime fixture_clock_gettime
#define sysconf fixture_sysconf
#define nanosleep fixture_nanosleep
#define printf fixture_printf
#define fprintf fixture_fprintf
#define fputs fixture_fputs
#define putchar fixture_putchar
#define fflush fixture_fflush
#define ferror fixture_ferror
#define isatty fixture_isatty
#define WIFI_EXPLICIT_CLEAR(buffer, size) \
	fixture_explicit_clear((buffer), (size))
#define main wifi_program_main
#include "userland/base/wifi/main.c"
#undef main
#undef WIFI_EXPLICIT_CLEAR
#undef isatty
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
#undef read
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
	fixture.expected_passphrase =
	    (const uint8_t *)FIXTURE_PASSPHRASE;
	fixture.expected_passphrase_length = sizeof(FIXTURE_PASSPHRASE) - 1U;
	fixture.scan_result_count = 1U;
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
	fixture.flush_calls++;
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
	if (descriptor == WIFI_SECRET_DESCRIPTOR) {
		fixture.secret_close_calls++;
		return 0;
	}
	fixture_require(descriptor == FIXTURE_DESCRIPTOR, "close",
	    "descriptor");
	fixture.close_calls++;
	return 0;
}

static ssize_t
fixture_read(int descriptor, void *buffer, size_t capacity)
{
	size_t available;
	size_t count;

	fixture_require(descriptor == WIFI_SECRET_DESCRIPTOR, "secret read",
	    "descriptor");
	fixture_require(buffer != NULL && capacity != 0U, "secret read",
	    "buffer");
	fixture.secret_read_calls++;
	if (fixture.secret_eintr_once && !fixture.secret_eintr_seen) {
		fixture.secret_eintr_seen = 1U;
		fixture_errno = EINTR;
		return -1;
	}
	available = fixture.secret_input_length - fixture.secret_input_offset;
	if (available == 0U)
		return 0;
	count = available < capacity ? available : capacity;
	if (fixture.secret_read_chunk != 0U && count > fixture.secret_read_chunk)
		count = fixture.secret_read_chunk;
	memcpy(buffer, fixture.secret_input + fixture.secret_input_offset, count);
	fixture.secret_input_offset += count;
	return (ssize_t)count;
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
fixture_isatty(int descriptor)
{
	fixture_require(descriptor == STDOUT_FILENO, "output",
	    "terminal query descriptor");
	return fixture.stdout_tty != 0U;
}

static int
fixture_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
	uint64_t nanoseconds;

	fixture_require(delay != NULL && remaining == NULL, "wait",
	    "sleep arguments");
	fixture_require(delay->tv_sec == 0 && delay->tv_nsec > 0 &&
	    delay->tv_nsec <= 100000000L, "wait", "sleep bound");
	if (fixture.interrupt_on_sleep && fixture.sleep_calls == 0U) {
		fixture.sleep_calls++;
		fixture_errno = EINTR;
		fixture_require(raise(SIGINT) == 0, "wait", "raise SIGINT");
		return -1;
	}
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
	status->operation_generation = fixture.active_connect_generation;
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
	if (command == SIOCGIFFLAGS || command == SIOCSIFFLAGS) {
		struct ifreq *request = argument;

		fixture_require(strcmp(request->ifr_name, "wlan0") == 0,
		    "interface", "interface name");
		fixture_record(command, 0U);
		if (command == SIOCGIFFLAGS)
			request->ifr_flags = fixture.interface_up ? IFF_UP : 0;
		else
			fixture.interface_up =
			    (request->ifr_flags & IFF_UP) != 0U;
		return 0;
	}
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
		fixture_record(command, 0U);
		if (fixture.auto_scan && fixture.scan_starts == 0U) {
			request->state = WLAN_SCAN_IDLE;
			return 0;
		}
		request->generation = FIXTURE_SCAN_GENERATION;
		request->scan_generation = FIXTURE_SCAN_GENERATION;
		request->cache_sequence = 1U;
		if (fixture.scan_generation_replaced &&
		    fixture.scan_status_polls == 1U) {
			request->scan_generation = FIXTURE_SCAN_GENERATION + 1U;
			request->state = WLAN_SCAN_RUNNING;
			fixture.scan_status_polls++;
			return 0;
		}
		if (fixture.auto_scan &&
		    (fixture.scan_never_completes ||
		    fixture.scan_status_polls++ == 0U)) {
			if (fixture.scan_never_completes)
				fixture.scan_status_polls++;
			request->state = WLAN_SCAN_RUNNING;
		} else {
			request->state = WLAN_SCAN_COMPLETE;
			request->result_count = fixture.scan_result_count;
		}
		return 0;
	}
	if (command == SIOCGWLANBSS) {
		struct wlan_bss_request *request = argument;

		fixture_require(request->generation == FIXTURE_SCAN_GENERATION &&
		    request->index < fixture.scan_result_count &&
		    request->reserved0 == 0U &&
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

		if (!wifi_quiet) {
			fixture_require(fixture.flush_calls != 0U &&
			    ((wifi_machine && strstr(fixture.output,
			    "WIFI1 connect state=starting generation=0 error=0") != NULL) ||
			    (!wifi_machine && strstr(fixture.output,
			    "Selecting a supported BSS and preparing the radio...") != NULL)),
			    "connect", "progress not flushed before admission ioctl");
		}
		fixture_require(request->ssid_length == sizeof(FIXTURE_SSID) - 1U &&
		    memcmp(request->ssid, FIXTURE_SSID,
			sizeof(FIXTURE_SSID) - 1U) == 0, "connect", "SSID input");
		fixture_require(request->passphrase_length ==
		    fixture.expected_passphrase_length &&
		    memcmp(request->passphrase, fixture.expected_passphrase,
			fixture.expected_passphrase_length) == 0, "connect",
		    "passphrase input");
		fixture_require(request->generation == 0U && request->state == 0U &&
		    request->terminal_error == 0 &&
		    fixture_bytes_zero(request->reserved,
			sizeof(request->reserved)), "connect", "request input");
		fixture_record(command, 0U);
		fixture.connect_attempts++;
		fixture.connect_request = request;
		if (fixture.connect_immediate_fatal) {
			request->generation = FIXTURE_CONNECT_GENERATION;
			fixture_errno = EACCES;
			return -1;
		}
		if (fixture.connect_busy_once && fixture.connect_attempts == 1U) {
			fixture_errno = EBUSY;
			return -1;
		}
		if (fixture.auto_scan && fixture.connect_attempts == 1U) {
			fixture_errno = ENOENT;
			return -1;
		}
		fixture.connect_seen = 1U;
		fixture.connect_admissions++;
		fixture.connect_attempt_status_polls = 0U;
		fixture.active_connect_generation = FIXTURE_CONNECT_GENERATION +
		    fixture.connect_admissions - 1U;
		request->generation = fixture.active_connect_generation;
		request->state = WLAN_STATE_AUTHENTICATING;
		fixture.connect_deadline = fixture.ticks + 5U * FIXTURE_CLOCK_HZ;
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
		if (fixture.connect_generation_replaced) {
			fixture_fill_status(request, WLAN_STATE_AUTHENTICATING);
			request->operation_generation =
			    fixture.active_connect_generation + 1U;
			fixture.connect_status_polls++;
			return 0;
		}
		fixture_require(fixture.request_clear_calls ==
		    fixture.connect_attempts, "connect",
		    "connect request was not cleared before status polling");
		if (fixture.connect_seen && !fixture.connect_slow_timeout &&
		    !fixture.interrupt_on_sleep) {
			fixture_require(fixture.connect_attempt_status_polls < 4U,
			    "connect", "excess status polls");
		}
		if (fixture.connect_slow_timeout) {
			if (fixture.connect_admissions == 1U &&
			    fixture.connect_attempt_status_polls >= 250U) {
				fixture_fill_status(request, WLAN_STATE_FAILED);
				request->terminal_error = ETIMEDOUT;
			} else {
				fixture_fill_status(request, WLAN_STATE_AUTHENTICATING);
			}
			request->deadline_ticks = 0U;
			fixture.connect_status_polls++;
			fixture.connect_attempt_status_polls++;
			return 0;
		}
		if (fixture.connect_fatal) {
			fixture_fill_status(request, WLAN_STATE_FAILED);
			request->terminal_error = EACCES;
			fixture.connect_status_polls++;
			fixture.connect_attempt_status_polls++;
			return 0;
		}
		if (fixture.connect_retry_once &&
		    fixture.connect_admissions == 1U) {
			/* The first asynchronous generation becomes terminal after one
			 * observed protocol state.  Userspace must initiate generation 2. */
			if (fixture.connect_attempt_status_polls == 0U) {
				fixture_fill_status(request, WLAN_STATE_AUTHENTICATING);
			} else {
				fixture_fill_status(request, WLAN_STATE_FAILED);
				request->terminal_error = ETIMEDOUT;
				request->retry_count = 1U;
			}
			fixture.connect_status_polls++;
			fixture.connect_attempt_status_polls++;
			return 0;
		}
		state = fixture.connect_attempt_status_polls == 0U ?
		    WLAN_STATE_AUTHENTICATING :
		    (fixture.connect_attempt_status_polls == 1U ?
		    WLAN_STATE_ASSOCIATING :
		    (fixture.connect_attempt_status_polls == 2U ? WLAN_STATE_FOUR_WAY :
		    WLAN_STATE_CONNECTED));
		fixture.connect_status_polls++;
		fixture.connect_attempt_status_polls++;
		fixture_fill_status(request, state);
		return 0;
	}
	if (command == SIOCSWLANDISCONNECT) {
		struct wlan_disconnect_request *request = argument;

		if (fixture.connect_seen && !fixture.connect_slow_timeout &&
		    !fixture.interrupt_on_sleep) {
			fixture_require(fixture.connect_status_polls == 4U,
			    "disconnect", "disconnect before authorized status");
		}
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
fixture_expect_machine_terminal(const char *test, int error)
{
	char expected[64];
	const char *cursor;
	const char *newline;
	const char *terminal;

	if (error == 0)
		(void)snprintf(expected, sizeof(expected),
		    "WIFI1 terminal ok 0\n");
	else
		(void)snprintf(expected, sizeof(expected),
		    "WIFI1 terminal error %d\n", error);
	terminal = strstr(fixture.output, "WIFI1 terminal ");
	fixture_require(terminal != NULL && strcmp(terminal, expected) == 0,
	    test, "missing or non-final terminal record");
	cursor = fixture.output;
	while (*cursor != '\0') {
		fixture_require(strncmp(cursor, "WIFI1 ", 6U) == 0, test,
		    "machine line prefix");
		newline = strchr(cursor, '\n');
		fixture_require(newline != NULL, test, "partial machine record");
		cursor = newline + 1;
	}
	fixture_require(fixture.error_length == 0U && !fixture.output_overflow,
	    test, "machine diagnostic or output overflow");
}

static void
fixture_machine_connect(void)
{
	static const uint8_t secret[] = FIXTURE_PASSPHRASE;
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char *arguments[] = { "wifi", "--machine", "--passphrase-fd=4",
	    interface, "connect", ssid, NULL };

	fixture_reset();
	fixture.secret_input = secret;
	fixture.secret_input_length = sizeof(secret) - 1U;
	fixture.secret_read_chunk = 3U;
	fixture.secret_eintr_once = 1U;
	fixture_require(fixture_invoke(6, arguments, NULL, 0U) == 0,
	    "machine connect", "exit status");
	fixture_expect_machine_terminal("machine connect", 0);
	fixture_require(fixture.secret_read_calls > 2U &&
	    fixture.secret_close_calls == 1U && fixture.connect_admissions == 1U &&
	    fixture.connect_redacted == 1U && fixture.argv_secret == NULL,
	    "machine connect", "secret descriptor or admission contract");
	fixture_require(strstr(fixture.output,
	    "WIFI1 connect state=starting generation=0 error=0\n") != NULL &&
	    strstr(fixture.output, "WIFI1 connect state=6 generation=29 "
	    "authorized=1 error=0\n") != NULL &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL,
	    "machine connect", "connect records or secret output");
}

static void
fixture_machine_secret_failures(void)
{
	static const uint8_t short_secret[] = "1234567";
	static const uint8_t invalid_secret[] = {
		'a', 'b', 'c', 'd', 'e', 'f', 'g', '\n'
	};
	uint8_t maximum_secret[WLAN_PASSPHRASE_MAX];
	uint8_t overflow_secret[WLAN_PASSPHRASE_STORAGE];
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char *arguments[] = { "wifi", "--machine", "--passphrase-fd=4",
	    interface, "connect", ssid, NULL };
	char *missing_fd[] = { "wifi", "--machine", interface, "connect",
	    ssid, NULL };
	char *unexpected_fd[] = { "wifi", "--machine", "--passphrase-fd=4",
	    interface, "status", NULL };

	memset(maximum_secret, 'm', sizeof(maximum_secret));
	memset(overflow_secret, 'o', sizeof(overflow_secret));

	fixture_reset();
	fixture.secret_input = short_secret;
	fixture.secret_input_length = sizeof(short_secret) - 1U;
	fixture_require(fixture_invoke(6, arguments, NULL, 0U) == 1,
	    "machine short secret", "exit status");
	fixture_expect_machine_terminal("machine short secret", EINVAL);
	fixture_require(fixture.socket_calls == 0U &&
	    fixture.secret_close_calls == 1U,
	    "machine short secret", "operation escaped validation");

	fixture_reset();
	fixture.secret_input = invalid_secret;
	fixture.secret_input_length = sizeof(invalid_secret);
	fixture_require(fixture_invoke(6, arguments, NULL, 0U) == 1,
	    "machine invalid secret", "exit status");
	fixture_expect_machine_terminal("machine invalid secret", EINVAL);
	fixture_require(fixture.socket_calls == 0U,
	    "machine invalid secret", "operation escaped validation");

	fixture_reset();
	fixture.secret_input = overflow_secret;
	fixture.secret_input_length = sizeof(overflow_secret);
	fixture.secret_read_chunk = WLAN_PASSPHRASE_MAX;
	fixture_require(fixture_invoke(6, arguments, NULL, 0U) == 1,
	    "machine overflow secret", "exit status");
	fixture_expect_machine_terminal("machine overflow secret", E2BIG);
	fixture_require(fixture.secret_read_calls == 2U &&
	    fixture.socket_calls == 0U,
	    "machine overflow secret", "64th-byte overflow probe");

	fixture_reset();
	fixture.secret_input = maximum_secret;
	fixture.secret_input_length = sizeof(maximum_secret);
	fixture.expected_passphrase = maximum_secret;
	fixture.expected_passphrase_length = sizeof(maximum_secret);
	fixture_require(fixture_invoke(6, arguments, NULL, 0U) == 0,
	    "machine maximum secret", "exit status");
	fixture_expect_machine_terminal("machine maximum secret", 0);
	fixture_require(fixture.secret_read_calls == 2U &&
	    strstr(fixture.output, "mmmmmmmm") == NULL,
	    "machine maximum secret", "EOF or secret output");

	fixture_reset();
	fixture_require(fixture_invoke(5, missing_fd, NULL, 0U) == 1,
	    "machine missing fd", "exit status");
	fixture_expect_machine_terminal("machine missing fd", EINVAL);
	fixture_require(fixture.socket_calls == 0U &&
	    fixture.secret_read_calls == 0U,
	    "machine missing fd", "operation escaped grammar");

	fixture_reset();
	fixture_require(fixture_invoke(5, unexpected_fd, NULL, 0U) == 1,
	    "machine unexpected fd", "exit status");
	fixture_expect_machine_terminal("machine unexpected fd", EINVAL);
	fixture_require(fixture.socket_calls == 0U &&
	    fixture.secret_read_calls == 0U,
	    "machine unexpected fd", "operation escaped grammar");
}

static void
fixture_machine_simple_operations(void)
{
	char interface[] = "wlan0";
	char *up[] = { "wifi", "--machine", interface, "up", NULL };
	char *down[] = { "wifi", "--machine", interface, "down", NULL };
	char *search_start[] = { "wifi", "--machine", interface, "search",
	    "start", NULL };
	char *search_stop[] = { "wifi", "--machine", interface, "search",
	    "stop", NULL };
	char *list[] = { "wifi", "--machine", interface, "list", NULL };
	char *status[] = { "wifi", "--machine", interface, "status", NULL };
	char *disconnect[] = { "wifi", "--machine", interface, "disconnect",
	    NULL };
	const char *cursor;
	unsigned records;

	fixture_reset();
	fixture_require(fixture_invoke(4, up, NULL, 0U) == 0,
	    "machine up", "exit status");
	fixture_expect_machine_terminal("machine up", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 interface administrative=1\n") != NULL,
	    "machine up", "interface record");

	fixture_reset();
	fixture.interface_up = 1U;
	fixture_require(fixture_invoke(4, down, NULL, 0U) == 0,
	    "machine down", "exit status");
	fixture_expect_machine_terminal("machine down", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 interface administrative=0\n") != NULL,
	    "machine down", "interface record");

	fixture_reset();
	fixture_require(fixture_invoke(5, search_start, NULL, 0U) == 0,
	    "machine search start", "exit status");
	fixture_expect_machine_terminal("machine search start", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 scan action=1 state=1 generation=17 error=0\n") != NULL,
	    "machine search start", "scan record");

	fixture_reset();
	fixture_require(fixture_invoke(5, search_stop, NULL, 0U) == 0,
	    "machine search stop", "exit status");
	fixture_expect_machine_terminal("machine search stop", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 scan action=2 state=3 generation=17 error=0\n") != NULL,
	    "machine search stop", "scan record");

	fixture_reset();
	fixture_require(fixture_invoke(4, list, NULL, 0U) == 0,
	    "machine list", "exit status");
	fixture_expect_machine_terminal("machine list", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 scan state=2 generation=17 results=1 available=1 "
	    "truncated=0 error=0\n") != NULL &&
	    strstr(fixture.output,
	    "WIFI1 bss index=0 ssid=666978747572652d6170 "
	    "bssid=020000000042 channel=6 frequency=2437 rssi=-35 age=25 "
	    "security=00000035 flags=00000011\n") != NULL,
	    "machine list", "scan or canonical BSS record");

	fixture_reset();
	fixture.scan_result_count = WLAN_BSS_MAX;
	fixture_require(fixture_invoke(4, list, NULL, 0U) == 0,
	    "machine list bound", "exit status");
	fixture_expect_machine_terminal("machine list bound", 0);
	records = 0U;
	for (cursor = fixture.output; *cursor != '\0'; cursor++) {
		if (*cursor == '\n')
			records++;
	}
	fixture_require(records == 64U && fixture.ioctl_calls == 63U &&
	    strstr(fixture.output,
	    "results=62 available=64 truncated=1 error=0") != NULL &&
	    strstr(fixture.output, "WIFI1 bss index=61 ") != NULL &&
	    strstr(fixture.output, "WIFI1 bss index=62 ") == NULL,
	    "machine list bound", "record ceiling or truncation");

	fixture_reset();
	fixture_require(fixture_invoke(4, status, NULL, 0U) == 0,
	    "machine status", "exit status");
	fixture_expect_machine_terminal("machine status", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 status state=1 scan=2 operation-generation=0 "
	    "scan-generation=17 snapshot-generation=17") != NULL,
	    "machine status", "status record");

	fixture_reset();
	fixture_require(fixture_invoke(4, disconnect, NULL, 0U) == 0,
	    "machine disconnect", "exit status");
	fixture_expect_machine_terminal("machine disconnect", 0);
	fixture_require(strstr(fixture.output,
	    "WIFI1 connect state=1 generation=29 error=0\n") != NULL,
	    "machine disconnect", "disconnect record");
}

static void
fixture_expect_output_order(void)
{
	static const char *const markers[] = {
		"search state=scanning",
		"scan state=complete",
		"ssid=\"" FIXTURE_SSID "\"",
		"state=idle scan=complete",
		"Connected: controlled port authorized",
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
fixture_auto_scan_connect(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSCAN,
		SIOCSWLANSCAN,
		SIOCGWLANSCAN,
		SIOCGWLANSCAN,
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS
	};
	static const char *const markers[] = {
		"Scanning... (30 seconds remaining)",
		"Selecting a supported BSS...",
		"Preparing the radio and starting 802.11 authentication...",
		"Associating with the access point...",
		"Completing the WPA2 four-way handshake...",
		"Connected: controlled port authorized"
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	const char *cursor;
	unsigned index;

	fixture_reset();
	fixture.auto_scan = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 0, "auto scan", "connect status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "auto scan", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "auto scan", "ioctl sequence");
	fixture_require(fixture.connect_attempts == 2U &&
	    fixture.request_clear_calls == 2U && fixture.scan_starts == 1U &&
	    fixture.connect_status_polls == 4U, "auto scan",
	    "operation sequence");
	fixture_require(fixture.sleep_calls == 4U && fixture.clock_calls == 6U,
	    "auto scan", "single bounded timeline");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "auto scan", "argv secret retained");
	fixture_require(fixture.error_length == 0U &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL,
	    "auto scan", "clean nonsecret output");
	cursor = fixture.output;
	for (index = 0U; index < sizeof(markers) / sizeof(markers[0]); index++) {
		cursor = strstr(cursor, markers[index]);
		fixture_require(cursor != NULL, "auto scan",
		    "missing progress marker");
		cursor += strlen(markers[index]);
	}
}

static void
fixture_quiet_interface_control(void)
{
	char interface[] = "wlan0";
	char *up[] = { "wifi", "--quiet", interface, "up", NULL };
	char *down[] = { "wifi", "--quiet", interface, "down", NULL };

	fixture_reset();
	fixture_require(fixture_invoke(4, up, NULL, 0U) == 0,
	    "quiet interface", "up status");
	fixture_require(fixture.interface_up == 1U,
	    "quiet interface", "up state");
	fixture_require(fixture_invoke(4, down, NULL, 0U) == 0,
	    "quiet interface", "down status");
	fixture_require(fixture_invoke(4, down, NULL, 0U) == 0,
	    "quiet interface", "idempotent down status");
	fixture_require(fixture.interface_up == 0U && fixture.ioctl_calls == 6U,
	    "quiet interface", "down state or ioctl count");
	fixture_require(fixture.output_length == 0U &&
	    fixture.error_length == 0U, "quiet interface", "unexpected output");
}

static void
fixture_transient_connect_busy(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSCAN,
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	unsigned index;

	fixture_reset();
	fixture.connect_busy_once = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 0, "connect busy", "connect status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "connect busy", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "connect busy", "ioctl sequence");
	fixture_require(fixture.connect_attempts == 2U &&
	    fixture.scan_starts == 0U && fixture.connect_seen == 1U,
	    "connect busy", "busy caused a replacement scan");
	fixture_require(fixture.sleep_calls == 4U && fixture.clock_calls == 5U,
	    "connect busy", "bounded retry timeline");
	fixture_require(strstr(fixture.output,
	    "Waiting for the radio to become ready...") != NULL &&
	    strstr(fixture.output, "Selecting a supported BSS...") == NULL,
	    "connect busy", "busy progress or duplicate BSS selection");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "connect busy", "argv secret retained");
}

static void
fixture_auto_scan_deadline(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };

	fixture_reset();
	fixture.auto_scan = 1U;
	fixture.scan_never_completes = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "scan deadline", "connect status");
	fixture_require(fixture.ticks == 100U +
	    WIFI_CONNECT_SECONDS * FIXTURE_CLOCK_HZ, "scan deadline",
	    "deadline was not command-wide 30 seconds");
	fixture_require(fixture.connect_attempts == 1U &&
	    fixture.scan_starts == 1U && fixture.scan_stops == 1U &&
	    fixture.sleep_calls == WIFI_FALLBACK_POLLS,
	    "scan deadline", "bounded scan cancellation");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "scan deadline", "argv secret retained");
	fixture_require(strstr(fixture.error, "(42)") != NULL &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "scan deadline", "terminal diagnostic");
}

static void
fixture_connect_terminal_retry(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSCAN,
		SIOCSWLANSCAN,
		SIOCGWLANSCAN,
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	unsigned index;

	fixture_reset();
	fixture.connect_retry_once = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 0, "connect retry", "exit status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "connect retry", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "connect retry", "ioctl sequence");
	fixture_require(fixture.connect_attempts == 2U &&
	    fixture.connect_admissions == 2U &&
	    fixture.connect_status_polls == 6U && fixture.scan_starts == 1U,
	    "connect retry", "operation sequence");
	fixture_require(fixture.active_connect_generation ==
	    FIXTURE_CONNECT_GENERATION + 1U, "connect retry",
	    "generation did not advance");
	fixture_require(fixture.sleep_calls == 5U &&
	    fixture.clock_calls == 7U, "connect retry",
	    "single userspace deadline timeline");
	fixture_require(strstr(fixture.output,
	    "Connection attempt failed; rescanning and retrying...") != NULL &&
	    strstr(fixture.output, "Connected: controlled port authorized") !=
	    NULL && fixture.error_length == 0U, "connect retry",
	    "retry progress");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U) &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "connect retry", "secret clearing");
}

static void
fixture_connect_fatal_error(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	unsigned index;

	fixture_reset();
	fixture.connect_fatal = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "connect fatal", "exit status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "connect fatal", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "connect fatal", "ioctl sequence");
	fixture_require(fixture.connect_attempts == 1U &&
	    fixture.connect_admissions == 1U &&
	    fixture.connect_status_polls == 2U &&
	    fixture.scan_starts == 0U && fixture.disconnect_seen == 0U,
	    "connect fatal", "fatal error was retried or cancelled");
	(void)snprintf(marker, sizeof(marker), "(%d)", EACCES);
	fixture_require(strstr(fixture.output, "state=failed") != NULL &&
	    strstr(fixture.output, "error=") != NULL &&
	    strstr(fixture.error, marker) != NULL &&
	    strstr(fixture.output, "rescanning and retrying") == NULL,
	    "connect fatal", "fatal diagnostic");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U) &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "connect fatal", "secret clearing");
}

static void
fixture_connect_failed_admission(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };

	fixture_reset();
	fixture.connect_immediate_fatal = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "failed admission", "exit status");
	fixture_require(fixture.connect_attempts == 1U &&
	    fixture.connect_admissions == 0U && fixture.disconnect_seen == 0U &&
	    fixture.ioctl_calls == 1U, "failed admission",
	    "failed ioctl acquired cancellation ownership");
	(void)snprintf(marker, sizeof(marker), "(%d)", EACCES);
	fixture_require(strstr(fixture.error, marker) != NULL &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "failed admission", "fatal diagnostic or redaction");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "failed admission", "secret clearing");
}

static void
fixture_quiet_connect_retry(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", "--quiet", interface, "connect", ssid,
	    passphrase, NULL };

	fixture_reset();
	fixture.connect_retry_once = 1U;
	fixture_require(fixture_invoke(6, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 0, "quiet retry", "exit status");
	fixture_require(fixture.connect_admissions == 2U &&
	    fixture.scan_starts == 1U && fixture.output_length == 0U &&
	    fixture.error_length == 0U, "quiet retry",
	    "retry or output suppression");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "quiet retry", "secret clearing");
}

static void
fixture_retry_deadline_is_not_reset(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	const char *first_retry;

	fixture_reset();
	fixture.connect_slow_timeout = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "retry deadline", "exit status");
	fixture_require(fixture.ticks == 100U +
	    WIFI_CONNECT_SECONDS * FIXTURE_CLOCK_HZ, "retry deadline",
	    "deadline reset across generations");
	fixture_require(fixture.connect_admissions == 2U &&
	    fixture.scan_starts == 1U && fixture.disconnect_seen == 1U,
	    "retry deadline", "attempt or cancellation ownership");
	first_retry = strstr(fixture.output,
	    "Connection attempt failed; rescanning and retrying...");
	fixture_require(first_retry != NULL &&
	    strstr(first_retry + 1, "Connection attempt failed; rescanning") ==
	    NULL, "retry deadline", "unbounded retry output");
	(void)snprintf(marker, sizeof(marker), "(%d)", ETIMEDOUT);
	fixture_require(strstr(fixture.error, marker) != NULL &&
	    fixture.output_overflow == 0U &&
	    fixture.output_length < FIXTURE_OUTPUT_MAX &&
	    fixture_bytes_zero(passphrase, sizeof(passphrase) - 1U) &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "retry deadline", "bounded diagnostic or secret clearing");
}

static void
fixture_scan_generation_replacement(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSCAN,
		SIOCSWLANSCAN,
		SIOCGWLANSCAN,
		SIOCGWLANSCAN,
		SIOCGWLANSTATUS
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	unsigned index;

	fixture_reset();
	fixture.auto_scan = 1U;
	fixture.scan_generation_replaced = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "scan replacement", "exit status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "scan replacement", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "scan replacement", "ioctl sequence");
	fixture_require(fixture.connect_attempts == 1U &&
	    fixture.connect_admissions == 0U && fixture.scan_starts == 1U &&
	    fixture.scan_stops == 0U, "scan replacement",
	    "stale generation was consumed or cancelled");
	(void)snprintf(marker, sizeof(marker), "(%d)", ESTALE);
	fixture_require(strstr(fixture.error, marker) != NULL &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "scan replacement", "stale diagnostic or secret output");
}

static void
fixture_connect_generation_replacement(void)
{
	static const unsigned long expected[] = {
		SIOCSWLANCONNECT,
		SIOCGWLANSTATUS,
		SIOCGWLANSTATUS
	};
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	unsigned index;

	fixture_reset();
	fixture.connect_generation_replaced = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "connect replacement",
	    "exit status");
	fixture_require(fixture.ioctl_calls == sizeof(expected) /
	    sizeof(expected[0]), "connect replacement", "ioctl count");
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++)
		fixture_require(fixture.trace[index].command == expected[index],
		    "connect replacement", "ioctl sequence");
	fixture_require(fixture.connect_admissions == 1U &&
	    fixture.disconnect_seen == 0U && fixture.scan_starts == 0U,
	    "connect replacement", "replacement generation was cancelled");
	(void)snprintf(marker, sizeof(marker), "(%d)", ESTALE);
	fixture_require(strstr(fixture.error, marker) != NULL &&
	    fixture_bytes_zero(passphrase, sizeof(passphrase) - 1U) &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "connect replacement", "stale diagnostic or secret output");
}

static void
fixture_connect_signal_cancellation(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char marker[32];
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };

	fixture_reset();
	fixture.interrupt_on_sleep = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "connect signal", "exit status");
	fixture_require(fixture.connect_admissions == 1U &&
	    fixture.connect_status_polls == 2U && fixture.sleep_calls == 1U &&
	    fixture.disconnect_seen == 1U, "connect signal",
	    "owned generation was not cancelled");
	(void)snprintf(marker, sizeof(marker), "(%d)", EINTR);
	fixture_require(strstr(fixture.error, marker) != NULL &&
	    strstr(fixture.output, FIXTURE_PASSPHRASE) == NULL &&
	    strstr(fixture.error, FIXTURE_PASSPHRASE) == NULL,
	    "connect signal", "signal result or diagnostic redaction");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "connect signal", "secret clearing");
}

static void
fixture_terminal_scan_overwrite(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = FIXTURE_PASSPHRASE;
	char piped_passphrase[] = FIXTURE_PASSPHRASE;
	char *arguments[] = { "wifi", interface, "connect", ssid,
	    passphrase, NULL };
	char *piped_arguments[] = { "wifi", interface, "connect", ssid,
	    piped_passphrase, NULL };

	/* An interactive terminal refreshes the countdown in place and the
	 * following state line still starts on its own row. */
	fixture_reset();
	fixture.auto_scan = 1U;
	fixture.stdout_tty = 1U;
	fixture_require(fixture_invoke(5, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 0, "terminal scan", "connect status");
	fixture_require(strstr(fixture.output,
	    "\rScanning... (30 seconds remaining)\n"
	    "Selecting a supported BSS...\n") != NULL, "terminal scan",
	    "countdown overwrite or line termination");
	fixture_require(strstr(fixture.output,
	    "Scanning... (30 seconds remaining)\n\r") == NULL &&
	    strstr(fixture.output, "Connected: controlled port authorized") !=
	    NULL, "terminal scan", "terminal output shape");

	/* A pipe keeps one complete line per refresh. */
	fixture_reset();
	fixture.auto_scan = 1U;
	fixture_require(fixture_invoke(5, piped_arguments, piped_passphrase,
	    sizeof(piped_passphrase) - 1U) == 0, "piped scan",
	    "connect status");
	fixture_require(strstr(fixture.output, "\r") == NULL &&
	    strstr(fixture.output,
	    "Scanning... (30 seconds remaining)\n"
	    "Selecting a supported BSS...\n") != NULL, "piped scan",
	    "line-per-refresh output");
}

static void
fixture_quiet_validation_failure(void)
{
	char interface[] = "wlan0";
	char ssid[] = FIXTURE_SSID;
	char passphrase[] = "short";
	char *arguments[] = { "wifi", "--quiet", interface, "connect", ssid,
	    passphrase, NULL };

	fixture_reset();
	fixture_require(fixture_invoke(6, arguments, passphrase,
	    sizeof(passphrase) - 1U) == 1, "quiet validation", "status");
	fixture_require(fixture.output_length == 0U &&
	    fixture.error_length == 0U && fixture.socket_calls == 0U,
	    "quiet validation", "output or operation escaped");
	fixture_require(fixture_bytes_zero(passphrase,
	    sizeof(passphrase) - 1U), "quiet validation", "secret retained");
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
	fixture_machine_connect();
	fixture_machine_secret_failures();
	fixture_machine_simple_operations();
	fixture_normal_sequence();
	fixture_auto_scan_connect();
	fixture_transient_connect_busy();
	fixture_quiet_interface_control();
	fixture_auto_scan_deadline();
	fixture_connect_terminal_retry();
	fixture_connect_fatal_error();
	fixture_connect_failed_admission();
	fixture_quiet_connect_retry();
	fixture_retry_deadline_is_not_reset();
	fixture_scan_generation_replacement();
	fixture_connect_generation_replacement();
	fixture_connect_signal_cancellation();
	fixture_terminal_scan_overwrite();
	fixture_quiet_validation_failure();
	fixture_basic_bounds();
	puts("NET-T24 minimum wifi command fixture: PASS");
	return 0;
}
