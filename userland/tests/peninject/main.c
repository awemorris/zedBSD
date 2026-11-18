/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * peninject: drives the test pen of /dev/input-inject and reads it back.
 *
 * The injector exists only in kernels built with CONFIG_INPUT_TEST_INJECT=y
 * (WS079 p002), and only root may open it.
 *
 *   peninject [SCRIPT]   replays a pen script (standard input without SCRIPT)
 *   peninject --pointer [SCRIPT]
 *                        replays it with a pen on a tablet beside the
 *                        display (INPUT_INJECT_KIND_PEN_POINTER, WS217)
 *                        instead of one on the display
 *   peninject -c         checks the injector's refusals (EACCES and EPERM for a
 *                        non-root user, EBUSY, EINVAL)
 *   peninject -d MS      waits for the test pen's evdev node and prints its
 *                        name, its axes and every event for MS milliseconds
 *
 * A script has one command per line; '#' starts a comment.
 *   size W H                 declares the pen with ABS_X 0..W, ABS_Y 0..H
 *                            (the first command; default 32767 32767)
 *   tool pen|rubber          chooses the tool for the next approach
 *   down X Y P [TX TY]       brings the tool in and touches with pressure P
 *   move X Y P [TX TY]       moves while touching or hovering
 *   hover X Y [TX TY]        brings the tool in, or moves it, without touching
 *   ramp FROM TO STEPS MS    steps the pressure FROM..TO, MS between steps
 *   button stylus|stylus2 0|1
 *   up                       lifts and takes the tool out of range
 *   lift                     lifts but keeps the tool in range
 *   wait MS                  sleeps
 *   hold MS                  sleeps (kept for scripts that end with it)
 * Every command that changes the pen is written as one frame, ending with
 * SYN_REPORT.  The pen stays declared until the program ends.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <uapi/input-inject.h>
#include <uapi/input.h>

/* The longest script line. */
#define PENINJECT_LINE_MAX	256

/* The most events one write may carry, as the injector allows. */
#define PENINJECT_BATCH_MAX	INPUT_INJECT_EVENTS_MAX

/* The size the pen is declared with when a script does not say. */
#define PENINJECT_DEFAULT_SIZE	32767

/* The injector's node. */
#define PENINJECT_NODE		"/dev/input-inject"

/* The directory of the evdev nodes, and the name the test pen's node reports. */
#define PENINJECT_INPUT_DIRECTORY	"/dev/input"
#define PENINJECT_PEN_NAME		"Test pen (input-inject)"

/* The user the refusal check turns into to be refused as a non-root user. */
#define PENINJECT_CHECK_UID	1000

/* The commands of a script. */
enum pen_command {
	COMMAND_NONE,
	COMMAND_SIZE,
	COMMAND_TOOL,
	COMMAND_DOWN,
	COMMAND_MOVE,
	COMMAND_HOVER,
	COMMAND_RAMP,
	COMMAND_BUTTON,
	COMMAND_UP,
	COMMAND_LIFT,
	COMMAND_WAIT,
};

/*
 * One script word and the command it names.
 *
 * The table of them is constant for the program's life.
 */
struct pen_command_word {
	const char *word;
	enum pen_command command;
};

/*
 * The pen as a replayed script has left it.
 *
 * One instance lives for one replay; the batch collects the events of the
 * frame being written.
 */
