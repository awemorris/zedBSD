/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/net/wifi-store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int read_line(char *buffer, size_t capacity);
static int provision(void);
static int run_command(char **arguments);
static int connect_band(unsigned index);

/*
 * Runs a private hardware fixture without placing passphrases in arguments.
 */
int
main(
	int argc,
	char **argv)
{
	int result;

	/* Selects a fixed fixture action or an ordinary command observer. */
	result = 2;
	if (argc == 2 && strcmp(argv[1], "setup") == 0) {
		result = provision();
	} else if (argc == 2 && strcmp(argv[1], "connect2g") == 0) {
		result = connect_band(0U);
	} else if (argc == 2 && strcmp(argv[1], "connect5g") == 0) {
		result = connect_band(1U);
	} else if (argc >= 3 && strcmp(argv[1], "run") == 0) {
		result = run_command(argv + 2);
	}

	/* Publishes only the fixture action's exit status. */
	printf("q080-exit %d\n", result);
	return result;
}

/* Reads one bounded terminal line without printing its contents. */
static int
read_line(
	char *buffer,
	size_t capacity)
{
	size_t used;
	ssize_t count;
	char byte;

	/* Accepts printable credential bytes until the terminal newline. */
	used = 0U;
	while (used < capacity) {
		count = read(STDIN_FILENO, &byte, 1U);
		if (count != 1) {
			return EIO;
		}

		/* Terminates a complete nonempty line. */
		if (byte == '\n') {
			buffer[used] = '\0';
			return used != 0U ? 0 : EINVAL;
		}

		/* Rejects control bytes and overlong records. */
		if ((unsigned char)byte < 0x20U || (unsigned char)byte > 0x7eU) {
			return EINVAL;
		}

		/* Reserves the terminator while still admitting a final newline. */
		if (used + 1U == capacity) {
			return E2BIG;
		}
		buffer[used++] = byte;
	}

	/* Refuses truncated input. */
	return E2BIG;
}

/* Provisions the ordinary store through its production API with echo disabled. */
static int
provision(void)
{
	struct termios original;
	struct termios hidden;
	char ssid2[WIFI_CONF_SSID_MAX + 1U];
	char ssid5[WIFI_CONF_SSID_MAX + 1U];
	char key[WIFI_CONF_PASSPHRASE_MAX + 1U];
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	int result;

	/* Disables terminal echo before admitting runtime credentials. */
	memset(ssid2, 0, sizeof(ssid2));
	memset(ssid5, 0, sizeof(ssid5));
	memset(key, 0, sizeof(key));
	memset(diagnostic, 0, sizeof(diagnostic));
	if (tcgetattr(STDIN_FILENO, &original) != 0) {
		return errno;
	}
	hidden = original;
	hidden.c_lflag &= ~(ECHO | ECHONL);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
		return errno;
	}
	printf("q080-credential-input\n");
	fflush(stdout);

	/* Reads both selections and the shared key into bounded private buffers. */
	result = read_line(ssid2, sizeof(ssid2));
	if (result == 0) {
		result = read_line(ssid5, sizeof(ssid5));
	}
	if (result == 0) {
		result = read_line(key, sizeof(key));
	}

	/* Uses the same atomic store writer as the ordinary net command. */
	if (result == 0) {
		result = wifi_store_set_key_for_effective_user(
			ssid2, key, 0, diagnostic, sizeof(diagnostic));
	}
	if (result == 0) {
		result = wifi_store_set_key_for_effective_user(
			ssid5, key, 0, diagnostic, sizeof(diagnostic));
	}

	/* Converts the store's POSIX failure to a stable diagnostic number. */
	if (result < 0) {
		result = errno != 0 ? errno : EIO;
	}

	/* Erases credential buffers before restoring normal terminal behavior. */
	wifi_conf_explicit_clear(ssid2, sizeof(ssid2));
	wifi_conf_explicit_clear(ssid5, sizeof(ssid5));
	wifi_conf_explicit_clear(key, sizeof(key));
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0 && result == 0) {
		result = errno;
	}

	/* Returns the store or terminal result without credential diagnostics. */
	return result;
}

/* Runs an existing user command and observes its real exit status. */
static int
run_command(
	char **arguments)
{
	pid_t child;
	pid_t waited;
	int status;

	/* Executes the production utility with the fixture's ordinary descriptors. */
	fflush(NULL);
	child = fork();
	if (child < 0) {
		return errno;
	}
	if (child == 0) {
		execv(arguments[0], arguments);
		_exit(127);
	}

	/* Distinguishes command failure from a killed or uncollectable child. */
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child || !WIFEXITED(status)) {
		return 126;
	}

	/* Preserves the command's exit value. */
	return WEXITSTATUS(status);
}

/* Selects a provisioned band and invokes the ordinary networkd client. */
static int
connect_band(
	unsigned index)
{
	struct wifi_conf_model model;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	char ssid[WIFI_CONF_SSID_MAX + 1U];
	char *arguments[5];
	int result;

	/* Loads the fixed root store without printing any profile or key. */
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	memset(ssid, 0, sizeof(ssid));
	result = wifi_store_load_for_effective_user(
		&model, diagnostic, sizeof(diagnostic));
	if (result == 0 && index >= model.profile_count) {
		result = ENOENT;
	}
	if (result == 0) {
		memcpy(ssid, model.profiles[index].ssid,
			model.profiles[index].ssid_length);
	}
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));

	/* Supplies only the network name to net; networkd owns the fd-4 key path. */
	if (result == 0) {
		arguments[0] = "/sbin/net";
		arguments[1] = "wifi";
		arguments[2] = "connect";
		arguments[3] = ssid;
		arguments[4] = NULL;
		result = run_command(arguments);
	}
	wifi_conf_explicit_clear(ssid, sizeof(ssid));

	/* Returns only the command result. */
	return result;
}
