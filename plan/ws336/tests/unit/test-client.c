/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of the ssh program's own parts (ssh-client-*.c): the command
 * line (ssh(1) SYNOPSIS: options before the destination, "[user@]host",
 * the command), the settings it gives, the host key decision
 * (ssh_config(5) StrictHostKeyChecking and BatchMode, with the rule that a
 * changed key is refused), the host key check against known_hosts files
 * written here (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT) and the rule that a
 * re-exchange must show the first key, the escapes (ssh(1) ESCAPE
 * CHARACTERS), "~/" paths, the reason found in the unencrypted output
 * (RFC 4253 6 and 11.1), and the order of the backlog.  The keys are the
 * public keys of RFC 8032 section 7.1 (TEST 1 and TEST 2); blobs are
 * built with wire.h (RFC 8709 4) and encoded by a base64 encoder written
 * here.  The expected values come from those documents and from hand.
 *
 * run-unit-sources: userland/base/ssh/ssh-client-options.c userland/base/ssh/ssh-client-file.c userland/base/ssh/ssh-client-net.c userland/base/ssh/ssh-client-hostkey.c userland/base/ssh/ssh-client-auth.c userland/base/ssh/ssh-client-session.c
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "check.h"
#include "wire.h"
#include "ssh-client-common.h"

/* The test public keys: RFC 8032 section 7.1, TEST 1 and TEST 2. */
static const char t1_public_hex[] =
	"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char t2_public_hex[] =
	"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";

/* The key bytes and their blobs (RFC 8709 4) and base64. */
static uint8_t key1[SSH_ED25519_PUBLIC];
static uint8_t key2[SSH_ED25519_PUBLIC];
static struct wire blob1;
static struct wire blob2;
static char base1[128];
static char base2[128];

/* The directory of the test's files (a new one under the unit-test output). */
static char directory[512];

/* A connection used by the host key and reason checks (large for the stack). */
static struct ssh_client client;

/* Encodes size bytes as base64 with '=' padding (RFC 4648 4, Table 1) into out, apart from the module. */
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

	position = 0;
	length = 0;
	while (size - position >= 3U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8) | in[position + 2U];
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		out[length++] = alphabet[group & 63U];
		position += 3U;
	}
	if (size - position == 1U) {
		group = (uint32_t)in[position] << 16;
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = '=';
		out[length++] = '=';
	} else if (size - position == 2U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8);
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		out[length++] = '=';
	}
	out[length] = '\0';
}

/* Writes text to the file at path (replacing it). */
static void
test_write_file(
	const char *path,
	const char *text)
{
	FILE *file;

	file = fopen(path, "wb");
	CHECK(file != NULL);
	if (file == NULL)
		return;
	fputs(text, file);
	fclose(file);
}

/* Reads the file at path into out (capacity bytes with the NUL); an absent file reads as "<absent>". */
static void
test_read_file(
	const char *path,
	char *out,
	size_t capacity)
{
	FILE *file;
	size_t size;

	file = fopen(path, "rb");
	if (file == NULL) {
		snprintf(out, capacity, "<absent>");
		return;
	}
	size = fread(out, 1, capacity - 1U, file);
	out[size] = '\0';
	fclose(file);
}

/* The descriptor the standard error stream had before a capture, and the capture file. */
static int saved_stderr = -1;
static char capture_path[600];

/* Sends the standard error stream into a new file. */
static void
test_capture_start(void)
{
	int descriptor;

	fflush(stderr);
	snprintf(capture_path, sizeof(capture_path), "%s/stderr", directory);
	descriptor = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(descriptor >= 0);
	saved_stderr = dup(STDERR_FILENO);
	dup2(descriptor, STDERR_FILENO);
	close(descriptor);
}

/* Puts the standard error stream back and reads what was captured. */
static void
test_capture_end(
	char *out,
	size_t capacity)
{
	fflush(stderr);
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	test_read_file(capture_path, out, capacity);
}

/* Builds argv from a NUL-separated list of words ending with an empty word, into words. */
static int
test_argv(
	char **argv,
	char *storage,
	const char *const *words)
{
	int count;
	size_t used;
	size_t length;

	count = 0;
	used = 0;
	while (words[count] != NULL) {
		length = strlen(words[count]);
		memcpy(storage + used, words[count], length + 1U);
		argv[count] = storage + used;
		used += length + 1U;
		count++;
	}
	argv[count] = NULL;
	return count;
}

/* Parses the words as a command line and returns the error. */
static int
test_parse(
	const char *const *words,
	struct ssh_client_args *args)
{
	static char storage[8192];
	static char *argv[128];
	char error[256];
	int argc;

	argc = test_argv(argv, storage, words);
	return ssh_client_parse_args(argc, argv, args, error, sizeof(error));
}

