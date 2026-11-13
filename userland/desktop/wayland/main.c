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
#include "threads.h"
#include "media.h"
#include "role.h"
#include "toplevel.h"
#include "keymap.h"
#include "ime.h"
#include "data.h"
#include "titlebar.h"
#include "userland/desktop/paths.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Signal handlers only request ordinary event-loop cleanup, they own no GPU state. */
static volatile sig_atomic_t stop_requested;

/* The write end of the threads' wake pipe for the signal handlers (-1 until it is open). */
static volatile sig_atomic_t signal_wake = -1;

static void stop_service(int signal_number);
static void child_ended(int signal_number);
static void signal_wake_loop(void);
static int child_wake_install(void);
static void compose_unavailable_reason(void);
static void log_append(int descriptor);
static int parse_options(struct kwl_server *server, int count, char **arguments);
static int unsigned_option(const char *text, uint64_t maximum, uint64_t *number);
static int listen_socket(struct kwl_server *server);
static void unlink_socket(struct kwl_server *server);
static int accept_client(struct kwl_server *server);
static void kwl_perf_report(struct kwl_server *server, uint64_t now);
static void perf_walk_report(const struct kwl_perf *perf, double per_ms);
static void perf_walk_class(const struct kwl_perf *perf, double per_ms, const char *name, uint32_t lowest, uint32_t highest, unsigned quiet, unsigned replay);
static int perf_cycles_order(const void *left, const void *right);
static void perf_pass_report(struct kwl_perf *perf, double per_ms);
static void perf_pass_begin(struct kwl_server *server);
static void perf_pass_end(struct kwl_server *server, uint64_t cycles);
static void perf_parts_take(const struct kwl_perf *perf, struct kwl_perf_parts *parts);
static uint64_t perf_part_us(uint64_t cycles, uint64_t pass_cycles, uint64_t pass_us);
static void perf_box_report(struct kwl_perf *perf, const struct kwl_cursor_stats *stats);
static uint32_t perf_percentile(uint32_t *values, uint32_t count, unsigned percent, uint32_t *largest);
static int perf_value_order(const void *left, const void *right);
static int event_loop(struct kwl_server *server);
static int loop_timeout(struct kwl_server *server);
static void wake_drain(struct kwl_server *server);
static void input_scan_pace(struct kwl_server *server, uint64_t now);
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
	int timers_error;
	int cleanup_failed;
	int locked;
	uint64_t step_start;

	/* Every descriptor starts invalid so partial initialization can use ordinary cleanup. */
	memset(&server, 0, sizeof(server));
	server.listener = -1;
	server.auth_fd = -1;
	server.control_fd = -1;
	server.wake[0] = -1;
	server.wake[1] = -1;
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
	server.size_width = 320;
	server.size_height = 240;

	/* The session starts on the middle desktop, the others to its left and right. */
	server.desktop = KWL_DESKTOP_START;
	server.desktop_from = (float)KWL_DESKTOP_START;
	server.desktop_to = (float)KWL_DESKTOP_START;
	strcpy(server.socket_path, "/tmp/wayland-0");
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The log is written at its end, whoever truncated it meanwhile (no hole of NUL bytes). */
	log_append(STDOUT_FILENO);
	log_append(STDERR_FILENO);

	/* When the program began, against the kernel's start (KWL STARTUP step=start), and the steps before Vulkan from it. */
	step_start = kwl_milliseconds();
	step_start = startup_step("start", step_start);

	/* The backend reports nothing to the compositor yet; its callbacks come with the areas that need them. */
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
	backend_host.keys_changed = kwl_backend_keys_changed;

	/* Reads the command line; a mistake ends the run with the usage. */
	error = parse_options(&server, count, arguments);
	if (error != 0) {
		fprintf(stderr, "usage: wayland [--socket=/path] [--width=N] [--height=N] [--rotate=0|90|180|270] [--testing [--timeout=seconds] [--max-frames=N]] [--log-frames] [--no-layer-cache] [--keyboard-blur] [--glass] [--font=/path] [--fallback-font=/path] [--wallpaper=/path.png|.jpg] [--window-opacity=1..100] [--desktop-client=COMMAND|none] [--desktop-token=TOKEN] [--session [--control-fd=N [--locked]] [--lock-idle=seconds] | --greeter --auth-fd=N]\n");
		return 2;
	}

	/* The desktop's size until the display is known: the size asked for, unrotated. */
	kwl_output_size_set(&server, server.size_width, server.size_height, KWL_ROTATE_0);

	/*
	 * A login session locks after the time without input its settings
	 * give (settings.c, power.lock: never unless chosen), or the time
	 * --lock-idle gave; the time counts from now.
	 */
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
	 * (settings.c), read before the look draws its wallpaper so
	 * that the picture is read once.
	 */
	if (!server.greeter)
		kwl_settings_open(&server);

	/* The login screen's text is in the system's language; a session's comes with its settings. */
	if (server.greeter)
		kwl_language_system(&server);

	/* A test image's screen capture listens (shot.c; nothing elsewhere). */
	kwl_shot_open(&server);

	/* How long the settings and the language took. */
	step_start = startup_step("settings", step_start);

	/* Catch normal termination without performing allocation or I/O inside a signal handler. */
	previous_handler = signal(SIGINT, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/* SIGTERM follows the same lease-safe shutdown path. */
	previous_handler = signal(SIGTERM, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/* So does SIGHUP, so that the session's settings are written. */
	previous_handler = signal(SIGHUP, stop_service);
	if (previous_handler == SIG_ERR)
		return 1;

	/*
	 * Opens the operating system's side (libkeiland-backend) before
	 * anything asks it for a resource.  The login screen's and a session's
	 * descriptors to sessiond carry the session and the power
	 * requests.
	 */
	backend_options.greeter_descriptor = -1;
	backend_options.session_descriptor = -1;
	if (server.greeter)
		backend_options.greeter_descriptor = server.auth_fd;
	else
		backend_options.session_descriptor = server.control_fd;

	/*
	 * The threads' wake pipe first, lent to the backend with the thread
	 * register for the daemons' connections' threads (without the pipe
	 * the backend's updates find their state on their own).
	 */
	error = kwl_wake_open(&server);
	if (error != 0)
		printf("KWL FAILED site=wake errno=%d\n", error);
	signal_wake = server.wake[1];

	/*
	 * The main thread's timers, which features arm from now on; their
	 * thread starts after the other threads.  Without them the event
	 * loop's own deadline takes the earliest.
	 */
	timers_error = kwl_timers_init(&server.timers);
	if (timers_error == 0) {
		server.timers_ready = 1U;
	} else {
		printf("KWL TIMER unavailable errno=%d\n", timers_error);
	}

	/* The gone clients' releases have their timer from the start. */
	kwl_retire_init(&server);

	/*
	 * A child's end wakes the event loop, which reaps it (the desktop's
	 * program, the input method, App Home's programs) and starts it again
	 * when it has to; the loop does not look for it on a clock.
	 */
	if (child_wake_install() != 0)
		return 1;
	backend_options.threads.wake_descriptor = server.wake[1];
	backend_options.threads.start = kwl_thread_start_lent;
	backend_options.threads.ended = kwl_thread_ended;
	error = kl_backend_open(&backend_options, &backend_host, &server.backend);
	if (error != 0)
		printf("KWL BACKEND unavailable errno=%d\n", error);

	/* How long the operating system's side took to open. */
	step_start = startup_step("backend", step_start);

	/* The power as the backend knows it now, for the bar's battery. */
	kwl_power_read(&server);
	step_start = startup_step("power", step_start);

	/* Takes the OS's seat resources before Vulkan opens the display. */
	if (error == 0) {
		error = kwl_os_open(&server);
		if (error != 0)
			printf("KWL OS unavailable errno=%d\n", error);
	}

	/* How long the seat's resources took. */
	step_start = startup_step("seat", step_start);

	/*
	 * A session serves the notifications of the desktop's bus where the
	 * system has one (Linux's session bus); without it the
	 * session goes on with Keiland's own notifications only.
	 */
	if (error == 0 && !server.greeter)
		(void)kwl_notify_bus_open(&server);

	/* Reads the wallpaper while Vulkan starts, only after OS startup succeeded. */
	if (error == 0)
		kwl_glass_prefetch(&server);

	/* The threads' wake pipe and the cursor's box, which the cursor plane's thread reads from the output's opening on. */
	if (error == 0) {
		error = kwl_threads_prepare(&server);
		if (error != 0)
			printf("KWL FAILED site=threads errno=%d\n", error);
	}

	/* The device I/O thread, which opens and closes the input devices away from the event loop (io.c). */
	if (error == 0)
		kwl_io_start(&server);

	/* A session's printers, opened before the clients come; their file is the file I/O thread's from then on (printers-shell.c). */
	if (error == 0 && !server.greeter)
		kwl_printers_prepare(&server);

	/* How long the notifications' bus, the wallpaper's reading thread and the threads' pipe took to start. */
	step_start = startup_step("prepare", step_start);

	/* The compose step is timed from the Vulkan device on; the compositor opens no GPU node of its own. */

	/*
	 * Window mode's Vulkan device, which also gives the display's size and
	 * refresh.  Without it nothing can be shown, so the compositor does not start.
	 */
	if (error == 0) {
		error = kwl_compose_open(&server);
		if (error != 0) {
			printf("KWL COMPOSE unavailable errno=%d\n", error);
			compose_unavailable_reason();
		}

		/* How long the device, the wallpaper and the glyphs took (KWL STARTUP). */
		step_start = startup_step("compose", step_start);
	}

	/* The pointer starts in the middle of the output, its arrow not shown until it moves. */
	server.pointer_x = (int32_t)(server.width / 2U);
	server.pointer_y = (int32_t)(server.height / 2U);
	server.pointer_unmoved = 1U;

	/* The input thread reads the devices and moves the pointer in the cursor's box. */
	if (error == 0) {
		error = kwl_input_threads_start(&server);
		if (error != 0)
			printf("KWL FAILED site=input-thread errno=%d\n", error);
	}

	/*
	 * The input devices are looked for before READY (where the device I/O
	 * thread opens them they join a moment later, from the event loop); a
	 * seat without devices is still valid.
	 */
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

	/* A session started again after a crash while it was locked shows the lock screen from its first frame. */
	if (error == 0 && server.start_locked) {
		locked = kwl_lock(&server, "restart");
		if (!locked)
			printf("KWL LOCK restart refused\n");
	}

	/* READY appears only after the hardware contract and socket namespace are both usable. */
	if (error == 0) {
		printf("KWL READY socket=%s width=%u height=%u timeout_ms=%llu pid=%ld role=%s\n", server.socket_path, server.width, server.height, (unsigned long long)server.timeout_ms, (long)getpid(), kwl_role_name(server.role));

		/* The timer thread, after the other threads, so that a timer's handler finds them started. */
		if (server.timers_ready) {
			timers_error = kwl_timers_start(&server.timers, server.wake[1]);
			if (timers_error != 0)
				printf("KWL TIMER unavailable errno=%d\n", timers_error);
		}

		/* The threads of the compositor's register, by name (threads.c). */
		kwl_threads_report();
		error = event_loop(&server);
	}

	/* The timer thread ends before the other threads (no timer fires after the event loop). */
	kwl_timers_stop(&server.timers);

	/* No exit path leaves a lease, imported image or owned socket generation behind. */
	service_cleanup(&server);
	cleanup_failed = server.failed;

	/* The timers' lock and condition, once nothing arms them any more. */
	kwl_timers_destroy(&server.timers);

	/* A failed run ends with its line in the system log too, which the next session does not truncate. */
	if (error != 0 || cleanup_failed) {
		kwl_fatal("KWL EXIT frames=%llu error=%d cleanup_failed=%d pid=%ld input_events=%llu seat_events=%llu at_ms=%llu", (unsigned long long)server.frame, error, cleanup_failed, (long)getpid(), (unsigned long long)server.input_events, (unsigned long long)server.seat_events, (unsigned long long)kwl_milliseconds());
		return 1;
	}

	/* An ordinary end's line, in the session's output only. */
	printf("KWL EXIT frames=%llu error=%d cleanup_failed=%d pid=%ld input_events=%llu seat_events=%llu at_ms=%llu\n", (unsigned long long)server.frame, error, cleanup_failed, (long)getpid(), (unsigned long long)server.input_events, (unsigned long long)server.seat_events, (unsigned long long)kwl_milliseconds());

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

/*
 * Reports why the compositor ends, as one KWL line: to its output like
 * every line, flushed (an _exit after it flushes nothing), and to the
 * system log, where it outlives the session (the next login truncates the
 * session's output: a crash on the Pi 2026-10-12 left no line).
 */
void
kwl_fatal(
	const char *format,
	...)
{
	va_list arguments;
	char line[256];

	/* The line, cut to what one system log record takes. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* The session's output. */
	printf("%s\n", line);
	fflush(stdout);

	/* The system log, under the compositor's name. */
	openlog("wayland", LOG_PID, LOG_USER);
	syslog(LOG_ERR, "%s", line);
}

/* Requests cleanup from the main loop without touching non-signal-safe state. */
static void
stop_service(
	int signal_number)
{
	/* Both accepted termination signals mean the same orderly service withdrawal. */
	(void)signal_number;
	stop_requested = 1;

	/* The loop's wait ends now. */
	signal_wake_loop();

	/* Succeeded: the event loop will withdraw this service generation. */
	return;
}

/* Wakes the event loop when a child ended: the parts that reap their children look at once. */
static void
child_ended(
	int signal_number)
{
	/* Only the wake; the reaping is the loop's. */
	(void)signal_number;
	signal_wake_loop();
}

/* Writes one byte to the threads' wake pipe from a signal handler (write is async-signal-safe). */
static void
signal_wake_loop(
	void)
{
	unsigned char byte;
	int saved;
	int descriptor;

	/* The handler keeps the errno of the code it interrupted. */
	saved = errno;
	descriptor = signal_wake;
	byte = 1U;
	if (descriptor >= 0)
		(void)write(descriptor, &byte, 1U);
	errno = saved;
}

/* Installs the SIGCHLD handler that wakes the loop; restartable calls go on after it. */
static int
child_wake_install(
	void)
{
	struct sigaction action;

	/* Only ends and stops of children matter for the reaping: ends alone. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = child_ended;
	(void)sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &action, NULL) != 0)
		return errno;

	/* Succeeded: an ended child wakes the loop. */
	return 0;
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
	int differs;
	int valid;

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
			server->size_width = (uint32_t)number;
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
			server->size_height = (uint32_t)number;
			server->size_given = 1;
			continue;
		}

		/* A test's rotation of the output, over displays.conf's: one of the four angles. */
		match = strncmp(argument, "--rotate=", 9);
		if (match == 0) {
			/* 0 is an angle (unsigned_option refuses a zero); any other number is read bounded. */
			differs = strcmp(argument + 9, "0");
			number = 0U;
			if (differs != 0) {
				error = unsigned_option(argument + 9, 270, &number);
				if (error != 0)
					return error;
			}

			/* A number that is not an angle is a mistake. */
			valid = kwl_rotate_valid((unsigned)number);
			if (!valid)
				return EINVAL;

			/* Kept for the output's opening, which tells whether the display can take it. */
			server->rotate_option = (unsigned)number;
			server->rotate_given = 1;
			continue;
		}

		/* A test image's own option (shot.c: --rotate-policy); a product's compositor takes none. */
		match = kwl_shot_option(server, argument);
		if (match == 1)
			continue;
		if (match < 0)
			return EINVAL;

		/* The desktop surface's program and token (desktop.c). */
		match = kwl_desktop_option(server, argument);
		if (match == 1)
			continue;
		if (match != 0)
			return EINVAL;

		/* A test run: finite, without a login session's features. */
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

		/* The on-screen keyboard's glass on the scene under it, blurred (the default is the blurred wallpaper). */
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

		/* Every frame drawn whole, without the layers' pictures (the reference for their tests). */
		match = strcmp(argument, "--no-layer-cache");
		if (match == 0) {
			server->layer_cache_off = 1U;
			continue;
		}

		/* The login screen: no socket, sessiond asked on --auth-fd, no deadline. */
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

		/* The descriptor a login session says READY to sessiond on. */
		match = strncmp(argument, "--control-fd=", 13);
		if (match == 0) {
			error = unsigned_option(argument + 13, 1023, &number);
			if (error != 0)
				return error;
			server->control_fd = (int)number;
			request.control_fd = 1U;
			continue;
		}

		/* How long a login session goes without input before it locks (0: never). */
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

		/* A login session that starts with its lock screen (sessiond's, after a crash while it was locked). */
		match = strcmp(argument, "--locked");
		if (match == 0) {
			server->start_locked = 1U;
			request.locked = 1U;
			continue;
		}

		/* A login session, which is now the default: kept as a name for it. */
		match = strcmp(argument, "--session");
		if (match == 0) {
			request.session = 1U;
			continue;
		}

		/* The glass look of window mode. */
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

	/* The role, decided once whatever the options' order. */
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
	int error;

	/* Nonblocking acceptance cannot stall already connected clients. */
	descriptor = accept4(server->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (descriptor < 0) {
		/* Transient readiness or signal changes leave the listening generation usable. */
		if (errno == EAGAIN ||
		    errno == EWOULDBLOCK ||
		    errno == EINTR)
			return 0;

		/* Per-process descriptor exhaustion is explicit and terminates this finite run. */
		error = errno;
		kwl_fatal("KWL FAILED site=accept errno=%d", error);
		return error;
	}

	/* Client state owns every subsequently received byte, fd and object. */
	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(descriptor);
		kwl_fatal("KWL FAILED site=accept-client errno=%d", ENOMEM);
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
		kwl_fatal("KWL FAILED site=accept-display errno=%d", ENOMEM);
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
	struct pollfd *descriptors;
	uint64_t started;
	uint64_t now;
	size_t count;
	size_t index;
	size_t wake_slot;
	size_t first_fence;
	size_t first_os;
	size_t os_count;
	size_t first_watched;
	unsigned fence;
	int retire_ready;
	uint64_t mark;
	uint64_t part;
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
		if (now == UINT64_MAX) {
			kwl_fatal("KWL FAILED site=clock errno=%d", EIO);
			return EIO;
		}

		/* The optional frame bound counts only completed hardware selections. */
		if (now - started >= server->timeout_ms ||
		    (server->max_frames != 0 && server->frame >= server->max_frames))
			break;

		/* The ticks before the poll are timed as one part of the pass (KWL PERF main). */
		part = kwl_cycles();

		/* Evdev nodes that appeared since the last scan join the seat. */
		input_scan_pace(server, now);

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

		/* A key held in a titlebar's field is pressed again when its repeat is due (titlebar-shell.c). */
		kwl_titlebar_tick(server, now);
		server->perf.tick_cycles += kwl_cycles() - part;

		/* Allocate exactly enough poll storage for the presently live client and device set. */
		count = 1;
		for (client = server->clients; client != NULL; client = client->next)
			count++;


		/*
		 * The pipe the compositor's threads wake the loop with follows the
		 * clients: the input thread's reports.  The devices
		 * themselves are the input thread's to poll.
		 */
		wake_slot = count;
		count++;

		/*
		 * The acquire fences of committed images are polled last.  A fatal
		 * client's are not: its commits are never taken, and a fence that is
		 * always readable (any fd a client sent) would wake the loop until the
		 * client is retired.
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

		/* Counts the OS entries after the input and fence descriptors, then the watched ones. */
		first_os = count;
		os_count = kwl_os_poll_count(server);
		count += os_count;
		first_watched = count;
		count += server->watched_count;

		/* Allocation failure leaves all live clients owned by service cleanup. */
		descriptors = calloc(count, sizeof(*descriptors));
		if (descriptors == NULL) {
			kwl_fatal("KWL FAILED site=poll-set count=%zu errno=%d", count, ENOMEM);
			return ENOMEM;
		}

		/* The parallel pointer array resolves readiness to this snapshot's generation. */
		clients = calloc(count, sizeof(*clients));
		if (clients == NULL) {
			free(descriptors);
			kwl_fatal("KWL FAILED site=poll-set count=%zu errno=%d", count, ENOMEM);
			return ENOMEM;
		}

		/* The listener first. */
		descriptors[0].fd = server->listener;
		descriptors[0].events = POLLIN;

		/* Each client's connection after the listener. */
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

			/* A fully flushed fatal connection can be retired immediately, one that is not when its time is up. */
			if (client->fatal && client->output_head == NULL)
				kwl_due(server, 0U);
			if (client->fatal && client->output_head != NULL)
				kwl_due(server, client->fatal_time + 2000U);

			/* Each slot and pointer now describe one live connection generation. */
			index++;
		}

		/* The threads' wake pipe. */
		descriptors[wake_slot].fd = server->wake[0];
		descriptors[wake_slot].events = POLLIN;

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

		/* Lets the OS populate its entries in this descriptor snapshot, and the watched descriptors after them. */
		kwl_os_poll_fill(server, descriptors + first_os);
		for (index = 0; index < server->watched_count; index++)
			descriptors[first_watched + index] = server->watched[index];

		/* The input thread computes the motions from now on with the settings and outputs as they are. */
		kwl_input_sync(server);

		/* The images' moves to their layout recorded in this pass go to the render thread before the loop waits. */
		kwl_compose_give_tasks(server);

		/*
		 * The wait ends with an event, or when work without one is due; a
		 * service with a bound ends at the bound.  A backend that reads some
		 * of its areas without waiting has them looked at soon.
		 */
		if (server->timeout_ms != UINT64_MAX)
			kwl_due(server, started + server->timeout_ms);
		if (!kl_backend_wakes(server->backend))
			kwl_due_soon(server);

		/* Without the timer thread the earliest timer is the loop's own deadline. */
		if (!server->timers.running)
			kwl_due(server, kwl_timers_next(&server->timers));

		/* A change to draw made since the scheduler ran is drawn at once (the scheduler waits for nothing else it). */
		if ((server->dirty || server->damaged) && !server->schedule_left)
			kwl_due(server, 0U);
		timeout = loop_timeout(server);

		/* Poll sees sockets only; typed image fds are consumed immediately during import. */
		mark = kwl_cycles();
		ready = poll(descriptors, count, timeout);
		server->perf.poll_cycles += kwl_cycles() - mark;
		mark = kwl_cycles();

		/* The pass's work comes from now on: its time, and the work without an event it leaves for later. */
		now = kwl_milliseconds();
		server->due_ms = UINT64_MAX;
		server->watched_count = 0;

		/* The pass's work starts now; its parts are told apart at its end if it runs long. */
		perf_pass_begin(server);
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
			kwl_fatal("KWL FAILED site=poll count=%zu errno=%d", count, error);
			return error;
		}

		/*
		 * The threads' wake pipe is emptied before any of their queues is
		 * taken this pass (the render thread's below, the input thread's
		 * after), so a message queued later wakes the next poll.
		 */
		if (descriptors[wake_slot].revents != 0)
			wake_drain(server);

		/* Accept at most one client per pass so existing streams retain service opportunity. */
		error = 0;
		if ((descriptors[0].revents & POLLIN) != 0)
			error = accept_client(server);

		/* A failed listener cannot serve this generation any further. */
		if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			kwl_fatal("KWL FAILED site=listener revents=0x%x errno=%d", (unsigned)descriptors[0].revents, EIO);
			error = EIO;
		}

		/* Device authority changes before the old snapshot can read revoked input or complete a frame. */
		kwl_os_poll_done(server, descriptors + first_os);

		/* The input devices the device I/O thread opened are offered to the seat (io.c). */
		kwl_io_take(server);

		/*
		 * A finished frame releases its buffers and sends its callbacks, and
		 * so does a present of the game mode that returned on the present
		 * thread.
		 */
		part = kwl_cycles();
		kwl_scanout_take(server);
		kwl_compose_poll(server);
		kwl_display_take(server);
		kwl_glass_glyphs_poll(server);
		kwl_glass_blur_take(server);
		kwl_keyboard_hand_take(server);
		server->perf.frame_done_cycles += kwl_cycles() - part;

		/*
		 * The hotplug thread's notes, then the follow of the displays they,
		 * the frames' results and the display thread's ask for (output-switch.c).
		 */
		kwl_output_hotplug_take(server);

		/*
		 * The input thread's reports are applied before clients are flushed,
		 * so they leave in this pass, in the order the devices made them.
		 */
		part = kwl_cycles();
		kwl_input_drain(server);
		server->perf.input_cycles += kwl_cycles() - part;

		/* The seat hears what the input thread has let go of, and the end of its pause. */
		kwl_backend_seat_tick(server);

		/* The timers due now fire, after every thread's results and before the clients' requests (threads.c). */
		(void)kwl_timers_take(&server->timers, now, server);

		/* Process only the clients captured by this poll snapshot. */
		part = kwl_cycles();
		for (index = 1; index < wake_slot; index++) {
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

			/* Withdrawal releases unread rights and scanout ownership before fd reuse (a log line a test reads). */
			if (remove) {
				printf("KWL CLIENT gone client=%llu reason=%s\n", (unsigned long long)client->number, why);
				kwl_client_destroy(client);
			}
		}

		/* The clients' requests and events are timed as one part (KWL PERF main). */
		server->perf.client_cycles += kwl_cycles() - part;

		/* The clipboard's history reads what a source has written so far (clipboard.c; the pass is at most 10 ms). */
		kwl_clipboard_poll(server);

		/* A drag let go before its target answered is decided once it has (data.c). */
		kwl_data_tick(server);

		/* A test image's screen capture takes its requests (shot.c). */
		kwl_shot_tick(server);

		/* The snapshot contains no ownership references beyond this iteration. */
		free(clients);
		free(descriptors);
		if (error != 0)
			return error;

		/* The scheduler takes the committed images and draws a frame when one is due. */
		part = kwl_cycles();
		kwl_schedule(server);
		server->perf.schedule_cycles += kwl_cycles() - part;
		server->schedule_left = server->dirty || server->damaged;

		/*
		 * One gone client's buffer released after the frame, only while the
		 * compositor is idle, so that neither a frame nor the input waits
		 * for a release (T1-392).
		 */
		retire_ready = kwl_retire_ready(server, kwl_milliseconds());
		if (retire_ready)
			(void)kwl_retire_tick(server);

		/* The next release: the next pass when it may go now, the timer when it waits for a time, else an event. */
		kwl_retire_pace(server);

		/*
		 * The presentation queued frame callbacks and buffer releases; they
		 * leave now, not after the next poll, which would otherwise hold a
		 * FIFO client for up to the poll timeout every frame.
		 */
		part = kwl_cycles();
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

		/* The last flush's part (KWL PERF main). */
		server->perf.flush_cycles += kwl_cycles() - part;

		/* The pass's work, kept for its longest and 99th percentile, and told part by part when it ran long. */
		part = kwl_cycles() - mark;
		server->perf.work_cycles += part;
		perf_pass_end(server, part);

		/* The window's lines every five seconds. */
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

	/* The gone clients' buffers still waiting are released before the device. */
	kwl_retire_flush(server);

	/* The swapchain and the Vulkan device go after every buffer's image. */
	kwl_compose_close(server);

	/* The input thread lets go of every device; then, no client remaining to hear from the seat, they close quietly. */
	kwl_input_threads_stop(server);
	kwl_input_cleanup(server);

	/* The handwriting's recognition pool ends (keyboard.c). */
	kwl_keyboard_hand_stop();

	/* The device I/O thread ends; the devices it opened and the closes it was given are closed (io.c). */
	kwl_io_stop(server);

	/* The session's volume is kept for the next login (volume.c). */
	kwl_volume_keep(server, "end");

	/* The file I/O thread writes everything given (displays.conf, the Log Out's settings), then ends (io.c). */
	kwl_io_file_finish(server);

	/* The session's settings are kept for the next login (settings.c). */
	kwl_settings_close(server);

	/* The system extension's threads end before the backend closes (system.c), with the Bluetooth's and the phone's connections. */
	kwl_system_close(server);

	/* The daemons' connections the bar follows, each with its thread (network.c, media.c, volume.c). */
	kwl_network_close();
	kwl_media_close();
	kwl_volume_close(server);

	/* Returns the OS resources after input and display cleanup. */
	kwl_os_close(server);

	/* A screen the lid put out is lit for whoever comes next, and the backlight closed (backend-host.c). */
	kwl_lid_screen_restore(server);
	kwl_backlight_close(server);

	/* Closes the operating system's side last, after everything that used it. */
	kl_backend_close(server->backend);
	server->backend = NULL;

	/* No thread writes to the wake pipe any more. */
	kwl_wake_close(server);

	/* Succeeded: the service retains no listener, client or Vulkan object. */
	return;
}

