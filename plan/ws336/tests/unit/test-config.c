/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-config.c.  The files are written here in the syntax of
 * sshd_config(5) and ssh_config(5) of OpenSSH 10.5p1: "Keyword value" or
 * "Keyword=value", keywords without regard to case, '#' comments, double
 * quotes for a value with spaces, the first value of a keyword winning,
 * Host blocks with white-space separated patterns where a negated pattern
 * that matches rules the block out (ssh_config(5) Host and PATTERNS),
 * and the times of sshd_config(5) TIME FORMATS (600 seconds, 10m, 1h30m).
 * The expected values are written by hand from those pages; no output of
 * another implementation is used.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "ssh-channel.h"
#include "ssh-config.h"

/* The sshd_config and ssh_config files of the image today, copied as they are. */
static const char shipped_sshd[] =
	"# zedBSD sshd configuration.\n"
	"# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib\n"
	"\n"
	"Port 22\n"
	"AddressFamily inet\n"
	"ListenAddress 0.0.0.0\n"
	"\n"
	"HostKey /etc/ssh/ssh_host_ed25519_key\n"
	"HostKey /etc/ssh/ssh_host_rsa_key\n"
	"\n"
	"PermitRootLogin yes\n"
	"PasswordAuthentication yes\n"
	"PermitEmptyPasswords no\n"
	"PubkeyAuthentication yes\n"
	"\n"
	"# The system has no name service of its own to consult about its clients.\n"
	"UseDNS no\n"
	"\n"
	"Subsystem sftp /usr/libexec/sftp-server\n";

static const char shipped_ssh[] =
	"# zedBSD ssh client configuration.\n"
	"# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib\n"
	"\n"
	"Host *\n"
	"\t# Nothing here resolves a name to check a fingerprint against.\n"
	"\tVerifyHostKeyDNS no\n";

/* The number of random lines tried in each fuzz test. */
#define TEST_FUZZ_COUNT		30000UL

/* The configurations under test (they are large, so they are not on the stack), and the error message. */
static struct sshd_config sshd;
static struct ssh_config client;
static char error[SSH_CONFIG_ERROR_MAX + 8];

/* One row of the time table: the text and the seconds it stands for, or -1 for a text that is not a time. */
struct time_case {
	const char *text;
	long seconds;
};

/*
 * The times.  The qualifiers and the sums are those of sshd_config(5)
 * TIME FORMATS: 10m is 600, 1h30m is 5400, d is 86400, w is 604800, a
 * number alone is seconds; the largest accepted is 2147483647.
 */
static const struct time_case time_cases[] = {
	{ "0", 0 }, { "1", 1 }, { "120", 120 }, { "600", 600 },
	{ "2m", 120 }, { "2M", 120 }, { "90s", 90 }, { "90S", 90 },
	{ "1h", 3600 }, { "1H", 3600 }, { "1h30m", 5400 }, { "10m", 600 },
	{ "1d", 86400 }, { "1D", 86400 }, { "1w", 604800 }, { "2W", 1209600 },
	{ "1h30", 3630 }, { "30s30", 60 }, { "1m1m", 120 }, { "1w1d1h1m1s", 694861 },
	{ "00000000120", -1 }, { "2147483647", 2147483647L }, { "2147483648", -1 }, { "4294967296", -1 },
	{ "35791394m", 2147483640L }, { "35791395m", -1 }, { "999999999", 999999999L },
	{ "1000000000", 1000000000L }, { "9999999999999999999999", -1 },
	{ "m", -1 }, { "h", -1 }, { "1x", -1 }, { "1.5h", -1 }, { "-1", -1 }, { "+1", -1 },
	{ "1m30x", -1 }, { "1 m", -2 }, { "0x10", -1 }, { "1mm", -1 }, { "m1", -1 }, { "1h-", -1 }
};

/* The number of rows of the time table. */
#define TIME_CASE_COUNT		(sizeof(time_cases) / sizeof(time_cases[0]))

static int client_add(const char *text, const char *host);

/* Fills sshd with the defaults and reads text into it; the status of sshd_config_parse. */
static int
sshd_read(
	const char *text)
{
	char *copy;
	size_t length;
	int status;

	/* The text is copied to a block of exactly its size, so that a read past its end is caught by the checker. */
	length = strlen(text);
	copy = malloc(length != 0 ? length : 1);
	CHECK(copy != NULL);
	memcpy(copy, text, length);
	sshd_config_defaults(&sshd);
	memset(error, 'x', sizeof(error) - 1);
	error[sizeof(error) - 1] = '\0';
	status = sshd_config_parse(&sshd, copy, length, error);
	free(copy);
	return status;
}

/* Clears the client configuration and reads text into it for host; the status of ssh_config_parse. */
static int
client_read(
	const char *text,
	const char *host)
{
	ssh_config_init(&client);
	return client_add(text, host);
}

/* Applies one option to the client configuration, which is not cleared. */
static int
client_option(
	const char *option)
{
	char *copy;
	size_t length;
	int status;

	/* The option is copied to a block of exactly its size with its NUL, so that a read past its end is caught. */
	length = strlen(option);
	copy = malloc(length + 1);
	CHECK(copy != NULL);
	memcpy(copy, option, length + 1);
	memset(error, 'x', sizeof(error) - 1);
	error[sizeof(error) - 1] = '\0';
	status = ssh_config_option(&client, copy, error);
	free(copy);
	return status;
}

/* Reads text into the client for host without clearing it first (a second file); the status. */
static int
client_add(
	const char *text,
	const char *host)
{
	char *copy;
	size_t length;
	int status;

	/* The text is copied to a block of exactly its size, so that a read past its end is caught by the checker. */
	length = strlen(text);
	copy = malloc(length != 0 ? length : 1);
	CHECK(copy != NULL);
	memcpy(copy, text, length);
	memset(error, 'x', sizeof(error) - 1);
	error[sizeof(error) - 1] = '\0';
	status = ssh_config_parse(&client, copy, length, host, error);
	free(copy);
	return status;
}

/* Reads text into the client for host with the defaults applied after it; the status. */
static int
client_read_defaults(
	const char *text,
	const char *host)
{
	int status;

	/* Reads the text and then fills in what it did not set. */
	status = client_read(text, host);
	ssh_config_defaults(&client);
	return status;
}

/* Reports whether the message has the prefix. */
static int
error_starts(
	const char *prefix)
{
	/* Compares the first bytes. */
	return strncmp(error, prefix, strlen(prefix)) == 0;
}

/* Reports whether the message holds the text. */
static int
error_has(
	const char *text)
{
	/* Looks for the text. */
	return strstr(error, text) != NULL;
}

/* Builds a line of the keyword, a space and count copies of the byte into line (room for count + 64 bytes). */
static void
make_line(
	char *line,
	const char *keyword,
	char byte,
	size_t count)
{
	size_t length;

	/* The keyword and the separator. */
	length = strlen(keyword);
	memcpy(line, keyword, length);
	line[length] = ' ';
	length++;

	/* The run of bytes and the end. */
	memset(line + length, byte, count);
	line[length + count] = '\0';
}

/* Counts the bits set in a mask. */
static int
bit_count(
	uint32_t mask)
{
	int count;

	/* Adds one for each set bit. */
	count = 0;
	while (mask != 0) {
		count += (int)(mask & 1U);
		mask >>= 1;
	}

	return count;
}

/* Checks the defaults of sshd (ssh-config.h). */
static void
test_sshd_defaults(void)
{
	/* Every default the header lists. */
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd.port, 22);
	CHECK(strcmp(sshd.listen_address, "0.0.0.0") == 0);
	CHECK_EQ(sshd.inet6, 0);
	CHECK_EQ(sshd.host_key_count, 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD);
	CHECK_EQ(sshd.password, 1);
	CHECK_EQ(sshd.pubkey, 1);
	CHECK_EQ(sshd.permit_empty, 0);
	CHECK_EQ(sshd.max_auth_tries, 6);
	CHECK_EQ(sshd.login_grace_time, 120);
	CHECK_EQ(sshd.max_sessions, SSH_CHANNEL_MAX);
	CHECK_EQ(sshd.max_startups, 10);
	CHECK(strcmp(sshd.authorized_keys_file, ".ssh/authorized_keys") == 0);
	CHECK_EQ(sshd.strict_modes, 1);
	CHECK_EQ(sshd.sftp_command[0], 0);
	CHECK_EQ(sshd.client_alive_interval, 0);
	CHECK_EQ(sshd.client_alive_count, 3);

	/* The default host key is added by finish and only when no HostKey gave one. */
	CHECK_EQ(sshd_config_finish(&sshd), 0);
	CHECK_EQ(sshd.host_key_count, 1);
	CHECK(strcmp(sshd.host_key[0], "/etc/ssh/ssh_host_ed25519_key") == 0);
	CHECK_EQ(sshd_config_finish(&sshd), 0);
	CHECK_EQ(sshd.host_key_count, 1);
	CHECK_EQ(sshd_read("HostKey /k1\n"), 0);
	CHECK_EQ(sshd_config_finish(&sshd), 0);
	CHECK_EQ(sshd.host_key_count, 1);
	CHECK(strcmp(sshd.host_key[0], "/k1") == 0);

	/* A missing structure is refused or ignored without a crash. */
	sshd_config_defaults(NULL);
	CHECK_EQ(sshd_config_finish(NULL), EINVAL);
	CHECK_EQ(sshd_config_parse(NULL, "Port 1\n", 7, error), EINVAL);
}

