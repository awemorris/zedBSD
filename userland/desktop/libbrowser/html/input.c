/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input stream of the HTML parser: decoded text in UTF-16 with its
 * line breaks normalized as the standard's preprocessing asks.
 */

#include "html/html.h"

/* The line feed and carriage return code units. */
#define INPUT_LF 0x0aU
#define INPUT_CR 0x0dU

/*
 * Prepares an empty, open input stream.
 */
void
html_input_init(
	struct html_input *input)
{
	/* Prepare empty unit storage and start the tokenizer before the first input unit. */
	wb_units_init(&input->units);
	input->position = 0;

	/* An open stream asks the tokenizer to wait for incomplete tokens instead of reporting EOF. */
	input->closed = 0;

	/* No preceding CR can suppress the first appended LF in a new stream. */
	input->after_cr = 0;

	/* The sentinel tells tokenizer-side inserted-text normalization that no CR is pending. */
	input->inserted_cr = (size_t)-1;

	/* Succeeded: appends and tokenizer reads share an empty open stream. */
	return;
}

/*
 * Appends decoded text, turning CR LF and lone CRs into LF.
 *
 * A CR at the end of one piece and an LF at the start of the next are one
 * line break.
 */
int
html_input_append(
	struct html_input *input,
	const uint16_t *units,
	size_t length)
{
	uint16_t unit;
	size_t index;
	int error;

	/* Makes room for the whole piece (normalizing only shortens it). */
	error = wb_units_reserve(&input->units, length);
	if (error != 0)
		return error;

	/* Copies each unit, folding carriage returns into line feeds. */
	for (index = 0; index < length; index++) {
		unit = units[index];

		/* An LF right after a CR belongs to the CR's line break. */
		if (unit == INPUT_LF && input->after_cr) {
			input->after_cr = 0;
			continue;
		}

		/* A CR becomes an LF and remembers it may swallow the next LF. */
		input->after_cr = 0;
		if (unit == INPUT_CR) {
			unit = INPUT_LF;
			input->after_cr = 1;
		}

		/* Publish one normalized unit to the tokenizer within the capacity reserved above. */
		input->units.data[input->units.length] = unit;
		input->units.length++;
	}

	/* Succeeded: the text is readable. */
	return 0;
}

/*
 * Marks the end of the stream: after what was appended comes end of file.
 */
void
html_input_close(
	struct html_input *input)
{
	/* The tokenizer may now report end of file. */
	input->closed = 1;

	/* Succeeded: the already appended units remain readable through the stream's end. */
	return;
}

/*
 * Frees the stream's text.
 */
void
html_input_release(
	struct html_input *input)
{
	/* Release the owned normalized text before resetting the tokenizer protocol state. */
	wb_units_release(&input->units);

	/* Restore the reusable stream to its empty open state. */
	html_input_init(input);

	/* Succeeded: no normalized input storage remains owned by the stream. */
	return;
}
