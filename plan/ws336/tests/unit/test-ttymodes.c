/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-ttymodes.c.  The encoded modes are read back with a
 * small parser written here (opcode, uint32 pairs, RFC 4254 section 8),
 * and the streams given to ssh_ttymodes_apply are built with wire.h, so
 * the module is never tested only with bytes its own writer made.  The
 * opcode numbers in the checks are the numbers of RFC 4254 section 8 and
 * RFC 8160 section 3; the flag values are the local <termios.h> names.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "check.h"
#include "wire.h"
#include "ssh-ttymodes.h"

/* The stream wire.h builds for one call of ssh_ttymodes_apply; static because it is large. */
static struct wire stream;

/* The state of the pseudo-random generator of the random run, never zero. */
static uint32_t random_state = 2463534242UL;

static void test_put_raw(void);
static void test_put_cooked(void);
static void test_put_speeds(void);
static void test_put_limit(void);
static void test_roundtrip(void);
static void test_apply_flags_and_chars(void);
static void test_apply_stops(void);
static void test_apply_unknown_opcodes(void);
static void test_apply_truncated(void);
static void test_apply_limit(void);
static void test_apply_speeds(void);
static void test_apply_random(void);
static int find_argument(const struct ssh_buf *encoded, unsigned opcode, uint32_t *argument);
static int check_stream_shape(const struct ssh_buf *encoded);
static void make_cooked(struct termios *terminal);
static void make_raw(struct termios *terminal);
static uint32_t next_random(void);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_put_raw();
	test_put_cooked();
	test_put_speeds();
	test_put_limit();
	test_roundtrip();
	test_apply_flags_and_chars();
	test_apply_stops();
	test_apply_unknown_opcodes();
	test_apply_truncated();
	test_apply_limit();
	test_apply_speeds();
	test_apply_random();

	/* Reports the result of the whole test. */
	return check_finish("ttymodes");
}

/*
 * Fills terminal with a typical interactive (cooked) state: the usual
 * flags and control characters, 38400 baud in and out.
 */
static void
make_cooked(
	struct termios *terminal)
{
	memset(terminal, 0, sizeof(*terminal));
	terminal->c_iflag = ICRNL | IXON | IGNPAR;
	terminal->c_oflag = OPOST;
	terminal->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
	terminal->c_cflag = CS8 | CREAD;
	terminal->c_cc[VINTR] = 3;
	terminal->c_cc[VQUIT] = 28;
	terminal->c_cc[VERASE] = 127;
	terminal->c_cc[VKILL] = 21;
	terminal->c_cc[VEOF] = 4;
	terminal->c_cc[VSTART] = 17;
	terminal->c_cc[VSTOP] = 19;
	terminal->c_cc[VSUSP] = 26;
	terminal->c_cc[VEOL] = _POSIX_VDISABLE;
	cfsetispeed(terminal, B38400);
	cfsetospeed(terminal, B38400);
}

/*
 * Fills terminal with the state cfmakeraw gives to a cooked one.
 */
static void
make_raw(
	struct termios *terminal)
{
	make_cooked(terminal);
	cfmakeraw(terminal);
}

/*
 * Reads the encoded modes pair by pair, as RFC 4254 section 8 describes
 * them, and finds the argument of an opcode.  Returns 1 when the opcode
 * is there (its argument in *argument), 0 when not.
 */
static int
find_argument(
	const struct ssh_buf *encoded,
	unsigned opcode,
	uint32_t *argument)
{
	size_t offset;

	offset = 0;

	/* Walks the pairs up to the end opcode. */
	while (offset < encoded->size && encoded->data[offset] != 0) {
		if (offset + 5 > encoded->size)
			return 0;
		if (encoded->data[offset] == opcode) {
			*argument = ((uint32_t)encoded->data[offset + 1] << 24) |
			    ((uint32_t)encoded->data[offset + 2] << 16) |
			    ((uint32_t)encoded->data[offset + 3] << 8) |
			    (uint32_t)encoded->data[offset + 4];
			return 1;
		}
		offset += 5;
	}
	return 0;
}

