/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * touchinject: drives the test touch screen of /dev/input-inject and reads
 * it back (WS079 p012).
 *
 * The injector exists only in kernels built with CONFIG_INPUT_TEST_INJECT=y,
 * and only root may open it.  Its touch screen takes frames of fingers and
 * runs them through the USB touch screen's state machine, so its evdev node
 * speaks multitouch protocol B as a USB touch screen does.
 *
 *   touchinject [SCRIPT]   replays a touch script (standard input without SCRIPT)
 *   touchinject -c         checks the injector's refusals of touch setups and frames
 *   touchinject -s         checks the Scan Time of a touch screen that has one
 *                          (ws081-p002): its refusals, and MSC_TIMESTAMP read back
 *   touchinject -d MS      waits for the test touch screen's evdev node and prints
 *                          its name, its axes and every event for MS milliseconds
 *   touchinject -t MS      the same, each event with its evdev time (seconds.micro)
 *
 * A script has one line per frame; '#' starts a comment.  A line holds one
 * command, or finger commands separated by ';', which make one frame
 * together:
 *   size W H [N] [scan]   declares the screen with fingers in 0..W and 0..H and
 *                         N fingers per report (the first command; default
 *                         32767 32767 2: three or more fingers make the split
 *                         reports of a "hybrid" USB touch screen); "scan" gives
 *                         the screen a Scan Time, which runs with the script's
 *                         own times (swipe and wait), not with the sleeps
 *   down ID X Y           finger ID touches at X, Y
 *   move ID X Y           finger ID moves to X, Y
 *   up ID                 finger ID lifts
 *   swipe DX DY STEPS MS [JITTER]
 *                         moves every touching finger by DX, DY in STEPS frames,
 *                         MS between them (a decimal: 11.111 is 90 Hz); with
 *                         JITTER each interval is changed by up to +-JITTER ms
 *                         (a fixed pseudo-random sequence, so a script repeats
 *                         itself) while the fingers move at a steady speed over
 *                         STEPS * MS, as a panel that scans unevenly sees them
 *   wait MS / hold MS     sleeps (a decimal)
 *   pad W H [N] [scan]    declares a touch pad instead of a screen (ws159-p003):
 *                         the same fingers, read back with BTN_TOOL_FINGER to
 *                         BTN_TOOL_QUINTTAP and BTN_LEFT (the first command)
 *   press / release       holds the pad's button from this frame on, or lets
 *                         it go; with finger commands on the same line they
 *                         make one frame
 * The script's times are a schedule from the screen's declaration: each
 * frame is written as soon as its time has come, so late wakes do not add
 * up and a Scan Time stays with the real clock, as a panel's does.
 * Every finger that touches is in every frame; a finger that lifts is in its
 * last frame with its tip up.  The screen stays declared until the program
 * ends.
 *
 * Every way out says why on standard error (BUG-099): a replay ends with
 * "touchinject: done lines=N" or with the line and the reason it stopped,
 * and -c, -s, -d and -t that fail say which.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <uapi/input-inject.h>
#include <uapi/input.h>

/* The longest script line, and the longest command in it. */
#define TOUCHINJECT_LINE_MAX	256
#define TOUCHINJECT_WORD_MAX	16

/* The size and the fingers per report the screen is declared with when a script does not say. */
#define TOUCHINJECT_DEFAULT_SIZE	32767
#define TOUCHINJECT_DEFAULT_PER_REPORT	2

/* The injector's node. */
#define TOUCHINJECT_NODE	"/dev/input-inject"

/* The directory of the evdev nodes, and the name the test touch screen's node reports. */
#define TOUCHINJECT_INPUT_DIRECTORY	"/dev/input"
#define TOUCHINJECT_SCREEN_NAME		"Test touchscreen (input-inject)"
#define TOUCHINJECT_PAD_NAME		"Test touchpad (input-inject)"

/* The commands of a script. */
enum touch_command {
	COMMAND_NONE,
	COMMAND_SIZE,
	COMMAND_DOWN,
	COMMAND_MOVE,
	COMMAND_UP,
	COMMAND_SWIPE,
	COMMAND_WAIT,
	COMMAND_PAD,
	COMMAND_PRESS,
	COMMAND_RELEASE,
};

/*
 * One script word and the command it names.
 *
 * The table of them is constant for the program's life.
 */
struct touch_command_word {
	const char *word;
	enum touch_command command;
};

/*
 * One finger the script holds.
 *
 * A finger is used from its down to the frame after its up; lifting marks a
 * finger whose next frame is its last, with its tip up.
 */
struct touch_finger {
	int used;
	int lifting;
	int contact_id;
	int x;
	int y;
};

/*
 * The screen as a replayed script has left it.
 *
 * One instance lives for one replay.
 */
struct touch_screen {
	int fd;
	int declared;
	int width;
	int height;
	/*
	 * The screen has a Scan Time.  scan_us is the script's time since the
	 * declaration in microseconds (the Scan Time, and the schedule the
	 * frames are written on from origin_us, the monotonic clock at the
	 * declaration); jitter_state is the jitter's generator.
	 */
	int scan_time;
	unsigned long long scan_us;
	long long origin_us;
	unsigned long long jitter_state;
	/* The device is a touch pad (ws159-p003), and whether its button is held. */
	int pad;
	int pressed;
	struct touch_finger fingers[INPUT_INJECT_TOUCH_CONTACTS];
};

/*
 * The counts of one refusal check.
 *
 * One instance lives for the check; every case adds to one of the counts.
 */
struct check_result {
	unsigned passed;
	unsigned failed;
};

/*
 * One evdev code and the name the dump prints it by.
 *
 * The table of them is constant for the program's life.
 */
struct code_name {
	int type;
	int code;
	const char *name;
};

/*
 * The words a script's commands are written with.
 *
 * wait and hold are the same command; hold reads better at a script's end.
 */
static const struct touch_command_word touch_commands[] = {
	{ "size", COMMAND_SIZE },
	{ "down", COMMAND_DOWN },
	{ "move", COMMAND_MOVE },
	{ "up", COMMAND_UP },
	{ "swipe", COMMAND_SWIPE },
	{ "wait", COMMAND_WAIT },
	{ "hold", COMMAND_WAIT },
	{ "pad", COMMAND_PAD },
	{ "press", COMMAND_PRESS },
	{ "release", COMMAND_RELEASE },
};

/*
 * The names the dump prints the touch screen's events by.
 *
 * The axes are listed first, in the order the dump prints their ranges.
 */
