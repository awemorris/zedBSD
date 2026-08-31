/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ls userland command.
 */

#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_PATH_CAPACITY 1024U
#define LS_RECURSION_LIMIT 64

struct options {
	int all, directory, classify, human, long_format, recursive, reverse,
	    time_sort, one, columns;
};
struct entry {
	char *name;
	struct stat status;
	int status_valid;
};

struct long_widths {
	size_t links, user, group, size;
};

static int list_operand(const char *path, const struct options *o, int header);
static int list_directory(const char *path, const struct options *o, int header, int depth);
static int load(const char *path, const struct options *o, struct entry **result, size_t *result_count);
static char *copy_string(const char *text);
static int join_path(const char *directory, const char *name, char *out, size_t cap);
static void sort_entries(struct entry *items, size_t count, const struct options *o);
static int compare(const struct entry *a, const struct entry *b, const struct options *o);
static int print_entries(const char *path, struct entry *items, size_t count, const struct options *o);
static void measure_long(const struct entry *items, size_t count, const struct options *o, struct long_widths *w);
static void human_size(off_t value, char out[16]);
static const char *uid_name(uid_t id, char out[24]);
static const char *gid_name(gid_t id, char out[24]);
static int print_long(const char *directory, const struct entry *item, const struct options *o, const struct long_widths *w);
static void mode_text(mode_t m, char out[11]);
static char type_char(mode_t m);
static void ls_time(time_t value, char out[32]);
static int leap(long long y);
static void print_name(const struct entry *item, const struct options *o);
static char suffix(mode_t mode);
static void free_entries(struct entry *items, size_t count);