/*
 * Checks that the encoded modes are whole pairs with strictly ascending
 * opcodes below 160, ended by a single 0 as the last byte.  Returns the
 * number of pairs, or -1 when the shape is wrong.
 */
static int
check_stream_shape(
	const struct ssh_buf *encoded)
{
	size_t offset;
	int count;
	int previous;

	offset = 0;
	count = 0;
	previous = 0;

	/* Walks the pairs. */
	while (offset < encoded->size && encoded->data[offset] != 0) {
		if (offset + 5 > encoded->size)
			return -1;
		if ((int)encoded->data[offset] <= previous)
			return -1;
		if (encoded->data[offset] >= 160)
			return -1;
		previous = encoded->data[offset];
		offset += 5;
		count++;
	}

	/* The end opcode must be the last byte. */
	if (offset + 1 != encoded->size)
		return -1;
	return count;
}

/*
 * A raw terminal: the flags cfmakeraw clears are sent as 0, CS8 as 1.
 */
static void
test_put_raw(void)
{
	struct termios terminal;
	struct ssh_buf encoded;
	uint32_t argument;

	make_raw(&terminal);
	ssh_buf_init(&encoded, 4096, 0);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK_EQ(ssh_buf_error(&encoded), 0);
	CHECK(check_stream_shape(&encoded) > 20);

	/* RFC 4254 section 8: 50 ISIG, 51 ICANON, 53 ECHO, 59 IEXTEN, 70 OPOST are off in a raw terminal. */
	CHECK(find_argument(&encoded, 50, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 51, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 53, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 59, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 70, &argument) == 1 && argument == 0);

	/* 91 CS8 is on, 90 CS7 and 92 PARENB are off. */
	CHECK(find_argument(&encoded, 91, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 90, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 92, &argument) == 1 && argument == 0);

	/* 36 ICRNL and 38 IXON are off after cfmakeraw. */
	CHECK(find_argument(&encoded, 36, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 38, &argument) == 1 && argument == 0);

	ssh_buf_free(&encoded);
}

/*
 * A cooked terminal: the flags set are 1, the others 0, the characters
 * are their values, a disabled character is 255.
 */
static void
test_put_cooked(void)
{
	struct termios terminal;
	struct ssh_buf encoded;
	uint32_t argument;

	make_cooked(&terminal);
	ssh_buf_init(&encoded, 4096, 0);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK_EQ(ssh_buf_error(&encoded), 0);
	CHECK(check_stream_shape(&encoded) > 20);

	/* RFC 4254 section 8: 1 VINTR, 2 VQUIT, 3 VERASE, 4 VKILL, 5 VEOF, 8 VSTART, 9 VSTOP, 10 VSUSP carry the characters. */
	CHECK(find_argument(&encoded, 1, &argument) == 1 && argument == 3);
	CHECK(find_argument(&encoded, 2, &argument) == 1 && argument == 28);
	CHECK(find_argument(&encoded, 3, &argument) == 1 && argument == 127);
	CHECK(find_argument(&encoded, 4, &argument) == 1 && argument == 21);
	CHECK(find_argument(&encoded, 5, &argument) == 1 && argument == 4);
	CHECK(find_argument(&encoded, 8, &argument) == 1 && argument == 17);
	CHECK(find_argument(&encoded, 9, &argument) == 1 && argument == 19);
	CHECK(find_argument(&encoded, 10, &argument) == 1 && argument == 26);

	/* 6 VEOL is switched off: "255 if none". */
	CHECK(find_argument(&encoded, 6, &argument) == 1 && argument == 255);

	/* 30 IGNPAR on, 31 PARMRK off, 36 ICRNL on, 38 IXON on, 40 IXOFF off. */
	CHECK(find_argument(&encoded, 30, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 31, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 36, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 38, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 40, &argument) == 1 && argument == 0);

	/* 50 ISIG, 51 ICANON, 53 ECHO, 54 ECHOE, 55 ECHOK, 59 IEXTEN on; 56 ECHONL, 57 NOFLSH, 58 TOSTOP off. */
	CHECK(find_argument(&encoded, 50, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 51, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 53, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 54, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 55, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 59, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 56, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 57, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 58, &argument) == 1 && argument == 0);

	/* 70 OPOST on, 75 ONLRET off. */
	CHECK(find_argument(&encoded, 70, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 75, &argument) == 1 && argument == 0);

	/* 91 CS8 on, 90 CS7 off, 92 PARENB off, 93 PARODD off. */
	CHECK(find_argument(&encoded, 91, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 90, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 92, &argument) == 1 && argument == 0);
	CHECK(find_argument(&encoded, 93, &argument) == 1 && argument == 0);

#ifdef IUTF8
	/* RFC 8160 section 3: 42 IUTF8, 0 until it is set, then 1. */
	CHECK(find_argument(&encoded, 42, &argument) == 1 && argument == 0);
	ssh_buf_reset(&encoded);
	terminal.c_iflag |= IUTF8;
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 42, &argument) == 1 && argument == 1);
#endif

	/* A 7-bit terminal: 90 CS7 on and 91 CS8 off. */
	ssh_buf_reset(&encoded);
	terminal.c_cflag = (terminal.c_cflag & ~CSIZE) | CS7;
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 90, &argument) == 1 && argument == 1);
	CHECK(find_argument(&encoded, 91, &argument) == 1 && argument == 0);

	ssh_buf_free(&encoded);
}

