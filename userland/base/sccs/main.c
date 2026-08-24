/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static const char *
base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void
error_path(const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

static const char *
current_user(void)
{
	const char *name = getenv("LOGNAME");
	static char numeric[32];
	if (name && *name)
		return name;
	snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)getuid());
	return numeric;
}

static int
write_all(int fd, const void *data, size_t size)
{
	const char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += (size_t)n;
		size -= (size_t)n;
	}
	return 0;
}

static int
write_path(const char *path, const void *data, size_t size, int exclusive)
{
	int flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
	int fd = open(path, flags, 0644);
	if (fd < 0)
		return -1;
	if (write_all(fd, data, size) || fsync(fd) || close(fd)) {
		int saved = errno;
		close(fd);
		if (exclusive)
			unlink(path);
		errno = saved;
		return -1;
	}
	return 0;
}

static int
admin_main(int argc, char **argv)
{
	struct sccs_history history = {0};
	const char *input_path = NULL, *sid = "1.1",
		   *comment = "date and time created";
	char *text = NULL;
	size_t text_size = 0;
	int ch, status = 1;
	while ((ch = getopt(argc, argv, "i:nr:y:")) != -1) {
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
			return 2;
		}
	}
	if (optind + 1 != argc || !sccs_sid_valid(sid))
		return 2;
	if (!access(argv[optind], F_OK)) {
		errno = EEXIST;
		error_path(argv[optind]);
		return 1;
	}
	if (input_path) {
		if (sccs_read_regular(input_path, &text, &text_size)) {
			error_path(input_path);
			return 1;
		}
	} else {
		text = strdup("");
		if (!text)
			return 1;
	}
	if (sccs_add(&history, sid, current_user(), comment, text, text_size,
		     0) ||
	    sccs_save(argv[optind], &history))
		error_path(argv[optind]);
	else
		status = 0;
	free(text);
	sccs_free(&history);
	return status;
}

