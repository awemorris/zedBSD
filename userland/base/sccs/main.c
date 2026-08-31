/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sccs userland command.
 */

#include "userland/base/common/sccs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *program;

static const char *base_name(const char *path);
static int admin_main(int argc, char **argv);
static void error_path(const char *path);
static const char *current_user(void);
static int get_main(int argc, char **argv);
static int write_path(const char *path, const void *data, size_t size, int exclusive);
static int write_all(int fd, const void *data, size_t size);
static int keyword_output(int fd, const struct sccs_delta *delta, const char *module, int keep);
static int delta_main(int argc, char **argv);
static int read_pending(const char *path, char *old_sid, char *new_sid, char *user);
static int prs_main(int argc, char **argv);
static void prs_format(const char *format, const struct sccs_delta *delta);
static int val_main(int argc, char **argv);
static int what_main(int argc, char **argv);
static int sact_main(int argc, char **argv);
static int unget_main(int argc, char **argv);
static int rmdel_main(int argc, char **argv);
static int sccs_main(int argc, char **argv);

/*
 * Runs the sccs command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	program = base_name(argv[0]);

	/* Selects the matching value. */
	if (!strcmp(program, "admin")) {
		/* Obtains the admin main result. */
		function_result = admin_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "get")) {
		/* Obtains the get main result. */
		function_result = get_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "delta")) {
		/* Obtains the delta main result. */
		function_result = delta_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "prs")) {
		/* Obtains the prs main result. */
		function_result = prs_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "val")) {
		/* Obtains the val main result. */
		function_result = val_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "what")) {
		/* Obtains the what main result. */
		function_result = what_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "sact")) {
		/* Obtains the sact main result. */
		function_result = sact_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "unget")) {
		/* Obtains the unget main result. */
		function_result = unget_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "rmdel")) {
		/* Obtains the rmdel main result. */
		function_result = rmdel_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (!strcmp(program, "sccs")) {
		/* Obtains the sccs main result. */
		function_result = sccs_main(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports operation failure. */
	return 2;
}

/* Supports the base name operation. */
static const char *
base_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash ? slash + 1 : path;
}

/* Supports the admin main operation. */
static int
admin_main(
	int argc,
	char **argv)
{
	struct sccs_history history = {0};
	const char *input_path, *sid, *comment;
	char *text;
	size_t text_size;
	int ch, status;

	/* Parse each command-line option. */
	input_path = NULL;
	sid = "1.1";
	comment = "date and time created";
	text = NULL;
	text_size = 0;
	status = 1;
	while ((ch = getopt(argc, argv, "i:nr:y:")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'i':
			input_path = optarg;
			break;
		case 'n':
			break;
		case 'r':
			sid = optarg;
			break;
		case 'y':
			comment = optarg;
			break;
		default:
			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind + 1 != argc || !sccs_sid_valid(sid))
		return 2;

	/* Validates the command-line arguments. */
	if (!access(argv[optind], F_OK)) {
		errno = EEXIST;
		error_path(argv[optind]);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the input path condition. */
	if (input_path) {
		/* Handles the sccs read regular condition. */
		if (sccs_read_regular(input_path, &text, &text_size)) {
			error_path(input_path);

			/* Reports operation failure. */
			return 1;
		}
	} else {
		text = strdup("");

		/* Validates the current text. */
		if (!text)
			return 1;
	}

	/* Validates the command-line arguments. */
	if (sccs_add(&history, sid, current_user(), comment, text, text_size,
		     0) ||
	    sccs_save(argv[optind], &history))
		error_path(argv[optind]);
	else
		status = 0;
	free(text);
	sccs_free(&history);

	/* Returns the computed result. */
	return status;
}

/* Supports the error path operation. */
static void
error_path(
	const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

/* Supports the current user operation. */
static const char *
current_user(
	void)
{
	const char *name;
	static char numeric[32];

	name = getenv("LOGNAME");

	/* Validates the current name. */
	if (name && *name)
		return name;
	snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)getuid());

	/* Returns the computed result. */
	return numeric;
}

/* Supports the get main operation. */
static int
get_main(
	int argc,
	char **argv)
{
	char next[64], record[512];
	const struct sccs_delta *latest;
	int length;
	struct sccs_history history;
	const struct sccs_delta *delta;
	const char *sid;
	char *gfile, *pfile;
	int print, edit, keep, silent, ch, fd, status;

	/* Parse each command-line option. */
	sid = NULL;
	gfile = NULL;
	pfile = NULL;
	print = 0;
	edit = 0;
	keep = 0;
	silent = 0;
	fd = -1;
	status = 1;
	while ((ch = getopt(argc, argv, "ekpsr:")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'e':
			edit = 1;
			break;
		case 'k':
			keep = 1;
			break;
		case 'p':
			print = 1;
			break;
		case 's':
			silent = 1;
			break;
		case 'r':
			sid = optarg;
			break;
		default:
			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind + 1 != argc || (edit && print))
		return 2;

	/* Validates the command-line arguments. */
	if (sccs_load(argv[optind], &history)) {
		error_path(argv[optind]);

		/* Reports operation failure. */
		return 1;
	}
	delta = sccs_find(&history, sid);

	/* Handles the delta condition. */
	if (!delta) {
		errno = ENOENT;
		error_path(sid ? sid : argv[optind]);
		goto out;
	}
	gfile = sccs_gfile_name(argv[optind]);
	pfile = sccs_aux_name(argv[optind], 'p');

	/* Handles the gfile condition. */
	if (!gfile || !pfile)
		goto out;

	/* Handles the edit condition. */
	if (edit) {

		latest = sccs_find(&history, NULL);

		/* Handles a failed sccs sid next operation. */
		if (sccs_sid_next(delta->sid, delta != latest, next,
				  sizeof(next)))
			goto out;
		length = snprintf(record, sizeof(record), "%s %s %s\n",
				  delta->sid, next, current_user());

		/* Handles the write path condition. */
		if (write_path(pfile, record, (size_t)length, 1)) {
			error_path(pfile);
			goto out;
		}
	}
	fd = print ? STDOUT_FILENO
		   : open(gfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	/* Handles a failed keyword output operation. */
	if (fd < 0 ||
	    keyword_output(fd, delta, base_name(gfile), keep || edit)) {
		error_path(print ? "standard output" : gfile);

		/* Handles the edit condition. */
		if (edit)
			unlink(pfile);
		goto out;
	}

	/* Handles a failed close operation. */
	if (!print && close(fd))
		goto out;
	fd = -1;

	/* Handles the silent condition. */
	if (!silent)
		fprintf(stderr, "%s\n", delta->sid);
	status = 0;
out:

	/* Checks the file descriptor. */
	if (fd >= 0 && fd != STDOUT_FILENO)
		close(fd);
	free(gfile);
	free(pfile);
	sccs_free(&history);

	/* Returns the computed result. */
	return status;
}

/* Supports the write path operation. */
static int
write_path(
	const char *path,
	const void *data,
	size_t size,
	int exclusive)
{
	int saved;
	int flags;
	int fd;

	flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
	fd = open(path, flags, 0644);

	/* Checks the file descriptor. */
	if (fd < 0)
		return -1;

	/* Handles the write all condition. */
	if (write_all(fd, data, size) || fsync(fd) || close(fd)) {
				saved = errno;
		close(fd);

		/* Handles the exclusive condition. */
		if (exclusive)
			unlink(path);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const void *data,
	size_t size)
{
	ssize_t n;
	const char *p;

	/* Process each remaining element. */
	p = data;
	while (size) {

		n = write(fd, p, size);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0)
			return -1;
		p += (size_t)n;
		size -= (size_t)n;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the keyword output operation. */
static int
keyword_output(
	int fd,
	const struct sccs_delta *delta,
	const char *module,
	int keep)
{
	int function_result;
	const char *replacement;
	char combined[512];
	size_t i_index_for;

	/* Handles the keep condition. */
	if (keep) {
		/* Obtains the write all result. */
		function_result = write_all(fd, delta->text, delta->text_size);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < delta->text_size;) {

		replacement = NULL;

		/* Handles the i index for condition. */
		if (i_index_for + 3 <= delta->text_size && delta->text[i_index_for] == '%' &&
		    delta->text[i_index_for + 2] == '%') {
			/* Dispatch the selected operation case. */
			switch (delta->text[i_index_for + 1]) {
			case 'M':
				replacement = module;
				break;
			case 'I':
				replacement = delta->sid;
				break;
			case 'W':
				snprintf(combined, sizeof(combined),
					 "@(#)%s\t%s", module, delta->sid);
				replacement = combined;
				break;
			default:
				break;
			}
		}

		/* Handles the replacement condition. */
		if (replacement) {
			/* Handles the write all condition. */
			if (write_all(fd, replacement, strlen(replacement)))
				return -1;
			i_index_for += 3;
		} else if (write_all(fd, delta->text + i_index_for++, 1))

			/* Reports operation failure. */
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the delta main operation. */
static int
delta_main(
	int argc,
	char **argv)
{
	struct sccs_history history;
	const char *comment;
	char old_sid[64] = {0}, new_sid[64] = {0}, owner[128] = {0};
	char *gfile, *pfile, *text;
	const struct sccs_delta *old;
	size_t text_size;
	int ch, status;

	/* Parse each command-line option. */
	comment = "";
	gfile = NULL;
	pfile = NULL;
	text = NULL;
	status = 1;
	while ((ch = getopt(argc, argv, "y:")) != -1) {
		/* Handles the ch condition. */
		if (ch == 'y')
			comment = optarg;
		else

			/* Reports operation failure. */
			return 2;
	}

	/* Validates the command-line arguments. */
	if (optind + 1 != argc)
		return 2;
	gfile = sccs_gfile_name(argv[optind]);
	pfile = sccs_aux_name(argv[optind], 'p');

	/* Handles the selected command-line operation. */
	if (!gfile || !pfile || read_pending(pfile, old_sid, new_sid, owner) ||
	    strcmp(owner, current_user()) ||
	    sccs_read_regular(gfile, &text, &text_size) ||
	    sccs_load(argv[optind], &history)) {
		/* Handles the owner condition. */
		if (owner[0] && strcmp(owner, current_user()))
			errno = EPERM;
		error_path(argv[optind]);
		goto out_no_history;
	}
	old = sccs_find(&history, old_sid);

	/* Validates the command-line arguments. */
	if (!old ||
	    sccs_add(&history, new_sid, current_user(), comment, text,
		     text_size, old ? old->serial : 0) ||
	    sccs_save(argv[optind], &history)) {
		error_path(argv[optind]);
	} else {
		unlink(pfile);
		unlink(gfile);
		printf("%s\n", new_sid);
		status = 0;
	}
	sccs_free(&history);
out_no_history:
	free(gfile);
	free(pfile);
	free(text);

	/* Returns the computed result. */
	return status;
}

/* Supports the read pending operation. */
static int
read_pending(
	const char *path,
	char *old_sid,
	char *new_sid,
	char *user)
{
	char *data;
	char *old_word, *new_word, *user_word, *extra;
	size_t size;
	int valid;

	data = NULL;

	/* Handles the sccs read regular condition. */
	if (sccs_read_regular(path, &data, &size))
		return -1;
	old_word = strtok(data, " \t\r\n");
	new_word = strtok(NULL, " \t\r\n");
	user_word = strtok(NULL, " \t\r\n");
	extra = strtok(NULL, " \t\r\n");
	valid = old_word && new_word && user_word && !extra &&
		strlen(old_word) < 64 && strlen(new_word) < 64 &&
		strlen(user_word) < 128;

	/* Handles the valid condition. */
	if (valid) {
		strcpy(old_sid, old_word);
		strcpy(new_sid, new_word);
		strcpy(user, user_word);
	}
	free(data);

	/* Handles a failed sccs sid valid operation. */
	if (!valid || !sccs_sid_valid(old_sid) || !sccs_sid_valid(new_sid)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the prs main operation. */
static int
prs_main(
	int argc,
	char **argv)
{
	struct sccs_history history;
	const struct sccs_delta *delta;
	const char *sid, *format;
	int ch;

	/* Parse each command-line option. */
	sid = NULL;
	format = ":I:\t:D:\t:P:\t:C:";
	while ((ch = getopt(argc, argv, "d:r:")) != -1) {
		/* Handles the ch condition. */
		if (ch == 'd')
			format = optarg;
		else if (ch == 'r')
			sid = optarg;
		else

			/* Reports operation failure. */
			return 2;
	}

	/* Validates the command-line arguments. */
	if (optind + 1 != argc || sccs_load(argv[optind], &history)) {
		/* Validates the command-line arguments. */
		if (optind < argc)
			error_path(argv[optind]);

		/* Reports operation failure. */
		return 1;
	}
	delta = sccs_find(&history, sid);

	/* Handles the delta condition. */
	if (!delta) {
		errno = ENOENT;
		error_path(sid);
		sccs_free(&history);

		/* Reports operation failure. */
		return 1;
	}
	prs_format(format, delta);
	sccs_free(&history);

	/* Reports successful completion. */
	return 0;
}

/* Supports the prs format operation. */
static void
prs_format(
	const char *format,
	const struct sccs_delta *delta)
{
	const char *replacement;
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; format[i_index_for];) {

		replacement = NULL;

		/* Handles the format condition. */
		if (format[i_index_for] == ':' && format[i_index_for + 1] && format[i_index_for + 2] == ':') {
			/* Dispatch the selected operation case. */
			switch (format[i_index_for + 1]) {
			case 'I':
				replacement = delta->sid;
				break;
			case 'D':
				replacement = delta->timestamp;
				break;
			case 'P':
				replacement = delta->user;
				break;
			case 'C':
				replacement = delta->comment;
				break;
			default:
				break;
			}
		}

		/* Handles the replacement condition. */
		if (replacement) {
			fputs(replacement, stdout);
			i_index_for += 3;
		} else
			putchar(format[i_index_for++]);
	}
	putchar('\n');
}

/* Supports the val main operation. */
static int
val_main(
	int argc,
	char **argv)
{
	struct sccs_history history;
	int i_index_for;
	int failed;

	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 2)
		return 2;

	/* Process each remaining command-line operand. */
	for (i_index_for = 1; i_index_for < argc; i_index_for++) {
		/* Validates the command-line arguments. */
		if (sccs_load(argv[i_index_for], &history)) {
			error_path(argv[i_index_for]);
			failed = 1;
		} else
			sccs_free(&history);
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the what main operation. */
static int
what_main(
	int argc,
	char **argv)
{
	char *data;
	size_t size;
	int i_index_for;
	size_t p_index_for;
	int failed;

	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 2)
		return 2;

	/* Process each remaining command-line operand. */
	for (i_index_for = 1; i_index_for < argc; i_index_for++) {
		/* Validates the command-line arguments. */
		if (sccs_read_regular(argv[i_index_for], &data, &size)) {
			error_path(argv[i_index_for]);
			failed = 1;
			continue;
		}

		/* Process each remaining element. */
		for (p_index_for = 0; p_index_for + 4 <= size; p_index_for++) {
			/* Handles the memcmp condition. */
			if (memcmp(data + p_index_for, "@(#)", 4))
				continue;
			p_index_for += 4;
			fputs("\t", stdout);

			/* Process each remaining element. */
			while (p_index_for < size && data[p_index_for] != '\n' && data[p_index_for] != '"' &&
			       data[p_index_for] != '>' && data[p_index_for] != '\0')
				putchar((unsigned char)data[p_index_for++]);
			putchar('\n');
		}
		free(data);
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the sact main operation. */
static int
sact_main(
	int argc,
	char **argv)
{
	char *pfile;
	char *data;
	size_t size;
	int status;

	/* Validates the command-line arguments. */
	if (argc != 2)
		return 2;
	pfile = sccs_aux_name(argv[1], 'p');

	/* Handles a failed sccs read regular operation. */
	if (!pfile || sccs_read_regular(pfile, &data, &size)) {
		error_path(pfile ? pfile : argv[1]);
		free(pfile);

		/* Reports operation failure. */
		return 1;
	}
	status = write_all(STDOUT_FILENO, data, size) != 0;
	free(data);
	free(pfile);

	/* Returns the computed result. */
	return status;
}

/* Supports the unget main operation. */
static int
unget_main(
	int argc,
	char **argv)
{
	char old_sid[64], new_sid[64], owner[128];
	char *pfile, *gfile;
	int status;

	/* Validates the command-line arguments. */
	if (argc != 2)
		return 2;
	pfile = sccs_aux_name(argv[1], 'p');
	gfile = sccs_gfile_name(argv[1]);

	/* Handles a failed read pending operation. */
	if (!pfile || !gfile || read_pending(pfile, old_sid, new_sid, owner) ||
	    strcmp(owner, current_user())) {
		errno = EPERM;
		error_path(argv[1]);
		free(pfile);
		free(gfile);

		/* Reports operation failure. */
		return 1;
	}
	status = unlink(pfile);

	/* Checks the operation status. */
	if (!status)
		unlink(gfile);
	free(pfile);
	free(gfile);

	/* Returns the computed result. */
	return status != 0;
}

/* Supports the rmdel main operation. */
static int
rmdel_main(
	int argc,
	char **argv)
{
	struct sccs_history history;
	const char *sid;
	int ch, status;

	/* Parse each command-line option. */
	sid = NULL;
	status = 1;
	while ((ch = getopt(argc, argv, "r:")) != -1)

		/* Handles the ch condition. */
		if (ch == 'r')
			sid = optarg;
		else

			/* Reports operation failure. */
			return 2;

	/* Validates the command-line arguments. */
	if (!sid || optind + 1 != argc)
		return 2;

	/* Validates the command-line arguments. */
	if (sccs_load(argv[optind], &history) || sccs_remove(&history, sid) ||
	    sccs_save(argv[optind], &history))
		error_path(argv[optind]);
	else
		status = 0;
	sccs_free(&history);

	/* Returns the computed result. */
	return status;
}

/* Supports the sccs main operation. */
static int
sccs_main(
	int argc,
	char **argv)
{
	size_t i_index_for;
	static const char *const commands[] = {"admin", "delta", "get",
					       "prs",	"rmdel", "sact",
					       "unget", "val",	 "what"};
	char path[1024];
	const char *slash;
	int allowed;

	allowed = 0;

	/* Validates the command-line arguments. */
	if (argc < 2)
		return 2;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < sizeof(commands) / sizeof(commands[0]); i_index_for++)

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[1], commands[i_index_for]))
			allowed = 1;

	/* Handles the allowed condition. */
	if (!allowed) {
		fprintf(stderr, "sccs: unknown command: %s\n", argv[1]);

		/* Reports operation failure. */
		return 2;
	}
	slash = strrchr(argv[0], '/');

	/* Handles the slash condition. */
	if (slash)
		snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - argv[0]),
			 argv[0], argv[1]);
	else
		snprintf(path, sizeof(path), "/bin/%s", argv[1]);
	argv[1] = path;
	execv(path, argv + 1);
	error_path(path);

	/* Reports operation failure. */
	return 1;
}
