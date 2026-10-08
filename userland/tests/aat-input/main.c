/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * aat-input: the mouse and the keyboard of the Agent Acceptance Test
 * (ws173-p001).  An agent drives a machine over SSH with it; the devices
 * are the test kernel's /dev/input-inject (CONFIG_INPUT_TEST_INJECT), so the
 * compositor sees them like a USB mouse and keyboard.  Test images only;
 * root only.
 *
 *   aat-input start [--width W --height H]
 *           starts a server in the background that holds four devices: a
 *           relative mouse, an absolute pointer over W x H output pixels
 *           (default 1920 x 1200), a keyboard and a touch screen over the
 *           same pixels (ws190-p002), and listens on
 *           /run/aat-input.sock (root's, 0600).  Prints "AAT-INPUT ready".
 *   aat-input stop
 *   aat-input COMMAND [ARGUMENT...]
 *           one command to the server; prints "ok" (status 0) or
 *           "error WHY" (status 1).
 *
 * The commands:
 *   move-to X Y                       the absolute pointer to pixel (X, Y)
 *   move DX DY                        the relative mouse
 *   click [left|right|middle] [X Y]   at (X, Y) when given
 *   double-click [BUTTON] [X Y]
 *   down BUTTON | up BUTTON
 *   drag X1 Y1 X2 Y2 [STEPS]          the left button, absolute, STEPS moves (20)
 *   wheel N | hwheel N                notches, N > 0 up or right
 *   key NAME[+NAME...]                pressed in order, released in reverse
 *   key-down NAME | key-up NAME
 *   type TEXT                         US layout, ASCII; Shift added as needed
 *   sleep MS
 *
 * The touch screen (ws190-p002), its fingers in output pixels:
 *   tap X Y                           one finger touches and lifts
 *   double-tap X Y                    two taps, AAT_DOUBLE_TAP_MS apart
 *   touch-drag X1 Y1 X2 Y2 [STEPS]    one finger, STEPS moves (20), then lifts
 *   touch-down ID X Y | touch-move ID X Y | touch-up ID
 *                                     a finger (ID 0..9) held, moved and lifted
 *                                     by itself (a long press is touch-down,
 *                                     sleep and touch-up)
 *
 * A name is the evdev key's name in lower case (a, enter, leftctrl, f5,
 * ...); ctrl, alt, shift, super and meta name the left ones.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <uapi/input-inject.h>
#include <uapi/input.h>

/* The server's socket, the injector, and the longest command. */
#define AAT_SOCKET		"/run/aat-input.sock"
#define AAT_INJECT		"/dev/input-inject"
#define AAT_LINE_MAX		4096U

/* The default area of the absolute pointer (the Latitude 5330's panel). */
#define AAT_DEFAULT_WIDTH	1920
#define AAT_DEFAULT_HEIGHT	1200

/* The pauses: a button's press, between keys, between typed characters, and the compositor's rescan. */
#define AAT_CLICK_MS		30U
#define AAT_KEY_MS		20U
#define AAT_TYPE_MS		15U
#define AAT_SETTLE_MS		2000U

/* A finger's tap: how long it touches, and the pause between the two taps of a double tap (within libkeiland's 400 ms). */
#define AAT_TAP_MS		40U
#define AAT_DOUBLE_TAP_MS	120U

/* The fingers the touch screen holds (its identifiers 0..AAT_FINGERS - 1), and how many go in one report. */
#define AAT_FINGERS		10U
#define AAT_TOUCH_REPORT	2U

/* The most keys of one combination, and of one write. */
#define AAT_COMBO_MAX		8U
#define AAT_EVENTS_MAX		16U

/* A key's name and its code. */
struct aat_key {
	const char *name;
	uint16_t code;
};

/*
 * One finger of the touch screen: whether it touches, and where (output
 * pixels).  The server keeps every finger between commands, since each
 * frame of the screen carries all the fingers that touch.
 */
struct aat_finger {
	int touching;
	int x;
	int y;
};

/* The devices the server holds, the absolute pointer's area, and the touch screen's fingers. */
struct aat_server {
	int mouse;
	int pointer;
	int keyboard;
	int touch;
	int width;
	int height;
	struct aat_finger fingers[AAT_FINGERS];
};

