/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sets the options of a terminal (POSIX XCU stty).
 *
 *	stty [-a|-g]
 *	stty operand...
 *
 * stty works on the terminal that is its standard input.  Without an
 * operand, or with -a, it writes every setting:
 *
 *	speed 9600 baud; rows 24; columns 80;
 *	intr = ^C; quit = ^\; erase = ^?; ... min = 1; time = 0;
 *	-parenb -parodd cs8 -hupcl -cstopb cread clocal
 *	(the input, output and local modes the same way)
 *
 * -g writes them in a form stty takes back as its one operand (numbers in
 * hexadecimal, separated by colons).
 *
 * The operands are the modes by name (a leading - turns one off), cs5 to
 * cs8, the delay groups (nl0, cr0 ... cr3, tab0 ... tab3, bs0, vt0, ff0),
 * a speed (a number, or ispeed and ospeed with one), a control character
 * and its value (a character, ^X, ^?, ^- or undef), min and time with a
 * number, the combinations (evenp and parity, oddp, -parity, -evenp and
 * -oddp, raw, -raw and cooked, nl and -nl, ek, sane, tabs and -tabs, hup
 * and -hup), saved settings from -g, and the window size (rows and cols
 * or columns with a number, and size, which writes the rows and columns).
 *
 * The settings are applied together once every operand has been read, and
 * read back: a setting the terminal did not take is reported and stty
 * exits with 1, as it does for an operand it does not know.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* The fields of struct termios a mode lives in. */
#define STTY_INPUT 0
#define STTY_OUTPUT 1
#define STTY_CONTROL 2
#define STTY_LOCAL 3

/* The erase character of sane and ek: delete, as getty sets it. */
#define STTY_ERASE_DEFAULT 0x7f

/* The number of fields of -g before the control characters. */
#define STTY_SAVED_FIELDS 6

/*
 * A mode by name.
 *
 * value is set in the bits of mask in the field.  A mode that can be
 * turned off (-name) is a single bit; a member of a group (cs7, cr2) is
 * the group's mask and its value, and cannot.
 */
struct stty_mode {
	const char *name;
	int field;
	tcflag_t mask;
	tcflag_t value;
	int negatable;
};

/*
 * A control character by name, and its index in c_cc.
 */
struct stty_character {
	const char *name;
	int index;
	cc_t sane;
};

/*
 * A speed by the number of bauds, and the speed_t that names it.
 */
struct stty_speed {
	unsigned long baud;
	speed_t speed;
};

/*
 * What the operands ask for besides the settings: a new window size, and
 * whether size asked for it to be written.
 */
struct stty_window {
	int rows;
	int columns;
	int write_size;
};

/*
 * The modes, in the order -a writes them.
 */
