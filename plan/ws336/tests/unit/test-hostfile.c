/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-hostfile.c.  The patterns are those of ssh_config(5)
 * PATTERNS ('*' matches any run of characters, '?' one character, '!'
 * negates, a negated match wins).  The files are built here from the
 * layouts of sshd(8) AUTHORIZED_KEYS FILE FORMAT and SSH_KNOWN_HOSTS FILE
 * FORMAT: "[options] type base64 [comment]" and "[marker] hosts type
 * base64 [comment]", with the host "[host]:port" for a port other than
 * 22.  The keys are the public keys of RFC 8032 section 7.1 (TEST 1 and
 * TEST 2) and a third made of counting bytes; the public key blob is
 * built with wire.h (RFC 8709 section 4) and encoded by a base64 encoder
 * written apart from the module (RFC 4648 section 4).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "wire.h"
#include "ssh-hostfile.h"

/* The room of the text buffers of this test, in bytes: the largest file and a little more. */
#define TEST_TEXT_MAX		(SSH_HOSTFILE_MAX + 64U)

/* The number of random files tried in the fuzz test. */
#define TEST_FUZZ_COUNT		20000UL

/* The length of the long string of the time test, in characters. */
#define TEST_LONG_SIZE		100000U

/* The test public keys: RFC 8032 section 7.1, TEST 1 and TEST 2; the third is 0, 1, 2, ... 31. */
static const char t1_public_hex[] =
	"d75a980182b10ab7d54bfed3c964073a 0ee172f3daa62325af021a68f707511a";
static const char t2_public_hex[] =
	"3d4017c3e843895a92b70aa74d1b7ebc 9c982ccf2ec4968cc0cd55f12af4660c";

/* The key bytes read from the hexadecimal text above. */
static uint8_t key1[SSH_ED25519_PUBLIC];
static uint8_t key2[SSH_ED25519_PUBLIC];
static uint8_t key3[SSH_ED25519_PUBLIC];

/* The file text being built (a file is at most SSH_HOSTFILE_MAX bytes) and its size; one thread. */
static char file_text[TEST_TEXT_MAX];
static size_t file_size;

/* The hashed host name field of the test: a "|1|" salt and hash as OpenSSH writes them (the module does not read them). */
static const char hashed_hosts[] = "|1|c2FsdHNhbHRzYWx0c2FsdHNhbHQ=|aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGE=";

/* One row of the pattern table: pattern, string, fold, expected answer. */
struct pattern_case {
	const char *pattern;
	const char *string;
	int fold;
	int expected;
};

/* One row of the list table: list, string, fold, expected answer. */
struct list_case {
	const char *list;
	const char *string;
	int fold;
	int expected;
};

static void load_keys(void);
static void test_base64(const uint8_t *in, size_t size, char *out);
static void text_reset(void);
static void text_add(const char *text);
static void text_add_key(const uint8_t *key);
static void text_add_bytes(const char *bytes, size_t size);
static void expect_known(int line, const char *host, unsigned port, const uint8_t *key, int expected, unsigned expected_skipped);
static void expect_find(int line, const uint8_t *key, int expected, unsigned expected_skipped);
static void test_patterns(void);
static void test_pattern_speed(void);
static void test_lists(void);
static void test_authorized(void);
static void test_authorized_edges(void);
static void test_known_basic(void);
static void test_known_ports(void);
static void test_known_priority(void);
static void test_known_lines(void);
static void test_known_host_errors(void);
static void test_put_line(void);
static void test_remove(void);
static void test_fuzz(void);
static int buf_contains(const struct ssh_buf *buf, const char *needle);

#define EXPECT_KNOWN(host, port, key, expected, skipped) expect_known(__LINE__, host, port, key, expected, skipped)
#define EXPECT_FIND(key, expected, skipped) expect_find(__LINE__, key, expected, skipped)

/* Runs every check of the hostfile module and prints the last line. */
int
main(void)
{
	/* Reads the test keys. */
	load_keys();

	/* Runs each group of checks. */
	test_patterns();
	test_pattern_speed();
	test_lists();
	test_authorized();
	test_authorized_edges();
	test_known_basic();
	test_known_ports();
	test_known_priority();
	test_known_lines();
	test_known_host_errors();
	test_put_line();
	test_remove();
	test_fuzz();

	/* Prints the verdict. */
	return check_finish("hostfile");
}

/* Reads the hexadecimal key texts into bytes and makes the third key. */
static void
load_keys(void)
{
	size_t count;
	unsigned index;

	/* TEST 1 and TEST 2. */
	count = wire_hex(t1_public_hex, key1, sizeof(key1));
	CHECK_EQ(count, SSH_ED25519_PUBLIC);
	count = wire_hex(t2_public_hex, key2, sizeof(key2));
	CHECK_EQ(count, SSH_ED25519_PUBLIC);

	/* The third key: 0, 1, 2, ... 31. */
	for (index = 0; index < SSH_ED25519_PUBLIC; index++)
		key3[index] = (uint8_t)index;
}

/*
 * Encodes size bytes as base64 with '=' padding (RFC 4648 4, Table 1)
 * into out with a NUL, written apart from the module.
 */
static void
test_base64(
	const uint8_t *in,
	size_t size,
	char *out)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t position;
	size_t length;
	uint32_t group;
	size_t rest;

	/* Starts at the first input byte with no output. */
	position = 0;
	length = 0;

	/* Takes whole groups of three bytes into four characters. */
	while (size - position >= 3U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8) | in[position + 2U];
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		out[length++] = alphabet[group & 63U];
		position += 3U;
	}

	/* Takes the last one or two bytes, with padding. */
	rest = size - position;
	if (rest == 1U) {
		group = (uint32_t)in[position] << 16;
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = '=';
		out[length++] = '=';
	} else if (rest == 2U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8);
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		out[length++] = '=';
	}

	/* Ends the text. */
	out[length] = '\0';
}

/* Empties the file text. */
static void
text_reset(void)
{
	/* Starts with nothing and a terminating NUL. */
	file_size = 0;
	file_text[0] = '\0';
}

/* Appends size bytes to the file text. */
static void
text_add_bytes(
	const char *bytes,
	size_t size)
{
	/* A test that overflows its buffer is a bug of the test. */
	CHECK(size <= TEST_TEXT_MAX - 1U - file_size);
	if (size > TEST_TEXT_MAX - 1U - file_size)
		return;

	/* Copies the bytes and keeps a NUL after them. */
	memcpy(file_text + file_size, bytes, size);
	file_size += size;
	file_text[file_size] = '\0';
}

/* Appends a NUL-terminated text to the file text. */
static void
text_add(
	const char *text)
{
	/* Appends the text without its NUL. */
	text_add_bytes(text, strlen(text));
}

/* Appends "ssh-ed25519 <base64 of the public key blob>" for key to the file text. */
static void
text_add_key(
	const uint8_t *key)
{
	static struct wire blob;
	char encoded[128];

	/* Builds the blob of RFC 8709 4: string "ssh-ed25519", string of the 32-byte key. */
	wire_reset(&blob);
	wire_text(&blob, "ssh-ed25519");
	wire_string(&blob, key, SSH_ED25519_PUBLIC);
	CHECK_EQ(blob.size, SSH_KEY_BLOB_SIZE);

	/* Encodes it. */
	test_base64(blob.data, blob.size, encoded);

	/* Appends the type, a space and the text. */
	text_add("ssh-ed25519 ");
	text_add(encoded);
}

/* Checks the answer of ssh_known_hosts_check on the file text built so far. */
static void
expect_known(
	int line,
	const char *host,
	unsigned port,
	const uint8_t *key,
	int expected,
	unsigned expected_skipped)
{
	enum ssh_known_result result;
	unsigned skipped;
	int error;

	/* Starts with values that the call must overwrite. */
	result = SSH_KNOWN_MATCH;
	skipped = 12345U;

	/* Asks. */
	error = ssh_known_hosts_check(file_text, file_size, host, (uint16_t)port, key, &result, &skipped);
	check_record_long(error, 0, __FILE__, line, "known_hosts_check returns 0");
	check_record_long((long)result, expected, __FILE__, line, "known_hosts_check result");
	check_record_long((long)skipped, (long)expected_skipped, __FILE__, line, "known_hosts_check skipped");
}