/*
 * The speeds: 128 is the input and 129 the output speed, in bits per
 * second; B0 is left out.
 */
static void
test_put_speeds(void)
{
	struct termios terminal;
	struct ssh_buf encoded;
	uint32_t argument;

	make_cooked(&terminal);

	/* 38400 both ways (some systems keep one speed field for both directions, so the two are set alike). */
	cfsetispeed(&terminal, B38400);
	cfsetospeed(&terminal, B38400);
	ssh_buf_init(&encoded, 4096, 0);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 128, &argument) == 1 && argument == 38400);
	CHECK(find_argument(&encoded, 129, &argument) == 1 && argument == 38400);
	CHECK(check_stream_shape(&encoded) > 20);

	/* 9600 out: 129 carries it. */
	ssh_buf_reset(&encoded);
	cfsetospeed(&terminal, B9600);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 129, &argument) == 1 && argument == 9600);

	/* 115200 both ways. */
	ssh_buf_reset(&encoded);
	cfsetispeed(&terminal, B115200);
	cfsetospeed(&terminal, B115200);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 128, &argument) == 1 && argument == 115200);
	CHECK(find_argument(&encoded, 129, &argument) == 1 && argument == 115200);

	/* B0 (hang up) is not a speed to send. */
	ssh_buf_reset(&encoded);
	cfsetospeed(&terminal, B0);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 129, &argument) == 0);

#if defined(__linux__) && defined(CBAUD) && defined(CBAUDEX)
	/* A speed field no standard rate has (the extended bit alone) is left out. */
	ssh_buf_reset(&encoded);
	cfsetispeed(&terminal, B9600);
	terminal.c_cflag = (terminal.c_cflag & ~(tcflag_t)(CBAUD | CBAUDEX)) | CBAUDEX;
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK(find_argument(&encoded, 129, &argument) == 0);
	CHECK(check_stream_shape(&encoded) > 20);
#endif

	ssh_buf_free(&encoded);
}

/*
 * A buffer with a small limit records the error and the call returns.
 */
static void
test_put_limit(void)
{
	struct termios terminal;
	struct ssh_buf encoded;

	make_cooked(&terminal);
	ssh_buf_init(&encoded, 12, 0);
	ssh_ttymodes_put(&terminal, &encoded);
	CHECK_EQ(ssh_buf_error(&encoded), EMSGSIZE);
	CHECK(encoded.size <= 12);
	ssh_buf_free(&encoded);
}

/*
 * What put writes, apply reads back: a terminal in the opposite state
 * takes on the flags, the characters and the speeds of the source.
 */
