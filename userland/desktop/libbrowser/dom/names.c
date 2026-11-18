/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The element names the engine knows, and the lookup from a name to its
 * tag number.
 */

#include "dom/dom.h"

#include <string.h>

/* The longest known name ("annotation-xml"), in characters. */
#define NAMES_LONGEST		16U

/*
 * The names in enum dom_tag order, which is byte order, so a name is found
 * by binary search.  The first entry stands for unknown names.
 */
static const char *const dom_tag_names[DOM_TAG_COUNT] = {
	"",
	"a",
	"abbr",
	"acronym",
	"address",
	"annotation-xml",
	"applet",
	"area",
	"article",
	"aside",
	"audio",
	"b",
	"base",
	"basefont",
	"bdi",
	"bdo",
	"bgsound",
	"big",
	"blink",
	"blockquote",
	"body",
	"br",
	"button",
	"canvas",
	"caption",
	"center",
	"cite",
	"code",
	"col",
	"colgroup",
	"data",
	"datalist",
	"dd",
	"del",
	"desc",
	"details",
	"dfn",
	"dialog",
	"dir",
	"div",
	"dl",
	"dt",
	"em",
	"embed",
	"fieldset",
	"figcaption",
	"figure",
	"font",
	"footer",
	"foreignobject",
	"form",
	"frame",
	"frameset",
	"h1",
	"h2",
	"h3",
	"h4",
	"h5",
	"h6",
	"head",
	"header",
	"hgroup",
	"hr",
	"html",
	"i",
	"iframe",
	"image",
	"img",
	"input",
	"ins",
	"isindex",
	"kbd",
	"keygen",
	"label",
	"legend",
	"li",
	"link",
	"listing",
	"main",
	"malignmark",
	"map",
	"mark",
	"marquee",
	"math",
	"menu",
	"menuitem",
	"meta",
	"meter",
	"mglyph",
	"mi",
	"mn",
	"mo",
	"ms",
	"mtext",
	"multicol",
	"nav",
	"nextid",
	"nobr",
	"noembed",
	"noframes",
	"noscript",
	"object",
	"ol",
	"optgroup",
	"option",
	"output",
	"p",
	"param",
	"picture",
	"plaintext",
	"pre",
	"progress",
	"q",
	"rb",
	"rp",
	"rt",
	"rtc",
	"ruby",
	"s",
	"samp",
	"script",
	"search",
	"section",
	"select",
	"slot",
	"small",
	"source",
	"spacer",
	"span",
	"strike",
	"strong",
	"style",
	"sub",
	"summary",
	"sup",
	"svg",
	"table",
	"tbody",
	"td",
	"template",
	"textarea",
	"tfoot",
	"th",
	"thead",
	"time",
	"title",
	"tr",
	"track",
	"tt",
	"u",
	"ul",
	"var",
	"video",
	"wbr",
	"xmp",
};

/*
 * Finds the tag number of a lower case element name, or DOM_TAG_UNKNOWN.
 */
int
dom_tag_lookup(
	const uint16_t *units,
	size_t length)
{
	char name[NAMES_LONGEST + 1U];
	size_t index;
	int low;
	int high;
	int middle;
	int order;

	/* A name longer than any known one is unknown. */
	if (length == 0 || length > NAMES_LONGEST)
		return DOM_TAG_UNKNOWN;

	/* Copies the name as a C string; known names are ASCII. */
	for (index = 0; index < length; index++) {
		/* A character past ASCII makes the name unknown. */
		if (units[index] > 0x7fU)
			return DOM_TAG_UNKNOWN;

		/* Keeps the character. */
		name[index] = (char)units[index];
	}

	/* Ends the C string. */
	name[length] = '\0';

	/* Searches the sorted names after the unknown entry. */
	low = 1;
	high = DOM_TAG_COUNT - 1;
	while (low <= high) {
		middle = low + (high - low) / 2;
		order = strcmp(name, dom_tag_names[middle]);
		if (order == 0)
			return middle;

		/* Continues in the half that can hold the name. */
		if (order < 0) {
			high = middle - 1;
		} else {
			low = middle + 1;
		}
	}

	/* The name is not a known one. */
	return DOM_TAG_UNKNOWN;
}

/*
 * Names a tag number (the empty string for DOM_TAG_UNKNOWN).
 */
const char *
dom_tag_name(
	int tag)
{
	/* Anything outside the table is unknown. */
	if (tag <= DOM_TAG_UNKNOWN || tag >= DOM_TAG_COUNT)
		return "";

	/* Reports the name. */
	return dom_tag_names[tag];
}