/* Every key's name, the aliases last. */
static const struct aat_key aat_keys[] = {
	{ "esc", 1 }, { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 }, { "6", 7 }, { "7", 8 },
	{ "8", 9 }, { "9", 10 }, { "0", 11 }, { "minus", 12 }, { "equal", 13 }, { "backspace", 14 }, { "tab", 15 },
	{ "q", 16 }, { "w", 17 }, { "e", 18 }, { "r", 19 }, { "t", 20 }, { "y", 21 }, { "u", 22 }, { "i", 23 },
	{ "o", 24 }, { "p", 25 }, { "leftbrace", 26 }, { "rightbrace", 27 }, { "enter", 28 }, { "leftctrl", 29 },
	{ "a", 30 }, { "s", 31 }, { "d", 32 }, { "f", 33 }, { "g", 34 }, { "h", 35 }, { "j", 36 }, { "k", 37 },
	{ "l", 38 }, { "semicolon", 39 }, { "apostrophe", 40 }, { "grave", 41 }, { "leftshift", 42 },
	{ "backslash", 43 }, { "z", 44 }, { "x", 45 }, { "c", 46 }, { "v", 47 }, { "b", 48 }, { "n", 49 },
	{ "m", 50 }, { "comma", 51 }, { "dot", 52 }, { "slash", 53 }, { "rightshift", 54 }, { "leftalt", 56 },
	{ "space", 57 }, { "capslock", 58 }, { "f1", 59 }, { "f2", 60 }, { "f3", 61 }, { "f4", 62 }, { "f5", 63 },
	{ "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 }, { "f10", 68 }, { "f11", 87 }, { "f12", 88 },
	{ "rightctrl", 97 }, { "sysrq", 99 }, { "rightalt", 100 }, { "home", 102 }, { "up", 103 }, { "pageup", 104 },
	{ "left", 105 }, { "right", 106 }, { "end", 107 }, { "down", 108 }, { "pagedown", 109 }, { "insert", 110 },
	{ "delete", 111 }, { "mute", 113 }, { "volumedown", 114 }, { "volumeup", 115 }, { "leftmeta", 125 },
	{ "rightmeta", 126 }, { "compose", 127 }, { "f13", 183 }, { "f14", 184 }, { "f15", 185 }, { "f16", 186 },
	{ "f17", 187 }, { "f18", 188 }, { "f19", 189 }, { "f20", 190 }, { "f21", 191 }, { "f22", 192 },
	{ "f23", 193 }, { "f24", 194 }, { "brightnessdown", 224 }, { "brightnessup", 225 },
	{ "ctrl", 29 }, { "alt", 56 }, { "shift", 42 }, { "super", 125 }, { "meta", 125 },
};

/* The characters of the US layout without Shift (index by key code) and with it; 0 for none. */
static const char aat_plain[58] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, '\t',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};
static const char aat_shifted[58] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, 0
};

static int aat_start(int width, int height);
static int aat_stop(void);
static int aat_send(int argc, char **argv);
static int aat_serve(struct aat_server *server, int listener, int ready);
static int aat_declare(uint32_t kind, int x_max, int y_max);
static int aat_command(struct aat_server *server, char *line, char *why, size_t why_size);
static int aat_emit(int device, const struct input_event *events, unsigned count);
static void aat_event(struct input_event *event, uint16_t type, uint16_t code, int32_t value);
static int aat_button_code(const char *name, uint16_t *code);
static int aat_key_code(const char *name, uint16_t *code);
static int aat_button(struct aat_server *server, uint16_t code, int value);
static int aat_move_to(struct aat_server *server, int x, int y);
static int aat_key(struct aat_server *server, uint16_t code, int value);
static int aat_type(struct aat_server *server, const char *text);
static void aat_sleep_ms(unsigned milliseconds);
static int aat_connect(void);
static int aat_declare_touch(int x_max, int y_max);
static int aat_touch_command(struct aat_server *server, char **words, unsigned count, char *why, size_t why_size, int *matched);
static int aat_touch_tap(struct aat_server *server, int x, int y);
static int aat_touch_drag(struct aat_server *server, int x1, int y1, int x2, int y2, int steps);
static int aat_finger(struct aat_server *server, unsigned id, int touching, int x, int y);
static int aat_touch_frame(struct aat_server *server, unsigned lifting);
static int aat_finger_id(const char *word, unsigned *id);

/* Starts the server, stops it, or sends it one command. */
int
main(
	int argc,
	char **argv)
{
	int width;
	int height;
	int index;
	int status;

	/* What to do. */
	if (argc < 2) {
		fprintf(stderr, "usage: aat-input start [--width W --height H] | stop | COMMAND [ARGUMENT...]\n");
		return 2;
	}

	/* start, with the absolute pointer's area. */
	if (strcmp(argv[1], "start") == 0) {
		width = AAT_DEFAULT_WIDTH;
		height = AAT_DEFAULT_HEIGHT;
		for (index = 2; index + 1 < argc; index += 2) {
			if (strcmp(argv[index], "--width") == 0)
				width = atoi(argv[index + 1]);
			else if (strcmp(argv[index], "--height") == 0)
				height = atoi(argv[index + 1]);
		}
		status = aat_start(width, height);
		return status;
	}

	/* stop. */
	if (strcmp(argv[1], "stop") == 0) {
		status = aat_stop();
		return status;
	}

	/* Anything else is a command for the server. */
	status = aat_send(argc - 1, argv + 1);
	return status;
}

