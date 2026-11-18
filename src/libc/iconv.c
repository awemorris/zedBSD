/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Character set conversion between UTF-8 and ASCII.
 *
 * These are the only character sets the system uses, so they are the only
 * ones converted (WS034 decision, 2026-09-23).  Names are matched without
 * regard to case:
 *
 *	UTF-8		"UTF-8", "UTF8"
 *	ASCII		"ASCII", "US-ASCII", "ANSI_X3.4-1968", "646"
 *
 * An empty name means the character set of the locale, which is UTF-8.
 *
 * The target name may carry the suffixes other systems accept.  With
 * //TRANSLIT a character the target cannot hold becomes '?'; with //IGNORE
 * it is left out, and so is input that is not valid in its character set.
 * Either way the character is counted in the return value as a conversion
 * that cannot be undone.  Without them, such a character stops the
 * conversion with EILSEQ.
 *
 * UTF-8 input is checked as strictly as the standard asks: no overlong forms,
 * no surrogates, nothing above U+10FFFF.  Both character sets are stateless,
 * so there is no shift state to reset.
 */

#include <errno.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum charset {
	CHARSET_UTF8,
	CHARSET_ASCII
};

#define ICONV_TRANSLIT	0x01U
#define ICONV_IGNORE	0x02U

/* One open conversion. */
struct iconv_state {
	enum charset from;
	enum charset to;
	unsigned flags;
};

static int parse_name(const char *name, enum charset *charset,
		      unsigned *flags);
static int decode(enum charset charset, const unsigned char *input,
		  size_t length, unsigned *code, size_t *used);
static size_t encode(enum charset charset, unsigned code,
		     unsigned char *output);

/*
 * Reads one character set name and the suffixes after it.
 * Returns 0, or -1 for a name this library does not convert.
 */
static int
parse_name(
	const char *name,
	enum charset *charset,
	unsigned *flags)
{
	static const struct {
		const char *name;
		enum charset charset;
	} names[] = {
		{ "", CHARSET_UTF8 },
		{ "UTF-8", CHARSET_UTF8 },
		{ "UTF8", CHARSET_UTF8 },
		{ "ASCII", CHARSET_ASCII },
		{ "US-ASCII", CHARSET_ASCII },
		{ "ANSI_X3.4-1968", CHARSET_ASCII },
		{ "646", CHARSET_ASCII },
	};
	const char *suffix;
	size_t length;
	size_t i;

	/* Splits the name from its suffixes. */
	suffix = strstr(name, "//");
	length = suffix != NULL ? (size_t)(suffix - name) : strlen(name);

	/* Reads the suffixes, each introduced by "//". */
	*flags = 0;
	while (suffix != NULL) {
		suffix += 2;
		if (strncasecmp(suffix, "TRANSLIT", 8) == 0 &&
		    (suffix[8] == '\0' || suffix[8] == '/'))
			*flags |= ICONV_TRANSLIT;
		else if (strncasecmp(suffix, "IGNORE", 6) == 0 &&
			 (suffix[6] == '\0' || suffix[6] == '/'))
			*flags |= ICONV_IGNORE;
		else if (*suffix != '\0')
			return -1;
		suffix = strstr(suffix, "//");
	}

	/* Finds the character set. */
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strlen(names[i].name) == length &&
		    strncasecmp(names[i].name, name, length) == 0) {
			*charset = names[i].charset;
			return 0;
		}
	}
	return -1;
}

/*
 * Reads one character from the input.
 *
 * Returns 0 with the character and the bytes it took, EINVAL when the input
 * ends inside a character that could still be valid, or EILSEQ when it is
 * not valid at all (then *used is how many bytes to skip past it).
 */
static int
decode(
	enum charset charset,
	const unsigned char *input,
	size_t length,
	unsigned *code,
	size_t *used)
{
	unsigned value;
	unsigned minimum;
	size_t need;
	size_t i;

	/* An ASCII byte is its own character; a byte with the top bit is not ASCII. */
	*used = 1;
	if (input[0] < 0x80U) {
		*code = input[0];
		return 0;
	}
	if (charset == CHARSET_ASCII)
		return EILSEQ;

	/* Finds the length of the UTF-8 sequence from its lead byte. */
	if (input[0] >= 0xc2U && input[0] <= 0xdfU) {
		need = 2;
		value = input[0] & 0x1fU;
		minimum = 0x80U;
	} else if (input[0] >= 0xe0U && input[0] <= 0xefU) {
		need = 3;
		value = input[0] & 0x0fU;
		minimum = 0x800U;
	} else if (input[0] >= 0xf0U && input[0] <= 0xf4U) {
		need = 4;
		value = input[0] & 0x07U;
		minimum = 0x10000U;
	} else {
		return EILSEQ;
	}

	/* Takes the continuation bytes that are there. */
	for (i = 1; i < need; i++) {
		if (i >= length)
			return EINVAL;
		if ((input[i] & 0xc0U) != 0x80U) {
			*used = i;
			return EILSEQ;
		}
		value = value << 6 | (input[i] & 0x3fU);

		/* Rejects an overlong form or a surrogate as soon as it shows. */
		if (i == 1 && need == 3 &&
		    ((input[0] == 0xe0U && input[1] < 0xa0U) ||
		     (input[0] == 0xedU && input[1] >= 0xa0U))) {
			*used = 1;
			return EILSEQ;
		}
		if (i == 1 && need == 4 &&
		    ((input[0] == 0xf0U && input[1] < 0x90U) ||
		     (input[0] == 0xf4U && input[1] >= 0x90U))) {
			*used = 1;
			return EILSEQ;
		}
	}

	/* The checks above already keep the value in range; this says so. */
	if (value < minimum || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU)) {
		*used = need;
		return EILSEQ;
	}
	*code = value;
	*used = need;
	return 0;
}

