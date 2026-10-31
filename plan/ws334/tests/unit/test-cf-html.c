/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the CF_HTML clipboard format (win32/cf-html.h): the
 * bytes the encoder writes, its sizes and refusals, the decoder on data
 * built by hand, the round trip of the two, and the decoder on damaged
 * and random data.
 *
 * The expected bytes and offsets were worked out by hand from the layout
 * the header file describes (counting the bytes of each line, and finding
 * the markers in the text), not taken from the encoder.  Every decoded
 * block is first copied to a block of exactly its size, so that a read
 * past its end is caught by the address sanitizer.
 */

#include "check.h"

#include "userland/desktop/libkeiland/win32/cf-html.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of random blocks fed to the decoder. */
#define RANDOM_BLOCKS		100000U

/* The data's length when the fragment is empty and there is no URL: 105 header bytes, 34 opening, 18 comment, 18 closing. */
#define EMPTY_LENGTH		175U

/*
 * One fragment and URL to encode, and the data the encoder must write for
 * them.
 */
struct encode_case {
	const char *fragment;
	const char *url;
	const char *expected;
};

/*
 * One block of data to decode, and the answer worked out by hand: whether
 * it decodes, where the fragment lies, and the URL.
 */
struct decode_case {
	const char *name;
	const char *text;
	int decodes;
	size_t offset;
	size_t length;
	const char *url;
	int has_url;
};

/*
 * What one decoding gave, as offsets into the block that was decoded, so
 * that the answer outlives the copy of the block.
 */
struct decoded {
	int error;
	size_t offset;
	size_t length;
	int has_url;
	size_t url_offset;
	size_t url_length;
	int broken;
};

/*
 * The data the encoder must write.  Each line is counted by hand:
 * "Version:0.9" and its end is 13 bytes, each offset line is the name, 10
 * digits and the end (22, 20, 26, 24), so the header is 105 bytes and the
 * HTML starts there.
 */