/* The command-line checks. */
static void
test_args(void)
{
	static const char *const basic[] = { "ssh", "host", NULL };
	static const char *const joined[] = { "ssh", "-p2222", "-lbob", "-ifile", "-oUser=x", "user@host", "ls", "-l", "/", NULL };
	static const char *const separate[] = { "ssh", "-p", "2222", "-l", "bob", "-F", "conf", "host", NULL };
	static const char *const flags[] = { "ssh", "-ttvvqnN", "host", NULL };
	static const char *const tee[] = { "ssh", "-T", "-t", "host", NULL };
	static const char *const dashdash[] = { "ssh", "-v", "--", "host", "-p", "1", NULL };
	static const char *const dashhost[] = { "ssh", "--", "-host", NULL };
	static const char *const unknown[] = { "ssh", "-X", "host", NULL };
	static const char *const missing_value[] = { "ssh", "host", "-p", NULL };
	static const char *const trailing_option[] = { "ssh", "-p", NULL };
	static const char *const no_destination[] = { "ssh", "-v", NULL };
	static const char *const empty_user[] = { "ssh", "@host", NULL };
	static const char *const empty_host[] = { "ssh", "user@", NULL };
	static const char *const last_at[] = { "ssh", "a@b@host", NULL };
	static const char *const n_with_command[] = { "ssh", "-N", "host", "ls", NULL };
	static const char *const dash_alone[] = { "ssh", "-", NULL };
	static const char *const dash_then_host[] = { "ssh", "-", "host", NULL };
	static const char *const p_last[] = { "ssh", "-v", "-p", NULL };
	static char storage[8192];
	static char *argv[16];
	char error[256];
	char *long_word;
	int argc;
	struct ssh_client_args args;
	char long_host[SSH_HOST_MAX + 2];
	char long_user[SSH_USER_MAX + 2 + 6];
	const char *words[SSH_CLIENT_SETTINGS_MAX + 4];
	struct ssh_buf command;
	int index;

	/* The destination alone. */
	CHECK_EQ(test_parse(basic, &args), 0);
	CHECK(strcmp(args.host, "host") == 0);
	CHECK(args.user[0] == '\0');
	CHECK_EQ(args.command_count, 0);
	CHECK_EQ(args.setting_count, 0);
	CHECK(args.config_file == NULL);

	/* Values in the same argument, in order, and the options after the destination belong to the command. */
	CHECK_EQ(test_parse(joined, &args), 0);
	CHECK_EQ(args.setting_count, 4);
	CHECK_EQ(args.settings[0].kind, SSH_CLIENT_SETTING_PORT);
	CHECK(strcmp(args.settings[0].value, "2222") == 0);
	CHECK_EQ(args.settings[1].kind, SSH_CLIENT_SETTING_USER);
	CHECK(strcmp(args.settings[1].value, "bob") == 0);
	CHECK_EQ(args.settings[2].kind, SSH_CLIENT_SETTING_IDENTITY);
	CHECK(strcmp(args.settings[2].value, "file") == 0);
	CHECK_EQ(args.settings[3].kind, SSH_CLIENT_SETTING_OPTION);
	CHECK(strcmp(args.settings[3].value, "User=x") == 0);
	CHECK(strcmp(args.user, "user") == 0);
	CHECK(strcmp(args.host, "host") == 0);
	CHECK_EQ(args.command_count, 3);
	CHECK(strcmp(args.command[1], "-l") == 0);

	/* Values in the next argument, and -F. */
	CHECK_EQ(test_parse(separate, &args), 0);
	CHECK_EQ(args.setting_count, 2);
	CHECK(strcmp(args.settings[0].value, "2222") == 0);
	CHECK(strcmp(args.settings[1].value, "bob") == 0);
	CHECK(args.config_file != NULL && strcmp(args.config_file, "conf") == 0);
	CHECK(strcmp(args.host, "host") == 0);

	/* Flags put together and counted. */
	CHECK_EQ(test_parse(flags, &args), 0);
	CHECK_EQ(args.tty_count, 2);
	CHECK_EQ(args.verbose, 2);
	CHECK_EQ(args.quiet, 1);
	CHECK_EQ(args.null_stdin, 1);
	CHECK_EQ(args.no_command, 1);
	CHECK_EQ(args.no_tty, 0);
	CHECK_EQ(test_parse(tee, &args), 0);
	CHECK_EQ(args.no_tty, 1);
	CHECK_EQ(args.tty_count, 1);

	/* "--" ends the options; a host after it may still not look like an option. */
	CHECK_EQ(test_parse(dashdash, &args), 0);
	CHECK(strcmp(args.host, "host") == 0);
	CHECK_EQ(args.verbose, 1);
	CHECK_EQ(args.command_count, 2);
	CHECK_EQ(test_parse(dashhost, &args), EINVAL);

	/* The refusals. */
	CHECK_EQ(test_parse(unknown, &args), EINVAL);
	CHECK_EQ(test_parse(missing_value, &args), 0);
	CHECK_EQ(args.command_count, 1);
	CHECK_EQ(test_parse(trailing_option, &args), EINVAL);
	CHECK_EQ(test_parse(no_destination, &args), EINVAL);
	CHECK_EQ(test_parse(empty_user, &args), EINVAL);
	CHECK_EQ(test_parse(empty_host, &args), EINVAL);
	CHECK_EQ(test_parse(n_with_command, &args), EINVAL);
	CHECK_EQ(test_parse(dash_alone, &args), EINVAL);
	CHECK_EQ(test_parse(dash_then_host, &args), EINVAL);

	/* An option without its value says so (not that the destination is missing). */
	argc = test_argv(argv, storage, p_last);
	CHECK_EQ(ssh_client_parse_args(argc, argv, &args, error, sizeof(error)), EINVAL);
	CHECK(strcmp(error, "option -p needs an argument") == 0);

	/* The user ends at the last '@'. */
	CHECK_EQ(test_parse(last_at, &args), 0);
	CHECK(strcmp(args.user, "a@b") == 0);
	CHECK(strcmp(args.host, "host") == 0);

	/* The longest host and user, and one more. */
	memset(long_host, 'h', sizeof(long_host));
	long_host[SSH_HOST_MAX] = '\0';
	words[0] = "ssh";
	words[1] = long_host;
	words[2] = NULL;
	CHECK_EQ(test_parse(words, &args), 0);
	CHECK_EQ(strlen(args.host), SSH_HOST_MAX);
	long_host[SSH_HOST_MAX] = 'h';
	long_host[SSH_HOST_MAX + 1] = '\0';
	CHECK_EQ(test_parse(words, &args), EINVAL);
	memset(long_user, 'u', sizeof(long_user));
	memcpy(long_user + SSH_USER_MAX, "@host", 6);
	words[1] = long_user;
	CHECK_EQ(test_parse(words, &args), 0);
	CHECK_EQ(strlen(args.user), SSH_USER_MAX);
	memset(long_user, 'u', sizeof(long_user));
	memcpy(long_user + SSH_USER_MAX + 1, "@host", 6);
	CHECK_EQ(test_parse(words, &args), EINVAL);

	/* At most SSH_CLIENT_SETTINGS_MAX settings. */
	words[0] = "ssh";
	for (index = 0; index < (int)SSH_CLIENT_SETTINGS_MAX; index++)
		words[index + 1] = "-oUser=a";
	words[SSH_CLIENT_SETTINGS_MAX + 1] = "host";
	words[SSH_CLIENT_SETTINGS_MAX + 2] = NULL;
	CHECK_EQ(test_parse(words, &args), 0);
	CHECK_EQ(args.setting_count, SSH_CLIENT_SETTINGS_MAX);
	words[SSH_CLIENT_SETTINGS_MAX + 1] = "-oUser=a";
	words[SSH_CLIENT_SETTINGS_MAX + 2] = "host";
	words[SSH_CLIENT_SETTINGS_MAX + 3] = NULL;
	CHECK_EQ(test_parse(words, &args), EINVAL);

	/* The command is the arguments joined by single spaces. */
	CHECK_EQ(test_parse(joined, &args), 0);
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), 0);
	CHECK_EQ(command.size, 7);
	CHECK(memcmp(command.data, "ls -l /", 7) == 0);
	ssh_buf_free(&command);

	/* A command of exactly SSH_CLIENT_COMMAND_MAX bytes is taken, one byte more is EMSGSIZE. */
	long_word = malloc(SSH_CLIENT_COMMAND_MAX + 2U);
	CHECK(long_word != NULL);
	if (long_word == NULL)
		return;
	memset(long_word, 'c', SSH_CLIENT_COMMAND_MAX + 1U);
	long_word[SSH_CLIENT_COMMAND_MAX - 2U] = '\0';
	args.command[0] = long_word;
	args.command[1] = "x";
	args.command_count = 2;
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), 0);
	CHECK_EQ(command.size, SSH_CLIENT_COMMAND_MAX);
	ssh_buf_free(&command);
	args.command[1] = "xy";
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), EMSGSIZE);
	ssh_buf_free(&command);
	ssh_buf_init(&command, 2U * SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), EMSGSIZE);
	ssh_buf_free(&command);
	long_word[SSH_CLIENT_COMMAND_MAX - 2U] = 'c';
	long_word[SSH_CLIENT_COMMAND_MAX] = '\0';
	args.command_count = 1;
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), 0);
	CHECK_EQ(command.size, SSH_CLIENT_COMMAND_MAX);
	ssh_buf_free(&command);
	args.command_count = 2;
	args.command[1] = "";
	ssh_buf_init(&command, SSH_CLIENT_COMMAND_MAX, 0);
	CHECK_EQ(ssh_client_join_command(&args, &command), EMSGSIZE);
	ssh_buf_free(&command);
	free(long_word);
}