/*
 * Reads the processor's cycle counter (the millisecond clock has only tick
 * resolution).  On arm64 (the Linux arm64 package) the virtual
 * counter of the generic timer; on riscv64 the monotonic clock in
 * nanoseconds.
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
#elif defined(__riscv)
	struct timespec now;

	/* The monotonic clock in nanoseconds (the cycle counter is not readable from user mode on every hart). */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the count. */
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
#else
	uint32_t low;
	uint32_t high;

	/* The time stamp counter's two halves. */
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));

	/* Succeeded: the counter as one value. */
	return ((uint64_t)high << 32) | low;
#endif
}

/*
 * Counts one piece of the main thread's Vulkan work outside a walk for
 * KWL PERF pass's vk_outside_max_us (the design's 6.4).
 */
void
kwl_perf_vk_outside(
	struct kwl_server *server,
	uint64_t cycles)
{
	/* The pass's share, for its KWL SLOW_PASS line. */
	server->perf.pass_vk_outside_cycles += cycles;

	/* The longest of the window. */
	if (cycles > server->perf.vk_outside_max_cycles)
		server->perf.vk_outside_max_cycles = cycles;
}

/*
 * Lowers the time the event loop's wait ends at the latest to at_ms
 * (kwl_milliseconds' clock; 0 or a time past: at once).  A part of the
 * compositor whose work comes with time, not with an event, calls it on
 * every pass while the work waits.
 */