static const struct stty_mode stty_modes[] = {
	{"parenb", STTY_CONTROL, PARENB, PARENB, 1},
	{"parodd", STTY_CONTROL, PARODD, PARODD, 1},
	{"cs5", STTY_CONTROL, CSIZE, CS5, 0},
	{"cs6", STTY_CONTROL, CSIZE, CS6, 0},
	{"cs7", STTY_CONTROL, CSIZE, CS7, 0},
	{"cs8", STTY_CONTROL, CSIZE, CS8, 0},
	{"hupcl", STTY_CONTROL, HUPCL, HUPCL, 1},
	{"cstopb", STTY_CONTROL, CSTOPB, CSTOPB, 1},
	{"cread", STTY_CONTROL, CREAD, CREAD, 1},
	{"clocal", STTY_CONTROL, CLOCAL, CLOCAL, 1},
	{"ignbrk", STTY_INPUT, IGNBRK, IGNBRK, 1},
	{"brkint", STTY_INPUT, BRKINT, BRKINT, 1},
	{"ignpar", STTY_INPUT, IGNPAR, IGNPAR, 1},
	{"parmrk", STTY_INPUT, PARMRK, PARMRK, 1},
	{"inpck", STTY_INPUT, INPCK, INPCK, 1},
	{"istrip", STTY_INPUT, ISTRIP, ISTRIP, 1},
	{"inlcr", STTY_INPUT, INLCR, INLCR, 1},
	{"igncr", STTY_INPUT, IGNCR, IGNCR, 1},
	{"icrnl", STTY_INPUT, ICRNL, ICRNL, 1},
	{"ixon", STTY_INPUT, IXON, IXON, 1},
	{"ixoff", STTY_INPUT, IXOFF, IXOFF, 1},
	{"ixany", STTY_INPUT, IXANY, IXANY, 1},
	{"opost", STTY_OUTPUT, OPOST, OPOST, 1},
	{"onlcr", STTY_OUTPUT, ONLCR, ONLCR, 1},
	{"ocrnl", STTY_OUTPUT, OCRNL, OCRNL, 1},
	{"onocr", STTY_OUTPUT, ONOCR, ONOCR, 1},
	{"onlret", STTY_OUTPUT, ONLRET, ONLRET, 1},
	{"ofill", STTY_OUTPUT, OFILL, OFILL, 1},
	{"ofdel", STTY_OUTPUT, OFDEL, OFDEL, 1},
	{"nl0", STTY_OUTPUT, NLDLY, NL0, 0},
	{"nl1", STTY_OUTPUT, NLDLY, NL1, 0},
	{"cr0", STTY_OUTPUT, CRDLY, CR0, 0},
	{"cr1", STTY_OUTPUT, CRDLY, CR1, 0},
	{"cr2", STTY_OUTPUT, CRDLY, CR2, 0},
	{"cr3", STTY_OUTPUT, CRDLY, CR3, 0},
	{"tab0", STTY_OUTPUT, TABDLY, TAB0, 0},
	{"tab1", STTY_OUTPUT, TABDLY, TAB1, 0},
	{"tab2", STTY_OUTPUT, TABDLY, TAB2, 0},
	{"tab3", STTY_OUTPUT, TABDLY, TAB3, 0},
	{"bs0", STTY_OUTPUT, BSDLY, BS0, 0},
	{"bs1", STTY_OUTPUT, BSDLY, BS1, 0},
	{"vt0", STTY_OUTPUT, VTDLY, VT0, 0},
	{"vt1", STTY_OUTPUT, VTDLY, VT1, 0},
	{"ff0", STTY_OUTPUT, FFDLY, FF0, 0},
	{"ff1", STTY_OUTPUT, FFDLY, FF1, 0},
	{"isig", STTY_LOCAL, ISIG, ISIG, 1},
	{"icanon", STTY_LOCAL, ICANON, ICANON, 1},
	{"iexten", STTY_LOCAL, IEXTEN, IEXTEN, 1},
	{"echo", STTY_LOCAL, ECHO, ECHO, 1},
	{"echoe", STTY_LOCAL, ECHOE, ECHOE, 1},
	{"echok", STTY_LOCAL, ECHOK, ECHOK, 1},
	{"echonl", STTY_LOCAL, ECHONL, ECHONL, 1},
	{"noflsh", STTY_LOCAL, NOFLSH, NOFLSH, 1},
	{"tostop", STTY_LOCAL, TOSTOP, TOSTOP, 1},
#ifdef ECHOCTL
	{"echoctl", STTY_LOCAL, ECHOCTL, ECHOCTL, 1},
#endif
	{NULL, 0, 0, 0, 0}
};

/*
 * The control characters, in the order -a writes them, with the values
 * sane gives them.
 */
static const struct stty_character stty_characters[] = {
	{"intr", VINTR, 0x03},
	{"quit", VQUIT, 0x1c},
	{"erase", VERASE, STTY_ERASE_DEFAULT},
	{"kill", VKILL, 0x15},
	{"eof", VEOF, 0x04},
	{"eol", VEOL, _POSIX_VDISABLE},
	{"start", VSTART, 0x11},
	{"stop", VSTOP, 0x13},
	{"susp", VSUSP, 0x1a},
#ifdef VWERASE
	{"werase", VWERASE, 0x17},
#endif
#ifdef VLNEXT
	{"lnext", VLNEXT, 0x16},
#endif
#ifdef VREPRINT
	{"rprnt", VREPRINT, 0x12},
#endif
	{NULL, 0, 0}
};