/* Declares the devices, opens the socket and leaves the server running in the background. */
static int
aat_start(
	int width,
	int height)
{
	struct aat_server server;
	struct sockaddr_un address;
	char ready;
	int pipes[2];
	int listener;
	pid_t child;
	ssize_t count;

	/* A sane area. */
	if (width < 2 || height < 2 || width > INPUT_INJECT_AXIS_MAX + 1 || height > INPUT_INJECT_AXIS_MAX + 1) {
		printf("AAT-INPUT error bad-size\n");
		return 1;
	}

	/* The four devices: a relative mouse, an absolute pointer over the output's pixels, a keyboard, a touch screen over the same pixels. */
	memset(&server, 0, sizeof(server));
	server.width = width;
	server.height = height;
	server.mouse = aat_declare(INPUT_INJECT_KIND_MOUSE, 0, 0);
	server.pointer = aat_declare(INPUT_INJECT_KIND_MOUSE, width - 1, height - 1);
	server.keyboard = aat_declare(INPUT_INJECT_KIND_KEYBOARD, 0, 0);
	server.touch = aat_declare_touch(width - 1, height - 1);
	if (server.mouse < 0 || server.pointer < 0 || server.keyboard < 0 || server.touch < 0) {
		printf("AAT-INPUT error inject errno=%d (%s)\n", errno, strerror(errno));
		return 1;
	}

	/* The socket, root's alone. */
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0) {
		printf("AAT-INPUT error socket errno=%d\n", errno);
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", AAT_SOCKET);
	(void)unlink(AAT_SOCKET);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0) {
		printf("AAT-INPUT error bind errno=%d\n", errno);
		return 1;
	}
	(void)chmod(AAT_SOCKET, 0600);

	/* The server goes to the background; the parent waits until it is ready. */
	if (pipe(pipes) != 0) {
		printf("AAT-INPUT error pipe errno=%d\n", errno);
		return 1;
	}
	child = fork();
	if (child < 0) {
		printf("AAT-INPUT error fork errno=%d\n", errno);
		return 1;
	}
	if (child == 0) {
		/* The server: its own session, no terminal. */
		(void)close(pipes[0]);
		(void)setsid();
		(void)aat_serve(&server, listener, pipes[1]);
		_exit(0);
	}

	/* The parent: the server's word that the compositor has had time to take the devices. */
	(void)close(pipes[1]);
	ready = 0;
	count = read(pipes[0], &ready, 1U);
	(void)close(pipes[0]);
	if (count != 1 || ready != 'r') {
		printf("AAT-INPUT error server\n");
		return 1;
	}
	printf("AAT-INPUT ready width=%d height=%d pid=%d\n", width, height, (int)child);
	return 0;
}

/* Asks the server to stop. */
static int
aat_stop(
	void)
{
	char *argv[1];
	int status;

	/* The stop command. */
	argv[0] = "stop";
	status = aat_send(1, argv);
	return status;
}

/* Sends one command (its words joined by spaces) and prints the answer. */
static int
aat_send(
	int argc,
	char **argv)
{
	char line[AAT_LINE_MAX];
	char answer[256];
	size_t length;
	ssize_t count;
	size_t used;
	int connection;
	int index;

	/* The command's line. */
	used = 0U;
	for (index = 0; index < argc; index++) {
		length = strlen(argv[index]);
		if (used + length + 2U > sizeof(line)) {
			printf("error too-long\n");
			return 1;
		}
		if (index > 0)
			line[used++] = ' ';
		memcpy(line + used, argv[index], length);
		used += length;
	}
	line[used++] = '\n';

	/* To the server. */
	connection = aat_connect();
	if (connection < 0) {
		printf("error no-server (aat-input start first)\n");
		return 1;
	}
	if (write(connection, line, used) != (ssize_t)used) {
		printf("error write\n");
		(void)close(connection);
		return 1;
	}

	/* Its answer, one line. */
	count = read(connection, answer, sizeof(answer) - 1U);
	(void)close(connection);
	if (count <= 0) {
		printf("error no-answer\n");
		return 1;
	}
	answer[count] = '\0';
	printf("%s", answer);
	if (strncmp(answer, "ok", 2U) == 0)
		return 0;
	return 1;
}

