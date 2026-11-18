/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where Mail keeps the accounts' passwords (WS169 p004).
 *
 * 2026-10-06 user (plan/ws169/phase001/phase.md): for now a file only its
 * owner reads, ~/.config/keiland/mailer-accounts (mode 0600), one line
 * "address<TAB>password" for each account, in plain text.  These two
 * functions are the only ones that read or write it, so that the store of
 * secrets the desktop gets later replaces them alone
 * (plan/ws177/backlog-p2.md).
 */

#include "mailer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The file's name in the folder of the desktop's settings, and the name it is written under first. */
#define SECRET_NAME		"mailer-accounts"
#define SECRET_NEW_NAME		"mailer-accounts.new"

/* The most accounts the file keeps (two lines for each account Mail can have). */
#define SECRET_LINES_MAX	(ML_ACCOUNTS_MAX * 2U)

/* One line of the file: the account's address and its password. */
struct secret_line {
	char address[ML_TEXT_MAX];
	char password[ML_TEXT_MAX];
};

static int secret_read(const char *folder, struct secret_line *lines, size_t *count);

/*
 * Finds the password kept for an address.  Returns 0 with it, ENOENT when
 * none is kept, or an errno value of the file.
 */
int
ml_secret_load(
	const char *folder,
	const char *address,
	char *password,
	size_t size)
{
	struct secret_line lines[SECRET_LINES_MAX];
	size_t count;
	size_t index;
	int same;
	int error;

	/* The lines. */
	password[0] = '\0';
	error = secret_read(folder, lines, &count);
	if (error != 0)
		return error;

	/* The address's. */
	for (index = 0; index < count; index++) {
		same = strcmp(lines[index].address, address);
		if (same == 0) {
			(void)snprintf(password, size, "%s", lines[index].password);
			memset(lines, 0, sizeof(lines));
			return 0;
		}
	}

	/* None kept. */
	memset(lines, 0, sizeof(lines));
	return ENOENT;
}

/*
 * Keeps the password of an address (in place of an earlier one), or
 * forgets it when the password is empty (ws177-p015: an account removed,
 * or its address changed): the file written anew beside the old one with
 * mode 0600, then renamed over it.  Returns 0 or an errno value of the
 * file.
 */
int
ml_secret_save(
	const char *folder,
	const char *address,
	const char *password)
{
	struct secret_line lines[SECRET_LINES_MAX];
	char path[ML_PATH_MAX];
	char fresh[ML_PATH_MAX];
	size_t count;
	size_t index;
	const char *breaks;
	FILE *file;
	int same;
	int fd;
	int error;
	int status;

	/* A line end in the password, or a tab in the address, would break the line. */
	breaks = strchr(password, '\n');
	if (breaks == NULL)
		breaks = strchr(address, '\t');
	if (breaks != NULL)
		return EINVAL;

	/* The lines now (none when the file is not there). */
	error = secret_read(folder, lines, &count);
	if (error == ENOENT)
		count = 0;
	else if (error != 0)
		return error;

	/* The address's line replaced, or added. */
	for (index = 0; index < count; index++) {
		same = strcmp(lines[index].address, address);
		if (same == 0)
			break;
	}

	/* Forgetting: the address's line goes, the later ones move up (an address without one has nothing to forget). */
	if (password[0] == '\0') {
		if (index == count) {
			memset(lines, 0, sizeof(lines));
			return 0;
		}

		/* The later lines move up over it, and the last place is wiped. */
		while (index + 1U < count) {
			lines[index] = lines[index + 1U];
			index++;
		}

		/* One line fewer. */
		count--;
		memset(&lines[count], 0, sizeof(lines[count]));
	}

	/* A new address: one more line. */
	if (password[0] != '\0' && index == count) {
		if (count == SECRET_LINES_MAX)
			return ENOSPC;
		count++;
	}

	/* The line's address and password. */
	if (password[0] != '\0') {
		(void)snprintf(lines[index].address, sizeof(lines[index].address), "%s", address);
		(void)snprintf(lines[index].password, sizeof(lines[index].password), "%s", password);
	}

	/* The new file, its owner's only. */
	(void)snprintf(path, sizeof(path), "%s/%s", folder, SECRET_NAME);
	(void)snprintf(fresh, sizeof(fresh), "%s/%s", folder, SECRET_NEW_NAME);
	fd = open(fresh, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		error = errno;
		memset(lines, 0, sizeof(lines));
		return error;
	}

	/* Its owner's only, whatever the umask; written through a stream. */
	(void)fchmod(fd, 0600);
	file = fdopen(fd, "w");
	if (file == NULL) {
		error = errno;
		(void)close(fd);
		memset(lines, 0, sizeof(lines));
		return error;
	}

	/* Each line. */
	for (index = 0; index < count; index++)
		(void)fprintf(file, "%s\t%s\n", lines[index].address, lines[index].password);
	memset(lines, 0, sizeof(lines));

	/* Written out. */
	status = fclose(file);
	if (status != 0)
		return EIO;

	/* In place of the old file. */
	status = rename(fresh, path);
	if (status != 0)
		return errno;

	/* Succeeded: the password is kept. */
	return 0;
}

/* Reads the file's lines (ENOENT when it is not there). */
static int
secret_read(
	const char *folder,
	struct secret_line *lines,
	size_t *count)
{
	char path[ML_PATH_MAX];
	char line[ML_TEXT_MAX * 2U + 4U];
	char *read_line;
	char *tab;
	char *end;
	FILE *file;

	/* The file. */
	*count = 0;
	(void)snprintf(path, sizeof(path), "%s/%s", folder, SECRET_NAME);
	file = fopen(path, "r");
	if (file == NULL)
		return errno;

	/* Each line "address<TAB>password". */
	while (*count < SECRET_LINES_MAX) {
		read_line = fgets(line, (int)sizeof(line), file);
		if (read_line == NULL)
			break;

		/* Without its line end. */
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';

		/* A line without its tab is skipped. */
		tab = strchr(line, '\t');
		if (tab == NULL)
			continue;
		*tab = '\0';

		/* Kept, each cut to its room. */
		(void)snprintf(lines[*count].address, sizeof(lines[*count].address), "%.*s", (int)(ML_TEXT_MAX - 1U), line);
		(void)snprintf(lines[*count].password, sizeof(lines[*count].password), "%.*s", (int)(ML_TEXT_MAX - 1U), tab + 1);
		(*count)++;
	}

	/* The last line's secret wiped, and the file closed. */
	memset(line, 0, sizeof(line));
	fclose(file);

	/* Succeeded: the lines are read. */
	return 0;
}