/* Applies the words' settings and returns the error. */
static int
test_apply(
	const char *const *words,
	struct ssh_config *config)
{
	struct ssh_client_args args;
	char error[SSH_CONFIG_ERROR_MAX];
	int status;

	status = test_parse(words, &args);
	CHECK_EQ(status, 0);
	ssh_config_init(config);
	status = ssh_client_apply_args(&args, config, error);
	if (status == 0)
		ssh_config_defaults(config);
	return status;
}

/* The settings the command line gives. */
static void
test_apply_args(void)
{
	static const char *const user_wins[] = { "ssh", "-l", "bob", "-o", "User=carol", "alice@h", NULL };
	static const char *const l_first[] = { "ssh", "-l", "bob", "-o", "User=carol", "h", NULL };
	static const char *const o_first[] = { "ssh", "-o", "User=carol", "-l", "bob", "h", NULL };
	static const char *const port[] = { "ssh", "-p", "2222", "-o", "Port=3333", "h", NULL };
	static const char *const bad_port[] = { "ssh", "-p", "0", "h", NULL };
	static const char *const identities[] = { "ssh", "-i", "a b", "-i", "/k2", "h", NULL };
	static const char *const quote[] = { "ssh", "-i", "a\"b", "h", NULL };
	static const char *const tt[] = { "ssh", "-tt", "h", NULL };
	static const char *const t[] = { "ssh", "-t", "h", NULL };
	static const char *const tee[] = { "ssh", "-t", "-T", "h", NULL };
	static const char *const tty_over_o[] = { "ssh", "-o", "RequestTTY=no", "-t", "h", NULL };
	static const char *const quiet[] = { "ssh", "-q", "-v", "h", NULL };
	static const char *const verbose[] = { "ssh", "-v", "h", NULL };
	static const char *const debug[] = { "ssh", "-vv", "h", NULL };
	static const char *const none[] = { "ssh", "h", NULL };
	static const char *const bad_option[] = { "ssh", "-o", "NoSuchKeyword=1", "h", NULL };
	struct ssh_config config;

	/* The destination's user wins over -l and -o; between -l and -o the first wins. */
	CHECK_EQ(test_apply(user_wins, &config), 0);
	CHECK(strcmp(config.user, "alice") == 0);
	CHECK_EQ(test_apply(l_first, &config), 0);
	CHECK(strcmp(config.user, "bob") == 0);
	CHECK_EQ(test_apply(o_first, &config), 0);
	CHECK(strcmp(config.user, "carol") == 0);

	/* -p, its range, and -i with a space in the path, in order. */
	CHECK_EQ(test_apply(port, &config), 0);
	CHECK_EQ(config.port, 2222);
	CHECK_EQ(test_apply(bad_port, &config), EINVAL);
	CHECK_EQ(test_apply(identities, &config), 0);
	CHECK_EQ(config.identity_count, 2);
	CHECK(strcmp(config.identity[0], "a b") == 0);
	CHECK(strcmp(config.identity[1], "/k2") == 0);
	CHECK_EQ(test_apply(quote, &config), EINVAL);

	/* The terminal flags: -tt force, -t yes, -T wins, and the flags win over -o. */
	CHECK_EQ(test_apply(tt, &config), 0);
	CHECK_EQ(config.request_tty, SSH_TTY_FORCE);
	CHECK_EQ(test_apply(t, &config), 0);
	CHECK_EQ(config.request_tty, SSH_TTY_YES);
	CHECK_EQ(test_apply(tee, &config), 0);
	CHECK_EQ(config.request_tty, SSH_TTY_NO);
	CHECK_EQ(test_apply(tty_over_o, &config), 0);
	CHECK_EQ(config.request_tty, SSH_TTY_YES);
	CHECK_EQ(test_apply(none, &config), 0);
	CHECK_EQ(config.request_tty, SSH_TTY_AUTO);

	/* The log flags: -q wins, -v VERBOSE, -vv DEBUG, none INFO. */
	CHECK_EQ(test_apply(quiet, &config), 0);
	CHECK_EQ(config.log_level, SSH_CLIENT_LOG_QUIET);
	CHECK_EQ(test_apply(verbose, &config), 0);
	CHECK_EQ(config.log_level, SSH_CLIENT_LOG_VERBOSE);
	CHECK_EQ(test_apply(debug, &config), 0);
	CHECK_EQ(config.log_level, SSH_CLIENT_LOG_DEBUG);
	CHECK_EQ(test_apply(none, &config), 0);
	CHECK_EQ(config.log_level, SSH_CLIENT_LOG_INFO);

	/* A -o the configuration refuses. */
	CHECK_EQ(test_apply(bad_option, &config), EINVAL);
}