/*
 * Runs the ls command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *p;
	struct options o = {0};
	int index, failed, operands;

	/* Process each remaining command-line operand. */
	index = 1;
	failed = 0;
	for (; index < argc && argv[index][0] == '-' && argv[index][1];
	     index++) {
		p = argv[index] + 1;

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[index], "--")) {
			index++;
			break;
		}

		/* Process each element required by the operation. */
		for (; *p; p++) {
			/* Dispatch the selected operation case. */
			switch (*p) {
			case 'a':
				o.all = 1;
				break;
			case 'd':
				o.directory = 1;
				break;
			case 'F':
				o.classify = 1;
				break;
			case 'h':
				o.human = 1;
				break;
			case 'l':
				o.long_format = 1;
				o.one = 1;
				break;
			case 'R':
				o.recursive = 1;
				break;
			case 'r':
				o.reverse = 1;
				break;
			case 't':
				o.time_sort = 1;
				break;
			case '1':
				o.one = 1;
				o.columns = 0;
				break;
			case 'C':
				o.columns = 1;
				o.one = 0;
				break;
			default:
				fprintf(stderr,
					"usage: ls [-adFhlRrt1C] [file...]\n");

				/* Reports operation failure. */
				return 1;
			}
		}
	}
	operands = argc - index;

	/* Handles the operands condition. */
	if (!operands) {
		/* Computes the function result. */
		function_result = list_operand(".", &o, 0) ? 0 : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (!list_operand(argv[index], &o, operands > 1))
			failed = 1;

		/* Validates the command-line arguments. */
		if (index + 1 < argc)
			putchar('\n');
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the list operand operation. */
static int
list_operand(
	const char *path,
	const struct options *o,
	int header)
{
	int function_result;
	struct long_widths widths;
	struct stat status;
	struct entry item;

	/* Handles the lstat condition. */
	if (lstat(path, &status)) {
		command_error("ls", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed S ISDIR operation. */
	if (S_ISDIR(status.st_mode) && !o->directory) {
		/* Obtains the list directory result. */
		function_result = list_directory(path, o, header, 0);

		/* Returns the computed result. */
		return function_result;
	}

	memset(&item, 0, sizeof(item));
	item.name = (char *)path;
	item.status = status;
	item.status_valid = 1;

	/* Handles the o condition. */
	if (o->long_format) {
		measure_long(&item, 1, o, &widths);

		/* Obtains the print long result. */
		function_result = print_long("", &item, o, &widths);

		/* Returns the computed result. */
		return function_result;
	}
	print_name(&item, o);
	putchar('\n');

	/* Reports operation failure. */
	return 1;
}

/* Supports the list directory operation. */
static int
list_directory(
	const char *path,
	const struct options *o,
	int header,
	int depth)
{
	char child[LS_PATH_CAPACITY];
	struct entry *items;
	size_t count, i;
	int ok;

	ok = 1;

	/* Handles the depth condition. */
	if (depth > LS_RECURSION_LIMIT) {
		errno = ELOOP;
		command_error("ls", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed load operation. */
	if (!load(path, o, &items, &count)) {
		command_error("ls", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the header condition. */
	if (header)
		printf("%s:\n", path);

	/* Handles a failed print entries operation. */
	if (!print_entries(path, items, count, o))
		ok = 0;

	/* Handles the o condition. */
	if (o->recursive) {
		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			/* Handles a failed S ISDIR operation. */
			if (!items[i].status_valid ||
			    !S_ISDIR(items[i].status.st_mode) ||
			    !strcmp(items[i].name, ".") ||
			    !strcmp(items[i].name, ".."))
				continue;

			/* Handles a failed join path operation. */
			if (!join_path(path, items[i].name, child,
				       sizeof(child))) {
				command_error("ls", items[i].name);
				ok = 0;
				continue;
			}
			putchar('\n');

			/* Handles a failed list directory operation. */
			if (!list_directory(child, o, 1, depth + 1))
				ok = 0;
		}
	}
	free_entries(items, count);

	/* Returns the computed result. */
	return ok;
}

/* Supports the load operation. */
static int
load(
	const char *path,
	const struct options *o,
	struct entry **result,
	size_t *result_count)
{
	const char *name;
	char child[LS_PATH_CAPACITY];
	struct entry *larger;
	int saved;
	size_t i;
	DIR *d;
	struct dirent *de;
	struct entry *items;
	size_t count, capacity;
	static const char *const dots[] = {".", ".."};
	unsigned dot;

	d = opendir(path);
	items = NULL;
	count = 0;
	capacity = 0;
	dot = 0;

	/* Checks the current descriptor. */
	if (!d)
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the o condition. */
		if (o->all && dot < 2U) {
			name = dots[dot++];
		} else {
			de = readdir(d);

			/* Handles the de condition. */
			if (!de)
				break;
			name = de->d_name;

			/* Handles the o condition. */
			if (!o->all && name[0] == '.')
				continue;

			/* Handles the o condition. */
			if (o->all &&
			    (!strcmp(name, ".") || !strcmp(name, "..")))
				continue;
		}

		/* Checks the remaining item count. */
		if (count == capacity) {
			capacity = capacity ? capacity * 2U : 16U;
			larger = realloc(items, capacity * sizeof(*items));

			/* Handles the larger condition. */
			if (!larger)
				goto failed;
			items = larger;
		}
		items[count].name = copy_string(name);

		/* Handles the items condition. */
		if (!items[count].name)
			goto failed;
		items[count].status_valid =
		    join_path(path, name, child, sizeof(child)) &&
		    lstat(child, &items[count].status) == 0;
		count++;
	}

	/* Handles the closedir condition. */
	if (closedir(d)) {
		d = NULL;
		goto failed;
	}
	sort_entries(items, count, o);
	*result = items;
	*result_count = count;
	/* Reports operation failure. */
	return 1;
failed:

saved = errno;

/* Checks the current descriptor. */
if (d)
	closedir(d);

/* Process each remaining element. */
for (i = 0; i < count; i++)
	free(items[i].name);
free(items);
errno = saved;

/* Reports successful completion. */
return 0;
}

/* Supports the copy string operation. */
static char *
copy_string(
	const char *text)
{
	size_t n;
	char *p;

	n = strlen(text) + 1U;
	p = malloc(n);

	/* Checks the current pointer. */
	if (p)
		memcpy(p, text, n);

	/* Returns the computed result. */
	return p;
}

/* Supports the join path operation. */
static int
join_path(
	const char *directory,
	const char *name,
	char *out,
	size_t cap)
{
	size_t a = strlen(directory), b = strlen(name);
	int slash = a && directory[a - 1] != '/';

	/* Handles the a condition. */
	if (a + (size_t)slash + b + 1U > cap) {
		errno = ENAMETOOLONG;

		/* Reports successful completion. */
		return 0;
	}
	memcpy(out, directory, a);

	/* Handles the slash condition. */
	if (slash)
		out[a++] = '/';
	memcpy(out + a, name, b + 1U);

	/* Reports operation failure. */
	return 1;
}

/* Supports the sort entries operation. */
static void
sort_entries(
	struct entry *items,
	size_t count,
	const struct options *o)
{
	struct entry value;
	size_t at;
	size_t i;

	/* Process each remaining element. */
	for (i = 1; i < count; i++) {
		/* Continue while the operation condition remains true. */
		value = items[i];
		at = i;
		while (at && compare(&items[at - 1U], &value, o) > 0) {
			items[at] = items[at - 1U];
			at--;
		}
		items[at] = value;
	}
}

/* Supports the compare operation. */
static int
compare(
	const struct entry *a,
	const struct entry *b,
	const struct options *o)
{
	int value;

	/* Handles the o condition. */
	if (o->time_sort && a->status_valid && b->status_valid) {
		/* Handles the a condition. */
		if (a->status.st_mtime > b->status.st_mtime)
			value = -1;
		else if (a->status.st_mtime < b->status.st_mtime)
			value = 1;
		else
			value = strcmp(a->name, b->name);
	} else {
		value = strcmp(a->name, b->name);
	}

	/* Returns the computed result. */
	return o->reverse ? -value : value;
}

/* Supports the print entries operation. */
static int
print_entries(
	const char *path,
	struct entry *items,
	size_t count,
	const struct options *o)
{
	size_t n_local;
	size_t index_local, n_local1;
	unsigned long long blocks;
	char total[24];
	struct long_widths widths;
	size_t width, columns, rows, row, column;
	size_t i;
	int ok;

	ok = 1;

	/* Handles the o condition. */
	if (o->long_format) {
		blocks = 0;

		measure_long(items, count, o, &widths);

		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			/* Handles the items condition. */
			if (items[i].status_valid &&
			    items[i].status.st_blocks > 0) {
				blocks += (unsigned long long)items[i]
					      .status.st_blocks;
			}
		}

		/* Handles the o condition. */
		if (o->human)
			human_size((off_t)(blocks * 512ULL), total);
		else
			snprintf(total, sizeof(total), "%llu",
				 (blocks + 1ULL) / 2ULL);
		printf("total %s\n", total);

		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			/* Handles a failed print long operation. */
			if (!print_long(path, &items[i], o, &widths))
				ok = 0;
		}

		/* Returns the computed result. */
		return ok;
	}

	/* Handles the o condition. */
	if (o->one || !o->columns) {
		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			print_name(&items[i], o);
			putchar('\n');
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining element. */
	width = 1;
	for (i = 0; i < count; i++) {
		n_local = strlen(items[i].name) + (o->classify ? 1U : 0U);

		/* Handles the n local condition. */
		if (n_local > width)
			width = n_local;
	}
	width += 2U;
	columns = 80U / width;

	/* Handles the columns condition. */
	if (columns == 0)

	/* Process each element required by the operation. */
		columns = 1;
	rows = (count + columns - 1U) / columns;
	for (row = 0; row < rows; row++) {
		/* Process each element required by the operation. */
		for (column = 0; column < columns; column++) {
			index_local = column * rows + row;

			/* Handles the index local condition. */
			if (index_local >= count)
				continue;
			print_name(&items[index_local], o);
			n_local1 = strlen(items[index_local].name) +
			    (o->classify && items[index_local].status_valid &&
				     suffix(items[index_local].status.st_mode)
				 ? 1U
				 : 0U);

			/* Handles the column condition. */
			if (column + 1U < columns &&
			    index_local + rows < count) {
				/* Continue while the operation condition remains true. */
				while (n_local1++ < width)
					putchar(' ');
			}
		}
		putchar('\n');
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the measure long operation. */
static void
measure_long(
	const struct entry *items,
	size_t count,
	const struct options *o,
	struct long_widths *w)
{
	char links[24], size[32], ub[24], gb[24];
	const char *u, *g;
	size_t i;

	memset(w, 0, sizeof(*w));

	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		/* Handles the items condition. */
		if (!items[i].status_valid)
			continue;
		snprintf(links, sizeof(links), "%lu",
			 (unsigned long)items[i].status.st_nlink);

		/* Handles the o condition. */
		if (o->human)
			human_size(items[i].status.st_size, size);
		else
			snprintf(size, sizeof(size), "%lld",
				 (long long)items[i].status.st_size);
		u = uid_name(items[i].status.st_uid, ub);
		g = gid_name(items[i].status.st_gid, gb);

		/* Handles a failed strlen operation. */
		if (strlen(links) > w->links)
			w->links = strlen(links);

		/* Handles a failed strlen operation. */
		if (strlen(u) > w->user)
			w->user = strlen(u);

		/* Handles a failed strlen operation. */
		if (strlen(g) > w->group)
			w->group = strlen(g);

		/* Handles a failed strlen operation. */
		if (strlen(size) > w->size)
			w->size = strlen(size);
	}
}

/* Supports the human size operation. */
static void
human_size(
	off_t value,
	char out[16])
{
	unsigned long long tenth;
	unsigned long long whole, remainder;
	static const char suffixes[] = "BKMGTPE";
	unsigned unit;
	unsigned long long scale, magnitude;

	unit = 0;
	scale = 1;

	/* Validates the current value. */
	if (value < 0) {
		snprintf(out, 16, "%lld", (long long)value);

		/* Returns the computed result. */
		return;
	}

	/* Process each remaining element. */
	magnitude = (unsigned long long)value;
	while (unit + 1U < sizeof(suffixes) - 1U &&
	       magnitude >= scale * 1024ULL) {
		scale *= 1024ULL;
		unit++;
	}

	/* Handles the unit condition. */
	if (unit == 0) {
		snprintf(out, 16, "%llu", magnitude);

		/* Returns the computed result. */
		return;
	}

	whole = magnitude / scale;
	remainder = magnitude % scale;

	/* Handles the whole condition. */
	if (whole < 10U) {
		tenth = (remainder * 10ULL + scale / 2ULL) / scale;

		/* Handles the tenth condition. */
		if (tenth == 10U) {
			whole++;
			tenth = 0;
		}
		snprintf(out, 16, "%llu.%llu%c", whole, tenth,
			 suffixes[unit]);
	} else {
		whole = (magnitude + scale / 2ULL) / scale;
		snprintf(out, 16, "%llu%c", whole, suffixes[unit]);
	}
}

/* Supports the uid name operation. */
static const char *
uid_name(
	uid_t id,
	char out[24])
{
	struct passwd p, *r;
	char b[512];

	r = NULL;

	/* Handles a failed getpwuid r operation. */
	if (getpwuid_r(id, &p, b, sizeof(b), &r) == 0 && r && r->pw_name) {
		snprintf(out, 24, "%s", r->pw_name);

		/* Returns the computed result. */
		return out;
	}
	snprintf(out, 24, "%u", (unsigned)id);

	/* Returns the computed result. */
	return out;
}

/* Supports the gid name operation. */
static const char *
gid_name(
	gid_t id,
	char out[24])
{
	struct group g, *r;
	char b[512];

	r = NULL;

	/* Handles a failed getgrgid r operation. */
	if (getgrgid_r(id, &g, b, sizeof(b), &r) == 0 && r && r->gr_name) {
		snprintf(out, 24, "%s", r->gr_name);

		/* Returns the computed result. */
		return out;
	}
	snprintf(out, 24, "%u", (unsigned)id);

	/* Returns the computed result. */
	return out;
}

/* Supports the print long operation. */
static int
print_long(
	const char *directory,
	const struct entry *item,
	const struct options *o,
	const struct long_widths *w)
{
	char target[LS_PATH_CAPACITY];
	ssize_t n;
	char mode[11], path[LS_PATH_CAPACITY], size[32], when[32], ub[24],
	    gb[24];
	const char *user, *group;

	/* Handles the item condition. */
	if (!item->status_valid) {
		command_error("ls", item->name);

		/* Reports successful completion. */
		return 0;
	}
	mode_text(item->status.st_mode, mode);

	/* Handles the o condition. */
	if (o->human)
		human_size(item->status.st_size, size);
	else
		snprintf(size, sizeof(size), "%lld",
			 (long long)item->status.st_size);
	ls_time(item->status.st_mtime, when);
	user = uid_name(item->status.st_uid, ub);
	group = gid_name(item->status.st_gid, gb);
	printf("%s %*lu %-*s %-*s %*s %s %s", mode, (int)w->links,
	       (unsigned long)item->status.st_nlink, (int)w->user, user,
	       (int)w->group, group, (int)w->size, size, when, item->name);

	/* Handles a failed S ISLNK operation. */
	if (S_ISLNK(item->status.st_mode) &&
	    join_path(directory, item->name, path, sizeof(path))) {
		n = readlink(path, target, sizeof(target) - 1U);

		/* Checks the current item count. */
		if (n >= 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	putchar('\n');

	/* Reports operation failure. */
	return 1;
}

/* Supports the mode text operation. */
static void
mode_text(
	mode_t m,
	char out[11])
{
	static const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR,
				      S_IRGRP, S_IWGRP, S_IXGRP,
				      S_IROTH, S_IWOTH, S_IXOTH};
	static const char letters[] = "rwx";
	unsigned i;

	/* Process each element required by the operation. */
	out[0] = type_char(m);
	for (i = 0; i < 9; i++)
		out[i + 1] = (m & bits[i]) ? letters[i % 3] : '-';

	/* Handles the m condition. */
	if (m & S_ISUID)
		out[3] = (m & S_IXUSR) ? 's' : 'S';

	/* Handles the m condition. */
	if (m & S_ISGID)
		out[6] = (m & S_IXGRP) ? 's' : 'S';

	/* Handles the m condition. */
	if (m & S_ISVTX)
		out[9] = (m & S_IXOTH) ? 't' : 'T';
	out[10] = '\0';
}

/* Supports the type char operation. */
static char
type_char(
	mode_t m)
{
	/* Handles the m condition. */
	if (S_ISDIR(m))
		return 'd';

	/* Handles the m condition. */
	if (S_ISCHR(m))
		return 'c';

	/* Handles the m condition. */
	if (S_ISBLK(m))
		return 'b';

	/* Handles the m condition. */
	if (S_ISFIFO(m))
		return 'p';

	/* Handles the m condition. */
	if (S_ISLNK(m))
		return 'l';

	/* Handles the m condition. */
	if (S_ISSOCK(m))
		return 's';

	/* Returns the computed result. */
	return '-';
}

/* Supports the ls time operation. */
static void
ls_time(
	time_t value,
	char out[32])
{
	static const int md[] = {31, 28, 31, 30, 31, 30,
				 31, 31, 30, 31, 30, 31};
	static const char *mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
				   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	long long days, sec, y;
	time_t now;
	int m;

	days = value / 86400;
	sec = value % 86400;
	y = 1970;
	now = time(NULL);
	m = 0;

	/* Handles the sec condition. */
	if (sec < 0) {
		sec += 86400;
		days--;
	}
	while (days >= 365 + leap(y)) {
		days -= 365 + leap(y);
		y++;
	}
	while (days < 0) {
		y--;
		days += 365 + leap(y);
	}
	while (m < 11 && days >= md[m] + (m == 1 && leap(y))) {
		days -= md[m] + (m == 1 && leap(y));
		m++;
	}

	/* Handles the now condition. */
	if (now != (time_t)-1 && (value < now - 15552000 || value > now + 3600))
		snprintf(out, 32, "%s %2d  %4lld", mn[m], (int)days + 1, y);
	else
		snprintf(out, 32, "%s %2d %02lld:%02lld", mn[m], (int)days + 1,
			 sec / 3600, (sec / 60) % 60);
}

/* Supports the leap operation. */
static int
leap(
	long long y)
{
	/* Returns the computed result. */
	return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

/* Supports the print name operation. */
static void
print_name(
	const struct entry *item,
	const struct options *o)
{
	char mark;

	printf("%s", item->name);

	/* Handles the o condition. */
	if (o->classify && item->status_valid) {
		mark = suffix(item->status.st_mode);

		/* Handles the mark condition. */
		if (mark)
			putchar(mark);
	}
}

/* Supports the suffix operation. */
static char
suffix(
	mode_t mode)
{
	/* Validates the selected mode. */
	if (S_ISDIR(mode))
		return '/';

	/* Validates the selected mode. */
	if (S_ISLNK(mode))
		return '@';

	/* Validates the selected mode. */
	if (S_ISFIFO(mode))
		return '|';

	/* Validates the selected mode. */
	if (S_ISSOCK(mode))
		return '=';

	/* Validates the selected mode. */
	if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
		return '*';

	/* Returns the computed result. */
	return '\0';
}

/* Supports the free entries operation. */
static void
free_entries(
	struct entry *items,
	size_t count)
{
	size_t i;

	/* Process each remaining element. */
	for (i = 0; i < count; i++)
		free(items[i].name);
	free(items);
}