struct pen_state {
	int fd;
	uint32_t kind;
	int declared;
	int tool;
	int in_range;
	int touching;
	int x;
	int y;
	int pressure;
	int tilt_x;
	int tilt_y;
	struct input_event batch[PENINJECT_BATCH_MAX];
	unsigned count;
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
 * The words a script's commands are written with.
 *
 * wait and hold are the same command; hold reads better at a script's end.
 */
static const struct pen_command_word pen_commands[] = {
	{ "size", COMMAND_SIZE },
	{ "tool", COMMAND_TOOL },
	{ "down", COMMAND_DOWN },
	{ "move", COMMAND_MOVE },
	{ "hover", COMMAND_HOVER },
	{ "ramp", COMMAND_RAMP },
	{ "button", COMMAND_BUTTON },
	{ "up", COMMAND_UP },
	{ "lift", COMMAND_LIFT },
	{ "wait", COMMAND_WAIT },
	{ "hold", COMMAND_WAIT },
};

static int replay(const char *path, uint32_t kind);
static int run_line(struct pen_state *pen, char *line, unsigned number);
static int run_command(struct pen_state *pen, enum pen_command command, const char *line);
static enum pen_command command_of(const char *word);
static int run_size(struct pen_state *pen, const char *line);
static int run_tool(struct pen_state *pen, const char *line);
static int run_wait(const char *line);
static int run_position(struct pen_state *pen, enum pen_command command, const char *line);
static int run_ramp(struct pen_state *pen, const char *line);
static int run_button(struct pen_state *pen, const char *line);
static int run_up(struct pen_state *pen, int leave);
static int pen_declare(struct pen_state *pen, int width, int height);
static void pen_add(struct pen_state *pen, int type, int code, int value);
static int pen_flush(struct pen_state *pen);
static void sleep_ms(long milliseconds);
static int check(void);
static void check_expect(struct check_result *result, const char *name, ssize_t written, int error, int expected);
static int check_non_root(void);
static ssize_t check_write_event(int fd, int type, int code, int value);
static int dump(long milliseconds);
static int dump_find(char *path, size_t size);
static void dump_axes(int fd);
static long long now_ms(void);

/*
 * Replays a script, checks the refusals, or dumps the pen, as asked.
 */
int
main(
	int argc,
	char **argv)
{
	uint32_t kind;
	int first;
	int status;
	int same;
	long milliseconds;

	/* -c checks the refusals. */
	same = 1;
	if (argc == 2)
		same = strcmp(argv[1], "-c");
	if (same == 0) {
		status = check();
		return status;
	}

	/* -d MS dumps the pen's node. */
	same = 1;
	if (argc == 3)
		same = strcmp(argv[1], "-d");
	if (same == 0) {
		milliseconds = strtol(argv[2], NULL, 10);
		status = dump(milliseconds);
		return status;
	}

	/* --pointer replays with a pen on a tablet beside the display. */
	kind = INPUT_INJECT_KIND_PEN;
	first = 1;
	same = 1;
	if (argc >= 2)
		same = strcmp(argv[1], "--pointer");
	if (same == 0) {
		kind = INPUT_INJECT_KIND_PEN_POINTER;
		first = 2;
	}

	/* More than one script is not a replay. */
	if (argc > first + 1) {
		fprintf(stderr, "usage: peninject [--pointer] [SCRIPT] | -c | -d MS\n");
		return 2;
	}

	/* Replays the named script, or standard input. */
	if (argc == first + 1) {
		status = replay(argv[first], kind);
	} else {
		status = replay(NULL, kind);
	}

	/* Reports a replay that failed. */
	if (status != 0)
		return status;

	/* Succeeded: the whole script was replayed. */
	return 0;
}

/* Replays one script through the injector. */
static int
replay(
	const char *path,
	uint32_t kind)
{
	struct pen_state pen;
	char line[PENINJECT_LINE_MAX];
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

	/* Opens the injector; the pen is declared by the first command. */
	memset(&pen, 0, sizeof(pen));
	pen.kind = kind;
	pen.tool = BTN_TOOL_PEN;
	pen.fd = open(PENINJECT_NODE, O_WRONLY);
	if (pen.fd < 0) {
		perror(PENINJECT_NODE);
		return 1;
	}

	/* Runs the script line by line. */
	number = 0;
	while (1) {
		/* The end of the script ends the replay. */
		read_line = fgets(line, sizeof(line), script);
		if (read_line == NULL)
			break;

		/* A bad line stops the replay. */
		number++;
		error = run_line(&pen, line, number);
		if (error != 0)
			return 1;
	}

	/* Declares a default pen for a script without commands. */
	if (!pen.declared) {
		error = pen_declare(&pen, PENINJECT_DEFAULT_SIZE, PENINJECT_DEFAULT_SIZE);
		if (error != 0)
			return 1;
	}

	/* Closing the injector removes the pen. */
	close(pen.fd);

	/* Succeeded: every command was written. */
	return 0;
}

/* Runs one script line. */
static int
run_line(
	struct pen_state *pen,
	char *line,
	unsigned number)
{
	enum pen_command command;
	char word[16];
	char *hash;
	int count;
	int error;

	/* Drops a comment. */
	hash = strchr(line, '#');
	if (hash != NULL)
		*hash = '\0';

	/* A blank line does nothing. */
	count = sscanf(line, "%15s", word);
	if (count != 1)
		return 0;

	/* Any first command but size declares the default pen. */
	command = command_of(word);
	if (!pen->declared && command != COMMAND_SIZE) {
		error = pen_declare(pen, PENINJECT_DEFAULT_SIZE, PENINJECT_DEFAULT_SIZE);
		if (error != 0)
			return -1;
	}

	/* Runs the command; a bad line is reported with its number. */
	error = run_command(pen, command, line);
	if (error < 0) {
		fprintf(stderr, "peninject: line %u: bad command\n", number);
		return -1;
	}

	/* A write the kernel refused stops the replay. */
	if (error > 0)
		return -1;

	/* Succeeded: the line was run. */
	return 0;
}

/* Finds the command a script word names (COMMAND_NONE for none). */
static enum pen_command
command_of(
	const char *word)
{
	unsigned index;
	int same;

	/* Compares the word with every command's. */
	for (index = 0; index < sizeof(pen_commands) / sizeof(pen_commands[0]); index++) {
		/* The command whose word it is. */
		same = strcmp(word, pen_commands[index].word);
		if (same == 0)
			return pen_commands[index].command;
	}

	/* The word names no command. */
	return COMMAND_NONE;
}

/*
 * Runs one command.  Reports -1 for a bad line, 1 for a write the kernel
 * refused, 0 for success.
 */
static int
run_command(
	struct pen_state *pen,
	enum pen_command command,
	const char *line)
{
	int status;

	/* Each command's own runner. */
	switch (command) {
	case COMMAND_SIZE:
		status = run_size(pen, line);
		break;
	case COMMAND_TOOL:
		status = run_tool(pen, line);
		break;
	case COMMAND_DOWN:
	case COMMAND_MOVE:
	case COMMAND_HOVER:
		status = run_position(pen, command, line);
		break;
	case COMMAND_RAMP:
		status = run_ramp(pen, line);
		break;
	case COMMAND_BUTTON:
		status = run_button(pen, line);
		break;
	case COMMAND_UP:
		status = run_up(pen, 1);
		break;
	case COMMAND_LIFT:
		status = run_up(pen, 0);
		break;
	case COMMAND_WAIT:
		status = run_wait(line);
		break;
	default:
		status = -1;
		break;
	}

	/* Reports a command that failed. */
	if (status != 0)
		return status;

	/* Succeeded: the command ran. */
	return 0;
}

/* Runs size: declares the pen; it must be the first command. */
static int
run_size(
	struct pen_state *pen,
	const char *line)
{
	int values[2];
	int count;
	int error;

	/* A second declaration, or one without both sizes, is refused. */
	count = sscanf(line, "%*s %d %d", &values[0], &values[1]);
	if (pen->declared || count != 2)
		return -1;

	/* Declares the pen with the script's size. */
	error = pen_declare(pen, values[0], values[1]);
	if (error != 0)
		return 1;

	/* Succeeded: the pen exists. */
	return 0;
}

/* Runs tool: chooses the pen tip or the eraser end for the next approach. */
static int
run_tool(
	struct pen_state *pen,
	const char *line)
{
	char name[16];
	int count;
	int same;

	/* The tool cannot change while it is in range. */
	count = sscanf(line, "%*s %15s", name);
	if (count != 1 || pen->in_range)
		return -1;

	/* The pen tip. */
	same = strcmp(name, "pen");
	if (same == 0) {
		pen->tool = BTN_TOOL_PEN;
		return 0;
	}

	/* The eraser end. */
	same = strcmp(name, "rubber");
	if (same == 0) {
		pen->tool = BTN_TOOL_RUBBER;
		return 0;
	}

	/* Any other tool is refused. */
	return -1;
}

/* Runs wait and hold: sleeps between commands. */
static int
run_wait(
	const char *line)
{
	long milliseconds;
	int count;

	/* A negative or missing time is refused. */
	count = sscanf(line, "%*s %ld", &milliseconds);
	if (count != 1 || milliseconds < 0)
		return -1;

	/* Succeeded: the time has passed. */
	sleep_ms(milliseconds);
	return 0;
}

/*
 * Runs down, move or hover: brings the tool in when it is out of range and
 * writes the axes, with the touch for down.  Reports -1 for a bad line, 1
 * for a refused write.
 */
static int
run_position(
	struct pen_state *pen,
	enum pen_command command,
	const char *line)
{
	int values[5];
	int count;
	int hover;
	int error;

	/* hover has no pressure; down and move have one. */
	hover = 0;
	if (command == COMMAND_HOVER)
		hover = 1;

	/* Reads the position, the pressure and the tilt the line gives. */
	if (hover) {
		count = sscanf(line, "%*s %d %d %d %d", &values[0], &values[1], &values[3], &values[4]);
		if (count != 2 && count != 4)
			return -1;
		values[2] = 0;
		count++;
	} else {
		count = sscanf(line, "%*s %d %d %d %d %d", &values[0], &values[1], &values[2], &values[3], &values[4]);
		if (count != 3 && count != 5)
			return -1;
	}

	/* Brings the tool into range first. */
	if (!pen->in_range) {
		pen_add(pen, EV_KEY, pen->tool, 1);
		pen->in_range = 1;
	}

	/* Takes the values the line gave; a tilt not given stays as it was. */
	pen->x = values[0];
	pen->y = values[1];
	pen->pressure = values[2];
	if (count == 5) {
		pen->tilt_x = values[3];
		pen->tilt_y = values[4];
	}

	/* Queues the axes; the kernel refuses values out of range. */
	pen_add(pen, EV_ABS, ABS_X, pen->x);
	pen_add(pen, EV_ABS, ABS_Y, pen->y);
	pen_add(pen, EV_ABS, ABS_PRESSURE, pen->pressure);
	pen_add(pen, EV_ABS, ABS_TILT_X, pen->tilt_x);
	pen_add(pen, EV_ABS, ABS_TILT_Y, pen->tilt_y);

	/* down touches after the axes, as a real pen reports it. */
	if (command == COMMAND_DOWN && !pen->touching) {
		pen_add(pen, EV_KEY, BTN_TOUCH, 1);
		pen->touching = 1;
	}

	/* Writes the frame. */
	pen_add(pen, EV_SYN, SYN_REPORT, 0);
	error = pen_flush(pen);
	if (error != 0)
		return 1;

	/* Succeeded: the frame was taken. */
	return 0;
}

/*
 * Runs ramp: one frame per pressure step at the current position.
 * Reports -1 for a bad line, 1 for a refused write.
 */
static int
run_ramp(
	struct pen_state *pen,
	const char *line)
{
	long milliseconds;
	int from;
	int to;
	int steps;
	int step;
	int count;
	int error;

	/* Reads the range, the number of steps and the time between them. */
	count = sscanf(line, "%*s %d %d %d %ld", &from, &to, &steps, &milliseconds);
	if (count != 4)
		return -1;

	/* A ramp needs a sane step count and time, and the tool in range. */
	if (steps < 1 || steps > 100000)
		return -1;
	if (milliseconds < 0 || !pen->in_range)
		return -1;

	/* Writes one frame per step, from FROM to TO inclusive. */
	for (step = 0; step <= steps; step++) {
		/* The pressure of this step. */
		pen->pressure = from + (int)((long)(to - from) * step / steps);
		pen_add(pen, EV_ABS, ABS_PRESSURE, pen->pressure);
		pen_add(pen, EV_SYN, SYN_REPORT, 0);

		/* A refused write stops the ramp. */
		error = pen_flush(pen);
		if (error != 0)
			return 1;

		/* Paces the steps. */
		sleep_ms(milliseconds);
	}

	/* Succeeded: every step was taken. */
	return 0;
}

/*
 * Runs button: one frame with the barrel button's new state.
 * Reports -1 for a bad line, 1 for a refused write.
 */
static int
run_button(
	struct pen_state *pen,
	const char *line)
{
	char name[16];
	int state;
	int count;
	int code;
	int same;
	int error;

	/* Reads the button and its state. */
	count = sscanf(line, "%*s %15s %d", name, &state);
	if (count != 2)
		return -1;

	/* The first or the second barrel button; any other name is refused. */
	code = 0;
	same = strcmp(name, "stylus");
	if (same == 0)
		code = BTN_STYLUS;
	same = strcmp(name, "stylus2");
	if (same == 0)
		code = BTN_STYLUS2;
	if (code == 0)
		return -1;
	pen_add(pen, EV_KEY, code, state);

	/* Writes the frame. */
	pen_add(pen, EV_SYN, SYN_REPORT, 0);
	error = pen_flush(pen);
	if (error != 0)
		return 1;

	/* Succeeded: the button changed. */
	return 0;
}

/*
 * Runs up or lift: pressure 0 and touch 0, then, for up, the tool out of
 * range, in one frame.
 */
static int
run_up(
	struct pen_state *pen,
	int leave)
{
	int error;

	/* Lifts the tip. */
	pen->pressure = 0;
	pen_add(pen, EV_ABS, ABS_PRESSURE, 0);
	if (pen->touching) {
		pen_add(pen, EV_KEY, BTN_TOUCH, 0);
		pen->touching = 0;
	}

	/* up also takes the tool out of range. */
	if (leave && pen->in_range) {
		pen_add(pen, EV_KEY, pen->tool, 0);
		pen->in_range = 0;
	}

	/* Writes the frame. */
	pen_add(pen, EV_SYN, SYN_REPORT, 0);
	error = pen_flush(pen);
	if (error != 0)
		return -1;

	/* Succeeded: the pen is lifted. */
	return 0;
}

/* Declares the pen through the injector's setup record. */
static int
pen_declare(
	struct pen_state *pen,
	int width,
	int height)
{
	struct input_inject_setup setup;
	ssize_t written;

	/* Describes the pen: its kind and the size of its area. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = pen->kind;
	setup.x_max = width;
	setup.y_max = height;

	/* Writes the setup record, which registers the pen. */
	written = write(pen->fd, &setup, sizeof(setup));
	if (written != (ssize_t)sizeof(setup)) {
		perror("peninject: declare");
		return -1;
	}

	/* Succeeded: the pen exists until the injector is closed. */
	pen->declared = 1;
	return 0;
}

/* Queues one event for the next write. */
static void
pen_add(
	struct pen_state *pen,
	int type,
	int code,
	int value)
{
	struct input_event *event;

	/* Keeps the batch within the injector's limit. */
	if (pen->count == PENINJECT_BATCH_MAX)
		return;

	/* Fills the next slot; the kernel stamps the time. */
	event = &pen->batch[pen->count];
	pen->count++;
	memset(event, 0, sizeof(*event));
	event->type = (uint16_t)type;
	event->code = (uint16_t)code;
	event->value = value;
}

/* Writes the queued events as one batch. */
static int
pen_flush(
	struct pen_state *pen)
{
	ssize_t written;
	size_t size;

