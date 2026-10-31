/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoded terminal modes of a pty-req.
 *
 * The table of opcodes is data: each row names the opcode and the flag or
 * the control character it stands for, and rows whose flag or character
 * the local <termios.h> lacks are left out at compile time.
 */

#include <string.h>
#include <unistd.h>

#include "ssh-ttymodes.h"

/*
 * The first opcode that is not defined; parsing stops there (RFC 4254 8:
 * opcodes 160 to 255 are not yet defined and cause parsing to stop).
 */
#define TTY_OP_UNDEFINED_FIRST	160U

/* The argument that stands for "no such character" (RFC 4254 8, opcode 1). */
#define TTY_CHAR_NONE		255U

/*
 * The value of a control character that is switched off locally.  It is
 * _POSIX_VDISABLE where the system defines it, otherwise 255.
 */
#ifdef _POSIX_VDISABLE
#define TTY_LOCAL_DISABLED	((cc_t)_POSIX_VDISABLE)
#else
#define TTY_LOCAL_DISABLED	((cc_t)255)
#endif

/* What part of struct termios an opcode stands for. */
enum tty_kind {
	/* One control character: value is its index in c_cc. */
	TTY_KIND_CHAR = 1,

	/* One bit of c_iflag: value is the mask. */
	TTY_KIND_INPUT = 2,

	/* One bit of c_oflag: value is the mask. */
	TTY_KIND_OUTPUT = 3,

	/* One bit of c_lflag: value is the mask. */
	TTY_KIND_LOCAL = 4,

	/* One bit of c_cflag: value is the mask. */
	TTY_KIND_CONTROL = 5,

	/* One of the character sizes in the CSIZE field of c_cflag: value is the size. */
	TTY_KIND_SIZE = 6
};

/* One row of the opcode table: the opcode, what it stands for, and its local flag mask, character index or size. */
struct tty_mode {
	unsigned char opcode;
	enum tty_kind kind;
	unsigned long value;
};

/* One row of the speed table: the local constant for a line speed and the speed in bits per second. */
struct tty_speed {
	speed_t constant;
	uint32_t bits_per_second;
};

/*
 * The table of opcodes, in ascending order of opcode.
 *
 * Values referenced:
 *   RFC 4254 section 8 (Encoding of Terminal Modes): opcodes 1 to 18 are
 *   the control characters, 30 to 41 the input flags, 50 to 62 the local
 *   flags, 70 to 75 the output flags, 90 to 93 the control flags;
 *   RFC 8160 section 3: opcode 42 is IUTF8.
 * A row exists only when the local <termios.h> has the name.  The last
 * row only keeps the array from being empty and is never read.
 */