/* The answer ssh_client_host_decide must give, from the rules of its comment written out apart. */
static enum ssh_client_host_action
test_expected_action(
	enum ssh_known_result user_answer,
	enum ssh_known_result global_answer,
	enum ssh_strict_host strict,
	int batch_mode)
{
	if (user_answer == SSH_KNOWN_REVOKED)
		return SSH_CLIENT_HOST_REFUSE_REVOKED;
	if (global_answer == SSH_KNOWN_REVOKED)
		return SSH_CLIENT_HOST_REFUSE_REVOKED;
	if (user_answer == SSH_KNOWN_MATCH)
		return SSH_CLIENT_HOST_ACCEPT;
	if (global_answer == SSH_KNOWN_MATCH)
		return SSH_CLIENT_HOST_ACCEPT;
	if (user_answer == SSH_KNOWN_CHANGED)
		return SSH_CLIENT_HOST_REFUSE_CHANGED;
	if (global_answer == SSH_KNOWN_CHANGED)
		return SSH_CLIENT_HOST_REFUSE_CHANGED;
	if (strict == SSH_STRICT_YES)
		return SSH_CLIENT_HOST_REFUSE_UNKNOWN;
	if (strict == SSH_STRICT_NO)
		return SSH_CLIENT_HOST_ADD;
	if (strict == SSH_STRICT_ACCEPT_NEW)
		return SSH_CLIENT_HOST_ADD;
	if (batch_mode)
		return SSH_CLIENT_HOST_REFUSE_UNKNOWN;
	return SSH_CLIENT_HOST_ASK;
}