/* Checks the answer of ssh_authorized_keys_find on the file text built so far. */
static void
expect_find(
	int line,
	const uint8_t *key,
	int expected,
	unsigned expected_skipped)
{
	unsigned skipped;
	int found;
	int error;

	/* Starts with values that the call must overwrite. */
	found = 7;
	skipped = 12345U;

	/* Asks. */
	error = ssh_authorized_keys_find(file_text, file_size, key, &found, &skipped);
	check_record_long(error, 0, __FILE__, line, "authorized_keys_find returns 0");
	check_record_long(found, expected, __FILE__, line, "authorized_keys_find found");
	check_record_long((long)skipped, (long)expected_skipped, __FILE__, line, "authorized_keys_find skipped");
}

/* Checks ssh_pattern_match on a table of cases worked out by hand from ssh_config(5) PATTERNS. */
static void
test_patterns(void)
{
	static const struct pattern_case cases[] = {
		/* Empty pattern and empty string. */
		{ "", "", 0, 1 },
		{ "", "a", 0, 0 },
		{ "*", "", 0, 1 },
		{ "**", "", 0, 1 },
		{ "?", "", 0, 0 },
		{ "a", "", 0, 0 },

		/* Stars. */
		{ "*", "anything at all", 0, 1 },
		{ "*", "a", 0, 1 },
		{ "a*", "a", 0, 1 },
		{ "a*", "abc", 0, 1 },
		{ "a*", "ba", 0, 0 },
		{ "*a", "ba", 0, 1 },
		{ "*a", "ab", 0, 0 },
		{ "*.example.com", "www.example.com", 0, 1 },
		{ "*.example.com", ".example.com", 0, 1 },
		{ "*.example.com", "example.com", 0, 0 },
		{ "*.example.com", "www.example.org", 0, 0 },
		{ "*.example.com", "www.example.comx", 0, 0 },
		{ "a*b*c", "abc", 0, 1 },
		{ "a*b*c", "axxbyyc", 0, 1 },
		{ "a*b*c", "axxbyy", 0, 0 },
		{ "a*b*c", "acb", 0, 0 },
		{ "a*b*c", "abcb", 0, 0 },
		{ "a*b*c", "abcbc", 0, 1 },
		{ "*ab*ab", "xabab", 0, 1 },
		{ "*ab*ab", "xab", 0, 0 },
		{ "*ab*ab", "abxab", 0, 1 },
		{ "*aab", "aaab", 0, 1 },
		{ "*aab", "aaaab", 0, 1 },
		{ "*aab", "aa", 0, 0 },
		{ "**a**", "a", 0, 1 },

		/* Question marks. */
		{ "?", "a", 0, 1 },
		{ "?", "ab", 0, 0 },
		{ "??", "ab", 0, 1 },
		{ "a?c", "abc", 0, 1 },
		{ "a?c", "ac", 0, 0 },
		{ "a?c", "abbc", 0, 0 },
		{ "192.168.0.?", "192.168.0.7", 0, 1 },
		{ "192.168.0.?", "192.168.0.17", 0, 0 },
		{ "192.168.0.?", "192.168.0.", 0, 0 },
		{ "*?", "a", 0, 1 },
		{ "*?", "", 0, 0 },
		{ "?*?", "ab", 0, 1 },
		{ "?*?", "a", 0, 0 },

		/* Plain bytes, with and without folding. */
		{ "host", "host", 0, 1 },
		{ "host", "hosts", 0, 0 },
		{ "host", "hos", 0, 0 },
		{ "Host", "host", 0, 0 },
		{ "Host", "host", 1, 1 },
		{ "HOST", "host", 1, 1 },
		{ "host", "HOST", 1, 1 },
		{ "*.Example.COM", "www.example.com", 1, 1 },
		{ "*.Example.COM", "www.example.com", 0, 0 },
		{ "a?c", "AbC", 1, 1 },
		{ "[a]", "[A]", 1, 1 },
		{ "[a]", "[b]", 1, 0 },

		/* Folding changes only ASCII letters: '@' (0x40) and '`' (0x60) differ by 0x20 but are not letters. */
		{ "@", "`", 1, 0 },
		{ "`", "@", 1, 0 },
		{ "[", "{", 1, 0 },
		{ "{", "[", 1, 0 },
		{ "Z", "z", 1, 1 },
		{ "z", "Z", 1, 1 },
		{ "A", "a", 1, 1 },
		{ "a", "A", 1, 1 },
		{ "\x80", "\x80", 1, 1 },
		{ "\xc1", "\xe1", 1, 0 },

		/* The stars and question marks are not special in the string. */
		{ "a", "*", 0, 0 },
		{ "*", "*", 0, 1 },
		{ "a*", "a*", 0, 1 },
		{ "a?", "a?", 0, 1 },
		{ "ab", "a?", 0, 0 },

		/* A '!' in a single pattern is an ordinary byte (the list function reads it as a negation). */
		{ "!a", "!a", 0, 1 },
		{ "!a", "a", 0, 0 }
	};
	size_t index;
	int answer;

	/* Runs each row. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		answer = ssh_pattern_match(cases[index].pattern, strlen(cases[index].pattern), cases[index].string, cases[index].fold);
		check_record_long(answer, cases[index].expected, __FILE__, __LINE__, cases[index].pattern);
		if (answer != cases[index].expected)
			printf("  pattern \"%s\" string \"%s\" fold %d\n", cases[index].pattern, cases[index].string, cases[index].fold);
	}

	/* Only the size bytes of the pattern are read. */
	CHECK_EQ(ssh_pattern_match("abc", 2, "ab", 0), 1);
	CHECK_EQ(ssh_pattern_match("abc", 2, "abc", 0), 0);
	CHECK_EQ(ssh_pattern_match("a*c", 2, "abc", 0), 1);
	CHECK_EQ(ssh_pattern_match("a*c", 1, "abc", 0), 0);
	CHECK_EQ(ssh_pattern_match("a?c", 2, "ab", 0), 1);
	CHECK_EQ(ssh_pattern_match("abc", 0, "", 0), 1);
	CHECK_EQ(ssh_pattern_match("abc", 0, "a", 0), 0);

	/* A NUL byte inside the pattern matches no string byte, and the match fails rather than stopping there. */
	CHECK_EQ(ssh_pattern_match("a\0b", 3, "a", 0), 0);
	CHECK_EQ(ssh_pattern_match("a\0b", 3, "ab", 0), 0);
	CHECK_EQ(ssh_pattern_match("a*\0", 3, "abc", 0), 0);

	/* Missing arguments match nothing. */
	CHECK_EQ(ssh_pattern_match(NULL, 0, "", 0), 1);
	CHECK_EQ(ssh_pattern_match(NULL, 1, "", 0), 0);
	CHECK_EQ(ssh_pattern_match("*", 1, NULL, 0), 0);
}