static const struct tty_mode tty_modes[] = {
#ifdef VINTR
	{ 1, TTY_KIND_CHAR, VINTR },
#endif
#ifdef VQUIT
	{ 2, TTY_KIND_CHAR, VQUIT },
#endif
#ifdef VERASE
	{ 3, TTY_KIND_CHAR, VERASE },
#endif
#ifdef VKILL
	{ 4, TTY_KIND_CHAR, VKILL },
#endif
#ifdef VEOF
	{ 5, TTY_KIND_CHAR, VEOF },
#endif
#ifdef VEOL
	{ 6, TTY_KIND_CHAR, VEOL },
#endif
#ifdef VEOL2
	{ 7, TTY_KIND_CHAR, VEOL2 },
#endif
#ifdef VSTART
	{ 8, TTY_KIND_CHAR, VSTART },
#endif
#ifdef VSTOP
	{ 9, TTY_KIND_CHAR, VSTOP },
#endif
#ifdef VSUSP
	{ 10, TTY_KIND_CHAR, VSUSP },
#endif
#ifdef VDSUSP
	{ 11, TTY_KIND_CHAR, VDSUSP },
#endif
#ifdef VREPRINT
	{ 12, TTY_KIND_CHAR, VREPRINT },
#endif
#ifdef VWERASE
	{ 13, TTY_KIND_CHAR, VWERASE },
#endif
#ifdef VLNEXT
	{ 14, TTY_KIND_CHAR, VLNEXT },
#endif
#ifdef VFLUSH
	{ 15, TTY_KIND_CHAR, VFLUSH },
#endif
#if defined(VSWTCH)
	{ 16, TTY_KIND_CHAR, VSWTCH },
#elif defined(VSWTC)
	{ 16, TTY_KIND_CHAR, VSWTC },
#endif
#ifdef VSTATUS
	{ 17, TTY_KIND_CHAR, VSTATUS },
#endif
#ifdef VDISCARD
	{ 18, TTY_KIND_CHAR, VDISCARD },
#endif
#ifdef IGNPAR
	{ 30, TTY_KIND_INPUT, IGNPAR },
#endif
#ifdef PARMRK
	{ 31, TTY_KIND_INPUT, PARMRK },
#endif
#ifdef INPCK
	{ 32, TTY_KIND_INPUT, INPCK },
#endif
#ifdef ISTRIP
	{ 33, TTY_KIND_INPUT, ISTRIP },
#endif
#ifdef INLCR
	{ 34, TTY_KIND_INPUT, INLCR },
#endif
#ifdef IGNCR
	{ 35, TTY_KIND_INPUT, IGNCR },
#endif
#ifdef ICRNL
	{ 36, TTY_KIND_INPUT, ICRNL },
#endif
#ifdef IUCLC
	{ 37, TTY_KIND_INPUT, IUCLC },
#endif
#ifdef IXON
	{ 38, TTY_KIND_INPUT, IXON },
#endif
#ifdef IXANY
	{ 39, TTY_KIND_INPUT, IXANY },
#endif
#ifdef IXOFF
	{ 40, TTY_KIND_INPUT, IXOFF },
#endif
#ifdef IMAXBEL
	{ 41, TTY_KIND_INPUT, IMAXBEL },
#endif
#ifdef IUTF8
	{ 42, TTY_KIND_INPUT, IUTF8 },
#endif
#ifdef ISIG
	{ 50, TTY_KIND_LOCAL, ISIG },
#endif
#ifdef ICANON
	{ 51, TTY_KIND_LOCAL, ICANON },
#endif
#ifdef XCASE
	{ 52, TTY_KIND_LOCAL, XCASE },
#endif
#ifdef ECHO
	{ 53, TTY_KIND_LOCAL, ECHO },
#endif
#ifdef ECHOE
	{ 54, TTY_KIND_LOCAL, ECHOE },
#endif
#ifdef ECHOK
	{ 55, TTY_KIND_LOCAL, ECHOK },
#endif
#ifdef ECHONL
	{ 56, TTY_KIND_LOCAL, ECHONL },
#endif
#ifdef NOFLSH
	{ 57, TTY_KIND_LOCAL, NOFLSH },
#endif
#ifdef TOSTOP
	{ 58, TTY_KIND_LOCAL, TOSTOP },
#endif
#ifdef IEXTEN
	{ 59, TTY_KIND_LOCAL, IEXTEN },
#endif
#ifdef ECHOCTL
	{ 60, TTY_KIND_LOCAL, ECHOCTL },
#endif
#ifdef ECHOKE
	{ 61, TTY_KIND_LOCAL, ECHOKE },
#endif
#ifdef PENDIN
	{ 62, TTY_KIND_LOCAL, PENDIN },
#endif
#ifdef OPOST
	{ 70, TTY_KIND_OUTPUT, OPOST },
#endif
#ifdef OLCUC
	{ 71, TTY_KIND_OUTPUT, OLCUC },
#endif
#ifdef ONLCR
	{ 72, TTY_KIND_OUTPUT, ONLCR },
#endif
#ifdef OCRNL
	{ 73, TTY_KIND_OUTPUT, OCRNL },
#endif
#ifdef ONOCR
	{ 74, TTY_KIND_OUTPUT, ONOCR },
#endif
#ifdef ONLRET
	{ 75, TTY_KIND_OUTPUT, ONLRET },
#endif
#if defined(CSIZE) && defined(CS7)
	{ 90, TTY_KIND_SIZE, CS7 },
#endif
#if defined(CSIZE) && defined(CS8)
	{ 91, TTY_KIND_SIZE, CS8 },
#endif
#ifdef PARENB
	{ 92, TTY_KIND_CONTROL, PARENB },
#endif
#ifdef PARODD
	{ 93, TTY_KIND_CONTROL, PARODD },
#endif
	{ 0, TTY_KIND_CHAR, 0 }
};

