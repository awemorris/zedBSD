/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library visual encoding.
 *
 * Text that arrived from elsewhere is not safe to print: an escape sequence
 * inside it acts on the terminal of whoever reads the log rather than being
 * read as text.  These calls rewrite such a string so that it can only print
 * as itself, and read the result back.
 *
 * Written from what the interface is defined to do, not from another
 * system's source.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <vis.h>

/*
 * Supports the needs encoding operation.
 *
 * Decides whether one character may be written as itself.
 */
static int
needs_encoding(
	int c,
	int flag,
	int next)
{
	unsigned char byte;

	byte = (unsigned char)c;

	/* The ones the caller asked for by name. */
	if (byte == ' ')
		return (flag & VIS_SP) != 0;
	if (byte == '\t')
		return (flag & VIS_TAB) != 0;
	if (byte == '\n')
		return (flag & VIS_NL) != 0;
	if (byte == '"')
		return (flag & VIS_DQ) != 0;

	/* A pattern character encoded so that it cannot match by accident. */
	if ((flag & VIS_GLOB) != 0 &&
	    (byte == '*' || byte == '?' || byte == '[' || byte == '#'))
		return 1;

	/* The backslash, unless the encoding has no escape to confuse. */
	if (byte == '\\')
		return (flag & VIS_NOSLASH) == 0;

	/*
	 * VIS_SAFE keeps whatever cannot act on a terminal, which is what a
	 * caller wants when the text is meant to stay readable.
	 */
	if ((flag & VIS_SAFE) != 0 &&
	    (byte == '\b' || byte == '\007' || byte == '\r' ||
	     isgraph(byte)))
		return 0;

	/* Everything printable stands for itself. */
	if (byte >= 32U && byte <= 126U)
		return 0;

	/*
	 * A digit after an octal escape would be read as part of it, so the
	 * escape that precedes one is never shortened.
	 */
	(void)next;

	/* Returns the computed result. */
	return 1;
}

/*
 * Supports the encode one operation.
 *
 * Writes one encoded character and reports how many bytes it took.  The
 * destination is assumed to hold the longest form, which is four bytes and
 * the terminator.
 *
 * Without VIS_OCTAL a character is written the way a terminal user reads it:
 * a control character as the letter it is typed with after a caret, and a
 * character with its high bit set as M- before the rest of it.  That is the
 * form this encoding is shared in, so text encoded here reads the same
 * elsewhere.
 */
static size_t
encode_one(
	char *out,
	int c,
	int flag,
	int next)
{
	unsigned char byte;
	unsigned char rest;
	size_t used;

	byte = (unsigned char)c;
	used = 0;

	/* Handles a character that may stand for itself. */
	if (!needs_encoding(c, flag, next)) {
		out[used++] = (char)byte;
		out[used] = '\0';

		/* Returns the computed result. */
		return used;
	}

	/* The backslash doubles, which is the whole of its encoding. */
	if (byte == '\\' && (flag & VIS_OCTAL) == 0) {
		out[used++] = '\\';
		out[used++] = '\\';
		out[used] = '\0';
		return used;
	}

	/* A named escape, when the caller prefers one and there is one. */
	if ((flag & VIS_CSTYLE) != 0 && (flag & VIS_OCTAL) == 0) {
		const char *letter = NULL;

		/* Dispatch the selected control character. */
		switch (byte) {
		case '\n': letter = "\\n"; break;
		case '\r': letter = "\\r"; break;
		case '\b': letter = "\\b"; break;
		case '\a': letter = "\\a"; break;
		case '\v': letter = "\\v"; break;
		case '\t': letter = "\\t"; break;
		case '\f': letter = "\\f"; break;
		case ' ':  letter = "\\s"; break;
		case '\0': letter = "\\0"; break;
		default:   break;
		}

		/*
		 * The zero escape may not be followed by a digit, which
		 * would read as part of it.
		 */
		if (letter != NULL &&
		    !(byte == '\0' && next >= '0' && next <= '7')) {
			out[used++] = letter[0];
			out[used++] = letter[1];
			out[used] = '\0';
			return used;
		}
	}

	/* The caret and meta form, which is the one this is shared in. */
	if ((flag & VIS_OCTAL) == 0 &&
	    (byte < 32U || byte == 127U || byte >= 128U)) {
		out[used++] = '\\';
		rest = byte;

		/* A high bit is written as M before the rest of it. */
		if (rest >= 128U) {
			out[used++] = 'M';
			rest = (unsigned char)(rest & 0x7fU);
			out[used++] = rest < 32U || rest == 127U ? '^' : '-';
		} else {
			out[used++] = '^';
		}

		/* A control character is the letter it is typed with. */
		if (rest < 32U || rest == 127U)
			out[used++] = rest == 127U ?
				      '?' : (char)(rest + '@');
		else
			out[used++] = (char)rest;
		out[used] = '\0';

		/* Returns the computed result. */
		return used;
	}

	/*
	 * Otherwise the number itself, in three octal digits so that a digit
	 * after it cannot be read as part of it.
	 */
	out[used++] = '\\';
	out[used++] = (char)('0' + ((byte >> 6) & 7U));
	out[used++] = (char)('0' + ((byte >> 3) & 7U));
	out[used++] = (char)('0' + (byte & 7U));
	out[used] = '\0';

	/* Returns the computed result. */
	return used;
}