/*
 * Writes one character, returning the bytes it takes, or 0 when the
 * character set cannot hold it.  output has room for four bytes.
 */
static size_t
encode(
	enum charset charset,
	unsigned code,
	unsigned char *output)
{
	/* ASCII holds only the first 128 characters. */
	if (charset == CHARSET_ASCII) {
		if (code >= 0x80U)
			return 0;
		output[0] = (unsigned char)code;
		return 1;
	}

	/* UTF-8 takes one to four bytes. */
	if (code < 0x80U) {
		output[0] = (unsigned char)code;
		return 1;
	}
	if (code < 0x800U) {
		output[0] = (unsigned char)(0xc0U | code >> 6);
		output[1] = (unsigned char)(0x80U | (code & 0x3fU));
		return 2;
	}
	if (code < 0x10000U) {
		output[0] = (unsigned char)(0xe0U | code >> 12);
		output[1] = (unsigned char)(0x80U | (code >> 6 & 0x3fU));
		output[2] = (unsigned char)(0x80U | (code & 0x3fU));
		return 3;
	}
	output[0] = (unsigned char)(0xf0U | code >> 18);
	output[1] = (unsigned char)(0x80U | (code >> 12 & 0x3fU));
	output[2] = (unsigned char)(0x80U | (code >> 6 & 0x3fU));
	output[3] = (unsigned char)(0x80U | (code & 0x3fU));
	return 4;
}

/*
 * Opens a conversion from one character set to another.
 */
iconv_t
iconv_open(
	const char *tocode,
	const char *fromcode)
{
	struct iconv_state *state;
	enum charset from;
	enum charset to;
	unsigned from_flags;
	unsigned to_flags;

	/* Accepts only the character sets this library converts. */
	if (tocode == NULL || fromcode == NULL ||
	    parse_name(tocode, &to, &to_flags) != 0 ||
	    parse_name(fromcode, &from, &from_flags) != 0) {
		errno = EINVAL;
		return (iconv_t)-1;
	}

	/* Keeps the pair; the suffixes are read from the target name. */
	state = malloc(sizeof(*state));
	if (state == NULL)
		return (iconv_t)-1;
	state->from = from;
	state->to = to;
	state->flags = to_flags;
	return state;
}

/*
 * Converts as much of the input as fits in the output.
 *
 * Returns the number of characters converted in a way that cannot be undone,
 * or (size_t)-1 with errno: E2BIG when the output is full, EILSEQ at input
 * that cannot be converted, EINVAL when the input ends inside a character.
 * The pointers and counts are left just past what was converted.
 */
size_t
iconv(
	iconv_t descriptor,
	char **__restrict inbuf,
	size_t *__restrict inbytesleft,
	char **__restrict outbuf,
	size_t *__restrict outbytesleft)
{
	struct iconv_state *state = descriptor;
	const unsigned char *input;
	unsigned char *output;
	unsigned char bytes[4];
	size_t in_left;
	size_t out_left;
	size_t irreversible = 0;
	size_t used;
	size_t written;
	unsigned code;
	int lossy;
	int error = 0;

	/* Rejects a descriptor that iconv_open() did not return. */
	if (state == NULL || descriptor == (iconv_t)-1) {
		errno = EBADF;
		return (size_t)-1;
	}

	/* With no input there is no shift state to reset or to write. */
	if (inbuf == NULL || *inbuf == NULL)
		return 0;
	if (inbytesleft == NULL || outbuf == NULL || *outbuf == NULL ||
	    outbytesleft == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}

	input = (const unsigned char *)*inbuf;
	in_left = *inbytesleft;
	output = (unsigned char *)*outbuf;
	out_left = *outbytesleft;

	/* Converts one character at a time. */
	while (in_left > 0) {
		error = decode(state->from, input, in_left, &code, &used);

		/* Input that ends inside a character waits for more. */
		if (error == EINVAL)
			break;

		/* Invalid input is skipped with //IGNORE and stops otherwise. */
		if (error == EILSEQ) {
			if ((state->flags & ICONV_IGNORE) == 0)
				break;
			input += used;
			in_left -= used;
			irreversible++;
			error = 0;
			continue;
		}

		/* A character the target cannot hold is replaced, left out or refused. */
		written = encode(state->to, code, bytes);
		lossy = written == 0;
		if (lossy) {
			if ((state->flags & ICONV_TRANSLIT) != 0) {
				bytes[0] = '?';
				written = 1;
			} else if ((state->flags & ICONV_IGNORE) != 0) {
				input += used;
				in_left -= used;
				irreversible++;
				continue;
			} else {
				error = EILSEQ;
				break;
			}
		}

		/* Stops before a character that does not fit, leaving it unread. */
		if (written > out_left) {
			error = E2BIG;
			break;
		}
		memcpy(output, bytes, written);
		output += written;
		out_left -= written;
		input += used;
		in_left -= used;
		if (lossy)
			irreversible++;
	}

	/* Leaves the caller's pointers just past what was converted. */
	*inbuf = (char *)input;
	*inbytesleft = in_left;
	*outbuf = (char *)output;
	*outbytesleft = out_left;
	if (error != 0) {
		errno = error;
		return (size_t)-1;
	}
	return irreversible;
}

/*
 * Closes a conversion.
 */
int
iconv_close(
	iconv_t descriptor)
{
	/* Rejects a descriptor that iconv_open() did not return. */
	if (descriptor == NULL || descriptor == (iconv_t)-1) {
		errno = EBADF;
		return -1;
	}
	free(descriptor);
	return 0;
}