/* The number of rows of the opcode table that stand for something (the last row is not one). */
#define TTY_MODE_COUNT	((sizeof(tty_modes) / sizeof(tty_modes[0])) - 1)

/*
 * The table of line speeds the opcodes 128 and 129 can carry.
 *
 * Values referenced:
 *   RFC 4254 section 8: opcodes 128 and 129 carry the input and output
 *   baud rate "in bits per second";
 *   POSIX <termios.h>: the Bnnn constants name the standard rates (B0,
 *   which means hang up, is left out on purpose).
 * A row exists only when the local <termios.h> has the constant.  The last
 * row only keeps the array from being empty and is never read.
 */
static const struct tty_speed tty_speeds[] = {
#ifdef B50
	{ B50, 50 },
#endif
#ifdef B75
	{ B75, 75 },
#endif
#ifdef B110
	{ B110, 110 },
#endif
#ifdef B150
	{ B150, 150 },
#endif
#ifdef B200
	{ B200, 200 },
#endif
#ifdef B300
	{ B300, 300 },
#endif
#ifdef B600
	{ B600, 600 },
#endif
#ifdef B1200
	{ B1200, 1200 },
#endif
#ifdef B1800
	{ B1800, 1800 },
#endif
#ifdef B2400
	{ B2400, 2400 },
#endif
#ifdef B4800
	{ B4800, 4800 },
#endif
#ifdef B9600
	{ B9600, 9600 },
#endif
#ifdef B19200
	{ B19200, 19200 },
#endif
#ifdef B38400
	{ B38400, 38400 },
#endif
#ifdef B57600
	{ B57600, 57600 },
#endif
#ifdef B115200
	{ B115200, 115200 },
#endif
#ifdef B230400
	{ B230400, 230400 },
#endif
#ifdef B460800
	{ B460800, 460800 },
#endif
#ifdef B500000
	{ B500000, 500000 },
#endif
#ifdef B576000
	{ B576000, 576000 },
#endif
#ifdef B921600
	{ B921600, 921600 },
#endif
#ifdef B1000000
	{ B1000000, 1000000 },
#endif
#ifdef B1152000
	{ B1152000, 1152000 },
#endif
#ifdef B1500000
	{ B1500000, 1500000 },
#endif
#ifdef B2000000
	{ B2000000, 2000000 },
#endif
#ifdef B2500000
	{ B2500000, 2500000 },
#endif
#ifdef B3000000
	{ B3000000, 3000000 },
#endif
#ifdef B3500000
	{ B3500000, 3500000 },
#endif
#ifdef B4000000
	{ B4000000, 4000000 },
#endif
	{ 0, 0 }
};

/* The number of rows of the speed table that stand for a speed (the last row is not one). */
#define TTY_SPEED_COUNT	((sizeof(tty_speeds) / sizeof(tty_speeds[0])) - 1)

static void put_mode(const struct tty_mode *mode, const struct termios *terminal, struct ssh_buf *out);
static void put_speed(unsigned opcode, speed_t speed, struct ssh_buf *out);
static void apply_mode(const struct tty_mode *mode, uint32_t argument, struct termios *terminal);
static void apply_speed(unsigned opcode, uint32_t bits_per_second, struct termios *terminal);
static const struct tty_mode *find_mode(unsigned opcode);

/*
 * Appends the encoded modes of terminal (every opcode of the table whose
 * flag or character exists locally, the two speeds in bits per second,
 * then TTY_OP_END) to out, as a raw sequence (the caller wraps it in a
 * string).
 */
void
ssh_ttymodes_put(
	const struct termios *terminal,
	struct ssh_buf *out)
{
	size_t index;
	speed_t input_speed;
	speed_t output_speed;

	/* Writes one (opcode, argument) pair for each row of the table. */
	for (index = 0;
	     index < TTY_MODE_COUNT;
	     index++) {
		put_mode(&tty_modes[index], terminal, out);
	}

	/* Writes the input and output speeds when they are standard rates. */
	input_speed = cfgetispeed(terminal);
	output_speed = cfgetospeed(terminal);
	put_speed(SSH_TTY_OP_ISPEED, input_speed, out);
	put_speed(SSH_TTY_OP_OSPEED, output_speed, out);