/* Checks that the time of a match on a long string is near linear: 100000 characters in well under a second. */
static void
test_pattern_speed(void)
{
	static char long_text[TEST_LONG_SIZE + 2U];
	static char pattern[TEST_LONG_SIZE + 2U];
	clock_t begin;
	clock_t end;
	size_t index;
	int answer;

	/* The string is 100000 letters 'a'. */
	for (index = 0; index < TEST_LONG_SIZE; index++)
		long_text[index] = 'a';
	long_text[TEST_LONG_SIZE] = '\0';

	/* Times several patterns whose naive matching would take exponential time. */
	begin = clock();

	/* Four stars with letters that never appear: no match, and the answer comes at once. */
	answer = ssh_pattern_match("a*b*c*d", 7, long_text, 0);
	CHECK_EQ(answer, 0);
	answer = ssh_pattern_match("*a*a*a*a*a*a*a*a*a*a*b", 22, long_text, 0);
	CHECK_EQ(answer, 0);
	answer = ssh_pattern_match("*a*a*a*a*a*a*a*a*a*a", 20, long_text, 0);
	CHECK_EQ(answer, 1);
	answer = ssh_pattern_match("*aaaaaaaaab", 11, long_text, 0);
	CHECK_EQ(answer, 0);
	answer = ssh_pattern_match("a?*?*?*?*?*?*?*?*?b", 19, long_text, 0);
	CHECK_EQ(answer, 0);

	/* A pattern as long as the string, all question marks and stars. */
	for (index = 0; index < TEST_LONG_SIZE; index++) {
		if (index % 2U == 0)
			pattern[index] = '*';
		else
			pattern[index] = '?';
	}

	/* Every character of the string is matched by a star or a question mark. */
	answer = ssh_pattern_match(pattern, TEST_LONG_SIZE, long_text, 0);
	CHECK_EQ(answer, 1);

	/* The same string with a last letter that differs. */
	long_text[TEST_LONG_SIZE - 1U] = 'b';
	answer = ssh_pattern_match("*a*a*a*a*a*a*a*a*a*a", 20, long_text, 0);
	CHECK_EQ(answer, 0);
	answer = ssh_pattern_match("*a*a*a*a*a*a*a*a*a*b", 20, long_text, 0);
	CHECK_EQ(answer, 1);
	end = clock();

	/* The whole group takes under a second of processor time. */
	CHECK((double)(end - begin) / (double)CLOCKS_PER_SEC < 1.0);
}

/* Checks ssh_pattern_list_match: a positive match, a negated match that wins, empty entries. */
static void
test_lists(void)
{
	static const struct list_case cases[] = {
		/* Single entries. */
		{ "a", "a", 0, 1 },
		{ "a", "b", 0, 0 },
		{ "", "a", 0, 0 },
		{ "", "", 0, 0 },
		{ "*", "", 0, 1 },
		{ "*", "x", 0, 1 },

		/* Lists. */
		{ "a,b", "a", 0, 1 },
		{ "a,b", "b", 0, 1 },
		{ "a,b", "c", 0, 0 },
		{ "a,b,", "b", 0, 1 },
		{ ",a", "a", 0, 1 },
		{ ",,a,,", "a", 0, 1 },
		{ ",", "a", 0, 0 },
		{ "*.example.com,*.example.org", "x.example.org", 0, 1 },

		/* Negation wins over a positive match in any order (ssh_config(5) PATTERNS). */
		{ "!bad,*", "bad", 0, -1 },
		{ "*,!bad", "bad", 0, -1 },
		{ "!bad,*", "good", 0, 1 },
		{ "*,!bad", "good", 0, 1 },
		{ "*.example.com,!bad.example.com", "bad.example.com", 0, -1 },
		{ "*.example.com,!bad.example.com", "good.example.com", 0, 1 },
		{ "!*.dialup.example.com,*.example.com", "x.dialup.example.com", 0, -1 },
		{ "!*.dialup.example.com,*.example.com", "x.office.example.com", 0, 1 },

		/* A negated match never produces a positive result by itself (ssh_config(5) PATTERNS, host3 example). */
		{ "!host1,!host2", "host3", 0, 0 },
		{ "!host1,!host2,*", "host3", 0, 1 },
		{ "!host1,!host2,*", "host2", 0, -1 },
		{ "!host1", "host1", 0, -1 },
		{ "!host1", "host2", 0, 0 },

		/* Case folding applies inside lists. */
		{ "!BAD,*", "bad", 1, -1 },
		{ "!BAD,*", "bad", 0, 1 },
		{ "Host", "HOST", 1, 1 },

		/* A lone '!' and an empty negation are passed over; a second '!' is a byte. */
		{ "!", "a", 0, 0 },
		{ "!,a", "a", 0, 1 },
		{ "!!a,*", "!a", 0, -1 },
		{ "!!a,*", "a", 0, 1 },
		{ "a,!", "a", 0, 1 },
		{ "!", "", 0, 0 },
		{ "a,!", "", 0, 0 },
		{ "!,*", "", 0, 1 },

		/* Port-bearing names. */
		{ "[host]:2222", "[host]:2222", 1, 1 },
		{ "[host]:2222,other", "[host]:22", 1, 0 },
		{ "[*]:2222", "[host]:2222", 1, 1 }
	};
	size_t index;
	int answer;

	/* Runs each row. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		answer = ssh_pattern_list_match(cases[index].list, strlen(cases[index].list), cases[index].string, cases[index].fold);
		check_record_long(answer, cases[index].expected, __FILE__, __LINE__, cases[index].list);
		if (answer != cases[index].expected)
			printf("  list \"%s\" string \"%s\" fold %d\n", cases[index].list, cases[index].string, cases[index].fold);
	}

	/* Only the size bytes of the list are read. */
	CHECK_EQ(ssh_pattern_list_match("a,b", 1, "b", 0), 0);
	CHECK_EQ(ssh_pattern_list_match("a,b", 3, "b", 0), 1);
	CHECK_EQ(ssh_pattern_list_match("a,!b", 3, "b", 0), 0);
	CHECK_EQ(ssh_pattern_list_match("a,!b", 4, "b", 0), -1);
	CHECK_EQ(ssh_pattern_list_match("a,bcd", 4, "bc", 0), 1);
	CHECK_EQ(ssh_pattern_list_match("a,bcd", 4, "bcd", 0), 0);

	/* Missing arguments match nothing. */
	CHECK_EQ(ssh_pattern_list_match(NULL, 0, "a", 0), 0);
	CHECK_EQ(ssh_pattern_list_match(NULL, 3, "a", 0), 0);
	CHECK_EQ(ssh_pattern_list_match("a", 1, NULL, 0), 0);
}