static const struct code_name code_names[] = {
	{ EV_ABS, ABS_X, "ABS_X" },
	{ EV_ABS, ABS_Y, "ABS_Y" },
	{ EV_ABS, ABS_MT_SLOT, "ABS_MT_SLOT" },
	{ EV_ABS, ABS_MT_TRACKING_ID, "ABS_MT_TRACKING_ID" },
	{ EV_ABS, ABS_MT_POSITION_X, "ABS_MT_POSITION_X" },
	{ EV_ABS, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y" },
	{ EV_KEY, BTN_TOUCH, "BTN_TOUCH" },
	{ EV_KEY, BTN_LEFT, "BTN_LEFT" },
	{ EV_KEY, BTN_TOOL_FINGER, "BTN_TOOL_FINGER" },
	{ EV_KEY, BTN_TOOL_DOUBLETAP, "BTN_TOOL_DOUBLETAP" },
	{ EV_KEY, BTN_TOOL_TRIPLETAP, "BTN_TOOL_TRIPLETAP" },
	{ EV_KEY, BTN_TOOL_QUADTAP, "BTN_TOOL_QUADTAP" },
	{ EV_KEY, BTN_TOOL_QUINTTAP, "BTN_TOOL_QUINTTAP" },
	{ EV_MSC, MSC_TIMESTAMP, "MSC_TIMESTAMP" },
	{ EV_SYN, SYN_REPORT, "SYN_REPORT" },
};

/* The number of axes at the head of code_names. */
#define TOUCHINJECT_AXIS_COUNT	6U

static int replay(const char *path);
static int run_line(struct touch_screen *screen, char *line, unsigned number);
static int run_part(struct touch_screen *screen, char *part, int *frame);
static enum touch_command command_of(const char *word);
static int run_size(struct touch_screen *screen, const char *part, int pad);
static int run_finger(struct touch_screen *screen, enum touch_command command, const char *part);
static int run_swipe(struct touch_screen *screen, const char *part);
static int run_wait(struct touch_screen *screen, const char *part);
static struct touch_finger * finger_of(struct touch_screen *screen, int contact_id);
static int screen_declare(struct touch_screen *screen, int width, int height, int per_report, int scan_time, int pad);
static int screen_frame(struct touch_screen *screen);
static void sleep_ms(long milliseconds);
static void sleep_us(long long microseconds);
static void sleep_until_scheduled(const struct touch_screen *screen);
static long long now_us(void);
static int rounded(double value);
static long long jitter_us(struct touch_screen *screen, double jitter_ms);
static int check_scan(void);
static int check_capable_msc(int node);
static size_t check_read_events(int node, struct input_event *events, size_t capacity);
static void check_scan_frames(struct check_result *result, const struct input_event *events, size_t count);
static int frame_timestamp(const struct input_event *events, size_t start, size_t end);
static int frame_same_time(const struct input_event *events, size_t start, size_t end);
static void check_report(const char *name, const struct check_result *result);
static int check(void);
static void check_expect(struct check_result *result, const char *name, ssize_t written, int error, int expected);
static ssize_t check_setup(int fd, unsigned kind, unsigned per_report, unsigned reserved);
static ssize_t check_frame(int fd, unsigned count, int contact_id, int tip, int x, unsigned reserved);
static int dump(long milliseconds, int with_time);
static int dump_find(char *path, size_t size);
static void dump_axes(int fd);
static void dump_properties(int fd);
static const char *code_name_of(int type, int code);
static long long now_ms(void);

/*
 * Replays a script, checks the refusals, or dumps the touch screen, as asked.
 */
int
main(
	int argc,
	char **argv)
{
	int status;
	int same;
	long milliseconds;

	/* -c checks the refusals. */
	same = 1;
	if (argc == 2)
		same = strcmp(argv[1], "-c");
	if (same == 0) {
		status = check();
		if (status != 0) {
			/* Reports which run failed. */
			fprintf(stderr, "touchinject: -c failed (status %d)\n", status);
			return status;
		}

		/* Succeeded: every refusal was as expected. */
		return 0;
	}

	/* -s checks the Scan Time. */
	same = 1;
	if (argc == 2)
		same = strcmp(argv[1], "-s");
	if (same == 0) {
		status = check_scan();
		if (status != 0) {
			/* Reports which run failed. */
			fprintf(stderr, "touchinject: -s failed (status %d)\n", status);
			return status;
		}

		/* Succeeded: the Scan Time read back as expected. */
		return 0;
	}

	/* -d MS dumps the touch screen's node. */
	same = 1;
	if (argc == 3)
		same = strcmp(argv[1], "-d");
	if (same == 0) {
		milliseconds = strtol(argv[2], NULL, 10);
		status = dump(milliseconds, 0);
		if (status != 0) {
			/* Reports which run failed. */
			fprintf(stderr, "touchinject: -d failed (status %d)\n", status);
			return status;
		}

		/* Succeeded: the node was dumped. */
		return 0;
	}

	/* -t MS dumps it with each event's time. */
	same = 1;
	if (argc == 3)
		same = strcmp(argv[1], "-t");
	if (same == 0) {
		milliseconds = strtol(argv[2], NULL, 10);
		status = dump(milliseconds, 1);
		if (status != 0) {
			/* Reports which run failed. */
			fprintf(stderr, "touchinject: -t failed (status %d)\n", status);
			return status;
		}

		/* Succeeded: the node was dumped with the times. */
		return 0;
	}

	/* More than one argument is not a replay. */
	if (argc > 2) {
		fprintf(stderr, "usage: touchinject [SCRIPT] | -c | -s | -d MS | -t MS\n");
		return 2;
	}

	/* Replays the named script, or standard input. */
	if (argc == 2) {
		status = replay(argv[1]);
	} else {
		status = replay(NULL);
	}

	/* Reports a replay that stopped. */
	if (status != 0)
		return status;

	/* Succeeded: the whole script was replayed. */
	return 0;
}

/* Replays one script through the injector. */
static int
replay(
	const char *path)
{
	struct touch_screen screen;
	char line[TOUCHINJECT_LINE_MAX];
	FILE *script;
	char *read_line;
	unsigned number;
	int error;

	/* Opens the script, or reads standard input. */
	script = stdin;
	if (path != NULL) {
		script = fopen(path, "r");
		if (script == NULL) {
			perror(path);
			return 1;
		}
	}

	/* Opens the injector; the screen is declared by the first command. */
	memset(&screen, 0, sizeof(screen));
	screen.fd = open(TOUCHINJECT_NODE, O_WRONLY);
	if (screen.fd < 0) {
		perror(TOUCHINJECT_NODE);
		return 1;
	}

	/* Runs the script line by line. */
	number = 0;
	while (1) {
		/* The end of the script ends the replay. */
		read_line = fgets(line, sizeof(line), script);
		if (read_line == NULL)
			break;

		/* A bad line stops the replay, saying where. */
		number++;
		error = run_line(&screen, line, number);
		if (error != 0) {
			fprintf(stderr, "touchinject: stopped at line %u\n", number);
			return 1;
		}
	}

	/* A script that could not be read to its end is a failure. */
	error = ferror(script);
	if (error != 0) {
		fprintf(stderr, "touchinject: reading the script failed after line %u\n", number);
		return 1;
	}

	/* Declares a default screen for a script without commands. */
	if (!screen.declared) {
		error = screen_declare(&screen, TOUCHINJECT_DEFAULT_SIZE, TOUCHINJECT_DEFAULT_SIZE, TOUCHINJECT_DEFAULT_PER_REPORT, 0, 0);
		if (error != 0) {
			fprintf(stderr, "touchinject: declaring the default screen failed\n");
			return 1;
		}
	}

	/* Closing the injector removes the screen. */
	error = close(screen.fd);
	if (error != 0) {
		perror("touchinject: close");
		return 1;
	}

	/* Succeeded: every line was written (said, so that a lost output can be told from a failure). */
	fprintf(stderr, "touchinject: done lines=%u\n", number);
	return 0;
}

/*
 * Runs one script line: its commands in order, then, when a finger command
 * changed the fingers, one frame.
 */
static int
run_line(
	struct touch_screen *screen,
	char *line,
	unsigned number)
{
	char *hash;
	char *part;
	char *rest;
	int frame;
	int error;

	/* Drops a comment. */
	hash = strchr(line, '#');
	if (hash != NULL)
		*hash = '\0';

	/* Runs each ';'-separated part; a bad part is reported with its line. */
	frame = 0;
	rest = line;
	while (rest != NULL) {
		/* Cuts the next part out of the line. */
		part = rest;
		rest = strchr(part, ';');
		if (rest != NULL) {
			*rest = '\0';
			rest++;
		}

		/* A bad command stops the replay. */
		error = run_part(screen, part, &frame);
		if (error < 0) {
			fprintf(stderr, "touchinject: line %u: bad command: %s\n", number, part);
			return -1;
		}

		/* A write the kernel refused stops the replay. */
		if (error > 0) {
			fprintf(stderr, "touchinject: line %u: the injector refused a write\n", number);
			return -1;
		}
	}

	/* The finger commands of the line make one frame. */
	if (frame) {
		error = screen_frame(screen);
		if (error != 0) {
			fprintf(stderr, "touchinject: line %u: the injector refused the frame\n", number);
			return -1;
		}
	}

	/* Succeeded: the line was run. */
	return 0;
}

/*
 * Runs one command of a line.  Sets *frame when the command changed the
 * fingers.  Returns 0, -1 for a bad command, or 1 for a refused write.
 */
static int
run_part(
	struct touch_screen *screen,
	char *part,
	int *frame)
{
	enum touch_command command;
	char word[TOUCHINJECT_WORD_MAX];
	int count;
	int error;

	/* A blank part does nothing. */
	count = sscanf(part, "%15s", word);
	if (count != 1)
		return 0;

	/* Any first command but size and pad declares the default screen. */
	command = command_of(word);
	if (!screen->declared && command != COMMAND_SIZE && command != COMMAND_PAD) {
		error = screen_declare(screen, TOUCHINJECT_DEFAULT_SIZE, TOUCHINJECT_DEFAULT_SIZE, TOUCHINJECT_DEFAULT_PER_REPORT, 0, 0);
		if (error != 0)
			return 1;
	}

	/* Runs the command. */
	switch (command) {
	case COMMAND_SIZE:
		error = run_size(screen, part, 0);
		break;
	case COMMAND_PAD:
		error = run_size(screen, part, 1);
		break;
	case COMMAND_PRESS:
	case COMMAND_RELEASE:
		/* Only a touch pad has a button; the change is written with the line's frame. */
		error = -1;
		if (screen->pad) {
			screen->pressed = 0;
			if (command == COMMAND_PRESS)
				screen->pressed = 1;
			*frame = 1;
			error = 0;
		}

		break;
	case COMMAND_DOWN:
	case COMMAND_MOVE:
	case COMMAND_UP:
		error = run_finger(screen, command, part);
		if (error == 0)
			*frame = 1;
		break;
	case COMMAND_SWIPE:
		error = run_swipe(screen, part);
		break;
	case COMMAND_WAIT:
		error = run_wait(screen, part);
		break;
	default:
		error = -1;
		break;
	}

	/* Reports the command's outcome. */
	if (error != 0)
		return error;

	/* Succeeded: the command was run. */
	return 0;
}

/* Finds the command a script word names (COMMAND_NONE for none). */
static enum touch_command
command_of(
	const char *word)
{
	unsigned index;
	int same;

	/* Compares the word with every command's. */
	for (index = 0; index < sizeof(touch_commands) / sizeof(touch_commands[0]); index++) {
		/* The command whose word it is. */
		same = strcmp(word, touch_commands[index].word);
		if (same == 0)
			return touch_commands[index].command;
	}

	/* The word names no command. */
	return COMMAND_NONE;
}

/* Declares the screen's (or the touch pad's) size and fingers per report; only as the first command. */
static int
run_size(
	struct touch_screen *screen,
	const char *part,
	int pad)
{
	char scan[TOUCHINJECT_WORD_MAX];
	int width;
	int height;
	int per_report;
	int scan_time;
	int count;
	int same;
	int error;

	/* A screen that is declared already cannot change. */
	if (screen->declared)
		return -1;

	/* The size, the fingers per report and "scan" when given. */
	per_report = TOUCHINJECT_DEFAULT_PER_REPORT;
	scan[0] = '\0';
	count = sscanf(part, "%*s %d %d %d %15s", &width, &height, &per_report, scan);
	if (count < 2)
		return -1;

	/* Only the word scan may follow the fingers per report. */
	scan_time = 0;
	if (count == 4) {
		same = strcmp(scan, "scan");
		if (same != 0)
			return -1;
		scan_time = 1;
	}

	/* Declares the screen. */
	error = screen_declare(screen, width, height, per_report, scan_time, pad);
	if (error != 0)
		return 1;

	/* Succeeded: the screen exists. */
	return 0;
}

/* Puts a finger down, moves it, or marks it to lift in the next frame. */
static int
run_finger(
	struct touch_screen *screen,
	enum touch_command command,
	const char *part)
{
	struct touch_finger *finger;
	int contact_id;
	int x;
	int y;
	int count;

	/* A lift names only the finger. */
	if (command == COMMAND_UP) {
		count = sscanf(part, "%*s %d", &contact_id);
		if (count != 1)
			return -1;

		/* Only a finger that touches can lift. */
		finger = finger_of(screen, contact_id);
		if (finger == NULL)
			return -1;
		finger->lifting = 1;
		return 0;
	}

	/* A touch or a move names the finger and where it is. */
	count = sscanf(part, "%*s %d %d %d", &contact_id, &x, &y);
	if (count != 3)
		return -1;

	/* A move needs the finger down. */
	finger = finger_of(screen, contact_id);
	if (command == COMMAND_MOVE && finger == NULL)
		return -1;

	/* A new finger takes a free place (the kernel refuses a bad identifier). */
	if (finger == NULL) {
		finger = finger_of(screen, -1);
		if (finger == NULL)
			return -1;
		finger->used = 1;
		finger->lifting = 0;
		finger->contact_id = contact_id;
	}

	/* The finger is where the command says. */
	finger->x = x;
	finger->y = y;

	/* Succeeded: the finger is in the next frame. */
	return 0;
}

/*
 * Moves every touching finger by DX, DY in STEPS frames, MS between them,
 * each interval changed by up to +-JITTER ms.  The screen's Scan Time runs
 * on by each interval; the sleep may be coarser (the kernel's tick).
 */
static int
run_swipe(
	struct touch_screen *screen,
	const char *part)
{
	int start_x[INPUT_INJECT_TOUCH_CONTACTS];
	int start_y[INPUT_INJECT_TOUCH_CONTACTS];
	struct touch_finger *finger;
	double milliseconds;
	double jitter;
	double fraction;
	long long interval;
	long long elapsed;
	unsigned index;
	int dx;
	int dy;
	int steps;
	int step;
	int count;
	int error;

	/* The way, the frames, the time between them and its jitter. */
	jitter = 0.0;
	count = sscanf(part, "%*s %d %d %d %lf %lf", &dx, &dy, &steps, &milliseconds, &jitter);
	if (count < 4)
		return -1;

	/* At least one frame, no negative time, and no jitter larger than the interval. */
	if (steps < 1)
		return -1;
	if (milliseconds < 0.0)
		return -1;
	if (jitter < 0.0 || jitter > milliseconds)
		return -1;

	/* Where each finger starts. */
	for (index = 0; index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
		start_x[index] = screen->fingers[index].x;
		start_y[index] = screen->fingers[index].y;
	}

	/* Each step is one frame, a part of the way further. */
	elapsed = 0;
	for (step = 1; step <= steps; step++) {
		/*
		 * The part of the way: the step's share, or, with jitter, the share
		 * of the steady motion's time that has passed at this frame.
		 */
		fraction = (double)step / (double)steps;
		if (jitter > 0.0) {
			fraction = ((double)elapsed + milliseconds * 1000.0) / (milliseconds * 1000.0 * (double)steps);
			if (fraction > 1.0 || step == steps)
				fraction = 1.0;
		}

		/* Every touching finger that far along its way. */
		for (index = 0; index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
			finger = &screen->fingers[index];
			if (!finger->used || finger->lifting)
				continue;
			if (jitter > 0.0) {
				finger->x = start_x[index] + rounded(dx * fraction);
				finger->y = start_y[index] + rounded(dy * fraction);
			} else {
				finger->x = start_x[index] + dx * step / steps;
				finger->y = start_y[index] + dy * step / steps;
			}
		}

		/* Writes the step's frame. */
		error = screen_frame(screen);
		if (error != 0)
			return 1;

		/* The interval to the next frame, on the Scan Time and in the sleep. */
		interval = (long long)(milliseconds * 1000.0 + 0.5) + jitter_us(screen, jitter);
		if (interval < 0)
			interval = 0;
		screen->scan_us += (unsigned long long)interval;
		elapsed += interval;
		sleep_until_scheduled(screen);
	}

	/* Succeeded: every step was written. */
	return 0;
}

/* Sleeps for the time a wait or hold command gives; the Scan Time runs on by it. */
static int
run_wait(
	struct touch_screen *screen,
	const char *part)
{
	double milliseconds;
	long long microseconds;
	int count;

	/* A negative or missing time is refused. */
	count = sscanf(part, "%*s %lf", &milliseconds);
	if (count != 1 || milliseconds < 0.0)
		return -1;

	/* The time passes: on the schedule once the screen is declared. */
	microseconds = (long long)(milliseconds * 1000.0 + 0.5);
	screen->scan_us += (unsigned long long)microseconds;
	if (screen->declared) {
		sleep_until_scheduled(screen);
	} else {
		sleep_us(microseconds);
	}

	/* Succeeded: the time has passed. */
	return 0;
}

/*
 * Draws the next change of an interval: uniform in +-jitter_ms, from a
 * fixed sequence (xorshift), so that a script gives the same intervals on
 * every run.
 */
static long long
jitter_us(
	struct touch_screen *screen,
	double jitter_ms)
{
	double fraction;

	/* No jitter asked. */
	if (jitter_ms <= 0.0)
		return 0;

	/* Advances the generator. */
	screen->jitter_state ^= screen->jitter_state << 13;
	screen->jitter_state ^= screen->jitter_state >> 7;
	screen->jitter_state ^= screen->jitter_state << 17;
	fraction = (double)(screen->jitter_state >> 11) / 9007199254740992.0;

	/* Succeeded: the change in microseconds. */
	return (long long)((2.0 * fraction - 1.0) * jitter_ms * 1000.0);
}

/* Finds the used finger with a contact identifier, or a free place for -1; NULL for none. */
static struct touch_finger *
finger_of(
	struct touch_screen *screen,
	int contact_id)
{
	struct touch_finger *finger;
	unsigned index;

	/* Looks at every place. */
	for (index = 0; index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
		finger = &screen->fingers[index];

		/* A free place, when one is asked for. */
		if (contact_id < 0) {
			if (!finger->used)
				return finger;
			continue;
		}

		/* The finger with that identifier. */
		if (finger->used && finger->contact_id == contact_id)
			return finger;
	}

	/* Nothing matches. */
	return NULL;
}

/* Declares the touch screen: its area and the fingers per report. */
static int
screen_declare(
	struct touch_screen *screen,
	int width,
	int height,
	int per_report,
	int scan_time,
	int pad)
{
	struct input_inject_setup setup;
	ssize_t written;

	/* Describes the screen: its kind, its area and its reports. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = INPUT_INJECT_KIND_TOUCH;
	if (pad)
		setup.kind = INPUT_INJECT_KIND_TOUCHPAD;
	setup.x_max = width;
	setup.y_max = height;
	setup.report_contacts = (uint32_t)per_report;
	if (scan_time)
		setup.reserved = INPUT_INJECT_TOUCH_SCAN_TIME;

	/* Writes the setup record, which registers the screen. */
	written = write(screen->fd, &setup, sizeof(setup));
	if (written != (ssize_t)sizeof(setup)) {
		perror("touchinject: declare");
		return -1;
	}

	/* Succeeded: the screen exists until the injector is closed. */
	screen->declared = 1;
	screen->width = width;
	screen->height = height;
	screen->scan_time = scan_time;
	screen->scan_us = 0;
	screen->origin_us = now_us();
	screen->jitter_state = 0x9e3779b97f4a7c15ULL;
	screen->pad = pad;
	screen->pressed = 0;
	return 0;
}

/*
 * Writes one frame: every finger that touches, and every finger that lifts
 * with its tip up (which then leaves the script).
 */
static int
screen_frame(
	struct touch_screen *screen)
{
	struct input_inject_touch_frame frame;
	struct input_inject_contact *contact;
	struct touch_finger *finger;
	ssize_t written;
	unsigned index;

	/* Collects the used fingers in their places' order, and the Scan Time (100 us units) on a screen with one. */
	memset(&frame, 0, sizeof(frame));
	if (screen->scan_time)
		frame.reserved = (uint32_t)((screen->scan_us / 100ULL) % (INPUT_INJECT_SCAN_TIME_MAX + 1ULL));

	/* A touch pad's held button rides above the Scan Time. */
	if (screen->pad && screen->pressed)
		frame.reserved |= 1U << INPUT_INJECT_PAD_BUTTONS_SHIFT;
	for (index = 0; index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
		finger = &screen->fingers[index];
		if (!finger->used)
			continue;
		contact = &frame.contacts[frame.count];
		contact->contact_id = finger->contact_id;
		contact->tip = !finger->lifting;
		contact->x = finger->x;
		contact->y = finger->y;
		frame.count++;
	}

	/* Writes the frame; the kernel takes it whole or refuses it. */
	written = write(screen->fd, &frame, sizeof(frame));
	if (written != (ssize_t)sizeof(frame)) {
		perror("touchinject: frame");
		return -1;
	}

	/* A finger that lifted in this frame leaves the script. */
	for (index = 0; index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
		finger = &screen->fingers[index];
		if (finger->used && finger->lifting)
			finger->used = 0;
	}

	/* Succeeded: the frame is written. */
	return 0;
}

/* Sleeps for a number of milliseconds. */
static void
sleep_ms(
	long milliseconds)
{
	struct timespec pause;

	/* Splits the time into seconds and nanoseconds. */
	pause.tv_sec = milliseconds / 1000;
	pause.tv_nsec = (milliseconds % 1000) * 1000000L;

	/* Sleeps; an early wake is of no matter to a test script. */
	(void)nanosleep(&pause, NULL);
}

/* Sleeps until the script's time on the screen's schedule has come; a late wake is not carried on. */
static void
sleep_until_scheduled(
	const struct touch_screen *screen)
{
	long long due;
	long long now;

	/* The frame is due at the declaration plus the script's time. */
	due = screen->origin_us + (long long)screen->scan_us;
	now = now_us();
	if (due <= now)
		return;

	/* Succeeded: sleeps the rest. */
	sleep_us(due - now);
}

/* Rounds a distance to the nearest whole pixel, halves away from zero. */
static int
rounded(
	double value)
{
	/* A negative distance rounds down, a positive one up, at a half. */
	if (value < 0.0)
		return (int)(value - 0.5);

	/* Succeeded: the nearest whole number. */
	return (int)(value + 0.5);
}

/* Reports the monotonic time in microseconds. */
static long long
now_us(void)
{
	struct timespec now;

	/* Reads the monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time in microseconds. */
	return (long long)now.tv_sec * 1000000LL + now.tv_nsec / 1000L;
}

/* Sleeps for a number of microseconds. */
static void
sleep_us(
	long long microseconds)
{
	struct timespec pause;

	/* Splits the time into seconds and nanoseconds. */
	pause.tv_sec = (time_t)(microseconds / 1000000LL);
	pause.tv_nsec = (long)(microseconds % 1000000LL) * 1000L;

	/* Sleeps; an early wake is of no matter to a test script. */
	(void)nanosleep(&pause, NULL);
}

/*
 * Checks that the injector refuses bad touch setups and bad frames, and
 * takes good ones.
 */
static int
check(void)
{
	struct check_result result;
	struct input_event event;
	ssize_t written;
	int fd;

	/* Nothing has been checked yet. */
	memset(&result, 0, sizeof(result));

	/* Opens the injector as root. */
	fd = open(TOUCHINJECT_NODE, O_WRONLY);
	if (fd < 0) {
		perror(TOUCHINJECT_NODE);
		return 1;
	}

	/* A touch screen of no finger per report, or of more than a frame holds, is refused. */
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, 0, 0);
	check_expect(&result, "setup-no-finger", written, errno, EINVAL);
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, INPUT_INJECT_TOUCH_CONTACTS + 1U, 0);
	check_expect(&result, "setup-eleven-fingers", written, errno, EINVAL);

	/* An unknown bit of the reserved word, a pen with fingers and an unknown kind are refused. */
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, 2, 2);
	check_expect(&result, "setup-reserved", written, errno, EINVAL);
	written = check_setup(fd, INPUT_INJECT_KIND_PEN, 2, 0);
	check_expect(&result, "setup-pen-with-fingers", written, errno, EINVAL);
	written = check_setup(fd, 4, 2, 0);
	check_expect(&result, "setup-unknown-kind", written, errno, EINVAL);

	/* A good setup declares the touch screen. */
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, 2, 0);
	check_expect(&result, "setup-good", written, errno, 0);

	/* A pen's event is not a frame. */
	memset(&event, 0, sizeof(event));
	event.type = EV_SYN;
	event.code = SYN_REPORT;
	errno = 0;
	written = write(fd, &event, sizeof(event));
	check_expect(&result, "frame-pen-event", written, errno, EINVAL);

	/* Too many fingers, a tip of 2, an identifier past 255, a finger off the screen, the reserved word. */
	written = check_frame(fd, INPUT_INJECT_TOUCH_CONTACTS + 1U, 1, 1, 10, 0);
	check_expect(&result, "frame-eleven-fingers", written, errno, EINVAL);
	written = check_frame(fd, 1, 1, 2, 10, 0);
	check_expect(&result, "frame-tip-2", written, errno, EINVAL);
	written = check_frame(fd, 1, INPUT_INJECT_CONTACT_ID_MAX + 1, 1, 10, 0);
	check_expect(&result, "frame-id-256", written, errno, EINVAL);
	written = check_frame(fd, 1, 1, 1, 101, 0);
	check_expect(&result, "frame-off-screen", written, errno, EINVAL);
	written = check_frame(fd, 1, 1, 1, 10, 1);
	check_expect(&result, "frame-reserved", written, errno, EINVAL);

	/* A good frame, and a frame of no finger, are still taken. */
	written = check_frame(fd, 1, 1, 1, 10, 0);
	check_expect(&result, "frame-good", written, errno, 0);
	written = check_frame(fd, 0, 0, 0, 0, 0);
	check_expect(&result, "frame-empty", written, errno, 0);

	/* Closing removes the screen. */
	close(fd);

	/* Reports the totals. */
	check_report("TOUCHCHECK", &result);
	if (result.failed != 0)
		return 1;

	/* Succeeded: every refusal was as expected. */
	return 0;
}