	/* Ends the run of pairs. */
	ssh_buf_put_u8(out, SSH_TTY_OP_END);
}

/*
 * Applies size bytes of encoded modes to terminal (which holds the
 * pseudo-terminal's current settings): each known opcode sets or clears
 * its flag (argument 0 clears, any other sets) or sets its character
 * (the argument's low 8 bits; 255 is taken as _POSIX_VDISABLE where that
 * is the local value), the speeds go to cfsetispeed and cfsetospeed when
 * they name a speed the local system has (otherwise they are ignored).
 *
 * The settings are worked out on a copy and stored only when the whole
 * string has been read without a fault, so a string that is cut inside an
 * argument leaves terminal as it was.
 */
int
ssh_ttymodes_apply(
	const uint8_t *modes,
	size_t size,
	struct termios *terminal)
{
	struct ssh_reader reader;
	struct termios work;
	const struct tty_mode *mode;
	unsigned opcode;
	uint32_t argument;
	size_t left;
	int reader_error;

	/* Refuses a string longer than the limit. */
	if (size > SSH_TTYMODES_MAX)
		return EINVAL;

	/* Refuses bytes that are missing. */
	if (modes == NULL && size != 0)
		return EINVAL;

	/* Works on a copy, so that a fault leaves the caller's settings untouched. */
	memcpy(&work, terminal, sizeof(work));
	ssh_reader_init(&reader, modes, size);

	/* Reads the pairs until the end opcode, an undefined opcode, or the end of the bytes. */
	for (;;) {
		left = ssh_reader_left(&reader);
		if (left == 0)
			break;

		opcode = ssh_reader_get_u8(&reader);

		/* The end opcode stops the run; any bytes after it are not read. */
		if (opcode == SSH_TTY_OP_END)
			break;

		/* An undefined opcode stops the run (RFC 4254 8). */
		if (opcode >= TTY_OP_UNDEFINED_FIRST)
			break;

		/* Every defined opcode has a uint32 argument, which must be all there. */
		argument = ssh_reader_get_u32(&reader);
		reader_error = ssh_reader_error(&reader);
		if (reader_error != 0)
			return EINVAL;

		/* The speeds are set apart from the table. */
		if (opcode == SSH_TTY_OP_ISPEED || opcode == SSH_TTY_OP_OSPEED) {
			apply_speed(opcode, argument, &work);
			continue;
		}

		/* An opcode this system has no name for is skipped. */
		mode = find_mode(opcode);
		if (mode == NULL)
			continue;

		/* Sets or clears the flag or the character the opcode stands for. */
		apply_mode(mode, argument, &work);
	}

	/* Stores the settings now that the whole string has been read. */
	memcpy(terminal, &work, sizeof(work));

	/* Succeeded: the modes are applied. */
	return 0;
}

/*
 * Appends the pair for one row of the table.
 */
static void
put_mode(
	const struct tty_mode *mode,
	const struct termios *terminal,
	struct ssh_buf *out)
{
	uint32_t argument;
	tcflag_t flags;

	argument = 0;
	flags = 0;

	/* Works out the argument from the part of the terminal the row stands for. */
	switch (mode->kind) {
	case TTY_KIND_CHAR:
		/* A character that is switched off is sent as 255, "none" in the RFC. */
		if (terminal->c_cc[mode->value] == TTY_LOCAL_DISABLED)
			argument = TTY_CHAR_NONE;
		else
			argument = terminal->c_cc[mode->value];
		break;
	case TTY_KIND_INPUT:
		flags = terminal->c_iflag;
		if ((flags & mode->value) != 0)
			argument = 1;
		break;
	case TTY_KIND_OUTPUT:
		flags = terminal->c_oflag;
		if ((flags & mode->value) != 0)
			argument = 1;
		break;
	case TTY_KIND_LOCAL:
		flags = terminal->c_lflag;
		if ((flags & mode->value) != 0)
			argument = 1;
		break;
	case TTY_KIND_CONTROL:
		flags = terminal->c_cflag;
		if ((flags & mode->value) != 0)
			argument = 1;
		break;
	case TTY_KIND_SIZE:
#ifdef CSIZE
		/* The size opcodes are TRUE when the character size field holds exactly their size. */
		flags = terminal->c_cflag & CSIZE;
		if (flags == mode->value)
			argument = 1;
#endif
		break;
	}

