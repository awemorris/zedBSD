/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of src/libc/iconv.c (ws034-p040).
 *
 * The file is built into this program against the host's headers, so its
 * iconv_open/iconv/iconv_close take the place of the host C library's.  The
 * host's own functions stay reachable through dlsym(RTLD_NEXT), and random
 * inputs are converted by both to see that they agree.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef iconv_t (*open_fn)(const char *, const char *);
typedef size_t (*conv_fn)(iconv_t, char **, size_t *, char **, size_t *);
typedef int (*close_fn)(iconv_t);

struct result {
	size_t value;
	int error;
	size_t consumed;
	size_t produced;
	char output[256];
};

static void
run(open_fn o, conv_fn c, close_fn cl, const char *to, const char *from,
    const char *input, size_t length, size_t room, struct result *r)
{
	iconv_t cd = o(to, from);
	char *in = (char *)input;
	char *out = r->output;
	size_t in_left = length;
	size_t out_left = room;

	assert(cd != (iconv_t)-1);
	memset(r->output, 0, sizeof(r->output));
	errno = 0;
	r->value = c(cd, &in, &in_left, &out, &out_left);
	r->error = r->value == (size_t)-1 ? errno : 0;
	r->consumed = length - in_left;
	r->produced = room - out_left;
	cl(cd);
}