void
kwl_due(
	struct kwl_server *server,
	uint64_t at_ms)
{
	/* The earliest wins. */
	if (at_ms < server->due_ms)
		server->due_ms = at_ms;
}

/*
 * Asks for the next pass within KWL_DUE_SOON_MS: for work that follows
 * time closely (a state looked at again and again while it lasts), as the
 * loop's passes always came before.
 */
void
kwl_due_soon(
	struct kwl_server *server)
{
	/* One step of the old pace. */
	kwl_due(server, kwl_milliseconds() + KWL_DUE_SOON_MS);
}

/*
 * Asks for a pass just after the wall clock's next minute (a clock shown
 * turns over then): the time to it is taken from the real-time clock and
 * counted on kwl_milliseconds' clock.
 */
void
kwl_due_next_minute(
	struct kwl_server *server)
{
	struct timespec wall;
	uint64_t into;
	uint64_t left;

	/* The milliseconds into the present minute. */
	if (clock_gettime(CLOCK_REALTIME, &wall) != 0) {
		kwl_due(server, kwl_milliseconds() + 1000U);
		return;
	}
	into = (uint64_t)(wall.tv_sec % 60) * 1000U + (uint64_t)wall.tv_nsec / 1000000U;

	/* The rest of it, and a little more so that the minute has turned. */
	left = 60000U - into;
	kwl_due(server, kwl_milliseconds() + left + KWL_MINUTE_LATE_MS);
}