static void
test_roundtrip(void)
{
	struct termios source;
	struct termios target;
	struct ssh_buf encoded;
	tcflag_t input_mask;
	tcflag_t output_mask;
	tcflag_t local_mask;
	tcflag_t control_mask;
	int pass;

	input_mask = ICRNL | IGNCR | INLCR | IGNPAR | PARMRK | INPCK | ISTRIP | IXON | IXOFF;
	output_mask = OPOST;
	local_mask = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL | NOFLSH | TOSTOP | IEXTEN;
	control_mask = PARENB | PARODD;

	/* Once from a cooked terminal onto a raw one, once the other way round. */
	for (pass = 0; pass < 2; pass++) {
		if (pass == 0) {
			make_cooked(&source);
			make_raw(&target);
		} else {
			make_raw(&source);
			make_cooked(&target);
			source.c_cc[VINTR] = 5;
			source.c_cc[VEOF] = 9;
			source.c_cc[VEOL] = 200;
			cfsetispeed(&source, B9600);
			cfsetospeed(&source, B9600);
		}

		ssh_buf_init(&encoded, 4096, 0);
		ssh_ttymodes_put(&source, &encoded);
		CHECK_EQ(ssh_ttymodes_apply(encoded.data, encoded.size, &target), 0);

		CHECK_EQ(target.c_iflag & input_mask, source.c_iflag & input_mask);
		CHECK_EQ(target.c_oflag & output_mask, source.c_oflag & output_mask);
		CHECK_EQ(target.c_lflag & local_mask, source.c_lflag & local_mask);
		CHECK_EQ(target.c_cflag & control_mask, source.c_cflag & control_mask);
		CHECK_EQ(target.c_cflag & CSIZE, source.c_cflag & CSIZE);
		CHECK_EQ(target.c_cc[VINTR], source.c_cc[VINTR]);
		CHECK_EQ(target.c_cc[VQUIT], source.c_cc[VQUIT]);
		CHECK_EQ(target.c_cc[VERASE], source.c_cc[VERASE]);
		CHECK_EQ(target.c_cc[VKILL], source.c_cc[VKILL]);
		CHECK_EQ(target.c_cc[VEOF], source.c_cc[VEOF]);
		CHECK_EQ(target.c_cc[VEOL], source.c_cc[VEOL]);
		CHECK_EQ(target.c_cc[VSTART], source.c_cc[VSTART]);
		CHECK_EQ(target.c_cc[VSTOP], source.c_cc[VSTOP]);
		CHECK_EQ(target.c_cc[VSUSP], source.c_cc[VSUSP]);
		CHECK_EQ(cfgetispeed(&target), cfgetispeed(&source));
		CHECK_EQ(cfgetospeed(&target), cfgetospeed(&source));
		ssh_buf_free(&encoded);
	}
}

/*
 * Flags are set by a non-zero argument and cleared by 0; characters take
 * the low 8 bits, and 255 is the disabled character.
 */