/*
 * The speeds the system names.
 */
static const struct stty_speed stty_speeds[] = {
	{0, B0}, {50, B50}, {75, B75}, {110, B110}, {134, B134},
	{150, B150}, {200, B200}, {300, B300}, {600, B600}, {1200, B1200},
	{1800, B1800}, {2400, B2400}, {4800, B4800}, {9600, B9600},
	{19200, B19200}, {38400, B38400},
	{0, 0}
};

static int apply_operands(int count, char **operands, struct termios *settings, struct stty_window *window);
static int apply_valued(const char *operand, const char *value, struct termios *settings, struct stty_window *window);
static int name_is(const char *operand, const char *first, const char *second, const char *third);
static int apply_mode(const char *operand, struct termios *settings);
static int apply_combination(const char *operand, struct termios *settings);
static int apply_character(const char *name, const char *value, struct termios *settings);
static int apply_saved(const char *operand, struct termios *settings);
static tcflag_t *field_of(struct termios *settings, int field);
static tcflag_t field_value(const struct termios *settings, int field);
static void set_sane(struct termios *settings);
static int read_number(const char *text, unsigned long limit, unsigned long *value);
static int speed_of(unsigned long baud, speed_t *speed);
static unsigned long baud_of(speed_t speed);
static void write_all(const struct termios *settings);
static void write_modes(const struct termios *settings, int field);
static void write_character(const char *name, cc_t value);
static void write_saved(const struct termios *settings);
static int same_settings(const struct termios *left, const struct termios *right);
static void usage(void);

/*
 * Runs stty.
 */
int
main(
	int argc,
	char **argv)
{
	struct termios settings;
	struct termios wanted;
	struct termios result;
	struct stty_window window;
	struct winsize size;
	int status;
	int first;
	int compare;
	int same;

	/* Reads the terminal's settings. */
	status = tcgetattr(STDIN_FILENO, &settings);
	if (status != 0) {
		fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
		return 1;
	}

	/* -- before the operands is taken and dropped. */
	first = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			first = 2;
	}

	/* -a and -g take no operand. */
	if (argc > 2) {
		compare = strcmp(argv[1], "-a");
		if (compare != 0)
			compare = strcmp(argv[1], "-g");
		if (compare == 0)
			usage();
	}

	/* No operand, or -a: every setting. */
	if (argc == first) {
		write_all(&settings);
		return 0;
	}

	/* -a alone, and -g alone. */
	if (argc == 2) {
		compare = strcmp(argv[1], "-a");
		if (compare == 0) {
			write_all(&settings);
			return 0;
		}

		/* -g: the settings in the form stty takes back. */
		compare = strcmp(argv[1], "-g");
		if (compare == 0) {
			write_saved(&settings);
			return 0;
		}
	}

	/* The operands change a copy of the settings. */
	wanted = settings;
	memset(&window, 0, sizeof(window));
	window.rows = -1;
	window.columns = -1;
	status = apply_operands(argc - first, argv + first, &wanted, &window);
	if (status != 0)
		return 1;

	/* Applies the settings that changed, and reads them back. */
	same = same_settings(&wanted, &settings);
	if (!same) {
		status = tcsetattr(STDIN_FILENO, TCSADRAIN, &wanted);
		if (status != 0) {
			fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
			return 1;
		}

		/* Reads them back to see that all were taken. */
		status = tcgetattr(STDIN_FILENO, &result);
		same = 0;
		if (status == 0)
			same = same_settings(&wanted, &result);
		if (!same) {
			fprintf(stderr, "stty: standard input: unable to perform all requested operations\n");
			return 1;
		}
	}

	/* The window size: rows and columns, and size. */
	if (window.rows >= 0 || window.columns >= 0 || window.write_size) {
		status = ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
		if (status != 0) {
			fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
			return 1;
		}

		/* The new rows and columns. */
		if (window.rows >= 0)
			size.ws_row = (unsigned short)window.rows;
		if (window.columns >= 0)
			size.ws_col = (unsigned short)window.columns;
		if (window.rows >= 0 || window.columns >= 0) {
			status = ioctl(STDIN_FILENO, TIOCSWINSZ, &size);
			if (status != 0) {
				fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
				return 1;
			}
		}

		/* size writes them. */
		if (window.write_size)
			printf("%u %u\n", (unsigned)size.ws_row, (unsigned)size.ws_col);
	}

	/* Succeeded: the terminal has the settings. */
	return 0;
}

