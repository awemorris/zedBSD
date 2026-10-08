/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's accounts on the disk (WS169 p004): ~/.config/keiland/mailer.conf
 * holds what is not secret, an "account" line starting each account and
 * its "key=value" lines after (name, address, user, imap, smtp, and
 * imap_pin and smtp_pin for the certificates the user trusts though they
 * do not verify, ws177-p015); the passwords are secret.c's.  The file is written anew beside the old one
 * and renamed over it.
 */

#include "mailer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The folder under the home, and the file's names. */
#define ACCOUNT_FOLDER		".config/keiland"
#define ACCOUNT_NAME		"mailer.conf"
#define ACCOUNT_NEW_NAME	"mailer.conf.new"

static void account_server_text(const struct ml_server *server, char *text, size_t size);
static void account_set(struct ml_account_config *account, const char *key, const char *value);

/*
 * Gives the folder of the desktop's settings in the home (made when it is
 * not there).  Returns 0, or ENOENT without a home.
 */
int
ml_config_folder(
	char *folder,
	size_t size)
{
	const char *home;

	/* The home. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return ENOENT;

	/* Its .config and the desktop's folder in it (they may be there already). */
	(void)snprintf(folder, size, "%s/.config", home);
	(void)mkdir(folder, 0700);
	(void)snprintf(folder, size, "%s/%s", home, ACCOUNT_FOLDER);
	(void)mkdir(folder, 0700);

	/* Succeeded: the folder's path. */
	return 0;
}

/*
 * Reads the accounts and their passwords.  Returns 0 (none when the file
 * is not there) or an errno value of the file.
 */
int
ml_accounts_load(
	const char *folder,
	struct ml_account_config *accounts,
	size_t capacity,
	size_t *count)
{
	char path[ML_PATH_MAX];
	char line[ML_TEXT_MAX * 2U];
	char *equals;
	char *end;
	FILE *file;
	size_t index;
	int same;
	int error;

	/* The file; none is no account. */
	*count = 0;
	(void)snprintf(path, sizeof(path), "%s/%s", folder, ACCOUNT_NAME);
	file = fopen(path, "r");
	if (file == NULL) {
		error = errno;
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Each line. */
	for (;;) {
		end = fgets(line, (int)sizeof(line), file);
		if (end == NULL)
			break;

		/* Without its line end. */
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';

		/* "account" starts the next account. */
		same = strcmp(line, "account");
		if (same == 0) {
			if (*count == capacity)
				break;
			memset(&accounts[*count], 0, sizeof(accounts[*count]));
			(*count)++;
			continue;
		}

		/* A key and its value, of the account being read. */
		equals = strchr(line, '=');
		if (equals == NULL || *count == 0U)
			continue;
		*equals = '\0';
		account_set(&accounts[*count - 1U], line, equals + 1);
	}

	/* The file is read. */
	fclose(file);

	/* Each account's password. */
	for (index = 0; index < *count; index++)
		(void)ml_secret_load(folder, accounts[index].address, accounts[index].password, sizeof(accounts[index].password));

	/* Succeeded: the accounts are read. */
	return 0;
}

/*
 * Writes the accounts (and their passwords to secret.c's file).  Returns
 * 0 or an errno value of the files.
 */
int
ml_accounts_save(
	const char *folder,
	const struct ml_account_config *accounts,
	size_t count)
{
	char path[ML_PATH_MAX];
	char fresh[ML_PATH_MAX];
	char imap[ML_TEXT_MAX + 16U];
	char smtp[ML_TEXT_MAX + 16U];
	FILE *file;
	size_t index;
	int status;
	int error;

	/* The new file. */
	(void)snprintf(path, sizeof(path), "%s/%s", folder, ACCOUNT_NAME);
	(void)snprintf(fresh, sizeof(fresh), "%s/%s", folder, ACCOUNT_NEW_NAME);
	file = fopen(fresh, "w");
	if (file == NULL)
		return errno;

	/* Each account, what is not secret. */
	for (index = 0; index < count; index++) {
		account_server_text(&accounts[index].imap, imap, sizeof(imap));
		account_server_text(&accounts[index].smtp, smtp, sizeof(smtp));
		(void)fprintf(file, "account\nname=%s\naddress=%s\nuser=%s\nimap=%s\nsmtp=%s\n",
		    accounts[index].name, accounts[index].address, accounts[index].user, imap, smtp);

		/* The certificates the user trusts. */
		if (accounts[index].imap.pin[0] != '\0')
			(void)fprintf(file, "imap_pin=%s\n", accounts[index].imap.pin);
		if (accounts[index].smtp.pin[0] != '\0')
			(void)fprintf(file, "smtp_pin=%s\n", accounts[index].smtp.pin);
	}

	/* Written out, and in place of the old one. */
	status = fclose(file);
	if (status != 0)
		return EIO;
	status = rename(fresh, path);
	if (status != 0)
		return errno;

	/* The passwords. */
	for (index = 0; index < count; index++) {
		error = ml_secret_save(folder, accounts[index].address, accounts[index].password);
		if (error != 0)
			return error;
	}

	/* Succeeded: the accounts are kept. */
	return 0;
}

/* Writes a server as "host:port". */
static void
account_server_text(
	const struct ml_server *server,
	char *text,
	size_t size)
{
	/* The host and its port. */
	(void)snprintf(text, size, "%s:%u", server->host, server->port);
}

/* Sets one key of an account read from the file. */
static void
account_set(
	struct ml_account_config *account,
	const char *key,
	const char *value)
{
	char pin[ML_PIN_MAX];
	int same;

	/* The user's name. */
	same = strcmp(key, "name");
	if (same == 0) {
		(void)snprintf(account->name, sizeof(account->name), "%s", value);
		return;
	}

	/* The address. */
	same = strcmp(key, "address");
	if (same == 0) {
		(void)snprintf(account->address, sizeof(account->address), "%s", value);
		return;
	}

	/* The user name the servers take. */
	same = strcmp(key, "user");
	if (same == 0) {
		(void)snprintf(account->user, sizeof(account->user), "%s", value);
		return;
	}

	/* The IMAP server (its pin, read before or after it, is kept). */
	same = strcmp(key, "imap");
	if (same == 0) {
		(void)snprintf(pin, sizeof(pin), "%s", account->imap.pin);
		(void)ml_server_parse(value, 993U, &account->imap);
		(void)snprintf(account->imap.pin, sizeof(account->imap.pin), "%s", pin);
		return;
	}

	/* The SMTP server (likewise). */
	same = strcmp(key, "smtp");
	if (same == 0) {
		(void)snprintf(pin, sizeof(pin), "%s", account->smtp.pin);
		(void)ml_server_parse(value, 465U, &account->smtp);
		(void)snprintf(account->smtp.pin, sizeof(account->smtp.pin), "%s", pin);
		return;
	}

	/* The IMAP server's trusted certificate. */
	same = strcmp(key, "imap_pin");
	if (same == 0) {
		(void)snprintf(account->imap.pin, sizeof(account->imap.pin), "%s", value);
		return;
	}

	/* The SMTP server's. */
	same = strcmp(key, "smtp_pin");
	if (same == 0)
		(void)snprintf(account->smtp.pin, sizeof(account->smtp.pin), "%s", value);
}