/* Reads the files the image has today, as copied above and as they are in the tree when it is there. */
static void
test_shipped(void)
{
	FILE *file;
	char text[2048];
	size_t size;

	/* The copy of sshd_config: every line is read, the rsa key is the second HostKey. */
	CHECK_EQ(sshd_read(shipped_sshd), 0);
	CHECK_EQ(sshd.port, 22);
	CHECK(strcmp(sshd.listen_address, "0.0.0.0") == 0);
	CHECK_EQ(sshd.inet6, 0);
	CHECK_EQ(sshd.host_key_count, 2);
	CHECK(strcmp(sshd.host_key[0], "/etc/ssh/ssh_host_ed25519_key") == 0);
	CHECK(strcmp(sshd.host_key[1], "/etc/ssh/ssh_host_rsa_key") == 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_YES);
	CHECK_EQ(sshd.password, 1);
	CHECK_EQ(sshd.permit_empty, 0);
	CHECK_EQ(sshd.pubkey, 1);
	CHECK(strcmp(sshd.sftp_command, "/usr/libexec/sftp-server") == 0);
	CHECK_EQ(error[0], 0);

	/* The copy of ssh_config: the Host * block applies and VerifyHostKeyDNS no is accepted. */
	CHECK_EQ(client_read_defaults(shipped_ssh, "any.example.com"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client.port, 22);
	CHECK_EQ(client.strict, SSH_STRICT_ASK);
	CHECK_EQ(error[0], 0);

	/* The files in the tree, when this test runs from the top of the tree. */
	file = fopen("userland/packages/network/openssh/files/sshd_config", "rb");
	if (file != NULL) {
		size = fread(text, 1, sizeof(text), file);
		fclose(file);
		sshd_config_defaults(&sshd);
		CHECK_EQ(sshd_config_parse(&sshd, text, size, error), 0);
	}

	file = fopen("userland/packages/network/openssh/files/ssh_config", "rb");
	if (file != NULL) {
		size = fread(text, 1, sizeof(text), file);
		fclose(file);
		ssh_config_init(&client);
		CHECK_EQ(ssh_config_parse(&client, text, size, "host", error), 0);
	}
}

/* Checks a yes/no keyword of sshd, whose field is at offset in struct sshd_config. */
static void
check_sshd_bool(
	const char *keyword,
	size_t offset)
{
	char line[128];
	const int *field;
	static const char *const bad[] = { "maybe", "true", "false", "1", "0", "yes no", "y", "" };
	size_t index;

	/* The field in the structure that was last filled. */
	field = (const int *)((const char *)&sshd + offset);

	/* yes and no, in either case, plain or in quotes, with '=' or a tab. */
	sprintf(line, "%s yes\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 1);
	sprintf(line, "%s no\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 0);
	sprintf(line, "%s YES\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 1);
	sprintf(line, "%s No\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 0);
	sprintf(line, "%s \"yes\"\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 1);
	sprintf(line, "%s=no\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 0);
	sprintf(line, "%s\t=\tyes # comment\n", keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 1);

	/* The keyword is read without regard to case. */
	sprintf(line, "%s no\n", keyword);
	line[0] = (char)(line[0] >= 'A' && line[0] <= 'Z' ? line[0] - 'A' + 'a' : line[0]);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 0);

	/* A value that is not yes or no, a missing value and two values are errors with the line number. */
	for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
		sprintf(line, "# first\n%s %s\n", keyword, bad[index]);
		CHECK_EQ(sshd_read(line), EINVAL);
		CHECK(error_starts("line 2: "));
		CHECK(error_has(keyword));
	}

	/* The first value wins, also when the second is the other one. */
	sprintf(line, "%s yes\n%s no\n", keyword, keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 1);
	sprintf(line, "%s no\n%s yes\n", keyword, keyword);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, 0);

	/* A bad second line is still an error. */
	sprintf(line, "%s yes\n%s maybe\n", keyword, keyword);
	CHECK_EQ(sshd_read(line), EINVAL);
	CHECK(error_starts("line 2: "));
}

/* Checks a numeric keyword of sshd, whose field is at offset in struct sshd_config and whose range is minimum to maximum. */
static void
check_sshd_number(
	const char *keyword,
	size_t offset,
	unsigned minimum,
	unsigned maximum)
{
	char line[128];
	const unsigned *field;
	static const char *const bad[] = { "-1", "1x", "", "0x10", "1.5", "+1", "1 2", "one", "1,5" };
	size_t index;

	/* The field in the structure that was last filled. */
	field = (const unsigned *)((const char *)&sshd + offset);

	/* The two ends of the range and a middle value. */
	sprintf(line, "%s %u\n", keyword, minimum);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, minimum);
	sprintf(line, "%s %u\n", keyword, maximum);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, maximum);
	sprintf(line, "%s=%u\n", keyword, minimum + 1);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, minimum + 1);
	sprintf(line, "%s \"%u\"\n", keyword, maximum);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, maximum);

	/* One below and one above the range. */
	if (minimum > 0) {
		sprintf(line, "%s %u\n", keyword, minimum - 1);
		CHECK_EQ(sshd_read(line), EINVAL);
		CHECK(error_starts("line 1: "));
	}

	sprintf(line, "%s %u\n", keyword, maximum + 1);
	CHECK_EQ(sshd_read(line), EINVAL);
	CHECK(error_starts("line 1: "));
	sprintf(line, "%s 4294967296\n", keyword);
	CHECK_EQ(sshd_read(line), EINVAL);

	/* Words that are not numbers. */
	for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
		sprintf(line, "%s %s\n", keyword, bad[index]);
		CHECK_EQ(sshd_read(line), EINVAL);
	}

	sprintf(line, "%s \"\"\n", keyword);
	CHECK_EQ(sshd_read(line), EINVAL);
	sprintf(line, "%s\n", keyword);
	CHECK_EQ(sshd_read(line), EINVAL);

	/* The first value wins. */
	sprintf(line, "%s %u\n%s %u\n", keyword, minimum + 1, keyword, maximum);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(*field, minimum + 1);
}

/* Checks a time keyword of sshd, whose field is at offset in struct sshd_config. */
static void
check_sshd_time(
	const char *keyword,
	size_t offset)
{
	char line[128];
	const unsigned *field;
	size_t index;
	int status;

	/* The field in the structure that was last filled. */
	field = (const unsigned *)((const char *)&sshd + offset);

	/* Each row of the time table. */
	for (index = 0; index < TIME_CASE_COUNT; index++) {
		sprintf(line, "%s %s\n", keyword, time_cases[index].text);
		status = sshd_read(line);
		if (time_cases[index].seconds >= 0) {
			CHECK_EQ(status, 0);
			CHECK_EQ(*field, time_cases[index].seconds);
		} else {
			CHECK_EQ(status, EINVAL);
		}
	}

	/* An empty value and a missing value. */
	sprintf(line, "%s \"\"\n", keyword);
	CHECK_EQ(sshd_read(line), EINVAL);
	sprintf(line, "%s\n", keyword);
	CHECK_EQ(sshd_read(line), EINVAL);
}

/* Checks Port, ListenAddress and AddressFamily of sshd. */
static void
test_sshd_listen(void)
{
	char line[256];

	/* Port: the range and the forms of a number. */
	CHECK_EQ(sshd_read("Port 2222\n"), 0);
	CHECK_EQ(sshd.port, 2222);
	CHECK_EQ(sshd_read("port=1\n"), 0);
	CHECK_EQ(sshd.port, 1);
	CHECK_EQ(sshd_read("PORT 65535\n"), 0);
	CHECK_EQ(sshd.port, 65535);
	CHECK_EQ(sshd_read("Port \"22\"\n"), 0);
	CHECK_EQ(sshd.port, 22);
	CHECK_EQ(sshd_read("Port 000000022\n"), 0);
	CHECK_EQ(sshd.port, 22);
	CHECK_EQ(sshd_read("Port 0\n"), EINVAL);
	CHECK_EQ(sshd_read("Port 65536\n"), EINVAL);
	CHECK_EQ(sshd_read("Port 0000000022\n"), EINVAL);
	CHECK_EQ(sshd_read("Port -1\n"), EINVAL);
	CHECK_EQ(sshd_read("Port 22a\n"), EINVAL);
	CHECK_EQ(sshd_read("Port\n"), EINVAL);
	CHECK_EQ(sshd_read("Port #22\n"), EINVAL);
	CHECK_EQ(sshd_read("Port 1 2\n"), EINVAL);

	/* Port: the same value again is fine, another value is refused (only one port is kept). */
	CHECK_EQ(sshd_read("Port 2222\nPort 2222\n"), 0);
	CHECK_EQ(sshd.port, 2222);
	CHECK_EQ(sshd_read("Port 2222\nPort 22\n"), EINVAL);
	CHECK(error_starts("line 2: "));
	CHECK_EQ(sshd_read("Port 2222\nPort 99999\n"), EINVAL);

	/* ListenAddress: names and addresses without a port. */
	CHECK_EQ(sshd_read("ListenAddress 192.168.1.1\n"), 0);
	CHECK(strcmp(sshd.listen_address, "192.168.1.1") == 0);
	CHECK_EQ(sshd_read("ListenAddress ::1\n"), 0);
	CHECK(strcmp(sshd.listen_address, "::1") == 0);
	CHECK_EQ(sshd_read("ListenAddress fe80::1:2\n"), 0);
	CHECK_EQ(sshd_read("ListenAddress host-1.example.com\n"), 0);
	CHECK(strcmp(sshd.listen_address, "host-1.example.com") == 0);
	CHECK_EQ(sshd_read("ListenAddress my_host\n"), 0);
	CHECK(strcmp(sshd.listen_address, "my_host") == 0);
	CHECK_EQ(sshd_read("ListenAddress zz.ZZ-Az09\n"), 0);
	CHECK(strcmp(sshd.listen_address, "zz.ZZ-Az09") == 0);
	CHECK_EQ(sshd_read("ListenAddress \"::\"\n"), 0);
	CHECK(strcmp(sshd.listen_address, "::") == 0);
	make_line(line, "ListenAddress", 'a', 63);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(strlen(sshd.listen_address), 63);
	make_line(line, "ListenAddress", 'a', 64);
	CHECK_EQ(sshd_read(line), EINVAL);

	/* ListenAddress: a port, brackets, a second word, a bad byte and a missing value are errors. */
	CHECK_EQ(sshd_read("ListenAddress 1.2.3.4:22\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress host:22\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress [::1]:22\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress [::1]\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress 1.2.3.4 rdomain 5\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress a/b\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress \"a b\"\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress\n"), EINVAL);
	CHECK_EQ(sshd_read("ListenAddress \"\"\n"), EINVAL);

	/* ListenAddress: the first stays, a second that differs is refused. */
	CHECK_EQ(sshd_read("ListenAddress 1.1.1.1\nListenAddress 1.1.1.1\n"), 0);
	CHECK(strcmp(sshd.listen_address, "1.1.1.1") == 0);
	CHECK_EQ(sshd_read("ListenAddress 1.1.1.1\nListenAddress 2.2.2.2\n"), EINVAL);
	CHECK(error_starts("line 2: "));

	/* AddressFamily: any and inet read as 0, inet6 as 1; anything else is an error. */
	CHECK_EQ(sshd_read("AddressFamily inet\n"), 0);
	CHECK_EQ(sshd.inet6, 0);
	CHECK_EQ(sshd_read("AddressFamily inet6\n"), 0);
	CHECK_EQ(sshd.inet6, 1);
	CHECK_EQ(sshd_read("AddressFamily INET6\n"), 0);
	CHECK_EQ(sshd.inet6, 1);
	CHECK_EQ(sshd_read("AddressFamily any\n"), 0);
	CHECK_EQ(sshd.inet6, 0);
	CHECK_EQ(sshd_read("AddressFamily inet6\nAddressFamily inet\n"), 0);
	CHECK_EQ(sshd.inet6, 1);
	CHECK_EQ(sshd_read("AddressFamily ipv4\n"), EINVAL);
	CHECK_EQ(sshd_read("AddressFamily\n"), EINVAL);
	CHECK_EQ(sshd_read("AddressFamily inet inet6\n"), EINVAL);
}