/*
 * Implements the vis operation.
 */
char *
vis(
	char *out,
	int c,
	int flag,
	int next)
{
	size_t used;

	/* Handles the out availability. */
	if (out == NULL)
		return NULL;
	used = encode_one(out, c, flag, next);

	/* Returns the computed result, at the terminator it wrote. */
	return out + used;
}

/*
 * Implements the nvis operation.
 */
char *
nvis(
	char *out,
	size_t size,
	int c,
	int flag,
	int next)
{
	char staging[5];
	size_t used;

	/* Handles the out availability. */
	if (out == NULL || size == 0U) {
		errno = ENOSPC;
		return NULL;
	}
	used = encode_one(staging, c, flag, next);

	/* Handles an encoding that does not fit. */
	if (used + 1U > size) {
		out[0] = '\0';
		errno = ENOSPC;

		/* Reports operation failure. */
		return NULL;
	}
	memcpy(out, staging, used + 1U);

	/* Returns the computed result. */
	return out + used;
}

/*
 * Supports the encode string operation.
 *
 * Encodes length bytes, stopping cleanly when the destination is full.
 * Reports the number of bytes the whole encoding needs, so that a caller can
 * tell a truncated result from a complete one, the way strlcpy does.
 */
static int
encode_string(
	char *out,
	size_t size,
	const char *in,
	size_t length,
	int flag,
	int bounded)
{
	char staging[5];
	size_t produced;
	size_t used;
	size_t index;
	int next;

	produced = 0;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		next = index + 1U < length ?
		       (unsigned char)in[index + 1U] : '\0';
		used = encode_one(staging, (unsigned char)in[index], flag,
				  next);

		/* Copies as much as there is room for, terminator included. */
		if (bounded) {
			if (produced + used + 1U <= size)
				memcpy(out + produced, staging, used + 1U);
		} else {
			memcpy(out + produced, staging, used + 1U);
		}
		produced += used;
	}

	/* Handles a bounded destination that could not hold it all. */
	if (bounded) {
		if (size == 0U)
			return (int)produced;
		if (produced + 1U > size)
			out[size - 1U] = '\0';
		else
			out[produced] = '\0';
	} else {
		out[produced] = '\0';
	}

	/* Returns the computed result. */
	return (int)produced;
}

/*
 * Implements the strvis operation.
 */
int
strvis(
	char *out,
	const char *in,
	int flag)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return encode_string(out, 0, in, strlen(in), flag, 0);
}

/*
 * Implements the strvisx operation.
 */
int
strvisx(
	char *out,
	const char *in,
	size_t length,
	int flag)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return encode_string(out, 0, in, length, flag, 0);
}

/*
 * Implements the strnvis operation.
 *
 * The destination size comes third here, beside the string it bounds, which
 * is the order OpenBSD chose and the one callers are written against.
 */