/*
 * Adds a descriptor a part reads without waiting on every pass to the
 * event loop's next wait (for events, POLLIN and POLLOUT), so that what
 * comes on it ends the wait; with no room left the loop comes back soon.
 */
void
kwl_watch(
	struct kwl_server *server,
	int descriptor,
	short events)
{
	/* Nothing to wait for. */
	if (descriptor < 0 || events == 0)
		return;

	/* No room: the old pace instead. */
	if (server->watched_count >= KWL_WATCH_MAX) {
		kwl_due_soon(server);
		return;
	}

	/* The descriptor joins the next wait. */
	server->watched[server->watched_count].fd = descriptor;
	server->watched[server->watched_count].events = events;
	server->watched[server->watched_count].revents = 0;
	server->watched_count++;
}

/*
 * Works out the event loop's wait (milliseconds for poll, -1 for none)
 * from the time its work without an event is due, rounded up so that the
 * work is due when the wait ends.
 */
static int
loop_timeout(
	struct kwl_server *server)
{
	uint64_t now;

	/* Nothing due: the wait lasts until an event. */
	if (server->due_ms == UINT64_MAX)
		return -1;

	/* Due already: no wait; later: until then. */
	now = kwl_milliseconds();
	if (server->due_ms <= now)
		return 0;
	if (server->due_ms - now > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)(server->due_ms - now);
}