	/* Writes the opcode and its argument. */
	ssh_buf_put_u8(out, mode->opcode);
	ssh_buf_put_u32(out, argument);
}

/*
 * Appends the pair for a speed, or nothing when it is not a standard rate.
 */
static void
put_speed(
	unsigned opcode,
	speed_t speed,
	struct ssh_buf *out)
{
	size_t index;

	/* Looks for the speed among the standard rates and writes it in bits per second. */
	for (index = 0;
	     index < TTY_SPEED_COUNT;
	     index++) {
		if (tty_speeds[index].constant == speed) {
			ssh_buf_put_u8(out, (uint8_t)opcode);
			ssh_buf_put_u32(out, tty_speeds[index].bits_per_second);
			return;
		}
	}
}

/*
 * Sets or clears the flag, or sets the character, that one row stands for.
 */
static void
apply_mode(
	const struct tty_mode *mode,
	uint32_t argument,
	struct termios *terminal)
{
	unsigned char character;

	/* Changes the part of the terminal the row stands for. */
	switch (mode->kind) {
	case TTY_KIND_CHAR:
		/* The low 8 bits are the character; 255 means "none", which is the local disabled value. */
		character = (unsigned char)(argument & 0xffU);
		if (character == TTY_CHAR_NONE)
			terminal->c_cc[mode->value] = TTY_LOCAL_DISABLED;
		else
			terminal->c_cc[mode->value] = (cc_t)character;
		break;
	case TTY_KIND_INPUT:
		if (argument != 0)
			terminal->c_iflag |= (tcflag_t)mode->value;
		else
			terminal->c_iflag &= (tcflag_t)~mode->value;
		break;
	case TTY_KIND_OUTPUT:
		if (argument != 0)
			terminal->c_oflag |= (tcflag_t)mode->value;
		else
			terminal->c_oflag &= (tcflag_t)~mode->value;
		break;
	case TTY_KIND_LOCAL:
		if (argument != 0)
			terminal->c_lflag |= (tcflag_t)mode->value;
		else
			terminal->c_lflag &= (tcflag_t)~mode->value;
		break;
	case TTY_KIND_CONTROL:
		if (argument != 0)
			terminal->c_cflag |= (tcflag_t)mode->value;
		else
			terminal->c_cflag &= (tcflag_t)~mode->value;
		break;
	case TTY_KIND_SIZE:
#ifdef CSIZE
		/*
		 * A TRUE size opcode selects that character size.  A FALSE one
		 * says only what the size is not, so it leaves the field alone:
		 * the other size opcode, or the default, decides it.
		 */
		if (argument != 0) {
			terminal->c_cflag &= (tcflag_t)~CSIZE;
			terminal->c_cflag |= (tcflag_t)mode->value;
		}
#endif
		break;
	}
}

/*
 * Sets the input or output speed when bits_per_second is a standard rate
 * the local system has; any other value, including 0, is ignored.
 */
static void
apply_speed(
	unsigned opcode,
	uint32_t bits_per_second,
	struct termios *terminal)
{
	size_t index;
	int status;

	/* Looks for the rate among the standard rates. */
	for (index = 0;
	     index < TTY_SPEED_COUNT;
	     index++) {
		if (tty_speeds[index].bits_per_second != bits_per_second)
			continue;

		/* Sets the speed the opcode names; a system that refuses it keeps the old one. */
		if (opcode == SSH_TTY_OP_ISPEED)
			status = cfsetispeed(terminal, tty_speeds[index].constant);
		else
			status = cfsetospeed(terminal, tty_speeds[index].constant);

		/* The rate was found, so the search ends whatever the system answered. */
		(void)status;
		return;
	}
}

/*
 * Finds the row of an opcode, or NULL when this system has none.
 */
static const struct tty_mode *
find_mode(
	unsigned opcode)
{
	size_t index;

	/* Looks for the opcode in the table. */
	for (index = 0;
	     index < TTY_MODE_COUNT;
	     index++) {
		if (tty_modes[index].opcode == opcode)
			return &tty_modes[index];
	}

	/* Reports that the opcode is not in the table. */
	return NULL;
}