static void
test_apply_flags_and_chars(void)
{
	struct termios terminal;

	make_cooked(&terminal);

	/* RFC 4254 section 8: 53 ECHO off, 50 ISIG kept on, 56 ECHONL on (any non-zero value sets). */
	wire_reset(&stream);
	wire_u8(&stream, 53);
	wire_u32(&stream, 0);
	wire_u8(&stream, 50);
	wire_u32(&stream, 1);
	wire_u8(&stream, 56);
	wire_u32(&stream, 0x80000000UL);
	wire_u8(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_lflag & ECHO, 0);
	CHECK(terminal.c_lflag & ISIG);
	CHECK(terminal.c_lflag & ECHONL);
	CHECK(terminal.c_lflag & ICANON);

	/* 36 ICRNL off, 34 INLCR on; 70 OPOST off, 72 ONLCR on. */
	wire_reset(&stream);
	wire_u8(&stream, 36);
	wire_u32(&stream, 0);
	wire_u8(&stream, 34);
	wire_u32(&stream, 1);
	wire_u8(&stream, 70);
	wire_u32(&stream, 0);
	wire_u8(&stream, 72);
	wire_u32(&stream, 1);
	wire_u8(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_iflag & ICRNL, 0);
	CHECK(terminal.c_iflag & INLCR);
	CHECK_EQ(terminal.c_oflag & OPOST, 0);
	CHECK(terminal.c_oflag & ONLCR);

	/* 92 PARENB on, 93 PARODD on. */
	wire_reset(&stream);
	wire_u8(&stream, 92);
	wire_u32(&stream, 1);
	wire_u8(&stream, 93);
	wire_u32(&stream, 1);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK(terminal.c_cflag & PARENB);
	CHECK(terminal.c_cflag & PARODD);

	/* 90 CS7 on selects 7 bits; 91 CS8 off then leaves it; 91 CS8 on goes back to 8. */
	wire_reset(&stream);
	wire_u8(&stream, 90);
	wire_u32(&stream, 1);
	wire_u8(&stream, 91);
	wire_u32(&stream, 0);
	wire_u8(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_cflag & CSIZE, CS7);
	wire_reset(&stream);
	wire_u8(&stream, 90);
	wire_u32(&stream, 0);
	wire_u8(&stream, 91);
	wire_u32(&stream, 1);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_cflag & CSIZE, CS8);

	/* Characters: 1 VINTR 0x05, 3 VERASE 8, 5 VEOF with high bits ignored (0x104 is 4), 6 VEOL to 255 (none). */
	wire_reset(&stream);
	wire_u8(&stream, 1);
	wire_u32(&stream, 5);
	wire_u8(&stream, 3);
	wire_u32(&stream, 8);
	wire_u8(&stream, 5);
	wire_u32(&stream, 0x104);
	wire_u8(&stream, 6);
	wire_u32(&stream, 255);
	wire_u8(&stream, 4);
	wire_u32(&stream, 0xffffff00UL | 22);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_cc[VINTR], 5);
	CHECK_EQ(terminal.c_cc[VERASE], 8);
	CHECK_EQ(terminal.c_cc[VEOF], 4);
	CHECK_EQ(terminal.c_cc[VEOL], _POSIX_VDISABLE);
	CHECK_EQ(terminal.c_cc[VKILL], 22);

	/* A character given as 0x1ff has 255 in its low 8 bits: disabled. */
	wire_reset(&stream);
	wire_u8(&stream, 2);
	wire_u32(&stream, 0x1ff);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_cc[VQUIT], _POSIX_VDISABLE);

#ifdef IUTF8
	/* RFC 8160 section 3: 42 IUTF8 on, then off. */
	wire_reset(&stream);
	wire_u8(&stream, 42);
	wire_u32(&stream, 1);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK(terminal.c_iflag & IUTF8);
	wire_reset(&stream);
	wire_u8(&stream, 42);
	wire_u32(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_iflag & IUTF8, 0);
#endif
}

/*
 * The end opcode stops at once; an opcode of 160 or more stops parsing
 * and what follows is not read, however it is shaped.
 */
static void
test_apply_stops(void)
{
	struct termios terminal;
	struct termios before;
	unsigned opcode;

	/* Opcode 0 ends the modes: the pair after it is not applied. */
	make_cooked(&terminal);
	wire_reset(&stream);
	wire_u8(&stream, 50);
	wire_u32(&stream, 0);
	wire_u8(&stream, 0);
	wire_u8(&stream, 53);
	wire_u32(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_lflag & ISIG, 0);
	CHECK(terminal.c_lflag & ECHO);

	/* RFC 4254 section 8: opcodes 160 to 255 cause parsing to stop; 160, 207 and 254 are tried. */
	for (opcode = 160; opcode <= 255; opcode += 47) {
		make_cooked(&terminal);
		wire_reset(&stream);
		wire_u8(&stream, 50);
		wire_u32(&stream, 0);
		wire_u8(&stream, opcode);
		wire_u32(&stream, 0xdeadbeefUL);
		wire_u8(&stream, 53);
		wire_u32(&stream, 0);
		CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
		CHECK_EQ(terminal.c_lflag & ISIG, 0);
		CHECK(terminal.c_lflag & ECHO);
	}

	/* An undefined opcode with no argument bytes at all is not a truncated pair. */
	make_cooked(&terminal);
	wire_reset(&stream);
	wire_u8(&stream, 53);
	wire_u32(&stream, 0);
	wire_u8(&stream, 160);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_lflag & ECHO, 0);

	/* No bytes at all changes nothing. */
	make_cooked(&terminal);
	before = terminal;
	CHECK_EQ(ssh_ttymodes_apply(NULL, 0, &terminal), 0);
	CHECK_BYTES(&terminal, &before, sizeof(terminal));
	CHECK_EQ(ssh_ttymodes_apply(stream.data, 0, &terminal), 0);
	CHECK_BYTES(&terminal, &before, sizeof(terminal));

	/* The end of the bytes without opcode 0 ends the modes too (RFC 4254 does not require the end opcode of a short string). */
	wire_reset(&stream);
	wire_u8(&stream, 53);
	wire_u32(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_lflag & ECHO, 0);
}

