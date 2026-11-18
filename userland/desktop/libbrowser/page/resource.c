/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declared MIME essence selects the native document parser independently of resource names or contents.
 */

#include "page/page.h"

#include <errno.h>
#include <string.h>

static int resource_space(unsigned char character);
static int resource_token(unsigned char character);
static int resource_equal(const char *mime, size_t length, const char *expected);

/*
 * Selects a supported document kind from a validated declared MIME essence.
 *
 * Parameters do not change the essence; invalid or unsupported declarations
 * preserve the caller's output instead of inventing a parser or sniffed type.
 */
int
page_document_content(
	const char *mime,
	size_t length,
	enum dom_document_content *content)
{
	size_t start;
	size_t end;
	size_t slash;
	size_t index;
	int space;
	int token;
	int same;

	/* Missing declaration storage or output is not a default HTML resource. */
	if (mime == NULL || content == NULL)
		return EINVAL;

	/* Skip only the leading HTTP whitespace permitted before a declared MIME type. */
	start = 0;
	while (start < length) {
		space = resource_space((unsigned char)mime[start]);
		if (!space)
			break;
		start++;
	}

	/* The type ends at the first real slash; every code point must be an HTTP token. */
	slash = start;
	while (slash < length && mime[slash] != '/') {
		token = resource_token((unsigned char)mime[slash]);
		if (!token)
			return EINVAL;
		slash++;
	}

	/* Empty type or absent separator rejects this declaration before subtype inspection. */
	if (slash == start || slash == length)
		return EINVAL;

	/* Locate the subtype boundary before optional MIME parameters. */
	end = slash + 1;
	while (end < length && mime[end] != ';')
		end++;

	/* MIME parsing removes trailing HTTP whitespace from the subtype, never leading whitespace. */
	while (end > slash + 1) {
		space = resource_space((unsigned char)mime[end - 1]);
		if (!space)
			break;
		end--;
	}

	/* Every actual subtype character must be a token; another slash or embedded NUL is invalid. */
	if (end == slash + 1)
		return EINVAL;

	/* Validate every remaining subtype byte before recognizing any supported processing model. */
	for (index = slash + 1; index < end; index++) {
		token = resource_token((unsigned char)mime[index]);
		if (!token)
			return EINVAL;
	}

	/* Supported specialized processing models take precedence over the general XML suffix group. */
	same = resource_equal(mime + start, end - start, "text/html");
	if (same) {
		*content = DOM_CONTENT_HTML;
		return 0;
	}

	/* XHTML remains an XML parser input with its actual declared content identity. */
	same = resource_equal(mime + start, end - start, "application/xhtml+xml");
	if (same) {
		*content = DOM_CONTENT_XHTML;
		return 0;
	}

	/* Only the genuine SVG MIME essence selects the SVG document content kind. */
	same = resource_equal(mime + start, end - start, "image/svg+xml");
	if (same) {
		*content = DOM_CONTENT_SVG;
		return 0;
	}

	/* Ordinary XML declarations and any valid subtype ending in +xml share the strict XML parser. */
	same = resource_equal(mime + start, end - start, "text/xml");
	if (!same)
		same = resource_equal(mime + start, end - start, "application/xml");
	if (!same && end - slash - 1 >= 4)
		same = resource_equal(mime + end - 4, 4, "+xml");
	if (!same)
		return ENOTSUP;

	/* Publish ordinary XML processing only after its complete declared essence is recognized. */
	*content = DOM_CONTENT_XML;

	/* Succeeded: validated actual metadata supplies this processing model. */
	return 0;
}

/* Recognizes the exact HTTP whitespace used by native MIME parsing. */
static int
resource_space(
	unsigned char character)
{
	/* MIME whitespace excludes form feed and other arbitrary control characters. */
	if (character == '\t' ||
	    character == '\n' ||
	    character == '\r' ||
	    character == ' ')
		return 1;

	/* This byte belongs to the actual MIME declaration rather than surrounding whitespace. */
	return 0;
}

/* Recognizes HTTP token bytes without locale-dependent character classification. */
static int
resource_token(
	unsigned char character)
{
	/* ASCII alphanumeric characters are token bytes independently of process locale. */
	if (character >= 'A' && character <= 'Z')
		return 1;

	/* Lowercase ASCII letters have the same HTTP-token membership as uppercase letters. */
	if (character >= 'a' && character <= 'z')
		return 1;

	/* Decimal digits are the remaining alphanumeric HTTP-token bytes. */
	if (character >= '0' && character <= '9')
		return 1;

	/* These remaining literal token bytes cannot admit a slash, space or control byte. */
	switch (character) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return 1;
	default:
		return 0;
	}
}

/* Compares an already validated native essence with an ASCII canonical processing type. */
static int
resource_equal(
	const char *mime,
	size_t length,
	const char *expected)
{
	size_t expected_length;
	size_t index;
	unsigned char character;

	/* Complete length equality prevents prefix or trailing-garbage MIME matches. */
	expected_length = strlen(expected);
	if (length != expected_length)
		return 0;

	/* Compare the whole declared essence after folding only ASCII uppercase bytes. */
	for (index = 0; index < length; index++) {
		character = (unsigned char)mime[index];
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';

		/* Any differing canonical byte rejects this complete MIME essence. */
		if (character != (unsigned char)expected[index])
			return 0;
	}

	/* Succeeded: the actual complete essence matches without allocation or locale effects. */
	return 1;
}