/*
 * Checks a touch screen with a Scan Time (ws081-p002): the setups it
 * refuses (a pen with a Scan Time, an unknown bit), a frame's Scan Time past
 * 65535, and a finger's four frames read back: down, a move 8.3 ms later,
 * the same place 8.3 ms later again, and the lift.  Each frame must end with
 * MSC_TIMESTAMP (0, 8300, 16600, 24900 us) right before SYN_REPORT, the
 * unchanged frame must be those two events alone, and every event of a
 * frame must carry the same time.
 */
static int
check_scan(void)
{
	struct check_result result;
	struct input_event events[64];
	char path[64];
	ssize_t written;
	size_t count;
	int capable;
	int found;
	int fd;
	int node;

	/* Nothing has been checked yet. */
	memset(&result, 0, sizeof(result));

	/* Opens the injector as root. */
	fd = open(TOUCHINJECT_NODE, O_WRONLY);
	if (fd < 0) {
		perror(TOUCHINJECT_NODE);
		return 1;
	}

	/* A pen with a Scan Time, and an unknown bit beside it, are refused. */
	written = check_setup(fd, INPUT_INJECT_KIND_PEN, 0, INPUT_INJECT_TOUCH_SCAN_TIME);
	check_expect(&result, "scan-pen", written, errno, EINVAL);
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, 2, INPUT_INJECT_TOUCH_SCAN_TIME | 2U);
	check_expect(&result, "scan-unknown-bit", written, errno, EINVAL);

	/* A touch screen with a Scan Time is declared. */
	written = check_setup(fd, INPUT_INJECT_KIND_TOUCH, 2, INPUT_INJECT_TOUCH_SCAN_TIME);
	check_expect(&result, "scan-setup", written, errno, 0);

	/* A Scan Time past 65535 is refused. */
	written = check_frame(fd, 1, 1, 1, 10, INPUT_INJECT_SCAN_TIME_MAX + 1U);
	check_expect(&result, "scan-past-max", written, errno, EINVAL);

	/* Opens the screen's node before its frames. */
	node = -1;
	found = dump_find(path, sizeof(path));
	if (found)
		node = open(path, O_RDONLY | O_NONBLOCK);
	check_expect(&result, "scan-node", node, errno, 0);
	if (node < 0) {
		close(fd);
		check_report("SCANCHECK", &result);
		return 1;
	}

	/* The node declares MSC_TIMESTAMP. */
	capable = check_capable_msc(node);
	written = 0;
	if (!capable)
		written = -1;
	check_expect(&result, "scan-capability", written, EINVAL, 0);

	/* Down, a move, the same place, the lift: 83 units (8.3 ms) apart. */
	written = check_frame(fd, 1, 1, 1, 10, 1000);
	check_expect(&result, "scan-down", written, errno, 0);
	written = check_frame(fd, 1, 1, 1, 11, 1083);
	check_expect(&result, "scan-move", written, errno, 0);
	written = check_frame(fd, 1, 1, 1, 11, 1166);
	check_expect(&result, "scan-still", written, errno, 0);
	written = check_frame(fd, 1, 1, 0, 11, 1249);
	check_expect(&result, "scan-lift", written, errno, 0);

	/* Reads the frames back and judges them. */
	count = check_read_events(node, events, sizeof(events) / sizeof(events[0]));
	check_scan_frames(&result, events, count);

	/* Closing removes the screen. */
	close(node);
	close(fd);

	/* Reports the totals. */
	check_report("SCANCHECK", &result);
	if (result.failed != 0)
		return 1;

	/* Succeeded: the Scan Time reads back as it was written. */
	return 0;
}