/* Serves the commands, one connection at a time, until stop. */
static int
aat_serve(
	struct aat_server *server,
	int listener,
	int ready)
{
	char line[AAT_LINE_MAX];
	char why[128];
	char answer[160];
	size_t used;
	ssize_t count;
	int connection;
	int error;

	/* The compositor finds the new devices at its next rescan. */
	(void)signal(SIGPIPE, SIG_IGN);
	aat_sleep_ms(AAT_SETTLE_MS);
	(void)write(ready, "r", 1U);
	(void)close(ready);

	/* Each connection: one line, one answer. */
	for (;;) {
		connection = accept(listener, NULL, NULL);
		if (connection < 0)
			continue;
		used = 0U;
		while (used + 1U < sizeof(line)) {
			count = read(connection, line + used, sizeof(line) - 1U - used);
			if (count <= 0)
				break;
			used += (size_t)count;
			if (memchr(line, '\n', used) != NULL)
				break;
		}
		line[used] = '\0';
		if (used > 0U && line[used - 1U] == '\n')
			line[used - 1U] = '\0';

		/* stop ends the server (its devices go with it). */
		if (strcmp(line, "stop") == 0) {
			(void)write(connection, "ok\n", 3U);
			(void)close(connection);
			(void)unlink(AAT_SOCKET);
			return 0;
		}

		/* Any other command. */
		why[0] = '\0';
		error = aat_command(server, line, why, sizeof(why));
		if (error == 0)
			snprintf(answer, sizeof(answer), "ok\n");
		else
			snprintf(answer, sizeof(answer), "error %s\n", why);
		(void)write(connection, answer, strlen(answer));
		(void)close(connection);
	}
}