/*
 * Opcodes 1 to 159 that are not in the table are skipped with their
 * argument, and the pairs after them still apply.
 */
static void
test_apply_unknown_opcodes(void)
{
	struct termios terminal;
	unsigned unknown[8];
	unsigned index;

	/* RFC 4254 section 8 leaves 19 to 29, 43 to 49, 63 to 69, 76 to 89, 94 to 127 and 130 to 159 undefined. */
	unknown[0] = 19;
	unknown[1] = 29;
	unknown[2] = 43;
	unknown[3] = 69;
	unknown[4] = 76;
	unknown[5] = 94;
	unknown[6] = 127;
	unknown[7] = 159;

	for (index = 0; index < 8; index++) {
		make_cooked(&terminal);
		wire_reset(&stream);
		wire_u8(&stream, unknown[index]);
		wire_u32(&stream, 0x12345678UL);
		wire_u8(&stream, 53);
		wire_u32(&stream, 0);
		wire_u8(&stream, 0);
		CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
		CHECK_EQ(terminal.c_lflag & ECHO, 0);
		CHECK(terminal.c_lflag & ISIG);
	}

	/* An unknown opcode whose argument is cut is still a fault. */
	make_cooked(&terminal);
	wire_reset(&stream);
	wire_u8(&stream, 99);
	wire_u8(&stream, 0);
	wire_u8(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), EINVAL);
}

/*
 * An argument cut short is EINVAL and nothing is changed, not even by the
 * valid pairs before it.
 */
static void
test_apply_truncated(void)
{
	struct termios terminal;
	struct termios before;
	size_t cut;

	/* Valid pairs, then a pair cut after 0, 1, 2 and 3 bytes of its argument. */
	for (cut = 0; cut < 4; cut++) {
		make_cooked(&terminal);
		before = terminal;
		wire_reset(&stream);
		wire_u8(&stream, 53);
		wire_u32(&stream, 0);
		wire_u8(&stream, 50);
		wire_u32(&stream, 0);
		wire_u8(&stream, 129);
		wire_u32(&stream, 9600);
		wire_u8(&stream, 1);
		wire_bytes(&stream, "\x00\x00\x00\x05", cut);
		CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), EINVAL);
		CHECK_BYTES(&terminal, &before, sizeof(terminal));
	}

	/* The same with the complete pair: accepted. */
	wire_reset(&stream);
	wire_u8(&stream, 1);
	wire_u32(&stream, 5);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_cc[VINTR], 5);

	/* A cut speed argument is a fault too. */
	make_cooked(&terminal);
	before = terminal;
	wire_reset(&stream);
	wire_u8(&stream, 128);
	wire_bytes(&stream, "\x00\x00", 2);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), EINVAL);
	CHECK_BYTES(&terminal, &before, sizeof(terminal));
}

/*
 * 1024 bytes are accepted, 1025 are EINVAL with nothing changed.
 */
