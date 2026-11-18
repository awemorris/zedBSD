/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The escape-sequence reader of the virtual consoles.
 *
 * Bytes come in one at a time, possibly split across writes.  A control
 * sequence is ESC, '[', parameters separated by ';', and one final byte.
 * A sequence with a private marker, a sub-parameter or an intermediate
 * byte is read to its end and dropped, so it never shows up as text.
 * The rendition is kept in ANSI colour numbers and turned into the
 * display's attribute byte only when text is written.
 */

#include "tty-csi.h"

/* The byte that opens every escape sequence. */
#define TTY_CSI_ESCAPE_BYTE 0x1bU

/* The selection that follows an extended colour introducer names a palette index. */
#define TTY_CSI_EXTENDED_INDEXED 5U

/* The selection that follows an extended colour introducer names red, green and blue. */
#define TTY_CSI_EXTENDED_DIRECT 2U

/*
 * The display's colour number for each ANSI colour number.
 *
 * ANSI counts red as bit 0 and blue as bit 2; the display's attribute
 * byte (the VGA one, which every board's text output takes) counts blue
 * as bit 0 and red as bit 2.  The table never changes.
 */
static const uint8_t tty_csi_display_colors[8] = {
	0x0U,
	0x4U,
	0x2U,
	0x6U,
	0x1U,
	0x5U,
	0x3U,
	0x7U,
};

static void tty_csi_begin(struct tty_csi_parser *parser);
static void tty_csi_add_digit(struct tty_csi_parser *parser, unsigned char byte);
static unsigned tty_csi_coordinate(const struct tty_csi_parser *parser, unsigned index, unsigned limit);
static unsigned tty_csi_extended_length(const struct tty_csi_parser *parser, unsigned index);

/*
 * Returns a parser to reading text.
 *
 * A console that redraws from its history starts here, so a sequence cut
 * in half by the history's start does not swallow the text after it.
 */
void
tty_csi_reset(
	struct tty_csi_parser *parser)
{
	/* Forgets any open sequence and its parameters. */
	tty_csi_begin(parser);
	parser->state = TTY_CSI_GROUND;
}

/*
 * Reads one output byte and reports what the console does with it.
 */
enum tty_csi_result
tty_csi_feed(
	struct tty_csi_parser *parser,
	unsigned char byte)
{
	/* Outside a sequence, every byte but an escape is text. */
	if (parser->state == TTY_CSI_GROUND) {
		/* Shows the byte as text. */
		if (byte != TTY_CSI_ESCAPE_BYTE)
			return TTY_CSI_PLAIN;

		/* Waits for the byte that says what the escape starts. */
		parser->state = TTY_CSI_ESCAPE;
		return TTY_CSI_PENDING;
	}

	/* After an escape, only an opening bracket starts a control sequence. */
	if (parser->state == TTY_CSI_ESCAPE) {
		/* Shows the escape and this byte as text, as the console always has. */
		if (byte != '[') {
			parser->state = TTY_CSI_GROUND;
			return TTY_CSI_LITERAL;
		}

		/* Starts a control sequence with no parameters yet. */
		tty_csi_begin(parser);
		parser->state = TTY_CSI_SEQUENCE;
		return TTY_CSI_PENDING;
	}

	/* A digit adds to the parameter being read. */
	if (byte >= '0' && byte <= '9') {
		tty_csi_add_digit(parser, byte);
		return TTY_CSI_PENDING;
	}

	/*
	 * A semicolon ends one parameter and starts the next.  Past the last
	 * kept parameter the count stops, and later digits are dropped.
	 */
	if (byte == ';') {
		if (parser->current < TTY_CSI_PARAMETER_MAX)
			parser->current++;
		return TTY_CSI_PENDING;
	}

	/*
	 * A private marker, a sub-parameter separator or an intermediate byte
	 * makes a sequence the console does not know; it is read to its end.
	 */
	if (byte >= 0x20U && byte <= 0x3fU) {
		parser->ignored = 1;
		return TTY_CSI_PENDING;
	}

	/* Any other byte finishes the sequence. */
	parser->state = TTY_CSI_GROUND;
	parser->command = byte;

	/* Drops a sequence the console does not know. */
	if (parser->ignored)
		return TTY_CSI_DISCARDED;

	/* Succeeded: the console carries out the finished sequence. */
	return TTY_CSI_COMMAND;
}