/*
 * Applies the operands to the settings in order.  Returns -1 after a
 * diagnostic for an operand that is not known or lacks its number.
 */
static int
apply_operands(
	int count,
	char **operands,
	struct termios *settings,
	struct stty_window *window)
{
	unsigned long value;
	speed_t speed;
	const char *operand;
	int index;
	int status;
	int compare;

	/* Each operand, with the one after it for those that take a value. */
	for (index = 0; index < count; index++) {
		operand = operands[index];

		/* A mode by name, or a combination. */
		status = apply_mode(operand, settings);
		if (status == 0)
			continue;
		status = apply_combination(operand, settings);
		if (status == 0)
			continue;

		/* size writes the window size. */
		compare = strcmp(operand, "size");
		if (compare == 0) {
			window->write_size = 1;
			continue;
		}

		/* A number alone is a speed for both directions. */
		status = read_number(operand, 4294967295UL, &value);
		if (status == 0) {
			status = speed_of(value, &speed);
			if (status != 0) {
				fprintf(stderr, "stty: invalid speed: '%s'\n", operand);
				return -1;
			}

			/* Both directions. */
			cfsetispeed(settings, speed);
			cfsetospeed(settings, speed);
			continue;
		}

		/* Saved settings from -g. */
		status = apply_saved(operand, settings);
		if (status == 0)
			continue;

		/* The rest take a value from the next operand. */
		status = 1;
		if (index + 1 < count)
			status = apply_valued(operand, operands[index + 1], settings, window);
		if (status < 0)
			return -1;
		if (status > 0) {
			fprintf(stderr, "stty: invalid argument '%s'\n", operand);
			return -1;
		}

		/* The value was the next operand. */
		index++;
	}

	/* Succeeded: every operand was applied. */
	return 0;
}

/*
 * Applies an operand that takes a value: ispeed and ospeed, min and time,
 * rows and cols (or columns), and the control characters.  Returns 1
 * when the operand is none of them, and -1 after a diagnostic for a
 * value that is not right.
 */
static int
apply_valued(
	const char *operand,
	const char *value,
	struct termios *settings,
	struct stty_window *window)
{
	unsigned long number;
	speed_t speed;
	int status;
	int speed_operand;
	int character_operand;
	int size_operand;

	/* What the operand is. */
	speed_operand = name_is(operand, "ispeed", "ospeed", NULL);
	character_operand = name_is(operand, "min", "time", NULL);
	size_operand = name_is(operand, "rows", "cols", "columns");

	/* A speed for one direction. */
	if (speed_operand) {
		status = read_number(value, 4294967295UL, &number);
		if (status == 0)
			status = speed_of(number, &speed);
		if (status != 0) {
			fprintf(stderr, "stty: invalid speed: '%s'\n", value);
			return -1;
		}

		/* ispeed or ospeed. */
		if (operand[0] == 'i')
			cfsetispeed(settings, speed);
		else
			cfsetospeed(settings, speed);
		return 0;
	}

	/* min and time: numbers of characters and tenths of a second. */
	if (character_operand) {
		status = read_number(value, 255, &number);
		if (status != 0) {
			fprintf(stderr, "stty: invalid number: '%s'\n", value);
			return -1;
		}

		/* min or time. */
		if (operand[0] == 'm')
			settings->c_cc[VMIN] = (cc_t)number;
		else
			settings->c_cc[VTIME] = (cc_t)number;
		return 0;
	}

	/* rows and cols: the window size. */
	if (size_operand) {
		status = read_number(value, 65535, &number);
		if (status != 0) {
			fprintf(stderr, "stty: invalid number: '%s'\n", value);
			return -1;
		}

		/* rows, or cols and columns. */
		if (operand[0] == 'r')
			window->rows = (int)number;
		else
			window->columns = (int)number;
		return 0;
	}