/* Opens the injector and declares one device; returns the descriptor, or -1 with errno. */
static int
aat_declare(
	uint32_t kind,
	int x_max,
	int y_max)
{
	struct input_inject_setup setup;
	ssize_t written;
	int descriptor;
	int saved;

	/* The open. */
	descriptor = open(AAT_INJECT, O_WRONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;

	/* The setup. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = kind;
	setup.x_max = x_max;
	setup.y_max = y_max;
	written = write(descriptor, &setup, sizeof(setup));
	if (written != (ssize_t)sizeof(setup)) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}

	/* The device exists while the descriptor is open. */
	return descriptor;
}

/* Runs one command line. */
static int
aat_command(
	struct aat_server *server,
	char *line,
	char *why,
	size_t why_size)
{
	struct input_event events[AAT_EVENTS_MAX];
	uint16_t codes[AAT_COMBO_MAX];
	uint16_t code;
	char *words[8];
	char *cursor;
	char *name;
	char *text;
	unsigned count;
	unsigned index;
	int x1;
	int y1;
	int x2;
	int y2;
	int steps;
	int error;
	int matched;

	/* type keeps everything after its word as the text. */
	if (strncmp(line, "type ", 5U) == 0) {
		text = line + 5;
		error = aat_type(server, text);
		if (error != 0)
			snprintf(why, why_size, "type errno=%d", error);
		return error;
	}

	/* The words of the line. */
	count = 0U;
	cursor = strtok(line, " \t");
	while (cursor != NULL && count < sizeof(words) / sizeof(words[0])) {
		words[count++] = cursor;
		cursor = strtok(NULL, " \t");
	}
	if (count == 0U) {
		snprintf(why, why_size, "empty");
		return EINVAL;
	}

	/* The touch screen's commands (ws190-p002). */
	error = aat_touch_command(server, words, count, why, why_size, &matched);
	if (matched)
		return error;

	/* move-to X Y. */
	if (strcmp(words[0], "move-to") == 0 && count == 3U) {
		error = aat_move_to(server, atoi(words[1]), atoi(words[2]));
		if (error != 0)
			snprintf(why, why_size, "move-to errno=%d", error);
		return error;
	}

	/* move DX DY. */
	if (strcmp(words[0], "move") == 0 && count == 3U) {
		aat_event(&events[0], EV_REL, REL_X, atoi(words[1]));
		aat_event(&events[1], EV_REL, REL_Y, atoi(words[2]));
		aat_event(&events[2], EV_SYN, SYN_REPORT, 0);
		error = aat_emit(server->mouse, events, 3U);
		if (error != 0)
			snprintf(why, why_size, "move errno=%d", error);
		return error;
	}

	/* click and double-click [BUTTON] [X Y]. */
	if (strcmp(words[0], "click") == 0 || strcmp(words[0], "double-click") == 0) {
		code = BTN_LEFT;
		index = 1U;
		if (count >= 2U && aat_button_code(words[1], &code) == 0)
			index = 2U;
		if (count == index + 2U) {
			error = aat_move_to(server, atoi(words[index]), atoi(words[index + 1U]));
			if (error != 0) {
				snprintf(why, why_size, "move-to errno=%d", error);
				return error;
			}
			aat_sleep_ms(AAT_CLICK_MS);
		} else if (count != index) {
			snprintf(why, why_size, "usage: %s [BUTTON] [X Y]", words[0]);
			return EINVAL;
		}
		for (x1 = 0; x1 < (strcmp(words[0], "click") == 0 ? 1 : 2); x1++) {
			error = aat_button(server, code, 1);
			if (error == 0) {
				aat_sleep_ms(AAT_CLICK_MS);
				error = aat_button(server, code, 0);
			}
			if (error != 0) {
				snprintf(why, why_size, "button errno=%d", error);
				return error;
			}
			aat_sleep_ms(AAT_CLICK_MS);
		}
		return 0;
	}

	/* down and up BUTTON. */
	if ((strcmp(words[0], "down") == 0 || strcmp(words[0], "up") == 0) && count == 2U) {
		if (aat_button_code(words[1], &code) != 0) {
			snprintf(why, why_size, "unknown button %s", words[1]);
			return EINVAL;
		}
		error = aat_button(server, code, strcmp(words[0], "down") == 0);
		if (error != 0)
			snprintf(why, why_size, "button errno=%d", error);
		return error;
	}

	/* drag X1 Y1 X2 Y2 [STEPS]. */
	if (strcmp(words[0], "drag") == 0 && (count == 5U || count == 6U)) {
		x1 = atoi(words[1]);
		y1 = atoi(words[2]);
		x2 = atoi(words[3]);
		y2 = atoi(words[4]);
		steps = 20;
		if (count == 6U)
			steps = atoi(words[5]);
		if (steps < 1)
			steps = 1;
		error = aat_move_to(server, x1, y1);
		if (error == 0) {
			aat_sleep_ms(AAT_CLICK_MS);
			error = aat_button(server, BTN_LEFT, 1);
		}
		for (index = 1U; error == 0 && index <= (unsigned)steps; index++) {
			aat_sleep_ms(AAT_TYPE_MS);
			error = aat_move_to(server, x1 + (x2 - x1) * (int)index / steps, y1 + (y2 - y1) * (int)index / steps);
		}
		if (error == 0) {
			aat_sleep_ms(AAT_CLICK_MS);
			error = aat_button(server, BTN_LEFT, 0);
		}
		if (error != 0)
			snprintf(why, why_size, "drag errno=%d", error);
		return error;
	}

	/* wheel and hwheel N. */
	if ((strcmp(words[0], "wheel") == 0 || strcmp(words[0], "hwheel") == 0) && count == 2U) {
		aat_event(&events[0], EV_REL, strcmp(words[0], "wheel") == 0 ? REL_WHEEL : REL_HWHEEL, atoi(words[1]));
		aat_event(&events[1], EV_SYN, SYN_REPORT, 0);
		error = aat_emit(server->pointer, events, 2U);
		if (error != 0)
			snprintf(why, why_size, "wheel errno=%d", error);
		return error;
	}

	/* key NAME[+NAME...]: pressed in order, released in reverse. */
	if (strcmp(words[0], "key") == 0 && count == 2U) {
		count = 0U;
		name = strtok(words[1], "+");
		while (name != NULL) {
			if (count == AAT_COMBO_MAX || aat_key_code(name, &codes[count]) != 0) {
				snprintf(why, why_size, "unknown key %s", name);
				return EINVAL;
			}
			count++;
			name = strtok(NULL, "+");
		}
		for (index = 0U; index < count; index++) {
			error = aat_key(server, codes[index], 1);
			if (error != 0) {
				snprintf(why, why_size, "key errno=%d", error);
				return error;
			}
			aat_sleep_ms(AAT_KEY_MS);
		}
		for (index = count; index > 0U; index--) {
			error = aat_key(server, codes[index - 1U], 0);
			if (error != 0) {
				snprintf(why, why_size, "key errno=%d", error);
				return error;
			}
			aat_sleep_ms(AAT_KEY_MS);
		}
		return 0;
	}

	/* key-down and key-up NAME. */
	if ((strcmp(words[0], "key-down") == 0 || strcmp(words[0], "key-up") == 0) && count == 2U) {
		if (aat_key_code(words[1], &code) != 0) {
			snprintf(why, why_size, "unknown key %s", words[1]);
			return EINVAL;
		}
		error = aat_key(server, code, strcmp(words[0], "key-down") == 0);
		if (error != 0)
			snprintf(why, why_size, "key errno=%d", error);
		return error;
	}

	/* sleep MS. */
	if (strcmp(words[0], "sleep") == 0 && count == 2U) {
		aat_sleep_ms((unsigned)atoi(words[1]));
		return 0;
	}

	/* Anything else. */
	snprintf(why, why_size, "unknown command %s", words[0]);
	return EINVAL;
}

/* Writes a batch of events to a device; returns 0 or errno. */
static int
aat_emit(
	int device,
	const struct input_event *events,
	unsigned count)
{
	ssize_t written;

	/* One write: the kernel checks it whole. */
	written = write(device, events, count * sizeof(events[0]));
	if (written != (ssize_t)(count * sizeof(events[0])))
		return errno != 0 ? errno : EIO;
	return 0;
}

/* Fills one event (the kernel stamps the time). */
static void
aat_event(
	struct input_event *event,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->code = code;
	event->value = value;
}

/* Gives a button's code from its name. */
static int
aat_button_code(
	const char *name,
	uint16_t *code)
{
	if (strcmp(name, "left") == 0)
		*code = BTN_LEFT;
	else if (strcmp(name, "right") == 0)
		*code = BTN_RIGHT;
	else if (strcmp(name, "middle") == 0)
		*code = BTN_MIDDLE;
	else
		return EINVAL;
	return 0;
}

/* Gives a key's code from its name. */
static int
aat_key_code(
	const char *name,
	uint16_t *code)
{
	size_t index;

	for (index = 0U; index < sizeof(aat_keys) / sizeof(aat_keys[0]); index++) {
		if (strcmp(aat_keys[index].name, name) == 0) {
			*code = aat_keys[index].code;
			return 0;
		}
	}
	return EINVAL;
}

/* Presses or releases a button of the absolute pointer (where the pointer is). */
static int
aat_button(
	struct aat_server *server,
	uint16_t code,
	int value)
{
	struct input_event events[2];

	aat_event(&events[0], EV_KEY, code, value);
	aat_event(&events[1], EV_SYN, SYN_REPORT, 0);
	return aat_emit(server->pointer, events, 2U);
}

/* Moves the absolute pointer to a pixel (clamped to the area). */
static int
aat_move_to(
	struct aat_server *server,
	int x,
	int y)
{
	struct input_event events[3];

	if (x < 0)
		x = 0;
	if (x > server->width - 1)
		x = server->width - 1;
	if (y < 0)
		y = 0;
	if (y > server->height - 1)
		y = server->height - 1;
	aat_event(&events[0], EV_ABS, ABS_X, x);
	aat_event(&events[1], EV_ABS, ABS_Y, y);
	aat_event(&events[2], EV_SYN, SYN_REPORT, 0);
	return aat_emit(server->pointer, events, 3U);
}

/* Presses or releases a key. */
static int
aat_key(
	struct aat_server *server,
	uint16_t code,
	int value)
{
	struct input_event events[2];

	aat_event(&events[0], EV_KEY, code, value);
	aat_event(&events[1], EV_SYN, SYN_REPORT, 0);
	return aat_emit(server->keyboard, events, 2U);
}

/* Types ASCII text on the US layout, Shift held for the characters that need it. */
static int
aat_type(
	struct aat_server *server,
	const char *text)
{
	uint16_t code;
	int shift;
	int error;
	char character;

	for (; *text != '\0'; text++) {
		/* The key and whether Shift is needed. */
		character = *text;
		shift = -1;
		for (code = 0U; code < sizeof(aat_plain); code++) {
			if (aat_plain[code] == character && character != 0) {
				shift = 0;
				break;
			}
			if (aat_shifted[code] == character && character != 0) {
				shift = 1;
				break;
			}
		}
		if (shift < 0)
			return EINVAL;

		/* Shift, the key, Shift again. */
		error = 0;
		if (shift)
			error = aat_key(server, KEY_LEFTSHIFT, 1);
		if (error == 0)
			error = aat_key(server, code, 1);
		if (error == 0)
			error = aat_key(server, code, 0);
		if (error == 0 && shift)
			error = aat_key(server, KEY_LEFTSHIFT, 0);
		if (error != 0)
			return error;
		aat_sleep_ms(AAT_TYPE_MS);
	}
	return 0;
}

/* Sleeps some milliseconds. */
static void
aat_sleep_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	wait.tv_sec = (time_t)(milliseconds / 1000U);
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (nanosleep(&wait, &wait) != 0 && errno == EINTR)
		continue;
}