/*
 * Reports how many parameters the finished sequence has, empty ones included.
 *
 * A sequence with no parameter text still has one, empty, parameter.
 */
unsigned
tty_csi_parameter_count(
	const struct tty_csi_parser *parser)
{
	/* Reports only the parameters that were kept. */
	if (parser->current >= TTY_CSI_PARAMETER_MAX)
		return TTY_CSI_PARAMETER_MAX;

	/* Reports the parameters before the last separator and the one after it. */
	return parser->current + 1U;
}

/*
 * Reports whether a parameter of the finished sequence had any digit.
 */
int
tty_csi_parameter_present(
	const struct tty_csi_parser *parser,
	unsigned index)
{
	/* A parameter past the kept ones was not written. */
	if (index >= TTY_CSI_PARAMETER_MAX)
		return 0;

	/* Reports a parameter with at least one digit. */
	if (parser->present[index])
		return 1;

	/* Reports an empty or missing parameter. */
	return 0;
}

/*
 * Reports a parameter of the finished sequence, or a fallback where it was left empty.
 */
unsigned
tty_csi_parameter(
	const struct tty_csi_parser *parser,
	unsigned index,
	unsigned fallback)
{
	int present;

	/* Finds out whether the sequence wrote this parameter. */
	present = tty_csi_parameter_present(parser, index);

	/* An empty parameter takes the command's own default. */
	if (!present)
		return fallback;

	/* Reports the written value. */
	return parser->values[index];
}

/*
 * Computes the cell a cursor-position command names.
 *
 * The sequence counts rows and columns from one, and an empty or zero
 * parameter means the first.  A position past the grid stops at its edge.
 */
void
tty_csi_position(
	const struct tty_csi_parser *parser,
	unsigned rows,
	unsigned columns,
	unsigned *row,
	unsigned *column)
{
	/* Takes the row from the first parameter and the column from the second. */
	*row = tty_csi_coordinate(parser, 0U, rows);
	*column = tty_csi_coordinate(parser, 1U, columns);
}

/*
 * Returns a rendition to plain text: the default colours, neither bold nor reversed.
 */
void
tty_csi_attribute_reset(
	struct tty_csi_attribute *attribute)
{
	/* Sets every part of the rendition to its default. */
	attribute->foreground = TTY_CSI_FOREGROUND_DEFAULT;
	attribute->background = TTY_CSI_BACKGROUND_DEFAULT;
	attribute->bold = 0;
	attribute->reverse = 0;
}

/*
 * Changes a rendition by the parameters of a select-graphic-rendition command.
 *
 * Parameters the console cannot show are passed over one by one, so a
 * later parameter in the same command still takes effect.  An extended
 * colour is passed over with all of its own parameters, so its numbers
 * are not read as renditions of their own.
 */
void
tty_csi_attribute_apply(
	struct tty_csi_attribute *attribute,
	const struct tty_csi_parser *parser)
{
	unsigned count;
	unsigned index;
	unsigned code;

	/* Carries out each parameter in order. */
	count = tty_csi_parameter_count(parser);
	index = 0;
	while (index < count) {
		/* Reads the next rendition code; an empty parameter means a reset. */
		code = tty_csi_parameter(parser, index, 0U);

		/* Changes the part of the rendition the code names; other codes change nothing. */
		if (code == 0U) {
			/* Returns to plain text. */
			tty_csi_attribute_reset(attribute);
		} else if (code == 1U) {
			/* Shows text in the bright form of its colour. */
			attribute->bold = 1;
		} else if (code == 22U) {
			/* Ends the bright form. */
			attribute->bold = 0;
		} else if (code == 7U) {
			/* Swaps the text colour and the colour behind it. */
			attribute->reverse = 1;
		} else if (code == 27U) {
			/* Ends the swap. */
			attribute->reverse = 0;
		} else if (code >= 30U && code <= 37U) {
			/* Selects one of the eight text colours. */
			attribute->foreground = code - 30U;
		} else if (code == 39U) {
			/* Returns the text colour to its default. */
			attribute->foreground = TTY_CSI_FOREGROUND_DEFAULT;
		} else if (code >= 40U && code <= 47U) {
			/* Selects one of the eight colours behind text. */
			attribute->background = code - 40U;
		} else if (code == 49U) {
			/* Returns the colour behind text to its default. */
			attribute->background = TTY_CSI_BACKGROUND_DEFAULT;
		} else if (code == 38U || code == 48U) {
			/* Passes over the parameters of a colour the console cannot show. */
			index += tty_csi_extended_length(parser, index);
		}

		/* Moves to the parameter after this code. */
		index++;
	}
}