	/* A control character, or nothing known. */
	status = apply_character(operand, value, settings);
	return status;
}

/* Tells whether an operand is one of up to three names. */
static int
name_is(
	const char *operand,
	const char *first,
	const char *second,
	const char *third)
{
	int compare;

	/* Each name given. */
	compare = strcmp(operand, first);
	if (compare == 0)
		return 1;
	compare = strcmp(operand, second);
	if (compare == 0)
		return 1;
	if (third == NULL)
		return 0;
	compare = strcmp(operand, third);
	if (compare == 0)
		return 1;
	return 0;
}

/*
 * Applies a mode by name, or -name to turn one off.  Returns -1 when the
 * operand names no mode.
 */
static int
apply_mode(
	const char *operand,
	struct termios *settings)
{
	const struct stty_mode *mode;
	const char *name;
	tcflag_t *field;
	int off;
	int compare;

	/* The name, after a - that turns the mode off. */
	name = operand;
	off = 0;
	if (operand[0] == '-') {
		name = operand + 1;
		off = 1;
	}

	/* Finds the mode. */
	for (mode = stty_modes; mode->name != NULL; mode++) {
		compare = strcmp(mode->name, name);
		if (compare == 0)
			break;
	}

	/* No such mode. */
	if (mode->name == NULL)
		return -1;
	if (off && !mode->negatable)
		return -1;

	/* Sets the bits of the group, or clears the mode's bit. */
	field = field_of(settings, mode->field);
	*field &= ~mode->mask;
	if (!off)
		*field |= mode->value;
	return 0;
}

/*
 * Applies a combination of modes.  Returns -1 when the operand names
 * none.
 */
static int
apply_combination(
	const char *operand,
	struct termios *settings)
{
	int compare;

	/* evenp and parity: even parity on seven bits. */
	compare = strcmp(operand, "evenp");
	if (compare != 0)
		compare = strcmp(operand, "parity");
	if (compare == 0) {
		settings->c_cflag &= ~(CSIZE | PARODD);
		settings->c_cflag |= PARENB | CS7;
		return 0;
	}

	/* oddp: odd parity on seven bits. */
	compare = strcmp(operand, "oddp");
	if (compare == 0) {
		settings->c_cflag &= ~CSIZE;
		settings->c_cflag |= PARENB | PARODD | CS7;
		return 0;
	}

	/* -parity, -evenp and -oddp: no parity on eight bits. */
	compare = strcmp(operand, "-parity");
	if (compare != 0)
		compare = strcmp(operand, "-evenp");
	if (compare != 0)
		compare = strcmp(operand, "-oddp");
	if (compare == 0) {
		settings->c_cflag &= ~(CSIZE | PARENB | PARODD);
		settings->c_cflag |= CS8;
		return 0;
	}

	/* raw: characters as they come, with no processing. */
	compare = strcmp(operand, "raw");
	if (compare == 0) {
		settings->c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
		settings->c_oflag &= ~OPOST;
		settings->c_lflag &= ~(ISIG | ICANON | IEXTEN);
		settings->c_cc[VMIN] = 1;
		settings->c_cc[VTIME] = 0;
		return 0;
	}

	/* -raw and cooked: lines and signals again. */
	compare = strcmp(operand, "-raw");
	if (compare != 0)
		compare = strcmp(operand, "cooked");
	if (compare == 0) {
		settings->c_iflag |= BRKINT | IGNPAR | ISTRIP | ICRNL | IXON;
		settings->c_oflag |= OPOST;
		settings->c_lflag |= ISIG | ICANON;
		return 0;
	}

	/* nl: a newline alone ends a line and is written alone. */
	compare = strcmp(operand, "nl");
	if (compare == 0) {
		settings->c_iflag &= ~ICRNL;
		settings->c_oflag &= ~ONLCR;
		return 0;
	}