/* The host key decision over every combination. */
static void
test_decide(void)
{
	enum ssh_client_host_action action;
	unsigned user_answer;
	unsigned global_answer;
	unsigned strict;
	int batch;

	/* Every answer of both files, every StrictHostKeyChecking and both BatchMode values. */
	for (user_answer = 0; user_answer <= SSH_KNOWN_OTHER_TYPES; user_answer++) {
		for (global_answer = 0; global_answer <= SSH_KNOWN_OTHER_TYPES; global_answer++) {
			for (strict = 0; strict <= SSH_STRICT_ACCEPT_NEW; strict++) {
				for (batch = 0; batch <= 1; batch++) {
					action = (enum ssh_client_host_action)99;
					CHECK_EQ(ssh_client_host_decide((enum ssh_known_result)user_answer, (enum ssh_known_result)global_answer, (enum ssh_strict_host)strict, batch, &action), 0);
					CHECK_EQ(action, test_expected_action((enum ssh_known_result)user_answer, (enum ssh_known_result)global_answer, (enum ssh_strict_host)strict, batch));
				}
			}
		}
	}

	/* A few rows by hand: a changed key is refused even with "no", other types count as unknown. */
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_CHANGED, SSH_KNOWN_ABSENT, SSH_STRICT_NO, 0, &action), 0);
	CHECK_EQ(action, SSH_CLIENT_HOST_REFUSE_CHANGED);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_OTHER_TYPES, SSH_KNOWN_ABSENT, SSH_STRICT_ACCEPT_NEW, 1, &action), 0);
	CHECK_EQ(action, SSH_CLIENT_HOST_ADD);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_CHANGED, SSH_KNOWN_MATCH, SSH_STRICT_YES, 1, &action), 0);
	CHECK_EQ(action, SSH_CLIENT_HOST_ACCEPT);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_MATCH, SSH_KNOWN_REVOKED, SSH_STRICT_NO, 0, &action), 0);
	CHECK_EQ(action, SSH_CLIENT_HOST_REFUSE_REVOKED);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_ABSENT, SSH_KNOWN_ABSENT, SSH_STRICT_ASK, 0, &action), 0);
	CHECK_EQ(action, SSH_CLIENT_HOST_ASK);

	/* Values out of range. */
	CHECK_EQ(ssh_client_host_decide((enum ssh_known_result)5, SSH_KNOWN_ABSENT, SSH_STRICT_ASK, 0, &action), EINVAL);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_ABSENT, (enum ssh_known_result)5, SSH_STRICT_ASK, 0, &action), EINVAL);
	CHECK_EQ(ssh_client_host_decide(SSH_KNOWN_ABSENT, SSH_KNOWN_ABSENT, (enum ssh_strict_host)4, 0, &action), EINVAL);
}

/* Starts the test connection with the settings lines (each a -o option), for example.org port 2222. */
static void
test_client_setup(
	const char *const *options,
	const char *host,
	uint16_t port)
{
	char error[SSH_CONFIG_ERROR_MAX];
	int index;

	ssh_client_free(&client);
	ssh_client_init(&client);
	ssh_config_init(&client.config);
	for (index = 0; options[index] != NULL; index++)
		CHECK_EQ(ssh_config_option(&client.config, options[index], error), 0);
	ssh_config_defaults(&client.config);
	client.log_level = SSH_CLIENT_LOG_QUIET;
	snprintf(client.host_name, sizeof(client.host_name), "%s", host);
	client.port = port;
}