static int
keyword_output(int fd, const struct sccs_delta *delta, const char *module,
	       int keep)
{
	if (keep)
		return write_all(fd, delta->text, delta->text_size);
	for (size_t i = 0; i < delta->text_size;) {
		const char *replacement = NULL;
		char combined[512];
		if (i + 3 <= delta->text_size && delta->text[i] == '%' &&
		    delta->text[i + 2] == '%') {
			switch (delta->text[i + 1]) {
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
		if (replacement) {
			if (write_all(fd, replacement, strlen(replacement)))
				return -1;
			i += 3;
		} else if (write_all(fd, delta->text + i++, 1))
			return -1;
	}
	return 0;
}

static int
get_main(int argc, char **argv)
{
	struct sccs_history history;
	const struct sccs_delta *delta;
	const char *sid = NULL;
	char *gfile = NULL, *pfile = NULL;
	int print = 0, edit = 0, keep = 0, silent = 0, ch, fd = -1, status = 1;
	while ((ch = getopt(argc, argv, "ekpsr:")) != -1) {
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
			return 2;
		}
	}
	if (optind + 1 != argc || (edit && print))
		return 2;
	if (sccs_load(argv[optind], &history)) {
		error_path(argv[optind]);
		return 1;
	}
	delta = sccs_find(&history, sid);
	if (!delta) {
		errno = ENOENT;
		error_path(sid ? sid : argv[optind]);
		goto out;
	}
	gfile = sccs_gfile_name(argv[optind]);
	pfile = sccs_aux_name(argv[optind], 'p');
	if (!gfile || !pfile)
		goto out;
	if (edit) {
		char next[64], record[512];
		const struct sccs_delta *latest = sccs_find(&history, NULL);
		if (sccs_sid_next(delta->sid, delta != latest, next,
				  sizeof(next)))
			goto out;
		int length = snprintf(record, sizeof(record), "%s %s %s\n",
				      delta->sid, next, current_user());
		if (write_path(pfile, record, (size_t)length, 1)) {
			error_path(pfile);
			goto out;
		}
	}
	fd = print ? STDOUT_FILENO
		   : open(gfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 ||
	    keyword_output(fd, delta, base_name(gfile), keep || edit)) {
		error_path(print ? "standard output" : gfile);
		if (edit)
			unlink(pfile);
		goto out;
	}
	if (!print && close(fd))
		goto out;
	fd = -1;
	if (!silent)
		fprintf(stderr, "%s\n", delta->sid);
	status = 0;
out:
	if (fd >= 0 && fd != STDOUT_FILENO)
		close(fd);
	free(gfile);
	free(pfile);
	sccs_free(&history);
	return status;
}

static int
read_pending(const char *path, char *old_sid, char *new_sid, char *user)
{
	char *data = NULL;
	size_t size;
	if (sccs_read_regular(path, &data, &size))
		return -1;
	char *old_word = strtok(data, " \t\r\n");
	char *new_word = strtok(NULL, " \t\r\n");
	char *user_word = strtok(NULL, " \t\r\n");
	char *extra = strtok(NULL, " \t\r\n");
	int valid = old_word && new_word && user_word && !extra &&
		    strlen(old_word) < 64 && strlen(new_word) < 64 &&
		    strlen(user_word) < 128;
	if (valid) {
		strcpy(old_sid, old_word);
		strcpy(new_sid, new_word);
		strcpy(user, user_word);
	}
	free(data);
	if (!valid || !sccs_sid_valid(old_sid) || !sccs_sid_valid(new_sid)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int
delta_main(int argc, char **argv)
{
	struct sccs_history history;
	const char *comment = "";
	char old_sid[64] = {0}, new_sid[64] = {0}, owner[128] = {0};
	char *gfile = NULL, *pfile = NULL, *text = NULL;
	size_t text_size;
	int ch, status = 1;
	while ((ch = getopt(argc, argv, "y:")) != -1) {
		if (ch == 'y')
			comment = optarg;
		else
			return 2;
	}
	if (optind + 1 != argc)
		return 2;
	gfile = sccs_gfile_name(argv[optind]);
	pfile = sccs_aux_name(argv[optind], 'p');
	if (!gfile || !pfile || read_pending(pfile, old_sid, new_sid, owner) ||
	    strcmp(owner, current_user()) ||
	    sccs_read_regular(gfile, &text, &text_size) ||
	    sccs_load(argv[optind], &history)) {
		if (owner[0] && strcmp(owner, current_user()))
			errno = EPERM;
		error_path(argv[optind]);
		goto out_no_history;
	}
	const struct sccs_delta *old = sccs_find(&history, old_sid);
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
	return status;
}

static void
prs_format(const char *format, const struct sccs_delta *delta)
{
	for (size_t i = 0; format[i];) {
		const char *replacement = NULL;
		if (format[i] == ':' && format[i + 1] && format[i + 2] == ':') {
			switch (format[i + 1]) {
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
		if (replacement) {
			fputs(replacement, stdout);
			i += 3;
		} else
			putchar(format[i++]);
	}
	putchar('\n');
}

static int
prs_main(int argc, char **argv)
{
	struct sccs_history history;
	const char *sid = NULL, *format = ":I:\t:D:\t:P:\t:C:";
	int ch;
	while ((ch = getopt(argc, argv, "d:r:")) != -1) {
		if (ch == 'd')
			format = optarg;
		else if (ch == 'r')
			sid = optarg;
		else
			return 2;
	}
	if (optind + 1 != argc || sccs_load(argv[optind], &history)) {
		if (optind < argc)
			error_path(argv[optind]);
		return 1;
	}
	const struct sccs_delta *delta = sccs_find(&history, sid);
	if (!delta) {
		errno = ENOENT;
		error_path(sid);
		sccs_free(&history);
		return 1;
	}
	prs_format(format, delta);
	sccs_free(&history);
	return 0;
}

static int
val_main(int argc, char **argv)
{
	int failed = 0;
	if (argc < 2)
		return 2;
	for (int i = 1; i < argc; i++) {
		struct sccs_history history;
		if (sccs_load(argv[i], &history)) {
			error_path(argv[i]);
			failed = 1;
		} else
			sccs_free(&history);
	}
	return failed;
}

static int
what_main(int argc, char **argv)
{
	int failed = 0;
	if (argc < 2)
		return 2;
	for (int i = 1; i < argc; i++) {
		char *data;
		size_t size;
		if (sccs_read_regular(argv[i], &data, &size)) {
			error_path(argv[i]);
			failed = 1;
			continue;
		}
		for (size_t p = 0; p + 4 <= size; p++) {
			if (memcmp(data + p, "@(#)", 4))
				continue;
			p += 4;
			fputs("\t", stdout);
			while (p < size && data[p] != '\n' && data[p] != '"' &&
			       data[p] != '>' && data[p] != '\0')
				putchar((unsigned char)data[p++]);
			putchar('\n');
		}
		free(data);
	}
	return failed;
}

static int
sact_main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	char *pfile = sccs_aux_name(argv[1], 'p');
	char *data;
	size_t size;
	if (!pfile || sccs_read_regular(pfile, &data, &size)) {
		error_path(pfile ? pfile : argv[1]);
		free(pfile);
		return 1;
	}
	int status = write_all(STDOUT_FILENO, data, size) != 0;
	free(data);
	free(pfile);
	return status;
}

static int
unget_main(int argc, char **argv)
{
	char old_sid[64], new_sid[64], owner[128];
	if (argc != 2)
		return 2;
	char *pfile = sccs_aux_name(argv[1], 'p');
	char *gfile = sccs_gfile_name(argv[1]);
	if (!pfile || !gfile || read_pending(pfile, old_sid, new_sid, owner) ||
	    strcmp(owner, current_user())) {
		errno = EPERM;
		error_path(argv[1]);
		free(pfile);
		free(gfile);
		return 1;
	}
	int status = unlink(pfile);
	if (!status)
		unlink(gfile);
	free(pfile);
	free(gfile);
	return status != 0;
}

static int
rmdel_main(int argc, char **argv)
{
	struct sccs_history history;
	const char *sid = NULL;
	int ch, status = 1;
	while ((ch = getopt(argc, argv, "r:")) != -1)
		if (ch == 'r')
			sid = optarg;
		else
			return 2;
	if (!sid || optind + 1 != argc)
		return 2;
	if (sccs_load(argv[optind], &history) || sccs_remove(&history, sid) ||
	    sccs_save(argv[optind], &history))
		error_path(argv[optind]);
	else
		status = 0;
	sccs_free(&history);
	return status;
}

static int
sccs_main(int argc, char **argv)
{
	static const char *const commands[] = {"admin", "delta", "get",
					       "prs",	"rmdel", "sact",
					       "unget", "val",	 "what"};
	char path[1024];
	const char *slash;
	int allowed = 0;
	if (argc < 2)
		return 2;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		if (!strcmp(argv[1], commands[i]))
			allowed = 1;
	if (!allowed) {
		fprintf(stderr, "sccs: unknown command: %s\n", argv[1]);
		return 2;
	}
	slash = strrchr(argv[0], '/');
	if (slash)
		snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - argv[0]),
			 argv[0], argv[1]);
	else
		snprintf(path, sizeof(path), "/bin/%s", argv[1]);
	argv[1] = path;
	execv(path, argv + 1);
	error_path(path);
	return 1;
}

int
main(int argc, char **argv)
{
	program = base_name(argv[0]);
	if (!strcmp(program, "admin"))
		return admin_main(argc, argv);
	if (!strcmp(program, "get"))
		return get_main(argc, argv);
	if (!strcmp(program, "delta"))
		return delta_main(argc, argv);
	if (!strcmp(program, "prs"))
		return prs_main(argc, argv);
	if (!strcmp(program, "val"))
		return val_main(argc, argv);
	if (!strcmp(program, "what"))
		return what_main(argc, argv);
	if (!strcmp(program, "sact"))
		return sact_main(argc, argv);
	if (!strcmp(program, "unget"))
		return unget_main(argc, argv);
	if (!strcmp(program, "rmdel"))
		return rmdel_main(argc, argv);
	if (!strcmp(program, "sccs"))
		return sccs_main(argc, argv);
	return 2;
}