/* Checks HostKey, AuthorizedKeysFile, PermitRootLogin, MaxStartups and Subsystem of sshd. */
static void
test_sshd_values(void)
{
	char line[1400];
	char text[2200];
	unsigned index;
	size_t length;

	/* HostKey: lines add to the list in order, a repeated path is added once. */
	CHECK_EQ(sshd_read("HostKey /a\nHostKey /b\nHostKey /a\n"), 0);
	CHECK_EQ(sshd.host_key_count, 2);
	CHECK(strcmp(sshd.host_key[0], "/a") == 0);
	CHECK(strcmp(sshd.host_key[1], "/b") == 0);
	CHECK_EQ(sshd_read("HostKey \"/etc/ssh/my key\"\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/etc/ssh/my key") == 0);
	CHECK_EQ(sshd_read("HostKey=/x\n"), 0);

	/* HostKey: eight are the most; a ninth is an error. */
	text[0] = '\0';
	for (index = 0; index < 8; index++) {
		sprintf(line, "HostKey /key%u\n", index);
		strcat(text, line);
	}

	CHECK_EQ(sshd_read(text), 0);
	CHECK_EQ(sshd.host_key_count, 8);
	CHECK(strcmp(sshd.host_key[7], "/key7") == 0);
	strcat(text, "HostKey /key8\n");
	CHECK_EQ(sshd_read(text), EINVAL);
	CHECK(error_starts("line 9: "));

	/* HostKey: path length 1023 is the longest, a control character, a token, an empty path and a missing value are errors. */
	make_line(line, "HostKey", 'k', 1023);
	CHECK_EQ(sshd_read(line), 0);
	CHECK_EQ(strlen(sshd.host_key[0]), 1023);
	make_line(line, "HostKey", 'k', 1024);
	CHECK_EQ(sshd_read(line), EINVAL);
	CHECK_EQ(sshd_read("HostKey \"/a\tb\"\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey \"/a\037b\"\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey \"/a\177b\"\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey \"/a~b \"\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/a~b ") == 0);
	CHECK_EQ(sshd_read("HostKey none\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "none") == 0);
	CHECK_EQ(sshd_read("HostKey /etc/%h\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey \"\"\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey\n"), EINVAL);
	CHECK_EQ(sshd_read("HostKey /a /b\n"), EINVAL);

	/* AuthorizedKeysFile: plain paths, the h, u and percent tokens, none. */
	CHECK_EQ(sshd_read("AuthorizedKeysFile .ssh/authorized_keys2\n"), 0);
	CHECK(strcmp(sshd.authorized_keys_file, ".ssh/authorized_keys2") == 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile %h/.ssh/authorized_keys\n"), 0);
	CHECK(strcmp(sshd.authorized_keys_file, "%h/.ssh/authorized_keys") == 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /etc/ssh/keys/%u\n"), 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%%b\n"), 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile none\n"), 0);
	CHECK(strcmp(sshd.authorized_keys_file, "/dev/null") == 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%x\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%h"), 0);
	CHECK_EQ(sshd_read("AuthorizedKeysFile %"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile /a%%%\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile a b\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile \"\"\n"), EINVAL);
	CHECK_EQ(sshd_read("AuthorizedKeysFile a\nAuthorizedKeysFile b\n"), 0);
	CHECK(strcmp(sshd.authorized_keys_file, "a") == 0);
	make_line(line, "AuthorizedKeysFile", 'p', 1023);
	CHECK_EQ(sshd_read(line), 0);
	make_line(line, "AuthorizedKeysFile", 'p', 1024);
	CHECK_EQ(sshd_read(line), EINVAL);

	/* PermitRootLogin: the three values it supports, in any case; forced-commands-only and others are errors. */
	CHECK_EQ(sshd_read("PermitRootLogin yes\n"), 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_YES);
	CHECK_EQ(sshd_read("PermitRootLogin no\n"), 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_NO);
	CHECK_EQ(sshd_read("PermitRootLogin prohibit-password\n"), 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD);
	CHECK_EQ(sshd_read("PermitRootLogin Prohibit-Password\n"), 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD);
	CHECK_EQ(sshd_read("PermitRootLogin forced-commands-only\n"), EINVAL);
	CHECK_EQ(sshd_read("PermitRootLogin without-password\n"), EINVAL);
	CHECK_EQ(sshd_read("PermitRootLogin maybe\n"), EINVAL);
	CHECK_EQ(sshd_read("PermitRootLogin\n"), EINVAL);
	CHECK_EQ(sshd_read("PermitRootLogin no\nPermitRootLogin yes\n"), 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_NO);

	/* MaxStartups: a number, or start:rate:full whose first number is kept. */
	CHECK_EQ(sshd_read("MaxStartups 5\n"), 0);
	CHECK_EQ(sshd.max_startups, 5);
	CHECK_EQ(sshd_read("MaxStartups 10:30:100\n"), 0);
	CHECK_EQ(sshd.max_startups, 10);
	CHECK_EQ(sshd_read("MaxStartups 3:0:3\n"), 0);
	CHECK_EQ(sshd.max_startups, 3);
	CHECK_EQ(sshd_read("MaxStartups 7:100:700\n"), 0);
	CHECK_EQ(sshd.max_startups, 7);
	CHECK_EQ(sshd_read("MaxStartups 1\n"), 0);
	CHECK_EQ(sshd.max_startups, 1);
	CHECK_EQ(sshd_read("MaxStartups 1000000\n"), 0);
	CHECK_EQ(sshd.max_startups, 1000000);
	CHECK_EQ(sshd_read("MaxStartups 1000001\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 0\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 20:30:10\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10:101:100\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10:30\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10:30:100:1\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10::100\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups :30:100\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10:30:\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 10:30:1000001\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups a\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups -1\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 1 2\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxStartups 4\nMaxStartups 8\n"), 0);
	CHECK_EQ(sshd.max_startups, 4);

	/* MaxSessions: 0 to 65535, a limit above the channels one connection has is cut to that number. */
	CHECK_EQ(sshd_read("MaxSessions 0\n"), 0);
	CHECK_EQ(sshd.max_sessions, 0);
	CHECK_EQ(sshd_read("MaxSessions 1\n"), 0);
	CHECK_EQ(sshd.max_sessions, 1);
	CHECK_EQ(sshd_read("MaxSessions 10\n"), 0);
	CHECK_EQ(sshd.max_sessions, 10);
	CHECK_EQ(sshd_read("MaxSessions 11\n"), 0);
	CHECK_EQ(sshd.max_sessions, SSH_CHANNEL_MAX);
	CHECK_EQ(sshd_read("MaxSessions 65535\n"), 0);
	CHECK_EQ(sshd.max_sessions, SSH_CHANNEL_MAX);
	CHECK_EQ(sshd_read("MaxSessions 65536\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxSessions -1\n"), EINVAL);
	CHECK_EQ(sshd_read("MaxSessions 3\nMaxSessions 4\n"), 0);
	CHECK_EQ(sshd.max_sessions, 3);

	/* Subsystem: only sftp with a one-word command. */
	CHECK_EQ(sshd_read("Subsystem sftp /usr/libexec/sftp-server\n"), 0);
	CHECK(strcmp(sshd.sftp_command, "/usr/libexec/sftp-server") == 0);
	CHECK_EQ(sshd_read("Subsystem sftp internal-sftp\n"), 0);
	CHECK(strcmp(sshd.sftp_command, "internal-sftp") == 0);
	CHECK_EQ(sshd_read("Subsystem=sftp /x\n"), 0);
	CHECK_EQ(sshd_read("Subsystem sftp \"/my dir/sftp-server\"\n"), 0);
	CHECK(strcmp(sshd.sftp_command, "/my dir/sftp-server") == 0);
	CHECK_EQ(sshd_read("Subsystem sftp\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp /x -l INFO\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp /x -l\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp /x%h\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp none\n"), 0);
	CHECK(strcmp(sshd.sftp_command, "none") == 0);
	CHECK_EQ(sshd_read("AcceptEnv w1 w2\nSubsystem sftp\n"), EINVAL);
	CHECK(error_starts("line 2: missing value"));
	CHECK_EQ(sshd_read("Subsystem scp /usr/bin/scp\n"), EINVAL);
	CHECK(error_starts("line 1: "));
	CHECK_EQ(sshd_read("Subsystem SFTP /x\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp \"\"\n"), EINVAL);
	CHECK_EQ(sshd_read("Subsystem sftp /a\nSubsystem sftp /b\n"), 0);
	CHECK(strcmp(sshd.sftp_command, "/a") == 0);
	CHECK_EQ(sshd_read("Subsystem sftp /a\nSubsystem other /b\n"), EINVAL);
	length = 0;
	make_line(line, "Subsystem sftp", '/', 1023);
	CHECK_EQ(sshd_read(line), 0);
	length = strlen(sshd.sftp_command);
	CHECK_EQ(length, 1023);
	make_line(line, "Subsystem sftp", '/', 1024);
	CHECK_EQ(sshd_read(line), EINVAL);
}

/* Checks the keywords sshd reads and ignores. */
static void
test_sshd_ignored(void)
{
	static const char *const accepted[] = {
		"UseDNS no", "UseDNS yes", "UseDNS=no", "PrintMotd no", "PrintMotd yes", "PrintLastLog no", "PrintLastLog yes",
		"UsePAM no", "X11Forwarding no", "AllowTcpForwarding no", "AllowAgentForwarding no", "GatewayPorts no",
		"PermitTunnel no", "PermitUserEnvironment no", "Compression no", "KbdInteractiveAuthentication no",
		"ChallengeResponseAuthentication no", "GSSAPIAuthentication no", "HostbasedAuthentication no",
		"Banner none", "AcceptEnv LANG LC_*", "AcceptEnv TERM", "acceptenv=LANG", "AcceptEnv FOO BAR BAZ", "usepam NO"
	};
	static const char *const refused[] = {
		"UseDNS maybe", "UseDNS", "PrintMotd 1", "UsePAM yes", "UsePAM", "X11Forwarding yes", "AllowTcpForwarding yes",
		"AllowTcpForwarding local", "AllowTcpForwarding all", "AllowAgentForwarding yes", "GatewayPorts yes",
		"GatewayPorts clientspecified", "PermitTunnel yes", "PermitTunnel point-to-point", "PermitUserEnvironment yes",
		"Compression yes", "Compression delayed", "KbdInteractiveAuthentication yes", "ChallengeResponseAuthentication yes",
		"GSSAPIAuthentication yes", "HostbasedAuthentication yes", "Banner /etc/issue", "Banner", "AcceptEnv",
		"UsePAM no yes", "Banner none none"
	};
	char line[160];
	char long_line[400];
	size_t index;

	/* Each accepted line changes nothing. */
	for (index = 0; index < sizeof(accepted) / sizeof(accepted[0]); index++) {
		sprintf(line, "%s\n", accepted[index]);
		CHECK_EQ(sshd_read(line), 0);
		CHECK_EQ(sshd.port, 22);
		CHECK_EQ(sshd.host_key_count, 0);
		CHECK_EQ(error[0], 0);
	}

	/* A value that would turn on a feature these programs lack is an error that names the keyword. */
	for (index = 0; index < sizeof(refused) / sizeof(refused[0]); index++) {
		sprintf(line, "\n%s\n", refused[index]);
		CHECK_EQ(sshd_read(line), EINVAL);
		CHECK(error_starts("line 2: "));
	}

	/* Match is refused whatever follows it, and an error after a Match line is not reached. */
	CHECK_EQ(sshd_read("Match User bob\n"), EINVAL);
	CHECK(error_starts("line 1: Match is not supported"));
	CHECK_EQ(sshd_read("Port 22\nMatch\n"), EINVAL);
	CHECK(error_starts("line 2: Match is not supported"));
	CHECK_EQ(sshd_read("match all\n"), EINVAL);

	/* Keywords of OpenSSH that these programs do not know are errors with the line number and the keyword. */
	CHECK_EQ(sshd_read("Port 22\n\n# c\nBogus 1\n"), EINVAL);
	CHECK(error_starts("line 4: unknown keyword: Bogus"));
	CHECK_EQ(sshd_read("Include /etc/ssh/sshd_config.d/*.conf\n"), EINVAL);
	CHECK_EQ(sshd_read("Ciphers aes128-ctr\n"), EINVAL);
	CHECK_EQ(sshd_read("ForceCommand /bin/true\n"), EINVAL);
	CHECK_EQ(sshd_read("Ports 22\n"), EINVAL);
	CHECK_EQ(sshd_read("Por 22\n"), EINVAL);
	CHECK_EQ(sshd_read("Port2 22\n"), EINVAL);

	/* A keyword with a control byte or a long name is shown cleaned and cut, in a message that ends properly. */
	CHECK_EQ(sshd_read("Bo\001gus 1\n"), EINVAL);
	CHECK(error_has("Bo?gus"));
	CHECK(strlen(error) < SSH_CONFIG_ERROR_MAX);
	memset(long_line, 'K', 300);
	strcpy(long_line + 300, " 1\n");
	CHECK_EQ(sshd_read(long_line), EINVAL);
	CHECK(strlen(error) < SSH_CONFIG_ERROR_MAX);
	CHECK(strlen(error) > 20);
	CHECK(error_has("KKKKKKKKKKKKKKKK"));
	CHECK(error_has("unknown keyword: KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"));
	CHECK(!error_has("KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"));
}

/* Checks the shape of lines: white space, comments, quotes, CR LF, long lines and line numbers. */
static void
test_syntax(void)
{
	static char big[SSH_CONFIG_FILE_MAX + 64U];
	char line[9000];
	char words[1200];
	unsigned count;
	size_t size;

	/* Blank lines, indentation, tabs, comments and a last line with no LF. */
	CHECK_EQ(sshd_read("\n\n   \n\t\n# Port 1\n   # Port 2\n\tPort\t2200\t# the port\nMaxAuthTries 3"), 0);
	CHECK_EQ(sshd.port, 2200);
	CHECK_EQ(sshd.max_auth_tries, 3);

	/* CR LF line ends, also on the last line, and a CR that stands alone as a blank. */
	CHECK_EQ(sshd_read("Port 2201\r\nMaxAuthTries 4\r\nMaxSessions 5\r"), 0);
	CHECK_EQ(sshd.port, 2201);
	CHECK_EQ(sshd.max_auth_tries, 4);
	CHECK_EQ(sshd.max_sessions, 5);
	CHECK_EQ(sshd_read("# x\r\n\r\nBogus\r\n"), EINVAL);
	CHECK(error_starts("line 3: "));

	/* The separator: blanks, or one '=' with blanks around it; a second '=' belongs to the value. */
	CHECK_EQ(sshd_read("Port=2202\n"), 0);
	CHECK_EQ(sshd.port, 2202);
	CHECK_EQ(sshd_read("Port =2203\n"), 0);
	CHECK_EQ(sshd.port, 2203);
	CHECK_EQ(sshd_read("Port= 2204\n"), 0);
	CHECK_EQ(sshd.port, 2204);
	CHECK_EQ(sshd_read("Port \t = \t 2205\n"), 0);
	CHECK_EQ(sshd.port, 2205);
	CHECK_EQ(sshd_read("Port==2206\n"), EINVAL);
	CHECK_EQ(sshd_read("Port = = 2206\n"), EINVAL);
	CHECK_EQ(sshd_read("=2206\n"), EINVAL);
	CHECK(error_starts("line 1: missing keyword"));
	CHECK_EQ(sshd_read("  = 1\n"), EINVAL);

	/* Quotes: a quoted word may hold blanks and '#', must close, and must be followed by a blank. */
	CHECK_EQ(sshd_read("HostKey \"/a b#c\"\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/a b#c") == 0);
	CHECK_EQ(sshd_read("HostKey \"/a\"   # tail\n"), 0);
	CHECK_EQ(sshd_read("HostKey \"/a\n"), EINVAL);
	CHECK(error_has("unterminated quote"));
	CHECK_EQ(sshd_read("HostKey \"/a\"b\n"), EINVAL);
	CHECK(error_has("text after a closing quote"));
	CHECK_EQ(sshd_read("HostKey /a\"b\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/a\"b") == 0);
	CHECK_EQ(sshd_read("HostKey \"\"\"\"\n"), EINVAL);

	/* A '#' that starts a word is a comment; one inside a word is part of it. */
	CHECK_EQ(sshd_read("HostKey /a#b\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/a#b") == 0);
	CHECK_EQ(sshd_read("HostKey /a #b\n"), 0);
	CHECK(strcmp(sshd.host_key[0], "/a") == 0);
	CHECK_EQ(sshd_read("Port 22 # 33 44\n"), 0);

	/* A vertical tab and a form feed are blanks. */
	CHECK_EQ(sshd_read("Port\v2207\f\n"), 0);
	CHECK_EQ(sshd.port, 2207);
	CHECK_EQ(sshd_read("\v\fPort\f2208\v# x\n"), 0);
	CHECK_EQ(sshd.port, 2208);

	/* A line number counts every line, also blank and comment lines and CR LF ones. */
	CHECK_EQ(sshd_read("Port 22\n\n# a\n  \r\nPort x\n"), EINVAL);
	CHECK(error_starts("line 5: "));

	/* A line of 8192 bytes is the longest; a longer one is an error; a long comment is not a line. */
	strcpy(line, "AcceptEnv ");
	memset(line + 10, 'E', 8192U - 10U);
	line[8192] = '\0';
	CHECK_EQ(sshd_read(line), 0);
	memset(line + 10, 'E', 8193U - 10U);
	line[8193] = '\0';
	CHECK_EQ(sshd_read(line), EINVAL);
	CHECK(error_starts("line 1: line too long"));
	line[0] = '#';
	CHECK_EQ(sshd_read(line), 0);
	strcpy(line, "    AcceptEnv ");
	memset(line + 14, 'E', 8192U - 10U);
	line[14 + 8192U - 10U] = '\0';
	CHECK_EQ(sshd_read(line), 0);

	/* Trailing blanks do not count towards the length. */
	strcpy(line, "AcceptEnv ");
	memset(line + 10, 'E', 8192U - 10U);
	memset(line + 8192, ' ', 400);
	line[8592] = '\0';
	CHECK_EQ(sshd_read(line), 0);

	/* A NUL byte is an error, with the number of its line, also in a comment. */
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\n#a\0b\n", 12, error), EINVAL);
	CHECK(error_starts("line 2: NUL byte"));
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\0\n", 9, error), EINVAL);
	CHECK(error_starts("line 1: NUL byte"));

	/* The most words on a line is 128. */
	strcpy(words, "AcceptEnv");
	for (count = 0; count < 128; count++)
		strcat(words, " w");
	strcat(words, "\n");
	CHECK_EQ(sshd_read(words), 0);
	strcpy(words, "AcceptEnv");
	for (count = 0; count < 129; count++)
		strcat(words, " w");
	strcat(words, "\n");
	CHECK_EQ(sshd_read(words), EINVAL);
	CHECK(error_has("too many values"));

	/* The largest file is SSH_CONFIG_FILE_MAX bytes; one more is EMSGSIZE. */
	memset(big, '#', SSH_CONFIG_FILE_MAX + 1U);
	size = SSH_CONFIG_FILE_MAX;
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, big, size, error), 0);
	memcpy(big, "Port 7\n", 7);
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, big, size, error), 0);
	CHECK_EQ(sshd.port, 7);
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, big, size + 1U, error), EMSGSIZE);
	CHECK(error_has("too large"));
	CHECK_EQ(sshd.port, 22);
	ssh_config_init(&client);
	CHECK_EQ(ssh_config_parse(&client, big, size + 1U, "h", error), EMSGSIZE);

	/* No text at all is a configuration with nothing in it; text of a size with no bytes is an error. */
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, NULL, 0, error), 0);
	CHECK_EQ(sshd_config_parse(&sshd, "", 0, error), 0);
	CHECK_EQ(sshd_config_parse(&sshd, NULL, 5, error), EINVAL);
	CHECK_EQ(sshd_config_parse(&sshd, "Bogus 1\n", 0, error), 0);
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\nPort 3", 7, error), 0);

	/* A message is optional, and a message left from an earlier call is cleared by a call that succeeds. */
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, "Bogus 1\n", 8, NULL), EINVAL);
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\n", 8, NULL), 0);
	strcpy(error, "stale");
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\n", 8, error), 0);
	CHECK_EQ(error[0], 0);

	/* The text is cut at the size: a line that continues past it is not read. */
	sshd_config_defaults(&sshd);
	CHECK_EQ(sshd_config_parse(&sshd, "Port 22\nPort 3333\n", 13, error), EINVAL);
	CHECK_EQ(sshd_config_parse(&sshd, "Port 33333\n", 9, error), 0);
	CHECK_EQ(sshd.port, 3333);
}