/*
 * Computes the display's attribute byte for a rendition.
 *
 * The low nibble is the text colour and the high nibble the colour behind
 * it.  The bright form sets the intensity bit of the text colour only:
 * the same bit of the background makes text blink on a VGA text screen.
 */
uint8_t
tty_csi_attribute_byte(
	const struct tty_csi_attribute *attribute)
{
	unsigned text_color;
	unsigned back_color;
	unsigned swapped;

	/* Turns the ANSI colours into the display's colour numbers. */
	text_color = tty_csi_display_colors[attribute->foreground & 7U];
	back_color = tty_csi_display_colors[attribute->background & 7U];

	/* Swaps the two colours for reverse video. */
	if (attribute->reverse) {
		swapped = text_color;
		text_color = back_color;
		back_color = swapped;
	}

	/* Brightens the text colour for bold. */
	if (attribute->bold)
		text_color |= 0x8U;

	/* Reports the colours packed into one attribute byte. */
	return (uint8_t)((back_color << 4) | text_color);
}

/* Empties the parameters for a new sequence. */
static void
tty_csi_begin(
	struct tty_csi_parser *parser)
{
	unsigned index;

	/* Marks every parameter as empty. */
	for (index = 0; index < TTY_CSI_PARAMETER_MAX; index++) {
		parser->values[index] = 0;
		parser->present[index] = 0;
	}

	/* Starts at the first parameter of a sequence nothing has spoiled yet. */
	parser->current = 0;
	parser->ignored = 0;
	parser->command = 0;
}

/* Adds one decimal digit to the parameter being read. */
static void
tty_csi_add_digit(
	struct tty_csi_parser *parser,
	unsigned char byte)
{
	unsigned index;

	/* Drops a digit of a parameter past the kept ones. */
	index = parser->current;
	if (index >= TTY_CSI_PARAMETER_MAX)
		return;

	/* Marks the parameter written, even when its value stops growing. */
	parser->present[index] = 1;

	/* Stops a long number at a bounded value instead of wrapping around. */
	if (parser->values[index] >= TTY_CSI_VALUE_LIMIT)
		return;

	/* Shifts the value one decimal place and adds the digit. */
	parser->values[index] = parser->values[index] * 10U + (unsigned)(byte - '0');
}

/* Turns one one-based coordinate parameter into a zero-based cell on a grid line. */
static unsigned
tty_csi_coordinate(
	const struct tty_csi_parser *parser,
	unsigned index,
	unsigned limit)
{
	unsigned position;

	/* Reads the coordinate; empty means the first. */
	position = tty_csi_parameter(parser, index, 1U);

	/* Zero also means the first. */
	if (position == 0U)
		position = 1U;

	/* A grid with no cells has only the origin. */
	if (limit == 0U)
		return 0;

	/* Stops a coordinate past the grid at its last cell. */
	if (position > limit)
		return limit - 1U;

	/* Reports the coordinate counted from zero. */
	return position - 1U;
}

/*
 * Counts the parameters that belong to an extended colour after its introducer.
 *
 * "5;n" names a palette index and "2;r;g;b" a direct colour.  Any other
 * selection is not an extended colour, and nothing after it is taken.
 */
static unsigned
tty_csi_extended_length(
	const struct tty_csi_parser *parser,
	unsigned index)
{
	unsigned selection;

	/* Reads what kind of colour follows the introducer. */
	selection = tty_csi_parameter(parser, index + 1U, 0U);

	/* A palette index is the selection and one number. */
	if (selection == TTY_CSI_EXTENDED_INDEXED)
		return 2U;

	/* A direct colour is the selection and three numbers. */
	if (selection == TTY_CSI_EXTENDED_DIRECT)
		return 4U;

	/* Reports that no parameter belongs to the introducer. */
	return 0;
}