	/* -nl: carriage returns are newlines, and newlines are written with one. */
	compare = strcmp(operand, "-nl");
	if (compare == 0) {
		settings->c_iflag &= ~(INLCR | IGNCR);
		settings->c_iflag |= ICRNL;
		settings->c_oflag &= ~(OCRNL | ONLRET);
		settings->c_oflag |= ONLCR;
		return 0;
	}

	/* ek: the usual erase and kill characters. */
	compare = strcmp(operand, "ek");
	if (compare == 0) {
		settings->c_cc[VERASE] = STTY_ERASE_DEFAULT;
		settings->c_cc[VKILL] = 0x15;
		return 0;
	}

	/* sane: settings a terminal can be used with. */
	compare = strcmp(operand, "sane");
	if (compare == 0) {
		set_sane(settings);
		return 0;
	}

	/* tabs and -tabs: tabs written as they are, or expanded. */
	compare = strcmp(operand, "tabs");
	if (compare == 0) {
		settings->c_oflag &= ~TABDLY;
		settings->c_oflag |= TAB0;
		return 0;
	}

	/* -tabs expands them. */
	compare = strcmp(operand, "-tabs");
	if (compare == 0) {
		settings->c_oflag &= ~TABDLY;
		settings->c_oflag |= TAB3;
		return 0;
	}

	/* hup and -hup: hupcl. */
	compare = strcmp(operand, "hup");
	if (compare == 0) {
		settings->c_cflag |= HUPCL;
		return 0;
	}

	/* -hup. */
	compare = strcmp(operand, "-hup");
	if (compare == 0) {
		settings->c_cflag &= ~HUPCL;
		return 0;
	}

	/* No combination. */
	return -1;
}

/*
 * Sets a control character from its value: one character, ^X for a
 * control character, ^? for delete, and ^- or undef for none.  Returns 1
 * when the name is no control character, and -1 after a diagnostic for a
 * value that is none.
 */
static int
apply_character(
	const char *name,
	const char *value,
	struct termios *settings)
{
	const struct stty_character *character;
	size_t length;
	cc_t code;
	int compare;

	/* Finds the character. */
	for (character = stty_characters; character->name != NULL; character++) {
		compare = strcmp(character->name, name);
		if (compare == 0)
			break;
	}

	/* No such character. */
	if (character->name == NULL)
		return 1;

	/* undef and ^- disable it. */
	length = strlen(value);
	compare = strcmp(value, "undef");
	if (compare != 0)
		compare = strcmp(value, "^-");
	if (compare == 0) {
		settings->c_cc[character->index] = _POSIX_VDISABLE;
		return 0;
	}

	/* ^? is delete; ^X is X less 64. */
	if (length == 2 && value[0] == '^') {
		code = (cc_t)(value[1] & 0x1f);
		if (value[1] == '?')
			code = 0x7f;
		settings->c_cc[character->index] = code;
		return 0;
	}

	/* Otherwise the value is the one character. */
	if (length != 1) {
		fprintf(stderr, "stty: invalid integer argument: '%s'\n", value);
		return -1;
	}

	/* The character itself. */
	settings->c_cc[character->index] = (cc_t)value[0];
	return 0;
}

/*
 * Applies settings written by -g: the four fields of modes, the two
 * speeds and the control characters, in hexadecimal between colons.
 * Returns -1 when the operand is not such settings.
 */
static int
apply_saved(
	const char *operand,
	struct termios *settings)
{
	unsigned long values[STTY_SAVED_FIELDS + NCCS];
	struct termios saved;
	const char *cursor;
	char *end;
	int count;
	int index;

	/* Reads every number; there must be exactly as many as -g writes. */
	cursor = operand;
	for (count = 0; count < STTY_SAVED_FIELDS + NCCS; count++) {
		if (*cursor < '0' || (*cursor > '9' && *cursor < 'a') || *cursor > 'f')
			return -1;
		errno = 0;
		values[count] = strtoul(cursor, &end, 16);
		if (errno != 0)
			return -1;

		/* A colon after each number but the last. */
		cursor = end;
		if (count + 1 < STTY_SAVED_FIELDS + NCCS) {
			if (*cursor != ':')
				return -1;
			cursor++;
		}
	}