/* Checks the ssh_config keywords that take a yes/no, a number, a time or one of a few words. */
static void
test_ssh_values(void)
{
	char line[1400];
	char text[2200];
	size_t index;
	unsigned count;
	int status;

	/* BatchMode, PasswordAuthentication and PubkeyAuthentication take yes and no. */
	CHECK_EQ(client_read_defaults("BatchMode yes\n", "h"), 0);
	CHECK_EQ(client.batch_mode, 1);
	CHECK_EQ(client_read_defaults("BatchMode=No\n", "h"), 0);
	CHECK_EQ(client.batch_mode, 0);
	CHECK_EQ(client_read_defaults("PasswordAuthentication no\n", "h"), 0);
	CHECK_EQ(client.password, 0);
	CHECK_EQ(client.pubkey, 1);
	CHECK_EQ(client_read_defaults("PubkeyAuthentication no\n", "h"), 0);
	CHECK_EQ(client.pubkey, 0);
	CHECK_EQ(client.password, 1);
	CHECK_EQ(client_read("BatchMode maybe\n", "h"), EINVAL);
	CHECK(error_starts("line 1: "));
	CHECK_EQ(client_read("PasswordAuthentication\n", "h"), EINVAL);
	CHECK_EQ(client_read("PubkeyAuthentication true\n", "h"), EINVAL);

	/* Port: 1 to 65535. */
	CHECK_EQ(client_read("Port 2222\n", "h"), 0);
	CHECK_EQ(client.port, 2222);
	CHECK_EQ(client_read("Port 65535\n", "h"), 0);
	CHECK_EQ(client.port, 65535);
	CHECK_EQ(client_read("Port 1\n", "h"), 0);
	CHECK_EQ(client.port, 1);
	CHECK_EQ(client_read("Port 0\n", "h"), EINVAL);
	CHECK_EQ(client_read("Port 65536\n", "h"), EINVAL);
	CHECK_EQ(client_read("Port ssh\n", "h"), EINVAL);

	/* ConnectTimeout and ServerAliveInterval take the times of the time table. */
	for (index = 0; index < TIME_CASE_COUNT; index++) {
		sprintf(line, "ConnectTimeout %s\n", time_cases[index].text);
		status = client_read(line, "h");
		if (time_cases[index].seconds >= 0) {
			CHECK_EQ(status, 0);
			CHECK_EQ(client.connect_timeout, time_cases[index].seconds);
		} else {
			CHECK_EQ(status, EINVAL);
		}

		sprintf(line, "ServerAliveInterval %s\n", time_cases[index].text);
		status = client_read(line, "h");
		if (time_cases[index].seconds >= 0) {
			CHECK_EQ(status, 0);
			CHECK_EQ(client.server_alive_interval, time_cases[index].seconds);
		} else {
			CHECK_EQ(status, EINVAL);
		}
	}

	/* ServerAliveCountMax: 0 to 1000000. */
	CHECK_EQ(client_read("ServerAliveCountMax 0\n", "h"), 0);
	CHECK_EQ(client.server_alive_count, 0);
	CHECK_EQ(client_read("ServerAliveCountMax 1000000\n", "h"), 0);
	CHECK_EQ(client.server_alive_count, 1000000);
	CHECK_EQ(client_read("ServerAliveCountMax 1000001\n", "h"), EINVAL);
	CHECK_EQ(client_read("ServerAliveCountMax -1\n", "h"), EINVAL);
	CHECK_EQ(client_read("ServerAliveCountMax x\n", "h"), EINVAL);

	/* StrictHostKeyChecking: the five words, off as no. */
	CHECK_EQ(client_read("StrictHostKeyChecking yes\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_YES);
	CHECK_EQ(client_read("StrictHostKeyChecking no\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_NO);
	CHECK_EQ(client_read("StrictHostKeyChecking off\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_NO);
	CHECK_EQ(client_read("StrictHostKeyChecking ask\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_ASK);
	CHECK_EQ(client_read("StrictHostKeyChecking accept-new\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_ACCEPT_NEW);
	CHECK_EQ(client_read("StrictHostKeyChecking ACCEPT-NEW\n", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_ACCEPT_NEW);
	CHECK_EQ(client_read("StrictHostKeyChecking maybe\n", "h"), EINVAL);
	CHECK_EQ(client_read("StrictHostKeyChecking\n", "h"), EINVAL);
	CHECK_EQ(client_read("StrictHostKeyChecking yes no\n", "h"), EINVAL);
	CHECK_EQ(client_read_defaults("", "h"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_ASK);

	/* RequestTTY: no, yes, force, auto. */
	CHECK_EQ(client_read("RequestTTY no\n", "h"), 0);
	CHECK_EQ(client.request_tty, SSH_TTY_NO);
	CHECK_EQ(client_read("RequestTTY yes\n", "h"), 0);
	CHECK_EQ(client.request_tty, SSH_TTY_YES);
	CHECK_EQ(client_read("RequestTTY force\n", "h"), 0);
	CHECK_EQ(client.request_tty, SSH_TTY_FORCE);
	CHECK_EQ(client_read("RequestTTY auto\n", "h"), 0);
	CHECK_EQ(client.request_tty, SSH_TTY_AUTO);
	CHECK_EQ(client_read("RequestTTY always\n", "h"), EINVAL);

	/* LogLevel: QUIET, FATAL and ERROR as 0, INFO 1, VERBOSE 2, DEBUG to DEBUG3 as 3. */
	CHECK_EQ(client_read("LogLevel QUIET\n", "h"), 0);
	CHECK_EQ(client.log_level, 0);
	CHECK_EQ(client_read("LogLevel FATAL\n", "h"), 0);
	CHECK_EQ(client.log_level, 0);
	CHECK_EQ(client_read("LogLevel ERROR\n", "h"), 0);
	CHECK_EQ(client.log_level, 0);
	CHECK_EQ(client_read("LogLevel INFO\n", "h"), 0);
	CHECK_EQ(client.log_level, 1);
	CHECK_EQ(client_read("LogLevel VERBOSE\n", "h"), 0);
	CHECK_EQ(client.log_level, 2);
	CHECK_EQ(client_read("LogLevel DEBUG\n", "h"), 0);
	CHECK_EQ(client.log_level, 3);
	CHECK_EQ(client_read("LogLevel DEBUG1\n", "h"), 0);
	CHECK_EQ(client.log_level, 3);
	CHECK_EQ(client_read("LogLevel DEBUG2\n", "h"), 0);
	CHECK_EQ(client.log_level, 3);
	CHECK_EQ(client_read("LogLevel debug3\n", "h"), 0);
	CHECK_EQ(client.log_level, 3);
	CHECK_EQ(client_read("LogLevel DEBUG4\n", "h"), EINVAL);
	CHECK_EQ(client_read("LogLevel LOUD\n", "h"), EINVAL);
	CHECK_EQ(client_read("LogLevel\n", "h"), EINVAL);

	/* HostName and User: printable words of a bounded length, no token. */
	CHECK_EQ(client_read("HostName real.example.com\n", "h"), 0);
	CHECK(strcmp(client.host_name, "real.example.com") == 0);
	CHECK_EQ(client_read("User alice\n", "h"), 0);
	CHECK(strcmp(client.user, "alice") == 0);
	make_line(line, "HostName", 'h', 255);
	CHECK_EQ(client_read(line, "h"), 0);
	CHECK_EQ(strlen(client.host_name), 255);
	make_line(line, "HostName", 'h', 256);
	CHECK_EQ(client_read(line, "h"), EINVAL);
	make_line(line, "User", 'u', 64);
	CHECK_EQ(client_read(line, "h"), 0);
	CHECK_EQ(strlen(client.user), 64);
	make_line(line, "User", 'u', 65);
	CHECK_EQ(client_read(line, "h"), EINVAL);
	CHECK_EQ(client_read("HostName %h.example.com\n", "h"), EINVAL);
	CHECK_EQ(client_read("User %r\n", "h"), EINVAL);
	CHECK_EQ(client_read("User \"a b\"\n", "h"), EINVAL);
	CHECK_EQ(client_read("User \"\"\n", "h"), EINVAL);
	CHECK_EQ(client_read("User\n", "h"), EINVAL);
	CHECK_EQ(client_read("HostName a b\n", "h"), EINVAL);
	CHECK_EQ(client_read("HostName a\001b\n", "h"), EINVAL);
	CHECK_EQ(client_read("HostName a\177b\n", "h"), EINVAL);
	CHECK_EQ(client_read("HostName a~b\n", "h"), 0);
	CHECK_EQ(client_read("User a\177b\n", "h"), EINVAL);

	/* IdentityFile: paths accumulate in order, a repeat is kept once, eight are the most. */
	CHECK_EQ(client_read("IdentityFile ~/.ssh/id_a\nIdentityFile ~/.ssh/id_b\nIdentityFile ~/.ssh/id_a\n", "h"), 0);
	CHECK_EQ(client.identity_count, 2);
	CHECK(strcmp(client.identity[0], "~/.ssh/id_a") == 0);
	CHECK(strcmp(client.identity[1], "~/.ssh/id_b") == 0);
	text[0] = '\0';
	for (count = 0; count < 8; count++) {
		sprintf(line, "IdentityFile /id%u\n", count);
		strcat(text, line);
	}

	CHECK_EQ(client_read(text, "h"), 0);
	CHECK_EQ(client.identity_count, 8);
	strcat(text, "IdentityFile /id8\n");
	CHECK_EQ(client_read(text, "h"), EINVAL);
	CHECK(error_starts("line 9: "));
	CHECK_EQ(client_read("IdentityFile %d/id\n", "h"), EINVAL);
	CHECK_EQ(client_read("IdentityFile %h/id\n", "h"), EINVAL);
	CHECK_EQ(client_read("IdentityFile none\n", "h"), 0);
	CHECK(strcmp(client.identity[0], "none") == 0);
	CHECK_EQ(client_read("IdentityFile\n", "h"), EINVAL);
	CHECK_EQ(client_read("IdentityFile a b\n", "h"), EINVAL);
	make_line(line, "IdentityFile", 'i', 1023);
	CHECK_EQ(client_read(line, "h"), 0);
	make_line(line, "IdentityFile", 'i', 1024);
	CHECK_EQ(client_read(line, "h"), EINVAL);

	/* UserKnownHostsFile and GlobalKnownHostsFile: a path, /dev/null, none as /dev/null, the first of several. */
	CHECK_EQ(client_read("UserKnownHostsFile /dev/null\n", "h"), 0);
	CHECK(strcmp(client.user_known_hosts, "/dev/null") == 0);
	CHECK_EQ(client_read("UserKnownHostsFile ~/.ssh/kh\n", "h"), 0);
	CHECK(strcmp(client.user_known_hosts, "~/.ssh/kh") == 0);
	CHECK_EQ(client_read("UserKnownHostsFile none\n", "h"), 0);
	CHECK(strcmp(client.user_known_hosts, "/dev/null") == 0);
	CHECK_EQ(client_read("UserKnownHostsFile /one /two\n", "h"), 0);
	CHECK(strcmp(client.user_known_hosts, "/one") == 0);
	CHECK_EQ(client_read("UserKnownHostsFile=/dev/null\n", "h"), 0);
	CHECK_EQ(client_read("UserKnownHostsFile\n", "h"), EINVAL);
	CHECK_EQ(client_read("UserKnownHostsFile %d/kh\n", "h"), EINVAL);
	CHECK_EQ(client_read("GlobalKnownHostsFile /etc/kh\n", "h"), 0);
	CHECK(strcmp(client.global_known_hosts, "/etc/kh") == 0);
	CHECK_EQ(client_read("GlobalKnownHostsFile none\n", "h"), 0);
	CHECK(strcmp(client.global_known_hosts, "/dev/null") == 0);
	CHECK_EQ(client_read("GlobalKnownHostsFile\n", "h"), EINVAL);
}

/* Checks the keywords ssh reads and ignores, Match and unknown keywords. */
static void
test_ssh_ignored(void)
{
	static const char *const accepted[] = {
		"VerifyHostKeyDNS no", "HashKnownHosts no", "SendEnv LANG LC_*", "SendEnv TERM", "ForwardAgent no", "ForwardX11 no",
		"ForwardX11Trusted no", "Compression no", "AddKeysToAgent no", "GSSAPIAuthentication no",
		"KbdInteractiveAuthentication no", "HostbasedAuthentication no", "PermitLocalCommand no", "IdentitiesOnly yes",
		"IdentitiesOnly no", "ControlMaster auto", "ControlMaster yes", "ControlPath ~/.ssh/cm-%r@%h:%p", "ControlPersist 10m",
		"ControlPersist yes", "controlmaster=no", "verifyhostkeydns NO"
	};
	static const char *const refused[] = {
		"VerifyHostKeyDNS yes", "VerifyHostKeyDNS ask", "VerifyHostKeyDNS", "HashKnownHosts yes", "ForwardAgent yes",
		"ForwardAgent /tmp/agent", "ForwardX11 yes", "ForwardX11Trusted yes", "Compression yes", "AddKeysToAgent yes",
		"AddKeysToAgent ask", "AddKeysToAgent confirm", "GSSAPIAuthentication yes", "KbdInteractiveAuthentication yes",
		"HostbasedAuthentication yes", "PermitLocalCommand yes", "IdentitiesOnly maybe", "SendEnv", "ControlMaster",
		"ForwardAgent no no"
	};
	char line[160];
	size_t index;

	/* Each accepted line changes nothing. */
	for (index = 0; index < sizeof(accepted) / sizeof(accepted[0]); index++) {
		sprintf(line, "%s\n", accepted[index]);
		CHECK_EQ(client_read(line, "h"), 0);
		CHECK_EQ(client.set_mask, 0);
		CHECK_EQ(client.identity_count, 0);
		CHECK_EQ(error[0], 0);
	}

	/* A value that would turn on a feature ssh lacks is an error with the line number. */
	for (index = 0; index < sizeof(refused) / sizeof(refused[0]); index++) {
		sprintf(line, "\n%s\n", refused[index]);
		CHECK_EQ(client_read(line, "h"), EINVAL);
		CHECK(error_starts("line 2: "));
	}

	/* Match is refused, also with no words; an unknown keyword is an error with its line. */
	CHECK_EQ(client_read("Match host x\n", "h"), EINVAL);
	CHECK(error_starts("line 1: Match is not supported"));
	CHECK_EQ(client_read("Host *\nMatch all\n", "h"), EINVAL);
	CHECK_EQ(client_read("Match\n", "h"), EINVAL);
	CHECK_EQ(client_read("User a\nProxyJump bastion\n", "h"), EINVAL);
	CHECK(error_starts("line 2: unknown keyword: ProxyJump"));
	CHECK_EQ(client_read("LocalForward 1 a:2\n", "h"), EINVAL);
	CHECK_EQ(client_read("Ciphers aes128-ctr\n", "h"), EINVAL);
	CHECK_EQ(client_read("Include x\n", "h"), EINVAL);
}

/* Checks Host blocks: which hosts they match, the first value, blocks that do not match, errors inside them. */
static void
test_ssh_host(void)
{
	/* A block applies to a host that matches; a later "Host *" gives the rest, and the first value of a field wins. */
	CHECK_EQ(client_read("Host a.example.com\n  Port 2222\n  User ann\nHost *\n  Port 22\n  User bob\n  StrictHostKeyChecking no\n", "a.example.com"), 0);
	CHECK_EQ(client.port, 2222);
	CHECK(strcmp(client.user, "ann") == 0);
	CHECK_EQ(client.strict, SSH_STRICT_NO);
	CHECK_EQ(client_read("Host a.example.com\n  Port 2222\n  User ann\nHost *\n  Port 22\n  User bob\n  StrictHostKeyChecking no\n", "b.example.com"), 0);
	CHECK_EQ(client.port, 22);
	CHECK(strcmp(client.user, "bob") == 0);
	CHECK_EQ(client.strict, SSH_STRICT_NO);

	/* Lines before the first Host line apply to every host. */
	CHECK_EQ(client_read("User first\nHost zzz\n  User other\n", "h"), 0);
	CHECK(strcmp(client.user, "first") == 0);

	/* A pattern list: any of the patterns matches, a negated one that matches rules the block out. */
	CHECK_EQ(client_read("Host a b\n Port 1111\n", "a"), 0);
	CHECK_EQ(client.port, 1111);
	CHECK_EQ(client_read("Host a b\n Port 1111\n", "b"), 0);
	CHECK_EQ(client.port, 1111);
	CHECK_EQ(client_read("Host a b\n Port 1111\n", "c"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host *.example.com !secret.example.com\n Port 3333\n", "x.example.com"), 0);
	CHECK_EQ(client.port, 3333);
	CHECK_EQ(client_read("Host *.example.com !secret.example.com\n Port 3333\n", "secret.example.com"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host *.example.com !secret.example.com\n Port 3333\n", "example.org"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host !secret.example.com *.example.com\n Port 3333\n", "secret.example.com"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host !a\n Port 4444\n", "b"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host !a *\n Port 4444\n", "b"), 0);
	CHECK_EQ(client.port, 4444);
	CHECK_EQ(client_read("Host !a *\n Port 4444\n", "a"), 0);
	CHECK_EQ(client.set_mask, 0);

	/* Patterns: ? and * and the case of the letters (ssh_config(5) PATTERNS), '=' and tabs after Host. */
	CHECK_EQ(client_read("Host ?b\n Port 5555\n", "ab"), 0);
	CHECK_EQ(client.port, 5555);
	CHECK_EQ(client_read("Host ?b\n Port 5555\n", "abb"), 0);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_read("Host A.EXAMPLE.COM\n Port 5555\n", "a.example.com"), 0);
	CHECK_EQ(client.port, 5555);
	CHECK_EQ(client_read("Host=h*\n Port 5556\n", "host"), 0);
	CHECK_EQ(client.port, 5556);
	CHECK_EQ(client_read("Host\tx\ty\t# comment\n Port 5557\n", "y"), 0);
	CHECK_EQ(client.port, 5557);
	CHECK_EQ(client_read("Host \"x\" \"y\"\n Port 5558\n", "y"), 0);
	CHECK_EQ(client.port, 5558);
	CHECK_EQ(client_read("HOST x\r\n Port 5559\r\n", "x"), 0);
	CHECK_EQ(client.port, 5559);

	/* A block that does not match is still read for errors, wherever its Host line is. */
	CHECK_EQ(client_read("Host zzz\n Bogus 1\n", "a"), EINVAL);
	CHECK(error_starts("line 2: unknown keyword"));
	CHECK_EQ(client_read("Host zzz\n Port 99999\n", "a"), EINVAL);
	CHECK_EQ(client_read("Host zzz\n StrictHostKeyChecking maybe\n", "a"), EINVAL);
	CHECK_EQ(client_read("Host zzz\n Match all\n", "a"), EINVAL);
	CHECK_EQ(client_read("Host zzz\n UsePAM yes\n", "a"), EINVAL);

	/* A Host line needs a pattern and a host name has to be given; a missing structure is refused. */
	CHECK_EQ(client_read("Host\n", "a"), EINVAL);
	CHECK(error_starts("line 1: missing value"));
	CHECK_EQ(client_read("Host # none\n", "a"), EINVAL);
	ssh_config_init(&client);
	CHECK_EQ(ssh_config_parse(&client, "Port 5\n", 7, NULL, error), EINVAL);
	CHECK_EQ(ssh_config_parse(NULL, "Port 5\n", 7, "a", error), EINVAL);
	CHECK_EQ(ssh_config_option(NULL, "Port=5", error), EINVAL);
	CHECK_EQ(ssh_config_option(&client, NULL, error), EINVAL);
	CHECK_EQ(ssh_config_parse(&client, "Port 5\n", 7, "a", NULL), 0);
	CHECK_EQ(client.port, 5);

	/* A host name that is empty matches the pattern "*" and no pattern that needs a character. */
	CHECK_EQ(client_read("Host *\n Port 6001\n", ""), 0);
	CHECK_EQ(client.port, 6001);
	CHECK_EQ(client_read("Host x\n Port 6001\n", ""), 0);
	CHECK_EQ(client.set_mask, 0);
}

/* Checks the order of sources: the first value wins, so -o goes in before the files, and the files in the order read. */
static void
test_ssh_order(void)
{
	/* -o first, then a file: the option wins. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port=2200"), 0);
	CHECK_EQ(client_add("Port 22\nUser file\n", "h"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.port, 2200);
	CHECK(strcmp(client.user, "file") == 0);

	/* A file first, then -o: the file wins (the caller applies -o first). */
	ssh_config_init(&client);
	CHECK_EQ(client_add("Port 22\n", "h"), 0);
	CHECK_EQ(client_option("Port=2200"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.port, 22);

	/* The user's file, then the system's: the user's value wins and the system's fills the rest. */
	ssh_config_init(&client);
	CHECK_EQ(client_add("Host h\n StrictHostKeyChecking yes\n", "h"), 0);
	CHECK_EQ(client_add("Host *\n StrictHostKeyChecking no\n UserKnownHostsFile /dev/null\n", "h"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.strict, SSH_STRICT_YES);
	CHECK(strcmp(client.user_known_hosts, "/dev/null") == 0);

	/* The mask: each field a source sets has its own bit, a field is set once. */
	ssh_config_init(&client);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_option("Port=1"), 0);
	CHECK_EQ(bit_count(client.set_mask), 1);
	CHECK_EQ(client_option("Port=2"), 0);
	CHECK_EQ(bit_count(client.set_mask), 1);
	CHECK_EQ(client.port, 1);
	CHECK_EQ(client_option("User=u"), 0);
	CHECK_EQ(bit_count(client.set_mask), 2);
	CHECK_EQ(client_option("BatchMode=yes"), 0);
	CHECK_EQ(bit_count(client.set_mask), 3);

	/* IdentityFile adds across sources, even though other fields are set once; the default is only for no identity. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("IdentityFile=/a"), 0);
	CHECK_EQ(client_add("IdentityFile /b\n", "h"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.identity_count, 2);
	CHECK(strcmp(client.identity[0], "/a") == 0);
	CHECK(strcmp(client.identity[1], "/b") == 0);

	/* A field that a failed line did not complete is not marked as set. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port=0"), EINVAL);
	CHECK_EQ(client.set_mask, 0);
	CHECK_EQ(client_option("Port=7"), 0);
	CHECK_EQ(client.port, 7);

	/* The defaults: everything the header lists, and only for fields no source set. */
	ssh_config_init(&client);
	ssh_config_defaults(&client);
	CHECK_EQ(client.user[0], 0);
	CHECK_EQ(client.host_name[0], 0);
	CHECK_EQ(client.port, 22);
	CHECK_EQ(client.identity_count, 1);
	CHECK(strcmp(client.identity[0], "~/.ssh/id_ed25519") == 0);
	CHECK_EQ(client.strict, SSH_STRICT_ASK);
	CHECK(strcmp(client.user_known_hosts, "~/.ssh/known_hosts") == 0);
	CHECK(strcmp(client.global_known_hosts, "/etc/ssh/ssh_known_hosts") == 0);
	CHECK_EQ(client.batch_mode, 0);
	CHECK_EQ(client.connect_timeout, 0);
	CHECK_EQ(client.server_alive_interval, 0);
	CHECK_EQ(client.server_alive_count, 3);
	CHECK_EQ(client.request_tty, SSH_TTY_AUTO);
	CHECK_EQ(client.password, 1);
	CHECK_EQ(client.pubkey, 1);
	CHECK_EQ(client.log_level, 1);
	CHECK_EQ(client.set_mask, 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.identity_count, 1);
	CHECK_EQ(client.port, 22);

	/* A set value survives the defaults, also a zero and an empty-looking one. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("ServerAliveCountMax=0"), 0);
	CHECK_EQ(client_option("PasswordAuthentication=no"), 0);
	CHECK_EQ(client_option("LogLevel=QUIET"), 0);
	CHECK_EQ(client_option("ConnectTimeout=0"), 0);
	CHECK_EQ(client_option("IdentityFile=/only"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.server_alive_count, 0);
	CHECK_EQ(client.password, 0);
	CHECK_EQ(client.log_level, 0);
	CHECK_EQ(client.identity_count, 1);
	CHECK(strcmp(client.identity[0], "/only") == 0);

	/* Each field set alone, to a value that is not its default, survives the defaults. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("User=zed"), 0);
	ssh_config_defaults(&client);
	CHECK(strcmp(client.user, "zed") == 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("HostName=real"), 0);
	ssh_config_defaults(&client);
	CHECK(strcmp(client.host_name, "real") == 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port=99"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.port, 99);
	ssh_config_init(&client);
	CHECK_EQ(client_option("StrictHostKeyChecking=accept-new"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.strict, SSH_STRICT_ACCEPT_NEW);
	ssh_config_init(&client);
	CHECK_EQ(client_option("UserKnownHostsFile=/uk"), 0);
	ssh_config_defaults(&client);
	CHECK(strcmp(client.user_known_hosts, "/uk") == 0);
	CHECK(strcmp(client.global_known_hosts, "/etc/ssh/ssh_known_hosts") == 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("GlobalKnownHostsFile=/gk"), 0);
	ssh_config_defaults(&client);
	CHECK(strcmp(client.global_known_hosts, "/gk") == 0);
	CHECK(strcmp(client.user_known_hosts, "~/.ssh/known_hosts") == 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("BatchMode=yes"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.batch_mode, 1);
	ssh_config_init(&client);
	CHECK_EQ(client_option("ConnectTimeout=5"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.connect_timeout, 5);
	CHECK_EQ(client.server_alive_interval, 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("ServerAliveInterval=7"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.server_alive_interval, 7);
	CHECK_EQ(client.connect_timeout, 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("ServerAliveCountMax=9"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.server_alive_count, 9);
	CHECK_EQ(client.server_alive_interval, 0);
	ssh_config_init(&client);
	CHECK_EQ(client_option("RequestTTY=force"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.request_tty, SSH_TTY_FORCE);
	ssh_config_init(&client);
	CHECK_EQ(client_option("PasswordAuthentication=no"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.password, 0);
	CHECK_EQ(client.pubkey, 1);
	ssh_config_init(&client);
	CHECK_EQ(client_option("PubkeyAuthentication=no"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.pubkey, 0);
	CHECK_EQ(client.password, 1);
	ssh_config_init(&client);
	CHECK_EQ(client_option("LogLevel=DEBUG"), 0);
	ssh_config_defaults(&client);
	CHECK_EQ(client.log_level, 3);
	CHECK_EQ(client.request_tty, SSH_TTY_AUTO);

	/* A string written over a field that held other bytes ends where the value ends. */
	ssh_config_init(&client);
	memset(client.user, 'x', sizeof(client.user));
	memset(client.host_name, 'x', sizeof(client.host_name));
	CHECK_EQ(client_option("User=ab"), 0);
	CHECK_EQ(client_option("HostName=cd"), 0);
	CHECK(strcmp(client.user, "ab") == 0);
	CHECK(strcmp(client.host_name, "cd") == 0);
	sshd_config_defaults(&sshd);
	memset(sshd.listen_address, 'x', sizeof(sshd.listen_address));
	CHECK_EQ(sshd_config_parse(&sshd, "ListenAddress ab\n", 17, error), 0);
	CHECK(strcmp(sshd.listen_address, "ab") == 0);

	/* Null arguments to the other two do not crash. */
	ssh_config_init(NULL);
	ssh_config_defaults(NULL);
}

/* Checks -o options: both forms, spaces, errors without a line number, Host refused. */
static void
test_ssh_option(void)
{
	char option[9000];

	/* "Keyword=value" and "Keyword value", with spaces around the '=' and in quotes. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port=2222"), 0);
	CHECK_EQ(client.port, 2222);
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port 2223"), 0);
	CHECK_EQ(client.port, 2223);
	ssh_config_init(&client);
	CHECK_EQ(client_option("  Port = 2224  "), 0);
	CHECK_EQ(client.port, 2224);
	ssh_config_init(&client);
	CHECK_EQ(client_option("StrictHostKeyChecking=no"), 0);
	CHECK_EQ(client.strict, SSH_STRICT_NO);
	CHECK_EQ(client_option("UserKnownHostsFile=/dev/null"), 0);
	CHECK(strcmp(client.user_known_hosts, "/dev/null") == 0);
	CHECK_EQ(client_option("IdentityFile=\"/a b\""), 0);
	CHECK(strcmp(client.identity[0], "/a b") == 0);
	CHECK_EQ(client_option("port=1"), 0);
	CHECK_EQ(client_option("VerifyHostKeyDNS=no"), 0);

	/* An error has no line number but names what was wrong and the keyword. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("Port"), EINVAL);
	CHECK(!error_starts("line"));
	CHECK(error_has("missing value"));
	CHECK(error_has("Port"));
	CHECK_EQ(client_option("Port=0"), EINVAL);
	CHECK(error_has("out of range"));
	CHECK_EQ(client_option("Bogus=1"), EINVAL);
	CHECK(error_starts("unknown keyword: Bogus"));
	CHECK_EQ(client_option("Match all"), EINVAL);
	CHECK(error_has("Match is not supported"));
	CHECK_EQ(client_option(""), EINVAL);
	CHECK_EQ(client_option("   "), EINVAL);
	CHECK_EQ(client_option("#Port=1"), EINVAL);
	CHECK_EQ(client_option("=1"), EINVAL);
	CHECK(error_has("missing keyword"));
	CHECK(!error_starts("line"));
	CHECK_EQ(client_option("Port=\"1"), EINVAL);
	CHECK(!error_starts("line"));
	CHECK_EQ(client_option("Port=\"1"), EINVAL);
	CHECK_EQ(client_option("Port=1\nUser=x"), EINVAL);
	CHECK(error_has("line break"));
	CHECK_EQ(client_option("Port=1\r"), EINVAL);
	CHECK_EQ(client.set_mask, 0);

	/* Host cannot start a block in an option. */
	CHECK_EQ(client_option("Host=x"), EINVAL);
	CHECK(error_has("Host cannot be given as an option"));
	CHECK_EQ(client_option("Host x"), EINVAL);

	/* An option is at most one line long. */
	strcpy(option, "SendEnv ");
	memset(option + 8, 'E', 8192U - 8U);
	option[8192] = '\0';
	CHECK_EQ(client_option(option), 0);
	option[8192] = 'E';
	option[8193] = '\0';
	CHECK_EQ(client_option(option), EINVAL);
	CHECK(error_has("too long"));

	/* A message is optional. */
	CHECK_EQ(ssh_config_option(&client, "Bogus=1", NULL), EINVAL);
	CHECK_EQ(ssh_config_option(&client, "Port=3", NULL), 0);
	CHECK_EQ(client.port, 3);
}

/*
 * Every keyword of sshd_config once with a value different from its
 * default, and then each again with another value: the first line of each
 * keyword is the one used, so no keyword's "already given" mark may be
 * shared with another keyword.
 */
static void
test_sshd_all(void)
{
	static const char first[] =
		"Port 2022\nListenAddress 10.1.2.3\nAddressFamily inet6\nHostKey /h1\nPermitRootLogin no\n"
		"PasswordAuthentication no\nPermitEmptyPasswords yes\nPubkeyAuthentication no\nAuthorizedKeysFile /ak\n"
		"MaxAuthTries 3\nLoginGraceTime 45\nMaxSessions 4\nMaxStartups 7:30:70\nStrictModes no\n"
		"Subsystem sftp /sftp\nClientAliveInterval 15\nClientAliveCountMax 5\n";
	static const char second[] =
		"Port 2022\nListenAddress 10.1.2.3\nAddressFamily inet\nHostKey /h2\nPermitRootLogin yes\n"
		"PasswordAuthentication yes\nPermitEmptyPasswords no\nPubkeyAuthentication yes\nAuthorizedKeysFile /ak2\n"
		"MaxAuthTries 9\nLoginGraceTime 99\nMaxSessions 8\nMaxStartups 9:30:90\nStrictModes yes\n"
		"Subsystem sftp /sftp2\nClientAliveInterval 25\nClientAliveCountMax 6\n";
	char text[1024];

	/* The first group is read in full. */
	CHECK_EQ(sshd_read(first), 0);
	CHECK_EQ(sshd.port, 2022);
	CHECK(strcmp(sshd.listen_address, "10.1.2.3") == 0);
	CHECK_EQ(sshd.inet6, 1);
	CHECK_EQ(sshd.host_key_count, 1);
	CHECK(strcmp(sshd.host_key[0], "/h1") == 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_NO);
	CHECK_EQ(sshd.password, 0);
	CHECK_EQ(sshd.permit_empty, 1);
	CHECK_EQ(sshd.pubkey, 0);
	CHECK(strcmp(sshd.authorized_keys_file, "/ak") == 0);
	CHECK_EQ(sshd.max_auth_tries, 3);
	CHECK_EQ(sshd.login_grace_time, 45);
	CHECK_EQ(sshd.max_sessions, 4);
	CHECK_EQ(sshd.max_startups, 7);
	CHECK_EQ(sshd.strict_modes, 0);
	CHECK(strcmp(sshd.sftp_command, "/sftp") == 0);
	CHECK_EQ(sshd.client_alive_interval, 15);
	CHECK_EQ(sshd.client_alive_count, 5);

	/* The second group after it changes nothing but adds its HostKey. */
	strcpy(text, first);
	strcat(text, second);
	CHECK_EQ(sshd_read(text), 0);
	CHECK_EQ(sshd.port, 2022);
	CHECK(strcmp(sshd.listen_address, "10.1.2.3") == 0);
	CHECK_EQ(sshd.inet6, 1);
	CHECK_EQ(sshd.host_key_count, 2);
	CHECK(strcmp(sshd.host_key[1], "/h2") == 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_NO);
	CHECK_EQ(sshd.password, 0);
	CHECK_EQ(sshd.permit_empty, 1);
	CHECK_EQ(sshd.pubkey, 0);
	CHECK(strcmp(sshd.authorized_keys_file, "/ak") == 0);
	CHECK_EQ(sshd.max_auth_tries, 3);
	CHECK_EQ(sshd.login_grace_time, 45);
	CHECK_EQ(sshd.max_sessions, 4);
	CHECK_EQ(sshd.max_startups, 7);
	CHECK_EQ(sshd.strict_modes, 0);
	CHECK(strcmp(sshd.sftp_command, "/sftp") == 0);
	CHECK_EQ(sshd.client_alive_interval, 15);
	CHECK_EQ(sshd.client_alive_count, 5);

	/* The lines in the other order: now the second group's values are the ones used. */
	strcpy(text, second);
	strcat(text, first);
	CHECK_EQ(sshd_read(text), 0);
	CHECK_EQ(sshd.inet6, 0);
	CHECK_EQ(sshd.permit_root, SSHD_PERMIT_ROOT_YES);
	CHECK_EQ(sshd.password, 1);
	CHECK_EQ(sshd.permit_empty, 0);
	CHECK_EQ(sshd.pubkey, 1);
	CHECK(strcmp(sshd.authorized_keys_file, "/ak2") == 0);
	CHECK_EQ(sshd.max_auth_tries, 9);
	CHECK_EQ(sshd.login_grace_time, 99);
	CHECK_EQ(sshd.max_sessions, 8);
	CHECK_EQ(sshd.max_startups, 9);
	CHECK_EQ(sshd.strict_modes, 1);
	CHECK(strcmp(sshd.sftp_command, "/sftp2") == 0);
	CHECK_EQ(sshd.client_alive_interval, 25);
	CHECK_EQ(sshd.client_alive_count, 6);
}

/*
 * Every acted-on keyword of ssh_config once with a value different from
 * its default, then each again with another value, as for sshd: each field
 * has its own bit in set_mask, and the two files are two sources.
 */
static void
test_ssh_all(void)
{
	static const char first[] =
		"User u1\nHostName hn1\nPort 2201\nIdentityFile /i1\nStrictHostKeyChecking yes\nUserKnownHostsFile /uk\n"
		"GlobalKnownHostsFile /gk\nBatchMode yes\nConnectTimeout 30\nServerAliveInterval 20\nServerAliveCountMax 5\n"
		"RequestTTY force\nPasswordAuthentication no\nPubkeyAuthentication no\nLogLevel VERBOSE\n";
	static const char second[] =
		"User u2\nHostName hn2\nPort 2202\nIdentityFile /i2\nStrictHostKeyChecking no\nUserKnownHostsFile /uk2\n"
		"GlobalKnownHostsFile /gk2\nBatchMode no\nConnectTimeout 31\nServerAliveInterval 21\nServerAliveCountMax 6\n"
		"RequestTTY no\nPasswordAuthentication yes\nPubkeyAuthentication yes\nLogLevel DEBUG\n";

	/* The first file sets every field, and each field has a bit of its own. */
	ssh_config_init(&client);
	CHECK_EQ(client_add(first, "h"), 0);
	CHECK_EQ(bit_count(client.set_mask), 15);
	CHECK(strcmp(client.user, "u1") == 0);
	CHECK(strcmp(client.host_name, "hn1") == 0);
	CHECK_EQ(client.port, 2201);
	CHECK_EQ(client.identity_count, 1);
	CHECK(strcmp(client.identity[0], "/i1") == 0);
	CHECK_EQ(client.strict, SSH_STRICT_YES);
	CHECK(strcmp(client.user_known_hosts, "/uk") == 0);
	CHECK(strcmp(client.global_known_hosts, "/gk") == 0);
	CHECK_EQ(client.batch_mode, 1);
	CHECK_EQ(client.connect_timeout, 30);
	CHECK_EQ(client.server_alive_interval, 20);
	CHECK_EQ(client.server_alive_count, 5);
	CHECK_EQ(client.request_tty, SSH_TTY_FORCE);
	CHECK_EQ(client.password, 0);
	CHECK_EQ(client.pubkey, 0);
	CHECK_EQ(client.log_level, 2);

	/* A second file after it changes nothing but adds an identity. */
	CHECK_EQ(client_add(second, "h"), 0);
	CHECK_EQ(bit_count(client.set_mask), 15);
	CHECK(strcmp(client.user, "u1") == 0);
	CHECK(strcmp(client.host_name, "hn1") == 0);
	CHECK_EQ(client.port, 2201);
	CHECK_EQ(client.identity_count, 2);
	CHECK(strcmp(client.identity[1], "/i2") == 0);
	CHECK_EQ(client.strict, SSH_STRICT_YES);
	CHECK(strcmp(client.user_known_hosts, "/uk") == 0);
	CHECK(strcmp(client.global_known_hosts, "/gk") == 0);
	CHECK_EQ(client.batch_mode, 1);
	CHECK_EQ(client.connect_timeout, 30);
	CHECK_EQ(client.server_alive_interval, 20);
	CHECK_EQ(client.server_alive_count, 5);
	CHECK_EQ(client.request_tty, SSH_TTY_FORCE);
	CHECK_EQ(client.password, 0);
	CHECK_EQ(client.pubkey, 0);
	CHECK_EQ(client.log_level, 2);

	/* The defaults leave all of it alone. */
	ssh_config_defaults(&client);
	CHECK_EQ(client.port, 2201);
	CHECK_EQ(client.identity_count, 2);
	CHECK_EQ(client.strict, SSH_STRICT_YES);
	CHECK_EQ(client.server_alive_count, 5);
	CHECK_EQ(client.log_level, 2);

	/* The second file read first: its values are the ones used. */
	ssh_config_init(&client);
	CHECK_EQ(client_add(second, "h"), 0);
	CHECK_EQ(client_add(first, "h"), 0);
	CHECK(strcmp(client.user, "u2") == 0);
	CHECK(strcmp(client.host_name, "hn2") == 0);
	CHECK_EQ(client.port, 2202);
	CHECK_EQ(client.strict, SSH_STRICT_NO);
	CHECK(strcmp(client.user_known_hosts, "/uk2") == 0);
	CHECK(strcmp(client.global_known_hosts, "/gk2") == 0);
	CHECK_EQ(client.batch_mode, 0);
	CHECK_EQ(client.connect_timeout, 31);
	CHECK_EQ(client.server_alive_interval, 21);
	CHECK_EQ(client.server_alive_count, 6);
	CHECK_EQ(client.request_tty, SSH_TTY_NO);
	CHECK_EQ(client.password, 1);
	CHECK_EQ(client.pubkey, 1);
	CHECK_EQ(client.log_level, 3);

	/* The same fields set from -o options, one at a time, give the same bits. */
	ssh_config_init(&client);
	CHECK_EQ(client_option("User=u1"), 0);
	CHECK_EQ(client_option("HostName=hn1"), 0);
	CHECK_EQ(client_option("Port=2201"), 0);
	CHECK_EQ(client_option("IdentityFile=/i1"), 0);
	CHECK_EQ(client_option("StrictHostKeyChecking=yes"), 0);
	CHECK_EQ(client_option("UserKnownHostsFile=/uk"), 0);
	CHECK_EQ(client_option("GlobalKnownHostsFile=/gk"), 0);
	CHECK_EQ(client_option("BatchMode=yes"), 0);
	CHECK_EQ(client_option("ConnectTimeout=30"), 0);
	CHECK_EQ(client_option("ServerAliveInterval=20"), 0);
	CHECK_EQ(client_option("ServerAliveCountMax=5"), 0);
	CHECK_EQ(client_option("RequestTTY=force"), 0);
	CHECK_EQ(client_option("PasswordAuthentication=no"), 0);
	CHECK_EQ(client_option("PubkeyAuthentication=no"), 0);
	CHECK_EQ(client_option("LogLevel=VERBOSE"), 0);
	CHECK_EQ(bit_count(client.set_mask), 15);
}

/* Words a random line is made of: keywords of both files, values of every kind and punctuation. */
static const char *const fuzz_words[] = {
	"Port", "ListenAddress", "AddressFamily", "HostKey", "PermitRootLogin", "PasswordAuthentication", "PermitEmptyPasswords",
	"PubkeyAuthentication", "AuthorizedKeysFile", "MaxAuthTries", "LoginGraceTime", "MaxSessions", "MaxStartups",
	"StrictModes", "Subsystem", "ClientAliveInterval", "ClientAliveCountMax", "Host", "HostName", "User", "IdentityFile",
	"StrictHostKeyChecking", "UserKnownHostsFile", "GlobalKnownHostsFile", "BatchMode", "ConnectTimeout",
	"ServerAliveInterval", "ServerAliveCountMax", "RequestTTY", "LogLevel", "Match", "UseDNS", "AcceptEnv", "Banner",
	"yes", "no", "none", "sftp", "any", "inet", "inet6", "force", "auto", "ask", "accept-new", "off", "INFO", "DEBUG3",
	"22", "0", "65535", "65536", "10:30:100", "2m", "1h30m", "999999999", "4294967296", "-1", "%h", "%u", "%%", "%x",
	"/etc/ssh/key", "/dev/null", "~/.ssh/id", "*", "?", "!a", "a.example.com", "*.example.com", "\"", "\"a b\"", "#", "=",
	" ", "\t", "\r", "::1", "[::1]:22", "1.2.3.4:22", "x", "\001", "\377"
};

/* The host names the random ssh_config files are read for. */
static const char *const fuzz_hosts[3] = { "a.example.com", "x", "" };

/* The number of words of fuzz_words. */
#define FUZZ_WORD_COUNT		(sizeof(fuzz_words) / sizeof(fuzz_words[0]))

/* The next value of a small pseudo-random sequence (a linear congruential generator, fixed seed for repeatable runs). */
static unsigned long fuzz_state = 12345UL;

/* Returns the next pseudo-random number below limit. */
static unsigned
fuzz_next(
	unsigned limit)
{
	/* A 32-bit linear congruential step; only the high bits are used. */
	fuzz_state = (fuzz_state * 1103515245UL + 12345UL) & 0xffffffffUL;
	return (unsigned)((fuzz_state >> 8) % limit);
}

/* Builds a random file of a few lines from the words, with random bytes now and then, and returns its size. */
static size_t
fuzz_text(
	char *text,
	size_t capacity)
{
	size_t length;
	size_t word_length;
	unsigned lines;
	unsigned words;
	unsigned line;
	unsigned word;
	unsigned pick;

	/* Builds one to six lines of one to five words. */
	length = 0;
	lines = 1 + fuzz_next(6);
	for (line = 0; line < lines; line++) {
		words = 1 + fuzz_next(5);
		for (word = 0; word < words; word++) {
			pick = fuzz_next(100);
			if (pick < 6) {
				/* A random byte, any value except that a NUL is also possible. */
				if (length + 2 >= capacity)
					break;
				text[length] = (char)fuzz_next(256);
				length++;
				continue;
			}

			pick = fuzz_next((unsigned)FUZZ_WORD_COUNT);
			word_length = strlen(fuzz_words[pick]);
			if (length + word_length + 3 >= capacity)
				break;
			memcpy(text + length, fuzz_words[pick], word_length);
			length += word_length;
			if (fuzz_next(4) != 0) {
				text[length] = ' ';
				length++;
			}
		}

		if (length + 2 < capacity) {
			text[length] = '\n';
			length++;
		}
	}

	return length;
}

/* Checks that a sshd configuration holds only values the parser allows. */
static void
check_sshd_invariants(void)
{
	unsigned index;

	/* The counts and the strings are inside their arrays. */
	CHECK(sshd.host_key_count <= SSH_CONFIG_KEYS_MAX);
	for (index = 0; index < sshd.host_key_count; index++) {
		CHECK(strlen(sshd.host_key[index]) > 0);
		CHECK(strlen(sshd.host_key[index]) < SSH_CONFIG_PATH_MAX);
	}

	CHECK(strlen(sshd.listen_address) < sizeof(sshd.listen_address));
	CHECK(strlen(sshd.authorized_keys_file) < SSH_CONFIG_PATH_MAX);
	CHECK(strlen(sshd.sftp_command) < SSH_CONFIG_PATH_MAX);

	/* The numbers are in the ranges of their keywords. */
	CHECK(sshd.port >= 1);
	CHECK(sshd.max_auth_tries >= 1 && sshd.max_auth_tries <= 1000);
	CHECK(sshd.max_sessions <= SSH_CHANNEL_MAX);
	CHECK(sshd.max_startups >= 1);
	CHECK(sshd.login_grace_time <= 2147483647U);
	CHECK(sshd.permit_root == SSHD_PERMIT_ROOT_NO || sshd.permit_root == SSHD_PERMIT_ROOT_YES ||
	    sshd.permit_root == SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD);
	CHECK(sshd.inet6 == 0 || sshd.inet6 == 1);
}

/* Checks that a client configuration holds only values the parser allows. */
static void
check_client_invariants(void)
{
	unsigned index;

	/* The counts and the strings are inside their arrays. */
	CHECK(client.identity_count <= SSH_CONFIG_KEYS_MAX);
	for (index = 0; index < client.identity_count; index++) {
		CHECK(strlen(client.identity[index]) > 0);
		CHECK(strlen(client.identity[index]) < SSH_CONFIG_PATH_MAX);
	}

	CHECK(strlen(client.user) <= SSH_USER_MAX);
	CHECK(strlen(client.host_name) < sizeof(client.host_name));
	CHECK(strlen(client.user_known_hosts) < SSH_CONFIG_PATH_MAX);
	CHECK(strlen(client.global_known_hosts) < SSH_CONFIG_PATH_MAX);

	/* The enumerations and the log level are in range. */
	CHECK(client.strict >= SSH_STRICT_ASK && client.strict <= SSH_STRICT_ACCEPT_NEW);
	CHECK(client.request_tty >= SSH_TTY_AUTO && client.request_tty <= SSH_TTY_FORCE);
	CHECK(client.log_level >= 0 && client.log_level <= 3);
}

/* Reads random files with both parsers and checks that they end properly and keep their values valid. */
static void
test_fuzz(void)
{
	static char text[4096];
	char *copy;
	unsigned long round;
	size_t size;
	int status;

	/* Random sshd files. */
	for (round = 0; round < TEST_FUZZ_COUNT; round++) {
		size = fuzz_text(text, sizeof(text));
		sshd_config_defaults(&sshd);
		memset(error, 'x', sizeof(error) - 1);
		error[sizeof(error) - 1] = '\0';
		copy = malloc(size != 0 ? size : 1);
		CHECK(copy != NULL);
		memcpy(copy, text, size);
		status = sshd_config_parse(&sshd, copy, size, error);
		free(copy);
		CHECK(status == 0 || status == EINVAL);
		if (status != 0) {
			CHECK(strlen(error) > 0);
			CHECK(strlen(error) < SSH_CONFIG_ERROR_MAX);
		}

		check_sshd_invariants();
		CHECK_EQ(sshd_config_finish(&sshd), 0);
		CHECK(sshd.host_key_count >= 1);
	}

	/* Random ssh_config files, for a few host names, then the defaults. */
	for (round = 0; round < TEST_FUZZ_COUNT; round++) {
		size = fuzz_text(text, sizeof(text));
		ssh_config_init(&client);
		memset(error, 'x', sizeof(error) - 1);
		error[sizeof(error) - 1] = '\0';
		copy = malloc(size != 0 ? size : 1);
		CHECK(copy != NULL);
		memcpy(copy, text, size);
		status = ssh_config_parse(&client, copy, size, fuzz_hosts[round % 3], error);
		free(copy);
		CHECK(status == 0 || status == EINVAL);
		if (status != 0) {
			CHECK(strlen(error) > 0);
			CHECK(strlen(error) < SSH_CONFIG_ERROR_MAX);
		}

		ssh_config_defaults(&client);
		check_client_invariants();
		CHECK(client.port >= 1);
	}

	/* Random options. */
	for (round = 0; round < TEST_FUZZ_COUNT; round++) {
		size = fuzz_text(text, sizeof(text));
		text[size] = '\0';
		ssh_config_init(&client);
		copy = malloc(strlen(text) + 1);
		CHECK(copy != NULL);
		strcpy(copy, text);
		status = ssh_config_option(&client, copy, error);
		free(copy);
		CHECK(status == 0 || status == EINVAL);
		ssh_config_defaults(&client);
		check_client_invariants();
	}
}

/* Runs every part of the test. */
int
main(void)
{
	/* The defaults and the files of the image today. */
	test_sshd_defaults();
	test_shipped();

	/* The yes/no, number and time keywords of sshd. */
	check_sshd_bool("PasswordAuthentication", offsetof(struct sshd_config, password));
	check_sshd_bool("PermitEmptyPasswords", offsetof(struct sshd_config, permit_empty));
	check_sshd_bool("PubkeyAuthentication", offsetof(struct sshd_config, pubkey));
	check_sshd_bool("StrictModes", offsetof(struct sshd_config, strict_modes));
	check_sshd_number("MaxAuthTries", offsetof(struct sshd_config, max_auth_tries), 1, 1000);
	check_sshd_number("ClientAliveCountMax", offsetof(struct sshd_config, client_alive_count), 0, 1000000);
	check_sshd_time("LoginGraceTime", offsetof(struct sshd_config, login_grace_time));
	check_sshd_time("ClientAliveInterval", offsetof(struct sshd_config, client_alive_interval));

	/* The other keywords of sshd, the ignored ones and the shape of lines. */
	test_sshd_listen();
	test_sshd_values();
	test_sshd_ignored();
	test_syntax();
	test_sshd_all();

	/* The client's keywords, Host blocks, the order of sources, options. */
	test_ssh_values();
	test_ssh_ignored();
	test_ssh_host();
	test_ssh_order();
	test_ssh_option();
	test_ssh_all();

	/* Random input. */
	test_fuzz();

	/* The result. */
	return check_finish("config");
}