/*
 * Scans the evdev nodes when the seat's devices may have changed: when a
 * scan was asked (a device's event, the seat's resume, a scan that came
 * while a finding was under way), and quickly for a while after a device's
 * event, because sessiond gives the new node to the seat's user on the same
 * event and the compositor cannot open it before.  Only a backend
 * that does not tell the devices' comings has them looked for every
 * KWL_INPUT_SCAN_MS as well.
 */
static void
input_scan_pace(
	struct kwl_server *server,
	uint64_t now)
{
	int told;

	/* A paused seat reads no device; its resume asks for the scan. */
	if (server->os_paused != 0)
		return;

	/*
	 * An asked scan goes now, or the quick scan after a device's event once
	 * KWL_INPUT_SETTLE_SCAN_MS passed, or the backend's lack of events
	 * makes the ordinary one due.
	 */
	told = kl_backend_input_events(server->backend);
	if (server->input_scan_time == 0U) {
		kwl_input_scan(server);
	} else if (now < server->input_settle_until) {
		if (now - server->input_scan_time >= KWL_INPUT_SETTLE_SCAN_MS)
			kwl_input_scan(server);
	} else if (!told) {
		if (now - server->input_scan_time >= KWL_INPUT_SCAN_MS)
			kwl_input_scan(server);
	}

	/* A finding under way keeps the scan asked: its end wakes the loop, not the clock. */
	if (server->input_scan_time == 0U)
		return;

	/* The quick scans' next while the event's time lasts. */
	if (now < server->input_settle_until) {
		kwl_due(server, server->input_scan_time + KWL_INPUT_SETTLE_SCAN_MS);
		return;
	}

	/* The ordinary scan's next, for a backend that does not tell the devices' comings. */
	if (!told)
		kwl_due(server, server->input_scan_time + KWL_INPUT_SCAN_MS);
}