/* Whether a lead byte, with the byte after it if any, can never be valid. */
static int
never_valid(const unsigned char *p, size_t n)
{
	if (p[0] >= 0xf5)
		return 1;
	if (n < 2)
		return 0;
	return (p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
	       (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90);
}

static void
ours(const char *to, const char *from, const char *input, size_t length,
     size_t room, struct result *r)
{
	run(iconv_open, (conv_fn)iconv, iconv_close, to, from, input, length,
	    room, r);
}

static void
expect(const char *to, const char *from, const char *input, size_t length,
       size_t room, size_t value, int error, size_t consumed,
       const char *output, size_t produced)
{
	struct result r;

	ours(to, from, input, length, room, &r);
	if (r.value != value || r.error != error || r.consumed != consumed ||
	    r.produced != produced || memcmp(r.output, output, produced) != 0) {
		fprintf(stderr, "FAIL %s<-%s len=%zu: value=%zd error=%d "
			"consumed=%zu produced=%zu\n", to, from, length,
			(ssize_t)r.value, r.error, r.consumed, r.produced);
		exit(1);
	}
}

int
main(void)
{
	static const char *good[] = { "UTF-8", "utf8", "ASCII", "us-ascii",
		"ANSI_X3.4-1968", "646", "", "UTF-8//TRANSLIT",
		"ascii//TRANSLIT//IGNORE" };
	static const char *bad[] = { "ISO-8859-1", "UTF-16", "UTF-8//BOGUS",
		"ASCI", "UTF-88" };
	open_fn host_open = (open_fn)dlsym(RTLD_NEXT, "iconv_open");
	conv_fn host_conv = (conv_fn)dlsym(RTLD_NEXT, "iconv");
	close_fn host_close = (close_fn)dlsym(RTLD_NEXT, "iconv_close");
	struct result a, b;
	char input[16];
	size_t i, n, length, compared = 0, lenient = 0;
	unsigned seed = 12345;
	iconv_t cd;
	const char *target;

	/* Names. */
	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		cd = iconv_open(good[i], "UTF-8");
		assert(cd != (iconv_t)-1);
		iconv_close(cd);
	}
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		errno = 0;
		assert(iconv_open(bad[i], "UTF-8") == (iconv_t)-1 && errno == EINVAL);
		errno = 0;
		assert(iconv_open("UTF-8", bad[i]) == (iconv_t)-1 && errno == EINVAL);
	}

	/* Valid UTF-8 passes through: 1, 2, 3 and 4 byte characters. */
	expect("UTF-8", "UTF-8", "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10, 64,
	       0, 0, 10, "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10);
	/* Boundaries: U+007F, U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF. */
	expect("UTF-8", "UTF-8", "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"
	       "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 19, 64, 0, 0, 19,
	       "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"
	       "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 19);

	/* Invalid input stops at its first byte. */
	expect("UTF-8", "UTF-8", "ab\xc0\x80", 4, 64, (size_t)-1, EILSEQ, 2, "ab", 2);
	expect("UTF-8", "UTF-8", "\xe0\x80\x80", 3, 64, (size_t)-1, EILSEQ, 0, "", 0);
	expect("UTF-8", "UTF-8", "\xf0\x80\x80\x80", 4, 64, (size_t)-1, EILSEQ, 0, "", 0);
	expect("UTF-8", "UTF-8", "\xed\xa0\x80", 3, 64, (size_t)-1, EILSEQ, 0, "", 0);
	expect("UTF-8", "UTF-8", "\xf4\x90\x80\x80", 4, 64, (size_t)-1, EILSEQ, 0, "", 0);
	expect("UTF-8", "UTF-8", "x\x80", 2, 64, (size_t)-1, EILSEQ, 1, "x", 1);
	expect("UTF-8", "UTF-8", "\xff", 1, 64, (size_t)-1, EILSEQ, 0, "", 0);
	expect("UTF-8", "UTF-8", "\xe2\x28\xa1", 3, 64, (size_t)-1, EILSEQ, 0, "", 0);

	/* Input that ends inside a character: EINVAL, left unread. */
	expect("UTF-8", "UTF-8", "a\xe2\x82", 3, 64, (size_t)-1, EINVAL, 1, "a", 1);
	expect("UTF-8", "UTF-8", "\xf0\x9f\x98", 3, 64, (size_t)-1, EINVAL, 0, "", 0);

	/* A full output stops before the character that does not fit. */
	expect("UTF-8", "UTF-8", "a\xe2\x82\xac", 4, 3, (size_t)-1, E2BIG, 1, "a", 1);
	expect("UTF-8", "UTF-8", "abc", 3, 0, (size_t)-1, E2BIG, 0, "", 0);

	/* UTF-8 to ASCII: refused, replaced or left out. */
	expect("ASCII", "UTF-8", "a\xc3\xa9z", 4, 64, (size_t)-1, EILSEQ, 1, "a", 1);
	expect("ASCII//TRANSLIT", "UTF-8", "a\xc3\xa9z", 4, 64, 1, 0, 4, "a?z", 3);
	expect("ASCII//IGNORE", "UTF-8", "a\xc3\xa9z", 4, 64, 1, 0, 4, "az", 2);
	expect("UTF-8//IGNORE", "UTF-8", "a\xffz", 3, 64, 1, 0, 3, "az", 2);
	expect("ASCII//TRANSLIT", "UTF-8", "\xc3\xa9", 2, 0, (size_t)-1, E2BIG, 0, "", 0);

	/* ASCII to UTF-8: a byte above 127 is not ASCII. */
	expect("UTF-8", "ASCII", "ok\x80", 3, 64, (size_t)-1, EILSEQ, 2, "ok", 2);
	expect("UTF-8", "US-ASCII", "plain", 5, 64, 0, 0, 5, "plain", 5);

	/* No input: nothing to reset or write. */
	cd = iconv_open("UTF-8", "UTF-8");
	assert(iconv(cd, NULL, NULL, NULL, NULL) == 0);
	iconv_close(cd);

	/* Random inputs, compared with the host C library. */
	if (host_open != NULL && host_conv != NULL && host_close != NULL) {
		for (n = 0; n < 200000; n++) {
			length = (size_t)(rand_r(&seed) % 9);
			for (i = 0; i < length; i++) {
				unsigned pick = (unsigned)rand_r(&seed) % 8;
				static const unsigned char bytes[] = { 0x41, 0x7f,
					0x80, 0xbf, 0xc2, 0xdf, 0xe0, 0xed, 0xef, 0xf0,
					0xf4, 0xf5, 0x90, 0xa0, 0x8f, 0xc0 };
				input[i] = (char)(pick < 3 ? rand_r(&seed) & 0xff :
					bytes[(unsigned)rand_r(&seed) % sizeof(bytes)]);
			}
			target = (n & 1U) != 0 ? "ASCII" : "UTF-8";
			ours(target, "UTF-8", input, length, 64, &a);
			run(host_open, host_conv, host_close, target, "UTF-8",
			    input, length, 64, &b);
			/*
			 * This library refuses a sequence as soon as its first
			 * two bytes show it can never be valid (an overlong
			 * form, a surrogate, a code point above U+10FFFF).  The
			 * host decoder takes code points above U+10FFFF and
			 * waits for more input on the others.  Such a
			 * difference is counted, not failed.
			 */
			if (a.error == EILSEQ && a.consumed < length &&
			    b.consumed >= a.consumed &&
			    never_valid((const unsigned char *)input + a.consumed,
					length - a.consumed)) {
				lenient++;
				continue;
			}
			if (a.value != b.value || a.error != b.error ||
			    a.consumed != b.consumed || a.produced != b.produced ||
			    memcmp(a.output, b.output, a.produced) != 0) {
				fprintf(stderr, "DIFF input:");
				for (i = 0; i < length; i++)
					fprintf(stderr, " %02x", (unsigned char)input[i]);
				fprintf(stderr, " ours=%zd/%d/%zu host=%zd/%d/%zu\n",
					(ssize_t)a.value, a.error, a.consumed,
					(ssize_t)b.value, b.error, b.consumed);
				return 1;
			}
			compared++;
		}
	}
	printf("iconv-test: PASS (host comparisons %zu, host more lenient %zu)\n",
	       compared, lenient);
	return 0;
}