/* The host key check against known_hosts files. */
static void
test_check_host_key(void)
{
	char user_option[SSH_CLIENT_PATH_MAX];
	char global_option[SSH_CLIENT_PATH_MAX];
	char user_path[SSH_CLIENT_PATH_MAX];
	char global_path[SSH_CLIENT_PATH_MAX];
	char home_path[SSH_CLIENT_PATH_MAX];
	char text[4096];
	char expected[4096];
	char captured[1024];
	const char *options[6];
	struct stat information;
	int status;

	snprintf(user_path, sizeof(user_path), "%s/known_hosts", directory);
	snprintf(global_path, sizeof(global_path), "%s/global_known_hosts", directory);
	snprintf(user_option, sizeof(user_option), "UserKnownHostsFile %s", user_path);
	snprintf(global_option, sizeof(global_option), "GlobalKnownHostsFile %s", global_path);

	/* accept-new: an unknown host is added as "[host]:port ssh-ed25519 <base64>" and accepted. */
	options[0] = user_option;
	options[1] = global_option;
	options[2] = "StrictHostKeyChecking accept-new";
	options[3] = NULL;
	test_client_setup(options, "example.org", 2222);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	CHECK_EQ(client.have_host_key, 1);
	test_read_file(user_path, text, sizeof(text));
	snprintf(expected, sizeof(expected), "[example.org]:2222 ssh-ed25519 %s\n", base1);
	CHECK(strcmp(text, expected) == 0);

	/* A re-exchange with the same key passes and writes nothing; another key is refused. */
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	test_read_file(user_path, text, sizeof(text));
	CHECK(strcmp(text, expected) == 0);
	CHECK_EQ(ssh_client_check_host_key(&client, blob2.data, blob2.size), EACCES);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size - 1U), EINVAL);

	/* The next connection finds the key with "yes", and does not add it again. */
	options[2] = "StrictHostKeyChecking yes";
	test_client_setup(options, "example.org", 2222);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	test_read_file(user_path, text, sizeof(text));
	CHECK(strcmp(text, expected) == 0);

	/* Another port of the same host is another name: unknown, refused with "yes". */
	test_client_setup(options, "example.org", 2223);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), EACCES);
	CHECK_EQ(client.have_host_key, 0);

	/* Port 22 is named without brackets, in the file and in the message. */
	options[2] = "StrictHostKeyChecking no";
	test_client_setup(options, "example.org", 22);
	client.log_level = SSH_CLIENT_LOG_INFO;
	test_capture_start();
	CHECK_EQ(ssh_client_check_host_key(&client, blob2.data, blob2.size), 0);
	test_capture_end(captured, sizeof(captured));
	CHECK(strcmp(captured, "Warning: Permanently added 'example.org' (ED25519) to the list of known hosts.\n") == 0);
	test_read_file(user_path, text, sizeof(text));
	snprintf(expected, sizeof(expected), "[example.org]:2222 ssh-ed25519 %s\nexample.org ssh-ed25519 %s\n", base1, base2);
	CHECK(strcmp(text, expected) == 0);

	/* A changed key is refused, even with "no", and the file is left alone. */
	test_client_setup(options, "example.org", 2222);
	CHECK_EQ(ssh_client_check_host_key(&client, blob2.data, blob2.size), EACCES);
	CHECK_EQ(client.have_host_key, 0);
	test_read_file(user_path, text, sizeof(text));
	CHECK(strcmp(text, expected) == 0);

	/* A revoked key is refused for every host. */
	snprintf(text, sizeof(text), "@revoked * ssh-ed25519 %s\n", base1);
	test_write_file(global_path, text);
	test_client_setup(options, "example.org", 2222);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), EACCES);

	/* A key known in the global file passes and adds nothing to the user's file. */
	snprintf(text, sizeof(text), "other.example ssh-ed25519 %s\n", base1);
	test_write_file(global_path, text);
	options[2] = "StrictHostKeyChecking yes";
	test_client_setup(options, "OTHER.example", 22);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	test_read_file(user_path, text, sizeof(text));
	CHECK(strcmp(text, expected) == 0);

	/* Ask in batch mode is a refusal, without a question. */
	options[2] = "StrictHostKeyChecking ask";
	options[3] = "BatchMode yes";
	options[4] = NULL;
	test_client_setup(options, "new.example", 22);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), EACCES);

	/* A user file without its last LF gets the new line on a line of its own. */
	snprintf(text, sizeof(text), "# no final newline");
	test_write_file(user_path, text);
	options[2] = "StrictHostKeyChecking accept-new";
	options[3] = NULL;
	test_client_setup(options, "new.example", 22);
	CHECK_EQ(ssh_client_check_host_key(&client, blob2.data, blob2.size), 0);
	test_read_file(user_path, text, sizeof(text));
	snprintf(expected, sizeof(expected), "# no final newline\nnew.example ssh-ed25519 %s\n", base2);
	CHECK(strcmp(text, expected) == 0);

	/* "none" (read as /dev/null) adds nothing, says nothing, and accepts with "no". */
	options[0] = "UserKnownHostsFile none";
	options[2] = "StrictHostKeyChecking no";
	test_client_setup(options, "new.example", 22);
	client.log_level = SSH_CLIENT_LOG_INFO;
	test_capture_start();
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	test_capture_end(captured, sizeof(captured));
	CHECK(strcmp(captured, "") == 0);

	/* The default file under HOME: the missing key directory is made with mode 0700. */
	snprintf(home_path, sizeof(home_path), "%s/home", directory);
	CHECK_EQ(mkdir(home_path, 0700), 0);
	CHECK_EQ(setenv("HOME", home_path, 1), 0);
	options[0] = global_option;
	options[1] = "StrictHostKeyChecking accept-new";
	options[2] = NULL;
	test_client_setup(options, "home.example", 2022);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	snprintf(user_path, sizeof(user_path), "%s/.ssh", home_path);
	status = stat(user_path, &information);
	CHECK_EQ(status, 0);
	CHECK_EQ(information.st_mode & 0777, 0700);
	snprintf(user_path, sizeof(user_path), "%s/.ssh/known_hosts", home_path);
	test_read_file(user_path, text, sizeof(text));
	snprintf(expected, sizeof(expected), "[home.example]:2022 ssh-ed25519 %s\n", base1);
	CHECK(strcmp(text, expected) == 0);

	/* A missing directory other than the user's key directory is not made (the line is not written). */
	snprintf(user_option, sizeof(user_option), "UserKnownHostsFile %s/missing/known_hosts", directory);
	options[0] = user_option;
	options[1] = "StrictHostKeyChecking accept-new";
	options[2] = NULL;
	test_client_setup(options, "home.example", 2022);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	snprintf(user_path, sizeof(user_path), "%s/missing", directory);
	status = stat(user_path, &information);
	CHECK(status != 0);

	/* Nor is the user's key directory made for a file elsewhere. */
	snprintf(home_path, sizeof(home_path), "%s/home2", directory);
	CHECK_EQ(mkdir(home_path, 0700), 0);
	CHECK_EQ(setenv("HOME", home_path, 1), 0);
	test_client_setup(options, "home.example", 2022);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), 0);
	snprintf(user_path, sizeof(user_path), "%s/.ssh", home_path);
	status = stat(user_path, &information);
	CHECK(status != 0);

	/* A user file that cannot be read is a refusal. */
	options[0] = "UserKnownHostsFile /";
	options[1] = "StrictHostKeyChecking no";
	options[2] = NULL;
	test_client_setup(options, "x.example", 22);
	CHECK_EQ(ssh_client_check_host_key(&client, blob1.data, blob1.size), EACCES);
	ssh_client_free(&client);
}

