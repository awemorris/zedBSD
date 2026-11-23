/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Host of ssh_config a WSL distribution gets after its first login
 * (wsl-record.h).
 *
 * The block is added as the settings tab's Hosts pane adds one (the
 * terminal's settings-hosts.c): before the first Host block that would
 * apply to the name (so a "Host wsl-*" or "Host *" after it still gives
 * what the new block does not), indented and ending its lines as the file
 * does, every other byte kept.  It names no HostName: the distribution's
 * address changes with each start of WSL, and the launch screen finds it
 * each time.  A text that already has a Host of that name (in any case)
 * is not changed.
 */

#include "wsl-record.h"

#include "userland/base/ssh/ssh-config.h"
#include "userland/desktop/terminal/settings-hosts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest Host name made, with its NUL. */
#define WSL_RECORD_NAME_MAX		128U

/* What wsl_record_named answers when the text has the Host already. */
#define WSL_RECORD_FOUND		1

static int wsl_record_named(void *data, const char *name, size_t size);
static int wsl_record_add(char **text, size_t *size, unsigned block, const char *keyword, const char *value);

/*
 * Makes ssh\config's text (size bytes; NULL or 0 for no file) with a Host
 * wsl-NAME block for a distribution: User user, Port port when it is not
 * SSH's own, IdentityFile identity (as ssh_config names it) when it is
 * given and is not the key read by default.  *out (malloc'd, a NUL after
 * its *out_size bytes) is the new text.  Returns 0; EEXIST when the text
 * has a Host of that name already (nothing made); EINVAL for a name, a
 * user or a key a line cannot hold, or a text whose new Host ssh would
 * refuse; or ENOMEM.
 */
int
wsl_record_text(
	const char *text,
	size_t size,
	const char *name,
	const char *user,
	unsigned port,
	const char *identity,
	char **out,
	size_t *out_size)
{
	struct terminal_hosts hosts;
	struct ssh_config *config;
	char host[WSL_RECORD_NAME_MAX];
	char written[TERMINAL_HOSTS_VALUE_MAX];
	char number[16];
	char error_text[SSH_CONFIG_ERROR_MAX];
	char *made;
	size_t made_size;
	unsigned block;
	int same;
	int status;

	/* Nothing made yet; no file is an empty text. */
	*out = NULL;
	*out_size = 0U;
	if (text == NULL) {
		text = "";
		size = 0U;
	}

	/* The Host's name. */
	status = snprintf(host, sizeof(host), "%s%s", WSL_RECORD_PREFIX, name);
	if (status < 0 || (size_t)status >= sizeof(host))
		return EINVAL;

	/* A Host of that name already: nothing to add. */
	status = ssh_config_hosts(text, size, wsl_record_named, host, NULL);
	if (status == WSL_RECORD_FOUND)
		return EEXIST;

	/* The new block, with its Host line alone. */
	status = terminal_hosts_put_names(host, written, sizeof(written));
	if (status != 0)
		return EINVAL;
	status = terminal_hosts_read(&hosts, text, size);
	if (status != 0)
		return status;
	status = terminal_hosts_add_host(&hosts, written, &made, &made_size, &block);
	terminal_hosts_release(&hosts);
	if (status != 0)
		return status;

	/* The user logged in as. */
	status = terminal_hosts_put_word(user, written, sizeof(written));
	if (status == 0)
		status = wsl_record_add(&made, &made_size, block, "User", written);

	/* The port, which a Host without one would not connect to. */
	if (status == 0 && port != WSL_RECORD_SSH_PORT) {
		(void)snprintf(number, sizeof(number), "%u", port);
		status = wsl_record_add(&made, &made_size, block, "Port", number);
	}

	/* The key the server took, unless ssh reads it without being told. */
	if (status == 0 && identity != NULL && identity[0] != '\0') {
		same = strcmp(identity, WSL_RECORD_DEFAULT_KEY);
		if (same != 0) {
			status = terminal_hosts_put_word(identity, written, sizeof(written));
			if (status == 0)
				status = wsl_record_add(&made, &made_size, block, "IdentityFile", written);
		}
	}
	if (status != 0) {
		free(made);
		if (status == ENAMETOOLONG)
			return EINVAL;
		return status;
	}

	/* The new text must be one ssh reads for the Host. */
	config = malloc(sizeof(*config));
	if (config == NULL) {
		free(made);
		return ENOMEM;
	}
	ssh_config_init(config);
	status = ssh_config_parse(config, made, made_size, host, error_text);
	free(config);
	if (status != 0) {
		free(made);
		return EINVAL;
	}

	/* Succeeded: the text with the Host. */
	*out = made;
	*out_size = made_size;
	return 0;
}

/* Tells whether a Host name is the one looked for, in any case (ssh_config_hosts' handler); answers WSL_RECORD_FOUND or 0. */
static int
wsl_record_named(
	void *data,
	const char *name,
	size_t size)
{
	const char *wanted;
	size_t length;
	size_t index;
	int a;
	int b;

	/* The same length. */
	wanted = data;
	length = strlen(wanted);
	if (length != size)
		return 0;

	/* Each character, the ASCII letters folded. */
	for (index = 0U; index < size; index++) {
		/* The two characters, small. */
		a = (unsigned char)name[index];
		b = (unsigned char)wanted[index];
		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return 0;
	}

	/* The one looked for. */
	return WSL_RECORD_FOUND;
}

/*
 * Adds a keyword's line to the block of *text (malloc'd, *size bytes),
 * which becomes the new text.  Returns 0, EINVAL or ENOMEM.
 */
static int
wsl_record_add(
	char **text,
	size_t *size,
	unsigned block,
	const char *keyword,
	const char *value)
{
	struct terminal_hosts hosts;
	char *made;
	size_t made_size;
	int status;

	/* The text read again, and the line added to the block. */
	status = terminal_hosts_read(&hosts, *text, *size);
	if (status != 0)
		return status;
	status = terminal_hosts_add(&hosts, block, keyword, value, &made, &made_size);
	terminal_hosts_release(&hosts);
	if (status != 0)
		return status;

	/* Succeeded: the new text in the old one's place. */
	free(*text);
	*text = made;
	*size = made_size;
	return 0;
}