/* Tells whether an evdev node declares EV_MSC's MSC_TIMESTAMP. */
static int
check_capable_msc(
	int node)
{
	unsigned char bits[8];
	int length;

	/* Asks the node for its EV_MSC bits (the kernel answers 0, or -1 for a kind it has none of). */
	memset(bits, 0, sizeof(bits));
	length = ioctl(node, EVIOCGBIT(EV_MSC, sizeof(bits)), bits);
	if (length < 0)
		return 0;

	/* The bit of MSC_TIMESTAMP. */
	if ((bits[MSC_TIMESTAMP / 8] & (1U << (MSC_TIMESTAMP % 8))) == 0)
		return 0;

	/* Succeeded: the node declares it. */
	return 1;
}

/* Reads a node's events for up to a second, or until the buffer is full; returns how many. */
static size_t
check_read_events(
	int node,
	struct input_event *events,
	size_t capacity)
{
	struct pollfd poller;
	long long deadline;
	long long now;
	ssize_t bytes;
	size_t count;
	int ready;

	/* Takes whatever arrives until the second is over or the buffer is full. */
	count = 0;
	deadline = now_ms() + 1000;
	while (count < capacity) {
		/* The second is over. */
		now = now_ms();
		if (now >= deadline)
			break;

		/* Waits for events for the rest of the second. */
		poller.fd = node;
		poller.events = POLLIN;
		poller.revents = 0;
		ready = poll(&poller, 1, (int)(deadline - now));
		if (ready <= 0)
			continue;

		/* Reads the events that are ready. */
		bytes = read(node, events + count, (capacity - count) * sizeof(events[0]));
		if (bytes <= 0)
			continue;
		count += (size_t)bytes / sizeof(events[0]);
	}

	/* Succeeded: the events read. */
	return count;
}