/* Filters text through a fresh escape state in pieces of size step and checks the bytes sent and the disconnect. */
static void
test_escape_case(
	const char *input,
	size_t step,
	const char *expected,
	int expected_disconnect)
{
	struct ssh_client_escape escape;
	uint8_t out[64];
	uint8_t total[256];
	size_t size;
	size_t count;
	size_t offset;
	size_t piece;
	int disconnect;

	ssh_client_escape_init(&escape);
	count = 0;
	disconnect = 0;
	for (offset = 0; offset < strlen(input) && !disconnect; offset += piece) {
		piece = strlen(input) - offset;
		if (piece > step)
			piece = step;
		ssh_client_escape_filter(&escape, (const uint8_t *)input + offset, piece, out, &size, &disconnect);
		CHECK(size <= piece + 1U);
		memcpy(total + count, out, size);
		count += size;
	}
	CHECK_EQ(count, strlen(expected));
	CHECK(memcmp(total, expected, count) == 0);
	CHECK_EQ(disconnect, expected_disconnect);
}

/* The escapes of a terminal. */
static void
test_escape(void)
{
	size_t step;

	/* Each case whole, byte by byte, and in pieces of two and three. */
	for (step = 1; step <= 64; step++) {
		if (step > 3 && step != 64)
			continue;
		test_escape_case("~.", step, "", 1);
		test_escape_case("ls~.\r", step, "ls~.\r", 0);
		test_escape_case("ls\r~.rest", step, "ls\r", 1);
		test_escape_case("ls\n~.", step, "ls\n", 1);
		test_escape_case("~~", step, "~", 0);
		test_escape_case("~~.", step, "~.", 0);
		test_escape_case("~x", step, "~x", 0);
		test_escape_case("~\r~.", step, "~\r", 1);
		test_escape_case("~\n~~", step, "~\n~", 0);
		test_escape_case("a~~b", step, "a~~b", 0);
		test_escape_case("~", step, "", 0);
		test_escape_case(".~", step, ".~", 0);
		test_escape_case("ab", step, "ab", 0);
		test_escape_case("\r\r~.", step, "\r\r", 1);
	}
}

/* The "~/" paths. */
static void
test_paths(void)
{
	char out[64];
	char tiny[8];

	CHECK_EQ(setenv("HOME", "/home/u", 1), 0);
	CHECK_EQ(ssh_client_expand_path("~/.ssh/known_hosts", out, sizeof(out)), 0);
	CHECK(strcmp(out, "/home/u/.ssh/known_hosts") == 0);
	CHECK_EQ(ssh_client_expand_path("~", out, sizeof(out)), 0);
	CHECK(strcmp(out, "/home/u") == 0);
	CHECK_EQ(ssh_client_expand_path("~user/x", out, sizeof(out)), 0);
	CHECK(strcmp(out, "~user/x") == 0);
	CHECK_EQ(ssh_client_expand_path("/abs/~/x", out, sizeof(out)), 0);
	CHECK(strcmp(out, "/abs/~/x") == 0);
	CHECK_EQ(ssh_client_expand_path("~/abc", tiny, sizeof(tiny)), ENAMETOOLONG);
	CHECK_EQ(ssh_client_expand_path("~/a", tiny, sizeof(tiny)), ENAMETOOLONG);
	CHECK_EQ(setenv("HOME", "/h", 1), 0);
	CHECK_EQ(ssh_client_expand_path("~/abcd", tiny, sizeof(tiny)), 0);
	CHECK(strcmp(tiny, "/h/abcd") == 0);
	CHECK_EQ(ssh_client_expand_path("~/abcde", tiny, sizeof(tiny)), ENAMETOOLONG);
	CHECK_EQ(setenv("HOME", "/home/u", 1), 0);
	CHECK_EQ(ssh_client_expand_path("1234567", tiny, sizeof(tiny)), 0);
	CHECK_EQ(ssh_client_expand_path("12345678", tiny, sizeof(tiny)), ENAMETOOLONG);
	CHECK_EQ(ssh_client_home(out, sizeof(out)), 0);
	CHECK(strcmp(out, "/home/u") == 0);
}