int
strnvis(
	char *out,
	const char *in,
	size_t size,
	int flag)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return encode_string(out, size, in, strlen(in), flag, 1);
}

/*
 * Implements the strnvisx operation.
 */
int
strnvisx(
	char *out,
	size_t size,
	const char *in,
	size_t length,
	int flag)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return encode_string(out, size, in, length, flag, 1);
}

/*
 * Implements the stravis operation.
 *
 * Allocates a destination large enough for the longest encoding, so that a
 * caller with text of unknown length need not size one itself.
 */
int
stravis(
	char **out,
	const char *in,
	int flag)
{
	char *buffer;
	size_t length;
	int produced;

	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;
	*out = NULL;
	length = strlen(in);

	/* Four bytes per character is the longest an encoding can be. */
	if (length > (SIZE_MAX - 1U) / 4U) {
		errno = ENOMEM;
		return -1;
	}
	buffer = malloc(length * 4U + 1U);

	/* Handles a failed malloc operation. */
	if (buffer == NULL)
		return -1;
	produced = encode_string(buffer, 0, in, length, flag, 0);
	*out = buffer;

	/* Returns the computed result. */
	return produced;
}

/*
 * Implements the unvis operation.
 *
 * Reads one byte at a time and reports whether a character came out of it.
 * The caller keeps the state between bytes, so a sequence may be split
 * across any number of reads.  The low byte of the state says where in a
 * sequence the reader is, and the rest carries what has been gathered.
 */
int
unvis(
	char *out,
	int c,
	int *state,
	int flag)
{
	unsigned char byte;
	unsigned step;
	unsigned held;

	/* Handles the arguments availability. */
	if (out == NULL || state == NULL)
		return UNVIS_ERROR;
	byte = (unsigned char)c;
	step = (unsigned)*state & 0xffU;
	held = (unsigned)*state >> 8;

	/* The end of the input finishes a number that was still growing. */
	if ((flag & UNVIS_END) != 0) {
		*state = 0;
		if (step == 2U || step == 3U) {
			*out = (char)(held & 0xffU);

			/* Returns the computed result. */
			return UNVIS_VALID;
		}

		/* An unfinished sequence of any other shape is not readable. */
		return step == 0U ? UNVIS_NOCHAR : UNVIS_SYNBAD;
	}

	/* Dispatch the selected point in the sequence. */
	switch (step) {
	case 0:

		/* Anything but a backslash stands for itself. */
		if (byte != '\\') {
			*out = (char)byte;
			return UNVIS_VALID;
		}
		*state = 1;
		return UNVIS_NOCHAR;
	case 1:

		/* A digit begins a number, which may run to three of them. */
		if (byte >= '0' && byte <= '7') {
			*state = 2 | (int)((unsigned)(byte - '0') << 8);
			return UNVIS_NOCHAR;
		}

		/* A caret begins a control character. */
		if (byte == '^') {
			*state = 4;
			return UNVIS_NOCHAR;
		}

		/* An M begins a character whose high bit is set. */
		if (byte == 'M') {
			*state = 5;
			return UNVIS_NOCHAR;
		}

		/* Dispatch the selected named escape. */
		switch (byte) {
		case '\\': *out = '\\'; break;
		case 'n':  *out = '\n'; break;
		case 'r':  *out = '\r'; break;
		case 'b':  *out = '\b'; break;
		case 'a':  *out = '\a'; break;
		case 'v':  *out = '\v'; break;
		case 't':  *out = '\t'; break;
		case 'f':  *out = '\f'; break;
		case 's':  *out = ' ';  break;
		case 'E':  *out = '\033'; break;
		case '-':

			/* An explicit nothing, which encodes no character. */
			*state = 0;
			return UNVIS_NOCHAR;
		default:
			*state = 0;

			/* Reports operation failure. */
			return UNVIS_SYNBAD;
		}
		*state = 0;
		return UNVIS_VALID;
	case 2:
	case 3:

		/* A further digit extends the number. */
		if (byte >= '0' && byte <= '7') {
			held = held * 8U + (unsigned)(byte - '0');
			if (step == 2U) {
				*state = 3 | (int)(held << 8);
				return UNVIS_NOCHAR;
			}
			*out = (char)(held & 0xffU);
			*state = 0;
			return UNVIS_VALID;
		}

		/* Anything else ends the number, and is read again after it. */
		*out = (char)(held & 0xffU);
		*state = 0;

		/* Returns the computed result. */
		return UNVIS_VALIDPUSH;
	case 4:

		/* The letter a control character is typed with. */
		*state = 0;
		if (byte == '?') {
			*out = (char)127;
			return UNVIS_VALID;
		}
		if (byte < '@' || byte > 127U)
			return UNVIS_SYNBAD;
		*out = (char)(byte & 31U);
		return UNVIS_VALID;
	case 5:

		/* Either a further caret, or the character itself. */
		if (byte == '^') {
			*state = 7;
			return UNVIS_NOCHAR;
		}
		if (byte == '-') {
			*state = 6;
			return UNVIS_NOCHAR;
		}
		*state = 0;

		/* Reports operation failure. */
		return UNVIS_SYNBAD;
	case 6:

		/* The character, with its high bit put back. */
		*state = 0;
		*out = (char)(byte | 0x80U);
		return UNVIS_VALID;
	case 7:

		/* A control character with its high bit put back. */
		*state = 0;
		if (byte == '?') {
			*out = (char)(127U | 0x80U);
			return UNVIS_VALID;
		}
		if (byte < '@' || byte > 127U)
			return UNVIS_SYNBAD;
		*out = (char)((byte & 31U) | 0x80U);
		return UNVIS_VALID;
	default:
		break;
	}
	*state = 0;

	/* Reports operation failure. */
	return UNVIS_SYNBAD;
}