/* Checks ssh_authorized_keys_find on files built from the layout of sshd(8) AUTHORIZED_KEYS FILE FORMAT. */
static void
test_authorized(void)
{
	int found;
	unsigned skipped;
	int error;

	/* A file of one key finds that key and not another. */
	text_reset();
	text_add_key(key1);
	text_add(" alice@example\n");
	EXPECT_FIND(key1, 1, 0);
	EXPECT_FIND(key2, 0, 0);
	EXPECT_FIND(key3, 0, 0);

	/* Several keys: each is found, a missing one is not. */
	text_reset();
	text_add_key(key2);
	text_add("\n");
	text_add_key(key1);
	text_add(" second\n");
	EXPECT_FIND(key1, 1, 0);
	EXPECT_FIND(key2, 1, 0);
	EXPECT_FIND(key3, 0, 0);

	/* The same key twice is still found once. */
	text_reset();
	text_add_key(key1);
	text_add("\n");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 1, 0);

	/* Comments and blank lines are ignored and not counted: even one that holds a key. */
	text_reset();
	text_add("# a comment\n\n   \n\t\n  # indented comment\n#");
	text_add_key(key1);
	text_add("\n");
	text_add("   # ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 0, 0);
	text_add_key(key2);
	text_add("\n");
	EXPECT_FIND(key2, 1, 0);
	EXPECT_FIND(key1, 0, 0);

	/* CR LF line ends, with and without a comment, and a last line with no LF. */
	text_reset();
	text_add_key(key1);
	text_add("\r\n");
	text_add_key(key2);
	text_add(" bob\r\n");
	text_add_key(key3);
	EXPECT_FIND(key1, 1, 0);
	EXPECT_FIND(key2, 1, 0);
	EXPECT_FIND(key3, 1, 0);

	/* A last line with a CR and no LF. */
	text_reset();
	text_add_key(key1);
	text_add(" c\r");
	EXPECT_FIND(key1, 1, 0);

	/* Tabs and several spaces separate the fields; leading and trailing blanks are allowed. */
	text_reset();
	text_add("  \t");
	text_add_key(key1);
	text_add("\t  comment text with spaces   \t\n");
	EXPECT_FIND(key1, 1, 0);

	/* The text "ssh-ed25519" in the comment of another line, or in a comment line, finds nothing. */
	text_reset();
	text_add("ssh-rsa AAAAB3NzaC1yc2E ssh-ed25519 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 0, 1);

	/* Lines of a CR alone, or of blanks and a CR, are blank lines: ignored and not counted. */
	text_reset();
	text_add("\r\n \t\r\n\r\n");
	text_add_key(key1);
	text_add("\r\n\r");
	EXPECT_FIND(key1, 1, 0);

	/* A line with options before the type is passed over and counted, with every option form. */
	text_reset();
	text_add("no-pty ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 0, 1);
	text_add("command=\"echo a b\",no-port-forwarding ");
	text_add_key(key1);
	text_add(" c\n");
	EXPECT_FIND(key1, 0, 2);
	text_add("from=\"*.example.com\",restrict\t");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 0, 3);

	/* A key line after the options lines is still found, and the count stays. */
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 1, 3);

	/* Lines of another key type are passed over and counted. */
	text_reset();
	text_add("ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ== a@b\n");
	text_add("ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTY= x\n");
	text_add("sk-ssh-ed25519@openssh.com AAAA y\n");
	EXPECT_FIND(key1, 0, 3);
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 1, 3);

	/* Malformed lines are passed over and counted: a type alone, no blob, bad base64, a short blob, a blob with a changed type. */
	text_reset();
	text_add("ssh-ed25519\n");
	text_add("ssh-ed25519 \n");
	text_add("ssh-ed25519 !!!!\n");
	text_add("ssh-ed25519 AAAA\n");
	text_add("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1E=\n");
	text_add("SSH-ED25519 AAAAC3NzaC1lZDI1NTE5AAAAINdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea\n");
	EXPECT_FIND(key1, 0, 6);

	/* A good key line whose blob names another type is malformed. */
	text_reset();
	text_add("ssh-ed25519 AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	EXPECT_FIND(key1, 0, 1);

	/* A NUL byte in a line passes it over. */
	text_reset();
	text_add_key(key1);
	text_add_bytes(" a\0b\n", 5);
	EXPECT_FIND(key1, 0, 1);

	/* A letter changed inside the key part is another key, not a match for the original. */
	text_reset();
	text_add_key(key1);
	text_add("\n");
	if (file_text[60] == 'A')
		file_text[60] = 'B';
	else
		file_text[60] = 'A';
	EXPECT_FIND(key1, 0, 0);

	/* A letter changed inside the type part of the blob makes the line malformed. */
	text_reset();
	text_add_key(key1);
	text_add("\n");
	if (file_text[20] == 'A')
		file_text[20] = 'B';
	else
		file_text[20] = 'A';
	EXPECT_FIND(key1, 0, 1);

	/* The size is taken from the argument, not from a NUL. */
	text_reset();
	text_add_key(key1);
	text_add("\n");
	text_add_key(key2);
	text_add("\n");
	found = 7;
	skipped = 9;
	error = ssh_authorized_keys_find(file_text, file_size / 2U, key2, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);
	CHECK_EQ(skipped, 0);
	error = ssh_authorized_keys_find(file_text, file_size / 2U + 10U, key2, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);
	CHECK_EQ(skipped, 1);
	error = ssh_authorized_keys_find(file_text, file_size - 1U, key2, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(skipped, 0);

	/* A skipped count is optional. */
	found = 7;
	error = ssh_authorized_keys_find(file_text, file_size, key1, &found, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);

	/* Empty text finds nothing; text may be NULL when empty. */
	found = 7;
	skipped = 9;
	error = ssh_authorized_keys_find("", 0, key1, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);
	CHECK_EQ(skipped, 0);
	found = 7;
	error = ssh_authorized_keys_find(NULL, 0, key1, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);

	/* Missing arguments are EINVAL. */
	found = 7;
	CHECK_EQ(ssh_authorized_keys_find(NULL, 5, key1, &found, &skipped), EINVAL);
	CHECK_EQ(found, 0);
	CHECK_EQ(ssh_authorized_keys_find("", 0, NULL, &found, &skipped), EINVAL);
	CHECK_EQ(ssh_authorized_keys_find("", 0, key1, NULL, &skipped), EINVAL);
}

/* Checks the limits of the authorized_keys reader: line length and file size. */
static void
test_authorized_edges(void)
{
	size_t pad;
	int found;
	unsigned skipped;
	int error;

	/* A key line of exactly SSH_HOSTFILE_LINE_MAX bytes (its comment fills the rest) is read. */
	text_reset();
	text_add_key(key1);
	text_add(" ");
	for (pad = file_size; pad < SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("x");
	CHECK_EQ(file_size, SSH_HOSTFILE_LINE_MAX);
	text_add("\n");
	EXPECT_FIND(key1, 1, 0);

	/* One byte more is too long: passed over and counted, not matched. */
	text_reset();
	text_add_key(key1);
	text_add(" ");
	for (pad = file_size; pad <= SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("x");
	CHECK_EQ(file_size, SSH_HOSTFILE_LINE_MAX + 1U);
	text_add("\n");
	EXPECT_FIND(key1, 0, 1);

	/* A CR before the LF counts in the length: the same line of the limit with a CR is too long. */
	text_reset();
	text_add_key(key1);
	text_add(" ");
	for (pad = file_size; pad < SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("x");
	text_add("\r\n");
	EXPECT_FIND(key1, 0, 1);

	/* The too-long line does not hide the lines after it. */
	text_add_key(key2);
	text_add("\n");
	EXPECT_FIND(key2, 1, 1);

	/* A very long comment line is not counted. */
	text_reset();
	text_add("#");
	for (pad = 0; pad < 3U * SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("y");
	text_add("\n");
	text_add_key(key1);
	text_add("\n");
	EXPECT_FIND(key1, 1, 0);

	/* A file of exactly SSH_HOSTFILE_MAX bytes is read: blank lines, and the key line at the very end with no LF. */
	text_reset();
	text_add_key(key2);
	pad = file_size;
	memmove(file_text + SSH_HOSTFILE_MAX - pad, file_text, pad);
	memset(file_text, '\n', SSH_HOSTFILE_MAX - pad);
	file_size = SSH_HOSTFILE_MAX;
	found = 7;
	skipped = 9;
	error = ssh_authorized_keys_find(file_text, SSH_HOSTFILE_MAX, key2, &found, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(skipped, 0);

	/* One byte more than the limit is EMSGSIZE, with found 0 and skipped 0. */
	found = 7;
	skipped = 9;
	error = ssh_authorized_keys_find(file_text, SSH_HOSTFILE_MAX + 1U, key2, &found, &skipped);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(found, 0);
	CHECK_EQ(skipped, 0);
	text_reset();
}

/* Checks ssh_known_hosts_check with plain names, and the names of a line's first field. */
static void
test_known_basic(void)
{
	/* A file of one line: the host with the key. */
	text_reset();
	text_add("example.com ");
	text_add_key(key1);
	text_add(" a comment\n");
	EXPECT_KNOWN("example.com", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("example.com", 22, key2, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("example.org", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("example.co", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("example.comm", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* The host name compares without regard to case (ASCII). */
	EXPECT_KNOWN("EXAMPLE.COM", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("Example.Com", 22, key2, SSH_KNOWN_CHANGED, 0);

	/* A pattern list, wildcards and a negation in the first field. */
	text_reset();
	text_add("*.example.com,!bad.example.com,10.0.0.? ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("www.example.com", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("bad.example.com", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("10.0.0.5", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("10.0.0.55", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("example.com", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* A negation alone never produces a match (ssh_config(5) PATTERNS). */
	text_reset();
	text_add("!host1,!host2 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host3", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host1", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* Comments, blank lines, indentation, tabs between fields, CR LF, a last line without LF. */
	text_reset();
	text_add("# comment\n\n   \n");
	text_add("  alpha\t");
	text_add_key(key1);
	text_add("\t\tcomment\r\n");
	text_add("beta ");
	text_add_key(key2);
	EXPECT_KNOWN("alpha", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("beta", 22, key2, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("beta", 22, key1, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("gamma", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* Lines of a CR alone are blank lines: ignored and not counted. */
	text_reset();
	text_add("\r\n \t\r\n\r");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* A commented-out line names nothing. */
	text_reset();
	text_add("# example.com ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("example.com", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* An IPv6 address is a name like another (its colons are not special). */
	text_reset();
	text_add("2001:db8::1 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("2001:db8::1", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("2001:db8::2", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* Empty text names no host. */
	text_reset();
	EXPECT_KNOWN("example.com", 22, key1, SSH_KNOWN_ABSENT, 0);
}

/* Checks the name looked up for each port (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT: "[host]:port" for a non-standard port). */
static void
test_known_ports(void)
{
	/* A plain line is the host on port 22 only. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 2222, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 0, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 65535, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 23, key1, SSH_KNOWN_ABSENT, 0);

	/* A bracketed line is that host and port only. */
	text_reset();
	text_add("[host]:2222 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 2222, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 2222, key2, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("host", 22222, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 222, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("other", 2222, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("HOST", 2222, key1, SSH_KNOWN_MATCH, 0);

	/* A line "[host]:22" is not the name of port 22: the brackets are for a non-standard port, so it matches nothing. */
	text_reset();
	text_add("[host]:22 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 2222, key1, SSH_KNOWN_ABSENT, 0);

	/* The extreme ports have their own names. */
	text_reset();
	text_add("[host]:0 ");
	text_add_key(key1);
	text_add("\n[host]:65535 ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 0, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 65535, key2, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 65535, key1, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("host", 6553, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 655, key1, SSH_KNOWN_ABSENT, 0);

	/* Wildcards and lists work on the bracketed name. */
	text_reset();
	text_add("[*.example.com]:2222,[10.0.0.1]:2222 ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("a.example.com", 2222, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("10.0.0.1", 2222, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("a.example.com", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("a.example.com", 2223, key1, SSH_KNOWN_ABSENT, 0);

	/* A plain line and a bracketed line for one host: each answers for its own port. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n[host]:2222 ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("host", 2222, key2, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 2222, key1, SSH_KNOWN_CHANGED, 0);
}

/* Checks the order of the answers: revoked, match, changed, other types, absent. */
static void
test_known_priority(void)
{
	uint8_t last_differs[SSH_ED25519_PUBLIC];

	/* A matching key wins over a changed key, in either order. */
	text_reset();
	text_add("host ");
	text_add_key(key2);
	text_add("\nhost ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 22, key3, SSH_KNOWN_CHANGED, 0);
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\nhost ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);

	/* Changed wins over other types, in either order. */
	text_reset();
	text_add("host ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\nhost ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_CHANGED, 0);
	text_reset();
	text_add("host ");
	text_add_key(key2);
	text_add("\nhost ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_CHANGED, 0);

	/* A line of another type alone says the host is known with another type; it is not counted as passed over. */
	text_reset();
	text_add("host ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	text_add("host ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTY= c\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_OTHER_TYPES, 0);
	EXPECT_KNOWN("other", 22, key1, SSH_KNOWN_ABSENT, 0);
	EXPECT_KNOWN("host", 2222, key1, SSH_KNOWN_ABSENT, 0);

	/* A match with a line of another type: the match. */
	text_add("host ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);

	/* A revoked key is refused for every host, whatever else says. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n@revoked unrelated.example ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_REVOKED, 0);
	EXPECT_KNOWN("never.seen", 22, key1, SSH_KNOWN_REVOKED, 0);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_CHANGED, 0);
	EXPECT_KNOWN("never.seen", 22, key2, SSH_KNOWN_ABSENT, 0);

	/* The revoked line before the matching line gives the same answer. */
	text_reset();
	text_add("@revoked * ");
	text_add_key(key1);
	text_add("\nhost ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_REVOKED, 0);
	EXPECT_KNOWN("host", 22, key3, SSH_KNOWN_CHANGED, 0);

	/* A revoked line of the same host with another key does not revoke this key. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n@revoked host ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_REVOKED, 0);

	/* A revoked line whose key differs from this key in the last byte only does not revoke this key. */
	memcpy(last_differs, key1, sizeof(last_differs));
	last_differs[SSH_ED25519_PUBLIC - 1U] ^= 0x01U;
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n@revoked host ");
	text_add_key(last_differs);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN("host", 22, last_differs, SSH_KNOWN_REVOKED, 0);

	/* A revoked line whose host field is hashed still revokes (the key is refused for every host). */
	text_reset();
	text_add("@revoked ");
	text_add(hashed_hosts);
	text_add(" ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_REVOKED, 0);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_ABSENT, 0);

	/* A revoked line of another key type cannot revoke this type: passed over and counted. */
	text_reset();
	text_add("@revoked host ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	text_add("host ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 1);

	/* A revoked line with a broken key is passed over and counted. */
	text_reset();
	text_add("@revoked host ssh-ed25519 AAAA\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 1);

	/* The marker is exact: "@Revoked" and "@revokedx" are markers these programs do not know. */
	text_reset();
	text_add("@Revoked host ");
	text_add_key(key1);
	text_add("\n@revokedx host ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 2);
}

/* Checks the lines that are passed over and counted: certificates, hashed names, other markers, malformed and long lines. */
static void
test_known_lines(void)
{
	enum ssh_known_result result;
	unsigned skipped;
	size_t pad;
	int error;

	/* A @cert-authority line is passed over and never matches, whatever its host and key. */
	text_reset();
	text_add("@cert-authority host ");
	text_add_key(key1);
	text_add("\n@cert-authority * ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 2);
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_ABSENT, 2);

	/* A hashed line is passed over, and a plain line after it is still read. */
	text_reset();
	text_add(hashed_hosts);
	text_add(" ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 1);
	text_add("host ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN("host", 22, key2, SSH_KNOWN_MATCH, 1);
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_CHANGED, 1);

	/* A '|' elsewhere than at the start of the field is an ordinary byte. */
	text_reset();
	text_add("ho|st ");
	text_add_key(key1);
	text_add("\n");
	EXPECT_KNOWN("ho|st", 22, key1, SSH_KNOWN_MATCH, 0);

	/* Malformed lines: no key field, no type, bad base64, a short key, a marker alone, a type alone. */
	text_reset();
	text_add("host\n");
	text_add("host ssh-ed25519\n");
	text_add("host ssh-ed25519 !!!\n");
	text_add("host ssh-ed25519 AAAA\n");
	text_add("@revoked\n");
	text_add("@revoked host\n");
	text_add("@cert-authority\n");
	text_add("@x host\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 8);

	/* A good line of this type with broken base64 does not make the host known. */
	text_reset();
	text_add("host ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1E=\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 1);

	/* A line of a host other than the one asked, with another type, is not counted. */
	text_reset();
	text_add("elsewhere ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 0);

	/* A NUL byte in a line passes it over. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add_bytes("\0 x\n", 4);
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 1);

	/* A line of exactly the limit is read; one byte more is passed over. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add(" ");
	for (pad = file_size; pad < SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("x");
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_MATCH, 0);
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add(" ");
	for (pad = file_size; pad <= SSH_HOSTFILE_LINE_MAX; pad++)
		text_add("x");
	text_add("\n");
	EXPECT_KNOWN("host", 22, key1, SSH_KNOWN_ABSENT, 1);

	/* Size is taken from the argument: a text cut in the middle of a line reads only the part. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\nother ");
	text_add_key(key2);
	text_add("\n");
	result = SSH_KNOWN_MATCH;
	skipped = 5;
	error = ssh_known_hosts_check(file_text, 5U + 11U + 1U + 68U + 1U, "other", 22, key2, &result, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);
	CHECK_EQ(skipped, 0);
	error = ssh_known_hosts_check(NULL, 0, "other", 22, key2, &result, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);
}

/* Checks the arguments of the known_hosts functions: host name errors, sizes, missing arguments. */
static void
test_known_host_errors(void)
{
	static char long_host[300];
	static const char *const bad_hosts[] = {
		"",
		"a b",
		" ",
		"a\tb",
		"a\nb",
		"a\rb",
		"a\x01",
		"\x1f",
		"a\x7f",
		"\x7f",
		"\x80",
		"a\xc3\xa9",
		"\xff"
	};
	enum ssh_known_result result;
	unsigned skipped;
	size_t index;
	int error;

	/* Builds a file with one line for the host. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n");

	/* A host name that is empty, holds a blank, a control byte or a byte above 0x7e is EINVAL. */
	for (index = 0; index < sizeof(bad_hosts) / sizeof(bad_hosts[0]); index++) {
		result = SSH_KNOWN_MATCH;
		skipped = 9;
		error = ssh_known_hosts_check(file_text, file_size, bad_hosts[index], 22, key1, &result, &skipped);
		check_record_long(error, EINVAL, __FILE__, __LINE__, "bad host name is EINVAL");
		check_record_long(result, SSH_KNOWN_ABSENT, __FILE__, __LINE__, "bad host name leaves ABSENT");
		check_record_long((long)skipped, 0, __FILE__, __LINE__, "bad host name leaves skipped 0");
	}

	/* 255 characters is the longest; 256 is EINVAL. */
	memset(long_host, 'a', 255U);
	long_host[255] = '\0';
	error = ssh_known_hosts_check(file_text, file_size, long_host, 22, key1, &result, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);
	error = ssh_known_hosts_check(file_text, file_size, long_host, 2222, key1, &result, &skipped);
	CHECK_EQ(error, 0);
	long_host[255] = 'a';
	long_host[256] = '\0';
	error = ssh_known_hosts_check(file_text, file_size, long_host, 22, key1, &result, &skipped);
	CHECK_EQ(error, EINVAL);
	memset(long_host, 'a', 299U);
	long_host[299] = '\0';
	error = ssh_known_hosts_check(file_text, file_size, long_host, 22, key1, &result, &skipped);
	CHECK_EQ(error, EINVAL);

	/* A 255-character host finds its own line (with the port, the name is longer than the host). */
	text_reset();
	memset(long_host, 'b', 255U);
	long_host[255] = '\0';
	text_add(long_host);
	text_add(" ");
	text_add_key(key1);
	text_add("\n[");
	text_add(long_host);
	text_add("]:65535 ");
	text_add_key(key2);
	text_add("\n");
	EXPECT_KNOWN(long_host, 22, key1, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN(long_host, 65535, key2, SSH_KNOWN_MATCH, 0);
	EXPECT_KNOWN(long_host, 65535, key1, SSH_KNOWN_CHANGED, 0);

	/* A skipped count is optional. */
	error = ssh_known_hosts_check(file_text, file_size, long_host, 22, key1, &result, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(result, SSH_KNOWN_MATCH);

	/* Missing arguments are EINVAL; text may be NULL when empty only. */
	CHECK_EQ(ssh_known_hosts_check(NULL, 4, "h", 22, key1, &result, &skipped), EINVAL);
	CHECK_EQ(ssh_known_hosts_check("", 0, NULL, 22, key1, &result, &skipped), EINVAL);
	CHECK_EQ(ssh_known_hosts_check("", 0, "h", 22, NULL, &result, &skipped), EINVAL);
	CHECK_EQ(ssh_known_hosts_check("", 0, "h", 22, key1, NULL, &skipped), EINVAL);

	/* A file larger than the limit is EMSGSIZE. */
	CHECK_EQ(ssh_known_hosts_check(file_text, SSH_HOSTFILE_MAX + 1U, "h", 22, key1, &result, &skipped), EMSGSIZE);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);
}

/* Checks ssh_known_hosts_put_line: the exact bytes, the round trip, the refusals and the all-or-nothing append. */
static void
test_put_line(void)
{
	static struct wire blob;
	char encoded[128];
	char expected[512];
	struct ssh_buf out;
	enum ssh_known_result result;
	unsigned skipped;
	size_t before;
	size_t index;
	int error;
	static const char *const bad_hosts[] = {
		"", "a b", "a,b", "a*", "*", "a?", "?", "!a", "a!", "[a]", "a[", "a]", "|1|x", "@revoked", "#a", "a\tb", "a\x7f", "\x80", "a\nb"
	};

	/* The expected line is built apart from the module: the host, a space, the type, the base64 of the blob, LF. */
	wire_reset(&blob);
	wire_text(&blob, "ssh-ed25519");
	wire_string(&blob, key1, SSH_ED25519_PUBLIC);
	test_base64(blob.data, blob.size, encoded);

	/* Port 22: the host alone. */
	ssh_buf_init(&out, 1024U, 0);
	error = ssh_known_hosts_put_line("example.com", 22, key1, &out);
	CHECK_EQ(error, 0);
	strcpy(expected, "example.com ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n");
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);

	/* Another port: the host in brackets and the port in decimal. */
	ssh_buf_init(&out, 1024U, 0);
	error = ssh_known_hosts_put_line("example.com", 2222, key1, &out);
	CHECK_EQ(error, 0);
	strcpy(expected, "[example.com]:2222 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n");
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);

	/* The smallest and the largest ports, and a one-digit and two-digit port. */
	ssh_buf_init(&out, 1024U, 0);
	CHECK_EQ(ssh_known_hosts_put_line("h", 0, key1, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("h", 65535, key1, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("h", 7, key1, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("h", 10, key1, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("h", 100, key1, &out), 0);
	strcpy(expected, "[h]:0 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n[h]:65535 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n[h]:7 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n[h]:10 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n[h]:100 ssh-ed25519 ");
	strcat(expected, encoded);
	strcat(expected, "\n");
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);

	/* The line written is read back: every (host, port) in a file is found with its own key. */
	ssh_buf_init(&out, 65536U, 0);
	CHECK_EQ(ssh_known_hosts_put_line("a.example", 22, key1, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("a.example", 2222, key2, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("Bee.Example", 22, key3, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("2001:db8::7", 22, key2, &out), 0);
	CHECK_EQ(ssh_known_hosts_put_line("2001:db8::7", 830, key1, &out), 0);
	CHECK_EQ(ssh_buf_error(&out), 0);
	error = ssh_known_hosts_check((const char *)out.data, out.size, "a.example", 22, key1, &result, &skipped);
	CHECK_EQ(error, 0);
	CHECK_EQ(result, SSH_KNOWN_MATCH);
	CHECK_EQ(skipped, 0);
	ssh_known_hosts_check((const char *)out.data, out.size, "a.example", 22, key2, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_CHANGED);
	ssh_known_hosts_check((const char *)out.data, out.size, "a.example", 2222, key2, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_MATCH);
	ssh_known_hosts_check((const char *)out.data, out.size, "bee.example", 22, key3, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_MATCH);
	ssh_known_hosts_check((const char *)out.data, out.size, "2001:db8::7", 22, key2, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_MATCH);
	ssh_known_hosts_check((const char *)out.data, out.size, "2001:db8::7", 830, key1, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_MATCH);
	ssh_known_hosts_check((const char *)out.data, out.size, "2001:db8::7", 831, key1, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);
	ssh_known_hosts_check((const char *)out.data, out.size, "c.example", 22, key1, &result, &skipped);
	CHECK_EQ(result, SSH_KNOWN_ABSENT);

	/* The lines written are authorized_keys lines too (the host part aside): the key part reads back. */
	ssh_buf_free(&out);

	/* A host that would mean more than one name, or break the line, is EINVAL and appends nothing. */
	ssh_buf_init(&out, 1024U, 0);
	for (index = 0; index < sizeof(bad_hosts) / sizeof(bad_hosts[0]); index++) {
		error = ssh_known_hosts_put_line(bad_hosts[index], 22, key1, &out);
		check_record_long(error, EINVAL, __FILE__, __LINE__, "put_line refuses the host");
		check_record_long((long)out.size, 0, __FILE__, __LINE__, "put_line appended nothing");
		error = ssh_known_hosts_put_line(bad_hosts[index], 2222, key1, &out);
		check_record_long(error, EINVAL, __FILE__, __LINE__, "put_line refuses the host with a port");
		check_record_long((long)out.size, 0, __FILE__, __LINE__, "put_line appended nothing with a port");
	}

	/* No append may have left an error behind. */
	CHECK_EQ(ssh_buf_error(&out), 0);

	/* A host of 256 characters is EINVAL and 255 is written. */
	memset(expected, 'h', 255U);
	expected[255] = '\0';
	error = ssh_known_hosts_put_line(expected, 22, key1, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 255U + 1U + 11U + 1U + 68U + 1U);
	ssh_buf_reset(&out);
	expected[255] = 'h';
	expected[256] = '\0';
	error = ssh_known_hosts_put_line(expected, 22, key1, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.size, 0);

	/* A buffer too small for the whole line gets nothing: the error is EMSGSIZE and the size does not change. */
	ssh_buf_free(&out);
	for (before = 0; before < 100U; before += 7U) {
		ssh_buf_init(&out, before, 0);
		error = ssh_known_hosts_put_line("example.com", 22, key1, &out);
		check_record_long(error, EMSGSIZE, __FILE__, __LINE__, "put_line into a small buffer");
		check_record_long((long)out.size, 0, __FILE__, __LINE__, "small buffer stays empty");
		ssh_buf_free(&out);
	}

	/* Existing content stays in front of the new line. */
	ssh_buf_init(&out, 1024U, 0);
	ssh_buf_put_bytes(&out, "# head\n", 7U);
	CHECK_EQ(ssh_known_hosts_put_line("h", 22, key1, &out), 0);
	CHECK_BYTES(out.data, "# head\nh ssh-ed25519 ", 21);
	ssh_buf_free(&out);

	/* Missing arguments are EINVAL. */
	ssh_buf_init(&out, 1024U, 0);
	CHECK_EQ(ssh_known_hosts_put_line(NULL, 22, key1, &out), EINVAL);
	CHECK_EQ(ssh_known_hosts_put_line("h", 22, NULL, &out), EINVAL);
	CHECK_EQ(ssh_known_hosts_put_line("h", 22, key1, NULL), EINVAL);
	ssh_buf_free(&out);

	/* An error already recorded in out is returned and nothing is appended. */
	ssh_buf_init(&out, 1024U, 0);
	out.error = ENOMEM;
	CHECK_EQ(ssh_known_hosts_put_line("h", 22, key1, &out), ENOMEM);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);
}

/* Tells whether the bytes of buf (which need not end with a NUL) hold the text needle. */
static int
buf_contains(
	const struct ssh_buf *buf,
	const char *needle)
{
	size_t length;
	size_t position;
	int different;

	/* Takes the needle's length once. */
	length = strlen(needle);

	/* Compares at each position where the needle fits. */
	for (position = 0; position + length <= buf->size; position++) {
		different = memcmp(buf->data + position, needle, length);
		if (different == 0)
			return 1;
	}

	/* Not found. */
	return 0;
}

/* Checks ssh_known_hosts_remove: the lines left out, the bytes kept, the refusals. */
static void
test_remove(void)
{
	struct ssh_buf out;
	struct ssh_buf again;
	unsigned removed;
	size_t index;
	int error;
	static const char *const bad_hosts[] = { "", "a b", "a\x01", "\x80" };

	/* A file of three hosts loses the lines of one; the others stay byte for byte, comments and CR LF included. */
	text_reset();
	text_add("# header\r\n\n");
	text_add("keep1 ");
	text_add_key(key1);
	text_add(" c\r\n");
	text_add("gone ");
	text_add_key(key2);
	text_add("\n");
	text_add("  keep2\t");
	text_add_key(key2);
	text_add("   \n");
	text_add("a,gone,b ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	text_add("#gone ");
	text_add_key(key1);
	text_add("\n");
	text_add("tail ");
	text_add_key(key3);
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	removed = 99;
	error = ssh_known_hosts_remove(file_text, file_size, "gone", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 2);
	text_reset();
	text_add("# header\r\n\n");
	text_add("keep1 ");
	text_add_key(key1);
	text_add(" c\r\n");
	text_add("  keep2\t");
	text_add_key(key2);
	text_add("   \n");
	text_add("#gone ");
	text_add_key(key1);
	text_add("\n");
	text_add("tail ");
	text_add_key(key3);
	CHECK_EQ(out.size, file_size);
	CHECK_BYTES(out.data, file_text, file_size);
	ssh_buf_free(&out);

	/* Nothing to remove: the output equals the input and removed is 0. */
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	removed = 99;
	error = ssh_known_hosts_remove(file_text, file_size, "absent", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(out.size, file_size);
	CHECK_BYTES(out.data, file_text, file_size);
	ssh_buf_free(&out);

	/* The last line without an LF can be removed; the earlier lines keep their LF. */
	text_reset();
	text_add("one ");
	text_add_key(key1);
	text_add("\ntwo ");
	text_add_key(key2);
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "two", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 1);
	CHECK_EQ(out.size, 4U + 11U + 1U + 68U + 1U);
	CHECK_BYTES(out.data, file_text, out.size);
	ssh_buf_free(&out);

	/* Only the port asked for is removed: port 22 names the plain host, others the bracketed form. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add("\n[host]:2222 ");
	text_add_key(key2);
	text_add("\n[host]:22 ");
	text_add_key(key3);
	text_add("\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "HOST", 2222, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 1);
	CHECK(memcmp(out.data, "host ", 5) == 0);
	CHECK(!buf_contains(&out, "[host]:2222"));
	CHECK(buf_contains(&out, "[host]:22 "));
	ssh_buf_free(&out);
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "host", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 1);
	CHECK(memcmp(out.data, "[host]:2222 ", 12) == 0);
	ssh_buf_free(&out);

	/* Wildcard and list lines that name the host are removed whole, a negated host keeps its line. */
	text_reset();
	text_add("*.example.com ");
	text_add_key(key1);
	text_add("\n*.example.com,!x.example.com ");
	text_add_key(key2);
	text_add("\nother ");
	text_add_key(key3);
	text_add("\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "x.example.com", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 1);
	CHECK(memcmp(out.data, "*.example.com,!x.example.com ", 29) == 0);
	ssh_buf_free(&out);

	/* A revoked line that names the host is removed; hashed, cert-authority, unknown markers and malformed lines are kept. */
	text_reset();
	text_add("@revoked host ");
	text_add_key(key1);
	text_add("\n");
	text_add(hashed_hosts);
	text_add(" ");
	text_add_key(key1);
	text_add("\n@cert-authority host ");
	text_add_key(key1);
	text_add("\n@other host ");
	text_add_key(key1);
	text_add("\nhost\nhost ssh-ed25519\n");
	text_add("host ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQ==\n");
	text_add("host ssh-ed25519 !!!!\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "host", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 3);
	CHECK(!buf_contains(&out, "@revoked"));
	CHECK(buf_contains(&out, "|1|"));
	CHECK(buf_contains(&out, "@cert-authority host "));
	CHECK(buf_contains(&out, "@other host "));
	CHECK(buf_contains(&out, "\nhost\nhost ssh-ed25519\n"));
	CHECK(!buf_contains(&out, "ssh-rsa"));
	CHECK(!buf_contains(&out, "!!!!"));
	ssh_buf_free(&out);

	/* A hashed line is kept even when the name asked for is the text of its hashed field. */
	text_reset();
	text_add(hashed_hosts);
	text_add(" ");
	text_add_key(key1);
	text_add("\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, hashed_hosts, 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(out.size, file_size);
	ssh_buf_free(&out);

	/* A line over the limit is kept even when its first field names the host. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add(" ");
	while (file_size <= SSH_HOSTFILE_LINE_MAX)
		text_add("x");
	text_add("\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "host", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(out.size, file_size);
	ssh_buf_free(&out);

	/* A NUL in a line keeps the line. */
	text_reset();
	text_add("host ");
	text_add_key(key1);
	text_add_bytes("\0\n", 2);
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "host", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(out.size, file_size);
	CHECK_BYTES(out.data, file_text, file_size);
	ssh_buf_free(&out);

	/* Removing twice removes nothing the second time. */
	text_reset();
	text_add("a ");
	text_add_key(key1);
	text_add("\na ");
	text_add_key(key2);
	text_add("\nb ");
	text_add_key(key3);
	text_add("\n");
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "a", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 2);
	ssh_buf_init(&again, SSH_HOSTFILE_MAX, 0);
	error = ssh_known_hosts_remove((const char *)out.data, out.size, "a", 22, &again, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(again.size, out.size);
	CHECK_BYTES(again.data, out.data, out.size);
	ssh_buf_free(&again);
	ssh_buf_free(&out);

	/* A buffer that is too small gets nothing: EMSGSIZE, size unchanged, removed 0. */
	ssh_buf_init(&out, file_size - 1U, 0);
	removed = 99;
	error = ssh_known_hosts_remove(file_text, file_size, "zz", 22, &out, &removed);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(removed, 0);
	ssh_buf_free(&out);

	/* A buffer of exactly the text's size is enough when nothing is removed. */
	ssh_buf_init(&out, file_size, 0);
	error = ssh_known_hosts_remove(file_text, file_size, "zz", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, file_size);
	ssh_buf_free(&out);

	/* Content already in out stays in front. */
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	ssh_buf_put_bytes(&out, "X", 1U);
	error = ssh_known_hosts_remove(file_text, file_size, "a", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.data[0], 'X');
	CHECK_EQ(out.size, 1U + 2U + 11U + 1U + 68U + 1U);
	ssh_buf_free(&out);

	/* Bad host names are EINVAL with nothing appended. */
	ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
	for (index = 0; index < sizeof(bad_hosts) / sizeof(bad_hosts[0]); index++) {
		removed = 99;
		error = ssh_known_hosts_remove(file_text, file_size, bad_hosts[index], 22, &out, &removed);
		check_record_long(error, EINVAL, __FILE__, __LINE__, "remove refuses the host");
		check_record_long((long)out.size, 0, __FILE__, __LINE__, "remove appended nothing");
		check_record_long((long)removed, 0, __FILE__, __LINE__, "remove removed 0");
	}

	/* Missing arguments, a file over the limit. */
	CHECK_EQ(ssh_known_hosts_remove(NULL, 3, "a", 22, &out, &removed), EINVAL);
	CHECK_EQ(ssh_known_hosts_remove("", 0, NULL, 22, &out, &removed), EINVAL);
	CHECK_EQ(ssh_known_hosts_remove("", 0, "a", 22, NULL, &removed), EINVAL);
	CHECK_EQ(ssh_known_hosts_remove("", 0, "a", 22, &out, NULL), EINVAL);
	CHECK_EQ(ssh_known_hosts_remove(file_text, SSH_HOSTFILE_MAX + 1U, "a", 22, &out, &removed), EMSGSIZE);
	CHECK_EQ(out.size, 0);
	error = ssh_known_hosts_remove(NULL, 0, "a", 22, &out, &removed);
	CHECK_EQ(error, 0);
	CHECK_EQ(removed, 0);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);
}

/* A small pseudo-random generator (xorshift32), so the fuzz run is the same each time. */
static uint32_t fuzz_state = 2463534242U;

/* Returns the next pseudo-random number. */
static uint32_t
fuzz_next(void)
{
	/* One xorshift32 step. */
	fuzz_state ^= fuzz_state << 13;
	fuzz_state ^= fuzz_state >> 17;
	fuzz_state ^= fuzz_state << 5;
	return fuzz_state;
}

/*
 * Mutates a known_hosts text at random and runs every function on it
 * under the sanitizers: nothing crashes, the answers are in range, the
 * output of remove is never longer than its input and removing again
 * removes nothing, and put_line output is found again.
 */
static void
test_fuzz(void)
{
	static const char alphabet[] = " \t\r\n,*?!#@|[]:-.abcXYZ019\x01\x80";
	static const char *const hosts[] = { "host", "a.example", "[host]:2222", "h,*", "*", "!h,*", "[a]:22", "@revoked", "|1|ab|cd", "@cert-authority" };
	struct ssh_buf out;
	struct ssh_buf again;
	enum ssh_known_result result;
	unsigned long round;
	unsigned skipped;
	unsigned removed;
	unsigned removed_again;
	size_t position;
	size_t count;
	size_t index;
	unsigned key_pick;
	unsigned end_pick;
	unsigned cut_pick;
	int found;
	int error;

	/* Each round builds a file from a few good lines, changes some bytes and runs the functions. */
	for (round = 0; round < TEST_FUZZ_COUNT; round++) {
		text_reset();
		count = 1U + fuzz_next() % 6U;
		for (index = 0; index < count; index++) {
			text_add(hosts[fuzz_next() % (sizeof(hosts) / sizeof(hosts[0]))]);
			text_add(" ");
			key_pick = fuzz_next() & 1U;
			if (key_pick != 0)
				text_add_key(key1);
			else
				text_add_key(key2);

			/* Ends the line with a comment or with a carriage return. */
			end_pick = fuzz_next() & 1U;
			if (end_pick != 0)
				text_add(" c\n");
			else
				text_add("\r\n");
		}

		/* Changes up to four bytes, and sometimes cuts the text. */
		count = fuzz_next() % 5U;
		for (index = 0; index < count && file_size > 0; index++) {
			position = fuzz_next() % file_size;
			file_text[position] = alphabet[fuzz_next() % (sizeof(alphabet) - 1U)];
		}

		/* Sometimes cuts the text short at a random place. */
		cut_pick = fuzz_next() & 3U;
		if (cut_pick == 0 && file_size > 0)
			file_size = fuzz_next() % file_size;

		/* The check answers in range with either key and on both name forms. */
		error = ssh_known_hosts_check(file_text, file_size, "host", 22, key1, &result, &skipped);
		check_record_long(error, 0, __FILE__, __LINE__, "fuzz check");
		check_record((int)result >= SSH_KNOWN_ABSENT && (int)result <= SSH_KNOWN_OTHER_TYPES, __FILE__, __LINE__, "fuzz result in range");
		check_record(skipped <= file_size, __FILE__, __LINE__, "fuzz skipped at most the lines");
		error = ssh_known_hosts_check(file_text, file_size, "a.example", 2222, key2, &result, &skipped);
		check_record_long(error, 0, __FILE__, __LINE__, "fuzz check 2");

		/* The authorized_keys reader answers 0 or 1. */
		error = ssh_authorized_keys_find(file_text, file_size, key1, &found, &skipped);
		check_record_long(error, 0, __FILE__, __LINE__, "fuzz find");
		check_record(found == 0 || found == 1, __FILE__, __LINE__, "fuzz found is 0 or 1");

		/* Removing is never longer than the input; removing again removes nothing. */
		ssh_buf_init(&out, SSH_HOSTFILE_MAX, 0);
		error = ssh_known_hosts_remove(file_text, file_size, "host", 22, &out, &removed);
		check_record_long(error, 0, __FILE__, __LINE__, "fuzz remove");
		check_record(out.size <= file_size, __FILE__, __LINE__, "fuzz remove is not longer");
		check_record((removed == 0) == (out.size == file_size), __FILE__, __LINE__, "fuzz removed matches the size");
		ssh_buf_init(&again, SSH_HOSTFILE_MAX, 0);
		error = ssh_known_hosts_remove((const char *)out.data, out.size, "host", 22, &again, &removed_again);
		check_record_long(error, 0, __FILE__, __LINE__, "fuzz remove again");
		check_record_long((long)removed_again, 0, __FILE__, __LINE__, "fuzz remove again removes nothing");
		check_record(again.size == out.size, __FILE__, __LINE__, "fuzz remove again keeps all");
		ssh_buf_free(&again);
		ssh_buf_free(&out);
	}
}
