/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The escape-sequence reader of the virtual consoles.
 *
 * The console understands a small part of ANSI X3.64: the cursor
 * movements, the erases, the cursor position with a row and a column, and
 * the graphic rendition (bold, reverse and eight colours).  This reader
 * turns the byte stream into those commands and keeps the rendition a
 * console writes with.  It touches no display and no kernel state, so the
 * same file is compiled into a host test.
 */

#ifndef KERN_TTY_CSI_H
#define KERN_TTY_CSI_H

#include <stdint.h>

/*
 * The number of parameters a sequence keeps.  Later parameters are read
 * and dropped, which no command the console knows needs.
 */
#define TTY_CSI_PARAMETER_MAX 16U

/* The largest parameter value is kept below this bound times ten. */
#define TTY_CSI_VALUE_LIMIT 1000U

/* The ANSI colour a console shows text in until a sequence says otherwise. */
#define TTY_CSI_FOREGROUND_DEFAULT 7U

/* The ANSI colour a console shows behind text until a sequence says otherwise. */
#define TTY_CSI_BACKGROUND_DEFAULT 0U

/*
 * Where the reader is inside a sequence.
 *
 * GROUND reads text, ESCAPE has seen an escape byte and waits for the
 * byte that tells what follows, and SEQUENCE reads the parameters of a
 * control sequence up to its final byte.
 */
enum tty_csi_state {
	TTY_CSI_GROUND,
	TTY_CSI_ESCAPE,
	TTY_CSI_SEQUENCE
};

/*
 * What one byte meant to the reader, and so what the console does with it.
 *
 * PLAIN is text to show.  PENDING was taken into an unfinished sequence.
 * LITERAL is an escape that does not start a control sequence: the
 * console shows the escape byte and then this byte as text.  COMMAND
 * finished a sequence the console carries out, and DISCARDED finished
 * one the console does not know.
 */
enum tty_csi_result {
	TTY_CSI_PLAIN,
	TTY_CSI_PENDING,
	TTY_CSI_LITERAL,
	TTY_CSI_COMMAND,
	TTY_CSI_DISCARDED
};

/*
 * The reading state of one console's output stream.
 *
 * One parser belongs to each virtual console and lives as long as the
 * console.  A sequence may be split across writes, so the parameters read
 * so far stay here between calls.  After a COMMAND result, command and the
 * parameters describe the finished sequence until the next byte is fed.
 */
struct tty_csi_parser {
	enum tty_csi_state state;

	/* The value of each parameter, zero where no digit was written. */
	unsigned values[TTY_CSI_PARAMETER_MAX];

	/* One where the parameter had at least one digit, zero where it was left empty. */
	unsigned present[TTY_CSI_PARAMETER_MAX];

	/* The parameter the next digit belongs to: the number of separators read. */
	unsigned current;

	/* One once the sequence holds a byte the console does not understand. */
	unsigned ignored;

	/* The final byte of the finished sequence. */
	unsigned command;
};

/*
 * The graphic rendition one console writes text with.
 *
 * Colours are ANSI numbers (0 black, 1 red, ... 7 white), not the bit
 * order of the display's attribute byte.  The rendition lives with its
 * console and changes only through a select-graphic-rendition command.
 */
struct tty_csi_attribute {
	unsigned foreground;
	unsigned background;
	unsigned bold;
	unsigned reverse;
};

void
tty_csi_reset(
	struct tty_csi_parser *parser);

enum tty_csi_result
tty_csi_feed(
	struct tty_csi_parser *parser,
	unsigned char byte);

unsigned
tty_csi_parameter_count(
	const struct tty_csi_parser *parser);

int
tty_csi_parameter_present(
	const struct tty_csi_parser *parser,
	unsigned index);

unsigned
tty_csi_parameter(
	const struct tty_csi_parser *parser,
	unsigned index,
	unsigned fallback);

void
tty_csi_position(
	const struct tty_csi_parser *parser,
	unsigned rows,
	unsigned columns,
	unsigned *row,
	unsigned *column);

void
tty_csi_attribute_reset(
	struct tty_csi_attribute *attribute);

void
tty_csi_attribute_apply(
	struct tty_csi_attribute *attribute,
	const struct tty_csi_parser *parser);

uint8_t
tty_csi_attribute_byte(
	const struct tty_csi_attribute *attribute);

#endif