/* Connects to the server; returns the socket or -1. */
static int
aat_connect(
	void)
{
	struct sockaddr_un address;
	int connection;

	connection = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connection < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", AAT_SOCKET);
	if (connect(connection, (struct sockaddr *)&address, sizeof(address)) != 0) {
		(void)close(connection);
		return -1;
	}
	return connection;
}

/* Opens the injector and declares the touch screen over the output's pixels; returns the descriptor, or -1 with errno. */
static int
aat_declare_touch(
	int x_max,
	int y_max)
{
	struct input_inject_setup setup;
	ssize_t written;
	int descriptor;
	int saved;

	/* The open. */
	descriptor = open(AAT_INJECT, O_WRONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;

	/* The setup: the screen's axes are the output's pixels, two fingers a report as a USB touch screen sends. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = INPUT_INJECT_KIND_TOUCH;
	setup.x_max = x_max;
	setup.y_max = y_max;
	setup.report_contacts = AAT_TOUCH_REPORT;
	written = write(descriptor, &setup, sizeof(setup));
	if (written != (ssize_t)sizeof(setup)) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}

	/* Succeeded: the device exists while the descriptor is open. */
	return descriptor;
}

/*
 * Runs a command of the touch screen, when the line is one (*matched 1):
 * tap, double-tap, touch-drag, touch-down, touch-move and touch-up.
 * Returns 0 or an errno value with the reason in why.
 */
static int
aat_touch_command(
	struct aat_server *server,
	char **words,
	unsigned count,
	char *why,
	size_t why_size,
	int *matched)
{
	unsigned id;
	int steps;
	int error;
	int same;

	/* Not a touch command until one of the names matches. */
	*matched = 0;

	/* tap X Y: one finger touches and lifts. */
	same = strcmp(words[0], "tap");
	if (same == 0) {
		*matched = 1;
		if (count != 3U) {
			snprintf(why, why_size, "usage: tap X Y");
			return EINVAL;
		}

		/* The finger at the point. */
		error = aat_touch_tap(server, atoi(words[1]), atoi(words[2]));
		if (error != 0) {
			snprintf(why, why_size, "tap errno=%d", error);
			return error;
		}

		/* Succeeded: tapped. */
		return 0;
	}

	/* double-tap X Y: two taps soon after each other at one place. */
	same = strcmp(words[0], "double-tap");
	if (same == 0) {
		*matched = 1;
		if (count != 3U) {
			snprintf(why, why_size, "usage: double-tap X Y");
			return EINVAL;
		}

		/* The first tap. */
		error = aat_touch_tap(server, atoi(words[1]), atoi(words[2]));
		if (error != 0) {
			snprintf(why, why_size, "double-tap errno=%d", error);
			return error;
		}

		/* The second tap after the pause. */
		aat_sleep_ms(AAT_DOUBLE_TAP_MS);
		error = aat_touch_tap(server, atoi(words[1]), atoi(words[2]));
		if (error != 0) {
			snprintf(why, why_size, "double-tap errno=%d", error);
			return error;
		}

		/* Succeeded: tapped twice. */
		return 0;
	}

	/* touch-drag X1 Y1 X2 Y2 [STEPS]: one finger from one point to another. */
	same = strcmp(words[0], "touch-drag");
	if (same == 0) {
		*matched = 1;
		if (count != 5U && count != 6U) {
			snprintf(why, why_size, "usage: touch-drag X1 Y1 X2 Y2 [STEPS]");
			return EINVAL;
		}

		/* The moves: 20 unless given, at least one. */
		steps = 20;
		if (count == 6U)
			steps = atoi(words[5]);
		if (steps < 1)
			steps = 1;
		error = aat_touch_drag(server, atoi(words[1]), atoi(words[2]), atoi(words[3]), atoi(words[4]), steps);
		if (error != 0) {
			snprintf(why, why_size, "touch-drag errno=%d", error);
			return error;
		}

		/* Succeeded: dragged. */
		return 0;
	}

	/* touch-down ID X Y and touch-move ID X Y: a finger held at a point. */
	same = strcmp(words[0], "touch-down");
	if (same != 0)
		same = strcmp(words[0], "touch-move");
	if (same == 0) {
		*matched = 1;
		if (count != 4U) {
			snprintf(why, why_size, "usage: %s ID X Y", words[0]);
			return EINVAL;
		}

		/* The finger named. */
		error = aat_finger_id(words[1], &id);
		if (error != 0) {
			snprintf(why, why_size, "bad finger %s", words[1]);
			return error;
		}

		/* It touches, or moves, at the point. */
		error = aat_finger(server, id, 1, atoi(words[2]), atoi(words[3]));
		if (error != 0) {
			snprintf(why, why_size, "%s errno=%d", words[0], error);
			return error;
		}

		/* Succeeded: the finger is there. */
		return 0;
	}

	/* touch-up ID: a finger lifts where it is. */
	same = strcmp(words[0], "touch-up");
	if (same == 0) {
		*matched = 1;
		if (count != 2U) {
			snprintf(why, why_size, "usage: touch-up ID");
			return EINVAL;
		}

		/* The finger named. */
		error = aat_finger_id(words[1], &id);
		if (error != 0) {
			snprintf(why, why_size, "bad finger %s", words[1]);
			return error;
		}

		/* It lifts where it is. */
		error = aat_finger(server, id, 0, server->fingers[id].x, server->fingers[id].y);
		if (error != 0) {
			snprintf(why, why_size, "touch-up errno=%d", error);
			return error;
		}

		/* Succeeded: the finger lifted. */
		return 0;
	}

	/* Not a touch command. */
	return 0;
}

/* Taps one finger (finger 0) at a point: it touches, waits AAT_TAP_MS and lifts. */
static int
aat_touch_tap(
	struct aat_server *server,
	int x,
	int y)
{
	int error;

	/* The finger touches. */
	error = aat_finger(server, 0U, 1, x, y);
	if (error != 0)
		return error;

	/* And lifts after a short while. */
	aat_sleep_ms(AAT_TAP_MS);
	error = aat_finger(server, 0U, 0, x, y);
	if (error != 0)
		return error;

	/* Succeeded: one tap. */
	return 0;
}

/* Drags one finger (finger 0) from one point to another in steps, then lifts it. */
static int
aat_touch_drag(
	struct aat_server *server,
	int x1,
	int y1,
	int x2,
	int y2,
	int steps)
{
	int index;
	int x;
	int y;
	int error;

	/* The finger touches at the start. */
	error = aat_finger(server, 0U, 1, x1, y1);
	if (error != 0)
		return error;
	aat_sleep_ms(AAT_CLICK_MS);

	/* Each step along the line. */
	for (index = 1; index <= steps; index++) {
		aat_sleep_ms(AAT_TYPE_MS);
		x = x1 + (x2 - x1) * index / steps;
		y = y1 + (y2 - y1) * index / steps;
		error = aat_finger(server, 0U, 1, x, y);
		if (error != 0)
			return error;
	}

	/* It lifts at the end. */
	aat_sleep_ms(AAT_CLICK_MS);
	error = aat_finger(server, 0U, 0, x2, y2);
	if (error != 0)
		return error;

	/* Succeeded: the drag is over. */
	return 0;
}

/* Puts a finger down or moves it (touching 1), or lifts it (0), at a point clamped to the screen, and sends the frame. */
static int
aat_finger(
	struct aat_server *server,
	unsigned id,
	int touching,
	int x,
	int y)
{
	struct aat_finger *finger;
	unsigned lifting;
	int error;

	/* The point within the screen's pixels. */
	if (x < 0)
		x = 0;
	if (x > server->width - 1)
		x = server->width - 1;
	if (y < 0)
		y = 0;
	if (y > server->height - 1)
		y = server->height - 1;

	/* A finger that lifts is in this frame with its tip up, and no more after it. */
	finger = &server->fingers[id];
	lifting = AAT_FINGERS;
	if (!touching) {
		if (!finger->touching)
			return EINVAL;
		lifting = id;
	}

	/* Where the finger is now. */
	finger->x = x;
	finger->y = y;
	if (touching)
		finger->touching = 1;

	/* The frame with every finger that touches. */
	error = aat_touch_frame(server, lifting);
	if (!touching)
		finger->touching = 0;
	if (error != 0)
		return error;

	/* Succeeded: the screen has the frame. */
	return 0;
}

/*
 * Writes one frame of the touch screen: every finger that touches, the one
 * that lifts (lifting, AAT_FINGERS for none) with its tip up.  Returns 0 or
 * errno.
 */
static int
aat_touch_frame(
	struct aat_server *server,
	unsigned lifting)
{
	struct input_inject_touch_frame frame;
	unsigned index;
	ssize_t written;

	/* The fingers of the frame. */
	memset(&frame, 0, sizeof(frame));
	for (index = 0; index < AAT_FINGERS; index++) {
		if (!server->fingers[index].touching)
			continue;
		frame.contacts[frame.count].contact_id = (int32_t)index;
		frame.contacts[frame.count].tip = 1;
		if (index == lifting)
			frame.contacts[frame.count].tip = 0;
		frame.contacts[frame.count].x = server->fingers[index].x;
		frame.contacts[frame.count].y = server->fingers[index].y;
		frame.count++;
	}

	/* One write: the kernel takes the frame whole. */
	written = write(server->touch, &frame, sizeof(frame));
	if (written != (ssize_t)sizeof(frame)) {
		if (errno != 0)
			return errno;
		return EIO;
	}

	/* Succeeded: the frame is written. */
	return 0;
}

/* Reads a finger's identifier (0..AAT_FINGERS - 1); returns 0 or EINVAL. */
static int
aat_finger_id(
	const char *word,
	unsigned *id)
{
	char *end;
	unsigned long value;

	/* A whole decimal number. */
	value = strtoul(word, &end, 10);
	if (end == word || *end != '\0')
		return EINVAL;

	/* Within the fingers kept. */
	if (value >= AAT_FINGERS)
		return EINVAL;

	/* Succeeded: the identifier. */
	*id = (unsigned)value;
	return 0;
}
