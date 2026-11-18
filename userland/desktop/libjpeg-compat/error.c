/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The standard error manager, as libjpeg's: an error formats its message,
 * prints it on standard error, destroys the object and exits, unless the
 * program replaced error_exit (and longjmps out of its own).  A warning is
 * printed once per object and counted in num_warnings.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The texts of the messages, by code (internal.h).  A text with "%s"
 * takes msg_parm.s, any other takes msg_parm.i.  The table is constant
 * for the life of the program.
 */
static const char *const jpeg_compat_messages[] = {
	"Bogus message code %d",
	"Wrong JPEG library version: library is %d, caller expects %d",
	"JPEG parameter struct mismatch: library thinks size is %d, caller expects %d",
	"Improper call to JPEG library in state %d",
	"Insufficient memory (case %d)",
	"Not a JPEG file: starts with 0x%02x 0x%02x",
	"JPEG datastream contains no image",
	"No data source was set (jpeg_stdio_src or jpeg_mem_src)",
	"Empty input file",
	"Bogus marker length",
	"Unsupported JPEG data precision %d",
	"Empty JPEG image (DNL not supported)",
	"Image too big to decode (%d x %d)",
	"Unsupported number of components %d",
	"Bogus sampling factors",
	"Bogus table definition",
	"Huffman or quantization table 0x%02x was not defined",
	"Invalid SOS parameters",
	"Invalid JPEG file structure: SOS before SOF",
	"Arithmetic coding is not supported",
	"Unsupported JPEG process: SOF type 0x%02x",
	"Unsupported color conversion request",
	"Application transferred too few scanlines",
	"Unsupported marker type 0x%02x",
	"Premature end of JPEG file",
	"Corrupt JPEG data: %d extraneous bytes before marker 0x%02x",
	"Corrupt JPEG data: bad Huffman code",
	"Corrupt JPEG data: premature end of data segment",
	"Corrupt JPEG data: found marker 0x%02x instead of RST%d",
	NULL
};

static void jpeg_compat_error_exit(j_common_ptr cinfo);
static void jpeg_compat_emit_message(j_common_ptr cinfo, int msg_level);
static void jpeg_compat_output_message(j_common_ptr cinfo);
static void jpeg_compat_format_message(j_common_ptr cinfo, char *buffer);
static void jpeg_compat_reset_error_mgr(j_common_ptr cinfo);

/*
 * Fills an error manager with the standard methods and returns it (the
 * program then sets cinfo->err to it).
 */
struct jpeg_error_mgr *
jpeg_std_error(
	struct jpeg_error_mgr *err)
{
	/* The methods, the message table, and no message yet. */
	memset(err, 0, sizeof(*err));
	err->error_exit = jpeg_compat_error_exit;
	err->emit_message = jpeg_compat_emit_message;
	err->output_message = jpeg_compat_output_message;
	err->format_message = jpeg_compat_format_message;
	err->reset_error_mgr = jpeg_compat_reset_error_mgr;
	err->jpeg_message_table = jpeg_compat_messages;
	err->last_jpeg_message = (int)JMSG_LASTMSGCODE - 1;
	return err;
}

/* Raises an error with no parameter; error_exit does not return. */
void
jpeg_compat_fail(
	j_common_ptr cinfo,
	int code)
{
	/* The message, then the program's exit. */
	jpeg_compat_fail_number(cinfo, code, 0, 0);
}

/* Raises an error with two numbers for its message; error_exit does not return. */
void
jpeg_compat_fail_number(
	j_common_ptr cinfo,
	int code,
	int first,
	int second)
{
	/* The message's code and numbers. */
	cinfo->err->msg_code = code;
	cinfo->err->msg_parm.i[0] = first;
	cinfo->err->msg_parm.i[1] = second;

	/* The program's exit; one that returns breaks libjpeg's contract, so the process ends. */
	cinfo->err->error_exit(cinfo);
	exit(EXIT_FAILURE);
}

/* Emits a warning (the data is corrupt but decoding goes on). */
void
jpeg_compat_warn(
	j_common_ptr cinfo,
	int code)
{
	/* The message's code, then the warning level (-1). */
	cinfo->err->msg_code = code;
	cinfo->err->emit_message(cinfo, -1);
}

/* The standard exit: the message, then the object is destroyed and the process exits. */
static void
jpeg_compat_error_exit(
	j_common_ptr cinfo)
{
	/* What went wrong, on standard error. */
	cinfo->err->output_message(cinfo);

	/* Nothing is left behind. */
	jpeg_destroy(cinfo);
	exit(EXIT_FAILURE);
}

/* Prints a warning the first time (or every trace message at its level), and counts warnings. */
static void
jpeg_compat_emit_message(
	j_common_ptr cinfo,
	int msg_level)
{
	struct jpeg_error_mgr *err;

	/* A warning: printed when it is the first, or when tracing asks for all. */
	err = cinfo->err;
	if (msg_level < 0) {
		if (err->num_warnings == 0 || err->trace_level >= 3)
			err->output_message(cinfo);
		err->num_warnings++;
		return;
	}

	/* A trace message, printed at its level. */
	if (err->trace_level >= msg_level)
		err->output_message(cinfo);
}

/* Prints the current message on standard error. */
static void
jpeg_compat_output_message(
	j_common_ptr cinfo)
{
	char buffer[JMSG_LENGTH_MAX];

	/* The text, then its line. */
	cinfo->err->format_message(cinfo, buffer);
	fprintf(stderr, "%s\n", buffer);
}

/* Writes the current message's text (at most JMSG_LENGTH_MAX bytes with its NUL). */
static void
jpeg_compat_format_message(
	j_common_ptr cinfo,
	char *buffer)
{
	struct jpeg_error_mgr *err;
	const char *text;
	const char *found;
	int code;

	/* The text of the code: the library's, a program's add-on table's, or the bogus one. */
	err = cinfo->err;
	code = err->msg_code;
	text = NULL;
	if (code > 0 && code <= err->last_jpeg_message) {
		text = err->jpeg_message_table[code];
	} else if (err->addon_message_table != NULL && code >= err->first_addon_message && code <= err->last_addon_message) {
		text = err->addon_message_table[code - err->first_addon_message];
	}

	/* An unknown code says so. */
	if (text == NULL) {
		err->msg_parm.i[0] = code;
		text = err->jpeg_message_table[0];
	}

	/* The parameters: a string, or the eight numbers. */
	found = strstr(text, "%s");
	if (found != NULL) {
		snprintf(buffer, JMSG_LENGTH_MAX, text, err->msg_parm.s);
		return;
	}

	/* Otherwise the numbers (a text uses the first ones it names). */
	snprintf(buffer, JMSG_LENGTH_MAX, text, err->msg_parm.i[0], err->msg_parm.i[1], err->msg_parm.i[2], err->msg_parm.i[3], err->msg_parm.i[4], err->msg_parm.i[5], err->msg_parm.i[6], err->msg_parm.i[7]);
}

/* Forgets the warnings of the last image (a new image starts clean). */
static void
jpeg_compat_reset_error_mgr(
	j_common_ptr cinfo)
{
	/* No warning and no message. */
	cinfo->err->num_warnings = 0;
	cinfo->err->msg_code = 0;
}