/*
 * Judges the four frames of the Scan Time check: each ends with the
 * expected MSC_TIMESTAMP right before SYN_REPORT and has one time for all
 * its events, the third (the finger did not move) is those two events
 * alone, and there are exactly four.
 */
static void
check_scan_frames(
	struct check_result *result,
	const struct input_event *events,
	size_t count)
{
	static const int expected[] = { 0, 8300, 16600, 24900 };
	size_t start;
	size_t index;
	int timestamp;
	int same_time;
	int frames;

	/* Each SYN_REPORT ends a frame that started after the last one. */
	frames = 0;
	start = 0;
	for (index = 0; index < count; index++) {
		/* Only the end of a frame is judged. */
		if (events[index].type != EV_SYN)
			continue;
		if (events[index].code != SYN_REPORT)
			continue;

		/* The frame's time stamp and whether its events share one time. */
		timestamp = frame_timestamp(events, start, index);
		same_time = frame_same_time(events, start, index);
		printf("SCANCHECK frame=%d timestamp=%d same-time=%d\n", frames, timestamp, same_time);

		/* The frame is right when its stamp is the expected one and its time is one. */
		if (frames < 4 &&
		    timestamp == expected[frames] &&
		    same_time) {
			result->passed++;
		} else {
			result->failed++;
		}

		/* The frame of a finger that did not move is MSC_TIMESTAMP and SYN_REPORT alone. */
		if (frames == 2 && index - start + 1 != 2) {
			result->failed++;
			printf("SCANCHECK case=scan-still-alone events=%zu FAIL\n", index - start + 1);
		}

		/* The next frame starts after this one. */
		start = index + 1;
		frames++;
	}

	/* Exactly the four frames. */
	if (frames == 4) {
		result->passed++;
		printf("SCANCHECK case=scan-frames ok\n");
	} else {
		result->failed++;
		printf("SCANCHECK case=scan-frames frames=%d FAIL\n", frames);
	}
}