static void
test_apply_limit(void)
{
	struct termios terminal;
	struct termios before;
	size_t pairs;

	/* 1024 bytes: ECHO off, 203 skipped pairs of an unknown opcode, then 9 bytes of zero (ended by the first). */
	make_cooked(&terminal);
	wire_reset(&stream);
	wire_u8(&stream, 53);
	wire_u32(&stream, 0);
	for (pairs = 0; pairs < 203; pairs++) {
		wire_u8(&stream, 99);
		wire_u32(&stream, (uint32_t)pairs);
	}
	while (stream.size < 1024)
		wire_u8(&stream, 0);
	CHECK_EQ(stream.size, SSH_TTYMODES_MAX);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(terminal.c_lflag & ECHO, 0);

	/* 1025 bytes: EINVAL and the terminal is not changed, although the first pair is valid. */
	make_cooked(&terminal);
	before = terminal;
	wire_u8(&stream, 0);
	CHECK_EQ(stream.size, SSH_TTYMODES_MAX + 1);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), EINVAL);
	CHECK_BYTES(&terminal, &before, sizeof(terminal));

	/* A very large size is refused without reading. */
	CHECK_EQ(ssh_ttymodes_apply(stream.data, (size_t)-1, &terminal), EINVAL);
	CHECK_BYTES(&terminal, &before, sizeof(terminal));
}

/*
 * The speeds: 128 and 129 set the input and output speed when they are
 * standard rates; an unknown rate and 0 are ignored.
 */
static void
test_apply_speeds(void)
{
	struct termios terminal;

	/* 38400 in, given in bits per second as RFC 4254 section 8 says. */
	make_cooked(&terminal);
	cfsetispeed(&terminal, B9600);
	cfsetospeed(&terminal, B9600);
	wire_reset(&stream);
	wire_u8(&stream, 128);
	wire_u32(&stream, 38400);
	wire_u8(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetispeed(&terminal), B38400);

	/* 9600 out. */
	wire_reset(&stream);
	wire_u8(&stream, 129);
	wire_u32(&stream, 9600);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetospeed(&terminal), B9600);

	/* An unknown rate (12345) changes neither speed. */
	wire_reset(&stream);
	wire_u8(&stream, 128);
	wire_u32(&stream, 12345);
	wire_u8(&stream, 129);
	wire_u32(&stream, 12345);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetospeed(&terminal), B9600);

	/* 0 would be a hang-up: ignored. */
	wire_reset(&stream);
	wire_u8(&stream, 128);
	wire_u32(&stream, 0);
	wire_u8(&stream, 129);
	wire_u32(&stream, 0);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetospeed(&terminal), B9600);

	/* The largest argument is no rate: ignored. */
	wire_reset(&stream);
	wire_u8(&stream, 129);
	wire_u32(&stream, 0xffffffffUL);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetospeed(&terminal), B9600);

	/* 115200 out. */
	wire_reset(&stream);
	wire_u8(&stream, 129);
	wire_u32(&stream, 115200);
	CHECK_EQ(ssh_ttymodes_apply(stream.data, stream.size, &terminal), 0);
	CHECK_EQ(cfgetospeed(&terminal), B115200);
}

/*
 * Random bytes in every shape: the answer is 0 or EINVAL, and EINVAL
 * leaves the terminal as it was.
 */
static void
test_apply_random(void)
{
	struct termios terminal;
	struct termios before;
	uint8_t bytes[SSH_TTYMODES_MAX + 8];
	size_t size;
	size_t index;
	int round;
	int error;

	/* Runs many random strings under the sanitizers. */
	for (round = 0; round < 20000; round++) {
		size = next_random() % (sizeof(bytes) + 1);
		for (index = 0; index < size; index++) {
			/* Opcodes are drawn from a small set so that known pairs, unknown pairs and stops all happen. */
			if (next_random() % 3 == 0)
				bytes[index] = (uint8_t)(next_random() % 256);
			else
				bytes[index] = (uint8_t)(next_random() % 4);
		}
		make_cooked(&terminal);
		before = terminal;
		error = ssh_ttymodes_apply(bytes, size, &terminal);
		CHECK(error == 0 || error == EINVAL);
		if (error == EINVAL)
			CHECK(memcmp(&terminal, &before, sizeof(terminal)) == 0);
		if (size > SSH_TTYMODES_MAX)
			CHECK_EQ(error, EINVAL);
	}
}

/*
 * Steps the pseudo-random generator (xorshift32) and returns the next value.
 */
static uint32_t
next_random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