	/* An empty batch writes nothing. */
	size = pen->count * sizeof(pen->batch[0]);
	pen->count = 0;
	if (size == 0)
		return 0;

	/* Writes the batch; the kernel takes all of it or none. */
	written = write(pen->fd, pen->batch, size);
	if (written != (ssize_t)size) {
		perror("peninject: write");
		return -1;
	}

	/* Succeeded: the frame was emitted. */
	return 0;
}

/* Sleeps for a number of milliseconds. */
static void
sleep_ms(
	long milliseconds)
{
	struct timespec delay;
	int slept;

	/* Splits the time into seconds and nanoseconds. */
	delay.tv_sec = milliseconds / 1000;
	delay.tv_nsec = (milliseconds % 1000) * 1000000L;

	/* Sleeps through interruptions. */
	while (1) {
		slept = nanosleep(&delay, &delay);
		if (slept == 0)
			break;
		if (errno != EINTR)
			break;
	}
}

/*
 * Checks the injector's refusals: a non-root open, a second open, and
 * malformed or out-of-range writes.  Prints one PENCHECK line per case.
 */
static int
check(void)
{
	struct check_result result;
	struct input_inject_setup setup;
	struct input_event events[PENINJECT_BATCH_MAX + 1U];
	ssize_t written;
	int first;
	int second;
	int error;
	unsigned index;
	const char *verdict;

	/* Nothing has been checked yet. */
	memset(&result, 0, sizeof(result));

	/* A non-root user is refused by the node's mode, 0600 (in a child that gives up root). */
	error = check_non_root();
	check_expect(&result, "non-root-open-mode", -1, error, EACCES);

	/* With the mode opened up, the driver itself refuses a non-root user with EPERM. */
	error = chmod(PENINJECT_NODE, 0666);
	if (error != 0) {
		check_expect(&result, "chmod-0666", -1, errno, 0);
	} else {
		error = check_non_root();
		check_expect(&result, "non-root-open-driver", -1, error, EPERM);
		(void)chmod(PENINJECT_NODE, 0600);
	}

	/* The first root open is admitted. */
	first = open(PENINJECT_NODE, O_WRONLY);
	if (first < 0) {
		perror("peninject: first open");
		return 1;
	}

	/* A second open while the first is held is refused with EBUSY. */
	second = open(PENINJECT_NODE, O_WRONLY);
	error = errno;
	if (second >= 0) {
		close(second);
		error = 0;
	}

	/* Records the second open's answer. */
	check_expect(&result, "second-open", second, error, EBUSY);

	/* A setup record with an empty area is refused. */
	memset(&setup, 0, sizeof(setup));
	setup.magic = INPUT_INJECT_MAGIC;
	setup.kind = INPUT_INJECT_KIND_PEN;
	setup.x_max = 0;
	setup.y_max = 100;
	written = write(first, &setup, sizeof(setup));
	check_expect(&result, "setup-empty-area", written, errno, EINVAL);

	/* A setup record with a wrong magic is refused. */
	setup.x_max = 100;
	setup.magic = 0;
	written = write(first, &setup, sizeof(setup));
	check_expect(&result, "setup-bad-magic", written, errno, EINVAL);

	/* A good setup record declares the pen. */
	setup.magic = INPUT_INJECT_MAGIC;
	written = write(first, &setup, sizeof(setup));
	check_expect(&result, "setup-good", written, errno, 0);

	/* A pressure above the 4096 levels is refused. */
	written = check_write_event(first, EV_ABS, ABS_PRESSURE, INPUT_INJECT_PRESSURE_MAX + 1);
	check_expect(&result, "pressure-4096", written, errno, EINVAL);

	/* A tilt beyond 60 degrees is refused. */
	written = check_write_event(first, EV_ABS, ABS_TILT_X, INPUT_INJECT_TILT_MAX + 1);
	check_expect(&result, "tilt-61", written, errno, EINVAL);

	/* A position beyond the declared area is refused. */
	written = check_write_event(first, EV_ABS, ABS_X, 101);
	check_expect(&result, "x-beyond-area", written, errno, EINVAL);

	/* A relative event, which the pen does not declare, is refused. */
	written = check_write_event(first, EV_REL, REL_X, 1);
	check_expect(&result, "relative-event", written, errno, EINVAL);

	/* A mouse button, which the pen does not declare, is refused. */
	written = check_write_event(first, EV_KEY, BTN_LEFT, 1);
	check_expect(&result, "mouse-button", written, errno, EINVAL);

	/* A button value other than 0 and 1 is refused. */
	written = check_write_event(first, EV_KEY, BTN_STYLUS, 2);
	check_expect(&result, "button-value-2", written, errno, EINVAL);

	/* A write that is not a whole number of events is refused. */
	memset(events, 0, sizeof(events));
	written = write(first, events, sizeof(events[0]) + 1U);
	check_expect(&result, "torn-event", written, errno, EINVAL);

	/* A write of more events than the injector takes at once is refused. */
	for (index = 0; index < PENINJECT_BATCH_MAX + 1U; index++) {
		events[index].type = EV_SYN;
		events[index].code = SYN_REPORT;
	}

	/* Writes them all at once. */
	written = write(first, events, sizeof(events));
	check_expect(&result, "too-many-events", written, errno, EINVAL);

	/* A good event is still taken after the refusals. */
	written = check_write_event(first, EV_ABS, ABS_PRESSURE, INPUT_INJECT_PRESSURE_MAX);
	check_expect(&result, "pressure-4095", written, errno, 0);

	/* Closing the first open removes the pen. */
	close(first);

	/* Reports the totals: ok when no check failed. */
	verdict = "ok";
	if (result.failed != 0)
		verdict = "FAIL";
	printf("PENCHECK result=%s passed=%u failed=%u\n", verdict, result.passed, result.failed);
	if (result.failed != 0)
		return 1;

	/* Succeeded: every refusal was as expected. */
	return 0;
}

/*
 * Records one case: a write or an open that should have failed with the
 * expected error, or succeeded when the expected error is 0.
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
		printf("PENCHECK case=%s expect=%d got=%d ok\n", name, expected, got);
	} else {
		result->failed++;
		printf("PENCHECK case=%s expect=%d got=%d FAIL\n", name, expected, got);
	}
}

/*
 * Opens the injector as a non-root user in a child process and reports the
 * error the open failed with (0 when it did not fail).
 */
static int
check_non_root(void)
{
	pid_t child;
	pid_t waited;
	int status;
	int exited;
	int code;
	int error;
	int fd;

	/* The child gives up root and tries the open. */
	child = fork();
	if (child < 0)
		return errno;
	if (child == 0) {
		/* Without the uid change the case means nothing. */
		error = setuid(PENINJECT_CHECK_UID);
		if (error != 0)
			_exit(255);

		/* The error of the open is the child's exit status. */
		fd = open(PENINJECT_NODE, O_WRONLY);
		if (fd >= 0)
			_exit(0);
		_exit(errno & 0x7f);
	}

	/* Waits for the child's answer. */
	waited = waitpid(child, &status, 0);
	if (waited != child)
		return errno;

	/* A child that could not become the user reports that as a failure. */
	exited = WIFEXITED(status);
	if (!exited)
		return -1;
	code = WEXITSTATUS(status);
	if (code == 255)
		return -1;

	/* Succeeded: the child's error. */
	return code;
}

/* Writes one event and a SYN_REPORT in one batch. */
static ssize_t
check_write_event(
	int fd,
	int type,
	int code,
	int value)
{
	struct input_event events[2];
	ssize_t written;

	/* The event, then the frame's end. */
	memset(events, 0, sizeof(events));
	events[0].type = (uint16_t)type;
	events[0].code = (uint16_t)code;
	events[0].value = value;
	events[1].type = EV_SYN;
	events[1].code = SYN_REPORT;

	/* Writes both; the kernel takes both or neither. */
	errno = 0;
	written = write(fd, events, sizeof(events));

	/* Succeeded or refused, as the kernel answered. */
	return written;
}

/*
 * Waits up to MS milliseconds for the test pen's evdev node, then prints its
 * name, its axes and every event it reports until MS milliseconds have
 * passed from the start or the pen goes away.
 */
static int
dump(
	long milliseconds)
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
	int found;
	int ready;
	int fd;
	int ended;