/* Gives the MSC_TIMESTAMP right before a frame's SYN_REPORT at end, or -1 when there is none. */
static int
frame_timestamp(
	const struct input_event *events,
	size_t start,
	size_t end)
{
	const struct input_event *before;

	/* A frame of the SYN_REPORT alone has none. */
	if (end <= start)
		return -1;

	/* The event right before the SYN_REPORT. */
	before = &events[end - 1];
	if (before->type != EV_MSC)
		return -1;
	if (before->code != MSC_TIMESTAMP)
		return -1;

	/* Succeeded: its value. */
	return before->value;
}

/* Tells whether every event of the frame from start to its SYN_REPORT at end has the SYN_REPORT's time. */
static int
frame_same_time(
	const struct input_event *events,
	size_t start,
	size_t end)
{
	size_t index;

	/* Compares each event's time with the SYN_REPORT's. */
	for (index = start; index < end; index++) {
		if (events[index].time.tv_sec != events[end].time.tv_sec)
			return 0;
		if (events[index].time.tv_usec != events[end].time.tv_usec)
			return 0;
	}

	/* Succeeded: one time for the whole frame. */
	return 1;
}

/* Prints a check's totals under its name. */
static void
check_report(
	const char *name,
	const struct check_result *result)
{
	const char *verdict;

	/* The check passed when nothing failed. */
	verdict = "FAIL";
	if (result->failed == 0)
		verdict = "ok";

	/* One line of totals. */
	printf("%s result=%s passed=%u failed=%u\n", name, verdict, result->passed, result->failed);
}

