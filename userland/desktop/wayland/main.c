/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A finite, fullscreen Wayland service using independent shared GPU resources.
 */

#include "language.h"
#include "desktop.h"
#include "kwl.h"
#include "role.h"
#include "toplevel.h"
#include "keymap.h"
#include "ime.h"
#include "data.h"
#include "userland/desktop/paths.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

/* How long a login session goes without input before it locks, unless --lock-idle says (ws035-p102). */
#define MAIN_LOCK_IDLE_MS	(10U * 60U * 1000U)
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Signal handlers only request ordinary event-loop cleanup, they own no GPU state. */
static volatile sig_atomic_t stop_requested;

static void stop_service(int signal_number);
static void log_append(int descriptor);
static int parse_options(struct kwl_server *server, int count, char **arguments);
static int unsigned_option(const char *text, uint64_t maximum, uint64_t *number);
static int listen_socket(struct kwl_server *server);
static void unlink_socket(struct kwl_server *server);
static int accept_client(struct kwl_server *server);
static void kwl_perf_report(struct kwl_server *server, uint64_t now);
static int event_loop(struct kwl_server *server);
static void service_cleanup(struct kwl_server *server);
static uint64_t startup_step(const char *step, uint64_t since);

/*
 * Runs one bounded fullscreen compositor instance and cleans up its own resources.
 */