	/* Waits for the node, which appears when the pen is declared. */
	deadline = now_ms() + milliseconds;
	found = 0;
	while (1) {
		/* A node with the pen's name ends the wait. */
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
		printf("PENDUMP end events=0 reason=no-node\n");
		return 1;
	}

	/* Opens the node and prints what it is. */
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	/* The node's path, name and axes. */
	printf("PENDUMP node=%s name=%s\n", path, PENINJECT_PEN_NAME);
	dump_axes(fd);
	fflush(stdout);

	/* Prints every event until the time is over or the pen goes away. */
	total = 0;
	while (1) {
		/* Waits for events for the rest of the time. */
		now = now_ms();
		if (now >= deadline) {
			printf("PENDUMP end events=%lu reason=time\n", total);
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
			/* The error of a failed read, none for the end of the node. */
			ended = 0;
			if (bytes < 0)
				ended = errno;
			printf("PENDUMP end events=%lu reason=gone errno=%d\n", total, ended);
			break;
		}

		/* Prints each event: its type, code and value. */
		count = (size_t)bytes / sizeof(events[0]);
		for (index = 0; index < count; index++) {
			printf("PENDUMP event type=%u code=0x%x value=%d\n", events[index].type, events[index].code, events[index].value);
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

/* Finds the evdev node whose name is the test pen's; reports whether it did. */
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
	directory = opendir(PENINJECT_INPUT_DIRECTORY);
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
		length = snprintf(path, size, "%s/%s", PENINJECT_INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= size)
			continue;
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		/* The test pen's name ends the search. */
		memset(name, 0, sizeof(name));
		length = ioctl(fd, EVIOCGNAME(sizeof(name) - 1U), name);
		close(fd);
		if (length < 0)
			continue;
		same = strcmp(name, PENINJECT_PEN_NAME);
		if (same == 0)
			found = 1;
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: whether the node was found (its path is in path). */
	return found;
}

/* Prints the range and resolution of the pen's five axes. */
static void
dump_axes(
	int fd)
{
	static const int codes[] = { ABS_X, ABS_Y, ABS_PRESSURE, ABS_TILT_X, ABS_TILT_Y };
	struct input_absinfo axis;
	unsigned index;
	int error;

	/* Asks the node for each axis. */
	for (index = 0; index < sizeof(codes) / sizeof(codes[0]); index++) {
		/* An axis that cannot be read is printed as missing. */
		memset(&axis, 0, sizeof(axis));
		error = ioctl(fd, EVIOCGABS(codes[index]), &axis);
		if (error < 0) {
			printf("PENDUMP abs code=0x%x missing errno=%d\n", codes[index], errno);
			continue;
		}

		/* The axis's range and resolution. */
		printf("PENDUMP abs code=0x%x min=%d max=%d resolution=%d\n", codes[index], axis.minimum, axis.maximum, axis.resolution);
	}
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