/*
 * Records one case: a write that should have failed with the expected
 * error, or succeeded when the expected error is 0.
 */
static void
check_expect(
	struct check_result *result,
	const char *name,
	ssize_t written,
	int error,
	int expected)
{
	int got;

	/* A call that did not fail reports no error. */
	got = 0;
	if (written < 0)
		got = error;

	/* Counts and prints the case. */
	if (got == expected) {
		result->passed++;
		printf("TOUCHCHECK case=%s expect=%d got=%d ok\n", name, expected, got);
	} else {
		result->failed++;
		printf("TOUCHCHECK case=%s expect=%d got=%d FAIL\n", name, expected, got);
	}
}

/* Writes one setup record of an area of 100 x 100. */
static ssize_t
check_setup(
	int fd,
	unsigned kind,
	unsigned per_report,
	unsigned reserved)
{
	struct input_inject_setup setup;
	ssize_t written;

	/* The record as asked. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = kind;
	setup.x_max = 100;
	setup.y_max = 100;
	setup.report_contacts = per_report;
	setup.reserved = reserved;

	/* Writes it; the kernel takes it or refuses it. */
	errno = 0;
	written = write(fd, &setup, sizeof(setup));

	/* Succeeded or refused, as the kernel answered. */
	return written;
}

/* Writes one frame of count fingers, all alike but for their identifiers. */
static ssize_t
check_frame(
	int fd,
	unsigned count,
	int contact_id,
	int tip,
	int x,
	unsigned reserved)
{
	struct input_inject_touch_frame frame;
	ssize_t written;
	unsigned index;

	/* The frame as asked (a count past the array names only its fingers). */
	memset(&frame, 0, sizeof(frame));
	frame.count = count;
	frame.reserved = reserved;
	for (index = 0; index < count && index < INPUT_INJECT_TOUCH_CONTACTS; index++) {
		frame.contacts[index].contact_id = contact_id + (int)index;
		frame.contacts[index].tip = tip;
		frame.contacts[index].x = x;
		frame.contacts[index].y = 10;
	}

	/* Writes it; the kernel takes it whole or refuses it. */
	errno = 0;
	written = write(fd, &frame, sizeof(frame));

	/* Succeeded or refused, as the kernel answered. */
	return written;
}