int
main(
	int count,
	char **arguments)
{
	struct kwl_server server;
	struct kl_backend_options backend_options;
	struct kl_backend_host backend_host;
	void (*previous_handler)(int);
	int error;
	int keymap_error;
	int cleanup_failed;
	uint64_t step_start;

	/* Every descriptor starts invalid so partial initialization can use ordinary cleanup. */
	memset(&server, 0, sizeof(server));
	server.listener = -1;
	server.frame_fd = -1;
	server.auth_fd = -1;
	server.control_fd = -1;
	server.font_path = KEILAND_DATADIR "/fonts/keiland.ttf";
	server.fallback_font_path = KEILAND_DATADIR "/fonts/keiland-fallback.ttf";
	server.window_opacity = 1.0f;
	server.mouse_speed = 150;
	server.mouse_acceleration = KWL_ACCEL_STRONG;
	server.mouse_natural = 0;
	server.touchpad_speed = 100;
	server.touchpad_acceleration = KWL_ACCEL_MEDIUM;
	server.touchpad_natural = 1;
	server.repeat_rate = 25;
	server.repeat_delay_ms = 400;
	server.ime_method = 1;
	server.width = 320;
	server.height = 240;

	/* The session starts on the middle desktop, the others to its left and right (ws181-p006). */
	server.desktop = KWL_DESKTOP_START;
	server.desktop_from = (float)KWL_DESKTOP_START;
	server.desktop_to = (float)KWL_DESKTOP_START;
	strcpy(server.socket_path, "/tmp/wayland-0");
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The log is written at its end, whoever truncated it meanwhile (no hole of NUL bytes). */
	log_append(STDOUT_FILENO);
	log_append(STDERR_FILENO);

	/* The backend reports nothing to the compositor yet (WS131 p003); its callbacks come with the areas that need them. */
	memset(&backend_options, 0, sizeof(backend_options));
	memset(&backend_host, 0, sizeof(backend_host));
	backend_host.data = &server;
	backend_host.session_stop = kwl_handoff_stop;
	backend_host.session_answer = kwl_handoff_answer;
	backend_host.session_paused = kwl_backend_session_paused;
	backend_host.session_resumed = kwl_backend_session_resumed;
	backend_host.input_paused = kwl_backend_input_paused;
	backend_host.input_resumed = kwl_backend_input_resumed;
	backend_host.input_gone = kwl_backend_input_gone;
	backend_host.input_known = kwl_backend_input_known;
	backend_host.input_found = kwl_backend_input_found;
	backend_host.input_changed = kwl_backend_input_changed;
	backend_host.power_changed = kwl_backend_power_changed;
	backend_host.power_button = kwl_backend_power_button;
	backend_host.lid_changed = kwl_backend_lid_changed;

	/* Reads the command line; a mistake ends the run with the usage. */
	error = parse_options(&server, count, arguments);
	if (error != 0) {
		fprintf(stderr, "usage: wayland [--socket=/path] [--width=N] [--height=N] [--testing [--timeout=seconds] [--max-frames=N]] [--log-frames] [--keyboard-blur] [--glass] [--font=/path] [--fallback-font=/path] [--wallpaper=/path.png|.jpg] [--window-opacity=1..100] [--desktop-client=COMMAND|none] [--desktop-token=TOKEN] [--session [--control-fd=N] [--lock-idle=seconds] | --greeter --auth-fd=N]\n");
		return 2;
	}

	/* A login session locks after ten minutes without input unless told otherwise (ws035-p102). */
	if (server.session && !server.lock_idle_given)
		server.lock_idle_ms = MAIN_LOCK_IDLE_MS;
	server.lock_input_ms = kwl_milliseconds();

	/* The times without input before a sleep until the settings say others (the login screen keeps them, N9). */
	server.sleep_ac_minutes = KWL_SLEEP_AC_MINUTES;
	server.sleep_battery_minutes = KWL_SLEEP_BATTERY_MINUTES;

	/* The session's descriptor to sessiond does not go to the programs the compositor starts, and is read without waiting. */
	if (server.control_fd >= 0) {
		(void)fcntl(server.control_fd, F_SETFD, FD_CLOEXEC);
		(void)fcntl(server.control_fd, F_SETFL, fcntl(server.control_fd, F_GETFL) | O_NONBLOCK);
	}

	/* The login screen is the glass look's, and asks sessiond on a descriptor it was given. */
	if (server.greeter) {
		if (server.auth_fd < 0) {
			fprintf(stderr, "wayland: --greeter needs --auth-fd=N\n");
			return 2;
		}

		/* The glass look draws it. */
		server.glass = 1;
	}

	/*
	 * A desktop that is not the login screen holds the desktop's settings
	 * (settings.c, WS135), read before the look draws its wallpaper so
	 * that the picture is read once.
	 */
	if (!server.greeter)
		kwl_settings_open(&server);

	/* The login screen's text is in the system's language; a session's comes with its settings (WS158). */
	if (server.greeter)
		kwl_language_system(&server);

	/* A test image's screen capture listens (shot.c; nothing elsewhere, ws173-p002). */
	kwl_shot_open(&server);

	/* Catch normal termination without performing allocation or I/O inside a signal handler. */
	previous_handler = signal(SIGINT, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/* SIGTERM follows the same lease-safe shutdown path. */
	previous_handler = signal(SIGTERM, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/* So does SIGHUP, so that the session's settings are written (WS135). */
	previous_handler = signal(SIGHUP, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/*
	 * Opens the operating system's side (libkeiland-backend) before
	 * anything asks it for a resource.  The login screen's and a session's
	 * descriptors to sessiond carry the session (ws131-p006) and the power
	 * requests (ws131-p005).
	 */
	backend_options.greeter_descriptor = -1;
	backend_options.session_descriptor = -1;
	if (server.greeter)
		backend_options.greeter_descriptor = server.auth_fd;
	else
		backend_options.session_descriptor = server.control_fd;
	error = kl_backend_open(&backend_options, &backend_host, &server.backend);
	if (error != 0)
		printf("KWL BACKEND unavailable errno=%d\n", error);

	/* The power as the backend knows it now, for the bar's battery (ws132-p003). */
	kwl_power_read(&server);

	/* Takes the OS's seat resources before Vulkan opens the display. */
	if (error == 0) {
		error = kwl_os_open(&server);
		if (error != 0)
			printf("KWL OS unavailable errno=%d\n", error);
	}

	/* Reads the wallpaper while Vulkan starts, only after OS startup succeeded. */
	if (error == 0)
		kwl_glass_prefetch(&server);

	/* The start is timed from the Vulkan device on (KWL STARTUP); the compositor opens no GPU node of its own (ws103-p006). */
	step_start = kwl_milliseconds();

	/*
	 * Window mode's Vulkan device, which also gives the display's size and
	 * refresh.  Without it nothing can be shown, so the compositor does not start.
	 */
	if (error == 0) {
		error = kwl_compose_open(&server);
		if (error != 0)
			printf("KWL COMPOSE unavailable errno=%d\n", error);

		/* How long the device, the wallpaper and the glyphs took (KWL STARTUP). */
		step_start = startup_step("compose", step_start);
	}

	/* The pointer starts in the middle of the output, its arrow not shown until it moves. */
	server.pointer_x = (int32_t)(server.width / 2U);
	server.pointer_y = (int32_t)(server.height / 2U);
	server.pointer_unmoved = 1U;

	/* Input devices are found before READY; a seat without devices is still valid. */
	if (error == 0) {
		kwl_input_scan(&server);
		step_start = startup_step("input", step_start);
	}

	/* The keyboards' XKB keymap (keymap.c); without it they say there is none. */
	if (error == 0) {
		keymap_error = kwl_keymap_open();
		if (keymap_error == 0) {
			printf("KWL KEYMAP format=xkb_v1 errno=0\n");
		} else {
			printf("KWL KEYMAP format=none errno=%d\n", keymap_error);
		}

		/* How long the keymap took. */
		step_start = startup_step("keymap", step_start);
	}

	/* The login screen's users and sessiond's answers (greeter.c). */
	if (error == 0 && server.greeter)
		error = kwl_greeter_open(&server);

	/* The endpoint is published last; the login screen has none. */
	if (error == 0 && !server.greeter) {
		error = listen_socket(&server);
		(void)startup_step("socket", step_start);
	}

	/* The system's input method starts on a connection of its own (input-method.c). */
	if (error == 0)
		kwl_ime_start(&server);

	/* READY appears only after the hardware contract and socket namespace are both usable. */
	if (error == 0) {
		printf("KWL READY socket=%s width=%u height=%u timeout_ms=%llu pid=%ld role=%s\n", server.socket_path, server.width, server.height, (unsigned long long)server.timeout_ms, (long)getpid(), kwl_role_name(server.role));
		error = event_loop(&server);
	}

	/* No exit path leaves a lease, imported image or owned socket generation behind. */
	service_cleanup(&server);
	cleanup_failed = server.failed;
	printf("KWL EXIT frames=%llu error=%d cleanup_failed=%d pid=%ld input_events=%llu seat_events=%llu at_ms=%llu\n", (unsigned long long)server.frame, error, cleanup_failed, (long)getpid(), (unsigned long long)server.input_events, (unsigned long long)server.seat_events, (unsigned long long)kwl_milliseconds());
	if (error != 0 || cleanup_failed)
		return 1;

	/* Succeeded: the finite service run completed and every owned resource was retired. */
	return 0;
}

/*
 * Asks the event loop to end the compositor in order, as SIGTERM does: the login
 * screen after a login, a session at its Log Out.
 */
void
kwl_request_stop(
	void)
{
	/* The loop sees this at its next pass. */
	stop_requested = 1;
}

/* Requests cleanup from the main loop without touching non-signal-safe state. */
static void
stop_service(
	int signal_number)
{
	/* Both accepted termination signals mean the same orderly service withdrawal. */
	(void)signal_number;
	stop_requested = 1;

	/* Succeeded: the event loop will withdraw this service generation. */
	return;
}

/* Parses explicit bounded service settings without depending on environment state. */
static int
parse_options(
	struct kwl_server *server,
	int count,
	char **arguments)
{
	struct kwl_role_request request;
	struct kwl_role role;
	const char *argument;
	const char *text;
	uint64_t number;
	size_t length;
	int index;
	int match;
	int error;

	/* Each option is independent; the ones that bear on the role are only noted (role.c). */
	memset(&request, 0, sizeof(request));
	for (index = 1; index < count; index++) {
		/* Endpoint selection is validated before copying it into bounded service storage. */
		argument = arguments[index];
		match = strncmp(argument, "--socket=", 9);
		if (match == 0) {
			/* Relative or overlong endpoints cannot identify the service's owned pathname. */
			text = argument + 9;
			length = strlen(text);
			if (text[0] != '/' || length >= sizeof(server->socket_path))
				return EINVAL;

			/* Absolute paths make socket-generation ownership explicit. */
			strcpy(server->socket_path, text);
			server->socket_given = 1;
			continue;
		}

		/* Width and height remain subject to the native side-effect-free mode validation. */
		match = strncmp(argument, "--width=", 8);
		if (match == 0) {
			/* An out-of-range width cannot reach native mode validation. */
			error = unsigned_option(argument + 8, 16384, &number);
			if (error != 0)
				return error;

			/* Publish a positive bounded width only after parsing succeeds. */
			server->width = (uint32_t)number;
			server->size_given = 1;
			continue;
		}

		/* The configured image shape is fixed throughout this compositor generation. */
		match = strncmp(argument, "--height=", 9);
		if (match == 0) {
			/* An out-of-range height cannot reach native mode validation. */
			error = unsigned_option(argument + 9, 16384, &number);
			if (error != 0)
				return error;

			/* Publish a positive bounded height only after parsing succeeds. */
			server->height = (uint32_t)number;
			server->size_given = 1;
			continue;
		}

		/* The desktop surface's program and token (desktop.c, ws094-p002). */
		match = kwl_desktop_option(server, argument);
		if (match == 1)
			continue;
		if (match != 0)
			return EINVAL;

		/* A test run (WS110): finite, without a login session's features. */
		match = strcmp(argument, "--testing");
		if (match == 0) {
			request.testing = 1U;
			continue;
		}

		/* A test run's deadline (it needs --testing, role.c). */
		match = strncmp(argument, "--timeout=", 10);
		if (match == 0) {
			/* A positive finite timeout; a day is the longest. */
			error = unsigned_option(argument + 10, 86400, &number);
			if (error != 0)
				return error;

			/* Noted in the monotonic clock's units. */
			request.timeout = 1U;
			request.timeout_ms = number * 1000U;
			continue;
		}

		/* An optional frame bound can end a short smoke run before its wall-clock timeout. */
		match = strncmp(argument, "--max-frames=", 13);
		if (match == 0) {
			/* Completed-frame accounting remains finite when this additional limit is selected. */
			error = unsigned_option(argument + 13, 1000000, &number);
			if (error != 0)
				return error;

			/* Only completed GPU presentations count toward this bound; it needs --testing (role.c). */
			server->max_frames = number;
			request.max_frames = 1U;
			continue;
		}

		/* The on-screen keyboard's glass on the scene under it, blurred (ws075-p029; the default is the blurred wallpaper). */
		match = strcmp(argument, "--keyboard-blur");
		if (match == 0) {
			server->keyboard_blur = 1;
			continue;
		}

		/* The per-frame lines cost a console write each, so they are printed only on request. */
		match = strcmp(argument, "--log-frames");
		if (match == 0) {
			server->log_frames = 1;
			continue;
		}

		/* The login screen (ws035-p095): no socket, sessiond asked on --auth-fd, no deadline. */
		match = strcmp(argument, "--greeter");
		if (match == 0) {
			server->greeter = 1;
			request.greeter = 1U;
			continue;
		}

		/* The descriptor sessiond answers the login screen on. */
		match = strncmp(argument, "--auth-fd=", 10);
		if (match == 0) {
			error = unsigned_option(argument + 10, 1023, &number);
			if (error != 0)
				return error;
			server->auth_fd = (int)number;
			continue;
		}

		/* The descriptor a login session says READY to sessiond on (ws035-p101). */
		match = strncmp(argument, "--control-fd=", 13);
		if (match == 0) {
			error = unsigned_option(argument + 13, 1023, &number);
			if (error != 0)
				return error;
			server->control_fd = (int)number;
			request.control_fd = 1U;
			continue;
		}

		/* How long a login session goes without input before it locks (ws035-p102; 0: never). */
		match = strncmp(argument, "--lock-idle=", 12);
		if (match == 0) {
			error = unsigned_option(argument + 12, 86400, &number);
			if (error != 0)
				return error;
			server->lock_idle_ms = (uint64_t)number * 1000U;
			server->lock_idle_given = 1U;
			request.lock_idle = 1U;
			continue;
		}

		/* A login session (ws035-p095), which is now the default: kept as a name for it (WS110). */
		match = strcmp(argument, "--session");
		if (match == 0) {
			request.session = 1U;
			continue;
		}

		/* The glass look of window mode (ws035-p059). */
		match = strcmp(argument, "--glass");
		if (match == 0) {
			server->glass = 1;
			continue;
		}

		/* The glass look's font. */
		match = strncmp(argument, "--font=", 7);
		if (match == 0) {
			/* An absolute path, kept in argv's storage. */
			if (argument[7] != '/')
				return EINVAL;
			server->font_path = argument + 7;
			continue;
		}

		/* The glass look's font for the characters the first font lacks (optional). */
		match = strncmp(argument, "--fallback-font=", 16);
		if (match == 0) {
			/* An absolute path, kept in argv's storage. */
			if (argument[16] != '/')
				return EINVAL;
			server->fallback_font_path = argument + 16;
			continue;
		}

		/* The glass look's wallpaper, a binary PPM. */
		match = strncmp(argument, "--wallpaper=", 12);
		if (match == 0) {
			/* An absolute path, kept in argv's storage. */
			if (argument[12] != '/')
				return EINVAL;
			server->wallpaper_path = argument + 12;
			continue;
		}

		/* How opaque window bodies are over frosted glass, in percent. */
		match = strncmp(argument, "--window-opacity=", 17);
		if (match == 0) {
			error = unsigned_option(argument + 17, 100, &number);
			if (error != 0)
				return error;
			server->window_opacity = (float)number / 100.0f;
			continue;
		}

		/* Unknown arguments cannot silently alter the test or service contract. */
		return EINVAL;
	}

	/* The role, decided once whatever the options' order (WS110). */
	error = kwl_role_resolve(&request, &role);
	if (error != 0) {
		fprintf(stderr, "wayland: %s\n", role.refusal);
		return error;
	}

	/*
	 * A user's desktop is a login session: no deadline, App Home's Log
	 * Out, the desktop's files, the lock after idle time (home.c,
	 * desktop.c, main).
	 */
	server->role = role.role;
	server->timeout_ms = role.timeout_ms;
	server->session = 0;

	/* Only the normal role is a login session (the test run and the login screen are not). */
	if (role.role == KWL_ROLE_NORMAL)
		server->session = 1;

	/* Succeeded: every option is explicit, bounded and understood. */
	return 0;
}

/* Parses one positive decimal value without overflow, whitespace or trailing garbage. */
static int
unsigned_option(
	const char *text,
	uint64_t maximum,
	uint64_t *number)
{
	uint64_t parsed;
	unsigned digit;

	/* Empty values are not a request to retain the default setting. */
	if (*text == '\0')
		return EINVAL;

	/* A bounded multiply-add exposes each malformed or overflowing input directly. */
	parsed = 0;
	while (*text != '\0') {
		/* Signs, whitespace and trailing text are outside the numeric option grammar. */
		if (*text < '0' || *text > '9')
			return EINVAL;

		/* Refuse overflow before extending the number by another decimal digit. */
		digit = (unsigned)(*text - '0');
		if (parsed > maximum / 10U || (parsed == maximum / 10U && digit > maximum % 10U))
			return EINVAL;

		/* The validated digit extends the same positive bounded value. */
		parsed = parsed * 10U + digit;
		text++;
	}

	/* Zero would make dimensions invalid or silently remove a finite bound. */
	if (parsed == 0)
		return EINVAL;

	/* Succeeded: the caller receives one positive representable setting. */
	*number = parsed;
	return 0;
}

/* Publishes a new Unix endpoint without deleting or replacing an existing server path. */
static int
listen_socket(
	struct kwl_server *server)
{
	struct sockaddr_un address;
	struct stat status;
	socklen_t length;
	int error;

	/* Binding an existing pathname must fail instead of guessing whether its owner is stale. */
	server->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (server->listener < 0)
		return errno;

	/* The pathname terminator is part of the Unix address length. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, server->socket_path);
	length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1U);
	error = bind(server->listener, (const struct sockaddr *)&address, length);
	if (error != 0)
		return errno;

	/* Record the actual created inode so cleanup never intentionally removes a replacement. */
	error = lstat(server->socket_path, &status);
	if (error != 0)
		return errno;

	/* The pathname identity belongs only to this successfully bound generation. */
	server->socket_device = status.st_dev;
	server->socket_inode = status.st_ino;
	server->socket_owned = 1;
	error = listen(server->listener, 16);
	if (error != 0)
		return errno;

	/* Succeeded: incoming clients can reach this exact socket generation. */
	return 0;
}

/* Removes only the pathname identity created by this service instance. */
static void
unlink_socket(
	struct kwl_server *server)
{
	struct stat status;
	int error;

	/* Failed binds never own an existing server's pathname. */
	if (!server->socket_owned)
		return;

	/* A removed or replaced socket path no longer belongs to this cleanup pass. */
	server->socket_owned = 0;
	error = lstat(server->socket_path, &status);
	if (error != 0)
		return;

	/* Inode identity protects ordinary concurrent server-generation replacement. */
	if (status.st_dev != server->socket_device || status.st_ino != server->socket_inode)
		return;

	/* The known pathname is withdrawn before the listener descriptor is closed. */
	error = unlink(server->socket_path);
	if (error != 0 && errno != ENOENT)
		server->failed = 1;

	/* Succeeded: this service generation no longer publishes its pathname. */
	return;
}

/* Accepts one client and creates its required display object before processing requests. */
static int
accept_client(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *display;
	int descriptor;

	/* Nonblocking acceptance cannot stall already connected clients. */
	descriptor = accept4(server->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (descriptor < 0) {
		/* Transient readiness or signal changes leave the listening generation usable. */
		if (errno == EAGAIN ||
		    errno == EWOULDBLOCK ||
		    errno == EINTR)
			return 0;

		/* Per-process descriptor exhaustion is explicit and terminates this finite run. */
		return errno;
	}

	/* Client state owns every subsequently received byte, fd and object. */
	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* The service-local client serial distinguishes descriptor-number reuse in logs. */
	client->fd = descriptor;
	client->server = server;
	client->number = ++server->client_serial;
	client->connected_ms = kwl_milliseconds();
	client->next = server->clients;
	server->clients = client;
	display = kwl_create(client, 1, KWL_DISPLAY, 1);
	if (display == NULL) {
		kwl_client_destroy(client);
		return ENOMEM;
	}

	/* READY already guarantees that this independent namespace can import images. */
	printf("KWL CLIENT client=%llu fd=%d\n", (unsigned long long)client->number, descriptor);

	/* Succeeded: the display object owns the connection's first live identity. */
	return 0;
}

/* Services independent client streams and schedules bounded fullscreen presentations. */
static int
event_loop(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_client **clients;
	struct kwl_object *surface;
	struct kwl_input_device *devices[KWL_INPUT_MAX];
	struct kwl_input_device *ready_devices[KWL_INPUT_MAX];
	size_t ready_count;
	struct pollfd *descriptors;
	uint64_t started;
	uint64_t now;
	uint64_t scan_period;
	size_t count;
	size_t index;
	size_t first_input;
	size_t last_input;
	size_t frame_slot;
	size_t first_fence;
	size_t first_os;
	size_t os_count;
	unsigned fence;
	int waiting;
	int retire_ready;
	unsigned slot;
	uint64_t mark;
	int timeout;
	int ready;
	int flushed;
	int error;
	int remove;
	const char *why;

	/* A finite monotonic deadline covers both idle service and active clients. */
	started = kwl_milliseconds();
	if (started == UINT64_MAX)
		return EIO;

	/* Each pass rebuilds the descriptor generation snapshot after prior cleanup. */
	while (!stop_requested && !server->failed) {
		/* Clock failure must not turn the configured service deadline into an endless run. */
		now = kwl_milliseconds();
		if (now == UINT64_MAX)
			return EIO;

		/* The optional frame bound counts only completed hardware selections. */
		if (now - started >= server->timeout_ms ||
		    (server->max_frames != 0 && server->frame >= server->max_frames))
			break;

		/* Evdev nodes that appeared since the last scan join the seat, soon after a device's event more often. */
		scan_period = KWL_INPUT_SCAN_MS;
		if (now < server->input_settle_until)
			scan_period = KWL_INPUT_SETTLE_SCAN_MS;
		if (server->os_paused == 0 && now - server->input_scan_time >= scan_period)
			kwl_input_scan(server);

		/* The touch pads' timers: a tap's click completes when no drag came (input.c, ws159-p004). */
		kwl_input_tick(server, now);

		/* A client that has left a ping unanswered too long is not responding (toplevel.c). */
		kwl_ping_check(server, now);

		/* The windows hear new bounds when the space for bodies changed (the glass look given up, protocol.c). */
		kwl_window_bounds_refresh(server);

		/* The settings: a wallpaper read meanwhile, audiod's sound, and the changes told to the clients (settings.c). */
		kwl_settings_tick(server);

		/* The system extension: its threads' work taken, the sound's changes told (system.c). */
		kwl_system_tick(server);

		/* The input method is looked after: started again, passed by when it does not answer (input-method.c). */
		kwl_ime_tick(server, now);

		/* Allocate exactly enough poll storage for the presently live client and device set. */
		count = 1;
		for (client = server->clients; client != NULL; client = client->next)
			count++;


		/* Input devices follow the clients in the same poll snapshot. */
		first_input = count;
		for (slot = 0; slot < KWL_INPUT_MAX; slot++) {
			/* Only open devices are polled; the table keeps each slot's address stable. */
			if (server->inputs[slot].live) {
				devices[count - first_input] = &server->inputs[slot];
				count++;
			}
		}

		/* The devices end here. */
		last_input = count;

		/* The fence fd of window mode's frame in flight is polled next. */
		frame_slot = 0;
		if (server->frame_fd >= 0) {
			frame_slot = count;
			count++;
		}

		/*
		 * The acquire fences of committed images are polled last.  A fatal
		 * client's are not: its commits are never taken, and a fence that is
		 * always readable (any fd a client sent) would wake the loop until the
		 * client is retired (ws103-p006).
		 */
		first_fence = count;
		for (client = server->clients; client != NULL; client = client->next) {
			/* A connection being retired contributes no fence. */
			if (client->fatal)
				continue;

			/* Each live surface's pending fences. */
			for (surface = client->objects; surface != NULL; surface = surface->next) {
				if (surface->kind == KWL_SURFACE && !surface->dead)
					count += surface->fence_count;
			}
		}

		/* Counts the OS entries after the input and fence descriptors. */
		first_os = count;
		os_count = kwl_os_poll_count(server);
		count += os_count;

		/* Allocation failure leaves all live clients owned by service cleanup. */
		descriptors = calloc(count, sizeof(*descriptors));
		if (descriptors == NULL)
			return ENOMEM;

		/* The parallel pointer array resolves readiness to this snapshot's generation. */
		clients = calloc(count, sizeof(*clients));
		if (clients == NULL) {
			free(descriptors);
			return ENOMEM;
		}

		/* A short finite poll interval also bounds cleanup of fatal clients with stalled output. */
		descriptors[0].fd = server->listener;
		descriptors[0].events = POLLIN;
		timeout = 10;
		waiting = kwl_compose_waiting(server);
		if (waiting)
			timeout = 2;

		/* A gone client's buffer that may be released now: the input is looked at, then it is released (BUG-239). */
		retire_ready = kwl_retire_ready(server, kwl_milliseconds());
		if (retire_ready)
			timeout = 0;
		index = 1;
		for (client = server->clients; client != NULL; client = client->next) {
			/* Retain the snapshot identity while suppressing new input on fatal connections. */
			clients[index] = client;
			descriptors[index].fd = client->fd;
			if (!client->fatal)
				descriptors[index].events = POLLIN;

			/* Queued bytes retain interest in writable readiness without busy-looping send. */
			if (client->output_head != NULL)
				descriptors[index].events |= POLLOUT;

			/* A fully flushed fatal connection can be retired immediately. */
			if (client->fatal && client->output_head == NULL)
				timeout = 0;

			/* Each slot and pointer now describe one live connection generation. */
			index++;
		}

		/* Every open device is polled for readable events. */
		for (; index < last_input; index++) {
			descriptors[index].fd = devices[index - first_input]->fd;
			descriptors[index].events = POLLIN;
		}

		/* The frame's fence becomes readable when the frame is done. */
		if (frame_slot != 0) {
			descriptors[frame_slot].fd = server->frame_fd;
			descriptors[frame_slot].events = POLLIN;
		}

		/* An acquire fence wakes the loop when its image is rendered; the scheduler then takes it. */
		index = first_fence;
		for (client = server->clients; client != NULL; client = client->next) {
			/* A connection being retired contributes no fence (counted the same way above). */
			if (client->fatal)
				continue;

			/* Each live surface's pending fences. */
			for (surface = client->objects; surface != NULL; surface = surface->next) {
				if (surface->kind != KWL_SURFACE || surface->dead)
					continue;
				for (fence = 0; fence < surface->fence_count; fence++) {
					descriptors[index].fd = surface->fences[fence].fd;
					descriptors[index].events = POLLIN;
					index++;
				}
			}
		}

		/* Lets the OS populate its entries in this descriptor snapshot. */
		kwl_os_poll_fill(server, descriptors + first_os);

		/* Poll sees sockets only; typed image fds are consumed immediately during import. */
		mark = kwl_cycles();
		ready = poll(descriptors, count, timeout);
		server->perf.poll_cycles += kwl_cycles() - mark;
		mark = kwl_cycles();
		server->perf.passes++;
		if (ready == 0)
			server->perf.timeouts++;
		if (ready < 0) {
			error = errno;
			free(clients);
			free(descriptors);

			/* A signal wakeup returns to the loop's ordinary shutdown and deadline checks. */
			if (error == EINTR)
				continue;

			/* Other poll failures stop the service through ordinary cleanup. */
			return error;
		}

		/* Accept at most one client per pass so existing streams retain service opportunity. */
		error = 0;
		if ((descriptors[0].revents & POLLIN) != 0)
			error = accept_client(server);

		/* A failed listener cannot serve this generation any further. */
		if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			error = EIO;

		/* Device authority changes before the old snapshot can read revoked input or complete a frame. */
		kwl_os_poll_done(server, descriptors + first_os);

		/* A finished frame releases its buffers and sends its callbacks; a fence without an fd is asked. */
		if (frame_slot != 0 && descriptors[frame_slot].revents != 0)
			kwl_frame_done(server);
		kwl_compose_poll(server);

		/*
		 * Device events are applied before clients are flushed, so they leave in this pass.  A readable, failed or
		 * vanished device is read, all of them together in the order their events were made (BUG-142); a read
		 * failure closes the device.
		 */
		ready_count = 0;
		for (index = first_input; index < last_input; index++) {
			if (descriptors[index].revents != 0 && devices[index - first_input]->fd >= 0)
				ready_devices[ready_count++] = devices[index - first_input];
		}

		/* Reads them together. */
		if (ready_count != 0)
			kwl_input_read_devices(server, ready_devices, ready_count);

		/* Process only the clients captured by this poll snapshot. */
		for (index = 1; index < first_input; index++) {
			/* Apply readiness only to this iteration's still-owned connection generation. */
			client = clients[index];
			remove = 0;
			why = "read";
			if ((descriptors[index].revents & POLLIN) != 0 && !client->fatal) {
				/* Decode all complete requests while retaining partial bytes and ancillary ownership. */
				ready = kwl_read(client);
				if (ready != 0 && !client->fatal) {
					/* Malformed ancillary data gets a terminal protocol event before withdrawal. */
					if (ready == EPROTO)
						(void)kwl_error(client, 1, "malformed ancillary input");
					else
						remove = 1;
				}
			}

			/* A disconnected peer cannot receive queued events or use future GPU imports. */
			if (!remove && (descriptors[index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				remove = 1;
				why = "hangup";
			}

			/* Flush events produced by requests even when POLLOUT was absent from this snapshot. */
			if (!remove) {
				/* Preserve unsent suffixes, but close a stream that can no longer accept events. */
				ready = kwl_flush(client);
				if (ready != 0) {
					remove = 1;
					why = "flush";
				}
			}

			/* Protocol errors get a bounded final flush before their namespace is destroyed. */
			if (client->fatal &&
			    (client->output_head == NULL ||
			     (now >= client->fatal_time && now - client->fatal_time >= 2000U))) {
				remove = 1;
				why = "error";
			}

			/* Withdrawal releases unread rights and scanout ownership before fd reuse (the log line BUG-121's test reads). */
			if (remove) {
				printf("KWL CLIENT gone client=%llu reason=%s\n", (unsigned long long)client->number, why);
				kwl_client_destroy(client);
			}
		}

		/* The clipboard's history reads what a source has written so far (clipboard.c; the pass is at most 10 ms). */
		kwl_clipboard_poll(server);

		/* A test image's screen capture takes its requests (shot.c, ws173-p002). */
		kwl_shot_tick(server);

		/* The snapshot contains no ownership references beyond this iteration. */
		free(clients);
		free(descriptors);
		if (error != 0)
			return error;

		/* The scheduler takes the committed images and draws a frame when one is due. */
		kwl_schedule(server);

		/*
		 * One gone client's buffer released after the frame, only while the
		 * compositor is idle, so that neither a frame nor the input waits
		 * for a release (BUG-239, T1-392).
		 */
		retire_ready = kwl_retire_ready(server, kwl_milliseconds());
		if (retire_ready)
			(void)kwl_retire_tick(server);

		/*
		 * The presentation queued frame callbacks and buffer releases; they
		 * leave now, not after the next poll, which would otherwise hold a
		 * FIFO client for up to the poll timeout every frame.
		 */
		for (client = server->clients; client != NULL; client = client->next) {
			/* A connection that cannot take its events is retired on the next pass. */
			if (client->fatal)
				continue;
			flushed = kwl_flush(client);
			if (flushed != 0) {
				client->fatal = 1;
				client->fatal_time = kwl_milliseconds();
			}
		}

		/* The pass's work. */
		server->perf.work_cycles += kwl_cycles() - mark;
		kwl_perf_report(server, now);
	}

	/* A hardware cleanup failure is distinct from a normal finite timeout or signal exit. */
	if (server->failed)
		return EIO;

	/* Succeeded: the requested finite service interval has ended. */
	return 0;
}

/* Withdraws display ownership, client generations and the service's own pathname in order. */
static void
service_cleanup(
	struct kwl_server *server)
{
	/* No new client should observe a service whose cleanup has begun. */
	unlink_socket(server);
	if (server->listener >= 0) {
		close(server->listener);
		server->listener = -1;
	}

	/* Window mode's frame in flight finishes before the clients it holds go; the capture answers what waits. */
	kwl_compose_quiesce(server);
	kwl_shot_close(server);

	/* Each client cleanup closes both user-received and still-kernel-queued rights. */
	while (server->clients != NULL)
		kwl_client_destroy(server->clients);

	/* The gone clients' buffers still waiting are released before the device (BUG-239). */
	kwl_retire_flush(server);

	/* The swapchain and the Vulkan device go after every buffer's image. */
	kwl_compose_close(server);

	/* No client remains to hear from the seat, so its devices close quietly. */
	kwl_input_cleanup(server);

	/* The session's volume and settings are kept for the next login (volume.c, BUG-161; settings.c, WS135). */
	kwl_volume_keep(server, "end");
	kwl_settings_close(server);

	/* The system extension's threads end before the backend closes (system.c). */
	kwl_system_close(server);

	/* Returns the OS resources after input and display cleanup. */
	kwl_os_close(server);

	/* A screen the lid put out is lit for whoever comes next, and the backlight closed (backend-host.c, ws132-p008). */
	kwl_lid_screen_restore(server);
	kl_backend_backlight_close(server->backlight);
	server->backlight = NULL;

	/* Closes the operating system's side last, after everything that used it. */
	kl_backend_close(server->backend);
	server->backend = NULL;

	/* Succeeded: the service retains no listener, client or Vulkan object. */
	return;
}

/*
 * Reads the processor's cycle counter (the millisecond clock has only tick
 * resolution).  On arm64 (the Linux arm64 package, WS112) the virtual
 * counter of the generic timer.
 */
uint64_t
kwl_cycles(
	void)
{
#if defined(__aarch64__)
	uint64_t value;

	/* The generic timer's virtual count. */
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));

	/* Succeeded: the counter. */
	return value;
#else
	uint32_t low;
	uint32_t high;

	/* The time stamp counter's two halves. */
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));

	/* Succeeded: the counter as one value. */
	return ((uint64_t)high << 32) | low;
#endif
}

/* Prints the loop's timing every five seconds and starts a new window. */
static void
kwl_perf_report(
	struct kwl_server *server,
	uint64_t now)
{
	struct kwl_perf *perf;
	uint64_t cycles;
	double per_ms;

	/* The first call starts the window. */
	perf = &server->perf;
	if (perf->window_start_ms == 0) {
		perf->window_start_ms = now;
		perf->window_start_cycles = kwl_cycles();
		return;
	}

	/* A window shorter than five seconds is not reported yet. */
	if (now - perf->window_start_ms < 5000U)
		return;

	/* The window's length in cycles scales the counters to milliseconds. */
	cycles = kwl_cycles() - perf->window_start_cycles;
	per_ms = (double)cycles / (double)(now - perf->window_start_ms);

	/* The loop's line. */
	printf("KWL PERF %llums: passes=%u timeouts=%u | poll %.1f%% work %.1f%%\n",
	    (unsigned long long)(now - perf->window_start_ms),
	    perf->passes,
	    perf->timeouts,
	    100.0 * (double)perf->poll_cycles / (double)cycles,
	    100.0 * (double)perf->work_cycles / (double)cycles);

	/* Window mode's frames: how many, the CPU time to record and submit one, and the time until its fence. */
	if (perf->compose_frames != 0) {
		printf("KWL PERF compose frames=%u draw_ms=%.2f (acquire %.2f submit+present %.2f) frame_ms=%.2f\n",
		    perf->compose_frames,
		    (double)perf->compose_draw_cycles / per_ms / (double)perf->compose_frames,
		    (double)perf->compose_acquire_cycles / per_ms / (double)perf->compose_frames,
		    (double)perf->compose_present_cycles / per_ms / (double)perf->compose_frames,
		    (double)perf->compose_cycles / per_ms / (double)perf->compose_frames);
	}

	/* wl_shm's copies: how many, and the CPU time of one. */
	if (perf->shm_copies != 0) {
		printf("KWL PERF shm copies=%u copy_ms=%.3f\n",
		    perf->shm_copies,
		    (double)perf->shm_copy_cycles / per_ms / (double)perf->shm_copies);
	}

	/* The lines go out, and a new window starts. */
	fflush(stdout);
	memset(perf, 0, sizeof(*perf));
	perf->window_start_ms = now;
	perf->window_start_cycles = kwl_cycles();
}

/*
 * Reports how long one step of the start took, and when it ended (the
 * diagnostic line KWL STARTUP, ws035-p129); returns the time it ended.
 */
static uint64_t
startup_step(
	const char *step,
	uint64_t since)
{
	uint64_t now;

	/* The step's length, and the monotonic time the hand-over's lines use too. */
	now = kwl_milliseconds();
	printf("KWL STARTUP step=%s ms=%llu at_ms=%llu\n", step, (unsigned long long)(now - since), (unsigned long long)now);

	/* Succeeded: the next step starts now. */
	return now;
}

/*
 * Makes every write to a log descriptor go to the end of its file
 * (O_APPEND on the open file, which the programs the compositor starts share).
 * A log the shell opened with ">" may be truncated by the next run while
 * this one (or a program it started) still writes; at the old offset that
 * write left a hole of NUL bytes, and grep then read the log as binary.  A
 * pipe or a terminal is not changed by the flag, and a descriptor that
 * cannot take it is left as it is.
 */
static void
log_append(
	int descriptor)
{
	int flags;
	int error;

	/* The open file's status flags. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0)
		return;

	/* Already appending. */
	if ((flags & O_APPEND) != 0)
		return;

	/* Appending from now on; a refusal keeps the old way of writing. */
	error = fcntl(descriptor, F_SETFL, flags | O_APPEND);
	if (error != 0)
		return;

	/* Succeeded: the log is written at its end. */
	return;
}