	/* Nothing may follow the last number. */
	if (*cursor != '\0')
		return -1;

	/* The settings they describe. */
	saved = *settings;
	saved.c_iflag = (tcflag_t)values[0];
	saved.c_oflag = (tcflag_t)values[1];
	saved.c_cflag = (tcflag_t)values[2];
	saved.c_lflag = (tcflag_t)values[3];
	cfsetispeed(&saved, (speed_t)values[4]);
	cfsetospeed(&saved, (speed_t)values[5]);
	for (index = 0; index < NCCS; index++)
		saved.c_cc[index] = (cc_t)values[STTY_SAVED_FIELDS + index];

	/* Succeeded: they replace the settings. */
	*settings = saved;
	return 0;
}

/* Returns the field of the settings a mode lives in. */
static tcflag_t *
field_of(
	struct termios *settings,
	int field)
{
	/* One of the four. */
	if (field == STTY_INPUT)
		return &settings->c_iflag;
	if (field == STTY_OUTPUT)
		return &settings->c_oflag;
	if (field == STTY_CONTROL)
		return &settings->c_cflag;
	return &settings->c_lflag;
}

/* Returns the value of the field of the settings a mode lives in. */
static tcflag_t
field_value(
	const struct termios *settings,
	int field)
{
	/* One of the four. */
	if (field == STTY_INPUT)
		return settings->c_iflag;
	if (field == STTY_OUTPUT)
		return settings->c_oflag;
	if (field == STTY_CONTROL)
		return settings->c_cflag;
	return settings->c_lflag;
}

/*
 * Sets sane: modes for an interactive terminal and the usual control
 * characters, keeping the speeds, the character size and the parity.
 */
static void
set_sane(
	struct termios *settings)
{
	const struct stty_character *character;

	/* The modes. */
	settings->c_cflag |= CREAD;
	settings->c_iflag &= ~(IGNBRK | INLCR | IGNCR | IXOFF | IXANY);
	settings->c_iflag |= BRKINT | ICRNL | IXON;
	settings->c_oflag &= ~(OCRNL | ONOCR | ONLRET | OFILL | OFDEL | NLDLY | CRDLY | TABDLY | BSDLY | VTDLY | FFDLY);
	settings->c_oflag |= OPOST | ONLCR;
	settings->c_lflag &= ~(ECHONL | NOFLSH | TOSTOP);
	settings->c_lflag |= ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK;

	/* The control characters. */
	for (character = stty_characters; character->name != NULL; character++)
		settings->c_cc[character->index] = character->sane;
	settings->c_cc[VMIN] = 1;
	settings->c_cc[VTIME] = 0;
}

/*
 * Reads a decimal number no larger than a limit.  Returns -1 for
 * anything else.
 */
static int
read_number(
	const char *text,
	unsigned long limit,
	unsigned long *value)
{
	const char *cursor;
	unsigned long number;
	unsigned digit;

	/* Digits only, at least one. */
	if (*text == '\0')
		return -1;
	number = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		digit = (unsigned)(*cursor - '0');
		if (number > (limit - digit) / 10)
			return -1;
		number = number * 10 + digit;
	}

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Finds the speed_t of a number of bauds; returns -1 for one not named. */
static int
speed_of(
	unsigned long baud,
	speed_t *speed)
{
	const struct stty_speed *entry;

	/* The speed with that number; B0 is the first entry. */
	for (entry = stty_speeds; entry == stty_speeds || entry->baud != 0; entry++) {
		if (entry->baud == baud) {
			*speed = entry->speed;
			return 0;
		}
	}

	/* Not named. */
	return -1;
}

/* Returns the number of bauds of a speed_t, or the value itself. */
static unsigned long
baud_of(
	speed_t speed)
{
	const struct stty_speed *entry;

	/* The entry of the speed. */
	for (entry = stty_speeds; entry == stty_speeds || entry->baud != 0; entry++) {
		if (entry->speed == speed)
			return entry->baud;
	}

	/* A speed without a name. */
	return (unsigned long)speed;
}