/* The reason found in the unencrypted output, and the order of the backlog. */
static void
test_reason_and_backlog(void)
{
	static const char *const options[] = { NULL };
	static uint8_t message[SSH_PAYLOAD_SEND_MAX];
	char reason[SSH_TRANSPORT_REASON_MAX];
	char small[5];
	size_t size;
	unsigned sent;
	unsigned kept;
	int status;
	uint32_t length;
	size_t offset;

	/* Nothing is found before the transport starts, or before a DISCONNECT is queued. */
	test_client_setup(options, "example.org", 22);
	CHECK_EQ(ssh_client_local_reason(&client, reason, sizeof(reason)), 0);
	CHECK_EQ(ssh_client_start(&client), 0);
	CHECK_EQ(ssh_client_local_reason(&client, reason, sizeof(reason)), 0);

	/* The DISCONNECT behind the identification line gives its description, cut to the room. */
	ssh_transport_disconnect(&client.transport, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "no matching kex_algorithms");
	CHECK_EQ(ssh_client_local_reason(&client, reason, sizeof(reason)), 1);
	CHECK(strcmp(reason, "no matching kex_algorithms") == 0);
	CHECK_EQ(ssh_client_local_reason(&client, small, sizeof(small)), 1);
	CHECK(strcmp(small, "no m") == 0);
	CHECK_EQ(ssh_client_local_reason(&client, small, 0), 0);
	CHECK_EQ(ssh_client_local_reason(&client, reason, 26), 1);
	CHECK(strcmp(reason, "no matching kex_algorithm") == 0);
	CHECK_EQ(ssh_client_local_reason(&client, reason, 27), 1);
	CHECK(strcmp(reason, "no matching kex_algorithms") == 0);

	/* Without the identification line in front (written already) it is found too. */
	(void)ssh_transport_pending(&client.transport, &size);
	CHECK(size > strlen(SSH_VERSION_STRING) + 2U);
	ssh_transport_written(&client.transport, strlen(SSH_VERSION_STRING) + 2U);
	CHECK_EQ(ssh_client_local_reason(&client, reason, sizeof(reason)), 1);
	CHECK(strcmp(reason, "no matching kex_algorithms") == 0);

	/* With everything written, nothing is left to find. */
	(void)ssh_transport_pending(&client.transport, &size);
	ssh_transport_written(&client.transport, size);
	CHECK_EQ(ssh_client_local_reason(&client, reason, sizeof(reason)), 0);

	/* Before the key exchange the transport holds messages; when it is full they wait in the backlog, in order. */
	test_client_setup(options, "example.org", 22);
	CHECK_EQ(ssh_client_start(&client), 0);
	sent = 0;
	status = 0;
	while (status == 0 && sent < 1000) {
		memset(message, 0, sizeof(message));
		message[0] = SSH_MSG_CHANNEL_DATA;
		ssh_store_be32(message + 1, sent);
		status = ssh_client_send(&client, message, sizeof(message));
		if (status == 0)
			sent++;
	}
	CHECK_EQ(status, ENOBUFS);
	CHECK(client.backlog.size > 0);
	CHECK(client.backlog.size <= SSH_CLIENT_BACKLOG_MAX);
	CHECK(client.backlog.size + 4U + sizeof(message) > SSH_CLIENT_BACKLOG_MAX);

	/* The backlog holds the last messages sent, in the order sent, and a flush that cannot send keeps them. */
	CHECK_EQ(ssh_client_flush(&client), 0);
	kept = (unsigned)(client.backlog.size / (4U + sizeof(message)));
	CHECK_EQ(client.backlog.size, (size_t)kept * (4U + sizeof(message)));
	offset = 0;
	while (offset < client.backlog.size) {
		length = ssh_load_be32(client.backlog.data + offset);
		CHECK_EQ(length, sizeof(message));
		CHECK_EQ(client.backlog.data[offset + 4U], SSH_MSG_CHANNEL_DATA);
		CHECK_EQ(ssh_load_be32(client.backlog.data + offset + 5U), sent - kept + (unsigned)(offset / (4U + sizeof(message))));
		offset += 4U + length;
	}

	/* The backlog's limit counts the length field: a message 2 bytes too big is refused, one that fits exactly is kept. */
	CHECK_EQ(ssh_client_send(&client, message, SSH_CLIENT_BACKLOG_MAX - client.backlog.size - 2U), ENOBUFS);
	size = client.backlog.size;
	CHECK_EQ(ssh_client_send(&client, message, SSH_CLIENT_BACKLOG_MAX - client.backlog.size - 4U), 0);
	CHECK_EQ(client.backlog.size, SSH_CLIENT_BACKLOG_MAX);
	CHECK(client.backlog.size > size);
	CHECK_EQ(ssh_client_send(&client, message, 1), ENOBUFS);
	ssh_client_free(&client);
}

/* Reading a whole file within a limit. */
static void
test_read_limit(void)
{
	char path[600];
	struct ssh_buf text;

	snprintf(path, sizeof(path), "%s/ten", directory);
	test_write_file(path, "0123456789");
	ssh_buf_init(&text, 64, 0);
	CHECK_EQ(ssh_client_read_file(path, &text, 10), 0);
	CHECK_EQ(text.size, 10);
	ssh_buf_free(&text);
	CHECK_EQ(ssh_client_read_file(path, &text, 9), EMSGSIZE);
	ssh_buf_free(&text);
	snprintf(path, sizeof(path), "%s/absent", directory);
	CHECK_EQ(ssh_client_read_file(path, &text, 9), ENOENT);
	ssh_buf_free(&text);
}

/*
 * Runs the checks of the program's own parts.
 */
int
main(void)
{
	int written;

	/* The keys and their blobs and base64 forms. */
	CHECK_EQ(wire_hex(t1_public_hex, key1, sizeof(key1)), 32);
	CHECK_EQ(wire_hex(t2_public_hex, key2, sizeof(key2)), 32);
	wire_reset(&blob1);
	wire_text(&blob1, "ssh-ed25519");
	wire_string(&blob1, key1, sizeof(key1));
	wire_reset(&blob2);
	wire_text(&blob2, "ssh-ed25519");
	wire_string(&blob2, key2, sizeof(key2));
	CHECK_EQ(blob1.size, SSH_KEY_BLOB_SIZE);
	test_base64(blob1.data, blob1.size, base1);
	test_base64(blob2.data, blob2.size, base2);

	/* A new directory for the files. */
	written = snprintf(directory, sizeof(directory), "build/tmp/ssh-unit/client.XXXXXX");
	CHECK(written > 0);
	CHECK(mkdtemp(directory) != NULL);

	ssh_client_init(&client);
	test_args();
	test_apply_args();
	test_decide();
	test_check_host_key();
	test_escape();
	test_paths();
	test_reason_and_backlog();
	test_read_limit();
	ssh_client_free(&client);
	return check_finish("client");
}