/*
 * Supports the decode string operation.
 */
static int
decode_string(
	char *out,
	size_t size,
	const char *in,
	size_t length,
	int bounded)
{
	size_t produced;
	size_t index;
	char decoded;
	int state;
	int result;

	produced = 0;
	state = 0;
	index = 0;

	/* Continue while the operation condition remains true. */
	while (index < length) {
		result = unvis(&decoded, (unsigned char)in[index], &state, 0);

		/* Dispatch the selected outcome of the byte. */
		switch (result) {
		case UNVIS_VALID:
			index++;
			break;
		case UNVIS_VALIDPUSH:

			/* The byte ended a number and must be read again. */
			break;
		case UNVIS_NOCHAR:
			index++;
			continue;
		default:

			/* Reports operation failure. */
			return -1;
		}

		/* Stores the character, if the destination has room. */
		if (bounded && produced + 1U >= size) {
			if (size != 0U)
				out[size - 1U] = '\0';

			/* Reports operation failure. */
			return -1;
		}
		out[produced++] = decoded;
	}

	/* Finishes a number the input ended in the middle of. */
	result = unvis(&decoded, 0, &state, UNVIS_END);
	if (result == UNVIS_VALID) {
		if (bounded && produced + 1U >= size) {
			if (size != 0U)
				out[size - 1U] = '\0';
			return -1;
		}
		out[produced++] = decoded;
	} else if (result == UNVIS_SYNBAD) {
		return -1;
	}
	out[produced] = '\0';

	/* Returns the computed result. */
	return (int)produced;
}

/*
 * Implements the strunvis operation.
 */
int
strunvis(
	char *out,
	const char *in)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return decode_string(out, 0, in, strlen(in), 0);
}

/*
 * Implements the strnunvis operation.
 */
int
strnunvis(
	char *out,
	size_t size,
	const char *in)
{
	/* Handles the arguments availability. */
	if (out == NULL || in == NULL)
		return -1;

	/* Returns the computed result. */
	return decode_string(out, size, in, strlen(in), 1);
}

/*
 * Implements the strunvisx operation.
 */
int
strunvisx(
	char *out,
	const char *in,
	int flag)
{
	(void)flag;

	/* Returns the computed result. */
	return strunvis(out, in);
}

/*
 * Implements the strnunvisx operation.
 */
int
strnunvisx(
	char *out,
	size_t size,
	const char *in,
	int flag)
{
	(void)flag;

	/* Returns the computed result. */
	return strnunvis(out, size, in);
}