static const struct encode_case encode_cases[] = {
	{
		"",
		NULL,
		"Version:0.9\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000175\r\n"
		"StartFragment:0000000139\r\n"
		"EndFragment:0000000139\r\n"
		"<html><body>\r\n"
		"<!--StartFragment--><!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
	{
		"<b>x</b>",
		NULL,
		"Version:0.9\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000183\r\n"
		"StartFragment:0000000139\r\n"
		"EndFragment:0000000147\r\n"
		"<html><body>\r\n"
		"<!--StartFragment--><b>x</b><!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
	{
		"\346\227\245\346\234\254",
		NULL,
		"Version:0.9\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000181\r\n"
		"StartFragment:0000000139\r\n"
		"EndFragment:0000000145\r\n"
		"<html><body>\r\n"
		"<!--StartFragment-->\346\227\245\346\234\254<!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
	{
		"<b>x</b>",
		"http://a/",
		"Version:0.9\r\n"
		"StartHTML:0000000126\r\n"
		"EndHTML:0000000204\r\n"
		"StartFragment:0000000160\r\n"
		"EndFragment:0000000168\r\n"
		"SourceURL:http://a/\r\n"
		"<html><body>\r\n"
		"<!--StartFragment--><b>x</b><!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
	{
		"",
		"",
		"Version:0.9\r\n"
		"StartHTML:0000000117\r\n"
		"EndHTML:0000000187\r\n"
		"StartFragment:0000000151\r\n"
		"EndFragment:0000000151\r\n"
		"SourceURL:\r\n"
		"<html><body>\r\n"
		"<!--StartFragment--><!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
	{
		"a\r\nb: c\001",
		"https://example.com/p?q=1",
		"Version:0.9\r\n"
		"StartHTML:0000000142\r\n"
		"EndHTML:0000000220\r\n"
		"StartFragment:0000000176\r\n"
		"EndFragment:0000000184\r\n"
		"SourceURL:https://example.com/p?q=1\r\n"
		"<html><body>\r\n"
		"<!--StartFragment-->a\r\n"
		"b: c\001<!--EndFragment-->\r\n"
		"</body>\r\n"
		"</html>"
	},
};

/*
 * The data to decode and what it holds.  The offsets of each were found
 * by searching the text for the markers, with the line ends the case
 * names.
 */
static const struct decode_case decode_cases[] = {
	{
		"version 1.0 crlf",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 131, 5, NULL, 0
	},
	{
		"version 0.9",
		"Version:0.9\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 131, 5, NULL, 0
	},
	{
		"japanese fragment",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000165\r\n"
		"StartFragment:0000000134\r\n"
		"EndFragment:0000000140\r\n"
		"<html>\343\201\202<!--StartFragment-->\346\227\245\346\234\254<!--EndFragment--></html>",
		1, 134, 6, NULL, 0
	},
	{
		"start html -1, end html -1",
		"Version:1.0\r\n"
		"StartHTML:-1\r\n"
		"EndHTML:-1\r\n"
		"StartFragment:0000000109\r\n"
		"EndFragment:0000000114\r\n"
		"<!--StartFragment-->HELLO<!--EndFragment-->",
		1, 109, 5, NULL, 0
	},
	{
		"start html -1, no end html",
		"Version:1.0\r\n"
		"StartHTML:-1\r\n"
		"StartFragment:0000000097\r\n"
		"EndFragment:0000000102\r\n"
		"<!--StartFragment-->HELLO<!--EndFragment-->",
		1, 97, 5, NULL, 0
	},
	{
		"html only",
		"Version:0.9\r\n"
		"StartHTML:0000000055\r\n"
		"EndHTML:0000000067\r\n"
		"<p>hello</p>",
		1, 55, 12, NULL, 0
	},
	{
		"fragment only",
		"Version:1.0\r\n"
		"StartFragment:0000000086\r\n"
		"EndFragment:0000000088\r\n"
		"<p><!--StartFragment-->AB<!--EndFragment--></p>",
		1, 86, 2, NULL, 0
	},
	{
		"fragment wins over html",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000168\r\n"
		"StartFragment:0000000136\r\n"
		"EndFragment:0000000139\r\n"
		"<p>head</p><!--StartFragment-->mid<!--EndFragment--><p>tail</p>",
		1, 136, 3, NULL, 0
	},
	{
		"lf only",
		"Version:1.0\n"
		"StartHTML:0000000100\n"
		"EndHTML:0000000147\n"
		"StartFragment:0000000123\n"
		"EndFragment:0000000125\n"
		"<p><!--StartFragment-->AB<!--EndFragment--></p>",
		1, 123, 2, NULL, 0
	},
	{
		"cr only",
		"Version:1.0\r"
		"StartHTML:0000000100\r"
		"EndHTML:0000000147\r"
		"StartFragment:0000000123\r"
		"EndFragment:0000000125\r"
		"<p><!--StartFragment-->AB<!--EndFragment--></p>",
		1, 123, 2, NULL, 0
	},
	{
		"unknown and selection lines",
		"Version:1.0\r\n"
		"Generator:abc:def\r\n"
		"X-Other_1:\r\n"
		"StartHTML:0000000181\r\n"
		"EndHTML:0000000220\r\n"
		"StartFragment:0000000201\r\n"
		"EndFragment:0000000202\r\n"
		"StartSelection:zzz\r\n"
		"EndSelection:0000000001\r\n"
		"<!--StartFragment-->Z<!--EndFragment-->",
		1, 201, 1, NULL, 0
	},
	{
		"many leading zeros",
		"Version:1.0\r\n"
		"StartHTML:0000000000000000000000000000000000000000156\r\n"
		"EndHTML:0000000000000000000000000212\r\n"
		"StartFragment:0000000182\r\n"
		"EndFragment:0000000187\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 182, 5, NULL, 0
	},
	{
		"offsets without padding",
		"Version:1.0\r\n"
		"StartHTML:76\r\n"
		"EndHTML:132\r\n"
		"StartFragment:102\r\n"
		"EndFragment:107\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 102, 5, NULL, 0
	},
	{
		"fragment may be empty",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000156\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000131\r\n"
		"<html><!--StartFragment--><!--EndFragment--></html>",
		1, 131, 0, NULL, 0
	},
	{
		"source url",
		"Version:1.0\r\n"
		"StartHTML:0000000144\r\n"
		"EndHTML:0000000200\r\n"
		"StartFragment:0000000170\r\n"
		"EndFragment:0000000175\r\n"
		"SourceURL:https://example.com/a?b=c:d\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 170, 5, "https://example.com/a?b=c:d", 1
	},
	{
		"source url first of two",
		"Version:1.0\r\n"
		"StartHTML:0000000135\r\n"
		"EndHTML:0000000191\r\n"
		"StartFragment:0000000161\r\n"
		"EndFragment:0000000166\r\n"
		"SourceURL:one\r\n"
		"SourceURL:two\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 161, 5, "one", 1
	},
	{
		"empty source url",
		"Version:1.0\r\n"
		"StartHTML:0000000117\r\n"
		"EndHTML:0000000173\r\n"
		"StartFragment:0000000143\r\n"
		"EndFragment:0000000148\r\n"
		"SourceURL:\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 143, 5, "", 1
	},
	{
		"source url before offsets",
		"Version:1.0\r\n"
		"SourceURL:http://x/\r\n"
		"StartHTML:0000000126\r\n"
		"EndHTML:0000000182\r\n"
		"StartFragment:0000000152\r\n"
		"EndFragment:0000000157\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 152, 5, "http://x/", 1
	},
	{
		"duplicate offset keeps first",
		"Version:1.0\r\n"
		"StartHTML:0000000131\r\n"
		"EndHTML:0000000187\r\n"
		"StartFragment:0000000157\r\n"
		"EndFragment:0000000162\r\n"
		"StartFragment:0000000001\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 157, 5, NULL, 0
	},
	{
		"version line after offsets",
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"Version:1.0\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 131, 5, NULL, 0
	},
	{
		"version value is not checked",
		"Version:9.9\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		1, 131, 5, NULL, 0
	},
	{
		"html body with colon after header",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000183\r\n"
		"StartFragment:0000000157\r\n"
		"EndFragment:0000000158\r\n"
		"<html><a href=\"http://x/y\">q</a><!--StartFragment-->t<!--EndFragment--></html>",
		1, 157, 1, NULL, 0
	},
	{
		"fragment holds a header look-alike",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000169\r\n"
		"StartFragment:0000000125\r\n"
		"EndFragment:0000000151\r\n"
		"<!--StartFragment-->StartHTML:5\r\n"
		"Version:1.0\r\n"
		"<!--EndFragment-->",
		1, 125, 26, NULL, 0
	},
	{
		"fragment holds a control byte",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000146\r\n"
		"StartFragment:0000000125\r\n"
		"EndFragment:0000000128\r\n"
		"<!--StartFragment-->a\001b<!--EndFragment-->",
		1, 125, 3, NULL, 0
	},
	{
		"no version",
		"StartHTML:0000000092\r\n"
		"EndHTML:0000000148\r\n"
		"StartFragment:0000000118\r\n"
		"EndFragment:0000000123\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"lower case version name",
		"version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"version with space before colon",
		"Version :1.0\r\n"
		"StartHTML:0000000106\r\n"
		"EndHTML:0000000162\r\n"
		"StartFragment:0000000132\r\n"
		"EndFragment:0000000137\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"lower case offset names are unknown",
		"Version:1.0\r\n"
		"startfragment:0000000089\r\n"
		"endfragment:0000000094\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"version only",
		"Version:1.0\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"blank line ends the header",
		"Version:1.0\r\n"
		"\r\n"
		"StartHTML:0000000107\r\n"
		"EndHTML:0000000163\r\n"
		"StartFragment:0000000133\r\n"
		"EndFragment:0000000138\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end fragment past the data",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000162\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end html past the data",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000162\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"start fragment past the data",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000162\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end before start in the fragment",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000136\r\n"
		"EndFragment:0000000131\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end before start in the html",
		"Version:1.0\r\n"
		"StartHTML:0000000111\r\n"
		"EndHTML:0000000055\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end before start by one",
		"Version:1.0\r\n"
		"StartFragment:0000000095\r\n"
		"EndFragment:0000000094\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is not a number",
		"Version:1.0\r\n"
		"StartHTML:0000000099\r\n"
		"EndHTML:0000000155\r\n"
		"StartFragment:12ab\r\n"
		"EndFragment:0000000130\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is empty",
		"Version:1.0\r\n"
		"StartHTML:0000000095\r\n"
		"EndHTML:0000000151\r\n"
		"StartFragment:\r\n"
		"EndFragment:0000000126\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset has a plus sign",
		"Version:1.0\r\n"
		"StartHTML:0000000098\r\n"
		"EndHTML:0000000154\r\n"
		"StartFragment:+30\r\n"
		"EndFragment:0000000129\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset has a leading space",
		"Version:1.0\r\n"
		"StartHTML:0000000099\r\n"
		"EndHTML:0000000155\r\n"
		"StartFragment: 125\r\n"
		"EndFragment:0000000130\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is hexadecimal",
		"Version:1.0\r\n"
		"StartHTML:0000000099\r\n"
		"EndHTML:0000000155\r\n"
		"StartFragment:0x10\r\n"
		"EndFragment:0000000130\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is -2",
		"Version:1.0\r\n"
		"StartHTML:0000000097\r\n"
		"EndHTML:0000000153\r\n"
		"StartFragment:-2\r\n"
		"EndFragment:0000000128\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is a minus sign",
		"Version:1.0\r\n"
		"StartHTML:0000000096\r\n"
		"EndHTML:0000000152\r\n"
		"StartFragment:-\r\n"
		"EndFragment:0000000127\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is -01",
		"Version:1.0\r\n"
		"StartHTML:0000000098\r\n"
		"EndHTML:0000000154\r\n"
		"StartFragment:-01\r\n"
		"EndFragment:0000000129\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset is huge",
		"Version:1.0\r\n"
		"StartHTML:0000000155\r\n"
		"EndHTML:0000000211\r\n"
		"StartFragment:999999999999999999999999999999999999999999999999999999999999\r\n"
		"EndFragment:0000000186\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offset overflows a size",
		"Version:1.0\r\n"
		"StartHTML:0000000115\r\n"
		"EndHTML:0000000171\r\n"
		"StartFragment:18446744073709551616\r\n"
		"EndFragment:0000000146\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end fragment is -1",
		"Version:1.0\r\n"
		"StartHTML:0000000097\r\n"
		"EndHTML:0000000153\r\n"
		"StartFragment:0000000123\r\n"
		"EndFragment:-1\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"start fragment is -1",
		"Version:1.0\r\n"
		"StartHTML:0000000097\r\n"
		"EndHTML:0000000153\r\n"
		"StartFragment:-1\r\n"
		"EndFragment:0000000128\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end html is -1 with a start",
		"Version:1.0\r\n"
		"StartHTML:0000000047\r\n"
		"EndHTML:-1\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"start html is -1 with an end",
		"Version:1.0\r\n"
		"StartHTML:-1\r\n"
		"EndHTML:0000000103\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"no html context and no fragment",
		"Version:1.0\r\n"
		"StartHTML:-1\r\n"
		"EndHTML:-1\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"only a start fragment",
		"Version:1.0\r\n"
		"StartFragment:0000000065\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"only an end html",
		"Version:1.0\r\n"
		"EndHTML:0000000089\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"header only, offsets at the end",
		"Version:1.0\r\n"
		"StartHTML:0000000055\r\n"
		"EndHTML:0000000055\r\n",
		0, 0, 0, NULL, 0
	},
	{
		"bad html pair with a good fragment",
		"Version:1.0\r\n"
		"StartHTML:0000000161\r\n"
		"EndHTML:0000000105\r\n"
		"StartFragment:0000000131\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"good html pair with a bad fragment",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:2000000000\r\n"
		"EndFragment:0000000136\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"only an end html of -1 with a fragment",
		"Version:1.0\r\n"
		"EndHTML:-1\r\n"
		"StartFragment:0000000101\r\n"
		"EndFragment:0000000106\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"end html of -1 with a start and a fragment",
		"Version:1.0\r\n"
		"StartHTML:0000000097\r\n"
		"EndHTML:-1\r\n"
		"StartFragment:0000000123\r\n"
		"EndFragment:0000000128\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"lone end fragment of -1 with an html pair",
		"Version:1.0\r\n"
		"StartHTML:0000000071\r\n"
		"EndHTML:0000000127\r\n"
		"EndFragment:-1\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"lone start fragment of -1 with an html pair",
		"Version:1.0\r\n"
		"StartHTML:0000000073\r\n"
		"EndHTML:0000000129\r\n"
		"StartFragment:-1\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offsets inside the header cut it",
		"Version:1.0\r\n"
		"StartHTML:0000000105\r\n"
		"EndHTML:0000000161\r\n"
		"StartFragment:0000000030\r\n"
		"EndFragment:0000000040\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"offsets inside the version line",
		"Version:1.0\r\n"
		"StartFragment:0000000005\r\n"
		"EndFragment:0000000010\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"example of the format's description",
		"Version:1.0\r\n"
		"StartHTML:0121\r\n"
		"EndHTML:0272\r\n"
		"StartFragment:0006\r\n"
		"EndFragment:0106\r\n"
		"StartSelection:0180\r\n"
		"EndSelection:0225\r\n"
		"<html><!--StartFragment--><body>This is normal. <b>This is bold.</b> <i><b>This is bold italic.</b> This is italic.</i></body><!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
	{
		"start html of -1 with an end html and a fragment",
		"Version:1.0\r\n"
		"StartHTML:-1\r\n"
		"EndHTML:0000000153\r\n"
		"StartFragment:0000000123\r\n"
		"EndFragment:0000000128\r\n"
		"<html><!--StartFragment-->HELLO<!--EndFragment--></html>",
		0, 0, 0, NULL, 0
	},
};

/* The state of the pseudo-random generator that makes the damaged blocks; a fixed seed makes each run the same. */
static uint32_t random_state = 20260101U;

static void test_encode_exact(void);
static void test_encode_sizes(void);
static void test_encode_errors(void);
static void test_encode_limit(void);
static void test_decode_cases(void);
static void test_decode_arguments(void);
static void test_round_trip(void);
static void test_truncation(void);
static void test_mutation(void);
static void test_random(void);
static void check_round_trip(const char *fragment, const char *url);
static int make_encoded(const char *fragment, const char *url, char **data, size_t *length);
static int decode_copy(const char *bytes, size_t length, struct decoded *answer);
static unsigned count_violations(const struct decoded *answer, size_t length);
static char *exact_copy(const char *bytes, size_t length);
static uint32_t random_next(void);

/*
 * Runs every group of checks.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The runner passes the system the test is built for. */
	if (argc < 2) {
		printf("test-cf-html: FAIL (no system argument)\n");
		return 1;
	}
	if (strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0) {
		printf("test-cf-html: FAIL (unknown system %s)\n", argv[1]);
		return 1;
	}

	/* Runs the groups of checks. */
	test_encode_exact();
	test_encode_sizes();
	test_encode_errors();
	test_encode_limit();
	test_decode_cases();
	test_decode_arguments();
	test_round_trip();
	test_truncation();
	test_mutation();
	test_random();

	/* Prints the summary line. */
	status = check_finish("cf-html");

	/* Reports the status the summary decided. */
	return status;
}

/*
 * Encodes each case into a buffer of exactly the size it needs and
 * compares the bytes with the hand-made data.
 */
static void
test_encode_exact(void)
{
	size_t index;
	size_t expected_length;
	size_t out_length;
	char *buffer;
	int error;

	/* Checks every case. */
	for (index = 0U;
	     index < sizeof(encode_cases) / sizeof(encode_cases[0]);
	     index++) {
		expected_length = strlen(encode_cases[index].expected);

		/* Encodes into a block that holds the data and its NUL and nothing more. */
		buffer = malloc(expected_length + 1U);
		CHECK(buffer != NULL);
		if (buffer == NULL)
			return;
		out_length = 0U;
		error = keiui_win32_cf_html_encode(encode_cases[index].fragment, strlen(encode_cases[index].fragment), encode_cases[index].url, buffer, expected_length + 1U, &out_length);

		/* The data is exactly the hand-made one, followed by the NUL. */
		CHECK_EQ(error, 0);
		CHECK_EQ(out_length, expected_length);
		CHECK(memcmp(buffer, encode_cases[index].expected, expected_length + 1U) == 0);
		free(buffer);
	}

	/* An empty fragment may be given as a NULL pointer too. */
	buffer = malloc(EMPTY_LENGTH + 1U);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	out_length = 0U;
	error = keiui_win32_cf_html_encode(NULL, 0U, NULL, buffer, EMPTY_LENGTH + 1U, &out_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(out_length, EMPTY_LENGTH);
	CHECK(memcmp(buffer, encode_cases[0].expected, EMPTY_LENGTH + 1U) == 0);
	free(buffer);
}

/*
 * Checks the buffer sizes around the size the data needs: the size of the
 * data and its NUL succeeds, one less is ERANGE with an empty string and
 * the needed length, and a size of 0 touches nothing.
 */
static void
test_encode_sizes(void)
{
	char buffer[400];
	size_t out_length;
	size_t size;
	int error;

	/* The size of the data plus the NUL is enough. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode("<b>x</b>", 8U, NULL, buffer, 184U, &out_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(out_length, 183U);
	CHECK_EQ(buffer[183], 0);
	CHECK(strcmp(buffer, encode_cases[1].expected) == 0);

	/* The bytes after the NUL are not written. */
	CHECK_EQ(buffer[184], 'Z');

	/* One byte less cannot hold the NUL: the buffer holds an empty string and the length needed is reported. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode("<b>x</b>", 8U, NULL, buffer, 183U, &out_length);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(out_length, 183U);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(buffer[1], 'Z');

	/* Every smaller size gives the same refusal. */
	for (size = 1U;
	     size < 183U;
	     size += 17U) {
		memset(buffer, 'Z', sizeof(buffer));
		out_length = 99U;
		error = keiui_win32_cf_html_encode("<b>x</b>", 8U, NULL, buffer, size, &out_length);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(out_length, 183U);
		CHECK_EQ(buffer[0], 0);
		CHECK_EQ(buffer[size], 'Z');
	}

	/* A size of 0 is ERANGE and writes nothing, not even the empty string. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode("<b>x</b>", 8U, NULL, buffer, 0U, &out_length);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(out_length, 183U);
	CHECK_EQ(buffer[0], 'Z');

	/* A URL counts: its line is 10 + 9 + 2 = 21 bytes, so the same fragment needs 204. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode("<b>x</b>", 8U, "http://a/", buffer, 204U, &out_length);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(out_length, 204U);
	CHECK_EQ(buffer[0], 0);
	error = keiui_win32_cf_html_encode("<b>x</b>", 8U, "http://a/", buffer, 205U, &out_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(out_length, 204U);
	CHECK(strcmp(buffer, encode_cases[3].expected) == 0);
}

/*
 * Checks the refusals that are EINVAL: a URL with a line break, a NULL
 * fragment with a length, and NULL outputs.
 */
static void
test_encode_errors(void)
{
	static const char *const bad_urls[] = {
		"a\rb",
		"a\nb",
		"a\r\nb",
		"\r",
		"\n",
		"http://x/\r",
		"http://x/\n",
		"\r\nSourceURL:y"
	};
	char buffer[400];
	size_t out_length;
	size_t index;
	int error;

	/* A URL with a CR or an LF anywhere is refused, leaving an empty string and a length of 0. */
	for (index = 0U;
	     index < sizeof(bad_urls) / sizeof(bad_urls[0]);
	     index++) {
		memset(buffer, 'Z', sizeof(buffer));
		out_length = 99U;
		error = keiui_win32_cf_html_encode("x", 1U, bad_urls[index], buffer, sizeof(buffer), &out_length);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(buffer[0], 0);
		CHECK_EQ(out_length, 0U);
	}

	/* A fragment with a length and no bytes is refused. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode(NULL, 1U, NULL, buffer, sizeof(buffer), &out_length);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(out_length, 0U);

	/* A NULL buffer is refused, and so is a NULL place for the length. */
	out_length = 99U;
	error = keiui_win32_cf_html_encode("x", 1U, NULL, NULL, 0U, &out_length);
	CHECK_EQ(error, EINVAL);
	error = keiui_win32_cf_html_encode("x", 1U, NULL, NULL, 400U, &out_length);
	CHECK_EQ(error, EINVAL);
	memset(buffer, 'Z', sizeof(buffer));
	error = keiui_win32_cf_html_encode("x", 1U, NULL, buffer, sizeof(buffer), NULL);
	CHECK_EQ(error, EINVAL);

	/* A URL that is an empty string is not a bad one: its line is "SourceURL:" and the line end. */
	memset(buffer, 'Z', sizeof(buffer));
	error = keiui_win32_cf_html_encode("", 0U, "", buffer, sizeof(buffer), &out_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(out_length, 187U);
	CHECK(strcmp(buffer, encode_cases[4].expected) == 0);
}

/*
 * Checks the limit of the ten digit offsets, with lengths that are never
 * read: the encoder refuses before it writes, and the sums do not wrap.
 */
static void
test_encode_limit(void)
{
	static const char one_byte[1] = { 'x' };
	char buffer[16];
	size_t out_length;
	int error;

	/* The largest length that is not above the limit is ERANGE here, since the buffer is small, and says what it needs. */
	if (sizeof(size_t) > 4U) {
		memset(buffer, 'Z', sizeof(buffer));
		out_length = 99U;
		error = keiui_win32_cf_html_encode(one_byte, (size_t)9999999824ULL, NULL, buffer, sizeof(buffer), &out_length);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(buffer[0], 0);
		CHECK(out_length == (size_t)9999999999ULL);

		/* One byte more is above the limit. */
		memset(buffer, 'Z', sizeof(buffer));
		out_length = 99U;
		error = keiui_win32_cf_html_encode(one_byte, (size_t)9999999825ULL, NULL, buffer, sizeof(buffer), &out_length);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(buffer[0], 0);
		CHECK_EQ(out_length, 0U);

		/* A URL takes from the limit: "abc" makes a line of 10 + 3 + 2 = 15 bytes. */
		error = keiui_win32_cf_html_encode(one_byte, (size_t)9999999809ULL, "abc", buffer, sizeof(buffer), &out_length);
		CHECK_EQ(error, ERANGE);
		CHECK(out_length == (size_t)9999999999ULL);
		error = keiui_win32_cf_html_encode(one_byte, (size_t)9999999810ULL, "abc", buffer, sizeof(buffer), &out_length);
		CHECK_EQ(error, EINVAL);
	}

	/* A length near the top of a size would wrap in a sum. */
	memset(buffer, 'Z', sizeof(buffer));
	out_length = 99U;
	error = keiui_win32_cf_html_encode(one_byte, SIZE_MAX, NULL, buffer, sizeof(buffer), &out_length);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out_length, 0U);
	error = keiui_win32_cf_html_encode(one_byte, SIZE_MAX - 100U, NULL, buffer, sizeof(buffer), &out_length);
	CHECK_EQ(error, EINVAL);
	error = keiui_win32_cf_html_encode(one_byte, SIZE_MAX - 175U, "abc", buffer, sizeof(buffer), &out_length);
	CHECK_EQ(error, EINVAL);
}

/*
 * Decodes each hand-made block and compares where the fragment lies and
 * what the URL is.  The same block is decoded with and without a NUL
 * added to its length.
 */
static void
test_decode_cases(void)
{
	size_t index;
	size_t with_nul;
	size_t length;
	struct decoded answer;
	char *copy;
	int error;

	/* Checks every case. */
	for (index = 0U;
	     index < sizeof(decode_cases) / sizeof(decode_cases[0]);
	     index++) {
		length = strlen(decode_cases[index].text);

		/* Decodes a block of exactly the text. */
		error = decode_copy(decode_cases[index].text, length, &answer);
		if (decode_cases[index].decodes) {
			if (error != 0)
				printf("FAIL decode case \"%s\" gave %d\n", decode_cases[index].name, error);
			CHECK_EQ(error, 0);
			if (answer.offset != decode_cases[index].offset || answer.length != decode_cases[index].length)
				printf("FAIL decode case \"%s\" gave %zu, %zu\n", decode_cases[index].name, answer.offset, answer.length);
			CHECK_EQ(answer.offset, decode_cases[index].offset);
			CHECK_EQ(answer.length, decode_cases[index].length);
			CHECK_EQ(answer.has_url, decode_cases[index].has_url);
			if (decode_cases[index].has_url) {
				CHECK_EQ(answer.url_length, strlen(decode_cases[index].url));
				CHECK(memcmp(decode_cases[index].text + answer.url_offset, decode_cases[index].url, answer.url_length) == 0);
			}
		} else {
			if (error != EINVAL)
				printf("FAIL decode case \"%s\" gave %d, expected EINVAL\n", decode_cases[index].name, error);
			CHECK_EQ(error, EINVAL);
			CHECK_EQ(answer.offset, 0U);
			CHECK_EQ(answer.length, 0U);
			CHECK_EQ(answer.has_url, 0);
		}
		CHECK_EQ(answer.broken, 0);

		/*
		 * Decodes a block that decodes again with the NUL of the clipboard's
		 * block counted in the length.  A block that is refused is not tried
		 * again, since some are refused for an offset one past the end, which
		 * the NUL makes a position inside.
		 */
		if (!decode_cases[index].decodes)
			continue;
		with_nul = length + 1U;
		copy = exact_copy(decode_cases[index].text, with_nul);
		CHECK(copy != NULL);
		if (copy == NULL)
			return;
		error = decode_copy(copy, with_nul, &answer);
		CHECK_EQ(error, 0);
		CHECK_EQ(answer.offset, decode_cases[index].offset);
		CHECK_EQ(answer.length, decode_cases[index].length);
		CHECK_EQ(answer.broken, 0);
		free(copy);
	}
}

/*
 * Checks the arguments the decoder refuses, and the outputs it may leave
 * out.
 */
static void
test_decode_arguments(void)
{
	static const char data[] =
	    "Version:1.0\r\n"
	    "StartHTML:0000000118\r\n"
	    "EndHTML:0000000174\r\n"
	    "StartFragment:0000000144\r\n"
	    "EndFragment:0000000149\r\n"
	    "SourceURL:u\r\n"
	    "<html><!--StartFragment-->HELLO<!--EndFragment--></html>";
	const char *fragment;
	const char *url;
	size_t fragment_length;
	size_t url_length;
	char *copy;
	int error;

	/* NULL data with a length is refused and leaves the outputs empty. */
	fragment = data;
	fragment_length = 99U;
	url = data;
	url_length = 99U;
	error = keiui_win32_cf_html_decode(NULL, 10U, &fragment, &fragment_length, &url, &url_length);
	CHECK_EQ(error, EINVAL);
	CHECK(fragment == NULL);
	CHECK_EQ(fragment_length, 0U);
	CHECK(url == NULL);
	CHECK_EQ(url_length, 0U);

	/* NULL data with no length has no Version line. */
	error = keiui_win32_cf_html_decode(NULL, 0U, &fragment, &fragment_length, &url, &url_length);
	CHECK_EQ(error, EINVAL);

	/* Data with no bytes has no Version line either. */
	copy = exact_copy("", 0U);
	CHECK(copy != NULL);
	if (copy == NULL)
		return;
	error = keiui_win32_cf_html_decode(copy, 0U, &fragment, &fragment_length, &url, &url_length);
	CHECK_EQ(error, EINVAL);
	CHECK(fragment == NULL);
	free(copy);

	/* A missing place for the fragment is refused. */
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, NULL, &fragment_length, &url, &url_length);
	CHECK_EQ(error, EINVAL);
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, &fragment, NULL, &url, &url_length);
	CHECK_EQ(error, EINVAL);

	/* The URL outputs may be left out together or one at a time. */
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, &fragment, &fragment_length, NULL, NULL);
	CHECK_EQ(error, 0);
	CHECK(fragment == data + 144);
	CHECK_EQ(fragment_length, 5U);
	url = NULL;
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, &fragment, &fragment_length, &url, NULL);
	CHECK_EQ(error, 0);
	CHECK(url == data + 115);
	url_length = 0U;
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, &fragment, &fragment_length, NULL, &url_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(url_length, 1U);

	/* The URL starts after the 105 bytes of the other lines and the 10 of "SourceURL:", and is the one byte "u". */
	url = NULL;
	url_length = 0U;
	error = keiui_win32_cf_html_decode(data, sizeof(data) - 1U, &fragment, &fragment_length, &url, &url_length);
	CHECK_EQ(error, 0);
	CHECK(url != NULL);
	CHECK_EQ(url_length, 1U);
	CHECK_EQ(url[0], 'u');
}

/*
 * Encodes fragments and decodes the results.
 */
static void
test_round_trip(void)
{
	check_round_trip("", NULL);
	check_round_trip("<b>x</b>", NULL);
	check_round_trip("\346\227\245\346\234\254\343\201\256\343\203\206\343\202\255\343\202\271\343\203\210", "https://example.com/\346\227\245?q=a:b#c");
	check_round_trip("a\r\nb: c\nVersion:1.0\r\n", "");
	check_round_trip("<!--StartFragment--><!--EndFragment-->", "http://x/");
	check_round_trip("<p>one</p><p>two</p>", "file:///C:/a%20b.html");
}

/*
 * Decodes every shorter length of a block that has an exact length.  Each
 * one must be refused: the end of the HTML is past it.
 */
static void
test_truncation(void)
{
	struct decoded answer;
	char *data;
	char *copy;
	size_t length;
	size_t cut;
	int error;

	/* Makes a block that has a URL. */
	error = make_encoded("<i>t</i>", "http://a/", &data, &length);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* The whole block decodes. */
	error = decode_copy(data, length, &answer);
	CHECK_EQ(error, 0);
	CHECK_EQ(answer.broken, 0);

	/* Every block cut short is refused, whether the cut is in the header or the HTML. */
	for (cut = 0U;
	     cut < length;
	     cut++) {
		copy = exact_copy(data, cut);
		CHECK(copy != NULL);
		if (copy == NULL)
			break;
		error = decode_copy(copy, cut, &answer);
		if (error != EINVAL)
			printf("FAIL cut at %zu gave %d\n", cut, error);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(answer.broken, 0);
		free(copy);
	}
	free(data);
}

/*
 * Changes each byte of a good block to each of the 256 values and decodes
 * it: whatever the answer, it must keep inside the block, and the changes
 * of the HTML bytes must still decode.
 */
static void
test_mutation(void)
{
	struct decoded answer;
	char *data;
	char *copy;
	size_t length;
	size_t position;
	unsigned value;
	unsigned violations;
	unsigned successes;
	int error;

	/* Makes a block that has a URL. */
	error = make_encoded("<b>mutate</b>", "http://a/b", &data, &length);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* Decodes every changed copy. */
	violations = 0U;
	successes = 0U;
	for (position = 0U;
	     position < length;
	     position++) {
		for (value = 0U;
		     value < 256U;
		     value++) {
			copy = exact_copy(data, length);
			CHECK(copy != NULL);
			if (copy == NULL)
				break;
			copy[position] = (char)value;
			error = decode_copy(copy, length, &answer);
			violations += count_violations(&answer, length);
			if (error == 0)
				successes++;
			free(copy);
		}
	}
	free(data);

	/* No answer left the block, and changes that leave the header alone still decode. */
	CHECK_EQ(violations, 0U);
	CHECK(successes > 256U);
}

/*
 * Decodes random bytes and random strings of header words and numbers,
 * which reach the offset checks that random bytes alone seldom do.
 */
static void
test_random(void)
{
	static const char *const words[] = {
		"Version:", "1.0", "0.9", "StartHTML:", "EndHTML:", "StartFragment:", "EndFragment:",
		"StartSelection:", "EndSelection:", "SourceURL:", "http://x/", "\r\n", "\n", "\r", ":",
		"-1", "-", "0", "00", "1", "9", "17", "42", "60", "99999999999999999999999999",
		"<!--StartFragment-->", "<!--EndFragment-->", "<html>", "x", " "
	};
	static const char *const offset_names[4] = {
		"StartHTML:", "EndHTML:", "StartFragment:", "EndFragment:"
	};
	struct decoded answer;
	char block[2048];
	char *copy;
	size_t count;
	size_t length;
	size_t index;
	size_t word;
	size_t word_length;
	unsigned round;
	unsigned violations;
	unsigned successes;
	int error;

	/* Decodes each random block. */
	violations = 0U;
	successes = 0U;
	for (round = 0U;
	     round < RANDOM_BLOCKS;
	     round++) {
		length = 0U;
		if ((round & 3U) == 0U) {
			/* Makes a block of random bytes, mostly printable ones. */
			count = random_next() % 300U;
			for (index = 0U;
			     index < count;
			     index++) {
				if ((random_next() & 3U) == 0U)
					block[length++] = (char)(random_next() & 0xFFU);
				else
					block[length++] = (char)(32U + random_next() % 95U);
			}
		} else if ((round & 3U) == 2U) {
			/* Makes a block with a header of the right form and offsets that are small numbers, or -1. */
			length = (size_t)sprintf(block, "Version:1.0\r\n");
			for (index = 0U;
			     index < 4U;
			     index++) {
				if ((random_next() & 7U) == 0U)
					length += (size_t)sprintf(block + length, "%s-1\r\n", offset_names[index]);
				else
					length += (size_t)sprintf(block + length, "%s%u\r\n", offset_names[index], 100U + random_next() % 120U);
			}
			count = random_next() % 100U;
			for (index = 0U;
			     index < count;
			     index++)
				block[length++] = (char)(32U + random_next() % 95U);
		} else {
			/* Makes a block of words a header is made of. */
			count = 1U + random_next() % 40U;
			for (index = 0U;
			     index < count;
			     index++) {
				word = random_next() % (sizeof(words) / sizeof(words[0]));
				word_length = strlen(words[word]);
				memcpy(block + length, words[word], word_length);
				length += word_length;
			}
		}

		/* Decodes a block of exactly this length. */
		copy = exact_copy(block, length);
		CHECK(copy != NULL);
		if (copy == NULL)
			return;
		error = decode_copy(copy, length, &answer);
		violations += count_violations(&answer, length);
		if (error == 0)
			successes++;
		free(copy);
	}

	/* No answer left its block. */
	CHECK_EQ(violations, 0U);

	/* The words reach the success path at least sometimes, so the sweep is not only refusals. */
	CHECK(successes > 0U);
}

/*
 * Encodes a fragment and a URL and decodes the data, both with and without
 * the NUL in the length, and compares the fragment and the URL.
 */
static void
check_round_trip(
	const char *fragment,
	const char *url)
{
	struct decoded answer;
	char *data;
	char *copy;
	size_t length;
	size_t fragment_length;
	int error;

	/* Encodes into a block of exactly the size of the data and its NUL. */
	error = make_encoded(fragment, url, &data, &length);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	fragment_length = strlen(fragment);

	/* Decodes the data without the NUL in the length. */
	copy = exact_copy(data, length);
	CHECK(copy != NULL);
	if (copy == NULL)
		return;
	error = decode_copy(copy, length, &answer);
	CHECK_EQ(error, 0);
	CHECK_EQ(answer.length, fragment_length);
	CHECK(memcmp(copy + answer.offset, fragment, fragment_length) == 0);
	CHECK_EQ(answer.has_url, url != NULL);
	if (url != NULL) {
		CHECK_EQ(answer.url_length, strlen(url));
		CHECK(memcmp(copy + answer.url_offset, url, answer.url_length) == 0);
	}
	CHECK_EQ(answer.broken, 0);
	free(copy);

	/* Decodes the data with the NUL in the length. */
	copy = exact_copy(data, length + 1U);
	CHECK(copy != NULL);
	if (copy == NULL)
		return;
	error = decode_copy(copy, length + 1U, &answer);
	CHECK_EQ(error, 0);
	CHECK_EQ(answer.length, fragment_length);
	CHECK(memcmp(copy + answer.offset, fragment, fragment_length) == 0);
	CHECK_EQ(answer.has_url, url != NULL);
	CHECK_EQ(answer.broken, 0);
	free(copy);
	free(data);
}

/*
 * Encodes a fragment into a new block of the size of the data and its NUL.
 */
static int
make_encoded(
	const char *fragment,
	const char *url,
	char **data,
	size_t *length)
{
	char none[1];
	size_t needed;
	int error;

	/* Asks for the length the data needs, with no room to write it. */
	needed = 0U;
	error = keiui_win32_cf_html_encode(fragment, strlen(fragment), url, none, 0U, &needed);
	if (error != ERANGE)
		return EINVAL;

	/* Allocates the block and writes the data into it. */
	*data = malloc(needed + 1U);
	if (*data == NULL)
		return ENOMEM;
	error = keiui_win32_cf_html_encode(fragment, strlen(fragment), url, *data, needed + 1U, length);
	if (error != 0) {
		free(*data);
		return error;
	}

	/* Succeeded: the block holds the data and its NUL. */
	return 0;
}

/*
 * Decodes bytes and gives the answer as offsets into them.  The outputs
 * are set to values that no decoding gives before the call, so that a
 * decoder which leaves them alone is noticed.
 */
static int
decode_copy(
	const char *bytes,
	size_t length,
	struct decoded *answer)
{
	const char *fragment;
	const char *url;
	size_t fragment_length;
	size_t url_length;
	char *copy;
	int error;

	/* Copies the bytes to a block of exactly their length. */
	copy = exact_copy(bytes, length);
	if (copy == NULL) {
		memset(answer, 0, sizeof(*answer));
		answer->broken = 1;
		return ENOMEM;
	}

	/* Decodes with outputs that look like an answer. */
	fragment = copy;
	fragment_length = 12345U;
	url = copy;
	url_length = 12345U;
	error = keiui_win32_cf_html_decode(copy, length, &fragment, &fragment_length, &url, &url_length);

	/* Turns the pointers into offsets, noting any that are not inside the block. */
	memset(answer, 0, sizeof(*answer));
	answer->error = error;
	if (error != 0) {
		if (fragment != NULL || fragment_length != 0U || url != NULL || url_length != 0U)
			answer->broken = 1;
	} else {
		if (fragment < copy || (size_t)(fragment - copy) > length || fragment_length > length - (size_t)(fragment - copy)) {
			answer->broken = 1;
		} else {
			answer->offset = (size_t)(fragment - copy);
			answer->length = fragment_length;
		}
		if (url != NULL) {
			if (url < copy || (size_t)(url - copy) > length || url_length > length - (size_t)(url - copy)) {
				answer->broken = 1;
			} else {
				answer->has_url = 1;
				answer->url_offset = (size_t)(url - copy);
				answer->url_length = url_length;
			}
		} else if (url_length != 0U) {
			answer->broken = 1;
		}
	}
	free(copy);

	/* Reports the decoder's own answer. */
	return error;
}

/*
 * Counts what is wrong with an answer: the pointers outside the block
 * and the outputs a refusal must leave empty.
 */
static unsigned
count_violations(
	const struct decoded *answer,
	size_t length)
{
	unsigned violations;

	/* Counts a broken answer. */
	violations = 0U;
	if (answer->broken)
		violations++;
	if (answer->offset > length)
		violations++;
	if (answer->length > length - answer->offset)
		violations++;

	/* Reports the count. */
	return violations;
}

/*
 * Copies bytes to a new block of exactly their length (one byte when
 * there are none).
 */
static char *
exact_copy(
	const char *bytes,
	size_t length)
{
	char *copy;

	/* Allocates the block, which has at least one byte so that an empty copy has an address. */
	copy = malloc(length > 0U ? length : 1U);
	if (copy == NULL)
		return NULL;

	/* Copies up to the length of the bytes, and the NUL after them when the length asks for it. */
	if (length > 0U)
		memcpy(copy, bytes, length);

	/* Succeeded: the block holds the bytes. */
	return copy;
}

/*
 * Gives the next 32 random bits.
 */
static uint32_t
random_next(void)
{
	/* Steps a xorshift generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the new state is the number. */
	return random_state;
}