/* Writes every setting (-a, and no operand). */
static void
write_all(
	const struct termios *settings)
{
	const struct stty_character *character;
	struct winsize size;
	unsigned long input;
	unsigned long output;
	int status;

	/* The speeds, and the window size when there is one. */
	input = baud_of(cfgetispeed(settings));
	output = baud_of(cfgetospeed(settings));
	if (input == output)
		printf("speed %lu baud;", output);
	else
		printf("ispeed %lu baud; ospeed %lu baud;", input, output);
	status = ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
	if (status == 0)
		printf(" rows %u; columns %u;", (unsigned)size.ws_row, (unsigned)size.ws_col);
	printf("\n");

	/* The control characters, then min and time. */
	for (character = stty_characters; character->name != NULL; character++)
		write_character(character->name, settings->c_cc[character->index]);
	printf("min = %u; time = %u;\n", (unsigned)settings->c_cc[VMIN], (unsigned)settings->c_cc[VTIME]);

	/* The modes, a line for each field. */
	write_modes(settings, STTY_CONTROL);
	write_modes(settings, STTY_INPUT);
	write_modes(settings, STTY_OUTPUT);
	write_modes(settings, STTY_LOCAL);
}

/*
 * Writes the modes of a field: each single mode, with a - when it is
 * off, and the member of each group that is set.
 */
static void
write_modes(
	const struct termios *settings,
	int field)
{
	const struct stty_mode *mode;
	const char *separator;
	const char *sign;
	tcflag_t value;

	/* Each mode of the field. */
	value = field_value(settings, field);
	separator = "";
	for (mode = stty_modes; mode->name != NULL; mode++) {
		if (mode->field != field)
			continue;

		/* A single mode on or off; a group only by its member. */
		if (mode->negatable) {
			sign = "-";
			if ((value & mode->mask) != 0)
				sign = "";
			printf("%s%s%s", separator, sign, mode->name);
			separator = " ";
		} else if ((value & mode->mask) == mode->value) {
			printf("%s%s", separator, mode->name);
			separator = " ";
		}
	}

	/* The end of the line. */
	printf("\n");
}

/* Writes one control character as "name = value;". */
static void
write_character(
	const char *name,
	cc_t value)
{
	/* Disabled, delete, a control character, or itself. */
	if (value == _POSIX_VDISABLE)
		printf("%s = <undef>; ", name);
	else if (value == 0x7f)
		printf("%s = ^?; ", name);
	else if (value < 0x20)
		printf("%s = ^%c; ", name, value + '@');
	else
		printf("%s = %c; ", name, value);
}

/* Writes the settings in the form apply_saved() reads (-g). */
static void
write_saved(
	const struct termios *settings)
{
	int index;

	/* The modes and the speeds, then each control character. */
	printf("%lx:%lx:%lx:%lx:%lx:%lx", (unsigned long)settings->c_iflag, (unsigned long)settings->c_oflag, (unsigned long)settings->c_cflag, (unsigned long)settings->c_lflag, (unsigned long)cfgetispeed(settings), (unsigned long)cfgetospeed(settings));
	for (index = 0; index < NCCS; index++)
		printf(":%x", (unsigned)settings->c_cc[index]);
	printf("\n");
}

/* Tells whether two settings are the same in everything stty sets. */
static int
same_settings(
	const struct termios *left,
	const struct termios *right)
{
	speed_t left_input;
	speed_t right_input;
	speed_t left_output;
	speed_t right_output;
	int compare;

	/* The modes, the speeds and the control characters. */
	if (left->c_iflag != right->c_iflag || left->c_oflag != right->c_oflag)
		return 0;
	if (left->c_cflag != right->c_cflag || left->c_lflag != right->c_lflag)
		return 0;
	left_input = cfgetispeed(left);
	right_input = cfgetispeed(right);
	left_output = cfgetospeed(left);
	right_output = cfgetospeed(right);
	if (left_input != right_input || left_output != right_output)
		return 0;
	compare = memcmp(left->c_cc, right->c_cc, sizeof(left->c_cc));
	if (compare != 0)
		return 0;
	return 1;
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr, "usage: stty [-a|-g]\n       stty operand...\n");
	exit(1);
}