/* Prints the loop's timing every five seconds and starts a new window. */
static void
kwl_perf_report(
	struct kwl_server *server,
	uint64_t now)
{
	struct kwl_cursor_stats stats;
	struct kwl_perf *perf;
	uint64_t cycles;
	uint64_t main_parts;
	double main_other;
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

	/* The cursor thread's counts of the window. */
	memset(&stats, 0, sizeof(stats));
	if (server->threads_prepared)
		kwl_cursor_box_take_stats(&server->cursor_box, &stats);
	perf->cursor_presents = stats.presents;
	perf->cursor_deferred = stats.held;
	perf->cursor_fence_cycles = stats.fence_cycles;
	perf->cursor_acquire_cycles = stats.acquire_cycles;
	perf->cursor_copy_cycles = stats.copy_cycles;
	perf->cursor_queue_present_cycles = stats.present_cycles;
	perf->cursor_queue_present_max_cycles = stats.present_max_cycles;

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

	/* The render thread's steps of a frame, averages then longest, in milliseconds. */
	if (perf->rec_frames != 0) {
		printf("KWL PERF rec frames=%u copied=%u begin=%.2f encode=%.2f end=%.2f submit=%.2f present=%.2f | max end=%.2f submit=%.2f present=%.2f\n",
		    perf->rec_frames,
		    perf->rec_copied,
		    (double)perf->rec_begin_cycles / per_ms / (double)perf->rec_frames,
		    (double)perf->rec_encode_cycles / per_ms / (double)perf->rec_frames,
		    (double)perf->rec_end_cycles / per_ms / (double)perf->rec_frames,
		    (double)perf->rec_submit_cycles / per_ms / (double)perf->rec_frames,
		    (double)perf->rec_present_cycles / per_ms / (double)perf->rec_frames,
		    (double)perf->rec_end_max_cycles / per_ms,
		    (double)perf->rec_submit_max_cycles / per_ms,
		    (double)perf->rec_present_max_cycles / per_ms);
	}

	/*
	 * The pass's parts in milliseconds of the window: the
	 * ticks before the poll (outside work), then the parts of work, and
	 * the scheduler's own parts.  other is the work none of them covers
	 * (the poll's descriptors, accept, the OS, the clipboard and the rest).
	 */
	main_parts = perf->input_cycles + perf->client_cycles + perf->frame_done_cycles + perf->schedule_cycles + perf->flush_cycles;
	main_other = 0.0;
	if (perf->work_cycles > main_parts)
		main_other = (double)(perf->work_cycles - main_parts) / per_ms;
	printf("KWL PERF main ticks=%.1f input=%.1f clients=%.1f frame_done=%.1f schedule=%.1f flush=%.1f other=%.1f | schedule ticks=%.1f adopt=%.1f shm=%.1f cursor=%.1f layers=%.1f draw=%.1f | cursor presents=%u held=%u fence=%.1f acquire=%.1f copy=%.1f present=%.1f present_max=%.2f\n",
	    (double)perf->tick_cycles / per_ms,
	    (double)perf->input_cycles / per_ms,
	    (double)perf->client_cycles / per_ms,
	    (double)perf->frame_done_cycles / per_ms,
	    (double)perf->schedule_cycles / per_ms,
	    (double)perf->flush_cycles / per_ms,
	    main_other,
	    (double)perf->schedule_tick_cycles / per_ms,
	    (double)perf->adopt_cycles / per_ms,
	    (double)perf->shm_cycles / per_ms,
	    (double)perf->cursor_cycles / per_ms,
	    (double)perf->layers_cycles / per_ms,
	    (double)perf->draw_cycles / per_ms,
	    perf->cursor_presents,
	    perf->cursor_deferred,
	    (double)perf->cursor_fence_cycles / per_ms,
	    (double)perf->cursor_acquire_cycles / per_ms,
	    (double)perf->cursor_copy_cycles / per_ms,
	    (double)perf->cursor_queue_present_cycles / per_ms,
	    (double)perf->cursor_queue_present_max_cycles / per_ms);

	/* The passes' longest work, and the cursor's box. */
	perf_pass_report(perf, per_ms);
	perf_box_report(perf, &stats);

	/* The drawings' walks and replays. */
	perf_walk_report(perf, per_ms);

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
 * Prints the window's drawings (KWL PERF walk): the walk that
 * wrote each frame's display list, by how many things the frame drew
 * (fewer than 100, 100 to 299, and 300 or more as App Home draws), the
 * replay that recorded the frames' lists, and the walk and the replay of
 * the layers' quiet pictures; each as how many, the median, the 90th
 * percentile and the longest, in milliseconds.
 */
static void
perf_walk_report(
	const struct kwl_perf *perf,
	double per_ms)
{
	/* Nothing was drawn in the window. */
	if (perf->walk_count == 0U && perf->walk_dropped == 0U)
		return;

	/* The drawings kept and those past the window's room. */
	printf("KWL PERF walk drawings=%u dropped=%u", perf->walk_count, perf->walk_dropped);

	/* The frames' walks by the things they drew, then their replays. */
	perf_walk_class(perf, per_ms, "small", 0U, 99U, 0U, 0U);
	perf_walk_class(perf, per_ms, "middle", 100U, 299U, 0U, 0U);
	perf_walk_class(perf, per_ms, "large", 300U, UINT32_MAX, 0U, 0U);
	perf_walk_class(perf, per_ms, "replay", 0U, UINT32_MAX, 0U, 1U);

	/* The quiet pictures' walks and replays, and the line's end. */
	perf_walk_class(perf, per_ms, "quiet", 0U, UINT32_MAX, 1U, 0U);
	perf_walk_class(perf, per_ms, "quiet_replay", 0U, UINT32_MAX, 1U, 1U);
	printf("\n");
}

/*
 * Prints one class of the window's drawings: those that drew from lowest
 * to highest things and presented nothing or not (quiet), by their walks'
 * or their replays' times.
 */
static void
perf_walk_class(
	const struct kwl_perf *perf,
	double per_ms,
	const char *name,
	uint32_t lowest,
	uint32_t highest,
	unsigned quiet,
	unsigned replay)
{
	uint64_t times[KWL_PERF_WALKS];
	uint32_t count;
	uint32_t index;

	/* The times of the drawings of the class. */
	count = 0U;
	for (index = 0U; index < perf->walk_count; index++) {
		/* Another class's drawing. */
		if (perf->walk_quiet[index] != quiet)
			continue;
		if (perf->walk_things[index] < lowest || perf->walk_things[index] > highest)
			continue;

		/* Its walk's or its replay's time. */
		times[count] = perf->walk_cycles[index];
		if (replay)
			times[count] = perf->replay_cycles[index];
		count++;
	}

	/* A class with no drawing is shown as empty. */
	if (count == 0U) {
		printf(" | %s n=0", name);
		return;
	}

	/* The median, the 90th percentile (the nearest lower rank) and the longest. */
	qsort(times, count, sizeof(times[0]), perf_cycles_order);
	printf(" | %s n=%u p50=%.2f p90=%.2f max=%.2f",
	    name,
	    count,
	    (double)times[(count - 1U) * 50U / 100U] / per_ms,
	    (double)times[(count - 1U) * 90U / 100U] / per_ms,
	    (double)times[count - 1U] / per_ms);
}

/* Orders two cycle counts from the smaller (qsort's comparison). */
static int
perf_cycles_order(
	const void *left,
	const void *right)
{
	uint64_t first;
	uint64_t second;

	/* The two counts. */
	memcpy(&first, left, sizeof(first));
	memcpy(&second, right, sizeof(second));

	/* Smaller first, equal ones as they are. */
	if (first < second)
		return -1;
	if (first > second)
		return 1;
	return 0;
}

/*
 * Reports how long one step of the start took, and when it ended (the
 * diagnostic line KWL STARTUP); returns the time it ended.
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

/*
 * Empties the pipe the compositor's threads wake the event loop with; what
 * they queued is taken after, so a later queue wakes the next poll.
 */
static void
wake_drain(
	struct kwl_server *server)
{
	unsigned char bytes[64];
	ssize_t got;

	/* Every byte waiting; the pipe does not block. */
	do {
		got = read(server->wake[0], bytes, sizeof(bytes));
	} while (got > 0 || (got < 0 && errno == EINTR));
}

/*
 * Prints the window's passes (KWL PERF pass): the longest
 * pass's work and the 99th percentile, the longest walk of a drawing, and
 * the longest Vulkan work of the main thread outside a walk, in
 * microseconds.  The passes' array
 * is sorted in place; the window ends after the report.
 */
static void
perf_pass_report(
	struct kwl_perf *perf,
	double per_ms)
{
	uint64_t p99;
	uint64_t walk_max;
	uint32_t index;

	/* No pass was counted. */
	if (perf->pass_count == 0U)
		return;

	/* The 99th percentile of the passes kept. */
	qsort(perf->pass_cycles, perf->pass_count, sizeof(perf->pass_cycles[0]), perf_cycles_order);
	index = (uint32_t)(((uint64_t)perf->pass_count * 99U) / 100U);
	if (index >= perf->pass_count)
		index = perf->pass_count - 1U;
	p99 = perf->pass_cycles[index];

	/* The longest walk of the window's drawings. */
	walk_max = 0U;
	for (index = 0U; index < perf->walk_count; index++) {
		if (perf->walk_cycles[index] > walk_max)
			walk_max = perf->walk_cycles[index];
	}

	/* In microseconds. */
	printf("KWL PERF pass passes=%u pass_p99_us=%.0f pass_max_us=%.0f walk_max_us=%.0f vk_outside_max_us=%.0f\n",
	    perf->pass_count,
	    (double)p99 / per_ms * 1000.0,
	    (double)perf->pass_max_cycles / per_ms * 1000.0,
	    (double)walk_max / per_ms * 1000.0,
	    (double)perf->vk_outside_max_cycles / per_ms * 1000.0);
}

/* Notes the parts' sums and the clock as a pass's work begins. */
static void
perf_pass_begin(
	struct kwl_server *server)
{
	/* The sums the pass's own parts are measured from. */
	perf_parts_take(&server->perf, &server->perf.pass_start);
	server->perf.pass_start_us = kwl_microseconds();

	/* The pass has done no Vulkan work outside a walk and no walk yet. */
	server->perf.pass_vk_outside_cycles = 0U;
	server->perf.pass_walk_cycles = 0U;
}

/*
 * Keeps one pass's work for the window's longest and 99th percentile, and
 * prints a KWL SLOW_PASS line, at most KWL_PERF_SLOW_PASSES a window, for a
 * pass over two refreshes: its time and when it ended, and its parts in
 * microseconds (the parts of KWL PERF main, the scheduler's own, and its
 * walks and Vulkan work outside a walk), so a long pass names its cause.
 */
static void
perf_pass_end(
	struct kwl_server *server,
	uint64_t cycles)
{
	struct kwl_perf *perf;
	struct kwl_perf_parts now;
	struct kwl_perf_parts *start;
	uint64_t ended_us;
	uint64_t pass_us;
	uint64_t parts;
	uint64_t other;

	/* The pass's work for the window. */
	perf = &server->perf;
	if (cycles > perf->pass_max_cycles)
		perf->pass_max_cycles = cycles;
	if (perf->pass_count < KWL_PERF_PASSES) {
		perf->pass_cycles[perf->pass_count] = cycles;
		perf->pass_count++;
	}

	/* A pass within two refreshes, or one past the window's lines, is not told apart. */
	ended_us = kwl_microseconds();
	pass_us = 0U;
	if (ended_us > perf->pass_start_us)
		pass_us = ended_us - perf->pass_start_us;
	if (pass_us < 33300U || perf->slow_pass_lines >= KWL_PERF_SLOW_PASSES)
		return;
	perf->slow_pass_lines++;

	/* The pass's own share of each part, and the rest of its work. */
	perf_parts_take(perf, &now);
	start = &perf->pass_start;
	parts = now.input_cycles - start->input_cycles;
	parts += now.client_cycles - start->client_cycles;
	parts += now.frame_done_cycles - start->frame_done_cycles;
	parts += now.schedule_cycles - start->schedule_cycles;
	parts += now.flush_cycles - start->flush_cycles;
	other = 0U;
	if (cycles > parts)
		other = cycles - parts;

	/* The line, in microseconds of the pass's clock. */
	printf("KWL SLOW_PASS us=%llu at_ms=%llu input_us=%llu clients_us=%llu frame_done_us=%llu schedule_us=%llu flush_us=%llu other_us=%llu | adopt_us=%llu shm_us=%llu cursor_us=%llu layers_us=%llu draw_us=%llu | walk_us=%llu vk_outside_us=%llu frame=%llu\n",
	    (unsigned long long)pass_us,
	    (unsigned long long)(ended_us / 1000U),
	    (unsigned long long)perf_part_us(now.input_cycles - start->input_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.client_cycles - start->client_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.frame_done_cycles - start->frame_done_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.schedule_cycles - start->schedule_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.flush_cycles - start->flush_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(other, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.adopt_cycles - start->adopt_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.shm_cycles - start->shm_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.cursor_cycles - start->cursor_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.layers_cycles - start->layers_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(now.draw_cycles - start->draw_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(perf->pass_walk_cycles, cycles, pass_us),
	    (unsigned long long)perf_part_us(perf->pass_vk_outside_cycles, cycles, pass_us),
	    (unsigned long long)server->frame);
}

/* Copies the window's sums of the event loop's parts. */
static void
perf_parts_take(
	const struct kwl_perf *perf,
	struct kwl_perf_parts *parts)
{
	/* The parts of the work after the poll. */
	parts->input_cycles = perf->input_cycles;
	parts->client_cycles = perf->client_cycles;
	parts->frame_done_cycles = perf->frame_done_cycles;
	parts->schedule_cycles = perf->schedule_cycles;
	parts->flush_cycles = perf->flush_cycles;

	/* The scheduler's own parts. */
	parts->adopt_cycles = perf->adopt_cycles;
	parts->shm_cycles = perf->shm_cycles;
	parts->cursor_cycles = perf->cursor_cycles;
	parts->layers_cycles = perf->layers_cycles;
	parts->draw_cycles = perf->draw_cycles;
}

/* Scales a part of a pass from cycles to microseconds by the pass's own length on both clocks. */
static uint64_t
perf_part_us(
	uint64_t cycles,
	uint64_t pass_cycles,
	uint64_t pass_us)
{
	/* A pass of no cycles has no parts. */
	if (pass_cycles == 0U)
		return 0U;

	/* The part's share of the pass's microseconds. */
	return (uint64_t)((double)cycles * (double)pass_us / (double)pass_cycles);
}

/*
 * Prints the cursor's box (KWL PERF cursor):
 * how many writes the main thread applied, their lag from the report's
 * evdev time (the 99th percentile, the longest and how many over a
 * refresh), the lag beyond the reports' spacing, and the gap between the
 * writes while the pointer moved, in microseconds; then the cursor
 * thread's presents, the moves it held, its copies, the longest time
 * between two presents while the pointer moved and the longest present.
 * The arrays are sorted in place.
 */
static void
perf_box_report(
	struct kwl_perf *perf,
	const struct kwl_cursor_stats *stats)
{
	uint32_t lag_p99;
	uint32_t lag_max;
	uint32_t excess_p99;
	uint32_t excess_max;
	uint32_t gap_p99;
	uint32_t gap_max;

	/* No write and no present in the window. */
	if (perf->box_count == 0U && perf->box_dropped == 0U && stats->presents == 0U)
		return;

	/* Each measure's 99th percentile and longest. */
	lag_p99 = perf_percentile(perf->box_lag_us, perf->box_count, 99U, &lag_max);
	excess_p99 = perf_percentile(perf->box_excess_us, perf->box_count, 99U, &excess_max);
	gap_p99 = perf_percentile(perf->box_gap_us, perf->box_gap_count, 99U, &gap_max);

	/* One line. */
	printf("KWL PERF cursor box_writes=%u dropped=%u box_lag_p99_us=%u box_lag_max_us=%u box_lag_over=%u box_excess_p99_us=%u box_excess_max_us=%u box_gap_p99_us=%u box_gap_max_us=%u presents=%u held=%u copies=%u present_gap_max_us=%llu present_max_us=%llu\n",
	    perf->box_count,
	    perf->box_dropped,
	    lag_p99,
	    lag_max,
	    perf->box_over,
	    excess_p99,
	    excess_max,
	    gap_p99,
	    gap_max,
	    stats->presents,
	    stats->held,
	    stats->copies,
	    (unsigned long long)stats->present_gap_max_us,
	    (unsigned long long)stats->present_max_us);
}

/* Sorts values in place and gives a percentile of them and their largest (0 for none). */
static uint32_t
perf_percentile(
	uint32_t *values,
	uint32_t count,
	unsigned percent,
	uint32_t *largest)
{
	uint32_t index;

	/* None. */
	*largest = 0U;
	if (count == 0U)
		return 0U;

	/* In order, the largest last. */
	qsort(values, count, sizeof(values[0]), perf_value_order);
	*largest = values[count - 1U];

	/* Succeeded: the value at the percentile. */
	index = (uint32_t)(((uint64_t)count * percent) / 100U);
	if (index >= count)
		index = count - 1U;
	return values[index];
}

/* Orders two 32-bit values for qsort, the smaller first. */
static int
perf_value_order(
	const void *left,
	const void *right)
{
	uint32_t first;
	uint32_t second;

	/* The two values. */
	first = *(const uint32_t *)left;
	second = *(const uint32_t *)right;

	/* Smaller, larger or the same. */
	if (first < second)
		return -1;
	if (first > second)
		return 1;
	return 0;
}

/*
 * Tells the person who started the compositor why it has no GPU, when the
 * system knows: a GPU the kernel holds for root's start, a GPU node
 * that belongs to the graphical login's user, or one that only root and
 * the video group may open.
 */
static void
compose_unavailable_reason(
	void)
{
	char reason[256];
	int error;

	/* Asks the system; one that knows no reason has nothing more to say. */
	error = kl_backend_gpu_unavailable_reason(reason, sizeof(reason));
	if (error != 0)
		return;

	/* The log's line, and the sentence on the terminal the desktop was started from. */
	printf("KWL COMPOSE reason=\"%s\"\n", reason);
	fprintf(stderr, "wayland: the desktop cannot start: %s\n", reason);
}