/*
 * Waits up to MS milliseconds for the test touch screen's evdev node, then
 * prints its name, its axes and every event it reports until MS
 * milliseconds have passed from the start or the screen goes away.
 */
static int
dump(
	long milliseconds,
	int with_time)
{
	struct input_event events[32];
	struct pollfd poller;
	char path[64];
	long long deadline;
	long long now;
	ssize_t bytes;
	size_t count;
	size_t index;
	unsigned long total;
	const char *name;
	int found;
	int ready;
	int error;
	int fd;

	/* Waits for the node, which appears when the screen is declared. */
	deadline = now_ms() + milliseconds;
	found = 0;
	while (1) {
		/* A node with the screen's name ends the wait. */
		found = dump_find(path, sizeof(path));
		if (found)
			break;

		/* Gives up when the time is over. */
		now = now_ms();
		if (now >= deadline)
			break;
		sleep_ms(20);
	}

	/* Without the node there is nothing to read. */
	if (!found) {
		printf("TOUCHDUMP end events=0 reason=no-node\n");
		return 1;
	}

	/* Opens the node and prints what it is. */
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	/* The node's path, name and axes. */
	printf("TOUCHDUMP node=%s name=%s\n", path, TOUCHINJECT_SCREEN_NAME);
	dump_properties(fd);
	dump_axes(fd);
	fflush(stdout);

	/* Prints every event until the time is over or the screen goes away. */
	total = 0;
	while (1) {
		/* Waits for events for the rest of the time. */
		now = now_ms();
		if (now >= deadline) {
			printf("TOUCHDUMP end events=%lu reason=time\n", total);
			break;
		}

		/* Polls the node for the rest of the time. */
		poller.fd = fd;
		poller.events = POLLIN;
		poller.revents = 0;
		ready = poll(&poller, 1, (int)(deadline - now));
		if (ready <= 0)
			continue;

		/* Reads the events that are ready. */
		bytes = read(fd, events, sizeof(events));
		if (bytes < 0 && errno == EAGAIN)
			continue;
		if (bytes <= 0) {
			error = 0;
			if (bytes < 0)
				error = errno;
			printf("TOUCHDUMP end events=%lu reason=gone errno=%d\n", total, error);
			break;
		}

		/* Prints each event by its name and value. */
		count = (size_t)bytes / sizeof(events[0]);
		for (index = 0; index < count; index++) {
			name = code_name_of(events[index].type, events[index].code);
			if (with_time) {
				printf("TOUCHDUMP event %s %d time=%lld.%06lld\n", name, events[index].value,
				       (long long)events[index].time.tv_sec, (long long)events[index].time.tv_usec);
			} else {
				printf("TOUCHDUMP event %s %d\n", name, events[index].value);
			}

			/* One more event printed. */
			total++;
		}

		/* The lines leave at once, for a reader of the output file. */
		fflush(stdout);
	}

	/* The node is no longer read. */
	close(fd);

	/* Succeeded: the events were printed. */
	return 0;
}

/* Finds the evdev node whose name is the test touch screen's; reports whether it did. */
static int
dump_find(
	char *path,
	size_t size)
{
	DIR *directory;
	struct dirent *entry;
	char name[64];
	int found;
	int fd;
	int length;
	int same;

	/* Without the directory there is no node. */
	directory = opendir(TOUCHINJECT_INPUT_DIRECTORY);
	if (directory == NULL)
		return 0;

	/* Asks every eventN node for its name. */
	found = 0;
	while (!found) {
		/* The end of the directory ends the search. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only eventN nodes speak evdev. */
		same = strncmp(entry->d_name, "event", 5);
		if (same != 0)
			continue;

		/* Opens the node to ask for its name. */
		length = snprintf(path, size, "%s/%s", TOUCHINJECT_INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= size)
			continue;
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		/* The test touch screen's name ends the search. */
		memset(name, 0, sizeof(name));
		length = ioctl(fd, EVIOCGNAME(sizeof(name) - 1U), name);
		close(fd);
		if (length < 0)
			continue;
		same = strcmp(name, TOUCHINJECT_SCREEN_NAME);
		if (same == 0)
			found = 1;
		same = strcmp(name, TOUCHINJECT_PAD_NAME);
		if (same == 0)
			found = 1;
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: whether the node was found (its path is in path). */
	return found;
}

/*
 * Prints the device's INPUT_PROP_* bits (EVIOCGPROP, ws159-p003): 0x1 a
 * pointer, 0x2 direct, 0x4 a button pad.
 */
static void
dump_properties(
	int fd)
{
	unsigned char bits[4];
	int result;

	/* Asks for the bitmap. */
	memset(bits, 0, sizeof(bits));
	result = ioctl(fd, EVIOCGPROP(sizeof(bits)), bits);
	if (result < 0) {
		printf("TOUCHDUMP props=unknown\n");
		return;
	}

	/* Prints it as one number. */
	printf("TOUCHDUMP props=0x%x\n", (unsigned)bits[0] | ((unsigned)bits[1] << 8) | ((unsigned)bits[2] << 16) | ((unsigned)bits[3] << 24));
}

/* Prints the range and resolution of the touch screen's six axes. */
static void
dump_axes(
	int fd)
{
	struct input_absinfo axis;
	unsigned index;
	int error;

	/* Asks the node for each axis. */
	for (index = 0; index < TOUCHINJECT_AXIS_COUNT; index++) {
		/* An axis that cannot be read is printed as missing. */
		memset(&axis, 0, sizeof(axis));
		error = ioctl(fd, EVIOCGABS(code_names[index].code), &axis);
		if (error < 0) {
			printf("TOUCHDUMP abs %s missing errno=%d\n", code_names[index].name, errno);
			continue;
		}

		/* The axis's range and resolution. */
		printf("TOUCHDUMP abs %s min=%d max=%d resolution=%d\n", code_names[index].name, axis.minimum, axis.maximum, axis.resolution);
	}
}

/* Tells the name of an event's code, or "UNKNOWN" for one the screen does not declare. */
static const char *
code_name_of(
	int type,
	int code)
{
	unsigned index;

	/* Looks the code up among the screen's. */
	for (index = 0; index < sizeof(code_names) / sizeof(code_names[0]); index++) {
		if (code_names[index].type == type && code_names[index].code == code)
			return code_names[index].name;
	}

	/* The screen does not declare it. */
	return "UNKNOWN";
}

/* Reports the monotonic time in milliseconds. */
static long long
now_ms(void)
{
	struct timespec now;

	/* Reads the monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time in milliseconds. */
	return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}
